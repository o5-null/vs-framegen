# VFG 模型档位与流水式批量：交付记录（2026-11-25 / 26）

本轮把 `vvfg`（VFG）从「只有固定 high 档、单张串行」推进到「三档可选、默认 medium、N−1 张内插流水批量」，并把档位接到 mpv / k7sfunc 的各条入口，最后发布了 playkit 预览包。

## 1. 引擎侧（vs-framegen 0.2.2）

- 引擎自带的 `Mode` 参数（0 low / 1 medium / 2 high）以前被插件固定成 2。现在 `vvfg` 滤镜有 `mode:int:opt;`，默认取 `medium`（`fg_types.h` 的 `int quality = 1;`），越界报 `mode must be 0 (low), 1 (medium) or 2 (high), got N`；环境变量 `VFG_MODE` 可临时覆盖。
- 像素打包/解包改成 AVX2 逐行处理（`packRow` / `unpackRow`，8 像素并行，NaN 折 0）。
- 源帧改用 `cuMemAllocManaged` 跨 lane 共享（CUDA IPC 在 Windows 不可用），每条 lane 只留自己的 pinned 输出。
- 一对帧的 N−1 张内插改为流水：`serve i → sync i → issue i+1 → unpack i`，解包藏到下一帧的 GPU 时间后面。`VFG_BATCH=0` 可退回旧的整对提交。

## 2. 数字（RTX 3080 10GB，1080p sintel Trim 90..113）

| 口径 | high | medium | low |
| --- | --- | --- | --- |
| ms / 输出帧（multi=6） | 16.6 | 4.2 | — |
| 显存 / lane（1080p） | 1406 MiB | 714 MiB | 354 MiB |
| 建 lane 时间 | 0.4–0.75 s | 1.5–2.5 s | 1.4–1.9 s |
| PSNR-Y | 33.91 | 33.52 | — |

- 流水批量本身：1080p medium multi=6 5.64 → 4.18 ms（−26%）、multi=4 6.05 → 4.49；high multi=6 −6.6%；4K medium multi=4 −14%。
- VS core 每帧 RGBS 宿主成本约 0.55 ms/Mpx（1920×1080 = 1.146 ms、3840×1632 = 3.525 ms），扣掉后插件真实成本 1080p medium 约 2.6 ms、4K 约 6.9 ms。
- 旧文档里「已到平台期」只对 high 成立；medium 的瓶颈是 host 侧打包/解包与 VS core 本身。

## 3. mpv / k7sfunc 入口

- `mod_vpy.py`：`FRAMEGEN_VPY(..., mode=1)`，`_FRAMEGEN_PLUGIN_ARGS["vvfg"] = ("mode",)`，`_FRAMEGEN_VALUE_SPECS["mode"] = (int, 0, 2)`；优先级 脚本字面量 < `VVFG_MODE` < `user-data="mode=N"`；转发判断要特判 0（`mode=0` 是合法值）。
- `mod_memc.py`：`VFG_NV(..., mode=1)` 与 `FRAMEGEN_HUB(..., mode=1)`，只有 `vvfg` 校验并下传。
- 两份 `MEMC_VFG_NV.vpy` 加 `Mode = 1` 并传给 `FRAMEGEN_VPY`；`k7f_vpypar.lua`（k7sfunc 与 vsdlssg 两份副本）只在 VFG 脚本上多显示一项「档位」。
- 版本 `1.9.0+pre.10`；wiki（`3_K7sfunc` / `4_FrameGen` / `4_FrameGen_基准与FAQ` / `Home`）同步。

## 4. 发布

- commit：根仓库 `37e2e89` / `c89209b` / `304ef55`；k7sfunc `40d1a11`；k7sfunc-wiki `ac8ad64`。
- `mpv-playkit` 版本 `2026.11.25`，k7sfunc 重新 pin 到 `40d1a11`（`ba0d6ab`）。
- release `pre-2026.11.25-993e00d`：base 167716553 B / vsdlssg 347376752 B / vs-mlrt 698762379 B，三个包与 sha256 共 6 个资产，`归档校验通过` + `冒烟测试通过`（冒烟可见 k7sfunc 1.9.0+pre.10）。

## 5. 注意事项

- VFG 首次挂载会卡一下（每条 lane 建 CUDA context + 装载模型），1080p 6× medium 首次约 10 s，属引擎初始化，不是卡死。
- `fmParallelRequests` 在 10 GB 卡上会因显存分页把端到端时间放大两个数量级，插件内部流水才是正确做法。
- 测 vs-* 滤镜端到端务必扣掉 VS core 的 RGBS 宿主成本。
