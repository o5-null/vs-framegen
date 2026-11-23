这是一个dlss帧生成尝试迁移到视频的项目仓库（vs-framegen）。
.\测试数据 是用来测试实际生成速度和质量的
.\参考插件 不应该进行更改，只是用于参考
.\bench 是八引擎帧生成基准（脚本 + 结果快照），原始数据在 .\测试数据\engines
源码以 .\vs-dlssg\src 为准（一个引擎一个 DLL，build.bat 一次编四个）；.\archive 与 .\ref 只读
构建产物、引擎包、SDK、第三方装包与媒体素材不进 git（见 .gitignore）；提交前跑 git status 确认没有大文件

## Release 规则

- 安装与用法只维护 `docs\install.md`，release 正文**只写这一版改了什么 + 附件清单与 sha256**，
  安装说明写一行链接过去：`https://github.com/o5-null/vs-framegen/blob/main/docs/install.md`。
  以后所有 release 都链这一份，不逐版复述目录布局、参数表、推荐装法。
- `docs\install.md` 的推荐顺序固定为：优先 **mpv-playkit-pre 懒人包**（含 `-pack-framegen` 补丁包），
  自行搭建建议用 **K7 包**（K7sfunc 的 mpv 覆盖层）+ 本仓库 release 的 `vs-plugins` 附件。
- 正文里不要再列"四引擎插件组"表、不要写版本主线总结、不要写 OSD 参数桥细节（后者归 K7sfunc）。
- 构建产物（DLL、引擎包）不入库，只作为 release 附件分发。

## 邻近仓库（都在本目录下，各自独立 .git，已被 .gitignore 忽略）

- .\k7sfunc：K7sfunc 的 fork（o5-null/K7sfunc），PLAYKIT 预设层与 RIFE_AJI 算子在这里；改动 push 后由 mpv-playkit 按 commit 钉版本
- .\mpv-playkit：Windows x64 便携 mpv 预览包的构建链（o5-null/mpv-playkit-pre），manifest + sources.lock
- .\vsaji：AnimeJaNai (libaji) 的 VapourSynth 插件与构建脚本，产物 dist\aji 供 k7sfunc 的 RIFE_AJI 使用
- .\k7sfunc-wiki、.\k7sfunc-wiki-pages、.\vs-framegen-wiki-pages：文档与静态页，只读产出，改动跟在对应仓库后面

跨仓库的改动要分别提交到各自的仓库；本仓库的 .gitignore 只负责把它们挡在外面，不会被一起提交。

## 测试环境（Python / VapourSynth）

脚本用本仓库自己的 uv 虚拟环境 `.venv`（已被 .gitignore 挡住），不要借用其它项目的 venv：

```
uv venv --python 3.12 .venv
uv pip install --python .venv\Scripts\python.exe vapoursynth==74 numpy vsrepo
.venv\Scripts\vsrepo.exe install ffms2 imwri
```

- vapoursynth 钉在 **74**：`测试环境\mpv-lazy\vs-plugins\akarin.dll` 是 API 3 插件，R82 起拒绝加载
  （`uses API 3, which is no longer supported`），而 k7sfunc 的 RIFE_NV / RIFE_AJI 都依赖 akarin。
- ffms2 / imwri 由 vsrepo 装进 `.venv\Lib\site-packages\vapoursynth\plugins\vsrepo`（VS 自动扫描该子目录）。
- 其余插件不进 venv，按路径 LoadPlugin：akarin/vstrt（+ vsmlrt-cuda 运行时）在 `测试环境\mpv-lazy\vs-plugins`；
  aji 运行时是 `work\aji-models`（`vsaji\dist\aji` 的拷贝，用 `AJI_DIR` 指过去）。
- 测速脚本在 `work\aji-rife-test`（不进 git）：`one.py` 一次跑一种引擎+模型，`mk_src.py` 把
  `测试数据\1080p` 的 PNG 转成无损 FFV1（`--source ffv1`），避免把 PNG 解码算进引擎耗时。
- aji 的分阶段计时桩默认关闭，`AJI_PROFILE=1`（`AJI_PROFILE_EVERY=N`，默认 10 帧）打开，
  stderr 出 `[aji-profile/plugin]`（上传/交织/回传）与 `[aji-profile/trt]`（setup…infer…crop）两行；
  桩在 `vsaji/src/aji_host.cpp`、`aji_frames.cpp`、`.deps/engine/src/aji_trt.cpp`，改完要重编 engine + 插件。
