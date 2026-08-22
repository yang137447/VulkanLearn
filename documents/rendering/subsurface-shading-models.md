# Subsurface 三类 Shading Model 实现合同

## 状态

- 类型：当前渲染实现合同
- 状态：已实现
- 完成日期：2026-08-22
- 覆盖范围：Opaque Deferred 的 `Subsurface` / 2、`PreintegratedSkin` / 3、`SubsurfaceProfile` / 5
- 历史执行记录：`documents/plan/rendering/subsurface-shading-models-development-plan.md`

本文定义 VulkanLearn 当前三类 SSS Shading Model 的资产、GPU 预计算、GBuffer、
lighting、Render Graph、资源生命周期和验证合同。后续改动必须维持本文的明确边界，
不能把三类模型重新合并成一个无语义的 `subsurfaceStrength` 分支。

## 1. 实现边界

| Shading Model | ID | 当前闭合 | 邻域读取 | Lookup 资源 |
| --- | ---: | --- | --- | --- |
| `Subsurface` | 2 | 当前像素的 wrap + backscatter + thickness transmission | 否 | 无 |
| `PreintegratedSkin` | 3 | 当前像素的二维 `(N·L, thickness)` LUT closure | 否 | Compute 生成的 skin LUT atlas |
| `SubsurfaceProfile` | 5 | lighting 后的全分辨率 separable profile filter | 是 | Compute 生成的 profile table |

当前只支持 Opaque Deferred。以下方向不属于当前合同：

- Transparent Forward profile convolution；
- checkerboard lighting packing；
- 半分辨率 SSS 与上采样；
- Burley + TAA kernel；
- ray-traced diffusion；
- object ID rejection；
- 三维 `(N·L, N·V, thickness)` skin LUT；
- per-pixel profile 资产切换。

## 2. CPU 与 GPU 职责

### 2.1 强制边界

SSS lookup 数据必须由 Compute Shader 生成。CPU 只允许：

- 枚举并解析 `<resourcePath>/subsurfaceProfiles/*.json`；
- 枚举并解析 `<resourcePath>/skinLuts/*.json`；
- 校验 schema、版本、ID、单位、范围和重复项；
- 将作者参数序列化到 generation parameter buffer；
- 创建 GPU image、buffer、descriptor、pipeline 和同步命令；
- 计算源资产与 generator shader 的稳定内容摘要；
- 管理 World-local 资源包与事务提交。

CPU 不得保留以下逻辑：

- profile radial function 求值；
- profile tap 权重生成或归一化；
- preintegrated skin response 求值；
- skin IBL average 求值；
- CPU `HostImage` lookup 图构建；
- CPU lookup 图上传作为运行时 fallback。

`shader/glsl/generator/subsurfaceLookupTables.comp` 是两张 lookup texture 数值生成的
唯一运行时真相源。若 Compute 生成失败，World candidate 构建失败；不得回退到 CPU。

### 2.2 代码注释约束

本功能新增或修改的 C++/GLSL 代码，对以下非显然边界必须提供必要的中文注释：

- CPU 参数打包与 Compute SSBO/GLSL layout 的对应关系；
- stable ID、neutral fallback 和 GBuffer channel 的语义；
- Vulkan image layout/barrier、descriptor 和 GPU 资源所有权；
- World candidate 复用、事务提交和 GPU epoch 退休；
- profile filter 的 rejection、有效权重归一化和能量分配；
- 有意采用的性能取舍与当前不支持的路径。

注释解释设计意图和生命周期，不逐行翻译代码，也不把已经废弃的 CPU 预计算描述成可用路径。

### 2.3 生成流程

```text
World candidate prepare
  -> parse and validate SSS JSON assets on CPU
  -> build sourceDigest(asset bytes + generator shader bytes)
  -> reuse previous World-local package when digest matches
  -> serialize authored parameters into SSBO
  -> dispatch generator/subsurfaceLookupTables.comp
  -> transition both images to shader-read layout
  -> bind immutable lookup textures to candidate materials/passes
  -> commit through the existing World/Graph transaction
```

资源生成发生在 pass material 和 mesh material 加载之前，因此所有 SSS 材质都只消费
同一 World generation 的 lookup 资源。Active World 不在 prepare 阶段被修改。

## 3. Lookup 资源合同

### 3.1 Subsurface Profile Table

| 属性 | 合同 |
| --- | --- |
| 格式 | `R16G16B16A16_SFLOAT` |
| 尺寸 | `14 x 256` |
| 采样 | nearest、clamp-to-edge |
| Profile ID | `1..255`；`0` 保留为 neutral fallback |
| Kernel | 13 taps，版本 1 |

每一行对应一个 `profileId`：

- `x = 0`：`rgb = meanFreePathColor`，`a = maximumRadiusWorld`；
- `x = 1..13`：`rgb = 已归一化的 RGB tap weight`，`a = signed tap position`。

Compute Shader 使用作者提供的 MFP 颜色、距离和单位比例生成 separable profile。每个 RGB
通道独立归一化；inactive 行保持 neutral center response。

### 3.2 Preintegrated Skin LUT Atlas

| 属性 | 合同 |
| --- | --- |
| 格式 | `R16G16B16A16_SFLOAT` |
| 尺寸 | `128 x 1024` |
| Tile | 16 个 `128 x 64` tile |
| 采样 | linear、clamp-to-edge |
| Skin LUT ID | `1..15`；`0` 保留为 neutral fallback |
| LUT 版本 | 1 |

每个 tile 的第 0 行保存 metadata：

- texel `(0, 0)`：IBL average response 与 `outputMode`；
- texel `(1, 0)`：transmission color 与最大世界厚度；
- 其余行：`N·L` 与 normalized thickness 的 RGB response。

V1 支持两种 `outputMode`：

- `scatteringMultiplier`：LUT 输出无 Lambert 项的 scattering multiplier；
- `finalDiffuseResponse`：LUT 输出已经包含局部 wrapped diffuse 形状的最终 diffuse response。

## 4. 资产合同

### 4.1 Profile Asset

目录：`<resourcePath>/subsurfaceProfiles/`

```json
{
    "name": "Skin Profile",
    "type": "subsurfaceProfile",
    "schemaVersion": 1,
    "profileId": 1,
    "meanFreePathColor": [1.0, 0.42, 0.28],
    "meanFreePathDistance": 1.2,
    "distanceUnit": "millimeter",
    "worldUnitScale": 0.001,
    "kernelVersion": 1
}
```

约束：

- `profileId` 必须在 `1..255`；
- `schemaVersion == 1`；
- `kernelVersion == 1`；
- RGB MFP color 必须有限且大于零；
- distance 必须有限且大于零；
- `distanceUnit` 支持 `millimeter`、`centimeter`、`meter`；
- `worldUnitScale` 必须与单位合同一致；
- 规范化 asset path 和 ID 都必须唯一。

### 4.2 Skin LUT Asset

目录：`<resourcePath>/skinLuts/`

```json
{
    "name": "Preintegrated Skin Final Diffuse",
    "type": "preintegratedSkinLut",
    "schemaVersion": 1,
    "skinLutId": 1,
    "lutVersion": 1,
    "width": 128,
    "height": 64,
    "thicknessMax": 8.0,
    "thicknessUnit": "millimeter",
    "worldUnitScale": 0.001,
    "outputMode": "finalDiffuseResponse",
    "scatterColor": [1.0, 0.48, 0.32],
    "transmissionColor": [1.0, 0.18, 0.08],
    "sourceIdentity": "vulkanlearn.preintegrated-skin.compute.v1"
}
```

约束：

- `skinLutId` 必须在 `1..15`；
- `schemaVersion == 1`；
- `lutVersion == 1`；
- `width == 128`、`height == 64`；
- thickness、颜色和单位必须通过加载期校验；
- `outputMode` 只能是 `scatteringMultiplier` 或 `finalDiffuseResponse`；
- 规范化 asset path 和 ID 都必须唯一。

### 4.3 Material Instance 资产引用

`PreintegratedSkin` MI 使用：

```json
"skinLut": "skinLuts/PSL_skin.json"
```

`SubsurfaceProfile` MI 使用：

```json
"subsurfaceProfile": "subsurfaceProfiles/SSP_skin.json"
```

Loader 将资产路径解析为稳定 ID，并覆盖引擎派生参数：

- `u_skinLutId`；
- `u_subsurfaceProfileId`。

热重载或候选状态迁移后必须重新应用派生 ID，不能让旧 MI snapshot 覆盖新解析结果。
Lookup textures 由引擎绑定，MI 不直接作者化 `subsurfaceProfileTable` 或
`preintegratedSkinLutTable`。

## 5. Material Inputs 与 GBuffer

### 5.1 Material Model Inputs

`Subsurface`：

```text
color, weight, wrapWidth, backscatterPower,
backscatterWeight, thickness, transmissionWeight
```

`PreintegratedSkin`：

```text
skinLutId, thickness, thicknessScale,
weight, curvature, transmissionWeight
```

`SubsurfaceProfile`：

```text
profileId, weight, thickness, transmissionWeight
```

V1 要求 `PreintegratedSkin.curvature == 0`。材质范围、厚度域和保留字段全部在加载期
校验；shader 不负责按帧修补错误资产。

### 5.2 GBuffer Packing

| ID | GBufferA.a | GBufferD | GBufferF |
| --- | --- | --- | --- |
| 2 | `transmissionWeight` | `color.rgb, weight` | `wrapWidth, backscatterPower, backscatterWeight, thickness` |
| 3 | 普通 opacity | `skinLutId, thickness, thicknessScale, weight` | `curvature, transmissionWeight, 0, 0` |
| 5 | 普通 opacity | `profileId, weight, thickness, transmissionWeight` | 普通 tangent / anisotropy |

ID 2 当前只允许 Opaque，因此借用 `GBufferA.a` 保存 transmission weight，并在 decode 后
恢复 opacity 为 1。CPU codec 只用于 encode/decode 合同测试，不生成任何 lookup 数据。

## 6. Lighting 与能量合同

Deferred lighting 保持以下分量：

```text
diffuseLighting
nonDiffuseLighting = emissive + directSpecular + indirectSpecular * AO
transmissionLighting
finalColor = diffuseLighting + nonDiffuseLighting + transmissionLighting
```

DefaultLit 的 direct diffuse、direct specular、diffuse IBL、specular IBL、AO 与阴影公式
保持不变。拆分只改变中间数据组织，DefaultLit 重组结果必须与拆分前一致。

三类 SSS 都使用显式能量预留：

```text
transmissionWeight = subsurfaceWeight * authoredTransmissionWeight
reflectedFraction = 1 - transmissionWeight
```

反射 diffuse 乘 `reflectedFraction`，transmission 单独输出，禁止让 profile 和
transmission 同时消费完整 diffuse 能量。

### 6.1 ID 2：Local Subsurface

- direct lighting 使用 wrap diffuse 与 backscatter；
- indirect diffuse 使用同一局部 closure 的 IBL 近似；
- thickness transmission 单独写入 `transmissionLighting`；
- 不读取 profile table 或邻域像素。

### 6.2 ID 3：Preintegrated Skin

- 每个 direct light 以 `(N·L, normalizedThickness)` 查询 Compute 生成 LUT；
- `outputMode` 决定运行时是否补乘 Lambert 项；
- IBL 使用 LUT metadata 中的 average response；
- transmission 使用 LUT metadata 中的 transmission color 与厚度域；
- 不读取邻域像素。

### 6.3 ID 5：Subsurface Profile

- deferred lighting 先输出未过滤的 diffuse；
- specular、emissive 与 transmission 保持在 filter 外；
- profile filter 只处理 ID 5 像素的 diffuse；
- composition 使用 `profileWeight` 混合原始与过滤后的 diffuse。

## 7. Render Graph 合同

### 7.1 资源

新增全分辨率 `R16G16B16A16_SFLOAT` 资源：

- `diffuseLighting`；
- `nonDiffuseLighting`；
- `transmissionLighting`；
- `sssPing`；
- `sssPong`。

### 7.2 Pass 顺序

```text
deferredLighting
  -> sssHorizontal
  -> sssVertical
  -> sssComposition
  -> bloom / tone mapping / post process
```

`deferredLighting` 不再直接写最终 scene color，而是输出三路 lighting lobe。
`sssComposition` 是重新合成 scene color 的唯一 SSS composition 边界。

### 7.3 Bilateral Filter

水平和垂直 pass 使用同一 profile kernel，并拒绝：

- 屏幕范围之外的采样；
- depth discontinuity；
- normal discontinuity；
- 非 `SubsurfaceProfile` shading model；
- 不同 profile ID。

每个 RGB 通道按实际接受的有效权重重新归一化。当前未加入 object ID rejection；
该限制意味着同 depth、同 normal、同 profile 的相邻不同物体仍可能互相泄漏。

GBuffer 的 shading model、profile ID、normal 和 scene depth 在 rejection 与 composition
路径都按 texel 语义读取；不能依赖普通资源的 linear sampler 对离散编码做插值。lighting source
也按接受的邻域 texel 读取，避免过滤边界再次引入跨表面混合。

世界半径通过 view-space depth、投影矩阵和 viewport 高度转换为像素半径，避免固定像素
kernel 随相机距离或分辨率改变材质外观。

## 8. 资源生命周期

`SubsurfaceResourceSet` 属于一个 World-local resource package，持有：

- 已验证 profile/LUT 资产；
- path -> ID 与 ID -> asset 索引；
- `sourceDigest`；
- 两张 immutable GPU lookup texture。

候选 World 构建时：

1. 先准备 SSS resource set；
2. 再构建 pass/mesh material；
3. 所有材质引用候选 set 的 texture 与派生 ID；
4. 通过现有 World/Graph transaction 一次性发布；
5. 旧资源随 World-local package 按 GPU epoch 退休。

生成器使用 candidate-only Compute pipeline，不写入 active PipelineFactory cache。image、
buffer、descriptor pool、sampler 和 single-time command buffer 在 candidate 失败路径上
必须由局部所有权守卫清理；只有 Texture 成功接管 lookup image 后，临时所有权才释放。

相同 `sourceDigest` 可复用上一 generation 的 resource set。摘要包含资产内容和 Compute
shader 内容，因此修改 JSON 或 generator shader 都会触发重建。

## 9. Debug Views

| Mode | 名称 | 用途 |
| ---: | --- | --- |
| 12 | Shading Model | 检查 ID 2 / 3 / 5 分流 |
| 13 | Subsurface Weight | 检查模型混合权重 |
| 14 | Transmission Weight | 检查最终能量预留权重 |
| 15 | SSS Asset ID | 检查 profile/LUT 稳定 ID |
| 16 | Local SSS Response | 检查 ID 2/3 当前像素 response |
| 17 | Diffuse Before SSS | 检查 filter 输入 |
| 18 | Diffuse After SSS | 检查 profile filter 输出 |
| 19 | SSS Pixel Radius | 检查世界半径到像素半径 |
| 20 | SSS Valid Weight | 检查 bilateral 接受与归一化 |

Mode 16 只定义 ID 2/3 的当前像素 local response；ID 5 的空间 response 尚未经过 profile filter，必须输出黑色。
Mode 19 和 20 对非 ID 5 像素必须输出黑色。

## 10. 验证合同

### 10.1 自动测试

`subsurface_contract` 覆盖：

- profile schema/version 拒绝；
- skin LUT schema、固定分辨率和 output mode；
- mm/cm/m 单位等价；
- stable ID 边界；
- 三类 GBuffer encode/decode round trip；
- DefaultLit component 重组容差；
- CPU profile/LUT 生成符号不存在；
- Compute generator 与归一化合同标记存在；
- composition 对离散 GBuffer 使用 texel sampling；
- Debug View 16 不把 ID 5 的未过滤 diffuse 当作 local response。

2026-08-22 串行结果：

```text
ctest --test-dir build -j 1 --output-on-failure
100% tests passed, 5/5
```

### 10.2 Runtime Smoke

已通过：

```text
build/bin/main.exe --framesmoke 2 --exit-after-tests
build/bin/main.exe --initial-scene scenes/SC_subsurface_models.json --framesmoke 3 --exit-after-tests
```

验证范围包括 shader cache、Compute lookup 生成、两类资产加载、三种材质、Render Graph、
World transaction 和实际帧提交。

### 10.3 Visual Check

测试场景：`<resourcePath>/scenes/SC_subsurface_models.json`

已检查：

- 三种模型均可见并保持独立外观；
- Full 模式下 specular highlight 保持锐利；
- Mode 18 只显示过滤后的 diffuse；
- Mode 19 只有 ID 5 显示非零 pixel radius；
- Mode 20 只有 ID 5 显示有效 kernel weight。

本次没有进行 path-traced BSSRDF reference 对比，因此不得宣称物理误差已经量化。

### 10.4 本地性能基线

2026-08-22、当前本机、120 帧 `framesmoke`：

| Scene | avgFrameMs | minFrameMs | maxFrameMs | avgRenderLoopMs |
| --- | ---: | ---: | ---: | ---: |
| `SC_subsurface_models.json` | 2.431223 | 1.427300 | 39.469500 | 1.490060 |
| `SC_car_showcase.json` | 3.469403 | 2.426000 | 37.252400 | 2.569046 |

这些数字是场景级 smoke baseline，不是严格的 SSS pass GPU timing，也不能直接用两个复杂度
不同的场景计算 SSS 开销。后续优化应增加 GPU timestamp pass breakdown 后再做成本结论。

## 11. 关键文件

- `source/render/subsurface/subsurfaceAssets.*`
- `source/render/subsurface/subsurfaceMaterialContract.*`
- `source/render/subsurface/subsurfaceResourceLoader.*`
- `source/render/subsurface/subsurfaceResourceSet.*`
- `source/pipeline/subsurfaceLookupTableGenerator.*`
- `shader/glsl/generator/subsurfaceLookupTables.comp`
- `shader/glsl/engine/subsurfaceLighting.glsl`
- `shader/glsl/engine/preintegratedSkinLighting.glsl`
- `shader/glsl/engine/subsurfaceProfileLighting.glsl`
- `shader/glsl/engine/subsurfaceProfileFilter.glsl`
- `shader/glsl/pass/sssHorizontal.*`
- `shader/glsl/pass/sssVertical.*`
- `shader/glsl/pass/sssComposition.*`
- `config/renderGraphConfig.json`
- `tool/subsurface-tests/subsurfaceTests.cpp`
