# 包里 VS 补帧四引擎检查（2026-11-23）

检查目标：`mpv-playkit` 打出来的 vsdlssg 包里，`vs/MEMC_DLSSG_NV.vpy`、
`vs/MEMC_VFG_NV.vpy`、`vs/MEMC_FSR4.vpy`、`vs/MEMC_XESSFG_NV.vpy` 四个 VS 补帧预设
能否在包内直接跑通。结论先说：**都能跑，但旧版脚本每帧多两趟全分辨率 float 转换，
且 XeSS-FG 在 3× 时会让进程自杀**；两个问题都已修。

## 探测方法（唯一可靠的一套）

mpv + VS 补帧跑到片尾会卡死（日志反复 `video EOF (status=3)`，进程不退），
`--frames=N`、`--length=1`、`--vo=null` 都救不了。可用做法是真实 VO + 定时 dump 再退出：

```bat
cd /d <stage 目录>
mpv.exe --no-terminal --vo=gpu-next --gpu-context=d3d11 --ao=null --hwdec=auto-copy ^
        --vf=vapoursynth=~~/vs/<vpy> --script=...\.work\vsfps.lua <输入文件>
```

`.work\vsfps.lua` 在 `vo-configured` + 2.5 秒后把 `vf`、`estimated-vf-fps`、
`container-fps`、`hwdec-current`、`video-format`、`video-params/pixelformat`、
`frame-drop-count`、`estimated-frame-number` 追加写文件，然后 `quit`（另有 15 秒兜底）。

输入：`测试数据\test_60fps.mp4`（hevc、p010、1920×1080、60 fps）。

## 结果（stage = `mpv-playkit-pre-2026.11.20-e1628f1-windows-x64-vsdlssg`）

| 预设 | 旧版脚本 | 修好之后 |
| --- | --- | --- |
| DLSSG 3× | 180 fps，正常 | 180 fps，`frame-drop-count=1` |
| VFG 2× | 120 fps，正常 | 120 fps |
| FSR4 2× | 120 fps，正常 | 120 fps |
| XeSS-FG 3× | **进程退出 `0xE0D55001`** | 默认改 2×：120 fps 正常 |

全部样本 `hwdec-current=d3d11va-copy`、`exit=0`。

### XeSS-FG 崩溃原因

包内 `vs-plugins\xessfg\dlssg_sm86\logs\dlssg_sm86.log` 里有确切记录：

- `xefg_context`：`graph:"2x"` → `graph:"mfg"`、`multi_frame_count:2`；
- 之后每约 17 ms 一条 `xefg_reset`，`reasons:["dlssg_reset","no_history"]`；
- 致命：`{"code":"0xe0d55001","event":"xefg_kernel_fatal","kind":"index",
  "reason":"DLSSG.MultiFrameIndex 2 arrived twice for one real frame"}`，
  模块 `...\xessfg\dlssg_sm86\backend\sm86_backend.dll`、offset `0x11050d`。

即引擎在多帧生成（≥3×）路径上认为索引配对错乱，直接终止进程。同一份 vpy 用
`VXESSFG_MULTI=2` 强制 2× 时完全正常——**这是引擎限制，不是打包或脚本问题**。
另外 `user-data="multi=2"` 在这个 vpy 里传不进去（只有环境变量生效），所以包里直接
把默认值改成 2。

## 修了什么

### 1. 新版 vpy 主流程（`k7sfunc` fork，commit `c65c024`）

四个 vpy 换成 `vs-dlssg\mpv\` 的新版：

- 旧版：`YUV → RGBS（matrix_in_s="709"）→ 引擎 → 转回 fmt_in → AssumeFPS`，
  每帧两趟全分辨率 float 转换、`range_s="limited"` 补正；
- 新版：`if yuv_in: src = clip`（YUV 直通），`out = fg if yuv_in else resize(...)`，
  不再传 `proxy_dir`（插件按 `moduleDirectory()\dlss45|xessfg|vfg|fsr4` 自找引擎包），
  并新增 `Preset` 与 user-data 入参。

### 2. 包布局的 DLL 路径

新版脚本默认按 `_here\..\vs-plugins\<dll>` 找插件，但包里 vpy 在
`portable_config\vs\`、DLL 在包根 `vs-plugins\`，差两级。四份脚本各补一条
包根候选（放在最前）：

```python
os.path.join(_here, os.pardir, os.pardir, "vs-plugins", "<dll>")
```

不补这条时包里会直接报 `MEMC_DLSSG: 找不到 vdlssg.dll`。

### 3. XeSS-FG 默认 2×

`MEMC_XESSFG_NV.vpy` 的 `Multi = 3` 改成 `2`，头注释写明「3× 及以上会触发引擎致命
错误（0xe0d55001，进程直接退出）」；`manifests/config.yml` 里预设标签改成
`预设 补帧 XeSS-FG 2× (DLC-vsNV)`（`mpv-playkit` commit `0ec9b3c`），
菜单与 `input_uosc.conf` 快捷键说明同步。

## 还没做的

- `sources.lock.json` 里 k7sfunc 仍钉在 `11fea6b`：要更新必须先把这个本地 commit
  **push 到 `o5-null/K7sfunc`**（codeload tarball 只认远端 commit），再
  `python -m build pin`；之后重出 vsdlssg / vs-mlrt 包并复测。
- 零拷贝（`vf=playkit`）跨显卡共享句柄的根因已定位在
  `vs-dlssg\src\bridge.cpp`（丢弃 D3D11 LUID），本轮不处理，见
  `docs\mpv-playkit-bridge-probe.md`。
