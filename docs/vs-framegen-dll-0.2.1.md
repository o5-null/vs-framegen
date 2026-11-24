# vs-framegen DLL 0.2.1：vvfg 反复初始化崩溃的修复

2026-11-24

## 症状

`vvfg.dll`（NVIDIA VFG 帧生成插件）在同一 mpv 进程里**重复初始化**时崩溃：

- `mpv.exe` 退出码 `-1073741819`（`0xC0000005` 访问违例）；
- Windows 事件日志：`出错模块名称: unknown`、`错误偏移: 0x0`（模块名都取不到，说明调用落在了
  一个已被卸载的映像/悬空指针上）；
- 触发条件：改 `user-data` 里的倍数导致 `vapoursynth` 滤镜重建，**第 3 次初始化**必崩。第一次到
  第二次重建都正常（`0.2.0` 上实测 2×→3× 成功、3×→4× 崩）。

## 对照实验（定性）

| 引擎 | 连续 3 次 `vf clr` + `vf set` | 结论 |
| --- | --- | --- |
| VFG（`vvfg.dll`） | 第 3 次崩 | 只有它有问题 |
| DLSSG / XeSS-FG / FSR4 | 全部 `exit=0` | 不是 VapourSynth 的限制 |

排除了几条"应用层绕行"：`vf clr` 重来、`loadfile` 重开当前文件、先 `enabled=false` 再改参数、
`pause` 等待——都以同样的 `0xC0000005` 结束，说明问题在插件 DLL 自身。

## 根因

`vs-dlssg/src/vfg_session.cpp` 为了让 VFG 在 sm86（RTX 20/30 系）上工作，用 `patchIat` 把
`nvcuda.dll` 的 `cuDeviceGetAttribute`、`cuModuleLoadData` 换成指向本 DLL 的钩子函数：

1. 钩子的**真函数指针**只存在本 DLL 的静态变量里；
2. VapourSynth 每次销毁环境时会 `FreeLibrary` 插件 DLL，mpv 下次重建滤镜又在同一基址重新
   `LoadLibrary`；
3. 重载后静态变量归零（真指针丢失），而 `nvcuda.dll` 的 IAT 槽里仍然写着**上一次**那个指针；
4. `patchIat` 见到"槽已经是自己"就跳过重装 → 钩子调用一个空指针。

## 修法（提交 `c99ff14`）

1. `pinPluginImage()`：`GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
   GET_MODULE_HANDLE_EX_FLAG_PIN, &pinPluginImage, …)` + `std::call_once`，把本映像钉在进程里，
   VapourSynth 卸载时不会真的卸掉，静态量不再归零。
2. `patchIat` 开头无条件用 `GetProcAddress(nvcuda, …)` 重新解出两个真函数指针，不再信任 IAT
   槽里的值——即使映像被重载、槽是旧的，也会被覆盖成当前的正确指针。

## 验证（全部 `exit=0`）

- 连续三次重建滤镜；
- 2× → 3× → 4× → 清空 热改倍数，sidecar `output.fps` 依次 120 / 180 / 240，清空回落 120；
- DLSSG → VFG 换引擎（同进程）；
- `loadfile` 重开当前文件。

## 发行

- 仓库 `o5-null/vs-framegen`，tag `v0.2.1`（`0d152c8`），release
  <https://github.com/o5-null/vs-framegen/releases/tag/v0.2.1>；
- 资产 `vs-framegen-0.2.1.zip`：**253,831,573 B**，
  `sha256 578ccafe694d3dd4ba64131aca41aa9e14b103ec30652a1fc2451d64c7253fc5`；
- 内容与 `v0.2.0` 同构：4 个插件 DLL（`vs-plugins/vdlssg.dll`、`vxessfg.dll`、`vvfg.dll`、
  `vfsr4.dll`）+ 四棵引擎运行时（`vs-plugins/dlss45`、`xessfg`、`vfg`、`fsr4`）；差异只有四个
  DLL 重新编译，另剔除测试产生的 `fsr4/shader_cache`（vkd3d 运行时缓存，不该进发行包）。

## mpv-playkit 侧

- `sources.lock.json` 里指向该资产的两条（`vs-framegen-dlls` 与合并后的
  `vsdlssg/engine-framegen`）一起升到 `0.2.1`，sha256 相同 → 构建不再需要
  `PLAYKIT_LOCAL_VS_FRAMEGEN_DLLS` 覆盖件；
- 决策记在 `mpv-playkit/docs/DECISIONS.md` 第 29 条；事后把四颗引擎的四条
  `vsdlssg/engine-*` 组件合并成一条 `engine-framegen`、并把下载缓存改成按 sha256 命名，
  记在第 30 条。

## 教训

- "只崩这一个引擎"这类对照实验很便宜，先做它再猜"是 VapourSynth 的限制"。
- Windows 事件日志里 `出错模块名称: unknown` 几乎等价于"跳到已卸载映像/悬空指针"，对 DLL
  自卸载 + 静态钩子的组合要格外警惕；跨 `FreeLibrary` 的钩子必须重新解析真函数或钉住映像。
