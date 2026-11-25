# 统一工作区：子仓库与构建产物收进 `repos\`（2026-11-26）

## 起因

同一套东西的子仓库原本散在 `E:\code\dlssg-vfi\` 根下（`mpv-playkit`、`k7sfunc`、`k7sfunc-wiki`、
`mpv-src`、`ngxsdk`、`vsaji`），每个仓库各自有 `.work\downloads` 与 `dist\out`：同一份
253.8 MB 的 vs-framegen 发行 zip、同一批 vsrife / model 组件在多个仓库各存一份，
`mpv-playkit\dist\out` 还堆了 8 组历史档位包。这次把它们统一收进工作区根，缓存与产物也挪出仓库。

## 现在的布局

```
E:\code\dlssg-vfi\
├─ vs-dlssg\ …            这个仓库本体（vs-framegen 源码与 docs）
├─ repos\                 统一工作区根（整个目录被 .gitignore 挡掉）
│  ├─ mpv-playkit\        便携 mpv 预览包构建链（o5-null/mpv-playkit-pre）
│  ├─ k7sfunc\            K7sfunc fork
│  ├─ k7sfunc-wiki\       wiki 源
│  ├─ mpv-src\            mpv 源码 + MSYS 构建树
│  ├─ ngxsdk\             NVIDIA DLSS SDK
│  ├─ vsaji\              AnimeJaNai 插件与构建脚本
│  ├─ .cache\downloads\   所有子仓库共用的下载缓存（按 sha256 定位）
│  └─ dist\               交付归档：本仓库的 vs-framegen-<版本>.zip、mpv-playkit 的基础包与两个档位 7z
└─ work\、测试环境\ …      照旧
```

`k7sfunc-wiki-pages\`、`vs-framegen-wiki-pages\` 是静态页输出（不是 git 仓库），仍留在根下。

## 代码侧怎么让步

`repos\mpv-playkit\build\common.py` 新增 `_shared_root()`：

- `PLAYKIT_SHARED_ROOT` 环境变量显式指定优先（必须存在，否则视为未指定）；
- 否则只有 `ROOT.parent.name == "repos"` 才认——避免把任意上级目录当成共享区。

命中共享根时：

| 常量 | 值 |
| --- | --- |
| `DOWNLOAD_DIR` | `repos\.cache\downloads` |
| `OUT_DIR` | `repos\dist` |
| `STAGE_ROOT` | `<repo>\dist\stage`（单包 0.6–1.7 GB，不放共享区） |

`build/fetch.py` 的下载顺序变成：缓存命中 → `repos\dist` 里同名且 sha256 一致的归档
（`os.link` 硬链接进缓存，不复制）→ 真正下载。CI 上 checkout 路径没有 `repos` 父目录，
三个常量全部退回旧路径，workflow 与 actions/cache 都不用改。

## 验证

1. `python -m build fetch`（在 `repos\mpv-playkit`，先删掉缓存里那份 vs-framegen 资产）：
   exit 0，15 条缓存命中 + 1 条 `复用共享产物 vs-framegen-dlls: …\repos\dist\vs-framegen-0.2.2.zip`。
2. 用几 KB 的假 stage 调 `package.zip_stage()`：归档确实写到 `repos\dist` 并同时产出 `.sha256`。
3. 常量打印：`shared -> …\repos`、`dl -> …\repos\.cache\downloads`、`out -> …\repos\dist`、
   `stage -> …\repos\mpv-playkit\dist\stage`。

## 搬目录时踩的坑

- `Move-Item` 对目录会退化成"复制 + 删除"：`mpv-playkit\.git` 被某个 watcher 持有句柄时
  报 `Cannot remove item … You do not have sufficient access rights`，仓库被拆成两半
  （顶层文件进新位置，目录还留在原地）。同卷搬目录要用 `cmd move`（改名）或
  `robocopy /MOVE`，不要用 `Move-Item`。
- 目录被持有句柄时连 `move` 都会 `Access is denied`（本次 `dist`、`manifests` 就是）——
  `robocopy <src> <dst> /E /MOVE /R:1 /W:1` 能过，因为它逐文件搬并最后清目录。
- 用 `cmd` 跑的批处理如果含中文且行尾是 LF，`cmd` 会把行首字节吃掉（出现
  `'aykit' is not recognized` 这种）。给 cmd 的脚本要么纯 ASCII，要么写成 CRLF。

## 后续

- `repos\dist` 里已有 `vs-framegen-0.2.2.zip`、基础包与 `5f1aa34` 两个档位 7z（含 `.sha256`）；
  老版本档位包仍留在 `repos\mpv-playkit\dist\out`，需要时手工清理。
- 本地 `.work\downloads` 已闲置（缓存都搬走了），可直接删。
