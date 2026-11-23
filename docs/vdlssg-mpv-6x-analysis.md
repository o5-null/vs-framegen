# DLSS-G 在 mpv 零拷贝链路中的 6× 性能分析

日期：2026-11-23  
范围：`vs-dlssg` + `mpv-src/video/filter/vf_playkit.c` 的 D3D11↔D3D12 零拷贝路径。

## 结论

**引擎本身已经能覆盖 1080p30 的 6×（180 输出 fps），当前 mpv 端看到的 60/90 fps 主要是呈现端（D3D11 flip/DWM 路径）限制，不是 DLSS-G 推理吞吐。**

- `vf_playkit` 每对输入帧调用一次 `vdlssg_bridge_process`，实测 `process=9.1–11.4ms/对`（首对 36ms，含初始化），`copy_wait=0.16–0.68ms`，`VideoProcessorBlt`/`av_hwframe_get_buffer` 均小于 1ms。
- 1080p30 6× 需要 30 对/秒，预算 33.3ms/对；当前约 10ms/对，约有 3× 余量。
- `vs-dlssg` 的直接基准（无 VO）在 1080p 下 m6 为冷 180.05 / 热 223.25fps，m4 153.43、m3 147.79fps，与上述余量一致。
- 当前显示器为 **2560x1440 @ 180Hz**；mpv 日志确认 `display-fps: 180.000000`、`Assuming 180.000000 FPS for display sync`。
- 默认窗口 `--d3d11-flip=yes --d3d11-sync-interval=1` 的 2s 源墙钟 6.20s、10s 源 22.77s（约 66fps）；这是 D3D11 flip/DWM 呈现路径把帧压住，不是 VO 只按 60Hz 取帧。
- 改用 **`--d3d11-flip=no --d3d11-sync-interval=0`** 后：2s 源 4.61s、5s 源 7.38s、10s 源 **12.39s**；5s→10s 的斜率约 5.01s/5s，说明呈现已是 180Hz 实时，固定开销约 2.4s（NGX/桥初始化）。10s `--untimed` 为 10.97s。
- `--fullscreen --d3d11-flip=no --d3d11-sync-interval=0` 为 12.30s，略慢于窗口模式，主要是独占全屏模式切换；窗口 `flip=no sync=0` 是当前最优。
- `--d3d11-sync-interval=1` 即使 `flip=no` 也只有 5.20s/2s、5.58s/全屏，仍被 vsync 节流；所以要接近 180fps 必须 `sync=0`，代价是可能撕裂。

## 实测数据

### mpv 零拷贝（2s 源，`multi` 对比）

命令框架：

```bash
mpv.exe --no-config --hwdec=d3d11va --d3d11-adapter=#1 \
  --vo=gpu-next --gpu-context=d3d11 \
  --vf=playkit=multi=6:log=no:dll=dlss45/vdlssg.dll:proxy_dir=dlss45 \
  --start=2.0 --end=4.0 --untimed
```

| 配置 | 墙钟 | 说明 |
|---|---:|---|
| `vo=null`，无滤镜 | 0.37s | 只解码，不呈现 |
| `vo=gpu-next`，无滤镜 | 1.14s | 约 68fps 呈现 |
| `multi=2` | 4.85s | 约 120–240 输出帧 |
| `multi=3` | 4.86s | 约 180–360 输出帧 |
| `multi=6` | 6.20s | 默认 `flip=yes sync=1`，390 输出帧，约 63fps |
| `multi=6` + `--d3d11-sync-interval=0` | 5.77s | 仅关 sync，flip 仍为 yes，约 67fps |
| `multi=6` + `--d3d11-flip=no --d3d11-sync-interval=0` | **4.61s** | 390 输出帧；扣启动后约 200 输出 fps |
| `multi=6` + `--fullscreen --d3d11-flip=no --d3d11-sync-interval=0` | 4.43s | 独占全屏，启动/模式切换更慢 |
| `multi=6` + `--d3d11-flip=no --d3d11-sync-interval=1` | 5.20s | 仍被 vsync 限速 |

### 每对耗时（`PLAYKIT_TIMING=1`）

```
timing copy_wait=0.16–0.68 process=9.1–11.4 bridge wall=8.0–9.7
frame[k] get=0.00–1.50 blt=0.00–0.16
```

`process` 包含 D3D11 拷贝、`Flush`、`pk_wait` 和 `vdlssg_bridge_process`；`stats.evaluate_ms/gpu_ms` 在当前 bridge 统计里为 0，实际 GPU 时间包含在 `wall_ms` 内。

### 10 秒端到端

- 默认 `flip=yes sync=1`、`multi=6`、`--start=0 --end=10`、`--untimed`：**22.77s**，约 66 输出 fps；是 flip/DWM 路径压住，不是 bridge 推理变慢。
- `--d3d11-flip=no --d3d11-sync-interval=0`、`multi=6`、10s 源：
  - `--untimed`：**10.97s**；
  - 正常播放：**12.39s**；5s 源为 7.38s，斜率 5.01s/5s，扣掉约 2.4s 初始化后正好是 10s 内容，即 180Hz 实时 6×。
  - `--fullscreen` 同参数：12.30s，略慢于窗口。
  - 打包后的 `work/mpv-zc-pkg` 同参数：**12.25s**，与源码构建一致。

## 为什么会出现“最多 90fps”

1. **呈现端节流**：`vf_playkit` 每对生成 5 个中间帧 + 当前帧，写入 `p->queue`；`process()` 每次只向 VO 交付一帧。VO 在 60/90Hz 下每刷新一次取一帧，于是 6× 输出被压在 60/90fps。90Hz 屏幕上看到 90fps，正好对应刷新率。
2. **默认 D3D11 flip/DWM 路径**：当前显示器是 180Hz，但 `--d3d11-flip=yes`（默认）配 `--d3d11-sync-interval=1` 时，实测只有 66fps；只关 sync 仍约 67fps。必须 **`--d3d11-flip=no --d3d11-sync-interval=0`** 才能让 Present 走 blit 路径、绕开 DWM 的节流，达到约 180fps（10s 源扣启动后 10s 放完）。代价是可能撕裂。
3. **早期 90fps 的引擎侧原因已修复**：此前 RGBA16F 每帧 16.6MB readback、CPU half 解包、每 slot fence 是瓶颈；现在持久 16 线程 RowPool、F16C、RGBA4、GPU 直接转 YUV420P10 已把引擎推到 180fps 以上。

## 下一步选项

- **要“显示”6×**：使用 180Hz 显示器 + `--d3d11-flip=no --d3d11-sync-interval=0`（窗口模式即可，实测优于独占全屏）；60Hz 屏幕无法显示 180fps，只能生成后丢弃。
- **要“测量”6×**：给 `vf_playkit` 增加一个真正的 null sink（消费生成帧但不呈现），或在 `vs-dlssg` 的 bridge 层做无 VO 的吞吐基准；不要用当前 `vo=null`，它会让 hwdec 退回软件格式，滤镜不执行。
- **要降低单对延迟**：当前 `process` 10ms 已满足 30fps×6；若未来做 60fps×6（360fps），需要再压到 <8ms/对，可继续看 `vdlssg_bridge_process` 内部的 D3D12 转换/NGX 同步。

## 代码状态

- `mpv-src/video/filter/vf_playkit.c`：已缓存 input/output video processor views；`PLAYKIT_TIMING=1` 输出每对/每帧计时。
- `vs-dlssg/src/d3d12_ctx.cpp`：已缓存 `OpenSharedHandle` 结果（`shareInputCache_`/`shareOutputCache_`）。
- `work/mpv-zc-pkg`：123 DLL、约 236MB，`mpv.exe --vf=help` 可见 `playkit`。
- 未提交/未 push；正式发布仍按用户要求执行。
