# 用 OSD 改 vpy 参数：可行性与做法

问题：能不能在 mpv 里用 OSD 界面调 `MEMC_DLSSG_NV.vpy` 这类补帧脚本的 `Multi/Scene/Pre_H/Preset/Log`，让 vpy 读到界面上改的值？

结论：**能**，但只有一条通路真正可用——`vf=vapoursynth` 的 `user-data` 子参数 + 改参后重建滤镜。
mpv 没有任何「把属性实时喂进 vpy」的接口，所有"实时"方案都是自己搭的旁路。下文把四条路逐一判定、给出实现与代价。

## 1. 四条通路

| 方案 | 能否做到 | 关键限制 |
|---|---|---|
| A. `user-data` 传参 + `vf` 重建滤镜 | ✅ **采用** | 改参=重新初始化引擎（秒级卡顿）；`multi` 只能这样改 |
| B. `vf-command @vpy custom …` 回调脚本内函数 | ❌ 不可能 | 官方明文「currently, this only works with the lavfi filter」；`vf_vapoursynth.c` 的 `mp_filter_info` 没有 `.command` 字段 |
| C. vpy 内 `FrameEval` 按帧切到另一张预建滤镜图 | ⚠️ 可行但代价大 | 每套参数常驻一份模型与缓冲（显存翻倍）；`multi` 改变输出帧数，时间基不能中途变，不可用 |
| D. vpy 内起线程连 mpv IPC（`observe_property`） | ⚠️ 能，但不建议 | 脚本顶层由 mpv core 线程执行，同步请求 IPC 必自锁；每次 seek 重载脚本会漏线程/管道；公开实现里找不到先例 |
| E. vpy 每 N 帧读一次 Lua 写的小文件 | ⚠️ 可行 | 轮询延迟、磁盘 IO；只适合低频参数 |
| F. vpy 写 `<脚本>.k7f.json`，mpv 脚本轮询显示 | ✅ **采用（上行）** | 只回传状态，不参与控制；最多 0.5 s 延迟，详见 `docs/vpy-bridge.md` |

被排除的前提：**`open_props` 不存在**。`vf=vapoursynth` 只有 `file:buffered-frames:concurrent-frames:user-data` 四个子参数（`vf.rst` 与源码 `vf_vapoursynth.c:805-815` 一致），也没有"初始化时展开成 mpv 属性名"的机制。脚本启动时能拿到的只有 `video_in / video_in_dw / video_in_dh / container_fps / display_fps / display_res / user_data`。

## 2. `user-data` 的实际行为（实测）

- 它在每次滤镜初始化时被写进 VS 变量表，脚本里是全局 `user_data`，**永远是字符串**，默认 `''`。
- 值里含 `=` 或 `;` 时，`vf` 串里必须整体加双引号，否则 mpv 按子参数分隔符切分：

  ```
  --vf=vapoursynth=ud_test.vpy:user-data=multi=4      → Error parsing option vf
  --vf=vapoursynth=ud_test.vpy:user-data="multi=4;scene=0.35"   → 脚本收到 'multi=4;scene=0.35'
  ```

- 含 `:` 或 `,` 会被 vf 解析吃掉（这两个是滤镜串的分隔符），参数值里不能用。
- **每次 seek 都会重跑 vpy**（`vf.rst:514`「The script will be reloaded on every seek」）。这意味着 seek 之后滤镜按当前 `user-data` 重新初始化——参数不会丢。
- `--vf=` 里的 `~~/` 只在有配置目录时展开；用 `--no-config` 测试时要么给绝对路径，要么用相对路径（Windows 盘符的冒号会被当分隔符）。

## 3. 实现

### 3.1 vpy 侧：`_parse_user_data()`

> 本节记录的是改造**之前**的做法（四个脚本各自解析、各自钳制）。现在脚本里只剩用户选项，
> 解析、钳制、插件定位、主流程与上报都在 `k7sfunc.mod_vpy.FRAMEGEN_VPY` 里，
> 详见 `docs/vpy-bridge.md`。下面是当时的脚本内实现，保留作为语义参考。

`vs-dlssg/mpv/` 下四个脚本（DLSSG / XESSFG / VFG / FSR4）都在环境变量块之后套了一层 user-data 覆盖。
解析优先走 `k7f_bridge.py` 的 `parse_user_data()` / `get()`，环境里没有桥文件时才退回脚本内的 12 行手工解析：

```python
_ud = _parse_user_data(globals().get("user_data", "") or "")
if "multi" in _ud:
    Multi = max(2, min(6, int(float(_ud["multi"]))))
if "scene" in _ud:
    Scene = max(0.0, float(_ud["scene"]))
...
```

优先级：**脚本字面量 < 环境变量 `<VDLSSG_*>` < `user-data`**。键名不分大小写，分隔符 `;` / `,` / 空格都可以。
VFG 固定 2×、FSR4 固定 2× 所以只认 `scene/pre_h/gpu/log`（FSR4 另有 `camera`）；`preset` 只有 DLSSG 认，
其它脚本收到会忽略。

### 3.2 mpv 侧：`vs-dlssg/mpv/k7f_vpypar.lua`

一个脚本完成「OSD 菜单 → 改状态 → 重建滤镜」：

- 多引擎：按 `vf` 链里 `vapoursynth` 的 `params.file` 文件名认引擎（DLSSG / XeSS-FG / VFG / FSR4），
  没挂任何 vpy 时用 `script-opts k7f_vpypar-script` 兜底。参数候选按引擎生成：
  `multi`（DLSSG/VFG 2–6，XeSS-FG 只有 2，FSR4 无）、`preset`（DLSSG，A / C·FG Lite）、
  `mode`（VFG low/medium/high）、`camera`（FSR4 关闭/0/1/2/4），
  其余 `scene / duplicate / pre_h / gpu / log` 四个引擎共用。
- 状态：`~~/_cache/k7f_vpypar.json` 按脚本名分桶（`{"memc_dlssg_nv.vpy":{"multi":"4",...}}`），
  旧的扁平结构会当成 DLSSG 的一份读入；`multi` 归 `k7f_multi.lua` 管，链上已有时以链为准。
- OSD 菜单：`mp.input.select()` 两级列表（主菜单显示「名称：当前值」，选中进候选值列表），选完立刻重建并回到主菜单。
  在候选值列表按 Esc 只退回主菜单（`closed` 回调里重开主菜单），在主菜单按 Esc 才关闭面板；
  `close` 消息可直接关闭，不会被 `closed` 回调再拉起。
- 重建：整条链只替换目标那条 vpy —— 按 `params.file` 认出当前引擎的所有条目，剔除后追加一条带 `label` 的，
  一次 `set_property_native('vf', chain)` 落地；重建后复核条目数必须为 1（多于 1 会报错并再合并一次）。
  顺序与其它滤镜（`format`、着色器等）保持不变。
- 接管无标签的预设：menu.conf 里的 `vf set vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy"` 挂出来的滤镜没有标签，
  脚本按文件名认领它（旧实现要先用 `vf remove vapoursynth=<参数串>` 精确移除，现已不需要），
  此后所有改参都是「重建这一条」。
- 观察者自愈：`mp.observe_property('vf')` 发现引擎变了就切换上下文并重建状态桶；
  `user-data` 与当前状态不一致时延时 0.05 s 补一次参数，所以用户点菜单预设、或别的脚本重挂滤镜，
  也不会退回脚本里的硬编码默认值。
- 对外接口：`script-binding k7f_vpypar/menu`；`script-message-to k7f_vpypar set <key> <value>` / `apply` / `menu` / `close` / `status`。
- 上行：vpy 末尾调用 `k7f_bridge.publish()` 写 `<脚本名>.k7f.json`，本脚本每 0.5 s 轮询；改参后 5 秒内到达的新报告会弹一次 `实际：…`，`status` 消息输出“设置/实际”两行。接口细节见 `docs/vpy-bridge.md`。

## 4. 代价与限制

1. **改参 = 重新初始化**：vpy 由 `vsscript_evaluateFile()` 在播放线程里同步执行，DLSSG 要重建 NGX 会话，期间画面冻住（百毫秒到秒级）。这不是可以绕过的优化项，是当前接口的固有代价。
2. **`multi` 只能重建**：它改变输出帧数/时间基，`FrameEval` 做不到中途变帧率（mpv 靠 `_Duration` 算 PTS）。
3. **暂停时改参可能初始化两次**：`vf` 变更后 `mp_force_video_refresh()` 在暂停/帧显示超过 0.5 s 时发一次 `MPSEEK_VERY_EXACT`，seek 又会重跑 vpy。播放中改一般只抖 1–2 帧。
4. **重建从当前帧开始**：脚本若在初始化时就回看早期帧（`clip[0]`、`Trim`），会撞 `Frame %d requested, but only have frames starting from %d`，必要时把 `buffered-frames` 调大。
5. **插件是进程内单实例单尺寸**（见 `docs/mpv-dlssg.md` 坑 3）：换片源分辨率仍旧要重启 mpv，OSD 改参解决不了这个。
6. 实测中见过一次「改参瞬间 mpv 静默退出」，同一命令复跑 3 次都不再复现（真实 mpv-lazy 配置 + 真 vdlssg 引擎）；`--hwdec=no` 下未出现过。若遇到，先判 hwdec。

## 5. 部署

两边各放一半：

1. k7sfunc 侧（`k7sfunc/mpv/portable_config/`）：`vs\` 放四个 `MEMC_*.vpy`，`scripts\` 放
   `k7f_vpypar.lua` 与 `k7f_bridge.lua`。这四个 vpy 已经在 mpv-playkit 的 k7sfunc 组件白名单里，
   打包时按 `components.yml` 的 paths 复制，不需要额外映射。
2. mpv-playkit 侧（本仓库）：`manifests/config.yml` 新增 `vpypar` 段
   （`section: 补帧参数 (&V)` / `target: k7f_vpypar` / `key: V`），由 `build/config.py` 渲染进
   `input_uosc.conf` 与 `menu.conf` 的 `playkit-config:vpypar` 标记区。生成条件是**本档位包启用了
   任一 `multi: true` 的预设**（即带帧生成），与 `multi` 段同进同退；`base` 档位包不出现这两处。
   - `input_uosc.conf`：` V  script-binding k7f_vpypar/menu  #! VF 滤镜 > 补帧参数`（`v` 已用于音频延迟）
   - `menu.conf`：`补帧参数 (&V)` → `打开参数面板 (&A)` / `当前参数 (&S)`
3. script-opts 只剩「链上没有 vpy 时认哪个引擎」的兜底，日常不用配：

   ```
   script-opts-append = k7f_vpypar-script=~~/vs/MEMC_DLSSG_NV.vpy
   script-opts-append = k7f_vpypar-state_file=~~/_cache/k7f_vpypar.json
   ```

4. 手动给 mpv-lazy 之类没有这套构建流程的配置装时，等价写法是 `input.conf` 一行
   `V  script-binding k7f_vpypar/menu`，菜单项同理。

## 6. 实测记录

源：`测试数据\阿哈角色PV 1080p.mkv`，`--vo=null --ao=null --untimed`（排除渲染）；vpy 末尾临时打印 `PARAMS multi=… scene=… ud=…`。

| 操作 | 观察到的 vpy 输出 |
|---|---|
| 启动 `user-data="multi=3;scene=0.12"` | `multi=3 scene=0.12 ud='multi=3;scene=0.12'` |
| 状态文件里是 4×，命令行给的是 3× | 启动后自愈重建为 `multi=4`（观察者发现 ud 与状态不符） |
| `set multi 5` | `multi=5 scene=0.12 ud='multi=5;preset=a;scene=0.12;duplicate=0.005;pre_h=0;log=0'` |
| `set preset c` | `preset=c ud='multi=5;preset=c;scene=0.12;duplicate=0.005;pre_h=0;log=0'`；插件日志 `preset_pinned … preset_id:3, lite_effective:1` |
| `set scene 0.15` | `multi=5 scene=0.15 …` |
| 模拟点预设（挂一个无标签、无 user-data 的滤镜） | 先 `multi=3 ud=''`（插件默认），随即被认领重建为 `multi=5 scene=0.15` |
| 真引擎（mpv-lazy 配置 + vdlssg.dll） | `[vdlssg] frame 1/3/5… slot …` 按 2× 出帧；`set multi 4` 后按 4× 出帧（207/210/211/213…），seek 之后仍是 4× |
