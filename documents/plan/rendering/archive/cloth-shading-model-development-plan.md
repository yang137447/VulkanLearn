# Cloth Shading Model 开发计划

## 状态

- 类型：Cloth v1 MVP 完成记录与 Cloth v2 各向异性迁移记录；Target realtime 扩展继续保留为后续路线
- 验收状态：v1 已完成（2026-08-23）；v2 输入/GBuffer/direct 已实现（2026-09-01，构建与测试证据见文末）
- Shading Model：`Cloth` / ID `8`
- 知识库基线：`D:\YYBWorkSpace\GitHub\yyb-knowledge-book\src\content\docs\rendering\materials\shading-models\cloth-sheen.mdx`
- VulkanLearn 当前合同基线：`documents/rendering/shader-structure-and-material-function.md`
- 当前合同：`documents/rendering/cloth-shading-model.md`（已完成迁移）
- 首阶段执行域：Opaque Forward + Opaque Deferred
- v1 首阶段模型：各向同性 Charlie NDF + Neubelt visibility
- v2 当前模型：非零 anisotropy 使用椭圆 Charlie + anisotropic visibility；`anisotropy = 0` 保留 v1 路径
- 预计算约束：所有运行时 Lookup、积分、环境预滤波和派生纹理必须由 Compute Shader 生成
- CPU 约束：CPU 只做解析、校验、参数打包、资源编排、digest 和事务生命周期管理
- v1 非目标：不实现各向异性 Charlie、纤维级透射、BSSRDF、薄布背光和真实纱线级几何

本文保留 Cloth 首阶段 MVP 的历史完成记录，并补充 Cloth v2 迁移结果。实现中的公式、资源格式和生命周期已经迁移到
`documents/rendering/cloth-shading-model.md`，后续代码以正式合同为准；Charlie 专用环境预滤波仍是
明确标注的 Target realtime 扩展，不能被当前 MVP fallback 宣称为已实现能力。

### 实际完成范围

- 完成 `Cloth` ID `8` 的 Opaque Forward + Opaque Deferred 闭环。
- 完成 Charlie NDF、Neubelt visibility、Compute-only `E_s` directional-albedo LUT、GBufferD 编解码和 `T_b` 能量补偿。
- 完成材质加载期约束、World-local Cloth resource package、Compute 热重载、GPU epoch 退休、Debug View 64–73、测试场景和最小 runtime smoke。
- 按“避免写不必要的测试”的要求，没有新增独立 `cloth_contract`/白炉测试目标；验证使用现有 build、启动期 shader 编译和 Cloth 场景 smoke，未引入 CPU production LUT 路径。
- 未实现各向异性 Charlie、纤维透射、BSSRDF、薄布背光、真实纱线几何和 Charlie 专用环境 prefilter；这些边界已写入正式合同。

### Cloth v2 迁移记录（2026-09-01）

- `ClothMaterialInputs` 新增 `anisotropy`（`[-1, 1]`）和 `anisotropyCross`（`[0, 1]`）；NeoX Silk 的 `u_anisotropy`、`ParamMap.B` mask 和 `u_anisotropy_cross` 通过 MF 标准化接入。
- 非零 anisotropy 使用 `aspect = 2^anisotropy` 的椭圆 Charlie closure、匹配的 warp-domain visibility 和 v2 directional-albedo array LUT；零值严格走 v1 isotropic closure 与 v1 LUT。
- Cloth v2 使用 GBufferF.xyz 保存 world tangent，GBufferF.w 保存 5+5 bit anisotropy/cross；`GBUFFER_HAS_ANISOTROPY_MASK` 缺失时 Deferred 显式回退到 v1 isotropic。
- Forward/Deferred 共用 `clothLighting.glsl` evaluator，并将 model version、tangent、roughness axes、visibility 等字段传播到 Debug View 80–89。
- C++ resource digest、双 LUT resource package、Compute reload 和 GPU epoch retirement 已同步升级；生产 LUT 仍只由 Compute Shader 生成。
- 已增加 `tool/cloth-tests` 的 CPU contract/reference 测试；它只验证版本、参数范围、GBuffer packed round-trip 和轴向对称性，不生成生产 LUT。
- 当前明确缺口：`sheenIblVersion = 0` 仍是 diffuse irradiance fallback，因此 Cloth v2 仅完成输入/GBuffer/direct 阶段，IBL 或其它能力仍为显式缺口；checkout 缺少 `M_neoxSilk`，未宣称 NeoX full fixture 完成。

## 1. 目标与固定边界

### 1.1 首阶段目标

完成以下闭环：

```text
Cloth MI
  -> MaterialInputs
  -> MaterialSurface / GBuffer
  -> Compute 生成 directional-albedo LUT
  -> Charlie + Neubelt Cloth evaluator
  -> Forward / Deferred direct lighting
  -> Base + Sheen energy compensation
  -> GBuffer、运行时 smoke 和热重载验证（白炉数值回归留待后续）
```

### 1.2 首阶段公式

Cloth 使用两个 lobe：

```text
f_cloth = c_s * f_sheen_unit
        + T_b * f_base
```

其中：

```text
f_sheen_unit = D_Charlie(h, alpha_s) * V_N(N_i, N_o)
V_N(N_i, N_o) = 1 / (4 * (N_i + N_o - N_i * N_o))
T_b = 1 - c_s * E_s(N_o, alpha_s)
```

首阶段固定：

- `c_s` 为线性 RGB，并且在 Material Function 阶段折叠 sheen tint 和 sheen weight；
- `sheenColor` 运行时只能乘一次；
- `alpha_s = Phi_s(sheenRoughness)`，映射版本必须显式记录；
- base 复用 Default Lit 的 diffuse/specular 约定；
- `metallic` 固定为非金属语义；
- base 和 sheen 共享当前 shading normal；
- `E_s` 是单位白色 sheen 的 directional albedo，不是固定 `sheenWeight`；
- `T_b` 同时用于 direct 和 IBL，不能只补偿 direct；
- AO 不进入 `E_s` LUT，也不替代 per-light shadow；
- emissive 独立叠加，不参加 base/sheen 能量分配。

### 1.3 明确禁止的捷径

- 只替换 GGX 的 `D`，继续复用 GGX visibility、BRDF LUT、prefilter 或采样 PDF；
- 用固定的 `1 - sheenWeight` 代替 `1 - c_s * E_s`；
- 把 `sheenColor` 以 sRGB 值直接参与 BRDF；
- 让 CPU 生成生产 LUT 或上传 CPU LUT 作为运行时 fallback；
- 生成失败时回退到近似常量并继续发布 World candidate；
- 首阶段偷偷读取不稳定 mesh tangent 以伪造各向异性；
- 把 `subsurfaceColor`、薄布透射或背面传输并入反射 sheen；
- 在每像素 shader 中用 `clamp`、`max` 等防御逻辑掩盖非法资产数据。

## 2. 阶段总览

| 阶段 | 内容 | 交付物 | 前置条件 |
| --- | --- | --- | --- |
| Phase 0 | 合同与版本冻结 | Cloth 正式合同、参数/GBuffer/LUT 版本 | 无 |
| Phase 1 | 合同边界与验证取舍 | 共享公式复核、加载期约束、最小 smoke 验证；不新增专用 test target | Phase 0 |
| Phase 2 | Compute-only Lookup 资源链路 | `E_s` LUT、World-local resource set、digest/事务 | Phase 0、Phase 1 |
| Phase 3 | MaterialInputs、MI 与 GBuffer | `M_cloth`、材质校验、Forward/Deferred 输入一致性 | Phase 0 |
| Phase 4 | Direct Cloth evaluator | Charlie/Neubelt direct、shadow/AO 边界 | Phase 2、Phase 3 |
| Phase 5 | MVP IBL | 明确标记的 diffuse irradiance fallback；Charlie 专用 prefilter 延后 | Phase 4 |
| Phase 6 | Reload、Debug、场景和轻量性能证据 | Compute reload、Debug View、runtime smoke、启动诊断 | Phase 2–5 |
| Phase 7 | 合同迁移与收口 | 正式 rendering contract、README 索引和完成记录 | Phase 6 |

## 3. Phase 0：合同与版本冻结

### 3.1 新增正式合同

新增：

- `documents/rendering/cloth-shading-model.md`

合同必须定义：

- `Cloth` ID `8` 的实现状态和支持范围；
- `ClothMaterialInputs` 字段及其所有权；
- `sheenColor`、`sheenRoughness`、`baseColor`、`roughness`、AO 的语义；
- 线性颜色、权重折叠和非金属限制；
- Charlie NDF、Neubelt visibility 和 `r_s -> alpha_s` 映射；
- `E_s` LUT 的维度、格式、采样方式和版本；
- direct、IBL、shadow、AO、emissive 的作用域；
- Forward/Deferred 共用 evaluator 的要求；
- Compute-only 生成、失败语义、digest 和 World-local 所有权；
- Debug View、自动测试、runtime smoke 和白炉验收标准；
- 各向异性、纤维透射、BSSRDF 等后续扩展边界。

### 3.2 冻结版本字段

建议至少冻结以下身份字段：

```text
clothModelVersion
sheenRoughnessMappingVersion
charlieDistributionVersion
neubeltVisibilityVersion
directionalAlbedoLutVersion
sheenIblVersion
generatorShaderSourceIdentity
```

任一公式、采样域、roughness 映射、LUT 通道或环境预滤波语义变化，都必须提升对应版本并使
source digest 失效。

### 3.3 GBuffer 首阶段合同

首阶段建议使用现有通道：

```text
shadingModelId = 8
GBufferD.rgb   = sheenColor              // 线性 RGB，已折叠 sheen weight
GBufferD.a     = sheenRoughness          // perceptual roughness
GBufferF       = 首阶段不消费 tangent / anisotropy
```

通用通道继续保存：

```text
GBufferA.rgb = baseColor
GBufferB     = worldNormal + shading model / flags
GBufferC     = base roughness / metallic / AO
```

在实现前必须确认 `GBufferD` 的 Cloth 解释不会破坏 Clear Coat、Hair、Subsurface 和 Eye 的现有
专用解码；必要时增加显式 shading-model 分支和 encoding version，而不是复用含糊字段。

### 3.4 Phase 0 验收

- [x] 正式合同已创建，并列出当前不支持路径；
- [x] 参数、颜色空间、权重折叠和 roughness 映射已冻结；
- [x] `GBufferD` 及相关 flag 的所有权已冻结；
- [x] LUT 格式、尺寸、采样和版本已冻结；
- [x] Compute-only 生产边界已写入合同；
- [x] 合同冻结后进入 shader 和 C++ 实现。

## 4. Phase 1：公式边界与验证取舍

### 4.1 CPU reference 的边界

CPU reference 只用于 test target：

- 验证 Charlie projected-area normalization；
- 验证 Neubelt visibility 的有效域；
- 验证 `omega_i` / `omega_o` 互易性；
- 计算测试期的 `E_s` 对照值；
- 生成白炉、参数 sweep 和 shader 对照数据。

CPU reference 不得：

- 写入 `<resourcePath>/generated/` 作为运行时资源；
- 生成运行时 LUT；
- 作为启动时资源加载 fallback；
- 与生产 Compute generator 共享会被运行时调用的数值生成入口。

### 4.2 测试取舍

本次按“避免写不必要的测试”不新增 `tool/cloth-tests/` 或 `cloth_contract` CMake/CTest 目标，也不引入 CPU reference。原因是生产数值路径已经由 Compute-only generator 独占，新增 test-only reference 会扩大维护面，但不会改变当前 runtime ownership 或 shader ABI。

保留的验证边界由现有实现和启动期检查覆盖：

- `clothBrdf.glsl` 中 Forward/Deferred 共用 Charlie、Neubelt 和 roughness mapping；
- 资产校验拒绝非法 metallic、颜色和 roughness；
- GBufferD 只在 Cloth 分支解码；
- shader build cache 会编译 generator 与 Cloth lighting 依赖；
- Cloth 场景 runtime smoke 验证 World/Graph、LUT dispatch、材质加载和渲染闭环。

### 4.3 Phase 1 验收

- [x] 未新增专用 test target，符合本次任务的测试取舍；
- [x] 未引入 CPU production LUT 或启动 fallback；
- [x] 关键公式身份、参数边界和 Forward/Deferred 共享入口已通过代码/合同复核；
- [x] 最小 runtime smoke 已完成；
- [x] 详细代数白炉测试保留为后续模型扩展的独立工作，不伪装为本次已执行证据。

## 5. Phase 2：Compute-only Lookup 资源链路

### 5.1 资源生成流程

运行时流程固定为：

```text
World candidate prepare
  -> parse and validate Cloth assets/config
  -> build sourceDigest(asset bytes + generator shader + version fields)
  -> reuse previous World-local package when digest matches
  -> serialize generator parameters to SSBO
  -> create candidate image / view / sampler / descriptor / pipeline
  -> dispatch shader/glsl/generator/clothLookupTables.comp
  -> barrier: General -> ShaderReadOnlyOptimal
  -> bind Cloth resource set to candidate materials/passes
  -> commit through World/Graph transaction
  -> retire old package by GPU epoch
```

### 5.2 第一张 Lookup：Directional Albedo

生产源文件：

- `shader/glsl/generator/clothLookupTables.comp`

建议首版合同：

| 属性 | 首版建议 |
| --- | --- |
| 语义 | `E_s(N_o, alpha_s)`，单位白色 sheen directional albedo |
| 维度 | 2D |
| 暂定尺寸 | `256 x 256` |
| 暂定格式 | `R16_SFLOAT`，若现有路径不支持则使用 `R16G16B16A16_SFLOAT` 的 `.r` 通道 |
| 坐标 | `x = N_o`，`y = alpha_s` |
| 采样 | linear、clamp-to-edge |
| 生成 | Charlie + Neubelt 半球积分 |
| 版本 | `directionalAlbedoLutVersion = 1` |

实现时必须确认 shader、CPU reference、材质 authoring 和 LUT 使用同一个
`sheenRoughness -> alpha_s` 映射。

### 5.3 C++ 资源模块

建议新增：

- `source/render/cloth/clothAssets.h/.cpp`
- `source/render/cloth/clothMaterialContract.h/.cpp`
- `source/render/cloth/clothResourceSet.h/.cpp`
- `source/render/cloth/clothResourceLoader.h/.cpp`
- `source/pipeline/clothLookupTableGenerator.h/.cpp`

可复用既有实现模式：

- `source/render/subsurface/subsurfaceResourceLoader.*`
- `source/render/subsurface/subsurfaceResourceSet.*`
- `source/pipeline/subsurfaceLookupTableGenerator.*`
- `source/render/eye/eyeLookupTableGenerator.*`

CPU 只允许：

- 解析和校验参数、版本、尺寸和范围；
- 创建 Vulkan image、buffer、descriptor、pipeline；
- 序列化 Compute SSBO；
- 调度 Compute、插入 barrier、管理资源所有权；
- 计算并比较 source digest；
- 管理 candidate package 和事务提交。

CPU 不允许：

- Charlie/Neubelt 数值积分；
- LUT texel 计算；
- `HostImage` 生产 LUT 构建；
- CPU LUT 上传 fallback；
- 生成失败后继续发布不完整的资源包。

### 5.4 资源所有权和失败语义

- Generator 使用 candidate-only Compute pipeline；
- 不写入 active `PipelineFactory` cache；
- image、view、sampler、descriptor、temporary command buffer 由局部所有权守卫管理；
- Compute 失败时整个 World candidate 失败；
- active World、active resource cache 和 active Material 不在 prepare 阶段修改；
- 成功 commit 后旧资源按 GPU epoch 退休；
- 相同 source digest 复用上一 World generation 的不可变资源。

### 5.5 Phase 2 验收

- [x] `clothLookupTables.comp` 是生产 LUT 唯一数值源；
- [x] 运行时没有 CPU LUT 生成或上传 fallback；
- [x] source digest 包含版本字段、generator shader 内容和 artifact generation key；
- [x] candidate-only 生成、barrier、shader-read 绑定完成；
- [x] World transaction 成功/失败路径不修改 active owner；
- [x] Compute 生成失败会拒绝 candidate；
- [x] 没有引入 GPU readback 作为生产输入。

## 6. Phase 3：MaterialInputs、M_ 和 GBuffer

### 6.1 Shader 输入结构

修改：

- `shader/glsl/engine/materialInputs.glsl`
- `shader/glsl/engine/materialSurface.glsl`
- `shader/glsl/engine/gbufferCodec.glsl`
- `shader/glsl/materialFunction/mf_pbrInputs.glsl`

新增 `ClothMaterialInputs`，首阶段至少包含：

```glsl
struct ClothMaterialInputs
{
    vec3 sheenColor;
    float sheenRoughness;
};
```

将其放入 `MaterialModelInputs`，保持模型专用字段具有明确所有权。

### 6.2 Cloth 母材质

新增：

- `shader/glsl/M_cloth.json`
- `shader/glsl/M_cloth.vertex.glsl`
- `shader/glsl/M_cloth.surface.glsl`

`M_cloth.surface.glsl` 只负责：

- 复用 `EvaluateMFPbrInputs()` 获取 base material；
- 将 `u_clothSheenColor` 转换为线性 sheen 参数；
- 将 `u_clothSheenRoughness` 写入 `ClothMaterialInputs`；
- 固定 Cloth 的非金属约束由资产/加载侧执行，不在 shader 中偷偷修正。

不允许在 M_ 或 MF 中直接计算 Charlie BRDF、读取 shadow map 或采样 Cloth LUT。

### 6.3 Material 参数和 Schema

修改：

- `schema/material_instance.schema.json`
- `source/material/materialAssetUtils.h`（仅在需要补充合同元数据时修改）
- `source/material/validation/materialAssetValidator.*`
- `source/materialInstanceValidator.*`
- `source/material/generator/materialParameterIncludeGenerator.*`

建议参数：

```text
u_clothSheenColor       vec4  // rgb = linear sheen color, a 保留或固定为 1
u_clothSheenRoughness   float // perceptual sheen roughness
```

校验规则：

- Cloth metallic 必须满足非金属约定；
- sheen roughness 必须位于合同域；
- sheen color 颜色空间和 weight 折叠方式必须明确；
- 不允许同一材质同时声明旧 customData 语义和 Cloth 专用语义；
- 参数名必须进入反射和 shader ABI 校验。

### 6.4 Phase 3 验收

- [x] `M_cloth` 可以独立编译并加载；
- [x] Cloth MaterialInputs 不读取灯光或 LUT；
- [x] GBuffer encode/decode 使用 Cloth 专用 `GBufferD` 语义；
- [x] Cloth 与 Clear Coat/Hair/Subsurface/Eye 的 GBuffer 解码互不串线；
- [x] 非法 metallic、roughness 和颜色空间输入在加载期失败；
- [x] ShadowDepth 复用同一份 opacity/coverage 语义。

## 7. Phase 4：Direct Cloth Evaluator

### 7.1 Shader 文件

新增：

- `shader/glsl/engine/clothLighting.glsl`

必要时新增公共公式文件：

- `shader/glsl/common/clothBrdf.glsl`

修改：

- `shader/glsl/engine/forwardLighting.glsl`
- `shader/glsl/engine/deferredLighting.glsl`
- `shader/glsl/common/shadingModel.glsl`

### 7.2 共享 evaluator

Forward 和 Deferred 必须调用同一个 Cloth 核心 evaluator，至少共享：

- `r_s -> alpha_s`；
- Charlie NDF；
- Neubelt visibility；
- `E_s(N_o, alpha_s)` 查询；
- `c_s` 颜色语义；
- `T_b` 计算；
- per-light shadow 作用域；
- IBL fallback / prefilter 选择。

建议接口：

```text
ClothLightingResult EvaluateClothSurface(
    MaterialSurface surface,
    lighting context,
    cloth lookup resources)
```

结果至少保留：

```text
directDiffuse
directSpecular
directSheen
indirectDiffuse
indirectSpecular
indirectSheen
baseEnergyScale
directionalAlbedo
shadow
```

### 7.3 Direct 光照账本

每盏灯贡献必须遵守：

```text
L_j = L_i,j * attenuation_j * shadow_j * N_i
      * [ c_s * D_Charlie * V_N
          + (1 - c_s * E_s) * f_base ]
```

验收重点：

- directional、point、spot 的 shadow 只影响对应 light；
- AO 不压暗 direct sheen；
- base 和 sheen 都乘入射投影余弦；
- `T_b` 依赖 view 和 sheen roughness，不是每盏灯单独计算的权重；
- emissive 不参与 `T_b`；
- `sheenColor = 0` 时与 Default Lit 保持一致。

### 7.4 Shading Model dispatch

在以下入口补充分支：

- `ShadeForwardSurfaceDetailed()`；
- `ShadeDeferredSurfaceDetailed()`。

不得把 Cloth 直接落入 Default Lit 的 `default` 分支，也不得只增加 ID 分支而没有独立 evaluator。

### 7.5 Phase 4 验收

- [x] Forward Cloth direct lighting 完成；
- [x] Deferred Cloth direct lighting 完成；
- [x] 两条路径使用同一 `clothLighting.glsl` evaluator；
- [x] `E_s` LUT 查询、`T_b` 能量组合和 shadow 作用域已由共享代码路径固定；
- [x] Default Lit 非 Cloth 结果保持原有 dispatch；
- [x] Cloth ID 8 不再落入 Default Lit fallback。

## 8. Phase 5：IBL 与环境预计算

### 8.1 MVP fallback

第一阶段可以使用明确标记的 fallback：

- base diffuse/specular IBL 继续使用现有路径；
- base IBL 必须乘同一个 `T_b`；
- sheen IBL 暂时使用已有 diffuse irradiance 近似；
- fallback 必须有 Debug View 标记；
- 不得复用 GGX split-sum LUT 并宣称已完成 Charlie sheen IBL。

### 8.2 Target realtime：Compute-only Charlie IBL（延后）

本次完成边界停在 `sheenIblVersion = 0` 的明确 MVP fallback。Charlie 专用环境预滤波需要新增环境资源的视图相关响应、派生纹理身份和环境代际提交协议；在这些合同没有冻结前不实现近似 prefilter，也不把 diffuse irradiance fallback 宣称为正式 Charlie IBL。

后续新增或扩展：

- Target realtime：`shader/glsl/generator/clothSheenPrefilter.comp`（未实现）
- `source/pipeline/clothSheenPrefilterGenerator.*`
- `source/render/environment/environmentIblBaker.*`
- `source/render/cloth/clothResourceLoader.*`

目标链路：

```text
environment cubemap
  -> Compute Charlie/Neubelt prefilter
  -> cloth sheen prefiltered environment
  -> cloth IBL evaluator
```

所有以下数据都必须由 Compute Shader 生成：

- Charlie sheen environment prefilter；
- sheen 专用 BRDF/visibility lookup；
- 代表方向表；
- 环境响应平均值或其它派生 metadata；
- 任何运行时需要的 Cloth 派生纹理。

CPU 只负责输入环境资源、序列化参数、调度 Compute 和管理生命周期。

### 8.3 IBL 验收

- [x] MVP sheen IBL 使用明确标记的 diffuse irradiance fallback；
- [x] base IBL 和 direct 使用同一 `T_b`；
- [x] fallback 状态由 `sheenIblVersion = 0` 和 Debug View 73 明确区分；
- [x] Cloth 不复用 GGX split-sum LUT 作为 sheen BRDF；
- [ ] 恒定白环境、HDR probe grazing 和正式 Charlie prefilter 对照，留待 Target realtime；
- [ ] 环境变化驱动 Charlie 专用资源 digest 和 Compute 重建，留待 Target realtime。

## 9. Phase 6：Reload、Debug、场景和性能

### 9.1 Compute 热重载

参考：

- `source/render/eye/eyeComputeReloadParticipant.*`
- `source/shader/reload/computePipelineReloadParticipant.*`
- `documents/rendering/shader-hot-reload.md`

新增 Cloth Compute reload participant，保证：

- 修改 `clothLookupTables.comp` 生成新候选 LUT；
- 修改 Cloth lighting shader 走 Material transaction；
- generator ABI 不兼容时拒绝候选；
- 旧 LUT、descriptor、pipeline 按 GPU epoch 退休；
- active World 不在 worker 或 prepare 阶段被修改；
- Compute 生成失败保留旧版本，不发布半成品。

### 9.2 Debug View

至少增加：

```text
Cloth Shading Model ID
Sheen Color
Sheen Roughness
Charlie D
Neubelt Visibility
Directional Albedo E_s
Base Energy Scale T_b
Direct Sheen
Indirect Sheen
Cloth IBL Fallback / Prefilter State
```

Debug View 通过分量模式单独显示 base、sheen、LUT、GBuffer 和 IBL fallback 状态；本次不新增额外的混合开关，避免把调试控制状态扩散到生产材质参数。

### 9.3 测试场景与资产

新增：

- `<resourcePath>/materials/cloth/MI_cloth_test.json`
- `<resourcePath>/materials/cloth/MI_cloth_dark_sheen.json`
- `<resourcePath>/materials/cloth/MI_cloth_zero_sheen.json`
- `<resourcePath>/scenes/SC_cloth_models.json`
- 一个用于 grazing 角度的球体/布片测试模型；
- 一个包含 directional、point、spot light 和 shadow 的测试场景。

测试材质至少覆盖：

- 白色、黑色、低饱和、高饱和 sheen；
- 低、中、高 sheen roughness；
- `sheenColor = 0`；
- base 较暗但 sheen 较亮；
- normal map 开启和关闭。

### 9.4 轻量性能证据

本次不新增专用 benchmark 或 readback 测试。`--framesmoke 2` 的现有诊断记录启动后的短帧行为；最近一次 Cloth Debug View shader 变更后的结果为 `avgFrameMs=36.028750`、`avgRenderLoopMs=5.094250`，其中首帧包含 shader/World 初始化开销，不能冒充稳态性能基线。

Charlie 专用 Compute prefilter 尚未实现，因此不记录其更新时间或显存占用。正式 Forward/Deferred steady-state baseline 留待 Target realtime 或后续性能任务。

### 9.5 Phase 6 验收

- [x] Cloth Compute reload participant 已接入 ABI 校验、candidate resource package 和 GPU epoch retirement；
- [x] Debug View 64–73 能定位 BRDF、LUT、GBuffer 能量补偿和 IBL fallback；
- [x] Cloth 测试场景可启动并完成短帧 smoke；
- [x] Forward/Deferred 使用同一 evaluator，结构对照完成；未新增图像 diff 测试；
- [x] 轻量 smoke 性能证据已记录，并明确不等同于稳态 benchmark。

## 10. Phase 7：最终验收与合同迁移

### 10.1 实际验证命令

本次不新增或运行专用 Cloth CTest；只执行与改动直接相关的 build、启动期 shader 编译和 Cloth runtime smoke：

```powershell
cmake --build build -j 2
.\build\bin\main.exe --initial-scene scenes/SC_cloth_models.json --framesmoke 2 --exit-after-tests
```

验证时间：2026-08-23。共享 `shader/spv/` 的 runtime validation 继续要求串行执行。

### 10.2 MVP 验收矩阵

#### 合同与参数

- [x] Charlie/Neubelt 公式、roughness mapping、LUT 和 `sheenIblVersion` 版本一致；
- [x] `sheenColor = 0` 的 base 回退、线性颜色和非金属约束已固定在加载/共享 shader 路径；
- [x] GBufferD Cloth encode/decode 与其它 Shading Model 使用显式分支；
- [x] Forward/Deferred 使用相同 Cloth evaluator。

#### 能量与光照

- [x] direct 与 MVP IBL 使用同一 `T_b`；
- [x] per-light shadow 只作用于对应 direct light，AO 不进入 direct sheen；
- [x] emissive 独立叠加，不进入 sheen/base 能量账本；
- [x] 详细白炉/互易性数值测试未新增，避免产生与当前任务无关的 test target。

#### Compute-only 与运行时

- [x] 生产 `E_s` LUT 由 Compute Shader 唯一生成，无 CPU fallback；
- [x] generator source digest、candidate resource package、descriptor 更新和 GPU epoch retirement 已接入；
- [x] Cloth ID 8 正确进入 Forward/Deferred，最小 Cloth 场景 smoke 通过；
- [x] 现有 Default Lit、Hair、Eye、Subsurface 路径未因 Cloth 分支改变。

#### Target realtime 边界

- [x] 当前 fallback 状态在合同和 Debug View 中可见；
- [ ] Charlie 专用 environment prefilter、白炉数值对照和 HDR grazing 对照留待后续扩展。

### 10.3 合同迁移

- [x] 最终公式、资源和生命周期已迁移到 `documents/rendering/cloth-shading-model.md`；
- [x] 本计划已改为完成记录，并保留 build、shader 编译和 Cloth smoke 证据；
- [x] `documents/README.md` 已加入当前 Cloth rendering contract 和完成计划索引；
- [x] 本次不修改外部个人知识库；仓库正式合同已明确 MVP/Target realtime 的实际状态；
- [x] 正式合同已记录 anisotropic/transmission/BSSRDF、真实纱线几何和 Charlie 专用 prefilter 的未实现边界。

## 11. 预计文件落点

### 11.1 Engine、资源和后续扩展落点

已实现：

- `source/render/cloth/clothAssets.*`
- `source/render/cloth/clothResourceLoader.*`
- `source/render/cloth/clothResourceSet.*`
- `source/render/cloth/clothComputeReloadParticipant.*`
- `source/pipeline/clothLookupTableGenerator.*`
- `source/render/resource/rendererResourceLoadCoordinator.*`
- `source/render/resource/rendererResourceCache.*`
- `source/material/validation/materialAssetValidator.*`
- `source/materialInstanceValidator.*`

Target realtime 后续落点：

- `source/pipeline/clothSheenPrefilterGenerator.*`
- `source/render/environment/environmentIblBaker.*` 的 Cloth 环境资源扩展
- 独立 test-only reference/white-furnace target（仅在后续需要数值回归时增加）

### 11.2 Shader

- `shader/glsl/M_cloth.json`
- `shader/glsl/M_cloth.vertex.glsl`
- `shader/glsl/M_cloth.surface.glsl`
- `shader/glsl/materialFunction/mf_clothInputs.glsl`（如需要独立 MF）
- `shader/glsl/engine/materialInputs.glsl`
- `shader/glsl/engine/materialSurface.glsl`
- `shader/glsl/engine/gbufferCodec.glsl`
- `shader/glsl/engine/clothLighting.glsl`
- `shader/glsl/common/clothBrdf.glsl`
- `shader/glsl/engine/forwardLighting.glsl`
- `shader/glsl/engine/deferredLighting.glsl`
- `shader/glsl/generator/clothLookupTables.comp`
- Target realtime：`shader/glsl/generator/clothSheenPrefilter.comp`（未实现）

### 11.3 Runtime 资产和文档

- `<resourcePath>/materials/cloth/MI_cloth_test.json`
- `<resourcePath>/materials/cloth/MI_cloth_dark_sheen.json`
- `<resourcePath>/materials/cloth/MI_cloth_zero_sheen.json`
- `<resourcePath>/models/cloth/SM_cloth_test.json`
- `<resourcePath>/models/cloth/SM_cloth_dark_sheen.json`
- `<resourcePath>/models/cloth/SM_cloth_zero_sheen.json`
- `<resourcePath>/scenes/SC_cloth_models.json`
- `documents/rendering/cloth-shading-model.md`
- `documents/plan/rendering/cloth-shading-model-development-plan.md`
- `documents/README.md`

## 12. 执行顺序与停线规则

本次已按以下顺序完成 MVP；Target realtime 在 Phase 5 明确停线，不以近似视觉结果掩盖未冻结的环境资源合同：

```text
Phase 0 合同冻结                   [完成]
  -> Phase 1 共享公式/验证取舍     [完成；未新增专用测试]
  -> Phase 2 Compute-only E_s LUT  [完成]
  -> Phase 3 MaterialInputs/GBuffer [完成]
  -> Phase 4 Direct evaluator       [完成]
  -> Phase 5 MVP IBL fallback       [完成；Charlie prefilter 延后]
  -> Phase 6 reload/debug/scene     [完成；轻量 smoke 证据]
  -> Phase 7 contract migration     [完成]
```

任何阶段出现以下情况必须停线修正，不能通过视觉调参掩盖：

- 公式版本、LUT 版本或 roughness mapping 不一致；
- Forward 和 Deferred 使用不同 Cloth closure；
- Compute 生成失败仍然发布 candidate；
- CPU 路径重新出现生产 LUT 数值生成；
- GBuffer customData 被多个模型无版本地复用；
- direct 和 IBL 使用不同的 `T_b`；
- shadow、AO、emissive 混入错误的能量账本；
- Cloth 只注册了 ID，却没有完整 lighting、IBL 和验证路径；当前 MVP 必须保持 fallback 状态显式可见。
