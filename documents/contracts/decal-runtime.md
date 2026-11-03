# Decal Runtime

> 状态：**当前契约**。本文件描述 VulkanLearn 已落地的 `BeforeLighting`
> GBuffer decal 子集，不是 UE Deferred Decal 的完整替代。

## Pass 顺序

```text
geometry
  -> decals（写 GBufferC）
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
  "sortOrder": 0
}
```

- `size` 是完整局部尺寸（cm），X 是投影深度，Y/Z 是投影平面宽高；内部
  以 `size * 0.5` 烘进 `decalToWorld`。
- `position`/`rotation` 是场景作者 TRS；`scale` 可选，非零，参与半尺寸。
- `opacity`、`edgeSoftness`、`noiseStrength` 都在 `[0, 1]`；`sortOrder`
  是 `[0, 255]` 整数。所有数值必须有限。
- `type=decal` 的对象不创建网格或材质资源；SceneAssetValidator 负责
  字段边界，WorldBuilder 负责转换成 `DecalSnapshot`。

## 投影与输出

- `DecalRenderer` 用一个单位 cube 实例化绘制所有可见 decal，最多 64 个；
  实例按 `sortOrder` 稳定排序。
- 顶点只做 `decalToWorld -> viewProjection`；像素用 `sceneDepth` 重建
  世界位置，再乘 `worldToDecal`。
- 局部 cube 落在 `[-1,1]` 外则丢弃。局部 X 是投影深度，UV 使用局部
  `zy`，与 UE `DeferredDecal.usf` 的 `SwizzlePos.zyx` 语义一致。
- 当前只写 `GBufferC.rgb`（baseColor）。`DecalColor` 混合写掩码保留
  `GBufferC.a`（AO），不读取或写 GBufferC 本身。
- 这是 `BeforeLighting` 子集：decal 会在 `deferredLighting` 之前改变
  opaque GBuffer 表面颜色，因此仍接受场景光照。

## 当前限制

- 没有 DBufferA/B/C、DBufferMask、法线、roughness、metallic、AO 修改。
- 没有 decal 纹理/材质图；当前使用程序化径向 + value-noise 形状。
- 没有硬件 depth/stencil 附件；像素以 `sceneDepth` 重建投影体判断，
  大屏或相机进入 decal 体积的鲁棒性仍需后续 stencil 路径。
- 只影响 deferred opaque GBuffer；`ForwardOpaque`、透明表面和粒子不会
  自动接收 decal。
- 没有逐 decal 资源创建/退休；实例数据走每 swapchain image 的 SSBO。

## 验证

- 数据/边界：`tool/script-tests/scriptRuntimeTests.cpp` 的
  `ScriptScene.DecalComponentValidatesProjectionAndRanges`。
- 运行时：`SC_particle_ue_explosion` 的 `ExplosionScorch` 应在
  `deferredLighting` 前把地面基色压暗；日志不应出现 `VUID-` 或
  `Validation Error`。
