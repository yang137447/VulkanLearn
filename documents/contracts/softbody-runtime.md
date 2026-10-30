# 橡皮软体气球运行时

> 状态：当前契约（实现已落地）。描述 `balloon` 场景对象从资源加载、World 描述、
> GT 侧 `SoftBodyRuntime`、Jolt 冲击、动态网格发布、爆裂软片和资产优先爆裂音的当前行为。
> 它不描述玻璃破碎、HRTF/混响等空间音频或编辑器软体作者工具；这些能力尚未接入。

## 场景对象

`balloon` 是 `SceneAssetValidator` 接受的 Scene object type。必填字段：

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `name`、`type` | string | `type` 逐字为 `balloon` |
| `position`、`rotation`、`scale` | vec3 | 与 mesh 相同，scale 不得为 0 |
| `materialInstance` | string | 非空；资源加载阶段通过 `RendererMaterialLoader::LoadSceneMaterialInstance` 创建 |
| `radius`、`thickness` | number | `radius > 0`，`0 < thickness < radius` |
| `subdivisions` | integer | `[0, 3]` |
| `siteCount` | integer | `[4, 64]` |
| `pressure` | number | `>= 0` |
| `burstStrain` | number | `> 0` |
| `restContraction` | number | `(0, 1]` |
| `seed` | unsigned integer | 破裂拓扑的确定性种子 |

`HasSupportedSceneObjectTypes` 认可 `balloon`，因此 UI 场景选择器不会把它当成未知场景。
离线渲染路径拒绝 balloon；当前只有实时路径创建软体。

## 资源与 World 数据

`RendererResourceLoadCoordinator` 在候选资源包中加载 balloon 的材质实例，产生
`SoftBodyObjectBuildPlan`。它只记录对象 TRS、软体参数、`materialKey` 和
`materialInstanceKey`，不创建软体网格。

`WorldBuilder` 把该计划转成 `WorldSoftBodyObject`。`World` 只保存作者参数与材质 key；
`RubberShell` 粒子、动态网格和爆裂状态不进入 `World`。

## GT 运行时

`SoftBodyRuntime` 在 World generation 变化时：

1. 用 `subdivisions` 运行时生成 icosphere；
2. 完整气球使用零重力，保持近中性浮力；
3. 创建 host-visible 动态 `RenderableObject`，key 形如
   `softbody|<scenePath>|<objectName>|<generation>|intact`；
4. 绑定到活动 `RendererResourceCache`；
5. 添加 `WorldMeshObject`，复用场景解析出的 `materialKey`/`materialInstanceKey`；
6. 后续 `WorldSnapshot` 通过普通 mesh draw 发布该对象。

每帧 GT `Tick` 调用 `RubberShell::Step`，随后 `BuildRenderMesh` 并写入
`RenderableObject::UpdateVertices`，同时更新 `WorldMeshObject` 局部 bounds。

## 爆裂

完整气球 `ShouldBurst()` 后：

1. 先向 `FractureBudget` 申请软片数：回收其他对象已静止的旧碎片，再按剩余
   全局额度裁剪 `siteCount`（单气球上限 48，视觉下限 4）；随后用 descriptor 的
   seed/radius/thickness 和裁剪后的 siteCount 运行时生成球面 Voronoi；
   冲击触发时 `impactDirection` 取世界命中点经 `model` 逆变换后的局部方向，
   因此破口 cell 始终落在实际命中点；压力自爆没有命中点时使用固定默认方向；
2. 调 `RubberShell::TearBySphericalPatternExact` 做三角面级精确裁剪；
3. 移除完整气球的 `WorldMeshObject`；
4. 完整气球的 `RenderableObject` 进入 `ResourceRetireQueue`，等 GPU epoch 完成后释放；
5. 每个软片建立独立动态 `RenderableObject` 和 `WorldMeshObject`，name 为
   `<objectName>~shard<N>`，scene identity 仍为 `<objectName>`；
6. 软片重力为 `(0, -981, 0)` cm/s²，完整气球重力为零；
7. 爆裂时对碎片施加径向 `≈220 cm/s`（随冲击速度缩放）和切向随机冲量；命中点附近
   的粒子额外获得 `0.9 * cos²` 权重的径向增量，表现破口先被气流冲开。碎片阻尼设为
   `4 s^-1`，静止边长按场景 `restContraction` 收缩；
8. 最低处的碎片作为 neck shard：最近 3 个粒子固定为 pinned，不注册 Jolt body，
   其余碎片注册 Jolt 凸包并跟随其姿态；每个凸包还获得由 seed 派生的确定性初始角速度
   （`2.5–8 rad/s`，按冲击能量缩放），避免碎片只平动、不翻滚；
9. 爆裂瞬间记录一个 0.18 秒的空气扰动：以命中点（压力自爆时用气球中心）为圆心，
   用 debug draw 发布一个半径扩张并淡出的线框球壳。它只是视觉占位，不进入物理、
   音频或 `WorldSnapshot`，结束时按 owner 移除 overlay key。

## 漏气（低能量命中）

Jolt 冲击的相对速度低于 `4.0 m/s` 时不爆裂，而是进入漏气状态：

1. 壳的重力设为 `(0,-981,0)`，气球开始下落；
2. 阻尼设为 `3 s^-1`，并施加整体向内的 `ApplyDeflateImpulse`；
   同时按“破口方向的反向”施加 `ApplyAerodynamicThrust`：整体平移加速度 +
   绕质心自旋（`950 cm/s²`、`7 rad/s`，随 1.4 秒泄压线性衰减），
   让气球边漏气边乱飞、翻跟头；
3. 接下来 `1.2 s` 每帧按 `dt` 继续向内抽气，压力保持 0；
4. 粒子受地面钳制，最终成为落在地面的塌陷软壳，不生成碎片；
5. 渲染破口：`SetRenderHole` 在 `BuildRenderMesh` 中跳过命中点附近的三角面
   （0.12 → 0.22 rad 随泄压扩大）；只是渲染挖洞，粒子与约束仍保持闭合；
6. 播放 1.15 秒资产嘶声 + 反向充气膜层；资产缺失时回退 0.9 秒程序化嘶声。

漏气状态不参与 `ShouldBurst`，因此不会在塌陷过程中意外二次爆裂。

## Jolt 冲击

`FractureImpactWorld`（原名 `SoftBodyImpactWorld`）是独立的 Jolt CPU 世界，
不属于 C# Blueprint 物理世界；它同时服务气球和玻璃板，玻璃侧行为见
`glass-fracture-runtime.md`：

- 每个 balloon 注册一个静态球体 proxy，半径来自 balloon descriptor，并显式标记
  `FractureMaterialProfile::Balloon`；
- 爆裂后每个软片注册一个动态 Jolt convex hull proxy，同样标记 `Balloon`；
  点集不足时回退球体，初始速度和中心来自软片粒子平均；
- 调试/游戏投射物是动态球体，位置/速度从厘米边界换算成米；
- `ContactListener::OnContactAdded` 产出 `FractureImpact`（balloon name、接触点、法线、
  相对速度、material profile）；
- `SoftBodyRuntime::HandleImpacts` 只消费 `materialProfile == Balloon` 的事件，
  再按命中速度调用 `BurstInstance` 或漏气；
- 调 `softbody impact <name> [speed]` 时从 `+Z` 方向发射球体，默认速度 800 cm/s；
- shard 与地面接触产出 `ShardLanding` 事件，驱动软片落地音；
- 球面碎片使用 `impactBias=0.88` 的环状种子分布；单气球最多 48 片；
- 调试地面是 Jolt 静态盒，顶面位于 `y=-100 cm`；World 换代、`Shutdown` 或 `Clear`
  会移除 balloon/shard/projectile body。

Jolt 注册由 `physics/runtime/joltRuntime.*` 的进程级 token 统一拥有；C# Blueprint
与原生软体冲击不会各自创建第二份 Factory/Types。

## 声音

气球音效只负责提交 voice；输出是 48 kHz stereo、32 voice 的块混音器
（voice 预算、pan、距离低通见 `audio-runtime.md`）：

- 爆裂优先使用 `Common/Audio/balloon_pop.wav`（CC0）的真实录音，再叠加
  压力脉冲、橡胶回弹和尾部 flutter；
- 漏气优先使用 `gas_leak.wav` + 反向 `balloon_inflate.wav`；资产缺失时回退
  分层程序化样本；
- 软片落地音是 0.10 秒的衰减共振/噪声点击，按距离/方向做衰减与 pan，
  最近邻事件有 24 ms 冷却；
- 资产在启动时预加载并重采样到 48 kHz，播放时只复制样本，不阻塞 GT；
- 音频设备不可用时只报告 warning，不影响渲染和软体模拟。

## 当前边界

- 软片 Jolt 代理是凸包；软片之间的接触由 Jolt 解析，并产出 `ShardHit` 声音事件；
- 非 neck shard 的 XPBD 粒子由 Jolt body 的世界姿态增量驱动，再做局部软体求解；
  这不是统一的 deformable-hull 求解器，neck shard 仍是纯 XPBD。
- 地面是固定 `y=-100 cm` 的调试平面，不是场景网格碰撞。
- 空气扰动是 debug draw 的线框球壳占位，不是折射、体积雾或粒子系统；它只在
  debug draw 开启时可见。
- 声音有资产/程序化回退、距离衰减和全局 32 voice 预算，但没有 HRTF、混响或遮挡。
- 气球软片与玻璃碎片共享全场景 320 片预算；单气球上限 48。预算不足时先回收
  已静止软片，再裁剪本次爆裂的软片数，最低保留 4 片并记录超出量。
  详见 `fracture-budget.md`。
- 材质复用场景 MI；气球专用透明/双面材质和阴影策略尚未实现。
- 编辑器只支持场景选择器识别 balloon，不提供软体参数编辑面板。
- 离线渲染路径不支持 balloon。
