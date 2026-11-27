r"""mpv 的 JSON IPC 客户端（Windows 命名管道）。

用途：让测试侧在 mpv 播放过程中发命令（``frame-step`` / ``screenshot-to-file``）
或查属性（``get_property``），这是抓「真实显示路径」帧的唯一入口——
``--vo=image`` 走的是软件 VO，抓不到 GPU 色彩管理与缩放的结果。

实测要点（Windows）：
- 服务端参数是 ``--input-ipc-server=\\.\\pipe\\<name>``；
- **管道未就绪时 open() 会无限阻塞**，必须先 ``os.listdir(r"\\.\pipe\\")`` 确认；
- mpv 播完就退出，交互场景必须带 ``--keep-open=yes --idle=yes``；
- 每条命令恰好回一条 JSON，写完读一条即可，不需要处理异步事件。
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path


class IpcError(RuntimeError):
    pass


class MpvIpc:
    """一条命名管道连接；用 ``connect()`` 等待服务端就绪。"""

    def __init__(self, name: str, pipe_path: str | None = None) -> None:
        self.name = name
        self.path = pipe_path or rf"\\.\pipe\{name}"
        self._conn = None
        self._buf = b""
        self._rid = 0

    # ---- 生命周期 ----
    @classmethod
    def connect(
        cls,
        name: str,
        *,
        timeout: float = 20.0,
        alive=None,
        poll: float = 0.05,
    ) -> "MpvIpc":
        """等管道出现后连接；``alive`` 用来发现 mpv 已经提前退出。"""
        ipc = cls(name)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if alive is not None and not alive():
                raise IpcError(f"mpv 在管道 {name} 就绪前退出")
            if _pipe_exists(name):
                try:
                    ipc._conn = open(ipc.path, "r+b", buffering=0)
                    return ipc
                except OSError:
                    pass
            time.sleep(poll)
        raise IpcError(f"等待 IPC 管道超时：{name}")

    def close(self) -> None:
        if self._conn is not None:
            try:
                self._conn.close()
            finally:
                self._conn = None

    def __enter__(self) -> "MpvIpc":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # ---- 命令 ----
    def command(self, *args, timeout: float = 15.0) -> dict:
        """发一条命令并读回应答；非 success 时抛 ``IpcError``。

        mpv 的应答可能被合并/乱序写入管道，用 ``request_id`` 对齐，
        丢弃不属于本次请求的行（也让偶发的事件行不再影响解析）。
        """
        if self._conn is None:
            raise IpcError("IPC 尚未连接")
        self._rid += 1
        rid = self._rid
        payload = json.dumps(
            {"command": list(args), "request_id": rid}, ensure_ascii=False
        ) + "\n"
        self._conn.write(payload.encode("utf-8"))
        reply = self._read(rid, timeout)
        if reply.get("error") not in (None, "success"):
            raise IpcError(f"{args}: {reply.get('error')}")
        return reply

    def get_property(self, name: str, timeout: float = 15.0):
        return self.command("get_property", name, timeout=timeout).get("data")

    def set_property(self, name: str, value) -> None:
        self.command("set_property", name, value)
    def _read(self, rid: int, timeout: float) -> dict:
        # 管道读可能一次带回多条消息（mpv 会合并写），按行切分自己缓冲
        deadline = time.monotonic() + timeout
        while True:
            while b"\n" in self._buf:
                line, _, rest = self._buf.partition(b"\n")
                self._buf = rest
                line = line.strip()
                if not line:
                    continue
                msg = json.loads(line.decode("utf-8", errors="replace"))
                if msg.get("request_id") == rid:
                    return msg
                # 事件（无 request_id）或过期应答：丢掉继续等
            if time.monotonic() > deadline:
                raise IpcError("等待 IPC 应答超时")
            chunk = self._conn.read(4096)
            if not chunk:
                raise IpcError("IPC 连接被对端关闭")
            self._buf += chunk


def _pipe_exists(name: str) -> bool:
    try:
        return any(name == p for p in os.listdir(r"\\.\pipe\\"))
    except OSError:
        return False


def pipe_name(cwd: Path) -> str:
    """按目录 + pid 生成管道名，避免并发会话撞名。"""
    import hashlib

    digest = hashlib.sha1(str(Path(cwd).resolve()).encode("utf-8")).hexdigest()[:10]
    return f"vfi_{digest}_{os.getpid()}"
