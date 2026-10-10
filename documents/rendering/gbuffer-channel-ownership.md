# GBuffer 附件与通道所有权

## 状态

本文是 VulkanLearn 当前 GBuffer 布局、通道所有权、以及 `ShadingModelID` / flag 位域分配的正式渲染合同。

论文模型先在 VL 离线复刻；runtime 只有被选为核心的模型支持 deferred。本文的通道
分配与编解码检查只约束**进入 deferred 的模型**。其余进入 runtime 的论文模型走
forward，不因有独立 `ShadingModelID` 就占用 GBuffer 通道；材质和绘制路由必须拒绝
误入 deferred，不能依赖 DefaultLit 的静默回退。

它记录的是**已经存在于代码里的分配事实**，不是未来设计：附件清单来自
`config/renderGraphConfig.json`，编解码来自 `shader/glsl/engine/gbufferCodec.glsl`，
ID 与 flag 表来自 `shader/glsl/common/shadingModel.glsl` 与 `source/material/materialAssetUtils.h`。

两个必须写明的状态差异：

- **已实现（有 evaluator 消费）**：`Unlit`(0)、`DefaultLit`(1)。
- **保留但无消费者**：`Subsurface`(2)、`PreintegratedSkin`(3)、`ClearCoat`(4)、`SubsurfaceProfile`(5)、
  `TwoSidedFoliage`(6)、`Hair`(7)、`Cloth`(8)、`Eye`(9) 的编码/解码分支**全部还在**，
  但 2026-09-12 的旧实现清理删掉了它们的 evaluator（见 `plan/rendering/archive/README.md`）。
  这些分支是**有意留存的 UE 对齐接口面**：重建模型时按论文接回自己的求值器，
  **不要**把"分支还在"读成"模型已实现"。

相关文档：

- `plan/rendering/deferred-gbuffer-ue-aligned-plan.html`：本布局的来源计划（UE Legacy 槽位对齐、M_/MI_ 分层）。
- `plan/rendering/shading-model-alignment-plan.md` §0.6 / §3.2：逐模型"按论文重建"的状态与 case 归属。
- `rendering/shader-structure-and-material-function.md`：MaterialInputs / Shading Model 边界的上游规范。

## 1. 硬约束：8 个颜色附件已经占满

`geometry` pass 一次写出 **8 张颜色附件 + 1 张深度**（`config/renderGraphConfig.json:348-405`）：

```text
gbufferA, gbufferB, gbufferC, gbufferD, gbufferE,
gbufferVelocity, gbufferF, sceneColorBase   (8 color attachments)
+ sceneDepth
```

`shader/glsl/engine/passTemplate/base.frag.glsl:57` 明确记录了这条上限：
"Geometry 已占满设备允许的 8 个颜色附件"，正因如此**选中标记复用 `gbufferVelocity.z`**，
而不是新增一张附件（Eye 例外，它的 z/w 有既定语义，见 §4）。

由此得到的分配规则：

- **新增 deferred ShadingModel 不允许新增附件**。其数据必须落进既有 8 张附件的通道语义里，
  并按 §5 的版本/flag 纪律编码。
- 如果某个模型的论文真的需要新附件，那是一次**跨层改动**：graph 资源定义 + pass 输出 +
  材质输出壳（`base.frag.glsl` 的 out 声明）+ 上限说明 + 本文档，缺一项都不算完成。
  ThinTranslucent 的双源 Add/Mul 输出路径就是反例：它的输出壳现在是编译期 `#error`（`base.frag.glsl:26-30`）。

## 2. 附件清单

| 附件 | 格式 | 尺寸 | 写入 pass | 读取 pass |
| --- | --- | --- | --- | --- |
| `gbufferA` | `A2B10G10R10_UNORM_PACK32` | 1x | geometry (`clear`) | deferredLighting |
| `gbufferB` | `R16G16B16A16_SFLOAT` | 1x | geometry (`clear`) | deferredLighting |
| `gbufferC` | `R16G16B16A16_SFLOAT` | 1x | geometry (`clear`) | deferredLighting |
| `gbufferD` | `R16G16B16A16_SFLOAT` | 1x | geometry (`clear`) | deferredLighting |
| `gbufferE` | `R16G16B16A16_SFLOAT` | 1x | geometry (`clear`) | deferredLighting |
| `gbufferVelocity` | `R16G16B16A16_SFLOAT` | 1x | geometry (`clear`) | deferredLighting、toneMapping |
| `gbufferF` | `R16G16B16A16_SFLOAT` | 1x | geometry (`clear`) | deferredLighting |
| `sceneColorBase` | `R16G16B16A16_SFLOAT` | 1x | geometry (`clear`) | deferredLighting |
| `sceneDepth` | `D32_SFLOAT` | 1x | geometry (`clear`) | deferredLighting、forwardOpaque (`load`)、sky、forwardTransparent |
| `sceneColor` | `R16G16B16A16_SFLOAT` | 1x | deferredLighting (`clear`)、forwardOpaque/sky/forwardTransparent (`load`) | bloom、toneMapping |
| `selectionMask` | `R8G8B8A8_UNORM` | 1x | forwardOpaque (`clear`)、forwardTransparent (`load`) | toneMapping |

MSAA：`geometry` / `forwardOpaque` / `forwardTransparent` 都声明 `needMsaa: false`，
因此 GBuffer 族附件是**单采样**的；`config/config.json -> msaa` 只作用于声明了 `needMsaa` 的 pass
（判定点在 `source/renderGraph.cpp:1223`）。给 GBuffer pass 打开 MSAA 会同时改变附件数量与
resolve 行为（`renderGraph.cpp:1411-1473`），属于本文档范围内的高风险改动。

## 3. ID 与 flag 的 4 + 4 bit 账本

`gbufferB.a` 是一个 UE-style packed byte（附件里存 `value/255.0`）：

```text
low  4 bits = ShadingModelID          (DecodeGBufferShadingModel, gbufferCodec.glsl:64)
high 4 bits = SelectiveOutputMask     (DecodeGBufferFlags,        gbufferCodec.glsl:70)
```

### 低 4 bit：ShadingModelID 槽位表

| ID | 名称 | 可 authoring | 实现状态 |
| --- | --- | --- | --- |
| 0 | `Unlit` | ✅ | ✅ 已实现（forward） |
| 1 | `DefaultLit` | ✅ | ✅ 已实现（forward + deferred） |
| 2 | `Subsurface` | ✅ | 分支保留，无 evaluator |
| 3 | `PreintegratedSkin` | ✅ | 分支保留，无 evaluator |
| 4 | `ClearCoat` | ✅ | 分支保留，无 evaluator |
| 5 | `SubsurfaceProfile` | ✅ | 分支保留，无 evaluator |
| 6 | `TwoSidedFoliage` | ✅ | 分支保留，无 evaluator |
| 7 | `Hair` | ✅ | 分支保留，无 evaluator |
| 8 | `Cloth` | ✅ | 分支保留，无 evaluator |
| 9 | `Eye` | ✅ | 分支保留，无 evaluator |
| 10 | `SingleLayerWater` | ❌ 槽位保留 | 未实现 |
| 11 | `ThinTranslucent` | ✅ | 输出壳为编译期 `#error` |
| 12 | `Strata` | ❌ 槽位保留 | 未实现 |
| 13 | `ClothFiber` | ✅（本地扩展） | **2026-09-16 定给 B 臂（Sadeghi microcylinder，生产臂），尚未实现**，见 §7 |
| 14-15 | —— | —— | **空闲** |

规则（`source/material/materialAssetUtils.h:25-40`）：**槽位编号照抄 UE 5.8 的表，不得重排**。
追加到 13-15（13 已定给 `ClothFiber`）属于 VulkanLearn 本地扩展，必须在 `shadingModel.glsl` 与
`materialAssetUtils.h` 两处同时注明来源，并同步 `kShadingModels`、校验器与编辑器枚举。

### 高 4 bit：flag 已用满

| flag | 值 | 含义 | 生产者 | 消费者 |
| --- | --- | --- | --- | --- |
| `GBUFFER_HAS_CUSTOM_DATA_MASK` | `0x10` | `gbufferD` 内容有效 | 原在 `ResolveMaterialSurface` 的 per-model 分发链，**已随旧实现删除**（`materialSurface.glsl:110-114`） | TwoSidedFoliage 的 decode 门控 |
| `GBUFFER_HAS_PRECOMPUTED_SHADOW_MASK` | `0x20` | `gbufferE` 为预计算阴影 | 无 | 无（**声明但从未接线**） |
| `GBUFFER_HAS_VELOCITY_MASK` | `0x40` | `gbufferVelocity` 有效 | ✅ `materialGBufferOutput.glsl:15` 每帧设置 | —— |
| `GBUFFER_HAS_ANISOTROPY_MASK` | `0x80` | `gbufferF.a` 为版本化 anisotropy 打包 | 原在已删除的 Cloth v2 分发链 | 仅 Cloth 的 decode 门控 |

除 `0x40` 外，其余 flag 目前**没有生产者**：重建模型时要在 `ResolveMaterialSurface` 里把
"把模型专用输入编码进 customData 并置 flag"的分发分支接回，decode 侧的门控才有意义。

四个 bit 全部占满。因此：

- **没有空位表达"这条像素属于哪条闭包 / 哪个编码版本"**。这类身份只能走低 4 bit 的 ID，
  或者走 customData 内容——后者会让同一个 ID 出现两套语义，需要 flag 或版本域兜底，
  代价见 §7 的 Cloth 案例。
- 需要新 flag 时必须先写明**回收哪一个**，不能假设高 nibble 还有余量。

## 4. 通道所有权总表（按 ID）

`gbufferCodec.glsl` 的 `EncodeGBuffer`（218-361）与 `DecodeGBufferSurface`（363-532）
必须成对阅读；下表是两者的合并视图，也是"哪个模型拥有哪个通道"的唯一账本。

| ID / 模型 | A | B | C | D (customData) | E | Velocity | F | sceneColorBase |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 `Unlit` | normal, a=0 | metallic, 0.5, roughness | baseColor, AO | 通用 `customData` | `precomputedShadowFactors` | xy=velocity | tangent.xyz, a=anisotropy | emissive, opacity |
| 1 `DefaultLit` | normal, a=0 | metallic, **specular**, roughness | baseColor, AO | 通用 `customData` | `precomputedShadowFactors` | xy=velocity | tangent.xyz, a=anisotropy | emissive, opacity |
| 2 `Subsurface` | normal, a=0 | metallic, 0.5, roughness | baseColor, AO | color.rgb, weight.a | `precomputedShadowFactors` | xy=velocity | wrapWidth, backscatterPower, backscatterWeight, thickness | emissive, **transmissionWeight** |
| 3 `PreintegratedSkin` | normal, a=0 | metallic, specular, roughness | baseColor, AO | skinLutId, thickness, thicknessScale, weight | **characterLighting** | xy=velocity | curvature, transmissionWeight, oct(bottomNormal) | emissive, opacity |
| 4 `ClearCoat` | normal, a=0 | metallic, 0.5, roughness | baseColor, AO | weight, roughness, oct.y, oct.x（**z/w 顺序与 UE Legacy 一致**） | `precomputedShadowFactors` | xy=velocity | tangent.xyz, a=anisotropy | emissive, opacity |
| 5 `SubsurfaceProfile` | normal, a=0 | metallic, 0.5, roughness | baseColor, AO | profileId, weight, thickness, transmissionWeight | `precomputedShadowFactors` | xy=velocity | tangent.xyz, a=anisotropy | emissive, opacity |
| 6 `TwoSidedFoliage` | normal, a=0 | metallic, specular, roughness | baseColor, AO | subsurfaceColor.rgb（decode 需 `0x10` flag，否则回退 0） | `precomputedShadowFactors` | xy=velocity | tangent.xyz, a=anisotropy | emissive, opacity |
| 7 `Hair` | normal, a=0 | hairScatter, specular, roughness | baseColor（decode 成 `sigma_a`） | oct(normal).xy, backlit.z | `precomputedShadowFactors` | xy=velocity | tangent.xyz, **tangent.w = handedness** | emissive, opacity |
| 8 `Cloth` | normal, a=0 | metallic, 0.5, roughness（decode 强制 metallic=0） | baseColor, AO | sheenColor.rgb, sheenRoughness.a | `precomputedShadowFactors` | xy=velocity | tangent.xyz, **a(=.w) = 5+5 bit (anisotropy, cross)** | emissive, opacity |
| 9 `Eye` | cornea normal, a=0 | corneaIor, causticStrength, roughness | irisColor, AO | irisUv.xy, packed profile.z（**独占并版本化**）, irisMask.a | scleraColor.rgb, irisRadius.a | xy=velocity, **z=pupil/iris, w=limbus/iris** | irisNormal.xyz, irisDistance.a | emissive, opacity |

补充事实：

- `gbufferA.a` 是 UE Legacy 的 `PerObjectGBufferData` 槽位，当前无生产者，固定写 0
  （`gbufferCodec.glsl:221-224`）。它**不是空闲通道**：UE 对齐语义要求它留给 per-object data。
- `DefaultLit` 的 `gbufferB.g` 保存作者 `specular`，默认仍为 `0.5`；
  forward/deferred 的直接光和 IBL 均消费它。非金属 `specular=0` 明确关闭镜面
  lobe，供 lookdev 灰板测光使用；不会关闭金属反射。其余旧模型保留 V1 的
  `0.5` 或既有模型专用 B 语义，不新增附件或更改通道布局。
- **ID 2 `Subsurface` 的 opacity 不往返**：decode 把 `surface.opacity` 强制回 1.0
  （`gbufferCodec.glsl:433-435`），它的 `sceneColorBase.a` 已被 `transmissionWeight` 占用，
  不要用 ID 2 验证 alpha。
- `SelectionMask` 与 `gbufferVelocity.z`：geometry pass 对**非 Eye** 像素写
  `velocity.z = selectionData.x`（`base.frag.glsl:57-60`）；forward 路径则写独立的
  `selectionMask` 附件（`base.frag.glsl:33`、`65`）。两条路径的选中语义不同，不要混用。
- `gbufferVelocity.zw` 对普通表面为 0；Eye 把 `pupilRadius/irisRadius` 与
  `limbusWidth/irisRadius` 存在这里。

## 5. 打包与版本纪律

**精度的数值上限不在本文重复**：单源表在
`plan/rendering/shading-model-alignment-plan.md` §1.6.2「GBuffer 往返精度上限」
（法线 10 bit、packed byte 的 fp16 步长、Cloth 5+5 bit、Eye profile、ClearCoat 底层法线偏移等）。
本节只写由那张表推出的**分配规则**：

1. **单通道整数打包上限是 11 bit**（fp16 在 1.0 附近步长 `4.88e-4`，`2^11` 以内的整数逐码可分辨）。
   两个 8 bit 参数**塞不进**一个 `R16F` 通道（`gbufferCodec.glsl:111-112` 记录了这条）；
   Cloth 的 5+5 bit 与 Eye 的 1+8+9 bit 都是照着这个上限设计的。
2. 整数语义写入前归一化（`/255.0`、`/1023.0`），解码端用 `uint(value * 255.0 + 0.5)` 还原；
   **中间不要做浮点运算再还原**。
3. **decode 只允许消费本 ID 的语义。** 同一个附件在不同 ID 下含义不同（D 与 F 尤其明显），
   `gbufferCodec.glsl` 里每个分支都带模型注释，改动时必须同步两侧。
4. **flag 缺失必须回退到中性值**，禁止消费附件里的 stale 内容。已有两处先例：
   TwoSidedFoliage 缺 `0x10` 时 `subsurfaceColor = 0`；
   Cloth 缺 `0x80` 时 `anisotropy = anisotropyCross = 0`（即 v1 各向同性闭包）。
5. **版本化打包必须带版本域**，旧像素要有显式回退分支，而不是让新公式去解释旧数据。
6. **deferred 模型的 encode/decode、两条 dispatch、材质校验同批提交。** Forward-only
   模型不编码 GBuffer，但求值器、forward dispatch、材质校验和禁止 deferred 的路由
   必须同批完成。只有 ID 没有对应路径的 dispatch case 会静默落到 DefaultLit
   （alignment plan 的 `A-07`），画面上可能表现为"看起来还行"。

## 6. 新增 deferred ShadingModel 的分配检查清单

只有离线 case 完成、且模型被明确选作 runtime 核心后，才执行以下 GBuffer 清单。
Forward-only 模型不分配 GBuffer 附件，也不做 GBuffer `customData` 编码；需在材质和绘制
路由阻止 deferred。其 forward 材质参数仍按求值器所需提供；若分配运行时 ID，
仍须同步 `kShadingModels`、`shadingModel.glsl`、材质校验白名单和编辑器枚举，
并补 forward dispatch 与对应 case。不要套用下方仅供 deferred 的通道预算。

1. **先问能不能复用既有通道**：8 张颜色附件已满，按 §7 的可复用通道优先级（本 ID 未用通道 →
   `F.w` → `B.r` → `E`）找位置，而不是先想加附件。
2. **定 ID**：低 4 bit 中 13 已给 `ClothFiber`，14-15 空闲。同步更新 `kShadingModels`、`shadingModel.glsl`、
   材质校验白名单、编辑器枚举；若是本地扩展，写明它不是 UE 5.8 表内的槽位。
3. **定 flag**：需要新 flag 时先说明回收哪一个（高 nibble 已满）。
4. **写进本文档 §4 的表格**，包括缺失 flag / 旧版本时的回退值。
5. **额外 GPU 资源（LUT、预滤波环境等）走 set 3**，且 **binding 顺序必须等于 graph 的
   `input` 顺序**；LUT 生成器、pass 材质 texture 槽、shader sampler 声明要一起加回。
6. **Forward 与 Deferred 两条 dispatch 同时补**，并给 debug view 分配区间；本项仅适用于核心 deferred 模型。
7. **更新本文档与 `documents/README.md`**。

## 7. 通道预算：Cloth 双闭包（2026-09-16 定案）

**定案**：`Cloth` = **8**（A 臂：Estévez 完整模型 —— Charlie 分布 + 式(3) 的 G + 式(4) 软化 + §5 分层，**`F` 取 1**；**对照臂**）；
`ClothFiber` = **13**（B 臂：Sadeghi et al. 2013 microcylinder，**生产臂**，VulkanLearn 本地扩展）。
两臂是两个独立 `ShadingModelID`，不是一个 ID 加内部开关 —— 完整约定见
`plan/rendering/shading-model-alignment-plan.md` §2.3⑤。
以下通道预算是两臂旧的 deferred 方案，**不是自动启用决定**。按离线优先、
仅核心进入 deferred 的新路径规则，落地前要逐臂确认核心资格；forward-only 的臂
不执行本节的 GBuffer 分配或第二趟 geometry pass。

两条闭包都由论文给出，所以先算**每臂必须进 GBuffer 的量**：deferred 只看得到附件，
母材质里的常量也必须编码进来，否则 deferred 与 forward 会给出不同结果。

| 臂 | 公式需要的每像素量 | 已有来源 | 需要新增的通道 |
| --- | --- | --- | --- |
| A 臂 `Cloth` = 8（Estévez 完整模型：式(1)(2)(3) + 式(4) 软化 + §5 分层；**`F` 取 1**，见计划 §2.3⑧） | sheenColor（论文的 `specint`，纯颜色入口，不承担 `F0`）、`r`（感知粗糙度，直接进式(2) 与式(3) 的 `(1−r)²` 插值）、N、`N·V`、base 的 albedo/roughness | `gbufferD` 的 v1 语义正好是 `(sheenColor.rgb, sheenRoughness.a)`；**没有 F0 槽位需求**（不乘 Fresnel） | **0**（deferred 侧另需一张 `E_s` 表：16×16、`(roughness, 入射角)`，纯几何项，不放 GBuffer） |
| B 臂 `ClothFiber` = 13 —— ~~L1 预算（已作废）~~ | ~~thread tangent、`γ_s`、`γ_v`、`kd`、albedo `A`、`η`~~ → **2026-09-17 范围定案改为完整 §5，见下** | —— | —— |

### B 臂完整 §5 的 fabric payload（2026-09-17 新增；运行时 `T=8`，2026-09-22 改为允许新增附件）

论文 §5 的 deferred 端每像素需要三组量：

1. **两族的切线倾角**：tangent curve 给的是"切线相对**表面切线**的偏离角 `α`"（Fig 10：切线偏离
   表面切线 `α` 度，它的局部法线也偏离表面法线 `α` 度）。式(6) 沿 curve 取 `T` 个等距样本 →
   **每族 `T` 个倾角**；族的基方向就是 `gbufferF.xyz`（MikkTSpace 从 UV0 生成，与论文
   "use texture UV coordinates as the direction of microcylinders" 对齐）与 `B = ±(N×T)`。
   论文的曲线数据在 **Appendix A Table 3（p11）**：6 种织物、每族给控制点**偏移角与段长**
   （`n` 个控制点 `n−1` 个段长，曲线闭合、末段不计权），复现论文 case 时直接取用。
2. **每族材质量**：`η`、`A`（有色 albedo）、`kd`、`γ_s`、`γ_v`（论文 Table 2 给的实测值）。
3. **覆盖率** `a1`（无空隙时 `a2 = 1 − a1`）。式(7)(8)(11) 的 masking / reweighting 不需要额外通道——
   它们由倾角与 `N`/`T`/光视方向算出来。

按 §5「每通道 ≤ 11 bit」的规则用 8 bit/量打包：

| 量 | 数量 | 打包后通道 |
| --- | --- | --- |
| 切线倾角（`T` 个 × 2 族） | `T=8` → 16 个 | **8**（每通道两个 8 bit） |
| `A1.rgb` + `A2.rgb` | 6 × 8 bit | **3**（若接受 `A1 = gbufferC.rgb` 可省 3） |
| `η1` `η2` | 2 × 8 bit | **1** |
| `kd1` `kd2` | 2 × 8 bit | **1** |
| `γs1` `γv1` `γs2` `γv2` | 4 × 8 bit（度，0..90 足够） | **2** |
| `a1` | 1 × 8 bit | 与 `kd` 共用 |
| handedness（`B` 的符号，两族参数不同时必须） | 1 bit | 与倾角共用 |
| **合计** | | **`T=8` ≈ 15** |

第一趟能借出的是 `gbufferD`(4) + `gbufferE`(4) + `gbufferF.w`(1) + `gbufferB.rgb`(3) = 12。
`T=8` 比这多 3 个通道，所以还要第二趟的 1 张 payload。借用规则（必须同时登记进 §4 表格）：

- `ClothFiber` **不启用预计算阴影** → `gbufferE` 的 4 个通道让给 fabric payload（遵守 `0x20` 的语义）；
- deferred 端 `metallic` / `specular` / `roughness` 对本 ID 无意义（`B.r` 在 decode 已强制 `metallic = 0`）
  → `B.r` / `B.g` / `B.b` 可借用，但它们是 UE 语义槽，属于**显式借用**而不是重定义。

**倾角数据的来源**：curve 属于**材质**（论文用 texture map 指定 groom 方向，Fig 19 每织物一条 curve），
所以由材质函数计算/采样后输出 `T` 个倾角，deferred 端只做式(6) 的求和。
前向路径要么把倾角塞进 `MaterialInputs`（**材质 ABI 扩展**，要按仓库规矩 bump `abi=` 版本），
要么与 deferred 共用同一套 GBuffer 语义——两侧必须一致，否则同一材质前后向外观不同。

**归一化不在 payload 里**：式(13) 的 masking/reweighting（式 7-12）与分母 `Σ_{k∈C}P(k)` 都由倾角与
`N`/`T`/光视方向在运行时算，**不占通道**；但两条推论要写进合同：① 逐样本权重 `q`（段长或 `σ_t` 高斯）
只定样本形状，正式归一化必须用式(13) 的**跨族**分母，两族的 `T` 必须取同一个值（否则 `Σ_{k∈C}` 没有意义）；
② 间接光是式(13) 同一个核**在宏观上半球 $\Omega^+$ 上**的积分 `E_fabric`，不是逐族表（计划 §2.3⑪⑫、`S-35`）；**直接光仍按全域核求值**。直接光和这张表共用一套 `q`、`T` 和切线。

**2026-09-22**：① 运行时 `T` 定为 8。Table 3 最长是 Satin flat 的 8 个控制点；短族用 `q=0` 补齐。早先写成 4，是为了挤进第一趟的借用槽，分裂高光会被抹平，该决定作废。② 第一趟不加第九张颜色附件。差下的通道走路线 2，这张专属 payload 现在就在范围内，不再等“以后 T 不够”。③ `B.g` / `B.b` 仍按显式借用。④ 前向 `MaterialInputs` 的 ABI 扩展按计划 §2.3⑦ 第 7 条。

### 2026-09-17 决定：先借通道，真新增走"第二趟 geometry pass"

- **路线 1**：借 `B.r` / `B.g` / `B.b` 与 `gbufferE`，加上 `D` 和 `F.w`，共 12 个通道。`ClothFiber` 声明"不启用预计算阴影"。这些槽是显式借用，必须登记在 §4 表格里。12 个通道盖不住 `T=8`。
- **路线 2（ClothFiber 现在要用）**：加第二趟 geometry pass，只画 `ClothFiber` 物体（`depthCompareOp: equal`，深度由第一趟写好），写 **1** 张专属 payload。deferred 同时读两套。代价是 graph 新 pass、按 ShadingModelID 过滤绘制，以及多一次光栅化。这不是第一趟的第 9 个颜色附件。

`T` 与打包版本绑定：改 `T` 必须 bump `ClothFiber` 的 GBuffer 编码版本（`T` 是编译期常量）。

**可复用通道优先级**（新模型缺位置时按此顺序找，仍不够才谈新附件）：

1. 本 ID 自己 D/E/F 里未使用的通道（**按 ID 独立，绝不跨模型共享同一通道语义**）；
2. `gbufferF.w`：普通模型是 anisotropy，Cloth v2 是 5+5 bit 打包值 → 拆成两个 ID 后空出，
   且它是 16F，不打包即可存一个完整标量；
3. `gbufferB.r`（metallic）：Cloth 在 decode 已被强制 `metallic = 0`，对 Cloth 是闲置通道；
   但它同时是 UE 的 metallic 槽，借用必须登记在本表；
4. `gbufferE` 的 4 个通道：当前**对所有模型都写 1.0**（预计算阴影没有生产者），
   Skin 用 E 存 `characterLighting`、Eye 用 E 存 scleraColor/irisRadius。对 Cloth 可用，
   但必须写明"Cloth 不启用预计算阴影"，并遵守 `0x20` 的 flag 语义；
5. `gbufferA.a`：只有 2 bit（`A2B10G10R10`），且是 UE `PerObjectGBufferData` 槽 →
   **不适合放模型数据**；
6. **第一趟不能加第 9 张颜色附件。** 8 张已到设备上限（§1）。要更多通道就另开一趟 geometry pass，
   不能把第九张输出塞进 `base.frag.glsl`。ClothFiber 的 `T=8` 走的就是这条（本节路线 2，1 张专属 payload）。

同时这条预算给出了两个 ID 拆分的第二个理由：**按 ID 独立的 D/F 语义让两臂不需要 flag 或版本域来互相区分**，
`0x80` 与 5+5 bit 打包随之退役（它们当前只有 Cloth v2 使用），Cloth 的 `gbufferF` 回到普通
`tangent.xyz` 语义。若坚持单 ID + 内部开关，两臂就必须在同一组通道上叠加版本域，
正是旧 v1/v2 已经付过一次的代价。

三点在分配之外但要同时记住：

- **两臂不可能等价**：B 没有"各向同性档"——瓣的坐标架固定在 thread tangent 上，把 `γ_s`/`γ_v` 调大
  只是把条纹变宽；况且 A 是 `sin^(1/r)` 幂律分布、B 是 Gaussian 宽度分布，函数族与能量账本都不同。
  `S-31` 判的是"切线是否进入公式 + 差距有多大"，不是 A ≈ B；
- **`anisotropy` / `anisotropyCross` 两个输入退役**（A 臂论文里没有这个量；B 臂的各向异性由 thread
  tangent 表达），当前 `source/materialInstanceValidator.cpp` 仍强制要求这两个参数，按臂重写校验时一并删除；
- **B 臂依赖 mesh 切线**：`gbufferF.xyz` 在此之前对所有模型都不敏感（除 Hair 用 `.w` 存 handedness），
  没有切线的网格上 B 臂会退化成"全场同一方向"。`S-31` 的切线敏感性判定需要带切线的织纹网格。

## 8. 其它待定分配

- **`selectionMask` 的来源**：目前只在 forward 路径产出。如果将来 deferred 路径也需要选中标记，
  必须显式决定通道来源，不要临时占用 `gbufferVelocity.z` 之外的保留通道。
- **`gbufferA.a`（PerObjectGBufferData）**：仍在等待生产者。接 per-object 数据时要确认
  它与 Skin 的 `characterLighting`（走 E）不是同一份数据。

## 9. 相关文件

- `config/renderGraphConfig.json`：资源定义与 pass 输入/输出
- `shader/glsl/common/shadingModel.glsl`：ID 常量与 flag 位定义
- `shader/glsl/engine/gbufferCodec.glsl`：`EncodeGBuffer` / `DecodeGBufferSurface` 与所有打包函数
- `shader/glsl/engine/materialGBufferOutput.glsl`：velocity 与 `0x40` flag 的包装层
- `shader/glsl/engine/materialSurface.glsl`：`MaterialSurface` 字段与 `MATERIAL_SHADING_MODEL`
- `shader/glsl/engine/materialInputs.glsl`：`MaterialModelInputs` 各模型输入结构体
- `shader/glsl/engine/passTemplate/base.frag.glsl`：附件上限、out 声明、Alpha Clip 与选中标记
- `source/material/materialAssetUtils.h`：`ShadingModelID` 名称↔ID↔shader define 表
- `source/materialInstanceValidator.cpp`：按模型的材质参数校验
- `source/renderGraph.cpp`：MSAA/非 MSAA 附件与 resolve 构建
