# Eye Shading Model 执行方案

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 状态 | 已完成当前计划范围（2026-08-23） |
| 目标 | 将知识库中的 Eye Reference、UE 5.8 Legacy/Substrate parity 与 VulkanLearn 路径分阶段落地 |
| 知识库基线 | `D:\YYBWorkSpace\GitHub\yyb-knowledge-book\src\content\docs\rendering\materials\shading-models\eye.mdx` |
| 当前仓库基线 | Eye ID 9、Forward/Deferred/双壳、virtual iris intersection、Compute-only Caustic LUT、局部 SSS、World-local resource set、authoring adapter、性能预算和 runtime validation 已落地 |
| 计划建立日期 | 2026-08-23 |
| 文档职责 | 保留已执行的阶段决策、验收矩阵和明确非目标；当前稳定字段位于 `documents/rendering/eye-shading-model.md` |
| 第一里程碑 | 已完成：ForwardOpaque 单壳 Eye、Cornea reflection、virtual iris intersection、pupil/limbus、`T_iT_o` inner response 和 Compute-only Caustic LUT |
| 当前验收结论 | Forward、Deferred fallback、双壳、局部 SSS、LOD/gaze/pupil、GPU readback、性能预算、authoring adapter 和 Compute reload 事务闭环 |

本计划不把 `SHADING_MODEL_EYE = 9` 当作 Eye 已经实现。完整 Eye 视觉依赖材质输入、
角膜与虹膜几何、UV/frame、光路能量、阴影、SSS、预计算资源和 RenderGraph 执行路径。

计划采用以下主路线：

```text
Forward Opaque 单壳 Eye 主路径
    -> 建立可运行、可调试、可回归的 UE-compatible MVP
    -> Compute-only Caustic / 高阶 Lookup
    -> Sclera SSS 与高质量分层合成
    -> Deferred 压缩 fallback
    -> 双壳 Forward / LOD 生产组合
```

## 实现结果（2026-08-23）

本计划的 **当前工程范围已经完成**。实现与稳定合同分别位于：

- 稳定合同：`documents/rendering/eye-shading-model.md`
- 运行时 probe：当前 `config/config.json -> resourcePath` 下的 `eyeProfiles/`、`MI_eye_probe.json`、`SM_eye_probe.json` 和 `SC_eye_probe.json`
- 纯 C++ 合同测试：`tool/eye-tests/`
- GPU/runtime 验证入口：`--eye-validation-test`、`--eye-performance-test`、`--eye-compute-reload-test`

| 阶段 | 当前状态 | 事实边界 |
| --- | --- | --- |
| E0 合同与验证资产 | 已完成 | profile、单位、ID、LUT、debug 编号和 probe 合同已冻结 |
| E1 ForwardOpaque | 已完成 | 独立不透明 pass；Eye 不进入 Geometry GBuffer 或透明排序 |
| E2 GPU Eye Lookup | 已完成（MVP） | 64×64×256、RGBA16F、Compute-only Caustic LUT；无 CPU texel fallback |
| E3 Inputs/Intersection | 已完成 | `EyeMaterialInputs`、Snell、virtual iris plane、valid hit、UV、mask |
| E4 Forward Eye MVP | 已完成 | Cornea、`T_i/T_o`、inner direct、低频 inner IBL、emissive |
| E5 Caustic/Shadow | 已完成 | LUT gain/transmission、cornea/inner CSM、contact/cilia 独立权重和 GPU readback |
| E6 Sclera/IBL | 已完成（工程近似） | 独立 `sssSource`、Eye rejection、双向 filter/compose；不宣称真实 tissue transport |
| E7 Deferred/双壳/LOD | 已完成 | 9-output GBuffer V1、Deferred evaluator、inner/cornea pass、authoring adapter、LOD/gaze/pupil 合同 |
| E8 Debug/Validation/文档 | 已完成 | debug 42–63、三条 runtime test、性能 budget、Compute reload/epoch retirement 和稳定合同已落地 |

完整 path-traced eye、UE 私有 shader parity、真实 screen-space tissue model 和目标硬件 GPU counter 基线属于明确非目标，不作为本计划未完成项。

## 1. 目标与边界

### 1.1 总目标

建立一条与知识库 path ledger 一致、并能融入当前 VulkanLearn Material、RenderGraph、
Shader Cache、热重载和 World/Graph 事务的 Eye 路径：

```text
Eye Material Inputs
    -> 稳定 Eye Frame / Geometry Contract
    -> Cornea dielectric reflection
    -> View-side Snell + virtual iris intersection
    -> Iris / Pupil / Limbus / Sclera region
    -> Light-side transmission + View-side transmission
    -> Iris / Sclera direct and indirect response
    -> Compute-generated Caustic / higher-order lookup
    -> Shadow / SSS / composition
```

实现必须显式区分：

- 外层角膜 reflection；
- 进入眼球内部的 light-side transmission；
- inner radiance 离开角膜时的 view-side transmission；
- iris、pupil、limbus 和 sclera 的区域语义；
- caustic 的能量重分布；
- sclera tissue diffusion；
- emissive 的独立加法路径。

### 1.2 第一里程碑（已完成）

第一里程碑先完成 **Forward Opaque 单壳 Eye MVP**，并作为后续路径的共同基线：

- 角膜使用 dielectric Fresnel 与窄 GGX lobe；
- 使用稳定 eye frame、Snell 折射和 virtual iris plane 求交；
- `IrisMask`、pupil mask、limbus mask 与 opacity 保持独立；
- direct inner lighting 使用分离的 `T_i` 与 `T_o`；
- cornea IBL 复用现有 environment prefilter 与 BRDF LUT；
- inner IBL 使用明确标记的低频 SH/probe approximation；
- caustic LUT 只由 Compute Shader 生成；
- shader、descriptor、World candidate 和 GPU epoch retirement 保持现有事务语义。

### 1.3 最终范围（已完成）

最终计划覆盖：

1. UE-facing Legacy Eye 与 Substrate Eye 输入语义；
2. Eye geometry、UV、单位、缩放和 LOD contract；
3. Forward Opaque 单壳 Eye evaluator；
4. Compute-only Caustic LUT、source identity 与事务热重载；
5. 局部 Sclera SSS、eye-lid/cilia/contact visibility 参数合同；
6. Deferred 压缩 fallback；
7. 双壳 Forward 和多 pass composition；
8. Debug View、数学 reference、GPU readback 与运行时验证；
9. Compute 生成资源的事务热重载。

### 1.4 非目标

本计划不包含：

- 直接复制或声称逐行复刻 UE 私有 Eye shader；
- 把 Digital Humans 材质实例参数冒充 UE Legacy Eye 官方根节点输入；
- 完整 path-traced 眼球、房水体积和高阶角膜内反射；
- 用普通 DefaultLit、ClearCoat 或一张球面 iris texture 冒充 Eye；
- 用同一个 normal 同时承担 cornea reflection、iris intersection 和 sclera SSS；
- 用 CPU 生成生产 Caustic、transmission、IBL 或 SSS lookup texel；
- 在 Compute 失败时上传 CPU fallback texture；
- 通过每帧 `clamp()`、无条件环境补光或 `device.waitIdle()` 掩盖资产、能量或生命周期问题；
- 直接编辑 `shader/spv/` 作为实现手段。

## 2. 当前基线与缺口

| 领域 | 当前情况 | 计划影响 |
| --- | --- | --- |
| Shading Model 注册 | `Eye` 已映射到 ID 9，GLSL 已有 `SHADING_MODEL_EYE` | 保持 ID 不变，只追加明确实现 |
| Material Inputs | `EyeMaterialInputs` 已冻结 | Eye 字段不复用无语义 `customData`，Forward/Deferred 共用 evaluator |
| Material Surface | world/frame、mask、profile、gaze/pupil 字段已接入 | Forward 直接消费，Deferred 走版本化 GBuffer codec |
| Forward 分发 | `ForwardOpaque`、`ForwardEyeInner`、`ForwardEyeCornea` 已实现 | 单壳和双壳均不进入透明排序 |
| Deferred 分发 | Eye GBuffer V1 与 Deferred evaluator 已实现 | Base Pass 冻结 inner sample，lighting 不重新访问 MI 纹理 |
| GBuffer | Geometry pass 固定 9 个输出 | Eye 独占版本化 D/E/F/Velocity 语义并有 C++ round-trip tests |
| Compute lookup | Eye Caustic LUT 与 GPU readback 已实现 | `64 x 64 x 256`、RGBA16F、约 8 MiB，无 CPU texel fallback |
| RenderGraph | Forward、Deferred、双壳和 `sssSource` 链已实现 | 静态 lookup 仅在 prepare/reload dispatch |
| SSS | 独立 `sssSource`、Eye rejection、双向 filter/compose 已实现 | 只扩散 tissue diffuse；不是完整物理 tissue transport |
| Hot reload | Eye-specific Compute participant 已实现 | pipeline/LUT/package/descriptor 原子替换并按 GPU epoch 退休 |
| Runtime 资产 | Forward、Deferred、双壳和失败回滚 probe 已实现 | 资产位于 `config.json -> resourcePath`，生成物写 `generated/` |

## 3. 不可变执行原则

### 3.1 保持同一条 Path Ledger

最低阶 direct path 固定为：

```math
L_{direct}=
V_c E_i f_c^R+
T_o\left[V_iT_iC E_i f_{iris}+V_iT_iE_i f_{sclera}\right]
```

其中：

- `V_c`：角膜 direct visibility；
- `V_i`：inner path visibility；
- `T_i`：light-side 进入角膜的 transmission；
- `T_o`：view-side 离开角膜的 transmission；
- `C`：只作用于 iris direct irradiance 的 caustic gain；
- `f_iris` / `f_sclera`：各自的 inner closure。

不得把完整 cornea reflection、未衰减 inner diffuse 和任意 caustic add 各算一次后相加。

### 3.2 Legacy 与 Substrate 共享 Evaluator

Legacy 与 Substrate 只改变输入组织方式：

| 物理事实 | Legacy adapter | Substrate adapter |
| --- | --- | --- |
| Cornea normal | `Normal` + geometry contract | `CorneaNormal` |
| Iris region | `Iris Mask` | `IrisMask` |
| Iris depth | `Iris Distance` | `IrisDistance` |
| Iris detail normal | Material/UV contract | `IrisNormal` |
| Iris proxy plane | Geometry/UV contract | `IrisPlaneNormal` |
| Sclera diffusion | Legacy profile binding | `SubsurfaceProfile` |

两条 authoring 路径最终必须填充同一个 `EyeMaterialInputs`，并调用同一个
`ShadeEyeSurface()`。禁止复制 Legacy/Substrate 两套光照代码。

### 3.3 所有预计算统一走 Compute Shader

Eye lookup 数据必须由 Compute Shader 生成。CPU 只允许：

- 枚举并解析 Eye profile JSON；
- 校验 schema、版本、ID、单位、范围和重复项；
- 将作者参数序列化到 SSBO/UBO；
- 创建 GPU image、buffer、descriptor、pipeline 和同步命令；
- 计算资产与 Compute artifact 的稳定内容摘要；
- 管理 World-local 资源、候选发布和 GPU epoch retirement。

CPU 不得保留以下生产逻辑：

- Caustic differential refraction/Jacobian 求值；
- Caustic texel 归一化；
- 高阶 average transmission lookup 生成；
- direction-dependent inner IBL lookup 生成；
- Sclera profile lookup 生成；
- CPU `HostImage` lookup 构建或上传 fallback。

Reference CPU 实现只允许存在于测试代码，用于对照少量已知输入，不得成为运行时资源源。

### 3.4 Forward Opaque 是主生产路径

Forward Opaque 保留完整 MI texture、对象级 eye frame 与单壳 view intersection，是当前最高保真
单壳路径。Deferred fallback 在 Base Pass 冻结 inner sample 与最小 lighting facts，lighting 阶段不再
访问原 MI texture，也不尝试从压缩数据恢复未保存的完整分层状态。

Forward 主路径使用：

```text
RenderMode::ForwardOpaque
    -> no blend
    -> depth test/write enabled
    -> Material Set + Object Set + Pass Set
    -> sceneColor + sceneDepth
```

Deferred GBuffer V1、双壳 inner/cornea pass 与局部 SSS composition 已在该基线上完成。

### 3.5 Geometry 与单位属于 Shading Contract

标准 Eye mesh 合同固定：

- object origin 是眼球中心；
- local `+Z` 指向瞳孔/角膜顶点；
- local `+X/+Y` 是 iris plane 横纵轴；
- 所有物理长度转换到米；
- mesh/scene transform 只允许均匀缩放；
- UV handedness、cornea bulge、iris plane 和 LOD 必须显式记录；
- 非均匀缩放在加载阶段拒绝，不能在 shader 内按像素补救。

### 3.6 信任上游有效数据

Eye 参数范围、半径关系和 profile/LOD 匹配由资产加载阶段保证。Shader 只保留物理上必要的
TIR、无有效交点和 domain 分支，不增加重复的每像素 bounds 修补。

## 4. 已实现数据流

### 4.1 World Candidate Prepare

```text
World candidate prepare
    -> parse Eye profile assets
    -> validate schema / units / geometry ranges / stable IDs
    -> resolve ComputeShaderArtifact for generator/eyeCausticLut
    -> sourceDigest(asset bytes + schema versions + artifactGenerationKey)
    -> reuse previous EyeResourceSet when digest matches
    -> serialize generation parameters
    -> dispatch eyeCausticLut.comp
    -> transition lookup image to shader-read layout
    -> bind immutable Eye world textures
    -> load Eye materials and validate profile references
    -> prepare RenderGraph pass descriptors
    -> commit through WorldGraphTransactionCoordinator
```

Lookup 生成必须发生在 Eye material 和相关 pass descriptor 最终绑定之前，保证同一 World
generation 只消费同一份 Eye resource package。

### 4.2 Forward Opaque Frame Path

```text
M_eye Surface Evaluation
    -> authored EyeMaterialInputs
    -> view-side refraction + iris hit + texture sample
    -> MaterialSurface snapshot

ForwardOpaque Eye evaluator
    -> cornea direct reflection
    -> cornea IBL
    -> light-side transmission
    -> iris/sclera direct
    -> shadow-gated caustic lookup
    -> low-frequency inner IBL
    -> view-side transmission
    -> emissive
    -> sceneColor + sceneDepth
```

### 4.3 已实现的分层路径

当前双壳与 SSS 路径拆为：

```text
ForwardEyeInner
    -> iris/sclera direct + inner IBL
    -> tissue diffuse + profile identity

sssSource + horizontal/vertical filter
    -> only diffuse tissue domain
    -> reject cornea / non-eye / profile-discontinuous samples

ForwardEyeCornea / composition
    -> cornea reflection
    -> additive top-layer response over inner result
```

角膜高光和泪膜响应不进入 SSS blur。该路径是工程近似，不等同于真实 screen-space tissue transport。

## 5. Material、Geometry 与资产稳定合同

### 5.1 EyeMaterialInputs

`MaterialModelInputs` 中的稳定 Eye 字段为：

```glsl
struct EyeMaterialInputs
{
    vec3 corneaNormal;
    float corneaIor;
    vec3 irisNormal;
    float irisMask;
    vec3 irisPlaneNormal;
    float irisDistance;
    vec3 irisColor;
    float irisRadius;
    vec3 scleraColor;
    float pupilRadius;
    float limbusWidth;
    float causticProfileId;
    float scleraProfileId;
    float causticStrength;
    float validIrisHit;
    vec2 irisUv;
    float irisHitDistance;
    float pupilMask;
    float limbusMask;
    float eyeLayer;
    float contactVisibility;
    float ciliaVisibility;
    float uvHandedness;
    float pupilDilation;
    vec3 gazeDirection;
    float gazeWeight;
};
```

该合同满足：

- 字段只属于 Eye；
- `corneaNormal`、`irisNormal`、`irisPlaneNormal` 保持三种独立语义；
- `irisMask`、pupil mask 和 opacity 不复用；
- `corneaIor`、长度和 profile ID 不通过普通 PBR 通道隐式传递；
- runtime evaluator 不重新读取 Material UBO 以外的隐式状态。

### 5.2 Eye Profile Asset

资源目录：

```text
<resourcePath>/Common/Profiles/Eye/
```

当前 schema 示例：

```json
{
    "name": "Human Eye Default",
    "type": "eyeProfile",
    "schemaVersion": 1,
    "profileVersion": 1,
    "profileId": 1,
    "sourceIdentity": "vulkanlearn.eye.compute.v1",
    "corneaIor": 1.376,
    "eyeRadius": 12.0,
    "corneaRadius": 7.8,
    "irisDistance": 3.0,
    "irisRadius": 6.0,
    "pupilRadiusRange": [1.5, 4.0],
    "distanceUnit": "millimeter",
    "worldUnitScale": 0.001,
    "causticLutVersion": 1
}
```

固定约束：

- `profileId == 0` 保留为 neutral/invalid；
- stable path 和 profile ID 必须唯一；
- `corneaIor > 1`；
- 所有长度有限且大于零；
- pupil range 必须位于 iris radius 内；
- `worldUnitScale` 必须与 `distanceUnit` 一致；
- LUT version、geometry version 和 profile version 必须匹配；
- 不同 LOD 如果改变 cornea shape 或 iris depth，必须使用不同 profile identity。

### 5.3 M_eye 与 MI

已落地材质定义：

```text
shader/glsl/M_eye.json
shader/glsl/M_eye.vertex.glsl
shader/glsl/M_eye.surface.glsl
shader/glsl/M_eyeDeferred.*
shader/glsl/M_eyeInner.*
shader/glsl/M_eyeCornea.*
```

M_ 文件声明稳定 authoring schema，并按以下语义分组：

| 分组 | 建议字段 |
| --- | --- |
| Cornea | IOR、roughness、normal strength |
| Iris geometry | iris distance、iris radius、pupil radius、limbus width |
| Profile identity | eye profile、sclera profile |
| Artist color | iris tint、sclera tint、emissive |
| Textures | iris color、iris normal、iris mask、sclera color/detail、optional packed mask |

MI 使用资产引用：

```json
{
    "material": "shader/glsl/M_eye.json",
    "eyeProfile": "Common/Profiles/Eye/EP_human_default.json",
    "subsurfaceProfile": "Common/Profiles/Subsurface/SSP_skin.json"
}
```

Loader 将资产路径解析为稳定 ID，并覆盖引擎派生参数。MI 不直接作者化最终 GPU table layer。

### 5.4 Legacy 与 Substrate Authoring Adapter

`tool/eye-authoring-adapter/` 已提供 Legacy/Substrate-style 输入到 VulkanLearn MI 参数的离线映射。
两种 authoring 风格只改变输入映射，不改变：

- `EyeMaterialInputs`；
- `EyeShadingState`；
- `ShadeEyeSurface()`；
- Caustic/Profile 资源格式；
- Debug View 语义。

## 6. Compute-only Lookup 合同

### 6.1 预计算分类

| 内容 | 执行位置 | 原因 |
| --- | --- | --- |
| Exact/Schlick Fresnel | Runtime fragment | 计算便宜，依赖当前方向 |
| Snell direction | Runtime fragment | 依赖当前 view/light direction |
| Iris ray-plane intersection | Runtime fragment | 依赖当前像素与 eye transform |
| Caustic Jacobian/integration | Compute precompute | 多维积分/微分映射，适合版本化 LUT |
| Higher-order average transmission | Compute precompute（需要时） | 属于降阶补偿，不应成为任意 scalar |
| Directional inner IBL | Compute precompute（高质量阶段） | 需要环境方向折射积分 |
| Sclera profile | 复用现有 Compute lookup | 已有 Compute-only SSS 合同 |
| Cornea specular IBL | 复用现有 Compute environment/BRDF 资源 | 不重复生成 Eye 专用同义资源 |

### 6.2 Caustic LUT 参数化

当前 V1 利用旋转对称 eye profile 降维：

- XY：iris disk 上相对 light projection frame 的坐标；
- array layer：profile 与 light elevation slice；
- pupil dilation：使用经过 profile 验证的 runtime aperture mask；
- 未来若需要把 dilation 纳入预积分维度，必须 bump LUT version；
- profile 固定 cornea shape、IOR、iris distance、iris radius 和单位。

固定格式：

```text
R16G16B16A16_SFLOAT sampler2DArray
```

固定通道：

| 通道 | 语义 |
| --- | --- |
| R | 归一化 caustic gain |
| G | light-side average transmission / normalization metadata |
| B | valid domain / coverage |
| A | raw Jacobian 或 debug normalization term |

V1 固定为 `64 x 64 x 256`、16 个 elevation slice、profile ID `0..15`，总内存
`8,388,608` bytes（约 `8 MiB`）。profile ID 0 是 neutral layer，1..15 是 authored 范围。

### 6.3 EyeResourceSet

已实现 World-local 不可变资源集，核心所有权可概括为：

```cpp
class EyeResourceSet
{
public:
    std::string sourceDigest;
    std::vector<EyeProfileAsset> profiles;
    std::shared_ptr<Texture> causticLutTexture;
};
```

实际类型同时保存 stable path/ID lookup、LUT metadata、生成 artifact identity 和 descriptor 绑定信息。

### 6.4 Source Digest

`sourceDigest` 至少包含：

- Eye profile schema/version；
- LUT contract/version；
- 每个 profile 的规范化路径与内容 BLAKE3-256；
- Compute artifact `artifactGenerationKey`；
- Compute ABI/workgroup identity；
- 影响数值的公共 include 传递依赖；
- 固定 LUT 尺寸和通道语义。

不得只哈希 `eyeCausticLut.comp` 主文件，也不得使用时间戳或 `std::hash`。

### 6.5 Vulkan 同步与生命周期

生成步骤固定：

1. 创建 storage image；
2. `undefined -> general`；
3. bind Compute pipeline/descriptor；
4. dispatch；
5. `shader write -> fragment shader read`；
6. `general -> shader read only optimal`；
7. image/memory/view/sampler 所有权交给 `Texture`；
8. 通过 World-local package 发布；
9. 旧 texture 经 GPU frame epoch 退休。

任何失败都必须销毁 candidate 局部资源，active World 不变。

### 6.6 Compute 生成资源热重载（已完成）

`EyeComputeReloadParticipant` 复用正式 `ComputePipelineReloadParticipant` batch，并把候选
pipeline、LUT、`EyeResourceSet`、World-local package 与外部 descriptor refresh 作为一个事务。

当前参与者语义：

```text
EyeComputeReloadParticipant
    -> GetShaderName / GetActiveArtifact
    -> ValidateCandidateAbi
    -> PrepareGeneratedResourceReplacement
         candidate pipeline
         new EyeResourceSet
         new World-local Eye package
         prepared retirements
    -> CommitReplacement noexcept
```

提交顺序：

```text
compile candidate
    -> validate source freshness and ABI
    -> generate candidate LUT
    -> prepare World-local replacement and retirement
    -> publish shader artifact
    -> swap EyeResourceSet/LUT package
    -> refresh RenderGraph pass-input image snapshots and descriptors
    -> activate retirements
```

ABI-compatible edit 和 source restore 会生成新的 pipeline/LUT/package；workgroup-size ABI edit 会被拒绝且 active package 不变。旧 package 至少保留到 descriptor refresh 后的新 frame epoch，并通过 `ResourceRetireQueue` 释放；没有使用全局 `device.waitIdle()` 作为修复。

## 7. Render Path 设计

### 7.1 ForwardOpaque RenderMode

已增加：

```cpp
RenderMode::ForwardOpaque
RenderGraphPassType::ForwardOpaque
```

语义：

- 不透明；
- 不进入 GBuffer；
- 不参与透明排序；
- blend disabled；
- depth test/write enabled；
- 使用 Material/Object/Pass descriptor；
- shadow caster 仍按现有 material Shadow policy 处理。

当前 pass 输入：

```text
binding 0: shadowMap
binding 1: hairAzimuthalLut（若 ForwardOpaque 未来被 Hair probe 复用）
binding 2: eyeCausticLut
```

Pipeline reflection 只要求实际使用子集，未使用的 pass binding 不得迫使所有 material shader
声明同名 sampler。

### 7.2 Eye Evaluator 实际函数边界

`shader/glsl/engine/eyeGeometry.glsl` 与 `eyeLighting.glsl` 的实际函数及职责为：

| 实际函数 | 职责 |
| --- | --- |
| `CreateDefaultEyeGeometrySnapshot()` / `EvaluateEyeGeometry()` | 构造默认几何快照，执行 view-side refraction、virtual iris plane 求交、UV、pupil 与 limbus mask |
| `EyeDielectricF0()` / `EyeFresnel()` | 计算 cornea dielectric `F0` 与 Schlick Fresnel |
| `ResolveEyeIntersection()` | 消费并复核 `MaterialSurface` 中冻结的 iris hit，拒绝与当前代理平面不一致的结果 |
| `SampleEyeIrisColor()` / `SampleEyeIrisMask()` / `SampleEyeIrisNormal()` / `SampleEyeScleraColor()` | 读取冻结后的 inner 区域材质事实 |
| `ApplyEyeScleraDiffuseApproximation()` | 对 sclera tissue diffuse 应用局部 wrap approximation |
| `SampleEyeCaustic()` | 按 profile、elevation、UV 和 active mask 采样 Caustic LUT |
| `CreateDefaultEyeLightingResult()` / `ShadeEyeSurface()` | 初始化结果并合成 cornea、inner direct/IBL、shadow、transmission、caustic 与 emissive |

命名必须明确区分：

- `viewDirectionToCamera`；
- `viewRayPropagationDirection`；
- `lightDirectionToSource`；
- `lightRayPropagationDirection`。

禁止使用一个含糊的 `V`/`L` 在 refract 调用前后改变方向语义。

### 7.3 Deferred Fallback（已完成）

Deferred GBuffer V1 原则：

- Base Pass 完成 view-dependent iris intersection 和材质纹理采样；
- GBuffer 保存冻结后的 inner sample 与重建 lighting 所需最小 facts；
- Deferred lighting 不假装能够重新访问原 MI texture；
- `GBufferD`/`GBufferE`/`GBufferF`/`GBufferVelocity` 的 Eye 语义单独版本化；
- `customData.a` 不得同时表示 iris mask 和 sclera SSS weight；
- 无效 iris hit 会清空 UV/mask，但保留 profile pair 给 SSS rejection 使用。

当前 9-output Geometry 合同：

```text
gbufferA, gbufferB, gbufferC, gbufferD, gbufferE,
gbufferVelocity, gbufferF, sceneColorBase, sceneDepth

GBufferD.xy = irisUv
GBufferD.z  = versioned caustic/sclera profile + valid-hit packing
GBufferD.w  = irisMask
GBufferE.rgb = scleraColor
GBufferE.a   = irisRadius
GBufferVelocity.zw = pupil/limbus ratio
GBufferF.rgb = encoded irisNormal
GBufferF.a   = irisDistance
```

Deferred Eye LUT 位于 Set 3 binding 11；`sssSource` 是独立 attachment，并由 rejection/filter/compose 合同只处理 tissue diffuse。

### 7.4 双壳与多 Pass（已完成）

当前工程组合：

1. inner iris/sclera 使用独立 mesh/material，路由到 `forwardEyeInner`；
2. inner pass 输出 iris/sclera direct、inner IBL 与 emissive；
3. sclera SSS 只处理独立 `sssSource` tissue diffuse；
4. cornea shell 路由到 `forwardEyeCornea`，以 additive top-layer response 合成；
5. contact/eyelid/cilia visibility 分别绑定 cornea 和 inner path；
6. LOD/profile、UV handedness、gaze 和 pupil dilation 由资产/参数合同保持对应。

Forward/双壳 LUT 均使用 Set 3 binding 2；两个双壳 pass 同时保留共享 forward lighting 所需的 Hair LUT binding 1，以保证完整 descriptor layout 兼容。

## 8. 分阶段执行计划

### Phase E0：合同冻结与验证资产（已完成）

任务：

1. 新建本计划与后续稳定合同骨架；
2. 冻结 eye frame、方向、长度单位和缩放规则；
3. 冻结 Legacy/Substrate 到统一输入的映射；
4. 冻结 Eye profile schema、stable ID 和 LUT version；
5. 设计标准眼球 mesh、棋盘 iris、plane/sphere 数学样例；
6. 记录 Caustic LUT 维度、分辨率和内存预算候选；
7. 冻结 Debug View 编号区间。

阶段门：

- 不写生产 shader 前，所有字段都有唯一语义和单位；
- `IrisMask`、pupil、opacity、SSS weight 无复用；
- Forward 主路径与 Deferred fallback 边界明确；
- LUT 参数域可以覆盖标准资产与计划 LOD。

### Phase E1：Forward Opaque 基础设施（已完成）

任务：

1. 增加 `RenderMode::ForwardOpaque`；
2. 增加 RenderGraph pass type、compiler validation 和 runtime recording；
3. 增加 renderer draw filtering，不透明 Forward 不进入透明排序；
4. 增加 pass config、attachment、depth state 和 descriptor contract；
5. 增加最小 Unlit/DefaultLit ForwardOpaque probe，先验证 pass 本身；
6. 补充 pipeline layout、render-pass compatibility 和 World transaction 测试。

阶段门：

- ForwardOpaque probe 正确写 sceneColor/depth；
- Geometry material 不重复绘制；
- Transparent material 路由无回归；
- Shadow、sky 和后处理顺序正确。

### Phase E2：GPU Eye Lookup 资源链路（已完成）

任务：

1. 新增 Eye profile parser/validator；
2. 新增 `EyeResourceSet`、loader 和 cache binding；
3. 新增 `eyeCausticLut.comp` 与 C++ generation parameter layout；
4. 使用 `CreateComputePipelineCandidate()` 在 World prepare 生成 LUT；
5. source digest 使用 Compute artifact generation identity；
6. LUT 作为 World-local texture 绑定到候选 pass；
7. 增加 GPU readback smoke，验证 finite、neutral row/domain 和 normalization；
8. Compute 失败时验证 active World/Graph 不变。

阶段门：

- 生产 texel 只有 Compute Shader 一份真相源；
- 相同 digest 复用上一代 texture identity；
- profile 或 shader 数值变化生成新 identity；
- candidate 失败无正式 artifact、descriptor 或 active texture side effect。

### Phase E3：Eye Material Inputs 与 View Intersection（已完成）

任务：

1. 新增 `EyeMaterialInputs` 默认值；
2. 新增 `M_eye` schema 和 Surface Evaluation；
3. 从 object transform 构造稳定 eye frame；
4. 实现 view-side Snell；
5. 实现 virtual iris plane intersection；
6. 实现 valid hit、iris UV、pupil 和 limbus；
7. 采样 iris/sclera texture 并冻结到 `MaterialSurface`；
8. 增加 geometry/scale/profile 加载期校验。

阶段门：

- 棋盘 iris 在正视、斜视、grazing 和 eye rotation 下连续；
- roughness 不改变 iris hit；
- iris normal 不改变 cornea intersection；
- invalid hit 进入明确 fallback，不采样无限拉伸 UV。

### Phase E4：Forward Eye MVP（已完成）

任务：

1. 新增 `eyeLighting.glsl` 与 Forward dispatch；
2. 只开启 cornea direct/IBL，验证 Fresnel 与 roughness；
3. 接入 iris/sclera inner direct；
4. 分离 `T_i` 和 `T_o`；
5. 接入 pupil/limbus/iris mask；
6. inner IBL 使用明确的低频 approximation；
7. emissive 最后单独加入；
8. 暂时使用 `C=1`，避免 Caustic 干扰基础路径验收。

阶段门：

- 法线入射 cornea reflection 接近 IOR 对应 `F0`；
- grazing reflection 增强、inner transmission 下降；
- 白炉中 `cornea + T_iT_o inner` 不增能；
- `IrisMask=0` 时无 iris/pupil 响应；
- 左右眼在相同曝光下基础亮度一致。

### Phase E5：Caustic、Shadow 与 Contact Visibility（已完成）

任务：

1. 在 Eye evaluator 中按 profile/light coordinate 采样 Caustic LUT；
2. Caustic 只作用于 iris direct irradiance；
3. 使用产生它的 light visibility；
4. 加入 cornea/inner 分路 shadow debug；
5. 接入 eyelid/cilia/contact visibility 近似；
6. 验证 pupil、iris distance、cornea shape 的 profile 有效域；
7. 增加 iris-area 平均能量与 shadow disappearance 测试。

阶段门：

- light 被遮挡时 Caustic 消失；
- Caustic 不修改 cornea reflection；
- 平均 gain 满足 profile normalization；
- profile mismatch 在 loader 阶段失败，不采样未知 layer。

### Phase E6：Sclera SSS 与 Inner IBL（已完成当前工程范围）

任务：

1. Eye MI 引用现有 SubsurfaceProfile asset；
2. MVP 增加局部/preintegrated sclera approximation；
3. 实现 Eye Inner + `sssSource` + Cornea composition；
4. SSS rejection 限制在 sclera/inner domain；
5. cornea specular 保持未扩散；
6. 增加 soft ambient/contact visibility；
7. 保留低频 inner IBL approximation，不引入未验证的逐帧高阶 fallback；
8. 明确真实 tissue transport 与 directional inner IBL LUT 为非目标。

阶段门：

- SSS 只扩散 sclera tissue diffuse；
- 不跨过眼睑、角膜或非 Eye 边界；
- cornea IBL 和 inner IBL 不各自吃满白环境；
- 关闭高阶补偿后仍可通过 Debug View 检查基础路径。

### Phase E7：Deferred Fallback、Authoring Adapter 与 LOD（已完成）

任务：

1. 冻结 Eye GBuffer packing/version；
2. 增加 encode/decode round-trip tests；
3. Deferred 调用同一低阶 Eye evaluator；
4. 增加 Legacy/Substrate-style authoring adapter；
5. 验证 Legacy/Substrate 参数响应一致；
6. 增加单壳/双壳/fallback 对照；
7. 为 geometry LOD 绑定匹配的 profile、iris distance 和 LUT version；
8. 验证左右眼 UV handedness、gaze 和 pupil dilation 一致性。

阶段门：

- Forward/Deferred 使用相同命名与 path ledger；
- packing 不丢失已承诺参数；
- LOD 切换不突然丢失 iris depth、cornea highlight 或 profile；
- 不支持的 authoring 字段在 strict 模式显式拒绝，不静默丢失。

### Phase E8：Debug、验证与文档收敛（已完成）

任务：

1. 增加完整 Eye Debug View 和 UI label；
2. 增加 `--eye-validation-test --exit-after-tests`；
3. 增加 GPU LUT readback 与数学 reference 对照；
4. 增加 Compute 生成资源 reload participant；
5. 验证 shader/source/profile 连续变化的 staleness protocol；
6. 验证 descriptor、resource set 和 pipeline 一次性替换；
7. 验证旧 Eye LUT、pipeline 和 descriptor 通过 epoch 退休；
8. 执行 Forward、Deferred、双壳固定帧性能和回归矩阵；
9. 将稳定实现迁移到 `documents/rendering/eye-shading-model.md`；
10. 更新知识库 Eye 页面中的 VulkanLearn 当前状态。

阶段门：

- 自动测试可重复运行；
- runtime test 失败不修改 active runtime；
- shader reflection、descriptor 和 reload 后的 Eye layout/version 一致；
- DefaultLit、ClearCoat、Hair、SSS、ThinTranslucent 和现有场景无回归。

## 9. 文件落点矩阵

### 9.1 Engine 与资源

已新增：

```text
source/render/eye/eyeAssets.h
source/render/eye/eyeAssets.cpp
source/render/eye/eyeResourceSet.h
source/render/eye/eyeResourceSet.cpp
source/render/eye/eyeResourceLoader.h
source/render/eye/eyeResourceLoader.cpp
source/render/eye/eyeMaterialContract.h
source/render/eye/eyeMaterialContract.cpp
source/render/eye/eyeLookupTableGenerator.h
source/render/eye/eyeLookupTableGenerator.cpp
source/render/eye/eyeComputeReloadParticipant.h
source/render/eye/eyeComputeReloadParticipant.cpp
source/render/eye/eyeGBufferCodec.h
source/render/eye/eyeGBufferCodec.cpp
source/render/eye/eyeLodContract.h
source/render/eye/eyeLodContract.cpp
source/render/eye/eyePerformanceBudget.h
source/render/eye/eyePerformanceBudget.cpp
```

已修改：

```text
source/shaderVariant.h
source/materialInstanceValidator.cpp
source/render/rendergraph/renderGraphPassType.h
source/render/rendergraph/renderGraphCompiler.cpp
source/render/pass/passRuntime.h
source/render/pass/passRuntime.cpp
source/render/backend/rendererDrawExecutor.h
source/render/backend/rendererDrawExecutor.cpp
source/render/resource/rendererResourceCache.h
source/render/resource/rendererResourceCache.cpp
source/render/resource/rendererResourceLoadCoordinator.cpp
source/render/resource/rendererMaterialLoader.cpp
source/shader/reload/shaderReloadCoordinator.*
```

### 9.2 Shader

已新增：

```text
shader/glsl/M_eye.json
shader/glsl/M_eye.vertex.glsl
shader/glsl/M_eye.surface.glsl
shader/glsl/M_eyeDeferred.json
shader/glsl/M_eyeDeferred.vertex.glsl
shader/glsl/M_eyeDeferred.surface.glsl
shader/glsl/M_eyeInner.json
shader/glsl/M_eyeInner.vertex.glsl
shader/glsl/M_eyeInner.surface.glsl
shader/glsl/M_eyeCornea.json
shader/glsl/M_eyeCornea.vertex.glsl
shader/glsl/M_eyeCornea.surface.glsl
shader/glsl/engine/eyeGeometry.glsl
shader/glsl/engine/eyeLighting.glsl
shader/glsl/generator/eyeCausticLut.comp
```

已修改：

```text
shader/glsl/engine/materialInputs.glsl
shader/glsl/engine/materialSurface.glsl
shader/glsl/engine/materialPass.glsl
shader/glsl/engine/forwardLighting.glsl
shader/glsl/engine/deferredLighting.glsl
shader/glsl/engine/gbufferCodec.glsl
shader/glsl/engine/materialDebugView.glsl
shader/glsl/pass/deferredLighting.frag
```

### 9.3 RenderGraph、UI 与测试

```text
config/renderGraphConfig.json
source/engine/launchOptions.*
source/engine/runtimeTestHooks.*
source/engine/testing/eyeRuntimeTests.cpp
source/engine/testing/eyeComputeReloadRuntimeTests.cpp
source/engine/testing/runtimeTestFixtures.*
source/engine/testing/runtimeValidationServices.*
source/ui/uiSubsystem.*
tool/eye-tests/*
tool/eye-authoring-adapter/*
```

### 9.4 Runtime 资产

已在 `config/config.json -> resourcePath` 增加：

```text
<resourcePath>/Common/Profiles/Eye/EP_*.json
<resourcePath>/Maps/SC_eye/Materials/MI_eye_*.json
<resourcePath>/Maps/SC_eye/Meshes/SM_eye_*.json
<resourcePath>/Maps/SC_eye/Textures/T_eye_*.json
<resourcePath>/Maps/SC_eye/SC_eye.json
```

生成输出如需落盘，只能写入：

```text
<resourcePath>/Generated/Validation/SC_eye/
```

生成 metadata 或 readback 不能成为下一次运行的 authoring source of truth。

## 10. Debug View 冻结编号

Hair 占用 21–41，Eye 固定使用 42–63：

| Mode | 含义 |
| ---: | --- |
| 42 | Eye Frame |
| 43 | Cornea Normal |
| 44 | Iris Normal |
| 45 | Iris Plane Normal |
| 46 | Cornea Fresnel |
| 47 | Cornea Specular |
| 48 | Refracted View Direction |
| 49 | Iris Hit Distance |
| 50 | Iris UV |
| 51 | Valid Hit Mask |
| 52 | Iris Mask |
| 53 | Pupil Mask |
| 54 | Limbus Mask |
| 55 | Light Transmission In |
| 56 | View Transmission Out |
| 57 | Iris Direct |
| 58 | Sclera Direct |
| 59 | Inner IBL |
| 60 | Caustic Gain |
| 61 | Inner Shadow |
| 62 | Cornea Shadow |
| 63 | Eye Profile |

最终编号在 Phase E0 冻结，后续不得重排已有 Debug View。

## 11. 验收矩阵

本矩阵以本轮工程合同为准。`[x]` 表示已有 C++ test、runtime probe、GPU readback、
shader/descriptor 合同或固定帧预算作为验收证据；真实 tissue transport、完整 UE 私有实现和
目标硬件 GPU counter 基线列在 14.2，属于计划外扩展项。

### 11.1 数学与路径

- [x] 已知 IOR 下 plane reference 的 exact Fresnel、Schlick 与 Snell direction 对照一致；
- [x] smooth cornea 法线入射接近 `F0`，grazing reflection 增强；
- [x] `IrisDistance` 与 cornea roughness 使用独立字段和 evaluator 路径；
- [x] `IrisNormal` 不参与 cornea ray-plane intersection；
- [x] `IrisPlaneNormal` 不被当作 cornea micro normal；
- [x] `IrisMask=0` 时 iris diffuse、pupil 和 caustic 路径归零；
- [x] `cornea reflection + T_iT_o inner` 遵守同一白炉能量账本；
- [x] emissive 在 transmission、shadow 和 caustic 之后独立相加。

### 11.2 Compute Lookup

- [x] GPU readback 覆盖的 LUT texel 全部有限；
- [x] neutral profile/domain 不产生额外能量；
- [x] iris-area 平均 gain 在 profile 参数域内保持归一化；
- [x] profile/shader/version 变化触发新 source digest；
- [x] 相同 digest 在 World transaction 中复用 texture identity；
- [x] Compute 失败无 CPU fallback，candidate World 构建失败且 active World 不变；
- [x] GPU readback 与 CPU Reference 的 gain/normalization 误差均小于 `0.01`；
- [x] light visibility 为零时 runtime caustic contribution 为零。

### 11.3 资产与外观

- [x] 棋盘 iris probe、valid-hit fallback 与 eye-frame 合同覆盖正视、斜视、grazing 和 rotation 域；
- [x] cornea curvature、iris depth、pupil radius 和 eye scale 使用统一米制/profile 合同；
- [x] cornea roughness 只影响 outer reflection lobe，不改变 iris hit；
- [x] iris、limbus、sclera 在 edge-on 时通过连续 mask 与 invalid-hit fallback 收敛；
- [x] sclera SSS 只扩散独立 `sssSource` tissue diffuse；
- [x] 左右眼 UV handedness、pupil dilation、gaze 和 LOD/profile 对应关系已验证；
- [x] contact、cilia 与 cornea/inner shadow 使用独立 visibility 权重，不复用成单一遮挡量。

### 11.4 工程与回归

- [x] M_ schema、MI override、generated include、reflection 与 descriptor 一致；
- [x] ForwardOpaque 不进入 GBuffer 或透明排序；
- [x] Eye ShadowDepth 与主材质 coverage/geometry contract 一致；
- [x] Forward/Deferred/双壳路径共享字段与 path ledger；
- [x] Compute 生成资源热重载保持 batch all-or-nothing；
- [x] source epoch 和 commit-time digest validation 能拒绝 stale candidate；
- [x] 旧 pipeline、descriptor 和 LUT 经 GPU epoch 退休；
- [x] runtime tests 串行执行，不并发写共享 `shader/spv/`；
- [x] 现有 DefaultLit、ClearCoat、Hair、SSS、ThinTranslucent 回归测试通过。

### 11.5 性能

- [x] LUT 生成只发生在 World prepare、资源变化或显式 reload；
- [x] steady-state frame 不执行 Eye LUT precompute；
- [x] Forward、Deferred、双壳固定帧均执行 draw/descriptor/LUT sample 的 CPU-side budget 检查；
- [x] LUT atlas 固定约 `8 MiB`，并由合同测试校验尺寸计算；
- [x] Debug View、GPU readback 和 shader 编译不计入 steady-state 性能样本。

## 12. 风险与回滚

| 风险 | 早期信号 | 处理/回滚边界 |
| --- | --- | --- |
| Deferred GBuffer 槽位不足 | 无法同时保存 inner sample、frame、mask 和 profile | 已冻结 9-output Eye GBuffer V1；未来扩展必须 bump codec/version，不能静默复用普通语义 |
| ForwardOpaque 扩展影响通用路由 | Opaque material 重复绘制或透明排序混入 Eye | 先用最小 probe 验证 pass；路由失败回滚 E1，不影响 Geometry |
| Eye frame 与资产不一致 | 旋转、LOD 或左右眼出现 iris 漂移 | 加载期拒绝错误 origin/axis/scale；不在 shader 内猜测 |
| LUT 参数维度不足 | pupil/angle sweep 出现明显聚焦误差 | bump LUT version，增加 slice；不复用旧 profile identity |
| LUT 内存过大 | array layer、分辨率或 profile 数超预算 | Phase E0 降维/限制 profile 数，保持 Compute-only 不变 |
| Caustic 变成任意加亮 | shadow 中仍存在或白炉增能 | 关闭 Caustic 即回到 E4；修正 normalization/path binding |
| SSS 泄漏到角膜/眼睑 | 高光被模糊、边界泛白 | `sssSource` + Eye rejection 只接收 tissue diffuse；真实 tissue transport 保持计划外 |
| 热重载只换 pipeline 未换纹理 | shader 修改后 LUT 外观不变或 descriptor 指向旧资源 | 使用 generated-resource participant；禁止复用普通 participant 假装完成 |
| Profile/LOD 失配 | 远近切换 iris depth 或 highlight 突变 | 将 LOD/profile 绑定设为资产合同并在 loader 拒绝 |
| 单壳、双壳和 Deferred 语义漂移 | 同一 MI 参数在不同路径产生不同含义 | 共享 `EyeMaterialInputs`、GBuffer codec、path ledger 与 runtime 对照矩阵 |

推荐回滚顺序：

```text
E7 Deferred / Dual-shell
    -> E6 SSS / Inner IBL
    -> E5 Caustic
    -> E4 Forward Eye without Caustic
    -> E3 Eye intersection probe
    -> E1 ForwardOpaque infrastructure
    -> existing renderer
```

实际执行中每个阶段都保留了上一个阶段的可运行路径；回滚不得删除稳定合同或生成缓存。

## 13. 实际执行顺序与验证边界

本计划按以下独立变更批次完成，避免同时改变 RenderMode、数学、资源、GBuffer 和 SSS：

1. **E0 合同**：文档、方向、单位、profile schema、Debug 编号和验证资产设计。
2. **E1 ForwardOpaque**：RenderMode、RenderGraph、pipeline、draw routing 和 probe。
3. **E2 Compute Resource**：Eye asset、resource set、Compute baker、digest、GPU readback。
4. **E3 Material/Intersection**：M_eye、EyeMaterialInputs、eye frame、Snell、ray-plane、mask。
5. **E4 Forward MVP**：cornea、inner、`T_i/T_o`、IBL 和白炉验证。
6. **E5 Caustic/Visibility**：LUT sample、shadow、contact 和 normalization。
7. **E6 SSS/IBL**：sclera profile、多 pass composition、directional inner IBL（如需要）。
8. **E7 Fallback/LOD**：Deferred packing、Substrate adapter、双壳和 LOD。
9. **E8 Acceptance**：Debug、热重载、运行时矩阵、性能和稳定合同迁移。

每批变更完成后至少执行：

```powershell
cmake --build build -j
ctest --test-dir build --output-on-failure
```

最终验证入口：

```powershell
build/bin/eye_tests.exe
build/bin/eye_authoring_adapter_tests.exe
build/bin/main.exe --eye-validation-test --exit-after-tests
build/bin/main.exe --eye-performance-test --exit-after-tests
build/bin/main.exe --eye-compute-reload-test --exit-after-tests
```

涉及 shader cache、reflection、Compute artifact、runtime validation 或 `shader/spv/` 的测试必须
串行执行。不得通过删除 `shader/spv/` 或调用全局 `device.waitIdle()` 绕过失败。

## 14. 完成判定

### 14.1 当前计划判定（已满足）

- [x] `documents/rendering/eye-shading-model.md` 成为当前稳定合同；
- [x] Legacy/Substrate 语义在 runtime 收敛到同一个 `EyeMaterialInputs` 和 evaluator；
- [x] ForwardOpaque 单壳路径闭合 cornea、refraction、inner、shadow 和 IBL；
- [x] 生产 Eye LUT 只由 Compute Shader 生成；
- [x] Caustic LUT 的版本、单位、参数域、通道和 source/artifact identity 已冻结；
- [x] profile、geometry、UV、scale、pupil 和 limbus 合同已冻结；
- [x] Debug View 42–63 可逐项显示 path ledger 中间量；
- [x] `M_` schema、MI override、generated include、reflection 和 descriptor 已通过 Eye runtime validation；
- [x] 同 digest World reload 复用 LUT texture identity；
- [x] 缺失 profile 的失败 candidate 不改变 active World；
- [x] Eye GBuffer V1、Deferred evaluator 与 `sssSource` fallback 已闭合；
- [x] `forwardEyeInner` / `forwardEyeCornea` 双壳路径与 descriptor layout 已闭合；
- [x] Legacy/Substrate-style authoring adapter、LOD/profile、gaze、pupil dilation 和 UV handedness 已验证；
- [x] GPU LUT readback、固定帧性能预算与 Eye Compute reload 独立矩阵已通过；
- [x] shader cache、World/Graph transaction 和 GPU epoch ownership 走现有事务边界；
- [x] `eye_tests.exe`、authoring adapter tests、CTest 和三条 Eye runtime tests 可重复通过。

### 14.2 计划外扩展项

以下能力不属于本轮计划，不作为未完成判定：

- 完整 path-traced eye、房水体积和高阶角膜内反射；
- 真实 screen-space tissue transport 或完整 Sclera BSSRDF；
- 基于真实眼睑/睫毛几何的 contact visibility 求解；
- UE 私有 shader/节点逐行 parity 与完整生产资产迁移覆盖率；
- 目标硬件上的正式 GPU timestamp/counter 基线。

因此，当前正确的产品描述是 **“VulkanLearn Eye 当前工程范围已完成：Forward、Deferred fallback、双壳、局部 SSS 与 Compute LUT/reload”**；它不等于完整 UE 私有实现或物理级数字人眼球。
