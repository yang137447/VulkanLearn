# NeoX `b_f_3725` Hair 效果对齐修复计划

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 状态 | P0–P5 已执行并验证 |
| 建立日期 | 2026-08-25 |
| 目标角色 | NeoX `b_f_3725` 当前 Pose 静态角色 |
| 目标材质槽 | `h_f_3725_high_0` Hair Cards |
| 视觉目标 | 黑色发束、宽而柔和的灰色高光、稳定的发片双面与透明边缘 |
| Shading Model 基线 | VulkanLearn 已有的 UE 对齐 `Hair` Shading Model |
| NeoX 源码根目录 | `C:\Software\WorkTemp\G66ShaderDevelop\shader-source` |
| 关联稳定合同 | `documents/rendering/hair-shading-model.md` |
| 关联角色交接 | `documents/plan/rendering/neox-b-f-3725-character-restoration-handoff.md` |

## 1. 目标与核心边界

本计划只解决 `b_f_3725` Hair 材质从 NeoX 资源语义到 VulkanLearn 当前 Hair
管线的效果对齐。目标不是把 NeoX 的 `HairShading` 逐行复制成一个新的 Shading Model，
而是建立以下边界：

```text
NeoX Tex0 / Normal / RDI / MTG 参数
    -> NeoX Material Functions
    -> 标准 Hair MaterialInputs
    -> VulkanLearn UE 对齐 Hair Shading Model
    -> Forward / Deferred / Shadow / Transparent 输出
```

### 1.1 Shading Model 冻结原则

1. 不新增 Hair Shading Model，不增加 NeoX 专用 Shading Model ID。
2. `shader/glsl/engine/hairScattering.glsl` 的目标始终是当前 UE 对齐 Hair
   R/TT/TRT/Scatter 合同，不以 NeoX `hair_bsdf.hlsl` 的逐字结果作为目标。
3. NeoX `HairShading`、`HairShadingAmbient` 只用于理解源材质的参数意义、光照组成和
   视觉意图，不能直接覆盖 UE 对齐 Hair 的路径公式。
4. 只有发现当前实现偏离 `documents/rendering/hair-shading-model.md` 的 UE 对齐合同时，
   才允许修改公共 Hair evaluator。
5. 公共 evaluator 中不得新增 `NEOX_*` 宏、槽位名称、RDI 通道解码或 NeoX 贴图采样。
6. 禁止为了匹配单张截图，直接修改公共 R/TT/TRT 常数、Fresnel、吸收或能量归一化。

### 1.2 NeoX 功能迁移原则

适合 Material Function 的 NeoX 功能统一放在 `mf_neoxHair*`：

- Tex0、NormalMap、RDI 的采样与 mip/LOD 规则；
- RDI 的 root、depth、strand ID、UV1 AO 通道解释；
- NeoX 双面 TBN 与 fiber axis 构造；
- Root/Tip、AO、Coverage、Alpha 双 Pass 的材质组合；
- BaseColor 到标准 Hair absorption/path color 输入的唯一转换；
- Ultra High 分支的 strand ID 发色变化与高频噪声；
- NeoX 作者参数到标准 Hair MaterialInputs 的映射。

依赖具体灯光方向、阴影贴图或场景曝光的行为不应塞进 MF。此类行为只能通过标准
Hair 输入、通用灯光积分或场景配置表达，不能让 MF 直接读取方向光和局部光状态。

## 2. 已核对的 NeoX 源合同

主要源码：

- `pbr/pbr_hair_transparent.nsf`
- `pbr/nodes/pbr_hair_transparent_parameters.hlsl`
- `pbr/nodes/pbr_hair_transparent_nodes.hlsl`
- `pbr/nodes/hair_functions.hlsl`
- `shaderlib/hair_bsdf.hlsl`
- `shaderlib/pixel_init.hlsl`
- `shaderlib/lighting_functions.hlsl`
- `shaderlib/shadow_functions.hlsl`

当前目标槽位已确认的源参数：

| NeoX 参数 | 源值 | VulkanLearn 目标输入 |
| --- | ---: | --- |
| `u_roughness` | `0.3` | Hair longitudinal roughness 基线 |
| `u_scatter` | `0.5` | Hair scatter |
| `u_specular` | `0.3` | Hair specular |
| `u_specular_shift` | `0.0` | Fiber frame shift |
| `u_root_intensity` | `0.0` | Root 作者效果关闭 |
| `u_depth_intensity` | `0.0` | RDI.G depth 调制关闭 |
| Hair IOR | `1.55` | Hair optical IOR |

RDI 通道：

| 通道 | NeoX 含义 | 迁移位置 |
| --- | --- | --- |
| R | Root mask | `mf_neoxHairInputs.glsl` |
| G | Depth/AO multiplier | `mf_neoxHairInputs.glsl` |
| B | Strand ID | Ultra High 发色变化 MF |
| A | UV1 AO | `mf_neoxHairTextures.glsl` |

NeoX 的 Hair 光照公式不是本计划的目标 Shading Model，但以下源意图需要保留：

- Hair 使用法线贴图构造的 fiber axis，而不是普通表面法线；
- 卡片几何法线用于双面修正、背面漏光抑制和阴影辅助；
- 环境 Hair 不使用源 TT 分量；
- `RDI.B` 只参与超高质量发色变化，不直接修改 roughness；
- Mode 8 使用 Alpha Test Core 与 Alpha Blend Fringe 两条路径组合。

## 3. 当前 VulkanLearn 基线

当前主要文件：

- `shader/glsl/M_neoxHair.json`
- `shader/glsl/M_neoxHair.surface.glsl`
- `shader/glsl/materialFunction/mf_neoxHairTextures.glsl`
- `shader/glsl/materialFunction/mf_neoxHairFiberFrame.glsl`
- `shader/glsl/materialFunction/mf_neoxHairInputs.glsl`
- `shader/glsl/engine/hairScattering.glsl`
- `shader/glsl/engine/hairLighting.glsl`
- `shader/glsl/engine/gbufferCodec.glsl`

当前运行时材质：

- `D:\YYBWorkSpace\GitHub\VukanLearnResources\materials\neox\b_f_3725\MI_hair_cards.json`
- `D:\YYBWorkSpace\GitHub\VukanLearnResources\materials\neox\b_f_3725\MI_hair_cards_clip.json`

当前场景：

- `D:\YYBWorkSpace\GitHub\VukanLearnResources\scenes\SC_b_f_3725_p0.json`

已经正确接入的部分：

- `M_neoxHair` 使用已有 `Hair` Shading Model；
- Blend Fringe 使用 `TransparentAlphaBlend`；
- Core MI 通过 `OpaqueClip` 恢复 NeoX Mode 8 的双 Pass 结构；
- Tex0、RDI、NormalMap 和真实 UV1 AO 已接入；
- Fiber axis 与卡片几何法线使用独立语义；
- 背面没有机械地翻转整个 TBN；
- Forward/Deferred 已能消费 Hair GBuffer；
- 当前目标参数已使用 `scatter=0.5`、`roughness=0.3`、`specular=0.3`。

## 4. 分阶段修复计划

### P0：冻结视觉基线

目标：在任何修复前建立可重复比较的输入条件。

1. 固定 `SC_b_f_3725_p0.json` 的相机、方向光、局部光、环境光与曝光。
2. 保存当前 Beauty 截图。
3. 保存以下 Hair Debug View：
   - Fiber Axis；
   - Card Geometric Normal；
   - R；
   - TT；
   - TRT；
   - Scatter；
   - AO；
   - Coverage；
   - Shadow Visibility。
4. 分别观察 Core 与 Fringe，确认白边、黑边、重复增亮和排序问题来自哪条 Pass。
5. 保持当前 Tone Mapping 配置，不把 Hable 或其他曲线写成全局默认。

阶段门：同一启动命令、同一相机和同一帧配置可以重复获得可比较截图。

### P1：修正 NeoX Hair Material Functions

目标：让 MF 输出正确、稳定的标准 Hair MaterialInputs，不改 UE Hair lobe。

#### P1.1 纹理采样合同

核对并保持：

- Tex0 使用源 sampler 的 mip bias；
- RDI.RGB 固定读取 LOD0；
- RDI.A 使用真实 UV1；
- NormalMap 使用与源 `MipLODBias + SampleBias` 等价的总偏移；
- Clamp/Repeat 地址模式由纹理资产 JSON 明确表达。

修改文件：

- `shader/glsl/materialFunction/mf_neoxHairTextures.glsl`
- 对应 `T_h_f_3725_cards_*.json` 纹理资产。

#### P1.2 Fiber Frame 与双面语义

核对：

- `context.worldNormal` 是卡片源几何法线；
- glTF Tangent 是发片宽度轴，不直接作为 root-to-tip；
- 使用源 `cross(normalTS, X)` 规则生成 fiber axis；
- 背面只翻几何法线，并据此重建基底；
- mirrored UV handedness 只应用一次。

修改文件：

- `shader/glsl/materialFunction/mf_neoxHairFiberFrame.glsl`
- 必要时核对 `source/mesh/loader/mesh/assimpSourceAdapter.cpp` 的 Tangent/UV1 导入。

#### P1.3 材质输入组合

核对：

- `HAIR_COLOR_MODE=NONE` 时保持 Tex0 原色；
- `u_root_intensity=0` 和 `u_depth_intensity=0` 时不产生隐藏染色或额外压暗；
- Coverage、Opacity、OpacityMask 与 absorption 相互独立；
- Core 与 Fringe 使用同一 Tex0.A 来源；
- BaseColor 到 absorption 只转换一次；
- `RDI.B` 不修改 roughness。

修改文件：

- `shader/glsl/materialFunction/mf_neoxHairInputs.glsl`
- `shader/glsl/M_neoxHair.surface.glsl`
- `shader/glsl/M_neoxHair.json`

阶段门：Debug View 中 Fiber Axis、Geometric Normal、AO 和 Coverage 均与资产通道一致，
且修改没有进入公共 Hair R/TT/TRT 公式。

### P2：校准 NeoX 输入到 UE Hair 的映射

目标：只调整输入映射和兼容层，不把 NeoX 公式写入 UE Hair Shading Model。

1. 以源 `u_roughness=0.3` 作为 longitudinal roughness 起点。
2. 保持 `u_specular=0.3`，不通过公共 Fresnel 常数补偿亮度。
3. 保持 `u_scatter=0.5`，使用标准 Hair scatter 输入。
4. IOR 使用 Hair 合同默认 `1.55`。
5. Fiber radius 与 cuticle tilt 使用现有 UE Hair authoring 合同；如需改值，只在 MI 或
   `M_neoxHair` 默认参数中调整，不按资产槽位修改公共 evaluator。
6. `u_hairCharacterLighting` 只作为通用环境、方向、局部和虚拟光倍率输入，不改变
   R/TT/TRT 的数学定义。
7. 审计公共 Hair 文件中的 NeoX 命名辅助函数：
   - 如果只是通用 Card visibility，应改为来源无关命名；
   - 如果是 NeoX 材质特性，应改由 MF 参数化；
   - 不允许把 RDI 或槽位判断带入公共 evaluator。

阶段门：同一 UE Hair Shading Model 可以同时服务普通 `M_hair` 与 `M_neoxHair`，且
`M_neoxHair` 的差异全部由标准输入表达。

### P3：修正 Mode 8 Core/Fringe 组合

目标：恢复发片实心区域与透明发梢的连续组合。

1. Core 使用原始 Tex0.A 与 `u_alphaClipThreshold` 做 Alpha Test。
2. Fringe 使用同一 coverage 做归一化 Alpha Blend。
3. 核对两个 Pass 的深度写入、绘制顺序、双面状态和阴影行为。
4. Core 与 Fringe 不能同时在实心区域输出完整颜色，避免双倍高光。
5. ShadowDepth 必须与 Core 使用相同 coverage 来源和 clip threshold。

主要文件和资源：

- `shader/glsl/M_neoxHair.json`
- `shader/glsl/materialFunction/mf_neoxHairInputs.glsl`
- `D:\YYBWorkSpace\GitHub\VukanLearnResources\materials\neox\b_f_3725\MI_hair_cards.json`
- `D:\YYBWorkSpace\GitHub\VukanLearnResources\materials\neox\b_f_3725\MI_hair_cards_clip.json`
- `D:\YYBWorkSpace\GitHub\VukanLearnResources\models\neox\b_f_3725\SM_b_f_3725_hair_core.json`

阶段门：近景没有双层亮边、黑边或 Core/Fringe 断层，远景 coverage 不发生突然跳变。

### P4：补齐 Ultra High Strand Variation

目标：补齐源 `QUALITY_SUPPORT_ULTRA_HIGH && PLAYERS_SELF` 分支，但不使用假噪声代替源资源。

1. 找到 NeoX `t_noise_high_freq` 对应源纹理。
2. 转换为 VulkanLearn 标准纹理资产 JSON。
3. 新增独立 Hair variation MF，或扩展 `mf_neoxHairInputs.glsl` 的明确子函数。
4. 按源顺序在 BaseColor 到 absorption 转换之前应用：
   - 高频噪声；
   - RDI.B strand ID 明度变化；
   - RDI.B 色相/饱和度变化。
5. 增加显式开关或强度参数，默认值必须对应目标槽位实际质量配置。
6. 不允许 RDI.B 影响 roughness、coverage、fiber axis 或阴影。

阶段门：Variation 关闭时回到 P3 基线；开启时只产生细微发束色差，不改变整体黑发身份。

### P5：固定机位视觉校准

目标：使用 MF/MI/场景参数对齐游戏截图，而不是修改 UE Hair 核心公式。

校准顺序：

1. Fiber Axis 与双面方向；
2. Core/Fringe Coverage；
3. Hair roughness 与 specular 输入；
4. Scatter 输入；
5. 环境、方向、局部与虚拟光倍率；
6. RDI.B Strand Variation；
7. 最后才单独比较 Tone Mapping。

每轮只修改一组参数，并保存 Beauty 与对应 Debug View。禁止同时修改 BaseColor、roughness、
灯光和 Tone Mapping，否则无法判断实际修复来源。

阶段门：达到第 7 节的视觉验收标准，并能明确说明每项差异由 MF、MI、Pass、场景光照
还是后处理负责。

## 5. Shading Model 修改准入条件

默认不修改：

- `shader/glsl/engine/hairScattering.glsl`
- `shader/glsl/engine/hairLighting.glsl` 中的 UE Hair lobe 数学部分。

只有满足以下全部条件才允许修改：

1. 有测试证明当前行为偏离 `documents/rendering/hair-shading-model.md`；
2. 问题也能在非 NeoX 的 `M_hair` 上复现；
3. 修复不包含 NeoX 贴图、RDI、槽位或材质名称；
4. 修复后普通 Hair 与 NeoX Hair 的合同测试都通过；
5. 修改原因记录为 UE Hair 合同修复，而不是 `b_f_3725` 截图调参。

以下情况不能作为修改 Shading Model 的理由：

- 单个角色高光过亮或过窄；
- NeoX 源参数无法一比一落入当前 MI；
- Core/Fringe 重复增亮；
- RDI.B 发色变化缺失；
- 场景曝光或 Tone Mapping 不同；
- 透明排序、ShadowDepth 或资产 Tangent 错误。

## 6. 测试与验证

### 6.1 静态与合同测试

1. JSON 解析：Material、MI、Texture、Scene、Mesh descriptor。
2. Shader 编译与反射：Base、ShadowDepth、Forward Transparent、Deferred。
3. Hair 合同测试：

```powershell
ctest --test-dir build -R hair_contract --output-on-failure
```

4. Python 资产脚本：

```powershell
py -3 -m py_compile tool/neox/create_bf3725_p0_mesh_asset.py
```

5. 完整构建：

```powershell
cmake --build build -j
```

### 6.2 运行时矩阵

| 场景 | 检查项 |
| --- | --- |
| 正面近景 | 宽灰高光、黑色基底、发束层次 |
| 侧面近景 | Fiber Axis 连续性、R/TRT 移动方向 |
| 背面 | 双面法线与背面漏光 |
| 镜像 UV 发片 | handedness 是否重复翻转 |
| 仅方向光 | Directional response 与阴影 |
| 仅点光 | Card geometric visibility |
| 仅聚光 | Cone attenuation 与背面抑制 |
| 仅环境光 | UE Hair indirect response，不引入普通 GGX 白带 |
| Core only | Alpha Clip、ShadowDepth、实心高光 |
| Fringe only | 透明发梢与排序 |
| Core + Fringe | 无重复亮边、无接缝 |
| Forward/Deferred | 标准 Hair 输入和最终亮度一致 |

## 7. 视觉验收标准

1. 无高光区域保持黑色，不整体泛白、泛灰或泛棕。
2. 主要高光为宽而柔和的灰色带，不是狭窄的白色塑料高光。
3. 高光沿发束方向移动，不沿发片宽度或错误 Tangent 方向移动。
4. 正反面和镜像 UV 发片不发生高光翻转或法线断裂。
5. Core 与 Fringe 组合后没有双倍高光、黑边、白边和透明断层。
6. RDI.B Variation 只增加细微发束差异，不改变整体 roughness 和 coverage。
7. Directional、Point、Spot 和 Virtual Light 不从卡片背面产生明显漏光。
8. 普通 `M_hair` 不因本次 NeoX 对齐发生视觉回归。
9. 最终结果能够在固定相机和固定曝光下稳定复现。

## 8. 非目标

本计划不包含：

- 新增或复制 NeoX Hair Shading Model；
- 用 NeoX `HairShading` 替换 UE 对齐 R/TT/TRT；
- 为单个角色修改公共 Hair Fresnel 或能量常数；
- 新增 strands/curves backend；
- 实现完整 Hair OIT 或深度不透明图；
- 把 Hable 持久化为项目默认 Tone Mapping；
- 修改源 BaseColor 纹理来掩盖光照问题；
- 直接编辑 `shader/spv/`；
- 清理或重置当前工作区中的其他未提交改动。

## 9. 推荐执行顺序

```text
P0 固定基线
 -> P1 MF 纹理/TBN/输入语义
 -> P2 NeoX 输入到 UE Hair 的映射
 -> P3 Core/Fringe 双 Pass
 -> 第一轮用户截图验收
 -> P4 Ultra High Strand Variation
 -> P5 最终视觉校准
 -> 构建、合同测试、运行时矩阵与文档收口
```

第一轮截图验收必须放在 P3 之后、P4 之前。这样可以先确认黑发、高光宽度和透明边缘的
核心问题已经解决，再决定是否需要补齐源 Ultra High 的细微发色变化。

## 10. 执行记录（2026-08-25）

本计划 P0–P5 已完成逐项核对与落地，验证结果如下。P5 使用固定头部机位和逐项
Debug View 截图完成验收；最终只把角色 Hair MI 的环境/虚拟光倍率写入资源生成器，
没有修改公共 Hair evaluator。

### 10.1 验证环境

- 构建：`cmake --build build -j` 通过（100%，0 失败）；
- 合同测试：`ctest --test-dir build -R hair_contract` 通过（1/1）；
- Hair 运行时矩阵：`build/bin/main.exe --hair-validation-test --exit-after-tests --no-dev-ui`
  通过（6 个固定场景 + 参数/几何 sweep + Debug View 21–41 全部可寻址）；
- 角色烟测：`build/bin/main.exe --initial-scene scenes/SC_b_f_3725_p0.json
  --framesmoke 3 --exit-after-tests --no-dev-ui` 通过（3/3 帧，`retiredPending=0`）；
- Python 资产脚本：`py -3 -m py_compile tool/neox/*.py` 通过。

完整 `ctest` 套件为 8/10 通过；失败的 `hair_lut_generator_contract` /
`hair_lut_generator_dispatch` 属于 `hair-shading-model-development-plan.md` 的 LUT
kernel v2 版本号未同步（`hairAssets.h` 已把 `lutVersion/kernelVersion` 提到 2，但
fixture 仍是 1），与本 NeoX Hair 对齐无关，且不在本计划第 6.1 节的测试范围内。

### 10.2 P1 核对结论（已与源合同逐项比对）

- P1.1 纹理采样：`mf_neoxHairTextures.glsl` 与源 `pbr_hair_transparent_nodes.hlsl` 一致。
  Tex0 源 sampler `MipLODBias=-1` 且 Bias 参数为 `0`（总 bias `-1`）；RDI.RGB 固定
  `SampleLevel(…,0)`；RDI.A 用真实 UV1 + `SampleBias(TEXTURE_BIAS=-1)`；Normal 源
  `MipLODBias=-1` 叠加 `SampleBias(-1)` 等价总 bias `-2`。当前实现均吻合。
- P1.2 Fiber Frame：已确认当前实现正确。通过解析 `b_f_3725_hair_core.gltf` 网格数据，
  平均 `|tangent · normalize(dP/du)| ≈ 0.997`、`|tangent · normalize(dP/dv)| ≈ 0.224`，
  且 `|dP/dv| / |dP/du| ≈ 2.14`，证明 glTF Tangent 沿 U（发片宽度轴）、Bitangent 沿
  V（root-to-tip）。NeoX 源把 `cross(normalTS, X)` 的 `.y`（≈ normal strength）落位到
  `tangent_to_world[1]`（源 Tangent，源约定即 root-to-tip）；VulkanLearn 用
  `cross(normal, tangent)` 得到 glTF Bitangent（root-to-tip）作为 fiber axis，是同一
  语义的等价补偿，不是轴交换 bug。双面只翻几何法线、handedness 只应用一次，均与源一致。
- P1.3 材质输入：`mf_neoxHairInputs.glsl` 与源一致。`HAIR_COLOR_MODE=NONE` 保持 Tex0
  原色；`u_root_intensity=0`、`u_depth_intensity=0` 不产生隐藏染色/压暗；Coverage、
  Opacity、OpacityMask 与 absorption 相互独立；BaseColor 到 absorption 只转换一次；
  RDI.B 不修改 roughness。

### 10.3 P2 / P3 核对结论

- P2 映射：`u_roughness=0.3`、`u_specular=0.3`、`u_scatter=0.5`、`u_specular_shift=0.0`、
  IOR `1.55` 已与 `b_f_3725_hair_core.gltf` 的 `neox_params` 逐项一致，且未改公共
  R/TT/TRT 公式。
- P3 Core/Fringe：Mode 8 双资源结构（`MI_hair_cards_clip.json` → `OpaqueClip` Core +
  `MI_hair_cards.json` → `TransparentAlphaBlend` Fringe）已核对，Core 用原始 Tex0.A、
  Fringe 用 `saturate(Tex0.A / 0.5)`，Coverage 来源与 clip threshold 一致。

### 10.4 P4 落地（默认关闭）

已按源 `QUALITY_SUPPORT_ULTRA_HIGH && PLAYERS_SELF` 分支补齐 Strand Variation：

- 噪声纹理：`tiling_noise_high_freq.tga`（64×64 灰度，WRAP/WRAP，linear）已转换为
  `textures/neox/b_f_3725/T_h_f_3725_hair_noise.json`；
- `M_neoxHair.json` 新增 `noiseMap` 纹理槽与 `u_hairStrandVariation`（默认 `0.0`）；
- `mf_neoxHairInputs.glsl` 新增 `ApplyMFNeoXHairStrandVariation` 与忠实移植的
  `MFNeoXHairRotateAboutAxis`，在 BaseColor 到 absorption 转换之前按源顺序应用
  高频噪声、RDI.B 明度变化与色相变化，RDI.B 不进入 roughness/coverage/阴影。

目标槽位来自 `*_high.mtg`（HIGH 档），源分支要求 `QUALITY_SUPPORT_ULTRA_HIGH`，因此
默认保持关闭（`u_hairStrandVariation=0.0`，即 P3 基线）。需要对齐 Ultra High 截图时，
在 `MI_hair_cards.json` / `MI_hair_cards_clip.json` 的 `parameters` 中加入
`"u_hairStrandVariation": 1.0` 即可开启；开启态已通过烟测验证（无崩溃、无 NaN）。
Strand Variation 的 GLSL 移植已用数值脚本与源 HLSL 公式逐点比对，误差为 0。

### 10.5 P5 视觉验收操作指南

P5 已在固定头部近景机位下完成对照验收。可重复执行：

```powershell
build/bin/main.exe --initial-scene scenes/SC_b_f_3725_p0.json --no-dev-ui
```

场景已保存 `position=[0,1.45,1.45]` 与 `look_at=[0,1.45,0]`。可直接保存 Beauty；
以下 `camera pose` 只用于显式复核当前姿态：

```text
camera pose 0 1.45 1.45 0 1.45 0
debugview 0
screenshot p5_beauty_front_close.bmp
```

本轮最终 Hair MI 参数为 `u_hairCharacterLighting=[0.25,1.0,0.55,0.25]`。
其中 `x` 环境倍率和 `w` 相机虚拟光倍率分别从初始 RenderDoc 量级
`[1.0,1.0,0.55,0.70588237]` 降低；`roughness=0.3`、`specular=0.3`、
`scatter=0.5`、BaseColor、Coverage、Core/Fringe 和 Tone Mapping 均未在该轮改变。

控制台命令：

- `debugview <mode>`：切换 Debug View；`screenshot [name.bmp]`：写
  `resourcePath/generated/screenshots/`；`tonemap <mode>`：0 Linear / 1 Reinhard /
  2 Hable / 3 ACES。

Hair Debug View 编号（对照第 7 节验收标准）：

| 编号 | 含义 | 验收标准对应项 |
| --- | --- | --- |
| 21 | Hair Frame（Bitangent） | 双面/镜像 UV 方向 |
| 22 | Hair Tangent（Fiber Axis） | 第 3 条：高光沿发束方向 |
| 25 / 26 / 27 | R / TT / TRT | 第 2 条：宽灰高光、非塑料窄高光 |
| 35 | Scatter | 第 1 条：无高光区保持黑色 |
| 6 / 30 / 31 | AO / Coverage / Shadow | 第 5、7 条：透明边缘、背面漏光 |

验收顺序按第 4 节 P5：先 Fiber Axis 与双面方向，再 Core/Fringe Coverage，然后
roughness/specular、scatter、灯光倍率，最后单独比较 Tone Mapping。每轮只改一组参数。
最终截图确认：无高光区保持黑色；高光沿 root-to-tip 发束方向形成宽而柔和的灰带；
Core/Fringe 没有重复亮边或透明断层；RDI.B variation 保持关闭，普通 Hair 合同未改动。
