# Material Multi-Pass 与 PassTag 方案

## 文档状态

本文档记录 VulkanLearn 后续 Material Multi-Pass 的讨论结论和候选数据格式。

- 状态：规划，尚未实现
- 目标：为后续评审保留一个明确起点
- 非当前契约：当前运行时代码和资产不能依赖本文中的 `passes`、`passTag` 或 Hook 格式

近期 ShadowCaster 不先落地本文的完整 `passes` / `passTag` 模型，而按
`documents/plan/rendering/lightweight-material-shadow-caster-plan.md` 实施：只保留公共 Opaque，
通过 `xxx.shadow.vert/.frag` 文件约定提供材质专用 ShadowCaster。本文继续作为完整
Material Multi-Pass 的长期规划，不是近期 ShadowCaster 的资产合同。

## 背景

当前材质定义由以下内容组成：

- `M_*.json`：材质默认参数、纹理、宏和静态状态
- `MI_*.json`：材质实例覆盖值
- 同名 `.vert/.frag`：当前主要 Surface Shader
- RenderGraph：全局 Pass 顺序、附件、资源依赖和 barrier

当前一个 Material 主要对应一套 vertex/fragment 和一条 Surface Pipeline。
Shadow 则由 RenderGraph Pass 自己持有公共 Shadow Material。这使普通不透明阴影很轻量，
但不能自然表达以下能力：

- 一个材质同时提供 `GBuffer`、`ShadowCaster`、`DepthOnly` 等实现
- 主 Pass 与 Shadow Pass 共享 Alpha Clip、WPO、Wind 或 Skinning 规则
- 同一物体参与 Outline、Hair Depth/Color、Selection Mask 等附加绘制
- 在一个材质资产中完整查看它支持的所有绘制方式

## 参考结论

### Unity

Unity ShaderLab 用 `Pass` 收拢 Shader 的多个绘制实现，用 `LightMode` Tag 让 Renderer
选择当前阶段所需的 Pass。全局顺序仍由 Built-in Renderer、URP/HDRP Renderer 或
RenderGraph 决定，不由 Shader 文件中的文本顺序直接决定。

任意自定义 `LightMode` 不会自动执行。Renderer 必须存在一个对应的 Draw Pass，或由
Renderer Feature / Custom Pass 在指定注入点向 RenderGraph 注册节点。

### Unreal Engine

UE 更强调固定 Mesh Pass：Depth、Base、Shadow、Velocity、CustomDepth 等由 Renderer
集中定义，Material 根据 Domain、Blend Mode、Shading Model、Masked、WPO 等属性参与。
MeshPassProcessor 会提前生成和缓存 MeshDrawCommand，而不是在每个 RDG Pass 执行时
重新查询所有材质。

### VulkanLearn 取向

采用两者的组合：

- 保留显式 RenderGraph，作为全局调度和资源依赖的唯一真相源
- 采用 Unity 风格的 Material Pass 资产组织和 `passTag` 匹配
- 采用 UE 风格的预编译 Draw List，不在每个 Pass 中遍历所有对象并做字符串查询
- 常见附加绘制通过 RG 预置 Hook 使用，复杂效果仍显式扩展 RG

## 核心术语

### RenderGraph Pass

负责：

- 什么时候执行
- 读取和写入哪些资源
- attachment、load/store、layer 和 sample count
- Pass 间依赖、layout transition 和 barrier
- 使用 scene、fullscreen 还是 compute executor

### Material Pass

负责：

- 当前物体在某个绘制角色下使用哪些 Shader Stage
- cull、depth、blend、stencil 等固定管线状态
- 当前 Pass 反射后实际使用哪些材质参数和纹理

### PassTag

Material Pass 的选择键，等价于 Unity 中用于 Renderer 选择的 `LightMode` 概念。

第一版约束：一个 Material 内同一个 `passTag` 最多对应一个 Material Pass。

### Hook Pass

RenderGraph 中预置的通用 scene draw 节点。Hook Pass 与普通 RG Pass 没有本质区别，
只是为常见扩展位置提供稳定契约，避免每增加一个简单材质效果就修改主图结构。

## 责任关系

```text
RenderGraph Pass
    决定顺序、资源和执行类型
        |
        | requests passTag
        v
Material Pass
    提供 Shader Stage 和 Pipeline State
        |
        v
Graphics Pipeline
    RG compatibility + Material Pass + Shader Variant
```

示例：四个 CSM RG Pass 可以请求同一个 `ShadowCaster` Tag。

```text
shadowCascade0 --+
shadowCascade1 --+--> Material Pass: ShadowCaster
shadowCascade2 --+
shadowCascade3 --+
```

## Material Definition JSON 草案

`passes` 使用对象而不是数组。对象的 key 就是 `passTag`，避免同时出现 `name`、
`phase`、`hook` 和 `passTag` 等重复概念。

```json
{
  "schemaVersion": 2,
  "name": "M_speedtree",
  "type": "material",
  "shadingModel": "DefaultLit",
  "renderMode": "OpaqueClip",

  "passDefaults": {
    "cullMode": "Back"
  },

  "macros": {
    "USE_ALBEDO_MAP": 1,
    "USE_NORMAL_MAP": 1
  },

  "parameters": {
    "u_tintColor": {
      "type": "vec4",
      "default": [1.0, 1.0, 1.0, 1.0]
    },
    "u_alphaClipThreshold": {
      "type": "float",
      "default": 0.5
    }
  },

  "textures": {
    "albedoMap": {
      "type": "sampler2D",
      "default": null
    }
  },

  "passes": {
    "GBuffer": {
      "vertex": "speedtree.vert",
      "fragment": "speedtree.frag",
      "renderStates": {
        "depthTest": true,
        "depthWrite": true,
        "depthCompare": "Less",
        "blendMode": "Opaque",
        "colorWriteMask": "RGBA"
      }
    },

    "ShadowCaster": {
      "vertex": "shadow/speedtree.vert",
      "fragment": "shadow/speedtreeAlphaClip.frag",
      "renderStates": {
        "cullMode": "None",
        "depthTest": true,
        "depthWrite": true,
        "depthCompare": "Less",
        "depthBias": {
          "enable": true,
          "constantFactor": 1.25,
          "slopeFactor": 1.75,
          "clamp": 0.0
        }
      }
    },

    "AfterOpaque": {
      "vertex": "effects/treeOutline.vert",
      "fragment": "effects/treeOutline.frag",
      "renderStates": {
        "cullMode": "Front",
        "depthTest": true,
        "depthWrite": false,
        "depthCompare": "LessEqual",
        "blendMode": "Alpha"
      }
    }
  }
}
```

## Shader Stage 路径

第一版直接指定 `vertex` 和 `fragment`，不再根据 `M_*.json` 文件名推导同名 Shader Pair。

- 路径相对 `shader/glsl/`
- 路径必须包含扩展名
- 使用 `/` 作为资产路径分隔符
- vertex 和 fragment 可以来自不同目录
- Depth-only Opaque Pass 可允许省略 fragment
- 有颜色输出或需要 `discard` 的 Pass 必须提供 fragment

Shader Program/Variant 的缓存键至少应包含：

```text
vertex source path
+ fragment source path or none
+ normalized macros
+ shading model
+ render mode / pass-specific static variant inputs
```

## 参数和反射

- `parameters`、`textures` 和材质宏属于整个 Material 的公开接口
- MaterialInstance 继续只保存实例值，不为每个 Pass 创建一份 MI
- 每个 Material Pass 独立编译和反射
- 每个 Pass 只绑定最终 SPIR-V 实际使用的资源
- 参数声明生成器继续服务 Shader 编写体验
- SPIR-V 反射继续作为 descriptor layout、write 和校验的运行时真相源

`renderMode` 是材质分类和静态变体语义，不应再暗中覆盖每个 Pass 的 depth/blend 状态。
Alpha Clip 的数值参数使用独立的 `u_alphaClipThreshold`，不再打包进 PBR 参数分量。

## Render State 草案

第一版候选状态：

- `cullMode`: `Back`、`Front`、`None`
- `depthTest`: boolean
- `depthWrite`: boolean
- `depthCompare`: `Never`、`Less`、`Equal`、`LessEqual`、`Greater`、`NotEqual`、`GreaterEqual`、`Always`
- `blendMode`: `Opaque`、`Alpha`、`PremultipliedAlpha`、`Additive`
- `colorWriteMask`: `RGBA`、`RGB`、`A`、`R`、`RG`、`None`
- `depthBias`: enable、constant factor、slope factor、clamp
- `stencil`: enable、compare、pass/fail/depthFail、reference、readMask、writeMask

状态合并顺序：

```text
engine defaults
    -> passDefaults
    -> current Material Pass renderStates
```

顶层不再使用含义模糊的公共 `renderStates`。`renderMode` 单独放在材质根部；
真正的 Graphics Pipeline State 属于 Material Pass。

Stencil 的 `reference`、`readMask`、`writeMask` 使用固定八位二进制字符串：

```json
{
  "reference": "0b00000001",
  "readMask": "0b00001111",
  "writeMask": "0b00000001"
}
```

格式必须匹配 `^0b[01]{8}$`。启用 Stencil 前还需要 RenderGraph 和 RHI 支持
`D24_UNORM_S8_UINT` 或 `D32_SFLOAT_S8_UINT` 等 Depth-Stencil 格式。

以下内容不属于 Material Render State：

- attachment 和 format
- load/store op
- MSAA sample count
- viewport/scissor 范围
- resource barrier/layout
- Pass 执行顺序

## RenderGraph JSON 草案

普通场景节点直接请求一个 `passTag`：

```json
{
  "name": "geometry",
  "type": "scene",
  "passTag": "GBuffer",
  "output": [
    { "resource": "gbufferA", "loadOp": "clear", "storeOp": "store" },
    { "resource": "sceneDepth", "loadOp": "clear", "storeOp": "store" }
  ]
}
```

通用 Hook 也是普通 RG scene 节点：

```json
{
  "name": "afterOpaqueHook",
  "type": "scene",
  "passTag": "AfterOpaque",
  "input": [
    { "resource": "sceneDepth" }
  ],
  "output": [
    {
      "resource": "sceneColor",
      "loadOp": "load",
      "storeOp": "store"
    }
  ]
}
```

建议第一批稳定 Tag/Hook：

- `ShadowCaster`
- `DepthOnly`
- `GBuffer`
- `AfterOpaque`
- `BeforeTransparent`
- `AfterTransparent`

只有 RG 已请求的 `passTag` 才会执行。Material 中声明任意自定义 Tag 不会自行修改或重排 RG。

## 简单 Hook 与复杂 Feature

简单附加绘制可直接使用预置 Hook：

- Outline 几何膨胀
- Selection Highlight
- X-Ray
- Hair Additional Layer
- Debug Wireframe

它们必须接受 Hook 已定义的相机、attachment 和资源访问契约。

需要自建中间资源或多个节点的功能，本质上仍是一段 RenderGraph：

- SSAO + Blur + Composite
- Outline Mask + Edge Detect + Composite
- SSR
- Weather/Cloud 多阶段更新

这类能力应使用模块化 RG Fragment。Fragment 只解决隔离、复用、启用和组合问题，
不应伪装成脱离 RenderGraph 的另一套调度系统。

新的执行机制仍需要 C++ Executor，例如：

- 特殊 Compute Dispatch
- GPU indirect draw
- 自定义剔除
- readback
- ray tracing

## Draw List 与性能

禁止在每个 RG Pass 执行时遍历全部对象并按字符串查询 Material Pass。

材质加载后应编译为稳定句柄和参与掩码：

```cpp
struct CompiledMaterial
{
    uint64_t supportedPassMask;
    std::array<MaterialPassHandle, PassCount> passes;
};
```

场景解析阶段把 Material Pass 解析进 Draw Packet，帧更新阶段完成可见性判断并分发到
对应 Draw List。RG Pass 只消费已经准备好的列表，并按 Pipeline/Material 排序。

```text
scene/material resolve
    -> pass handles and participation mask
camera visibility
    -> GBuffer DrawList
    -> DepthOnly DrawList
    -> Hook DrawLists
RenderGraph
    -> consume prepared lists
```

Shadow 需要独立 caster candidate list，因为光源视锥和每个 CSM Cascade 与主相机不同。
它仍不应重新做材质字符串查询。

一个物体参与多个 Pass 必然产生多次 GPU draw。Draw List 方案减少的是 CPU 侧重复查询、
过滤、排序和 command recording 开销，不会消除 Multi-Pass 本身的 GPU 成本。

## ShadowCaster 迁移方向

当前通用 Shadow 路径已经支持：

- `Opaque`：公共 depth-only Shadow Pipeline
- `Masked`：公共 masked Shadow Pipeline，直接从现有 Set 1 读取 `albedoMap`、
  `u_tintColor` 和 `u_alphaClipThreshold`

完整 Material Pass 落地后建议：

1. 简单 Opaque 继续允许公共 `ShadowCaster` fallback
2. 标准 Alpha Clip 可继续使用公共 masked fallback
3. 使用 WPO、Wind、Skinning 或特殊 opacity 计算的材质必须提供专用 `ShadowCaster`
4. Shadow vertex 必须复用主 Pass 的顶点变形函数
5. Transparent 默认不进入普通 Shadow Map，其他投影模型另行设计

## 第一版实施范围

建议第一版只实现：

1. `M_*.json` 增加 `passes` map
2. Pass key 作为唯一 `passTag`
3. 每个 Pass 直接指定 vertex/fragment
4. Material 根参数和纹理由所有 Pass 共享
5. 每个 Pass 独立编译、反射和创建 Pipeline
6. RG scene Pass 通过 `passTag` 选择材质实现
7. 支持 `GBuffer`、`ShadowCaster`、`DepthOnly`
8. 建立按 PassTag 分类的 Draw List
9. 再选择一个真实附加绘制需求验证通用 Hook

建议验证需求优先级：

```text
ShadowCaster migration
    -> DepthOnly
    -> one real Hook such as Outline or Selection
    -> Hair dual-pass or another ordered multi-pass case
```

## 第一版非目标

- ShaderLab 风格的新 DSL
- Shader Graph
- 同一个 Material 内多个相同 `passTag`
- Material 自动插入或重排 RenderGraph
- 自动执行 RG 未请求的自定义 Tag
- Geometry/Tessellation/Mesh Shader Stage
- 任意 Vulkan Blend Factor 全量暴露
- Colored/Translucent Shadow
- GPU Driven Draw List

GPU Driven 可作为后续优化：将可见性、LOD 和 indirect command 生成迁移到 Compute，
但不应成为 Material Multi-Pass 的前置条件。

## 待后续评审

- `passDefaults` 是否保留，还是要求每个 Pass 写完整状态
- MaterialInstance 是否允许覆盖静态 Pipeline State；若允许，覆盖范围和 variant 成本如何限制
- 第一批通用 Hook 的准确集合、attachment 契约和透明排序规则
- Depth-only Pass 缺省 fragment 的 Pipeline/反射实现细节
- Material Pass Handle、PassMask 和 Pipeline Cache Key 的最终类型
- RG Fragment 的文件格式、启用机制和冲突诊断
- 展开后的 RenderGraph dump/explain 工具格式

## 一句话结论

VulkanLearn 的目标不是复制 ShaderLab，而是保留显式 RenderGraph，同时让一个 Material
通过 `passTag` 集中提供多个绘制实现。RG 决定何时画和画到哪里，Material Pass 决定
当前对象怎么画，预编译 Draw List 负责让两者匹配而不引入逐 Pass 全场景查询。
