# 阶段 4：图库系统 — 主计划

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development。任务文件同目录 `2026-11-15-phase4-task-N-*.md`。
> **权威契约：`API-SPEC-library-helper.md`（helper wire 完整规格）+ `docs/DESIGN.md`（spklib 边界与 schema）——仓库外 spklib 检出（D:\spk-deps\spklib）为准。**

**Goal:** Windows Qt 版图库系统：目录（catalog）/会话（session）源、收藏集/相册/评分/色彩标签/过滤、导入与文件夹同步、持久缩略图库、图库页 UI（模式行 图库·冲洗·编辑 + 懒加载网格 + 侧栏）——与 macOS v2 同构（同一 spklib 存储）。

**Architecture:** 从源码构建 **spklib Windows 版**（C++20 + vendored nlohmann/json + SQLite amalgamation，MinGW 13.1 直接编译——不用 CMake，遵循仓库 build.sh 约定写 build-windows.ps1），Qt 经 QProcess 走 stdio line-JSON helper 协议（与 Mac app 同协议）。这与设计文档"阶段 5 spklib 从源码构建"的既有决策一致，图库存储与 Mac 完全同构、数据跨平台兼容，Capture One 只读集成（原阶段 5）随之免费获得。

## Global Constraints

1. 全量 CTest 保持绿（当前 30）；Qt 侧只新增（CatalogSourcesStore/LibraryHost/ThumbnailStore/ImportSession + 控制器接线），不改引擎
2. 数据兼容：sources.json / pairs.json / .spektrapair / helper wire JSON 键名与 Mac 逐字一致（ISO-8601 日期、snake_case）
3. spklib 铁律：永不写 Capture One；引用文件永不移动/改名/删除；"gone = 隐藏不删除"
4. UI 中文硬编码 + settingsEditable 门控惯例；测试隔离
5. 每任务一提交；验收 = 单元/集成测试 + offscreen 冒烟 + 真实目录/会话工作流 gate（合成 fixture 数据可自建——spklib 管理自己的库文件，测试可创建临时 catalog/session fixture，无需 RAW）

## 关键事实（探索报告沉淀）

- spklib = C++20，nlohmann/json 3.12.0 vendored，系统 SQLite（Windows 需 amalgamation）；构建 = build.sh 直接 clang++ 调用（无 CMake）——Windows 写 build-windows.ps1 等价物（MinGW g++/ar）
- helper 协议：stdio 每行一 JSON、id 回显、ready 事件 `{"event":"ready","version":...,"protocol":1}`、错误码 not_found/invalid_params/read_only/conflict/...
- 行/收藏集 JSON 键：images.list 行 `uuid/path/file_name/kind(raw|raster|movie|pair|other)/rating/color_tag/trashed/captured_local/origin/exists/variants[]` + full 模式 camera/exposure；collections.list 行 `uuid/parent/kind/name/position/origin/editable/image_count/folder/title/created_at`
- 过滤键：min_rating/ratings[]/color_tags[]/kinds[]/text/captured_from/captured_to/camera_models[]/lenses[]/keywords[]/has_edit/described
- meta.set: variants[]或images[] + rating 0-5/color_tag 0-7（色彩序=C1 菜单序 1红..7紫）
- sources.json 形状：{version, sources[{id,kind,name,path,libraryPath,addedAt,openedAt,copiedFrom}], selected}（ISO-8601；损坏文件挪到 sources.unreadable-<time>.json 不覆盖）
- .spektrapair 形状：HalfFramePair 顶层键（schemaVersion/folder/left/right/spacingMM/onStrip/turned/origin）+ spektralab 块 {kind:"half_frame_pair", version:1, pair, name, frames{side:{frame,path,name}}, edit{kept_in,frame}}——阶段 4 对库成员写/读此形状
- 图库 UI 映射（v2 §8 单窗口惯例）：模式行 图库·冲洗·编辑 领衔左栏；浏览器所有模式共享（挂载一次）；图库模式左栏=侧栏三 tab（库/过滤/信息）；网格 cell 112pt 默认（64-256 ×1.25 步进）2pt 沟槽 6pt 内衬；键盘 0-5 评分、−*+ 标签、Ctrl+C/V 组
- Mac 参考类（语义参考）：CatalogSourcesStore（sources.json 读写 + 树构建）、LibraryHost/HelperProcess（QProcess 等价）、MacImportSession（两遍扫描→分块 200→Recent Imports）、ThumbnailStore（路径键 NSCache 64MB）、SettingsClipboard（组剪贴板：组=filmAndPaper/filmExposure/format/decode/enlarger/couplers/preflight/light/curve/colour/hdr；粘贴写设置不写已解数值）
- 现有 Qt 库模型 = 无序会话（LibraryItem vector + filmstrip）；阶段 4 把 filmstrip 升级为共享浏览器（所有模式挂载一次）

## 任务索引

| # | 文件 | 交付物 |
|---|---|---|
| 0 | 2026-11-15-phase4-task-0-spklib.md | spklib Windows 构建 + host 冒烟 + wire 探测 |
| 1 | 4-1 | LibraryHost(QProcess 协议) + CatalogSourcesStore(sources.json) + 纯测试 |
| 2 | 4-2 | 图库数据接入（行模型/收藏集树/facets）+ EditStore 路径联接 |
| 3 | 4-3 | 导入管线 + 文件夹同步 + 缩略图库（持久化） |
| 4 | 4-4 | 图库页 UI（模式行/网格/侧栏/评分标签键盘/剪贴板组复制粘贴） |
| 5 | 4-5 | 收尾 + 终审 + 合并 |