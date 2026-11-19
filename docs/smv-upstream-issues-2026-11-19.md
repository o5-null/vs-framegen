# SmoothMyVideo 上游 issue 提交记录（2026-11-19）

向 https://github.com/flowreen/SmoothMyVideo（MIT，Electron+TS 前端 + C++/CUDA 原生宿主 smv-live.exe，版本 26.11.5）提交了 3 个独立 issue，均为 OPEN、无 label（o5-null 账号无 `AddLabelsToLabelable` 权限，需维护者自行打标）。

| # | 标题 | 地址 |
|---|---|---|
| 1 | Encoder: more codecs and bitrate/quality control (VP9, target bitrate, quality presets) | https://github.com/flowreen/SmoothMyVideo/issues/1 |
| 2 | Queue: editable per-item queue (settings per file, reorder, append while running) | https://github.com/flowreen/SmoothMyVideo/issues/2 |
| 3 | Allow an optional user-supplied unlocker for DLSS 4.5 and NVIDIA VFG on RTX 20/30 | https://github.com/flowreen/SmoothMyVideo/issues/3 |

正文草稿：`C:\Users\benben\AppData\Local\Temp\opencode\smv-issue-{encoding,queue,rtx30}.md`

## 立项依据（代码事实）

- **#1 编码**：`src/render/encode.ts:218 codecEncoder()` 只映射 av1→`av1_nvenc`、vvc→`libvvenc`、其余→`hevc_nvenc`，而 `outPixfmt()`（encode.ts:319）与 `profileArgs()`（encode.ts:392）却写着 `h264_nvenc` —— H.264 从 UI 不可达。`qualityArgs()`（encode.ts:327）把参数钉死为 `-preset p7 -tune hq -rc vbr -cq 17 -b:v 0 ...`，唯一外部调整途径是环境变量 `SMV_CQ / SMV_ENC_LOSSLESS / SMV_NVENC_LEVEL`。README 明确宣称 "no knob to guess at"，所以诉求定位为「可选高级项」而非推翻默认可复现性（encode.ts:306-313 有 resumeVenc 一致性校验）。
- **#2 队列**：README 已说明"同设置顺序批处理 + 崩溃后重排"，故不重复要"批量"，而要求**可编辑队列**：`renderer/app.js:777 queueVideos()` 只做 `batch = list.slice(1)`，全局 `batch`（app.js:50-61）无可见列表、无逐项参数、无排序、第二次调用覆盖队列。
- **#3 解锁**：`src/main-setup.ts:334` 的 `supported` 正则只影响默认选中，并非硬门槛；VFG 的 `nvvfg-ready` 仅查文件存在（main-setup.ts:295-313）。因此集成成本极低：检测到解锁器则放行，否则行为不变。

## #3 的技术论据（关键）

- DLSS 4.5 provider `engine\dlssg\nvngx_dlssg.dll` 310.9.16：`NVSDK_NGX_GetGPUArchitecture` = `mov eax,0x190; ret`，GetFeatureRequirements 记录里 FG feature id `0x0B` 后紧跟 `0x190` → sm_86 报 `snippet is using newer GPU arch 190 > 170` / CreateFeature `0xbad0000b`。
- VFG `engine\nvvfx\nVFXVideoFrameGeneration.dll`（207 MB，nvidia-vfx 0.2.0.0）：124 个 cubin 只有 `0x00590559`(sm_89)/`0x06007802`(sm_120)，无 PTX → `NvVFX_Load` = -1999。
- 3080 实测：DLSS-G 握手 `gen=5 max=5`（等价 50 系，`dlssgLimit()`=6x 一次通过），71/71 kernel 改写为 SM86 并真实执行；VFG 1920x1080 4x = 60.94 ms/pair。
- 性能对比（README 8 引擎基准，RTX 3080）：DLSS-G 1080p **8.00 ms** 最快、4K 29.38；NVIDIA VFG 4K **22.63 ms** 最快且几乎不随分辨率增长。→ Ampere 上这两个就是最优引擎，却被架构门槛挡住。
- 已在 issue 中声明的边界：RTX SDK 许可 §3 禁止再分发 runtime（解锁器必须用户自备、可选、非默认）；SM89 SASS 跑在 SM86 上今天可用但无保证，必须能回落到 GMFSS；HAGS 必需；RTX 20/GTX 16xx 未测/不支持；10 GB 显存 4K 6x 建议降到 4x。

## 后续

已在 #3 中主动提出可提 PR：`ngx_direct.h` 补丁（无解锁器时零行为变化）+ `main-setup.ts` 检测逻辑 + 自研 MIT `winmm.dll` 代理源码 + 复现脚本与数据。等维护者回应。
