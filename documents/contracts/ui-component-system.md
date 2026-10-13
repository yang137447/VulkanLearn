# UI 组件体系

状态：2026-10-13，扁平 Scene Outliner、运行时 actor 属性编辑、独立编辑器观察相机
及灯光/相机视口辅助图示已实现；场景/MI 草稿共享内容浏览器脏标记与保存确认。

## 工作区布局

默认布局向 UE 编辑器靠拢：顶部标题行最左侧是 File / Edit / Window / Help 菜单栏，
标题右侧保留原有命令按钮，右上 Scene Outliner，右下 Details，
中央/左侧独立 Viewport，底部 Content Browser / Renderer 标签页，最底部状态栏。
Material Slots 和 actor 参数放在 Details 中，MI 文档参数放在中央独立资产页。
每个打开的 MI 对应一个紧邻 Viewport 的停靠标签，显示文件名、脏标记和关闭按钮；
重复打开复用文档，关闭沿用保存/丢弃/取消确认，切回关卡后普通快照刷新不会抢回材质页。
中央页保留 General、Render States、Parameters、Texture Bindings、保存及既有 World 预览命令；
中央页左侧使用正式 `Common/Meshes/SM_material_preview_ball.json` 预览球，右侧为材质参数。
`PreviewMaterial` 经 LoadWorld 事务在 `SC_model_preview` 注入 `preview_material`，
实例级 `materialOverride` 绑定所选 MI，不写球模型或材质文件。新 World 与同路径槽位发布后才显示图像，
重连后补一次未保存的数值/渲染状态草稿。左/右拖动环绕、中键平移、滚轮缩放、相机按钮重新取景。
仍是单活动 World，不是每标签保留一份 World；返回关卡会重新加载记住的关卡路径。
贴图草稿的 live descriptor 替换仍未实现。材质标签同样不随布局写盘。
面板可四边停靠、合并标签、在主窗口内浮动后重新停靠，
分隔条支持拖动并受子树最小尺寸约束；普通面板切换只改变呈现。
模型/关卡页面切换通过既有加载事务选择单活动 World，不提交资产修改。
Scene Outliner 现在渲染真实 World actor 树（网格 / 四类灯 / 相机 / 环境 / 风剖面，
带类型图标、搜索、类型过滤与全局名称排序），行集合按滚动位置窗口化，
每个可渲染 actor 行带一个可见性开关（行尾眼睛），所有类型按名称统一排序，不生成类型文件夹；
选中 actor 由 Details 展示其类型化参数；
无打开的 MI 文档时 Details 不再为空。契约见“世界 actor 树契约”。
面板可以通过 Window 菜单隐藏和恢复，隐藏状态随布局一起保存。
布局保存至用户目录，支持跨启动恢复和 Reset Layout；不支持独立操作系统窗口。
工具条的 Immersive Mode 与 F11 切换窗口内沉浸模式（F1 兼容）：隐藏编辑器与 HUD，
不退出程序、不新建窗口、不改变系统全屏。模型页进入时先恢复记住的关卡，
事务失败或无可返回关卡时保持编辑器可见；退出沉浸模式恢复原停靠布局。
资产文档标签默认加入 Viewport 所在的关卡标签组，紧邻关卡标签；
同组只显示选中页面，普通快照刷新不改变当前标签。手动停靠位置仍可调整。
它**不在常驻面板集合**（`DockWorkspace::Panels()`）里，而是按需创建：
没有打开任何资产时，标签栏里**不存在**资产编辑器标签，默认布局也不含它的位置
第一次及后续打开资产均向关卡所在栈追加标签，不创建新的并排分栏。
布局校验因此只要求常驻面板齐备，**不要求**存在资产编辑器面板。
资产编辑器标签是**会话内**状态：写盘时被丢弃（写出的布局里不含它们），
留下的空栈会被剪掉，重启后不恢复打开的资产。
当前 Content Browser 浏览 MI、Mesh、Scene、Texture、Terrain JSON 元数据，
Mesh、Texture、Terrain 双击后进入资源标签页，右侧为只读 JSON Details。
Mesh 通过预览 World 显示真实模型；Texture 后台加载 PNG/JPEG/TGA/BMP 的 source 并等比例显示 RGB。
贴图标题显示原尺寸及 colorSpace，最长预览边为 1024；忽略 alpha，不把参数图当透明图。
预览区滚轮以鼠标位置为中心缩放（适应窗口倍率的 0.1 到 32 倍），左/中/右键拖动平移，
双击图片区域或 Fit Image 按钮复位。切换/重载贴图重置视图，失焦取消拖动。
缩放只改裁剪区域内的 UI 几何，不重新解码或上传图片；放大仍使用有界预览，不是原图 1:1 检视。
缺失、损坏、越界或未支持格式显示诊断，不沿用旧图。Terrain 仍为类型预览占位。
它不再使用平铺文件名列表：左侧目录树由 worker 发布的只读 JSON 目录派生，
上方提供前进/后退、父目录、面包屑与搜索，资产区支持网格/列表、
名称排序、类型筛选、尺寸档位、子目录开关与显式 Refresh，底部显示数量和完整选中路径。
单击只改变浏览选择；双击 MI 提交原有打开命令并激活中央材质文档页，保持可编辑，
双击 Scene 复用 `UiActionType::LoadWorld` 请求切换 World，并同步打开只读 JSON inspector；
Scene 仍请求 World 切换并使用主工作区 JSON inspector；Mesh、Texture、Terrain
使用资源编辑器标签页。资源标签支持重复打开去重、切换、关闭、返回 World 和重新读取 JSON。
目录与搜索不提交业务命令；贴图、实时模型与材质实例网格有真实缩略图，场景仍是类型占位。
Viewport 使用 RenderGraph 拥有的离屏场景纹理，完整 UV 显示，不拷贝或裁剪 swapchain。
GPU backing 保持原生窗口尺寸；相机 aspect、拾取和离线 film 尺寸跟随面板逻辑尺寸。
关卡左键拾取只更新对象描边、大纲选择与 Details，不创建或激活 MI 文档，不切换预览 World。
鼠标相对捕获已请求或已启用时不执行左键拾取；材质文档由材质槽或资源浏览器显式打开。
视口左上角叠加工具条；其中只有 view mode 有渲染后端，变换工具与坐标系只是呈现状态，
详细边界见“停靠与视口契约”。
视口左上视图模式下拉菜单提供 Lit、Unlit（既有 Base Color 调试映射）和禁用的 Wireframe。
Buffer Visualization 子菜单恢复 Base Color、Emissive、World Normal、Roughness、Metallic、AO、
Shadow、Direct Lighting、Indirect Diffuse/Specular、Shadow Cascade Index、Shading Model，
统一发送既有 SetDebugViewMode；标题和选中状态以运行时快照回显，子菜单限高滚动。
基础模式 0..12 的材质输出在优化运行时也保留，不依赖 ENABLE_DEBUG_VIEW；
Lit 直接返回正常光照，避免只在最终合成切换而材质仍输出 HDR 导致参数视图饱和。

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
| Views / 数据视图 | ListRow、TreeRow | 已实现行、缩进、展开符号、选择状态、行首类型图标、行尾附加控件（逐 actor 可见性）与行样式 class；世界 actor 树在此之上按行预算挂载可见窗口 | 通用虚拟化 ListView/TreeView、TableView、通用筛选 |
| Navigation / 导航 | DocumentTab、MenuItem | 已实现激活与关闭分离、菜单项勾选/禁用与条目索引寻址 | 菜单快捷键、Breadcrumb、子菜单键盘导航 |
| Workspace / 工作区 | DockWorkspace、DockTab、面板可见性、脏关闭弹窗 | 四边停靠、标签合并、窗口内浮动、布局持久化与重置、面板隐藏/恢复 | 原生多窗口、通用 Modal |
| Property / 属性 | PropertyRow、PropertyGroup、PropertyInfo、PropertyControlRow、CollapsibleSection、NumericComponents、ColorPreview | 已实现属性组合、Reset 地址、向量分量、可选范围滑条、折叠分组、sRGB 色板与十六进制读数、无 Reset 的只读/可编辑行、可寻址数值 span | 多对象混合值、属性适配器、可编辑调色板 |
| Editor / 业务 | 顶部菜单栏、世界 actor 树、JSON Content Browser、MI Details、actor Details、资源编辑器、离线控制、Viewport、视口工具条 | 已接入组件、受限浏览窗口/分页、资源标签、Mesh 双击经 `PreviewModel` 注入 `SC_model_preview`、Mesh 专属会话工具条与模型信息 Details、renderer-owned 离屏视口、扁平 World actor 列表、类型化数值编辑、可见性、灯光/相机辅助图示及场景/MI 保存确认 | 场景对象增删、gizmo |

“已实现”只表示上表的窄组件，不表示已经具备 Slate 同名控件的全部行为。
通用 ListRow/TreeRow、模型树和 Sources 树尚未虚拟化；
Content Browser 业务网格/列表与 Scene Outliner 世界 actor 树已有受限可见窗口，
JSON inspector 使用分页，不等同于通用 ListView/TreeView 已完成。
数值组件是文本输入加可选范围滑条：滑条只做范围语义下的快速调整，
精确输入仍由同一行的文本输入负责，两者共用同一个 action/index/component 地址。
分组折叠只改既有分组元素上的 class，不重建 DOM；
材质贴图下拉选项只在候选路径/有效性、当前有效绑定或 Details DOM 改变时重建，
相机移动不重新拼装整份选项字符串；控件当前值仍逐帧同步并保留焦点输入。
滑条的 min/max 属于分组 markup，随 identity（assetPath + schemaDigest）重建。
Color 仍按线性 RGBA 存储，编辑器只多出面向 sRGB 的色板与 `#RRGGBBAA` 读数，
没有可编辑调色板；场景缩略图、纹理逐像素读数、
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

- 没有 gizmo，也没有场景对象的增/删/复制/改名；Transform 可在 Details 数值编辑。
- 完全没有 Undo/Redo 系统；`Edit` 菜单里的 Undo/Redo 是常驻禁用的占位。
- 逐 actor 可见性只有**运行时**语义：开关不写进场景 JSON，`SceneAssetValidator`
  不认识它，也没有 `SaveVisibilityToScene` 之类的动作；重新加载场景后所有 actor
  都是可见的。Transform/灯光/相机参数编辑形成独立场景草稿，确认后写回场景资产。
- 没有地形节点：`World` 没有 terrain 访问器，仓库里也没有 terrain 运行时
  （场景选择器只把 `"terrain"` 列入受支持类型，资产浏览器只有 Terrain JSON 元数据）。
- 大纲行窗口化只覆盖世界 actor 树：通用 `TreeRow` 组件本身仍不虚拟化，
  模型树与 Sources 树也仍整棵挂载。
- 大纲仍是单选：没有多选、没有拖放/重父级，也没有改名（改名同样属于场景文档阶段）。
- 贴图、实时模型与材质实例网格有缓存缩略图，场景没有真实缩略图，也没有纹理逐像素读数；Mesh 预览目前复用中性场景视口，
  模型标签通过 `resource-preview-image` 显示当前 renderer-owned 离屏图像。
  工具条变换按钮目前仅切换本地状态，不提供 gizmo；模型信息 Details 中
  `Metadata ready` 仅表示 JSON 已读取，不表示模型渲染成功。发布后按模型所有 section 的世界包围盒自动取景。
  图像内左/右键拖动环绕、中键平移、滚轮缩放；双击图像或相机按钮重新取景。
  导航只修改预览相机，不写资产；动作带 World generation，切换/重载模型重新取景，失焦取消拖动。
  环绕预览隐藏场景的固定 `neutral_backdrop` 平面以免遮挡背面，保留原 HDRI 照明和背景；不写回场景文件。
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
大纲宿主按稳定 actor 身份记录材质槽展开，World generation 变化时清空展开、
搜索与过滤（排序是面板偏好，与 Content Browser 的排序键一样跨换代保留）；
子材质节点与现有 Material Slots 走同一个选择 sink。
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
  `LightSnapshot`（与渲染快照同结构）、相机展示字段与作者 `CameraExposure`、环境与环境/风剖面标量；
  没有 World、SceneNode、MaterialInstance 或 Vulkan 句柄。列表以
  `shared_ptr<const std::vector<...>>` 进入面板快照（与资产目录同一约定），
  逐帧不深拷贝 actor 数组。
- **覆盖范围**：网格（按 `MeshSceneObjectIdentity()` 给出的 `sceneObjectIdentity`
  聚合 section，逐 section 带
  mesh/material/MI 路径/槽名/风剖面 key）、directionalLight / pointLight /
  spotLight / squareAreaLight、相机、环境、SpeedTree 风剖面。
  灯光参数复用 `WorldSnapshotBuilder` 的同一份 World→`LightSnapshot` 映射
  （`BuildWorld*LightSnapshot`），对象 `RuntimeId` 复用 `WorldObjectRuntimeId`，
  避免第二份身份规则与选择描边漂移。
- **列表与顺序**：不自动按类型分组。`SceneActorGroupSpecs()` 只提供类型、过滤键、
  标题与图标。所有 actor 按显示名做 ASCII 大小写不敏感比较，再按稳定身份打破平局，
  不依赖 `unordered_map` 遍历顺序；展开的材质槽紧跟所属 actor。
- **行窗口预算**：`SceneActorMaxMountedRows = 192`、`SceneActorOverscanRows = 2`
  位于 `source/editor/ui/editorSceneActorTree.h`（与 Content Browser 的
  `MaxVisibleItems` 同一量级）。`#scene-models` 任一时刻挂载的行节点不超过该预算，
  另加最多两个 `.outliner-spacer`；spacer 用"整行高 × 未挂载行数"补齐滚动范围，
  因此完整 actor 列表仍可整段滚动到达，不是截断前 192 行。
  窗口由 `ComputeSceneActorWindow(行数, 可视高度, 滚动偏移, 行高)` 纯计算得出，
  宿主按"行集合 revision + 窗口恒等 + 选中态"决定是否重挂 DOM。
- **行高假设**：树行等高是窗口化的前提，由 RCSS 显式固定
  （`#scene-models .tree-row { height: 30px; }` + `.list-item { line-height: 16px;
  white-space: nowrap; text-overflow: ellipsis; }`）。actor 行与
  缩进的槽行因此同高；长名称按省略号裁剪而不是把一行撑成两行。
  宿主逐帧从已挂载行的 margin box 实测行高（首帧用
  `SceneActorRowHeightFallback = 30`），因为行高一变 spacer 高度就会与真实内容不符、
  滚动位置随之漂移。`EditorUiTest.OutlinerRowHeightsAreUniformAcrossRowKinds`
  用真实 RmlUi 布局量 actor/槽行的 margin box 来守这条假设。
- **展开与窗口**：只展开 actor 的材质槽，不折叠类型组；槽行参与同一窗口预算。
- **搜索、过滤与排序**：搜索复用 Content Browser 的 `FoldAsciiCase`
  （`editor/editorTextMatch.h`），只折叠 ASCII 大小写、保留 UTF-8 多字节名称；
  类型过滤取值就是分组键。`#outliner-sort` 与 `#browser-sort` 同键
  （`name-asc` / `name-desc`），统一重排所有类型的 actor，
  也不改变选中身份、展开集合与滚动偏移。三者都只改可见性/顺序，不提交任何命令。
  计数显示的是筛选结果（`N / M actors`，不计材质槽），
  与 Content Browser 的计数同一语义，与折叠状态和挂载窗口无关。
- **选中态与可见性原地刷新**：只改已挂载行的 class、眼睛图标及提示，
  不因第一次点击销毁目标 DOM；真实指针双击因此能跨过选中快照刷新后继续取景。
- **选择路由**：mesh actor 继续走 `RequestModelSelection`，材质槽继续走
  `RequestMaterialSelection`（MI 文档、Material Slots 与视口描边行为不变）；
  非网格 actor 走新的 `RequestActorSelection(稳定身份)`，同时要求 EngineLoop 清掉
  渲染端 mesh 描边，保证大纲高亮与视口描边不会同时指向两个对象。
- **Details**：`#actor-details-content` 中的可折叠分组以选中 actor 名称为标题。
  网格编辑根 Location/Rotation/Scale，节点灯光编辑 Location/Rotation/线性 RGB/强度；
  spot 编辑 inner/outer 半锥角，面光编辑位置/法线/边长/辐亮度；相机编辑位置/旋转/FOV/近远面。
  Camera Details 的 Exposure 分组编辑 Manual/Auto、手动 EV100、补偿、自动 EV 边界、
  上下适应速率（1/s）与目标灰度（linear）；只在选中 Camera 时出现，没有独立后处理标签页。
  physical 配置显示只读推导 EV100，点击 Manual 才显式选择 ev100；刷新不转换模式。
  参数行固定挂载，模式只改变可见性；隐藏参数不接受 UI 编辑，焦点输入与静止帧写缓存保留。
  环境和风剖面仍只读。Material Slots 仅在存在选中模型材质槽时显示，并位于 actor 参数之后。
  控件来自 `SceneActorEditableFields()`，值变化只刷新控件，不重建 DOM，保留焦点文本。
- **对象编辑边界**：`SetActorProperty` 通过 EngineLoop → RuntimeCommandExecutor →
  `ApplyEditorActorProperty` 在 GT 写 World，payload 是稳定身份、World generation、枚举属性、
  分量和值。执行端拒绝陈旧代数、错误类型/分量、非有限数、零缩放、负颜色/强度、
  交叉锥角与无效相机投影；每条编辑从当前对象取其它分量，不用旧快照覆盖同帧 XYZ 编辑。
  主方向光强度与 CSM castShadows 控件也使用带 World generation 的 actor 命令。网格根 TRS 从 mesh build plan 保存，
  新旧根矩阵的差应用于所有 section，保留子矩阵和负缩放。已接受的编辑记录到 GT
  `SceneDocumentService` 草稿，编辑本身不写资产；没有撤销系统。
- **逐 actor 可见性（第 6 轮新增，第 7 轮修正为整 actor 语义）**：网格与四类灯的 actor 行在行尾有一个眼睛开关
  （`data-action="outliner-visible"`），点击只提交一条
  `UiActionType::SetActorVisible`（`stringValue` = 稳定身份，`intValue` = 0/1），
  经 EngineLoop → `RuntimeCommandType::SetActorVisible` →
  `RuntimeCommandExecutor` 落到 World；没有第三条路径，UI 回调不访问 World。
  相机 / 环境 / 风剖面没有渲染侧可见性状态，因此**不显示**眼睛
  （`SceneActorHasVisibilityToggle()`）。
  - **状态归属与渲染过滤**：可见性是 World 拥有的运行时游戏状态
    （`WorldMeshObject::visible`、`SceneNode::visible`、
    `WorldSquareAreaLight::visible`），由 `World::SetMeshSceneObjectVisible` /
    `SetLightVisible` / `SetSquareAreaLightVisible` 写入；对象不在当前 World 时
    返回 false，执行端给出人类可读诊断而不是静默忽略。
    过滤发生在 `WorldSnapshotBuilder::Build()`——不可见的网格与灯光**不进入**
    `snapshot.meshDraws` / `snapshot.lights`。`meshDraws` 同时是出图、拾取、
    MaterialInstance 上下文与阴影 pass 的唯一来源，因此"不可见"在构造处生效
    就等于对整条渲染路径生效，渲染器里没有第二处可见性分支。
  - **一行 = 一个场景网格对象（整 actor 语义）**：网格的写入端按
    `sceneObjectIdentity` 寻址，一次写全该身份下的**所有** section，而不是按
    section 键（`<场景对象>::<section 名>`，`rendererMeshLoader` 的命名）只改一个渲染对象。
    多 section 模型的身份不是 `World::meshObjects` 里的任何一个键，这就是"眼睛点了没反应"
    的成因：执行端会以"actor is not in the active World"拒绝。
    身份推导只有 `MeshSceneObjectIdentity()`（`source/world/meshSceneObjectIdentity.h`）一份，
    读取端（actor 行聚合、`MeshDrawSnapshot::sceneObjectIdentity`）与写入端共用，
    防止两边再次漂移。单 section 网格的身份等于自己的对象名，行为与之前的逐对象写入完全一致。
  - **不一致 section 的点击规则**：字段默认可见，上一版构建也可能留下
    "部分 section 隐藏"的混合状态，因此行状态与查询都取
    **任一 section 可见即为可见**（`std::any_of`），点击写入的是该行显示状态的反面：
    可见 → 全部隐藏，隐藏（即全部不可见）→ 全部显示。
    于是从**任何**起始状态点击一次后所有 section 必然一致，行的显示状态与渲染状态严格吻合；
    不存在"点一次只收敛一半"或"行说隐藏、画面还有一半"的中间态。
    多 section actor 不再被拒绝；未知分组键、无渲染侧可见性的类型、
    身份确实不在当前 World（含旧场景/已改名的身份，以及把 section 键当身份下发）仍然拒绝。
  - **没有快照缓存需要失效**：`RenderSystem::Render()`（单线程）与
    EngineLoop 的 `PublishSnapshotFromActiveWorld()`（双线程）**每帧**都从 active
    World 重新构建快照，`currentRenderScene` 也随之逐帧重建，因此可见性翻转在下一帧
    直接上屏，不需要 World 换代，也不触发 `device.waitIdle()`。
  - **行表现**：可见时 `icons/eye.png`、隐藏时 `icons/eye-off.png`，
    隐藏行同时得到 class `hidden-row`（标签色 `#7c7c85`、行首图标 opacity 0.45），
    因此"这一行代表不可见对象"有两个像素可回读的信号。
    眼睛是行的 flex 兄弟节点（与 `document-tab` 的关闭按钮同一做法），
    行高仍由 RCSS 固定为 30px；点击眼睛不改变选择、展开、滚动偏移与行窗口。
  - **隐藏不改选择与文档**：隐藏的 actor 仍可选中、仍驱动 Details，
    它的材质实例文档与槽行都不受影响（可见性不参与 MI 导航）。
    视口拾取来自 `meshDraws`，因此隐藏的网格**不可拾取**，
    与"隐藏等于从场景里看不见"一致；重新显示后自动恢复可拾取。
  - **不持久化**：可见性不是场景 JSON 字段，`SceneAssetValidator` 不认识它，
    也没有保存命令；重新加载场景一律从"全部可见"开始。
- **仍是单选、没有拖放**：大纲一次只有一个选中身份（`snapshot.selectedActorIdentity`
  加可选的材质槽号），没有多选、没有拖放/重父级，也没有改名；
  窗口化不改变这一点——挂载的行节点与事件地址仍是一对一。

## Content Browser

`EditorContentBrowserController` 从 `MaterialInstanceEditorSnapshot.assetCatalog` 的
共享不可变目录派生 Sources 树、当前目录、历史和筛选集合；无目录时兼容旧 MI
`assets` 快照。正式目录是唯一资产集合，文档快照仅叠加已有 MI/Scene 的 dirty 标记，
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

### 修改文件与保存确认

GT runtime 合并所有已打开的脏 MI 与场景文档到 `modifiedDocuments`，
不依赖当前活动标签。场景对象修改标在所属 Scene 文件，而不是模型或材质文件上；
观察相机移动、临时可见性与 `SC_model_preview` 不产生可保存的场景草稿。
Content Browser 的星号原地更新；右键脏资产的 `Save...` 只列被右键的完整路径。
File→Save 与顶部保存图标打开同一个 `Save Content` 弹窗，默认勾选全部修改文件，
可逐项取消、Select All、Save Selected 或 Cancel；MI 页 Save / Ctrl+S 在材质页只列活动 MI，
其他页面的 Ctrl+S 列全部修改文件。无修改时不打开弹窗。

弹窗打开不写盘，先提交待处理 MI batch 并等待下一次权威 revision 快照再允许确认。
每个勾选文件发一条带 revision 的 `material.save` 或 `scene.save`；
批量保存是逐文件原子写入，不是跨文件全有或全无事务。成功才清除该文件的星号；
未勾选文件保持脏，全部选中文件成功后关闭弹窗。失败或冲突保留草稿、星号和诊断，
重试读取当前 revision。确认期间阻止工作区、视口输入和重复提交。
checkbox 的默认 click 必须到达目标节点；change 只同步选择，快照更新原地设置 checked，
不销毁焦点节点，程序刷新由 `WidgetRefreshScope` 抑制。

`SceneDocumentService` 按资源根内规范路径保存 baseline/draft、BLAKE3 摘要及编辑历史；
只补丁网格 TRS、灯光参数、作者 Camera 变换/投影/曝光与主光 castShadows，保留其它 JSON 字段。
灯光保留作者 lux/cd/lm，spot 锥角及面光尺寸变化反算作者强度以保持当前规范亮度。
Camera 变换后去掉会覆盖 rotation 的旧 look_at；曝光编辑只改接受的具名字段，
保留未触碰的 physical 参数和数字精度，省略的自动默认值在复位后仍省略。
切换预览/关卡后，源摘要未变时将接受历史重放到新 World generation；
外部变化拒绝重放与覆盖。未保存草稿仅存于当前进程，没有场景 Reload/Discard 界面。

图标为 Lucide 的已提交 PNG，许可位于 `ui/icons/LUCIDE-LICENSE.txt`，
没有运行时下载、SVG 解码或额外图标字体依赖。统一材质球及其他类型图标仅是类型图示，
贴图网格异步替换为真实 RGB 缩略图；模型与材质实例在独立离屏渲染完成后替换类型图标。
场景和不支持实时渲染的资产仍保留占位，不能把统一球图标视为真实资产内容。
资产生成入口为 `tool/editor-ui-assets/generate-content-browser.cjs`，
需要已安装的 `lucide` 与 `sharp` 模块目录作为参数，不参与引擎构建。

### 贴图缩略图

`EditorThumbnailLibrary` 是独立于 JSON 文档读取的 CPU worker，不加载 World，不调用 Vulkan。
请求只来自网格的可见窗口（含前后两行，最多 192 项）；列表模式、隐藏面板不请求。
窗口变化替换待办集合，离开窗口的旧任务不发布；失败结果有悬停诊断，显式 Refresh 才重试。
刷新同时使内存缩略图失效，并重新读取描述文件和 source 内容，不依赖时间戳判断一致性。

输出固定为 256×256 显示用 sRGB RGBA8，保持源图比例，空白处填深灰。
PNG/JPEG/TGA/BMP 等 STB 支持的 LDR 图片在线性域面积平均；Radiance HDR 用固定
Reinhard 映射，不自动曝光。EXR/DDS/KTX 暂不支持并保留图标和诊断。
默认展示 RGB；仅 `channelsDescription.a` 精确为 `opacity` 或 `alpha` 时合成棋盘格，
法线/参数图的打包 alpha 不作为透明度。输入先检查根内 canonical 路径、64 MiB 字节及 32 MP 像素上限。

缓存位于项目 `.cache/editor-thumbnails/`，不写正式资产；键由版本、尺寸、描述文件和源图片
完整内容的 BLAKE3-256 构成。固定尺寸像素载荷带独立摘要并原子写入，损坏缓存自动重建，
写盘失败仍显示内存结果。磁盘最多 512 个条目，按写入时间淘汰；CPU LRU 最多 256 项。
每帧最多发布两项新结果，场景视口输入或编辑器指针捕获期间暂停新任务/新图发布。
Rml 回调只消费已发布像素，UI/GPU 纹理离开可见窗口便经既有退役路径释放。
结果完成只更新原节点的 img src、角标和悬停提示，不替换卡片 DOM；静止帧不重复写图片属性。
卡片名称和类型行固定高度且不参与 flex 压缩，完整名称与路径放在悬停提示中。

### 模型与材质缩略图

`EditorThumbnailLibrary` 通过单槽值语义邮箱交出 Mesh/MI 请求，CPU worker 不调用 Vulkan。
`EngineLoop::TickEditorThumbnails` 在上一帧 RT 归还所有权后推进 `EditorThumbnailRenderSession`；
输入结束 750 ms 后，每帧至多推进一个准备阶段或一个 IBL mip。离开可见集合或 Refresh 的
迟到结果不能发布；隐藏编辑器、视口输入、指针捕获期间不推进。
模型导入与 MikkTSpace 切线生成、材质 shader 快照/编译/反射、贴图解码与行翻转
使用按值冻结请求的 CPU worker，GT 只轮询完成状态；会话最多一个在途 CPU 任务，
切换请求丢弃旧结果，退出前收口 worker。worker 使用独立 ShaderCompiler，只读正式
cache 并准备内存产物，不发布 manifest、不访问 live PipelineFactory 或 Vulkan。
GT 使用 shader 候选前复核源摘要，绑定材质前复核 M_/MI_/T_ 描述摘要。
贴图复用 Texture 的 sRGB/linear、四通道与一次 V 翻转合同，GT 每帧至多上传一张
已解码贴图；上传后释放其 CPU 像素。GPU 创建与材质绑定仍在 GT。
材质缩略图在导入球之前检查实时支持，CPU-only 材质直接显示诊断。

会话独占 RenderGraph、候选资源缓存、frame UBO、相机、环境烘焙和 readback buffer，
不发布 WorldManager、不提交主图资源包，也不复用活动关卡的可变 UBO/材质参数。
图从正式配置派生：固定 256 方形输出，保留 pass 输入/binding，去掉编辑器网格、自动曝光调度和 MSAA；
缩略图禁用 CSM，保留 HDRI 的 SH/镜面 IBL、材质真实 shader 与固定曝光。
使用 `SC_model_preview` 的 HDRI 与相机曝光，隐藏背板；模型按全部 section 包围盒取景。
MI 使用正式 `SM_material_preview_ball`，通过实例级 materialOverride 绑定目标材质。

同一批请求复用离屏图、pass 材质和 HDRI 烘焙；材质缩略图还复用固定预览球的不可变
CPU/GPU 几何与包围盒，只导入并生成切线一次。每个 MI 在独立候选中重新绑定。
会话最多缓存 64 个 CPU shader 变体，不缓存编译源快照或任意模型的导入结果。
材质、贴图、对象 descriptor 与任意模型的几何在本次 fence 完成后释放，不累积任意模型缓存。
每次提交只等待专用 fence，不调用全局 waitIdle。
窗口重建销毁会话，后续请求重新准备；Refresh 同时使会话环境和图缓存失效，
保留唯一在途 CPU 任务到完成后丢弃，不在刷新回调中等待。
结果复用上节有界 CPU LRU，但模型/MI **不写磁盘缓存**，没有跨启动恢复或自动依赖监听。
GT 的材质资源绑定、GPU 上传与管线创建仍是同步阶段，750 ms 是输入退避而非严格帧耗时预算。
离线专用材质、无效资产显示类型图标和悬停诊断，不降级成近似着色。场景截图缩略图尚未接入。

## JSON 元数据与输入边界

`source/editor/assets/editorJsonAssetLibrary.*` 提供 GT facade 与独立读取 worker。
初始化请求一次目录扫描，后续仅显式 Refresh 重扫；打开请求读取选定 JSON，贴图另读 source。
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
普通引用路径仅作为元数据；唯一像素例外是显式打开 Texture：worker 在 canonical 资源根内
解码 source，输入最多 64 MiB、解码最多 32 MP，面积缩小到最长边 1024 后发布不可变快照。
目录扫描不解码；worker 不接触 World/GPU。按 colorSpace 在线性域缩小后编码 sRGB 显示，
UI 以 revision URL 注册像素并走已有纹理上传/退役路径；切换、重载不会串图。
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
`source/editor/ui/editorContentBrowserController.cpp`；世界 actor 树的同类常量
`SceneActorMaxMountedRows = 192`、`SceneActorOverscanRows = 2`、
`SceneActorRowHeightFallback = 30` 位于 `source/editor/ui/editorSceneActorTree.h`。

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

### 右键上下文菜单

全工作区只有一个弹层 `#editor-context-menu`，位于根节点（与 `#close-dialog`、`#about-dialog`
同级），**不在 `#editor-toolbar` 内**，也不在 `#editor-content` 内：前者会让浮层改变工具条的
按钮统计，后者的 `overflow` 会裁掉靠近边缘的菜单。它自身不保存目标；标题与条目在每次打开时
由面板写入，条目复用 `.menu-item` 叶控件但**不复用 `.menu-popup`** —— 后者的显示依赖
`.menu-root.open` 祖先，本弹层没有菜单栏父节点。

**触发方式是 `mouseup` 且 `button == 1`**，不是 `contextmenu`：RmlUi 6.2 没有 `contextmenu`
事件标识。按钮号来自 `ToRmlMouseButton`（`source/ui/uiSubsystem.cpp`），
`PlatformMouseButton::Right → 1`，与 RmlUi 官方 SDL 后端一致。面板在捕获阶段监听
`mouseup`，而 `EditorDockingController` 只在 `button == 0` 时开始拖动，因此右键不会干扰停靠。

**目标在打开时就解析为稳定地址**（actor 用 `actor.identity`，资产用 DOM 的 `data-path`），
菜单随后只用这个地址寻址，**绝不回退到"当前选中"**。原因是菜单条目在之后的点击里才执行，
而快照下标只对当帧有效；目标身份已不在 World 里时，菜单**拒绝执行并给出 notice**，
而不是拿面板当前选中顶上 —— 那个回退正是"右键 A、命令作用到 B"的静默错误目标缺陷。
回归由 `EditorUiTest.ContextMenuVisibilityToggleTargetsTheRightClickedActorNotTheSelection`
钉住（选中一个 actor、右键另一个、断言命令携带被右键者的身份）。

**"菜单打开"蕴含"至少有一个真实条目"**：条目列表单独构建，构建后为空则不开菜单。
没有 actor 级动作的对象（相机、环境、SpeedTree 风剖面）与只读目录节点因此不会开出空弹窗。

大纲 actor 行的四个点击靶面（`outliner-actor`、`outliner-expand`、`outliner-visible`、
`outliner-slot`）携带同一个 actor 下标，所以在行内任意位置右键都指向同一个 actor；
右键眼睛打开的是 actor 菜单而不是眼睛专属动作 —— 菜单里的"显示/隐藏"与眼睛提交的是
同一条命令，标题又写着 actor 名，因此不存在寻址歧义。

视口右键原来在场景命中时被提前返回、根本到不了 RmlUi；现在只把
`PlatformMouseButton::Right` 放行给 RmlUi，左键保持原样（拾取是左键专属，相机朝向走
相对鼠标增量而非按键手势）。视口没有拾取能力，因此它的菜单**只含视口级命令**
（view mode 复用 `UiActionType::SetDebugViewMode`、保存打开共享修改文件确认），
不放任何逐对象动作，也不为了撑菜单而虚构拾取。

GT 文档只使用 `vl://scene-viewport` 逻辑纹理 token 和 framebuffer 像素矩形。
图像、兼容 render pass、framebuffer 和 GPU epoch 退休属于 renderer；
不得将裸 Vulkan handle 放入组件或 GT 文档。视口命中使用半开边界，
拾取先减去面板原点；输入模态、其他面板与拖动状态不穿透至场景。
完整渲染与 resize 契约见 `game-ui-stack.md`。

视口工具条 `#viewport-toolbar` 叠在场景图像左上角，分三组：
变换工具、变换坐标系、view mode。只有 view mode 有真实后端，
映射到既有 `UiActionType::SetDebugViewMode`：Lit=0（完整画面），Unlit=1（基础色）。
变换工具与坐标系**只是编辑器本地呈现状态**：仓库里没有 gizmo，
工具条不驱动 Details 的数值变换，切换它们只改变高亮，不提交任何 UiAction 或资产命令，
按钮提示文字明确写出这一点。Wireframe 没有 wireframe 光栅模式，
`debug.*` 表里也没有对应的 debug view，因此按钮常驻禁用并带说明，
没有拿最接近的调试视图冒充。view mode 的高亮来自冻结的运行时快照
`debugViewMode`，不是面板自己的第二份状态；运行时处于工具条之外的调试视图时
Lit/Unlit 都不高亮，工具条不声明自己没有设置过的模式。
`IsSceneViewportElement` 排除工具条与灯光/相机图标，否则点击按钮会同时驱动相机与拾取。

### Actor 可视化与观察相机

关卡编辑时 `World::GetRenderCamera()` 使用 World 持有的独立 editorCamera；
`GetCamera()` 仍是作者 Camera。EngineLoop 在稳定 GT 点切换 Controller 的 view target，
WorldSnapshotBuilder 冻结观察相机；退出编辑器回到作者相机，编辑器观察位姿跨切换保留。
模型/MI 的 `SC_model_preview` 不启用这条路径。曝光仍跟随作者 Camera。

`#viewport-actor-overlay` 是纯 UI 叠层，按观察相机的水平 FOV、基向量及视口实际尺寸投影。
辅助线与图标使用独立子层缓存；选中时只改图标 class，不销毁双击目标。
图标结构仅随 actor 地址/名称/类型变化重建；观察相机移动通过 `transform` 更新固定
24px 盒的位置，出屏或不可见时使用 `visibility`，不把投影坐标写进 RML。
选中对象的线段按未裁剪拓扑挂载固定槽位（点光/spot 最多 72 条），
1px 线盒通过平移/旋转/缩放显示，近面与屏幕裁剪只更新槽位的变换和可见性。
因此相机运动与浮点末位变化不会通过重建图标/线段触发整份工作区布局。
外层使用 `clip: always`：RmlUi 的自动裁剪依赖 scroll extent，负偏移的边缘图标
不一定扩大它，单靠 `overflow: hidden` 可能画进标签栏。
灯光/Camera 使用已有类型位图作为固定像素图标，选中时画方向箭头、点光位置圆、
spot 内外锥角、面光边框/法线或相机图示视锥。线段先裁近远面，再裁屏幕边界，
因此跨眼点的线不发生倒投影。叠层不读取深度，不提供实体模型或 gizmo；
point 圆是位置标识，不是衰减范围，spot 图示长度为 200 cm，不是光照截止距离。
工具条眼睛可关闭全部图标/辅助线。点击图标走既有 Actor 选择；双击图标/大纲 Actor
发带代数的 `FrameEditorActor`，只移动编辑器观察相机，不修改场景对象或文件。
网格取景合并全部 section 的变换后包围盒，按较窄 FOV 与 near clip 留余量，
必要时只扩展观察相机的 far clip；偏心模型不使用作者根位置作为包围盒中心。
相机图示视锥截面深度有显示上限，使用真实 FOV，不把截面冒充 far clipping plane。

### 角落坐标轴指示器

`#viewport-axis-indicator` 是**纯 UI 叠层**，不参与场景渲染图、不参与深度、不进 RenderGraph。
它放左下角（与左上的视口工具条同一侧但不同端，因此不重叠），并设 `pointer-events: none`
—— 它是读数不是控件，点它不应该抢走视口的场景输入。

它回答的是"**相机现在朝哪**"，因此与场景里世界原点处的真实三轴是**两件不同的东西**，
不是同一个功能的两种呈现：原点三轴是场景几何，会被物体遮挡、远看投影只剩约 1px；
这里的三根固定 20px 长、始终可见。两者并存是刻意的。

朝向来自**已冻结的大纲快照**：Camera 行的 `viewportCamera.forward/right/up` 由
`editorSceneActorBuilder` 从活动 World 的观察相机填充，
面板也依然不访问 renderer。

投影数学在 `source/editor/ui/editorViewportAxisIndicator.h`，**刻意不依赖 RmlUi**，
所以能脱离 UI 框架做模块测试（`editor_viewport_tests` 的 `EditorViewportAxisIndicator` 套件）。
三条约定值得记住，它们都是"写反了画面未必立刻看出来"的类型：

- 屏幕 y 向下，因此 `screenY = -dot(axis, up)`；世界 +Y 必须投到**负**的屏幕 y。
- `depth = dot(axis, forward)`，`forward` 指向屏幕内，所以**越小越靠近观察者**；
  面板按 depth 降序绘制，让朝向观察者的轴盖住背向的轴。
- 与视线近乎平行的轴**退化**：屏幕长度趋零、方向角没有意义。此时隐藏杆，
  **标签一并隐藏**并把偏移归零 —— 只收杆不藏标签会让它落到锚点上，与另外两轴的字母
  叠成一团（实测三个字母挤在 6x8 px 内，无法辨认）。

杆和标签的布局盒保持固定尺寸：旋转/位移使用 `transform`，退化使用 `visibility`，
不通过修改 margin、width 或 display 驱动逐帧布局。
角度的量化只用于**值缓存**：签名（角度/偏移取整 + 退化标志 + 深度排序）不变就不写 DOM，
因此相机静止时指示器不产生属性写入，与本文件其它控件同一策略。

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
`editor_ui_tests` 也编入 `source/world/world.cpp`、`source/world/worldSnapshotBuilder.cpp`、
`source/editor/scene/editorSceneActorBuilder.cpp` 与 `source/sceneNode.cpp`，
因此逐 actor 可见性的 World/快照回归跑的是真实 World 与真实快照构建器，
而不是测试替身。
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
actor 树用例已改为：跨类型统一排序、不生成类型文件夹、过滤计数不包含材质槽，
搜索的 ASCII 折叠与 UTF-8 多字节名称、类型过滤取值与 `SceneActorGroupSpecs()` 同源、
空状态区分“没有 World”与“没有匹配”、
mesh actor 与材质槽仍走既有 MI 选择路径、非网格 actor 走 `RequestActorSelection`、
无 MI 文档时 Details 展示选中 actor、只读字段按行刷新且节点身份稳定、
主方向光的两个可编辑控件（含同值不发命令、非有限数与非区间值拒绝）、
以及 Details 强度控件使用 0..150000 lux 物理单位区间的回归守卫。
第五批新增窗口/排序用例：纯数学放在 `widgetTests.cpp`（窗口挂载区间上界、
spacer 高度之和等于整棵树、滚动到底必须挂到真正的最后一行、50000 行仍受 192 预算约束、
扁平列表完整滚动范围、展开 mesh actor 按模型顺序追加槽行、
排序统一翻转所有类型顺序、排序键解析拒绝未知值、
行内容签名只跟随行级字段），面板侧放在 `editorUiTests.cpp`
（actor/槽行的 margin box 等高实测、122 mesh 场景挂载行数受预算约束且计数是筛选总数、
滚动换一批挂载行并能到达最后一行、不生成类型表头、
排序在保持选中/展开/滚动偏移的同时真正重排、排序控件落在过滤栏矩形内、
Detach 期间 Build 不触碰文档且重新 Attach 后重挂一次）。
第六批新增可见性用例：`widgetTests.cpp` 覆盖 `TreeRow` 行尾控件是选择按钮之外的
独立动作地址、`hiddenRow` 是固定 class 而不是宿主可控文本、
`SceneActorIdentity` ↔ `ParseSceneActorIdentity` 往返（含名字自带 `|`、
未知分组键、空名字被拒绝），以及只有网格与四类灯提供开关；
`editorUiTests.cpp` 覆盖真实 World 的三条可见性写入端（未知名字返回 false）、
快照构建器真的**省略**不可见网格与灯光（含恢复后数量与身份回到初始）、
`BuildEditorSceneActors` 把 World 可见性发布到冻结快照（相机/环境恒为可见）、
点眼睛只提交一条 `SetActorVisible` 且不动选择/展开/滚动/行高/行数，
以及隐藏态的 eye-off 图标、`hidden-row` class、仍可选中并驱动 Details、
眼睛几何落在树的可视矩形内、图标盒恒为 14px。
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

2026-10-13 新增 `actorPropertyTests.cpp`：代数拒绝、同帧分量不互相覆盖、
多 section TRS/负缩放、锥角/相机投影校验、主光旋转同步天空、独立观察相机、
偏心 section 合并取景，以及辅助线投影/近面裁剪/相机视锥。
面板测试覆盖枚举编辑地址、焦点部分输入、辅助图标不触发拾取、开关与强制裁剪、
大纲/图标的真实指针双击跨过首次选择快照。
观察相机移动回归使用真实 RmlUi `OnLayout` 计数，验证图标命中跟随绘制变换、
节点身份保持，以及选中点光跨近面/出屏再返回时线段槽位保持且工作区不重新布局。

第六批新增右键菜单用例（13 个，全部在 `editorUiTests.cpp`，名字都含 `ContextMenu`）：
右键大纲行打开该 actor 的菜单、**可见性项作用于被右键的 actor 而不是当前选中**、
可见性项跟随并反转 actor 状态、没有 actor 级动作的对象不提供菜单、选中条幕后关闭、
点击菜单外关闭、材质槽条目复用既有槽选择路由、右键资产打开既有打开命令、
右键文件夹不提供菜单、视口菜单只含视口级命令、视口 view mode 的勾选来自运行时快照
并复用 `SetDebugViewMode`、无修改文件时保存禁用、有修改时打开共享保存确认。
命名一律带 `ContextMenu` 是刻意的：早先有一个用例叫 `BrowserAssetRightClick...`，
`--gtest_filter="*ContextMenu*"` 会**静默跳过它**，运行数看起来是绿的却少一个。

**未覆盖的边界**：`uiSubsystem.cpp` 里把右键放行给编辑器文档的那 8 行没有自动化测试 ——
`editor_ui_tests` 不编译 `UiSubsystem`（需要 Vulkan），面板侧由 `ViewportContextMenu*` 用真实
RmlUi 指针事件覆盖，但从 `PlatformEvent` 到 RmlUi 这一段只有构建与人工核对。
弹层也**没有做窗口边界钳制**：在右下角右键可能让它部分移出窗口。

```powershell
cmake --build build --target editor_ui_tests editor_viewport_tests main -j4
ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|EditorActorProperty|EditorActorVisualization|SceneDocumentTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorViewportGridGeometry|EditorViewportAxisIndicator|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure
```

`EditorViewportGridGeometry` 必须留在选择集里：它住在 `editor_viewport_tests` 而不是
`editor_ui_tests`，漏掉它就会让编辑器网格在门禁里裸奔。选择集的权威副本在
`contracts/testing.md`，这里只是复制；改一处要改两处。

先让真实业务使用一个窄组件并补测试，再扩展类别。
新增工作区行为必须保留尺寸约束、焦点/捕获和布局数据校验；
新增 Viewport 能力必须遵循 renderer-owned 纹理生命周期与坐标契约。
