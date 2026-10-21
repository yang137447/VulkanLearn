# GASP P0 资产基线记录

日期：2026-10-18（本地）。这是执行证据，不是实现契约。
继 [P0 启动记录](gasp-p0-startup-record.md) 后，用户提供了实际项目路径，
并说明原项目不会改动。本轮继续执行
[GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md) 的资产基线。

## 结果与范围

**固定角色和最小动画集的导出、源空间数值对照通过；P0 整阶段仍为部分完成。**
本轮有可查看的 UE/FBX 骨架投影、数据报告和可重复工具入口。
没有运行 UE PIE 行为矩阵，也没有 VL 蒙皮角色、C# 动画状态机或 Motion Matching。
P1～P9 不声明完成。

引擎基准 HEAD：`14a6cad4769c3b8dde4446fe7c701cc2997051a7`，
预检工具为本地未提交改动；没有建立新的引擎提交。

## 输入与隔离

| 项目 | 记录 |
| --- | --- |
| 用户原项目 | `D:/YYBWorkSpace/UnrealProject/gasp/gasp.uproject` |
| 描述版本 | EngineAssociation `5.8`；Category `Samples`；EpicSampleNameHash `1673045636` |
| 实际导出编辑器 | `5.8.2-56702186+++UE5+Release-5.8` |
| 输入盘点 | descriptor 与 Content/Config 等盘点范围内共 3902 个文件 |
| 输入 BLAKE3 | `8f2fba8d398635082bd06428d6d971c828573f9613c4cd905a8b9fc204e40afe` |
| Content 副本 | 3898 文件，5,842,194,132 字节；另复制 descriptor 与 Config |
| 副本路径 | 引擎 `artifacts/gasp-p0/ue-project-20261018/gasp.uproject` |
| 副本身份 | 独立盘点得到相同文件数和相同输入 BLAKE3 |
| 审计输出 | 引擎 `artifacts/gasp-p0/ue-audit-06/` |

UE 命令行始终加载副本，没有对原项目调用保存或升级；
工具也不调用资产保存。副本生成了 Saved/Intermediate/DDC 等派生数据。
完整项目源代码 revision、Fab 下载内容使用范围仍未确认，不能把目录名、
EpicSampleNameHash 或本机成功打开当作来源/许可凭证。
官方页面检索本轮没有取得可核查内容；没有据此认定资产属于某一种许可。
导出仅留在引擎 gitignored 的技术审计目录，没有提升到正式资源仓库。

## Asset Registry 与行为矩阵

实际 UE Registry 返回 3897 个资产，不以文件名前缀猜测类型。
其中 AnimSequence 2008、AnimMontage 137、PoseSearchDatabase 155、
PoseSearchSchema 33、ChooserTable 15、AnimBlueprint 20、
SkeletalMesh 20、Skeleton 11。统计包含整个项目内容及重定向资产，
不是本轮导出闭包大小。

初始参考选 **CMC**：与当前 VL 的 Jolt 胶囊角色移动路径对应。
项目同时包含 Mover/ChaosMover/Locomotor 路径，不在本轮扩大实现范围。
下表“存在”仅表示读到资产/配置，不等于操作行为验收：

| 能力 | 本轮证据 | 执行状态 |
| --- | --- | --- |
| 站立/蹲伏待机 | 两段 Idle Loop | 已导出/对照 |
| 走/跑/冲刺 | 三段前向 Loop | 已导出/对照 |
| 起步/停止 | Walk Start/Stop，Lfoot | 已导出/对照；不代表全方向动作已齐 |
| 原地转身 | Stand Turn 180 L/R | 已导出/对照 |
| 起跳/下落/落地 | Stand Start、Loop Fall、Stand Light Land | 已导出/对照 |
| 蹲伏移动/碰撞 | Crouch 目录与对应数据库 | 仅资产存在，未导出完整集或执行碰撞 |
| Hurdle/Mantle/Vault | CMC Traversal Chooser 的嵌套表与 Montage 引用 | 已保留文本，未执行障碍动作 |
| Catch/Climb 等扩展 | Traversal 资产目录 | 仅登记，未定为本轮完整动作承诺 |
| UE 运行行为 | CMC AnimBlueprint 文本 | 未运行 PIE、未录操作对照片段 |

CMC AnimBlueprint 文本包含 MotionMatching、PoseSearchHistoryCollector、
BlendStack、OffsetRootBone、ResetRoot、OrientationWarping、Steering、
FootPlacement、LegIK 等节点声明。
声明存在不能证明节点在每个分支均启用，也不构成 VL 对应功能完成。

## 固定角色与最小片段

角色：`/Game/Characters/UEFN_Mannequin/Meshes/SKM_UEFN_Mannequin`；
骨架：同目录 `SK_UEFN_Mannequin`。
RAW 参考姿态共 93 个节点：91 根物理骨骼，以及
`VB hand_l_prop_01`、`VB hand_r_prop_01` 两根虚拟骨。
虚拟骨单列，未冒充 FBX 物理节点。

下表均位于 `/Game/Characters/UEFN_Mannequin/Animations/`；
时长四舍五入显示，JSON 保留原值。

| 片段 | 秒 | float 曲线数 | notify 数 |
| --- | --- | --- | --- |
| `Idle/M_Neutral_Stand_Idle_Loop` | 10.000000 | 3 | 1 |
| `Idle/M_Neutral_Crouch_Idle_Loop` | 10.000000 | 3 | 1 |
| `Idle/M_Neutral_Stand_Turn_180_L` | 2.166667 | 5 | 6 |
| `Idle/M_Neutral_Stand_Turn_180_R` | 2.166667 | 5 | 6 |
| `Walk/M_Neutral_Walk_Loop_F` | 4.000000 | 5 | 9 |
| `Walk/M_Neutral_Walk_Start_F_Lfoot` | 3.466667 | 5 | 8 |
| `Walk/M_Neutral_Walk_Stop_F_Lfoot` | 2.966667 | 3 | 9 |
| `Run/M_Neutral_Run_Loop_F` | 2.500000 | 5 | 9 |
| `Sprint/M_Neutral_Sprint_Loop_F` | 2.000000 | 5 | 9 |
| `Jump/M_Neutral_Jump_F_Start_Stand_Lfoot` | 2.500000 | 5 | 2 |
| `Jump/M_Neutral_Jump_Loop_Fall` | 3.333333 | 5 | 0 |
| `Jump/M_Neutral_Jump_F_Land_Stand_Light_Lfoot` | 2.766667 | 5 | 5 |

每段保留 FBX、JSON 侧车与 UObject `.copy`：
88 条作者轨道、float 曲线时间/值、notify 的轨道/时间/时长/结构、
同步标记、60 Hz 根轨道，以及含两端点的 5 个 RAW 骨骼参考帧。
`.copy` 补充 notify/state 的内嵌配置，不能只保存对象引用名称。
例如 Walk Loop 含 `contact_l`、`contact_r`、`movedata_speed`、
`enable_warping`、`Phase` 曲线，以及 PoseSearchBranchIn 与 Foley 事件。
曲线切线/插值的 VL 等价求值、事件语义与 Foley 功能没有实现或验收。

## Schema、数据库与 Chooser

已读取/导出 `PSS_Default`、`PSS_Idle`、`PSS_Jump`、`PSS_Traversal`，
Sparse Stand Walk 的 Loops/Starts/Stops 三个数据库，
`CHT_PoseSearchDatabases`、`CHT_TraversalMontages_CMC`，
以及 `SandboxCharacter_CMC_ABP`。

`PSS_Default` 的实际属性：

- SampleRate 30；单一 mannequin skeleton，无 MirrorDataTable。
- DataPreprocessor 为 `NormalizeWithCommonSchema`，NumberOfPermutations 为 1。
- Trajectory 样本偏移为 -0.05、0、0.35、0.7、1.0 秒；
  不同样本的 Flags/Weight 保留在侧车，不能按同一种位置通道理解。
- 姿态组含 foot_l 相对 foot_r 的位置、左右脚速度，以及 pelvis 的 Y 朝向。
  对应权重为 1、0.3、0.3、0.1，骨骼速度与轨迹速度的空间设置并不相同。

Walk Loops 数据库包含 11 项，引用 `PSS_Default` 和 `PSN_Sparse_All`，
成员含多方向循环、shuffle 与 Run-to-Walk；源文本含 `bDisableReselection`。
最小导出的 12 段动画不是该数据库的完整成员闭包。
根数据库 Chooser 按 MMDatabaseLOD 转入 Dense/Sparse/ExtremeSparse 子表，
fallback 指向 Dense；这里只保留参考，不据此让 VL 搜索越过 C# 允许集合。

结构化属性审计保留了 11 个 unavailable 字段：
8 个来自 PSS_Idle 的复合 Pose channel 不具备通用单骨属性，
3 个来自数据库私有 `DatabaseAnimationAssets` 的 Python 访问。
数据库成员和复合通道仍在完整 UObject 文本中可核对；
没有把 unavailable 当成空成员，也没有声称已完成通用 UE schema/Chooser 转换。
Trajectory 的 transient SubChannels 在本机 Python 未暴露，使用作者 Samples 与文本审阅。

## 坐标、绑定与数值验收

导出显式保留根轨道，不将 Actor 运动再次映射到根，不附带动画预览网格。
FBX 文件 UnitScaleFactor/OriginalUnitScaleFactor 均为 1，Z 向上，
FrontAxis 为 -Y。此次测量固定采用
`FBX = (UE.X, -UE.Y, UE.Z)`、厘米单位，没有拟合基底或放宽误差阈值。

**源动画的前向不能直接假定为 UE Actor 的 +X。**
选定 Walk_F 原始根位置从 `[0,0,0]` 到 `[0,800.0072021484375,0]`，
历时 4 秒，沿源骨架 +Y 前进；FBX 中相应为 -Y。
最终 VL 坐标、单位、模型组件朝向与角色朝向的归属尚未验收。

Assimp 预检禁用 axis correction、pivot 保留和空骨删除，不启用 GlobalScale，
不截断/归一化权重。含网格 FBX 的 94 个节点和纯动画的 92 个节点包括不同包装节点，
使用节点/父索引保留同名 armature 与 mesh 的区别。

| 核对项 | 结果 | 预设门槛 |
| --- | --- | --- |
| 网格参考骨架位置最大差 | `0.000018421642214740126 cm` | 0.1 cm |
| 网格参考骨架旋转最大差 | `0.000013476315436940451°` | 0.1° |
| bind 平移残差最大值 | `0.00004486200405405333 cm` | 0.1 cm |
| bind 线性矩阵残差最大值 | `0.00000033957906242498836` | 0.0001 |
| 12 段 × 5 帧 × 91 根物理骨 | 5460 个骨骼姿态 | 全部有模型空间对照 |
| 片段姿态位置最大差 | `0.022211555939196412 cm` | 0.1 cm |
| 片段姿态旋转最大差 | `0.024504844920257358°` | 0.1° |
| 60 Hz 根位置最大差 | `0.00010562856004980858 cm` | 0.1 cm |
| 时长最大差 | `0.00000007947285984855057 s` | 0.00001 s |
| 作者轨道缺失 | 0；每段保留 88 条 | 不允许丢作者轨道 |

三根 `props_root`、`prop_01`、`poi` 在源片段本就没有轨道：
按绑定姿态回退，仍参与上面的 91 骨误差比较。
此前对照 04/05 因错误要求“每根物理骨都必须有动画轨道”判失败；
最终改为按源作者轨道核对，并保留缺轨骨的数值检查，未放宽数值阈值。

Assimp 网格包含 27972 个未合并顶点、9324 个面；
无无效权重、无未加权顶点，最大权重和误差 `0.00000004284083843231201`。
**最大骨骼影响数为 9**。后续 P1/P2 不能无记录地截断为 4 或 8；
本轮没有验证顶点蒙皮、MikkTSpace 重排后的权重映射或 GPU palette。

## 验收入口与失败记录

工具与命令见 [Animation Preflight](../../tool/animation/README.md)。
本轮使用 UE PythonScript commandlet、AllowCommandletRendering/RenderOffscreen，
关闭材质烘焙，未导出或转换 VL 材质。
最终 UE `ue-audit-06` 退出码 0，`audit.json` 状态 `asset-export-complete`；
这只指固定导出列表成功。

引擎根目录下的验收文件：

- `artifacts/gasp-p0/ue-audit-06/audit.json`：最终资产与导出状态。
- `artifacts/gasp-p0/ue-audit-06/skeleton.json`：骨架、父链与参考变换。
- `artifacts/gasp-p0/fbx-inspection-06/`：13 个 FBX 的 Assimp 数据。
- `artifacts/gasp-p0/ue-fbx-comparison-acceptance.json`：最终对照，passed=true。
- `artifacts/gasp-p0/ue-fbx-walk-acceptance.png`：5 个时刻的根相对 YZ 投影；
  绿色 UE RAW 与粉色 FBX 虚线重合。同一循环可能采到相同步态相位，
  这是数据对照图，不是 UE 操作录像或 VL 渲染。
- `artifacts/gasp-p0/gasp-inventory-20261018.json` 与
  `gasp-isolated-inventory-20261018.json`：源输入/副本摘要。
- `artifacts/gasp-p0/ue-audit-01`～`06` 及同名前缀日志：过程证据。

失败没有覆盖：
01/02 的 NullRHI 骨骼导出触发 UE `SkinnedMeshComponent::MeshObject` 断言，
02 即使关闭材质烘焙仍失败；03 因 Notifies 是 protected，改用公开按轨道查询；
05 的 transient/private schema 属性访问失败，06 改为明确记录不可访问范围并保留文本。
04 成功导出最小列表；06 增加完整动画文本与结构化分组审计后成功。

最终可选工具和模块测试构建通过，18 项根 GTest 全通过；
Python 脚本语法、65 条架构边界、文档分层及 diff 空白检查通过。
没有运行全量根 CTest、VL 实窗或三个 Vulkan runtime test；
本轮没有改引擎动画运行时、shader、正式资源或 C# API。

## 尚未验收

P0 余项为实际内容来源/使用范围、UE 操作对照片段及完整行为矩阵、
最终 VL 轴向/单位/角色组件映射。正式资源提升以前继续保留该范围界线。
P1 可使用这批隔离数据推进骨架/绑定/权重/片段导入，
同时补缺轨、虚拟骨和顶点重排回归；不能以当前源空间比较替代 VL 导入验收。
