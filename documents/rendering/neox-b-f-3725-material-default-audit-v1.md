# b_f_3725 角色材质默认值双源核验 V1

## 状态

- **状态**：已完成第二轮核验；目标 `M_*.json` 只修正已被双源明确证明错误的值，其余迁移常量保留并单独标记。
- **角色**：`b_f_3725`
- **源目录**：`K:\future\res\shader`
- **源规则**：`.fx` 是美术宏、参数声明和编辑器默认值；`.nfx2` 是总宏展开后的最终宏环境、Uniform 节点和最终默认值。
- **目标范围**：VulkanLearn `shader/glsl/M_*.json` 与 `VukanLearnResources/materials/neox/b_f_3725/MI_*.json`。

## 1. 源文件指纹

以下 SHA-256 用于保证本次核验不是按文件名猜测：

| Shader | `.fx` | `.nfx2` |
|---|---|---|
| Hair | `32b82baa3b5ba366359d194da573bdb4043166bb728e3c23afb67f1bf26aed88` | `cb874f2149c9627610be848535049f41a715ce93226b3dac5dbd176b2691b45d` |
| Skin | `e6e265d519134b3a1d27c19f4480cb34869a665bf3a09836f9a036e83922f28a` | `c1830ed5a59b6e7fdcb3f231b6cc769c4d71fb5ced3f5da2b434203157c0fffb` |
| Default | `a74140b5fb96c6b6854592ab453a7e38badcf0ff56e8226c3e005feb16998a4a` | `c09b34a3f9be7aaf6f9f244a60628c2fcb66853ae828f5a31f6efbbd821d6cdd` |
| Silk | `6dd25952e551cd7c91ad0ade5de14727d73f37e95b3a8f86ae8a906cc6de5a37` | `3a9c35df65a17c6184cedea04153e7b83f4208f15c5dec1253308479447a75a6` |
| Subsurface Billboard | `3272c954e9dfaf1339249c5ffa5d911963e97f48f812a849cc09704c1d6c1eb4` | `d8525fb96b617ba2055bc244872ce3afb1a74c04296a8d272f9f9c00a02c561b` |
| Crystal | `ef755a76bdeb94c1732e07a70afbd8db0715cf2a167f3a0d70fc8a9df594ae64` | `20290dbd17810ee88a6acf9d0f2542e25ddba34b026f798e60e127c1e9af22bb` |
| Eye | `29d988df3e29ba499b99aa1e8b26ba769492424589bac04ecefb5a9190d4019c` | `c7c3047d1befb99f43b503de7c6e431714b67922318c7e837cb6654462e6e92a` |
| Eye Edge / Simple | `c67839719c139089cf2238a172c6ba15bdd322257adaff80fae6a6e42a8630e2` | `43392a8c4318d5a46720b30f581f386867d919051978ca48e7786a33d34995a6` |

## 2. 最终宏核验

以下是 `nfx2` 中与角色迁移直接相关的最终值；公共引擎宏不在表内重复展开。

| Shader | 关键最终宏 |
|---|---|
| `pbr_hair_transparent` | `SHADINGMODELID=5`, `SHADER_QUALITY=2`, `HAS_2U=1`, `HAS_VERTEX_ALPHA=0`, `HAIR_COLOR_MODE=0`, `PEARL_COLOR_ENABLE=0`, `EMISSIVE_FLOW_ENABLE=0`, `NORMAL_MAP_ENABLE=1`, `IS_TRANSPARENT=1`, `HAS_TWO_SIDE=1` |
| `pbr_skin` | `SHADINGMODELID=3`, `SHADER_QUALITY=2`, `NORMAL_MAP_ENABLE=1`, `IS_TRANSPARENT=0`, `ALPHA_TEST_ENABLE=0`, `HAS_TWO_SIDE=0` |
| `pbr_default` | `SHADINGMODELID=1`, `SHADER_QUALITY=2`, `PBR_PARAM_TEX=1`, `NORMAL_MAP_ENABLE=1`, `IS_TRANSPARENT=0`, `USE_2U_MODE=0`, `SPARKLE_ENABLE=0`, `ALPHA_TEST_ENABLE=0` |
| `pbr_silk` | `SHADINGMODELID=8`, `SHADER_QUALITY=2`, `PBR_PARAM_TEX=1`, `NORMAL_MAP_ENABLE=1`, `IS_TRANSPARENT=0`, `USE_2U_MODE=0`, `SPARKLE_ENABLE=0`, `ALPHA_TEST_ENABLE=0` |
| `pbr_subsurface_billboard` | `SHADINGMODELID=2`, `SHADER_QUALITY=2`, `NORMAL_MAP_ENABLE=0`, `PBR_PARAM_TEX=0`, `IS_TRANSPARENT=0`, `ALPHA_TEST_ENABLE=0` |
| `pbr_crystal` | `SHADINGMODELID=2`, `SHADER_QUALITY=2`, `NORMAL_MAP_ENABLE=0`, `IS_TRANSPARENT=0`, `TWO_SIDE_ENABLE=1`, `ALPHA_TEST_ENABLE=0` |
| `pbr_eye` | `SHADINGMODELID=7`, `SHADER_QUALITY=2`, `NORMAL_MAP_ENABLE=1`, `IS_TRANSPARENT=0`, `HAS_TWO_SIDE=0` |

Hair 的 `.fx` 美术宏默认还明确给出 `VERTEX_ANIMATION_ENABLE=FALSE`、`HAS_2U=TRUE`、`HAS_VERTEX_ALPHA=FALSE`、`HAIR_COLOR_MODE=HAIR_COLOR_NONE`、`PEARL_COLOR_ENABLE=FALSE`、`EMISSIVE_FLOW_ENABLE=FALSE`、`BATCH_SKINNED_MESH=FALSE`。当前 Hair M_ 的 `u_hairAuthoring.x=0` 保持了 `HAIR_COLOR_MODE=NONE` 的源行为；`u_hairScattering.y=0.5` 只作为辅助 Backlit 输出参数，不能把 `u_root_intensity=0.0` 当成 R/TT/TRT 主路径的关闭 gate。

## 3. 参数默认值

### Hair：`pbr_hair_transparent.fx/.nfx2`

| 源参数 | `.fx` / `.nfx2` 默认值 | 目标状态 |
|---|---:|---|
| `u_roughness` | `0.3` | `M_neoxHair.u_hairScattering.z=0.3`，已对齐 |
| `u_scatter` | `0.5` | `M_neoxHair.u_hairScattering.x=0.5`，已对齐 |
| `u_specular` | `0.3` | `M_neoxHair.u_hairSpecular=0.3`，已对齐；不能使用普通默认 `0.5` |
| `u_root_intensity` | `0.0` | 当前写入辅助 Backlit 输出；不作为 R/TT/TRT 主路径 gate |
| `u_depth_intensity` | `0.0` | 当前没有独立目标输入，记录为未迁移源功能 |
| `u_vertex_opacity_bias` | `3.0` | 当前宏 `HAS_VERTEX_ALPHA=0`，不生效 |
| `u_vertex_opacity_control` | `1.0` | 当前宏 `HAS_VERTEX_ALPHA=0`，不生效 |
| `u_shadow_boost` | `1.0` | 当前 ShadowDepth 未建立独立源参数槽，记录为待专项 |
| `u_two_pass_clip_value` | `0.5` | `M_neoxHair.u_alphaClipThreshold` 已从 `0.2` 修正为 `0.5` |
| `u_normal_strength` | `1.0` | `u_hairNormal.x=1.0`，已对齐 |
| `u_specular_shift` | `0.0` | 当前目标未单独暴露，按源默认保留零差异 |
| `u_ao_from_normal` | `1.0` | 当前 RDI.A/RDI.G AO 合同不再重复使用该源分支，记录为差异 |
| `u_alpha_length` | `1.0` | 当前 `HAS_2U=1`，源条件不启用 |
| `u_opacity` | `1.0` | Blend Pass 由 coverage/阈值组合恢复，已保留源基线 |
| `u_backlit_occlusion_tip` | `2.0` | 目标 Hair evaluator 暂不直接消费，记录为待专项 |
| `u_backlit_intensity` | `0.5` | `u_hairScattering.y=0.5`，仅保留辅助 Backlit 输出语义 |

### 其他 Shader 家族的已解析源默认

| Shader | `.fx/.nfx2` 已确认默认 | 目标说明 |
|---|---|---|
| Skin | `u_skin_color=(1,1,1,1)`, `u_skin_bright=1`, `u_skin_gray_intensity=0`, `u_skin_rebirth_intensity=0`, `u_skin_roughness=0`, `u_skin_specular=0.5`, `u_skin_detail=1`, `u_detail_tilling=60`, `u_curvature_curve=1`, `u_curvature_intensity=1`, `u_skin_opacity=1` | `M_neoxSkin` 的颜色、亮度、roughness offset、specular、detail strength/tiling 和 curvature 默认已对齐；thickness、transmission、LUT 与角色光照属于目标 `PreintegratedSkin` 迁移常量 |
| Default | `u_emissive_strength=5`, `u_sparkle_tiling=1`, `u_sparkle_intensity=1`, `u_sparkle_speed=0`, `u_pattern_2u_roughness=0.5`, `u_wrap_2u_opacity=1`, `u_specular=0.5`, `u_roughness=0.5`, `u_metallic=0` | 当前 `M_neoxDefault` 的 emission/sparkle 打包值是目标迁移常量；源 `EMISSIVE_MODE=0`、`SPARKLE_ENABLE=0`，不能把目标常量误称为源参数默认 |
| Silk | `u_emissive_strength=5`, `u_sparkle_tiling=5`, `u_sparkle_intensity=10`, `u_sparkle_speed=1`, `u_pattern_2u_roughness=0.5`, `u_wrap_2u_opacity=1`, `u_specular=0.8` | 当前 `M_neoxSilk` 保留 Cloth 对齐和静态遮罩；`u_neoxEmission`/`u_neoxSparkle` 属于有审计的迁移打包，不改 UE Cloth 光照 |
| Subsurface Billboard | `u_contract=0.75`, `u_fresnel=5`, `u_brightness=1.5`, `u_emissive_amount=0`, `u_roughness=0.2`, `u_metallic=0`, `u_uv_grid=2`, `u_scatter=1`, `u_thickness=0`, `u_subsurface_color=(0.5,0.02,0.1,1)` | `M_neoxPearl.u_pearlSurface=(0.75,5,1.5,0)` 与 `u_pearlPbr=(0.2,0,2,0)` 已直接对齐源默认；Subsurface shape/weight 仍是 UE 目标闭包迁移常量 |
| Crystal | `u_base_roughness=0.2`, `u_crystal_roughness=0.04`, `u_base_metallic=0`, `u_crystal_metallic=0`, `u_base_specular=0.5`, `u_crystal_specular=0.6`, `u_detail_tilling=1`, `u_detail_intensity=1`, `u_refraction_brightness=1`, `u_refraction_opacity=0`, `u_emissive_strength=0` | M_ 已分别保留 Base/Crystal 双层 PBR 默认；角色颜色、PBR、Detail tiling、折射、Subsurface 与 caustic brightness 由 MI 的 MTG 显式值覆盖。ThinTranslucent、Caustic RGB 和未接入的 SceneColor 折射仍是目标近似 |
| Eye | `u_eye_emissive_intensity_1=0`, `u_eye_emissive_intensity_2=0`, `u_eye_emissive_color2=(0,0.38,1,1)`, `u_shadow_top_boundary_rotation=0.04`, `u_shadow_top_boundary_offset=0.98`, `u_shadow_boundary_curvature=0.55`, `u_shadow_softness_max=0.66`, `u_shadow_softness_min=0.23` | 目标 EyeGeometry/caustic 参数属于迁移映射；源眼球宏为 `SHADINGMODELID=7`、`NORMAL_MAP_ENABLE=1`，已对齐模型边界 |
| Eye Edge / Simple | `u_roughness=0.05`, `u_metallic=0`, `u_specular=0.5` | 当前 `MI_eye_edge.u_pbrFactors=(0.6,0,1,0)` 是透明眼边的目标迁移常量，不是 `pbr_simple` 源默认 |

### 参数来源分类结论

| 材质族 | `sourceDefault` | 槽位显式源值 | `migrationConstant` | `unsupported` / 有意差异 |
|---|---|---|---|---|
| Hair | roughness、scatter、specular、backlit、normal strength、clip | MTG 没有额外 `u_*`；Core/Fringe 继承 M_ 默认 | coverage、角色光照和 `u_hairScattering.w` 为 P5 视觉校准 | depth intensity、独立 shadow boost、backlit occlusion tip、旧 AO-from-normal 分支 |
| Skin | color、bright、roughness offset、specular、detail、curvature | Face 的颜色/亮度/roughness/surface 数值来自源运行抓帧，不是 MTG ParamTable；Body 无参数覆盖 | thickness、transmission、LUT、角色光照 | makeup/glitter、wound/rebirth 等未启用源分支 |
| Default / Silk | Default specular `0.5`、Silk specular `0.8` | AlphaRef 和各 Silk/Sparkle 数值来自 MTG；Hair Default sparkle 也来自 MTG | 禁用 emission/sparkle 的母材质基线、Cloth sheen | 源动画时钟、完整 2U/流光动态分支 |
| Pearl | contract/fresnel/brightness/emissive/roughness/metallic/UV grid | Body/Hair Pearl 的六个 ParamTable 值和 AlphaRef 已写入 MI | UE Subsurface shape、color weight | Billboard 朝向、Velocity/TAA、depth offset |
| Crystal | Base/Crystal 双层 roughness/metallic/specular、Detail 默认、refraction brightness、emissive 零基线 | 三个 Crystal 槽的颜色、双层 PBR、Detail tiling、折射、Subsurface、caustic brightness 已写入 MI；Red Clip roughness 已恢复源默认 `0.2` | 母材质中性颜色、Caustic RGB 与 UE ThinTranslucent 接线 | SceneColor 折射、旋转/contrast/mipmap/opacity、caustic tiling/depth 和源双闭包最终光照 |
| Eye / Eye Edge | Eye 白色 iris/sclera 与零 emissive 有效行为 | Eye 的 `u_pipil_scale=0.61` 已折算为 pupil radius；`u_iris_range=5.85` 由目标几何近似承接 | Eye 其余 geometry/caustic 与 Eye Edge roughness `0.6` | MatCap、自定义 cube IBL 和 NeoX 眼部阴影曲线 |

### MI 与转换器复核

- 当前 `b_f_3725` 共 `20` 个 MI；Plain Silk 因源 Cull 合同不同拆为两份。逐个按父 `M_*.json` 复核后，未知参数为 `0`，与父材质默认完全相同的冗余覆盖为 `0`。
- `convert_bf3725_character.py` 过去会给 Hair Core/Fringe 重新写入与 `M_neoxHair` 相同的 clip 和 strand variation；本轮已改为继承母材质默认。
- `convert_bf3725_p0.py` 过去会给 Body Skin 重新写入与 `M_neoxSkin` 相同的角色光照向量；本轮已删除该冗余覆盖。
- `MI_b_f_3725_body_p0.json` 与 `MI_b_f_3725_high_1_pearl.json` 仍由两个专项转换器生成；全角色转换器引用它们但不会重建，本轮已增加写 Mesh 前的缺失检查。
- `parse_materials()` 当前只负责槽位、合同哈希和迁移清单；多数 MI 参数仍在 `build_instances()` 中显式写出。MTG 变更后必须重新执行本审计，不能把“槽位数量未变”视为参数仍有效。

### Crystal 第二轮结构修复

- `crystalMaskMap` 已恢复源 `R=Crystal 层遮罩`、`G=thickness`、`B=AO`、`A=coverage`；coverage 不再误读 BaseColor.A。
- `baseColorMap.A` 已作为 roughness 语义单独生成 Crystal 资产，不再复用把 A 标成 opacity 的通用 BaseColor 描述。
- `M_neoxCrystal` 新增 Crystal 层 roughness/metallic/specular、Detail tiling/intensity 和独立 Detail 贴图输入；MF 按同一层遮罩组合 Base/Crystal PBR。
- `M_neoxCrystal.surface.glsl` 现已显式构造 `MFNeoXCrystalInput`；材质参数 wiring 留在 M_，MF 不再隐式读取 Crystal 业务参数。
- Red Clip 使用源默认 roughness `0.2` 与公共 `crystal_bump_n` Detail；Red Opaque/Gold 使用各自 MTG 的 Crystal metallic/specular，并保留角色绑定的 Body DetailMap。
- mode 3 通过非零 `u_alphaClipThreshold` 在 MF 内二值化源 coverage；mode 4 保留连续 coverage。当前引擎仍不允许 `ThinTranslucent` ShadingModel 与 `OpaqueClip` RenderMode 直接组合，此处是已记录的目标边界。

### 本轮验证结果

- `cmake --build build -j`：通过。
- `thin_translucent`、`subsurface_contract`、`hair_contract`、`material_schema_contract`、`texture_asset_contract`：`5/5` 通过。
- `SC_b_f_3725_p0`：`--framesmoke 1 --exit-after-tests --no-dev-ui` 通过，World/Graph 事务提交成功。
- 两个 Crystal Material SPIR-V 变体均已重新生成，debug 组合源码包含 `MFNeoXCrystalInput`、`u_crystalLayerPbr`、`u_crystalDetail` 与 `detailNormalMap`。
- 四张 Crystal 离线资产逐像素对照源 TGA，RGBA 完全一致；当前 `20` 个 MI 未发现未知参数或冗余默认覆盖。

## 4. 验证与发布

### 4.1 本轮验证方式

本角色后续参数迭代优先使用热重载：`M_*.json`、角色 `MI_*.json`、Material Function 和可热更 Shader 源先通过 `shaderreload changed` 或 FileMonitor 验证；只有启动配置、设备初始化、不可热更的 RenderState/Pass 拓扑或资源路径变化才重启。Hair Core/Fringe 两份资源必须在同一轮热更提交摘要中确认成功，不能只热更其中一份后判断效果。
### 4.2 Hair Normal 与高光核验

- `T_h_f_3725_cards_Normal` 的来源是 `K:\future\res\character\players2021\b_f_3725\textures\h_f_3725001n.tga`；目标描述为线性、`R=normal.x`、`G=normal.y`、`B=normal.z`、`A=opaque`。
- 离线转换逐像素保留源 `R/G`，按 `Z=sqrt(max(0,1-X^2-Y^2))` 重建 `B`；本次复核结果为 `R/G` 全量一致、重建 `B` 最大误差为 `0`、`A` 全部为 `255`。
- 源 `.fx` 与目标 MF 都使用 NormalMap 的 XY、`u_normal_strength=1.0` 补 Z，再通过 `cross(normal, X)` 推导发丝轴；当前没有发现 R/G/B 反转、Y 翻转或切线基底交换。
- 截图中的高光不是法线贴图缺失或通道错位造成的。当前 Hair 使用源默认 `u_roughness=0.3`、`u_specular=0.3`，目标 UE-compatible R lobe 还按 `specular * 2` 计算；在当前 Key/Fill 光照下，高光峰偏集中、局部接近白色，属于光照能量/峰宽核验项，不应先改 Normal 贴图或伪造源默认。
- 进一步对照 `pbr_hair_simple.fx/.nfx2` 后确认：源三条纵向宽度为 `0.2 + [1, 0.5, 2] * u_roughness^2`。目标此前遗漏 R/TT 的基础 `0.2`，在 `u_roughness=0.3` 时会把主高光和透射峰压得过窄，分别表现为尖白和干硬；已在 `hairScattering.glsl` 修正并通过运行时 Shader 热更重新编译。
- 当前修正只恢复源峰宽基线，并移除错误的 Backlit→R/TT 能量 gate；不改变 `u_specular=0.3`、Normal 贴图、Shading Model 或两份 Hair MI。全路径 `2π` 放大已撤回，因为当前 Vulkan 光源 radiance 合同下会把发色直接冲白。
### 4.3 发布规则

1. MI 显式 ParamTable 值优先于 M_ 默认；本角色已有的 AlphaRef、Pearl、Silk、Crystal 数值不能被母材质默认覆盖。
2. M_ 默认值只允许来自本表的源默认映射，或有明确说明的 `migrationConstant`；不得在 MF、Shading Model 或 Pass 再偷偷补业务默认。
3. `.fx` 和 `.nfx2` 的 SHA-256 变化后，本表必须重新生成；不能沿用旧审计结果。
4. 当前未同步 `D:\YYBWorkSpace\GitHub\yyb-knowledge-book`；本次规则只记录在 VulkanLearn。
