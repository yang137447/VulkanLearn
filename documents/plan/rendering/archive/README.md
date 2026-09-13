# 归档：旧 shading model 合同与计划（2026-09-12）

这里放的是**被取代的旧实现文档**。归档原因只有一个：这些文档描述的实现在 2026-09-12 的清理中
被整体删除，随后每个 shading model 都**按论文重做**，接口与 GBuffer 继续参考 UE。

**这些文档不再是合同，不要按它们写新代码。** 它们保留的价值是：

- 记录当时实现的参数语义、通道约定与踩过的坑（重做时值得对照，但必须逐条与论文核实）；
- 保留"哪些代码路径曾经存在"的证据链（例如 NeoX 角色材质槽位、Hair Card 宏集、皮肤 LUT 字段）。

## 归档清单

| 归档文件 | 原位置 | 被取代的原因 |
| --- | --- | --- |
| `car-paint-shading-model.md` | `documents/rendering/` | ClearCoat 实现删除，改按论文重做 |
| `cloth-shading-model.md` | `documents/rendering/` | Cloth 实现删除（v2 各向异性含 5 项无出处公式） |
| `eye-shading-model.md` | `documents/rendering/` | Eye 实现删除，改按论文重做 |
| `hair-shading-model.md` | `documents/rendering/` | Hair 实现删除，改按论文重做 |
| `subsurface-shading-models.md` | `documents/rendering/` | Subsurface ×3 实现删除，改按论文重做 |
| `thin-translucent-shading-model.md` | `documents/rendering/` | ThinTranslucent 实现删除，改按论文重做 |
| `two-sided-foliage-shading-model.md` | `documents/rendering/` | TwoSidedFoliage 实现删除，改按论文重做 |
| `neox-character-alignment-contract-v1.md` | `documents/rendering/` | NeoX 角色材质对齐不再作为合同 |
| `neox-skin-effect-alignment-contract-v1.md` | `documents/rendering/` | 同上 |
| `neox-b-f-3725-material-default-audit-v1.md` | `documents/rendering/` | 同上 |
| `*-shading-model-development-plan.md`（cloth / eye / hair / subsurface / two-sided-foliage） | `documents/plan/rendering/` | 对应实现已删，计划作废 |
| `cloth-shading-model-v2-anisotropy-upgrade-handoff.md` | `documents/plan/rendering/` | 同上 |
| `neox-b-f-3725-*`（handoff / hair / skin / baseline） | `documents/plan/rendering/` | NeoX 角色恢复线不再推进 |
| `neox-character-shader-feature-inventory.md` | `documents/plan/rendering/` | 同上 |

## 同期删除的代码与资源（不备份，此处仅记账）

**shader 侧母材质与求值器**（`shader/glsl/`）：

```text
M_carPaint  M_cloth  M_eye  M_eyeCornea  M_eyeDeferred  M_eyeInner  M_hair  M_hairProbe
M_preintegratedSkin  M_subsurface  M_subsurfaceProfile  M_thinTranslucent  M_twoSidedFoliage
engine/{cloth,hair,eye,preintegratedSkin,subsurface,subsurfaceProfile,twoSidedFoliage}Lighting.glsl
engine/eyeGeometry.glsl  engine/hairScattering.glsl 及对应的 generate/M_*Paramter.glsl
materialFunction/mf_*（只服务这些模型的那部分：clothInputs / hairAbsorption / preintegratedSkinInputs /
subsurfaceInputs / thinTranslucentInputs / twoSidedFoliageInputs / alphaRange）
```

**LUT 生成器与 SSS 后处理**：

```text
generator/{clothLookupTables,eyeCausticLut,hairAzimuthalLut,subsurfaceLookupTables}.comp
pass/sss{Composition,Horizontal,Vertical}.{frag,vert}   pass/M_sss*.json   pass/generate/M_sss*Paramter.glsl
```

**render graph 结构收缩**（`config/renderGraphConfig.json`）：

```text
删除资源：diffuseLighting  nonDiffuseLighting  transmissionLighting  sssSource  sssPing  sssPong
删除 pass：sssHorizontal  sssVertical  sssComposition  forwardEyeInner  forwardEyeCornea
删除 pass input：set 3 的 binding 10..13（hairAzimuthalLut / eyeCausticLut /
                clothDirectionalAlbedoLut / clothAnisotropicDirectionalAlbedoLut）
结果：deferredLighting 回到单输出 sceneColor（loadOp=clear），sceneColor 的生产者只剩
      deferredLighting / forwardOpaque / sky / forwardTransparent
```

**C++ 侧**（`source/`）：

```text
render/{cloth,hair,eye,subsurface}/ 整目录；pipeline/{cloth,subsurface}LookupTableGenerator.*
render/eye/eyeLookupTableGenerator.*；共享文件里只服务这些模型的分支（资源缓存注入、compute reload
参与者注册、材质校验分支）；CMake 与逐模型 gtest 工程（tool/{thin-translucent,subsurface,hair,cloth}-tests、
tool/hair-lut-generator、tool/eye-tests、tool/eye-authoring-adapter）

保留：`source/render/foliage/`——它现在只剩 `speedTreeWindSystem.*` / `speedTreeWindTransform.h`，
服务 SpeedTree 风场而不是 TwoSidedFoliage。
```

**边界审计脚本**（`tool/ue-lite-boundary-audit.ps1`）：删掉 4 条 Hair test-oracle / authoring-metadata
规则——它们断言的对象已经不存在（其中一条要求 `tool/hair-tests/CMakeLists.txt` 存在，删掉测试工程后
必然失败）。重建 Hair 时按新实现重写这些边界断言。

**工具脚本**（`tool/neox/`，6 个）：角色导入 / 配置脚本，目标场景 `SC_simple_character` 与
`SC_b_f_3725` 都已不存在，NeoX 角色恢复线不再推进。

### 第二轮：证据截图与过期编译产物（同日稍后）

第一轮只删了代码与场景，现场证据与生成物留在原地；第二轮把它们也清掉：

```text
<resourcePath>/Generated/Screenshots/：65 个文件 / 169.5 MB
  hair_* ×20  foliage_* ×35（含 build/ 子目录）  cloth_* ×3  paper_* ×4
  marble_compare  thin_check  fiber_albedo_preview
  （同时删掉清空后的 Screenshots/build/ 目录）

shader/spv/：已删 pass 与 LUT 生成器的旧编译产物
  generator/{clothLookupTables,eyeCausticLut,hairAzimuthalLut,subsurfaceLookupTables}_comp.{spv,debug}
  pass/{sssComposition,sssHorizontal,sssVertical}/（各 4 个文件）
  注意：shader/spv/shader-build-cache.json **没有手工编辑**（AGENTS.md 禁止），
  其中指向已删 source 的历史条目只能由编译器自己淘汰

<resourcePath>/Common/Profiles/：删空后移除该目录
```

**保留的证据图（不要再删）**：M-07 / M-08 迭代截图（`m07*` / `m08*` / `legend_*` / `header_check` /
`axes_g1` / `plot_*` / `brdf_plot_*` / `uv_probe` / 清理后复验的 `c1..c4`），以及被计划 §1.5.3 引作
工具自测样本的 `plot_cloth_solo.bmp` / `plot_hair_solo.bmp`。

**资源库**（`<resourcePath>/`）：

```text
Maps/（约 868 MB）：SC_hair_showcase_6125  SC_hair_showcase  SC_marble_bust_01  SC_simple_character
                    SC_foliage_potted_plant_02  SC_paper_case_clearcoat_sweep  SC_paper_case_cloth_sheen
                    SC_paper_case_subsurface_models  SC_paper_case_thin_translucent
Common/Materials/Pass/：MI_sss{Composition,Horizontal,Vertical}.json
Common/Profiles/：Eye/  Hair/  SkinLuts/  Subsurface/
Generated/Runtime/：eyeCausticLut.json  hairAzimuthalLut.json
Generated/Import/：SC_marble_bust_01/  simple_character/
```

**保留**：`M_pbr` / `M_unlit` / `M_vertexColor` / `M_measureGrid` / `M_shadow` / `M_speedtree` / `M_brdfPlot`
与四个探针场景（`SM_plot_quad*`）、`SC_sphere_array` 球阵基线、`SC_car_showcase`、`SC_sifi_head`、
`SC_speedtree`、其余普通学习场景。`M_brdfPlot` 依赖的 `common/clothBrdf.glsl`、
`common/hairPathScattering.glsl`、`common/microfacetDistribution.glsl` 属于**测量设施**，同样保留。

**接口面（有意保留，不是漏删）**：`engine/materialInputs.glsl` 的逐模型 struct、
`engine/materialSurface.glsl` 字段、`engine/gbufferCodec.glsl` 的全部逐模型编解码分支、
`ShadingModelID` 表、`engine/materialDebugView.glsl` 的 debug 模式编号与 `SetMaterialDebug*Data()`、
`VL_MATERIAL_OUTPUT_*` 宏。这些是"按论文重做"时的落点，当前**没有任何生产者**：读到它们只说明接口还在，
不说明模型还在（详见 `shading-model-alignment-plan.md` §1.6.1）。

`SC_car_showcase` 的 `MI_car_carpaint` / `MI_car_light_glass` 已改指向 `M_pbr`：ClearCoat 重建之前，
车漆失去清漆层，这是清理的已知代价。

### 第三轮：清理时漏掉的死代码与文档死链（同日再查一遍）

第二轮之后按"文档 / 架构 / 代码"三层各查一遍残留，又清掉这些：

```text
shader/glsl/engine/subsurfaceProfileFilter.glsl（222 行）——只被已删的 pass/sssComposition.frag 使用；
  它的三个函数 CalculateSubsurfaceProfilePixelRadius / FilterSubsurfaceProfile /
  GetNearestSubsurfaceProfileTexel 在仓库其余位置零引用
shader/glsl/engine/virtualLight.glsl（47 行）——只服务已删的 PreintegratedSkin 虚拟补光
shader/glsl/materialFunction/mf_emission.glsl、mf_pearlescentInputs.glsl——NeoX 材质删除后无 include 者
shader/spv/pass/{sssComposition,sssHorizontal,sssVertical,gbufferDebug}_{frag,vert}.{spv,debug}（16 个）
  ——上一轮只删了同名子目录，平铺在 shader/spv/pass/ 下的这批漏了
shader/glsl/pass/generate/M_gbufferDebugParamter.glsl——已删 pass 的残留生成 include
```

**C++ 侧死 pass 类型**（15 个文件）：`RenderMode::ForwardEyeInner` / `ForwardEyeCornea` 与
`RenderGraphPassType` 里同名的 pass 类型、`RecordForwardEye*Pass`、`DrawForwardEye*Scene`、
`SurfaceDrawDomain::{ForwardEyeInner,ForwardEyeCornea}`、材质加载器的 pass 守卫、ShadowCaster 策略行、
材质的 renderMode 词表（9→7）与编辑器下拉项，以及 `shader/glsl/engine/materialPass.glsl` 里那两个
已经不可能被产生的 `RENDER_MODE_FORWARD_EYE_*` 宏引用。**ABI / 变体键未变**（variant key 用 renderMode
名字，不是序号），所以现存的 shader 缓存条目全部继续命中。
唯一顺带的测试改动：`tool/material-instance-editor-tests` 里两条断言把 `ForwardEyeInner` 换成
`ForwardOpaque`——规则 2（非 Eye 不得用 ForwardOpaque）仍在，所以两条断言的意图不变。

**文档侧同步修正**（只改与事实不符的地方，不做措辞润色）：

- `documents/rendering/shader-structure-and-material-function.md`：原先用 NeoX Default/Silk/Pearl/Crystal
  当"当前落地示例"，那些母材质与 MF 早已不存在；改为按现存结构写（`M_pbr`→`mf_pbrInputs`、
  SpeedTree 的顶点嵌套、`M_unlit`→`mf_normal`、Pass 级 `mf_alphaClip`），并注明 NeoX 线已归档；
- `documents/plan/rendering/shading-model-alignment-plan.md`：§1.7.5 场景索引按清理后的 **15 个场景 /
  268 个对象**重写（原先列着 9 个已删场景，且 `SC_car_showcase` 的描述已经失效——它的 12 个 MI 现在全部
  指向 `M_pbr`）；§1.5.1 / §1.5.2 / §4.2 / §4.3 里指向已删文件的行改为事实描述；5 处指向旧文档位置的
  链接改写到本目录；
- `documents/README.md`、`AGENTS.md`：删掉指向不存在的 `foliage-speedtree-sss-wind-roadmap.md` 与
  `sky-pass-environment-roadmap.md`（只有 `.html` 版本），并去掉已归档的 NeoX skin 基线条目；
- `documents/plan/rendering/archive/README.md` 自身也记入本轮清单。



