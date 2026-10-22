# 动画播放与 GPU 蒙皮

> 状态：当前契约。描述片段播放、C# 动作筛选、原生局部 TRS 交叉混合、
> 冻结姿态与实时渲染；Motion Matching 搜索边界见轨迹与特征契约。

## 资产入口

资源仓库 P2 审阅关卡为 `Maps/SC_gasp/SC_gasp.json`。专属 SM、MI 和导出 FBX 分别
位于 `Meshes/`、`Materials/`、`Source/`；审计清单位于 `Generated/Import/SC_gasp/`。
关卡采用厘米、Y-up，角色 scale=1，带静态地面、方向光和固定 EV100 相机。
根位移保留在骨架内，循环时归零；没有施加给 Actor 或 Jolt。
默认片段 0 为循环待机，片段 1 为 Walk。Walk 四秒保留约 800 cm 根位移，
固定审阅镜头不跟随角色；检查走路姿态可暂停/seek 或使用预览相机重新取景。
角色材质是 VL PBR 审阅材质，没有导入 UE 材质图或纹理外观。

P3 可运行关卡为 `Maps/SC_gasp/SC_gasp_playground.json`，由独立 C# 角色控制，
使用原生 in-place 片段和 Jolt 胶囊位移；不改变 P2 的根位移审阅语义。
DefaultLevel 的静态 LOD0、源贴图、重建材质、静态高度网格与 Logo 接收平面
位于同一正式 Maps 目录；环境 HDR 引用正式 Source 文件，不依赖 Generated。
当前审阅设置为方向光 10 lux、环境强度 2、EV100=3，不声明 UE 光照等价。
资源转换边界见 [动画工具](animation-preflight-tools.md)，
验收证据与近似项见 [P3 记录](../rationale/gasp-p3-control-and-level-record.md)。

场景对象仍为 `type: "mesh"`，SM 仍为 `type: "mesh"`。SM 可选：

```json
"skinning": {
  "skeleton": "Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json",
  "clips": [{"id": "Walk", "path": "Maps/SC_gasp/Source/M_Neutral_Walk_Loop_F.fbx"}]
}
```

clips 是有序数组，id 不可重复，空数组表示绑定姿态。retarget 目标资产的
sections、render skeleton、palette、IK 和 bounds 始终属于目标网格；播放时由
`player.Asset().AnimationAsset()` 读取 source asset 的 clips/montages，再由预编译的
`CompiledRetarget` 把 source 局部姿态转换到目标骨架。该转换支持不同骨长和不同
rest axes，并消费 profile 可选的 `sourcePose` / `targetPose` 局部旋转校准。
省略时沿用两侧 bind pose；未映射的目标 joint 保持目标校准参考的局部 TRS 并继承父骨动作。
原始 bind/inverse bind 不改写；格式、右乘约定与根限制见
[资产导入契约](animation-asset-import.md)。它不等价于 IK 或接触修正，
后两者仍是独立的后处理阶段。

retarget 目标的 `skinning` 声明 `skeleton`、空 `clips`、可选空 `montages`、
`sourceModel` 和 `retarget`。source clip、Montage、catalog、预览片段选择均来自
source asset。Motion warp 的 Bone provider 仍按 source skeleton 和 source clip
解释；retarget 只改变渲染/目标 palette，不改变 source root-motion 和 warp 的语义。

静态 section 过滤及补法线
选项不能用于此路径。文档校验检查源文件与 clip 引用，完整骨架/权重校验在正式导入时执行。
标准资产修改 API 保留该扩展但不开放 skinning 字段编辑。
实时加载要求材质 `VL_SKINNED_MESH=1` 与模型蒙皮类型一致。
离线渲染明确拒绝骨骼模型。

## 时钟与快照

一个 Actor 的 sections 共享一个 `SkeletalPlayback`。GT 在脚本更新后、构建快照前
推进一次原生时钟；没有把 ozz context、播放器或 Vulkan 对象传到 RT。
普通 mesh 的播放独立于 C# Play/Pause/Stop；动画暂停使用本模块控制。
Seek、Select、Play 与零时长 TransitionTo 增加 discontinuity；
保留源根的循环归零也增加，in-place 循环及非零时长混合不增加。
Console/UI 的 seek 暂停，Select 保持播放开关；Play 与 TransitionTo 开启播放。
采样及所有 section 的 CPU 姿态完成后才发布选择、时钟与不可变 `SkinPose`。

`SkinPose` 包含模型局部 palette、变形顶点的精确 AABB、discontinuity 和 playbackIdentity。
World、WorldSnapshot、RenderScene 仅共享其 const 所有权；旧快照不会被后续采样改写。
单帧局部采样创建独立 ozz context；批量 `SampleModels` 在同一片段内复用 context，
先验证整批时刻，空批次仍校验资产。姿态按全部顶点/权重求 AABB；
AABB 采用列优先 3×4 标量仿射累加，避免 Debug 的逐顶点 Eigen 表达式开销。
尚未提供大规模角色性能保证。

## 原生交叉混合

`SkeletalPlayback::TransitionTo(clipId, seconds, loop, blendSeconds, inPlace=true)`
接收搜索结果，目标片段/时间立即可读，目标速率固定为 1。
`SetRate(speed)` 只验证并改变 `(0,4]` 内的有限播放速率，不重采样或改变
时刻、姿态、混合源和 discontinuity。目标时钟按速率推进，
混合进度按 scaled delta 推进，不随目标速率缩放。
非零时长过渡开始时 alpha=0，保留已经显示的局部姿态及旧 SkinPose；
随后 Tick 用同一原生目标时钟推进目标时间与混合进度。
暂停或 delta=0 时两者均冻结。一次性动画到达目标末尾后仍完成剩余混合，
两者均结束才报告 Completed 并停止推进。

目标经 ozz SamplingJob 求局部 TRS，BlendingJob 将冻结源和目标混合，
再由 LocalToModelJob 求层级；没有模型矩阵 lerp。
中断只冻结当前显示的一份局部姿态，不堆叠旧播放器链。
正常混合保持 discontinuity，post-blend 姿态继续参与逐帧速度历史。
Seek、Select 和 Play 取消已有混合；零时长 TransitionTo 为明确硬切。
所有 section 的 palette、模型关节和 bounds 成功后同批发布，采样失败保留活动状态。

in-place 在混合前将根的完整局部 TRS 恢复绑定值，保留 pelvis 相对根运动，
没有向 Actor/Jolt 再施加源根位移。平滑过渡若改变 in-place 根消费策略会被拒绝；
调用方须显式使用零时长切换，避免旧根位移在混合期间重新进入胶囊拥有的运动。
该方案为线性交叉混合，没有根偏移修正；可选足部接地在混合后独立求值，
可选惯性化则以有限时偏移替代同一过渡的线性混合。两者都只作用于后续过渡。

## 可选足部接地

`Animation.Match` 的 `footPlacement=false` 保留未修正行为。
启用时验证 `pelvis → thigh_l/r → calf_l/r → foot_l/r → ball_l/r` 直接父链、
非退化绑定腿及组件的刚性正均匀缩放；不兼容输入拒绝整个请求批次。
普通片段 Play 禁用该修正，普通模型预览不自动执行 IK。

`SkeletalPlayback::PreIkModels()` 和冻结局部混合源保存 post-blend/pre-IK 姿态，
MM 临时观察和实际关节历史只读取这份姿态。
`Models()`、渲染 palette 与 bounds 使用 IK 后结果。
求解器状态和全部 section 的 SkinPose 在候选求值成功后一起发布，
不改写旧快照、根策略、动画时钟、播放身份或 Actor/Jolt 位移。

当帧最终物理同步后，`ScriptPhysicsWorld` 通过纯值 `GroundProbe` 同步探测。
世界空间长度为厘米，只在 Jolt 边界转米；查询忽略 owner 与 trigger，
尊重 owner 双向碰撞层/掩码，最近遮挡若非静态支撑即返回未命中。
从形状内部起射不伪造可站立法线。每脚探测两个参考点：
绑定 ball 在绑定根水平面的投影，以及踝下的 heel 代理（骨架没有 heel 骨，
两者都转换到 foot 局部）；各从参考点上方 30 cm 向下探测 70 cm，
只接受法线 Y 至少 `cos(45°)` 的静态支撑。

进入锁定要求历史有效、该参考点 pre-IK 世界水平速度不超过 40 cm/s、
到平面距离不超过 10 cm。超过 80 cm/s、16 cm、锚点偏差 20 cm 或支撑身份改变时释放。
两点共面且都可锁时取更贴近地面的参考点，1 cm 内保留原支点；
另一参考点低 1 cm 以上时切换支点，跟随 heel→ball 滚动。
踏面不共面时只取更高/可锁的踏面作单支点，不把台阶连成虚构斜坡；
另一参考点仅在同一支撑面内做非穿透修正，跨踏面时允许悬在下一级踏面。
摆腿只命中探测面而未通过锁定门控时保留源动画。

锁定足以 ozz `IKTwoBoneJob` 修正局部腿旋转；两点共面整脚接触时把足部 up
对齐地面法线，单支点保留源脚 roll、只叠加地形倾斜。
视觉 pelvis 向下最多 15 cm、每秒最多变化 60 cm，并受腿长球面截面约束；
未锁足参考点低于其地面时限制下移量，避免摆动脚被整体压入地面。
数值噪声小于 `1e-4 cm` 的骨盆偏移归零。

UE 5.8 `FAnimNode_FootPlacement` 对照：IK 落点只写 `FootBone`（注释为 heel/ankle
joint），`BallBone` 是 secondary plant bone，参与 plant-plane 距离（foot/ball 取最小）
和 pivot；默认 `LockType=PivotAroundBall`，`PivotAroundAnkle` 可选，
自动判定 ball/ankle 支撑仍是源码 TODO。`FPlantResult` 只输出 `FootTranformCS`，
`BallTransformCS` 与 procedural toe rolling 均为 TODO。

本实现提供三个显式模式：`Auto`（默认，VL 扩展：按更低参考点与滚动选支点）、
`PivotAroundBall`、`PivotAroundAnkle`。三者都只写 foot 骨，ball/heel 仅作为
锚点/支点，不增加 toe IK 目标。实测 `Ball` 接近 UE 默认（走跑滑移与 Auto 同量级），
但 Run 有一帧 ball 不可锁而 heel 可锁的 1.33 cm 代理穿入；`Ankle` 在无 heel 骨、
仅踝下代理上会固定错误参考点，滑移/穿入明显恶化。因此默认保持 `Auto`，
Ball/Ankle 作为 UE 对照模式；真实鞋底与 toe roll 的限制与 UE 当前节点同类。

无锁定足时骨盆以 60 cm/s 回位，可能产生视觉突变，不是惯性化。
Pause/零 delta 冻结求值；自然 Completed 与显式暂停分开记录。
一次性动画结束后再显式暂停，修改 MM 许可也保持时钟与姿态冻结，显式恢复后才继续搜索。
关闭、直接 Play、Seek/Select/硬切清理锁定。
空中、无支撑、超过 0.1 秒的更新，或相邻有效观察位移超过 100 cm/
旋转超过 60°时重置。owner 失活或失去物理查询同样清理。
移动平台、真实鞋底网格与完整 UE FootPlacement 不在此实现范围；
跨踏面只作单参考点，不宣称整段踏面或完整周期覆盖。
`Animation.FootPlacement` 发布启用/活动状态、原因、骨盆偏移及双足世界空间几何，
每足包含 pre/post ball/heel、支撑身份、锚点、目标误差与 `contactMode`
（`ball`/`heel`/`none`）。原生 `ScriptAnimationFeedback` 的每足 JSON 另含
只读判据 `soleSpeed/heelSpeed/soleGap/heelGap/canPlantBall/canPlantHeel`，
供诊断“命中但未锁”的原因，不参与求解。
公共 DrawDebug 显示 probe、法线、pre/post 参考点和锁定锚点；
`FootPlacement.active` 时 pre/post 连线按红→绿分段插值，即使 Motion 轨迹开关关闭
也从 `Engine.Motion` publisher 提交，便于单独观察 IK 纠正方向。
实测边界见 [P6 动态质量记录](../rationale/gasp-p6-dynamic-quality-record.md)。

## 可选惯性化

`Animation.Match` 的 `inertialization=false` 保留线性交叉混合；
启用后只影响后续非零时长过渡，硬切、直接 Play、Seek/Select 不保留旧偏移。
过渡边界以显示姿态为源、1 ms 邻域采样估计两侧局部速度和角速度，
在局部父空间构造平移、SO(3) 旋转与 log scale 偏移，
用五次 Hermite 使起点匹配源位移/速度、终点值与速度精确归零。
角速度经 SO(3) 左雅可比搬运，不直接相减；目标时钟仍唯一，不推进第二个动画时钟。
重复中断以当前显示姿态重建差值；所有 section 仍在候选成功后一起发布。
暂停/零 delta 冻结偏移求值，`IsBlending()` 同时包含线性混合与惯性化。
惯性化会改变基础姿态与后续查询，这是开关的正常行为，不能要求两侧查询逐帧相同。
它不写 Actor/Jolt，也不改变根消费策略。

## 可选视觉根偏移

`Animation.Match` 的 `rootOffset=false` 不产生视觉根修正。
启用时只补偿实际水平速度与 yaw 速度的跳变：每个轴用临界阻尼解析解，
半衰期 0.08 s，世界 XZ 平移最多 20 cm、yaw 最多 45°；匀速不持续注入误差。
修正从 `displayedLocals` 生成视觉根姿态，`PreIkModels` 单独保留不含根修正的
查询姿态；渲染 palette 与 bounds 使用修正后结果。
硬切、Seek/Select、传送、root-motion、owner 失活、失去地面查询、暂停和关闭清理。
它不写 Actor/Jolt，也不改变 Motion Matching 的下一帧输入。


## GPU 数据

静态 Vertex stride 不变，蒙皮使用对象级独立 SSBO：

| 位置 | 数据 | 所有者 |
| --- | --- | --- |
| Set 2 binding 1 | current/previous mat4 交错，每骨 128 bytes | 对象 GPU 资源包，逐 swapchain image |
| Set 2 binding 2 | CSR offsets + joint/floatBits(weight) words | Renderable 的不可变权重流 |

CSR 头是每个展开顶点的绝对 word 起止偏移，尾部每条影响占两个 uint。
shader 根据 gl_VertexIndex 遍历全部影响，包括九影响顶点；不截断为四或八骨。
palette 为 `modelJoint * inverseBind`，保留全部原始正权重。
法线取混合线性矩阵逆转置，切线变换后正交化并保留 determinant handedness；
奇异/非有限混合在 CPU 求姿态时拒绝，shader 不作逐顶点兜底。

Base 与 ShadowDepth 先执行同一 `skinnedVertexFactory.glsl`，再执行材质求值。
SkinnedMesh key 参与组合 shader 身份，并强制专用 ShadowDepth；
当前拒绝材质 WPO、standalone Base 和完整 standalone shadow override。
buffer 写入发生在对应 image fence 完成后，资源随现有对象包/GPU epoch 退休。

## 已提交历史

previous palette/model 来自最近成功 SubmitFrame 的绘制，不来自最近一个 GT 快照。
被 mailbox 丢弃的快照和 acquire 失败不会推进历史。
首次绘制、discontinuity 或 playbackIdentity 变化、隐藏后重现或离线返回会以 current 初始化历史。
相机 previous VP 同样在成功提交时推进，World 换代重置。
GBuffer velocity 使用 previous skinned position；没有新增附件或改变通道分配。

## C# 片段控制

`MeshComponent.Animation` 提供 `Clips`、`State`、`Play(clip, loop=true, rate=1, inPlace=true)`、
`PlayMontage(montageId, targets, loop=false, rate=1, inPlace=true, startTime=0)`（两个重载：
`Dictionary<string, Vector3>` 只提交位置，`Dictionary<string, WarpTarget>` 提交位置 +
可选世界旋转；两者都有 `startTime` 入口秒）、`Montage(id)` 与
`CurveValue(clipId, curveName, time)`。C# 只请求片段/Montage 与曲线采样，不持有骨骼姿态、
第二份时钟、warp 窗口或旋转数学。
可用片段目录由正式加载/预加载的原生模型发布；动态资产准备成功后刷新目录。
State 的片段、时间、时长、完成及播放标记来自原生反馈，在后续脚本回调前同步。
请求要求当前线程、存活组件及活动 gameplay 回调；OnCreate/OnReload/EndPlay 拒绝。
片段名与有限速率 `(0,4]` 在请求入口检查，回调异常回滚未交付请求。

`State.RootMotion` 与 `State.RootMotionYaw` 是本 tick 的根运动反馈：平移来自片段根轨道，
可被原生 MotionWarping 的 SkewWarp 平移改写；`RootMotionYaw` 默认是原始根 yaw，若当前
tick 有 `warpRotation=true` 且 target 提交了世界旋转的窗口，则替换为该窗口的 yaw-only
warp 结果。C# 控制器在位移之后把 `RootMotionYaw` 单独加到 Actor yaw，不把它混入速度
（资源 `GaspCharacter.blueprint.cs` 的 `TickTraversal`）。

Catch 也走同一 `PlayMontage` 提交面：C# 在 `MovementMode=Falling` 时播放
`Catch_Hurdle/Mantle/Vault` 家族（Chooser 按顶面纵深选，或显式 action）与
`Catch_Cliff_high`；入口时间是第一个 `PoseSearchBranchIn` notify 的 `Time`（无则 0），
作为 `startTime` 提交，之前的下坠前摇不执行。targets 的 `BackFloor` 仅 Hurdle Catch
有窗口，按顶面落点解释（有意偏离地面 Hurdle；见
[P24 记录](../rationale/gasp-p24-catch-runtime-record.md)、
[P25 记录](../rationale/gasp-p25-catch-entry-record.md) 与
[P26 记录](../rationale/gasp-p26-chooser-families-record.md)）。
`P8_MONTAGE_START` 在 Catch 行额外输出
`startTime=... entry=... window=[t,end] frames=... entrySource=window-min`
（非 Catch 行保持旧文本）；`P8_CATCH_GATE` 在进入 Falling 时输出
`jumpInitiated/catchOnFall/catchOnWalkOff`。

自动抓边的门控在 C# 控制器：`jumpInitiated` 由 Jump 状态/输入置位、落地清零；
跳跃失败受 `CatchOnFall`（默认 true）控制，走落受 `CatchOnWalkOff`（默认 false）控制；
`TraversalProbe=true` 的探针场景不跑自动触发。`PlayMontage` 接口与 wire 不变。

同一 Actor 一批最多 128 个不同组件请求，全部候选采样完成后才提交。
一个组件的全部 sections 共享一个时钟；独立组件和 Spawn 使用独立播放器，共享只读资产。
稳定 ID/模型/材质保持时，添加兄弟组件保留已有时钟。
同 ID 组件在同一导出边界内删除再重建尚未携带原生代际身份，不能据此保证重启时钟。

受控骨骼网格只在脚本路径推进一次，按 scaled delta 采样；
Pause、不活动、销毁或故障 owner 不推进。
Play/Stop 重建受控时钟及 playbackIdentity，避免旧提交姿态污染新实例速度历史。
Playing/Paused 且含受控骨骼模型时，程序集热重载在候选替换前明确拒绝，需先 Stop。

`inPlace` 移除根相对绑定姿态的完整 TRS，保留 pelvis 相对根的运动；
胶囊/Jolt 独占 Actor 位移与朝向。循环、一次性结束、速率与完成反馈由原生播放器拥有。
`Play` 保留直接换片段语义；Motion Matching 结果通过上述原生 TransitionTo 混合。
可选接地由上述原生路径执行。

Sequence/Montage notify 由原生播放器执行：瞬时 notify 在跨过其时间点时触发一次；
notifyState 是 `[time, time+duration)` 区间，进入触发 `begin`、离开触发 `end`。
原区间不裁剪，可越过片段/播放范围末尾；在 Stop、切片段、TransitionTo、循环回绕或
一次性播放到达播放范围末尾时，仍活动的区间补 `end(interrupted=true)`。循环回绕按
“先尾部、后头部”处理，跨环区间得到 `end→begin`。播放范围只限制播放长度、根位移
积分与 notify 有效域，不改变姿态采样；显式 `Seek` 清空活动区间且不补发被跳过的
事件。

事件经原生 `FiredNotifies` 逐 tick 发布：`World::ScriptAnimationFeedback()` 把它转成
feedback 的 `notifies[]`（`name/track/kind/phase/time/startTime/duration/interrupted`），
并按播放器发布 `montage{id,length,range:[start,end]}`；C# 对应 `AnimationState.Notifies`、
`State.Montage`、`State.MontageLength`、`State.MontageRange`。反馈在脚本 tick 之前发布，
因此 C# 读到的是上一动画 tick 的事件。Montage 的 sections/notify/catalog 与
`PlayMontage` 入口见 [动画 Montage：数据与播放](animation-montage.md)；动画曲线通过
catalog 键在 C# `CurveValue` 中线性采样，播放器不执行曲线（原生 `SampleFloatCurve`
仍是同一规范的参考实现）。

`Match(database, allowedClips, actionLocked=false, matchSpeed=false, footPlacement=false,
inertialization=false, rootOffset=false)` 提交正式数据库、业务允许片段、动作锁、
稳态步频、足部接地、惯性化与视觉根偏移开关，
不允许 C# 指定搜索选中的帧或维护播放时钟。`Motion` 是原生有限值反馈，
包含数据库身份、查询就绪、决策原因、搜索次数和选中片段/时间；
完整查询、候选及分项代价由不可变原生 Motion 调试快照发布。
`State.Rate` 为原生当前速率；`Motion.SpeedMatching` 发布启用/活动状态、
源与实际水平速度、目标与当前速率、有符号速度残差和退出原因。
数据库/query 版本绑定、无候选及继续播放策略见
[轨迹与特征数据库](animation-motion-features.md)。

编辑器 Motion 面板为被聚焦脚本网格提供只读时间轴：时长、当前时间与播放状态
来自 `MotionDebugView` 的原生播放器，play/pause/seek 只提交该网格身份的
`ControlScriptAnimation`（intValue 0=seek、1=play），不遍历或改写其它角色。
时间轴的当前时间以播放器播放范围起点为零点，播放器内部仍以 clip-local 绝对时间
驱动 notify、warp 与 section 链；seek 先 `SetPlaying(false)`，由 World 把相对秒数
换算为绝对时间后再 `Seek`，使 MM 下一帧不能覆盖定位；play 显式恢复。
拖动期间不覆盖焦点控件；离线、无控制器或时长为零时禁用且不产生命令。
时间轴不维护第二份时钟，也不替代 Motion 候选/查询显示。

Motion 面板另提供查询回放：World 保留被聚焦组件最近 600 帧的冻结
`MotionDebugView`（查询、候选、代价、轨迹线与播放状态）。Replay 进入时暂停
该组件播放器，只按历史帧索引选择快照，不再追加；退出 Replay 返回实时快照。
回放不重新搜索、不修改 Actor/Jolt，也不把历史候选写回播放器；切换
`componentIdentity` 或 World 换代时历史清空。回放选择只改变 UI/调试视图，
角色姿态保持暂停时刻，不宣称历史姿态重演。

托管 `StateMachine<TState>` 使用显式条件、降序优先级和注册顺序稳定决胜，
每次 Update 最多一次迁移。正式 gameplay 源码位于资源仓库
`Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs`，实现
Idle/Walk/Run/Jump/Fall/Land、第三人称跟随与障碍扫球缩臂。
地面状态限制为 Walk 或 Run 动作族，族内保留待机、起步、停止及左右转身供原生数值搜索；
空中和落地状态限制相应动作族，Jump/Land 提交动作锁。
`Character.LinearVelocity` 来自每帧角色实际位移除以帧 dt；零时步同步帧保留最近一帧结果，
碰墙后不把走路意图当实际速度。

正式 C# 控制器按当前 Actor yaw 向输入方向作有界最短角转向：
`TurnSpeed` 默认 `540°/s`，每帧角度增量不超过 `TurnSpeed * scaledDelta`。
角差归一到 `(-180°, 180°]`，恰好反向固定选择正向旋转；没有输入时保留朝向。
`Character.Move` 立即使用相机空间输入方向，身体转向不延迟移动意图；
相机局部 yaw 补偿 Actor yaw，使无鼠标输入时的世界观察方向保持独立。
这控制整个 Actor 的世界朝向，与上述 `0.18 s` 局部姿态交叉混合分属两层。
当前近似未来轨迹仍沿当前朝向发布朝向帧，不预测后续旋转；
移动方向先改变而身体尚未转到目标时，仍可能短暂侧滑或倒滑。
正式角色另开放默认关闭的 `MatchSpeed` 实验对照属性，改变开关会重新提交 Match，
HUD 显示当前速率与剩余速度差。原生只对方向一致的地面稳态移动循环作有界时间缩放；
混合、转向未对齐、动作锁或空中退出，详细边界见轨迹与特征契约。
根行程匹配不保证足底锁定，实测限制见
[P6 步频记录](../rationale/gasp-p6-speed-matching-record.md)。
正式角色另开放默认关闭的 `FootPlacement`、`Inertialization` 与 `RootOffset`
实例属性，改变开关会重新提交 Match；HUD 读取原生接地原因、pelvis 偏移与根偏移状态。
这三个修正都不改变 Actor/Jolt 位移，也没有实现骨骼步幅变形；
转向缺陷的独立验收记录见 [P6 有界转向](../rationale/gasp-p6-bounded-turn-record.md)，
动态接地与视觉修正的实测边界见
[P6 动态质量记录](../rationale/gasp-p6-dynamic-quality-record.md)。

## 预览与调试

双击骨骼 SM 使用既有模型资源标签和独立预览 World，标题显示 Skeletal Mesh，
提供播放/暂停图标、有序片段选择与时间 slider。静态标签隐藏这些控件。
双击 `RT_*.json` 则打开只读 Retarget 参考姿态页，按显式 SK 预览引用显示绑定/
校准骨架、关键关节局部轴和位置/方向测量；计算复用 `CompiledRetarget` 的
`SourceReference()` / `TargetReference()`。
配置 `preview.targetModelPath` 后默认进入 Animation，源/目标真实网格在同一预览
World 内按厘米尺度并排显示；目标 SM 必须绑定当前 RT，源从其 `sourceModel` 取得。
片段、播放/暂停和 seek 同时控制两边，各 section 共享所属模型的原生播放器；
`previewClip` 使用正式 `Play(..., inPlace=true)`，避免根运动使模型走出视野。
该命令只接受带成对源身份的 `SC_model_preview`；普通 `clip` 的根运动语义不变。
Reference Pose 切换保留参考检查，静态测量不随动画变化，也不冒充动态关节误差。
预览不执行足底 IK/接触修正；源与目标各自一个时钟，使用同一 World delta 与控制请求同步。
具体输入与测量边界见 [UI 组件体系](ui-component-system.md#retarget-参考姿态标签)。
UI 只发 generation-gated 值请求，GT 验证；刷新控件不产生业务命令。
预览按厘米 scale=1 放置，静态预览的既有比例不变。
关卡内动画可通过 [调试命令](debug-runtime-commands.md) 暂停、seek、切片段。
播放器在姿态提交时同步发布不可变 pre-IK 模型空间关节矩阵，供姿态历史读取；
不把 ozz context 暴露给 UI。骨骼预览展示当前帧 raw 特征，
受控角色支持实际根/姿态历史、近似未来轨迹和同空间查询。
其采样阶段、边界、缓存与限制见 [轨迹与特征数据库](animation-motion-features.md)。

模块验证入口及实测记录见 [测试契约](testing.md)；
CPU 导入空间与数据边界见 [导入契约](animation-asset-import.md)。
`SkeletalBlend` 的三关节手算夹具覆盖局部平移/四元数/缩放和父链、
中断、端点、Pause/0dt、完整根 TRS、epoch、失败保持及旧快照；
测试存在不代替正式 GASP 起停/转向的行为与实景验收。
