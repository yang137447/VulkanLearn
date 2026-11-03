# Decal Runtime

> 状态：**当前契约**。本文件描述 VulkanLearn 已落地的 `BeforeLighting`
> GBuffer decal 子集，不是 UE Deferred Decal 的完整替代。

## Pass 顺序

```text
geometry
  -> decals（写 DBuffer A/B/C/D 与 DBufferMask）
  -> deferredLighting
  -> forwardOpaque / sky / forwardTransparent / particle
```

`decals` 是独立的 `RenderGraphPassType::Decal`，由 `DecalRenderer` 拥有
unit cube、实例 SSBO、pipeline 与 descriptor。它不是 particle pass，也不
经过 Material 图或普通 mesh draw。

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
- `type=decal` 的对象不创建网格或材质资源；SceneAssetValidator 负责
  字段边界，WorldBuilder 负责转换成 `DecalSnapshot`。

## 投影与输出

- `DecalRenderer` 用一个单位 cube 实例化绘制所有可见 decal，最多 64 个；
  实例按 `sortOrder` 稳定排序。
- 顶点只做 `decalToWorld -> viewProjection`；像素用 `sceneDepth` 重建
  世界位置，再乘 `worldToDecal`。
- 局部 cube 落在 `[-1,1]` 外则丢弃。局部 X 是投影深度，UV 使用局部
  `zy`，与 UE `DeferredDecal.usf` 的 `SwizzlePos.zyx` 语义一致。
- `DBufferA` 写 baseColor + color coverage；`DBufferB` 写编码到 `[0,1]`
  的 world normal + normal coverage；`DBufferC` 写 roughness/AO +
  roughness coverage；`DBufferD` 写 metallic + metallic coverage。
  每个 decal 只写自己 response 允许的通道，避免重叠时把未响应通道的
  默认值泄漏给接收表面。UE 把 roughness/metallic/AO 打包在 DBufferC；
  VL 拆成 C/D，是为了让 roughness-only 与 metallic-only decal 重叠时
  仍能各自独立合成。四个数据缓冲区都由固定功能 AlphaBlend 预乘
  coverage，`deferredLighting` 按 over 形式合成。
- `DBufferMask` 是 `R8G8B8A8_UNORM`，RGBA 依次对应
  color/normal/roughness/metallic 响应位。decal pass 用 One/One 加法
  累积（重叠 decal 的同类位按“或”处理），合成端先 saturate 成 0/1，
  再乘到对应缓冲区的 coverage 上。AO 没有独立响应位，随 roughness 位生效。
- DBuffer A/B/C/D 与 Mask 使用不同的固定功能混合状态，因此设备必须启用
  `independentBlend`；`VulkanManager` 在逻辑设备创建时校验并显式启用，
  缺失时在初始化阶段报错。
- 法线目前完全程序化：以 FBM 高度场在 decal UV 上做有限差分，得到
  局部切平面法线 `(-1, -dH/dV*s, -dH/dU*s)`，`s = normalStrength * 0.12`；
  再用 `decalToWorld` 的逆转置变换到世界空间。没有 decal 法线纹理，
  也没有 RNM/whiteout 多层法线重建。
- 这是 `BeforeLighting` 子集：decal 在 `deferredLighting` 前修改
  `MaterialSurface`，因此仍接受场景光照；deferredLighting 通过
  `ApplyDecalDBuffer` 合成，不读写同一张 GBuffer。

## 当前限制

- 没有 decal 纹理/材质图；颜色、透明度和法线都来自程序化径向 +
  value-noise / FBM，材质 `DecalResponse` 节点尚未存在。
- 法线路径只适用于单层 decal；重叠 normal decal 没有 RNM/whiteout
  重建，叠加结果不保证与 UE 一致。
- 没有硬件 depth/stencil 附件；像素以 `sceneDepth` 重建投影体判断，
  大屏或相机进入 decal 体积的鲁棒性仍需后续 stencil 路径。
- 只影响 deferred opaque GBuffer；`ForwardOpaque`、透明表面和粒子不会
  自动接收 decal。
- 没有逐 decal 资源创建/退休；实例数据走每 swapchain image 的 SSBO。

## 验证

- 数据/边界：`tool/script-tests/scriptRuntimeTests.cpp` 的
  `ScriptScene.DecalComponentValidatesProjectionAndRanges` 与
  `ScriptScene.DecalResponseMapsToDBufferMask`；
  `ScriptRuntimeTest.DecalResponseAndNormalStrengthSurviveSnapshot`
  覆盖 World -> Snapshot 的数据传递。
- 运行时：`SC_particle_ue_explosion` 的 `ExplosionScorch` 应在
  `deferredLighting` 前把地面基色压暗；日志不应出现 `VUID-` 或
  `Validation Error`。
