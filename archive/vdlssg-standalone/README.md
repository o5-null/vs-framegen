# archive\vdlssg-standalone — 已归档

第一代 `vdlssg` 插件源码：一个 DLL 只跑 DLSS-G，滤镜注册、NGX 会话、D3D12 上下文各一个文件
（`vdlssg.cpp` / `ngx_session.cpp` / `d3d12_ctx.cpp`）。

已被 `vs-dlssg\src\` 取代：同一套代码重构成 `frame_gen.cpp`（滤镜 + 切镜检测）+ 每引擎一个
`*_session.cpp`，一次编译出四个插件（`vdlssg` / `vxessfg` / `vvfg` / `vfsr4`），见
`../vs-dlssg/src/build.bat` 与 `../docs/vdlssg-plugin.md`。

保留原因：`docs\dlssg-on-3080-spike.md`、`docs\vdlssg-throughput.md` 里的早期实测与结论仍引用
这一版的实现细节（例如为什么必须动态链接 `nvsdk_ngx_d.lib`）。除对照外不要再改这里。
