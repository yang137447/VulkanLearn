# 实时破碎物理与声音：研究与实施计划

> 状态：研究/规划（非契约）。当前已落地 `vl_fracture_core` 的平面破碎、球壳破碎、
> 橡皮膜 XPBD 软壳、精确裁剪，以及 `balloon` 场景对象到动态网格/WorldSnapshot 的
> 实时引擎纵切；Jolt 冲击触发和程序化爆裂音也已接通；
> 本文研究玻璃、气球、二次碎裂和事件驱动声音的完整路线，技术细节在实现前不能当作现状。
>
> 硬约束：**不使用预切割碎片资产、预制断裂图案或预 authored 气球条带**。断裂拓扑必须在运行时
> 由几何、冲击和确定性 seed 生成。
>
> 分支：`codex/physics-fracture-audio`
> 日期：2026-10-29

## 1. 目标与边界

目标不是做通用有限元或离线影视级断裂，而是让游戏实时场景中的三类事件可信、可控、可重复：

1. 玻璃被击碎：命中点局部开裂、碎片获得冲量、落地后大碎片继续碎。
2. 气球破裂：低压气球从完整状态瞬间进入快速泄气、橡胶碎片回弹和声音事件。
3. 声画同步：冲击、开裂、碎片落地、二次碎裂都从同一个物理事件驱动声音，而不是另做一套
   纯视觉计时器。

首版明确不做：

- 连续介质 FEM、相场断裂、真实裂纹尖端应力强度因子；
- 任意凹网格的实时体素化；
- 流体、布料、毛发与破碎的耦合；
- 网络同步和完整回放；
- 编辑器内的破碎资产作者工具（先走 JSON 资源和固定场景夹具）。

## 2. 当前仓库事实

这些是决定方案边界的现有实现，不是计划：

- Jolt 已通过 `script/CMakeLists.txt` 的 `VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS` 开关引入，
  当前只服务 C# Blueprint 的 `scriptPhysics.cpp`。它使用 1/60 固定子步、GT 同步执行、
  接触结果以 JSON 发布给托管层。
- `source/platform/platformApplication.cpp` 的 SDL 初始化只有
  `SDL_INIT_VIDEO | SDL_INIT_GAMEPAD`，仓库没有音频设备、混音器或声音资源路径。
- `RenderableObject` 已支持从 CPU 顶点/索引直接创建 GPU 缓冲；`RendererResourceCache`
  已有动态资源键与 `TrackDynamicAdditions`/`PruneDynamicResources` 生命周期机制。
- `World` 的渲染输入是不可变 `WorldSnapshot`，渲染侧只按 `meshKey`/`materialKey` 解析资源，
  不读可变 `World`。碎片必须通过 GT 的 World 数据与资源 key 进入快照。
- 当前 `vl_fracture_core` 已支持 2D 凸域 Voronoi、球面 Voronoi 和对应的薄片挤出，
  但不创建 Jolt 刚体、软体粒子或 GPU 资源。

## 3. 研究结论

### 3.1 不用单一算法覆盖所有材料

实时破碎常见路线各有边界：

| 路线 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| 预切割碎片（pre-fractured） | 极便宜、稳定、可控 | 图案与命中点无关，层级固定 | **不采用**；违反运行时生成约束 |
| 冲击局部 Voronoi | 图案随命中点变化，碎片可递归 | 需要几何裁剪、凸包和预算 | 玻璃板、薄片首版路线 |
| 球面 Voronoi | 气球断开拓扑可在运行时生成 | 需要薄壳软体/约束求解 | 气球首版路线 |
| 体素近似凸分解（VACD） | 可处理复杂视觉网格、局部破坏 | 预处理和内存成本高 | 后续通用物体；不预存断裂图案 |
| 连续 FEM/相场 | 物理最完整 | 实时成本、资产和调参不可接受 | 不进入首版 |

因此采用混合路线：

- **玻璃板首版：** 冲击局部 2D Voronoi + 挤出凸棱柱。它直接覆盖薄玻璃的主要视觉与刚体行为，
  且与当前 CPU 核心一致。
- **气球首版：** 运行时生成球壳网格与球面 Voronoi 碎片，碎片用薄壳粒子/约束或运行时凸包近似
  驱动回弹；不读取任何预制条带。
- **任意复杂物体：** 后续只预计算碰撞用的近似凸分解，不预存断裂图案；断裂拓扑仍在运行时生成。

这与 Müller/Chentanez/Kim 2013 的实时动态破碎方向一致：用近似凸分解和局部图案避免
“预切割图案不贴合命中点、层级固定”的问题，同时把玻璃和气球都纳入运行时生成。

### 3.2 玻璃：先局部开裂，再碎片化，再二次碎裂

建议把一次玻璃破坏拆成四个状态，而不是“命中即整个物体消失”：

1. **Intact**：完整网格 + 静态/运动刚体代理。
2. **Cracked**：局部裂纹覆盖层或材质参数变化；大裂纹但未完全分离。
3. **Fractured**：命中半径内的 Voronoi 碎片替换完整区域；外围仍是完整网格或大块。
4. **Debris**：碎片动态下落；落地冲击超过阈值时对大碎片递归切分。

状态转换由冲击能量和局部面积决定，不由固定动画时间决定。

### 3.2.1 断裂图案形态修正（2026-10-29）

玻璃首版直接复用了各向同性 Voronoi，实机读感“很平均、很假”：碎片尺寸接近、
多边形边数多且没有方向性，和 FEM/真实玻璃差异明显。真实玻璃冲击的形态是：

- 冲击点附近细碎（dicing / 粉碎区）；
- 从冲击点放射出的长径向裂纹，宽度不均，部分带细支；
- 环向裂纹把径向楔形再切断，碎片尺寸随半径增大；
- 边缘保留较大的板状碎片。

因此玻璃改为 `GenerateRadialImpactPattern`（冲击点核心三角 + 母扇区/细支 +
几何增长的同心环），二次碎裂改为 `SplitPlanarShardByCracks` 的直线弦切割。
图案仍然完全由命中点和 seed 在运行时生成，不读取预制断裂资产。

### 3.3 气球：运行时球壳切分 + 可变形薄壳

气球是薄壳、大变形、快速泄气问题。它需要和玻璃不同的动力学，但同样不能使用预切割：

- 破裂瞬间的爆裂声和空气脉冲；
- 橡胶碎片向外翻卷、快速减速；
- 少量碎片飘落，而不是玻璃式硬质崩裂；
- 体积从膨胀到塌缩的快速变化。

首版路线：

1. 运行时生成闭环球壳网格（UV sphere/icosphere 或参数化椭球）；
2. 壳顶点作为粒子，边作为距离约束，附加弯曲约束和内部压力；
3. 冲击把局部应变/压力推过阈值后，在球面运行时生成 Voronoi 种子并切出碎片；
4. 碎片保留粒子/约束拓扑，成为可弯曲的橡胶薄片；小碎片可以退化为凸包刚体；
5. 给碎片高角阻尼、空气阻力和生命周期预算；
6. 播放“压力爆裂 + 橡胶回弹 + 空气噪声”复合声音。

当前 CPU 核心已经能生成球面 Voronoi 边界、带厚度的薄壳网格，以及基于 XPBD 的橡皮膜
软壳、精确软片裁剪、粘弹/应变刚化和 CPU 渲染数据；下一步是接入 Jolt/场景、
World 快照和动态网格资源，而不是接入预制动画。

## 4. 物理设计

### 4.1 单位、坐标和确定性

- 场景和渲染继续使用厘米、Y 向上；Jolt 边界统一换算为米。
- 碎片局部原点取面积/体积质心，刚体变换和模型矩阵使用同一锚点。
- 物理固定 1/60 子步；破碎决策只在 GT 的稳定帧边界发生。
- 每个可破坏对象持有独立 RNG 状态。种子由
  `world generation + object stable id + impact sequence + quantized hit point` 派生，
  不使用 `std::random_device`、时间戳或全局 RNG。
- 所有阈值、质量、尺寸在资源加载边界校验；下游不再重复 clamp。

### 4.2 数据模型（计划）

建议把“资产作者数据”和“运行时损坏状态”分开：

```text
DestructibleProfile (asset JSON)
  materialProfile      glass / balloon / generic
  proxy                box / planar polygon / sphere shell / convex pieces
  thickness
  density
  crackEnergy
  fractureEnergy
  subfractureEnergy
  minFragmentArea
  maxInitialFragments
  maxSubFragments
  maxGlobalFragments
  audioProfile

DamageState (World/GT only)
  stableObjectId
  sourceMeshKey
  sourceMaterialKey
  intact / cracked / fractured / debris
  impactSequence
  seed
  fragments[]
  supportEdges[]
  crackOverlayState
```

`DestructibleProfile` 可以从场景对象或蓝图资产引用；`DamageState` 只在 Play/World 生命周期内存在，
Stop 或 World 换代时释放，不写回场景 JSON。

### 4.3 冲击能量与阈值

首版优先使用 Jolt 接触冲量或相对法向速度，而不是只凭投射物质量：

```text
E_impact ≈ 0.5 * m_effective * v_n^2
```

若 Jolt 能提供接触冲量 `J_n`，则使用 `E_contact ≈ 0.5 * J_n * v_n` 或引擎内统一标定的
等效能量。阈值分三档：

- `crackEnergy`：产生裂纹覆盖，不分离碎片；
- `fractureEnergy`：生成初始碎片；
- `subfractureEnergy`：碎片落地/再次受击时继续切分。

阈值不能只用焦耳硬编码；需要乘以材料厚度、面积和形状因子，最终在固定场景里用
“相同速度、不同玻璃厚度”的对照回归校准。

### 4.4 局部破碎与支撑岛

首版玻璃板可以采用简单规则：

- 命中半径内的 Voronoi cell 变为动态碎片；
- 命中半径外的区域保持静态完整网格；
- 若玻璃有边框，边框附近的 cell 保持附着或固定；
- 可选：删除 cell 后对剩余静态块做连通分量检测，失去支撑的块转为动态。

支撑图后续可扩展为 `fragment -> neighbor` 边表：

- 碎片尚未脱离时作为静态/睡眠体；
- 邻居被删除后，度数为零的连通块转为动态；
- 这比“整块玻璃同时变成动态”更接近 Müller 2013 的 partial destruction 与 island detection。

### 4.5 二次碎裂

二次碎裂必须是预算约束下的决策：

```text
if contactEnergy > subfractureEnergy
   and shard.area > minFragmentArea
   and shard.depth < maxDepth
   and globalFragmentCount + expectedChildren <= maxGlobalFragments:
       split shard
```

建议默认预算：

- 单块玻璃初始 32–64 片；
- 单个大碎片最多再切 4–12 片；
- 每个对象最多 2 层递归；
- 全场景动态碎片上限 512–1024，按距离和屏幕占比降级；
- 小于 `minFragmentArea` 的碎片直接保留为 debris，不再切。

这些数字是首版调参起点，不是契约。

### 4.6 Jolt 集成

当前 `ScriptPhysicsWorld` 是“GT 独占 + JSON 状态数组”的脚本物理世界。破碎不应把每个碎片
再绕一遍 CLR/JSON，而应增加原生 typed 物理层：

- 完整物体可用静态 `MeshShape` 或 box/sphere proxy；
- 碎片使用 `ConvexHullShapeSettings` + `ConvexHullShape`；
- 每个碎片一个 dynamic body，质量由体积 × 密度；
- 碰撞层/掩码沿用现有 Static/Moving layer 思路，但增加 debris 子层；
- 接触事件由 Jolt contact listener 或后步查询产出 typed `ContactEvent`；
- 破碎系统消费 `ContactEvent`，生成新的碎片和声音事件。

需要先验证当前 Jolt 版本是否暴露接触冲量；若只暴露接触点/法线，首版用相对速度和质量近似，
并把“冲量来源”记录在诊断里，避免把近似能量误写成真实冲量。

### 4.7 气球物理

完整气球：

- 运行时生成闭环球壳网格，顶点作为粒子；
- 约束包括边长、面积/弯曲和内部压力；压力用标量 `pressure` 驱动，不求解连续介质本构；
- 受击时把冲量转成局部应变/压力增量，超过 `burstPressure` 触发破裂；
- 破裂时在球面生成运行时 Voronoi 种子，切出 8–32 个薄壳碎片；
- 每个碎片保留自己的粒子/约束子图，作为软体薄片继续模拟；
- 小碎片或预算降级时，可把碎片粒子烘焙成运行时凸包刚体，但仍不使用预制几何；
- 碎片使用较高线性/角阻尼和空气阻力，1–2 秒后淡出/回收。

软体碎片是本路线的主要 CPU 成本。首版应限制粒子数、约束迭代次数和碎片数量，
并用固定步长保证回弹与破裂判定可重复。

## 5. 声音设计

### 5.1 事件模型

物理层只产出 typed 事件，不直接调用音频设备：

```text
FractureAudioEvent
  type: Impact / Crack / Shatter / BalloonBurst / DebrisLand / Subfracture
  position, normal
  energy
  materialProfile
  fragmentCount
  seed
```

事件从 GT 进入音频子系统；音频线程只读事件和音频资源，不读 `World`、不碰 Vulkan。
这与现有 GT/RT 快照边界同构：物理生产消息，音频消费消息。

### 5.2 后端选择

仓库已经 vendored SDL3，且 SDL3 构建配置里 Audio/WASAPI 已开启。首版建议：

- `SDL_Init` 增加 `SDL_INIT_AUDIO`；
- 用 `SDL_OpenAudioDeviceStream` 打开设备流；
- 自建轻量混音器：voice pool、增益、衰减、声像、简单低通/高通、优先级和 steal；
- 不引入 FMOD/Wwise/Steam Audio 等商业或大依赖；
- 若后续需要 HRTF/混响，再评估 OpenAL Soft 或 Steam Audio，但不在首版。

### 5.3 玻璃声音

玻璃是高频、短衰减、多模态材料。建议分三层：

1. **Crack transient**：极短的高频噪声/脉冲，表示裂纹起始；
2. **Shard tinkle**：每个较大碎片落地时触发 2–6 个模态谐振器，频率随碎片尺寸升高，
   衰减 30–300 ms；
3. **Debris bed**：小碎片群用低增益、随机延迟的点击声铺底，避免每个碎片一个 voice。

模态谐振器可以用二阶带通/谐振器实现：

```text
y[n] = 2 r cos(w) y[n-1] - r^2 y[n-2] + gain * x[n]
```

其中 `r` 控制衰减，`w` 控制频率。首版不追求真实玻璃模态频率，只要求随碎片大小、
冲击能量和距离产生可辨识变化。

### 5.4 气球声音

气球破裂不是单一“砰”。建议三层：

- **Pressure burst**：短噪声爆裂 + 低频空气脉冲；
- **Rubber snap**：1–3 个快速衰减的中频谐振/摩擦噪声；
- **Air tail**：高压空气泄出的短噪声尾，随时间快速下降。

音高、噪声带宽和尾音长度按气球尺寸/压力随机化。视觉破裂事件和音频事件共享同一个 seed，
避免每次重放声音都不同。

### 5.5 空间化与预算

- 近场：距离衰减 + 左右声像；远场：低增益、低通；
- 每个事件有 `cooldown`，防止同一碎片连续接触产生机关枪式声音；
- 全局 voice 上限 32–64，按能量、距离和事件类型 steal；
- 同一次 `Shatter` 只播放一次主爆裂，碎片落地各自独立但受全局预算；
- 音频线程不分配或尽量少分配；GT 只投递固定大小事件。

## 6. 渲染集成

### 6.1 动态碎片资源

首版路径：

1. GT 在破碎决策后调用 `ExtrudePlanarShard` 得到 `FragmentMesh`；
2. 把 `positions/normals/indices` 转成 `Vertex`（UV 可先用局部平面投影）；
3. 用 `RenderableObject` 创建 GPU 资源，键包含
   `world generation | destructible id | fragment id | revision`；
4. `RendererResourceCache::BindRenderableObject` 绑定；
5. `World::AddMeshObject` 增加碎片 `WorldMeshObject`，复用玻璃材质实例；
6. 下一帧 `WorldSnapshot` 自然带出碎片 key、模型矩阵和材质。

动态资源必须在 GT 创建，不能在 RT 或音频线程碰 `RendererBackendVulkan`。
删除碎片时通过现有动态资源裁剪/退休路径释放，避免 World 换代后悬挂。

### 6.2 透明与排序

玻璃碎片如果走透明材质，需要解决：

- 碎片之间的深度排序；
- 透明写入深度/不写深度的 pass 选择；
- 阴影：透明玻璃通常不投完整阴影，但碎片的厚度和边缘可能需要替代阴影；
- 反射/折射：首版不要求，使用简化玻璃材质。

如果透明排序成本过高，首版可用“半透明但近似不透明”的材质做视觉验证，再逐步接入
现有 forward transparent pass。这个决定必须在场景验收前定下来。

### 6.3 裂纹覆盖

在完全碎裂前，玻璃需要可见裂纹。可选路线：

- 单独一层裂纹贴花/薄片 mesh，使用 alpha 材质；
- 在玻璃材质里增加裂纹 mask 参数；
- 用 `Debug::DrawDebug` 先做开发期裂纹线，不作为最终视觉。

首版建议先做裂纹 overlay mesh：它不改变完整玻璃碰撞，只负责视觉反馈，资产和 pass 改动较小。

### 6.4 气球碎片

气球碎片必须从运行时球壳拓扑生成：

- 软体路线需要每帧更新顶点位置/法线；`RenderableObject` 已增加 host-visible
  动态顶点缓冲和 `UpdateVertices`，调用方必须保持固定拓扑；
- 小碎片降级为刚体时，可以复用球壳碎片的凸包点和静态网格；
- 两种路径都通过稳定的 `meshKey + generation` 进入 `WorldSnapshot`，不能把粒子数组塞进快照；
- 材质透明度、厚度和双面渲染需要和薄壳软体一起验证。

## 7. 线程、生命周期与确定性

- 破碎状态、Jolt body、RNG 和动态资源键都由 GT 拥有。
- 渲染线程只消费 `WorldSnapshot`，不得查询 `DamageState`。
- 音频线程只消费事件队列，不持有 `World` 或 Vulkan 资源。
- Stop/Play、World 换代、场景重载必须销毁所有碎片 body、动态资源 key 和音频 voice。
- 同一次破碎的几何、冲量和音频事件由同一 seed 派生；不能用帧率或线程调度决定碎片数量。
- 物理步长固定；渲染帧率只影响插值/快照频率，不影响碎裂判定。

## 8. 验证策略

### 8.1 已落地的 CPU 回归

`fracture_core_tests` 当前 42 项，覆盖：

- 半平面裁剪的面积与 CCW 顺序；
- Voronoi 覆盖域、无孔洞、凸性；
- 同 seed 确定性、不同 seed 变化；
- 子碎片面积守恒；
- 非法域/顺时针域拒绝；
- 冲击点在域外的投影；
- 径向裂纹图案覆盖凸域、尺寸随半径明显变化、同 seed 确定；
- 随机直线弦切割保持父碎片面积且输出仍为凸片；
- 挤出体积、外法线绕序、单位法线；
- 空碎片、非正厚度、非法三角列表拒绝；
- 球面 Voronoi 覆盖整个球面、同 seed 确定性、不同 seed 变化；
- 球壳碎片厚度小于半径、零冲击方向拒绝、球壳网格外向绕序；
- 运行时 icosphere 拓扑、压力充气、结构边/弯曲约束、爆裂应变；
- 球面 Voronoi 软片拆分、精确三角面裁剪、cell 包含性、橡皮回缩、拆分确定性；
- 应变刚化、粘弹阻尼、平滑法线渲染数据和动态顶点更新数据。
- 软片地面钳制不穿透。
- 爆裂冲量确定性地增加外向速度。
- 命中方向所在 cell 明显小于平均面积（破口从命中点开始）。
- 破口增强按 `cos²` 权重集中到命中点附近，而不是整体加速。
- neck 粒子 pin 后不被 XPBD 约束拖走。
- 刚体姿态增量能推动粒子位置/速度。

`physics_runtime_tests` 当前 9 项，覆盖 Jolt 投射物接触产出命名 balloon / glassPane
冲击事件、shard 与地面接触产出 landing 事件、凸包 shard 相互接触、`Clear` 移除
proxy/投射物后的空步进、碎片初始角速度驱动刚体旋转，以及 `RemoveShard` 释放 body；
投射物句柄查询与命中事件携带 `projectileId` 也由该套件覆盖。
`ScriptScene` 覆盖 balloon 字段校验。

### 8.2 下一批模块测试

- 冲击能量阈值：同一几何在不同质量/速度下的 crack/fracture/subfracture 决策；
- 支撑岛：删除一个 cell 后哪些块转为动态；
- 预算：超过全局碎片上限时拒绝切分，且不丢现有碎片；
- 确定性：同一 World 世代和 impact sequence 的碎片/音频事件完全一致；
- 音频谐振器：无 NaN、无自激、衰减到静音、峰值不越界；
- 事件队列：满队列时按优先级丢弃，不阻塞 GT。

### 8.3 集成与手工验证

不新增 Vulkan runtime test flag。集成验证使用：

- 固定场景夹具（玻璃板 + 投射物 + 地面）；
- 现有 World/Graph 事务与正常启动；
- 截图/日志记录碎片数量、位置和帧时间；
- 音频离线导出 WAV 后做频谱/峰值/衰减分析，不把音频设备接入 CI；
- 气球场景单独记录破裂时序和 voice 数。

当前实测纵切（2026-10-29）：

- `Maps/SC_softbody_balloon/SC_softbody_balloon.json` 通过 `validate_scene_schema.py`；
- `main.exe --initial-scene` 实时路径创建 162 顶点 / 320 三角面的完整气球；
- 充气阶段截图可见平滑球壳；
- 爆裂日志为 16 个软片，截图可见精确裁剪后的曲面软片；
- 完整气球移除时使用 `ResourceRetireQueue`，重复运行 stderr 无
  `vkDestroyBuffer-buffer-00922` 校验错误；
- 单线程与 `workerThreadCount=2` 的 RenderThread 运行均完成截图，stderr 为空。
- `softbody impact RubberBalloon 800` 由 Jolt 接触触发爆裂，日志为
  `SoftBody Jolt impact` → `SoftBody burst` → `Balloon burst audio queued`；
- SDL3 音频流以 48 kHz mono float 打开，爆裂样本为程序化生成；
- 单线程与 `workerThreadCount=2` 的 Jolt 冲击/音频链路均完成截图，stderr 为空。
- `Maps/SC_glass_fracture/SC_glass_fracture.json` 通过 `validate_scene_schema.py`；
- `glass impact WindowGlass 900` 日志为 `Glass pane impact` → `shattered: 80 fragments`
  → `Glass shatter audio queued`，随后 19 次落地二次碎裂各 3 片；
- 落地后截图可见整片玻璃板变成散布在地面的碎片，单/双线程运行 stderr 均为空。
- `Maps/SC_fracture_playground` 同时初始化 3 个气球和 2 块玻璃；
- `shoot` 在默认准星下命中 `BalloonCenter`（34.7 m/s）并爆裂；把相机对准
  `GlassLeft` 后同一命令触发 32 片玻璃碎裂，证明两种目标共享一个冲击世界；
- 可见箭在命中后冻结在命中点，截图可见准星、爆裂后的气球和落地碎片。

## 9. 里程碑与验收

### M0：分支、研究与 CPU 核心（当前）

- 建立 `codex/physics-fracture-audio` worktree；
- 落地 `vl_fracture_core` 的平面/球面运行时破碎、橡皮膜软壳、精确裁剪与 42 项单元测试；
- 写清玻璃/气球/声音的研究结论与风险。

### M0.5：气球实时引擎纵切（当前）

- 新增 `balloon` 场景类型、材质事务加载、World 描述和场景选择器识别；
- 新增 GT `SoftBodyRuntime`，发布动态 `RenderableObject` 到 `WorldSnapshot`；
- 完整气球零重力悬浮，爆裂后软片进入重力；
- 精确软片替换完整气球，旧动态网格通过 GPU epoch 退休；
- 实测充气与爆裂截图通过。

### M0.6：Jolt 冲击与程序化爆裂音（当前）

- Jolt 从 C# 专用开关抽出为共享 CPU 构建；
- 静态 balloon proxy + 动态投射物 + contact listener 产出命名冲击事件；
- `softbody impact` 调试命令贯通 Console → GT → Jolt → 软片替换；
- SDL3 音频流播放程序化压力爆裂/噪声尾音；
- 实测 `SoftBody Jolt impact`、`SoftBody burst`、`Balloon burst audio queued` 连续出现。

### M0.7：软片落地与空间衰减（当前）

- 每个软片注册 Jolt 动态凸包代理，调试地面为静态盒体；
- 软片之间由 Jolt 解析接触并产出 `ShardHit`；
- 软片 Jolt 接触产出 `ShardLanding`，程序化落地音按相机距离衰减；
- XPBD 粒子增加地面位置钳制，避免视觉软片穿地；
- 单气球碎片预算上限 48；
- 单/双线程实测 13 次左右落地音入队，stderr 为空；凸包软片运行无 capacity/validation 错误。

### M0.8：气球爆裂读感（当前）

- 爆裂冲量、空气阻尼和更强回缩；
- 冲击点附近更高密度的球面条带种子；
- 爆裂声拆为高频脆裂、压力、橡胶回弹和 flutter；
- 实测碎片爆裂后明显外翻扩散，随后落地触发声音。

### M0.9：颈口与刚体姿态耦合（当前）

- 最低处软片固定最近 3 个粒子，作为气球颈口悬挂条带；
- 非 neck shard 的 XPBD 粒子应用 Jolt 凸包姿态增量；
- 修正 pinned 粒子的 XPBD 有效逆质量，避免约束把锚点拖走；
- 实机爆裂截图无 Vulkan validation/capacity 错误。

### M0.10：命中点破口与空气扰动（当前）

- 破口拓扑的 `impactDirection` 改为真实命中点的局部方向，箭/投射物打到哪就从哪里开裂；
- 命中点附近的粒子按 `cos²` 权重获得额外径向冲量；
- 爆裂瞬间从命中点发布 0.18 秒扩张线框球壳，作为空气扰动占位；
- 每个 Jolt 碎片获得 seed 派生的确定性初始角速度，补上空中翻滚；
- 演示场景保持 `pressure: 0`，避免压力自爆时机不稳定；
- `fracture_core_tests` 增至 42 项、`physics_runtime_tests` 增至 5 项，覆盖破口 cell
  面积、puncture boost 分布与碎片角速度。

### M1：玻璃单板物理

- 当前（2026-10-29，首版已落地）：`glassPane` 场景对象、静态盒 proxy、投射物
  LinearCast、命中点局部 Voronoi（4–64 片）、凸包碎片、质量估算和落地二次碎裂
  已接通；细节见 `documents/contracts/glass-fracture-runtime.md`；
- 尚未完成：cracked 中间态、支撑岛/边框附着、碎片互相撞击触发二次碎裂、
  跨对象全局碎片预算；
- 验收：固定投射速度下碎片数量、总质量、飞散方向和二次碎裂次数可重复。

### M1.5：试玩关卡与共享冲击世界（当前）

- `EngineLoop` 持有唯一 `FractureImpactWorld`，气球与玻璃共享接触、事件和投射物；
- `ProjectileRuntime` 生成可见箭矢，左键/`shoot` 发射，命中后冻结 2.5 秒；
- debug draw 准星 + 0.15 秒射击冷却；
- `Maps/SC_fracture_playground`：3 气球 + 2 玻璃，Esc 鼠标捕获、WASD 移动；
- 实测同一场景内一支箭可打爆气球或打碎玻璃，日志分别命中对应运行时。

### M2：玻璃渲染

- 当前已落地：动态 `RenderableObject` + `WorldMeshObject` 注册、材质实例复用、
  完整板与碎片的 GPU 资源延迟退休；
- 尚未完成：透明排序、折射/双面材质、阴影策略；
- 裂纹 overlay；
- 验收：单/双线程截图一致，碎片 key 在 World 换代后完整释放。

### M3：玻璃声音

- `SDL3` 音频设备与轻量混音器；
- 物理事件 -> 音频事件队列；
- 裂纹、爆裂、碎片落地、二次碎裂四类声音；
- 验收：离线 WAV 的峰值、频谱重心、衰减时间在目标区间；实时不产生 NaN/爆音。

### M4：气球实时破裂

- 运行时球壳网格 + 粒子/距离/弯曲约束 + 内部压力；
- 运行时球面 Voronoi 切分，不读取预制条带；
- 精确裁剪后的碎片保留粒子/约束子图，作为软体薄片继续模拟；
- 预算降级时把碎片粒子烘焙为运行时凸包刚体；
- 回弹/阻尼/生命周期与爆裂复合声音；
- 验收：同一 seed 的破裂拓扑和粒子初态可重复，碎片在 1–2 秒内回收。

### M5：通用物体与 VACD

- 加载时近似凸分解（只服务碰撞/局部替换，不预存断裂图案）；
- 运行时局部凸块替换；
- 支撑岛检测；
- 更复杂的爆炸/连续冲击。

### M6：性能与打磨

- 碎片池化、批量上传、LOD/屏幕占比降级；
- 音频 voice steal 与距离低通；
- 单/双线程帧时间回归；
- 场景和资源文档。

## 10. 关键风险

1. **透明玻璃排序**：碎片多了以后透明 pass 的排序和 overdraw 可能比物理更贵。
   需要尽早用真实玻璃材质做性能采样。
2. **Jolt 接触冲量可见性**：当前脚本物理只发布接触对，不发布冲量。原生 typed 层必须先确认
   能拿到足够的能量信息，否则只能做速度近似。
3. **动态资源上传卡顿**：每片一个 `RenderableObject` 会创建多组 buffer；首版先验证正确性，
   后续必须做池化/批量上传，不能把“每片两次建 buffer”带进正式场景。
4. **碎片预算与玩法**：无限递归会拖垮物理和音频；预算必须和镜头距离、屏幕占比、声音优先级绑定。
5. **气球薄壳成本**：实时球壳粒子/约束比玻璃刚体贵得多。若首版无法稳定预算，降级为
   运行时生成碎片 + 刚体/少量软体混合，而不是退回预制条带。
6. **确定性**：任何时间戳、线程调度或全局 RNG 进入破碎决策都会破坏回放和测试。

## 11. 待验证问题

- 当前 Jolt 版本是否提供 contact impulse/相对速度，以及能否在 GT 同步路径安全读取；
- 现有透明 pass 能否直接消费动态碎片 mesh，还是需要单独的 debris pass；
- `RenderableObject` 的 buffer 创建能否在 GT 上以可接受频率调用；
- SDL3 音频在 Windows 双线程/编辑器 UI 场景下的设备初始化顺序；
- 玻璃材质实例是否已有合适的 opacity/refraction 参数，还是需要新材质模板；
- 资源仓库中是否已有可复用的玻璃/气球模型和声音素材；如果没有，先用程序化/临时素材。

## 12. 参考

- Matthias Müller, Nuttapong Chentanez, Tae-Yong Kim.
  *Real time dynamic fracture with volumetric approximate convex decompositions.*
  ACM Transactions on Graphics, 2013. DOI: `10.1145/2461912.2461934`.
- James F. O'Brien, Jessica K. Hodgins.
  *Graphical modeling and animation of brittle fracture.* SIGGRAPH 1999.
  DOI: `10.1145/311535.311550`.
- James F. O'Brien, Adam Bargteil, Jessica K. Hodgins.
  *Graphical modeling and animation of ductile fracture.* SIGGRAPH 2002.
  DOI: `10.1145/566570.566579`.
- James F. O'Brien.
  *Synthesizing Sounds from Physically Based Motion.* 2001.
  DOI: `10.1145/383259.383321`.
- Kees van den Doel, Dinesh K. Pai.
  *Modal Synthesis for Vibrating Objects.* 2007.
  DOI: `10.1201/9781439864869-7`.
- Federico Avanzini et al.
  *Physically-based audio rendering of contact.* ICME 2002.
  DOI: `10.1109/ICME.2002.1035636`.
- Jolt Physics documentation / source: `ConvexHullShape`, `MeshShape`,
  contact listener, body activation and island behavior.
- SDL3 audio API: `SDL_AudioStream`, `SDL_OpenAudioDeviceStream`,
  `SDL_LoadWAV`.
