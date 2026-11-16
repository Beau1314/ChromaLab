# 阶段 6：印相台 + 色彩管理 + 四语言 — 主计划

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development。任务文件同目录 `2026-11-16-phase6-task-N-*.md`。
> **权威参考：** LightTable 模型 = `app/SpektraLab/Sources/LightTable/LightTableModel.swift`（"statement for statement" 移植自 table.js）；色彩 = CONTRACT-frontend-backend.md §1.2 + spk_engine.h:361-407（spk_output_transform）；i18n = `app/SpektraLab/LOCALIZATION.md` + `Resources/Localizable.xcstrings`（1203 键）。

**Goal:** Windows Qt 版补齐：印相台（真实毫米尺寸摆放）、四色彩空间导出（sRGB/Adobe RGB/Display P3/ProPhoto + ICC 标记 + HEIF）、四语言（en/简中/繁中/日文）。

**Architecture:** ①i18n 先行（方案 B：运行时字符串表 L(key)，英语字面=键+回退，复用 Mac xcstrings 目录转换的 JSON，localization_check 规则移植为 gate；语言切换发 languageChanged 重渲染；不跟随系统——首启动派生一次）②色彩走引擎 `spk_output_transform`（返回适配后线性 3×3 矩阵 + CCTF 模式 + CAM16-UCS C_max 表）→ 前端 CPU 转换核（decode CCTF → 3×3 → CAM16 roll-off → encode CCTF，逐段移植 Mac OutputTransform.swift 的运行时编译内核）→ Qt QColorSpace 命名预设标记 ICC ③印相台按 LightTableModel.swift 语句级移植（mm 世界坐标/99 张上限/A4-A6 纸张/zoom 1-8/1:1 校样用 QScreen 物理毫米 + EDID 有效性守卫——Catalyst 0.77 因子不适用于 Windows，直接 devicePixelRatio）+ lighttable.json（schema 2 形状与 Mac 一致）。

## Global Constraints

1. 全量 CTest 保持绿（当前 35）；引擎不改（spk_output_transform 是现有 C API）
2. 数据兼容：lighttable.json 与 Mac schema 2 逐键一致；语言目录键与 Mac xcstrings 一致（未翻译键回退英语字面）
3. 不跟随系统语言：首启动从宿主语言派生一次（AppLanguage.derived 规则），之后用户选择
4. UI 中文为默认源语言之外仍保留：现有中文串 = 简中值；键 = 英语字面（Mac 惯例：英语是键；Windows 现状中文——迁移时键=英语、zh-Hans=现中文）
5. spklib 铁律/测试隔离/每任务一提交照旧

## 关键事实（探索报告沉淀）

- spk_output_transform(engine, src_cs, dst_cs, gamut_compress_json, out_json, out_cmax, out_cmax_count)：fetch 非渲染；返回 source/target_cctf_mode + matrix（CAT02 适配线性 3×3）+ CAM16-UCS C_max 表；未知空间按名报错绝不回退；导出取 {"lightness_compression_active":false}（明度级已在引擎内应用）
- Mac 导出四目标：displayP3/sRGB/"Adobe RGB (1998)"/ProPhoto RGB(ROMM)；ProPhoto 8bit 允许 band；ICC 由容器自动携带（Mac=ImageIO；Windows=Qt QImageWriter 嵌入 QImage colorSpace + TIFF 走引擎 writer 的 sRGB ICC——**非 sRGB 空间的 TIFF16 需要引擎 writer 扩展或 Qt TIFF 路径**，实现者判断并记录）
- 印相台模型：mm 世界坐标（x 右 y 上，原点=亮区中心）；A4 297×210/纸张 A4-A6；film 上限 99；zoom 1-8（1:1 需要时放宽）；吸附 4°/touch 7°/shift 15°；lens 3×；准入 = developed || filmEdge.active || pair；mm 尺寸来源 = FilmGeometry.canvasMM / pair 模式 / filmFormatMM（绝不猜）；持久化 lighttable.json {schemaVersion, portrait, paper, places:[{frame,on,x,y,rot,z}]} 防抖 300ms 原子写；帧尺寸不存（每次打开从库重读）
- Windows mm→px：QScreen physicalDotsPerInchX/Y（EDID）+ devicePixelRatio；无 EDID 尺寸 → 无 1:1（守卫移植：mm≥20、轴一致 5%、结果 0.25-20 px/mm）
- i18n 体量：QML ~374 串（Main 315/LibrarySidebar 41/GridPage 13/其余 6）+ C++ ~65（PreviewController 33/LibraryModel 26/main 2/hpp 4）；Mac xcstrings 1203 键已有 zh-Hans/zh-Hant/ja
- HEIF：Qt imageformats heif 插件（libheif）需编译；诚实评估成本后可降级为"记录边界"（Mac 8bit HEIC + ICC；Windows HEIF 编码依赖链重）

## 任务索引

| # | 文件 | 交付物 |
|---|---|---|
| 0 | 2026-11-16-phase6-task-0-i18n.md | Localization 核心（L() 表/语言切换/xcstrings 转换/check 移植） |
| 1 | 6-1 | 全部现有字符串迁移 L() + 四语言值 + 设置语言行 |
| 2 | 6-2 | 色彩管理：output_transform 消费 + CPU 核 + 四空间导出 + ICC |
| 3 | 6-3 | HEIF 评估与接入（或诚实降级记录） |
| 4 | 6-4 | 印相台（模型/换算/持久化/全窗口页/准入/Add 流） |
| 5 | 6-5 | 收尾 + 终审 + 合并 |