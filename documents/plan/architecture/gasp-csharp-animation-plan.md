# GASP 与 C# 动画控制开发计划

状态：当前开发目标的规划，2026-10-18；不是实现契约。
本计划取代 [ALS 计划](als-csharp-animation-plan.md) 的后续实施安排。
骨骼动画、Motion Matching、调试面板及翻越均尚未在本项目实施，不能把阶段目标当作现状。

## 1. 目标与决定

目标改为以 Epic 的 Game Animation Sample（GASP）为参考，在 VulkanLearn 建立
胶囊角色驱动的 Motion Matching 动画系统，并逐步交付移动和障碍翻越体验。
不先完成 ALS 专用动画状态机，再改造为 Motion Matching。

- **C# 完成高层状态机**：Grounded/InAir/Traversal、步态、朝向模式、
  游戏条件、候选数据库筛选、动作进入/退出、优先级和中断策略。
- **C++ 完成姿态执行**：轨迹预测、Pose History、特征提取、数据库构建与搜索、
  播放连续性、姿态混合、根骨修正、IK、冻结快照与 GPU 蒙皮。
- C# 不逐帧指定 Motion Matching 要选的 clip/frame；C++ 不另建一套业务状态机。
- 预览和可视化调试随运行时逐步交付；完整节点式动画蓝图编辑器后续单独安排。
- 首先固定一套骨架和单机单角色；不同时承诺所有角色、所有版本和全部示例功能。
- 每阶段交付验收入口、预期画面/交互、测试记录和未完成项，不以代码编译代替效果验收。

GASP 官方介绍使用 Motion Matching、Pose History 和按游戏条件筛选数据库的
Chooser，并展示移动与障碍交互；具体项目会随引擎版本变化。
这些是参考架构，不意味着 VulkanLearn 已有对应插件。
参考入口见第 10 节，具体资产和行为以 P0 锁定的实际项目为准。

## 2. 调研现状与 P0 输入

截至本计划建立，已做本地只读调查，未开始动画源码实现：

| 部分 | 已确认基础 | 需要补齐 |
| --- | --- | --- |
| C# | Blueprint、逻辑组件、输入、Tick/LateTick、Play/Pause/Stop、程序集换代 | 动画控制、数据库筛选输出、播放反馈与动画迁移 |
| Jolt | 胶囊角色、滑墙、台阶、贴地、跳跃 | 实际运动回传、蹲伏形状切换、翻越受控位移 |
| 相机 | 资源侧第三人称示例、视角与扫球避障 | 与移动模型和转向模式协同 |
| 导入 | Assimp 几何、材质槽、UV、MikkTSpace | 骨架、inverse bind、权重、动画和侧车元数据 |
| 渲染 | 静态 MeshDrawSnapshot、材质/阴影路径 | 姿态、palette、动态 bounds 与渲染历史 |
| 编辑器 | RmlUi、资产预览、冻结值快照和命令边界 | 动画、轨迹、特征与搜索调试 |
| 本地参考 | 已找到 ALS V4 项目；已核对一套本地 UE 编辑器版本为 5.8.2 | **尚未定位或审计 GASP 项目**，不能用 ALS 资产视为 GASP 输入已齐备 |
| 动画库候选 | 已查询 ozz-animation 0.16.0 的 tag/commit | 尚未下载构建或验证真实片段，不算依赖验证通过 |

引擎源码证据：
[导入器](../../../source/mesh/loader/mesh/assimpSourceAdapter.cpp)、
[顶点](../../../source/VertexDataStruct.h)、
[Jolt](../../../source/script/scriptPhysics.cpp)、
[C# 角色接口](../../../script/VulkanLearn.Runtime/GameplayServices.cs)、
[脚本调度](../../../source/script/scriptRuntime.cpp)、
[WorldSnapshot](../../../source/world/worldSnapshot.h)。

现有第三人称游戏代码位于资源根
`Maps/SC_actor_gameplay/Blueprints/Character.blueprint.cs`；
GASP 游戏控制代码也应归资源仓库，不进入引擎 Gameplay 示例目录。
当前行为以源码及
[C# 契约](../../contracts/csharp-blueprint-runtime.md)、
[资产组织](../../contracts/asset-organization.md)、
[UI 边界](../../contracts/game-ui-stack.md)、
[控件边界](../../contracts/ui-component-system.md) 为准。

## 3. 交付范围与阶段验收

### 3.1 两个里程碑

**M1 核心移动：P0～P7。** 固定骨架，待机/走跑与资产支持的步态，
起步、停止、方向变化/急转，跳跃/落地；包含真实 Motion Matching、
足部质量、调试、生命周期与独立包验收。
蹲伏列入 P0 能力矩阵，有相应片段时实现含碰撞的完整路径；
若锁定版本不含该能力，明确标为扩展，不借 ALS 内容冒充 GASP 功能。

**M2 障碍交互：P8～P9。** 在 M1 上实现固定规格障碍的翻越/攀上/落下子集，
具体动作在 P0 按锁定版本列出。M1 通过不等于 GASP 总目标完成；
M2 验收后才声明本计划约定的移动与翻越范围完成。

节点编辑器、联网预测与同步、任意骨架重定向、全套角色切换、布娃娃、
复杂武器叠加、多角色大规模优化不进入本轮承诺。
动作事件提供有界值事件与显示；不顺带承诺音频或粒子系统。

### 3.2 阶段效果

所有阶段起始状态均为待实施；P0 已有的只读线索不构成阶段完成。

| 阶段 | 实现内容 | 用户可验收的效果 | 验证门槛 |
| --- | --- | --- | --- |
| P0 参考与资产基线 | 锁定 GASP/UE 版本、行为矩阵、角色、片段/曲线/事件、依赖 | 可查看固定版本的 UE 对照片段与资产清单；最小网格/动画成功导出 | 来源与使用范围核对；摘要、坐标、骨架映射、MinGW 依赖验证；缺失项明确 |
| P1 动画资产 | 骨架、绑定、权重、clip、根轨道与元数据导入 | 可查看骨架层级、片段长度/采样信息及 UE/VL 参考帧对照报告 | 绑定姿态、缺轨、权重和顶点重排数值回归通过；坏资产拒绝 |
| P2 蒙皮出图 | palette、快照、主画面/阴影、bounds、姿态历史 | VL 中真实角色循环播放一段动画，阴影随身体变化；暂停可核对姿势 | CPU 对照、单/双线程、静态模型与速度输出回归 |
| P3 C# 控制闭环 | 高层状态机、参数/请求/反馈、角色移动与基础播放器 | 操作角色待机、走跑、跳跃；查看 C# 状态，Play/Pause/Stop 一致 | 真实 CLR/Jolt、优先级/中断、回调隔离与时间语义通过 |
| P4 轨迹与特征 | 运动预测、Pose History、schema、特征数据库构建 | 视口显示过去/未来轨迹；浏览动画帧与对应特征；固定输入可重建同一数据库 | 采样空间/时间、边界样本、归一化、身份/缓存失效与数据库校验通过 |
| P5 Motion Matching | 条件筛选、连续播放候选、精确搜索与切帧混合 | 同一角色根据输入自动选择起步、停止和转向帧；显示数据库、候选与代价 | 独立小型手算样本/穷举基准、筛选、无候选、重选与确定性通过 |
| P6 动画质量 | 连续性改善、根骨偏移/朝向修正、步幅处理与足部 IK | 低速、急停、反向、斜坡和台阶上减少滑步、跳变及穿地；各修正可对照开关 | 各空间约定、偏移界限、接地与跳跃解锁、传送重置及实际画面对照 |
| P7 调试与 M1 验收 | 完整 RmlUi 面板、时间轴、查询回放、生命周期/导出/性能 | 可暂停/拖时间轴，查看状态、权重、轨迹和分项代价；独立包运行核心移动 | 固定动作矩阵、UI 布局、程序集/World 换代、性能分项与正常退出 |
| P8 障碍交互 | 探测、C# 动作状态、动作/入口帧选择、对齐与受控物理移动 | 指定障碍可翻越或攀上；高度/净空不符时拒绝；中断和落地可观察 | 固定障碍矩阵、目标失效、碰撞恢复与根位移只消费一次 |
| P9 M2 与最终交付 | 动作质量、完整依赖闭包、集成回归和证据 | 同一正式场景可连续移动、翻越、落地；导出包可复现 | 完整动作矩阵、失败保留、GPU 生命周期、文档/构建/测试与实窗记录 |

P1 的报告是数据验收，P2 才开始承诺 VL 动态渲染；P3 的手动播放器用于打通链路，
不能标为 Motion Matching 已完成。P4 起必须同步提供轨迹/特征检查，
P5 起必须有查询与候选代价显示，不能把全部可观察性推迟到 P7。

### 3.3 每阶段的验收包

每阶段交付记录至少包含：

1. 正式资产/场景相对路径、启动命令或报告入口、构建配置与参考 revision。
2. 可重复的操作步骤，以及画面或数据的具体预期。
3. 数值测试、截图/动作片段、日志位置，明确通过、失败、跳过和未验收。
4. 本阶段相对上阶段的效果、已知差异和下一阶段的依赖。

场景拟为 `Maps/SC_gasp_locomotion/SC_gasp_locomotion.json`，
翻越阶段可在同内容域增加固定障碍验证场景，不强制复制 Epic 全部示例关卡。
中文 description 明确学习与工程回归用途，不声称是论文验证。
同一场景分别测试 30/60/120 FPS、停止、反向、撞墙、起跳落地和传送；
高帧率/低帧率的差异需实测，不用平均 FPS 推断动作稳定性。
视觉阈值在 P0/P3 选定骨架与移动模型后记录，例如脚底滑移距离、脚部穿透、
切换角度突变、输入到动作响应时间；先保留基线，再比较修正前后。

## 4. C# 与原生的所有权

### 4.1 C# 决策，原生搜索

C# 根据输入、实际物理结果和游戏条件维护高层状态，并输出允许搜索的
数据库集合、步态/朝向意图、动作锁与中断策略。
这承担 GASP 中 Chooser 的业务筛选职责；首版用类型化 C# 代码与参数描述实现，
不先复制一个通用 Chooser 表编辑器。

原生搜索只在允许集合中评估姿态；不得在找不到候选时自动扩大到业务禁止的库。
P0/P3 固定“无候选”的明确回退片段/保持规则与诊断，任何回退也必须符合当前状态。
步态、数据库和播放实例用稳定 ID；不使用 CLR 对象、委托或枚举序号作为持久身份。
状态更新按明确优先级与同优先级顺序处理，避免同一次更新无界连跳。

初始化注册可复用姿态组合，逐帧仅传意图、参数与有身份的请求；
不经现有 JSON 桥接逐骨传矩阵，不逐帧重建姿态图。
请求入口检查上下文/World/实例代际、有限数值、有效 ID 与容量，
回调失败不得发布半批参数；不承诺回滚任意用户 C# 字段。

原生唯一维护播放时间、继续候选、过渡进度与姿态历史；
C# 接收只读结果，不维护第二个 clip 时钟。
事件按播放实例和时间区间有界交付，循环、切帧、seek、暂停与中断不得重复触发游戏事件。

### 4.2 求值与物理顺序

M1 延续胶囊驱动移动，保留现有调度：

1. C# Tick 采集移动/视角意图，随后 `StepPhysics(dt)` 推进角色（按帧 dt）与刚体（内部 1/60 子步）。
2. 回传实际位姿、速度和着地状态，交付接触/查询回调。
3. C# LateTick 更新高层动画状态和数据库筛选；动画在 LateTick 后求值，帧末执行 `StepPhysics(0)` 同步（只同步指令）。
4. 冻结本帧控制、物理和历史输入；原生预测轨迹，构建查询、搜索、采样与混合。
5. 在明确物理阶段执行足部探测/修正，冻结姿态与调试数据。
6. World/UI 快照发布，RT 只消费值，不访问 CLR、活动 World 或 Jolt。

预测用与实际角色一致的加减速、制动、朝向和速度规则；不能只延长一条输入方向射线。
历史来自实际碰撞后的运动；碰墙/斜坡上的预测偏差需显示，首版的碰撞预测近似
必须在 P4 写明，不能把未来轨迹视为保证实现的物理路径。
历史按明确时间点重采样，不能按“过去 N 个渲染帧”编码时间。
LateTick 发出的现有异步查询不被当作当帧已完成查询。
动画 CPU 求值首版在 GT；移到 worker 前先冻结完整输入并测量收益。

M1 从源动画保留根轨道以构建轨迹和检查位移，但角色位置由 Jolt 决定，
姿态输出按约定去除已由胶囊承担的位移；不把原片段根位移再次施加到胶囊。
根骨旋转与视觉偏移另有归属及界限，不能让角色根、相机、动画各旋转一次。

M2 的动作移动需要扩展固定步路径：冻结动作请求 → 提取该步动作位移 →
Jolt 受控碰撞/动作求解 → 回传实际位移 → 发布对应姿态。
与普通 Move 互斥，事件和根位移均按实际时间推进一次。
不得在 LateTick 直接改根位置绕开碰撞；翻越时使用何种碰撞模式、
中断/失败怎样恢复在 P8 明确验证，不由动画视觉对齐隐式决定。

Pause 和 TimeScale=0 不推进播放、搜索切换、轨迹历史或动作事件。
编辑器显式预览 seek 使用独立时钟，不推进关卡游戏。
首帧、传送、Stop、World 换代和骨架不兼容变化重置适用的历史与脚锁。

## 5. Motion Matching 数据与算法

### 5.1 导出与特征的一致性

P0 对固定版本逐项记录骨架、clip、根轨道、采样率、曲线/事件、数据库成员、
schema、同步/接触标记和所用修正节点；不能仅导出 FBX 后认为所有 UE 数据仍在。
不能随意将根位移烘焙删除，也不能将 UE 缓存数据库二进制直接当 VL 兼容格式。
业务筛选和运行时支持范围逐项形成对照矩阵，未迁移项明确标记。

骨架、顶点、inverse bind、clip、root 和轨迹共同转换到已定义的 VL 坐标/单位。
特征包括 P0 选定的根局部空间骨骼位置/速度，以及指定过去/未来时刻的轨迹位置/
方向等；具体通道、时间偏移、权重和采样阶段由带版本的 schema 决定。
Pose History 首版采用混合后、接地 IK 前的约定姿态，
避免足部贴地修正反馈成查询目标；数据库与运行时按同一通道定义生成特征。
边界帧明确循环延拓或排除规则，不从下一条无关动画取未来样本。

归一化统计随数据库冻结，构建端和查询端共用；
常量通道使用明确的零方差规则，筛选多个数据库时保证 schema/统计兼容。
镜像不在首版默认开启；需要时共同验证骨骼映射、根轨迹和方向语义，
不能只翻一个速度分量。

### 5.2 构建身份与发布

数据库源描述引用正式 clip/schema；派生特征缓存进入 Generated。
身份使用仓库稳定 BLAKE3-256 包装，覆盖源字节、骨架映射、导入/坐标策略、
schema、归一化策略、算法版本与数据库成员顺序。
坏缓存、版本变化和源变化必须驱动正确重建，禁止依赖时间戳或手改缓存修复。
构建时完成索引、数值和预算校验；候选完整成功后再发布。
数据库与其 clip/统计作为同一资源版本保持存活，搜索时不能混用新旧数据。

### 5.3 搜索与连续性

先建立小规模、确定性的加权距离精确搜索基线；同成本候选按稳定规则选取。
若选定范围的实测性能要求索引加速，再评估成熟索引库，并与基线比较；
不以先写近似搜索换取“看起来选到了动作”。
采样/混合优先验证成熟库，ozz-animation 为候选，不假定它自带完整 GASP 搜索系统。

继续播放帧也作为候选，定义搜索频率、继续偏置、最小改善、重选窗口、
跳转时间限制与过渡策略；这些数值执行由原生维护，动作可否中断由 C# 决定。
数据库切换后重新验证继续候选资格，不能借继续播放绕过高层筛选。
P5 首先使用明确的交叉混合；P6 依据锁定 GASP 版本与观感缺口评估惯性化、
根偏移、朝向/步幅修正，各自记录与原版的差异。
不承诺以少量片段重现完整动作库，也不把传统速度 BlendSpace 演示标为 Motion Matching。

调试快照包含查询版本/帧号、schema、数据库集合、选中 clip/time、
继续候选、前几名候选的轨迹/姿态分项与策略代价、重选/保持原因。
查询/结果可在相同数据库版本上回放；展示混合后的姿态和最终修正姿态的差别。
回放观察不回放物理或游戏事件。

## 6. 渲染、预览与生命周期

独立蒙皮输入流/变体优先，静态网格不永久支付骨骼输入成本。
MikkTSpace 拆分和重排顶点时同步权重映射，骨架不能只收集有权重的骨骼而丢失父链。
主画面/ShadowDepth 共享姿态、蒙皮与法线/切线约定。
动态 bounds 覆盖变形；速度输出比较当前姿态和上一次实际提交渲染的姿态，
不能因单最新快照丢帧而把上一 GT 帧当上一渲染帧。
palette、buffer、descriptor 和资源引用遵守现有 GPU epoch 退休。

预览只执行原生片段/查询求值，不启动 Blueprint 或关卡物理；
运行时检查读取选中角色的冻结值，调参经带代际的值命令提交。
预览 seek/逐采样帧与游戏暂停分开，临时调参不自动保存作者文件；
UI 刷新不产生作者修改，换目标/关闭会话清理覆写。
面板遵守现有 RmlUi 控件、输入仲裁和 producer-only 边界。

Spawn/Destroy、组件移除重建、owner 故障/禁用、Stop 与 World 换代清理
播放实例、查询历史、回调、脚锁和资源租约。
C# 仅显式迁移稳定状态值；原生不保留 CLR 引用。
程序集候选准备时核对数据库/骨架和状态 ID 兼容性，
成功一起交换，失败保留活动程序集和动画；不能在受限 OnReload 中偷偷发播放指令。
不支持的播放中迁移应明确拒绝，不静默重置并声称保持状态。

正式导出包含动画、schema、数据库源描述、clip 和所有必要 sidecar；
派生缓存可随包携带或按明确路径重建，但包不能依赖开发机的 Generated 绝对地址。
M1 与 M2 均实际启动独立包验收，不能只验证导出命令退出码。

## 7. 模块与资源落点

以下目录为拟议方案，具体类型/文件格式在实施时与 loader/schema 一起确定：

| 内容 | 拟议归属 |
| --- | --- |
| 动画数据、采样、混合、轨迹/历史、特征构建和搜索 | 新增 `source/animation/`，按子模块分工 |
| 状态机通用支撑与参数/请求/反馈 API | `script/VulkanLearn.Runtime/` |
| GASP 高层控制器、数据库筛选与游戏配置 | 资源内容域 `Blueprints/` |
| 骨架与蒙皮导入 | 现有 `source/mesh/loader/` 加载链路及动画模块 |
| GPU palette 与姿态快照消费 | 现有 renderer/resource、WorldSnapshot 与 mesh pass |
| 构建工具与导出审计 | 拟新增 `tool/animation/`，不另建游戏 runtime test 入口 |
| 数值与查询回归 | 拟新增 `tool/animation-tests/`，根 GTest helper 注册 |
| CLR、Jolt、UI、渲染与导出集成 | 扩展各自现有模块套件 |

首套资产归 `Maps/SC_gasp_locomotion/`，拟在其中组织
Skeletons/Animations/MotionMatching/Source；跨地图复用后才提升到 Common。
正式资产引用不能指向 Generated；缓存/审计按资源根
`Generated/Import/`，截图按 `Generated/Screenshots/`，测试日志按引擎 gitignored `artifacts/`。

现有 Scene/Mesh/Texture 修改遵守
[标准资产接口](../../contracts/asset-modify-api.md)；
新格式和能力需同步 loader/schema/capability，不能假定现有命令已经支持。
动画模块落地时新增真实源码路由与对应当前契约；本计划不代替它们。

## 8. 验证与工期

| 验证对象 | 必需证据 |
| --- | --- |
| 资产与数学 | 绑定姿态、权重/顶点重排、层级/坐标、根轨道、缺轨、曲线/事件边界 GTest |
| 轨迹与历史 | 固定时间采样、不同帧率、碰撞反馈、传送/暂停重置、预测与实际运动对照 |
| 数据库 | 特征通道/空间一致、零方差、边界帧、坏数据/版本拒绝、输入摘要驱动失效 |
| 搜索 | 手算小样本、精确最优候选、稳定 tie-break、过滤/空集合、继续偏置与重选窗口 |
| C# | 真实 CLR 高层状态、动作优先级/中断、筛选请求、事件隔离与迁移失败保留 |
| 物理 | 实际速度、碰墙、跳跃/落地；适用时蹲伏顶棚/脚底保持；翻越位移及碰撞恢复 |
| 渲染与 UI | 主画面/阴影、速度/bounds、单/双线程；冻结快照、时间轴与查询回放无游戏副作用 |
| 收尾 | 固定动作/障碍矩阵、生命周期、独立包、分项性能、正常退出和持久证据 |

遵守 [测试治理](../../contracts/coding-guidelines.md#test-governance)
与 [验证映射](../../contracts/testing.md)。
纯逻辑/模块测试用 `vulkanlearn_add_gtest(...)`，真实 CLR 由现有 GTest 宿主驱动；
不加 test-only main，不新增 `--animation-test` 或 `--motion-matching-test`。
生命周期/并发回归至少两遍；三个获准 Vulkan runtime test 按变更映射串行执行。
模块通过、实窗出图和正常退出分开记录。

性能记录输入/脚本、桥接、轨迹、查询构建、搜索、采样、混合、IK、
快照和 GPU 上传成本，明确数据库帧数/维度、硬件与优化构建配置。
先测单角色，再测固定数据库的多实例；不把窗口 FPS 当搜索耗时，
不把千空 Blueprint 的历史数据当动画性能。

**新工期仅为未实测的工程预算**：
M1（P0～P7，含调试、集成及返工预留）暂按 **80～140 个 AI 有效执行小时**；
M2（P8～P9）额外 **30～60 小时**，本计划合计暂按 **110～200 小时**。
日历进度依每日实际运行时长、资产导出和交互验收安排，不预先保证完成日期。
不含资产补做、获取受限输入的等待、联网、通用重定向或完整节点编辑器。
原 ALS 的 53～90 小时不再适用，也不是本计划已消耗或剩余工时。
P0 资产锁定、P3 正式走跑闭环、P5 首次真实匹配完成后分别重估。

## 9. 下一步与阻塞处理

1. 定位/获取 GASP 实际项目，记录项目版本、引擎版本、内容摘要及行为矩阵。
2. 核对实际下载内容的使用范围；没有确认可用于自研引擎的资产不搬入资源仓库。
   如内容受限，保留架构目标并用可用自有/许可动作库验证，明确不宣称原资产对照完成。
3. 在隔离副本核对/导出最小角色与 Idle/Walk/Run/Turn/Start/Stop/Jump/Land，
   同时记录根轨道、曲线和数据库/schema/Chooser 信息；不升级保存原始项目。
4. 验证动画底座候选在 MinGW/C++17 上的构建与真实片段采样，锁定依赖和测试夹具。
5. 输出 P0 验收包与缺失项，具备输入的通用骨架/蒙皮任务继续推进。

GASP 版本尚未锁定是输入缺口，不把本机已有 ALS 项目当作替代。
遇到只有用户能提供的项目或权限信息时，明确指出缺失输入；
对不依赖它的通用基础继续开发，不能用占位结果勾选 GASP 效果验收。

## 10. 参考与变更记录

2026-10-18：用户将开发目标由 ALS 改为 GASP，保留 C# 高层状态机与
逐阶段效果验收；新增原生轨迹/历史/特征数据库/搜索和障碍交互工作。
原 ALS 文档保留方案历史，后续执行以本文为准。
本轮只更新计划与导航，没有新增动画实现，也没有完成 P0。

官方参考（资料会随版本变化，不能代替锁定项目审计）：

- [Game Animation Sample 项目说明](https://dev.epicgames.com/documentation/en-us/unreal-engine/game-animation-sample-project-in-unreal-engine)
- [Motion Matching 与 Pose Search](https://dev.epicgames.com/documentation/en-us/unreal-engine/motion-matching-in-unreal-engine)
- [ozz-animation 官方项目](https://github.com/guillaumeblanc/ozz-animation)

## 11. 执行记录

2026-10-18：用户同意开始执行，P0 已启动。
新增默认关闭的动画预检工具，锁定并验证 ozz-animation 0.16.0 的
MinGW/C++17 采样与离线构建，交付只读 UE 项目盘点和 15 项模块回归。
真实走路夹具的原始关键帧对照及最终两遍回归通过。

**P0 仍为部分完成**：本地限定范围调查未定位到实际 GASP 项目，
版本/资产/行为审计、最小导出、坐标映射与 UE 对照均未完成。
依赖自带走路夹具不作为 GASP 内容，也不构成 VL 骨骼动画出图。
后续从实际 GASP `.uproject` 路径或项目包继续资产基线工作。
完整证据、操作入口与未验收项见
[P0 启动记录](../../rationale/gasp-p0-startup-record.md)；
已实现的工具边界见
[动画预检工具](../../contracts/animation-preflight-tools.md)。

2026-10-18 后续：用户提供实际 `gasp` 项目；项目声明 UE 5.8，
以本机 UE 5.8.2 在隔离副本审计，输入 BLAKE3 与原目录盘点一致。
固定 CMC 参考路径，完成 mannequin、12 段动画及曲线/事件/根轨道/
schema/数据库/Chooser 文本导出；91 根物理骨与 2 根虚拟骨分别记录。
18 项模块回归通过，12 段源空间姿态/根位置/时长和网格绑定矩阵对照通过。
记录最大 9 个顶点骨骼影响、源 Walk_F 沿 +Y，以及 3 根道具骨的源缺轨回退。

**P0 仍为部分完成**：内容来源/使用范围、UE 操作行为录像与最终 VL 坐标映射未验收；
未声明 P1 导入、P2 蒙皮出图或后续运行时阶段完成。
第 2 节与此前执行记录反映计划建立时的调查状态；
最新执行证据以 [资产基线记录](../../rationale/gasp-p0-asset-baseline-record.md) 为准。

2026-10-18 P1 后续：落地可复用的 CPU 骨架/完整权重/片段导入与 ozz 采样，
统一厘米/Y-up 模型基底；保留 9 影响和三根缺轨道具骨，复用 MikkTSpace 顶点映射。
12 段 × 5 帧 × 91 骨的 UE/VL 对照及 2888 个根样本通过，
最大位置差 0.0551 cm、旋转差 0.0514°；38 项模块回归通过。
**P1 技术数据验收通过，正式资源未提升**；没有 World 动画组件、GPU 蒙皮或 C# 动画闭环。
P0 使用范围与 UE 操作录像仍未验收，Actor/组件朝向仍需在角色集成时落实。
完整入口、失败修正、限制与下一步见
[P1 CPU 导入记录](../../rationale/gasp-p1-cpu-import-record.md)；
本计划前文的“尚未实施”是计划建立时的历史状态，以本执行记录区分当前进展。

2026-10-18 P2 后续：用户同意在资源根创建正式关卡，
实际采用 `Maps/SC_gasp/SC_gasp.json`，替代第 7 节拟议的 `SC_gasp_locomotion`。
Meshes/Materials/Source 保存正式角色、12 段片段及审阅场景，
审计清单归 `Generated/Import/SC_gasp`；原 UE 项目只读输入摘要与 P0 一致。
默认 Idle，Walk 保留根位移；没有将根位移施加到 Actor/Jolt。

原生片段时钟、不可变姿态、完整权重 GPU 蒙皮、Base/ShadowDepth 同步、
动态 bounds 与实际提交的速度历史已落地。
既有模型资源标签支持骨骼预览播放/暂停、片段选择与时间轴。
CPU/GPU Beauty/Normal、单/双线程、确定性 seek 像素对照通过，
根 CTest 1360 项零失败、6 项环境跳过，获准的 Vulkan 重载/事务检查串行通过。
**P2 技术渲染验收通过；预览控件的真实鼠标交互仍待补验。**
实窗截图与模块控件测试分别记录，不把截图当作交互验收或正常退出证据。
P0 使用范围与 UE 行为录像没有由此次技术验收代为确认。
详见 [P2 蒙皮记录](../../rationale/gasp-p2-skinning-record.md) 与
[当前播放/蒙皮契约](../../contracts/animation-runtime-skinning.md)。

下一开发阶段为 P3：接入 C# 高层状态/动画请求与 Jolt 角色实际运动，
先形成可操控 Idle/Walk/Run 闭环，明确模型前向、根位移及暂停/Stop/World 换代语义。
Motion Matching 仍属于 P5，不以当前片段播放器标为完成。

2026-10-18 P3 后续：已接入 C# 状态机与原生片段请求/反馈，
Idle/Walk/Run/Jump/Fall/Land 以 Jolt 实际速度和触地状态决策。
原生负责片段时钟、in-place 根 TRS 消除、暂停与 Stop 身份重建；
Playing/Paused 的受控骨骼程序集热重载明确要求先 Stop。
角色操控模块回归覆盖走跑、跳落、碰墙与生命周期；真实键鼠/Play/Pause/Stop
交互仍待补验，自动 Play 截图只证明启动、角色相机与渲染。

按用户新增要求，独立 `SC_gasp_playground.json` 引入 DefaultLevel 的
110 个可见静态实例、9 张源贴图、78 个材质实例和 85 个模型描述，
另烘焙 Landscape 静态网格与 Logo 透明平面。方向光按用户建议改为 10 lux，
配套环境强度 2、EV100=3。P2 场景保留；正式引用不依赖 Generated。
源 Landscape 公开回读存在重采样，最大高度边界相差约 9.18 cm，
Logo 没有实现通用 deferred decal；不声称 UE 原图逐像素等价。
Motion Matching、混合、IK 与 UE 蓝图交互均没有由此次内容迁移实现。
详细验证及未验收项见 [P3 控制与关卡记录](../../rationale/gasp-p3-control-and-level-record.md)。

2026-10-18 用户实机补验：Development（RelWithDebInfo、C# ON、
Validation OFF）启动正式 P3 关卡，用户反馈“运行起来没啥问题”；
记为实机运行冒烟通过。运行日志确认正常退出且 stderr 为空。
用户没有逐项确认动作与 Play/Pause/Stop 矩阵，不将该反馈扩大为全部控件验收。
后续开发按 P4 轨迹/Pose History/特征数据库推进，
可验收效果为过去/未来轨迹可视化、动画帧特征检查与确定性数据库重建。

2026-10-18 P4 后续：实际根/姿态历史按时间重采样至 60 Hz，
新增版本化 heading 空间特征、向后速度差分、SE(3) 循环延拓和非循环完整窗口排除。
正式 `MotionMatching/PS_gasp.json`、`DB_gasp.json` 产出 12 成员、
1261 帧 × 42 维数据库；冻结总体统计和独立权重，缓存具有完整 BLAKE3 身份、
payload 校验及发布前输入复核。冷重建、损坏恢复和失效检查通过。

关卡 Motion 显示青色实际过去、橙色近似未来及绿色关节；
骨骼模型预览随片段/时间显示 raw 特征，独立 HTML 浏览器显示帧的归一化值/权重与图。
单/双线程生产链路截图、浏览器实际选择/帧输入及桌面/手机布局已核对；
原生编辑器控件仍只有模块交互覆盖，没有新增实机鼠标验收。
动画 61、脚本 316 全量连续两遍通过；UI 653 项两遍均零失败、5 项环境跳过。

P4 交付的是 P5 的数据与调试基础。历史为物理后 GT 观察插值，
预测沿当前 heading/意图并积分重力，没有未来碰撞/台阶/移动平台；
运行时尚未绑定数据库统计或执行搜索。当前仍为 P3 C# 片段控制及硬切，
混合、IK 和动作事件没有实现。下一阶段 P5 接入同版本数据库/query、
精确距离搜索、稳定决胜和连续性调试。

### P4.5 公共 DrawDebug 插入阶段

用户确认在 P5 前完善公共 DrawDebug。该阶段交付 C++/C# 点、线、折线、
箭头、圆、球、定向盒、胶囊、坐标轴、世界文字；统一生命周期、类别/owner
过滤、清理、冻结快照、Console 与视口控制。轨迹从专用显示迁到公共适配。
验收以 `debugdraw demo` 形状展示、单/双线程图片、过滤/清理对照、
真实 CLR 生命周期和 RmlUi 控件回归为准。

当前实现边界见 [DrawDebug 契约](../../contracts/debug-draw.md)；
按用户后续要求，默认 Overlay，可选 DepthTest；C++/C# 共用参数，
按 resolved 场景深度逐像素遮挡，涵盖线、点及世界文字。
P5 仍未开始，当前角色仍使用 C# 固定片段切换，没有运行时 MM 搜索、混合或 IK。
完整证据及最终构建结果见 [P4.5 记录](../../rationale/gasp-p4.5-debug-draw-record.md)。

### P5 执行中：原生搜索与局部交叉混合

2026-10-18：按用户要求由 subagent 执行、主线程规划与验收。
已新增同版本 query/database 绑定、精确候选搜索及原生策略状态；
C# Match 只传允许片段和动作锁，正式角色高层状态仍在资源侧 C#。
播放器新增 ozz 局部 TRS 交叉混合，单一目标时钟和有界冻结源支持中断，
普通混合保留 pose epoch；根消费策略变化要求明确硬切。
当前源码接口边界见 [动画运行时](../../contracts/animation-runtime-skinning.md)。

新增外部 `capture_gasp_p5.py`，仅在 Generated/Validation 建独立输入夹具，
经真实 Character/Jolt 与 Match 执行 idle/start/walk/stop/reverse/run/jump/land。
工具分别记录截图是否完成、原生实际选中类别是否符合动作阶段；
起步/停止/转向不能以“选中仍在允许集合内”代替对应动作，
失败保留完整 trace 和实景截图，不缩为单片段强制通过。
单/双线程和 1280/1920/900 宽桌面布局属于独立场景检查，
不声明帧率无关动作确定性或手机支持。

**P5 尚未验收完成**：本条只记录源码和工具落地，正式 GASP 动作矩阵、
候选/分项代价的实景可读性、生命周期门禁及最终构建仍由主线程逐项验收。
纯逻辑测试不能替代实际起步、停止与转向帧的选择效果。
没有提前声明 P6 的惯性化、根偏移、步幅修正或足部 IK。

P5 外部首轮实测：旧配置启动的 Debug 图像均已写出，但启动首帧的大 delta
跨过固定暖机，不能据此判定起停矩阵。工具修为真实 query 就绪后才启动输入。
随后 b 轮在新数据库身份 `9b6ae5f7668b...` 下，
Walk/Start/Stop 的真实原生反馈达到对应类别；Reverse 没有选中 Turn，
因此整体明确失败并保留 trace，未将“可截图”写成动作通过。
该轮 Debug 帧间隔约 0.08～0.2 秒，后段还发生超过历史 gap 阈值的停顿；
固定 60 Hz 的模块行为结果不能代替这条实景失败记录。
托管反馈与 RT 截图不保证同一帧，图表的实际 Selected 行需单独看图核对。
证据位于 `artifacts/gasp-p5-capture-smoke-20261018-a/` 与
`artifacts/gasp-p5-capture-smoke-20261018-b/`；最终优化构建/布局矩阵仍待验收。

P5 Development 的 c 轮完整执行 20 项：八动作各单/双线程、另加
1920×1080 与 900×700 Walk 布局。20 项截图完整且 stderr 为空，
19 项原生动作类别通过；900×700 双线程 Walk 始终选中 WalkStart，
实际速度约 200 cm/s、query 就绪且无历史 gap，故整体明确失败。
1280×720 的八动作单双线程均曾选中预期类别，Reverse 两线程选中 Turn180L。
其中单线程 Walk 图片实际 Selected 已回到 WalkStart，不能将阶段行为通过
当作该截图的 WalkLoop 验收；窄窗失败图片的固定镜头还发生角色离开可见区。
后续工具改用跟随实际角色的固定世界方向相机，图片与动作的关联限制仍然保留。
证据为 `artifacts/gasp-p5-capture-development-20261018-c/capture.json`，
配置原始字节已恢复，全部自有进程已结束；等待原生尾段策略修正后复验受影响项。

最终 Development d 轮完成 20/20 截图与原生阶段行为，整体退出 0。
Walk 改为输入至少四游戏秒且连续 WalkLoop 至少 0.75 游戏秒，六秒截止；
六个尺寸/线程组合实际在 4.00～5.08 秒通过，连续 Loop 为 0.75～3.55 秒。
1920 两项启动期各出现一次 >0.5 秒历史 gap，恢复真实查询后通过，
不声明全程无停顿或帧率无关确定性。e 轮只扩大独立夹具审阅地面，
六项 Walk 再次全部达到严格行为条件；其中 900×700 单线程的截图前反馈
已变回 Start，不能以其阶段通过替代该图片的 Loop 视觉验收。
完整报告分别为 `artifacts/gasp-p5-capture-development-20261018-d/capture.json`
与 `artifacts/gasp-p5-capture-development-20261018-e/capture.json`。

正式原 `SC_gasp_playground` 配合 Development 原 `Gameplay.dll`、临时 autoPlay
完成 1920×1080 单/双线程启动截图，保留地图贴图、Logo 和正式角色相机。
干净重跑图中实际 Selected=Idle、Query 42D ready；只是无输入的启动/待机冒烟，
不替代正式动作模块、真实键鼠或正常退出验收。
证据为 `artifacts/gasp-p5-formal-playground-clean-20261018/`；
首次带引号截图路径被既有 Console 拒绝，另保留原失败/补发目录，
没有把它冒充干净采集。启动参数的命令延迟单位为帧，此次使用 12 帧。

正式场景 description 经同一 `vl_asset_edit` JSONL host 完成
get_document/get_capabilities/modify/validate/save，全部 succeeded、
errorCode=none，最终 revision=3、退出 0；深比较确认仅说明字段语义变化。
说明同步 C# 高层状态机、原生 MM 基础和局部 TRS 交叉混合，
继续明确 UE 动作/灯光不等价及未实现 IK/惯性化等。
证据为 `artifacts/gasp-p5-playground-description-api.json`。

后续 blocked 定向验收完成：Development main 以 `-j2` 增量构建退出 0，
独立 Generated 静态墙持续收到 Move=200 cm/s。双线程首次、单线程定向重跑
均在 active≈4 秒满足连续约 2.02 秒的受阻 Idle：Jolt 实速=(0,0,0)，
意图=(0,0,-200)，胶囊停在 z=-199.8；实际图表为 Query42D ready blocked、
Selected Idle、trajectory cost=0，amber 继续显示不求碰撞的输入意图。
红色冻结查询的未来点在根处重合，因此为脚下短标记，不是长红线；
放大检查的足部 ROI 含 20 个红像素。原生未来点数值由独立模块快照断言佐证，
不把 CLR 推导的 `physicallyBlocked` 当作原生 query 标志。

双线程图为 `artifacts/gasp-p5-blocked-development-20261018/blocked-t2-1280x720.bmp`，
单线程为 `artifacts/gasp-p5-blocked-development-t1-retry-20261018/blocked-t1-1280x720.bmp`。
首轮单线程 100 秒墙钟预算主要消耗于初始化，暖机阶段触发 fallback，
尚未完成动作而失败；该报告保留，没有用重跑结果覆盖。
提高该次外部超时到 240 秒后实际约九秒完成并退出 0。
两次采集均精确恢复 config 并等待所有自有进程结束；
截图进程按约定强制终止，不充当正常 teardown 证据。

源码侧 `build_gasp_level.py` 和 `import_gasp_materials.py` 的 description
同步当前 C#/MM/交叉混合与未实现项，避免重新导入后回归“MM 尚未迁移”。
前者仍明确自己只生成初始简化色板，后者文本与已保存正式场景一致。
本轮没有重跑生成器，也没有因此修改正式对象、贴图或原 UE 内容。

最终兼容性补验：`build-release-mingw` 的 C# OFF Release main 与
script_tests 使用 `-j2` 构建成功、退出 0；完整 79 项连续两遍全部通过，
零失败、零跳过。日志分别为 `artifacts/gasp-p5-csharp-off-final-build.log`
与 `artifacts/gasp-p5-csharp-off-final-tests.log/xml`。
该路径不包含真实 CLR/Jolt 宿主；不能替代主线程的 C# ON 和 Vulkan 生命周期验收。

随后 120 Hz 正式控制器反向专项暴露子步边界问题：无 Jolt 子步的纯朝向更新
曾清除实际速度，使 C# 短暂撤销转身候选；查询当前根速度也曾由渲染间隔差分
产生 0/双倍振荡。修正后的 Development main 与 C# OFF main/script_tests
均以 `-j2` 增量构建成功，未改搜索策略或控制器候选集合。
本组只定向复验 reverse/blocked 各单/双线程，没有机械重跑前述 20 项矩阵。

`artifacts/gasp-p5-substep-reverse-blocked-final-20261018/capture.json`
记录 4/4 截图和阶段行为通过，近似截图前反馈亦 4/4 符合预期。
四图逐张目视：Reverse 的实际 Selected 均为 Turn180L，
Blocked 均为 Idle 且 Query42D ready blocked；脚下红色查询标记与 amber 输入意图仍可见。
所有 stderr 为空、整体退出 0，config 恢复并清理完全部自有进程。
这些仍为独立输入夹具，不能替代主线程的正式 30/60/120 Hz 模块证据或真实键鼠验收。
对应构建日志为 `artifacts/gasp-p5-development-substep-final-build.log`。

两补丁后的 C# OFF 79 项完整选择集再次连续两遍全部通过，零失败、零跳过，
日志为 `artifacts/gasp-p5-csharp-off-substep-final-build.log` 与
`artifacts/gasp-p5-csharp-off-substep-final-tests.log/xml`。
先前带 final 名称的文件继续保留其原轮次语义，不代替本次最终源码复验。

### P5 技术验收结论与 P6 起点

2026-10-18：主线程独立核对最终 C# ON 完整 349 项连续两遍全部通过、
退出 0；既有 World/Graph 事务检查退出 0，记录正常 teardown 且 stderr 为空，
无残留进程，config 已恢复。结合本轮搜索/混合模块、冻结 UI、
C# OFF 兼容性、正式控制器 30/60/120 Hz 动作与受阻专项，
以及实景动作/尺寸矩阵和最终 Reverse/Blocked 定向图片，
**P5 技术验收通过**。完整数量、日志、失败修正和各检查范围见
[P5 执行记录](../../rationale/gasp-p5-motion-matching-record.md)。
此前“尚未验收完成”及各轮失败语句保留其历史含义，不作追溯改写。

本结论覆盖 C# 高层许可族、同版本原生精确搜索、局部 TRS 交叉混合、
不可变查询/候选/代价调试及已执行的自动化和实景定向检查。
它不代表完整 GASP 或 M1 完成，也不将 P6 的脚滑/接地质量、
真人键鼠动作全矩阵、P0 内容使用范围与 UE 行为录像对照一并验收。
独立夹具截图、正式控制器模块和正常退出日志继续分别记账；
截图前托管反馈不提升为与 RT 图片精确同帧。

下一阶段 P6 先建立可复现的动画质量基线：固定起步、稳态、停止、
急转、跳落及墙前场景，记录脚滑、穿地、脚离地、根/胶囊偏差和过渡突变。
在该基线上分项引入根偏移、步幅与足部修正，并提供可关闭的同场景对照；
每项同时检查局部效果、原生时钟/暂停和已有动作语义，交付可直接操作的验收场景。
惯性化与 IK 的具体算法、接入顺序及误差阈值由下一阶段质量测量确定，
不把当前线性交叉混合提前称作这些修正已经实现。

### P6 首个缺陷修正：Actor 有界转向

2026-10-18：用户实操反馈转向像硬切。归因是资源侧 C# 立即设置整个
Actor yaw，既有原生局部 TRS 交叉混合无法平滑该世界朝向变化。
首项修正将 Actor 转向限制为最短角差上的 `540°/s`，使用 scaled delta，
恰好 180° 固定正向；Move 意图与观察相机维持原有即时语义。
当前源码边界同步两份动画/C# 契约，
实施与分项验收见 [P6 有界转向记录](../../rationale/gasp-p6-bounded-turn-record.md)。

本条处于自动化与实景验收阶段，**不代表 P6 完成**。
当前未来轨迹没有角速度预测，移动先于身体转向仍可能侧滑或倒滑；
根偏移、步幅修正、惯性化和足部 IK 仍须按质量基线逐项验收。

有界转向最终定向检查覆盖 34 个不同用例：首轮 34 项中 31 通过，
三个 Stop 夹具错误修正后，与新增相机断言影响的六项合计九项复跑全部通过；
未受影响的 25 项沿用首轮结果，没有完整重跑 377 个当前注册脚本用例。
30/60/120 Hz 的实际最大角步分别为 18°/9°/4.5°，
已测 90°/180°到达时间分别为 1/6、1/3 scaled 秒。
Development 正式 playground 的 Play/Immersive 已出图，
持续键输入下的转向观感尚未由自动化或主线程实操确认，交由用户当前窗口验收。
这仅完成首个转向缺陷的定向自动化，不改变 P6 尚未完成的阶段状态。

### P6 稳态步频分项

2026-10-18：用户要求继续 P6 并精简测试。本轮先收敛直线移动循环的速度失配：
Walk 源根约 200 cm/s、Run 500 cm/s、Sprint 700 cm/s，
正式角色 Walk/Run 为 200/450 cm/s。先交付原生有界步频匹配、
正式角色开关和同输入支撑脚漂移对照，再独立推进地形足部接触。
这些源根速度差不直接当作实测脚滑值。

本分项不扩 FPS 测试矩阵，也不提前声明根偏移、脚锁、足部 IK、
骨骼步幅变形或惯性化完成。实施约束、精简验收口径与后续实际结果见
[P6 稳态步频记录](../../rationale/gasp-p6-speed-matching-record.md)。

本分项最终只验收根行程匹配：正式控制器 Run 的行程差由约 50 cm/s
降至 0.001 cm/s 以下，Walk 保持近零；正式开关默认关闭。
首轮高 contact 踝漂移对照改善不足，失败证据保留，不能称为足底消滑通过。
新增四项 CPU 和一项正式控制器行程测试，复用 60 Hz 动作/受阻与受影响生命周期选择集，
未扩全量或多帧率矩阵；实测数量与构建见记录。
下一接地分项先定义足底锚点、地面查询及 pre-IK/渲染姿态隔离，
再接入已有两骨 IK 并验收足部漂移与穿地。P6 整阶段继续保持未完成。

### P6 静态足部接地分项

接续步频分项，新增可关闭的静态地面足部锁定、两骨 IK 与受限视觉骨盆下移。
采用绑定 ball 投影单参考点，保留 post-blend/pre-IK 姿态供 MM 查询，
不把 IK 后渲染姿态写回历史或混合源；Jolt 继续独占胶囊位移。
正式角色的 `FootPlacement` 默认为 false，提供同关卡开关对照。

本分项按固定 60 Hz 的 18° 静态横坡、独立参考点/法线几何误差验收，
复用已有动作、撞墙和 Pause/Stop 测试，不扩全量或 FPS 矩阵。
执行范围、阈值、失败及最终结果见
[P6 足部接地记录](../../rationale/gasp-p6-foot-placement-record.md)。
移动平台、完整鞋底支撑、台阶遍历质量、根偏移和惯性化不计入本次通过范围；
P6 整阶段仍未完成。

静态接地分项最终定向技术验收通过：18° 横坡稳态最大单参考点误差
`7.193770→0.272459 cm`，Actor/MM/时钟与全部 pre-IK 结果保持一致。
CPU 16 项与桥接/生命周期七项各连续两遍通过，
坡面和已有动作/撞墙 `/60` 三项通过，Development 实景开关对照完成。
这只推进静态单参考点接地，不改写先前脚滑失败证据；
走跑全周期接触、完整鞋底/台阶质量与后续根偏移、惯性化仍未验收。
最终构建、精简测试范围和失败修复详见
[P6 足部接地记录](../../rationale/gasp-p6-foot-placement-record.md)。

### P6 动态接地、惯性化与视觉根修正分项

2026-10-19：用户授权完成 P6 剩余工作。在静态单参考点接地之上，
新增 ball 与踝下 heel 双参考点、走跑全周期锁定/释放、heel→ball 滚动支点切换、
同面非穿透与跨踏面单支点、摆动脚地面间隙约束；骨盆仍限 15 cm / 60 cm/s。
同时接入可关闭的有限时惯性化（五次 Hermite、SO(3) 左雅可比搬运角速度、
端点精确归零）和有界视觉根偏移（世界 XZ ≤20 cm、yaw ≤45°、半衰期 0.08 s），
视觉修正不改写查询姿态、Actor/Jolt 或唯一动画时钟。

正式控制器夹具固定 60 Hz、MatchSpeed 两组相同，走/跑各 10 s、台阶 6 s。
最终两遍均通过：Walk 每足路径降幅 71.9%/67.6%、Run 80.1%/85.6%，
支撑窗漂移 ≤0.808 cm、代理点穿入 ≤0.265 cm、Walk/Run 覆盖 1.00；
台阶六段全部跨越，单参考点样本 25/42 帧、间隙 ≤0.292 cm、漂移 ≤0.117 cm。
足部开关下 Actor、MM、片段、时钟和 pre-IK 逐帧一致。
Debug 动画 129 项、生命周期/坡面 8 项、C# OFF 79 项均连续两遍通过；
Development 正式关卡完成关闭/足部/组合出图，Stop 取到根偏移 `smoothing`
（-2.334 cm），Blocked 取到足部 `planted`（骨盆 -0.752 cm）。
这些出图只作组合行为与画面完整性，不用于足底像素判定；台阶图取自跨越之后。

**P6 绑定代理口径的自动化分项技术验收通过**，但真实鞋底网格未通过：
同一批夹具中网格鞋底顶点最大穿入为 Walk 5.522 cm、Run 4.498 cm、Stairs 9.037 cm，
因此本条不写成真实鞋底、完整台阶质量、移动平台或完整 GASP/M1 通过。
首轮 Run 零支撑窗、支撑窗被支点切换拆碎、边界样本计入滑移、骨盆压入摆动脚
和跨踏面抬脚等失败数据与修正详见
[P6 动态质量记录](../../rationale/gasp-p6-dynamic-quality-record.md)。

2026-10-19 UE LockType 对照：按 UE 5.8 `FAnimNode_FootPlacement` 增加显式
`PivotAroundBall` / `PivotAroundAnkle`，并保留 VL 的 `Auto` 默认；C# 暴露
`Match(..., footLock)` 与 BP `FootLock`。同一 P6 夹具实测：`Ball` 走跑滑移与
`Auto` 同量级，仅 Run 单帧代理穿入 1.33 cm（>1 cm）；`Ankle` 因固定踝下投影
而非真实 heel 骨，Walk 滑移几乎不降、穿入 3.3 cm。因此默认仍为 `Auto`，
Ball/Ankle 只作 UE 对照，不新增 toe IK。详见
[P6 动态质量记录](../../rationale/gasp-p6-dynamic-quality-record.md)。

### P7 时间轴分项起步

2026-10-19：P6 绑定代理分项完成后进入 P7。首个分项给编辑器 Motion 面板加入
被聚焦脚本网格的原生时间轴：播放/暂停/拖动 seek，时长与当前时间来自
`MotionDebugView` 的播放器快照，命令通过 `ControlScriptAnimation` 只改该
`sceneObjectIdentity` 的唯一播放器；seek 先显式暂停，MM 下一帧不能覆盖定位。
离线、无控制器或时长为零时禁用，拖动期间不抢焦点。

实现与 UI 回归已通过：`editor_ui_tests` 668 项连续两遍（663 通过、5 项符号链接跳过）、
C# ON `script_tests` 384 项一轮通过、动画 129 项两遍、C# OFF 79 项通过；
时间轴定向 3 项连续两遍通过；Development 正式夹具采集 1/1 通过，
Motion 面板读回 play/pause、轨道与 `0.3435 / 4 s >`。

查询回放：World 保留被聚焦组件最近 600 帧的冻结 `MotionDebugView`；
Replay 进入时暂停目标播放器，只按历史帧选择查询/候选/代价/轨迹，退出恢复实时。
回放不重演骨骼姿态、不重搜、不写 Actor/Jolt。

独立包：先用 Debug 验证闭包，再以 `--binary-directory build-vscode/development/bin`
导出 `artifacts/gasp-p7-export-dev-01`，从仓库外 cwd 启动，World 提交成功，
空闲 clip=0，按 W 切到 clip=5 的 WalkLoop 并推进，松开回 clip=0，窗口 93 FPS；
`CloseMainWindow` 后正常 teardown，stderr 为空。`dev-02` 另验证按 identity 的
`animation get`（player/visual 与模板）与 108 FPS。
固定动作矩阵探针 `SC_gasp_package_probe.json`/`Gasp.PackageProbe` 导出为
`artifacts/gasp-p7-probe-export-01`，从仓库外启动，连续记录
Idle→Walk→Run→JumpStart→Fall→Land→Idle、yaw=180、位置 z≈-1304、
跳跃 grounded=False、落地 grounded=True、`P7_PROBE_FINISHED`；451 FPS，
正常 teardown，stderr 为空。正式 MM 探针 `SC_gasp_mm_probe.json` 通过默认关闭的
`PackageProbe` 属性驱动 `Gasp.CharacterBlueprint` 的 LocomotionState/MM，
记录 Walk→Run→Jump→Fall→Land→180°→Idle、z≈-1000、459 FPS，
正常 teardown；42 个场景 schema 通过。正式 playground 包真实键鼠冒烟为
clip=0/6/7/9（空闲/W/W+Shift/Space），无测试负载 97–117 FPS；
程序集 reload 29 项与 World replacement 6 项回归通过。
**P7 的时间轴、查询回放、独立包与生命周期技术验收分项完成**；
历史姿态重演、多人回放与逐 pass 性能分项保留为后续增强。
详情见 [P7 时间轴记录](../../rationale/gasp-p7-timeline-record.md)；
契约见 [动画运行时契约](../../contracts/animation-runtime-skinning.md#c-片段控制)。
### P8 障碍交互起步

从锁定 UE 5.8 工程导出 `M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot` 与
`M_Neutral_Traversal_Vault_1_0_stand_F_Lfoot` 两个 1 m stand 动作；
P9 再补同高度 walk/run 四个变体，放入 `Maps/SC_gasp/Source/` 并登记到
`SM_mannequin.json`。`Gasp.CharacterBlueprint` 增加
`TraversalEnabled/TraversalProbe/TraversalAction/TraversalGait`，
以 SphereCastLocal + 顶面向下射线 + 净空向上射线实现高度 70–130 cm 接受、
高度不足/过高/顶棚拒绝；动作期间用 Jolt 跳跃提供竖直位移、水平前向速度越沿，
播放 in-place Mantle/Vault，结束后回 Idle。

场景矩阵通过：1 m Mantle/Vault stand/walk/run、1.6 m 高度拒绝、0.5 m 高度拒绝、顶棚净空拒绝、后方无地面 airborne abort、前方高墙 stalled abort、无目标 no-hit；57 个场景 schema 通过。
F 正式键鼠路径未取得稳定焦点证据，当前证据来自同 `RequestTraversal` 的自动探针。
med/high Mantle/Vault、1 m Vault 地面根轨迹仍未接入；
2.5 m Climb 根运动已另做 P10 验收。
因此 **P8 的 1 m stand/walk/run 固定子集技术验收完成**；P8 全阶段与 M2 仍未完成。
详情见 [P8 记录](../../rationale/gasp-p8-traversal-record.md)。
### P9 独立包集成起步

首个 P9 分项：1 m Mantle 包 `artifacts/gasp-p9-traversal-export-01`
从仓库外启动，记录 `P8_TRAVERSAL_END pos=0,190,-199 grounded=True`，
约 605 FPS，正常 teardown，stderr 为空。矩阵包
`artifacts/gasp-p9-traversal-matrix-01` 通过 `--dynamic-assets` 收入六个
Traversal 场景，并用 `--initial-scene` 切换：Vault 越过到 `(0,90,-290)`；
高低/净空/无目标拒绝；airborne abort 均记录且正常 teardown。
`gasp-p9-final-package-01`（1725 文件）再验证 Mantle/Vault 的
walk/run 变体，四个场景均落地并正常退出。
`loadworld` 队列命令另验证 generation=2 的 Vault World 换代；`shaderreload all` 验证 compiled=16、committed=true、retiredPipelines=18；无负载 1280×720 采样 589–639 FPS；`reloadassembly` 在包内 Editing 提交，Playing 受控骨骼正确拒绝。
已覆盖 1 m stand/walk/run；med/high、长时间 GPU/压力、真实输入与完整动作矩阵未在包内重跑；P9 的 1 m stand/walk/run 独立包子集技术验收完成，完整 M2/最终交付仍未完成。
详情见 [P9 集成记录](../../rationale/gasp-p9-integration-record.md)。

2026-10-21 P27 三帧率与包内矩阵：`--frame-limit` 让渲染节拍钉在 30/60/120 Hz
（固定 60 Hz 物理子步不变；P28 已改为每帧变 dt，见文末 P28 小节）。仓库内 21 个矩阵场景 × 3 档 = 63 次通过 62 次：
60/120 全过，30 Hz 只有 `SC_gasp_catch_family_mantle` 确定性失败 2/2
（`P8_TRAVERSAL_ABORT reason=catch-missed-top progress=0`，没有 `P8_CATCH_LANDED`）；
60/120 同场景 `CATCH_LANDED y=288`、`END y=290`。独立包同一二进制三档
60/120 全过、30 首跑 20/21 + 重试 1 次后 21/21（`catch-family-hurdle` 首跑
`stalled`）；包内 30 Hz Mantle 首跑通过、未复现仓库内失败，30 Hz catch 仍 flaky。
详见 [P27 记录](../../rationale/gasp-p27-visual-frequency-record.md)。

### P10 2.5 m Climb 根运动

导出 UE `Climb_Start_2_5` stand/walk/run，新增 `SC_gasp_traversal_climb.json`、
`_walk.json`、`_run.json`：障碍顶面 250 cm，probe 球从局部 `y=120` 检测。
Climb 使用 `RootMotionDelta / dt` 覆盖 CharacterVirtual 速度，不再调用 Jump；
仓库与独立包均落地：

```text
stand: END pos=0,340,-169 / -173 grounded=True yaw=0
run:   END pos=0,340,-176 / -151…-196 grounded=True yaw=0（10/10）
walk:  END pos=0,340,-193 grounded=True yaw=0（竖直闭环校正）
```

Mantle/Vault 回归不变（`190,-202` / `90,-290`）。P10 包
`artifacts/gasp-p10-climb-package-05` 为 1721 文件；stand/run 前半段走根
竖直、后半段闭环收口，walk 全程竖直闭环，水平仍走根轨迹。package-04 的
run 3/10 airborne abort 已在该版本降到 0/10。
2 m/3 m 地面 Mantle/Vault 资产审计结论为**无可用 clip**，见
[med/high 资产审计](../../rationale/gasp-medhigh-asset-audit.md)。
详情见 [P10 记录](../../rationale/gasp-p10-climb-root-motion-record.md)。

### P11 2/3 m Hurdle gameplay stub

资产审计确认无 ground-start 2/3 m Mantle/Vault，因此用 1 m `Hurdle_1_0`
身体动作 + 根竖直缩放（2×/3×）和 3 m 有界水平补偿，新增
`SC_gasp_traversal_hurdle_2m.json`、`_3m.json`：

```text
2m: END pos=0,290,-233 / -246 grounded=True
3m: END pos=0,90,-297 / -306 grounded=True
```

这是 gameplay stub，不是 2/3 m 身体动画/资产验收。严格 UE GASP 范围下已从
主资源/场景移除，仅保留记录。详情见
[P11 记录](../../rationale/gasp-p11-hurdle-stub-record.md)。

### P12 输入注入

新增 `injectinput <action> <down|up>`，对应 UE `InjectInputForAction`；
`SC_gasp_traversal_input.json` 关闭自动探针，合成 `Interact` 触发 traversal
到 `END pos=0,190,-202 grounded=True`；`--focus-window` + 真实 F 在仓库和
`gasp-p12-input-package-01`(1723 文件) 也到达同一结果。合成 action 与 OS 事件分开记录。详见
[P12 记录](../../rationale/gasp-p12-input-injection-record.md)。
### P13 生成 2/3 m 根轨迹资产

用 UE Python 复制 Hurdle 并重写 root track，生成 `Mantle_2_0/3_0`、
`Vault_2_0/3_0`；Mantle2/3 落顶通过，Vault2/3 运行时仍未通过。
身体仍是 1 m Hurdle，不是最终质量资产。严格 UE GASP 范围下这些资产和场景
已从主资源移除，仅保留记录。详见
[P13 记录](../../rationale/gasp-p13-generated-2-3m-assets.md)。

### P14 Traversal Montage 脚手架

资源侧 `GaspCharacter` 新增 `traversalMontage`、按 clip 类型映射的 UE notify 名称和
`P8_MONTAGE_START/NOTIFY/END` 日志，section 固定 `Default`；Mantle 实跑
`END pos=0,190,-202 grounded=True`。
notify 时间仍用 `duration * (index+1)/(n+1)` 均分近似，不是 UE 资产时间。
**P14 的 Montage 日志/名称子集技术验收通过**；未完成项为精确 notify 时间、
section 跳转/链接、NotifyState 起止窗口及 Montage 资产内的 root motion 策略。
详见 [P14 记录](../../rationale/gasp-p14-montage-runtime-record.md)。

### P15 MotionWarping 核心数学

新增 `source/animation/motionWarping.*` 与 5 个 `MotionWarping.*` GoogleTest，
`WarpTranslationToTarget` 按 UE `RootMotionModifier_SkewWarp` 在根同步空间做
scale+shear，并把 `RootMotionTotal` 经反馈暴露给 C#；5/5 通过。
把 exact warp 直接塞进 Climb 20%–80% 窗口的尝试出现 ABORT airborne，已回退，
说明当时缺的是 target/window 空间语义，不是缩放公式本身。
**P15 只验收核心平移数学**；旋转 warp、窗口绑定、30/60/120 Hz 与包内验证未做。
详见 [P15 记录](../../rationale/gasp-p15-motion-warping-core-record.md)。

### P16 Traversal Chooser 脚手架

资源侧 `GaspCharacter` 增加 `TraversalAction=Auto` 与 UE
`CHT_TraversalMontages_CMC` 行的 C# 镜像，输出 `P8_CHOOSER height/mode/choice`；
1 m 障碍选 Hurdle 并落顶 `0,190,-234`，2 m 障碍选 `Catch_Hurdle_high` 后记录
`catch-runtime-not-implemented`。
**Chooser 选择子集通过**；未完成项是 UE Chooser 资产、PoseSearch 入口帧、
Catch/MovementMode 运行时，以及高障碍仍用两档球查询而非 front/back ledge context。
详见 [P16 记录](../../rationale/gasp-p16-chooser-scaffold-record.md)。

### P17 UE Montage 时长与窗口

从 34 个 `AM_M_Neutral_Traversal_*` 的 T3D 导出解析 `SequenceLength` 与
MotionWarping notify，Climb stand/run/walk 时长改为 2.750/3.250/3.600 s，
窗口改为 0.405–0.732 / 0.366–0.885 / 0.310–1.294 s；三个 gait 恢复
`END 0,340,-178/-139/-198 grounded=True`。
这一步只修时长/窗口，target 仍是 `top+forward` 日志值，warp 未应用；
Mantle/Vault/Hurdle 窗口虽已导出但未逐个接入。
**P17 是时长/窗口修正分项，不是 warp 运行时完成**。详见
[P17 记录](../../rationale/gasp-p17-ue-montage-timing-record.md)。

### P18 运行时 Ledge Context 与 Warp

Climb 命中后改为运行时计算 FrontLedge/BackLedge/BackFloor context（2.5 m 实测
`front=0,250,-120`、`backLedge=0,250,-260`、`backFloor=0,0,-280`、`depth=140`），
target 用胶囊中心 `0,340,-120`；warp 改用 `RootMotionRemaining` 与 exact
scale+shear 后 Climb stand/run/walk 落地 `0,340,-174/-198/-201`，全 grounded。
前探距离按 UE `MapRangeClamped(Speed,0,500,75,350)` 改为
`clamp(75+speed/500*275,75,350)`；Mantle/Vault 窗口与时长同步 UE，回归为
`0,190,-202/-201/-201` 与 `0,90,-290/-290/-290`。
Hurdle 的四窗口、顺序叠加和原生 modifier 尝试均未越过并已回退。
**P18 的 Climb/Mantle/Vault 落地子集通过**；Hurdle 未接、
BackLedge/BackFloor 未作 Vault target、30/60/120 Hz 与包内验证未做。详见
[P18 记录](../../rationale/gasp-p18-runtime-ledge-warp-record.md)。

### P19 原生 MotionWarping Runtime

把 `MotionWarpRuntime` 移到 C++：窗口队列、`PreviousPosition ∈ [Start,End)` 门控、
target 模型空间换算和单 tick 根位移由原生拥有，C# 只提交 `warpTargets` 并消费
`RootMotion`；Hurdle 三窗口实跑 `END 0,188,-266 grounded=True`，Climb 由 C# 消费
warped root 到 `0,340,-207/-207/-210`，Mantle/Vault 仅提交窗口、位移仍走
`Jump + timed Move`（`0,190,-202/-201/-201`、`0,90,-290/-290/-290`）。
**数字口径**：P20 已裁定该冲突。`artifacts/nativewarp-p20/final-hurdle.log` 写于
16:29:35，资源提交 `91f98e7` 在 16:37:29；`0,180,-280` 是迁移前旧三窗口/
C# 提交配置结果，`0,188,-266` 是迁移后 native 结果，二者不是抖动。详见
[P20 记录](../../rationale/gasp-p20-warp-ue-alignment-record.md)。
**P19 的原生子集技术验收通过**；P19 时未完成项为第二个 FrontLedge 窗口、
`Distance_From_Ledge` 曲线（当时 `dOut=0`）、Mantle/Vault 消费原生 warped root、
以及 30/60/120 Hz 场景矩阵与包内 warp 验证。单测 11 项（P19 时）；P20 后 16 项。
详见 [P19 记录](../../rationale/gasp-p19-native-motion-warping-runtime.md)。

### P20 Hurdle 四窗口与 Mantle/Vault 单次消费

2026-10-20 已确认：Hurdle 改用 UE montage 的四个窗口（两个 FrontLedge `attach`、BackLedge、
BackFloor），`subtractRemaining` 全 false，`dOut=56.810280`（UE 曲线采样
34.098671/90.908951 内联），`BackLedgeNormal` 改为指向落地侧；独立复跑
`END 0,90,-334 grounded=True`（作者轮 `-333..-354`）。
Mantle/Vault 各改为两个 FrontLedge 窗口（Vault stand 另有 BackLedge），两个
FrontLedge 均 `attach`，target 改为胶囊中心；删除 `Jump()` 与分段 `Move`，四个动作共用
`RootMotion → 世界化 → 1500 cm/s → ConsumeRootMotion`，并新增
`traversalLanded`/`ClearRootMotion` 收口。
独立复跑 Mantle `-167/-202/-209`、Vault `-383/-331/-341`、Climb `-201`，全
grounded；`abort_void` 仍下坠到 `y≈-2676` 后 airborne abort，`abort_stall`
为 `progress=10`。
**P20 的 Hurdle/Mantle/Vault 子集技术验收通过**；未完成项为场景触发距离
（40 cm 对首窗 80.7–316.7 cm 接近段，5 个片段只能对 attractor）、动画曲线通道、
旋转 warp、`FrontLedgeLocation` 球心短 20 cm、运行时 30/60/120 Hz 矩阵与包内
warp 验证。`MotionWarping.*` 16/16、全量 145/145。详见
[P20 记录](../../rationale/gasp-p20-warp-ue-alignment-record.md)。

### P27 帧率与包内矩阵

P21–P26 的执行记录在 [测试与验证](../../contracts/testing.md) 的对应段落与各自的
`rationale/gasp-p*.md` 里；本节只追加 P27 的频率/包内矩阵分项。

引擎 `98d791d`（`【Mod】add render frame-limit launch option`）新增
`--frame-limit <hz>`（可传 1..1000，不传 = 0 不限速；
只限渲染主循环节拍，`RuntimeClock`/动画/脚本 dt 随帧间隔变化，`ScriptRuntime`
内部 1/60 accumulator 的固定物理子步不变）。资源 `882cc90`
（`【Mod】render GASP traversal level geometry`）同时给 18 个 traversal 场景补
37 个与 physics 同 transform/尺寸的 GASP `LevelTextured` 可见 mesh
（障碍 `SM_gasp_055`、地面 `SM_gasp_025`，无 materialOverride），
18/18 截图与标记命中，contact-sheet 1248×1626。

仓库内 21 场景 × 30/60/120 共 63 次：60=21/21、120=21/21、30=20/21，**62/63**。
30 Hz 的 `SC_gasp_catch_family_mantle` 两次都 `catch-missed-top`
（`progress=0`，缺 `P8_CATCH_LANDED`/`P8_TRAVERSAL_END`），y 峰值 224/234 后
回落到地面 y=90；60/120 同场景 `CATCH_LANDED y=288`、`END y=290`。
该失败保留，没有靠调参或改资产掩盖。

三档稳态帧率（15 s 窗口）为 59.05/29.00/118.10；`P7_PROBE` 每帧行数
60=1231、30=617、120=2349，证明渲染节拍生效（`P8_TRAVERSAL action=` 行按
0.25 s 蒙太奇时间节流，不是帧率证据）。

独立包已导出（21 个矩阵场景 + DB/PS，1810 files / 1,638,002,923 B，包内
`main.exe` 与仓库同一 SHA256）；同一二进制跑完三档：60=21/21、120=21/21、
30 首跑 20/21 + 重试 1 次后 21/21（唯一首跑失败 `catch-family-hurdle`
`ABORT stalled progress=184`，重试 PASS END 290/-200）。包内 20/30 s 采样
59.05/29.0/115.31（120 低约 4%）。包内 30 Hz `catch-family-mantle` 首跑通过，
未复现仓库内的 2/2 `catch-missed-top`；两个环境在 30 Hz 各暴露一例不同的
catch abort，60/120 两边零失败。因此 30 Hz 是 catch 入口/warp 窗口对启动与
帧相位敏感的脆弱点，P9 三帧率门禁仍未全绿。**P27 不是 M2/最终交付完成**：
三帧率门禁（30 Hz catch flaky）、PoseSearch B1、非 `Slerp` 旋转与 Montage
质量仍为后续项。详见
[P27 记录](../../rationale/gasp-p27-visual-frequency-record.md)。

### P28 UE 帧语义对齐

引擎 `dd62695`（`【Mod】align gameplay simulation with UE frame semantics`，23 文件
+207/−262）删除脚本固定步：`ScriptRuntimeSubsystem::Tick` 每帧一次
`tick(dt=frameDt*timeScale)` → `StepPhysics(dt)` → `late(dt)` →
`TickScriptAnimations(dt)` → `StepPhysics(0)`；没有 accumulator、没有 `fixed` 回调，
`frameIndex` 每帧 +1。`ScriptPhysicsWorld::Step` 成为唯一物理入口，角色
`MoveCharacter(dt)` 按 UE CMC `0.05 s`/8 次迭代分段，刚体保留 `1/60` 子步（最多 16、
余量保留/超限丢弃），`dt=0` 只同步指令/接触。`EngineLoop` 对游戏/动画消费
`min(rawDelta, 0.1 s)` 的 hitch 防护，FPS/资产轮询仍用原始 dt。C# 删除
`FixedTick`、`Time.FixedDeltaTime/IsFixedTick/FixedFrameIndex`、NativeBridge
`"fixed"` 命令与 callback 位。资源 `b75b2de` 把 `catch-high` probe y 210 → 260。

测试：`script_tests` **400/400**（401 删 3 个 fixed、加 2 个 per-frame，净 −1）、
`animation_preflight_tests` **255/255**、main 构建 0 error / 0 native warning。
运行时只重跑 6 场景 × 30/60/120 Hz：30 Hz **5/6**、60/120 **6/6**；`catch-high`
三档全过（chooser Falling Hurdle/120、LANDED/END 290），`catch-jump` 触发 height
87/78/77 只记录，断言放宽为家族/纵深 + LANDED/END 290。30 Hz
`catch-family-mantle` 仍 2/2 `catch-missed-top`，变帧 dt + clamp 未修复 warp 窗口，
是 P28 未闭环项。**P27 的 21 场景全矩阵是旧固定步语义，不能称为 P28 已重验。**
动画仍在 LateTick 后求值（root motion 一帧滞后）、渲染插值未做、UE 引擎级
`bUseFixedFrameRate` 未使用；**P28 不是 GASP M2 全部完成**。详见
[P28 帧语义记录](../../rationale/gasp-p28-frame-semantics-record.md)。
