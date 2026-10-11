# RmlUi 编辑器路线

日期：2026-10-11。当前工作分支：`codex/rmlui-editor`。

## 隔离方式

代码基于原仓库已提交 HEAD 建立独立 worktree，不包含原工作目录未提交开发。
资源仓库也使用独立 worktree；相对 `resourcePath` 指向相邻资源目录。
构建目录、shader 缓存、MI 保存和 Generated 输出均属于独立验证目录。
不要在原开发目录切分支、stash、重置或覆盖资源来验证本分支。

## 当前基线

- 移除 ImGui 构建依赖、上下文、输入桥接、字体 atlas 和 draw-data 转换。
- RmlUi 同时承载运行时控制页和可停靠编辑器工作区。
- 工作区布局对齐 UE 的区域组织：右上 Outliner、右下 Details、底部 Content Browser /
  Renderer 标签页，中央/左侧独立 Viewport；Material Slots 归入 Details。
- 四边停靠、标签合并、分隔条、主窗口内浮动/重新停靠，布局保存/恢复与 Reset Layout 已实现。
- 资源编辑器作为 `Asset Editor` 正式 DockPanel 注册，默认与 Viewport 合并为中央标签组，
  参与同一套拖动、分隔、浮动、重新停靠和布局持久化。
- RenderGraph 导出离屏场景输出，UI 通过逻辑 token 采样；相机、拾取和离线尺寸跟随视口面板。
- 模型选择、材质槽导航、MI 浏览/标签页、参数/纹理/render-state 编辑继续复用现有后端。
- Content Browser 使用 Sources 目录树、面包屑/历史导航、搜索、递归目录筛选、
  网格/列表、排序、类型筛选、三档卡片尺寸与显式 Refresh；
  索引 MI、Mesh、Scene、Texture、Terrain JSON。单击选择，双击 MI 保持可编辑，
  双击 Scene 复用现有 World/RenderGraph 事务切换关卡并打开只读 JSON inspector；
  双击 Mesh、Texture、Terrain 进入独立资源编辑器标签，展示类型预览占位和只读 JSON
  Details；不加载模型、纹理像素或 `data` 引用。
- JSON inspector 支持字段展开、路径/值搜索、诊断和每页最多 64 行；
  任意层级的 `data` 字段及子树不展示、不保留，不读取引用的 payload 文件。
  内嵌 data 仍做语法/输入校验，并非完全跳过其字节读取或词法解析。
- 独立 worker 在初始化及显式 Refresh 时扫描目录，打开选定 JSON 也在 worker；
  GT 逐帧仅 Poll 完成邮箱，按代际发布共享不可变目录/文档，不逐帧扫描或深拷贝。
  目录不受 MI 命令默认 50 项分页限制，但受 worker 的 20000 项上限约束。
- 浏览器按可见窗口挂载最多 192 个条目根节点（含前后各两行预取，仍受上限约束）
  及最多两个整行高度 spacer；目录/筛选/markup dirty 驱动，
  选择和 MI dirty 标记原地更新，滚动后的双击校验路径、DOM 代次和滚动位置。
- WidgetDocument 缓存相同属性写入，避免静止帧重复 DirtyProperty；
  子 DOM 替换只失效后代缓存。UI 动态顶点/索引缓冲从 256/128 KiB 基数倍增，
  容量足够时复用；CPU draw 数组按上一帧数量预留。
- 资源编辑器支持最多 8 个去重标签、World 返回、关闭和 JSON 重新读取；
  类型图标和预览区为事实状态占位，不是实际缩略图、模型几何预览或纹理像素查看器。
- sRGB swapchain 上的 UI 颜色在注册边界转为线性预乘；场景 Viewport 不参与图片转换。
- 同帧修改合并为带原始 revision 的原子 batch；保存包含尚未发布的修改。
- 关闭脏文档需 Save/Discard/Cancel；保存失败不得关闭。
- 数值和 render-state 预览沿用 renderer-owned adapter；纹理可保存，但尚无 live descriptor 替换。
- 离线模式、设置、AOV、曝光、会话控制和导出仍通过 UiAction。
- RML/RCSS 热重载先校验两个候选文档；失败保留旧工作区。
- Color 当前直接编辑 Linear RGBA，不包含 sRGB 调色板。
- 顶部标题行最左侧是 File / Edit / Window / Help 菜单栏；File 与 Edit 的文档命令
  与工具栏按钮共用同一份可用性，Window 勾选直接查询停靠视图的可见性，
  File→Open Level 复用 Content Browser 双击关卡的 `LoadWorld` 请求。
  Edit→Undo/Redo 常驻禁用：仓库没有 undo/redo 系统，这是占位而不是可用命令。
- 面板可以通过 Window 菜单隐藏/恢复，隐藏集合随工作区文件持久化；
  隐藏面板保留布局树位置，整组隐藏时折叠出几何。
- Viewport 左上角叠加工具条：只有 view mode 接到既有 debug view（Lit=0、Unlit=1），
  变换工具与坐标系只是编辑器本地呈现状态，Wireframe 因没有后端而常驻禁用。

这是移除旧 UI 的可运行基线，不是完整 UE 编辑器。
GPU backing 保持原生窗口尺寸，完整 UV 显示至逻辑视口，避免分隔条拖动时重建整个图。
不包含原生多窗口或自适应 backing 分辨率。

JSON 输入边界来自 `source/editor/assets/editorJsonAssetLibrary.h`：
4 MiB/文件、100000 源码行、深度/路径深度约束 64、4096 展示字段、
1024 字节展示文本截断预算（另加 `...`）、4096 字节 JSON Pointer、
20000 目录资产及 100000 扫描条目。超过展示字段上限仍校验剩余语法；
具体当前契约见组件文档，不将这些上限推广为引擎资产加载规则。

## 后续阶段

组件分类、已实现范围和依赖边界以
`documents/architecture/ui-component-system.md` 为当前契约。
首批共享控件由 `vl_ui_widgets` 拥有，业务面板不再自行定义通用 RML/DOM 刷新逻辑。
后续按 Text / Input / Layout / Views / Navigation / Workspace / Property
逐类发展，不一次性创建无消费者的 Slate 同名占位类。

1. 通用组件：Content Browser 业务网格/列表已受限窗口化，JSON inspector 已分页；
   通用树列表、Sources 树和模型树虚拟化仍待补齐；折叠属性分类与数值范围滑条
   已在 2026-10-11 第三批补齐（见下节验证），数值步进、可编辑调色板、
   搜索/筛选的进一步通用化仍在后续；每类由至少一个业务面板使用并补模块测试。
2. 工作区布局：本批完成可调分隔条、可停靠标签页、布局保存与恢复；
   原生多窗口仍是后续能力。
3. 独立视口：本批完成 renderer-owned 场景纹理、相机 aspect、拾取坐标、
   离线显示和既有 resize 生命周期；自适应 backing 分辨率仍是后续能力。
4. 场景文档：对象树覆盖灯光、相机、环境和模型；通过命令增删对象、
   编辑 Transform，并用 gizmo 驱动同一事务。
5. Undo/Redo：以文档命令为单位，覆盖多字段 batch、脏状态和预览恢复；
   不从 UI 回调直接修改 World。
6. 资产工作流：本批已完成五类 JSON 目录、Mesh/Texture/Terrain 资源标签和非 MI 只读查看；
   通用资产注册/导入、真实缩略图、模型几何预览、拖放、纹理像素查看器和
   明确色彩空间的调色板仍属后续能力。Scene 切换仍不等于场景文档编辑。

各阶段单独定义验收和模块测试，再进入下一阶段；不以更换控件库替代数据所有权设计。

### 尚未实现的能力清单（2026-10-11 核对）

以下能力在当前分支确认缺席，后续阶段直接以此为准，不要重新推导：

- 没有 gizmo、没有 transform 编辑，也没有场景对象的增/删/复制/改名；
  场景文档阶段（第 4 项）要连命令、Transform 编辑和 gizmo 一起做。
- 完全没有 Undo/Redo（第 5 项）；`Edit` 菜单里的 Undo/Redo 只是常驻禁用的占位。
- Scene Outliner 只列出带有效材质实例的 Mesh，不是真正的 World 大纲：
  没有灯光、相机、地形、环境，没有层级、可见性开关和改名。
- 没有资产缩略图、没有模型几何预览、没有纹理像素查看器。
- 没有原生多窗口，没有 IME。
- 场景仍按 swapchain 全尺寸渲染，UI 以完整 UV 采样；视口不是按停靠尺寸渲染。
- 材质实例的纹理覆盖只在保存 JSON 后生效，不做 live descriptor 替换。
- 视口工具条的变换工具与坐标系没有渲染后端；Wireframe 没有 wireframe 光栅模式，
  也没有对应的 debug view。

## 验证边界

`tool/editor-ui-tests/` 使用真实 RmlUi 文档和 CPU fake sink，
覆盖解析、快照刷新、revision、输入合并、保存/关闭以及离线 action。
`jsonAssetLibraryTests.cpp` 覆盖类型识别、嵌套 data 省略、输入与路径边界、
worker 代际、共享快照和空闲 Poll 不重读；业务测试覆盖只读路由与 64 行分页。
`contentBrowserPerformanceTests.cpp` 使用 10000/50000 项合成快照覆盖 192 条目加
最多两个 spacer、静止帧缓存、整行滚动与双击身份；50000 项压力输入不放宽
worker 的 20000 项目录上限。widget/docking 测试覆盖属性缓存及静止布局写入。
这些是已有测试覆盖范围，不在本次文档更新中宣称重新构建、执行或测得实际 FPS。
现有 MI 测试继续覆盖服务与预览协议。
这些不是 Vulkan live preview、IME 或桌面布局的证明；
主程序构建、窗口出图和交互检查另行记录，不能新增 runtime test 入口。

### 2026-10-10 初始迁移验证

- MinGW 根构建：`editor_ui_tests` 与 `main` 成功。
- 组件、工作区与现有 MI 服务/协议相关的 58 项 GoogleTest 全部通过。
- 正常启动：双线程编辑器显示、单线程编辑器隐藏均生成内置截图；
  UE 区域布局调整后再次验证双线程启动，未发现 RML/RCSS 解析或 Vulkan 错误。
- 1280×720 窗口截图确认右侧上下分区、底部标签栏、可读的模型/MI 名称和树展开符号；
  960×540 / 1920×1080 的区域边界由真实 RmlUi CPU 布局测试覆盖，不冒充窗口实测。
- 日志位于隔离引擎目录 `artifacts/rmlui-editor/ue-layout-20261010-172841.log`；
  截图位于隔离资源目录 `Generated/Screenshots/ue-layout-20261010-172841.bmp`。
- 原资源基线的 `SC_scene01` 场景选择器警告仍存在，未在 UI 迁移中修改。
- 启动检查后终止了本次创建的进程；没有验证正常关闭流程。
  桌面完整交互、IME、GPU live preview 和自由停靠仍需后续验收。

### 2026-10-10 停靠与独立视口验证

- MinGW 构建：`editor_ui_tests`、`editor_viewport_tests` 与 `main` 成功。
- 组件/业务、DockWorkspace、布局持久化、真实 RmlUi 拖动、视口坐标和现有 MI
  服务/协议相关的 109 项 GoogleTest 全部通过。
- 离屏显示输出契约另有 8 项测试通过：首个 LOAD 拒绝、最终 discard 拒绝、
  中间 discard/后续 LOAD 许可及无 swapchain 输出等；提取的纯校验不改变通用图编译语义。
- `vl_offline_adapter_tests`、`vl_offline_coordinator_tests` 构建成功；
  离线显示数据、相机尺寸策略、场景适配和会话协调的 114 项既有回归测试通过。
- 双线程编辑器启动的 1280×720 内置截图确认独立 Viewport 非空、完整场景内容可见，
  面板边界与默认 UE 区域组织正确；单线程关闭编辑器时仍正常全窗口显示。
  两次启动均未发现 RML/RCSS 或 Vulkan 错误，仅有既有 `SC_scene01` 选择器警告。
- 截图位于隔离资源目录 `Generated/Screenshots/dock-viewport-20261010-180232.bmp`
  和 `dock-game-20261010-180434.bmp`，日志位于隔离引擎目录 `artifacts/rmlui-editor/`。
- 已有 `--world-graph-transaction-test --exit-after-tests` 在双线程编辑器模式通过，
  退出码 0：World/Graph 故障回滚、同步提交、图单独提交、零尺寸/同尺寸 resize、
  shader/M_ 热重载交错、epoch 退休排空和 fatal post-swapchain 检查通过。
  此次正常 teardown 完成，stderr 为空；日志为
  `artifacts/rmlui-editor/dock-world-transaction-20261010-180518.log`。
- 双线程 `--offline --initial-scene Maps/SC_offline_shared/SC_offline_shared.json`
  启动，内置截图确认渐进结果在独立 Viewport 中非空，stderr 为空。
  日志为 `artifacts/rmlui-editor/dock-offline-shared-20261010-180931.log`，
  截图为隔离资源目录 `Generated/Screenshots/dock-offline-shared-20261010-180931.bmp`。
  此截图不证明最终 spp 收敛或导出；截图检查后终止了本次创建的进程。
  首次使用默认球阵列启动离线模式时，既有环境光校验按预期拒绝；
  没有为迁就验证改动场景资产或放宽离线支持边界。
- CPU 指针测试不冒充用户桌面验收；原生拖拽手感、实际窗口尺寸变化、IME、
  场景拾取与 MI GPU live preview 仍需交互验收。浮动仅限主窗口内。

### 2026-10-10 Content Browser 验证

- MinGW 构建 `main`、`editor_ui_tests` 和
  `material_instance_editor_production_tests` 成功。
- 浏览器/组件/停靠/颜色转换 99 项与 MI/视口/输出契约 58 项 GoogleTest
  全部通过，共 157 项。包含跨帧实际指针双击、完整初始目录超过 50 项、
  多列滚动网格、三档卡片尺寸、复选框文字布局与小窗口完整首行。
- 实际出图发现并修复 CPU 初版未覆盖的两项边界：Windows 文档 URL
  使用正斜杠解析相对图片；共享滚动条必须显式尺寸，否则滚动网格退成单列。
  添加对应布局断言，未修改 RmlUi 第三方源码。
- 1280×720 与 960×540 双线程内置截图确认 Sources 树、图标、多列卡片、
  筛选栏、计数和场景 Viewport 非空；960×540 的默认小卡片首行完整显示。
  UI sRGB 背景像素与 RCSS 一致（Outliner 为 45/45/49，工具栏为 40/40/44）。
- 截图位于隔离资源目录
  `Generated/Screenshots/content-browser-final-20261010-190445.bmp` 和
  `Generated/Screenshots/content-browser-small-20261010-185918.bmp`；
  同名日志位于隔离引擎目录 `artifacts/rmlui-editor/`。
  无 RmlUi/纹理加载错误，stderr 为空；既有 `SC_scene01` 场景选择器警告未处理。
  截图采集后终止本次进程，不作为正常 teardown 的证据。
- 窗口尺寸临时配置已恢复为 1280×720。原引擎工作区和原资源工作区未修改。
- 已有 `--world-graph-transaction-test --exit-after-tests` 在双线程编辑器模式
  串行执行通过，退出码 0，正常 teardown 完成，stderr 为空。
  日志为 `artifacts/rmlui-editor/content-world-transaction-20261010-190253.log`。
  回滚、World/Graph 同步提交、resize、shader/M_ 交错与 epoch 退休检查通过；
  没有添加 runtime test 标志或放宽事务契约。
- 只读子代理复核路径身份、历史/选择、事件分发、双击与重新 Attach，
  未发现重要行为问题。此静态复核不替代用户桌面操作验收。
- 当次验收只浏览 MI（当前基线已扩展至五类 JSON）；统一球形图片不是资产实际预览。
  原生多窗口、真实缩略图、
  通用资产注册/导入、桌面 IME 和 GPU live preview 仍不在本次验收范围。

### 2026-10-11 菜单栏、面板可见性与视口工具条验证

本批新增：顶部 File / Edit / Window / Help 菜单栏（含 Open Level 子菜单与 About 面板）、
Window 菜单驱动的面板隐藏/恢复与可见性持久化、Viewport 左上角工具条覆盖层。

- MinGW 构建：`cmake --build build --target editor_ui_tests editor_viewport_tests main -j8`
  三个目标全部成功。
- `ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure`
  共 229 项，全部通过；其中 4 项既有 `JsonAssetLibraryTest` 符号链接用例按原有条件跳过。
  新增覆盖：菜单展开/互斥/点击外部收起、菜单条目复用工具栏 action 并收起菜单、
  Edit→Undo/Redo 确实带 `disabled` 且点击不产生命令、File/Edit 可用性与
  `#save-asset` 同步、File→Open Level 对场景目录 fixture 只提交
  `UiActionType::LoadWorld` 且不带资产命令、Window 勾选与可见标签一致、
  About 打开时作为模态阻止命令、工具条每组恰好一个 active 且静止帧不重复写 class、
  Lit/Unlit 映射 debug view 0/1、变换工具与坐标系不产生任何命令、
  工具条命中不是场景命中；以及隐藏面板后标签与正文消失、组内激活移交、
  整组隐藏时兄弟子树接管整块矩形、恢复后几何回到原值、拒绝隐藏最后一个可见面板、
  未知面板名拒绝、含 `"hidden"` 与旧格式（无该键）的持久化往返。
- 出图：从仓库根运行
  `main.exe --dev-ui --console-command "screenshot round1.bmp" --console-command-delay 120`，
  stderr 为空；stdout 无 RmlUi/RCSS 解析错误，只有既有的
  `SC_scene01` 场景选择器警告。截图为隔离资源目录
  `Generated/Screenshots/round1.bmp`（1280×720，2764854 字节），
  日志为隔离引擎目录
  `artifacts/rmlui-editor/menubar-visibility-viewport-20261011-030055.log`
  与同名 `.stderr.log`（0 字节）。
- 截图像素抽样确认静态默认状态：顶部标题行最左出现四簇菜单标题文字
  （与改动前同区域的连续“标题+按钮”完全不同）；视口左上角出现三组工具条，
  每组恰好一个激活底色 `#2a665b`（Select / World / Lit），
  分组底色 `#242428` 不透明。File 弹层的遮挡关系用一次临时的
  `#menu-file-popup { display: block; }` 截图确认弹层画在停靠内容之上，
  该临时规则已还原；这两项都不等于人工桌面操作验收。
- 该次进程由 `Start-Process` 启动、截图落盘后强制终止，**不是**正常关闭流程的证据。
- 本次没有验证：菜单的真实鼠标悬停展开与点击手感、About 面板与 Window 勾选的观感、
  隐藏/显示面板后的实际桌面布局、任何原生窗口行为。CPU 测试用真实 RmlUi 事件与
  指针坐标驱动，但仍是合成输入。
- 发现一个既有问题（非本批引入）：UI overlay 不合成半透明背景。
  临时把 `.viewport-tool-group` 改成 `rgba(255,0,255,0.85)` 后截图像素仍是场景值；
  把 `#editor-status` 改成 `rgba(0,255,255,0.85)` 只是把原底色变暗，没有变青。
  `#close-dialog`、`#dock-preview` 的 rgba 背板受同一限制影响，尚未定位根因。
  因此视口工具条底色改用不透明 `#242428`；该问题没有在本批修改渲染路径。
- Wireframe 按钮保持禁用：仓库没有 wireframe 光栅模式，`debug.*` 表中也没有
  对应的 debug view，没有拿其他调试视图顶替。变换工具与坐标系只切换高亮，
  仍在文档与代码注释中标注为无渲染后端的呈现状态。

### 2026-10-11 Details 面板恢复迁移能力验证

ImGui→RmlUi 迁移丢了 Details 面板的三项能力，本批把它们补回（组件契约见
`documents/architecture/ui-component-system.md` 的“本次恢复的迁移能力”）：
数值范围滑条（含 `ResolveSliderRange` 的通道 > 参数 > 按默认值推导的兜底语义）、
Color 参数的 sRGB 色板与 `#RRGGBBAA` 读数、General / Render States / Parameters /
Texture Bindings 四个可折叠分组（默认全展开）。
`ImGui::ColorPicker4` 等价的可编辑调色板仍缺席：RmlUi 没有 `<canvas>`，
SDL3 的 `SDL_dialog.h` 没有颜色对话框 API，本轮不做，也没有用别的控件假装。

- MinGW 构建：`cmake --build build --target editor_ui_tests editor_viewport_tests main -j8`
  三个目标全部成功。
- `ctest --test-dir build -R "^(UiColorTransform|UiWidgetTest|UiWidgetRefresh|EditorUiTest|ContentBrowserTest|DockWorkspaceTest|DockPersistenceTest|DockRmlTest|EditorViewportGeometry|EditorJsonAssetParsing|EditorJsonAssetPaths|EditorJsonAssetTypes|JsonAssetLibraryTest|ResourceEditorController|LargeCatalog/ContentBrowserPerformanceTest)\." --output-on-failure`
  共 245 项，全部通过；其中 4 项既有 `JsonAssetLibraryTest` 符号链接用例按原有条件跳过。
  相对上一批的 229 项新增 16 项：9 项在 `widgetTests.cpp`
  （滑条与文本输入共用地址、退化区间不写 `step`、重复快照推送不回写 `value`、
  `MakeColorReadout` 的 0/1/中灰/alpha、折叠分组只改 class 且保留节点身份与焦点、
  `UiLinearToSrgb`/`UiSrgbHexByte` 的曲线与 HDR 边界），
  7 项在 `editorUiTests.cpp`（三层范围解析、旧 M_ 兜底不跟随编辑值、
  非有限/反向区间仍有限有序、拖动与键入同一种 batch item、拖动不触发有限数校验、
  只有 Color 有 sRGB 色板与读数、折叠跨 Build/文档切换保留且静止帧写 0 次属性）。
- 出图：从仓库根按既有方式启动
  `main.exe --dev-ui --console-command "screenshot round3.bmp" --console-command-delay 120`
  （`Start-Process` 的 `-ArgumentList` 数组不会自动加引号，命令串必须自己写成
  `'"screenshot round3.bmp"'`，否则会被当成两个启动参数），
  stderr 为空（0 字节）；stdout 无 RmlUi/RCSS 解析错误，只有既有的
  `UI scene selector skipped an unsupported scene asset: ...SC_scene01.json` 警告。
  截图为 `Generated/Screenshots/round3.bmp`（1280×720，2764854 字节）。
- 该次进程由 `Start-Process` 启动、截图落盘后 `Stop-Process -Force` 强制终止，
  **不是**正常关闭流程的证据。
- Details 面板的像素测量由 `artifacts/rmlui-editor/editor_details_probe.py` 驱动真实窗口完成
  （该脚本与 `bmp_probe.py` 都在 gitignored 的 `artifacts/` 下）：
  先用 Content Browser 双击 MI 卡片打开文档，再在面板内滚轮滚到参数区；
  由于视口点击会把指针交给游戏、之后的 UI 鼠标事件全部失效，
  最后一步才用视口拾取换成球阵材质实例。测量结果（`round3-details.bmp`）：
  `py -3 artifacts/rmlui-editor/bmp_probe.py round3-details.bmp 896,512 918,512 941,512 950,430`
  给出色板 `rgb(188,188,188)`，即线性 `u_tintColor = [0.5, 0.5, 0.5, 1.0]`
  （`MI_sphere_array_x04_y06.json`，由日志
  `Selected Material Instance 'Maps/SC_sphere_array/Materials/MI_sphere_array_x04_y06.json'`
  确定是哪一个实例）的 sRGB 结果；色板是 46×16 的实心矩形，
  与 `.color-swatch` 尺寸一致，按线性直出会得到 128 而不是 188。
  同一帧里 `#c8c8cf` 滑条滑块出现 5 处共 600 像素，
  滑块所在行的 `#2a665b` 进度条、`#49494f` 轨道边框与 `#303034` 空轨依次相接，
  其中 `u_alphaClipThreshold = 0.5`（声明范围 0..1）的进度约占轨道一半。
- 本次没有验证：拖动滑条的真实鼠标手感与键盘操作、色板在 alpha < 1 时的观感
  （色板按不透明 `rgb()` 画，alpha 只在读数里）、滚动条拖动、
  以及折叠状态在重启后是否保留（它是会话状态，本来就不持久化）。
  折叠与滑条的行为断言由真实 RmlUi DOM 测试覆盖，但仍是合成输入。
