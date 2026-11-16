# 阶段 2：引擎内核移植（引擎+UI 同步）— 主计划

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development。任务文件同目录 `2026-11-15-phase2-task-N-*.md`。
> **权威实现指南：`engine/PORT_PLAN.md`——每个任务的实现者必须先读它对应的功能节（§1 DI、§2 latitude、§3 mask、§4 overscan、§5 date back）。**

**Goal:** 解除 Windows Vulkan 后端对六个功能的拒绝，每个功能解禁后立即接通完整 Qt UI。校验三层：float64 数值门 + Python 参考 parity + overscan_checks.py。

**Architecture:** Vulkan 后端按名字派发内核（vulkan_gpu.cpp kKernels[]），缺失内核报 "Vulkan kernel not ported"。每功能移植 = 五处编辑 + host 管道 + Qt UI + 持久化（RenderSettings 进 editing::State 自动持久化）。

## Global Constraints

1. 16 项引擎 CTest + 21 项 Qt 纯测试全程保持绿
2. 内核移植只动 host/io/shaders 层 + 新增 .comp；不重写引擎核心算法；metal_gpu.cpp / *.metal 一律不改
3. float64 门容差风格对齐现有内核测试（pointwise ~4 ulp；pow/exp 1e-6 相对）
4. 解禁 = 移除 windows_unported 标志 + engine.cpp unsupported_features + windows_feature_boundaries.cpp 断言翻转 + capabilities 更新
5. UI 文案中文硬编码；所有新控件 settingsEditable && hasFrame 门控
6. 测试永不读写用户真实编辑库
7. 不做像素级 Metal 对比（无 Mac）；整特性验收 = Python parity + overscan_checks + 数值门，文档如实记录该边界
8. 每任务一提交（修复轮追加 fix 提交）

## 关键架构事实（实现者必读）

- 内核注册表 vulkan_gpu.cpp:55-91 kKernels[]；派发接口 gpu.hpp:378 Gpu::dispatch（与 Metal 同名同 binding 序）
- 五处编辑模式：.comp（GLSL 450, local_size 256, std430 每 Metal buffer 一个 SSBO, spk_global_index(), 精确算术标 precise）→ kKernels[] → engine/CMakeLists.txt:112-124 SPEKTRALAB_VULKAN_KERNELS → tests/gpu_vulkan_<feature>.cpp float64 门 → 解禁
- 15 个 Metal-only 内核归属：spk_di_encode（DI）、spk_mask_reduce/epilogue（遮罩）、spk_scene_latitude/curve（宽容度）、spk_overscan_canvas/add_mask/film_present/light（边缘）、spk_scan_hdr_expand（HDR 扫描）
- 拒绝机制：windows_unported.hpp/.cpp（6 标志）、engine.cpp:342-349、engine.cpp:1805-1812、pipeline.cpp:119-127、tests/windows_feature_boundaries.cpp
- 校验基建：parity_*.py（需 Python numba + spektrafilm 参考检出，不需要 Mac）；overscan_checks.py（只需引擎 DLL，本机可跑：画布尺寸/门栅/种子/DX/半幅对用例）；float64 门模式 tests/gpu_vulkan_pointwise.cpp
- 已知小 bug（顺带修）：windows_exports.def 漏列 spk_overscan_geometry（Task 5）；scene_latitude.b_active 单独设置未被拒绝覆盖（Task 2）
- 半幅对（阶段 3）不被阻断：引擎侧参数/布局已可移植，overscan_checks.py:529-646 已有对用例

## 任务索引

| # | 文件 | 交付物 |
|---|---|---|
| 0 | 2026-11-15-phase2-task-0-parity-env.md | Python 3.12 + numba + spektrafilm 参考 + parity 基线 |
| 1 | 2026-11-15-phase2-task-1-di.md | Cineon DI：内核+解禁+parity+Qt UI |
| 2 | 2026-11-15-phase2-task-2-latitude.md | 宽容度映射：内核+解禁+gate+Qt UI |
| 3 | 2026-11-15-phase2-task-3-mask.md | 对比遮罩：host 线程化+2 内核+解禁+Qt UI |
| 4 | 2026-11-15-phase2-task-4-scan-hdr.md | scan_hdr：内核+解禁+Qt UI |
| 5 | 2026-11-15-phase2-task-5-overscan.md | 胶片边缘：文本层+线程卷积+4 内核+解禁+Qt UI |
| 6 | 2026-11-15-phase2-task-6-date-back.md | 日期背印：data 面文本层+Qt 日期控件 |
| 7 | 2026-11-15-phase2-task-7-closeout.md | 收尾：capabilities/coverage/README/全量验证 |