# mpv.net 版 vs-framegen 包（2026-11-26）

## 目标与结论

按 (m00001) 的要求做一个以 **mpv.net 上游预构建**为外壳、VapourSynth 版本与当前 mpv-playkit 包一致、
**不含 vf_playkit 原生滤镜**、只走 `vf=vapoursynth` 补帧的本地版本。

结论：已在 `E:\code\dlssg-vfi\dist\mpv.net-playkit-2026.11.26-vsdlssg` 构建完成并通过验证，
VS 滤镜（MVTools 与 DLSS-G 两条通路）在 mpv.net 内正常出帧；未推送、未上传。

## 上游与版本对齐

| 项 | 取值 |
| --- | --- |
| 外壳 | mpv.net v7.1.2.0（2026-01-09）portable x64，zip 114,173,733 B，sha256 `33cba35bc6a8ebbb441a68cb9f3d26c366a2457f6a969ce50b95b0e7dd163431` |
| 内置 mpv | v0.41.0-60-g85bf9f4ff，ffmpeg N-122394-g272c273d3，`-Dvapoursynth=enabled` |
| VapourSynth | R82（API R4.3），来自 `mpv-playkit\dist\stage\mpv-playkit-pre-2026.11.25-993e00d-windows-x64-vsdlssg` |
| Python | 3.15.2 嵌入式（python315.dll + python315.zip + python315._pth） |
| 补帧脚本 | portable_config\vs\ 五个 vpy + k7sfunc + k7f_*.lua，与 2026.11.25 包逐字节相同 |
| 引擎包 | vs-plugins\dlss45（DLSS-G sm86 解锁）、fsr4、vfg、xessfg，全部保留 |

mpv.net 的 libmpv 对 `VSScript.dll` 是 **delay-load**（`dumpbin /imports libmpv-2.dll` 只有一条
`0 getVSScriptAPI`），因此把 R82 的 vsscript.dll 放在 mpvnet.exe 同目录即可被找到；
是否兼容只取决于 `getVSScriptAPI(4.x)` 是否返回非空。

## 构建步骤（可复现）

1. 下载并解压上游 portable 包到 `work\mpvnet\x64\`。
2. 目标目录 = 上游全部文件 + 从 2026.11.25 stage 复制：
   - `python.exe/pythonw.exe/python3.dll/python315.dll/python315.zip/python315._pth`、全部 `*.pyd`
   - `vsscript.dll`、`libvapoursynth.dll`、`libvapoursynthfilters{,_avx2,_zn4}.dll`
   - `Lib\`（site-packages：vapoursynth、k7sfunc、插件）、`vs-plugins\`、`portable_config\`、
     `portable.vs`、`BUILD_INFO.json`、`licenses\`
3. 补 Python 扩展模块的系统外依赖（见下一节）：`libffi-8.dll`、`sqlite3.dll`、`libssl-3.dll`、
   `libcrypto-3.dll`。
4. 删除 `portable_config\menu.conf` 中 `# >>> playkit-config:native` … `# <<< playkit-config:native`
   整段（“原生滤镜”菜单，命令是 `vf set playkit:...`）。`input_uosc.conf` 只有 VF 预设段，无需改动。
5. 目录根部放 `MPVNET_VARIANT.txt` 说明本包与 mpv-playkit 包的差异。

## 首个失败与根因

初始装配后冒烟直接报：

```
[v][vapoursynth] using 24 concurrent requests.
[f][vapoursynth] Could not initialize VapourSynth scripting.
[e][user_filter_wrapper] Creating filter 'vapoursynth' failed.
```

`vf_vapoursynth.c` 里这句对应 `getVSScriptAPI(VSSCRIPT_API_VERSION)` 返回 NULL，即 vsscript 的
Python 环境起不来。用探针程序（`work\mpvnet\vsapiprobe.c`，直接 `LoadLibrary + GetProcAddress`）
在目标目录里跑，1.x–5.x 全部返回 NULL；把同一份 vsscript.dll 放进已知可用的 stage 目录却只有
4.x 返回非空 —— 说明 DLL 本身没问题，是目标目录缺少运行时依赖。

进一步定位：包内 `python.exe -c "import vapoursynth"` 直接挂在 `import ctypes`：

```
ImportError: DLL load failed while importing _ctypes: 找不到指定的模块。
```

即 mpv.net 外壳不带 mpv-playkit 那套平铺 DLL，而嵌入式 Python 的 `_ctypes/_ssl/_hashlib/_sqlite3`
需要 `libffi-8.dll / libssl-3.dll (+libcrypto-3.dll) / sqlite3.dll`。补齐后：

- `python.exe` 里 `import vapoursynth` → `vapoursynth R82 / Core R82 / API R4.3`
- 探针 `getVSScriptAPI(4.0..4.3)` → 非空，与 mpv 编译期常量匹配。

## 验证

测试素材：`E:\code\dlssg-vfi\测试数据\ah_1080p10.mkv`（复制到 ASCII 路径
`work\mpvnet\media\ah_1080p10.mkv`，避免 mpv.net 以本地代码页传参导致路径乱码）。
mpv.net 是 GUI 程序且 `idle=yes`，播完不会自行退出，测试用
`work\mpvnet\run-test.ps1` 启动、等待、再 `taskkill /T /F` 收尾。

统一命令行（另加 `--log-file=... --msg-level=all=v`）：

```
mpvnet.com <media> --vf=vapoursynth="~~/vs/MEMC_MVT_LQ.vpy" --frames=15 --no-audio --vo=null --keep-open=no
```

### 1) MVTools 预设（最终状态复测）

```
[v][vf]   vapoursynth (vapoursynth.00)
[v][vf] [vapoursynth] 1920x816 p010 bt.709/bt.709/bt.1886/limited/display CL=mpeg2/4/h264
[d][vapoursynth] initialized.
[v][cplayer] first video frame after restart shown
[v][cplayer] finished playback, success (reason 0)
```

### 2) DLSS-G 预设（最终状态复测）

```
[d][vapoursynth] initialized.
[v][cplayer] first video frame after restart shown
[v][cplayer] finished playback, success (reason 0)
```

插件侧日志（`vs-plugins\dlss45\dlssg_sm86\logs\dlssg_sm86.log`，进程名 `mpvnet.exe`）：

```
event fg_gate_create_feature  feature_id 11  parameters {width:1920,height:816}  status 0x1 succeeded true
event feature_create          create_span_ms 55.6  optimized_kernels true
event fg_gate_summary         create_calls 2  create_failures 0  last_create_status 0x1  reason "first_evaluate"
```

`fg_gate_create_feature` 是“真正打开帧生成”的调用，`reason=first_evaluate` 表示首帧的 evaluate
已发生（同一次运行记录 64 条 `cu_chain_launch`），即帧是在 mpv.net 进程里由 vdlssg.dll +
DLSS-G 运行时真实生成的，不是空跑。

### 3) 配置侧

- 启动日志行：`k7f_vpypar: 已加载 VDLSSG，当前：倍数 4× …`，`k7f_vs: 已写入 VapourSynth 运行时配置`。
- 菜单中已无 `playkit:` 命令：`findstr /S /I "playkit:" portable_config\*.conf` 无命中。

## 键位与菜单（2026-11-26 追加修复）

**症状**：右键没有 mpv.net 的上下文菜单，`Settings > 配置编辑器 / 输入编辑器 / 打开配置目录` 全部缺失，
Ctrl+, 无效。

**根因**：mpv.net 把配置目录的 `input.conf` 与内置默认键位/菜单表合并，然后以 `memory://`
形式作为 libmpv 的 `input-conf` 传入（`Player.cs:105-121`，`InputConf.GetContent()`）。
原包 `portable_config\mpv.conf:5` 的 `input-conf = "~~/input_uosc.conf"` 优先级更高，
把整张表换成 PlayKit 的那份：默认段实测 208 条绑定里只有 PlayKit 条目，
`MBTN_RIGHT -> show-menu`、`Ctrl+, -> show-conf-editor` 一条都不剩（IPC `get_property input-bindings` 取证）。
命令本身是好的：`script-message-to mpvnet show-conf-editor` 能弹出可见的 "Config Editor" 窗口。

**修复**：
1. `portable_config\mpv.conf` 注释掉 `input-conf`，改由 mpv.net 自己组表。
2. 新增 `portable_config\input.conf`（由 `input_uosc.conf` 派生），mpv.net 会把它与内置默认表合并，
   于是右键菜单、Ctrl+,、`k`（编辑 input.conf）、`Ctrl+f`（打开配置目录）全部回来，
   PlayKit 的 `! * ( ) _` 等 VF 预设键位照旧。
3. 右键归属：`MBTN_RIGHT` = mpv.net 菜单；`Alt+MBTN_RIGHT` = PlayKit 的 `context-menu`
   （menu.conf 的 VF/倍数菜单）；`Ctrl+MBTN_RIGHT` = uosc 菜单。
4. `Ctrl+Shift+v`（`update-clipboard`）在 mpv.net 自带的 mpv 构建里没有该命令，已停用；
   剪贴板加载用 mpv.net 自带的 Ctrl+v / Ctrl+Shift+v。

**取证**：IPC `input-bindings` 共 208 条，其中 31 条指向 `mpvnet` 脚本，
含 `MBTN_RIGHT -> script-message-to mpvnet show-menu`（priority 21，uosc 的同键绑定优先级 -1，不生效）、
`Ctrl+, -> show-conf-editor`、`Alt+MBTN_RIGHT -> context-menu`、`! -> MEMC_MVT_LQ.vpy`。
`input-conf` 属性值以 `o script-message-to mpvnet open-files  #menu: 文件 > 打开文件...` 开头，
说明菜单语法已进入 libmpv 的表。修复后重跑 MVTools / DLSS-G 预设，日志
`[vapoursynth] initialized.`、无 `[e]/[f]`，确认键位改动没有影响补帧通路。

**仍存在的键位覆盖**（PlayKit 优先，均为有意保留）：`c`（音频延迟，mpv.net 默认编辑 mpv.conf）、
`Ctrl+i`（uosc 按键列表，默认输入编辑器）、`Ctrl+s`（截屏，默认停止）。
配置编辑器统一走右键菜单 `设置` 或 Ctrl+,。

**注意**：已在运行的实例不会热加载这些改动，改完必须重启 mpvnet.exe。

## 目录与体积

总 967.5 MB。主要构成：`vs-plugins` 317 MB、`Lib` 163.5 MB、`portable_config` 162.8 MB、
`mpvnet.exe` 158.7 MB、`libmpv-2.dll` 111.7 MB。相比 mpv-playkit 包，多出 mpv.net 外壳，
少掉 180 个平铺 DLL（但保留 4 个 Python 依赖）。

## 已知事项 / 取舍

- **mpv.net 不会自行退出**：`idle=yes` + GUI 常驻。自动化验证必须外部 taskkill，
  或改用 `--frames` 后手动关闭窗口。
- **首次启动会写 `%APPDATA%\vapoursynth\vapoursynth.toml`**（k7f_vs.lua 负责），
  这是 R82 的 Python 定位机制；换机器/改目录后脚本会按新路径补一条。
- **VC 运行库**：`libvapoursynth/vsscript` 依赖 `MSVCP140.dll`。2026.11.25 包本身也没有内置它，
  本包保持一致（依赖系统 VC++ 2015-2022 运行库）。若要完全绿色，需要另外补 `MSVCP140.dll`。
- **缓存沿用 2026.11.25 包**：`portable_config\_cache\k7f_multi.json` 里是 4×，
  所以第一次开 DLSS-G 预设是 4×；需要 3× 就在菜单“倍数”里改。
- 保留上游 `*.pdb` 与 `Locale\`，未做裁剪，便于对齐官方发行物排查。
- 只验证了“滤镜能初始化、能出帧、DLSS-G 能建 feature 并 evaluate”。没有做画质/性能对照测试
  （同一素材在 mpv-playkit 包下的耗时对比不在本次范围内）。

## 产物位置

- 可运行目录：`E:\code\dlssg-vfi\dist\mpv.net-playkit-2026.11.26-vsdlssg`
- 验证日志：`E:\code\dlssg-vfi\work\mpvnet\{final-mvt.log,final-dlssg.log,test-dlssg*.log}`
- 测试辅助脚本：`E:\code\dlssg-vfi\work\mpvnet\{run-test.ps1,vsapiprobe.c,analyze_log.py,show_summary.py}`
- 未打包 7z、未推送。
