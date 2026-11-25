# benchmark

帧生成/插帧的测量数据与图。按测量目标分三组：

| 主题 | 文档 | 脚本 |
|---|---|---|
| 八引擎吞吐与质量（RIFE 4.25/4.26/4.6、DLSS-G、XeSS-FG、VFG、FSR4；2026-11-19） | [../benchmark.md](../benchmark.md)、[../engines-benchmark.md](../engines-benchmark.md) | `bench\src\run_throughput.py`、`bench\src\run_quality.py`；原始数据 `bench\results\` |
| vdlssg 纯生成（只测 GPU evaluate，排除传输；2026-11-22） | [vdlssg-gen-only-throughput.md](vdlssg-gen-only-throughput.md) | `work\bench_gen.py` + `work\gen_bench.vpy` |
| 四引擎全流程 × 两卡（含源侧、上传、回读；2026-11-22） | [full-flow-engines.md](full-flow-engines.md) | `work\flow_bench.py` + `work\flow_one.py`、`work\make_flow_charts.py` |
| VFG 档位与性能上限（`mode=` 三档、默认 medium、显存账本、分段瓶颈、四条被量过的加速路线、流水式批量 −26%；2026-11-25/26） | [vfg-perf-ceiling-2026-11-25.md](vfg-perf-ceiling-2026-11-25.md) | `vs-dlssg\test\engine_bench.py`（`VFG_MODE`/`VFG_TIMING`/`VFG_DIAG`/`VFG_BATCH`）、TEMP 下 `rep.py`/`bench_all.bat`/`timing_probe.py`/`ab_batch.py` |

全流程这一组在桌面程序全部关闭的空载状态下测量（有后台负载时整体慢 20% 以上）。

全流程这一组的图：

- [flow-fps-by-card.png](flow-fps-by-card.png) — 2× 下各引擎在两块卡上的输出帧率
- [flow-multi-ladder.png](flow-multi-ladder.png) — 每输出帧耗时随 multi 的变化
- [flow-pcie-gap.png](flow-pcie-gap.png) — PCIe ×4 / ×16 槽位的同配置对比
- [flow-budget.png](flow-budget.png) — 源侧 / 引擎 evaluate / 传输调度的三段构成
- [flow-gen-vs-full.png](flow-gen-vs-full.png) — DLSS-G 纯生成与全流程的差距

`pv-compare-*.png` 是 `../benchmark.md` 里的画质对比图。

原始数据：`flow.json`（每档样本与 baseline）、`flow.csv`（汇总表）。
