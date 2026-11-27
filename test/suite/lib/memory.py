"""进程内存 / 显存采样：给每个子进程留一条时间序列。

为什么要自己做：调试帧生成时，「跑到第几帧内存开始涨」「显存有没有泄漏」
「是不是掉进了共享内存」比单个峰值更有用。这里给的是**采样序列**，不是快照。

两个来源：

* **进程内存**：``K32GetProcessMemoryInfo``（psapi），拿工作集 / 私有提交 / 峰值。
  纯 ctypes，不依赖 psutil。
* **显存**：Windows 自带 ``Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory``
  性能计数器（按 pid + LUID 分实例，单位字节）。这是少数**按进程**统计显存的公开途径；
  ``nvidia-smi --query-compute-apps=used_gpu_memory`` 在 WDDM 的 GeForce 驱动上普遍返回
  ``[N/A]``，所以只在它确实给出数字时才用（优先、更快）。

采样由后台线程做：``Sampler(pid).start()``，``stop()`` 返回 ``Summary``（峰值 + 原始行）。
``runner.run(monitor=True)`` 已经接好，产物里会多一个 ``mem.csv`` 和 ``meta.json`` 的
``memory`` 段。
"""

from __future__ import annotations

import csv
import ctypes
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

IS_WINDOWS = sys.platform == "win32"

#: OpenProcess 需要的最小权限（查询内存 + 读 VM）
_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_PROCESS_VM_READ = 0x0010

_GB = 1024.0 ** 3
_MB = 1024.0 ** 2

CIM_QUERY = (
    "Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory "
    "| Select-Object Name,DedicatedUsage,SharedUsage | ConvertTo-Json -Compress"
)
_PID_RE = re.compile(r"pid_(\d+)_", re.IGNORECASE)


# ── 进程内存 ────────────────────────────────────────────────────────────────


class _PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.c_ulong),
        ("PageFaultCount", ctypes.c_ulong),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


def process_memory(pid: int) -> dict[str, int] | None:
    """返回 ``{wset, private, peak_wset, pagefile}``（字节）；进程已退出返回 None。"""
    if not IS_WINDOWS:
        return None
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    # 必须声明句柄宽度：默认 restype 是 c_int，64 位句柄被截断后会去查别的进程
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    psapi.GetProcessMemoryInfo.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32)
    psapi.GetProcessMemoryInfo.restype = ctypes.c_int

    handle = kernel32.OpenProcess(
        _PROCESS_QUERY_LIMITED_INFORMATION | _PROCESS_VM_READ, False, int(pid)
    )
    if not handle:
        return None
    try:
        counters = _PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(counters)
        ok = psapi.GetProcessMemoryInfo(
            handle, ctypes.byref(counters), ctypes.sizeof(counters)
        )
        if not ok:
            return None
        return {
            "wset": int(counters.WorkingSetSize),
            "private": int(counters.PrivateUsage),
            "peak_wset": int(counters.PeakWorkingSetSize),
            "pagefile": int(counters.PagefileUsage),
        }
    finally:
        kernel32.CloseHandle(handle)


# ── 进程树 ──────────────────────────────────────────────────────────────────


class _PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", ctypes.c_ulong),
        ("cntUsage", ctypes.c_ulong),
        ("th32ProcessID", ctypes.c_ulong),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", ctypes.c_ulong),
        ("cntThreads", ctypes.c_ulong),
        ("th32ParentProcessID", ctypes.c_ulong),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", ctypes.c_ulong),
        ("szExeFile", ctypes.c_char * 260),
    ]


_TH32CS_SNAPPROCESS = 0x00000002


def process_parents() -> dict[int, int]:
    """``{pid: ppid}`` 全表。venv/uv 的 python.exe 是启动器，真正的解释器是它的子进程，
    所以采样要按进程树求和，只盯 Popen 返回的 pid 会一直读到 5 MB 的空壳。"""
    if not IS_WINDOWS:
        return {}
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    kernel32.CreateToolhelp32Snapshot.argtypes = (ctypes.c_uint32, ctypes.c_uint32)
    kernel32.Process32First.argtypes = (ctypes.c_void_p, ctypes.POINTER(_PROCESSENTRY32))
    kernel32.Process32Next.argtypes = (ctypes.c_void_p, ctypes.POINTER(_PROCESSENTRY32))
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)

    snap = kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
    if not snap or snap == ctypes.c_void_p(-1).value:
        return {}
    out: dict[int, int] = {}
    try:
        entry = _PROCESSENTRY32()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32First(snap, ctypes.byref(entry))
        while ok:
            out[int(entry.th32ProcessID)] = int(entry.th32ParentProcessID)
            ok = kernel32.Process32Next(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return out


def process_tree(pid: int, parents: dict[int, int] | None = None) -> list[int]:
    """pid 及其所有后代（深度优先展开）。"""
    parents = process_parents() if parents is None else parents
    children: dict[int, list[int]] = {}
    for child, parent in parents.items():
        children.setdefault(parent, []).append(child)
    out, stack = [], [int(pid)]
    while stack:
        cur = stack.pop()
        out.append(cur)
        stack.extend(children.get(cur, ()))
    return out


def tree_memory(pid: int) -> dict[str, int] | None:
    """整棵进程树的内存合计（工作集/私有提交/峰值），外加进程数。"""
    pids = process_tree(pid)
    total = {"wset": 0, "private": 0, "peak_wset": 0, "pagefile": 0, "procs": 0}
    for one in pids:
        mem = process_memory(one)
        if not mem:
            continue
        total["procs"] += 1
        for key in ("wset", "private", "peak_wset", "pagefile"):
            total[key] += mem[key]
    return total if total["procs"] else None


# ── 显存 ────────────────────────────────────────────────────────────────────


def _parse_cim(text: str) -> dict[int, dict[str, int]] | None:
    """把 ConvertTo-Json 的结果解析成 ``{pid: {dedicated, shared}}``。"""
    import json

    text = (text or "").strip()
    if not text:
        return None
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return None
    if isinstance(data, dict):
        data = [data]
    out: dict[int, dict[str, int]] = {}
    for item in data or []:
        if not isinstance(item, dict):
            continue
        m = _PID_RE.match(str(item.get("Name", "")))
        if not m:
            continue
        pid = int(m.group(1))
        slot = out.setdefault(pid, {"dedicated": 0, "shared": 0})
        slot["dedicated"] += int(item.get("DedicatedUsage") or 0)
        slot["shared"] += int(item.get("SharedUsage") or 0)
    return out


def _nvidia_smi() -> dict[int, dict[str, int]] | None:
    """nvidia-smi 的按进程显存；WDDM GeForce 上通常是 [N/A]，那就返回 None。"""
    exe = shutil.which("nvidia-smi")
    if not exe:
        return None
    try:
        proc = subprocess.run(
            [exe, "--query-compute-apps=pid,used_gpu_memory",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    out: dict[int, dict[str, int]] = {}
    for line in (proc.stdout or "").splitlines():
        pid_s, _, mem_s = line.partition(",")
        pid_s, mem_s = pid_s.strip(), mem_s.strip()
        if not pid_s.isdigit() or not mem_s.isdigit():
            continue
        out[int(pid_s)] = {"dedicated": int(mem_s) * 1024 * 1024, "shared": 0}
    return out or None


#: 显存来源是否可用（首次探测后缓存，避免每次采样都试一遍）
_gpu_state = {"cim": None, "smi": None}


def gpu_process_memory() -> dict[int, dict[str, int]]:
    """整机的按进程显存表（字节）。两条来源都没有时返回空字典。"""
    merged: dict[int, dict[str, int]] = {}

    if _gpu_state["smi"] is not False:
        smi = _nvidia_smi()
        _gpu_state["smi"] = smi is not None
        merged.update(smi or {})

    if _gpu_state["cim"] is not False:
        cim = _cim_snapshot()
        _gpu_state["cim"] = cim is not None
        for pid, mem in (cim or {}).items():
            merged.setdefault(pid, {"dedicated": 0, "shared": 0})
            merged[pid]["dedicated"] = max(merged[pid]["dedicated"], mem["dedicated"])
            merged[pid]["shared"] = mem["shared"]
    return merged


def _cim_snapshot() -> dict[int, dict[str, int]] | None:
    exe = shutil.which("powershell") or shutil.which("pwsh")
    if not exe:
        return None
    try:
        proc = subprocess.run(
            [exe, "-NoProfile", "-NonInteractive", "-Command", CIM_QUERY],
            capture_output=True, text=True, timeout=30,
            encoding="utf-8", errors="replace",
            env={**os.environ, "PYTHONUTF8": "1"},
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    return _parse_cim(proc.stdout)


def gpu_available() -> bool:
    """显存统计到底有没有来源（给 env 用例做断言/跳过用）。"""
    return bool(gpu_process_memory())


# ── 采样器 ──────────────────────────────────────────────────────────────────


@dataclass
class Summary:
    seconds: float = 0.0
    samples: int = 0
    peak_wset_mb: float = 0.0
    peak_private_mb: float = 0.0
    peak_gpu_dedicated_mb: float | None = None
    peak_gpu_shared_mb: float | None = None
    last_wset_mb: float = 0.0
    procs: int = 0
    rows: list[dict] = field(default_factory=list)

    def as_dict(self) -> dict:
        """写进 meta.json 的紧凑形态（不带 rows）。"""
        return {
            "seconds": round(self.seconds, 3),
            "samples": self.samples,
            "procs": self.procs,
            "peak_wset_mb": round(self.peak_wset_mb, 1),
            "peak_private_mb": round(self.peak_private_mb, 1),
            "peak_gpu_dedicated_mb": _round_opt(self.peak_gpu_dedicated_mb),
            "peak_gpu_shared_mb": _round_opt(self.peak_gpu_shared_mb),
            "last_wset_mb": round(self.last_wset_mb, 1),
        }

    def write_csv(self, path: Path) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        cols = ["t_s", "procs", "wset_mb", "private_mb", "gpu_dedicated_mb", "gpu_shared_mb"]
        with path.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
            writer.writeheader()
            for row in self.rows:
                writer.writerow(row)
        return path


def _round_opt(value: float | None) -> float | None:
    return None if value is None else round(value, 1)


def _mb(value: int | float | None) -> float:
    return 0.0 if value is None else round(float(value) / _MB, 3)


class Sampler:
    """后台采样一个 pid 的内存与显存。

    两条独立线程：进程内存按 ``interval``（默认 0.2s，便宜），显存按
    ``gpu_interval``（默认 1s，每次要起 powershell 读性能计数器，贵得多）。
    分开的原因很实际：显存那次查询要几秒，混在一个循环里会把内存采样饿死。
    进程退出后拿不到句柄就记 0，所以不需要精确的结束时刻。
    """

    def __init__(
        self,
        pid: int,
        *,
        interval: float = 0.2,
        gpu: bool = True,
        gpu_interval: float = 1.0,
    ) -> None:
        self.pid = int(pid)
        self.interval = max(0.02, float(interval))
        self.gpu = gpu
        self.gpu_interval = max(self.interval, float(gpu_interval))
        self.rows: list[dict] = []
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self._lock = threading.Lock()
        self._started = 0.0
        # pid -> 最近一次显存读数（由 gpu 线程更新，内存线程只读）
        self._gpu_latest: dict[str, int] | None = None
        self._smi_ok = True
        self._cim_ok = True

    def start(self) -> "Sampler":
        self._started = time.perf_counter()
        self._threads = [
            threading.Thread(target=self._mem_loop, name=f"vfi-mem-{self.pid}", daemon=True),
        ]
        if self.gpu:
            self._threads.append(
                threading.Thread(target=self._gpu_loop, name=f"vfi-vram-{self.pid}", daemon=True)
            )
        for th in self._threads:
            th.start()
        return self

    def stop(self) -> Summary:
        self._stop.set()
        for th in self._threads:
            th.join(timeout=max(5.0, self.gpu_interval * 3))
        self._sample(final=True)
        return self._summary()

    # ---- 内部 ----
    def _mem_loop(self) -> None:
        while not self._stop.wait(self.interval):
            self._sample()

    def _gpu_loop(self) -> None:
        """显存采样线程：先立刻读一次（短进程常常在第一个间隔内就结束了）。"""
        while True:
            got = self._read_gpu()
            if got is not None:
                with self._lock:
                    self._gpu_latest = got
            if self._stop.wait(self.gpu_interval):
                return

    def _read_gpu(self) -> dict[str, int] | None:
        """显存取数：先 nvidia-smi（快，但 WDDM GeForce 常给 [N/A]），再退性能计数器。

        按进程树求和：venv/uv 的 python.exe 会把真正的解释器放在子进程里。
        """
        from . import memory as _m

        table = None
        if self._smi_ok:
            table = _m._nvidia_smi()
            self._smi_ok = table is not None
        if not table and self._cim_ok:
            table = _m._cim_snapshot()
            self._cim_ok = table is not None
        if not table:
            return None
        dedicated = shared = 0
        hit = False
        for one in process_tree(self.pid):
            mem = table.get(one)
            if mem:
                hit = True
                dedicated += mem["dedicated"]
                shared += mem["shared"]
        return {"dedicated": dedicated, "shared": shared} if hit else None

    def _sample(self, *, final: bool = False) -> None:
        mem = tree_memory(self.pid)
        with self._lock:
            gpu = dict(self._gpu_latest) if self._gpu_latest else None
        if self.gpu and final and gpu is None:
            gpu = self._read_gpu()
        self.rows.append({
            "t_s": round(time.perf_counter() - self._started, 3),
            "procs": (mem or {}).get("procs", 0),
            "wset_mb": _mb((mem or {}).get("wset")),
            "private_mb": _mb((mem or {}).get("private")),
            "gpu_dedicated_mb": None if gpu is None else _mb(gpu["dedicated"]),
            "gpu_shared_mb": None if gpu is None else _mb(gpu["shared"]),
        })

    def _summary(self) -> Summary:
        gpu_vals = [r["gpu_dedicated_mb"] for r in self.rows if r["gpu_dedicated_mb"] is not None]
        shared_vals = [r["gpu_shared_mb"] for r in self.rows if r["gpu_shared_mb"] is not None]
        wset = [r["wset_mb"] for r in self.rows]
        priv = [r["private_mb"] for r in self.rows]
        procs = [r.get("procs", 0) for r in self.rows]
        return Summary(
            seconds=time.perf_counter() - self._started,
            samples=len(self.rows),
            peak_wset_mb=max(wset) if wset else 0.0,
            peak_private_mb=max(priv) if priv else 0.0,
            peak_gpu_dedicated_mb=max(gpu_vals) if gpu_vals else None,
            peak_gpu_shared_mb=max(shared_vals) if shared_vals else None,
            last_wset_mb=wset[-1] if wset else 0.0,
            procs=max(procs) if procs else 0,
            rows=list(self.rows),
        )


def monitor_enabled() -> bool:
    """``VFI_MEM=0`` 可整体关掉采样。"""
    return os.environ.get("VFI_MEM", "1").strip().lower() not in ("0", "false", "no", "off")


def gpu_enabled(default: bool = False) -> bool:
    """显存采样默认只对「确实会用 GPU」的调用打开：每次读性能计数器要 ~1s。

    ``VFI_MEM_GPU=1`` 全局打开（调试时用），``=0`` 全局关掉。
    """
    raw = os.environ.get("VFI_MEM_GPU", "").strip().lower()
    if raw in ("1", "true", "yes", "on"):
        return True
    if raw in ("0", "false", "no", "off"):
        return False
    return default


def default_interval() -> float:
    try:
        return float(os.environ.get("VFI_MEM_INTERVAL", "0.2"))
    except ValueError:
        return 0.2


def default_gpu_interval() -> float:
    try:
        return float(os.environ.get("VFI_MEM_GPU_INTERVAL", "1.0"))
    except ValueError:
        return 1.0
