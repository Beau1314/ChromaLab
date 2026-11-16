# 阶段 3：半幅对（Qt 侧）— 主计划

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development。任务文件同目录 `2026-11-15-phase3-task-N-*.md`。
> **权威契约：`app/SpektraLab/PAIR-WIRING.md`（428 行 host/engine 契约）——每个任务实现者必读其相关节。**
> **参考模型：`app/SpektraLab/Sources/Model/HalfFramePair.swift`（语义参考，按 C++/Qt 习惯移植，不照搬 Swift）。**

**Goal:** Qt 版支持半幅对：两张照片组成一个 37×24mm 胶片件、逐孔独立曝光/日期/冲印、整件冲洗、逐帧切割。

**Architecture:** 引擎侧零改动（阶段 2 已解禁 overscan_pair/text_b/split/b_* 且 overscan_checks 已钉死行为）。全部工作在 host/Qt 层：PieceComposer 把两张解码 RAW 组装进一个 3ch float32 ProPhoto 线性 buffer（间隙=零=未曝光），经 spk_open 走现有单会话管线；逐孔测光用 ≤1024px 单孔会话 spk_solve("exposure")，**gain 由 host 乘进像素**（引擎测光关闭）。

## Global Constraints

1. 全量 CTest 保持绿（当前 28）；引擎 .cpp 内核/params 不改（host 层 desktop/preview_host 除外）
2. 对契约以 PAIR-WIRING.md 为准：间隙零、孔尺寸随较小照片、件不超较长照片、placement 不影响 meterKey、split=间隙中点、b_* 只属于第二帧
3. UI 中文硬编码 + settingsEditable 门控；测试隔离；每任务一提交
4. 持久化遵循 EditStore 可选组模式（carry-one-carry-all）
5. 验收：单元测试（composer 布局/gain/裁切旋转）+ 控制器测试 + overscan_checks 对用例复跑 + offscreen 冒烟

## 关键事实（探索报告沉淀）

- spk_open 输入 = spk_image{float*, w, h, 3}，caller-owned，线性 ProPhoto（opening_params: input_color_space="ProPhoto RGB", input_cctf_decoding=false）——两个 DecodedRaw 可直接组装（同格式同布局，top-down RGB row-major）
- spk_open_device 是 Metal-only（Vulkan 拒绝）——Windows 走 spk_open
- 布局：hole=18×24mm；advance=round(along×(18+spacing)/18)；件 37×24（level）/24×37（turned）；pieceLayout 取孔=较小照片在孔内的 fit 尺寸，件长 ≤ 较长照片
- 测光：单孔会话（≤1024px, AE on, exposure 0, format 24mm, 无边缘无日期）→ spk_solve("exposure") → 读 solved exposure_compensation_ev；stops = meteredEV + hole.exposureEV；meterKey = 帧+decode+WB+crop+intent（**不含 placement**）
- 缺口清单：①RenderSettings += scene_latitude_split/b_*5 字段（preview_host.hpp:61-62 明确 deferred）②sceneLatitude(request json 含 region)③overscan_geometry host wrapper④PieceComposer⑤对模型+持久化+UI
- engine/tests/overscan_checks.py:529-650 已钉死引擎侧行为（双门/间隙/转向/region/b_*）——复跑即证
- EditStore 严格键校验（缺或多余都拒）→ 对记录 = 新可选组或独立记录文件
- Qt 库模型：LibraryItem 按 URL 一项；Mac 模型中对是自己的库项（PAIR-WIRING §5）——Qt 版对作为新库项类型（含两个源 URL + 逐孔状态）

## 任务索引

| # | 文件 | 交付物 |
|---|---|---|
| 0 | 2026-11-15-phase3-task-0-host.md | host 扩展：split/b_* 字段 + sceneLatitude(region) + overscanGeometry wrapper |
| 1 | 2026-11-15-phase3-task-1-composer.md | PieceComposer + 逐孔测光（单元测试钉死） |
| 2 | 2026-11-15-phase3-task-2-controller.md | 控制器对集成（库项/开关流/逐孔编辑/持久化） |
| 3 | 2026-11-15-phase3-task-3-ui.md | QML 对 UI（建对/对编辑面板/整件画布） |
| 4 | 2026-11-15-phase3-task-4-closeout.md | 收尾 + 终审 + 合并 |