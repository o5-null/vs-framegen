# mpv 补帧滤镜叠成两条（599fps 问题）

## 现象

mpv OSD 顶部显示 `23.976 → 599fps`，`Frame Rate: 23.976 fps (specified) 599.4006 fps (estimated)`，
右侧 Filters 里出现**两个** vapoursynth 滤镜，都指向 `vs/MEMC_DLSSG_NV.vpy`，`user-data` 里都是 `multi=5`。

599.4 = 23.976 × 25 = 5 × 5，即两个 ×5 的 FrameGen 串联。

## 根因

两层原因叠在一起：

1. **mpv 的 `vf remove` 按内容匹配，找不到时静默成功**
   `obj_settings_find_by_content` 对条目比较的是 name + **全部** attribs
   （`mpv-src/options/m_option.c:3139-3160`、`:3703-3713`）。
   旧版 `k7f_vpypar.lua` 的移除规格是 `vapoursynth=<file>`，漏掉了 `user-data`；
   条目一旦被别的脚本（`k7f_multi.lua` 写倍数）加过 `user-data`，就匹配不上，
   `vf remove` 什么也没删却返回成功 —— 旧条目留在链上，紧接着 `vf append` 又加一条。

2. **两个脚本都在写同一条链，且各自判断“没变化”的方式不同**
   `k7f_multi` 把 `user-data` 重排成字母序（`multi=5;scene=…`），
   `k7f_vpypar` 只按字符串比较 `ud == build_user_data()`，于是把“同样的设置”当成变化反复重建；
   重建又走回 `append`，重复条目就这样留下。

`vs-dlssg/src/frame_gen.cpp:596-603` 里每经过一次 FrameGen 滤镜 fps 就乘一次 `multi`，
`frame_gen.cpp:160-172` 写的 `_DurationNum/_DurationDen` 被 `vf_vapysynth` 逐帧累加，
所以 OSD 的 “estimated” 是真实的输出帧间隔，读数没错，错的是滤镜链。

## 修复

`k7sfunc`（commit `de2304f`，`mpv/portable_config/scripts/`）:

- `k7f_vpypar.lua`
  - `apply()` 改成**整链重建**：剔掉所有指向本 vpy 的条目（标签 `VDLSSG` 或同文件名），
    末尾放一条新条目，再 `mp.set_property_native('vf', chain)` 一次性写回，
    不再依赖内容匹配的 `vf remove`。
  - 写完复核条数，`> 1` 时 `msg.error` + OSD；清理了重复条目时 `msg.warn`
    「清理了 N 条重复的 VDLSSG 滤镜（多条补帧会叠乘倍数）」。
  - 观者发现 `> 1` 条时先 warn 再合并成一条。
  - `user-data` 按键值比较（`B.parse_user_data`），不再被键序不同骗到。
  - **倍数以链上值为准**：链上已有 `multi` 就采用它并保存，避免与 `k7f_multi` 互相覆盖造成无限重建。
  - 状态文件读不出来时 `msg.warn`，不再静默回默认值。
- `k7f_multi.lua`：状态文件解析失败/为空、删除旧状态文件失败、0.5s 后链上仍没有 `multi` 键，都 `msg.warn`。
  复核只判断“缺键”，不比对具体值（mpv 属性写入排到播放循环，立刻读回是旧链，且别的脚本紧接着重建链表是常事）。
- `k7f_playkit.lua`：`vf remove` / `vf append` 的返回值都检查，失败 OSD 提示并中止。
- `k7f_bridge.lua`：`mkdir` 失败、上行报告解析失败（只报一次）不再静默。

`vs-dlssg/mpv/k7f_vpypar.lua`（commit `15263a9`）由修好的 k7sfunc 版重新生成，
保留本地的 `preset`（模型预设）参数块，两份不漂移。

## 验证

用 `mpv-playkit/dist/stage/mpv-playkit-pre-2026.11.20-b11cbd0-windows-x64-vsdlssg/mpv.exe`
+ `work/aji-rife-test/src1080p_ffv1.mkv`（`--no-audio --vo=null`），三个探针：

| 场景 | 修复前 | 修复后 |
| --- | --- | --- |
| 预置一条含两个同 vpy 条目的链（模拟遗留叠加） | changes=114（k7f_multi 与 vpypar 互写无限重建） | `VDLSSG 在滤镜链里有 2 条，合并为一条` → n=1，changes 停在 6 |
| 0.3s 套预设、2s 再套、4s 外部再 append 一条 | — | 合并回 n=1，changes 停在 9 |
| 真实流程 `vf set vapoursynth="~~/vs/MEMC_DLSSG_NV.vpy"` | — | 最终 `@VDLSSG ud=multi=5;log=0;pre_h=0;preset=a;scene=0.25`，n=1 |

## 待办

- k7sfunc 只做了本地 commit，需要用 `PLAYKIT_LOCAL_K7SFUNC` 本地覆盖或 push + 重新钉
  `mpv-playkit/sources.lock.json` 才能进分发包。
- 用户现有 `D:\Extra\mpv-playkit-…` 播放目录里的脚本仍是旧版，需替换
  `portable_config/scripts/k7f_{vpypar,multi,playkit,bridge}.lua`。
