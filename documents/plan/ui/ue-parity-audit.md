# UE 风格编辑器：能力对照与轮次审计

日期：2026-10-11。分支：`codex/rmlui-editor`。

这份文档是**工作审计**，不是设计契约。它记录"哪些能力在 ImGui→RmlUi 迁移中丢失了"、
每一轮的验收证据、以及尚未达成一致的判断。正式契约仍在
`documents/contracts/ui-component-system.md`，路线图仍在
`documents/plan/ui/rmlui-editor-plan.md`。

## 为什么需要这份文档

迁移完成后，代码里零 TODO/FIXME，缺口全部表达成显式拒绝或空状态，因此
**"看起来完整"和"确实完整"在源码里无法区分**。判断缺口的唯一可靠方法是把
迁移前的实现当作能力基线逐条对照。

基线命令：

```powershell
git show HEAD:source/editor/ui/materialInstanceAssetEditorPanel.cpp
```

## 能力对照表（HEAD = ImGui 版）

| 能力 | HEAD（ImGui） | 迁移后（RmlUi） | 处置 |
| --- | --- | --- | --- |
| 顶部菜单栏 | 无 | 无 | 第 1 轮新增 File/Edit/Window/Help |
| 视口工具条 | 无 | 无 | 第 1 轮新增；仅 view mode 有真后端 |
| 面板显隐 | 无 | 无 | 第 1 轮新增 |
| 数值滑块 | `ResolveSliderRange` + `ImGui::SliderFloat` | 纯文本框 | 第 3 轮恢复 |
| 颜色色块 / 十六进制 | `LinearToSrgb`/`ToSrgbHexByte` + 色块列 | 无 | 第 3 轮恢复 |
| 颜色取色器 | `ImGui::ColorPicker4` 弹窗 | 无 | **未恢复**，改用方案待定 |
| 折叠分类 | `CollapsingHeader`：General / Render States / Parameters / Texture Bindings | 三个平铺 Heading | 第 3 轮恢复 |
| 资源编辑器返回按钮 | 不适用 | `resource-json-close` 分支存在但无触发控件 | 已删除（见下） |
| 纹理选择弹窗 | `BeginPopup("MITexturePicker")` | `<select>` 下拉 | 不恢复：下拉覆盖同一需求 |
| 场景对象树 | 无（只有 MI 模型列表） | 无 | 第 4 轮新增真实 World actor 树；第 5 轮行窗口化 + 组内排序 |
| 大纲逐 actor 可见性 | 无 | 无 | 第 6 轮新增：World 可见性状态 + 快照过滤 + 眼睛开关（仅运行时） |
| 多 section actor 的整 actor 隐藏 | 不适用 | 第 6 轮只对单 section 生效 | 第 7 轮修正：按 `sceneObjectIdentity` 一次写全所有 section |
| 大纲多选 / 拖放 | 无 | 无 | 未排期，属于场景文档阶段 |
| gizmo / 变换编辑 | 无 | 无 | 未排期，需先改场景数据模型 |
| Undo / Redo | 无 | 无 | 未排期；Edit 菜单已留禁用占位 |

**HEAD 也没有**的东西不要记成迁移损失：菜单栏、视口工具条、面板显隐、场景树、
gizmo、Undo/Redo 在迁移前同样不存在。它们属于"向 UE 对齐"的新增工作，不是回归。
第 4 轮把其中的"场景对象树"做成了真实 World actor 树（见下文），它仍然不是迁移损失。
第 5 轮修掉了第 4 轮留下的两个短板：行窗口化（不再逐帧拼整棵树的 markup）
与低基数分组前置（灯光/相机/环境不再被 122 行网格挤出首屏），并补了组内排序控件。
第 5 轮的像素证据表里"树里只有网格行可见，灯光在可视区之外"一行，
读的就是第 5 轮改掉的症状；那张 `outliner-baseline.bmp` 现在作为 before 基线被重新测量。

## 第 1 轮：菜单栏、面板显隐、视口工具条

- 菜单栏位于 `#editor-toolbar` 行内左侧；`DockWorkspace::Panels()` 未变。
- 面板显隐由 `EditorDockingController` 拥有：隐藏保留布局树位置、隐藏激活标签
  移交激活、整栈全隐藏则折叠出几何、拒绝隐藏最后一个可见面板；布局文件新增
  可选 `"hidden"` 数组，缺该键的旧文件照常可读。
- 视口工具条只有 view mode 接真实后端（`UiActionType::SetDebugViewMode`，
  Lit=0 / Unlit=1）。变换工具与变换空间是**纯呈现状态**，只切高亮、不发命令。
  Wireframe 无后端，按钮禁用并标注，未伪造。

### 证据

- 构建 `editor_ui_tests` / `editor_viewport_tests` / `main` 成功。
- 文档化 ctest 选择 **229/229 通过**，4 项既有 symlink 用例按设计 skip。
- 断言未削弱：一处既有测试的探测像素从 `{50,100}` 移到 `{50,200}`，
  因为原坐标落在新增的视口工具条上。

## 第 2 轮：死代码与命令陷阱

- 删除 `source/editor/executor/materialInstanceEditorCommandExecutor.{h,cpp}`
  （全仓库无引用，`source/CMakeLists.txt` 走 GLOB_RECURSE，无需改构建）。
- `SetMaterialRenderStateOverride` / `ClearMaterialRenderStateOverride` 此前在
  runtime 顶层 switch 无 case，直接提交被判 `InvalidCommandType`，只能经受
  batch。现已直接路由到 `ExecuteEditorCommandBatch` 逐项调用的同一对 service
  方法，并纳入自动预览刷新条件。
- 陷阱由**修复前的真实失败**证明：先加测试 → 失败（`unsupported editor command
  type`）→ 修源码 → 通过。产物 `material_instance_editor_production_tests`
  **22/22 通过**。

## 第 3 轮：恢复迁移丢失的 Details 控件

滑块（含 `ResolveSliderRange` 语义：通道范围 > 参数范围 > 由当前值推导的固定兜底）、
sRGB 色块与十六进制、可折叠分类。色块为只读；HSV 取色器不在本轮范围，原因见上表。

## 第 4 轮：真实 World actor 树 + 选择驱动 Details

`#editor-navigation > #scene-models` 以前由 `MaterialInstanceEditorSnapshot::sceneModels`
构建，只包含携带有效材质实例的网格（`RenderSystem::GetMaterialInstanceModelContexts`
要求 `materialInstance.IsValid()`）。本轮换成真实大纲：

- **数据源**：`EngineLoop::UpdateUiViewModel` 在设置 `snapshot.activeWorldPath` 的同一处
  用 active World 调 `BuildEditorSceneActors(world)`，经
  `UiSubsystem::SetSceneActors(...)` 发布，位置与 `SetMaterialInstanceSceneModels` 并排、
  在 `uiSubsystem->Update(snapshot)` 之前。列表是
  `shared_ptr<const std::vector<EditorSceneActorSnapshot>>`，逐帧不深拷贝。
- **内容**：网格（按 `sceneObjectIdentity` 聚合 section，逐 section 带
  mesh/material/MI 路径/槽名/风剖面 key 与"是否带材质实例"）、
  directionalLight / pointLight / spotLight / squareAreaLight（参数与渲染快照同源：
  复用 `WorldSnapshotBuilder` 的 `BuildWorld*LightSnapshot`）、相机七个字段、
  环境（type/intensity/cubeSize/hdrPath/对象名）、SpeedTree 风剖面
  （key、source bounds、common 标量与 do* 开关）。
- **树形状**：`SceneActorGroupSpecs()` 固定分组顺序（Meshes / Directional Lights /
  Point Lights / Spot Lights / Square Area Lights / Cameras / Environments /
  SpeedTree Wind Profiles），组内按显示名做 ASCII 大小写不敏感比较、再按稳定身份打破平局；
  分组可折叠，折叠只影响行是否挂进 DOM，不改筛选计数。每行一个类型图标。
- **搜索与过滤**：搜索复用 Content Browser 的 `FoldAsciiCase`（现已提取到
  `source/editor/ui/editorTextMatch.h`，两处共用一份），只折叠 ASCII 大小写、
  保留 UTF-8 多字节名称；类型过滤取值就是分组键；底部计数显示筛选结果
  （`N / M actors`）。三者都不提交资产或运行时命令。
- **选择**：mesh actor → 既有 `RequestModelSelection`；材质槽 → 既有
  `RequestMaterialSelection`（MI 文档与 Material Slots 行为不变）；
  非网格 actor → 新 `RequestActorSelection(稳定身份)`，并要求 EngineLoop
  清掉渲染端 mesh 描边，避免大纲与视口各留一个选中对象。
- **Details**：`#details-content` 里新增一个以类型命名的可折叠分组
  （键 `actor-<分组键>`，与 General/Render States/Parameters/Texture Bindings 并列），
  只读字段按行带 id，值变化只刷新该行（节点身份不重建）；
  没有打开的 MI 文档时 Details 不再为空。
- **两条真实写入路径**（都复用既有命令，不新增 UI→renderer 路径）：
  主方向光的 Intensity（文本输入 + 与运行时页同区间的 0..10 滑条）与
  CSM Cast Shadows（运行时页同款 Enabled/Disabled 按钮），分别提交
  `UiActionType::SetSunIntensity` 与 `UiActionType::SetCsmCastShadows`。
  `SaveCsmSettingsToScene` 没有放进 Details：本轮只surface 两条，保存仍走运行时页。

### 本轮刻意不做

- **没有逐 actor 可见性/隐藏开关**。`WorldMeshObject`（`source/world/world.h`）
  没有可见性字段，`MeshDrawSnapshot`（`source/world/worldSnapshot.h`）与渲染图也都不读
  任何逐对象可见性状态；加它要同时改场景 JSON schema、`SceneAssetValidator`、
  World 加载/构建与渲染路径，属于单独一轮。放一个点了没有效果的隐藏按钮
  正是这个分支一直在删的东西。
  **（第 6 轮已落地，且证明"必须改场景 schema/validator"这一半是错的：
  可见性做成 World 侧运行时状态即可，见下文第 6 轮。）**
- **没有地形节点**：`World` 没有 terrain 访问器，仓库里也没有 terrain 运行时
  （`source/terrain.h`/`terrain.cpp` 在本 worktree 中不存在，尽管 `AGENTS.md` 仍引用它们）；
  场景选择器只把 `"terrain"` 列进受支持类型（`source/ui/sceneObjectTypeSupport.h`），
  资产浏览器只有 Terrain JSON 元数据。因此大纲不凭空造地形行。
- 没有 transform 编辑、gizmo、增/删/改名、Undo/Redo、拖放；大纲没有虚拟化
  （每帧重建 markup 字符串后由值缓存决定是否写 DOM，与迁移前的模型树同一策略）。
- 环境对象名来自新增的 `WorldEnvironment::name`（由 `worldBuilder` 从场景 JSON 写入）；
  场景没有 environment 对象时回退到固定标签 `Environment`。

### 渲染验证发现的两个真实缺陷（已修）

1. **分组折叠只改了 ± 标记，没有隐藏子行**：`RefreshOutliner` 把每组所有 actor 行都
   挂进了 DOM。单元测试当时没覆盖"折叠后子行消失"，是像素探针（折叠 Meshes 后
   网格图标像素没有减少）先发现的；现在由
   `EditorUiTest.OutlinerGroupCollapseHidesItsActorsAndKeepsTheFilterCount` 守住。
2. **组标题整行不可见**：`.tree-row .list-item` 曾用 `display: flex`，
   而 RmlUi 不会给 flex 容器里的裸文本子节点排版；分组行没有图标，标签就是裸文本，
   于是标题一个字都不画（像素实测：分组行在 113–142 行区间内只有 3 个 `-` 像素）。
   改为保持 `list-item` 的块级/行内默认布局、只给 `img.row-icon` 加
   `vertical-align: middle`。
3. **Details 强度控件的区间耦合**：最初允许键入任意有限值；像素探针实测到
   键入 13 后运行时页那个**隐藏**的 sun 滑条（`ui/runtime-control.rml` 的
   `#sun-intensity-slider`，0..10 双向 data binding）把它截断成 10 并回写一条
   `SetSunIntensity(10)`，日志出现 13/10/13/10 的来回命令。
   现在按同一区间校验并给出人类可读提示，不做无声改写；
   `EditorUiTest.ActorIntensitySliderRangeMatchesTheRuntimeSunSlider`
   直接把 Details 滑条的 min/max 与运行时页 RML 里的值对比，防止再次漂移。

## 第 4 轮验证（2026-10-11）

```powershell
cmake --build build --target editor_ui_tests editor_viewport_tests main material_instance_editor_production_tests -j8
ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure
ctest --test-dir build -R "MaterialInstanceEditor" --output-on-failure
```

- 四个目标全部构建成功（唯一告警是 MinGW 对 `<ciso646>` 的既有 deprecation 告警）。
- 编辑器 UI 选择 **262/262 通过**（基线 245/245，本批净增 17 项：
  `widgetTests.cpp` 4 项、`editorUiTests.cpp` 13 项；另有 1 项原模型树用例
  改写成 actor 树版本）。**两次运行都是 262/262**，
  含 4 项既有 `JsonAssetLibraryTest` 符号链接用例按原条件跳过。
- `MaterialInstanceEditor` **22/22 通过**，与基线一致。

### 像素证据（1280×720，`--dev-ui`，默认球阵场景）

探针 `artifacts/rmlui-editor/outliner_probe.py`（gitignored 的 `artifacts/` 下）驱动真实窗口，
截图经 `bmp_probe.py` 校验载荷后逐点读数。默认场景（`config/config.json -> initScene`
= `Maps/SC_sphere_array/SC_sphere_array.json`）实测含 **122 个 mesh、1 个 directionalLight
（`Sun_Light`）、1 个 pointLight（`Key_Point_Light`）、1 个 camera（`Camera_01`）、
1 个 environment（`Environment_01`，hdri）**，没有 spotLight / squareAreaLight / 风剖面。

| 观察点 | 读数 | 结论 |
| --- | --- | --- |
| 右列顶部 `#252528` | 行 74–107 | 新的大纲过滤栏（搜索 + 类型过滤）已渲染 |
| 大纲面板底色 `#2d2d31` | 行 109–247（树 109–226、计数栏 227–247） | 树与计数栏都在；计数文字像素在 235–241 |
| 折叠 Meshes 前网格图标色像素 | 286（其中 178 是 dock 活动标签下划线 `#62A58D`，分类器与网格图标同色系） | 树里只有网格行可见，灯光在可视区之外 |
| 折叠 Meshes 后 | 网格图标 0（只剩 178 下划线像素），暖色灯光图标 40 像素，bbox (920,180)-(931,191) | 灯光行确实挂进了树 |
| 点击 `Sun_Light` 行后 Details | `#C8C8CF` 滑条块 **恰好 120 像素**（10×12）于 (933,599)-(942,610)；选中/未选中按钮长段各 13 行，x=1160..1209 / 1216..1268；分组标题色 `#A6C6BA` 出现在 y=444 | actor 分组与两个可编辑控件都渲染了 |
| 点击 CSM 按钮 | 选中段与未选中段互换，日志 `[Diagnostics][Info] CSM disabled.` | UI → UiAction → RuntimeCommand → RenderSystem → 冻结快照 全链路生效 |
| 键入 5 后 | 日志恰好一条 `Sun intensity set to 5.000000`；滑条块从 x=933 移到 x=1077（轨道 895..1269，占 48.7% ≈ 0..10 的 50%） | 强度写路径生效且控件跟随真实值 |
| 每张截图载荷 | 2764800 B（完整，无半截） | 读数可信 |

- 该进程由探针强制终止，**这是渲染证据，不是 teardown 证据**。
- 产物：日志 `artifacts/rmlui-editor/outliner-121423.log`（stderr 0 字节）；
  截图在隔离资源目录 `Generated/Screenshots/`：
  `outliner-baseline.bmp`、`outliner-collapsed.bmp`、`outliner-sun.bmp`、
  `outliner-csm.bmp`、`outliner-intensity.bmp`；探针与地图脚本
  `artifacts/rmlui-editor/outliner_probe.py`、`outliner_map.py`、`click_check.py`。
- 未能上屏观察的 actor 类型：spotLight、squareAreaLight、SpeedTree 风剖面
  （默认场景没有；换场景需改资产，本轮没有为此改动场景资产）。
  这三类的行渲染由 `SceneActorGroupSpecs()` 与单元测试覆盖，没有像素证据。
- 未验证：真实鼠标的搜索键入与下拉选择手感（合成点击只验证了搜索框获得焦点边框
  `#62A58D`）、真实拖动滑条（合成拖动不改变 range 控件，与第三轮记录的限制相同）。

## 第 5 轮：大纲行窗口化、组内排序、低基数分组前置

第 4 轮留下两个具体短板：整棵树每帧拼成一个 markup 字符串交给
`WidgetDocument::SetMarkup`（实测 122 mesh 场景约 40 KB/帧），
以及默认视图里灯光/相机/环境被 122 行网格挤出可视区（折叠前灯光图标像素为 0）。
本轮不加功能面，只解决这两点：

- **行窗口化**：复用 Content Browser 的窗口化做法（读实时可视高度与滚动偏移，
  算有限行窗口 + 上下各两行预取，用两个 spacer 保留未挂载行的高度）。
  预算常量 `SceneActorMaxMountedRows = 192` 与 `SceneActorOverscanRows = 2` 与
  Content Browser 的 `MaxVisibleItems` 同量级，位于纯模块
  `source/editor/ui/editorSceneActorTree.{h,cpp}`；
  窗口数学、行集合构建（含折叠/展开/排序/槽行）与行内容签名都在这个模块里，
  因此可以不建 RmlUi 文档直接测。
- **折叠仍然只贡献表头**：折叠的分组不进入行集合，也就不占窗口槽位、不产生 spacer；
  整棵折叠后的树必须一次装进窗口（纯测试覆盖）。
- **行高实测**：宿主逐帧从已挂载行的 margin box 读行高（首帧用 RCSS 兜底 30），
  并要求树行等高。实测发现原 RCSS 下分组行（粗体）28.4px、actor/槽行 29.7px **不等高**，
  于是把行高在 RCSS 里显式固定为 30px（`.list-item` 行高 16px、不换行、省略号裁剪），
  再由 `EditorUiTest.OutlinerRowHeightsAreUniformAcrossRowKinds` 量三种行形态守这条假设。
- **组内排序**：过滤栏新增 `#outliner-sort`，取值与 Content Browser 的 `#browser-sort`
  同一套键（`name-asc` / `name-desc`）。排序只作用于组内，分组顺序不变；
  选中身份、展开集合与滚动偏移都不受影响。没有第三种排序：组内所有行类型相同，
  按类型名排序在组内恒为平局，因此没有为凑数加一个没有可见效果的选项。
- **低基数分组前置**（最小改法）：只改 `SceneActorGroupSpecs()` 的顺序
  （灯光 → 相机 → 环境 → 风剖面 → Meshes），默认展开状态不变。
  没有默认折叠 Meshes：Meshes 现在排在最后，折叠它不会改变首屏可见内容，
  只会让最常用的选择目标多一次点击；排序控件也不参与分组排列。

### 验证（2026-10-11）

```powershell
cmake --build build --target editor_ui_tests editor_viewport_tests main material_instance_editor_production_tests -j8
ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure
ctest --test-dir build -R "MaterialInstanceEditor" --output-on-failure
```

- 四个目标全部构建成功；编辑器 UI 选择 **275/275 通过**（基线 262/262，净增 13 项：`widgetTests.cpp` 7 项纯窗口/排序、`editorUiTests.cpp` 6 项面板与布局），
  **两次运行都是 275/275**；`MaterialInstanceEditor` **22/22 通过**，与基线一致。

| 观察点（探针 `artifacts/rmlui-editor/outliner_window_probe.py`） | 第 4 轮构建 | 本轮 |
| --- | --- | --- |
| 首屏（scroll 0、什么都不折叠）图标色 census | mesh 108，light 0，camera 0，environment 0 | light 73，mesh 0（灯光行在首屏，网格行在折叠线下） |
| 相机 / 环境行可达所需滚动 | 122 行网格之后 | 2–3 个滚轮刻度（每刻度 80px = 2.67 行；RmlUi 平滑滚动让刻度到行号有浮动） |
| 可视行数 vs 树行数 | 4 行可见，整棵树 130 行都在 DOM 里 | 3–4 行可见，挂载行数为窗口大小（≤192 预算由单元测试按真实 DOM 节点数守） |
| 滚动 | — | 滚动换一批挂载行；滚回顶部三行指纹与首屏完全一致；滚动条滑块 24px（min-height 下限）说明滚动范围是整棵树 |
| Z-A 排序 | 无该控件 | 非网格行三行指纹距离 0.0（排序不动它们），计数栏文字距离 0.0；`A→Z` 第一行网格 == `Z→A` 最后一行（距离 0.0），`A→Z` 最后一行 == `Z→A` 第一行网格（距离 0.0），且 `A→Z` 与 `Z→A` 的第一行网格不同 |
| 截图载荷 | 2764800 B | 2764800 B（完整） |

- 排序的像素比较用"行带墨迹指纹"（按行内相对坐标 + 量化颜色的列剖面），
  因此与滚动相位无关；行类型用从 `ui/icons/*.png` 现算的描边色（色相规则）判定，
  不写死颜色。
- 未验证 / 像素方法的边界：探针数不到被裁剪但已挂载的 DOM 节点，
  因此"≤192 挂载行"由单元测试断言真实节点数，像素只证明"上屏行数不随树增长"；
  文字没有回读，行身份靠指纹比较（同名行会被判为同一行）；
  spotLight / squareAreaLight / 风剖面仍无像素证据（默认场景没有这三类对象）。
- 该进程由探针强制终止，**这是渲染证据，不是 teardown 证据**。

### 本轮之后仍然缺席（与第 4 轮清单一致，未新增）

- 逐 actor 可见性的**持久化**：第 6 轮加了运行时开关，但它不写场景 JSON、
  `SceneAssetValidator` 不认识、也没有保存命令；重载场景后全部可见。
- 多选、拖放/重父级、改名：大纲一次只有一个选中身份，挂载行与事件地址一对一。
- transform 编辑与 gizmo：没有渲染后端，也没有命令。
- 通用虚拟化：窗口化只在世界 actor 树上实现，通用 `TreeRow`、模型树与 Sources 树未变。

## 第 6 轮：逐 actor 可见性（World / 快照 / 大纲）

第 4、5 轮都把"逐 actor 可见性"记为缺席，理由是"渲染路径没有逐对象可见性状态"。
本轮把它补上，并且刻意**越出 UI 层**改 World 与渲染快照：

- **状态归属**：可见性落在 World 拥有的对象上——`WorldMeshObject::visible`（网格按值存储）、
  `SceneNode::visible`（三类以 `shared_ptr` 持有的灯共用）、
  `WorldSquareAreaLight::visible`（按值存储的门面光）。
  没有引入"World 拥有的可见性集合"这第二套所有权；三个
  `World::Set*Visible(name, visible)` 是唯一写入端，名字不存在时返回 false。
- **渲染过滤点在快照构建处**：`WorldSnapshotBuilder::Build()` 跳过不可见对象，
  它们不进入 `snapshot.meshDraws` / `snapshot.lights`。`meshDraws` 同时是出图、拾取、
  MaterialInstance 上下文与阴影 pass 的唯一来源，所以在构造处过滤等于一次覆盖全部消费者；
  渲染器里没有第二处可见性分支（否则会写出"画不出来但仍能拾取"这类不一致）。
- **没有快照缓存需要失效（对任务书里的一项更正）**：任务书假设 `RenderSystem` 会缓存
  快照、需要额外的失效触发。实测不是这样：`RenderSystem::Render()`（单线程路径）
  与 EngineLoop 的 `PublishSnapshotFromActiveWorld()`（双线程路径）**每帧**都从 active
  World 重新 `Build()` 一份快照，`ConsumeLatestSnapshotIntoRenderScene()` 再逐帧重建
  `currentRenderScene`。因此可见性翻转下一帧就上屏，既不需要 World 换代，
  也不需要用整世界事务或 `device.waitIdle()` 去伪造即时性。
- **命令路径**：`UiActionType::SetActorVisible` → EngineLoop → `RuntimeCommandType::SetActorVisible`
  → `RuntimeCommandExecutor`。身份以稳定字符串跨越 GT→执行边界
  （`"mesh|Sphere_01"`、`"directionalLight|Sun_Light"`…），在执行边界经
  `ParseSceneActorIdentity()` 还原成类型 + 名字；未知分组键、空名字、
  类型没有可见性状态、对象不在当前 World，四种情况都给出诊断并拒绝，
  不静默忽略、也不套用到同名新对象上。
- **眼睛控件**：`icons/eye.png` / `icons/eye-off.png` 由既有
  `tool/editor-ui-assets/generate-content-browser.cjs` 生成（没有手绘 PNG、
  没有图标字体、没有运行时 SVG 解码）；`LUCIDE-LICENSE.txt` 保持一致。
  隐藏行另外得到 `hidden-row` class（标签 `#7c7c85`、行首图标 opacity 0.45）。
  开关是行的 flex 兄弟节点而不是绝对定位，行高仍由 RCSS 固定 30px，
  点击不改变选择、展开、滚动偏移与行窗口。

### 本轮像素探针发现的缺陷（已修）

**眼睛"布局上存在、像素上一个都没有"**：最初把 `.tree-trailing` 写成
`position: absolute; right: 5px`，而 14px 的图标尺寸规则
（`#scene-models .tree-row .list-item img.row-icon`）只作用于 `.list-item` 内的图标，
行尾按钮里的 `<img>` 保持 PNG 原生 32px。结果是按钮盒 18×18 存在、
合成点击测试**通过**，但真实窗口里该区域读回的行底色 `#2d2d31` 一个像素都没变
（`py -3 artifacts/rmlui-editor/bmp_probe.py` 在 x=1255..1269, y=152..171 全是 `#2d2d31`）。
改成 flex 兄弟节点 + 显式 14px 图标尺寸后，行尾出现 56 个中性灰描边像素、
包围盒 `(1246,153)-(1257,162)`。
`EditorUiTest.OutlinerVisibilityToggleQueuesOneCommandAndDisturbsNothingElse`
现在断言眼睛盒子落在 `#scene-models` 的可视矩形内、图标盒恒为 14px、行高不变——
只断言"点击后命令发出了"会让这类缺陷原样通过。

### 验证（2026-10-11，第六轮）

- MinGW 构建：`cmake --build build --target editor_ui_tests editor_viewport_tests main
  material_instance_editor_production_tests -j8` 四个目标全部成功。
- `ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure`
  **283/283 通过**（基线 275/275，本批净增 8 项：`widgetTests.cpp` 3 项、
  `editorUiTests.cpp` 5 项；4 项既有符号链接用例按原条件跳过）。
  **同一选择连跑两次都是 283/283**。
- `ctest --test-dir build -R "MaterialInstanceEditor" --output-on-failure`
  **22/22 通过**，与基线一致：材质槽 → MI 文档流程、非网格选择清描边、
  两条既有写入路径（Intensity / CSM Cast Shadows）都未回归。

### 像素证据：隐藏真的把 actor 从画面里拿掉（1280×720，`--dev-ui`，默认球阵场景）

探针 `artifacts/rmlui-editor/visibility_probe.py`（gitignored）驱动真实窗口：
`SendInput` 点行尾眼睛，经 `console_inject.ps1` 注入 `screenshot`，
再用 `bmp_probe.py` 的读像素函数逐点比较。每张截图载荷 2764800 B（完整 BMP，脚本会拒绝半截文件）。

探针第一步是**稳定性对照**：两条连续截图（中间不发任何命令）在
viewport 区域 `x∈[8,860) y∈[110,500)` 的 332280 个采样点上
`changed=0 strong=0 max_delta=0`。因此后面的差异只可能来自可见性命令。

| 观察点 | 读数 | 结论 |
| --- | --- | --- |
| 首屏行带 | `[(124,135) 分组标题, (153,164) Sun_Light, (184,195) 分组标题, (212,225) Key_Point_Light, …]` | 分组标题行 `eye-bbox=None`，两个 actor 行 `eye-bbox=(1246,153)-(1257,162)`（56 像素） | 眼睛只在 actor 行上 |
| 点 `Sun_Light` 眼睛 | 日志 `Actor Sun_Light hidden.` | UI → UiAction → RuntimeCommand → World 全链路生效 |
| 隐藏太阳后整帧 | 平均亮度 134.361 → 133.923；`changed=16729`、`strong=3390/332280`、`max_delta=349`，强变化像素 bbox `x=[282,596] y=[110,415]`（跨整幅画面） | 方向光是全局光：不是"某一行变了"，是画面本身变了 |
| 最强单像素（太阳） | `(587,348) rgb(235,234,234) → rgb(121,113,120)`（Δ=349）；`(588,378) rgb(222,221,221) → rgb(121,113,114)`；`(587,378) rgb(185,183,185) → rgb(91,84,90)` | 高光被拿掉、球面变暗 |
| 隐藏行状态 | 行尾眼睛 bbox `(1246,152)-(1257,163)`（描边像素 56 → 69），`#7c7c85` 暗标签像素 10 → 73 | 图标与行样式都切换了 |
| 再点一次恢复 | 日志 `Actor Sun_Light shown.`；平均亮度回到 134.361；`changed=0 strong=0 max_delta=0` | **与基线逐字节相同**，可见性可逆、无残留 |
| 滚到 Meshes 后（50 刻度滚轮 + 2s 静置） | 稳定性对照再次 `changed=0 strong=0 max_delta=0` | 滚动后画面仍然静止，网格测量可信 |
| 点某网格行眼睛 | 日志 `Actor SphereArray_X10_Y07 hidden.`；`strong=317`，bbox `x=[577,597] y=[189,209]`（约一个球格） | 隐藏网格只拿掉它自己那一格 |
| 最强单像素（网格） | `(588,207) rgb(81,79,84) → rgb(215,218,225)`（Δ=414） | 球体像素 → 球后背景像素：物体真的从画面里消失 |
| 另一条网格行所在水平条带 | `changed=0 strong=0 max_delta=1` | 隐藏是局部的，不是整幅重画 |

- 像素方法边界：`max_delta` 与 `strong` 是差分统计，不区分"被拿走"和"被遮挡"；
  但恢复后 `changed=0` 说明这不是相机漂移或累积副作用。
- 未验证：spotLight / squareAreaLight / 风剖面的行内眼睛（默认场景没有这三类对象）；
  真实鼠标拖选、隐藏对象的材质实例文档行为只由单元测试覆盖。
- 该进程由探针强制终止，**这是渲染证据，不是 teardown 证据**。
  产物：日志 `artifacts/rmlui-editor/visibility-132450.log`（stderr 0 字节），
  截图在隔离资源目录 `Generated/Screenshots/`
  （`vis-baseline`、`vis-control`、`vis-sun-hidden`、`vis-sun-shown`、
  `vis-mesh-rows`、`vis-mesh-control`、`vis-mesh-hidden`）。

## 第 7 轮：多 section 网格的整 actor 可见性

第 6 轮把"逐 actor 可见性"当成已完成，但写入端与大纲用的不是同一个键：

- 大纲把所有共享 `sceneObjectIdentity` 的 `WorldMeshObject` 聚合成**一行**
  （`CollectMeshGroups`，`source/editor/scene/editorSceneActorBuilder.cpp`）；
- 读取端也确实是整 actor 语义（`actor.visible = std::any_of(sections, visible)`）；
- 而写入端走的是 `World::SetMeshObjectVisible(name, visible)`，即
  `meshObjects.find(name)`。**多 section 模型的实现名是
  `<场景对象名>::<section 名>`**（`rendererMeshLoader.cpp` 在 section 数 > 1 时加后缀），
  身份（场景对象名）根本不是 `meshObjects` 里的键。

于是对多 section 网格点眼睛：`find` 失败 → 执行端打印
`Actor visibility rejected because the actor is not in the active World`
→ 画面毫无变化。默认球阵场景（`SC_sphere_array`）与当时所有 fixture 都是单 section
（单 section 时身份 == 对象名），因此这条路径在单元测试与第 6 轮像素证据里都没被走到——
**这是第 6 轮"已知未完成边界"从风险变成真实缺陷的那一步**。

修正（三个生产改动点，都不新增命令、不改场景 schema/validator）：

- **身份推导抽成一份**：新增 `source/world/meshSceneObjectIdentity.{h,cpp}` 的
  `MeshSceneObjectIdentity(object, objectName)`。读取端（actor 行聚合、
  `MeshDrawSnapshot::sceneObjectIdentity`）与写入端共用它，
  避免"大纲认得的名字 World 不认得"再次出现。
- **写入端按身份写全所有 section**：`World::SetMeshSceneObjectVisible(identity, visible)`
  对每个匹配身份的 section 施加同一操作，返回是否至少命中一个（没命中时执行端照旧诊断拒绝）。
  单 section 网格的身份等于对象名，行为与逐对象写入完全一致。
- **读取端不变**：`any_of` 仍是"任一 section 可见即整行可见"。
  点击写入的是**该行显示状态的反面**，因此从任何起始状态点一次之后所有 section 必然一致，
  行状态与渲染状态严格吻合。不一致 section 的规则由此写死：可见 → 全部隐藏，
  全部不可见 → 全部显示；不存在"点一次只收敛一半"。

### 验证（2026-10-11，第七轮）

```powershell
cmake --build build --target editor_ui_tests editor_viewport_tests main material_instance_editor_production_tests -j8
ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure
ctest --test-dir build -R "MaterialInstanceEditor" --output-on-failure
```

- 四个目标全部构建成功（唯一告警是既有 MinGW `<ciso646>` deprecation）。
- 编辑器 UI 选择 **287/287 通过**（基线 283/283，净增 4 项，全在
  `editorUiTests.cpp`），**同一选择连跑两次都是 287/287**；
  `MaterialInstanceEditor` **22/22 通过**，与基线一致。
- 新增 4 项断言真实 World + 真实 `WorldSnapshotBuilder` + 真实 actor builder：
  多 section（3 个 section 共享一个身份）按身份隐藏/显示时所有 section 一起变、
  快照同时省略/恢复全部 section、actor 行状态与逐 section 事实一致；
  不一致 section 从两个方向收敛；单 section 身份仍等于对象名；
  身份确实不在 World（含把 section 键当身份）仍然被拒绝。
- 断言与对象名形态取自真实数据：`SC_car_showcase` 的车轮是 3 个材质槽、
  车身是 8 个材质槽（`SM_car_wheel_bl.json` / `SM_car_main.json`），
  测试用 `<场景对象名>::<section 名>` 这一实现命名，不另造一套。

### 像素证据：多 section actor 真的整件从画面里消失（1280×720，`--dev-ui`）

探针 `artifacts/rmlui-editor/multisection_visibility_probe.py`（gitignored）。
默认场景全是单 section，因此探针用**既有** `SC_car_showcase` 地图
（通过调试控制台 `loadworld`，资源目录零改动）。

| 观察点 | 读数 | 结论 |
| --- | --- | --- |
| 世界提交 | `World/graph transaction committed: generation=2, scene=...SC_car_showcase.json` | 测的是真实多 section 地图 |
| 稳定性对照（无命令，两张连续截图） | `changed=0 strong=0 max_delta=0`（滚动后再测一次也是 `0`） | 后续差分不可能来自场景漂移 |
| 目标行身份 | 日志 `Actor SM_car_wheel_bl hidden.`，**没有任何 rejection 行** | 修复前这一行就是拒绝诊断，且画面不变 |
| 车辆腿隐藏 | `changed=6544`、`strong=5895/332280`、`max_delta=608`，强变化 bbox `x=[365,737] y=[119,337]`；最强像素 `(711,209) rgb(3,3,2) → rgb(196,203,217)` | 整件（三个 section 一起）离屏；`rgb(3,3,2)` 是车轮轮胎像素 |
| 变化范围 | 变化 bbox 之外 `changed=1 / strong=0`（`max_delta=33`） | 隐藏是局部事件，不是整幅重画 |
| 行尾眼睛 | eye bbox 描边像素 56 → 69 | 行自己的状态也切换了（第二信号） |
| 再点一次 | 日志 `Actor SM_car_wheel_bl shown.`；`changed=5 strong=4 max_delta=261`（该腿漂移对照 `≤10`） | 可见性可逆 |
| 单 section 回归腿（同一探针 `--single-section`，默认球阵场景） | 隐藏 `SphereArray_X02_Y00`：`changed=342 strong=328`，bbox `x=[341,360] y=[396,415]`（一个球格），最强像素 `(354,405) rgb(236,232,231) → rgb(116,116,128)`；恢复 `changed=0`（**逐字节相同**） | 第 6 轮已验证的单 section 行为未回归 |

- 该车辆腿的场景自身每两张连续截图有 **0–5 个像素**抖动（程序化天空 + CSM），
  因此那条腿的稳定性判据是"抖动 ≤ 10 像素"并把实测抖动当对照报出来；
  球阵腿仍是严格的 `changed=0`。隐藏一个车轮改变 6544 个像素，量级不可能混淆。
- **像素证据的边界**："整个 actor 的三个 section 都被写到了"由单元测试断言
  （`EditorUiTest.MultiSectionMeshVisibilityAppliesToEverySectionAtOnce`），
  像素只能证明"车**轮**这件物体从画面里消失了、别的没动"。
  探针选中的行身份（`SM_car_wheel_bl`）与地图里已知的多 section 场景对象匹配，
  这是探针能给出的最强身份约束。
- 未验证：spotLight / squareAreaLight / 风剖面的眼睛（场景没有这三类对象）；
  整车轮的**逐 section 半隐藏**中间态不可达（写入端总是整 actor 写），
  因此没有对应的像素观察点。
- 该进程由探针强制终止，**这是渲染证据，不是 teardown 证据**。产物：
  日志 `artifacts/rmlui-editor/visibility-multi-134407.log`（stderr 0 字节）、
  截图 `Generated/Screenshots/ms-multi-*.bmp`、`ms-single-*.bmp`。

## 待处置项

### `resource-json-close` 死分支：已删除

`resource-json-close` 分支在全仓库没有发出者：`ui/editor-workspace.rml` 只有
`json-close` 与 `resource-close`，`EditorJsonInspectorController` 对前缀
`resource-json` 只发 `expand/search/prev/next`。

它的分支体是 `resourceEditorVisible = false; RefreshResourceEditor();`，
而这两件事已经完整包含在 `resource-close → CloseActiveResource()` 中
（后者还会关闭标签、在没有剩余标签时重新激活视口、清空状态行）。
它既没有独立的触发控件，也没有独立语义，属于重复死代码，因此**直接删除**，
而不是补一个"返回"按钮出来——资源 JSON inspector 当前恒可见，
一个没有第二视图可返回的返回按钮只会制造困惑。

### 截图 readback 可能被半截写入（已观察，未定位）

实测中出现过一个 1267200 字节的 BMP，应为 2764800 字节（1280×720×3 + 54）。
`WriteScreenshotBmp`（`source/render/backend/rendererBackendVulkan.cpp:44-138`）
是同步写完整帧后才返回，因此更可能是**读取发生在写盘过程中**，而不是写盘被中断。
影响限于验证流程：任何读截图做像素断言的脚本必须先校验文件长度，
否则会把未写完的图当成渲染结果。`artifacts/rmlui-editor/bmp_probe.py`
现在会输出实际载荷字节数并对截断像素报错。

## 像素验证方法

本仓库的 agent 模型多无图像输入，`describe_image` 也可能不可用，因此
**界面状态必须用像素值证明，不能用"看起来对"描述**。

```powershell
py -3 artifacts/rmlui-editor/bmp_probe.py <bmp> x,y x,y ...
```

已知对照点（1280×720）：

| 点 | 值 | 说明 |
| --- | --- | --- |
| 顶部工具栏 | `#28282c` | 不透明 CSS 色精确回读 |
| 面板主体 | `#303034` / `#222225` | 同上 |
| 运行时页 `#control-panel` | `#111a2e` | 不透明，非半透明 |
| 状态栏 | `#222225` | 文字像素另计 |

**不透明 CSS 颜色能被精确回读**，这证明 CSS→线性→视口编码的往返正确；
配合 `uiColorTransform.h` 的还原-再预乘语义与 premultiplied 渲染管线，
半透明表面的合成在解析上是正确的。

### "UI 半透明背景不与场景合成"——已定位为 RmlUi 的 `rgba()` 解析缺陷

第 1 轮曾把该现象记为既有限制。**结论是错的，真实原因已实测定位。**
半透明合成一直是正常的；失效的是 `rgba()` 的 **alpha 解析**。

RmlUi 6.2 的 `PropertyParserColour` 在 RGBA 分支对每个分量统一走整数路径
（`extern/RmlUi/RmlUi/Source/Core/PropertyParserColour.cpp` 的 RGBA 循环），
该路径只接受 `0`–`255` 整数或百分比，**小数 alpha 被 `atoi` 截断为 0**。
于是 `rgba(0, 0, 0, 0.55)` 静默变成全透明，没有任何报错。

受影响的声明共 8 处，全部静默失效，现已全部改写为 `#RRGGBBAA`
（`#RRGGBBAA` 走独立的十六进制分支，alpha 正确）：

| 文件 | 原声明 | 现值 | 原本意图 | 可见后果 |
| --- | --- | --- | --- | --- |
| `ui/runtime-control.rcss` | `rgba(12, 18, 34, 0.76)` | `#0c1222c2` | HUD 卡片底色 | 仅在非 developer UI 模式可见 |
| `ui/runtime-control.rcss` | `rgba(127, 166, 255, 0.45)` | `#7fa6ff73` | HUD 卡片描边 | 同上 |
| `ui/runtime-control.rcss` | `rgba(73, 105, 157, 0.45)` | `#49699d73` | 运行时页控件描边 | 运行时页可见 |
| `ui/runtime-control.rcss` | `rgba(92, 126, 184, 0.7)` | `#5c7eb8b3` | 运行时页控件描边 | 运行时页可见 |
| `ui/editor-workspace.rcss` | `rgba(0, 0, 0, 0.55)` | `#0000008c` | About 对话框背板 | 对话框完全没有压暗 |
| `ui/editor-workspace.rcss` | `rgba(0, 0, 0, 0.55)` | `#0000008c` | 脏关闭对话框背板 | 同上 |
| `ui/editor-workspace.rcss` | `rgba(71, 153, 126, 0.30)` | `#47997e4d` | 停靠落点预览 | 拖动时看不到落点高亮 |

`ui/runtime-control.rcss` 另有两处 `rgba(0, 0, 0, 0)`（本意全透明）已改写为
`transparent` 关键字，避免以后被人"顺手修成"小数 alpha 而静默失效。

#### 验证证据（全部为像素实测）

受控扫描：把 `#editor-status` 底色依次替换后各启动一次并截图，
读取 `(900, 706)`：

| 声明 | 回读值 | 判定 |
| --- | --- | --- |
| `#222225` | `rgb(34,34,37)` | 不透明精确回读 |
| `#ff00ff` | `rgb(255,0,255)` | 不透明精确回读 |
| `#ff00ff80` | `rgb(189,15,189)` | 正确半透明；与线性空间 50% 混合解析值一致 |
| `rgba(255,0,255,0.5)` | body 底色 | alpha 被截断为 0 |
| `rgba(255,0,255,0.25)` | body 底色 | 同上 |
| `rgba(255,0,255,0.5)`（无空格） | body 底色 | 同上，与空格无关 |

背板功能性验证（`Help → About` 真实鼠标路径）：打开对话框前后同一坐标，
背板外像素比值约 **0.62–0.70**；"完全不合成"应读回约 1.00。
不同初值下比值一致，符合线性空间 55% 黑的预期（sRGB 非线性使比值高于 0.45）。

**验证方法上的两个教训**（已固化进
`artifacts/rmlui-editor/bmp_probe.py` 与各探针脚本）：

1. 截图可能是**半截文件**。实测出现过 1267200 字节而应为 2764800 字节的 BMP；
   读像素前必须校验文件长度，且要通过"大小不再增长"来判断写盘完成。
2. 判断样式是否生效应优先**像素回读**，不要凭观感下结论。
   本轮一度在"`.hud-card` 设成不透明红仍不可见"上绕了很久，
   最后发现是 `UiSubsystem` 在 developer UI 可见时把 `#hud` 显式设为
   `display: none`——元素本身按设计不显示，与颜色格式无关。

## 尚未实现能力清单（2026-10-11 核对，第七轮后）

以下在 HEAD 与当前分支中都不存在，属于新增而非回归：

- gizmo、变换编辑、场景对象增删改 / 重命名 / 重父级
- Undo / Redo（Edit 菜单为禁用占位）
- 逐 actor 可见性的持久化：第 6 轮加了运行时 World 状态、快照过滤与大纲眼睛，
  但开关不写场景 JSON、`SceneAssetValidator` 不认识、也没有保存命令；
  重新加载场景一律全部可见。要走持久化得连 transform 编辑一起做场景文档阶段。
- 地形节点（`World` 没有 terrain 访问器，仓库里也没有 terrain 运行时）
- 大纲多选、重命名、父级/文件夹层级、actor 增删
- 资产缩略图、模型几何预览、纹理像素查看
- 视口参考网格 / 地面 / 坐标轴显示（`shader/glsl` 里的 `measureGrid` 是 BRDF 测量探针的，
  不是编辑器参考系）
- 原生多窗口、IME 输入
- 视口渲染成本仍按完整 swapchain 尺寸（**注意这不是"投影错误"**：相机 aspect 已按视口
  rect 设置，见 `EngineLoop::SynchronizeSceneViewport`，画面比例是对的；`sceneViewportOutput`
  的 backing extent 也与 swapchain 一致，`renderGraph.cpp` 的注释把它写成刻意设计
  ——「UI 使用完整 UV 而非拷贝/裁剪屏幕」。所以缺的只是"按视口尺寸渲染以省算力"这一项优化，
  不是画面不正确。2026-10-12 核对时纠正了本条原先"按 dock 分辨率渲染"的措辞，那读起来像缺陷）
- 材质实例纹理 override 的 live 生效（保存 JSON 后才生效）
- HSV 取色器
- 编辑器偏好 / 项目设置面板；可配置快捷键
- 右键上下文菜单（全仓库无实现，`platformApplication.cpp` 里的命中只是鼠标按键分发）

**已从本清单消掉的**：大纲虚拟化在第 5 轮完成（受限挂载窗口 + spacer），
本条此前误留在"未实现"里，2026-10-11 核对时删除。

第 4 轮已经消掉的一条：Scene Outliner 不再只列出带有效 MI 的网格，
而是真实 World actor 树（网格 / 四类灯 / 相机 / 环境 / 风剖面，分组、图标、搜索、类型过滤）。
第 6 轮消掉的一条：逐 actor 可见性——它是**运行时**能力（World 状态 + 快照过滤 + 眼睛开关），
不是场景 schema 迁移，因此没有触碰场景 JSON、validator 或 World 保存路径。
第 7 轮补齐了它的多 section 边界：第 6 轮"已知未完成边界"（写入端按 section 键找对象）
已确认为真实缺陷并修掉，现在**没有遗留的多 section 可见性边界**；
仍未验证的只有场景里不存在的那几类对象的眼睛（spotLight / squareAreaLight / 风剖面，
与第 4、5、6 轮同一限制）。
