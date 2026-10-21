# 动画轨迹与特征数据库

> 状态：当前契约。实现轨迹/姿态历史、特征提取、确定性数据库、运行时精确
> Motion Matching 和冻结候选/代价显示；过渡使用交叉混合，查询与可选接地 IK 隔离。

## 空间与通道

`source/animation/motionFeatures.*` 为数据库与运行时查询共用编码。
schema 类型为 `motionFeatureSchema`、version=1；厘米、Y-up、模型 +Z 前向。
`root-heading-cm-yup-zforward` 以当前根位置为原点，仅消除当前根 yaw，
不把根 pitch/roll 带入 heading。位置为 cm，速度为 cm/s，朝向为单位方向的 XZ。

有序通道先按 trajectoryTimes 排列，每个时间含 position.x/z、facing.x/z；
然后按 joints 排列，每关节含 position.xyz、velocity.xyz。
正式 `Maps/SC_gasp/MotionMatching/PS_gasp.json` 使用
`[-0.4,-0.2,0,0.2,0.4,0.6]` 秒及 pelvis、foot_l、foot_r，共 42 维。
骨骼预览使用默认配置；没有这些骨骼的通用预览夹具回退到根骨，
不保证其维度与 GASP 数据库一致。受控角色使用所绑定数据库的 schema，
由数据库和模型共同解析特征骨骼，不把预览的默认配置冒充正式查询定义。

关节速度用 velocityStep=1/60 秒的**向后差分**：
先对世界/模型全局关节位置求位移速度，再旋转到当前 heading；
不得先逐帧减根后差分，否则丢失角色平移速度。
schema 标注 `post-blend-pre-ik`；实际姿态历史与搜索前临时观察均采集
播放器 `PreIkModels()` 的交叉混合结果，不含可选接地 IK。
渲染用 `Models()` 与这份历史独立。播放、混合与姿态所有权见
[动画运行时](animation-runtime-skinning.md)。

非循环动画只有过去/未来轨迹及速度差分窗口完整的帧才入库。
循环片段按首尾根的完整 SE(3) 周期变换延拓，支持负周期；
不跨到下一条片段，不仅累加根平移。非刚体循环根拒绝延拓。
入库采样为固定 sampleRate（正式为 30 Hz）、`frame/sampleRate`，不重复时长终点。
窗口过短的成员可无帧，整个数据库无完整帧则拒绝。

均值与总体方差以固定成员/帧顺序、双精度 Welford 累积；
方差 ≤1e-12 的通道 scale=1，其余为总体标准差。
`Normalize` 执行 `(raw-mean)/scale`，拒绝维度、非有限值和 float 溢出。
四组权重单独展开，不参与统计或归一化。
正式 GASP schema 的 trajectoryPosition=20，其余三组为 1；权重属于源 schema，
修改后随输入身份使旧缓存失效，不使用固定摘要绑定资源。
距离为 `sum(weight[i] * (normalizedQuery[i] - normalizedFrame[i])²)`，
以 double 累积，并分别保留 trajectory 与 pose 分项。

## 运行时绑定与搜索

`motionRuntimeDatabase.*` 是工具和运行时共用的输入捕获、缓存和加载入口；
`tool/animation/motionDatabaseBuild.*` 只保留工具命名空间适配。
首次请求按正式资源相对路径加载，核对模型、片段输入字节与骨架名字/父关系，
拒绝把同路径下已经改变的 FBX 与旧播放器混合。World 缓存持有 const 资源，
数据库的输入身份、payload 身份、schema、统计和片段映射一起保持存活。
实际骨骼导入资产的 `sourceInputs` 记录与描述解析同次读入的 JSON 摘要，
以及导入前的模型/片段 FBX BLAKE3；导入完成后重新核对整份清单才发布资产。
绑定另逐行核对数据库帧位于实际片段时长及完整特征窗口内。
首次 Match 同步核验磁盘缓存，未命中则冷构建；World 按数据库源、模型及导入输入身份
缓存该 const 绑定，同一缓存绑定的后续请求和逐帧搜索不读磁盘。
本实现每个受控组件绑定单个数据库源；C# 提供其 clip ID 子集，
没有把不同统计的多个数据库合并搜索。
正式 GASP C# 状态机从 Jolt 实际水平速度判断 Idle（<2 cm/s），
Idle 只允许 Idle/WalkStop；Walk/Run 保留各自较宽的移动/起停/转身候选族。
因此持续向墙输入但实际停止时，业务层不再允许起步/行走候选；
原生仍在其收到的许可族内计算选帧，不按输入键名或片段名硬选动作。

`FreezeMotionDatabase` 按值接管并校验数据，创建 const 搜索资源及预归一化帧。
`BindMotionQuery` 核对完整 schema，并使用该资源的统计归一化 raw query。
查询保留共享资源所有权；入口拒绝维度、非有限值、归一化 float 溢出、
无效策略和非有限时间。`MotionQuery` 是公开值类型，调用方按此绑定入口生产，
搜索边界仍检查其绑定、维度与有限值；没有对任意手工改写值提供不可伪造保证。

`SearchMotionDatabase` 精确扫描允许成员，不依赖近似索引。
空允许集合或仅包含未知 clip ID 的集合返回 `no-allowed-candidates`，
不扩大业务允许范围。C# 请求桥接还会拒绝数据库外的 clip ID。
同成本按 clip ID 字典序、成员索引、帧索引稳定决胜；
筛选集合顺序不参与决胜。topK 为 1～32，运行时默认 5；
有界候选表保留按代价排名的拒绝项，选中结果和继续候选另外发布，
因此不能假设 topK 的第一行一定是实际选中结果。

继续候选由当前数据库中的 clip ID、播放器实际时间和时长重新定位。
正式运行时使用 `ExtractClipFeatures` 提供准确时刻的 raw 特征，
并用当前数据库统计计算继续代价；不会复用上一数据库的归一化值。
未提供准确 raw 时才使用最近库帧，误差不得超过半个采样间隔，
并发布 `quantizationSeconds`；不借远处帧或越出真实片段时长的行保持播放。
非循环的准确继续也要求完整过去/未来窗口。
准确的半帧继续不删除不同时间的库候选，正常近邻限制仍独立生效。

默认搜索间隔 0.1 秒，达到间隔后的 GT 更新执行一次搜索；首次正时间、
数据库或允许集合变化、当前继续资格失效立即重新搜索。
继续代价减 0.05；新候选还需改善大于 0.05、距离上次选中至少 0.15 秒，
同片段跳转的时间差至少 0.25 秒，循环取较短周期距离。
当前允许的同库非循环片段进入缺失未来窗口的尾段或到达终点时，
即使没有可评分的继续帧，也以 `nonloop-tail-rewind` 拒绝跳回该片段早帧，
防止起步/停止动作在尾段反复重启；其他片段仍按代价竞争，拒绝项保留在 topK。
此规则不影响循环片段，也不把不存在的继续特征补齐。
`actionLocked` 由 C# 明确提供，只在当前候选仍合法时禁止切换；
筛选撤销当前片段后，动作锁不能绕过筛选。
没有候选时冻结播放器，节流期间保留 `no-allowed-candidates` 原因。

非循环锁定动作到达不完整特征窗口的尾段时，
运行时保留已经获准且仍在允许集合中的片段，继续原生时钟到 Completed。
此路径显示 `action-locked-tail`，清空查询与排名，不伪造尾端特征或代价。
这是已获准动作的完成路径，不是搜索器把不完整帧纳入数据库。
正常选帧通过原生 `TransitionTo` 开始 0.18 秒交叉混合，
Jolt 继续独占 Actor 位移与朝向。
首次接管非 inPlace 播放时，先以零时长转换到当前片段/时刻的 inPlace 姿态，
避免把旧根偏移作为交叉混合来源。独立暂停的动画不会被 MM 自动恢复播放。

`MotionMatcher` 只维护搜索节流和重选年龄，不持有第二份播放时钟或业务状态机。
零 delta 包括首次调用均不搜索、不接纳新的绑定/筛选、不积累搜索债务。
World 在 Stop、传送、播放身份/不兼容 schema 变化及组件失活清理适用状态；
RT 不访问搜索器、CLR、World 或 Jolt。

## 稳态步频匹配

`Animation.Match` 的 `matchSpeed` 默认关闭。绑定数据库时，
`ExtractLoopRootVelocity` 从已导入原根的周期端点派生模型水平平均速度，
按数据库 member 顺序保存在只读运行时绑定中；不读取 in-place 后的静止根。
非循环、原地循环、非刚体周期或周期旋转达到一度的成员不适用。
派生值受已有输入字节身份约束，不新增磁盘缓存或改变名义速率数据库。
周期平均没有验证片段内部的匀速直线性质；当前 GASP Walk/Run/Sprint 是已核对输入。

`ComputeMotionSpeedMatch` 将源速度经组件矩阵转为世界水平速度，
与 `track.achievedVelocity` 的 Jolt 实际水平速度比较，忽略 desired 输入和垂直速度。
仅在启用、grounded、非混合、无动作锁、双方速度至少 10 cm/s、
方向余弦至少 0.95 时活动；目标为 `actual/source`，限制在 `[0.8,1.2]`，
当前倍率每 scaled 秒最多变化 2。
关闭或退出条件立即回到 1；暖机、无候选、传送重置及新选帧同样回到 1。
Pause/零 delta 保留已求值状态，不推进倍率或动画时钟。

倍率经播放器 `SetRate` 作用于唯一目标时钟，混合时长继续按 scaled 秒推进，
不改 Actor/Jolt 位移、姿态 epoch 或冻结混合源。
继续候选的 `ExtractClipFeatures` 用 `t + rate*offset` 和
`t - rate*velocityStep` 采样，关节差分仍除以运行时 `velocityStep`；
循环继续沿完整 SE(3) 延拓。新候选、统计与 42D schema 仍保持源速率 1。
这是选帧后的有界稳态时间缩放，不是多速率搜索、骨骼步幅变形或足部 IK。

`State.Rate` 和 `Motion.SpeedMatching` 经原生反馈发布当前结果，
其中 `residualSpeed = actualSpeed - sourceSpeed*rate` 是有符号速度差，
不是测量到的脚滑。开关状态、活动状态、源/实际速度、目标/当前倍率和原因
属于最近动画求值结果；搜索候选表仍保持它自身上次搜索的冻结时刻。
正式角色的 `MatchSpeed` 默认为 false，作为可选对照项，HUD 显示倍率和残差。
源根行程对齐不保证脚踝或足底静止；高 `contact_l/r` 曲线值也不是已验证的二值支撑标签。
API 的默认 false 保留旧调用的原速行为。实测边界见
[P6 步频记录](../rationale/gasp-p6-speed-matching-record.md)。

## 实际历史与预测

`motionHistory.*` 接受物理后的 GT 世界变换和当帧动画姿态观察，
按 60 Hz 对根位置/旋转及关节位置重采样，保留约 1.1 秒有界历史。
这是 GT 帧末观察的插值，**不是每次 Jolt 子步的独立记录**。
30/60/120 Hz 输入对线性运动的重采样一致；不能据此声称碰撞曲线在任意帧率严格相同。

时间必须单调、骨骼数量一致；观察间隔 >0.5 秒或位移 >2000 cm 重置。
Pose epoch 隔离换片段/seek：根轨迹可跨正常换片段保留，
跨 epoch 的插值姿态无效，不能用于速度查询。缺少完整过去窗口或有效姿态对时显示 warming up，
不以零补齐。Pause/零倍率不推进；Stop、播放身份变化和组件失活/销毁清理历史。

`World::TickScriptAnimations` 在当帧采样后记录受控骨骼组件实际根/关节位置；
最多记录 8 个活动组件：MM 组件优先取得槽位，普通 P4 调试骨骼补足余量，
各优先级内按脚本数组顺序记录，同组件 sections 不重复记录。
候选 UI 焦点同样优先选择已记录的 MM 组件，否则选首个普通轨迹。
普通静态/骨骼 mesh 和资产预览没有 Character 物理历史。
碰墙后历史使用实际达到的位置，不能将 Move 意图当位移。

预测复用当前水平 walk 意图、当前 heading；地面锁高，
空中按 60 Hz 半隐式积分 981 cm/s² 重力，尾段按剩余步长推进。
未计算未来碰撞、台阶、移动平台、转向输入变化或额外制动。
因此预测线可穿过墙；它是近似未来意图，不是无碰撞路径保证。
MM 查询仅在当前已观察到地面受阻（grounded、水平 desired≥2 cm/s 且
actual<2 cm/s）时改按实际水平速度外推未来；自由移动和空中仍用上述 desired 预测。
这不求解未来碰撞，也不修正部分滑墙、台阶或移动平台。
运行时 raw query 使用实际历史与该查询预测，并绑定当前数据库统计。
每次实际搜索一并冻结 query 根参考系、schema 非负 offset 的世界未来点及
`queryUsesAchievedVelocity` 标志；节流/Pause 不拿当前根或当前输入重算旧查询图形。
调试中红色表示这组冻结查询未来点，橙色保留实时 desired 意图且不计算碰撞，
地面受阻时两者可以明显分离。
搜索前临时观察使用当前物理后的根和当前显示姿态，局部关节速度保留最近两次
实际 post-blend 差分，消除旧根速度、转到当前朝向后加入当前实际根速度：
`v_now = R_now * R_displayed^-1 * (v_joint_displayed - v_root_displayed) + v_root_now`。
`v_joint_displayed` 与 `v_root_displayed` 使用同一已显示历史窗口的差分，
`v_root_now` 来自最近完成物理帧发布的 `track.achievedVelocity`（角色每帧按帧 dt 推进）。
零时步同步帧保留该反馈，不用相邻渲染根位移除以渲染 dt 伪造速度。
角色纯 yaw 转向保持最近实际速度，位置传送仍清速并刷新支撑；
对应物理边界见 [C# 蓝图运行时](csharp-blueprint-runtime.md)。
不对重复的临时姿态直接差分，否则局部动画速度会被错误清零。

## 源、身份与缓存

正式源 `DB_gasp.json` 类型 `motionDatabaseSource`、version=1，
显式指定 model、schema、有序 members（clip ID、布尔 loop）及
`boundaryPolicy: exclude-nonloop-extend-loop-v1`。
片段必须存在于模型 clips 目录，成员不可重复；不从名字猜循环属性。
描述文件是新类型，不属于 Scene/Mesh/Texture 标准修改 API 的能力表。

`source/animation/motionRuntimeDatabase.*` 使用资源相对路径解析并检查规范化后留在资源根内，
拒绝绝对路径、反斜线及冒号。依次捕获源/schema/模型 JSON、模型 FBX、成员 FBX 的
完整 BLAKE3；JSON 解析和摘要使用同一次读入字节。
身份还含版本化坐标、ozz、根轨道、差分、SE(3) 延拓、统计及阶段策略。
模型描述包含骨骼映射和片段目录，顺序或源字节变化都会改变身份。

缓存为 `Generated/MotionMatching/<identity>.json`，包含输入清单、数据库及 payloadBLAKE3。
命中必须核对身份、payload 摘要并解析通道/统计/帧顺序；坏缓存重建。
发布前再次核对全部输入身份，再原子写缓存；输入变化则拒绝发布。
要求输入静止，不承诺任意并发写入/ABA 情况的文件系统事务快照。
缓存可删除重建，不复用 UE 二进制缓存。

JSON 读取预算 64 MiB；成员 ≤256、帧 ≤100000；
schema 轨迹和关节各 1～16 项，轨迹有序唯一且在 ±1 秒内，
sampleRate 为 1～120、velocityStep 为 0.001～0.1 秒。
采样时间和循环次数另有预算；检查在工具/模块入口完成，不添加逐渲染帧钳制。

## 验收入口

```powershell
.\build\bin\vl_animation_probe.exe --build-motion-db Maps/SC_gasp/MotionMatching/DB_gasp.json `
  --resource-root ../VukanLearnResources --output artifacts/new-db-report.json
py -3 tool/animation/browse_motion_database.py `
  ../VukanLearnResources/Generated/MotionMatching/<identity>.json --output artifacts/new-browser.html
```

报告拒绝覆盖和向源描述目录写入；缓存只写上述 Generated 子目录。
浏览器是自包含 HTML：片段/帧选择、原始与归一化值/权重表，以及俯视轨迹和关节图。
原生骨骼预览随时间轴显示当前帧 raw 特征；不加载数据库统计。

关卡 Motion 开关或 `motiondebug 1` 显示青色过去一秒、橙色未来一秒及绿色关节点。
World 只发布不可变线段/值快照；UI 使用冻结相机投影、近远面/视口裁剪，
命令插在场景图像之后、工具条之前；线段无场景深度遮挡。
checkbox 仅生产带 World generation 的值请求，GT 验证；离线禁用，刷新不发命令。
关闭或离线时隐藏状态栏，避免空文本留下背景条。
几何经 `World::CaptureDebugDraw` 接入 [公共 DrawDebug](debug-draw.md)，
显示适配为 `source/ui/debugDrawOverlay.*`；验证映射见 [测试](testing.md)。
实测数据和限制见 [P4 记录](../rationale/gasp-p4-trajectory-and-features-record.md)。

绑定 MM 的角色另显示只读候选面板：searchCount、数据库/payload 身份、
query 维度/就绪状态、当前选中与继续 clip/time、前五名的 trajectory/pose/policy/total、
拒绝原因及混合比例；查询预览用 `blocked` 标记该次搜索采用了实际水平速度。
查询行右侧列表图标展开完整 raw/normalized 通道表，
同时显示完整数据库/payload 摘要及组件身份；表内滚动可读全部通道。
摘要与数值均来自同一冻结快照。
搜索节流期间发布上次搜索对应的查询，不使用新查询解释旧排名。

面板只占视口右侧，可滚动且不遮工具条；Selected、Continue 和五个排名槽位只挂载一次，
每行以两行文字显示片段/时刻/原因和分项代价，真实 CSS hover 提示显示片段/时刻/原因全文。
查询展开只切换既有候选表和通道表的显示；通道名/schema 相同只更新数值 span，
不逐帧替换通道行。图标也有实际 CSS hover 提示；RmlUi 不会自动显示 HTML title，
因此保留的 title 属性只作元数据，不作为用户读取完整值的入口。
MM 模式底部 summary 仅显示单行查询序号、决策原因与轨迹颜色图例，
明确 Cyan actual、Amber desired/no-collision 和 Red query，不重复候选表。
值最多 10 Hz 或决策/代际变化时刷新，相同暂停快照不重写节点或布局属性。
文本转义，滚动/点击不穿透场景；无 MM、关闭、离线或无 World 时隐藏，
P4 的简单轨迹 summary 独立保留。面板不读取文件、World、CLR 或实时播放器。
搜索核心的手算/穷举与策略回归位于 `MotionMatching` 套件；
UI 回归为 `EditorUiTest.MotionMatching*`。P5 过程证据见
[执行记录](../rationale/gasp-p5-motion-matching-record.md)。
