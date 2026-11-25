# vf_playkit 流水线改造（2026-11-25）

## 起因

`docs/mpv-filter-bench.md` 与 `docs/k7-vpy-debug-probe-2026-11-25.md` 里的对比显示：同一条
VapourSynth（VS）路径比原生 `vf_playkit` 快 1.5–3.5 倍，而两者调用的是同一个引擎
（`vdlssg_bridge_process`，引擎内部 `copy_wait < 0.7ms`、`process 5–8ms/对`）。差别来自调度：

- VS 路径把帧交给 VapourSynth 的线程池，mpv 侧同时挂 24 帧在飞（实测 `同飞=24 / 线程=24`）；
- 原生 `vf_playkit` 的 `pk_generate()` 是「一对帧一次阻塞往返」：拷两帧 → `Flush` → 自旋等拷贝退休
  → 同步调引擎 → 才返回。桥里只有一组 `in_tex`/`out_tex`，同一时刻只有一对在飞，
  于是「每对延迟」直接等于「吞吐倒数」。

`PLAYKIT_TIMING=1` 的分解印证了这一点（旧逻辑，1080p 2×，298 样本，单位 ms）：
`copy_wait` 中位 0.16、`process` 中位 6.61、成对之间的空档 `idle` 4.12、`cycle` 11.09。
即拷贝本身不贵，贵的是每对帧串行等一次引擎。

## 改造：N 槽位流水线

文件：`E:\code\dlssg-vfi\mpv-src\video\filter\vf_playkit.c`

- `struct pk_slot`：每个槽位一组独立 `in_tex[2]` / `out_tex[out_count]`（各自的 NT handle），
  两个 `D3D11_QUERY_EVENT`：`copy_done`（拷贝退休）、`blit_done`（输出纹理被 `VideoProcessorBlt`
  读完），以及 `in_view[]` 缓存。槽位数 `PK_MAX_SLOT = 4`，`PK_MAX_OUT = 5`。
- 选项 `pipe`（`playkit=…:pipe=N`，缺省 2）：同时可在引擎里的对数。
- 线程模型：
  - filter 线程（`process()`）只负责「读源帧 → 提交 → 取回 → 出帧」，不阻塞在引擎里；
  - 每个槽位一个工作线程（`playkit-gen`）：等本槽位拷贝退休 → 调 `vdlssg_bridge_process`；
  - 提交顺序记在 `p->order[]`，取回严格按提交顺序，所以**输出顺序、时间戳、`multi` 帧数都不变**；
  - `pk_frame()`（VP blit + hw 池纹理）挪到取回时执行，并被 `blit_done` 保护，
    避免下一对复用该槽位的输出纹理时还在读。实测 blit 的 CPU 时间 0.01ms，排队化没有代价。
- 失败路径：任一对返回错误即 `mark_failed`，退化成原来的软件复制路径（与原逻辑一致）。
- 槽位用尽时 `pk_pump()` 直接返回，等下一次 `process()` 回调再试，不做忙等。

## 落地时踩到的三个坑（都写进了代码注释）

1. **`mp_cond_wait` 必须持锁调用。** `mpv-src/osdep/threads-win32.h` 里 `mp_mutex` = `SRWLOCK`、
   `mp_cond` = `CONDITION_VARIABLE`（零初始化即可用），但 `SleepConditionVariableSRW` 要求调用前
   持锁。第一版工作线程取走 job 后先解锁，干完活解锁回循环顶部，下一轮就在**未持锁**状态等待 ⇒
   偶发死锁，且 lock/unlock 次数不配（10 次 lock、11 次 unlock）。修法：`mp_filter_wakeup()` 之后
   重新 `mp_mutex_lock()` 再回循环顶部。
2. **`pk_threads_start()` 不能清空队列。** 它原先在「bridge 已存在」时才调用，而第一次提交就发生在
   建桥的同一轮，于是第一个 job 和 `order[]` 被抹掉，那个槽位永久 `BUSY`。修法：建桥成功后立即
   `if (!p->threads_started) pk_threads_start(f);`，并让 `pk_threads_stop()` 在 join 后把每个槽位
   置 `FREE`、解除 `cur` 引用。
3. **`mp_cond_signal` 只唤醒一个等待者。** 看门狗也会 `mp_cond_timedwait`，提交时的 signal 可能
   叫醒看门狗而不是工作线程 ⇒ job 永远没人接手（pipe=1 必现，pipe=2 概率低）。修法：看门狗改用
   独立条件变量 `wd_cond`，提交改 `mp_cond_broadcast`。

## 调试开关（默认全关）

| 变量 | 作用 |
| --- | --- |
| `PLAYKIT_TIMING=1` | 每对打 `playkit: timing copy= … process= … bridge wall= … idle= … cycle= … slot=N`（写 `--log-file`） |
| `PLAYKIT_TRACE=1` | 提交/取回/引擎/看门狗逐步埋点（`pump free=… order=… jobs=…`、`worker …`、`reap …`），并启动停摆看门狗 |
| `PLAYKIT_LOG=1` | 每对打输入/输出/源帧哈希，用于核对异步后输出是否逐帧一致 |
| `pipe=N` | 槽位数（1..4，默认 2） |

看门狗：`PLAYKIT_TRACE` 打开时每 500ms 检查一次，`job_count||order_len` 且 3 秒没有进展就
`playkit: WATCHDOG stall …ms jobs=… order=… engine=… slot=…`，并打印每个槽位的 `state/blit_pending`。
第 3 个坑就是靠它抓到的。

## 实测（600 输出帧/次，HD 源 1920x1080@30，3 轮交错取中位）

| 配置 | VS 路径 | 原生 pipe=2 | 原生/对 | VS/对 |
| --- | --- | --- | --- | --- |
| 1080p 2× | 210.9 | 179.9 | 11.1ms | 9.5ms |
| 1080p 6× | 259.7 | 210.3 | 23.8ms | 19.3ms |
| 2k 2× | 137.7 | **150.3** | 13.3ms | 14.5ms |
| 4k 2× | 72.6 | **108.9** | 18.3ms | 27.5ms |
| 4k 6× | 98.2 | **151.0** | 39.7ms | 61.0ms |

改造前同一台机器、同一条原生路径只有 118.8 / 54.3 / 46.9 fps（1080p / 2k / 4k 2×），
即流水线在 1080p 提速 51%、2k 2.8 倍、4k 2.3 倍。

- 1080p 仍落后 VS 约 15%：VS 侧 CPU 平面搬运在低分辨率下几乎免费，而它能把 24 帧同时挂着。
- 槽位数扫描（1080p 2×，交错 3 轮）：pipe=2 179.0、pipe=3 182.6、pipe=4 181.1 ⇒ 默认 2 足够，
  再加槽位只是多占显存（1080p 每槽约 58MB，4K 约 232MB）。
- 正确性：`PLAYKIT_LOG=1` 下 pipe=1 与 pipe=2 各 120 对的 `in[0]/in[1]`、`src[k]`、`out[k]`
  哈希序列逐条一致（`HASH CHECK OK`）；`PLAYKIT_TRACE=1` 短片段 pipe=1/pipe=2 各连跑 10 次无冻结。

## 下一层瓶颈

引擎侧是全局串行的：`vs-dlssg/src/ngx_session.cpp:77-78` 的 `std::mutex g_lock;`
（注释「Serialises every GPU and NGX call」）被 `acquire/release/newStream/generate` 全部持有。
所以槽位和线程再多也只重叠了 D3D11 拷贝与等待，引擎调用本身一次只能跑一个。
1080p 的天花板 ≈ 1/引擎 wall ≈ 200 对/s，与实测 180–183 吻合。要再快必须改引擎侧
（放开 `g_lock` 或让 NGX 允许多个 pending），而不是继续加槽位。

## 落地步骤（未做）

1. 提交 `mpv-src`（fork `o5-null/mpv`）的 `vf_playkit.c` 改动 → 触发该仓库的 release CI 产出
   `mpv-x86_64-playkit-<日期>-<n>.zip`；
2. `mpv-playkit/sources.lock.json` 的 `mpv` 条目改指新 release（url + sha256 + size + pinned_at）；
3. `mpv-playkit/DECISIONS.md` 追加一条记录（流水线、默认 pipe=2、调试变量）。
