# 2026-11-25 补帧 vpy 瘦身：宿主逻辑并入 K7sfunc

## 背景

vs-framegen 的四个 mpv 脚本（`MEMC_DLSSG_NV.vpy` / `MEMC_XESSFG_NV.vpy` /
`MEMC_VFG_NV.vpy` / `MEMC_FSR4.vpy`）各自 147–157 行，其中约 5/6 是同一份样板，
改一处就得同步四遍：

1. `sys.path` 注入 + `import k7f_bridge`；
2. 手写 `_parse_user_data()` 兜底；
3. 环境变量覆盖块（`VDLSSG_*` / `VXESSFG_*` / `VVFG_*` / `VFSR4_*`）；
4. `user_data` 覆盖块 + 逐个键的 `max/min` 钳制；
5. 插件候选列表（包根 `vs-plugins/`、脚本旁 `../`、`../../`、开发树 `vs-dlssg/bin/`）；
6. 主流程（pre_h 预缩、YUV 直通、`core.<ns>.FrameGen`、RGB 输出转 `YUV420P10`、
   缺帧率兜底 `AssumeFPS`、`publish` 上报）。

目标是让 vpy 回到 K7 其它脚本的形态：**只写用户选项，末尾一次调用**。

## 改动

### 1. K7sfunc fork（`1.9.0+pre.7` → `1.9.0+pre.8`）

新增 `src/k7sfunc/mod_vpy.py`（vpy 宿主层）：

| 接口 | 作用 |
| --- | --- |
| `VPY_PARSE_USER_DATA(text)` / `VPY_AS_BOOL` / `VPY_GET(...)` | 从原 `k7f_bridge.py` 迁入的 `user_data` 解析与取值（类型转换、非法值提示一次、越界钳制） |
| `VPY_LOAD_PLUGIN(ns, script_file, label=None)` | `core.<ns>` 已存在则跳过；否则按 包根 `vs-plugins/` → 脚本旁 `../../`、`../` → 脚本同目录 → 开发树 `vs-dlssg/bin/` 的顺序 `LoadPlugin`，失败 `vs.Error("MEMC_XXX: 找不到 xxx.dll")` |
| `VPY_SIDECAR_PATH` / `VPY_CLIP_INFO` / `VPY_PUBLISH` | 上行报告（`<脚本>.k7f.json`，原子替换 + 重试），字段与格式不变 |
| `FRAMEGEN_VPY(...)` | 四个脚本共用的宿主：解析优先级、钳制、pre_h 预缩、YUV 直通、`FrameGen` 调用、帧率兜底、上报 |

`FRAMEGEN_VPY` 的关键语义：

- 优先级：**脚本字面量 < 环境变量 `<NS>_<KEY>` < `user_data`**，之后按引擎允许表再钳一次。
- 每个引擎只读自己的键（`FRAMEGEN_VPY_OPTIONS`）：FSR4 没有 `multi` 通道（固定 2×），
  XeSS-FG / VFG 没有 `preset`，只有 FSR4 有 `camera`。
- YUV 输入直通进引擎（不再做 RGBS 往返，省两趟全分辨率转换），只有 RGB 源才转 `YUV420P10`（709）。
- 传入 `fps_in > 1` 时用容器帧率，否则退回 `input.fps`；输出没有帧率才 `AssumeFPS(fps * multi)`。
- OSD 摘要与 `params` 字段顺序保持旧脚本的样子（`DLSSG` / `XeSS-FG` / `VFG` / `FSR4` 前缀不变）。

其它：

- `mod_memc.py`：`FRAMEGEN_ENGINES["vxessfg"]["multi"]` 由 `[2,3,4]` 改为 `[2]`，
  `XESSFG_NV` 的 `multi` 类型同步收窄（sm86 上 3×/4× 会触发 `0xe0d55001` 引擎自杀，
  脚本层早就只放行 2×，模块层这次对齐）。
- 删除 `mpv/portable_config/vs/k7f_bridge.py`（逻辑已进包）。
- `FORK.md` 更新改动表、倍率表、参数桥章节；`k7f_bridge.lua` / `k7f_vpypar.lua` 只改注释指向新入口。

### 2. 四个 vpy

新形态（DLSSG 42 行，其余 39 行）：头注释 → `import k7sfunc as k7f` → `## 用户选项`
（原有注释逐条保留）→ 一次 `out = k7f.FRAMEGEN_VPY(...)` → `out.set_output()`。

`k7sfunc/mpv/portable_config/vs/` 与 `vs-dlssg/mpv/` 两份内容一致，均删掉 `k7f_bridge.py`。

### 3. mpv-playkit

`docs/DECISIONS.md` 追加第 31 条记录本次合并（决策 / 原因 / 代价），第 18 条原文未改。

## 验证

- 四个 vpy `compile()` 通过；`import k7sfunc` 得到 `1.9.0+pre.8`，`FRAMEGEN_VPY` 来自 `mod_vpy`。
- 用 stub core（模拟 `Fmt` / `Node` / 引擎）跑了 5 组断言，覆盖：
  YUV + `pre_h` 预缩 + `user_data` 覆盖（摘要 / params / 帧率兜底逐项比对）、
  RGB8 源（进 RGBS、出 `YUV420P10`、不额外 `AssumeFPS`）、
  FSR4 倍率强制 2 与相机字段、XeSS-FG 倍率钳到 2、缺插件时的报错文案 `MEMC_VFG: 找不到 vvfg.dll`。
  全部通过（`mod_vpy 自检通过`）。
- 真实 VapourSynth（`.venv`，R74）+ 真实 `vs-dlssg\bin\vdlssg.dll`：1280x544 30fps 源取 12 帧，
  `user_data="multi=2;scene=0.3;pre_h=360;log=0"` → 输出 848x360（预缩宽取偶）24 帧 60fps，
  `get_frame()` 逐帧取过，sidecar `summary` 为 `2× · 预设 a · 切镜 0.3 · 预缩 360p`。
- 真机 mpv（`测试环境\mpv-lazy`，把 fork 装进 `Lib\site-packages`）：四个脚本各跑一遍，
  `estimated-vf-fps` 都是源 30fps 的 2×（FSR4 也跑通），sidecar 的 `title` / `engine` / `params` / `summary` 与预期一致；
  `script-message-to k7f_vpypar set multi 3` + `apply` 后实测 90fps、sidecar 变 `multi=3`，播放中途重建不中断。
- 「叠成两条」回归：启动时同时塞两条 `vapoursynth` 滤镜（旧 bug 里 2×2 会变 4× 的场景），
  `k7f_vpypar` 打出「清理了 1 条重复的 VDLSSG 滤镜」，最终只剩 1 条、90fps 而非 6×。
- 测试中发现 `FRAMEGEN_VPY` 不传 `preset` 时 sidecar 会写出 `预设 None`：在 `_resolve_framegen_values()` 末尾补
  `vdlssg` 缺省回落到 `a`（与旧脚本 `Preset="a"` 一致），四个引擎的摘要都不再出现空字段。
- 未跑全量构建（`python -m build ci`）与播放器打包。

## 代价与边界

- vpy 不再能单独拷出 `vs/` 目录运行，必须有 `import k7sfunc`（包里已是常态）。
- 参数键名、`user-data` 语法、`<脚本>.k7f.json` 字段与格式都没变，mpv 侧 lua 不用改。
- `vs/` 与 `src/k7sfunc` 都是整目录复制，删文件不需要改 `components.yml`。
- `k7f_playkit.lua` 生成的临时脚本与 `MEMC_RIFE_AJI.vpy` 仍未接上行报告，与之前一致。
