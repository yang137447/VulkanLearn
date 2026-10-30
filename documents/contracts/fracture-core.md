# 物理 CPU 核心

> 状态：当前契约（实现已落地）。描述 `vl_fracture_core` 当前提供的平面冲击破碎、
> 球面冲击破碎、橡皮膜软壳、凸多边形裁剪、碎片挤出和体积校验。该库是纯 CPU 模块，
> 不读文件、不接触 Jolt、不持有 Vulkan 对象，也不修改 `World`。

## 所有权与依赖

| 职责 | 所有者 | 位置 |
| --- | --- | --- |
| 2D Voronoi 冲击布局与凸多边形裁剪 | `VL::Physics::Fracture` | `source/physics/fracture/planarFracture.*` |
| 玻璃“开裂 → 破碎”累计损伤策略 | `VL::Physics::Fracture` | `source/physics/fracture/glassDamagePolicy.*` |
| 平面碎片挤出为封闭凸棱柱、凸包点和面法线 | `VL::Physics::Fracture` | `source/physics/fracture/fragmentMesh.*` |
| 球面 Voronoi 冲击布局与薄壳碎片挤出 | `VL::Physics::Fracture` | `source/physics/fracture/sphericalFracture.*` |
| 运行时 icosphere、XPBD 橡皮膜、压力、软片拆分和渲染数据 | `VL::Physics::Softbody` | `source/physics/softbody/rubberShell.*` |
| 模块构建 | `vl_fracture_core` 静态库 | `source/physics/CMakeLists.txt` |
| 数值与几何回归 | `fracture_core_tests` | `tool/fracture-core-tests/` |

`vl_fracture_core` 只依赖 Eigen。它不依赖 `vl_script_host` 的 C# Blueprint 开关，
不依赖 Jolt，不被 `source/CMakeLists.txt` 的递归源文件再次编进 `main`。

## 输入契约

`GenerateImpactPattern` 的 `domain` 必须是 CCW、面积为正的凸多边形。函数在入口拒绝：

- 少于三个点；
- 非凸或面积为零/负；
- 无法形成至少两个不同种子的布局。

`impact` 可以在域外。实现会沿 `impact` 到域内质心的线段投影到域内，避免在玻璃边缘
生成域外种子。`siteCount` 被限制在 `[2, 256]`；去重后实际数量可以更少。

`PlanarFractureSettings::seed` 与输入决定结果。相同输入和 seed 返回逐点相同的布局；
`radialBias` 只改变种子分布，不改变面积守恒的裁剪规则。

## 输出契约

`PlanarPattern::shards` 中每个 `outline`：

- 是 CCW 顺序的凸多边形；
- 面积为正；
- 与其它 shard 覆盖同一个 `domain`，面积之和与 `domain` 面积的差只来自浮点误差；
- `patternIndex` 按生成顺序从 0 递增。

`SplitPlanarShard` 以碎片面积质心为冲击中心重新生成子布局，返回的子碎片面积之和
与父碎片面积一致。递归深度、全局碎片预算和最小可保留面积由调用方负责。

`GenerateRadialImpactPattern` 以 `impact` 为中心生成径向裂纹 + 同心环图案，用于玻璃冲击：

- `coreRadius` 内的每个细支形成一个三角，构成冲击点粉碎区；
- 每个母扇区可以永久分成 2–3 个细支，细支从冲击点延伸到边界，形成不等宽的长楔形；
- 环半径按 `ringGrowth` 增长并按 `ringJitter` 抖动，环数可因扇区而异；
- `stopRadius > 0` 时在伤害半径处强制收口：环半径在 `coreRadius..stopRadius`
  之间重新按几何级数分配，圈外只剩一块大板（调用方可保持静态）；
- 最外侧环总是补到域边界，因此输出仍然覆盖整个凸域，面积和与域面积只差浮点误差；
- 同一 seed + 同一输入产生逐点相同的布局。

`SplitPlanarShardByCracks` 用随机直线弦把凸碎片切成 `cutCount + 1` 个尖片；输出碎片
面积之和与父碎片一致，所有碎片仍为凸多边形。玻璃落地二次碎裂优先使用该入口，
因为 Voronoi 会把碎片再次切成圆润的小块。

## 玻璃开裂策略

`GlassDamagePolicy` 把“低能量累计开裂、达到阈值后破碎”抽成纯策略：

- `crackOnlySpeed = 4.0 m/s`：低于该速度只开裂，`>=` 该速度直接破碎；
- 单次低能量贡献 `energyPerHit * (v / crackOnlySpeed)^2`，默认 `energyPerHit = 0.4`；
- 累计能量达到 `breakEnergy = 1.0` 的那一次返回 `Fracture`；
- 决策同时给出 `crackRadiusScale`（0.5..0.8）、母裂纹数（6..12）和环数（1..3），
  调用方据此缩小径向图案并逐次加深裂纹；
- `Reset()` 清零累计能量与破碎标记，对应 World 换代清理。

该策略不生成碎片、不申请预算、不绘制 debug 几何；裂纹拓扑仍由
`GenerateRadialImpactPattern` 生成。`tool/fracture-core-tests/glassDamagePolicyTests.cpp`
覆盖单次低能量、累计到阈值、强命中直碎、重置和图案增长。

`ExtrudePlanarShard` 输出：

- `positions`/`normals`/`indices`：按面复制顶点的硬边渲染网格；
- `convexHullPoints`：相对 `localCenter` 的顶面和底面唯一顶点，供 Jolt 凸包使用；
- `localCenter`：面积质心在 XY、`centerZ` 在 Z；
- `volume = area * thickness`；
- 外法线绕序；`SignedMeshVolume` 在 float 精度内等于正体积。

厚度必须为正。局部坐标使用项目世界单位（厘米），但该库不负责把厘米换算成 Jolt 的米。

## 球壳契约

`GenerateSphericalBurstPattern` 在单位球面上生成运行时 Voronoi 拓扑。函数在入口拒绝：

- 零冲击方向；
- 非正半径、非正厚度或厚度不小于半径；
- 去重后少于四个种子。

`SphericalPattern::shards` 中每个 `boundary`：

- 至少三个单位方向；
- 按绕 `siteDirection` 的 CCW 顺序排列；
- 与其它 cell 覆盖整个球面，面积之和与 `4πR²` 的差只来自浮点误差；
- `siteDirection` 位于 cell 内部，`patternIndex` 按生成顺序从 0 递增。

`ExtrudeSphericalShard` 输出：

- 外表面沿径向朝外、内表面朝内；
- `convexHullPoints` 是相对 `localCenter` 的外/内边界与中心点，供 Jolt 凸包近似；
- `volume` 按外球面立体角沿径向积分，不使用 `area * thickness` 近似；
- 球面片用平面扇形三角化，因此 `SignedMeshVolume` 会比解析薄壳体积略小。

## 橡皮膜软壳契约

`RubberShell::BuildIcosphere` 在运行时生成闭环球壳。输入半径必须为正，
`subdivisions` 在 `[0, 4]`，总质量为正，约束迭代次数在 `[1, 32]`。
icosphere 细分 2 产生 162 顶点 / 320 三角面，细分 3 产生 642 / 1280。

`RubberShell` 当前提供：

- 结构边距离约束和共边对顶点弯曲约束，均用 XPBD compliance 求解；
- 全局速度阻尼与可注入重力；
- 正值 `pressure` 沿三角面外法线充气；
- `ComputeVolume`、`MaximumStructuralStrain` 和 `ShouldBurst`；
- 所有粒子位置/速度保持有限，同输入和步进序列产生相同结果。

`TearBySphericalPattern` 在运行时按 `SphericalPattern` 的 site 方向给三角面分区，
把同一张膜拆成多个 `RubberShell` 软片。每个碎片保留自己的粒子、结构边和弯曲约束；
`restContraction` 把静止边长缩到破裂瞬间长度的一个比例，用于橡皮回缩。
该入口的碎片边界沿现有 icosphere 三角网格边走。

`TearBySphericalPatternExact` 用球面 cell 边界平面裁剪每个三角面，再把交点按
重心权重投影回原球壳径向；边界因此贴合运行时 Voronoi cell，而不是整三角面归属。
裁剪后的凸多边形扇形三角化，顶点按空间量化键合并，最后重建碎片内部约束。

`RubberShellSettings::strainStiffening` 按应变降低有效 compliance，
`edgeDamping` 沿结构边相对速度耗散能量。`BuildRenderMesh` 输出不带 Vulkan 依赖的
`SoftBodyRenderMesh`（位置、平滑法线、索引），供引擎转换成 `Vertex` 后写入
`RenderableObject` 的 host-visible 动态顶点缓冲。

`ApplyBurstImpulse` 按粒子相对 `center` 的径向方向施加爆裂速度，并叠加确定性的切向
分量。传入非零 `punctureDirection`（局部单位方向）和正 `punctureBoost` 时，命中点附近
的粒子按 `cos²` 权重获得额外径向速度；零向量或非正 boost 退化为均匀冲量。该权重只改变
速度分布，不改变碎片拓扑、质量或约束。

该模块不读取预制条带，不创建 Jolt body，不生成 Vulkan 资源，也不把粒子数组发布给渲染线程。

## 当前不做的事

- 任意三角网格的体素化、VACD 或任意 3D Voronoi；
- 凹多边形域、非球面薄壳网格的运行时切割；
- 橡皮膜的塑性永久变形、撕裂尖端应力场和多层薄壳耦合；
- 支撑岛检测、接触冲量计算、二次破碎预算；
- Jolt 刚体创建、World/快照注册、动态网格资源生命周期、透明排序或阴影；
- 音频事件、采样播放或混音。

这些能力属于 `documents/plan/physics/realtime-fracture-and-audio-plan.md` 的研究范围，
不属于本契约。
