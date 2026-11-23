# mpv-playkit pre 2026.11.20（b11cbd0）发布记录

## 这一版解决的问题

| 问题 | 根因 | 修法 | 提交 |
| --- | --- | --- | --- |
| 右键菜单里 `VF 滤镜` 只剩「清空」 | `menu.conf` 的 `vf` 标记行生成在根层，预设缩进一层后全挂在 hidden 标记下；`select.lua:1378-1414` 建树、`menu.c:112-188` 直接丢掉 hidden 节点的整棵子树 | 标记行也缩进一层，`build/config.py` 加 `MENU_VF_INDENT` 并在 `check()` 里断言 | `b11cbd0` |
| 并行出档位包时磁盘写满 | 打包 `-mx=9` 让临时盘紧张；`common.make_temp_dir()` 用完不清理，`.work` 累积 51.57 GB | 压缩降到 `-mx=7`（体积 +0.2%）；`install_mpv/install_archive/install_wheel/install_git_tree` 全部 `try/finally` 清理 | `70b810a` |
| 同一进程里改倍数/换引擎，第 3 次初始化必崩 `0xC0000005` | `vs-dlssg/src` 的 CUDA IAT 钩子把真函数指针只存在本 DLL 静态量里，VapourSynth 反复卸载重载插件后静态量归零，IAT 仍指向旧钩子 | 钉住插件映像（`GetModuleHandleExW` + `PIN`）并从 `nvcuda.dll` 重新取函数地址 | 主仓库 `c99ff14` |
| 右键打开的是 uosc 写死的菜单，看不到 `menu.conf` | `input_uosc.conf` 把 `MBTN_RIGHT` 绑给 `uosc/menu` | `MBTN_RIGHT` → `context-menu`，uosc 菜单移到 `Ctrl+MBTN_RIGHT` | `1799779` |
| `k7f_playkit.conf` 全部配置不生效 | 文件里写的是 `key = value`（等号两侧有空格），mpv 报 11 条 `unknown key` | 改成 `key=value` | k7sfunc `8d7eb2a` |

## 新增功能

- **通用倍数菜单**（k7sfunc `78bc8b0` + mpv-playkit `d0ca253`）：`script-opts` 之外的独立脚本 `k7f_multi.lua`，右键菜单 `倍数 (&M)` 提供 `跟随预设 (&F)` 与 `2×`–`6×`；选择写入 `_cache/k7f_multi.json` 持久化，套用任何插帧预设时由 `observe_property('vf')` 自动补上 `user-data=multi=N`（VS 路径）或 `multi=N`（原生 `playkit` 路径）。
- **VFG 开放 2×–6×**：`MEMC_VFG_NV.vpy` 支持 `VVFG_MULTI` 环境变量与 `user-data` 双入口；XeSS-FG 仍限 2×（sm86 后端 3× 触发 `DLSSG.MultiFrameIndex 2 arrived twice for one real frame`，k7sfunc `a01dad1`）。
- **菜单随包不同**：`manifests/config.yml` 的 `packs.*.enable` 决定生成哪些预设行，未启用的预设整行不生成（不再显示 `（本包未启用…）`），键位表同步收窄。

## 产物

| 文件 | 体积 | SHA-256 |
| --- | --- | --- |
| `mpv-playkit-pre-2026.11.20-b11cbd0-windows-x64.7z` | 167.7 MB | `843ad790a09baacd730b5429e6f3c129e43a695193a6dd73aa10670945b4411d` |
| `mpv-playkit-pre-2026.11.20-b11cbd0-windows-x64-vsdlssg.7z` | 347.4 MB | `945782bd9efe548e7c851edb73e35790e58d4eabc2cbc25cde04a1cf3882b8f5` |
| `mpv-playkit-pre-2026.11.20-b11cbd0-windows-x64-vs-mlrt.7z` | 698.6 MB | `6461cfff4d2b39f432b599c799554a2407c4d87aeec9b14bf63175e6dddcae9b` |

发布位置：<https://github.com/o5-null/mpv-playkit-pre/releases/tag/pre-2026.11.20-b11cbd0>（tag 指向 `b11cbd0`）。

## 构建与验证

- 命令：`python -m build all` + `python -m build pack --name vsdlssg --name vs-mlrt`，带两个本地覆盖（`PLAYKIT_LOCAL_K7SFUNC`、`PLAYKIT_LOCAL_VS_FRAMEGEN_DLLS`）。
- 冒烟四项全过：`python 3.15.2`（`k7sfunc 1.9.0+pre.7`、`playkit presets auto,upscale,memc,denoise,deband,clean,anime,live`）、`vapoursynth R82`、`mpv v0.42.0-dev-g3876abfc7`（libplacebo v7.387.0 / FFmpeg 9.1）、`yt-dlp 2026.10.26`，外加 `mpv + vf=vapoursynth` 实跑。
- 归档内配置核对（`7z e -so` 抽配置，不整包解压）：base 预设 1 条、vsdlssg 预设 5 条 + 倍数 + 原生滤镜、vs-mlrt 预设 3 条；三包均为 `MBTN_RIGHT context-menu`。
- 运行期 `menu-data` 实测：`items=27`，`VF 滤镜` 下 5 条预设齐全，标记行 `state=hidden` 且不再吞子树。

## 遗留

- k7sfunc 的五个提交（`c65c024`、`f85f808`、`78bc8b0`、`a01dad1`、`8d7eb2a`）尚未 push，因此 `sources.lock.json` 仍 pin 旧版；包内用的是本地覆盖件，复现需等推送后 `python -m build pin`。
- 通用倍数对上游同步的 MVTools / RIFE / DRBA / UAI / SVP 预设无效（这些 vpy 不解析 `user-data`，且 `portable_config/vs` 属上游同步目录，不手改）。
