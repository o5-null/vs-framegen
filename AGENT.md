这是一个dlss帧生成尝试迁移到视频的项目仓库（vs-framegen）。
.\测试数据 是用来测试实际生成速度和质量的
.\参考插件 不应该进行更改，只是用于参考
.\bench 是八引擎帧生成基准（脚本 + 结果快照），原始数据在 .\测试数据\engines
源码以 .\vs-dlssg\src 为准（一个引擎一个 DLL，build.bat 一次编四个）；.\archive 与 .\ref 只读
构建产物、引擎包、SDK、第三方装包与媒体素材不进 git（见 .gitignore）；提交前跑 git status 确认没有大文件
