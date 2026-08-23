# Cloth Shading Model

## 文档状态

- 状态：已实现首阶段 Opaque Forward + Opaque Deferred
- 更新日期：2026-08-23
- 实现范围：各向同性 Charlie NDF、Neubelt visibility、Compute-only directional-albedo LUT、Forward/Deferred 共用 evaluator、Default Lit base energy compensation 和明确标记的 diffuse IBL sheen fallback。
- 相关计划：`documents/plan/rendering/cloth-shading-model-development-plan.md`
- Shading Model：`Cloth` / ID `8`
- 运行时资源：World-local `clothResources`，由候选 World 构建并随 GPU epoch 退休。

本文是当前代码的稳定合同。未列入“当前实现”的各向异性 Charlie、纤维级透射、BSSRDF、薄布背光、真实纱线级几何和 Charlie 专用环境预滤波仍属于后续扩展，不能被材质或调试输出伪装成已实现能力。

## 1. 当前实现闭环

```text
M_cloth / MI_cloth
    -> ClothMaterialInputs
    -> MaterialSurface / GBufferD
    -> Compute 生成 E_s directional-albedo LUT
    -> Charlie + Neubelt Cloth evaluator
    -> Forward / Deferred direct lighting
    -> T_b = 1 - c_s * E_s
    -> base + sheen + emissive
```

Forward 和 Deferred 都调用 `shader/glsl/engine/clothLighting.glsl` 中的同一组公式。Cloth 不会落入 Default Lit 的 `default` dispatch 分支。

## 2. ClothMaterialInputs

`shader/glsl/engine/materialInputs.glsl` 中的模型专用字段为：

```glsl
struct ClothMaterialInputs
{
    vec3 sheenColor;
    float sheenRoughness;
};
```

- `sheenColor` 是线性 RGB，表示已经折叠 tint/weight 后的 `c_s`；运行时 evaluator 只乘这一次。
- `sheenRoughness` 是 perceptual roughness，首阶段合法域为 `[0.02, 1.0]`。
- `baseColor`、`roughness`、AO 和 emissive 继续使用 Default Lit 的输入语义。
- `metallic` 必须为 `0`；Cloth 首阶段只支持非金属 base。
- `sheenColor` 的 alpha 只作为 authoring 保留通道，运行时不参与能量计算。

Material Function 只生成这些输入，不采样灯光、Shadow Map 或 Cloth LUT。

## 3. 版本冻结

| 字段 | 当前值 | 作用 |
| --- | ---: | --- |
| `clothModelVersion` | `1` | Cloth closure 和参数语义 |
| `sheenRoughnessMappingVersion` | `1` | `alpha_s = r_s * r_s` |
| `charlieDistributionVersion` | `1` | 各向同性 Charlie projected-area 分布 |
| `neubeltVisibilityVersion` | `1` | `1 / (4 * (NoL + NoV - NoL * NoV))` |
| `directionalAlbedoLutVersion` | `1` | `E_s(NoV, alpha_s)` |
| `sheenIblVersion` | `0` | 当前仍使用明确标记的 diffuse irradiance fallback |

版本字段与 `shader/glsl/generator/clothLookupTables.comp` 的 source digest 一起决定 World-local resource set 身份。公式、采样域、通道或预滤波语义变化时必须提升对应版本。

## 4. BRDF 与能量账本

首阶段 closure 为：

```text
f_cloth = c_s * f_sheen_unit + T_b * f_base
f_sheen_unit = D_Charlie(h, alpha_s) * V_N(NoL, NoV)
V_N = 1 / (4 * (NoL + NoV - NoL * NoV))
T_b = 1 - c_s * E_s(NoV, alpha_s)
```

- Charlie 只使用各向同性法线；不读取 mesh tangent。
- base lobe 复用当前 Default Lit diffuse/specular 约定，并同时乘 `T_b`。
- `E_s` 是单位白色 sheen 的 directional albedo，不是固定 sheen weight。
- `T_b` 同时作用于 direct 和 IBL base response。
- AO 只压暗 indirect lighting，不进入 LUT，也不替代 per-light shadow。
- emissive 独立加法，不参加 base/sheen 能量分配。
- 当前 renderer 的 shadow 输入是 directional CSM；Cloth evaluator 保留 direct lobe 分量，外层按现有 pass shadow 作用域应用。

## 5. Directional-albedo LUT

生产源唯一为 Compute Shader：`shader/glsl/generator/clothLookupTables.comp`。

| 属性 | 合同 |
| --- | --- |
| 语义 | 单位白色 sheen 的 `E_s(NoV, alpha_s)` |
| 维度 | 2D |
| 尺寸 | `256 x 256` |
| 格式 | `R16G16B16A16_SFLOAT`，只消费 `.r` |
| 坐标 | `x = NoV`，`y = alpha_s` |
| 采样 | linear、clamp-to-edge |
| 生成 | Charlie + Neubelt 半球积分 |
| 版本 | `directionalAlbedoLutVersion = 1` |

CPU 只解析版本、创建 Vulkan image/buffer/descriptor/pipeline、提交 dispatch、插入 barrier、计算 digest 和管理候选所有权。CPU 不计算 LUT texel，不写生产 LUT fallback。

## 6. GBuffer V1

Cloth 使用现有 attachment，不改变其它模型的字段所有权：

```text
GBufferA.rgb = baseColor
GBufferB.a   = packed shadingModelId / flags，Cloth ID = 8
GBufferC.r   = metallic，必须为 0
GBufferC.b   = base roughness
GBufferC.a   = ambient occlusion
GBufferD.rgb = sheenColor，线性 RGB，已折叠 weight
GBufferD.a   = sheenRoughness
GBufferF     = 不消费 tangent / anisotropy
```

`GBufferD` 只有在 `SHADING_MODEL_CLOTH` 分支中按上述语义解码；Clear Coat、Hair、Subsurface、Eye 等模型保持各自的显式 codec 分支。

## 7. IBL 状态

当前首阶段使用 `sheenIblVersion = 0` 的 MVP fallback：sheen 间接响应读取现有 diffuse irradiance，并标记 `clothIblFallback = 1`；base diffuse/specular IBL 使用现有路径，但 base IBL 同样乘 `T_b`。

该 fallback 不读取 GGX split-sum LUT 作为 sheen BRDF，也不宣称已经完成 Charlie 专用环境预滤波。后续 `sheenIblVersion = 1` 必须由 Compute 生成专用 Charlie/Neubelt prefilter 和对应资源 digest。

## 8. 资源生命周期与热重载

- `ClothResourceLoader` 在 pass/material 加载前构建候选 World-local resource set。
- digest 包含 Cloth 版本字段和 generator shader 内容；命中时复用上一 generation 的不可变 LUT。
- 生成失败拒绝整个 World candidate，不发布半成品资源包。
- Compute reload 使用 candidate-only pipeline；ABI 不兼容拒绝候选，旧 LUT、descriptor 和 pipeline 按 GPU epoch 退休。
- active World、active Material 和 active resource cache 不在 worker 或 prepare 阶段原地修改。

## 9. Debug 与验证

当前可直接定位的 Cloth 数据包括：Shading Model ID、sheen color、sheen roughness、Charlie D、Neubelt visibility、directional albedo、base energy scale、direct sheen、indirect sheen 和 IBL fallback 状态。Forward/Deferred 都把这些字段写入同一 `MaterialDebugLightingData` snapshot，基础 debug 入口仍使用现有 `MaterialDebugView`。

Debug View 模式 `64-73` 依次显示：Cloth Shading Model、Sheen Color、Sheen Roughness、Charlie D、Neubelt Visibility、Directional Albedo、Base Energy Scale、Direct Sheen、Indirect Sheen 和 IBL Fallback。Charlie D/Neubelt Visibility 的 debug 数值使用当前视线方向作为代表半角/入射方向，只用于定位公式与资源，不改变生产光照结果。

必要验证：

- shader source、CPU digest 和 LUT 版本一致；
- `sheenColor = 0` 时 Cloth 回到非金属 Default Lit base；
- Forward/Deferred 使用同一 evaluator；
- GBufferD encode/decode 保持 Cloth 专用语义；
- 生成器由 Compute 唯一写入 LUT；
- Compute 失败不发布 candidate；
- 现有 Default Lit、Hair、Eye、Subsurface 路径不改变。

## 10. 后续扩展边界

以下内容不属于当前合同：anisotropic Charlie、稳定 tangent/bitangent 依赖、纤维级透射、薄布背光、BSSRDF、真实纱线级几何、Charlie 专用环境 prefilter 和基于织物拓扑的覆盖率阴影。新增这些能力必须增加版本字段、输入所有权、GBuffer 语义和独立验证，不得复用当前 `GBufferD` 的含糊字段。

## 11. 关键文件

- `source/render/cloth/clothAssets.*`
- `source/render/cloth/clothResourceLoader.*`
- `source/render/cloth/clothResourceSet.*`
- `source/render/cloth/clothComputeReloadParticipant.*`
- `source/pipeline/clothLookupTableGenerator.*`
- `shader/glsl/generator/clothLookupTables.comp`
- `shader/glsl/common/clothBrdf.glsl`
- `shader/glsl/engine/clothLighting.glsl`
- `shader/glsl/M_cloth.*`
- `shader/glsl/engine/materialInputs.glsl`
- `shader/glsl/engine/gbufferCodec.glsl`
- `shader/glsl/engine/forwardLighting.glsl`
- `shader/glsl/engine/deferredLighting.glsl`
