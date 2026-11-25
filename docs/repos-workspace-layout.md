# 构建产物收进 `repos\`（2026-11-26）

## 布局

子仓库仍在工作区根下各自独立（`mpv-playkit`、`k7sfunc`、`k7sfunc-wiki`、`mpv-src`、`ngxsdk`、
`vsaji`），`repos\` 只放构建产出的交付归档与共享下载缓存：

```
E:\code\dlssg-vfi\
├─ vs-dlssg\ …            这个仓库本体（vs-framegen 源码与 docs）
├─ mpv-playkit\           便携 mpv 预览包构建链（o5-null/mpv-playkit-pre）
├─ k7sfunc\ k7sfunc-wiki\ mpv-src\ ngxsdk\ vsaji\   其它子仓库
├─ repos\                 .gitignore 忽略；只装产物与缓存
│  ├─ dist\               vs-framegen-<版本>.zip、mpv-playkit 的基础包与两个档位 7z（含 .sha256）
│  └─ .cache\downloads\   所有子仓库共用的下载缓存（按 sha256 定位）
└─ work\、测试环境\ …      照旧
```

`k7sfunc-wiki-pages\`、`vs-framegen-wiki-pages\` 是静态页输出（不是 git 仓库），仍在根下。

## 代码侧怎么让步

`mpv-playkit\build\common.py` 的 `_shared_root()` 判定顺序：

1. `PLAYKIT_SHARED_ROOT` 显式指定（必须存在的目录）；
2. 仓库父目录下存在 `repos`，且其下有 `dist` 或 `.cache` → 那就是共享根；
3. 仓库自身位于 `<root>\repos\<repo>` → 共享根是 `<root>\repos`（旧布局兼容）。

命中共享根时：

| 常量 | 值 |
| --- | --- |
| `DOWNLOAD_DIR` | `repos\.cache\downloads` |
| `OUT_DIR` | `repos\dist` |
| `STAGE_ROOT` | `<repo>\dist\stage`（单包 0.6–1.7 GB，不放共享区，打包后保留供手测） |

`build\fetch.py` 的下载顺序：缓存命中 → `repos\dist` 里同名且 sha256 一致的归档
（`os.link` 硬链接进缓存，不复制）→ 真正下载。CI 上 checkout 路径没有 `repos` 兄弟目录，
三个常量全部退回旧路径，workflow 与 actions/cache 都不用改。

## 验证

1. `python -m build fetch`（在 `mpv-playkit`，先删掉缓存里那份 vs-framegen 资产）：exit 0，
   15 条缓存命中 + 1 条 `复用共享产物 vs-framegen-dlls: …\repos\dist\vs-framegen-0.2.2.zip`。
2. 用几 KB 的假 stage 调 `package.zip_stage()`：归档确实写到 `repos\dist` 并同时产出 `.sha256`。
3. 常量打印：`shared -> …\repos`、`dl -> …\repos\.cache\downloads`、`out -> …\repos\dist`、
   `stage -> …\mpv-playkit\dist\stage`。

## 搬目录时踩的坑

- `Move-Item` 对目录会退化成"复制 + 删除"：`mpv-playkit\.git` 被某个 watcher 持有句柄时
  报 `Cannot remove item … You do not have sufficient access rights`，仓库被拆成两半。
  同卷搬目录要用 `cmd move`（改名）或 `robocopy /MOVE`，不要用 `Move-Item`。
- 目录被持有句柄时连 `move` 都会 `Access is denied`（本次 `dist`、`manifests` 就是）——
  `robocopy <src> <dst> /E /MOVE /R:1 /W:1` 能过，因为它逐文件搬并最后清目录。
- 用 `cmd` 跑的批处理如果含中文且行尾是 LF，`cmd` 会把行首字节吃掉（出现
  `'aykit' is not recognized` 这种）。给 cmd 的脚本要么纯 ASCII，要么写成 CRLF。

## 后续

- `repos\dist` 里已有 `vs-framegen-0.2.2.zip`、基础包与 `5f1aa34` 两个档位 7z（含 `.sha256`）；
  老版本档位包仍留在 `mpv-playkit\dist\out`，需要时手工清理。
- 各仓库 `.work\downloads` 已闲置（缓存集中在 `repos\.cache\downloads`），可直接删。
