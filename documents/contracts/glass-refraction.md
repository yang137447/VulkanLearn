# Glass Screen-Space Refraction

> 状态：**当前契约**。本文件描述 VulkanLearn 当前落地的屏幕空间玻璃折射，
> 不包含 Material Graph、体积厚度或透明-透明排序承诺。

## Pass 合同

渲染图在 `sky` 之后、`forwardTransparent` 之前插入 `sceneColorCopyPass`：

```text
geometry -> decals -> deferredLighting -> forwardOpaque -> sky
  -> sceneColorCopyPass -> forwardTransparent -> particle
```

- `sceneColorCopyPass` 是 `RenderGraphPassType::PostProcess`，材质为
  `Common/Materials/Pass/MI_sceneColorCopy.json`，该 MI 指向
  `shader/glsl/pass/M_postProcess.json`。
- 输入是 `sceneColor` binding 0；输出是 `sceneColorCopy`，`loadOp=clear`、
  `storeOp=store`。
- `sceneColorCopy` 是 `R16G16B16A16_SFLOAT`、1x 分辨率，
  usage 必须同时包含 `colorAttachment` 和 `sampled`。
- `forwardTransparent` 输出 `sceneColor`，因此不能在同一 pass 再采样
  `sceneColor`。它读取 `sceneColorCopy` binding 8 和 `gbufferA` binding 9。
  `RenderGraphCompiler` 的 `PassFeedbackLoop` 检查继续拒绝同一资源既作
  input 又作 attachment。

## 材质

`shader/glsl/M_glassRefraction.json` 是 `Unlit` +
`TransparentAlphaBlend` 材质，`cullMode=Back`。入口是
`M_glassRefraction.vertex.glsl` 和 `M_glassRefraction.surface.glsl`。

参数边界：

| 参数 | 类型 | 默认 | 范围 | 语义 |
| --- | --- | --- | --- | --- |
| `u_glassTint` | color | `(0.62, 0.82, 0.9, 1)` | 每通道 0..1 | 折射颜色乘数 |
| `u_opacity` | float | `0.9` | 0..1 | 玻璃表面覆盖率；折射样本已含背景 |
| `u_refractionStrength` | float | `1.0` | 0..4 | 屏幕空间偏移倍率 |
| `u_ior` | float | `1.45` | 1..2.5 | 折射率 |
| `u_roughness` | float | `0.08` | 0..1 | 额外 4 tap 屏幕空间柔化 |
| `u_thickness` | float | `18` | 0..500 cm | 折射出射点的近似玻璃厚度 |
| `u_backgroundNormalStrength` | float | `0.35` | 0..1 | G-buffer 背景法线扰动强度 |

Surface 求值：

1. `screenUv = gl_FragCoord.xy / textureSize(sceneColorCopy, 0)`。
2. 由世界法线 `N` 与视线 `V` 计算 `incident = -V`，并用
   `refract(incident, N, 1/u_ior)` 得到折射方向；全反射时回退到 `N`。
3. 出射点 `exit = worldPosition + refracted * u_thickness * clamp(dot(N,V), 0.15, 1)`，
   再经 `uboVP.viewProjection` 投回屏幕，得到
   `rayOffset = (exitUv - screenUv) * u_refractionStrength`。
4. 在 `screenUv + rayOffset` 采样 `sceneColorCopy`；可选读取
   `gbufferA.rgb` 解码背景世界法线，把视空间 XY 作为小偏移加入。
5. 用 Schlick Fresnel 抬高边缘覆盖率，并输出
   `emissiveColor = refractedColor * tint + fresnel * edgeColor`。

`u_opacity` 接近 1 是正确用法：屏幕空间折射样本本身已包含背景，
低 alpha 再和原 `sceneColor` 混合会产生双影。

## 资源

- `Common/Materials/Pass/MI_sceneColorCopy.json`：拷贝 pass 的 MI。
- `Common/Materials/MI_glass_refraction.json`：通用玻璃 MI。
- `Maps/SC_particle_ue_impact_variants/Materials/MI_ue_gallery_wall_glass.json`：
  Impact Glass gallery 的玻璃靶墙，使用 `M_glassRefraction`。

## 边界

- 折射是屏幕空间偏移，不重建真实双面玻璃厚度；`u_thickness` 是艺术参数，
  不等于网格物理厚度。
- 不实现 Material Graph、色散、逐像素厚度、透明表面之间的折射排序或
  平面反射。
- `sceneColorCopy` 包含 copy pass 之前的 deferred/forward opaque、sky 与
  decal 结果；`forwardTransparent` 之后的粒子、distortion、fogComposite 和
  UI 不在折射样本内。
- 透明材质仍不读取 DBuffer；decal 通过 `deferredLighting` 已写入
  `sceneColor`，所以玻璃背景会显示这些 decal，但玻璃表面不会额外叠加
  逐像素 DBuffer。
- 玻璃写入 `selectionMask` 与 `sceneColor`，不写 `sceneDepth`，仍参与对象级
  后向前排序。
