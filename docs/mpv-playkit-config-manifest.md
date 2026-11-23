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

## 追加：原生滤镜段（2026-11-23）

### 做了什么

- 配置清单新增 `native` 段：`section` / `clear_label` / `clear_command` / `entries`。
  `build/config.py` 的 `render_native()` 在 `menu.conf` 的 "VF 滤镜" 与 "着色器" 之间
  生成 "原生滤镜 (&P)" 段，内容只有列出的档位包才出现（当前只有 `vsdlssg`）：

  ```
  原生滤镜 (&P)
      清空 (&C)	vf clr playkit; set hwdec auto-copy
      DLSS 帧生成 (&D)
          2× (&2)	set hwdec d3d11va; vf set playkit:multi=2:dll=vs-plugins/vdlssg.dll:proxy_dir=vs-plugins/dlss45
          … 3× / 4× / 5× / 6× 同形
  ```

- 同时修掉两个 `menu.conf` 渲染缺陷（实测 `menu-data` 才发现）：`select.lua` 只按 `\t+`
  切分、不认 `#` 注释，原来 `# >>> playkit-config:vf` 这类标记与 "本包未启用" 注释全变成了
  空菜单项（27 项里有 12 条垃圾）。现在标记行一律加 `hidden=true`；未启用的预设
  **整行不生成**（`render_menu` / `render_input` 直接跳过）：这些 vpy 在装配时已被
  `prune_vpy` 删掉，留着灰条目命令也只会失败，两个文件里都看不到它们。
- `mpv-src: video/filter/vf_playkit.c` 的相对路径解析改成 "先工作目录、再 mpv.exe 目录"
  （`pk_abs_path` / `pk_exe_relative_exists`）：NGX 只认绝对路径，而工作目录会被快捷方式、
  文件关联改掉，便携包从别处启动时原来找不到 `vs-plugins/vdlssg.dll`。

- 右键菜单的入口是 `input_uosc.conf` 的鼠标绑定（`menu.conf` 自己不绑键）：上游默认
  `MBTN_RIGHT → script-binding uosc/menu`（uosc 自带的固定菜单，不读 `menu.conf`）、
  `Ctrl+MBTN_RIGHT → context-menu`。本包把两者**对调**：右键打开 `menu.conf` 菜单
  （插帧预设 / 倍数 / 原生滤镜），Ctrl+右键打开 uosc 菜单。uosc 的功能没有损失——
  `menu.conf` 第一项就是 `显示uosc菜单`（`script-binding uosc/menu`）。
  `menu.conf` → `menu-data` 的解析由 mpv 内置 `select.lua` 完成（见
  `mpv-src/DOCS/man/context_menu.rst`），Windows 上由 `context-menu` 弹原生菜单。

- **VF 预设必须挂在 `VF 滤镜` 之下，不能挂在隐藏标记之下**（2026-11-24 修）：
  `select.lua:1393-1412` 按行首缩进建树，标记行在根层时，缩进一层的 `预设 补帧 …`
  就成了"隐藏标记的子菜单"；而 Windows 原生菜单 `build_menu()` 是先递归 `CreatePopupMenu`
  再判 `if (fState == -1) // hidden continue;`（`mpv-src/video/out/win32/menu.c:150-161`），
  父项被跳过时子菜单随父项一起丢掉 —— 用户看到的就是 `VF 滤镜 > 只显示 清空 (&C)`。
  修法：`portable_config/menu.conf` 里 `vf` 那一对标记行缩进一层（`\t# >>> playkit-config:vf
  …`），预设行与标记同层，都落进 `VF 滤镜 (&F)` 的子菜单；`倍数 (&M)` / `原生滤镜 (&P)`
  是根层段标题，本来就不受影响，保持原样。`build/config.py` 的 `check()` 增加一条断言，
  标记行没有缩进时直接构建失败（否则这个缺陷不会在构建期暴露）。

### 实测（这次是真跑了，不是只看结构）

- 菜单：在 stage 里用 `--config-dir` 指到生成的配置跑 `menu-data` 转储，`items=27`、
  三对标记均为 `state=hidden`，`原生滤镜` 位于 `VF 滤镜` 与 `着色器` 之间
  （当时未启用预设还渲染成 `state=disabled`；本轮改成整行不生成，见上一条）。
- 路径回退：把工作目录换成 `E:\code\dlssg-vfi`（不是 stage 目录）再启动 stage 的 `mpv.exe`，
  滤镜照样加载成功并打出 `playkit: D3D11 adapter ... name=NVIDIA GeForce RTX 3080`、
  `playkit: vdlssg bridge ready 3840x1632 nv12 bits=8 multi=2` —— 证明相对路径确实是按 exe
  目录找到的（改之前这里会报 `cannot resolve bridge paths`）。
- **但是**：桥建起来之后第一帧就报
  `playkit: vdlssg_bridge_process failed (2): OpenSharedHandle (input) failed (HRESULT 0x80070057)`，
  之后每帧都退化成 `duplicating frames`。`vs-dlssg\src\fsr4_vk.h:6-10` 早就记过这堵墙：
  本机（RTX 3080 / 驱动 617.42）D3D12 打不开这些共享面，FSR4 桥因此改走 Vulkan 导入；
  DLSS-G 桥仍用 `D3D12 OpenSharedHandle`，所以零拷贝这条实际不工作，画面是复制帧。
- 小分辨率也会失败：`testsrc` 那种小图会先死在 `CreateDlssgFeature failed (NGX 0xbad00002)`，
  要 1080p 级的源才走到共享面那一步。

### 出包记录

- `mpv-src`：`a67869b`（原 `3876abf`，只因 commit message 不符合上游 `subsystem: 小写` 的
  lint 规则改写，树一样），tag `playkit-2026.11.23-2` 触发的 CI 产物
  `mpv-x86_64-playkit-2026.11.23-2.zip`（78309927 字节，sha256 `4496d055…202f4`）已钉进
  `sources.lock.json`；随包 `mpv.exe --version` 显示 `g3876abfc7`，`--vf=help` 里 `playkit`
  的描述已是 `PlayKit frame multiplier (D3D11 zero-copy DLSS-G)`。
- `mpv-playkit`：`af1e6f0`（清单与生成器）、`e1628f1`（AGENTS/DECISIONS + pin）。
- 两个包本地全流程构建通过（fetch→assemble→smoke→package→verify）：
  - `mpv-playkit-pre-2026.11.20-e1628f1-windows-x64-vsdlssg.7z`
    346611556 字节，sha256 `94dd76a0…c9e5eab`，menu.conf 有 "原生滤镜" 段；
  - `mpv-playkit-pre-2026.11.20-e1628f1-windows-x64-vs-mlrt.7z`
    691737269 字节，sha256 `cdd5467c…66ee0d3`，只有空的 native 标记、没有该段。

### 复现命令

```powershell
# 菜单是否解析正确（stage 目录里跑）
.\mpv.exe --config-dir=<配置目录> --no-terminal --vo=gpu-next --gpu-context=d3d11 `
  --ao=null --frames=20 --script=<dumpmenu.lua> av://lavfi:testsrc=duration=2:size=640x360:rate=30

# 路径回退 + 零拷贝（从别的工作目录启动，看日志里的 playkit: 行）
E:\...\stage\mpv.exe --no-terminal --vo=gpu-next --gpu-context=d3d11 --ao=null --hwdec=d3d11va `
  --vf=playkit:multi=2:dll=vs-plugins/vdlssg.dll:proxy_dir=vs-plugins/dlss45 `
  --frames=40 --log-file=<log> <1080p 以上的视频>
```
