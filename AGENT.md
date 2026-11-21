这是一个dlss帧生成尝试迁移到视频的项目仓库（vs-framegen）。
.\测试数据 是用来测试实际生成速度和质量的
.\参考插件 不应该进行更改，只是用于参考
.\bench 是八引擎帧生成基准（脚本 + 结果快照），原始数据在 .\测试数据\engines
源码以 .\vs-dlssg\src 为准（一个引擎一个 DLL，build.bat 一次编四个）；.\archive 与 .\ref 只读
构建产物、引擎包、SDK、第三方装包与媒体素材不进 git（见 .gitignore）；提交前跑 git status 确认没有大文件

## 邻近仓库（都在本目录下，各自独立 .git，已被 .gitignore 忽略）

- .\k7sfunc：K7sfunc 的 fork（o5-null/K7sfunc），PLAYKIT 预设层与 RIFE_AJI 算子在这里；改动 push 后由 mpv-playkit 按 commit 钉版本
- .\mpv-playkit：Windows x64 便携 mpv 预览包的构建链（o5-null/mpv-playkit-pre），manifest + sources.lock
- .\vsaji：AnimeJaNai (libaji) 的 VapourSynth 插件与构建脚本，产物 dist\aji 供 k7sfunc 的 RIFE_AJI 使用
- .\k7sfunc-wiki、.\k7sfunc-wiki-pages、.\vs-framegen-wiki-pages：文档与静态页，只读产出，改动跟在对应仓库后面

跨仓库的改动要分别提交到各自的仓库；本仓库的 .gitignore 只负责把它们挡在外面，不会被一起提交。
