# Decal Runtime

> 状态：**当前契约**。本文件描述 VulkanLearn 已落地的 `BeforeLighting`
> GBuffer decal 子集，不是 UE Deferred Decal 的完整替代。

## Pass 顺序

```text
geometry
  -> decalStencil（D24S8，unit cube 写 stencil bit0）
  -> decals（stencil test + 写 DBuffer A/B/C/D 与 DBufferMask）
  -> deferredLighting
  -> forwardOpaque / sky / forwardTransparent / particle
```

`decalStencil` 是独立的 `RenderGraphPassType::DecalStencil`，只挂
`decalStencil`（`D24_UNORM_S8_UINT`）深度/stencil 附件，不写颜色。
它和 `decals` 分开的原因是不能在同一个 pass 里既采样 `sceneDepth`
又把同一张 depth 当 attachment：`decals` 需要 `sceneDepth` 采样重建
世界位置，而 `decalStencil` 是它的 stencil attachment。`RenderGraphCompiler`
会拒绝任何 input/output 指向同一资源的 pass（`PassFeedbackLoop`）。

硬件 mask 算法：

1. `decalStencil` pass 先 clear stencil 为 `0`；`DecalRenderer` 用与
   decals 完全相同的 unit cube、实例 SSBO 和排序结果绘制所有可见 decal。
   pipeline 为 `cullMode=None`、depth test/write 关闭，stencil test 为
   `always`、`passOp=replace`、`writeMask=0x01`、`reference=1`。
2. 每个 cube 面覆盖的像素把 stencil bit0 置 `1`。cull 关闭保证相机进入
   decal 体积时，剩余可见面仍能写 mask，而不是因所有面都变成背面而漏写。
3. `decals` pass 以同一 `decalStencil` 为深度/stencil attachment，
   `stencilLoadOp=load`，pipeline 为 stencil test 开启、`compareOp=notEqual`、
   `reference=0`、`compareMask=0x01`、`writeMask=0`，失败片元在进入
   fragment shader 前被固定功能丢弃。

这里采用 union mask（所有可见 decal 共用 bit0），不是逐 decal 独立
stencil 值。像素落在 union 内但不在某个 decal 的投影体时，仍由 decals
fragment shader 用 `sceneDepth` 重建和局部 `[-1,1]` cube 判断丢弃；
因此 mask 是硬件屏幕空间 early gate，不是最终几何包含性判断。
静态 decal 与 `renderMode: "decal"` 粒子 decal 共享这同一遍 mask，
共享 64 槽排序与实例数据。

`decals` 是独立的 `RenderGraphPassType::Decal`，由 `DecalRenderer` 拥有
unit cube、实例 SSBO、pipeline 与 descriptor，并额外拥有
`DecalStencil` 的 mask pipeline。它不是 particle pass，也不经过 Material
图或普通 mesh draw。

`deferredLighting` 与 `forwardOpaque` 按像素屏幕 UV 读取同一组 DBuffer。
forward opaque 通过 set 3 binding 1..5 绑定 `dbufferA/B/C/D/Mask`，在材质
forward shading 前调用同一个 `ApplyDecalDBuffer`。透明表面不读取 DBuffer：
它在 deferred depth 之前时没有可靠的逐像素 decal 深度，直接把后方 opaque
的贴花串到前方透明面上会产生错误遮挡。

## 场景对象

```json
{
  "name": "GroundScorch",
  "type": "decal",
  "position": [0, 0, -800],
  "rotation": [0, 0, -90],
  "size": [100, 1400, 1400],
  "color": [0.012, 0.009, 0.006],
  "opacity": 0.92,
  "edgeSoftness": 0.48,
  "noiseStrength": 0.72,
  "roughness": 0.9,
  "metallic": 0.0,
  "ambientOcclusion": 0.45,
  "decalResponse": "colorNormalRoughness",
  "normalStrength": 0.35,
  "albedoMap": "Maps/SC_example/Textures/T_scorch_albedo.json",
  "normalMap": "Maps/SC_example/Textures/T_scorch_normal.json",
  "ormMap": "Maps/SC_example/Textures/T_scorch_orm.json",
  "sortOrder": 0
}
```

- `size` 是完整局部尺寸（cm），X 是投影深度，Y/Z 是投影平面宽高；内部
  以 `size * 0.5` 烘进 `decalToWorld`。
- `position`/`rotation` 是场景作者 TRS；`scale` 可选，非零，参与半尺寸。
- `opacity`、`edgeSoftness`、`noiseStrength` 都在 `[0, 1]`；`sortOrder`
  是 `[0, 255]` 整数。所有数值必须有限。
- `roughness`、`metallic`、`ambientOcclusion` 可选，默认 `1/0/1`，范围
  都在 `[0, 1]`，通过 DBufferC/D 合成到接收表面。
- `decalResponse` 可选，默认 `colorNormalRoughness`，取 UE
  `EDecalResponse` 对应的枚举名：`colorNormalRoughness`（Color + Normal +
  Roughness，不含 Metallic）、`colorNormal`、`color`、`normal`、
  `roughness`、`roughnessNormal`、`metallic`、`metallicNormal`、
  `metallicRoughness`、`metallicRoughnessNormal`。它描述 decal 希望覆盖
  哪些 DBuffer 通道；GPU 侧写入 `DBufferMask`，`deferredLighting` 在
  合成时逐通道消费。
- `normalStrength` 可选，默认 `0.35`，范围 `[0, 1]`；0 表示忽略法线响应。
- `albedoMap`、`normalMap`、`ormMap` 可选，值必须是**非空字符串**，引用
  `T_*.json` 纹理资产。三者默认都是空字符串，此时沿用现有的程序化
  径向 + noise/FBM 外观。`ormMap` 采用 glTF/UE 的 ORM 打包约定：
  R=roughness，G=metallic，B=ambientOcclusion。SceneAssetValidator 只
  校验字段类型与非空，不做文件系统 I/O；路径是否存在、资产是否可加载
  由 `DecalRenderer::Synchronize` 在渲染线程加载时决定，失败回退默认纹理。
- decal 材质完全由代码维护（shader + C++ 参数），**没有 UE Material
  Graph，也没有引入计划**。上面的贴图字段是可选纹理槽位，不是材质图
  节点描述。
- `type=decal` 的对象不创建网格或材质资源；SceneAssetValidator 负责
  字段边界，WorldBuilder 负责转换成 `DecalSnapshot`。

## 粒子驱动动态 decal

`PS_*.json` 的发射器支持 `renderMode: "decal"`。它不是新的 decal pass，
也不改变 DBuffer 布局；CPU 运行时把每个存活粒子转换成一个
`DecalSnapshot`，追加到 `RenderScene::decals` 后复用本文件描述的
`DecalRenderer`、DBuffer 合成、贴图槽与材质 descriptor。

```json
{
  "renderMode": "decal",
  "decal": {
    "size": [60.0, 300.0, 240.0],
    "rotation": [0.0, 0.0, 90.0],
    "color": [0.5, 0.25, 0.125],
    "opacity": 0.75,
    "edgeSoftness": 0.4,
    "noiseStrength": 0.6,
    "roughness": 0.8,
    "metallic": 0.1,
    "ambientOcclusion": 0.7,
    "normalStrength": 0.3,
    "decalResponse": "metallicRoughnessNormal",
    "albedoMap": "Maps/SC_example/Textures/T_burn_albedo.json",
    "normalMap": "Maps/SC_example/Textures/T_burn_normal.json",
    "ormMap": "Maps/SC_example/Textures/T_burn_orm.json",
    "sortOrder": 7
  }
}
```

- 字段边界与场景 decal 相同：`size` 每轴 `(0, 100000]`，rotation 有限，
  color 在 `[0, 64]`，其余材质/覆盖参数在 `[0, 1]`，`sortOrder`
  是 `[0, 255]` 整数。`decalResponse` 复用
  `world/decalResponse.h` 的 UE 枚举映射。三个贴图字段可为空字符串；
  空值使用程序化外观。
- 每个存活粒子先删除上一帧动态条目再重建，不堆叠。`decalId` 由
  发射器身份（scene object + assetPath + emitter name）与其稳定粒子槽位
  派生，不包含 burst 代际，因此 `DecalRenderer` 的纹理集可以在同一槽位
  反复生成/死亡时复用。
- `decalToWorld` 与场景 decal / `WorldBuilder` 使用同一约定：

  ```text
  position = particleWorldPosition
  basis    = normalizeColumns(emitter.model.linear)
  matrix   = T(position) * basis * R(decal.rotation) * scale(decal.size * 0.5)
  ```

  单位 cube 的 X 是投影深度，Y/Z 是投影平面，像素 UV 仍使用局部 ZY。
  父级缩放不进入 decal 体积；`decal.size` 是唯一尺寸来源。
  `localSpace=true` 的粒子位置先乘发射器模型。
- 颜色为 `decal.color * mix(colorStart.rgb, colorEnd.rgb) *
  colorTint.rgb`；opacity 为 `decal.opacity * clamp(mix(alpha) *
  colorTint.a, 0, 1)`。roughness/metallic/AO/normalStrength、response、
  贴图与 `sortOrder` 直接取自 `decal`。
- `DecalRenderer` 的 64 个实例槽由静态可见 decal 与粒子 decal 共享。
  运行时先移除自己的上一帧条目，再按剩余静态可见数量计算动态容量；
  超出时按 particleSystems / emitter 顺序确定性截断。静态 decal 保留
  原槽，不参与粒子预算。
- `RenderSystem` 在 `DecalRenderer::Synchronize` 前更新粒子 decal，
  因此纹理集看到的是本帧最终列表。实例 SSBO 与每 swapchain image
  的实例缓冲仍是固定 64 槽，没有逐帧扩容。

## 投影与输出

- `DecalRenderer` 用一个单位 cube 实例化绘制所有可见 decal，最多 64 个；
  实例按 `sortOrder` 稳定排序。静态场景 decal 与
  `renderMode: "decal"` 的 CPU 粒子 decal 共享该上限。
- 顶点只做 `decalToWorld -> viewProjection`；像素用 `sceneDepth` 重建
  世界位置，再乘 `worldToDecal`。
- 局部 cube 落在 `[-1,1]` 外则丢弃。局部 X 是投影深度，UV 使用局部
  `zy`，与 UE `DeferredDecal.usf` 的 `SwizzlePos.zyx` 语义一致。
- `DBufferA` 写 baseColor + color coverage；`DBufferB` 写**带符号的
  world normal** + normal coverage；`DBufferC` 写 roughness/AO +
  roughness coverage；`DBufferD` 写 metallic + metallic coverage。
  每个 decal 只写自己 response 允许的通道，避免重叠时把未响应通道的
  默认值泄漏给接收表面。UE 把 roughness/metallic/AO 打包在 DBufferC；
  VL 拆成 C/D，是为了让 roughness-only 与 metallic-only decal 重叠时
  仍能各自独立合成。四个数据缓冲区都由固定功能 AlphaBlend 预乘
  coverage，`deferredLighting` 按 over 形式合成。
- `DBufferB` 是 `R16G16B16A16_SFLOAT`（A/C/D 也是 16F），因此
  不需要 UE UNORM DBuffer 的 `128/255` 偏置编码。写端直接输出单位
  world normal，混合状态 `SrcAlpha / OneMinusSrcAlpha` 把它变成覆盖率
  预乘的向量和：

  ```text
  P = DBufferB.rgb = Σ N_i · w_i      （w_i 是 over 合成的逐层权重）
  c = DBufferB.a   = 1 - Π(1 - a_i)
  ```

  单层 decal 满足 `|P| = c`；多层方向相反而部分抵消时 `|P| < c`。
  合成端不再除 `c` 恢复平均法线，而是直接用 `P` 的模长作为相干强度，
  避免低覆盖率放大 fp16 噪声。
- `DBufferMask` 是 `R8G8B8A8_UNORM`，RGBA 依次对应
  color/normal/roughness/metallic 响应位。decal pass 用 One/One 加法
  累积（重叠 decal 的同类位按“或”处理），合成端先 saturate 成 0/1，
  再乘到对应缓冲区的 coverage 上。AO 没有独立响应位，随 roughness 位生效。
- DBuffer A/B/C/D 与 Mask 使用不同的固定功能混合状态，因此设备必须启用
  `independentBlend`；`VulkanManager` 在逻辑设备创建时校验并显式启用，
  缺失时在初始化阶段报错。
- 法线来源由 `normal.w` 选择：成功加载 `normalMap` 时采样切线空间法线
  （xy 乘 `normalStrength`），按 `(-t.z, t.y, t.x)` 映射到局部基；否则
  以 FBM 高度场在 decal UV 上做有限差分，得到局部切平面法线
  `(-1, -dH/dV*s, -dH/dU*s)`，`s = normalStrength * 0.12`。两条路径都用
  `decalToWorld` 的逆转置变换到世界空间。`normalStrength == 0` 时不写
  normal coverage，DBufferB 不生效。
- 法线合成由 `engine/decalComposite.glsl` 的 `DecalBlendWorldNormal`
  完成，输入是接收表面法线 `base`、预乘和 `P` 与总覆盖率 `c`：
  - 以 `base` 为 +Z 建正交基，把 `N = normalize(P)` 分解为切向
    `(dx, dy)` 与法向 `dz`。
  - `dz > 0`（同半球）走 **whiteout**（坡度叠加，Barré-Brisebois &
    Hill 2012）：base 在该局部基里的坡度为 0，
    `slope = (dx/dz, dy/dz) * fade`，whiteout 的坡度形式给出

    ```text
    normalize(base + T * slope.x + B * slope.y)
    fade = min(c, |P|)
    ```

    单层 decal 有 `fade = c`，所以 `fade = 0` 精确回到 `base`、
    `fade = 1` 精确回到 `N`；中间按坡度淡入，等价于标准
    detail-normal whiteout。多层时 `|P|` 的衰减会额外压低 detail
    强度，这正是相消程度的体现。
  - `dz <= 0`（跨地平线）gnomonic 坡度无定义，改用单位球上的
    **great-circle** 插值：旋转角 `acos(dz) * fade`，轴取
    `(T*dx + B*dy) / |(dx, dy)|`；对跖点 `|(dx, dy)| ≈ 0` 时取由
    `base` 唯一确定的切线。因此 `coverage = 0.5`、decal 法线与接收
    法线相反时，结果是与 `base` 垂直的单位向量，而不是零向量或 NaN。
  - `|P| ≈ 0`（多层法线完全相消）稳定回退到 `base`。
- 这与 UE 5.8 legacy DBuffer 的
  `WorldNormal = normalize(WorldNormal * NormalOpacity + PreMulWorldNormal)`
  有意不同：UE 的线性组合在对跖点 50/50 仍会得到零向量。VL 选择
  whiteout + great-circle 是为了让该退化不进入光照；同半球仍保持
  detail-normal 的坡度语义。这不是完整的 UE RNM 等价，残余近似见
  「当前限制」。
- 这是 `BeforeLighting` 子集：decal 在 `deferredLighting` 前修改
  `MaterialSurface`，因此仍接受场景光照；deferredLighting 通过
  `ApplyDecalDBuffer` 合成，不读写同一张 GBuffer。

## 贴图采样

- 贴图槽位由代码维护：`shader/glsl/decal/decal.frag` 声明
  `set = 1, binding = 0/1/2` 的 albedo/normal/ORM `sampler2D`，没有
  Material Graph，也没有材质节点编辑器。
- `DecalRenderer::Synchronize(const RenderScene&)` 在渲染线程、任何
  render pass 之外、录制命令之前调用；它按 `DecalSnapshot::decalId`
  维护纹理集，只有 decal 出现、消失或三个路径变化时才重新解析。
- 贴图通过 `LoadTextureAssetDesc` 与 `textureAssetLoader` 的
  `ToTextureCreateDesc` / `BuildTextureCacheKey` 加载；同 key 的纹理以
  weak 索引共享。decal 消失时纹理集交给 `ResourceRetireQueue`，等引用
  它的最后一个已提交 epoch 完成后再析构。
- 缺失/加载失败回退 1x1 默认纹理并只告警一次：白 sRGB albedo、
  `(128,128,255)` 线性 flat normal、白线性 ORM。`normalMap` 只有真正
  加载成功才把实例的 `normal.w` 置 1，否则 shader 继续走程序化法线。
- 采样语义：`baseColor = instance.color.rgb * albedoSample.rgb * mottling`，
  覆盖率乘 `albedoSample.a`；ORM 与作者值逐通道相乘
  （`roughness = surface.r * orm.r` 等）。
- 每个 decal 的材质 descriptor set 由 per swapchain image × per
  instance slot（最多 64）的 pool 提供；录制时逐 decal 绑定 set 1，
  `drawIndexed(36, 1, 0, 0, slot)` 让 `gl_InstanceIndex` 选中预填充的
  实例槽位。贴图路径未变化的槽位不会重写 descriptor。

## 当前限制

- 贴图没有运行时热重载：缓存 key 只包含路径与加载参数。同一场景重新
  加载时 decalId 和路径都不变，`Synchronize` 不会重新读文件；只有
  路径变化，或 shader reload/resize/进程重启清空纹理集与缓存后，才会
  重新加载磁盘内容。
- decal 贴图不做 bindless 数组；固定上限 64 个 decal，每个 swapchain
  image 为每个实例槽位维护独立 descriptor set。
- normal decal 的 DBufferB 只保存 `P = Σ N_i·w_i` 与总覆盖率 `c`，
  无法恢复每一层的独立权重和顺序；多层叠加先在世界空间法线上做线性
  累积，再对合成方向做一次 whiteout。方向接近相反时 `|P|` 会衰减
  （这是有意保留的相干性度量），完全相消时回退到接收表面法线。这与
  UE 按层顺序逐次执行 normal blend 的结果不等价。fp16 每通道约 11 bit
  有效精度；因为没有"除以覆盖率"的步骤，低覆盖率不会放大噪声。
- 没有真正的 RNM：RNM 需要每个 decal 在接收表面切空间里的偏导数并按
  层重建，而当前 DBuffer 只存合成后的法线向量，也不保存逐层顺序。
  要做到完整 UE 等价需要额外的逐层 payload 或有序 resolve，不在当前
  DBuffer 布局内；若将来引入，必须同时改 DBuffer 格式、
  `config/renderGraphConfig.json` 的附件/清屏值和本契约。
- 硬件 stencil 只提供屏幕空间 union mask，不是逐 decal 的 stencil ID，
  也不提供 per-pixel decal depth。像素的最终包含性仍由 `sceneDepth`
  重建 + 局部 cube 判断；因此 stencil 是 early gate/性能与反馈环隔离
  结构，不是完整的 UE 逐层 decal depth/stencil。
- stencil 资源是 `D24_UNORM_S8_UINT`，只使用 stencil bit0；当前没有
  stencil 值分组、stencil 深度剔除或 forward/transparent 接收路径。
- `ForwardOpaque` 已按屏幕 UV 接收 DBuffer；`ForwardTransparent` 明确不
  接收，因为当前 DBuffer 没有 per-pixel decal depth/stencil 来判断透明面
  与后方 opaque 的遮挡关系。粒子仍走自己的粒子 shader，不会自动接收 decal。
- decal 纹理集按 `decalId` 创建并交给 `ResourceRetireQueue` 退休；
  实例数据仍走每 swapchain image 的 SSBO。粒子 decal 的稳定槽位在寿命间隙
  期间保留纹理集最多 120 帧，超过后正常退休；World 换代立即清空持久集合，
  避免 emitter 删除/重命名后长期占用默认或真实纹理。
- 粒子 decal 只是静态 decal 的 CPU 动态来源。它不按命中表面自动定向、
  没有逐粒子图集/材质图或逐层 RNM；它复用静态 decal 的硬件 stencil
  union mask 与 64 槽实例数据。动态与静态共享 64 槽时，
  粒子路径没有抢占静态 decal 的优先级。Beam 的 `beam.hitScan` 是独立的
  受控平面命中，不会把命中法线传给粒子 decal。

## 验证

- 数据/边界：`tool/script-tests/scriptRuntimeTests.cpp` 的
  `ScriptScene.DecalComponentValidatesProjectionAndRanges` 与
  `ScriptScene.DecalResponseMapsToDBufferMask`；
  `ScriptRuntimeTest.DecalResponseAndNormalStrengthSurviveSnapshot`
  覆盖 World -> Snapshot 的 DecalResponse、normalStrength 与三个贴图
  路径数据传递。
- 法线合成数学：`tool/renderer-backend-tests/decal_normal_blend_test.cpp`
  对 `source/render/decal/decalNormalBlend.h`（shader 的 CPU 镜像）做
  数值回归：coverage 0/1、同半球半覆盖、对跖点 50/50 不抵消、跨半球
  满覆盖、多层完全相消回退、`normalStrength == 0` 不写 coverage。
- Graph/pipeline 合同：`tool/renderer-backend-tests/decal_stencil_graph_test.cpp`
  锁定 `decalStencil` 的 `D24_UNORM_S8_UINT` 附件、mask->decals 顺序、
  bit0 write/reference、`notEqual` gate、默认 stencil 关闭、反馈环拒绝、
  无 stencil 附件拒绝与非法 op/mask 拒绝；
  `tool/renderer-backend-tests/pass_pipeline_contract_test.cpp` 锁定
  `PassPipelineContractKey` 的 normalized key 与 `GraphicsPipelineStateDesc`
  映射。
- 粒子驱动路径：
  `tool/material-instance-editor-tests/particleDecalRuntimeTests.cpp`
  覆盖 schema/capability/round-trip、burst 确定性、decalToWorld 约定、
  fixedTimeStep、lifetime、shape/gravity、动态条目移除、稳定槽位 ID、
  静态/动态 64 槽分配、同名不同 assetPath、disable/re-enable；
  `tool/editor-ui-tests` 覆盖 Emitter 页 decal 控件与 World 预算排除；
  `tool/offline-integration-tests` 锁定离线模拟跳过 decal。
- 运行时：`SC_particle_ue_impact` 的墙面/地面贴花与
  `SC_particle_ue_explosion` 的 `ExplosionScorch` 应在
  `deferredLighting` 前把地面基色压暗；日志不应出现 `VUID-` 或
  `Validation Error`，也不应出现 `[Decals][Warning]`（出现即说明
  albedo/normal/ORM 之一加载失败并回退默认纹理）。
- 无 decal 场景（如 `SC_particle_advanced`）必须保持原有画面：DBuffer
  默认清除为零，`deferredLighting` 不合成任何贴花。
