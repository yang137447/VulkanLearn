# VulkanLearn

## 简介
这是一个基于 Vulkan 的渲染学习项目，支持阴影映射、后处理、场景加载等功能。项目集成了 Tracy Profiler 和 NVIDIA Nsight Systems 用于性能分析。

## 文档分工

为了避免“现状说明”、“AI 协作说明”和“未来规划”混在一起，仓库内文档按职责分工如下：

- `README.md`
  - 面向人类读者的项目入口
  - 负责说明项目是什么、怎么构建、怎么运行、去哪里继续读
  - 只写当前仍然有效的上手信息，不承载长期规划细节
- `AGENTS.md`
  - 面向 AI coding agent 的仓库协作说明
  - 负责说明阅读顺序、修改约定、高风险区域、隐式耦合和工作方式
  - 不负责替代架构设计文档
- `documents/README.md`
  - 面向所有协作者的文档分层说明
  - 说明三层各自能信到什么程度，以及"契约层不得写未来"这条硬纪律
- `documents/routing.md`
  - 源码路径 → 必读契约 → 守护门禁规则的映射
  - 改任何源码前先读它，按路径匹配全部适用规则
- `documents/contracts/`
  - 当前契约：系统**现在是什么**，每条可核对到源码、测试或门禁规则
  - 与源码矛盾时以源码为准，并修正契约
- `documents/rationale/` 与 `documents/plan/`
  - **非契约**：决策、证据归档、被否决的方案、退役实现、以及打算做什么
  - 可以任意写未来，但不能当事实引用
- `documents/reference/`
  - 存放教程、课程和背景学习资料

当前有效的正式文档入口：

- `AGENTS.md`
- `documents/README.md`
- `documents/contracts/vulkanlearn-architecture.html`
- `documents/contracts/game-ui-stack.md`
- `documents/contracts/texture-asset-json-v1.md`
- `documents/contracts/asset-modify-api.md`
- `documents/contracts/material-param-authoring-and-reflection.md`
- `documents/contracts/descriptor-imageinfo-management.md`

## 环境要求
- Windows 10/11
- CMake 3.24+
- MinGW-w64 with C++17 support
- Vulkan SDK (需包含 Nsight Systems)

当前仓库使用的是 MinGW 工具链，现有 `build/` 目录记录的 CMake generator 为 `MinGW Makefiles`。

## 构建指南

### 1. 配置项目
在项目根目录下打开终端，执行以下命令生成构建文件：

```bash
cmake -S . -B build -G "MinGW Makefiles"
```

### 2. 编译项目
执行以下命令进行编译：

```bash
cmake --build build -j
```

如果你的 MinGW 环境没有自动进入 `PATH`，请先确保 `g++`、`gcc` 和 `mingw32-make` 可用，再执行上面的 CMake 命令。

编译成功后，可执行文件 `main.exe` 将生成在 `build/bin` 目录下。

### C# 蓝图 游戏逻辑

Windows x64 安装 .NET 8 SDK 后开启：

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DVULKANLEARN_ENABLE_CSHARP_BLUEPRINTS=ON
cmake --build build -j8
.\build\bin\main.exe --dev-ui
```

未进入 PATH 的 SDK 可通过 `-DVULKANLEARN_DOTNET_EXECUTABLE=D:/path/to/dotnet.exe`
指定。功能默认关闭，不影响无 SDK 的 C++ 构建。
引擎的 `script/VulkanLearn.Runtime/` 提供核心 API；用户的 `*.blueprint.cs` 与蓝图配置资产
放在资源根的 `Common/Blueprints/` 或 `Maps/SC_*/Blueprints/`。
构建自动收集 Common/Maps 中的 `.cs`，产物在 `build/bin/managed`；
供 IDE 打开的项目生成在资源根 `Generated/Runtime/CSharp/Gameplay.csproj`。
蓝图资产使用 `Blueprints/BP_*.json`，场景类型为 `blueprintInstance`；旧 Actor 格式不兼容。
Content Browser 的 C# Classes 展示已编译的蓝图类；双击或右键 Open in VS Code 编辑源码，
不打开引擎 Tab。右键 Create Blueprint Asset 选择名称和资源路径，创建引用该类的 `BP_*.json`；
同一 C# 类可以派生多个配置资产，未覆写属性继承类默认值，不生成配对源码。
Outliner 底部加号也提供 Create Blueprint Asset，以及选择类、名称和可选网格的 Place instance。
Details 编辑 TRS 和 `[Expose]` 的标量、向量、枚举、稳定引用及内联自定义数据/集合，
Save Content 确认保存。
只有明确开放 Instance Editable 的属性允许场景覆写。
顶部图标控制 Play/Pause/Stop，Stop 恢复作者变换；修改 Gameplay 后重新编译，
可使用工具栏 Compile C# 或启用 Auto Compile C#，输出区显示文件/行/列诊断。
成功后通过同一关卡的程序集事务重载；播放/暂停期间按显式 `[Migrate]` 状态迁移，
失败保留旧程序集与配置。
完整构建后也可在停止播放的关卡页点击 Reload C# assembly；桥接/API 修改需要重启。
选中实例后可在 Details 的 New Asset 创建 `Blueprints/BP_*.json`，再从 Content Browser
拖入关卡视口生成新实例，落点为观察射线与 `y=0` 地面的交点。
实例只保存 `blueprintPath`、独立 TRS 和显式属性/组件字段覆写；
BP 的 `componentInstanceEditable` 按稳定组件 ID 开放字段，Scene 用稀疏 `componentOverrides` 保存。
未开放的字段只读；黄色边框表示覆写，回转箭头删除覆写并恢复继承。
示例场景为资源根下 `Maps/SC_actor_authoring/SC_actor_authoring.json`。
另有 `Maps/SC_actor_gameplay/SC_actor_gameplay.json` 展示组件父子树、Spawn 和 Jolt 刚体。
双击 BP 打开左侧组件树、中央 3D 预览、右侧 Details 的独立标签。
支持 scene/mesh/camera/点光/聚光/方向光组件局部 TRS、预加载 BP 的运行时生成/销毁、box/sphere/capsule 碰撞、
Compound/静态三角网格、碰撞过滤、Fixed/Hinge 约束、接触/Trigger 事件、射线/扫球查询和胶囊角色控制器；
输入映射、计时器和蓝图消息由 C# API 提供。运行时可添加/删除/挂接组件，
网格及材质组合需已由 World 或 BP 模板准备；`Assets.LoadBlueprintAsync` 成功后可 Spawn 动态 BP，
请求和租约支持取消/释放，Stop 清理播放期间的结构修改。
Editing 时 BP JSON 内容变化自动走 World 事务重载，播放时延后至 Stop。
构建配置决定托管 Debug/Release；`cmake --build build --target vl_script_package`
把私有 .NET 8 运行时放入 `build/bin/managed/dotnet`，随程序及 managed 目录搬移。
完整游戏包可直接导出到一个尚不存在的目录：

```powershell
cmake --build build --target main vl_export_game -j8
.\build\bin\vl_export_game.exe --project-root . --initial-scene Maps/SC_actor_gameplay/SC_actor_gameplay.json --output artifacts/game-export
```

导出器递归收集场景资产、shader、UI、原生 DLL、Gameplay 与私有 .NET 8，
写入 `game-export-manifest.json`，关闭编辑器并自动 Play；导出后的 `main.exe` 可从任意工作目录启动。
运行时拼接出的额外 BP/资产路径需用 `--dynamic-assets assets.json` 显式列入，
该文件内容是资源根相对路径的 JSON 字符串数组；不解析任意 C# 字符串。
目标机器仍需 Windows x64 和兼容的 Vulkan 驱动，无需安装 .NET SDK。
不含资产继承；构建开启时获取锁定的 Jolt 5.6.0 CPU 依赖。
物理同步执行在 GT，不包含 GPU 毛发模拟。
格式、API 和边界见 [C# 蓝图 契约](documents/contracts/csharp-blueprint-runtime.md)。

### VS Code 启动入口

Run and Debug 下拉菜单提供四个入口。启动前会依次执行对应的
CMake configure 与 `main` 构建，不需要手动切换同一个目录的编译选项。

| 入口 | CMake 类型 | Vulkan validation | Tracy / NVTX | 构建目录 |
| --- | --- | --- | --- | --- |
| Development | RelWithDebInfo | 关 | 关 | `build-vscode/development/` |
| Debug | Debug | 开 | 关 | `build/` |
| Profile | RelWithDebInfo | 关 | 开 | `build-vscode/profile/` |
| Release | Release | 关 | 关 | `build-vscode/release/` |

Development 放在菜单首位，也是 `Ctrl+Shift+B` 的默认构建任务。
四个入口中仅 Debug 开启 Vulkan validation，日常 Development 保留 CPU 调试符号。
Development 和 Debug 使用 GDB，支持 F5 断点调试；优化后的 Development
可能出现变量不可见、单步跳行。Profile 和 Release 通过 VS Code 内置的终端入口
（`node-terminal`）直接执行 C++ 程序，不挂 GDB，
用于性能采样与无埋点性能基线。Profile 的 VS Code 入口只启动被测程序；
Tracy 客户端或 Nsight 录制需要另行打开。

所有入口的工作目录都是仓库根目录，沿用 `config/config.json` 的场景、资源与 UI
设置。VS Code 需要安装 Microsoft C/C++ 扩展，且启动 VS Code 的环境应能找到
`cmake`、`gcc`、`g++`、`mingw32-make` 和 `gdb`。
共享配置位于 `CMakePresets.json`、`.vscode/launch.json` 与 `.vscode/tasks.json`；
机器专用的 `.vscode/settings.json` 等文件仍被 Git 忽略。

命令行与 CMake Tools 可使用同名预设，例如：

```powershell
cmake --preset development
cmake --build --preset development
```

四个构建预设只构建 `main`。Debug 的 configure 仍保留模块测试与可选 SpeedTree
数值验证目标；需要运行模块测试时先构建对应测试目标。
其他三个预设不生成这些额外目标。

Development、Profile 和 Release 定义 `NDEBUG`，当前 Shader 编译器因此采用优化构建的
runtime Shader 策略，不保留 Debug 的 `ENABLE_DEBUG_VIEW`；CPU 调试符号与
Shader 调试能力是不同的开关。所有配置共用 `shader/spv/`，启动时会根据编译策略
自动判定缓存失效，切换配置时不要同时启动多个实例。
Release 在这里表示优化运行版本，运行时仍依赖仓库配置与外部资源目录。

### 性能对照构建

编辑器性能应使用独立的优化构建测量，保留 `build/` 的 Debug/校验环境。
以下配置使用 MinGW 的 `-O2 -g -DNDEBUG`，关闭 Vulkan validation；
Shader 也会按现有 Release 策略生成，不是仅关闭校验的单变量对照。

```powershell
cmake -S . -B build-perf -G "MinGW Makefiles" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVULKANLEARN_ENABLE_VULKAN_VALIDATION=OFF -DVULKANLEARN_BUILD_TESTS=OFF -DVULKANLEARN_BUILD_SPEEDTREE_WIND_VALIDATION=OFF
cmake --build build-perf --target main -j4
.\build-perf\bin\main.exe --initial-scene Maps/SC_sifi_head/SC_sifi_head.json
```

对照时固定场景、窗口尺寸、相机和 Details 选中状态，排除启动与编译期间数据。
两种构建共享 `shader/spv/`，切换时由缓存的编译策略失效规则触发重建，不要同时启动。
优化构建不替代 Debug 校验与模块测试；窗口 FPS 也不等于 GPU 执行耗时。

### UI 构建与运行

运行时控制页和编辑器工作区统一使用 RmlUi，构建不再依赖 ImGui，
也不再提供 `VULKANLEARN_ENABLE_DEVELOPER_UI` 编译开关。
`ui/editor-workspace.rml` 和 `.rcss` 定义模型列表、材质槽、
JSON 资产浏览器、属性面板与离线控制。布局采用顶部命令栏、右上 Outliner、
右下 Details、底部 Content Browser / Renderer 标签页，中央/左侧透出 Vulkan 场景。
当前材质工作区支持自由停靠、标签合并、可拖分隔条、窗口内浮动及布局保存/重置；
Asset Editor 是正式 DockPanel，默认与 Viewport 合并为中央标签组，也可拖到其他区域或浮动，
并通过 renderer-owned 离屏纹理显示独立 Viewport；C# 蓝图 提供受限新增入口，
尚不支持原生多窗口、通用场景对象增删或跨资产通用 Undo。
Content Browser 使用目录树、路径导航、搜索、类型筛选和资产网格/列表，
索引 MI、Mesh、Scene、Texture、Terrain、Blueprint JSON。单击选中；双击 MI 在关卡旁打开独立中央标签，
参数编辑、保存和预览命令均在中央材质页，右侧 Details 保留场景对象信息；
材质页左侧显示独立预览球，支持拖动环绕、中键平移、滚轮缩放和重新取景；
双击 Scene 会复用现有 World/RenderGraph 事务切换关卡，并同步打开只读 JSON inspector；
双击 Mesh 进入模型预览标签并自动取景：左/右键拖动旋转、滚轮缩放、中键平移，
双击画面或点击相机按钮重新取景。切换/重载模型会重置相机，不修改模型资产。
Texture 标签提供 PNG/JPEG/TGA/BMP 的等比例 RGB 预览；Terrain 仍为类型预览占位。
贴图预览支持滚轮缩放、鼠标拖动平移，双击或 Fit Image 按钮复位；放大的是有界预览图。
贴图预览按 colorSpace 显示，最长边限制为 1024，标题显示原图尺寸；资源标签包含只读 JSON Details；
支持标签切换、关闭、返回 World、重复打开去重和重新读取 JSON。Scene 仍进入主工作区
只读 JSON inspector 并请求 World 切换。Mesh 通过预览 World 加载模型，Texture 在后台解码 source，不切换 World；
只读 JSON inspector 不展示、不保留任意层级的 `data` 字段及子树，也不读取 JSON 引用的数据文件。
内嵌 `data` 仍受 JSON 语法与输入上限校验，并非跳过文件字节读取。
目录由独立 worker 在初始化及显式 Refresh 时扫描，打开 JSON 也在 worker 完成；
逐帧仅轮询结果并共享不可变快照，不逐帧扫描或读取资产 JSON。
浏览器网格/列表最多挂载 192 个可见条目节点（含预取）及两个高度占位节点，
静止状态复用目录、筛选和 markup；属性值缓存避免重复触发布局 dirty，
Vulkan UI 顶点/索引缓冲按容量倍增复用。具体输入上限见组件契约。
贴图网格按需显示后台生成的真实缩略图，结果缓存在 `.cache/editor-thumbnails/`；
Refresh 重新校验源文件。模型与材质实例使用独立离屏渲染的真实缩略图，只做 GPU 会话缓存；
成图直接供 UI 采样，不回读 CPU 或再次上传，刷新和淘汰沿 GPU 帧完成状态释放。
空闲 750 ms 后分阶段准备，模型导入、shader 编译与贴图解码在 CPU worker 完成，
普通鼠标悬停不重置空闲等待，模型/材质等待渲染时不阻塞贴图缩略图；
复用中性 HDRI、材质预览球与有界 shader 产物；每帧至多上传一张贴图，不切换当前关卡。
首次 GPU 上传/管线创建仍有同步开销。
场景截图缩略图尚未接入；离线专用材质保留类型图标并显示诊断。
Color 参数目前通过 Linear RGBA 数值编辑，不提供旧的 sRGB 调色板。

运行时 UI 资产位于仓库内的 `ui/`，由 `config/config.json -> ui` 配置块选择。
旧 Runtime Debug 页及 Settings/F10/手柄 Start 入口已移除。`Esc` 切换鼠标捕获，
`F11` 切换编辑器沉浸模式（`F1` 兼容）。启动参数
`--dev-ui` 和 `--no-dev-ui` 可以覆盖配置中的开发者 UI 开关：

```bash
build/bin/main.exe --dev-ui
build/bin/main.exe --no-dev-ui
```

RML/RCSS/本地化文件在开发模式下支持候选解析、校验、提交式热重载；失败候选
不会替换最后一个有效页面。实现契约见 `documents/contracts/game-ui-stack.md`。
编辑器后续阶段见 `documents/plan/ui/rmlui-editor-plan.md`。
组件分类与当前覆盖见 `documents/contracts/ui-component-system.md`；
首批组件模块位于 `source/ui/widgets/`，共享样式位于 `ui/widgets/`。

从本工作目录根启动；默认资源目录为相邻的 `../VukanLearnResources`。
分支验证使用独立资源 worktree，保存 MI 或离线输出不会写入原开发资源目录。

### 标准资产修改工具

Scene、Mesh、Texture 提供统一的 get_document / get_capabilities / modify /
validate / save / revert / reload。AI 与脚本使用 `build/bin/vl_asset_edit.exe` 的
JSON Lines 接口；运行中的编辑器通过 Console `editor <JSON>` 调用同一协议。
修改先暂存，明确 save 才写盘，随后经正式加载生效。
命令示例、版本/冲突语义和能力边界见
[资产标准修改接口](documents/contracts/asset-modify-api.md)。

### Lookdev 资产审阅场景

`Maps/SC_lookdev/SC_lookdev.json` 是独立的中性审阅场景：镜面球、
18% 漫反射灰球、24 色参考卡和连续灰阶。它不改变默认启动场景，不启用
TAA/FSR。首次使用先用 Python 3 生成场景与本地派生资产，然后启动：

```powershell
py -3 tool/lookdev/build_scene.py
.\tool\lookdev\run_lookdev.ps1
```

生成器只依赖 Python 3 标准库。启动脚本固定 EV0 场景、
ACES fitted 显示曲线和零 Bloom；不要仅从其他场景继承未记录的后处理状态。
生成器会重建它拥有的 `Maps/SC_lookdev/` JSON，派生 HDR/OBJ/PNG 位于
`resourcePath/Generated/Lookdev/`。材质测量、截图命令与限制见
`documents/rendering/lookdev-scene.md`。

UE `AssetsPreview.zip` 中的文章 HDRI 可以通过 UE 5.7 导出，再用于文章构图
还原场景。该流程不直接解析 `.uasset`，也不覆盖正式 `SC_lookdev` 基线：

```powershell
.\tool\lookdev\convert_assets_preview.ps1 `
  -ZipPath 'C:\Users\pb.adcycyys001\Downloads\AssetsPreview.zip' `
  -UnrealEditor 'D:\sofeware\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe'
```

导出的 HDRI 位于 `resourcePath/Maps/SC_lookdev/Source/UE_AssetsPreview/`，
文章还原场景位于 `resourcePath/Generated/Lookdev/SC_lookdev_article.json`。
该场景使用 ZIP 中的 `HDRI/EpicQuadPanorama_Gray.uasset`，经 UE 5.7
导出为 HDR 后作为唯一环境光和反射源。这是一条几何、材质槽、参考纹理与
HDRI 的文章构图还原链路，不是 UE 材质资产或显示变换的逐项等价迁移；
当前也不包含 TAA/FSR。

### VL 离线渲染

VL 是离线渲染的唯一出图入口；纯 CPU `vl_offline_core` 由根工程构建，
数值测试不需要 Vulkan 窗口。当前架构边界见
`documents/contracts/vl-offline-rendering.md`。VL 已接入冻结的
World/MI/mesh/纹理输入、渐进会话和 Vulkan 离线显示；支持范围只限
Lambert、ClothSheen、ClothFiber、AshikhminVelvet、AshikhminSatin、
AshikhminSlantedVelvet 的已验证配置，遇到不支持的环境、光源、
动画或材质时会在准备阶段拒绝，不回退到 DefaultLit。资源仓库中的
`Maps/SC_offline_shared/SC_offline_shared.json` 是共用实时/离线路径的
Lambert 受控场景，启动不修改全局 `initScene`：

```powershell
build/bin/main.exe --initial-scene Maps/SC_offline_shared/SC_offline_shared.json --dev-ui
build/bin/main.exe --offline --initial-scene Maps/SC_offline_shared/SC_offline_shared.json --dev-ui
```

离线-only 的 A/B 双片案例在
`Maps/SC_offline_cloth/SC_offline_cloth.json`（方向光）和
`Maps/SC_offline_cloth/SC_offline_cloth_area.json`（单面方形面光）；
`SC_offline_cloth_texture.json` 验证 sRGB UV checker，
`SC_offline_cloth_groom.json` 验证 B 臂 signed-RG PFM；
它们使用资源仓库内的 Linen profile，实时模式会明确拒绝面光和 CPU-only
模型。这些是 UV 平片的能力探针，不是论文原图复刻。
同一资源目录的 `SC_offline_cloth_drape.json` 让 A/B 共用一片带褶皱和
有效 UV 的 FBX 布面，观察真实几何下的两种离线散射响应：

```powershell
build/bin/main.exe --offline --initial-scene Maps/SC_offline_cloth/SC_offline_cloth_drape.json --dev-ui
```

这仍是自建几何对照，不是带人模服装的 S-17 验收或论文原图复刻。
FBX 随附的 normal/AO 等烘焙图尚未进入离线材质适配，不参与此场景。

Ashikhmin、Premože 与 Shirley 2000 Fig. 11 的 Satin / Velvet 是两种独立
离线模型，不是同一材质的粗糙度变体。以下两场景共用同一 FBX、物体变换、
无地面的远处竖直背景板、相机与平行光；主光旋转 `[-25,-5,0]`、
强度 `5`。背景板正常受光，投影落在画面外，不通过关闭阴影修饰布料。
启动后设 `offline bounces 0`：

```powershell
build/bin/main.exe --offline --initial-scene Maps/SC_paper_case_ashikhmin_premoze_shirley_2000_fig11_satin/SC_paper_case_ashikhmin_premoze_shirley_2000_fig11_satin.json
build/bin/main.exe --offline --initial-scene Maps/SC_Cloth_Ashikhmin2000_Fig11Velvet/SC_Cloth_Ashikhmin2000_Fig11Velvet.json
```

Satin 的纤维方向取 UV0 U，MI 可通过 `u_fiberRotationDegrees` 旋转；
两臂的染色、灯位和机位是本地选择，不是恢复的原场景元数据。
验证结果与差异见 `documents/contracts/shading-model-case-records.md`
的 `AS-01` / `AS-02`。两者实时模式均明确拒绝。

Fig. 13 的黑色与红色倾斜纤维 Velvet 使用独立离线模型
`AshikhminSlantedVelvet`，不是修改 Fig. 11 的各向同性 Velvet。
两个场景仅改变染色，共用近观察方向的平行光和无地面背景：

```powershell
build/bin/main.exe --offline --initial-scene Maps/SC_paper_case_ashikhmin_premoze_shirley_2000_fig13_slanted_velvet_black/SC_paper_case_ashikhmin_premoze_shirley_2000_fig13_slanted_velvet_black.json --console-command "offline bounces 0"
build/bin/main.exe --offline --initial-scene Maps/SC_paper_case_ashikhmin_premoze_shirley_2000_fig13_slanted_velvet_red/SC_paper_case_ashikhmin_premoze_shirley_2000_fig13_slanted_velvet_red.json --console-command "offline bounces 0"
```

束轴与法线夹角为论文的 40°，侧面微法线平均方向相应倾斜 50°；
`sigma_x=0.5`、`sigma_y=∞`、`f0=0.05`。UV 束轴方位、染色、
灯位与机位为本地选择，不增加 runtime ID 或 forward/deferred 支持。
该分布不是物理 height field，不保证 `f0=1` 时能量守恒。
模型边界与数值证据见 `AS-03`。

单面方形面光的背面虽不发光，光源几何仍会遮挡相机主射线；调灯位或
尺寸时要避免让它进入主视线。
Estévez 与 Kulla 2017 Fig. 1 的 A 臂红布粗糙度扫描使用独立论文场景，
五块同源布面从左到右取 `r=0.15/0.25/0.40/0.65/1.0`：

```powershell
build/bin/main.exe --offline --initial-scene Maps/SC_paper_case_estevez_kulla_2017_sheen_roughness/SC_paper_case_estevez_kulla_2017_sheen_roughness.json --dev-ui
```

论文没有给出 Fig. 1 的完整相机、灯光和颜色数值；这里的平行光与灰色
舞台是受控对照所选配置，不是对原图灯型的断言。

Ashikhmin、Premože 与 Shirley 2000 §6.4 / Fig. 11 右图的 Velvet
独立离线 case 使用倒高斯微法线分布 `sigma=0.5` 和相关遮蔽。
场景沿用 VL 布料 FBX，近黑色、背景板、平行光和相机均为本地选择。
论文明确使用单个远处小光源且不包含间接光；在离线控制台执行
`offline bounces 0` 后再观察和导出：

```powershell
build/bin/main.exe --offline --initial-scene Maps/SC_Cloth_Ashikhmin2000_Fig11Velvet/SC_Cloth_Ashikhmin2000_Fig11Velvet.json --dev-ui
```

此 case 验证材质响应，不是 Fig. 11 的像素级复刻，也不增加 runtime `ShadingModelID`。

开发者 UI 的 Offline Renderer 面板可切换模式、设置 spp/反弹/seed、
暂停、继续、停止、重启，查看 Combined/Direct/Indirect 并导出。
使用 `--dev-ui` 启用后，在编辑器底部选择 Renderer 标签即可打开面板。
控制台也可输入 `offline on|off|samples N|bounces N|seed N|lightSamples N|aov MODE|exposure X|pause|resume|stop|restart|export`。
离线相机移动时以 1 spp 多核预览并放大显示：默认半宽、半高，准备与
等待超过 30 ms 时按每轴 1/2、1/4、1/8、1/16 逐级降档；低档预览
连续三张低于 5 ms 才升一级，给约四倍像素量留出余量，避免频繁跳档。
上一张预览尚未完成时合并后续
相机请求，完成后再取最新视角。最后一次
相机变化后 200 ms 无新变化，恢复原分辨率与面板或命令设置的目标
spp，并重新累积。移动预览与全尺寸导出是不同输入版本，不能混用
像素或宣称复用预览样本。移动预览还带约 30 ms 的软截止：按分散
8×8 tile 计算，截止后不再领取新 tile，已领取 tile 安全完成后发布
有效像素掩码；显示端只用同一预览的有效像素补洞，零有效像素时保留
上一张图。部分预览不进入正式 film、spp 累积或导出，超时期间仍可能
因单条光路或正在执行的 tile 出现软超时；新图未就绪时视口继续显示
上一张已发布结果。
导出位于配置 `resourcePath/Generated/OfflineRenderer/`，包含线性 PFM、
展示 PPM 与 JSON 清单；`--offline` 是进程启动模式，不改变场景资产。
边界、回归与未完成的人工交互验收见
`documents/plan/rendering/offline-renderer-vl-integration-plan.md`。
模块测试由根 CTest 运行；VL 场景导出和压力检查在
`tool/validation/offline/`，不另建独立渲染程序或第二份场景/材质配置。

### SpeedTree 风数值验证

本机安装 `C:\Software\SpeedTree_10.2.0_extracted\{app}` 时，默认构建额外的
`speedtree_wind_validation.exe`。它直接使用 Modeler 的 `SpeedTreeWind.h` 推进
官方 Runtime SDK CPU 状态，并逐层比较 SDK source-space 与 VulkanLearn Y-up
公式：

```bash
cmake --build build --target speedtree_wind_validation -j
build/bin/speedtree_wind_validation.exe
ctest --test-dir build -R speedtree_wind_numeric --output-on-failure
```

默认从 `config/config.json -> resourcePath` 查找
`Maps/SC_speedtree/Source/Models/Oak_Complex_Rules.stsdk`，也可以把其他 `.stsdk` 路径作为第一个参数传入。
Oak 使用固定顶点和贴图接缝回归；其他树种自动选择各 section 中风权重最大的代表顶点。

如果 Modeler 安装在其他位置，配置时传入
`-DSPEEDTREE_MODELER_ROOT=<Modeler 根目录>`。没有 SDK 的机器会跳过这个可选目标，
不会影响 `main.exe`。顶点对照目前是 CPU 上的双公式求值，不是 Vulkan shader 的
GPU readback；实际 SPIR-V 输出仍需后续 capture/readback 验证。

## 运行时资源位置

当前仓库不再以 git 形式保存完整 `resources/` 资产。

程序运行时从 `config/config.json` 的 `resourcePath` 读取资源树。该路径应直接指向独立资源仓库根目录，例如：

```json
"resourcePath": "D:\\YYBWorkSpace\\GitHub\\VukanLearnResources"
```

运行前请确保 `resourcePath` 至少包含：`Common/`、对应的 `Maps/SC_<scene-name>/`，以及用于导入、运行时、验证和截图输出的 `Generated/`。场景专属资产放在场景目录下的 `Meshes/`、`Materials/`、`Textures/`、`Environments/` 和 `Source/`；RenderGraph Pass 材质和引擎级 Profile 放在 `Common/`。

本地生成资源输出写入 `resourcePath/Generated/`。

单次启动需要临时覆盖 `config/config.json -> initScene` 时，可以使用进程级参数，
不会改写配置文件：

```bash
build/bin/main.exe --initial-scene Maps/SC_speedtree/SC_speedtree.json
```

## Shader 增量编译与热重载

启动阶段 Shader 编译是内容寻址增量构建：未修改的 entry/variant 直接命中
`shader/spv/shader-build-cache.json`，不再调用 shaderc。首次启动摘要类似：

```text
Shader build: entries=29, artifacts=17, hits=0, misses=17, compiled=17,
shaderc=29, failed=0, elapsedMs=2043.1
```

第二次无修改启动应看到 `compiled=0, shaderc=0`。缓存身份、manifest schema 和
失败语义见 `documents/contracts/shader-build-cache.md`。

运行时支持以下调试命令：

```text
shaderreload changed   重新编译并事务性发布受影响的 live graphics shader
shaderreload all       强制重新发布全部 live graphics shader
shadercache stats      输出启动构建统计与 manifest artifact 数
```

`shader/glsl` 下的 `.vert/.frag/.comp/.glsl` 与 `M_*.json` 会被自动监听：
CPU 编译在独立 worker 完成，EngineLoop 在 Render Thread 安全点创建 Vulkan
对象并事务提交，旧 Pipeline/descriptor 经 GPU frame epoch 延迟退休。热重载
的 ABI 边界、参与者和事务语义见 `documents/contracts/shader-hot-reload.md`。
连续保存会按 stable source identity 求并集；source epoch 用于快速拒绝旧任务，正式
提交前仍复核 primary/include digest。`M_*.json` schema 更新会在候选 World/Graph
中迁移兼容的 live 参数和贴图，失败时 active runtime 与正式 artifact 整包保持。

可重复验证入口（全部 `--exit-after-tests`，退出码 0 成功、2 失败，串行执行）：

```text
build/bin/main.exe --shader-reload-test --exit-after-tests
build/bin/main.exe --shader-compute-reload-test --exit-after-tests
build/bin/main.exe --world-graph-transaction-test --exit-after-tests
```

`--shader-force-rebuild` 强制重建全部启动 artifact。模块单元/集成测试统一由
GoogleTest 承载，CTest 会按每个 `TEST` 独立发现和报告；首次配置会通过 CMake
FetchContent 获取固定的 GoogleTest 版本。测试覆盖 BLAKE3 向量、规范化边界、
write-if-changed、FileMonitor 防抖、warm start、依赖失效、manifest 损坏/schema
升级和提交回滚：

```bash
ctest --test-dir build --output-on-failure
ctest --test-dir build -R shader_build --output-on-failure
```

## 性能分析工具使用

本项目集成了两种性能分析工具：**Tracy Profiler** 和 **NVIDIA Nsight Systems**。代码中的 `PROFILE_SCOPE` 宏会同时触发两者的标记，你可以根据需求选择使用。

开发时可以打开 Vulkan validation layer 来持续监测 API 使用错误；这类构建适合查 correctness，不适合作为长时间挂机 FPS 基线。Profiler 标记和 validation layer 都会显著改变高 FPS 空转场景下的 CPU/Vulkan API 开销，长时间挂机测性能时不要把它们打开后的 FPS 当作真实性能。

重新配置性能基线时显式传入 `OFF`，避免已有 `build/` 缓存沿用开发监测配置：

```bash
cmake -S . -B build -G "MinGW Makefiles" -DVULKANLEARN_ENABLE_TRACY=OFF -DVULKANLEARN_ENABLE_NVTX=OFF -DVULKANLEARN_ENABLE_VULKAN_VALIDATION=OFF
cmake --build build -j
```

需要做性能分析或 Vulkan API 校验时，显式打开对应选项后重新配置。日常开发如果只想监测 Vulkan 报错，可以只打开 `VULKANLEARN_ENABLE_VULKAN_VALIDATION`：

```bash
cmake -S . -B build -G "MinGW Makefiles" -DVULKANLEARN_ENABLE_TRACY=ON -DVULKANLEARN_ENABLE_NVTX=ON -DVULKANLEARN_ENABLE_VULKAN_VALIDATION=ON
cmake --build build -j
```

调试断点会让进程暂停，Tracy 这类实时 profiler 的 socket 连接可能因此被对端关闭，并在 Windows 上表现为 `10054 ConnectionReset`。如果当前目标是单步调试逻辑而不是采样性能，可以关闭 Tracy/NVTX，但保留 Vulkan validation layer 用来检查 API 错误。

### 1. 使用 Tracy Profiler (实时 CPU 分析)

Tracy 适合在开发过程中实时查看 CPU 函数耗时、帧率波动和内存分配。

**版本说明：**
项目集成的 Tracy 代码已更新为 **v0.13.1** (发布版)，你可以直接从 [GitHub Releases](https://github.com/wolfpld/tracy/releases/tag/v0.13.1) 下载 `Tracy-0.13.1.7z`，解压后直接使用 `Tracy.exe`。
*注意：无需再手动构建客户端。*

**使用步骤：**
1.  **启动 Tracy**：
    运行下载的 `Tracy.exe`，点击界面的 **"Connect"** 按钮等待连接。
2.  **运行程序**：
    启动 `build/bin/main.exe`。
3.  **查看数据**：
    Tracy 会自动连接并开始滚动显示实时火焰图。

### 2. 使用 NVIDIA Nsight Systems (GPU 同步与系统分析)

Nsight Systems 适合分析 CPU 与 GPU 的交互、Vulkan API 的调用时序以及 GPU 的负载情况。

**步骤：**
1.  **安装工具**：
    确保已安装 NVIDIA Nsight Systems (通常随 Vulkan SDK 或 CUDA Toolkit 安装，也可单独下载)。
2.  **创建项目**：
    打开 Nsight Systems，创建一个新项目。
3.  **配置目标**：
    - **Target application**: 选择你的 `build/bin/main.exe`。
    - **Working directory**: 建议设置为项目根目录 (例如 `D:\GitHub\VulkanLearn`)，并确保 `config/config.json` 中的 `resourcePath` 指向可用资源仓库。
4.  **开始录制**：
    点击 **"Start"** 按钮，程序会自动运行。
5.  **结束分析**：
    运行一段时间后点击停止，Nsight 会生成报告。
6.  **分析时间轴**：
    在报告中，你可以在 "NVTX" 行 (或 "Threads" 下) 看到代码中埋点的 `PROFILE_SCOPE` 区域，并能将其与下方的 "Vulkan Queue" (GPU 工作队列) 上下对齐，从而精确分析 CPU 提交命令和 GPU 执行命令之间的延迟（例如 `WaitForFences` 的等待情况）。

## 代码接入指南

为了在性能分析工具中显示自定义的标记，请使用 `source/Profiler.h` 中定义的宏。这些宏会自动适配 Tracy 和 Nsight Systems。

### 1. 标记作用域 (PROFILE_SCOPE)

用于手动标记一段代码块的执行范围。

```cpp
#include "Profiler.h"

void SomeFunction() {
    {
        PROFILE_SCOPE("MyCustomScope");
        // ... 需要分析的代码 ...
    }
}
```

### 2. 标记函数 (PROFILE_FUNCTION)

用于自动标记整个函数，名称自动获取。

```cpp
#include "Profiler.h"

void MyFunction() {
    PROFILE_FUNCTION();
    // ... 函数体 ...
}
```

### 3. 标记帧 (PROFILE_FRAME)

用于标记帧的结束，通常在主循环末尾调用（Tracy 专用）。

```cpp
// 在主循环中
while (running) {
    // ... 渲染逻辑 ...
    PROFILE_FRAME();
}
```

## 目录结构
- `source/`: 源代码
- `shader/`: GLSL 着色器
- `resources/`: 不再作为运行时资产目录；运行时资产由 `config/config.json -> resourcePath` 指定
- `config/`: 渲染管线和场景配置
- `documents/`: 正式设计文档和规划文档
- `extern/`: 第三方库 (SDL3, Assimp, GLM, Tracy, NVTX 等)
