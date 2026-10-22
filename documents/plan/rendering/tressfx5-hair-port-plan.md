# TressFX 5.0 for UE 5.4 头发移植开发计划

状态：本地分支 `codex/tressfx5-hair-port` 已实现 CPU 数据层与真实女性源资产的静态关卡；
完整绑定/贴图迁移、Marschner、抗锯齿、阴影和模拟尚未实现。这是 `plan/` 层，不是实现契约；
已实现边界见 [TressFX 资产导入](../../contracts/tressfx-asset-import.md)。资产路线与选项记录见
[hair-pipeline-options.md](hair-pipeline-options.md)。

## 首批落地范围（2026-10-22）

- 纯 CPU `vl_hair_assets`：`.tfx` 读取、属性/bounds、结构化错误、静态切线/rest length/ribbon 索引。
- `vl_tfx_inspect`：只读检查 JSON 报告；`hair_asset_tests`：根 GTest 模块套件。
- 以本地 5.0 exporter 核实：产品版本是 5.0，实际 `.tfx` header version 仍为 **4.0**；
  `.tfxbone` 实际按 bone count/name/strand weights 流写入，不能直接套未使用的
  `TressFXTFXBoneFileHeader`。
- 当前没有真实女性 `.tfx` 导出；M0 资产验收、M0.5 shader/渲染 spike 与 M1 均未完成。
- 2026-10-21 已关闭参考仓库 sparse checkout；该分支只有插件、引擎 patch、Maya 资产和工具，
  README 所称 `Asset/Project/UE544TFX` 未提交。后续 UE 画面对照依赖参考工程另行准备。
- 验证：隔离工作树 MinGW Debug 的 `hair_asset_tests` / `vl_tfx_inspect` 构建通过，
  36/36 项 CTest 通过；中文路径的合成 `.tfx` 报告为 2 guides / 8 points / 36 indices，
  缺失文件返回 `Hair.TfxRead`、exit 1。证据在 gitignored 的
  `artifacts/tressfx5-import/`。首次独立配置误选 Clang，改为显式 gcc/g++ 后通过；
  依赖源码复用本机下载目录，构建输出独立。共享目录的 main 构建曾通过，
  迁移后的隔离目录未重编完整 main，也未执行 Vulkan 发丝出图或三项 runtime gate。

## 第二批落地：正式网格路径的静态 guide

本段更新首批的实施状态；下文 groom 专用资产、pass 和完整 M1 仍为规划。

- 场景继续使用 `type: mesh`，`SM_*` 描述符使用 `type: tressfx`、`.tfx` 和显式
  `strandRadius`。通过 `TressfxModelImporter → MeshAssetLoader → RendererMeshLoader`
  复用 World 候选、上传、快照与资源退休，没有新增 groom 场景对象。
- `shader/glsl/engine/tressfxVertexFactory.glsl` 迁移 patch 中
  `TressFXVertexFactory.ush::ComputeViewAlignedWorldPosition` 的统一 ViewForward
  展开语义；局部中心线在 GPU 展开，保留旋转/缩放实例和材质工厂身份。
  下文旧拆解顺序的 `TressFXBasePass` 是概括名称，不能当作真实 shader 文件。
- 先用 `hair/M_strandProbe` 的双面 Unlit、alpha=1、写深度探针进入既有
  `forwardTransparent`；没有恢复 Hair ID，也没有启用 MSAA/A2C 或像素最小半径。
  材质边界拒绝 deferred、蒙皮和 WPO；离线三角形路径明确拒绝该工厂。
- 隔离工作树完整 main、导入工具、hair/material/UI 测试目标构建通过。
  首次链接因 CMake 的 `/hair/` 排除规则误伤嵌套 importer 失败，收窄到
  `/source/hair/` 后完整构建通过。
- 合成探针为 96 guides ×32 points、每实例 17,856 indices，包含正面和旋转 55°
  的两个实例。真实 Vulkan 正面/转相机截图已检查，均为完整 1280×720 BMP：
  `artifacts/tressfx5-static/Resources/Generated/Screenshots/front.bmp`、
  `orbit.bmp`。运行 stderr 为空，窗口正常关闭并完成 teardown。
  这只验证几何/朝向/正式加载链，不代表真实女性头发、光照或亚像素覆盖验收。
- `hair_asset_tests` 49/49、`material_schema_tests` 16/16；
  `editor_ui_tests` 全量 681 项，676 通过、5 项符号链接权限环境跳过。
  三项既有 Vulkan 门禁串行通过（exit 0）。最初缺少 Common 测试纹理导致
  fixture 加载失败，补齐隔离资源后通过；未修改原资源目录或门禁实现。
  合成 fixture 和运行日志位于 gitignored 的 `artifacts/tressfx5-static/`。

当前没有 `GR_*` 或 `.groom` 格式；下文 §4.3/§5 的专用格式是后续设计候选，
不能据此另建与正式 SM 加载并行的作者链。M0 的女性资产导出、完整逐文件 port map、
M1 的 Marschner/覆盖率仍未验收。

## 第三批落地：Female 正式资产关卡

- 配套资源分支为 `codex/tressfx5-assets`，工作树在引擎工作树同级的
  `VukanLearnResources`，沿用 config 的 `../VukanLearnResources`。
  只检出 Common、默认 sphere array 和本关卡，未改原资源仓库的 master 工作目录。
- 正式入口为 `Maps/SC_tressfx_female/SC_tressfx_female.json`；三个 SM、三个 MI、
  OBJ/TFX 与来源许可归属该地图。Generated 只存导入报告、验证和截图。
- 本机没有 Maya。新增受限离线提取器，按 `.ma` 内容摘要锁定源文件，通过 SciPy
  B-spline 求值保存的 NURBS 曲线；这是静态保存几何迁移，不声称执行过官方 Maya exporter、
  绑定图或 UE 工程。主组实际 40,334 根；另组 308 根使用重名节点，必须按 DAG 路径区分。
- 每根 32 点、共 1,290,688 点；完整导入主组，未抽 guide 子集或补 follow。
  BodyModel/眼球保留保存的 Orig 输入网格，OBJ 由 Assimp 正式加载；
  源包缺失的两张外部贴图没有伪造，使用中性 PBR 和粉色 Unlit 发丝。
- 已通过正式 scene API 读取/能力/修改/校验/保存相机，重启消费最终构图。
  相机水平 FOV 为 38°，首次误按竖直视场估算距离而裁边，已将位置调整为
  `[35,162,140]`、目标 `[0,151,0]`。开启编辑器后，较矮视口仍有裁边，
  最终把正式位置改为 `[50,165,210]` 并重启验证，完整头顶与发梢均在默认编辑器画面内。
  Console `camera pose` 改的是 active World 相机，不能当成独立编辑器相机的构图验证。
- 实测 `vl_tfx_inspect`、三个 SM schema 与 scene/mesh 标准 API 通过；
  稀疏资源根场景 schema 为 2/2，不代表原资源目录全量扫描。
  独立重导入 10 文件字节一致、1 个作者保存 JSON 语义一致；已有目录明确拒绝。
  `female-final.bmp` / `female-side.bmp` / `female-editor-final.bmp` 已检查，1280×720，日志见
  `artifacts/tressfx5-female/`。Python 默认选到 free-threaded 3.14，安装的
  NumPy 扩展不能加载，改用标准 `C:\Program Files\Python314\python.exe` 后成功；
  首次离线 mesh 提取只消费了首个显式 polyFaces 块，并拒绝了源中的五至七边形，
  补齐后续省略 type 的块与 n-gon 支持后，全部 face 数量和边闭环通过。
- 第三批仍不完成完整 M0/M1：没有 Maya/UE 对照、贴图、骨骼/follow、Marschner、
  发丝覆盖抗锯齿或阴影；静态关卡不等于完整角色移植。当前实现细节见
  [正式静态关卡契约](../../contracts/tressfx-asset-import.md#female-正式静态关卡)。

## 1. 目标与范围

目标：把 **TressFX 5.0 for UE 5.4** 示例里的粉发女性资产搬进 VulkanLearn，
先做静态头部肖像，再逐步接入 guide/follow 蒙皮、发丝模拟、阴影和 LOD，
最终接近 UE 5.4 参考画面的观感。

### 1.1 版本基线

- **目标版本：TressFX 5.0 for UE 5.4**（2025-08-04 发布，分支
  `TressFX5-5.4.4`）。
- 资产：该 patch 的 `Asset/Maya2018/Female_Outside/Female_Outside.zip`；
  `.ma` 实际是 **Maya 2020**（文档写 2018，以文件为准）+ collision mesh。
- UE5.4 相对 UE4.27 的差异：Lumen 软/硬光追、deep shadow approximation、
  continuous LOD、IndirectDraw texture clear。
- **不使用 UE4.27 版作为目标**；`TressFX5.0_DeveloperGuide.pdf` 主要写
  UE4.26/4.27，架构/导出流程可参考，但 UE5.4 的差异以更新说明为准。
- **运行时基座改为拆 TressFX 5.0 for UE 5.4 工程**：抽 TressFX 核心/插件/shader，
  把 UE glue 重写到 VulkanLearn；4.1 standalone 只作算法对照与缺失模块 fallback，
  不再作为主基座。

范围：

- 资产：female avatar 的头/身体、头发、贴图、骨骼、collision mesh。
- 渲染：strand 几何、forward 透明、MSAA/alpha-to-coverage、Marschner 头发着色、
  deep shadow、OIT（ShortCut/PPLL）、LOD。
- 模拟：guide/follow、骨骼蒙皮、SDF 碰撞、VSP、约束求解。
- 不包含：UE 渲染器整体移植、TressFX 5.0 的 UE 插件原样编译、毛发/fur 全量支持。

## 2. 当前事实与约束

- **TressFX 5.0 for UE 5.4** 不是独立库，是 UE 5.4.4 的 patch；女性资产在
  `Asset/Maya2018/Female_Outside/Female_Outside.zip`（`.ma` 是 Maya 2020，
  40,334 guides + `tfxmesh`），需要 Epic UE GitHub 权限。
- 5.0 for UE 5.4 的运行时是 UE Plugin + Renderer hooks，绑在 UE deferred 管线上；
  不能作为 VulkanLearn 的构建依赖。
- 独立可用的 Vulkan 参考是 **TressFX 4.1 standalone**（MIT）：
  DX12/Vulkan、Maya exporter、strand 渲染/模拟/SDF/LOD。
- VulkanLearn 当前是 deferred GBuffer；`forwardTransparent` 已存在但 `needMsaa:false`，
  `graphicsPipelineBuilder.cpp` 的 `alphaToCoverageEnable` 固定为 false。
- 角色侧已有 GPU 蒙皮与 GASP 骨架；发根可以绑到头骨/头皮骨骼。
- Hair 着色模型在 2026-09-12 被删除；`common/hairPathScattering.glsl` 仍在。
- 已有 `.tfx` guide 导入与静态 camera-facing ribbon；没有 `.tfxbone` / `.tfxmesh`
  导入、follow 生成、Marschner、发丝阴影或发丝模拟。当前契约以
  [TressFX 资产导入](../../contracts/tressfx-asset-import.md) 为准。

## 3. 总体路线

两条线并行，但以资产线为前提：

| 线 | 内容 | 关键依赖 |
| --- | --- | --- |
| 资产线 | UE5.4 patch → Maya 2018 → `.tfx`/`.tfxbone`/`.tfxmesh` + FBX + 贴图 | Epic UE 权限、Maya 2018 |
| 运行时线 | 拆 5.0 工程 → TressFX 核心/shader → VL strand 资产 → forward 渲染 → 蒙皮/模拟/阴影/LOD | TressFX 5.0 for UE 5.4 源码 |

运行时**以 TressFX 5.0 for UE 5.4 工程为主基座**：拆出 TressFX 核心/插件/shader，
把 UE glue 重写到 VulkanLearn；4.1 standalone 只作算法对照和缺失模块 fallback。
这样能避免“老基座 + 5.0 资产”造成的光照/阴影/透明路径不一致。

### 3.1 5.0 工程拆解边界

“拆”指**抽源码 + 重写 UE glue**，不是把 UE 代码直接编进 VulkanLearn。

| 5.0 工程部分 | 处理 | VL 落点 |
| --- | --- | --- |
| `Engine/Plugins/AMD/TressFX`（Asset/Importer/Simulation/SDF） | 抽核心数据结构与算法，去掉 UE 类型 | `source/hair/` |
| `Engine/Source/Runtime/Renderer/Private/TressFX`（BasePass/DeepShadow/Marschner/Composition） | 重写为 VL pass | `source/render/pass/` + `shader/glsl/hair/` |
| `Engine/Shaders` 的 TressFX `.ush`/`.usf` | HLSL→GLSL，替换 UE include | `shader/glsl/hair/` |
| UE Material/VertexFactory/SceneProxy hooks | 丢弃，换 VL material/vertex layout | `source/material/`、`source/pipeline/` |
| `Asset/Maya` + `.tfx`/`.tfxbone`/`.tfxmesh` | 保留资产管线 | `tool/hair/` |
| `Tools` Maya exporter | 原样使用 | 外部工具 |

不搬：UE RHI、Lumen、UE 材质图、`SceneTextures`/`LightBuffer`、UE 阴影管线。
UE5.4 的 Lumen 光追不在本期范围；只保留 TressFX 自己的 deep shadow/opacity。

拆解顺序：

1. **依赖审计**：列 5.0 工程文件、include、UE 类型；产出 port map（3–5 天）。
2. **数据层**：`TressFXAsset`/Importer + `.tfx` 解析 → VL groom asset。
3. **静态渲染**：`TressFXBasePass` 的 strand VS/PS + Marschner → VL forward hair。
4. **阴影**：DeepShadowMap/DeepOpacityMap → VL compute/raster pass。
5. **模拟**：`TressFXSimulation` / SDF / VSP compute → VL compute。
6. **合成/材质**：`TressFXComposition` / 材质参数 → VL pass/material。

拆解 spike 的验收：`.tfx` 能读、一根 strand 能在 VL 画出、构建里没有 UE 代码、
依赖 port map 完整。达不到就先退回“5.0 资产 + 4.1 运行时”的混合方案。

初步盘点见 [tressfx5-port-map.md](tressfx5-port-map.md)。

## 4. 资产管线

### 4.1 拉取

- 仓库：`GPUOpenSoftware/UnrealEngine`，分支 `TressFX5-5.4.4`。
- 只取资产：sparse checkout `Asset`，避免完整 UE 源码。
- 仓库当前没有预导出的 `.tfx`/`.tfxbone`/`.tfxmesh`；女性资产只有 `.ma`，
  必须走 Maya 导出。
- 只有 `.ma`/`.mb` 时，用 **Maya 2020** 打开（依赖 `hairPhysicalShader`/`mtoa`），
  再用仓库 `Tools/Maya2022/TressFX_Exporter.py` 导出 `.tfx`/`.tfxbone`/`.tfxmesh`。
  仓库没有预导出的 `.tfx`，必须走 Maya。

### 4.2 导出清单

| 资产 | 格式 | 用途 |
| --- | --- | --- |
| 头/身体/头皮 | FBX（带骨架） | 角色网格、发根绑定 |
| 头发 guide | `.tfx` | 位置、UV、宽度、颜色 |
| 骨骼蒙皮 | `.tfxbone` | 发根/发丝骨骼权重（5.0 最多 16 根） |
| collision mesh | `.tfxmesh` | SDF 碰撞 |
| 贴图 | PNG/TGA/DDS | hair/face 材质 |
| 参考图 | PNG | UE5.4 同机位对照 |

实际入口：`Asset/Maya2018/Female_Outside/Female_Outside.zip` → `Female_Outside.ma`
（Maya 2020，185 MB，40,334 个 XGen guide，13 个 hair joint，`tfxmesh`）。
这不是 Ratboy；Ratboy 只是插件默认内容。

### 4.3 离线转换

- `.tfx` 是简单二进制：header + positions(float4) + strand UV + vertex UV +
  thickness + vertex color；**只存 guide**，follow 运行时生成。
- `.tfxbone`：bone names + skinning indices/weights。
- `.tfxmesh`：collision triangles，用于生成 SDF。
- 写一个 `tool/hair/tfx_dump` 或 Python 转换器，输出：
  - `.groom` 二进制（VL 运行时格式）
  - `GR_*.json` 描述符（见 §5）
  - 统计/校验报告（strand 数、点数、bbox、骨骼数、SDF 分辨率）
- 5.0 的 `.tfx` version 可能高于 4.1；解析器要按 header/offset 做版本容忍，
  不直接复用 4.1 的版本拒绝逻辑。

## 5. 运行时数据模型

### 5.1 资产

新增 groom 资产（命名暂定 `GR_*`，最终以 `contracts/asset-organization.md` 为准）：

```json
{
  "name": "GR_female_hair",
  "type": "groom",
  "source": "Source/female_hair.groom",
  "guideCount": 40334,
  "followCount": 20000,
  "pointsPerStrand": 32,
  "scalpMesh": "SM_female_head",
  "skeleton": "SK_female",
  "material": "M_hair",
  "collisionMesh": "Source/female_hair_collision.groommesh",
  "sdfResolution": [64, 64, 64]
}
```

二进制 `.groom` 建议字段：

- header：magic/version/guideCount/followCount/pointsPerStrand
- positions `float3[guideCount * pointsPerStrand]`
- tangents（离线算或运行时算）
- width per point 或 per strand
- rootUV / strandUV
- bone indices/weights（可选）
- follow 生成参数（root radius、tip separation、clump）

### 5.2 场景对象

新增 `groom` 场景对象（或 mesh 的 `groom` 子对象），引用 `headMesh` 和 transform：

- `SceneAssetValidator` 增加字段校验；
- `worldLoader` / `worldBuilder` 增加构建计划；
- `schema/scene.schema.json` 与文档同步；
- 先支持静态/单实例，不做多 groom 合批。

## 6. 渲染管线

### 6.1 Pass 顺序

目标顺序：

```text
shadowCascade0..3        opaque shadow
hairSimulation           compute（V2）
hairDeepShadow           compute（V1/V2）
geometry                 deferred opaque
deferredLighting         采样 hairDeepShadow
forwardOpaque
sky
hairForward              forward + MSAA + alpha-to-coverage（V0 起）
forwardTransparent
editorGrid
autoExposure / bloom / toneMapping
```

V0 只加 `hairForward`；V1 加 `hairDeepShadow`；V2 加 `hairSimulation`。

### 6.2 Strand 几何

- 每个 strand 点扩成 camera-facing ribbon：2 顶点/点，2 三角形/段。
- 顶点着色器读取 `HairVertex` storage buffer + index buffer；
  不把每帧发丝位置写回静态顶点缓冲。
- 每顶点属性：position、tangent、width、strandUV、rootUV、strand index。
- 切线方向 root→tip；ribbon 宽度按视距/像素覆盖率钳制，避免亚像素闪烁。
- 索引缓冲离线生成；发丝数量/点数变化时重建。

### 6.3 Pass 类型与 MSAA

- 新增 `RenderGraphPassType::ForwardHair`（避免和已有 `forwardTransparent`
  单例行为冲突），或在 V0 复用 `forwardTransparent`，V1 再拆分。
- `needMsaa: true`；渲染目标用现有 MSAA source/resolve 资源。
- `graphicsPipelineBuilder.cpp` 增加 `alphaToCoverageEnable` 开关；
  `sampleCount` 用 pass 的 MSAA 配置。
- 无 TAA 时，MSAA 4x/8x + alpha-to-coverage 是底线；有 TAA 后再接
  velocity（`gbufferVelocity` 已有）。

### 6.4 着色

- Marschner R/TT/TRT + 多重散射；复用/重写 `common/hairPathScattering.glsl`。
- TressFX 5.0 的 Marschner shading model 作为参考；不要在 GLSL 里硬编码
  UE 的材质节点。
- 输入：base color / scatter / roughness / backlit / tangent；参数走
  `M_*.json` / `MI_*.json`，与现有材质系统一致。
- 先做单散射 + 简化多重散射；再按 TressFX 5.0 的 dual scattering/deep shadow 对齐。

### 6.5 透明与 OIT

- V0：alpha blend + MSAA；接受发丝间排序误差。
- V1：ShortCut（TressFX 3.1+ 的简化 OIT）或 per-pixel linked list（PPLL）；
  需要额外 buffer/atomic，作为 compute/raster 混合方案。
- 选 ShortCut 优先：实现量小于 PPLL，TressFX 4.1 有现成 shader。

### 6.6 发丝阴影

- V1 先做 deep shadow map / deep opacity map（每光源），或至少把发丝
  加入 CSM 的 alpha-tested 阴影。
- V2 对齐 TressFX 5.0：DeepShadowMap + DeepOpacityMap、每 TressFXComponent/光源；
  AMD 的 DeepShadow approximation 作为可选路径。
- 阴影必须用同一份模拟后位置，否则会拖影。

## 7. 模拟

### 7.1 设计

- guide/follow：只模拟 guide（约 512–2048 根），follow 由 guide 插值/程序化生成。
- 约束：距离（拉伸）、弯曲、local/global shape、碰撞、摩擦；固定步长 + 2–4 substep。
- 外力：重力 + 程序化风；阻尼分根/中/梢。
- 碰撞：`.tfxmesh` 生成 SDF；V2 先 SDF + capsule，V3 再优化。
- 蒙皮：发根随头骨；`.tfxbone` 的 16 骨权重映射到 VL 骨架。
- VSP：高速移动时传播根部速度，避免发丝拉长。
- LOD：V3 接 continuous LOD；V2 先固定 guide 数。

### 7.2 GPU 边界

- 模拟在 compute，双缓冲，CPU 零回读。
- 发丝位置/切线写 storage buffer；渲染 pass 直接读。
- 固定 dt（60/120 Hz），不跟可变帧时间；与现有动画/物理时钟分离。
- 模拟 pass 失败时保留上一帧姿势，不能破坏渲染。

## 8. 代码落点

### 8.1 资产/数据

- 5.0 拆解来源：`Engine/Plugins/AMD/TressFX`、
  `Engine/Source/Runtime/Renderer/Private/TressFX`、`Engine/Shaders`；
  M0.5 产出 port map（文件 → 依赖 → VL 落点）。
- `source/hair/tfxLoader.*`：`.tfx`/`.tfxbone`/`.tfxmesh` 解析（纯逻辑，可测）。
- `source/hair/groomAsset.*`：VL `.groom` / `GR_*.json`。
- `tool/hair/tfx_dump.*` 或 Python 转换器；输出报告。
- `source/mesh/loader/mesh/assimpModelImporter.*`：复用 FBX 导入。
- `source/animation/**`：骨骼名/权重映射。

### 8.2 渲染

- `shader/glsl/hair/hairStrands.vert`、`hairShading.glsl`、`hairForward.frag`。
- `shader/glsl/hair/hairSimulation.comp`、`hairSDF.comp`、`hairDeepShadow.comp`。
- `source/pipeline/graphicsPipelineBuilder.cpp`：alpha-to-coverage / sample count。
- `source/render/rendergraph/renderGraphPassType.h` + `renderGraphCompiler.*`：
  `ForwardHair` / `HairSimulation` 类型。
- `source/renderGraph.cpp`、`source/renderSystem.cpp`：MSAA 资源、pass 顺序、draw。
- `config/renderGraphConfig.json`：新增 pass/resource。
- `source/world/loading/worldLoader.*`、`worldBuilder.*`：
  `groom` 对象构建。
- `source/scene/validation/sceneAssetValidator.*`、`schema/scene.schema.json`：
  新对象校验。

### 8.3 文档/测试

- 落地后更新 `contracts/vulkanlearn-architecture.html`、`contracts/gbuffer-channel-ownership.md`、
  `contracts/shader-hot-reload.md`、`contracts/testing.md` 的相关条目。
- GoogleTest 放 `tool/hair-tests/`（或现有最近套件），不新增 runtime test 命令。
- 参考 `tool/validation/` 的截图/像素对照方式。

## 9. 里程碑与验收

### M0 资产可解析（1–2 天）

- 拿到 `.tfx`/`.tfxbone`/`.tfxmesh` + FBX + 贴图。
- `tfx_dump` 输出 strand 数、points、bbox、骨骼数、SDF 分辨率。
- 验收：数量/bbox 与 UE 内统计一致；无越界/NaN。

### M0.5 5.0 工程拆解审计（3–5 天）

- 列 5.0 工程文件、include、UE 类型依赖；产出 port map。
- 用最小 strand 数据验证“抽出的 shader/数据能进 VL 构建”，不接完整渲染。
- 验收：构建里没有 UE 代码；port map 覆盖 BasePass/DeepShadow/Marschner/
  Simulation/SDF；明确 full extraction 还是 hybrid。
- 如果 spike 失败，退回“5.0 资产 + 4.1 运行时”的混合方案，并记录卡点。

### M1 V0 静态肖像（1–2 周）

- female avatar + hair 在 VulkanLearn 静态渲染。
- forward strand pass、MSAA/alpha-to-coverage、基础 Marschner。
- 验收：同机位对照 UE5.4 参考图，轮廓/发丝密度/主高光位置一致；
  无明显缺丝/穿插/排序灾难。

### M2 V1 蒙皮与阴影（1–2 周）

- guide/follow 插值、骨骼蒙皮、转头不炸。
- hair deep shadow/opacity（至少 directional light）。
- 验收：头部转动/姿态变化时发根不漂移；发丝在脸上有正确投影；
  阴影不拖影。

### M3 V2 模拟（2–4 周）

- guide 模拟 + SDF 碰撞 + VSP + 重力/风。
- 验收：静置稳定、快速转头不拉长、发丝不穿脸/肩；
  连续 10 分钟无爆炸/NaN；GPU 时间可接受。

### M4 V3 对齐与 LOD（2–4 周）

- 连续 LOD、TAA/velocity、材质/光照对齐、性能优化。
- 验收：UE 参考图 ROI 像素误差在约定阈值内；目标分辨率下帧时间达标；
  LOD 切换无明显跳变。

> 工期是粗估，取决于资产导出、UE 参考环境、以及是否已有 TAA。
> 不建议把 M3/M4 与 M1 并行，模拟会掩盖渲染问题。

## 10. 测试与验证

- **单元测试**：`.tfx`/`.tfxbone`/`.tfxmesh` 解析、偏移/越界、bbox、骨骼映射。
- **离线数值**：SDF 生成、约束求解、follow 插值；放 `vl_offline_core`。
- **图像对照**：UE5.4 同机位截图 vs VulkanLearn；固定曝光/灯光；
  用 `tool/validation/` 的像素/ROI 统计。
- **稳定性**：固定 dt 下长时间运行、快速转头、传送/大幅位移。
- **性能**：20k render strands / 2k guides 的 GPU 时间、overdraw、显存。
- **回归**：现有 forwardTransparent/GBuffer/shadow 路径不受影响；
  shader cache/hot reload 正常。

## 11. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| UE patch 访问/资产 license | 拿不到 female avatar | 先确认 Epic 权限；资产条款单独记录；个人学习与发布分开 |
| `.tfx` 5.0 版本差异 | 4.1 解析器拒绝 | 写版本容忍 parser；按 offset 读，未知字段跳过 |
| 5.0 工程 UE 耦合深 | 拆解超预期、进度失控 | M0.5 spike gate；只抽核心/shader；失败转 hybrid |
| 40,334 guides | 模拟/渲染/显存过高 | 导出时抽 guide 子集 + follow；V3 接 continuous LOD |
| UE shader 依赖 | 不能直接编译 | HLSL→GLSL；替换 `SceneTextures`/`LightBuffer`；以 5.0 工程为源、4.1 为对照 |
| UE5.4 Lumen/DeepShadow 依赖 | 无法完全对齐 | Lumen 明确不做；只保留 TressFX 自己的 deep shadow/opacity |
| 无 TAA | 发丝闪烁 | MSAA + alpha-to-coverage 先顶；V3 补 TAA |
| MSAA/alpha-to-coverage 未接 | 边缘碎 | 先改 pipeline builder；确认 sampleCount/attachment |
| OIT 复杂度 | 发丝排序错误 | V0 alpha blend；V1 ShortCut；PPLL 作为可选 |
| Deep shadow 成本 | 帧时间高 | 先 directional light；V3 再全光源 |
| `forwardTransparent` 单例 | pass 冲突 | 新增 `ForwardHair` 类型，V0 可临时复用 |
| 资产过大/流式 | 显存/加载 | 先离线裁剪到肖像范围；后续 LOD/streaming |

## 12. 待定决策

已定：运行时基座 = 拆 TressFX 5.0 for UE 5.4 工程，4.1 只作 fallback；
M0.5 spike 验证可行性后再冻结。

待定：

1. V0 是否新增 `ForwardHair` pass 类型，还是临时复用 `forwardTransparent`。
2. groom 资产是独立 `GR_*` 场景对象，还是 mesh 的子资源。
3. follow hair 用 TressFX 程序化生成，还是 Maya 额外导出 follow 曲线。
4. OIT 选 ShortCut 还是 PPLL（建议 ShortCut 优先）。
5. 模拟直接 port TressFX 4.1 compute，还是先写 P2 简化版再替换。
6. TAA 是本期做，还是 V3 单独做。
7. 资产 license 是否允许非 UE 引擎使用；不允许时是否只做个人学习分支。

## 13. 参考

- [TressFX 5.0 for UE5 更新说明（UE 5.4）](https://gpuopen.com/learn/tressfx-5-0-unreal-engine-5-update/)
- [TressFX5-5.4.4 分支](https://github.com/GPUOpenSoftware/UnrealEngine/tree/TressFX5-5.4.4)
- [TressFX 5.0 Developer Guide（PDF，主要写 UE4.26/4.27）](https://gpuopen.com/download/TressFX5.0_DeveloperGuide.pdf)
- [TressFX 4.1 standalone（MIT）](https://github.com/GPUOpen-Effects/TressFX)
- [hair-pipeline-options.md](hair-pipeline-options.md)
- [shading-model-alignment-plan.md](shading-model-alignment-plan.md) §2.4
- [animation-runtime-skinning.md](../../contracts/animation-runtime-skinning.md)
- [shader-hot-reload.md](../../contracts/shader-hot-reload.md)
- [gbuffer-channel-ownership.md](../../contracts/gbuffer-channel-ownership.md)
- [testing.md](../../contracts/testing.md)

## 14. Female/GASP 重定向姿态校准调研（2026-10-22）

本节记录资料、测量与下一步建议，**不是已实现契约**。
当前实现边界见 [动画播放与 GPU 蒙皮](../../contracts/animation-runtime-skinning.md)
和 [Female 原骨架导入](../../contracts/tressfx-asset-import.md)。

### 官方资料

- [Epic UE 5.4 IK Rig Retargeting](https://dev.epicgames.com/documentation/unreal-engine/ik-rig-animation-retargeting-in-unreal-engine?application_version=5.4)
  的 Retarget Pose 节明确要求关注源/目标参考姿态差异，A/T pose 不一致时可以编辑独立
  Retarget Pose；姿态匹配能提高准确度。支持手工旋转、从动画指定帧或 Pose Asset 导入。
- [Epic UE 5.4 Auto Retargeting](https://dev.epicgames.com/documentation/en-us/unreal-engine/auto-retargeting-in-unreal-engine?application_version=5.4)
  的 Auto Align Poses 根据 retarget chains 对齐整体、选中骨或子链，也提供按蒙皮网格对齐。
  自动结果仍需检查肘膝弯曲方向、手腕和末端；不能把方向一致当成扭转角已确定。
- [Epic UE 5.4 Runtime IK Retargeting](https://dev.epicgames.com/documentation/en-us/unreal-engine/runtime-ik-retargeting-in-unreal-engine?application_version=5.4)
  同样使用事先创建的 IK Retargeter，由 Retarget Pose From Mesh 消费运行时源姿态。
  运行时传递动作不会省去校准资产。
- IK Rig 文档把通常位于 pelvis/hip 的 Retarget Root、FK chain 传递和可选 IK 分开描述。
  该 Retarget Root 概念不能直接等同于 VL 播放器固定索引 0 的 locomotion root。

### 本地证据

当前 `source/animation/animationRetarget.cpp` 的 `CompiledRetarget` 直接缓存源/目标
ozz rest pose 的模型空间旋转，并应用
`R_source(t) * inverse(R_source(rest)) * R_target(rest)`。
这支持不同骨轴的参考增量传递，但没有单独作者的 source/target Retarget Pose、
chain 插值或 IK；源 rest 输入会得到目标自身 rest，而不是姿态已校准的目标。

在正式 `SK_UEFN_Mannequin.json` 与 `SK_Female_Outside.json` 中按父链连乘
`restLocal`，用关节模型空间位置之差定义骨段方向，两份数据均为厘米/Y-up。
取 `acos(dot(normalize(sourceSegment), normalize(targetSegment)))`，得到：

| 左侧骨段 | GASP 与 Female 绑定方向夹角 |
| --- | --- |
| upperarm→lowerarm / Shoulder→Elbow | 14.0838° |
| lowerarm→hand / Elbow→Wrist | 18.7573° |
| thigh→calf / Hip→Knee | 3.9590° |
| calf→foot / Knee→Ankle | 2.4920° |

这些是绑定骨段方向差，不是动画重定向误差，也不能直接逐骨取反作为校准量；
父级旋转会改变子链，骨段方向本身还不能唯一确定轴向 twist。
本次正式运行截图完成 bind、Idle、Walk 0.25/0.75 s，Walk ROI 有 53,297 个像素
变化超过 4 byte，C# 请求的是 in-place Walk。这只证明导入/播放/蒙皮链路可用；
证据在本工作树 `artifacts/tressfx5-female-skin-v1/`，不构成姿态质量或足底接触验收。

### 建议实施顺序

1. 为 retarget profile 增加独立、可命名的源/目标校准姿态，明确每骨旋转偏移的空间、
   四元数顺序和父链传播；原始 skin bind/inverse bind 继续专属于网格蒙皮。
2. 先为 Female 对齐 GASP 的肩、上臂、前臂、手腕和腿链，提供校准姿态与骨架叠加对照；
   可从链方向生成 swing 初值，再单独作者 twist，不能只依赖骨名或 A/T 标签。
3. 同一份校准用于 Idle/Walk 运行时采样；验证校准参考姿态对应关系、目标骨长、
   未映射骨继承，以及没有校准配置时的既有行为。
4. 之后分别设计 pelvis 运动、身高比例及足底 IK/接触处理。当前只传索引 0 根位移，
   不传 pelvis 平移；不能把参考姿态校准说成完整解决脚滑或 Motion Matching。

### 实施补记（2026-10-22）

以上“本地证据”为校准前的历史状态。现已加入可选 source/target 参考姿态，
Female 的 14 个偏移与独立只读 Retarget 标签；运行时和查看页共享参考姿态计算。
首轮脚尖方向硬对齐的实景出现高跟鞋上翘，已改为保留 Female 原脚部模型参考朝向；
其余链方向与手掌框架仍对齐，脚型差异继续在查看页显示。
准确当前行为与边界以 [Female 导入契约](../../contracts/tressfx-asset-import.md)
及 [Retarget 标签](../../contracts/ui-component-system.md#retarget-参考姿态标签) 为准。
