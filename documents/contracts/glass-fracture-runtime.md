# 玻璃破碎运行时

> 状态：当前契约（实现已落地）。描述 `glassPane` 场景对象从资源加载、World 描述、
> GT 侧 `GlassRuntime`、命中点径向裂纹碎片、Jolt 凸包刚体、落地二次碎裂、
> cracked 中间态、支撑岛和资产优先玻璃音的当前行为。它不描述透明材质/排序；
> 共享冲击世界与可见箭矢见 `projectile-shooting-runtime.md`。

## 场景对象

`glassPane` 是 `SceneAssetValidator` 接受的新 Scene object type。板面在对象局部
XY 平面，厚度沿局部 Z，中心在对象原点。必填字段：

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `name`、`type` | string | `type` 逐字为 `glassPane` |
| `position`、`rotation`、`scale` | vec3 | 与 mesh 相同，scale 不得为 0 |
| `materialInstance` | string | 非空；资源加载阶段创建 MI |
| `width`、`height` | number | `> 0`，单位厘米 |
| `thickness` | number | `0 < thickness < min(width, height)` |
| `siteCount` | integer | `[4, 64]` |
| `seed` | unsigned integer | 断裂拓扑的确定性种子 |
| `subfracture` | boolean | 可选，默认 `true`；关闭后落地/再命中不再切分 |

`HasSupportedSceneObjectTypes` 认可 `glassPane`，场景选择器不会把它当成未知对象。
离线渲染路径拒绝 glassPane；当前只有实时路径创建玻璃运行时。

## 资源与 World 数据

`RendererResourceLoadCoordinator` 为 glassPane 加载材质实例，产生
`GlassObjectBuildPlan`；`WorldBuilder` 把它转成 `WorldGlassObject`。`World` 只保存
作者参数、TRS 和材质 key；完整板网格、碎片、Jolt body 和裂纹状态都在 GT 侧
`GlassRuntime` 内，不进入 `WorldSnapshot`。

## 完整板

`GlassRuntime` 在 World 换代时：

1. 生成 24 顶点 / 36 索引的六面盒网格（板面 XY、厚度 Z）；
2. 创建 host-visible 动态 `RenderableObject`，key 形如
   `glass|<scenePath>|<objectName>|<generation>|intact`；
3. 添加 `WorldMeshObject`，复用场景解析出的 `materialKey`/`materialInstanceKey`；
4. 在 Jolt 世界注册一个静态盒 proxy：中心取模型平移，旋转取模型旋转，
   半尺寸为 `(width/2, height/2, thickness/2) * scale`。

## 开裂中间态

每块玻璃在 `GlassRuntime` 内持有一个 `GlassDamagePolicy`
（`source/physics/fracture/glassDamagePolicy.*`，纯 CPU 策略，不依赖 Jolt/World）：

- 相对速度 `< 4.0 m/s` 视为低能量，只累计损伤，不申请碎片预算；
- 单次能量贡献为 `0.4 * (v / 4.0)^2`，累计归一化能量达到 `1.0` 时进入完整破碎；
- `>= 4.0 m/s` 视为强命中，直接进入现有 `FractureInstance`；
- 4.0 m/s 与气球漏气阈值同一量级，但是独立常量，二者不共享配置。

低能量命中时：

1. 用完整破碎的伤害半径公式算出参考半径，再乘 `0.5..0.8`
   （随累计能量线性增长）作为本次开裂的 `stopRadius`；
2. 用 `GenerateRadialImpactPattern` 以真实命中点在板面的投影为极点生成裂纹拓扑；
   母裂纹数随累计能量从 6 增长到 12，环数从 1 增长到 3；
3. 只保留 `stopRadius` 内的 cell 边界，把每段投影成板面前后各一层、宽 0.7 cm
   的四边形，累加进该玻璃唯一的 `<pane>~cracks` 动态网格；材质来自固定 MI
   `Common/Materials/MI_glass_crack.json`（深色、高粗糙），双面绕序，避免背面剔除；
   没有裂纹 MI 时才回退到 DebugDraw 折线（category `GlassCrack`）；
4. 播放专用的 0.14 秒 `PlayGlassCrack`（高频 click + 两个短模态，Impact 优先级），
   与整块破碎声区分；
5. 累计能量达到阈值后，先移除 `<pane>~cracks` 网格（及其 debug 回退），再调用既有
   `FractureInstance` 完成破碎。

同一场景 seed 与同一命中序号产生相同的裂纹拓扑；冲击点决定图案中心，
因此裂纹始终与真实命中位置相关，不是全板均匀图案。World 换代、破碎或
运行时关闭会清空裂纹列表与累计能量。cracked 阶段保留完整板网格与静态碰撞盒。

## 破碎

命中分类由 `GlassDamagePolicy` 完成；只有强命中或累计能量达到阈值才进入以下流程。

投射物与静态板代理接触后：

1. 把世界命中点经 `model` 逆变换到板局部 XY，作为径向裂纹中心；
   开裂/破碎阈值使用 `FractureImpact::normalSpeed`（速度在接触法线上的分量），
   重力带来的切向速度不计入伤害；
2. 用 `GenerateRadialImpactPattern` 在 `[-width/2,width/2] × [-height/2,height/2]`
   矩形域上生成运行时碎片：冲击点粉碎区（`coreRadius = min(width,height)*0.03`）、
   `siteCount/4` 个母扇区（每扇区可再分 2–3 个细支）、3 个同心环；
   伤害半径 `damageRadius = max(width,height)*(0.06+0.14*能量)` 处强制收口
   （`stopRadius`），碎片尺寸随半径明显增大，母扇区角度和环半径都带确定性抖动；
3. 移除完整板的 `WorldMeshObject` 和静态 proxy；完整板 `RenderableObject`
   进入 `ResourceRetireQueue`，等 GPU epoch 完成后释放；
4. 每个碎片用 `ExtrudePlanarShard` 挤出为封闭凸棱柱，顶点相对碎片质心；
5. 每个碎片创建一个动态 `RenderableObject` 和一个 `WorldMeshObject`，名字为
   `<paneName>~frag<N>`，`materialKey` 复用整板材质；
6. Jolt 凸包点按世界 scale 缩放后注册为动态 body；质量按
   `面积 × 厚度 × 2.5e-3 kg/cm³ × scale³` 估算；
7. 碎片初速度以“掉落 + 少量外翻”为主：面内径向 `60–150 cm/s`（随冲击速度缩放），
   面外随机 `±(25–60) cm/s`；每个碎片另有确定性初始角速度；
   质心在 `damageRadius*1.1` 外的板片标记为 attached：只保留静态网格和裂纹边界，
   以静态凸包刚体注册，所以玻璃通常只被打出一个碎洞而不是整块消失；
8. 每帧从 Jolt 姿态写回 `WorldMeshObject::model`，渲染只用局部网格 + 世界变换；
9. 在命中点播放资产优先的玻璃破碎音；`glass_break.wav` 缺失时回退 0.48 秒
   程序化合成。

## 累计伤害

attached 板片不是纯视觉网格：它们有静态凸包碰撞体。投射物命中任一块时：

1. `FractureImpactWorld` 产出 `AttachedHit`（objectName + shardName）；
2. `GlassRuntime` 用 `GenerateRadialImpactPattern` 以**新命中点**为极点、把该板片
   多边形作为域重新生成径向裂纹（母裂纹 4–10 条、1 圈环、核心粉碎区），
   与第一次破碎保持同一形态；不再使用随机直线切三片；
3. 移除静态 body 与网格，新尖片继承板片姿态并向外飞出；
4. 播放玻璃破碎音。

因此同一块玻璃可以被反复命中：第一次打出碎洞，后续命中继续卸掉外圈板片，
直到单板 96 片预算用尽。洞中心没有碰撞体，箭会直接穿过。

预算不足时，命中的 attached 板片不再切碎，而是**整块转成动态刚体掉落**
（净增 0 片）。这样玻璃不会出现“打到一定程度再也打不动”的死状态：只要还有
attached 板片，就一定能被继续打掉。

预算由 `FractureBudget` 统一管理：先回收“已落地静止 ≥1.5 秒”的动态碎片
（按屏幕占比/相机距离排序），再考虑 attached 整块掉落，最后才裁剪新碎片数。
玻璃动态碎片、attached 板片和气球软片共享 320 片的全场景上限；单板仍有
96 片上限。attached 板片不参与回收。完整规则见 `fracture-budget.md`。

## 网格池

玻璃碎片使用 `RenderableObject` 的固定容量动态网格模式：

- 构造时按 `vertexCapacity/indexCapacity`（64 顶点 / 48 索引分档）一次分配
  host-visible 顶点与索引缓冲；
- `UpdateDynamicMesh` 可整批替换拓扑，只要不超过容量；
- 回收时对象先按 GPU epoch 进入 `pendingShardMeshes`，epoch 完成后回到
  `freeShardMeshes`，由新碎片复用，避免 100+ 片时反复创建/销毁 buffer；
- 整板和箭矢仍使用普通 `RenderableObject`，不进入池。

### 支撑岛

attached 板片保留板面局部多边形。每次打掉一块后做一次连通性检查：

- 节点 = 剩余 attached 板片；边 = 共享顶点或点到边距离 `< 0.75 cm`；
- 锚点 = 多边形接触玻璃外框（距 pane 边界 `< 0.5 cm`）；
- 从锚点做 BFS，不可达的板片视为失去支撑，转成动态刚体整块掉落（不切碎）。

初次碎裂后会立即跑一次支撑岛检查。未接触外框的板片直接掉落；即使接触外框，
但整个连通残余岛总面积 `< 300 cm²` 时也转为动态掉落，避免没有真实窗框时
小残片停在原板面高度。后续每次打掉 attached 板片都会重新检查。

## 二次碎裂

碎片与地面、另一碎片，或被箭命中时：

1. 播放短促玻璃落地音（30 ms 冷却）；
2. 若场景 `subfracture=true`、碎片深度 `< 2`、接触速度 `>= 1.8 m/s`
   且总碎片数不超过 96，则按来源检查面积：
   - 箭命中动态碎片：面积 `>= 8 cm²`；
   - 碎片落地/碎片互撞：面积 `>= 60 cm²`；
3. 面积 `< 60 cm²` 时用 `SplitPlanarShardByCracks` 切 1 刀得到 2 片；
   大面积碎片切 2 刀得到 3 片，避免小块被切成看不清的粉尘；
4. 子碎片继承父碎片世界姿态、线速度和一小段向外偏移，深度加 1；
5. 父碎片的 Jolt body 销毁，网格走延迟退休；子碎片重新注册凸包 body。

## Jolt 冲击世界

`FractureImpactWorld` 是原生 Jolt 世界（原名 `SoftBodyImpactWorld`），同时服务气球和
玻璃板：

- 静态球体 proxy 表示 balloon，静态盒 proxy 表示 glassPane；注册时显式标记
  `FractureMaterialProfile::Balloon` / `Glass`，玻璃碎片也继承 `Glass`；
- 动态投射物使用 `LinearCast` 连续碰撞检测，薄玻璃板不会被高速弹丸穿透；
  投射物命中动态碎片会产生 `ShardHit`（带 `projectileId`），这是地上碎片继续
  被打碎的入口；`landingBodies` 只抑制落地/碎片互撞重复事件，不会吞掉后续箭矢；
- 凸包碎片、地面、接触事件和投射物生命周期与气球共用同一套实现；
- `GlassRuntime::HandleImpacts` 只消费 `materialProfile == Glass` 的事件；
  事件中的 `type` 只描述接触形态，不负责选择玻璃/气球效果。
- 气球、玻璃和可见箭矢共享一个 `FractureImpactWorld` 实例（由 `EngineLoop` 持有），
  因此同一支箭可以命中任意目标；每帧只步进一次，事件再分发给各运行时；
- 调试地面是顶面位于 `y=-100 cm` 的静态盒；World 换代或 `Shutdown` 会清理所有 body。

## 声音

玻璃声音提交到 48 kHz stereo、32 voice 的块混音器
（预算、pan、距离低通见 `audio-runtime.md`）：

- `PlayGlassCrack`：优先用 `glass_break.wav` 的 0.16 秒 attack 切片，叠高频
  click 和短模态；资产缺失时回退 0.14 秒程序化 click；
- `PlayGlassShatter`：优先用 `glass_break.wav` 的 0.58 秒片段（3 个播放速率变体），
  叠同步 click 和 118 Hz 低频冲击；资产缺失时回退 0.48 秒程序化破碎音；
- `PlayGlassShardLanding`：优先用破碎尾音的 4 个位置 × 3 个速率切片；
  资产缺失时回退 0.12 秒单模态 ping；30 ms 冷却，与气球落地音独立。

## 当前边界

- 材质仍复用场景 MI；玻璃专用透明/折射/双面材质和透明排序尚未实现。
- cracked 阶段的裂纹是板面局部薄四边形网格 + 深色 MI，不是透明折射/法线扰动覆盖层；
  MI 缺失时回退到 DebugDraw 折线；
  它不参与碰撞、阴影、透明排序或 World 快照。
- 裂纹 debug 几何受 `DrawDebug` 全局预算限制（1024 项 / 16384 条线）。
  超出时保留已有裂纹、停止本次提交并记录 warning；裂纹不会随时间自行消散。
- cracked 阶段不申请碎片预算；达到阈值后的破碎继续走 `FractureBudget`。
- 碎片之间由 Jolt 解析接触并产出 `ShardHit`，会按 60 cm² 阈值触发二次碎裂；
  箭命中动态碎片同样产出 `ShardHit`，但使用 8 cm² 阈值。
- 二次碎裂的碎片布局受落地/命中顺序影响；初始 71 片布局本身由 seed 决定且可复现。
- 碎片网格按容量进入玻璃运行时私有池，但每片仍是独立 `RenderableObject`
  和独立 draw；没有跨对象批量上传。
- 全场景预算与屏幕占比降级由 `fracture-budget.md` 描述；视觉下限允许玻璃
  初次碎裂短暂超出全局上限。
- 地面是固定调试平面，不是场景网格碰撞。
