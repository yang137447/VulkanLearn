# UI 组件体系

状态：2026-10-11，`codex/rmlui-editor` 分支第四批实现（Scene Outliner 变成真实 World actor 树，
选中驱动 Details，并接入两条既有运行时命令）。

## 工作区布局

默认布局向 UE 编辑器靠拢：顶部标题行最左侧是 File / Edit / Window / Help 菜单栏，
标题右侧保留原有命令按钮，右上 Scene Outliner，右下 Details，
中央/左侧独立 Viewport，底部 Content Browser / Renderer 标签页，最底部状态栏。
Material Slots 放在 Details 中。面板可四边停靠、合并标签、在主窗口内浮动后重新停靠，
分隔条支持拖动并受子树最小尺寸约束；标签切换不提交资产或运行时命令。
Scene Outliner 现在渲染真实 World actor 树（网格 / 四类灯 / 相机 / 环境 / 风剖面，
分组可折叠，带类型图标、搜索与类型过滤），选中 actor 由 Details 展示其类型化参数；
无打开的 MI 文档时 Details 不再为空。契约见“世界 actor 树契约”。
面板可以通过 Window 菜单隐藏和恢复，隐藏状态随布局一起保存。
布局保存至用户目录，支持跨启动恢复和 Reset Layout；不支持独立操作系统窗口。
资源编辑器是 DockWorkspace 的正式 `Asset Editor` 面板，默认与 Viewport
合并为中央标签组，也可独立拖到四边、浮动和重新停靠；面板内再管理资源文档标签。
当前 Content Browser 浏览 MI、Mesh、Scene、Texture、Terrain JSON 元数据，
Mesh、Texture、Terrain 双击后进入独立资源标签页：左侧为类型预览占位，
右侧为只读 JSON Details；当前只读元数据，不加载 `data` 引用或 GPU 资源。
它不再使用平铺文件名列表：左侧目录树由 worker 发布的只读 JSON 目录派生，
上方提供前进/后退、父目录、面包屑与搜索，资产区支持网格/列表、
名称排序、类型筛选、尺寸档位、子目录开关与显式 Refresh，底部显示数量和完整选中路径。
单击只改变浏览选择；双击 MI 提交原有打开命令，保持可编辑，
双击 Scene 复用 `UiActionType::LoadWorld` 请求切换 World，并同步打开只读 JSON inspector；
Scene 仍请求 World 切换并使用主工作区 JSON inspector；Mesh、Texture、Terrain
使用资源编辑器标签页。资源标签支持重复打开去重、切换、关闭、返回 World 和重新读取 JSON。
目录与搜索不提交业务命令；类型图标和材质球不代表实际缩略图或模型预览。
Viewport 使用 RenderGraph 拥有的离屏场景纹理，完整 UV 显示，不拷贝或裁剪 swapchain。
GPU backing 保持原生窗口尺寸；相机 aspect、拾取和离线 film 尺寸跟随面板逻辑尺寸。
视口左上角叠加工具条；其中只有 view mode 有渲染后端，变换工具与坐标系只是呈现状态，
详细边界见“停靠与视口契约”。

## 设计边界

组件分类参考 UE Slate，布局、输入、字体、焦点、DOM 和绘制仍交给 RmlUi。
不移植 Slate，不重建 SWidget 继承树，也不引入第二套事件/渲染系统。
叶控件生成经过转义的 RML；复合控件组合叶控件；业务面板消费冻结快照并提交业务命令。
组件模块不包含 World、MI schema、EditorCommand、Vulkan 或文件持久化依赖。

官方分类参考：

- `https://dev.epicgames.com/documentation/en-us/unreal-engine/slate-ui-widget-examples-for-unreal-engine`
- `https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Slate`
- `https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/SlateCore/SCompoundWidget`

## 分类与状态

| 分类 | 通用组件 | 本批状态 | 下一阶段 |
| --- | --- | --- | --- |
| Text / 展示 | Text、Heading、Description | 已实现转义和文本组合 | Image、Icon、Tooltip |
| Input / 输入 | Button、Input、Slider、Option、Select | 已实现交互地址和样式状态；`Slider` 是 `input type="range"`，与文本输入共用同一个 action/index/component 地址 | CheckBox、SpinBox、ColorPicker |
| Layout / 容器 | HorizontalBox、VerticalBox、ScrollBox、Splitter | RML/RCSS 组合与受最小尺寸约束的可调分区 | Grid |
| Views / 数据视图 | ListRow、TreeRow | 已实现行、缩进、展开符号、选择状态、行首类型图标与行样式 class | 虚拟化 ListView/TreeView、TableView、筛选 |
| Navigation / 导航 | DocumentTab、MenuItem | 已实现激活与关闭分离、菜单项勾选/禁用与条目索引寻址 | 菜单快捷键、Breadcrumb、子菜单键盘导航 |
| Workspace / 工作区 | DockWorkspace、DockTab、面板可见性、脏关闭弹窗 | 四边停靠、标签合并、窗口内浮动、布局持久化与重置、面板隐藏/恢复 | 原生多窗口、通用 Modal |
| Property / 属性 | PropertyRow、PropertyGroup、PropertyInfo、PropertyControlRow、CollapsibleSection、NumericComponents、ColorPreview | 已实现属性组合、Reset 地址、向量分量、可选范围滑条、折叠分组、sRGB 色板与十六进制读数、无 Reset 的只读/可编辑行、可寻址数值 span | 多对象混合值、属性适配器、可编辑调色板 |
| Editor / 业务 | 顶部菜单栏、世界 actor 树、JSON Content Browser、MI Details、actor Details、资源编辑器、离线控制、Viewport、视口工具条 | 已接入组件、受限浏览窗口/分页、资源标签、renderer-owned 离屏视口、World actor 树与 actor 详情（含两条既有运行时命令）、视口工具条中只有 view mode 有真实后端 | 真实模型/纹理预览、场景对象增删改、gizmo |

“已实现”只表示上表的窄组件，不表示已经具备 Slate 同名控件的全部行为。
通用 ListRow/TreeRow、模型树和 Sources 树尚未虚拟化；
Content Browser 业务网格/列表已有受限可见窗口，JSON inspector 使用分页，
不等同于通用 ListView/TreeView 已完成。
数值组件是文本输入加可选范围滑条：滑条只做范围语义下的快速调整，
精确输入仍由同一行的文本输入负责，两者共用同一个 action/index/component 地址。
分组折叠只改既有分组元素上的 class，不重建 DOM；
滑条的 min/max 属于分组 markup，随 identity（assetPath + schemaDigest）重建。
Color 仍按线性 RGBA 存储，编辑器只多出面向 sRGB 的色板与 `#RRGGBBAA` 读数，
没有可编辑调色板；真实资产缩略图、模型几何预览、纹理像素查看、
资产拖放、Undo/Redo 尚未实现。
`MenuItem` 只生成“动作名 + 条目索引 + 勾选/禁用”这一种行，
菜单的展开状态、勾选含义和命令语义全部属于业务宿主。

### 本次恢复的迁移能力（2026-10-11 第三批）

ImGui→RmlUi 迁移丢了三项 Details 面板能力，本批把它们补回，仍未补回的部分也一并写明：

- **数值范围滑条**：`input type="range"` 通过既有 `HandleChange` 提交与键入数字相同的
  `SetMaterialParameterOverride` batch item，没有第二条命令路径。
  取值改为从 `value` 属性读（RmlUi 的 `WidgetSlider` 用 `"%f"` 序列化，小于 1e-6 的
  拖动结果会被截成 0），因此拖动永远不会落进“有限数”校验分支。
  范围来源与 HEAD 的 ImGui 版本一致：通道范围 > 参数范围 > 按默认值推导的兜底；
  旧 M_ 文件没有声明范围时用 `extent = max(1, |defaultValue| * 2)` 生成 `[-extent, extent]`，
  基准是默认值而不是正在编辑的值，避免错误输入把可调区间越推越宽。
  兜底与滑条都只影响显示与拖动，不改变存储值，也不做逐帧 clamp。
- **sRGB 色板与十六进制读数**：只对 `EditorParameterType::Color` 参数显示。
  线性存储保持不变，`UiLinearToSrgb` 用扩展公式且不截断 HDR 值；
  alpha 不做 gamma 变换。色板底色是不透明 `rgb()`，
  因为当前 UI overlay 不合成半透明背景，alpha 只在 `#RRGGBBAA` 读数里表达。
- **折叠分组**：General / Render States / Parameters / Texture Bindings，默认全部展开。
  展开状态是面板会话状态，不进入资产文档、停靠布局或运行时快照；
  切换只改分组元素的 `collapsed` class，静止帧写 0 次属性。
  分组标题是 `<button>`（可聚焦/可 Tab 到达），
  **Enter/Space 激活没有后端**：RmlUi 6.2 只在鼠标抬起时派发 click，
  键盘激活与菜单快捷键一样列在上表的下一阶段。
- **仍未补回**：`ImGui::ColorPicker4` 等价的可编辑调色板（含 HSV 面板）。
  RmlUi 没有 `<canvas>`，SDL3 在 `SDL_dialog.h` 里也没有颜色对话框 API，
  真做就意味着手写色相/明度控件；本轮明确不做，也不用别的控件假装。
  SpinBox（步进）与多对象混合值同样仍缺席。

当前确认缺席的能力集中列在此处，供下一轮直接引用，不再逐条重新推导：

- 没有 gizmo、没有 transform 编辑，也没有场景对象的增/删/复制/改名。
- 完全没有 Undo/Redo 系统；`Edit` 菜单里的 Undo/Redo 是常驻禁用的占位。
- 没有逐 actor 的可见性/隐藏开关：`WorldMeshObject`、`MeshDrawSnapshot` 与渲染图
  都没有逐对象可见性状态，加它要同时改场景 JSON schema、`SceneAssetValidator`、
  World 构建与渲染路径，属于单独一轮；因此大纲里不放一个点了没有效果的隐藏按钮。
- 没有地形节点：`World` 没有 terrain 访问器，仓库里也没有 terrain 运行时
  （场景选择器只把 `"terrain"` 列入受支持类型，资产浏览器只有 Terrain JSON 元数据）。
- 大纲不做虚拟化：每帧重建 markup 字符串再由值缓存决定是否写 DOM，
  与迁移前的模型树同一策略。
- 没有资产缩略图、没有模型几何预览、没有纹理像素查看器。
- 没有可编辑调色板（含 HSV 面板），也没有步进控件：Details 目前只有
  范围滑条、文本输入和 sRGB 色板读数。
- 没有原生多窗口，没有 IME。
- 场景仍按 swapchain 全尺寸渲染，UI 以完整 UV 采样，不是按停靠尺寸渲染。
- 材质实例的纹理覆盖只在保存 JSON 后生效，不做 live descriptor 替换。
- 视口工具条的变换工具与坐标系没有渲染后端；view mode 里的 Wireframe 没有
  wireframe 光栅模式，也没有对应的 debug view，因此按钮保持禁用。

## 目录与依赖

`source/ui/widgets/` 是独立 `vl_ui_widgets` 静态库，只链接 RmlUi::Core：

```text
widgets/
    text/
    input/
    layout/
    views/
    navigation/
    workspace/
    property/
    widgetDocument.*
```

未实现的类别暂不创建空目录、占位类或未完成接口。
共享样式位于 `ui/widgets/editor-widgets.rcss`；
`ui/editor-workspace.rcss` 仅保留工作区位置、尺寸与特定区域覆盖。
业务面板位于 `source/editor/ui/`，不得反向成为 widgets 库的依赖。
main 排除递归收集的 widget 源文件，链接独立库，避免重复编译/定义。

## 交互与数据

`WidgetAction` 只包含 action 名称、条目索引和分量索引。
组件不解释它的业务含义；宿主统一解析事件并提交现有命令。
条目索引只在当前快照中有效，不能作为跨 World 的对象身份。
大纲宿主按稳定 actor 身份记录展开/折叠，World generation 变化时清空展开、折叠、
搜索与过滤；子材质节点与现有 Material Slots 走同一个选择 sink。
树组件不会打开文件或持有 scene object 指针。

用户/资产提供的标签、id、action、option 值均通过 Text() 转义。
`childrenRml`、`optionsRml` 是组件生成的可信结构，不是用于注入业务文本的接口。
业务文本只能进入具备转义责任的函数。

`WidgetDocument` 在 GT 借用已校验的 RML document：

- 相同 markup 不重建 DOM，避免无意义焦点/滚动抖动。
- `SetProperty` 按元素 id、属性名和值缓存；相同值不再次调用 RmlUi，
  避免静止帧重复 DirtyProperty，停靠几何与面板 display 也走此路径。
- `SetClass` 与属性共用同一份缓存：折叠分组、选中态、disabled 样式都在静止帧写 0 次。
- 范围控件（`input type="range"`）的 `GetValue()` 是 `"%f"` 文本，
  与快照推送的 9 位有效数字比较永远不相等；`SetControlValue` 对范围控件退化成数值比较，
  静止帧不回写 `value` 属性，也不把它从控件自己的浮点改成快照字符串。
- 属性面板的取值统一走宿主侧的 `ControlValue`：文本输入读 `GetValue()`，
  范围控件读 `value` 属性（精确浮点），两者都保证是可解析的有限数。
- Attach/Detach 清空 markup/属性缓存；替换子 DOM 只失效将销毁的后代缓存，
  局部文本刷新不清空宿主或其他面板缓存。宿主可显式 ForgetMarkup。
- 折叠分组的展开状态属于宿主（会话状态），切换只改既有分组元素的 class；
  分组 DOM 只在 identity（assetPath + schemaDigest）变化时重建。
- 替换 select 选项先 RemoveAll，防止 RmlUi 内部 widget 累积旧 option。
- 更新控件值保留焦点输入的未完成文本。
- disabled 属性与普通 button 的 disabled 样式 class 同步。
- WidgetRefreshScope 支持嵌套抑制，刷新触发的 change 不成为用户修改。

文档的生命周期、业务事件监听、revision、batch、保存、关闭决策和预览仍由业务宿主负责。
RT 只消费 UiRenderSnapshot，不访问组件、document、selection sink 或 asset session。

## 世界 actor 树契约

Scene Outliner（`#editor-navigation`，仍是 `DockWorkspace::Panels()` 里的 `outliner`）
渲染 active World 的真实 actor 列表。

- **数据源在 EngineLoop 的稳定帧边界**：`EngineLoop::UpdateUiViewModel` 用
  `GetSubsystems().GetWorldManager().GetActiveWorld()`（与设置
  `snapshot.activeWorldPath` 同一处）调
  `BuildEditorSceneActors(world)`，再经 `UiSubsystem::SetSceneActors(...)`
  发布，位置与 `SetMaterialInstanceSceneModels` 并排，且都在
  `uiSubsystem->Update(snapshot)` 之前。UI 回调不访问 World。
- **冻结快照是纯值**：`EditorSceneActorSnapshot` 只含字符串、`RuntimeId`、
  `LightSnapshot`（与渲染快照同结构）、相机七个展示字段、环境与环境/风剖面标量；
  没有 World、SceneNode、MaterialInstance 或 Vulkan 句柄。列表以
  `shared_ptr<const std::vector<...>>` 进入面板快照（与资产目录同一约定），
  逐帧不深拷贝 actor 数组。
- **覆盖范围**：网格（按 `sceneObjectIdentity` 聚合 section，逐 section 带
  mesh/material/MI 路径/槽名/风剖面 key）、directionalLight / pointLight /
  spotLight / squareAreaLight、相机、环境、SpeedTree 风剖面。
  灯光参数复用 `WorldSnapshotBuilder` 的同一份 World→`LightSnapshot` 映射
  （`BuildWorld*LightSnapshot`），对象 `RuntimeId` 复用 `WorldObjectRuntimeId`，
  避免第二份身份规则与选择描边漂移。
- **分组与顺序**：`SceneActorGroupSpecs()` 是唯一分组清单（类型、过滤键、标题、图标），
  顺序固定；组内按显示名做 ASCII 大小写不敏感比较，再按稳定身份打破平局，
  不依赖 `unordered_map` 遍历顺序。分组可折叠，折叠只影响行是否挂进 DOM。
- **搜索与过滤**：搜索复用 Content Browser 的 `FoldAsciiCase`
  （`editor/editorTextMatch.h`），只折叠 ASCII 大小写、保留 UTF-8 多字节名称；
  类型过滤取值就是分组键。两者都只改变可见性，不提交任何命令。
  计数显示的是筛选结果（`N / M actors`），与 Content Browser 的计数同一语义。
- **选择路由**：mesh actor 继续走 `RequestModelSelection`，材质槽继续走
  `RequestMaterialSelection`（MI 文档、Material Slots 与视口描边行为不变）；
  非网格 actor 走新的 `RequestActorSelection(稳定身份)`，同时要求 EngineLoop 清掉
  渲染端 mesh 描边，保证大纲高亮与视口描边不会同时指向两个对象。
- **Details**：选中 actor 在 `#details-content` 里得到一个以类型命名的可折叠分组
  （键 `actor-<分组键>`），只读字段由 `SceneActorFields()` 按类型生成，
  每行带 id，值变化时只刷新这一行；主方向光额外得到两个可编辑控件
  （Intensity 与 Cast Shadows），两者都只用既有 `UiActionType::SetSunIntensity` /
  `SetCsmCastShadows`，没有第三条 UI→renderer 路径，也不新增命令。
  MI 文档分组不变，actor 分组在它之前。
- **无可见性开关**：见“当前确认缺席的能力”；大纲不放没有后端的控件。

## Content Browser

`EditorContentBrowserController` 从 `MaterialInstanceEditorSnapshot.assetCatalog` 的
共享不可变目录派生 Sources 树、当前目录、历史和筛选集合；无目录时兼容旧 MI
`assets` 快照。正式目录是唯一资产集合，MI 快照仅叠加已有 MI 的 dirty 标记，
不能将目录外或已删除的路径混回。控制器不扫描文件、不读取 JSON、
不写资产，也不持有 renderer。路径边界按目录分隔符判断，不能将 `Maps/A`
误匹配为 `Maps/AB`；排序后的事件地址仍使用完整路径，不能只用文件名或旧条目索引。
搜索折叠 ASCII 大小写，并保留 UTF-8 多字节名称和完整目录树。

目录对象身份或 revision 变化才重建路径索引；目录、搜索、类型、递归或排序变化
才重建筛选集合。网格/列表按滚动位置和面板尺寸挂载最多 192 个条目节点，
前后各预取两行，仍受 192 上限约束；最多两个 spacer 保留未挂载行的高度。
此上限指条目根节点，不包括条目内图标/文本，也不约束 Sources 树。
网格占位按整行计算，完整筛选计数和尾部资产仍可滚动到达，不是截断前 192 项。
静止帧不重建目录、筛选集合或条目 markup，隐藏浏览器延后这些工作。

浏览状态跨文档重新 Attach 保留，不写入 docking 布局文件。资产目录移除时，
当前位置回退至最近存在的祖先，失效历史和选择清除。
单选 class 与固定 dirty marker 原地更新，不能替换 tile DOM，否则 RmlUi 跨帧双击目标会失效；
目录/视图结构更新延后至 Build，事件分发期间不销毁目标。
打开地址还须属于当前可见节点；双击核对首击的完整路径、DOM 代次和滚动位置，
防止虚拟窗口滚动后将复用的节点地址当成另一资产。
初始网格使用小尺寸，默认底部工作区加高；用户已保存的停靠比例仍由原文件恢复。
包含 Content Browser 的标签组最小高度为 246px，布局按此约束显示已有比例，
不覆盖保存文件；960×540 默认窗口能显示工具栏和完整第一行小卡片。
更大的卡片可滚动或拖动分隔条加高面板，极小窗口仍沿用布局树的比例缩减规则。

图标为 Lucide 的已提交 PNG，许可位于 `ui/icons/LUCIDE-LICENSE.txt`，
没有运行时下载、SVG 解码或额外图标字体依赖。统一材质球及其他类型图标仅是类型图示，
真实缩略图、模型预览、纹理像素查看和导入仍属后续能力。
资产生成入口为 `tool/editor-ui-assets/generate-content-browser.cjs`，
需要已安装的 `lucide` 与 `sharp` 模块目录作为参数，不参与引擎构建。

## JSON 元数据与输入边界

`source/editor/assets/editorJsonAssetLibrary.*` 提供 GT facade 与独立读取 worker。
初始化请求一次目录扫描，后续仅显式 Refresh 重扫；打开请求只读取选定 JSON。
GT 每帧 `Poll()` 使用非阻塞邮箱发布结果，不扫描目录、不读取资产 JSON。
刷新/打开待办合并，打开优先且扫描在文件之间服务新打开请求；
worker 投递和 GT 发布均核对代际，迟到结果不能覆盖最新选择。
目录和文档使用 `shared_ptr<const ...>` 进入编辑器快照，逐帧不深拷贝目录或字段数组。

目录只识别 MI、Mesh（含顶层 `speedtree`）、Scene、Texture、Terrain；
支持的顶层 `type` 优先于文件名前缀，缺失或非字符串时按 `MI_`/`SM_`/`SC_`/`T_`/`TR_`
和旧 `scene*` basename 识别，非字符串 type 另附诊断。
显式不支持的字符串类型排除；可识别的损坏候选保留诊断，
不展示半棵字段树。扫描跳过 `.git`、`Generated`、`data` 目录（ASCII 大小写不敏感），
不跟随目录符号链接；打开前校验资源根相对路径与 canonical 根内边界。

SAX 不构造完整 JSON DOM，只保留顶层分类/名称/说明及有界前序字段行。
任意深度、ASCII 大小写不敏感的 `data` 字段及其整个子树不进入展示行或快照；
引用路径仅作为普通元数据文本，不加载引用文件、World、模型、纹理像素或 GPU 资源。
JSON 文件字节仍在 worker 读取，内嵌 `data` 仍做语法、字节数与深度校验，
不能宣称跳过其词法解析或完全不读取内嵌 payload。

输入上限来自 `EditorJsonAssetLimits`（`source/editor/assets/editorJsonAssetLibrary.h`）：

| 边界 | 当前上限/行为 |
| --- | --- |
| 单文件 | 4 MiB；最多 100000 源码行 |
| JSON 容器深度 / 路径与扫描深度约束 | 64 |
| 文档展示字段 | 4096 行；超出后省略并诊断，剩余输入继续语法校验 |
| 标签、值及说明等展示文本 | 1024 字节截断预算，保持 UTF-8 边界，截断时追加 `...` |
| 单字段完整 JSON Pointer | 4096 字节，超出拒绝 |
| 目录 / 扫描访问 | 最多 20000 个资产 / 100000 个文件系统条目 |

`EditorJsonInspectorController` 借用共享文档，显示路径、类型、说明、诊断，
按 object/array/string/number/boolean/null 展示字段，支持展开和路径/值搜索。
`FieldsPerPage = 64`（`source/editor/ui/editorJsonInspectorController.cpp`）限制每页字段行；
快照、请求路径、loading、展开、搜索或页码变化才刷新，静止帧不重建字段 markup。
它无资产写入或运行时激活入口，MI 编辑仍由现有文档命令与 revision/batch 契约负责。
浏览窗口常量 `MaxVisibleItems = 192`、`OverscanRows = 2` 位于
`source/editor/ui/editorContentBrowserController.cpp`。

绘制侧仍每帧冻结 UI draw snapshot，但 CPU 数组按上一帧数量预留容量；
`UiOverlayRendererVulkan` 的 per-image 动态缓冲不足时才扩容，
顶点以 256 KiB、索引以 128 KiB 为基数倍增至满足需求，容量足够则复用。
这些优化不等同于整个 UI 零分配或资产规模无限。

## 停靠与视口契约

`DockWorkspace` 是无文件系统/JSON/renderer 依赖的纯布局树；
`EditorDockingController` 在 GT 管理 RmlUi 捕获、停靠壳、拖动反馈和持久化。
业务 DOM 只重新挂接，不因停靠而重建；焦点输入保留未完成文本。
事件分发期间不销毁停靠壳，结构变化延后至下一次 GT 刷新。
脏关闭弹窗阻止工作区拖动；失焦清理拖动捕获。

面板可见性由 `EditorDockingController` 拥有，不是布局模型的状态：

- 隐藏面板保留在布局树中的位置，只从标签栏和正文里抑制；
  重新显示时回到原来的标签组，`DockWorkspace::Panels()` 与布局树结构不变。
- 隐藏当前激活标签时，激活权交给同一标签组里仍可见的标签；
  整组都隐藏时该标签组折叠出几何（`Layout` 的 `collapsedStacks` 输入只表达
  “该子树不占矩形”这一纯几何事实），兄弟子树接管整块矩形，不留空灰框。
- 隐藏最后一个可见面板会被拒绝，并通过 `notice` 给出人类可读的原因。
  显示面板同时激活它，保证 Window 菜单的勾选与可见标签一致。
- 激活、拖放命中测试与几何计算都跳过隐藏标签组；
  隐藏面板不接受激活，避免同组可见标签失去正文。

持久化默认路径为 `%LOCALAPPDATA%/VulkanLearn/Editor/workspace-v1.json`，
无 LOCALAPPDATA 时回退 APPDATA。JSON 校验版本、完整已知面板集合、
节点数量/深度和有限尺寸，文件最大 64KB；损坏布局不替换当前布局。
可选顶层 `"hidden"` 数组记录被隐藏的面板名：缺省表示没有隐藏面板，
旧文件继续可读；成员必须是已知面板名、不得重复、最多 32 项，
并且不接受“所有面板都被隐藏”的文件。宿主通过临时文件和备份重命名提交，
失败保留旧文件。Window→Reset Layout 清空隐藏集合；Window→Restore Saved Layout
重新读取该文件，DOM 重排延后到下一次 GT refresh。

顶部菜单栏位于 `#editor-toolbar` 内的 `#editor-menubar`，在标题与命令按钮左侧。
展开状态是面板的本地状态，不进入资产文档、停靠布局或运行时快照；
条目复用工具栏的 `data-action`，不复制业务逻辑，File/Edit 的文档命令与工具栏按钮
共用同一份可用性。勾选状态直接查询停靠视图的可见性，不在菜单里维护第二份状态。
File→Open Level 从冻结的 `snapshot.assetCatalog` 筛出 `EditorAssetKind::Scene`，
按条目索引寻址，点击后走与 Content Browser 双击关卡相同的
`UiActionType::LoadWorld` 请求；菜单自己不认识 World。
Help→About 显示编辑器名称、RmlUi 说明和布局文件路径，打开期间作为模态阻止工作区命令。

GT 文档只使用 `vl://scene-viewport` 逻辑纹理 token 和 framebuffer 像素矩形。
图像、兼容 render pass、framebuffer 和 GPU epoch 退休属于 renderer；
不得将裸 Vulkan handle 放入组件或 GT 文档。视口命中使用半开边界，
拾取先减去面板原点；输入模态、其他面板与拖动状态不穿透至场景。
完整渲染与 resize 契约见 `game-ui-stack.md`。

视口工具条 `#viewport-toolbar` 叠在场景图像左上角，分三组：
变换工具、变换坐标系、view mode。只有 view mode 有真实后端，
映射到既有 `UiActionType::SetDebugViewMode`：Lit=0（完整画面），Unlit=1（基础色）。
变换工具与坐标系**只是编辑器本地呈现状态**：仓库里没有 gizmo，
也没有 transform 编辑后端，切换它们只改变高亮，不提交任何 UiAction 或资产命令，
按钮提示文字明确写出这一点。Wireframe 没有 wireframe 光栅模式，
`debug.*` 表里也没有对应的 debug view，因此按钮常驻禁用并带说明，
没有拿最接近的调试视图冒充。view mode 的高亮来自冻结的运行时快照
`debugViewMode`，不是面板自己的第二份状态；运行时处于工具条之外的调试视图时
Lit/Unlit 都不高亮，工具条不声明自己没有设置过的模式。
`IsSceneViewportElement` 排除工具条，否则点击按钮会同时驱动相机与拾取。

视口工具条分组底色使用不透明 `#242428`，这是可读性选择而不是管线限制：
半透明底色可用，但必须写成 `#RRGGBBAA`（原因见下方 RCSS 颜色格式约定）。

### RCSS 颜色格式约定：alpha 必须写成 `#RRGGBBAA`

RmlUi 6.2 的 `PropertyParserColour` 把 `rgba()` 的**每一个**分量都按
`atoi` 解析（`extern/RmlUi` 的 `Source/Core/PropertyParserColour.cpp`，
RGBA 分支的整数分量路径）。该分支只接受 `0`–`255` 整数或百分比，
**小数 alpha 会被 `atoi` 截断为 0**。因此：

- `rgba(0, 0, 0, 0.55)` → alpha 0 → 元素完全透明，且不报任何错误；
- `rgba(255, 0, 255, 0.5)` → alpha 0，同样静默失效；
- `#RRGGBBAA` 走独立的十六进制分支，alpha 正确；
- `transparent` 关键字正确。

**约定：本仓库的 RCSS 一律使用 `#RRGGBBAA` 表达半透明，不使用 `rgba()`。**
需要完全透明时写 `transparent`。这条不是风格偏好：`rgba()` 的失败是静默的，
只会表现为"样式没生效"，历史上已经导致一处误判为"UI overlay 不合成半透明背景"。

不透明 CSS 色可以被像素精确回读（`#28282c` 读回 `rgb(40,40,44)`），
因此颜色管线本身（sRGB→线性→视口编码）是正确且可验证的；
判断样式是否生效应优先用像素回读，而不是肉眼观感。

### 已知限制

- 半透明表面的合成已实测生效：`#about-dialog` 背板（`#0000008c`）把背板外的
  编辑器像素压到原值的约 0.62–0.70（55% 黑在 sRGB 非线性下的预期区间），
  而"完全不合成"应读回约 1.00。面板色块 `#ff00ff80` 读回 `rgb(189,15,189)`，
  与线性空间 50% 混合的解析值一致。
- 编辑器模式下 `#hud` 被 `UiSubsystem` 显式设为 `display: none`
  （`source/ui/uiSubsystem.cpp`，`developerUiVisible` 为真时），
  因此 HUD 卡片的底色在编辑器工作区中不可见，这是有意行为而非样式失效。
  验证 HUD 样式需要在 `ui.enabled` 且非 developer UI 的模式下进行。

## 测试与扩展

模块测试放在 `tool/editor-ui-tests/widgetTests.cpp`，使用真实 RmlUi，
经根 `vulkanlearn_add_gtest` 注册到 editor_ui_tests：
转义、选项替换、焦点保留、缓存、禁用状态、布局、树行和标签事件地址。
本批新增纯组件与颜色用例：范围分量生成滑条并与文本输入共用同一地址、
退化区间不写 `step`、范围控件在重复快照推送下不回写 `value`、
`MakeColorReadout` 的线性→sRGB→十六进制（0/1/中灰/alpha）、
折叠分组只靠 class 切换并保留子节点身份与焦点，
以及 `UiLinearToSrgb` / `UiSrgbHexByte` 在 `UiColorTransform` 套件里的曲线与 HDR 边界。
第四批新增：`TreeRow` 的行首图标与行样式 class 只进入选择按钮内部（点击靶面不变）、
图标路径与标签同样转义、`PropertyControlRow` 不生成 Reset 按钮、
`PropertyInfo` 可带可寻址数值 span。
业务回归放在 `editorUiTests.cpp`，验证组件组合后的命令、保存与选择行为。
本批新增行为用例：通道/参数/默认值三层范围解析、旧 M_ 无范围时的
`max(1, |defaultValue| * 2)` 兜底（且不跟随正在编辑的值）、
非有限或反向区间仍给出有限有序的滑条、拖动与键入产生同一种 batch item、
拖动不触发有限数校验、只有 Color 参数有 sRGB 色板与十六进制读数、
折叠切换不重建分组 DOM、展开状态跨 Build 与文档切换保留、静止帧写 0 次属性。
第四批新增 actor 树用例：分组顺序与组内排序、组折叠隐藏子行但不改变筛选计数、
搜索的 ASCII 折叠与 UTF-8 多字节名称、类型过滤取值与 `SceneActorGroupSpecs()` 同源、
空状态区分“没有 World”与“没有匹配”、
mesh actor 与材质槽仍走既有 MI 选择路径、非网格 actor 走 `RequestActorSelection`、
无 MI 文档时 Details 展示选中 actor、只读字段按行刷新且节点身份稳定、
主方向光的两个可编辑控件（含同值不发命令、非有限数与非区间值拒绝）、
以及 Details 强度控件区间与运行时页 `#sun-intensity-slider` 一致的漂移守卫。
工作区测试验证 960×540、1280×720、1920×1080 的分区边界，
以及 Viewport 命中穿透、标签切换和重载后的标签保持。
`dockingTests.cpp` 覆盖纯布局树、真实 RmlUi 指针拖动、分隔条、浮动/重新停靠、
持久化校验、重置和焦点保留；面板可见性用例与布局树共用 `DockWorkspaceTest`
套件名（gtest 不允许同一套件混用 TEST 与 TEST_F，因此没有单独建 fixture），
覆盖隐藏/恢复、组内激活移交、整组折叠与几何接管、拒绝隐藏最后一个可见面板、
未知面板名拒绝，以及含 `"hidden"` 与不含该键的持久化往返。
`tool/editor-viewport-tests/` 覆盖视口矩形、溢出边界、半开命中、坐标映射与 aspect。
`contentBrowserTests.cpp` 覆盖目录/历史、搜索与视图状态、地址校验、
跨刷新真实指针双击和滚动保持。`UiColorTransform` 验证 UI gamma 与预乘边界。
`jsonAssetLibraryTests.cpp` 覆盖分类、任意层级 data 省略、输入上限、路径边界、
worker 代际、共享快照及空闲 Poll 不重读；`editorUiTests.cpp` 覆盖只读打开路由、
64 行分页和转义，以及菜单栏的展开/互斥/关闭、Undo/Redo 常驻禁用、
菜单与工具栏共用可用性、Open Level 的 `LoadWorld` 请求、Window 勾选同步、
About 模态，和视口工具条的分组单选、view mode 的 debug mode 映射、
工具条命中不进入场景、静止帧不重复写 class。
`contentBrowserPerformanceTests.cpp` 使用 10000/50000 项合成目录
验证 192 条目加最多两个 spacer、静止帧缓存、整行滚动、类型筛选与双击身份。
50000 项是控制器压力输入，不表示 worker 的 20000 项目录上限已放宽。
widget/docking 测试另覆盖属性缓存失效和静止帧不重写布局属性；CPU fake GPU
工作量断言不作为实际 FPS 或 GPU 性能结论。
CPU 测试里的菜单展开、子菜单悬停和面板隐藏都用真实 RmlUi 事件与指针坐标驱动，
但不等于桌面人工操作验收。

```powershell
cmake --build build --target editor_ui_tests editor_viewport_tests main -j4
ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure
```

先让真实业务使用一个窄组件并补测试，再扩展类别。
新增工作区行为必须保留尺寸约束、焦点/捕获和布局数据校验；
新增 Viewport 能力必须遵循 renderer-owned 纹理生命周期与坐标契约。
