# mpv-playkit 基准配置替换为 mpv-lazy，档位差异改由配置清单生成

日期：2026-11-23
仓库：`mpv-playkit`（本地提交 `b15ceb2`、`8ce22b6`，未推送）

## 起因

用户反馈：当前构建产出的 `portable_config` 引入太多不认识的脚本和设置，要求以
`测试环境\mpv-lazy\portable_config`（mpv-lazy 最后发版）为基准；并且希望用一个配置清单
给每个包生成略有差异的配置文件——例如 `vsdlssg` 包里加上 4 个帧生成 vpy，
`input_uosc.conf` 里加 4 行调用，同时把不可用的 vpy 禁用掉。

## 决策

1. **基准换成本地维护**：`portable_config/` 不再跟随 hooke007/mpv_PlayKit 每日整目录覆盖，
   内容取自 `测试环境\mpv-lazy\portable_config`。上游只有 `portable_config/shaders/`
   仍在更新，`build/sync.py` 的 `SYNCED_DIRS` 因此缩到 `("portable_config/shaders",)`；
   `installer/` 冻结，不再同步、也不进包。
2. **配置清单**：新增 `manifests/config.yml`，只做两件事——
   - `presets`：VF 预设表（`key` 按键 / `vpy` 脚本 / `label` 显示名 / `requires` 依赖），
     共 11 条（MVTools_快速、RIFE_DX12、DRBA_DX12、RIFE_RTX、DRBA_RTX、UAI_DX12、
     UAI_RTX、Dlss、XeSS-FG、VFG、FSR4）；
   - `packs`：每个档位包启用哪几条——`base: [mvt_lq]`、
     `vsdlssg: [mvt_lq, dlssg, xessfg, vfg, fsr4]`、
     `vs-mlrt: [mvt_lq, rife_nv, drba_nv, uai_nv_trt]`。
     `base` 只服务中间基础 stage（`assemble` / `smoke` / `verify` 不带 `--pack` 时用它），
     只含 mpv + k7sfunc 基础依赖就能跑的预设；它不是交付物，用户拿到的是两个档位包。
3. **生成而不是维护副本**：新增 `build/config.py`，对 stage 做三件事：
   重写 `input_uosc.conf` / `menu.conf` 中 `# >>> playkit-config:vf` 标记区的内容；
   删掉 `vs/` 里未启用的 `.vpy`；自检标记区、启用集合与实际 vpy 是否一致。
   未启用的预设不是删除，而是在按键文件里整行注释并注明缺哪个组件
   （`（本包未启用：需 vsmlrt TensorRT）` 等），用户能看懂"为什么这个键没反应"。
4. **删除 overlay 机制**：`overlay/`、`build/overlay.py` 移除。原先 overlay 里的
   `idle = yes` 与 `[playkit]` 呈现参数由基准 `mpv.conf` 直接提供。

## 代码改动（`mpv-playkit`）

| 文件 | 变化 |
| --- | --- |
| `manifests/config.yml` | 新增，配置清单（schema 1） |
| `build/config.py` | 新增，约 220 行：`load` / `pack_config` / `render_input` / `render_menu` / `splice_block` / `prune_vpy` / `apply` / `check` |
| `build/assemble.py` | `copy_configuration(stage, pack)` 改为 `config.apply`；`assemble(..., pack=)`；`REQUIRED_ROOT` 增 `input_uosc.conf`、`menu.conf` |
| `build/pack.py` | 全量包 `assemble(version, stage=stage, pack=name)`；组件装完再 `config.apply(stage, name)`；`verify_pack_stage(..., pack=)` |
| `build/verify.py` | stage 必需项改为基准布局（`input_uosc.conf` / `menu.conf` / `script-opts.conf` / `scripts/uosc/main.lua` / `scripts/input_plus.lua` …）；末尾调 `config.check` |
| `build/inputs.py` | `overlay/` 换成 `configs/` 与 `portable_config` 的配置文本（`mpv.conf`、`input_uosc.conf`、`menu.conf`、`profiles.conf`、`script-opts.conf`、`scripts/`、`vs/`）；`shaders/`、`fonts/` 仍不进哈希 |
| `build/sync.py` | 只同步 `portable_config/shaders/`；去掉 overlay 调用 |
| `build/smoke.py` | 注释改为"基准 `mpv.conf` 是 `idle=yes`"，`--idle=no` 行为不变 |
| `build/__main__.py` | `assemble --pack`；新增 `config --stage <dir> [--pack]`；`verify --stage --pack <name>`；sync 帮助文本 |
| `.github/workflows/sync-upstream.yml` | 注释与提交信息改为"同步上游 shaders" |
| `AGENTS.md`、`docs/BUILD.md`、`docs/DECISIONS.md` | 目录职责、命令、inputs_hash 说明；新增决策第 24 条，第 19 条标注被取代 |

## 配置文件替换明细

从 `测试环境\mpv-lazy\portable_config` 复制：`mpv.conf`、`input_uosc.conf`、`menu.conf`、
`profiles.conf`、`script-opts.conf`、`scripts/{input_plus.lua, save_global_props.lua, uosc/, thumb_engine/}`、
`fonts/LXGWWenKaiMonoLite-Regular.ttf`（13.9MB，基准 `mpv.conf` 的 `sub-font` 依赖它）。

删除（`git rm`）：`fonts.conf`、`input.conf`、`input_easy.conf`、`input_list.conf`、
`input_scripts.conf`、`luts/`（9 个 cube，无任何配置引用）、`script-opts/`（15 个上游 conf，
`k7f_playkit.conf` 改由 k7sfunc 组件提供）、`scripts/{auto_load_fonts.js, auto_sub_fonts_dir.lua,
cover_art_fallback.lua, stats.lua, stats_mediainfo.lua, uosc/(旧版), uosc_addones/}`、`overlay/`、
`build/overlay.py`。

`shaders/`（441 文件 / 142.7MB）保留原样：基准 `mpv.conf`、`menu.conf` 引用的 shader
（Ani 系列、QCOM_SGEDS_ms_RT、Adaptive_sharpen 等）均已核对存在。

`vs/` 的 15 个上游 vpy 与基准同名文件 SHA-256 完全一致，因此不必替换，由清单裁剪。

## 各档位包最终形态

| 包 | VF 按键 | `vs/` 保留的 vpy |
| --- | --- | --- |
| `base`（中间基础 stage，非交付物） | `!` MVTools_快速 | `MEMC_MVT_LQ.vpy` |
| `vsdlssg` | `!` + `*` Dlss、`(` XeSS-FG、`)` VFG、`_` FSR4 | 上述 + 4 个帧生成 vpy |
| `vs-mlrt` | `!` + `$` RIFE_RTX、`%` DRBA_RTX、`&` UAI_RTX | 上述 + 3 个 TRT vpy |

四个帧生成 vpy 由 k7sfunc 组件带进 stage，所以全量档位包在组件装完之后会再按包名
重算一次配置；`MEMC_RIFE_AJI.vpy`（vsaji 暂停中）会被裁掉。
不可用预设的判定依据：`BM3D_NV` 需要 `bm3dcuda_rtc`、`RIFE_STD` 需要 `rife` 插件
（都未随包），`CCD_STD` 依赖的 `akarin` 已随包；DML 系列需要 vsmlrt DirectML，两个档位都没装。

## 验证记录

- `python -m build config --stage <空目录> --pack vsdlssg`：`启用 5 个预设，移除 15 个未启用的 vpy`，
  生成的两份文件标记区内容正确（启用的四行在前，其余整行注释并注明依赖）。
- 用旧版 `dist/stage/...-vsdlssg/portable_config`（含 20 个 vpy，15 上游 + k7sfunc 5）作为输入
  重算 `vsdlssg`：`移除 15 个未启用的 vpy`，剩下 `MEMC_MVT_LQ`、`MEMC_DLSSG_NV`、
  `MEMC_XESSFG_NV`、`MEMC_VFG_NV`、`MEMC_FSR4`，`MEMC_RIFE_AJI` 被裁掉。
- `vs-mlrt`：剩 `MEMC_MVT_LQ`、`MEMC_RIFE_NV`、`MEMC_DRBA_NV`、`MIX_UAI_NV_TRT`。
- `base`：只剩 `MEMC_MVT_LQ`（不带 `--pack` 时命中；`--pack nope` 报可用 `base, vs-mlrt, vsdlssg`）。
- `python -m build verify --stage <只含 portable_config 的目录>` 报缺 `mpv.exe` 等运行时，
  未对 `input_uosc.conf` / `menu.conf` 误报，说明必需项列表与基准布局一致。
- 按键名可用性：用包内 `mpv.exe` 以 `--input-conf` 载入 `! * ( ) _ ^ & $ % SHARP` 十个键位，
  退出码 0、无解析错误。
- `build.assemble / pack / verify / sync / config / inputs` 全部可导入；
  `inputs_hash` 正常计算。
- 行尾保持：`input_uosc.conf`、`menu.conf` 仍是 CRLF（生成逻辑用 bytes 读写 + 探测 EOL）。

## 未做 / 待办

- 未跑完整 `python -m build ci`（会下载数百 MB 并重打 ~1GB 包，按约定需用户明确要求）。
  需要重新出包时：`python -m build all` 出基础包，`python -m build pack` 出两个档位包，
  再 `python -m build verify --asset ...` 校验。
- 主仓库与 `mpv-playkit` 均只做本地提交，没有 push。

## 附：`base` 不是第三档

CI（`.github/workflows/build.yml`）的流程是 `fetch → assemble → smoke → verify → pack`：
`assemble` 不带 `--pack`，命中配置清单里的 `base`，产出的 stage 只用于冒烟与结构校验，
随后 `pack` 为每个全量档位包新建 stage，先按包名装配基础包、装完档位组件再按包名重算一次配置。
最终上传的 artifact 只有 `*-windows-x64-vsdlssg.7z` 与 `*-windows-x64-vs-mlrt.7z`。
`base` 不能把两个档位的预设并起来：它里面没有 vsmlrt 与各引擎运行时，并进来只会得到
"能按、播不了"的键，正好违背"禁用不可用 vpy"的初衷。
