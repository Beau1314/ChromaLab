# SpektraLab Windows 版对齐 macOS v2 — 设计文档

日期：2026-11-14
基线：`JamesQiu2005/SpektraLab` main 分支快照（本工作区 `SpektraLab-main/SpektraLab-main/`）
目标：将 `windows_UI`（Qt Quick/C++）演进到与 `app/SpektraLab`（macOS v2）功能一致。

## 0. 背景与决策记录

仓库含三前端一引擎：

| 目录 | 说明 |
|---|---|
| `app/SpektraLab` | macOS v2 正式版（SwiftUI + Metal，Mac Catalyst），功能全集 |
| `windows_UI` | Qt Quick/C++ Windows 前端（**选定基线**） |
| `desktop` | Tauri 2（TS+Rust）前端，对齐已移除源码的 1.x 版，作为逻辑参考 |
| `engine` | C++20 引擎：Metal（Mac）+ Vulkan（Windows/Linux），`spk_*` C API |

已确认决策：
- **基线**：windows_UI（Qt），方案 A 原地演进；desktop 的已验证逻辑按语义移植为 C++/QML，不照搬代码。
- **范围**：完全对齐 macOS v2，分六阶段，每阶段交付可构建可测试的完整应用。
- **UI 规范**：以 v2 文档为准——`app/SpektraLab/design/MAC-LIBRARY-UI.md`（窗口布局）、`app/SpektraLab/PAGES.md`（页面规则）；布局按 Windows 习惯适配，不做像素级复刻。
- **商标**：本复刻为个人用途，保留原名原图标（符合 README 商标条款的个人 fork 例外）。

## 1. 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│                    SpektraLabQt (Qt Quick)                   │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────────┐  │
│  │ 图库页    │ │ 冲洗页    │ │ 编辑页    │ │ 印相台/导入/导出│  │
│  └──────────┘ └──────────┘ └──────────┘ └────────────────┘  │
│  （共享：左侧模式行 图库·冲洗·编辑 + 底部浏览器条）              │
│  RenderScheduler（单飞行+合并）  EditStore（撤销+持久化）        │
│  LibraryStore（目录/会话/相册/评分/标签 JSON）                  │
├─────────────────────────────────────────────────────────────┤
│  spk_desktop_host（扩展）                                     │
│   - 会话/单飞行调度/负片缓存（已有）                             │
│   - 新增：film_edge/date_back/pairs/DI/mask/latitude 能力解禁  │
├─────────────────────────────────────────────────────────────┤
│  spk engine (C++20 + Vulkan) + spklib (Windows 构建, MIT)     │
│   - 现有 34 内核 + 新移植内核（对齐 Metal 逐值校验）              │
└─────────────────────────────────────────────────────────────┘
```

原则：
1. 一个应用、一个引擎、一个数据层；UI 为 QML 视图层，渲染调度保持单飞行 + 合并（现有行为不变）。
2. **能力门禁驱动 UI**：UI 读 `capabilities.backend.unsupported_features`，随内核移植逐个解禁；desktop 的 `params.wire()` 门禁机制语义照搬。
3. 编辑持久化 JSON 采用 v2 兼容形状（`MAC-LIBRARY-DATA.md` 子集，未知字段透传）。
4. 输入 RAW 与用户自有文件夹永不写入。

## 2. 六阶段路线图

### 阶段 1：编辑器完整化（纯 UI/host 层）
- Layer 2 全套：亮度/对比度/色调、5 通道曲线（增删点、直方图底衬）、HSL、通道、色彩轮（色相/饱和度盘+三区滑块）、自然饱和度/饱和度、色温、暗角。复用 desktop 调整模型语义（编码空间中灰枢轴：画布 sRGB、ProPhoto 文件 ROMM；掩膜随 Mac 端撤销不实现）。
- 分屏对比（拖动分割线，Before=引擎内嵌快速解码）、直方图（统计几何+Layer 2 输出，修正 Mac 端已知缺陷的口径）、宽容度卡片（`spk_scene_latitude` 实测绘图）。
- EDR（`spk_session_hdr_curve`）。
- 相机/白平衡行：测光、胶片曝光+As Shot、暗角、解码温度/色调+两个 As Shot 框+预设，编辑经 redecode 去抖 350ms 单撤销步；RAW 专属。镜头校正不做（LibRaw 无镜头档案，与 desktop 一致）；中性滴管暂不做。
- 验收：既有 Qt 测试全过 + Layer 2 CPU 孪生一致性测试。

### 阶段 2：引擎内核移植（C++/Vulkan）
- 胶片边缘：135/半幅/XPan/645–6×9/6×12/6×17 片基布局、齿孔、各胶片边缘印字与帧号（`spk_overscan_geometry` 接口已在）。
- 日期背印：七段码日期曝光。
- Cineon DI、对比度遮罩（`spk_contrast_mask_field`）、场景宽容度映射——后端实现并解禁。
- 校验：新内核 float64 数值门（容差风格对齐现有 IIR/重采样/几何测试）+ Metal parity harness + Mac `film_edge_parity.py` 同套用例。

### 阶段 3：半幅对
- 独立模型/线协议/UI：一帧位含两张，各自曝光/白平衡/冲印；对页的胶片边缘是对自己的开关（`filmEdge.active` 保持关，避免单帧门误读）。

### 阶段 4：图库系统
- 目录/会话数据层（JSON，v2 兼容）、相册、变体、评分、色彩标签。
- 图库页（懒加载网格）、导入页（浏览+拖放）、设置复制/粘贴/同步到多张（保留各自裁剪）。

### 阶段 5：Capture One 只读集成
- spklib 从源码构建 Windows 版；目录按快照副本、会话按文件夹读取；严格只读，无任何写回路径。

### 阶段 6：印相台 + 色彩管理 + 本地化
- 印相台：已冲洗帧按真实毫米尺寸摆放/移动/旋转。
- 导出色彩空间 sRGB/Adobe RGB/Display P3/ProPhoto（lcms2 ICC 嵌入）+ HEIF（libheif）+ 显示器 ICC（Windows Color System）。
- 四语言 en/简中/繁中/日文 + 键完整性检查。

依赖：1→2→3 串行；4 可与 2 并行；5、6 收尾。

## 3. 数据流

```
控件编辑 → EditStore（wanted 编辑+撤销栈）
  ├─ 胶片侧 → develop（整帧）
  └─ 印相侧 → reprint（负片缓存）
单飞行：新请求合并最新设置；被取代结果永不显示
层级：live(1600px) → preview(3400px) → full(原生)；编辑静默 400ms 升级
```
- 半幅对：一次编辑=一对 look，引擎按对渲染。
- 图库缩略图走独立低优先级队列，不阻塞冲洗/编辑作业。
- 持久化：EditStore 原子写/冲突检测/源指纹机制不变，JSON 升级为 v2 形状。

## 4. 错误处理

| 场景 | 行为 |
|---|---|
| 渲染失败 | 保留显示帧与控件，可重试 |
| 导出目标已存在 | 单张拒绝；批量跳过 |
| 图库 JSON 损坏/未来版本 | 隔离保留原文件，空库启动并告警 |
| 源文件移动/删除 | 缩略图保留、标记缺失，编辑记录不自动删 |
| C1 集成 | 快照副本打开；spklib 崩溃只影响本会话 |
| 新内核校验不过 | 功能保持禁用，UI 不显示死控件 |
| 导出缺 ICC | 降级 sRGB 并明示 |

## 5. 测试策略

既有资产不回归：16 项引擎 CTest、Qt 4 套件、离屏场景 gate、渲染矩阵工具。

| 阶段 | 新增 |
|---|---|
| 1 | Layer 2 CPU 孪生 vs 画布逐像素一致（sRGB/ROMM 枢轴）；直方图统计；trap-33 表面对齐 |
| 2 | 内核 float64 门（IIR 1.5e-8 / 重采样 3e-7 / 几何 8e-7 风格）；Metal parity；胶片边缘 parity |
| 3 | 半幅对线协议与双帧独立参数往返；边缘印字 parity |
| 4 | 图库工作流 gate（导入→评分→筛选→批量同步→导出 + 源零写入断言） |
| 5 | C1 只读 gate（目录树哈希前后一致 + 损坏降级） |
| 6 | ICC 标签与像素抽查；四语言键完整性 |

每阶段 DoD：完整构建 + 全部 CTest + 阶段 gate + 离屏场景 + 既有 Qt 测试不回归。

## 6. 明确不做（与 Mac 端或 desktop 一致）

- 掩膜（Masks）——Mac 端已撤销（FeatureFlags.masks=false）
- 色调遮罩（Tone Mask）——Mac 端已隐藏
- 镜头校正档案——LibRaw 无镜头档案
- 中性滴管——desktop 未移植，暂缓
- 更新检查/诊断打包/内存页——desktop 跳过；Windows 版提供日志目录与版本信息的简化版
