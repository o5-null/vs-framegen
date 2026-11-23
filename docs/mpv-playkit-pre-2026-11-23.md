# mpv-playkit-pre：自维护 mpv + 两档全量包（2026-11-23）

本次把 `mpv-playkit-pre` 从「上游 shinchiro mpv + 补丁包」改成「自维护 mpv + 两个全量档位包」，
并把 vs-framegen 的四颗帧生成引擎运行时全部并入默认档。

## 交付形态

| 档位 | 产物 | 内容 | 体积 |
| --- | --- | --- | --- |
| 默认（`vsdlssg`） | `mpv-playkit-pre-<ver>-windows-x64-vsdlssg.zip` | 完整播放器（自维护 mpv + Python + VS R82 + K7sfunc）+ 四个帧生成 DLL + DLSS-G 4.5 / XeSS-FG / VFG / FSR-FG 四套引擎运行时 | ~498 MB（stage 915 MB） |
| 可选（`vs-mlrt`） | `mpv-playkit-pre-<ver>-windows-x64-vs-mlrt.zip` | 同上的完整播放器 + `vstrt_rtx`（API4 移植）+ TensorRT-RTX 1.5 + 模型；**不含四颗帧生成引擎运行时** | ~1.09 GB（stage 1.65 GB） |

- 两包**互斥**，各自独立可跑，不再有「基础包 + 覆盖补丁」。
- `PACK_INFO.json` 记 `full: true`；`manifests/packs/vsdlssg.yml` 有 `default: true`，`common.default_pack()` 保证最多一个默认档。
- 四个插件 DLL（`vdlssg`/`vxessfg`/`vvfg`/`vfsr4`）由基础组件 `vs-framegen-dlls` 落在 `vs-plugins\`，任何档位都有。
- 默认档的引擎组件按 `include` 白名单从**同一个** `vs-framegen-0.2.0.zip` 里各取子树：
  `vs-plugins\dlss45\`（10 文件）、`xessfg\`（9）、`vfg\`（4，约 200 MB）、`fsr4\`（23）。
  v0.2.0 起上游把四个引擎合并进一个发行 zip，旧的分引擎资产已不存在。
- `vsaji` 组件本轮 `disable: true`（aji 未定版，引擎也不带）；启用方式 `build pack --name vsdlssg --with vsaji`，需先 pin。

## mpv 自维护构建

- 源码：`E:\code\dlssg-vfi\mpv-src`（fork `o5-null/mpv`，remote `fork`），分支 `master`，含 `vf_playkit`（帧倍增原型、EOF drain、outfmt、D3D11 直通桥、`PLAYKIT_PROXY` 预加载、适配器 `#<index>`）。
- 构建必须带 `-Dvapoursynth=enabled`：`E:\code\dlssg-vfi\build-mpv.sh`（MSYSTEM=CLANG64，`meson setup build-msys --prefix=/clang64 -Dlibmpv=true -Dd3d11=enabled -Dvulkan=enabled -Dshaderc=enabled -Dvapoursynth=enabled`）；依赖包 `mingw-w64-clang-x86_64-vapoursynth 65-8`。
- 出包：`mpv-src\scripts\package_windows.py`（objdump 递归依赖闭包、平铺 zip、固定时间戳 2020-01-01）。
- CI：`mpv-src\.github\workflows\playkit-windows.yml`（仅 windows-latest + MSYS2 CLANG64 x64；push tag `playkit-*` 时编译 → 打包 → 发 release）。上游 `build.yml`（多平台矩阵）与 `fuzz.yml` 已在本 fork 删除（`cd313c6`），只留 lint/docs/comment/cleanup。
- 首个 release：`o5-null/mpv` 的 `playkit-2026.11.23`，资产 `mpv-x86_64-playkit-2026.11.23.zip` 72,519,650 B，sha256 `1f736931960a4d521f73736341a818353afa7b56923cf266daeafba55158cd67`，run 45150619125 全绿。
- mpv 已 pin 到该资产（`sources.lock.json`），`assemble` 直接用缓存出的 zip。
- `vf_playkit.c` 只在 `features['d3d-hwaccel']`（win32）分支加入 sources（`mpv-src/meson.build:1439-1444`），非 Windows 不编译，不需要额外 `#ifdef`。

### 注意：mpv zip 不含 VapourSynth 运行库

zip 只带 `mpv.exe`/`mpv.com`/DLL 闭包。`vf vapoursynth` 的 `vsscript.dll` 是运行时延迟加载，由 playkit 包里 `vapoursynth` whl 复制到包根，所以单独解压 mpv zip 只能看到 `--vf=help` 里的条目，跑 `.vpy` 必须放进包内使用。

## 本机四引擎实测

测试包：本地 `vsdlssg` 全量包解压到 `work\pkgtest\mpv-playkit-pre-2026.11.20-79f8d93-windows-x64-vsdlssg`，
源 `work\bench_analysis\ah_1080p.mkv`（HEVC 1920x816，30 fps），RTX 3080。

统一命令形态（**必须带包内配置**，否则 vsscript 找不到 Python、`~~/` 也不指向包目录）：

```powershell
cd <包根>
.\mpv.com --vo=gpu-next --gpu-api=d3d11 --hwdec=d3d11va --no-audio --frames=180 `
  --vf=vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy" `
  --term-playing-msg='OUT vf_fps=${estimated-vf-fps}' E:\code\dlssg-vfi\work\bench_analysis\ah_1080p.mkv
```

| 引擎 | 脚本 | 默认倍率 | 实测输出 |
| --- | --- | --- | --- |
| DLSS-G 4.5 | `MEMC_DLSSG_NV.vpy` | 3× | `OUT vf_fps=90`（30×3） |
| XeSS-FG | `MEMC_XESSFG_NV.vpy` | 3× | `OUT vf_fps=90` |
| NVIDIA VFG | `MEMC_VFG_NV.vpy` | 2× | `OUT vf_fps=60` |
| AMD FSR-FG | `MEMC_FSR4.vpy` | 2× | `OUT vf_fps=60` |

四颗引擎都真实在插帧，且引擎目录按 `vs-plugins\<引擎>` 被脚本的回退链命中（`MEMC_VFG_NV.vpy:103-107`、`MEMC_FSR4.vpy:106` 先试 `bin\<引擎>` 再试同级）。

注意事项：

- `--no-config` 会跳过 `k7f_vs.lua`，vsscript 报 `Python executable and library path couldn't be determined`；`~~/` 也会指向用户配置目录而不是包内 `portable_config`。
- `--vf=vapoursynth=E:/.../x.vpy` 这种绝对路径写法会被 mpv 当成滤波参数解析而报错（`buffered-frames option must be an integer`），只能用 `~~/vs/...`（带包内配置）或改成相对包目录的路径。
- `vo=null` 没有 D3D11 设备，DLSS-G 会 `CreateDlssgFeature failed (NGX 0xbad00002)`；测帧生成必须 `--vo=gpu-next --gpu-api=d3d11`。
- 容器 fps 不可用的源（如 ffv1 测试片）会触发 `AssumeFPS: invalid framerate specified`（`MEMC_DLSSG_NV.vpy:118` 取到 nan），换正常 fps 的源即可。
- `VDLSSG_LOG=1` 下曾偶发一次 `exit=-1073741819`（0xC0000005），随后同参数连跑三次均 exit 0，暂记为已知 flaky，未定位。

## 期间踩到的坑

- **CI 用 MSYS 版 meson**：`ERROR: This python3 seems to be msys/python on MSYS2 Windows, but you are in a MinGW environment`。改装 `mingw-w64-clang-x86_64-{meson,ninja}`。
- **YAML 折叠标量里插 `#`**：把同一 `run:` 后半段（含包名）整段注释掉，表现为 `meson: command not found`。注释要放在步骤外。
- **CI 的 MSYS2 根目录是 `D:\a\_temp\msys64`**，硬编码 `C:\msys64\clang64\bin\python.exe` 会找不到；Package 步骤改用 MSYS2 外壳 + `cygpath -w /clang64/bin`。
- **k7sfunc 仓库把 `__pycache__/*.pyc` 提交进了 git**，codeload tar 带上后被 verify 拦下。已在 `install_git_tree` 固定 `ignore=["__pycache__", "*.pyc"]`。
- **出 zip 必须晚于最后一次成功链接**：曾用 17:05 的旧 zip 装包，smoke 报 `Option vf: 'vapoursynth' isn't supported`，实际 17:11 的重链接已带 `HAVE_VAPOURSYNTH 1`。
- 本地 urllib 走 `HTTPS_PROXY=http://127.0.0.1:7890` 下载 GitHub release 资产会 `ssl.SSLEOFError`；改用 `gh release download`，或把资产预放进 `.work/downloads/<id>/`。
- 上游 release 资产会被替换：`vs-framegen` v0.2.0 原来的 `*-mpv-lazy.zip` 全没了，只剩合并的 `vs-framegen-0.2.0.zip`；lock 里的旧 URL 直接 404。pin 时留意资产名是否还会变。

## 本地验证结果

- `vsdlssg`（四引擎版）：stage 915.0 MB → zip 498.1 MB，sha256 `49cc0bd4e6c39eb7ae326bb12c0f96e559299e47dca9cc2adbf220e4258f70c4`；`verify --pack` 与 `smoke` 均通过。
- `vs-mlrt`：stage 1653.2 MB → zip 1086.0 MB，sha256 `3ea92d02be528c7169af420940d94553ace342e018b64462ff5f0f6c60d36929`；校验与 smoke 通过（该包不含四颗帧生成引擎）。
- CI：`gh run 45153398904`（headSha `9dd2d60`）**success**，10:08:51Z 起、10:15:23Z 完，artifact `mpv-playkit-pre-windows-x64` 1,580,184,957 B，内含两档 zip。

## 仓库状态

- `o5-null/mpv-playkit-pre`：`ad90d83`（自维护 mpv + 两档全量包）→ `addff9d`（k7sfunc 重钉 11fea6b）→ `a442aef`（剔除 `__pycache__`）→ `c3c0fcb`（文档：首选推 tag 让 CI 出包）→ `79f8d93`（mpv 钉到 playkit-2026.11.23）→ `9dd2d60`（跟上 vs-framegen v0.2.0 合并发行包），均已推。
- `o5-null/K7sfunc`：`11fea6b feat(vpy): 四个补帧脚本接入公用参数桥` 已推（`k7f_bridge.lua`/`k7f_vpypar.lua`/`k7f_bridge.py`）。
- `o5-null/mpv`：`32c60a5`（playkit 桥）、`a9215ea`（打包脚本 + CI）、`16af031`/`d7d5972`/`1e6a683`（CI 修复）、`cd313c6`（只留 Win x64 构建），tag `playkit-2026.11.23`。

## 待办

- `vsaji` 定版后：推 GitHub、发 `dist/aji` release（zip 根 `aji/**`，由 `vsaji/scripts/make-release.py` 生成）、`build.pin --pack vsdlssg --id vsaji`、去 `disable`。
- `VDLSSG_LOG=1` 偶发访问违例继续观察（NGX 初始化、proxy 适配器、引擎包选择均已确认正常）。
- 包内上游配置噪音未修（`vulkan-device`/`include` 报错、`xxxxx.lua` 缺失、`uosc_addones/element_vcs.lua:278` 语法错、`script-opts/k7f_playkit.conf` 的 key 带尾随空格、`shaders/hooks_0*.glsl` 缺失）。
