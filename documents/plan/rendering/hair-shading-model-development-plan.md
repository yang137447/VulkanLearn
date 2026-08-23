# Hair Shading Model 执行方案

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 状态 | H0–H7 实现与合同测试已落地；运行时验收待正式 authoring LUT |
| 目标 | 将知识库中的 Hair Reference、UE 5.8 parity 和 VulkanLearn MVP 分阶段落地 |
| 知识库基线 | `D:\YYBWorkSpace\GitHub\yyb-knowledge-book\src\content\docs\rendering\materials\shading-models\hair.mdx` |
| 当前仓库基线 | Hair ID 7 已接入完整 VulkanLearn MVP 路径，稳定合同见 `documents/rendering/hair-shading-model.md` |
| 计划建立日期 | 2026-08-22 |
| 文档职责 | H0–H7 执行记录、阶段门和验收证据；稳定字段以 `documents/rendering/` 合同为准 |
| 最终验收 | 合同测试可重复通过；运行时验收须显式执行 `-RunHairValidation`，并要求 `<resourcePath>/hair/hairAzimuthalLut.json` 正式作者 metadata；当前工作区缺少该前置资产，不能宣称 runtime passed |

H0–H7 的实现、稳定字段、单位、编码、资源版本和显式 fallback 已迁移到
`documents/rendering/hair-shading-model.md`。合同测试与静态边界审计已落地；运行时验收仍受
正式 authoring LUT metadata 前置条件约束。本文保留阶段路线、阶段门、验收证据和已知生产边界，
不把透明排序、完整 TT/TRT IBL、生产级 MS LUT、OIT 或 strands backend 误写成已承诺能力。

## 1. 目标与边界

### 1.1 总目标

在不把 Kajiya-Kay、各向异性 GGX 或普通 DefaultLit 高光冒充 Hair 的前提下，建立一条
可验证、可回滚、与现有 Material/RenderGraph/Shader Cache 合同兼容的 Hair 路径：

```text
Hair Material Inputs
    -> 稳定 tangent frame 与角度约定
    -> R / TT / TRT direct single scattering
    -> coverage / shadow visibility
    -> Forward 与 Deferred 共用 Hair evaluator
    -> 独立 Hair IBL / multiple-scattering fallback
```

### 1.2 第一里程碑

第一里程碑只要求完成 **Hair Card + Forward + R/TT/TRT direct single scattering**：

- `Tangent`、`Roughness`、`Specular`、`Scatter`、`Backlit` 能分别产生可解释响应；
- R、TT、TRT 保持独立路径身份，R 不被错误染成头发颜色；
- 使用版本化 azimuthal/path LUT，不在每像素求全部方位根；
- 现有 shader 反射、descriptor、热重载和 GPU epoch retirement 仍然有效；
- 透明路径的排序和普通 Shadow Map 限制被明确标记为 MVP 限制，而不是宣称生产完成。

### 1.3 最终范围

最终计划覆盖：

1. UE-facing Hair 参数和 Material authoring 合同；
2. R / TT / TRT Reference 与实时 closure；
3. Hair Card 的 coverage、masked/dither、alpha-to-coverage 和 shadow；
4. Forward/Deferred 共用 evaluator；
5. 分路 Hair IBL；
6. 受能量预算约束的 multiple scattering；
7. 后续 strands/curves visibility 接口；
8. Debug View、CPU reference、资产校验和运行时验证场景。

### 1.4 非目标

本计划不包含：

- 直接复制 UE 私有 shader 常数或声称逐行 parity；
- 第一轮就实现完整 path-traced fiber renderer；
- 第一轮就实现深度不透明图、完整 OIT 或 ray-traced any-hit；
- 把 Hair Card 的 coverage alpha 当作 Beer-Lambert absorption；
- 通过每帧 `clamp`、无条件环境补光或 `device.waitIdle()` 掩盖数据、能量或生命周期问题；
- 直接编辑 `shader/spv/` 作为实现手段。

## 2. 当前基线与缺口

| 领域 | 当前情况 | 计划影响 |
| --- | --- | --- |
| Shading Model 注册 | `Hair` 已映射到 ID 7，GLSL 已有 `SHADING_MODEL_HAIR` | 保持 ID 不变，只追加实现 |
| Material Inputs | 有公共 `baseColor/specular/roughness/tangent/AO/emissive/PDO`，没有 Hair 专用 struct | 阶段 3 增加明确的 `HairMaterialInputs` |
| Material Surface | 有 `customData`、`worldTangent`、`anisotropy` | 先用于 Forward 内部语义，Deferred 编码在阶段 4 冻结 |
| Forward 分发 | `Unlit/ClearCoat/ThinTranslucent` 有独立路径，其余回退 DefaultLit | 阶段 3 增加 Hair 分支 |
| Deferred 分发 | 已有多种模型分支，Hair 未接入 | 阶段 4 使用同一 evaluator 接入 |
| LUT 资源 | 有 BRDF、Subsurface、Skin LUT 经验，但没有 Hair LUT | 阶段 2 建立独立 Hair 资产和版本校验 |
| RenderGraph | `forwardTransparent` 有 shadowMap 输入，Deferred 有独立输入集合 | LUT 采用 pass-level 资源，避免污染所有 Material Set |
| 透明材质 | `TransparentAlphaBlend` 进入 Forward，默认不投普通 Shadow Map | 阶段 3 仅作探针；阶段 5 再做 masked production path |
| ShadowDepth | 有公共/自动生成和显式 override 路由 | Hair coverage 必须与主 Pass 共用 |
| GBuffer | `gbufferF` 当前保存 tangent xyz 和 anisotropy | Hair 需要明确 tangent handedness 语义，禁止无记录覆盖 |
| Shader 工程 | 使用 BLAKE3-256 cache、批量发布和热重载 | 新增 include、M_、LUT binding 必须走现有合同 |
| 测试 | 有 Subsurface、ThinTranslucent、Shader Build 和 Runtime tests | 增加 CPU 数学、资产 round-trip 和运行时 Hair 验证 |

## 3. 不可变执行原则

1. **路径身份优先**：R、TT、TRT 在 Reference、LUT、Shader evaluator 和 Debug View 中使用同一命名。
2. **单一颜色转换点**：`BaseColor` 只能在导入阶段生成 absorption/path tint，或在 evaluator 中作为一次性 path tint；两者不得同时发生。
3. **Tangent 先于高光**：先建立稳定的 `(n, b, t)` frame，再计算 `theta_i/theta_o/theta_h/theta_d/deltaPhi`。
4. **Coverage 与吸收分离**：coverage 决定像素/发束可见性；absorption 决定命中纤维后的内部路径颜色。
5. **Forward 先行，Deferred 复用**：Hair evaluator 不复制两套；Forward/Deferred 只负责输入资源和输出合同。
6. **LUT 有身份**：LUT 必须记录 schema、kernel、坐标、IOR、roughness mapping、单位和 path convention 版本。
7. **显式 fallback**：缺失 Hair IBL、multiple scattering 或 strands visibility 时，输出可识别的 fallback 状态，不静默伪装成完整实现。
8. **保持现有线程/生命周期边界**：编译 worker 不接触 Vulkan 或 live Material；候选资源通过现有事务和 GPU epoch retirement 发布。
9. **小步可回滚**：每阶段都能单独编译、测试和回退；不跨阶段混入无关重构。
10. **预计算统一走 Compute Shader**：Hair LUT、IBL basis、multiple-scattering lookup 和其他生产预积分数据都由 Compute Shader 生成；CPU 只负责冻结输入快照、创建/调度资源、同步、读回校验和元数据提交，不提供 CPU texel 生成 fallback。

## 4. 目标数据流

```text
M_hair.json + MI_hair*.json
    -> MaterialInstanceResolver / MaterialAssetValidator
    -> generated material parameter include
    -> EvaluateMaterialInputs
        common: baseColor/specular/roughness/tangent/AO/emissive/PDO
        hair: scatter/backlit/shift/roughness/absorption/MS policy
    -> ResolveMaterialSurface
    -> HairLightingEvaluator
        frame -> angles -> longitudinal M_p -> azimuthal LUT N_p
        -> Fresnel/path absorption -> R + TT + TRT
        -> light visibility / coverage
    -> Forward output or GBuffer encode
    -> Deferred decode -> same HairLightingEvaluator
```

推荐的第一版 Hair 专用输入如下。公共输入继续使用现有字段，不重复定义：

| 输入 | 第一版语义 | 归属 |
| --- | --- | --- |
| `scatter` | 发束 through-scatter / inter-fiber scatter 控制 | Hair model |
| `backlit` | TT/back-light 方向项控制 | Hair model |
| `cuticleTilt` | R/TT/TRT longitudinal shift 基础量 | Hair model |
| `longitudinalRoughness` | `M_p` 峰宽 | Hair model |
| `azimuthalRoughness` | LUT 方位平滑参数 | Hair model |
| `ior` | 纤维折射率；MVP 可固定默认值 | Hair model |
| `absorption` | TT/TRT 路径吸收；与 `baseColor` 只能二选一作为颜色来源 | Hair model |
| `fiberRadius` | 路径长度单位换算 | Hair model |
| `multipleScatteringWeight` | 后续 MS 补偿预算 | Hair model |
| `coverage` | Hair Card 覆盖率 | Mesh Pass / visibility |
| `tangent.w` | rootward/handedness 约定 | Geometry / Hair frame |

第一版不把所有参数都塞进 GBuffer。Forward MVP 可以直接消费完整的 Hair inputs；Deferred
阶段只承诺经过明确 packing 的子集，额外 IOR、melanin ratio、独立 path weights 和 LUT ID
需要升级 GBuffer/material contract 时再开放。

## 5. 分阶段执行计划

### Phase 0：合同冻结与验收基线

**目标**：把知识库中的物理、UE-facing 和 VulkanLearn 语义变成可实现的仓库合同。

**任务**

- [x] 新增 `documents/rendering/hair-shading-model.md`，记录已冻结的字段、单位、方向、路径和 fallback。
- [x] 在本计划中维护未决事项，不把未验证内容写进当前合同。
- [x] 冻结 tangent rootward/tipward 方向、`tangent.w` handedness、mirrored UV 和 flow map 规则。
- [x] 冻结角度零点、`theta_h`、`theta_d`、`deltaPhi`、LUT wrap 和坐标方向。
- [x] 冻结 `BaseColor` 到 absorption/path tint 的唯一转换位置。
- [x] 冻结 `Scatter`、`Backlit`、`Specular`、`Roughness` 的作用域，禁止互相代替。
- [x] 冻结 Hair Card 的 `opacityMask/coverage` 语义，以及主 Pass 与 ShadowDepth 的一致性要求。
- [x] 建立一份参数 sweep 表，定义每个输入的预期可见响应和禁止影响的路径。

**输出**

- `documents/rendering/hair-shading-model.md`
- Hair 参数、单位和 GBuffer/Forward 边界表
- tangent/frame 与 LUT convention 决策记录
- 第一版 debug mode 编号草案

**阶段门**

- 未解决 `BaseColor` 双重染色、tangent 符号和 coverage/absorption 边界前，不进入 Shader 实现。

### Phase 1：CPU Reference 与能量验证

**目标**：在不依赖 Vulkan 的情况下验证 R/TT/TRT 的路径账本，为 Compute Shader baker 和 shader 提供唯一参考；本阶段的 CPU 代码是 oracle，不是生产预计算器。

**任务**

- [x] 新增 `tool/hair-tests/support/` 下的纯数学 reference 模块，避免依赖 Renderer、Material cache 或 Vulkan 对象；生产 `source/render/hair/` 不承载测试 oracle。
- [x] 实现纵向项 `M_p`：R、TT、TRT 的 shift、roughness 和归一化约定。
- [x] 实现方位映射 `Phi_p(h)`、全部有效 roots 和 `1 / abs(dPhi/dh)` Jacobian。
- [x] 实现 Fresnel、Snell、各路径界面权重和 Beer-Lambert absorption。
- [x] 明确 R 接近无色、TT/TRT 随路径长度和 absorption 衰减/染色的测试断言。
- [x] 增加白炉、roughness sweep、IOR sweep、fiber radius sweep 和 absorption sweep。
- [x] 通过固定输入断言和 LUT 坐标 golden contract 固定 CPU oracle 与运行时采样边界；
      不保留没有跨工具消费者的 JSON 样本序列化层。
- [x] 新增 `tool/hair-tests/`，并在根 `CMakeLists.txt` 的测试开关下注册；测试必须保持串行，不与共享 `shader/spv/` 的验证并发。

**建议测试项**

| 测试 | 验证内容 |
| --- | --- |
| `TestHairAngleConvention` | frame、角度零点和 tangent 翻转规则 |
| `TestHairAzimuthalRoots` | 多根、焦散邻域和 Jacobian |
| `TestHairPathWeights` | R/TT/TRT Fresnel 与吸收路径权重 |
| `TestHairWhiteFurnace` | roughness 改变峰宽但不无故增加总能量 |
| `TestHairColorSeparation` | R 不重复乘发色，TT/TRT 正确染色 |
| `TestHairParameterIsolation` | UE-facing 参数只影响约定的响应 |

**阶段门**

- CPU reference 在固定输入下可重复输出；
- 所有路径都能单独导出并比较；
- 白炉和参数隔离测试通过后，才允许提交 Compute Shader baker 生成生产 LUT。

### Phase 2：版本化 Hair LUT 与资源链路

**目标**：通过 Compute Shader 把高成本的方位求根、焦散平滑和路径权重预积分为带身份的资源。

**任务**

- [x] 设计 `hairAzimuthalLut` 资产格式，沿用现有 Subsurface/Skin LUT 的 schema/version 校验习惯。
- [x] 至少记录 `schemaVersion`、`lutVersion`、尺寸、通道、IOR、roughness mapping、单位、角度坐标、wrap 和 path convention。
- [x] 新增 Compute Shader baker，推荐入口为 `shader/glsl/generator/hairAzimuthalLut.comp`；CPU host 只构造 frozen input snapshot 和 dispatch 参数，不计算生产 texel。
- [x] 在 `source/render/hair/hairLutBaker.*` 中实现 GPU 资源创建、Compute dispatch、GPU barrier、可选 readback 和结果提交；禁止增加 CPU 生成 fallback，authoring metadata 缺失在 loader 阶段失败。
- [x] Compute Shader 生成结果写入 `config/config.json -> resourcePath -> generated/`，不写仓库根目录 `resources/`。
- [x] 记录生成输入的稳定 source identity，避免只依赖时间戳或文件名。
- [x] 设计 LUT 缺失、版本不匹配、尺寸错误、坐标 convention 不一致时的启动/加载失败信息。
- [x] 选择 pass-level descriptor 绑定，优先放在 Hair 实际使用的 Forward/Deferred pass 输入集合；不要把 Hair LUT 偷塞进所有材质的公共 Set 0。
- [x] 更新对应 RenderGraph 输入、pass shader binding、descriptor schema 和资源 loader。
- [x] 为 LUT 增加 CPU reference 采样、Compute Shader 输出和 GLSL 运行时采样之间的坐标 round-trip/golden test。

**推荐第一版 LUT 边界**

```text
path layers: R / TT / TRT
coordinates: theta_i, theta_o, delta_phi, roughness
fixed policy: IOR and unit convention recorded in metadata
generation: Compute Shader only; CPU is orchestration/validation only
runtime: filtered lookup, no per-pixel root solving
fallback: explicit Kajiya-Kay or disabled-path debug state, never silent mismatch
```

**阶段门**

- LUT metadata 能够独立判断 shader 是否兼容；
- Compute Shader baker、loader、descriptor 和 shader 对坐标约定一致；
- 生成前后的 GPU barrier、readback 和资源提交顺序可验证；
- 任一版本/尺寸错误都在资产阶段失败，而不是在每像素修补。

### Phase 3：Material Inputs 与 Forward Hair MVP

**目标**：在现有 Material Evaluation 和 Forward pass 体系中接入可运行的 Hair evaluator。

**任务**

- [x] 在 `shader/glsl/engine/materialInputs.glsl` 增加 `HairMaterialInputs`，由模型专用 struct 持有 Hair 字段。
- [x] 在 `shader/glsl/engine/materialSurface.glsl` 完成 Hair 输入解析；不要让 lighting pass 直接读取母材质私有参数。
- [x] 新增 `shader/glsl/engine/hairScattering.glsl`，统一提供 frame、角度、longitudinal、azimuthal、Fresnel 和 absorption helper。
- [x] 新增 `shader/glsl/engine/hairLighting.glsl` 或等价命名的独立 evaluator，明确输出 R/TT/TRT、direct single scattering 和路径调试值。
- [x] 在 `shader/glsl/engine/forwardLighting.glsl` 增加 `SHADING_MODEL_HAIR` 分支；禁止继续落入 `ShadeDefaultLitForwardSurface`。
- [x] 让 directional/point/spot 光源各自使用自己的 attenuation 和 visibility；第一版可以只承诺已有 CSM 能力覆盖 directional shadow。
- [x] 使用现有 `forwardTransparent` 作为第一张 Hair Card 探针，先验证 Forward 输出和参数响应。
- [x] 新增 `shader/glsl/M_hair.json`、`M_hair.vertex.glsl`、`M_hair.surface.glsl` 和最小 Hair MI 资产。
- [x] 在 material validator、parameter include generator、descriptor reflection 失败路径中补齐 Hair 参数。
- [x] 不把 `TransparentAlphaBlend` 探针当成最终 Hair Card 生产路径；记录排序、远景 coverage 和 shadow 限制。

**第一版 evaluator 约束**

```text
S_single = w_R * S_R + w_TT * S_TT + w_TRT * S_TRT
R        = interface response, near-uncolored
TT/TRT   = path absorption applied exactly once
Scatter  = versioned through-scatter control
Backlit  = TT/back-light direction term only
AO       = indirect lighting modulation only
Emissive = independent additive output
```

**阶段门**

- Hair Forward 材质不再显示为 DefaultLit；
- primary/secondary highlight、TT/backlit body response 和 tangent 旋转可独立观察；
- shader reflection、descriptor layout 和 hot reload 通过现有验证；
- Direct Light 结果能输出路径分解 Debug View。

### Phase 4：Deferred/GBuffer Hair Contract

**目标**：让 `OpaqueClip`/masked Hair Card 与 Forward 共用 evaluator，同时不破坏已有模型的 GBuffer 语义。

**任务**

- [x] 先写 Hair GBuffer encode/decode round-trip 测试，再修改 `shader/glsl/engine/gbufferCodec.glsl`。
- [x] 第一版 Hair packing 固定为：`customData.r = scatter`、`.g = backlit`、`.b = cuticleTilt`、`.a = multipleScatteringWeight`。
- [x] Hair 的 `gbufferF.rgb` 保存 tangent direction，`gbufferF.a` 保存 `tangent.w` handedness；非 Hair 保持现有 anisotropy 语义。
- [x] 在 `shadingModel.glsl` 注明 GBufferF.a 按 ShadingModel 解释，避免后续误读为通用 anisotropy。
- [x] 由 common `roughness` 经过一个版本化 mapping 同时得到 longitudinal/azimuthal roughness；未升级 GBuffer 前不开放独立 Deferred 参数。
- [x] 按 Phase 0 的唯一 policy 将 `baseColor` 解释为 absorption 或 path tint，不在 Deferred 再次染色。
- [x] 在 `shader/glsl/engine/deferredLighting.glsl` 增加 Hair 分支，并调用与 Forward 相同的 evaluator。
- [x] 为 Deferred pass 接入 Hair LUT 输入，更新 `config/renderGraphConfig.json`、pass shader 和 descriptor 合同。
- [x] 验证已有 ClearCoat、Subsurface、Skin、ThinTranslucent 等模型的 encode/decode 和光照结果不变。

**Deferred 版本边界**

第一版 Deferred 只承诺上述 packing、固定 IOR/default radius 和统一 roughness mapping。
如果需要独立 `ior`、melanin、fiber radius、path weights 或 LUT ID，必须提交新的 GBuffer/material contract
变更，不得复用同一通道的不同含义。

**阶段门**

- Hair GBuffer encode/decode bitwise/数值 round-trip 通过；
- Forward 与 Deferred 在同一输入下的路径分解和曝光范围可比较；
- 非 Hair 模型的 GBuffer 和 debug view 无回归；
- `OpaqueClip` Hair Card 可以进入主路径而不依赖 DefaultLit fallback。

### Phase 5：Coverage、Masked Card 与 Shadow

**目标**：把 Hair 的几何可见性和投影行为从 shading formula 中独立出来，形成可用的 Card 路径。

**任务**

- [x] 明确 `opacityMask`、`surfaceCoverage` 和最终 coverage 的计算顺序；主 Pass 与 ShadowDepth 必须共享同一输入。
- [x] 先实现 `OpaqueClip` + alpha clip 的稳定路径，再加入 dither/TAA 和 coverage-preserving mip。
- [x] 在 `source/render/shadow/materialShadowCasterPolicy.cpp` 和相关 builder 中确认 Hair Card 的 ShadowDepth 路由。
- [x] 让自动 ShadowDepth 复用 Hair Material Evaluation；不得为 Shadow 单独复制一套 alpha/flow/tangent 逻辑。
- [x] 为透明 Hair 探针保留“默认不投普通 Shadow Map”的行为，除非显式 shadow override 合同已定义。
- [x] 增加 alpha-to-coverage 试验开关，只有在 MSAA sample mask 和 temporal resolve 验证后才纳入生产选项。
- [x] 记录 premultiplied alpha、sorting 和 weighted OIT 的适用边界；第一版不以 OIT 取代正确的 coverage mip。
- [x] 分别验证近景边缘、远景 mip、edge-on silhouette、重叠卡片和 dense hair sweep。

**阶段门**

- 主渲染和 ShadowDepth 的 coverage 一致；
- 发量随 mip 变化可解释，不出现远景蒸发或阴影变黑片；
- 阴影只影响对应 light path，不因 MS/Scatter 无条件自发亮；
- masked、alpha blend、alpha-to-coverage 的限制在材质资产和 Debug View 中可见。

### Phase 6：Hair IBL、Multiple Scattering 与 Strands 接口

**目标**：补齐发束低频能量和环境响应，同时保持 R/TT/TRT 的路径身份。

**任务**

- [x] 为 `L_IBL,R`、`L_IBL,TT/TRT`、`L_IBL,MS` 建立独立函数接口和 Debug View。
- [x] 第一版只实现 R 的低阶方向 basis；TT/TRT 和 MS 缺失时使用显式 fallback 标记。
- [x] 任何预积分环境 basis、Hair IBL LUT 或 MS compensation table 都必须由 Compute Shader 生成；CPU 只提交参数快照、调度并校验结果。
- [x] 禁止同一个 GGX reflection vector/mip 同时代表 R、TT、TRT IBL。
- [x] 设计受剩余单次散射能量、coverage、density/opacity 约束的 MS closure。
- [x] 对 sparse-to-dense hair sweep 做暗部、逆光、白炉和 shadow 对照，防止无条件环境补光。
- [x] 预留 strands/curves visibility 输入：fiber visibility、self-shadow、transmittance、LOD/CLOD 不另起 Shading Model。
- [x] 最后再增加 Kajiya-Kay fallback，并在命名、debug 和文档中明确其非 Reference/非 UE parity 身份。

**阶段门**

- Hair IBL 分路可单独开关和比较；
- MS 增加不会突破能量预算或在阴影中自发亮；
- cards 与未来 strands 使用同一 Hair evaluator 接口。

### Phase 7：UE Parity、运行时样例与收敛

**目标**：用固定场景和参数 sweep 证明输入语义、几何表示和渲染路径边界没有混淆。

**任务**

- [x] 增加固定 Hair Card 参考场景：单卡、交叉卡、密集发束、背光和阴影五组构型。
- [x] 固定相机、Pose、灯光、环境、分辨率、曝光和后处理，避免只比较 tone-mapped 最终截图。
- [x] 增加 `WorldTangent`、`TangentRootward`、`ThetaI/O`、`DeltaPhi`、`R/TT/TRT`、`PathLength`、`Absorption`、`Coverage`、`ShadowTransmittance`、`LUTCoordinates` debug views。
- [x] 增加 `UEPrimaryHighlight`、`UESecondaryHighlight`、`Scatter`、`Backlit`、`RPathColor`、`TTPathColor`、`TRTPathColor` debug views。
- [x] 建立参数 round-trip：`Scatter`、`Backlit`、`Specular`、`Roughness`、`Tangent` 逐项 sweep。
- [x] 建立 geometry sweep：mirrored UV、skinning、LOD、flow map、card/strand 对照。
- [x] 将 Hair 运行时验证接入现有 Runtime Validation/UE-lite validation 入口，使用显式 `-RunHairValidation` 开关；入口必须先检查正式 authoring metadata，不自动生成或覆盖作者资产，并保持共享 `shader/spv/` 的测试串行。
- [x] 完成后把已稳定内容迁移到 `documents/rendering/hair-shading-model.md`，并更新 `documents/rendering/shader-structure-and-material-function.md` 的状态表。

## 6. 文件落点矩阵

| 阶段 | 主要文件/目录 | 责任 |
| --- | --- | --- |
| 0 | `documents/rendering/hair-shading-model.md` | 当前 Hair 实现合同 |
| 0 | `source/material/materialAssetTypes.h`、`source/material/validation/` | 资产字段和校验边界 |
| 1 | `tool/hair-tests/support/` | 测试专用 CPU oracle、路径权重和单位转换 |
| 1 | `source/render/hair/` | 生产侧 Hair 资产、convention、LUT 坐标和资源链路；不放测试 oracle |
| 1 | `tool/hair-tests/` | 合同、白炉、GBuffer round-trip 和参数隔离 tests |
| 2 | `shader/glsl/generator/hairAzimuthalLut.comp`、`source/render/hair/hairLutBaker.*` | Compute Shader 预积分、GPU barrier、readback 和资源提交 |
| 2 | `tool/hair-lut-generator/` | Compute Shader dispatch/validation host，不直接生成生产 texel |
| 2 | `schema/`、`<resourcePath>/generated/` | Hair LUT metadata 和生成输出 |
| 2 | `config/renderGraphConfig.json`、对应 pass shader | pass-level LUT descriptor |
| 3 | `shader/glsl/engine/materialInputs.glsl` | Hair MaterialInputs |
| 3 | `shader/glsl/engine/materialSurface.glsl` | MaterialInputs 到 Surface 的一次性解析 |
| 3 | `shader/glsl/engine/hairScattering.glsl`、`hairLighting.glsl` | 共享 Hair evaluator |
| 3 | `shader/glsl/engine/forwardLighting.glsl` | Forward ShadingModel dispatch |
| 3 | `shader/glsl/M_hair*`、Hair `MI_*` | Hair material authoring 和样例 |
| 4 | `shader/glsl/engine/gbufferCodec.glsl` | Hair GBuffer packing/解码 |
| 4 | `shader/glsl/engine/deferredLighting.glsl` | Deferred dispatch，复用 evaluator |
| 5 | `source/render/shadow/`、MeshPass templates | coverage/shadow 路由 |
| 5 | `shader/glsl/engine/passTemplate/shadowDepth.*` | 主 Pass/ShadowDepth coverage 一致性 |
| 6 | `shader/glsl/common/lighting.glsl`、`source/render/environment/` | Hair IBL 和环境资源接口 |
| 7 | `source/engine/testing/`、`tool/ue-lite-final-validation.ps1` | 运行时验证和回归矩阵 |

生成的 SPIR-V、reflection 输出和 LUT 二进制不能作为手工编辑的 source of truth；它们只能由
现有 shader build/cache 和生成工具产生。

## 7. 关键决策与暂定编码

### 7.1 Forward MVP 的材质路线

第一版 `M_hair` 使用现有 `forwardTransparent` 作为工程探针，原因是当前透明材质已经有
明确的 Forward 路由。此阶段只验证 Hair lighting，不把排序、远景发量和透明 Shadow 的缺口
伪装成完成状态。

### 7.2 Masked 生产路线

生产级 Hair Card 目标是 `OpaqueClip`/masked 路径：coverage 由 `opacityMask` 和固定
alpha-clip 合同决定，ShadowDepth 读取同一套材质输入。由于当前 `OpaqueClip` 进入 Geometry
和 Deferred，必须先完成 Phase 4 的 Hair Deferred dispatch，才能把 masked Card 标记为可用。

### 7.3 第一版 GBuffer packing

仅在 Hair ShadingModel 下使用以下语义：

```text
GBufferA.rgb = baseColor / single absorption policy
GBufferC.b   = common roughness -> versioned Hair roughness mapping
GBufferD.r   = scatter
GBufferD.g   = backlit
GBufferD.b   = cuticleTilt
GBufferD.a   = multipleScatteringWeight
GBufferF.rgb = world tangent direction
GBufferF.a   = tangent handedness
```

非 Hair 模型继续使用现有 `GBufferF.a = anisotropy` 语义。独立 `ior`、melanin、fiber
radius、path weights 或 LUT ID 不得在未升级 contract 前偷偷复用这些槽位。

### 7.4 LUT descriptor 位置

推荐把 Hair LUT 作为实际 Hair pass 的 pass-level 输入，而不是 Material Instance texture：

- 所有 Hair 材质共享同一版本化 LUT；
- 不让普通材质的 Set 1 schema 被公共 LUT 污染；
- LUT 版本由 pass/resource loader 校验；
- Forward 和 Deferred 可以使用各自明确的 binding，避免把普通 reflection probe 当 Hair IBL。

如果实现证明现有 RenderGraph 不能表达该资源，应先扩展 RenderGraph resource contract，再
退回 Material texture；不能通过隐藏的全局 sampler 或未反射的 binding 绕过 descriptor 校验。

### 7.5 预计算统一 Compute Shader

所有生产预计算遵循同一条 GPU 链路：

```text
CPU frozen input snapshot
    -> Compute pipeline / descriptor package
    -> storage image or SSBO write
    -> GPU pipeline barrier
    -> optional readback for golden validation
    -> metadata + generated artifact commit
    -> runtime sampled resource
```

CPU reference 可以继续执行同一组数学公式，用于 oracle、回归和白炉对照，但不能把 CPU
计算出的 texel 作为生产 LUT、IBL basis 或 MS table。Compute Shader baker 必须遵守现有
GT/worker 边界：worker 只准备源码和参数，真正的 Vulkan 创建、dispatch、barrier、readback
和资源发布在允许访问 Renderer 的阶段完成，并按 GPU epoch retirement 管理旧资源。

## 8. 验收矩阵

### 数学与路径

- [x] 固定 `theta_d` 扫描截面高度 `h`，全部 roots、`Phi_p` 和 Jacobian 连续且可解释。
- [x] 白炉下 roughness 改变峰宽/位置，不无故增加总能量。
- [x] 改变 absorption 和 fiber radius 时，TT/TRT 按路径长度衰减；R 保持近似无色。
- [x] 焦散附近使用 LUT 后无 NaN、Inf 或异常亮点。
- [x] R、TT、TRT 单独关闭/打开时，路径身份和颜色响应保持稳定。

### UE 输入语义

- [x] `Scatter` 只改变 through-scatter/MS 生产函数，不直接改变 coverage。
- [x] `Backlit` 只进入 TT/back-light 分支，不替代 Beer-Lambert transmittance。
- [x] `Specular` 影响界面 lobe 总体强度，不替代 Fresnel 本身。
- [x] `Roughness` 改变 longitudinal/azimuthal lobe 宽度，使用同一 mapping/LUT 约定。
- [x] tangent 旋转和 rootward 翻转会按预期移动高光，不出现 mirrored UV 反向跳变。
- [x] primary highlight 接近光源色，secondary highlight 保留头发/光源混合身份。

### 几何、覆盖率与阴影

- [x] 单卡、交叉卡、密集卡、edge-on 卡在近景和远景下 coverage 可控。
- [x] 主渲染和 ShadowDepth 使用同一 opacity/coverage 输入。
- [x] masked、alpha blend、alpha-to-coverage 的排序和 TAA 限制有明确记录。
- [x] shadow 不把稀疏卡片压成全黑实体，也不因 MS/Scatter 产生自发亮。
- [x] directional、point、spot 的 visibility 只影响对应 light path。

### 工程与回归

- [x] Material definition、MI override、generated include、reflection 和 descriptor schema 一致。
- [x] Hair shader 修改能被 BLAKE3-256 dependency graph 发现并正确热重载。
- [x] 候选 pipeline、Material、LUT descriptor 和旧资源按现有 GPU epoch 规则退休。
- [x] Hair GBuffer round-trip 通过，已有 DefaultLit/ClearCoat/Subsurface/Skin/ThinTranslucent 无回归。
- [x] Hair authoring LUT 缺失时在 loader 阶段失败；generated metadata 只在成功的 World/Graph transaction 发布后写入，不能作为下一次加载输入。
- [x] 运行时测试串行执行，不并发写共享 `shader/spv/`。

## 9. 风险与回滚

| 风险 | 早期信号 | 处理/回滚边界 |
| --- | --- | --- |
| GBuffer 槽位不足 | Deferred 无法同时保留 tangent handedness 和 Hair 参数 | 先退回 Forward MVP；升级 contract 前不开放额外 Deferred 参数 |
| tangent 符号漂移 | mirrored UV、skinning 或 LOD 后高光峰突然翻转 | 锁定 Phase 0 convention，禁止在 evaluator 内部多次翻转 |
| LUT/shader 失配 | 峰位偏移、焦散异常、版本无法识别 | metadata 版本拒绝加载；回退到显式低阶 probe，不读取未知 LUT |
| BaseColor 重复染色 | R 高光明显带发色、白炉能量异常 | 保留单一转换点，回滚 path tint 叠加逻辑 |
| 透明 Shadow 误用 | Hair Card 产生黑片阴影或完全无影 | 透明探针维持默认无普通 shadow，生产 masked 路径单独验收 |
| MS 无约束补光 | 阴影/背光区域自发亮 | 以单次散射剩余能量和 coverage 作为上限，关闭 MS 即可回到单次路径 |
| Shader 热重载破坏 live state | Hair pipeline 替换后 descriptor 或旧资源失效 | 保持候选隔离、批量校验和 GPU epoch retirement，不引入旁路 swap |
| 过早追求完整 strands | Card 基础路径尚未稳定却出现多套 visibility 代码 | strands 延后到 Phase 6，只先冻结 evaluator 输入接口 |

每个阶段必须保留上一个阶段的可运行路径。推荐回滚顺序：

```text
Phase 6 extensions -> Phase 5 visibility -> Phase 4 Deferred
-> Phase 3 Forward Hair -> DefaultLit/现有材质路径
```

## 10. 执行顺序与提交边界

建议按以下独立变更批次执行，避免一个提交同时改变数学、资源、GBuffer 和 Shadow：

1. **H0 合同**：Phase 0 文档、参数表、单位和 convention。
2. **H1 Reference**：CPU oracle、白炉和参数隔离测试，不生成生产 texel。
3. **H2 LUT**：Compute Shader baker、资产 schema、loader、版本校验和 pass binding。
4. **H3 Forward**：Hair MaterialInputs、共享 evaluator、Forward dispatch 和透明探针。
5. **H4 Deferred**：GBuffer packing、decode、Deferred dispatch 和 round-trip tests。
6. **H5 Visibility**：masked/coverage、ShadowDepth、mip、dither/TAA 验证。
7. **H6 Extensions**：Hair IBL、MS、strands interface、Kajiya-Kay fallback。
8. **H7 Acceptance**：固定场景、Debug View、运行时验证和稳定合同迁移。

每批变更完成后至少执行：

```powershell
cmake --build build -j
ctest --test-dir build --output-on-failure
powershell -ExecutionPolicy Bypass -File tool/ue-lite-final-validation.ps1 -RunHairValidation
```

涉及 shader cache、reflection、runtime validation 或 `shader/spv/` 的测试必须串行执行；
Hair runtime validation 还要求 `<resourcePath>/hair/hairAzimuthalLut.json` 已由作者流程提供；
`<resourcePath>/generated/hairAzimuthalLut.json` 不能替代该前置资产。若当前 build 未配置测试目标，
应先按仓库约定重新配置，而不是删除生成产物绕过失败。

## 11. 完成判定

只有同时满足以下条件，才能把 Hair 从“待专项”改为“已实现”：

- [x] `documents/rendering/hair-shading-model.md` 已成为稳定合同；
- [x] Reference、LUT、Forward、Deferred、coverage 和 shadow 的命名/单位/版本一致；
- [x] 所有生产预计算均由 Compute Shader 生成，CPU 只做 orchestration、readback validation 和 metadata commit；
- [x] R/TT/TRT、primary/secondary highlight、Scatter、Backlit 和 tangent sweep 有验证证据；
- [x] Hair Card 近景/远景、masked/透明和 ShadowDepth 行为有固定场景记录；
- [x] IBL/MS 缺失分支被显式标记，不能把 fallback 伪装成完整 parity；
- [x] 既有 Shading Model、shader cache、热重载和 GPU 资源生命周期无回归；
- [ ] 运行时资产校验、Debug View 和串行测试矩阵可重复执行；测试入口和清理边界已落地，但正式 authoring LUT 缺失时必须明确失败，不能用 generated-only metadata 冒充验收证据。

以上条件尚未全部满足。`Hair` ID 7 可标记为 **实现已落地、运行时验收待补齐**；当前实现只覆盖本文与当前合同明确的
R/TT/TRT、Card、Forward/Deferred、coverage、ShadowDepth、R-only IBL、MS/strands 接口和显式 fallback，
不等同于 UE 私有 shader 逐行 parity 或生产级透明/完整 IBL。
