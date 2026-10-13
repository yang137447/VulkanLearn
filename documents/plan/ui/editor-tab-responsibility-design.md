# 编辑器标签职责设计（对 UE 的引用与差距）

> 状态：**提案**（尚未实现）。依据是 Epic 官方文档 + 本仓库当前实现的实测结果。
> 本文回答两个问题："UE 里每个标签承载什么职责"，以及"本仓库偏离在哪"。

## UE 的模型（有官方文档依据）

### 1. 一个窗口里只有一套标签，Level Editor 自己就是一个标签

UE 编辑器主窗口只有一个标签栏。Level Editor（关卡编辑器）是其中一个标签；每个**资产编辑器**
（Static Mesh Editor、Material Editor、Material Instance Editor …）打开后也是同一栏里的标签。
Epic 把 Level Editor 描述成"引擎第一次打开时进入的那个界面"，而资产编辑器的打开方式是
"在 Content Browser 双击资产"。两者是**同级**的标签，不是"主界面 + 弹层"。

### 2. 每个标签是自足的编辑器，自带工具条 / 视口 / 细节

以 Material Instance Editor 为例，Epic 列出的区域是：
工具条（保存、在 Content Browser 中定位、显示隐藏参数、继承层级、平台统计）、
视口（实时预览材质球）、视口显示选项、以及 **Details Panel**。

关键点：**这个 Details 是 Material Instance Editor 自己的 Details**，装的是"所有暴露的材质参数
与属性"。它不是 Level Editor 里那个 Details。

### 3. Details = 当前选择，而且"当前"是**所在编辑器上下文**里的当前

Epic 对 Level Editor 的 Details 定义：

> contains information, utilities, and functions specific to **the current selection** in the
> viewport … displays all of the editable properties for the selected Actors

并且明确写了材质怎么处理：

> also allows you to view the Materials used by the selected Actors, and **quickly open them
> for editing**

也就是说：**Level Editor 的 Details 只描述"选中的 actor"，材质是从这里"打开"出去编辑的**，
材质参数不在 Level Editor 的 Details 里。

两条合起来就是 UE 的规则：

> **Details 的职责是"描述当前上下文里的当前选择"。它不承载标签，也不承载文档列表。**

### 4. 标签粒度 = 资产，重复打开聚焦而不是新建

"双击资产 → 打开该资产的编辑器标签"、"从层级菜单选父材质 → 在**新标签**里打开那个资产"。
所以标签的身份是**资产**（一个资产一个标签），不是"每个被点过的对象各一个标签"。

### 5. 点 actor 不产生标签

Level Editor 里点选 actor 只改 Details 的内容与视口描边。**只有双击资产才开标签。**
"点不同模型就出现独立 tab"在 UE 里并不成立 —— 成立的是"双击不同**资产**出现独立标签"。

## 本仓库当前实现（实测）

三套彼此独立的标签机制：

| 机制 | 位置 | 当前承载 | 来源 |
| --- | --- | --- | --- |
| `workspace-*-tab` | 各停靠面板标题栏 | 面板身份（Viewport / Outliner / Details / Content Browser / Renderer） | `DockWorkspace` 布局树 |
| `#document-tabs` | **Details 面板内部**、`Material Slots` 之下 | 打开的 MI 文档 | `snapshot.documentTabs` |
| `#workspace-resource-tab` | Asset Editor 停靠面板 | 打开的只读资源（Mesh / Texture / Scene / Terrain） | `resourceEditor.Tabs()` |

实测补充：`#document-tabs` 在未打开任何文档时是空的（视觉读数：Details 面板只有
`Material Slots`、`Material Instance` 两个标题与两个按钮，其余空白）；单击 Content Browser
磁贴只选中、Details 区**零像素变化**。

## 差距：为什么会出现"奇怪的子标签"

偏离 UE 的地方是**职责叠加**，共三处：

### 差距 1：MI 编辑器不是一个标签，而是被折进 Details 里

UE 里 MI 编辑器是独立标签，自带工具条 + 预览视口 + 自己的 Details。
本仓库把"MI 文档编辑"塞进了右侧 Details 面板内部，于是那个面板同时要表达：

1. 我是 `Details` 这个面板（停靠标签）
2. 我在描述当前选中的 actor（`Material Slots`）
3. 我还开着若干个 MI 文档（`#document-tabs`）

→ 打开多个 MI 后，那排标签的**从属关系无法阅读**：它们是 Details 的子标签？
是 Material Slots 的子标签？还是并排的独立文档？这正是"开了很多奇怪的子 tab"。

### 差距 2：Level Editor 与资产编辑器没有分开

UE 的层级是：`Level Editor 标签` 与 `资产编辑器标签` 并列。
本仓库里 Viewport / Outliner / Details 是常驻停靠面板，而"资源文档"另有一个 Asset Editor 面板
（`#workspace-resource-tab`），两套的标签语义不同却并排出现。

### 差距 3：`Details` 这个名字被两件事共用

- UE：Level Editor 的 Details（actor 属性）与各资产编辑器的 Details（资产属性）是**同名但不同实例**，
  各自跟随自己上下文的当前选择。
- 本仓库：只有一个 Details 面板，既当 actor 检视器又当资产参数编辑器。

## 建议的目标结构

一句话：**把"资产编辑器"做成真正的标签，把 Details 收窄成"描述当前上下文的选择"。**

| 层级 | 承载什么 | 数量 | 打开方式 |
| --- | --- | --- | --- |
| **Level Editor 标签** | 关卡视口 + Outliner + 它自己的 Details | 1 | 常驻（可选多关卡） |
| **资产编辑器标签** | 一个资产：工具条 + 预览视口 + 它自己的 Details | 每个打开的资产一个 | Content Browser 双击 / 右键 Edit |
| **面板标签** | 可停靠工具面板（Content Browser、Output Log …） | 固定若干 | Window 菜单 |
| **选择** | 不是标签。只改**当前标签内** Details 的内容 | 每上下文 1 | 点视口 / 点 Outliner |

落到本仓库，具体是三件事：

1. **MI 参数不再住在主 Details 里。** 双击 MI → 开一个 MI 编辑器标签（自带工具条 + 预览
   视口 + 参数 Details）。主 Details 回到"只描述选中的 actor"。
2. **`#document-tabs` 删除或改造。** 它承载的"打开的 MI 文档"由**资产编辑器标签**天然表达，
   不需要在面板内部再排一层。若保留，它应当变成"资产编辑器标签栏"本身，而不是 Details 的子控件。
3. **Mesh / Texture / Scene / Terrain 的只读资源页与 MI 编辑器合并成同一套资产标签。**
   现在它们是两套机制（`resourceEditor.Tabs()` 与 `documentTabs`），UE 里资产编辑器就是资产编辑器，
   只是工具条与内容随类型变。

## 需要你拍板

1. **MI 编辑器要不要独立成标签**（含自己的预览视口）？这是本设计的分水岭：不做，`#document-tabs`
   就没地方放；做了，Details 的职责才收得回来。
2. **是否接受"点 actor 不开标签"**（UE 行为）？".点不同模型出现独立 tab"若指 actor，UE 不这么做；
   若指**双击模型资产**，那与建议 1、3 一致。
3. **资产标签放哪**：中央文档区（UE），还是继续用现有的 Asset Editor 停靠面板？

## 与仓库纪律的关系

本文件是**提案**，不是契约：按 `documents/README.md` 的分层，它属于 `plan/`，
不得包含对未来行为的承诺式断言。定稿并实现后，职责应当写进
`documents/contracts/ui-component-system.md`，未实现的能力留在 `documents/plan/ui/ue-parity-audit.md`。

## 依据

- Epic，《Level Editor Details Panel》：
  https://dev.epicgames.com/documentation/en-us/unreal-engine/level-editor-details-panel-in-unreal-engine
- Epic，《Material Instance Editor UI》：
  https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-material-instance-editor-ui
- Epic，《Unreal Engine Interface and Navigation》：
  https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-interface-and-navigation
