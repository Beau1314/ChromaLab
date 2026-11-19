# ChromaLab — SpektraLab 的 Windows 移植版

> **ChromaLab**（非官方）：基于 [JamesQiu2005/SpektraLab](https://github.com/JamesQiu2005/SpektraLab)（macOS 版，GPL-3.0-or-later）的代码与数据移植到 Windows。按上游商标条款，公开分发的构建使用自有名称 **ChromaLab** 与自有图标（RGB 光斑叠加）；本移植非官方，与上游 SpektraLab、spektrafilm 项目均无隶属或背书关系。

一台 Windows 机器上的胶片模拟暗房：打开 RAW，选胶片与相纸，得到物理建模的印相——光线经 81 波长穿过胶片染料层与相纸层，带颗粒、光晕、DIR 成色剂与放大机滤镜包，全分辨率约一秒出图。

## 已实现（相对上游 Windows 前端的增量）

上游仓库的 Windows 前端仅有基础编辑器。本移植按 macOS v2 功能逐阶段补齐：

| 阶段 | 内容 |
|---|---|
| 1 编辑器完整化 | 编辑页 Layer 2 全套（曝光/对比度/亮度/高光/阴影/黑白点/饱和度/色温色调/5 通道曲线/四区色彩平衡/暗角）、分屏对比、直方图、宽容度卡片、白平衡行（温度/色调/As Shot/预设/测光） |
| 2 引擎内核 | 解除 Windows 引擎的六个功能拒绝：Cineon DI、场景宽容度映射、对比遮罩、HDR 扫描、胶片边缘（135/半幅/XPan/645–6×17 边缘印字+齿孔）、日期背印（lcd/dots/data 三面）——11 个 Vulkan 内核 + EdgeRaster 文本层（TeX Gyre Heros）+ 线程化卷积，`capabilities.unsupported_features` 清零 |
| 3 半幅对 | 半幅对全栈：建对、逐孔独立曝光/日期/冲印、组装冲洗、持久化 |
| 4 图库 | 与 macOS **同库 SDK**（spklib 0.4.0 Windows 构建）：目录/会话源、收藏集/相册/评分/色彩标签/过滤、导入（去重/分块/Recent Imports）、文件夹同步（新增/移动保评分/消失隐藏）、持久缩略图库、图库页（模式行 图库·冲洗·编辑 + 侧栏三 tab + 懒加载网格 + 键盘 + 组剪贴板） |
| 6 印相台+色彩+i18n | 印相台（A4-A6 真实毫米摆放、99 张、1:1 校样、lighttable.json 与 Mac schema 2 同构）、四色彩空间导出（sRGB/Display P3/Adobe RGB/ProPhoto，ICC 逐字节标记）、四语言（en/简中/繁中/日文，复用 Mac 的本地化目录与检查规则） |

（阶段 5 Capture One 只读集成的 wire 方法已就绪，待真实 C1 目录样例验收。）

## 构建

需求：Windows 10+，x64，MinGW 工具链由脚本自动获取；显卡驱动自带 Vulkan。

```powershell
# 1. Qt SDK 引导（约 278MB，pinned SHA256，含 MinGW 13.1）
./windows_UI/setup-qt.ps1 -DependencyDirectory ../deps

# 2. spklib（图库 SDK，MIT）——检出到仓库外并打上 Windows 可移植性补丁
#    详见 docs/spklib/portability-notes.md（5 处偏差 + 构建脚本）
#    构建脚本与补丁说明随本仓库携带：docs/spklib/

# 3. 完整构建 + 测试
./windows_UI/build.ps1 -SourceDirectory . -BuildDirectory ./build/qt -RunTests
```

产出 `build/qt/app/` 为自包含目录（EXE + 全部 DLL + QML + 资源 + 许可证 + spklib-host.exe），
整个文件夹可复制到任意位置日常使用，双击 `SpektraLabQt.exe` 启动。

**重要：构建必须在纯 ASCII 路径下进行**（MinGW 工具链对中文路径会静默出错；本机实践是建
`D:\spk` junction 指向项目）。运行时不限。

## 使用

- 模式行切换 图库 · 冲洗 · 编辑；图库页管目录/导入/评分标签，冲洗页选胶片相纸与效果，编辑页做后期
- 快捷键：Ctrl+O 导入、Ctrl+E 导出、Ctrl+Z/Y 撤销重做、0-5 评分、− * + 色标、Ctrl+=/− 网格缩放、V 对比
- 导出：TIFF16/PNG8/PNG16/JPEG8 × sRGB/Display P3/Adobe RGB/ProPhoto（ICC 标记），拒绝覆盖现有文件
- 输入照片永不写入；编辑数据存放在应用自有目录（`AppData\Local\SpektraLab Windows\Edits` 等）

## 已知边界（如实）

- **HEIF 导出未含**：x265 许可（GPL-2.0-only）与本移植（GPL-3.0-or-later）不兼容，分发层面被阻塞——唯一未对齐 Mac 的导出格式
- **Nikon HE/HE\* RAW 不支持**（LibRaw 限制）
- **显示器 ICC 感知、软打样**未接（画布按 sRGB 标记显示）
- 印相台的 loupe 3× 与分带全尺寸渲染梯未移植；图库的会话导入、.spektrapair 行、部分 §5 快捷键为已披露的后续项
- Capture One 集成已上线（合成目录全链路验收 + 只读字节级断言）；真实 .cocatalog 样本到手后复核一次 Windows 路径根的择根修复

## 许可

- 代码 GPL-3.0-or-later（`license/LICENSE`）；胶片/相纸档案与印相 LUT CC BY-SA 4.0（`license/SPEKTRAFILM_LICENSE.txt`）；TeX Gyre Heros 字体 GUST Font License（`license/GUST-FONT-LICENSE.txt`）；spklib MIT（独立仓库，本仓库 `docs/spklib/` 携带其 Windows 可移植性补丁与构建脚本）
- SpektraLab™ 名称与图标归 Guangzhou Songzi Intelligent Technology Services Co., Ltd.——**ChromaLab 之名与 RGB 光斑图标为本项目自有**，按上游商标条款用于公开分发；"基于 SpektraLab"仅为事实陈述，非官方
- 发布的每个 zip 携带完整许可与来源文件（`license/`）及对应源码指引（GPL-3.0 §6）；源码即本仓库，Release 的 tag 与二进制一一对应
- 详细边界与验收口径见 [`windows_UI/README.md`](windows_UI/README.md)；上游原版说明见 [`README.upstream.md`](README.upstream.md)
