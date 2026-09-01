# NeoX `b_f_3725` 角色还原交接

## 文档状态

- 交接日期：2026-08-24
- 当前状态：静态几何与 35/35 材质槽已迁移；Skin、Hair、Default、Silk、Pearl、Crystal 的 MF/母材质结构已对齐，但仍不是 NeoX 完整视觉还原
- 当前资源与 Shader 对齐合同：`documents/rendering/neox-character-alignment-contract-v1.md`
- 角色范围：`b_f_3725` 当前 Pose 静态角色
- 不包含：动画、Skin/Skeleton 运行时导入、动态 Billboard、Shading Model 新增或扩展
- 关联盘点：`documents/plan/rendering/neox-character-shader-feature-inventory.md`

## 一分钟接手

从 `D:\YYBWorkSpace\GitHub\VulkanLearn` 启动当前角色场景：

```powershell
build/bin/main.exe --initial-scene scenes/SC_b_f_3725_p0.json --no-dev-ui
```

当前场景资源：

- 场景：`D:\YYBWorkSpace\GitHub\VukanLearnResources\scenes\SC_b_f_3725_p0.json`
- 模型资产：`D:\YYBWorkSpace\GitHub\VukanLearnResources\models\neox\b_f_3725\SM_b_f_3725_p0.json`
- Hair mode 8 Core 模型资产：`D:\YYBWorkSpace\GitHub\VukanLearnResources\models\neox\b_f_3725\SM_b_f_3725_hair_core.json`
- glTF：`D:\YYBWorkSpace\GitHub\VukanLearnResources\models\datas\neox\b_f_3725\b_f_3725.gltf`
- Hair mode 8 Core glTF：`D:\YYBWorkSpace\GitHub\VukanLearnResources\models\datas\neox\b_f_3725\b_f_3725_hair_core.gltf`
- glTF Buffer：`D:\YYBWorkSpace\GitHub\VukanLearnResources\models\datas\neox\b_f_3725\b_f_3725.bin`
- 导出审计：`D:\YYBWorkSpace\GitHub\VukanLearnResources\models\datas\neox\b_f_3725\b_f_3725.audit.json`
- 材质迁移清单：`D:\YYBWorkSpace\GitHub\VukanLearnResources\generated\neox\b_f_3725\b_f_3725-material-migration.json`

注意：资源根目录的 `VukanLearnResources` 拼写是当前运行时既有事实，不要擅自改成 `VulkanLearnResources`。

## 用户已冻结的约束

1. 本任务不新增、不扩展 Shading Model；Shading Model 对齐由独立专项完成。
2. 只能使用现有 `DefaultLit`、`Subsurface`、`PreintegratedSkin`、`Cloth`、`Hair`、`Eye`、`ThinTranslucent` 等模型。
3. 模型交换格式使用 glTF。
4. 源网格真实存在 UV1 时才导出 `TEXCOORD_1`；禁止复制 UV0 或生成假的 UV1。
5. Blender 只能使用自构建版本：
   `D:\YYBWorkSpace\blender-4.5-lts-vs2026-relwithdebinfo-neox\bin\RelWithDebInfo\blender.exe`
6. NeoxIO 根目录：`D:\YYBWorkSpace\GitHub\NeoxIO`。
7. 新增或修改的非显然 C++、Shader 和迁移代码必须补简洁中文注释。
8. 迁移时必须阅读 NeoX 源 Shader 注释，把已验证的原始语义补回中文注释，并明确记录有意差异。
9. 不要直接编辑 `shader/spv/`；Shader 源码以 `shader/glsl/` 为准。
10. 未经用户明确要求，不提交 Git。

## 当前完成度

| 项目 | 当前结果 |
|---|---|
| 导出网格 | 10 个静态源 glTF Mesh；另生成 2 个 Hair Core 过滤 Mesh |
| 材质槽 | 35 个真实槽位 |
| 实际 MI | 20 个材质实例；Plain Silk 按 Cull 合同拆分，Hair mode 8 额外一份 Clip MI |
| fallback | 0 |
| UV0+UV1 网格 | 9 个 |
| 仅 UV0 网格 | `nf2022_f_01`，1 个 |
| Skin/Animation | 不导出，当前 Pose 静态烘焙 |
| Modifier | 已应用 |
| 对象世界变换 | 已烘焙 |
| 场景环境 | 程序化天空，强度 0.45 |
| 脸部基础贴图 | `Tex0`/`NormalMap`/`ParamMap` 已核对并迁移 |
| 脸部 Skin 辅助贴图 | `SkinParam`/`SkinAux`/`DetailNormal` 已生成并绑定 |
| 身体 P0 Skin 贴图 | `nb_f_2023002a/m/n` 与共享 `skin_detial_n` 已生成并绑定 |
| 脸部妆容/闪粉 | 未完整迁移；`tex_s`/Texture2DArray glitter 仍是已知缺口 |

审计规则：

```text
源只有 UV0     -> 只导出 TEXCOORD_0 -> texCoord1 保持零值
源有 UV0+UV1   -> 导出 TEXCOORD_0/1 -> 两套 UV 原样保留
任何源网格     -> 禁止用 UV0 合成 UV1
```

## 输入资产

### Blender 装配

```text
D:\YYBWorkSpace\GitHub\NeoxIO\artifacts\player_assembly_current_pose_fixed.blend
```

### NeoX 资源根

```text
K:\future\res
```

### 使用的 MTG

```text
K:\future\res\character\players2021\b_f_3725\b_f_3725_1_high.mtg
K:\future\res\character\players2021\b_f_3725\b_f_3725_2_high.mtg
K:\future\res\character\players2021\b_f_3725\b_f_3725_3_high.mtg
K:\future\res\character\players2021\b_f_3725\b_f_3725_4_high.mtg
K:\future\res\character\players2021\b_f_3725\b_f_3725_5_high.mtg
K:\future\res\character\players2021\b_f_3725\b_f_3725_6_high.mtg
K:\future\res\character\players2021\b_f_3725\b_f_3725_high001.mtg
K:\future\res\character\players2021\b_f_3725\h_f_3725_1_high.mtg
K:\future\res\character\players2021\nf2022_f_01\nf2022_f_01_high001.mtg
```

`h_f_3725_1_high.mtg` 与 `h_f_3725_high001.mtg` 的四个同名槽位合同等价，当前统一使用前者。

### NeoX Shader 源

```text
C:\Software\WorkTemp\G66ShaderDevelop\shader-source
```

主要参考：

```text
pbr/pbr_hair_transparent.nsf
pbr/nodes/pbr_silk_nodes.hlsl
pbr/nodes/pbr_silk_parameters.hlsl
pbr/nodes/pbr_default_nodes.hlsl
pbr/nodes/pbr_default_parameters.hlsl
pbr/nodes/pbr_hair_transparent_nodes.hlsl
pbr/nodes/pbr_hair_transparent_parameters.hlsl
pbr/nodes/pbr_eye_nodes.hlsl
pbr/nodes/pbr_eye_parameters.hlsl
pbr/nodes/pbr_subsurface_crystal_nodes.hlsl
pbr/nodes/pbr_subsurface_crystal_parameters.hlsl
pbr/nodes/crystal_functions.hlsl
shaderlib/hair_bsdf.hlsl
shaderlib/lighting_functions.hlsl
shaderlib/pixel_init.hlsl
shaderlib/shading_models.hlsl
shaderlib/shadow_functions.hlsl
shaderlib/surface_functions_shared.hlsl
```

## 迁移架构

迁移遵循“旧格式离线解包，运行时只消费标准语义”的原则：

```text
MTG + NeoX TGA
    -> Python 2 离线转换
    -> 标准 BaseColor / Normal / PBR / SkinParam / SkinAux / Detail / Surface / RDI 纹理
    -> Texture JSON
    -> 共享 MI
    -> M_ 母材质组合 MF
    -> 现有 Shading Model / RenderMode
```

禁止在每帧 Shader 中重新解释 NeoX ParamMap、NormalMap、SurfaceMap 或 RDI 打包格式；
脸部和身体 Skin 的源通道都在离线转换阶段拆到专用 SkinParam/SkinAux/Detail 资源。

### 已确认的通道

| 资源 | 通道 | 语义 |
|---|---|---|
| BaseColor | RGB | sRGB Base Color |
| BaseColor | A | 按材质族作为 Coverage 或离线拆分的控制量 |
| NeoX ParamMap | R | Roughness |
| NeoX ParamMap | G | Metallic |
| NeoX ParamMap | B | Skin/Thickness/Anisotropy 等材质族辅助量 |
| NeoX ParamMap | A | Ambient Occlusion |
| 目标 PBR | R/G/B/A | Roughness / Metallic / AO / Source Aux |
| NeoX Normal | R/G | Tangent Normal XY |
| NeoX Normal | B/A | Curvature/Weather 与 Detail Mask 等辅助量 |
| 目标 Normal | RGB | 离线重建完整 XYZ |
| Face/Body SkinParam | R/G/B/A | Roughness / Metallic / SkinColorMask / AO |
| Face/Body SkinAux | R/G | Curvature / DetailNormalMask |
| Face/Body DetailNormal | RGB/A | Detail Normal XYZ / PoreModulation |
| Silk Surface | R/G/B/A | Mask1 / Emission-Pearl-Flow / Sparkle / Detail Mask |
| Hair RDI | R/G/B/A | Root / Depth / Strand ID / AO |
| Crystal Mask | R/G/B/A | Dissolve / Thickness / AO / Fresnel Emission Mask |

## Shader 与 MF 落地

### 母材质

- `shader/glsl/M_neoxDefault.*`
  - `DefaultLit`
  - Default PBR、静态发光遮罩、Sparkle
- `shader/glsl/M_neoxSilk.*`
  - `Cloth`
  - Silk、Flow Mask、Sparkle、Emission Fresnel、Sheen
- `shader/glsl/M_neoxPearl.*`
  - `Subsurface`
  - Pearl MatCap、真实 UV1、圆形 Coverage、假球法线、Base/Emission Split
- `shader/glsl/M_neoxHair.*`
  - `Hair` + `TransparentAlphaBlend`
  - Tex0、Normal、RDI、AlphaRef=51 覆盖率归一化
- `shader/glsl/M_neoxCrystal.*`
  - `ThinTranslucent`
  - Crystal 透射近似、厚度、Coverage、Caustic/Emission 层次
- 现有 `M_eye`
  - `Eye`
  - Iris/Sclera 参数和现有 Eye 光照路径
- 现有 `M_preintegratedSkin`
  - 身体和脸部都使用独立 SkinParam/SkinAux/Detail 输入；旧 PBR/Emission 仅保留为兼容审计资产

### Material Function

- `shader/glsl/materialFunction/mf_neoxPackedSurface.glsl`
  - 标准 PBR/Surface 输入、静态 Emission、稳定 Sparkle、Aux Anisotropy
- `shader/glsl/materialFunction/mf_pearlescentInputs.glsl`
  - Pearl Coverage、假球法线、真实 UV1 2×2 MatCap、Noise
- `shader/glsl/materialFunction/mf_emission.glsl`
  - NeoX Base/Emission 拆分
- `shader/glsl/materialFunction/mf_subsurfaceInputs.glsl`
  - Subsurface 作者参数到稳定模型输入
- `shader/glsl/materialFunction/mf_neoxSkinTextures.glsl`、`shader/glsl/materialFunction/mf_neoxSkinInputs.glsl`
  - 脸部 NeoX packed skin 输入、细节法线、skin mask 与 curvature 拆分

### UV1 运行时支持

已修改：

- `source/VertexDataStruct.h`
- `source/mesh/loader/mesh/assimpSourceAdapter.cpp`
- `shader/glsl/engine/materialContext.glsl`
- `shader/glsl/engine/passTemplate/base.vert.glsl`
- `shader/glsl/engine/passTemplate/shadowDepth.vert.glsl`

这些改动只传递真实 `TEXCOORD_1`，没有 UV0 回填逻辑。

## 35 槽映射

| 目标 MI | Shading Model / 路径 | glTF 槽位 |
|---|---|---|
| `MI_body_silk_flow.json` | Cloth + Alpha Blend + ZWrite | `b_f_3725_1_high`、`b_f_3725_2_high_0`、`b_f_3725_high_7` |
| `MI_body_default_secondary.json` | DefaultLit + Alpha Blend + ZWrite | `b_f_3725_2_high_1`、`b_f_3725_high_8` |
| `MI_body_silk_emissive.json` | Cloth + OpaqueClip | `b_f_3725_2_high_2`、`b_f_3725_3_high_2`、`b_f_3725_4_high_1`、`b_f_3725_5_high_2`、`b_f_3725_6_high_2`、`b_f_3725_high_3` |
| `MI_b_f_3725_high_1_pearl.json` | Subsurface + OpaqueClip | `b_f_3725_3_high_0`、`b_f_3725_4_high_0`、`b_f_3725_5_high_0`、`b_f_3725_6_high_0`、`b_f_3725_high_1` |
| `MI_body_default_clip.json` | DefaultLit + OpaqueClip | `b_f_3725_3_high_1`、`b_f_3725_5_high_1`、`b_f_3725_6_high_1`、`b_f_3725_high_2` |
| `MI_body_silk_plain_back.json` | Cloth + Alpha Blend + ZWrite + Back Cull | `b_f_3725_high_4` |
| `MI_body_silk_plain.json` | Cloth + Alpha Blend + ZWrite + Two-sided | `b_f_3725_high_5` |
| `MI_body_silk_emissive_alt.json` | Cloth + Alpha Blend + ZWrite | `b_f_3725_high_6` |
| `MI_crystal_red_clip.json` | ThinTranslucent | `b_f_3725_6_high_3`、`b_f_3725_high_9` |
| `MI_crystal_red_opaque.json` | ThinTranslucent | `b_f_3725_high_10` |
| `MI_crystal_gold_opaque.json` | ThinTranslucent Two-sided | `b_f_3725_high_11` |
| `MI_b_f_3725_body_p0.json` | PreintegratedSkin | `b_f_3725_high_0` |
| `MI_hair_cards.json` | Hair Blend Fringe | `h_f_3725_high_0` 原角色资源 |
| `MI_hair_cards_clip.json` | Hair OpaqueClip Core | `h_f_3725_high_0` Hair-only 资源 |
| `MI_hair_pearl.json` | Subsurface + OpaqueClip | `h_f_3725_high_1` |
| `MI_hair_default_sparkle.json` | DefaultLit | `h_f_3725_high_2` |
| `MI_hair_silk.json` | Cloth | `h_f_3725_high_3` |
| `MI_eye.json` | Eye | `07 - Default` |
| `MI_face_skin.json` | PreintegratedSkin | `09 - Default` |
| `MI_eye_edge.json` | DefaultLit Transparent | `08 - Default` |

完整逐槽源合同、MTG 文件、SHA-256 合同摘要和目标 MI 以迁移清单为准：

```text
D:\YYBWorkSpace\GitHub\VukanLearnResources\generated\neox\b_f_3725\b_f_3725-material-migration.json
```

## RenderDoc 皮肤证据与当前边界

本次对比使用的实际捕获是：

```text
K:\future\Documents\pc_13th Gen Intel(R) Core(TM) i9-13900K_NVIDIA GeForce RTX 4090_08.24_12.56.04_frame306538.rdc
```

用户消息中的 `Documents\pc\...` 是路径误写；截图上的调试文字不是操作指令。

RenderDoc D3D11 `event 887` 的脸部 Draw 同时绑定：

- `Tex0 -> py(id : 14544)`；
- `tex_s -> py(id : 14545)`；
- `NormalMap -> py(id : 14546)`；
- `ParamMap -> py(id : 14547)`；
- `DetailMap -> common\\textures\\skin_detial_n.tga`；
- `t_glitter_noise_array -> ...\\glitter_noise_array.array`。

导出的 `ParamMap` 与源 `nf2022_f_01m.tga` 基本逐像素一致，说明脸部不是“贴图完全没过来”；
身体的主要问题则是旧迁移把 `b_f_3725` 服装/通用贴图误绑定到 Skin 槽位，实际 MTG 要求的是
`nb_f_2023` 皮肤组。两条问题都已通过离线拆包和 Skin 专用输入修复。
已修复为：

1. `ParamMap.B` 保留为 `skinColorMask`，`ParamMap.A` 正确作为 AO；
2. `NormalMap.B/A` 离线拆为 curvature/detail mask；
3. `common\\textures\\skin_detial_n.tga` 离线转换为 `DetailNormal`；
4. 脸部 BaseColor/Normal 改为 clamp，Detail 保持 repeat；
5. Skin specular 通过 `GBufferC.g` 保留到 deferred lighting。

RenderDoc `event 875` 的身体 Skin 证据为：

```text
character\\players2021\\nb_f_2023\\nb_f_2023002a.tga
character\\players2021\\nb_f_2023\\nb_f_2023002n.tga
character\\players2021\\nb_f_2023\\nb_f_2023002m.tga
common\\textures\\skin_detial_n.tga
```

因此 `MI_b_f_3725_body_p0.json` 现在只绑定 `BaseColor`、`Normal`、`SkinParam`、
`SkinAux` 和 `DetailNormal`；旧 `PBR`/`EmissionMask` 不再进入 P0 Skin shader。

仍未宣称完整恢复的部分：

- 本帧 `tex_s` 捕获内容为全黑；其运行时生成/绑定合同没有静态 2D 资产可直接复用；
- `glitter_noise_array` 是 Texture2DArray，当前 VulkanLearn 角色材质只支持 `sampler2D`，因此没有伪造一个“完整闪粉”绑定；
- VulkanLearn 的 PreintegratedSkin LUT 尚未复现 NeoX 的完整 curvature response、妆容分支和源统一光照；
- Blender/glTF 只保证当前 Pose 的几何和材质槽，不等于 NeoX 运行时的脸部妆容、动态光照或角色装配效果。

## 程序化天空

角色验证场景使用程序化天空，并与 `SC_car_showcase.json` 共用同一套方向光、阴影和环境参数；角色相机仍保持独立。生成脚本也已同步：

```json
{
    "type": "proceduralSky",
    "cubeSize": 128,
    "intensity": 1.0,
    "skyParameters": {
        "sunIntensity": 1.0,
        "sunColor": [22.0, 17.5, 10.0],
        "sunAngularRadius": 0.08,
        "zenithColor": [0.09, 0.32, 0.95],
        "horizonColor": [0.85, 0.78, 0.58],
        "groundColor": [0.06, 0.07, 0.055],
        "skyGradientExponent": 0.42,
        "groundGradientExponent": 0.35,
        "sunHaloExponent": 96.0,
        "sunHaloStrength": 0.45
    }
}
```

角色 Key Light 现在复用汽车展示场景的白色 `intensity=10` 主光、方向和完整 CSM 参数；
蓝色 Fill 只保留为 `intensity=0` 的禁用占位，不参与最终画面。这样材质对比共享同一场景
光照基线，NeoX 的角色专属能量分配继续由各材质的 `CharacterLighting` 参数表达。

## 全量重生成

### 1. 重新导出 glTF

```powershell
& 'D:\YYBWorkSpace\blender-4.5-lts-vs2026-relwithdebinfo-neox\bin\RelWithDebInfo\blender.exe' `
  --background `
  'D:\YYBWorkSpace\GitHub\NeoxIO\artifacts\player_assembly_current_pose_fixed.blend' `
  --python 'D:\YYBWorkSpace\GitHub\VulkanLearn\tool\neox\export_bf3725_gltf.py' `
  -- `
  --output 'D:\YYBWorkSpace\GitHub\VukanLearnResources\models\datas\neox\b_f_3725\b_f_3725.gltf'
```

导出后必须检查：

- `b_f_3725.audit.json` 中 `skinsExported=false`；
- `syntheticUv1=false`；
- 10 个 Mesh；
- 35 个真实槽位；
- 9 个 Mesh 导出 UV1；
- `nf2022_f_01` 只导出 UV0。

### 2. 转换全部纹理和 MI

当前终端的 `python` 是 Python 2.7，并安装了转换所需 Pillow/Numpy：

```powershell
python tool/neox/convert_bf3725_character.py --overwrite
```

预期输出：

```text
Converted 35 NeoX material slots with zero fallback
Shared material instances written: 17
```

统一转换器生成 18 个 MI；加上已有身体 Skin 和身体 Pearl，共 20 个实际 MI。

### 3. 重建模型资产与场景

```powershell
py -3.13 tool/neox/create_bf3725_p0_mesh_asset.py --overwrite
```

预期输出必须包含：

```text
Created b_f_3725 P0 mesh asset with 35 real material slots
Unmigrated slots: 0
```

## 验证命令

### Python 语法

```powershell
python -m py_compile tool/neox/convert_bf3725_character.py
py -3.13 -m py_compile tool/neox/create_bf3725_p0_mesh_asset.py tool/neox/export_bf3725_gltf.py
```

语法检查会产生 `tool/neox/__pycache__` 或 Python 2 `.pyc`，不要提交这些生成文件。

### C++ 构建

```powershell
cmake --build build -j
```

### Shading Model 与 Shader Cache 测试

```powershell
ctest --test-dir build -R "(hair|eye|cloth|thin_translucent|subsurface|shader_build_core)" --output-on-failure
```

历史记录为 8/8 通过；本次针对 Skin/GBuffer 改动补跑 `shader_build_core`、
`shader_build_integration`、`subsurface_contract`，结果为 3/3 通过。

2026-08-24 Hair 单材质结构对齐补跑 `main`、`hair_tests`、
`shader_build_integration_tests` 构建，以及 `hair_contract`、
`shader_build_integration`，结果为 2/2 通过。角色烟测同时确认
`mf_neoxHairInputs.glsl` 已进入 `M_neoxHair` 的传递 include 依赖和 Material Base 变体。

2026-08-24 Hair mode 8 双资源补跑相同构建与 2/2 定向测试，并完成 3/3 帧角色烟测。
Shader Cache 中同时出现 `OpaqueClip Base`、`OpaqueClip ShadowDepth` 和
`TransparentAlphaBlend Base` 三个 `M_neoxHair` 变体；World/Graph transaction 正常提交，
`retiredPending=0`。

同日视觉检查发现深色头发出现大面积白色条带。BaseColor 贴图审计确认源图为深棕黑，
白色来自无色 Hair R 反射：NeoX 源默认 `u_specular=0.3`、`u_roughness=0.3`、
`u_scatter=0.5`、`u_backlit_intensity=0.5`，而初版作者值更亮更尖，并且 Deferred
GBuffer 曾把 Hair specular 固定恢复为 `0.5`。当前已将 NeoX Hair 默认参数对齐源值，
并让 `GBufferC.g` 对 Hair 显式传递 specular。定向测试 2/2 和 3/3 帧烟测通过。

同日继续按 event 1708 接入 Hair 角色环境/方向/局部/相机虚拟光合同，并将验证场景
Key、Fill、环境 SH 校准到捕获量级。侧面近景已恢复深色发束和宽灰高光；对比截图使用
运行时 Hable 模式观察暗部层次，项目默认 Tone Mapping 尚未改动，不能把后处理曲线差异
误记为 HairBxDF 参数。

### 角色场景烟测

```powershell
build/bin/main.exe `
  --initial-scene scenes/SC_b_f_3725_p0.json `
  --framesmoke 3 `
  --exit-after-tests `
  --no-dev-ui
```

当前结果：3/3 帧完成，World/Graph transaction 正常提交，未发现资源退休堆积。
本次烟测同时成功编译并反射脸部 Skin 变体，绑定 `albedoMap`、`normalMap`、
`skinParamMap`、`skinAuxMap`、`skinDetailMap`，shader build failed=0。

### 静态资产审计

必须满足：

```text
materialSlots = 35
unique slot names = 35
fallbackSlotCount = 0
所有 materialInstancePath 存在
syntheticUv1 = false
real UV1 meshes = 9
```

## 有意差异与当前限制

### Face Skin

当前已恢复：

- `nf2022_f_01a.tga` BaseColor，使用 sRGB + clamp；
- `nf2022_f_01n.tga` 的 XY 法线，并离线拆出 B=curvature、A=detail mask；
- `nf2022_f_01m.tga` 的 R/G/B/A 原始 SkinParam 语义；
- `common\\textures\\skin_detial_n.tga` 的细节法线，使用线性 + repeat；
- Skin specular 在 deferred GBuffer 中的传递。

当前仍是有意差异：

- `skinAuxMap.R` 已写入 GBuffer，但现有 PreintegratedSkin LUT lighting 还没有
  NeoX 源 shader 的完整 curvature response；
- RenderDoc `event 887` 的 `tex_s` 本帧为全黑，未伪造动态妆容贴图；
- `t_glitter_noise_array` 是 Texture2DArray，当前材质描述符只支持 `sampler2D`，
  闪粉阵列尚未接入；
- 当前 Blender/glTF 仍只是静态 Pose 几何，不是 NeoX 运行时装配和光照结果。

### Body Skin

当前已恢复：

- `nb_f_2023002a.tga` BaseColor，使用 sRGB + clamp；
- `nb_f_2023002n.tga` 的 XY 法线，并离线拆出 B=curvature、A=detail mask；
- `nb_f_2023002m.tga` 的 R/G/B/A SkinParam 语义；
- `common\\textures\\skin_detial_n.tga` 的细节法线，使用线性 + repeat；
- `b_f_3725_high_0` 的 P0 MI 已切换到 `USE_SKIN_PARAM_MAP`、
  `USE_SKIN_AUX_MAP`、`USE_SKIN_DETAIL_MAP`。

当前仍是有意差异：

- RenderDoc `event 875` 的身体源材质包含 NeoX 角色统一光照、Fresnel 粗糙度和完整
  curvature response；VulkanLearn 当前只复用现有 PreintegratedSkin LUT，不能宣称逐像素等价；
- `nb_f_2023002a.tga` 为 RGB 不透明源图，因此 P0 不伪造 `Tex0.A` 发光/coverage；旧服装组
  `b_f_3725001a/m` 只留在未绑定兼容资产中；
- 当前 Blender/glTF 仍只是静态 Pose 几何，不是 NeoX 运行时装配、骨骼或动态光照结果。

### 静态 Billboard

NeoX Pearl 的 Billboard 顶点展开已经烘焙进当前 Pose glTF。当前不恢复：

- 动态相机朝向；
- 上一帧 Billboard Velocity/TAA 修正；
- 源 `u_depth_offset`；
- 动态动画或骨骼驱动。

### Silk / Default 动画相位

已保留：

- Surface 遮罩；
- Sparkle 密度和亮度；
- Emission Fresnel；
- Flow 使用的遮罩语义。

当前没有 NeoX 原始统一动画时钟合同，因此不伪造 Flow/Twinkle 的逐帧相位。后续如果补时钟，应接到共享 MF，不要新增 Shading Model。

### Crystal

当前使用现有 `ThinTranslucent` 近似，保留：

- Base/Crystal/Refraction 色；
- Thickness；
- Coverage；
- Caustic/Emission Mask；
- 双面状态。

当前没有：

- SceneColor 折射；
- 折射旋转采样；
- 源深度偏移；
- 完整 NeoX Caustic 场景采样。

如果未来渲染器增加 SceneColor 透明资源，应替换 Crystal 上层实现，不应增加新的角色专用 Shading Model。

### Eye

Eye 使用现有 VulkanLearn Eye Shading Model。源 `u_iris_range=5.85` 和 `u_pipil_scale=0.61` 已映射到当前作者参数；源 MatCap 和自定义 Cube IBL 由现有 Eye 光照、Caustic LUT 和场景环境替代。

### Hair

`M_neoxHair.surface.glsl` 现在只负责 MI 参数接线；BaseColor、Normal、RDI、coverage、
AO 和 HairMaterialInputs 统一由 `mf_neoxHairInputs.glsl` 生成。当前 mode 8 使用两份
静态资源临时恢复双 Pass：

- `SM_b_f_3725_hair_core.json` 只引用两个源 Hair 网格中的 `h_f_3725_high_0` primitive，
  绑定 `MI_hair_cards_clip.json`，进入 `OpaqueClip` Base/ShadowDepth；
- 原角色资源继续绑定 `MI_hair_cards.json`，进入 `TransparentAlphaBlend`，只补发梢；
- 两份 MI 共享纹理与参数，Core 用原始 `Tex0.A` Alpha Test，Fringe 使用
  `saturate(Tex0.A / 0.5)`；相同几何在 `LESS` 深度测试下避免核心区域重复混合；
- MTG `AlphaRef=51` 仍保留为 raw 审计字段；该源 Shader 的双 Pass 实际 ClipValue
  来自默认 `u_two_pass_clip_value=0.5`，当前没有用 `51/255` 替代它。

2026-08-24 已按真实槽位和 Shader 调用链重新核对源码：

- `h_f_3725_high_0` 明确启用 `SEPARATED_CHARACTER_LIGHTING=TRUE`、
  `PLAYERS_SELF=TRUE`，Technique 为 `shader\pbr_hair_transparent.fx::TShader`，
  实际入口为 `pbr/pbr_hair_transparent.nsf`；
- Hair Surface 会先把卡片几何法线写入 `world_tangent`，再把法线贴图生成的
  fiber axis 写入 `world_normal`。HairBxDF 的 `N` 因而是 fiber axis，
  `WorldTangent` 才是 GI、附加阴影和局部光背面抑制使用的几何法线；
- 双面路径只翻转一次几何 `world_normal`，随后重建 `world_binormal`，不按普通
  双面材质整体翻转完整 TBN；
- `Tex0` 使用 clamp 和 `-1` mip bias；Normal sampler 的 `-1` 与
  `SampleBias(TEXTURE_BIAS=-1)` 叠加为总 bias `-2`；RDI.RGB 使用 UV0、LOD0，
  只有 RDI.A 的 2U AO 使用 UV1 和 `SampleBias(-1)`；
- 透明 Hair 调用的通用 `CalcAmbientOcclusion()` 是空实现，不执行非透明 Hair 的
  `[0.8, 1.0]` AO 压缩。本槽位没有覆写 `u_ao_from_normal`，其默认值 `1.0`
  使法线方向 AO 乘数恒为 `1`；
- Direct 合同为 `PI * HairShading(..., Area=0)`；Ambient 合同为
  `2 * PI * HairShadingAmbient(..., Area=0.2)`，Ambient 明确只组合
  R、TRT 与 Scatter，不计算 TT；
- RDI.B 只在 `QUALITY_SUPPORT_ULTRA_HIGH && PLAYERS_SELF` 的发色 noise/variation
  分支中作为随机 ID 使用，不参与粗糙度。当前未接入该公共噪声纹理，因此保持
  `u_roughness`，不再用 RDI.B 制造粗糙度扰动。

当前仍有一个有意差异：尚未实现正式 Material Multi-Pass/PassTag，此双资源方案
只面向当前静态 Pose，未来动态角色必须保证 Core/Fringe 共享完全一致的骨骼、Morph
和 WPO 结果。

角色光照已按 RenderDoc event 1708 接入 `u_hairCharacterLighting`：`x/y/z/w` 分别是
环境、方向、局部光倍率和无色相机虚拟光强度。该角色的 MI 冻结为
初始 RenderDoc 量级为 `[1, 1, 0.55, 0.70588237]`；P5 固定机位复核后，目标 Hair 槽
收敛到 `[0.25, 1, 0.55, 0.25]`，只降低环境与相机虚拟光，保留方向/局部光语义。
虚拟光使用 `camera_vector` 复用完整 HairBxDF，不能再用 MI 发色压暗来掩盖角色光照缺口。

## 下一步建议

基础技术接入已完成，但视觉还原仍需要同机画面对比和参数校准：

1. 固定相机、程序化天空和 Key/Fill Light；
2. 分别截图 NeoX 与 VulkanLearn；
3. 先确认脸部几何/UV 与 NeoX event 887 的 Draw 范围，再校准 Skin；
4. 再处理 `tex_s` 动态妆容和 glitter array 的资源合同；
5. 最后校准 Silk Sheen、Emission、Sparkle、Hair Coverage、Eye Iris、Crystal 透射；
6. 每次只调整 MI 或上层 MF 参数；有 RenderDoc 证据时才修改 GBuffer 或新增资源类型。

建议把画面对比结果继续补到本交接文档的“视觉校准记录”或迁移清单旁的独立截图目录；不要把一次性截图二进制提交到源码目录。

## Git 与资源交接注意

- 当前修改尚未提交。
- Git 只覆盖 `VulkanLearn` 仓库中的源码、Shader、工具和文档。
- `D:\YYBWorkSpace\GitHub\VukanLearnResources` 是外部运行时资源根，不在本仓库 Git 状态中。
- 完整交接必须同时保留：
  - `VulkanLearn` 工作区修改；
  - `VukanLearnResources` 下生成的 glTF、纹理、Texture JSON、MI、模型资产、场景和迁移清单；
  - NeoxIO 的源 `.blend`；
  - `K:\future\res` NeoX 原始资源访问权限。
- 不要运行清理脚本删除 `generated/neox/b_f_3725`，除非准备立即执行全量重生成。

## 关键入口汇总

| 责任 | 文件 |
|---|---|
| Blender 静态 glTF 导出 | `tool/neox/export_bf3725_gltf.py` |
| 全材质转换 | `tool/neox/convert_bf3725_character.py` |
| P0 身体 Skin 转换 | `tool/neox/convert_bf3725_p0.py` |
| 35 槽模型与场景生成 | `tool/neox/create_bf3725_p0_mesh_asset.py` |
| NeoX Default/Silk 公共 MF | `shader/glsl/materialFunction/mf_neoxPackedSurface.glsl` |
| NeoX Hair 输入 MF | `shader/glsl/materialFunction/mf_neoxHairInputs.glsl` |
| Pearl MF | `shader/glsl/materialFunction/mf_pearlescentInputs.glsl` |
| Emission MF | `shader/glsl/materialFunction/mf_emission.glsl` |
| Subsurface MF | `shader/glsl/materialFunction/mf_subsurfaceInputs.glsl` |
| UV1 顶点合同 | `source/VertexDataStruct.h` |
| Assimp UV1 导入 | `source/mesh/loader/mesh/assimpSourceAdapter.cpp` |
| 迁移功能盘点 | `documents/plan/rendering/neox-character-shader-feature-inventory.md` |
| 完整迁移审计 | `VukanLearnResources/generated/neox/b_f_3725/b_f_3725-material-migration.json` |
