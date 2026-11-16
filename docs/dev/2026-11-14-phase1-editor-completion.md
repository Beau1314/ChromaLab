# 阶段 1：编辑器完整化 — 主计划

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans。
> 本计划拆分为 11 个任务文件，同目录命名 `2026-11-14-phase1-editor-completion-task-NN-<slug>.md`。
> 每个任务文件内的步骤用 checkbox（`- [ ]`）跟踪，逐任务提交代码。

**Goal:** 为 windows_UI (Qt) 补齐 macOS v2 编辑页能力：Layer 2 全套后期调整、分屏对比、直方图、宽容度卡片、白平衡行。纯 UI/host 层，不改引擎渲染内核。

**Architecture:** Layer 2（成品打印上的扫描侧评分）是前端侧第二遍处理，与 Mac 的 Metal layer2 内核、desktop/Tauri 的 GLSL+CPU 孪生同源。Qt 版采用**单一 CPU 实现**（`windows_UI/src/Layer2.cpp`，逐式移植 `desktop/src/shared/adjustments.ts`），预览与导出共用同一实现。胶片/印相侧参数照旧走引擎；Layer 2 编辑走**快路径**：不回引擎，在保留的完整打印上重新评分。发布管线：帧 → Layer 2（打印空间、几何之前）→ 几何 → 画布。

**Tech Stack:** Qt 6.11 (MinGW) / C++20 / Vulkan 引擎 spk_desktop_host / CTest。

**规范来源：**
- 设计文档：`docs/superpowers/specs/2026-11-14-windows-v2-parity-design.md`
- Layer 2 参考实现：`desktop/src/shared/adjustments.ts`（Mac Adjustments.swift + Shaders.metal layer2 的已验证移植）
- UI 行为规范：`app/SpektraLab/PAGES.md`、`app/SpektraLab/design/MAC-LIBRARY-UI.md`

## Global Constraints（每个任务隐含遵守）

1. 构建：仓库根 `./windows_UI/build.ps1 -SourceDirectory . -BuildDirectory ./build/qt -RunTests`；首次先 `./windows_UI/setup-qt.ps1 -DependencyDirectory ../deps`
2. 输入 RAW 与用户自有文件夹永不写入；编辑只写隔离 EditStore 目录
3. Layer 2 数学与 `adjustments.ts` 逐式一致；中灰枢轴 sRGB = `0.4614`（`kMidGreySrgb`）
4. Layer 2 应用于完整打印、几何之前（暗角在打印空间）
5. 曲线表 256×5，行序 rgb/luma/red/green/blue；插值 = Fritsch–Carlson 单调三次
6. UI 文案中文硬编码（四语言 i18n 属阶段 6）
7. 新文件登记进 `windows_UI/CMakeLists.txt`（应用目标 + 引用它的测试目标）
8. 测试永不读写用户真实编辑库（隔离目录注入）
9. 每任务一次提交：`feat|test|build|docs: 描述`

## 与设计文档的差异（计划阶段确认）

- **EDR 移出阶段 1**：Windows 桌面管线是 SDR（8-bit sRGB 显示），EDR 无法在 SDR 视图呈现，移入阶段 6（色彩管理批次）。设计文档已同步。
- **自然饱和度（vibrance）暂缺**：desktop 已验证模型只有 `saturation`；vibrance 与阶段 6 一起评估。

## 文件结构总览

```
windows_UI/
  src/Layer2.hpp / .cpp            [新建] Task 1-2：模型+JSON+数学（单一事实源）
  src/FrameOutput.hpp / .cpp       [修改] Task 3：取帧入口带 Adjustments
  src/PreviewController.hpp / .cpp [修改] Task 4/6/7/8/9：状态/快路径/直方图/宽容度/WB
  src/FrameCanvas.hpp / .cpp       [修改] Task 6：对比分割合成
  qml/Main.qml                     [修改] Task 5/6/7/8/9
  qml/CurveEditor.qml              [新建] Task 5
  qml/ColorBalance.qml             [新建] Task 5
  tests/layer2.cpp                 [新建] Task 1-2
  tests/wb_math.cpp                [新建] Task 9
  tests/edit_store.cpp             [修改] Task 4
  tests/frame_output.cpp           [修改] Task 3
  tests/editing_workflow.cpp       [修改] Task 4/6/7/8/9
engine/
  src/desktop/preview_host.hpp/.cpp [修改] Task 8/9（WB 字段 + scene_latitude 封装）
  src/io/raw_decoder.hpp/.cpp       [修改] Task 9（WB 覆盖 + as-shot 回传）
  CMakeLists.txt                    [修改] Task 9（white_balance.cpp 并入 spk_desktop_host）
  host/white_balance.cpp/.hpp       [复用] Task 9
```

## 任务索引

| # | 文件 | 交付物 |
|---|---|---|
| 0 | `task-00-environment.md` | git 基线 + Qt SDK + 基线构建测试 |
| 1 | `task-01-layer2-model-json.md` | Layer2 结构体 + encode/decode + 测试 |
| 2 | `task-02-layer2-math.md` | 曲线/uniforms/tone/pixel/apply + 测试 |
| 3 | `task-03-export-integration.md` | FrameOutput 评分集成 + 测试 |
| 4 | `task-04-controller.md` | 控制器状态/快路径/撤销/持久化 + 测试 |
| 5 | `task-05-edit-ui.md` | 编辑区 sections + CurveEditor + ColorBalance |
| 6 | `task-06-compare.md` | 分屏对比 + 测试 |
| 7 | `task-07-histogram.md` | 直方图 + 测试 |
| 8 | `task-08-latitude.md` | 宽容度卡片 + 测试 |
| 9 | `task-09-white-balance.md` | 白平衡行（host+decoder+UI）+ 测试 |
| 10 | `task-10-closeout.md` | 全量验证 + README 更新 |
