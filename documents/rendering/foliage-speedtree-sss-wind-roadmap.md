# Foliage / SpeedTree / SSS / Wind 落地路线

## 目标

本文档定义 VulkanLearn 中引入 SpeedTree 树木资产、foliage thin-surface SSS、风力动画与 multi-pivot 变形的分阶段落地路线。

这条路线的核心目标不是一次性做出生产级植被系统，而是用一棵真实树把以下能力逐步打通：

- 复杂植被资产进入当前数据驱动场景系统
- 树干与树叶材质分离
- alpha cutout 叶片稳定参与 forward / shadow / post-process 链路
- 树叶具备可解释的背光透射和薄表面 SSS 效果
- 风力参数进入全局渲染数据
- 顶点阶段完成基础风摆动
- 后续扩展 SpeedTree 风数据和 multi-pivot 层级摆动时，不推翻第一版管线

这里的 SSS 优先定义为 foliage translucency / transmission，而不是皮肤或蜡烛那类屏幕空间或 diffusion-profile SSS。树叶最重要的第一效果是逆光时被照亮、叶片厚度感和颜色透过感，而不是物体内部长距离散射。

## 当前工程基础

当前工程已经具备一些适合起步的能力：

- 场景通过 JSON 加载 mesh、light、camera、environment
- mesh JSON 当前通过 `modelDataPath` 指向模型文件，通过 `materialInstancePath` 指向材质实例；下一步会迁移为 `materialSlots`
- 材质实例已经支持 `renderStates.renderMode`
- `OpaqueClip` 可用于 alpha cutout
- 材质实例已经支持 `renderStates.cullMode = None`
- PBR shader 已经支持 albedo、normal、pbrParam、emission 贴图
- 顶点数据包含 position、normal、color、uv、tangent
- MikkTSpace tangent 已接入，用于稳定法线贴图解码
- 阴影 pass 与主几何 pass 已经走统一材质实例加载路径

同时也有几个必须正视的限制：

- `ModelLoader::GetVertexData()` 当前只返回 `meshes[0]`
- `RenderableObject` 当前面向单个 vertex/index buffer，而不是一个模型内的多 submesh / 多材质
- `Vertex` 当前没有 SpeedTree wind 或 multi-pivot 所需的额外属性
- 当前全局 UBO 还没有独立 wind 参数块
- 当前 PBR shader 没有 foliage 专用 direct lighting / transmission 分支
- 外部运行时资源默认位于 `../VulkanLearnAssets/resources`

因此路线必须先把“可加载、可显示、可验证”的静态树跑通，再逐步扩展资产格式和 shader 逻辑。

## 设计原则

- 先做 foliage material，不急着改通用 PBR 成一个庞大 uber shader。
- 第一版 SSS 走薄表面透射模型，不做屏幕空间 SSS。
- alpha cutout 叶片必须先能进 shadow pass，否则风和 SSS 的视觉判断都会失真。
- 风力先走工程内自定义参数，等链路稳定后再研究 SpeedTree 原生风数据。
- multi-pivot 依赖资产数据，不在 shader 里硬猜 pivot。
- 数据正确性由源头保证，shader 不做每帧冗余修正。
- 保持每个阶段都能独立验收，不把导入、材质、风、pivot 混成一次大改。

## 最终目标形态

理想完成态的数据流如下：

```text
SpeedTree export / converted asset
    -> model descriptor JSON
    -> trunk / leaf material instance JSON
    -> SceneLoader loads foliage scene object
    -> RenderableObject or mesh sections hold per-part geometry
    -> foliage shader
        -> alpha cutout
        -> normal mapped lighting
        -> thin-surface transmission
        -> wind deformation
        -> multi-pivot branch / leaf rotation
    -> shadow pass uses matching alpha cutout and wind deformation
    -> post-process consumes final HDR scene
```

第一版完成时，不要求完全还原 SpeedTree 官方 shader。真正重要的是 VulkanLearn 内部拥有一条清楚、可调试、可继续升级的植被链路。

## 阶段 0：资产约定与最小测试场景

目标：

- 明确第一棵树怎么放入外部资源目录
- 建立一个最小 foliage 测试场景
- 明确 SpeedTree 导出文件里 trunk、branch、leaf 等 mesh section 与 material slot 的命名

建议资产目录：

```text
../VulkanLearnAssets/resources/
  scenes/
    scene_foliage_test.json
  models/
    foliage/
      SM_speedtree_test_tree.json
      speedtree_test_tree.fbx
      speedtree_test_tree_static.glb
  materials/
    foliage/
      MI_speedtree_trunk.json
      MI_speedtree_leaf.json
  textures/
    foliage/
      speedtree_test_tree/
        bark_basecolor.*
        bark_normal.*
        bark_pbr.*
        leaf_basecolor_alpha.*
        leaf_normal.*
        leaf_pbr.*
        leaf_transmission.*
```

第一棵树至少要能识别两个明确材质部分：

- trunk / branch：普通 PBR opaque
- leaf cards：foliage alpha cutout

如果第一轮急着验证材质，仍可临时离线拆成两个模型描述文件：

```text
SM_speedtree_test_tree_trunk.json
SM_speedtree_test_tree_leaf.json
```

但推荐主线不再依赖这个拆分方案。SpeedTree 资产天然就是多 section / 多材质，模型导入地基应先支持一个模型资源内的多个 mesh section。

涉及文件：

- `../VulkanLearnAssets/resources/scenes/scene_foliage_test.json`
- `../VulkanLearnAssets/resources/models/foliage/*.json`
- `../VulkanLearnAssets/resources/materials/foliage/*.json`
- `config/config.json`

验收标准：

- 启动时能加载 foliage 测试场景
- 测试资产的 mesh section 和 material slot 名称明确
- 资源路径和材质实例路径清楚，不依赖仓库内 `resources/`

暂不处理：

- SpeedTree 风
- multi-pivot
- 完整 foliage shader

## 阶段 1：模型内多 mesh / 多材质导入地基

目标：

- 不再要求离线拆出 trunk 和 leaf 两个 scene object
- 让一个 SpeedTree 导出文件可以保留多个 mesh section 和多个材质槽
- 让后续 foliage shader、shadow、wind 都建立在真实资产结构上

当前限制是 `ModelLoader` 读取了多个 `Mesh`，但 `GetVertexData()` 只返回 `meshes[0]`。阶段 1 需要把这个临时结构升级成模型资源：

```cpp
struct MeshSection
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::string sectionName;
    std::string materialSlotName;
};

struct ModelResource
{
    std::vector<MeshSection> sections;
};
```

### 下一步升级：materialSlots

`materialSlots` 是阶段 1 的第一块可落地升级。它的职责是把模型文件里的材质槽名称映射到 VulkanLearn 的 material instance JSON，而不是让一个 mesh asset JSON 只能拥有单个 `materialInstancePath`。

### 模块拆分：Mesh Loader 与 Validation

`materialSlots` 不应该直接堆进 `SceneLoader::LoadMeshObject()`。推荐模仿现有 `source/material/` 结构，建立 `source/mesh/` 目录：

```text
source/mesh/
  meshAssetTypes.h
  loader/
    meshAssetResolver.h
    meshAssetResolver.cpp
    modelLoader.h
    modelLoader.cpp
  validation/
    meshAssetValidator.h
    meshAssetValidator.cpp
```

头文件注释遵循 `documents/architecture/coding-guidelines.md`。本模块第一轮落地时尤其要写清楚：

- `MeshAssetResolver` 头文件要说明它只展开 mesh asset JSON 的 effective data，不导入模型、不创建材质。
- `MeshAssetValidator` 头文件要说明它把 effective JSON 转成 load plan，并负责报出格式错误；不读取文件、不做模糊匹配、不修正资产命名。
- `ModelLoader` 头文件要说明它只导入 FBX / GLB / OBJ 的 geometry sections 和 material slot 名；不认识 VulkanLearn material instance JSON。

第一版只需要 `MeshAssetResolver` 和 `MeshAssetValidator` 两个模块。JSON 读取可以继续在 `SceneLoader` 调用处完成，避免为了读文件再多建一个 thin wrapper。

SpeedTree `.stsdk` 也走同一条 `SM_xxx.json` 入口，不单独发明 scene object 类型。`.stsdk` 是模型源文件格式，`SM_xxx.json` 仍然是 VulkanLearn 的模型资产描述，并保持现有 `name / type / modelDataPath` 的平铺风格：

```json
{
  "name": "Oak Complex Rules",
  "type": "speedtree",
  "modelDataPath": "models/datas/Oak_Complex_Rules.stsdk",
  "materialSlots": [
    {
      "name": "BarkBase",
      "materialInstancePath": "materials/foliage/MI_oak_bark.json"
    },
    {
      "name": "LeafSummer",
      "materialInstancePath": "materials/foliage/MI_oak_leaf_summer.json"
    }
  ]
}
```

第一版字段约定：

- `type = "mesh"` 时按普通 Assimp 模型处理。
- `type = "speedtree"` 时，`ModelLoader` 分派到 `SpeedTreeParserCore` / 后续 SDK-backed importer，并要求 `modelDataPath` 指向 `.stsdk`。
- `materialSlots` 字段是有序数组，并且不能为空。
- `type = "speedtree"` 时，`materialSlots` 的数量必须与 `.stsdk` 解析出的 SpeedTree material 数量一致；数组顺序对应解析出的 SpeedTree material 顺序，`name` 仅用于配置可读性和调试，不参与映射。
- SpeedTree 内部贴图路径仍可由 parser 读取，但 VulkanLearn material instance 绑定必须由 `materialSlots` 显式提供。
- 第一版不做 `importCache`，所有数据直接从 `.stsdk` 解析出来。
- scene 仍然只通过 `modelPath` 指向 `SM_xxx.json`。

调用链：

```text
SceneLoader reads SM_*.json
    -> MeshAssetResolver resolves inherited/default asset data
    -> MeshAssetValidator::BuildLoadPlan()
    -> ModelLoader imports mesh or dispatches speedtree by SM type
    -> MeshAssetValidator::BuildSectionLoadPlans()
    -> SceneLoader creates renderable sections and material instances
```

职责边界：

- `SceneLoader`
  - 读取 scene object 的 `modelPath`
  - 读取 `SM_*.json`
  - 根据验证后的 load plan 创建 `RenderableObject` / section scene object
  - 调用已有 `LoadMaterialInstance()`

- `MeshAssetResolver`
  - 对齐 material 侧 `MaterialInstanceResolver`
  - 第一版可以不做复杂继承，只保留接口位置
  - 后续如果 mesh asset 需要 defaults、preset 或平台覆写，在这里展开成 effective asset JSON

- `MeshAssetValidator`
  - 对齐 material 侧 `MaterialInstanceValidator`
  - 提供 `BuildLoadPlan()` 和 `BuildSectionLoadPlans()`
  - 校验 `type` 必须是 `mesh` 或 `speedtree`
  - 校验 `modelDataPath`
  - 禁止旧字段 `materialInstancePath`
  - 要求 `materialSlots` 必填且非空
  - `speedtree` 解析 `.stsdk` 后校验 `materialSlots` 数量与解析出的 material 数量一致
  - 校验 material slot 的 `name` 和 `materialInstancePath` 必须是非空字符串
  - 在模型 section 信息可用后，按源材质槽顺序映射到 `materialSlots`，不使用 `materialSlots.name` 做匹配
  - 输出清晰错误：mesh asset path、model path、section name、slot name

- `ModelLoader`
  - 位于 `source/mesh/loader/`
  - 只负责导入模型几何和材质槽名，或按 SM asset `type` 调用专用导入器
  - 输出 `ModelResource.sections`
  - 不知道 VulkanLearn material instance JSON

推荐数据结构：

```cpp
struct MeshAssetResolveResult
{
    nlohmann::json effectiveMeshAssetJson;
};

struct MeshMaterialSlot
{
    std::string name;
    std::string materialInstancePath;
};

struct MeshAssetBuildPlan
{
    std::string meshAssetPath;
    std::string modelDataPath;
    std::vector<MeshMaterialSlot> materialSlots;
    std::string modelCacheKey;
};

struct MeshSection
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::string sectionName;
    std::string materialSlotName;
};

struct MeshSectionLoadPlan
{
    std::string sectionName;
    std::string materialSlotName;
    std::string materialInstancePath;
};
```

推荐接口形状：

```cpp
class MeshAssetResolver
{
public:
    static MeshAssetResolveResult Resolve(
        std::string_view meshAssetPath,
        const nlohmann::json& meshAssetJson);
};

class MeshAssetValidator
{
public:
    static MeshAssetBuildPlan BuildLoadPlan(
        std::string_view meshAssetPath,
        const nlohmann::json& effectiveMeshAssetJson);

    static std::vector<MeshSectionLoadPlan> BuildSectionLoadPlans(
        const MeshAssetBuildPlan& buildPlan,
        const ModelResource& modelResource);
};
```

调用形状应该接近 `SceneLoader::LoadMaterialInstance()`：

```cpp
nlohmann::json meshAssetJson;
std::ifstream meshAssetFile(CommonFunction::Path(meshPath));
meshAssetFile >> meshAssetJson;

MeshAssetResolveResult resolveResult =
    MeshAssetResolver::Resolve(meshPath, meshAssetJson);
const nlohmann::json& effectiveMeshAssetJson = resolveResult.effectiveMeshAssetJson;
MeshAssetBuildPlan loadPlan =
    MeshAssetValidator::BuildLoadPlan(meshPath, effectiveMeshAssetJson);
ModelResource modelResource = ModelLoader::GetInstance().LoadModel(CommonFunction::Path(loadPlan.modelDataPath));
std::vector<MeshSectionLoadPlan> sectionPlans =
    MeshAssetValidator::BuildSectionLoadPlans(loadPlan, modelResource);
```

第一步可以先建立 `source/mesh/` 文件夹和 `MeshAssetResolver + MeshAssetValidator`。`MeshAssetResolver` 第一版只原样返回 effective JSON。即使 `ModelLoader` 还没完全升级，也先把 JSON 格式迁移和错误边界独立出来。

推荐迁移规则：

- 旧格式不再支持：

```json
{
  "name": "Sculpture",
  "type": "mesh",
  "modelDataPath": "models/SM_viking_room.obj",
  "materialInstancePath": "materials/MI_viking_room.json"
}
```

- 新格式必须使用 `materialSlots`。即使只有一个材质，也写成一个 slot：

```json
{
  "name": "SpeedTree Test Tree",
  "type": "mesh",
  "modelDataPath": "models/foliage/speedtree_test_tree.fbx",
  "materialSlots": [
    {
      "name": "Bark",
      "materialInstancePath": "materials/foliage/MI_speedtree_trunk.json"
    },
    {
      "name": "Leaves",
      "materialInstancePath": "materials/foliage/MI_speedtree_leaf.json"
    }
  ]
}
```

解析规则：

- `materialSlots` 是必填字段。
- 加载器不再读取 `materialInstancePath`。
- 如果 mesh asset JSON 仍包含 `materialInstancePath`，加载失败并提示迁移到 `materialSlots`。
- 如果缺少 `materialSlots`，加载失败并输出模型路径。
- 如果 `materialSlots` 为空，加载失败。
- 如果 `type = "speedtree"` 且 `modelDataPath` 不是 `.stsdk`，加载失败。
- section 材质绑定按源材质槽顺序映射到 `materialSlots`，不使用 `materialSlots.name` 做匹配；无法确定源槽序号时加载失败并输出 section 名称、slot 名称和模型路径。
- 如果 `type = "speedtree"`，还要校验 `materialSlots` 数量与 `.stsdk` 解析出的 material 数量一致；数组顺序对应 SpeedTree material 顺序，`name` 仅用于配置可读性和调试。
- 第一版不做模糊匹配，不自动大小写修正，不自动猜 trunk/leaf；材质槽正确性由资产/JSON 源头保证。

第一版验收标准：

- 所有旧 mesh asset JSON 都迁移到 `materialSlots`。
- 单材质模型也通过 `materialSlots` 绑定材质实例。
- 一个 SpeedTree FBX / GLB / `.stsdk` 可以通过 `materialSlots` 绑定树干和叶片两个材质实例。
- 渲染分组仍能按 material/materialInstance 分组，不破坏现有 `RenderSystem` 提交流程。
- 错误信息能定位到缺失的 material slot，而不是只报泛泛的 load failed。

模型描述 JSON 需要能把 material slot 映射到材质实例：

```json
{
  "modelDataPath": "models/foliage/speedtree_test_tree.fbx",
  "materialSlots": [
    {
      "name": "Bark",
      "materialInstancePath": "materials/foliage/MI_speedtree_trunk.json"
    },
    {
      "name": "Leaves",
      "materialInstancePath": "materials/foliage/MI_speedtree_leaf.json"
    }
  ]
}
```

渲染侧可以有两种落地方式：

1. 一个 `SceneObject` 持有多个 `RenderableObject + MaterialInstance`
2. loader 在加载时展开成多个内部 scene object

第一种更干净，第二种改动更小。当前学习阶段可以优先选择第二种，只要命名、生命周期和调试输出清楚。

涉及文件：

- `source/mesh/meshAssetTypes.h`
- `source/mesh/loader/meshAssetResolver.h`
- `source/mesh/loader/meshAssetResolver.cpp`
- `source/mesh/loader/modelLoader.h`
- `source/mesh/loader/modelLoader.cpp`
- `source/mesh/validation/meshAssetValidator.h`
- `source/mesh/validation/meshAssetValidator.cpp`
- `source/renderableObject.h`
- `source/renderableObject.cpp`
- `source/sceneLoader.cpp`
- `source/renderSystem.cpp`
- mesh asset JSON 格式

验收标准：

- 一个 SpeedTree 模型文件可显示 trunk、branch、leaf 多部分
- 每个 section 绑定自己的 material instance
- 现有普通模型迁移到 `materialSlots` 后仍可加载
- 出错信息能指出缺失的是哪个 section 或 material slot

暂不处理：

- meshlet
- GPU culling
- indirect draw
- LOD / billboard

## 阶段 2：静态树导入与多部分显示

目标：

- 让一棵静态树在当前渲染系统中稳定显示
- 通过阶段 1 的 material slot 映射打通树干 opaque 和叶片 alpha cutout 两种材质
- 先用现有 PBR shader 验证贴图、法线、切线、UV 和 alpha

第一步优先使用现有能力：

- 树干材质使用 `shader = "pbr"`、`renderMode = "Opaque"`
- 叶片材质使用 `shader = "pbr"`、`renderMode = "OpaqueClip"`
- 叶片材质使用 `cullMode = "None"`
- alpha 阈值继续放在 `u_pbrFactors.w`

模型描述文件应把材质槽映射到材质实例，而不是靠多个 scene object 表达一棵树：

```text
SpeedTree_Test_Tree -> SM_speedtree_test_tree.json
  Bark   -> MI_speedtree_trunk.json
  Leaves -> MI_speedtree_leaf.json
```

离线拆成 trunk / leaf 两个模型只作为阶段 1 卡住时的应急验证手段，不再作为推荐主线。

涉及文件：

- `source/mesh/loader/modelLoader.cpp`
- `source/vertexDataStruct.h`
- `shader/glsl/pbr.vert`
- `shader/glsl/pbr.frag`
- `../VulkanLearnAssets/resources/models/foliage/*.json`
- `../VulkanLearnAssets/resources/materials/foliage/*.json`
- `../VulkanLearnAssets/resources/scenes/scene_foliage_test.json`

可能需要的小修：

- 确认 Assimp 读取 SpeedTree 导出格式时是否保留 vertex color、UV、normal、tangent
- 如果导出模型没有 tangent，继续用 MikkTSpace 生成
- 如果叶片 UV 或 winding 反了，只修导出或模型处理中的明确约定，不在 shader 里临时反补

验收标准：

- 树干贴图正确
- 叶片贴图正确
- 叶片 alpha cutout 边界可接受
- 叶片双面可见
- normal map 没有明显接缝或高光方向错误
- shadow pass 中叶片轮廓和主 pass 轮廓一致

暂不处理：

- 透射
- 风
- LOD
- billboards
- SpeedTree 官方材质完整还原

## 阶段 3：Foliage 材质与薄表面 SSS

目标：

- 从普通 PBR 分出 foliage shader 或 foliage material variant
- 实现第一版树叶背光透射
- 把“叶片像薄片透光”作为可调材质效果，而不是后处理效果

推荐新 shader：

```text
shader/glsl/foliage.vert
shader/glsl/foliage.frag
shader/glsl/common/foliageLighting.glsl
```

推荐材质参数：

```glsl
layout(set = 1, binding = 0) uniform UBOMIParamters {
    vec4 u_tintColor;
    vec4 u_pbrFactors;          // x roughness, y metallic, z ao, w alphaClipThreshold
    vec4 u_transmissionColor;   // rgb color, a strength
    vec4 u_foliageFactors;      // x wrap, y backPower, z normalBlend, w shadowTransmission
    float u_emissiveStrength;
};
```

推荐贴图：

```glsl
layout(set = 1, binding = 1) uniform sampler2D albedoMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;
layout(set = 1, binding = 3) uniform sampler2D pbrParamMap;
layout(set = 1, binding = 4) uniform sampler2D emissionMap;
layout(set = 1, binding = 5) uniform sampler2D transmissionMap;
```

第一版 foliage direct lighting 可分成三项：

```text
front diffuse/specular:
    沿用当前 PBR direct lighting 的简化版本

back transmission:
    light 从叶背方向进入时，根据 dot(-N, L)、view、厚度/贴图增加透射颜色

indirect lighting:
    暂时沿用 SH diffuse IBL + specular IBL
```

建议公式从简单开始：

```glsl
float backLit = pow(max(dot(-N, L), 0.0), u_foliageFactors.y);
float wrapped = max((dot(N, L) + u_foliageFactors.x) / (1.0 + u_foliageFactors.x), 0.0);
vec3 transmission = lightColor * lightIntensity
    * baseColor
    * transmissionColor
    * transmissionStrength
    * backLit
    * wrapped;
```

这里不追求物理严格，重点是参数含义清楚：

- `transmissionColor`：叶片透过光的颜色
- `transmissionStrength`：透射强度
- `wrap`：让光绕过薄片边缘的程度
- `backPower`：背光聚集程度
- `shadowTransmission`：阴影里是否仍保留少量透光

涉及文件：

- `shader/glsl/foliage.vert`
- `shader/glsl/foliage.frag`
- `shader/glsl/common/foliageLighting.glsl`
- `source/materialInstanceValidator.cpp`
- `../VulkanLearnAssets/resources/materials/foliage/MI_speedtree_leaf.json`

验收标准：

- 顺光时叶片仍像普通叶子，不整体发亮
- 逆光时叶片边缘和薄片区域明显透亮
- 调低 transmission strength 后可以回到接近普通 PBR
- alpha cutout 仍稳定
- debug view 下 base color、normal、roughness、shadow 仍可判断

暂不处理：

- 屏幕空间 SSS blur
- thickness shadow map
- per-leaf self shadow approximation
- 多层叶冠体积透射

## 阶段 4：阴影与 Foliage 专用 Shadow Pass 对齐

目标：

- 确保 foliage 的 alpha cutout 和未来 wind deformation 同时作用于 shadow pass
- 避免主 pass 里叶子在动，阴影却不动

当前 shadow pass 使用 `materials/MI_shadow.json`。当 foliage 顶点阶段开始加入风之后，shadow pass 也必须使用相同的顶点变形逻辑。

推荐路线：

- 把 alpha cutout shadow 作为第一步
- 后续把 wind deformation 公共函数放入 include 文件
- `foliage.vert` 和 `foliageShadow.vert` 共享同一个 wind deformation 函数

推荐 shader 文件：

```text
shader/glsl/common/foliageWind.glsl
shader/glsl/foliageShadow.vert
shader/glsl/foliageShadow.frag
```

第一版 shadow frag 只需要做 alpha discard：

```glsl
vec4 albedo = texture(albedoMap, uv) * u_tintColor;
if (albedo.a < u_pbrFactors.w) {
    discard;
}
```

涉及文件：

- `config/renderGraphConfig.json`
- `shader/glsl/shadow.vert`
- `shader/glsl/shadow.frag`
- `shader/glsl/foliageShadow.vert`
- `shader/glsl/foliageShadow.frag`
- `source/sceneLoader.cpp`
- `source/renderSystem.cpp`

验收标准：

- 叶片阴影不是整张 card 的矩形
- foliage 主 pass 和 shadow pass 使用一致 alpha threshold
- 后续打开 wind 后，树影会跟着动

暂不处理：

- transparent shadow
- colored transmission shadow
- deep opacity map

## 阶段 5：全局 Wind 参数与简单顶点风

目标：

- 建立工程内的第一版 wind 数据入口
- 在顶点 shader 里让树冠动起来
- 不依赖 SpeedTree 原生风数据也能学习和调试

推荐新增全局参数：

```glsl
struct WindParameters {
    vec4 directionStrength;  // xyz direction WS, w strength
    vec4 timeFrequency;      // x time, y trunkFrequency, z branchFrequency, w leafFrequency
    vec4 gust;               // x strength, y frequency, z scale, w reserved
    vec4 bend;               // x trunk, y branch, z leaf, w reserved
};
```

CPU 侧可以先把它放进现有 global UBO，后续如果 `UBOGlobal` 继续膨胀，再拆成独立 `GlobalSet` buffer。

第一版顶点风只依赖已有数据：

- `inPosition.y` 作为高度权重
- `inColor.r` 作为 branch/leaf bend mask
- `inColor.g` 作为相位偏移
- `inColor.b` 作为刚性或叶片权重

如果资产没有 vertex color，可以先用高度生成权重。这个生成应放在导入或转换阶段，而不是 shader 每帧猜。

推荐函数：

```glsl
vec3 ApplySimpleFoliageWind(
    vec3 positionOS,
    vec3 normalOS,
    vec3 color,
    WindParameters wind)
{
    float heightWeight = color.r;
    float phase = color.g * 6.2831853;
    float stiffness = color.b;
    float t = wind.timeFrequency.x;
    float sway = sin(t * wind.timeFrequency.z + phase + positionOS.y * 0.2);
    vec3 offset = wind.directionStrength.xyz
        * wind.directionStrength.w
        * wind.bend.y
        * heightWeight
        * (1.0 - stiffness)
        * sway;
    return positionOS + offset;
}
```

涉及文件：

- `source/commonFunction.h`
- `source/renderSystem.cpp`
- `shader/glsl/common/commonUbo.glsl`
- `shader/glsl/common/foliageWind.glsl`
- `shader/glsl/foliage.vert`
- `shader/glsl/foliageShadow.vert`
- `config/config.json`

验收标准：

- 树干基本不动，树冠明显摆动
- 风向、强度、频率可通过 config 调整
- 主 pass 和 shadow pass 同步摆动
- 静止时可通过 strength = 0 回到阶段 2 效果

暂不处理：

- SpeedTree 官方风曲线
- branch hierarchy
- leaf flutter 独立 pivot
- GPU culling bounds 更新

## 阶段 6：SpeedTree 风数据接入评估

目标：

- 调研当前 SpeedTree 导出资产实际包含哪些 wind 数据
- 只接入已经能被 Assimp 或转换工具稳定读出的数据
- 确定 VulkanLearn 自己的 foliage vertex layout V1

SpeedTree 风数据可能落在不同位置：

- vertex color
- extra UV channel
- custom attribute
- per-vertex branch id / weight
- per-leaf anchor / pivot
- per-material wind settings

当前 `Vertex` 只有一个 UV 和一个 color，因此阶段 6 的重点不是立刻全接，而是定义一份中间格式。

推荐 `FoliageVertex`：

```cpp
struct FoliageVertex
{
    Eigen::Vector3f position;
    Eigen::Vector3f normal;
    Eigen::Vector3f color;
    Eigen::Vector2f texCoord0;
    Eigen::Vector4f tangent;
    Eigen::Vector4f wind0; // x branchWeight, y leafWeight, z phase, w stiffness
    Eigen::Vector4f wind1; // xyz primaryPivotOS, w pivotWeight
    Eigen::Vector4f wind2; // xyz secondaryPivotOS, w hierarchyLevel
};
```

不要急着把 `Vertex` 全局替换成 `FoliageVertex`。更稳的方向是让 pipeline state 支持不同 vertex layout：

- Static mesh：继续使用 `Vertex`
- Foliage mesh：使用 `FoliageVertex`
- Fullscreen pass：继续 `bUseVertexInput = false`

涉及文件：

- `source/vertexDataStruct.h`
- `source/mesh/loader/modelLoader.h`
- `source/mesh/loader/modelLoader.cpp`
- `source/renderableObject.h`
- `source/renderableObject.cpp`
- `source/pipeline/graphicsPipeline.cpp`
- `source/pipeline/graphicsPipeline.h`
- `source/materialInstanceValidator.cpp`
- `shader/glsl/foliage.vert`

验收标准：

- 普通 PBR 模型不受 foliage vertex layout 影响
- foliage shader 可以读取 wind0 / wind1 / wind2
- 没有 wind 数据的 foliage 资产可以通过转换工具生成默认值
- pipeline cache key 能区分不同 vertex layout

暂不处理：

- 自动识别所有 SpeedTree 格式
- 多 LOD
- GPU-driven vegetation

## 风力数据导入方式

SpeedTree 官方资料里需要区分两种“风”：

1. **离线缓存风**
   - Modeler 的 VFX Wind 可以导出 FBX point cache 或 Alembic。
   - 这种方式更像影视/VFX 动画缓存，导入后是逐帧几何动画。
   - 官方也说明这类风动画不循环，需要导出足够覆盖镜头的帧数。
   - 这不适合作为 VulkanLearn 第一版实时游戏树风，因为它不方便用统一风向、风强、gust、LOD 和实例化去驱动。

2. **实时 SDK 风**
   - SpeedTree SDK 路线里，`.stsdk` / `CCore` 保存 Modeler 调好的风配置。
   - `CWindStateMgr` 根据时间、风强、gust 等状态生成 shader constants。
   - 顶点 shader 再结合每个顶点携带的风权重、leaf group、pivot/axis 等数据做变形。
   - 这条路线最完整，但会把项目立刻带进 SpeedTree SDK 资源格式、运行时类和 shader 移植，不适合第一版。

VulkanLearn 第一版采用第三条中间路线：

```text
SpeedTree static mesh export
    -> Assimp / converter reads sections, material slots, vertex streams
    -> VulkanLearn FoliageVertex wind0 / wind1 / wind2
    -> mesh asset JSON records wind import policy and optional defaults
    -> scene/config provides global WindParameters
    -> foliage.vert and foliageShadow.vert consume the same wind data
```

长期目标是支持完整 SpeedTree 游戏风。这里的“完整”不是指解析 FBX point cache，而是指：

- 能读取 SpeedTree Modeler 中由艺术家调好的 SDK 风配置
- 能读取或转换 SpeedTree 顶点风数据，包括 branch level、leaf group、weights、phase、pivot/axis、frond/leaf ripple/tumble 等
- CPU 侧能根据时间、风强、风向和 gust 生成与 SpeedTree 风语义一致的 shader constants
- GPU 侧 foliage vertex shader 能执行与 SpeedTree SDK wind mode 对齐的 branch / leaf / frond 变形
- wind LOD 能支持 Full / Branch / Global / None 这种分级

要达到这个目标，推荐路线是 SDK-backed importer，而不是逆向 `.stsdk`：

```text
.stsdk / SpeedTree runtime asset
    -> SM_xxx.json model descriptor
    -> type = "speedtree"
    -> SpeedTreeParserCore or SDK-backed importer
    -> importer exposes geometry, LOD, material, wind config
    -> VulkanLearn converts geometry to MeshSection / FoliageVertex
    -> CWindStateMgr or equivalent wrapper updates wind constants
    -> VulkanLearn uploads SpeedTreeWindParameters
    -> GLSL port of SpeedTree wind shader functions
    -> foliage and foliageShadow passes share the same deformation
```

### UE 5.7 参考结论

本机 UE 5.7 已确认存在可用的 SpeedTreeImporter 参考链：

```text
D:\sofeware\Epic Games\UE_5.7\Engine\Plugins\Editor\SpeedTreeImporter
D:\sofeware\Epic Games\UE_5.7\Engine\Shaders\Private\SpeedTreeCommon.ush
D:\sofeware\Epic Games\UE_5.7\Engine\Source\Runtime\Engine\Public\SpeedTreeWind.h
D:\sofeware\Epic Games\UE_5.7\Engine\Source\Runtime\Engine\Private\SpeedTreeWind.cpp
```

UE5.7 支持 `.srt`、`.st`、`.st9` 导入；v9 走 SpeedTree GameEngine9 loader，导入后创建 `UStaticMesh`、material instances 和 SpeedTree9 master material 派生实例。它对 VulkanLearn 最有价值的不是资产 UI，而是三个事实：

- SpeedTree 树按 material slot / draw call 创建 StaticMesh material slots，这与 VulkanLearn 的 `materialSlots` 迁移方向一致。
- SpeedTree9 风数据会作为顶点属性进入 mesh，UE 将它们打包到 UV 通道。
- 实时风由 CPU 侧风状态更新和 GPU 侧 vertex deformation 共同完成，不依赖 Modeler 的 OpenGL 截帧，也不依赖 FBX point cache。

UE5.7 的 SpeedTree9 顶点 UV 风布局：

```text
UV0: Diffuse UV
UV1: Branch1Pos, Branch1Dir
UV2: Branch1Weight, RippleWeight
UV3: Lightmap UV
UV4: Branch2Pos, Branch2Dir      if Branch2 data exists
UV5: Branch2Weight, unused       if Branch2 data exists
UV6+: camera-facing anchor data  if facing geometry exists
```

这会作为 VulkanLearn 后续 `SpeedTreeVertex` / `FoliageVertex` 设计的重要参考。第一版不直接照搬 UE 的 UV 号位；转换器应该把这些语义转成 VulkanLearn 自己命名清楚的属性，例如 branch1、branch2、ripple、blend、cameraFacingAnchor。

如果暂时没有 SDK 授权或头文件，就只能做到“兼容 SpeedTree 导出资产的自定义风”，不能承诺完全还原官方 SDK 风。FBX / GLB 的静态 mesh 通常不足以携带完整实时风配置；VFX Wind 导出的 FBX point cache / Alembic 又是离线动画缓存，不适合作为大规模游戏实时风主线。

`.stsdk` 的正式接入入口仍是 `SM_xxx.json`。也就是说，场景层不关心 SpeedTree 文件格式，scene object 只写 `modelPath`；mesh asset 描述通过 `type: "speedtree"` 和 `modelDataPath` 表达 `.stsdk` 来源。第一版不写 `importCache`，数据都直接从 `.stsdk` 解析出来；但材质绑定必须由非空 `materialSlots` 有序数组显式声明，并与 `.stsdk` 解析出的材质数量一致，数组顺序对应 SpeedTree material 顺序，`name` 仅用于配置可读性和调试。这样普通模型、SpeedTree `.stsdk`、未来 `.st9` probe 都能复用同一套 mesh asset resolver / validator / section material 映射。

### 导入阶段的数据来源

优先级从高到低：

1. SpeedTree 导出文件里明确存在的 extra vertex attributes
2. 额外 UV channel 或 vertex color 中可稳定识别的 wind 权重、phase、leaf group
3. 转换工具根据 mesh section / material slot / leaf card bounds 生成的 pivot 与权重
4. mesh asset JSON 中显式写入的默认策略

不要在 shader 每帧根据 position 临时猜 pivot。可以在转换阶段根据几何生成默认值，但生成结果必须落到模型数据或派生资产里，便于调试和复现。

推荐模型描述字段：

```json
{
  "modelDataPath": "models/foliage/speedtree_test_tree.fbx",
  "vertexLayout": "Foliage",
  "materialSlots": [
    {
      "name": "Bark",
      "materialInstancePath": "materials/foliage/MI_speedtree_trunk.json"
    },
    {
      "name": "Leaves",
      "materialInstancePath": "materials/foliage/MI_speedtree_leaf.json"
    }
  ],
  "windImport": {
    "mode": "Generated",
    "source": "SpeedTreeStaticMesh",
    "useVertexColor": true,
    "useExtraUv": true,
    "generateLeafPivotFromSectionBounds": true,
    "defaults": {
      "branchWeight": 0.5,
      "leafWeight": 1.0,
      "stiffness": 0.2
    }
  }
}
```

推荐顶点属性语义：

```cpp
wind0.x = branchWeight;
wind0.y = leafWeight;
wind0.z = phase;
wind0.w = stiffness;

wind1.xyz = primaryPivotOS;
wind1.w   = pivotWeight;

wind2.xyz = secondaryPivotOS;
wind2.w   = hierarchyLevel;
```

### 运行时数据来源

风的“天气/场景状态”不从模型里来，而从 scene/config 来：

```json
{
  "wind": {
    "direction": [1.0, 0.0, 0.2],
    "strength": 0.35,
    "trunkFrequency": 0.25,
    "branchFrequency": 0.8,
    "leafFrequency": 2.4,
    "gustStrength": 0.4,
    "gustFrequency": 0.15
  }
}
```

CPU 每帧只更新 `time`、风向、风强、gust 等少量全局参数。每个顶点的权重、phase、pivot 和层级来自导入后的 `FoliageVertex`，主 pass 与 shadow pass 共享同一份 `foliageWind.glsl`，保证树和树影同步。

### 后续接近 SpeedTree SDK 的路线

等第一版中间格式稳定后，再评估两种升级：

- 写一个 SpeedTree 专用转换器，把 SpeedTree 导出的额外属性更完整地映射到 `FoliageVertex`。
- 引入 SpeedTree SDK-backed importer，用 `CCore` 读取树资产，用 `CWindStateMgr` 或等价封装生成风常量。
- 把 SpeedTree SDK wind shader 逻辑逐步移植为 VulkanLearn GLSL include，并用 debug view 对齐 branch、leaf、frond 分量。
- 保留当前 shader 侧的 `ApplyFoliageWind()` 接口，让近似风和完整 SpeedTree 风能通过不同 backend 切换。

## 完整 SpeedTree 风接入阶段

这个阶段可以放在阶段 6 之后、multi-pivot 之前，也可以作为单独长期专题推进。

在定义 VulkanLearn 自己的正式 foliage runtime asset 格式前，先执行 `documents/rendering/speedtree-sdk-data-probe.md`。该探针只负责通过 SpeedTree SDK 枚举 `.stsdk` 暴露的数据，并输出 verbose probe JSON；不要在第一版探针里提前固定 `SpeedTreeVertex`、`VLFoliageAsset` 或最终 wind buffer 布局。

目标：

- 建立 `SpeedTreeWindBackend` 抽象，隔离 SDK 依赖和 VulkanLearn 渲染代码
- 支持读取 SpeedTree SDK 风配置，而不是只读取静态 mesh
- 建立 SpeedTree 顶点风数据到 `FoliageVertex` 或专用 `SpeedTreeVertex` 的映射
- 建立 SpeedTree wind constants 到 Vulkan uniform / storage buffer 的映射
- 移植或重写 SpeedTree branch / leaf / frond wind shader 函数
- 支持 wind LOD：Full、Branch、Global、None

推荐分步：

1. **SDK 可用性验证**
   - 验证本地能否链接 SpeedTree SDK
   - 加载一个 `.stsdk` 测试树
   - 枚举 geometry、material、LOD 和 wind config

2. **几何与材质转换**
   - 把 SDK 输出的 draw calls / geometry groups 转成 `MeshSection`
   - 把材质槽映射到 VulkanLearn material instance
   - 保留 SDK 顶点风属性原始 dump，用于对照 shader

3. **风状态更新**
   - 用 SDK 的 `CWindStateMgr` 根据时间、strength、direction、gust 更新 wind state
   - 定义 `SpeedTreeWindParameters` buffer
   - 把 SDK wind state 映射到 Vulkan uniform / storage buffer

4. **Shader 移植**
   - 先移植 Global wind
   - 再移植 Branch wind
   - 再移植 Leaf ripple / tumble
   - 最后处理 Frond wind 和高级 turbulence

5. **对照验证**
   - 同一棵树在 SpeedTree Modeler / SDK reference app / VulkanLearn 中对比
   - 对齐静止姿态、风向、强度响应、gust、leaf group 和 LOD 切换

验收标准：

- 同一个 SpeedTree SDK 风配置能驱动 VulkanLearn 中的树
- 风强、风向、gust 变化和 Modeler / SDK reference app 趋势一致
- Full / Branch / Global / None 四级风 LOD 能切换
- 主 pass 和 shadow pass 完全同步
- 没有 SDK 时，工程仍可回退到 `Generated` / `Approximate` wind backend

不做的事：

- 不逆向未授权的 `.stsdk` 私有二进制格式
- 不把 FBX point cache 当作实时游戏风主线
- 不为了完整 SpeedTree 风破坏普通 foliage 的轻量路径

## 阶段 7：Multi-Pivot 基础变形

目标：

- 让 branch 和 leaf 围绕资产提供的 pivot 摆动
- 从“整体平移式风”升级到“层级旋转式风”
- 为 SpeedTree 风、树枝分层和叶片 flutter 打基础

multi-pivot 的最小数据：

```text
primaryPivotOS:
    当前顶点所属枝条或叶片簇的主要旋转中心

secondaryPivotOS:
    可选的更高层枝条 pivot

branchWeight:
    当前顶点受枝条摆动影响的权重

leafWeight:
    当前顶点受叶片局部 flutter 影响的权重

phase:
    每个枝条或叶片簇的相位偏移

stiffness:
    越硬越少动
```

推荐先做两层：

- branch sway：围绕 `primaryPivotOS` 做低频旋转
- leaf flutter：围绕 `secondaryPivotOS` 或 leaf center 做高频小旋转

推荐数学工具：

```glsl
vec3 RotateAroundAxis(vec3 p, vec3 pivot, vec3 axis, float angle)
{
    vec3 v = p - pivot;
    float s = sin(angle);
    float c = cos(angle);
    vec3 rotated = v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
    return pivot + rotated;
}
```

这里的 pivot 必须来自资产或转换结果。不要根据顶点位置在 shader 里猜 pivot，否则不同模型会表现不稳定。

涉及文件：

- `shader/glsl/common/foliageWind.glsl`
- `shader/glsl/foliage.vert`
- `shader/glsl/foliageShadow.vert`
- `source/mesh/loader/modelLoader.cpp`
- foliage asset conversion notes or scripts

验收标准：

- 枝条以合理根部为中心摆动，不是整片叶冠平移
- 叶片有局部 flutter，但不会脱离枝条
- 主 pass 和 shadow pass 完全同步
- strength = 0 时回到静态树

暂不处理：

- 真实树木物理模拟
- 碰撞反馈
- 分层骨骼动画

## 阶段 8：Foliage 调试视图与参数工作台

目标：

- 给 foliage 开发提供可视化调试入口
- 让 SSS、风、pivot 的问题可以快速定位

建议新增 debug view：

- alpha mask
- transmission map
- transmission strength
- front lighting
- back transmission
- wind weight
- wind phase
- branch pivot distance
- leaf weight
- final wind offset length

现有 `uboVP.debugViewMode` 已经能支持 PBR debug view。可以继续扩展编号，或者后续拆成更明确的 debug view registry。

涉及文件：

- `shader/glsl/foliage.frag`
- `shader/glsl/foliage.vert`
- `shader/glsl/common/commonUbo.glsl`
- `source/renderSystem.cpp`
- `config/config.json`

验收标准：

- 能单独观察 alpha、transmission、wind weight、wind offset
- 风或 pivot 出问题时，不需要靠肉眼猜
- debug view 不影响普通 PBR shader 的现有效果

暂不处理：

- UI 编辑器
- 实时材质面板

## 阶段 9：质量与性能整理

目标：

- 在功能跑通后整理质量和性能边界
- 明确哪些是 foliage 系统的默认行为，哪些是资产责任

关注点：

- alpha cutout 的 mip 和边缘质量
- normal map 与双面 foliage 的法线策略
- shadow acne / peter panning 对叶片的影响
- wind 后的包围盒是否需要放大
- 叶片 overdraw 对 bloom / tone mapping 的影响
- material variant 数量是否可控
- shader include 是否避免重复逻辑

推荐策略：

- alpha clip threshold 由材质实例提供
- 双面法线第一版使用 `cullMode=None`，不强制翻面
- wind bounds 先由模型描述 JSON 提供额外 bounds padding
- shader variant 控制在 `pbr`、`foliage`、`foliageShadow` 这一级，不把所有实验塞进一个 shader

涉及文件：

- `source/renderableObject.h`
- `source/renderSystem.cpp`
- `shader/glsl/foliage.frag`
- `shader/glsl/foliage.vert`
- `documents/rendering/texture-asset-json-v1.md`
- mesh asset JSON docs

验收标准：

- 静态普通模型没有被 foliage 改动拖慢或破坏
- foliage 场景在默认窗口尺寸下稳定运行
- shader 编译和反射路径稳定
- 文档记录了 foliage 资产必须提供的数据

## 推荐实施顺序

实际开发时建议按以下顺序开小 PR / 小提交：

1. 加 foliage 测试场景和 SpeedTree 测试资产
2. 建立 SpeedTree SDK 数据探针计划，先拆 `.stsdk` 能暴露哪些数据
3. 建立 `source/mesh/` 文件夹结构
4. 新增 `MeshAssetResolver`
5. 新增 `MeshAssetValidator`
6. 迁移 mesh asset JSON 到必填 `materialSlots`
7. 迁移 `ModelLoader` 到 `source/mesh/loader/` 并输出多 section `ModelResource`
8. 建立 section material slot -> material instance 映射
9. 用现有 PBR + OpaqueClip 显示静态树干和叶片
10. 根据第一份 SDK probe JSON 再决定 VulkanLearn foliage runtime asset / vertex layout
11. 新建 `foliage` shader，复制并收敛 PBR 基础逻辑
12. 加 thin-surface transmission 参数和贴图
13. 让 foliage shadow alpha cutout 正确
14. 加全局 wind 参数和简单风
15. 把 wind deformation 共享到 foliage 主 pass 和 shadow pass
16. 接入 pivot 数据并实现 multi-pivot
17. 增加 foliage debug view
18. 整理性能、bounds、文档和示例资产

如果只想先拿到可见成果，最小闭环是：

```text
阶段 0 -> 阶段 1 -> 阶段 2 -> 阶段 3 -> 阶段 5
```

这条最小闭环完成后，会得到一棵以真实多 section 模型加载、能显示、能透光、能摆动的树。之后再补 shadow 对齐、SpeedTree 风数据和 multi-pivot 会更有把握。

## 关键取舍

### 为什么先做 foliage transmission，而不是完整 SSS

树叶属于薄表面。第一版用 direct light 的背光透射就能获得主要观感，并且能直接和材质参数、贴图、阴影、风联动。屏幕空间 SSS 更适合皮肤或蜡质物体，会把学习重点转移到后处理、depth / normal buffer 和 blur kernel 上，不适合作为植被第一步。

### 为什么不直接照搬 SpeedTree shader

SpeedTree shader 通常包含大量产品化路径、LOD、billboard、平台宏和资产约定。VulkanLearn 当前更适合先建立自己的小型 foliage shader，然后逐步吸收 SpeedTree 数据。这样每个参数都能解释，调试也更可控。

### 为什么多 submesh / 多材质要前置

SpeedTree 资产天然会把 trunk、branch、leaf cards、fronds 等拆成多个 mesh section 和材质槽。如果第一版继续靠离线拆文件或多个 scene object 表达一棵树，后续 foliage shadow、wind deformation、LOD、bounds 和 debug view 都会在临时结构上继续叠债。因此多 submesh / 多材质导入应作为阶段 1 前置地基。

离线拆成 trunk / leaf 两个文件仍可作为短期诊断手段：当材质或贴图有问题时，用它快速排除导入器问题。但它不再是推荐实施主线。

### 为什么 multi-pivot 放在 wind 之后

multi-pivot 不是一个单独 shader 技巧，它依赖资产里的 pivot、权重、相位和层级信息。如果全局 wind 参数和主 pass / shadow pass 同步变形还没打通，先做 multi-pivot 会很难判断问题来自数据、数学还是渲染 pass。

## 第一轮建议落地范围

第一轮最推荐实现到：

- foliage 测试场景
- `source/mesh/` 文件夹结构
- `MeshAssetResolver`
- `MeshAssetValidator`
- mesh asset JSON `materialSlots` 迁移
- 模型内多 mesh / 多材质导入
- material slot 到材质实例映射
- 静态 SpeedTree 树显示
- trunk opaque 材质
- leaf alpha cutout 双面材质
- `foliage` shader
- thin-surface transmission
- 简单 wind 参数和顶点摆动

第一轮暂时不做：

- SpeedTree 原生风完全还原
- multi-pivot
- LOD / billboard
- 屏幕空间 SSS

第一轮完成后的效果应该是：场景里有一棵树，叶片不是矩形 card，逆光时能透亮，风强度调高时树冠会动，调成 0 时完全静止。这就是后续接 SpeedTree 风和 multi-pivot 的稳定地基。
