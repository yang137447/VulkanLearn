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
- mesh JSON 通过 `modelDataPath` 指向模型文件，通过 `materialInstancePath` 指向材质实例
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

模型描述 JSON 需要能把 material slot 映射到材质实例：

```json
{
  "modelDataPath": "models/foliage/speedtree_test_tree.fbx",
  "materialSlots": {
    "Bark": "materials/foliage/MI_speedtree_trunk.json",
    "Leaves": "materials/foliage/MI_speedtree_leaf.json"
  }
}
```

渲染侧可以有两种落地方式：

1. 一个 `SceneObject` 持有多个 `RenderableObject + MaterialInstance`
2. loader 在加载时展开成多个内部 scene object

第一种更干净，第二种改动更小。当前学习阶段可以优先选择第二种，只要命名、生命周期和调试输出清楚。

涉及文件：

- `source/modelLoader.h`
- `source/modelLoader.cpp`
- `source/renderableObject.h`
- `source/renderableObject.cpp`
- `source/sceneLoader.cpp`
- `source/renderSystem.cpp`
- model descriptor JSON 格式

验收标准：

- 一个 SpeedTree 模型文件可显示 trunk、branch、leaf 多部分
- 每个 section 绑定自己的 material instance
- 现有普通模型仍可按原格式加载
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

- `source/modelLoader.cpp`
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
- `source/modelLoader.h`
- `source/modelLoader.cpp`
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
- `source/modelLoader.cpp`
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
- model descriptor JSON docs

验收标准：

- 静态普通模型没有被 foliage 改动拖慢或破坏
- foliage 场景在默认窗口尺寸下稳定运行
- shader 编译和反射路径稳定
- 文档记录了 foliage 资产必须提供的数据

## 推荐实施顺序

实际开发时建议按以下顺序开小 PR / 小提交：

1. 加 foliage 测试场景和 SpeedTree 测试资产
2. 升级多 mesh / 多材质模型加载
3. 建立 material slot -> material instance 映射
4. 用现有 PBR + OpaqueClip 显示静态树干和叶片
5. 新建 `foliage` shader，复制并收敛 PBR 基础逻辑
6. 加 thin-surface transmission 参数和贴图
7. 让 foliage shadow alpha cutout 正确
8. 加全局 wind 参数和简单风
9. 把 wind deformation 共享到 foliage 主 pass 和 shadow pass
10. 定义 foliage vertex layout V1
11. 接入 pivot 数据并实现 multi-pivot
12. 增加 foliage debug view
13. 整理性能、bounds、文档和示例资产

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
