# Subsurface 三类 Shading Model 开发计划完成记录

## 状态

- 类型：实现完成、阶段验收记录
- 验收状态：基础合同测试、runtime smoke、视觉检查和性能 baseline 已记录；GPU 数值 reference、temporal/遮挡专项验证和可重放视觉断言仍未闭环
- 完成日期：2026-08-22
- 覆盖范围：`Subsurface` / 2、`PreintegratedSkin` / 3、`SubsurfaceProfile` / 5
- 当前实现合同：`documents/rendering/subsurface-shading-models.md`
- UE 对齐基线：UE 5.8 Legacy Material Shading Model 的执行边界与材质语义
- 最终执行域：Opaque Deferred
- 核心约束：DefaultLit 公式与外观保持不变，只做无损 lighting-lobe 拆分
- 预计算约束：profile table 与 skin LUT 全部由 Compute Shader 生成，不保留 CPU 数值生成逻辑
- 注释约束：新增/修改的非显然 C++/GLSL 逻辑带必要中文设计注释，说明所有权、同步、能量和性能取舍

本文保留本轮工作的目标、完成项、验收证据和未采纳决策。当前运行时合同已经迁移到
`documents/rendering/subsurface-shading-models.md`；后续实现应以正式合同为准，而不是把
本文件重新当作待执行路线图。

## 1. 完成结果

| Shading Model | ID | 已实现路径 | 状态 |
| --- | ---: | --- | --- |
| `Subsurface` | 2 | local wrap + backscatter + thickness transmission | 完成 |
| `PreintegratedSkin` | 3 | Compute 生成二维 LUT，local direct/IBL lookup + transmission | 完成 |
| `SubsurfaceProfile` | 5 | Compute 生成 profile table，全分辨率双向 filter + composition | 完成 |

三类模型保持独立：

- ID 2 不读取 LUT 或邻域像素；
- ID 3 不读取邻域像素；
- ID 5 只过滤 diffuse，不复用 ID 2 的 local closure；
- 三类模型拥有独立 Material Inputs、GBuffer 数据与 debug 语义；
- ID 2、3、5 没有被合并成一个含糊 evaluator。

## 2. 最终架构决策

### 2.1 DefaultLit 保持只读

Deferred lighting 被整理为：

```text
diffuseLighting
nonDiffuseLighting
transmissionLighting
finalColor = diffuseLighting + nonDiffuseLighting + transmissionLighting
```

DefaultLit 的 direct diffuse、direct specular、diffuse IBL、specular IBL、AO 与 shadow
公式没有改写。`subsurface_contract` 测试验证拆分后的 DefaultLit 分量重组仍满足原结果。

### 2.2 Compute-only Lookup 生成

最终没有保留最初计划中的 CPU profile/LUT 生成路线：

```text
CPU: parse + validate + serialize authored parameters
GPU: evaluate profile + normalize taps + evaluate LUT + write RGBA16F images
```

唯一数值生成入口：

- `shader/glsl/generator/subsurfaceLookupTables.comp`

CPU 端只负责：

- JSON 解析和一次性范围校验；
- stable ID 与重复项校验；
- 参数 SSBO 序列化；
- Compute pipeline、descriptor、image 和 barrier 管理；
- World-local 资源生命周期。

以下 CPU 逻辑已明确不保留：

- radial profile evaluator；
- tap weight 生成与归一化；
- skin response evaluator；
- skin IBL average evaluator；
- `HostImage` lookup 生成或上传 fallback。

### 2.3 显式能量分配

三类模型统一使用：

```text
transmissionWeight = subsurfaceWeight * authoredTransmissionWeight
reflectedFraction = 1 - transmissionWeight
```

Diffuse 反射能量先预留 transmission 份额；specular、emissive 和 transmission 始终位于
ID 5 profile blur 之外。

## 3. 分阶段完成记录

### Phase 0：合同冻结

- [x] 固定 Shading Model ID 2、3、5；
- [x] 固定三类 Material Model Inputs；
- [x] 固定 stable profile ID `1..255`；
- [x] 固定 stable skin LUT ID `1..15`；
- [x] 固定 profile 与 skin LUT schema/version；
- [x] 固定 mm/cm/m 到世界单位的换算合同；
- [x] 固定 GBuffer packing；
- [x] 固定 DefaultLit lobe 重组与 transmission 能量合同；
- [x] 固定“不保留 CPU lookup 数值生成”的实现边界。

### Phase 1：GPU Lookup 资源链路

- [x] 新增 `SubsurfaceProfileAsset` 与 `PreintegratedSkinLutAsset` 解析/校验；
- [x] 新增 World-local `SubsurfaceResourceSet`；
- [x] source digest 包含资产内容与 generator shader 内容；
- [x] 相同 digest 复用上一 World generation 的资源包；
- [x] 新增 Compute generator pipeline；
- [x] 生成 `14 x 256`、`RGBA16F`、nearest profile table；
- [x] 生成 `128 x 1024`、`RGBA16F`、linear skin LUT atlas；
- [x] 资源在 material 加载前准备；
- [x] 候选 World 通过既有 transaction 一次性发布；
- [x] 生成失败不回退 CPU。

### Phase 2：SubsurfaceProfile / ID 5

- [x] 新增 `M_subsurfaceProfile` 材质定义与 Surface Evaluation；
- [x] MI 使用 `subsurfaceProfile` asset path；
- [x] loader 解析并重新应用派生 `u_subsurfaceProfileId`；
- [x] GBufferD 保存 `profileId, weight, thickness, transmissionWeight`；
- [x] deferred lighting 输出 diffuse/non-diffuse/transmission 三路；
- [x] 新增 `sssHorizontal` 与 `sssVertical`；
- [x] 使用 13-tap separable profile；
- [x] 完成 screen bounds、depth、normal、shading model、profile ID rejection；
- [x] 完成 RGB 独立有效权重归一化；
- [x] 完成世界半径到像素半径换算；
- [x] 新增 `sssComposition`；
- [x] 只过滤 diffuse，保持 specular/emissive/transmission 锐利；
- [x] 完成全分辨率 `R16G16B16A16_SFLOAT` 路径。

### Phase 3：Subsurface / ID 2

- [x] 新增 `M_subsurface` 材质定义与 Surface Evaluation；
- [x] 新增独立 local wrap/backscatter evaluator；
- [x] direct 与 indirect diffuse 分别参与 local closure；
- [x] thickness transmission 单独输出；
- [x] GBufferA/D/F 完成专用 packing 与 decode；
- [x] `weight == 0` 保持 DefaultLit diffuse；
- [x] 不读取 ID 5 profile buffer。

### Phase 4：PreintegratedSkin / ID 3

- [x] 新增 `M_preintegratedSkin` 材质定义与 Surface Evaluation；
- [x] MI 使用 `skinLut` asset path；
- [x] loader 解析并重新应用派生 `u_skinLutId`；
- [x] 固定二维 `(N·L, thickness)` LUT 域；
- [x] 支持 `scatteringMultiplier` 与 `finalDiffuseResponse`；
- [x] Compute Shader 生成每个 tile 的 response 与 metadata；
- [x] direct light 使用 LUT response；
- [x] IBL 使用 Compute 生成的 average response metadata；
- [x] transmission 使用 LUT transmission metadata；
- [x] GBufferD/F 完成专用 packing 与 decode；
- [x] V1 加载期要求 `curvature == 0`；
- [x] 不读取邻域像素。

### Phase 5：集成、调试与收敛

- [x] 新增三类 shading model dispatch；
- [x] 新增 Render Graph lighting lobe resources；
- [x] 新增三项 SSS pass material；
- [x] 新增 Debug View 12..20；
- [x] UI、控制台和本地化同步；
- [x] 新增三类测试材质与球体场景；
- [x] 新增 profile/LUT authoring 样例；
- [x] 新增 `subsurface_contract` 自动测试；
- [x] 完成全量 CTest、短帧 smoke、120 帧 baseline 和视觉检查；
- [x] 当前合同迁移到 `documents/rendering/subsurface-shading-models.md`；
- [x] 知识库同步实际实现状态与 Compute-only 边界。

## 4. 实际文件落点

### 4.1 Engine 与资源

- `source/render/subsurface/subsurfaceAssets.*`
- `source/render/subsurface/subsurfaceGBufferCodec.*`
- `source/render/subsurface/subsurfaceMaterialContract.*`
- `source/render/subsurface/subsurfaceResourceLoader.*`
- `source/render/subsurface/subsurfaceResourceSet.*`
- `source/pipeline/subsurfaceLookupTableGenerator.*`
- `source/render/resource/rendererMaterialLoader.cpp`
- `source/render/resource/rendererResourceLoadCoordinator.cpp`
- `source/render/resource/rendererResourceCache.*`

### 4.2 Shader

- `shader/glsl/M_subsurface.*`
- `shader/glsl/M_preintegratedSkin.*`
- `shader/glsl/M_subsurfaceProfile.*`
- `shader/glsl/generator/subsurfaceLookupTables.comp`
- `shader/glsl/engine/subsurfaceLighting.glsl`
- `shader/glsl/engine/preintegratedSkinLighting.glsl`
- `shader/glsl/engine/subsurfaceProfileLighting.glsl`
- `shader/glsl/engine/subsurfaceProfileFilter.glsl`
- `shader/glsl/pass/sssHorizontal.*`
- `shader/glsl/pass/sssVertical.*`
- `shader/glsl/pass/sssComposition.*`

### 4.3 Render Graph、UI 与测试

- `config/renderGraphConfig.json`
- `shader/glsl/pass/M_deferredLighting.json`
- `source/ui/uiSubsystem.*`
- `source/debugConsole.cpp`
- `ui/runtime-control.rml`
- `ui/localization.json`
- `tool/subsurface-tests/`

### 4.4 Runtime 资产

位于 `<resourcePath>`：

- `subsurfaceProfiles/SSP_skin.json`
- `subsurfaceProfiles/SSP_wax_centimeter.json`
- `subsurfaceProfiles/SSP_marble_meter.json`
- `subsurfaceProfiles/SSP_skin_preview.json`
- `skinLuts/PSL_skin.json`
- `skinLuts/PSL_skin_multiplier.json`
- `materials/MI_subsurface_test.json`
- `materials/MI_preintegrated_skin_test.json`
- `materials/MI_subsurface_profile_test.json`
- `materials/pass/MI_sssHorizontal.json`
- `materials/pass/MI_sssVertical.json`
- `materials/pass/MI_sssComposition.json`
- `models/subsurface/`
- `scenes/SC_subsurface_models.json`

`SSP_skin_preview.json` 只用于让 demo 场景中的屏幕半径明显可见；物理尺度样例
`SSP_skin.json` 仍保留 1.2 mm MFP distance。

## 5. 验收证据

### 5.1 Build 与自动测试

```powershell
cmake --build build -j 2
ctest --test-dir build -j 1 --output-on-failure
```

结果：

```text
100% tests passed, 5/5
subsurface_contract passed
```

专项测试覆盖：

- profile/skin schema 与版本；
- 固定 LUT 分辨率与 output mode；
- mm/cm/m 单位等价；
- stable ID 边界；
- 三类 GBuffer round trip；
- DefaultLit component 重组；
- CPU profile/LUT 生成符号不存在；
- Compute generator 与归一化合同存在；
- profile filter 与 composition 的离散 GBuffer sampling；
- Debug View 16 对 ID 5 的黑色语义。

### 5.2 Runtime Smoke

```powershell
.\build\bin\main.exe --framesmoke 2 --exit-after-tests
.\build\bin\main.exe --initial-scene scenes/SC_subsurface_models.json --framesmoke 3 --exit-after-tests
```

两条命令均通过，覆盖 shader cache、Compute lookup 生成、资产加载、三种材质、
Render Graph、World transaction 和帧提交。

### 5.3 视觉检查

检查文件：

- `build/subsurface-full.png`
- `build/subsurface-diffuse-after.png`
- `build/subsurface-pixel-radius.png`
- `build/subsurface-valid-weight.png`

结果：

- 三个模型在测试场景中正常显示；
- Full 模式下 specular highlight 未被 profile blur；
- Diffuse After SSS 与最终 composition 分离；
- Pixel Radius 仅对 ID 5 非零；
- Valid Weight 仅对 ID 5 非零。

本轮没有完成 path-traced BSSRDF reference 对比，因此不将其列为已通过项。

### 5.4 本地性能记录

120 帧 `framesmoke`：

| Scene | avgFrameMs | minFrameMs | maxFrameMs | avgRenderLoopMs |
| --- | ---: | ---: | ---: | ---: |
| `SC_subsurface_models.json` | 2.431223 | 1.427300 | 39.469500 | 1.490060 |
| `SC_car_showcase.json` | 3.469403 | 2.426000 | 37.252400 | 2.569046 |

这些数据只作为当前机器的 smoke baseline。两个场景复杂度不同，不能据此推导 SSS pass
净开销；精确性能分析需要独立 GPU timestamp breakdown。

## 6. 明确未采纳的分支

以下内容没有进入本次实现：

- checkerboard SSS；
- half-resolution SSS；
- Burley + TAA；
- object ID rejection；
- transparent profile convolution；
- ray-traced diffusion；
- 3D skin LUT；
- CPU profile/LUT reference generator；
- CPU lookup image fallback；
- path-traced reference 自动对比。

未采纳不代表这些方向无价值，而是当前全分辨率、13-tap、depth/normal/model/profile
rejection 路线已经满足本轮完成定义。新增分支必须先更新正式合同和 Render Graph 成本评估。

## 7. 完成定义

- [x] ID 2、3、5 均具有独立且可运行的 evaluator；
- [x] DefaultLit 拆分不改变原公式与重组结果；
- [x] profile/LUT 资产具有 schema、版本、单位和 stable ID；
- [x] lookup 数值生成全部位于 Compute Shader；
- [x] CPU 预计算逻辑与 CPU fallback 不存在；
- [x] 非显然 C++/GLSL 边界已补充必要中文设计注释；
- [x] 三类 GBuffer packing 可 round trip；
- [x] ID 5 只过滤 diffuse；
- [x] profile/transmission 能量分配显式；
- [x] World/Graph 资源发布保持事务化；
- [x] Debug Views、测试场景和自动测试齐备；
- [x] Build、CTest、runtime smoke、视觉检查和性能 baseline 已记录；
- [x] 正式实现合同和知识库已同步。

### 7.1 尚未闭环的最终验收项

- [ ] path-traced BSSRDF 或独立 GPU 数值 reference 对比；
- [ ] temporal、动态遮挡和屏幕边缘的专项回归；
- [ ] 可重放视觉断言；
- [ ] 外部资源仓库提交固化。

本计划的实现主路径于 2026-08-22 完成；以上项目属于最终验收和持续验证，不将基础 smoke 结果误写为数值正确性证明。

## 8. 后续整改记录

2026-08-22 根据架构审查补充以下整改：

- [x] profile filter 与 composition 对 GBuffer、depth 和 lighting source 使用离散 texel 读取；
- [x] Compute lookup 生成器补充 image、buffer、descriptor pool、sampler 和 command buffer 的失败清理；
- [x] candidate Compute pipeline 不再写入 active PipelineFactory cache；
- [x] 发布后的 `SubsurfaceResourceSet` 通过 `shared_ptr<const ...>` 消费；
- [x] single-time submit 使用专用 fence，不再把正常路径扩大为 graphics queue `waitIdle`；
- [x] 补充 public struct 职责注释、资源生命周期注释、离散采样合同测试和 Debug View 16 合同测试。

外部资源仓库的提交固化、GPU 数值 reference、temporal/遮挡专项验证和可重放视觉断言仍属于
后续验收工作，不因本节整改而宣称已经完成。
