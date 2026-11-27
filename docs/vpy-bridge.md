# vpy ⇄ mpv 参数桥（k7sfunc.mod_vpy）

给 K7 的补帧脚本和 mpv 前端提供两条**公用**通道：

- **下行（mpv → vpy）**：`vf=vapoursynth=…:user-data="k=v;k=v"`，vpy 里读全局 `user_data`。改值要重建滤镜。
- **上行（vpy → mpv）**：vpy 把**实际生效**的参数写到与脚本同目录的 `<脚本名>.k7f.json`，mpv 脚本轮询读取并显示到 OSD。

两条通道都收在 `k7sfunc.mod_vpy`（vpy 侧）与 `k7f_bridge.lua`（mpv 侧）里。
补帧脚本只写用户选项，剩下的一次 `k7f.FRAMEGEN_VPY(...)` 全包了（解析、钳制、插件定位、主流程、上报）。

## 1. 文件与部署

| 文件 | 位置 | 作用 |
| --- | --- | --- |
| `mod_vpy.py` | K7sfunc 包内（`Lib/site-packages/k7sfunc/`） | vpy 侧：`FRAMEGEN_VPY` 宿主、`VPY_PARSE_USER_DATA` / `VPY_GET`、`VPY_LOAD_PLUGIN`、`VPY_PUBLISH` |
| `k7f_bridge.lua` | `portable_config/scripts/` | mpv 侧：拼 `user-data`、轮询/格式化报告 |
| `k7f_vpypar.lua` | `portable_config/scripts/` | 现成前端：按 `vf` 链自动认引擎、OSD 菜单 + 自愈 + 显示“设置/实际” |

部署时两个 lua 都要在，且需要在 `mpv-playkit/manifests/components.yml` 的 k7sfunc `paths`
白名单里逐条列出，否则不进包；`mod_vpy.py` 跟着 `src/k7sfunc` 整目录复制，不用单列。
`vs/` 目录同样是整目录打包，四个 `MEMC_*.vpy` 自动进包。
`vs-dlssg/mpv/` 是开发版源码，`k7sfunc/mpv/portable_config/` 是随包发布版，两边改动要同步。

## 2. 下行：`user-data` 约定

- 格式：`k=v;k=v`，键名不分大小写；值里**不能有空白、`:`、`,`、`;`、`=`、`"`**。
  这些字符会破坏 vf 子参数解析——`k7f_bridge.lua` 的 `build_user_data()` 会跳过非法值并 warn。
- 需要原样传递含这些字符的内容时，改用环境变量（不进 vf 串），不要在 `user-data` 里硬塞。
- 优先级由 `FRAMEGEN_VPY` 统一决定：**脚本字面量 < 环境变量 < `user-data`**（顺带按引擎表钳制）。
- 改任意值都要重建链上那条 `vapoursynth` 滤镜（按 `params.file` 认，不看标签），
  引擎会重新初始化（百毫秒到秒级卡顿）。seek 会重跑 vpy，但 `user-data` 不变，参数不会丢。

vpy 侧读取（补帧脚本不用自己写这一段，`FRAMEGEN_VPY` 内部已做）：

```python
import k7sfunc as k7f

_ud = k7f.VPY_PARSE_USER_DATA(globals().get("user_data"))
Multi = k7f.VPY_GET(_ud, "multi", Multi, int, 2, 6)     # 越界/非法值保持默认
Scene = k7f.VPY_GET(_ud, "scene", Scene, float, 0.0)
Log   = k7f.VPY_GET(_ud, "log", Log, k7f.VPY_AS_BOOL)
```

`VPY_GET()` 的 `cast` 支持 `int/float/bool/str`；`int` 会先过 `int(float(x))`，兼容 `"4.0"`。
非法值只警告一次，不抛异常。`FRAMEGEN_VPY` 还额外读环境变量（`<NS>_<KEY>`，如 `VDLSSG_MULTI=4`），
优先级是 **脚本字面量 < 环境变量 < `user-data`**，最后按引擎允许表统一钳制
（multi 按 `FRAMEGEN_VPY_OPTIONS`、scene 0–1、其余下限 0）。

## 3. 上行：`.k7f.json` 报告

### 3.1 为什么是文件

`vf_vapoursynth` 没有 `.command` 回调，vpy 也读不到 mpv 属性，没有直接回写通道。
写同目录文件 + mpv 轮询是唯一不依赖 IPC、不跨线程的方案；500 ms 一次轮询、内容没变就不解析，成本可以忽略。
文件名必须是 `<脚本名>.k7f.json`，因为 vpy 不知道 `~~`，而绝对路径含盘符冒号，不能塞进 `user-data` 回传。

### 3.2 字段

只增不改，读取方按需取：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `v` | int | 报告格式版本，当前 `1` |
| `ts` | float | 写入时间（unix 秒） |
| `pid` | int | 写报告的进程，便于确认不是旧文件 |
| `script` | str | 脚本文件名 |
| `ok` | bool | 初始化是否成功；`false` 时 OSD 显示 `⚠ 初始化失败` |
| `title` | str | OSD 前缀，如 `DLSSG` |
| `engine` | str | 实际引擎，如 `vdlssg` |
| `params` | object | 实际生效参数 |
| `summary` | str | 一行摘要（引擎侧自己拼，如 `3× · 切镜 0.25`） |
| `input` / `output` | object | `{w,h,fps}`；拿不到 fps 时省略 |
| `note` | str | 额外提示，如降级原因、预缩高度 |

### 3.3 vpy 侧

补帧脚本只需要一次调用，报告由 `FRAMEGEN_VPY` 内部发出：

```python
import k7sfunc as k7f

out = k7f.FRAMEGEN_VPY(
    video_in, __file__, engine="vdlssg", fps_in=container_fps,
    user_data=globals().get("user_data"),
    multi=Multi, scene=Scene, preset=Preset, pre_h=Pre_H, gpu=Gpu, log=Log,
)
out.set_output()
```

自己写算子的脚本（不经 `FRAMEGEN_VPY`）可以只接上行：

```python
k7f.VPY_PUBLISH(
    __file__,
    title="DLSSG", engine="vdlssg",
    params={"multi": Multi, "scene": Scene, "preset": Preset,
            "pre_h": Pre_H, "log": Log},
    summary=f"{Multi}× · 预设 {Preset} · 切镜 {Scene:g}",
    in_info=k7f.VPY_CLIP_INFO(video_in, fps=fps_in),
    out_info=k7f.VPY_CLIP_INFO(out),
)
```

- `VPY_PUBLISH()` 自己吞掉所有异常（目录不可写、文件被占用等），失败最多让 OSD 少一行，不影响播放。
- 写入是“临时文件 + `os.replace`”，Windows 读取占用时重试 3 次，再退回直接覆盖。
- `VPY_CLIP_INFO(clip, fps=None)`：`video_in.fps` 常是 0，容器帧率要用 `fps_in` 显式传入。
- 每次 seek 重跑脚本都会重写报告，时间戳/pid 会变；前端只在文本变化时更新。

### 3.4 mpv 侧

```lua
local B = require 'k7f_bridge'

-- 拼下行参数
local ud = B.build_user_data(state, { "multi", "preset", "scene", "pre_h", "log" })

-- 轮询上行报告
local timer = B.watch_status("~~/vs/MEMC_DLSSG_NV.vpy", 0.5, function(data, path)
    if not data then return end          -- 文件消失
    mp.osd_message(B.format_status(data), 3)
end)
```

`format_status(data, fallback_title, hide_title)` 的输出顺序：
`标题 ｜ 摘要 ｜ 1080p→1440p ｜ 24→72fps ｜ 提示 ｜ 实测 xx.xfps`。
`实测` 取 mpv 的 `estimated-vf-fps`，暂停/未出帧时自动省略。

现成的 `k7f_vpypar.lua` 已经把两边接好：

- 引擎：按 `vf` 链里 `vapoursynth` 的 `params.file` 文件名认（`MEMC_DLSSG_NV.vpy` /
  `MEMC_XESSFG_NV.vpy` / `MEMC_VFG_NV.vpy` / `MEMC_FSR4.vpy`），参数候选按引擎给；
  链上没有 vpy 时用 `script-opts k7f_vpypar-script` 兜底（默认 DLSSG）；
- 状态：`~~/_cache/k7f_vpypar.json` 按脚本名分桶，旧的扁平结构读入时归 DLSSG；
  `multi` 归 `k7f_multi.lua` 管，链上已有时以链为准；
- 改参后 5 秒内收到新报告，弹一次 `实际：…`；
- `script-message-to k7f_vpypar status` 输出两行：`设置：…` 和 `实际：…`；
- 找不到滤镜时清空 `remote`，不会一直显示上一部片的报告。

## 4. 排错

| 现象 | 检查 |
| --- | --- |
| OSD 一直没有“实际”行 | 报告文件是否存在；`vs/` 是否可写；脚本是否走了 `FRAMEGEN_VPY`（或自己调 `VPY_PUBLISH`）；`import k7sfunc` 是否成功 |
| 报告是旧的 | 看 `pid`/`ts`；seek 后脚本重跑才会更新；换脚本后旧 sidecar 不会自动删 |
| `多参数被跳过` warn | 值里有空白或 `: , ; = "`，换成不含这些字符的值，或改走环境变量 |
| `input.fps` 为 0 | 调用 `FRAMEGEN_VPY` / `VPY_CLIP_INFO(video_in)` 时没传 `fps_in` |
| 加载脚本报找不到 `k7f_bridge` | `require` 失败时 `k7f_vpypar` 会按脚本同目录、再按 `~~/scripts/` 兜底；命令行 `--script=<绝对路径>` 不会把目录加进 `package.path` |
| 加载脚本报找不到 `k7sfunc` | vpy 里只有 `import k7sfunc as k7f`，不再有同目录 `k7f_bridge.py`；确认包内有 `Lib/site-packages/k7sfunc/mod_vpy.py` 且版本 ≥ `1.9.0+pre.8` |
| 报 `MEMC_XXX: 找不到 xxx.dll` | `VPY_LOAD_PLUGIN` 依次找 包根 `vs-plugins/`、脚本旁 `../`/`../../`、开发树 `vs-dlssg/bin/`；确认 `vs-framegen-dlls` 组件已装 |
| 改了参数没生效 | 先看 `status` 的“设置”是否正确，再看“实际”是否是旧报告；重建失败看控制台 |

## 5. 边界

- 轮询周期默认 0.5 s，OSD 更新有最多半秒延迟；这是显示层，不参与控制回路。
- 报告文件与 vpy 同目录，`vs/` 只读时上行会失效（下行不受影响）。
- `k7f_playkit.lua` 生成的 `~~/k7f_playkit.vpy` 目前还没有接 `VPY_PUBLISH()`，所以 playkit 滤镜没有“实际”行；
  接的时候把生成脚本路径和最终 `engine` 传进去即可，宿主本身不用改。
- 报告只是状态快照，不是控制接口；要在线改参仍然只能重建滤镜。
