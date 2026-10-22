# TressFX 5.0 for UE 5.4 源码盘点与 Port Map（初稿）

状态：M0.5 初步盘点（2026-10-19）；CPU 数据层与 Female 保存几何的正式静态关卡已落地，
完整逐文件审计、绑定/贴图与 Marschner 验收未完成。这是 `plan/` 层，不是契约；
完整计划见 [tressfx5-hair-port-plan.md](tressfx5-hair-port-plan.md)。

## 2026-10-22 实施补充

- 参考仓库已关闭 sparse checkout，并同步确认该分支没有 `Asset/Project/UE544TFX`。
  下文的 sparse 清单是 2026-10-19 盘点时状态，不是当前检出限制。
- `.tfx` exporter 实写 version **4.0**；position 是每点 float4，w 是逆质量。
  header 的 `FLOAT4[numHairStrands]` 注释不准确，读取以 exporter 和 loader 的乘积为准。
- 可选 UV offset 为 0 时表示缺失；原 loader 无条件 Seek(0) 的行为不沿用。
- 静态资产不继承原 loader 的 64-guide 补齐；真实 guide 数、坐标轴和 UV 原样保留。
- `.tfxbone` exporter/loader 使用 count/name/weights 顺序流，未使用同文件声明的
  `TressFXTFXBoneFileHeader`；本批不实现 bone 或 collision 数据读取。
- 已加入 `.tfx` loader、静态 tangent/rest-length/ribbon 索引和只读检查工具。
  当前可依赖行为以 [CPU 导入契约](../../contracts/tressfx-asset-import.md) 为准。

### 第二批实施补充：静态渲染

- `source/mesh/loader/hair/tressfxModelImporter.*` 把 CPU guide 转为共享 Vertex
  中心线/切线/有符号半径与索引流，通过正式 `SM_* type: tressfx` 加载。
  本批不采用下文候选 `groomAsset`/专用组件，也不另建场景作者路径。
- patch 的 `TressFXVertexFactory.ush::ComputeViewAlignedWorldPosition` 对应
  `shader/glsl/engine/tressfxVertexFactory.glsl`；其 UE PositionBuffer 已预变换，
  VL 则先应用对象变换。两边均使用统一 ViewForward 叉积；VL 增加平行视线处理，
  暂不迁移必须搭配覆盖率的最小像素半径。注释与 AMD MIT 来源一并保留。
- `hair/M_strandProbe` 在既有 forwardTransparent 中绘制双面 Unlit ribbon。
  这没有移植 `TressFXRendering`、Composition、Marschner、DeepShadow、OIT、
  模拟或 LOD，也不说明 UE deferred hooks 已经复刻。
- 真实 Vulkan 已检查正面和转相机两张合成 guide 截图：
  `artifacts/tressfx5-static/Resources/Generated/Screenshots/`；
  49 项 hair、16 项 material 测试全部通过，UI 全量 681 项中 676 通过、
  5 项符号链接环境跳过；三项既有 Vulkan 门禁串行 exit 0。
  真实女性 `.tfx` 仍待导出。

### 第三批实施补充：Female 保存几何

- `tool/hair/import_female_scene.py` 受限解析并校验固定 `.ma`，依赖 SciPy
  B-spline 求值，不执行 Maya/XGen/蒙皮图。TFX 存真实 guide 的参数采样，不是 CV 折线。
- DAG 主组 `xgCurvesFromGuides1` 为 40,334 根；另组 308 根存在重名，
  本批排除另组，不能把短名字典的覆盖误当成正确去重。
- 正式 `Maps/SC_tressfx_female` 包括 40,334×32 guide、BodyModel/眼球 Orig 网格
  和对应 SM/MI。源包没有被外部路径引用的两张贴图，中性角色材质是 VL 作者选择。
- 来源 revision、`.ma` SHA256、MIT 许可在正式 Source 中；详细统计/摘要在 Generated/Import。
  这补上了第二批“真实静态女性 TFX 待导出”的边界，没有补上完整 rig/材质或 UE 对照验收。

## 1. 拉取结果

- 仓库：`GPUOpenSoftware/UnrealEngine`
- 分支：`TressFX5-5.4.4`
- commit：`61f8dd9b51b88d41937ef8d73618aac778642f41`
- 本地路径：`D:\YYBWorkSpace\GitHub\UnrealEngine-TressFX5`
- sparse checkout：`Engine/Plugins/AMD/TressFX`、
  `Engine/Source/Runtime/Renderer/Private/TressFX`、`Asset/Maya2018`、`Doc`

实际目录与文档有差异：

- 女性资产在 `Asset/Maya2018/Female_Outside/Female_Outside.zip`，不是 `Asset/Maya`。
- 渲染器新增文件在 `UE544_TressFX_Engine_Patch.patch` 里，不在源码树中；
  `Engine/Source/Runtime/Renderer/Private/TressFX/` 是 patch 新增的路径。
- 女性 `.ma` 是 **Maya ASCII 2020**，不是文档写的 2018。

## 2. 资产盘点

| 项 | 结果 |
| --- | --- |
| `Female_Outside.zip` | 47.6 MB |
| `Female_Outside.ma` | 185 MB，Maya 2020 |
| 依赖 | `hairPhysicalShader`、`mtoa`（Arnold） |
| XGen guide | 40,334 个唯一 `xgGuide*`，40,642 个 curve shape |
| Hair joints | 13 个（`HairA_*`、`Hair_*`） |
| Collision | `tfxmesh` / `tfxmeshShape` |
| Groom | `xgGroom` / `xgCurvesFromGuides1` |

结论：

- 资产是 **XGen groom + tfxmesh collision mesh**，需要 TressFX Maya exporter
  导 `.tfx`/`.tfxbone`/`.tfxmesh`。
- **40,334 guides 远高于常见 1–2k**；直接全量模拟/渲染会很重。
  需要先决定：全量保留，还是导出时抽 guide 子集 + follow。
- 女性资产不是 Ratboy；Ratboy 只是插件默认内容，不是这次的目标。

## 3. 源码盘点

| 部分 | 数量 | 说明 |
| --- | --- | --- |
| TressFXCore | 52 文件 | Asset/Importer/Simulation/SDF/Manager/Component/Resources |
| TressFXEditor | 34 文件 | Asset/Mesh editor、factory、importer |
| TressFXImportTranslator | 9 文件 | `.tfx`/`.tfxbone`/`.tfxmesh` 翻译 |
| TressFX Renderer | 29 个 cpp/h | Composition/DeepShadow/Environment/Group/LUT/Rendering/Tile/Transmittance/Visibility |
| UE Renderer hooks | 15 个文件 | DeferredShadingRenderer、LightRendering、ShadowRendering、SceneRendering 等 |
| TressFX shaders | 58 个 | `Engine/Shaders/Private/TressFX/*.usf/.ush` |

### 3.1 关键模块

- `TressFXCore/Private/TressFXTFXFileHeader.h`：`.tfx`/`.tfxbone` 头。
  与 4.1 **字段一致**；5.0 的差异在 16 骨、LOD、Lumen/DeepShadow approximation。
- `TressFXCore/Private/TressFXAsset.cpp`：`.tfx` 读取、follow hair 生成。
- `TressFXCore/Private/TressFXDatas.cpp`：strand/point/LOD 数据结构。
- `TressFXCore/Private/TressFXAssetSimulation.cpp`、
  `TressFXCoreSimulation.cpp`、`TressFXCoreSDF.cpp`：模拟/SDF。
- `TressFXCore/Private/TressFXVertexFactory.cpp`：UE 顶点工厂（要换成 VL 实现）。
- `TressFXImportTranslator`：`.tfx` → UE 资产；可参考/改写为离线工具。
- `TressFX Renderer`：`TressFXComposition`、`TressFXRendering`、
  `TressFXDeepShadow`、`TressFXTransmittance`、`TressFXVisibility`、
  `TressFXGroup`、`TressFXTile`、`TressFXLUT`。
- 58 个 shader 按功能分：Simulation（Integration/Length/LocalShape/VSP/SDF）、
  DeepShadow/Transmittance、Visibility/Tile/Cluster、Material/VertexFactory、
  LUT/Environment。

## 4. Port Map

| 5.0 模块 | 处理 | VL 落点 | 风险 |
| --- | --- | --- | --- |
| `TressFXTFXFileHeader` + `TressFXAsset` | 重写为纯 C++，去 UE 类型 | `source/hair/tfxLoader.*` | 低；格式已确认 |
| `TressFXDatas` / `TressFXResources` | 抽数据结构 | `source/hair/groomAsset.*` | 中；去 TArray/FVector |
| `TressFXImportTranslator` | 改为离线 CLI/Python | `tool/hair/` | 中；UE 资产依赖 |
| `TressFXComponent` / `TressFXManager` | 重写为 VL groom 组件 | `source/hair/` | 中 |
| `TressFXBoneSkinning` / `TressFXSkeletonInterface` | 映射到 VL 骨架 | `source/animation/`、`source/hair/` | 中 |
| `TressFXAssetSimulation` / `TressFXCoreSimulation` | 抽算法，compute 重写 | `source/hair/`、`shader/glsl/hair/*.comp` | 高 |
| `TressFXSDFComponent` / `TressFXCoreSDF` / `TressFXSimulationSDF*` | SDF 生成/碰撞 | `source/hair/`、compute | 高 |
| `TressFXRendering` / `TressFXComposition` | 重写 forward hair pass | `source/render/pass/`、`shader/glsl/hair/` | 高 |
| `TressFXDeepShadow*` / `TressFXTransmittance*` | 重写阴影 pass | `source/render/pass/`、compute | 高 |
| `TressFXVisibility*` / `TressFXTile` / `TressFXGroup` | 先砍，V3 再做 LOD/culling | `source/hair/` | 中 |
| `TressFXLUT` / `TressFXEnvironment` | 颜色/环境 LUT | `source/hair/`、textures | 中 |
| UE Renderer 15 个 hooks | 丢弃，改 VL pass/描述符 | — | 必须重写 |
| `MaterialTemplate.ush` 修改 | 换成 VL hair material | `shader/glsl/hair/` | 高 |

## 5. 关键发现

1. **`.tfx` 格式和 4.1 基本一致**，解析器可以先用 4.1 逻辑改；5.0 的 16 骨、
   LOD、Lumen/DeepShadow 差异要单独处理。
2. **渲染器代码不在源码树，在 patch 里**；要拆只能从 patch 提取新增文件，
   或先把 patch 应用到 UE5.4.4。对 VL 来说，提取文本即可，不需要编译 UE。
3. **40,334 guides 是最大的性能风险**。TressFX 5.0 有 continuous LOD；
   V0/V1 建议先导出 guide 子集或按距离 LOD，不能直接全量模拟。
4. **女性资产需要 Maya 2020 + hairPhysicalShader + mtoa**；没有这些，
   只能从 patch 的 UE 资产或已有 `.tfx` 导出，不能直接打开 `.ma`。
5. **UE glue 15 个文件**是重写工作量的大头；TressFX 自己的 29 个 renderer
   文件反而比较独立。

## 6. 下一步

- 完成逐文件 port map（当前按模块分组）。
- 从 patch 提取渲染器源码，做“能否在 VL 编译”的 spike。
- 用 `.ma`/Maya 或 UE 资产导出第一份 `.tfx`，确认 guide 数、点数、骨骼。
- 决定 guide 子集/LOD 策略后，再进 V0。
