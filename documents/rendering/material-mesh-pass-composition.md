# Material Evaluation 与 Mesh Pass 组合合同

本文记录 VulkanLearn 当前已经实现的轻量材质 Pass 组合架构。它只覆盖
`Base` 与 `ShadowDepth`，不表示已经实现 Material Graph、DepthOnly、Velocity
或通用 Vertex Factory 系统。

## 职责边界

```text
M_ 默认值 + MI_ override
    -> effective render state
    -> MaterialFeatureKey / MaterialDescriptorSchema
    -> MaterialShaderComposer
    -> ShaderCompiler
    -> SPIR-V reflection 子集校验
    -> GraphicsPipeline
```

- Material Evaluation 只实现 `EvaluateMaterialVertex()` 和
  `EvaluateMaterialInputs()`。
- Pass Template 持有 `main()`、投影、Alpha Clip 和 Pass 输出。
- `MaterialShaderComposer` 只组合 include、feature define 与模板，不解析 GLSL。
- `ShaderCompiler` 继续负责 shaderc、SPIR-V、优化和 debug reflection artifact。
- RenderGraph 仍负责 Pass 资源和 Pipeline Contract；Composer 不修改 RenderGraph。

## M_ 与 MI_ 分工

支持组合的 M_ 材质显式声明 Evaluation 源码：

```json
"shaderEvaluation": {
    "vertex": "M_pbr.vertex.glsl",
    "surface": "M_pbr.surface.glsl"
},
"features": {
    "modifiesMeshPosition": false
}
```

`shaderEvaluation` 和 `modifiesMeshPosition` 属于 M_ 静态定义，MI_ 不能覆写。
其余 Feature 必须在 M_ 与 MI_ 合并后从 effective render state 推导：

- `writesEveryPixel`: `renderMode == Opaque`
- `usesOpacityMask`: `renderMode == OpaqueClip`
- `twoSided`: `cullMode == None`
- `modifiesMeshPosition`: M_ 的显式静态声明

同一个 M_ 可以因此生成多个 Base/ShadowDepth shader variant。MI 仍以规范化
MI asset path 保持运行时唯一，不会为 ShadowDepth 创建第二份 MI。

## Shader 接口

```glsl
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput);
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context);
```

顶点 Evaluation 只返回局部空间材质结果。Base 与 ShadowDepth 模板分别完成
世界空间和裁剪空间变换。片元 Evaluation 只填 `MaterialInputs`，不得调用
`ApplyAlphaClip()`；需要 coverage 的模板在消费 Surface 后统一执行 Clip。

第一版固定 `vertexFactoryKey = StaticMesh`，对应当前固定顶点输入 locations。
这个 key 已进入 shader identity，但尚未提供可插拔 Vertex Factory 实现。

## Set 1 合同

M_ 的 `parameters` 和 `textures` 生成完整 `MaterialDescriptorSchema`：

- `set 1, binding 0`: std140 材质 UBO
- `set 1, binding 1..N`: 按 schema 稳定顺序排列的材质贴图
- 第一版所有 Set 1 binding 的 stage visibility 固定为 Vertex + Fragment

同一个 Schema 同时驱动 generated GLSL、Pipeline layout、MI UBO 大小与 offset。
SPIR-V reflection 只描述某个 Pass 实际使用的 binding，并必须是 Schema 子集。

Descriptor pool 按完整 layout 分配；descriptor write 和纹理必填校验按当前
Material 的 Base + ShadowDepth reflection 并集执行。这样未使用的可选贴图不需要
伪造默认资源，而显式 Shadow override 单独使用的 schema 贴图仍会正确写入。

## ShadowDepth 路由

路由顺序固定：

1. 完整 `xxx.shadow.vert/.frag` 使用显式 override。
2. 无 override 的透明材质跳过普通 Shadow Map。
3. `usesOpacityMask`、`modifiesMeshPosition` 或 `twoSided` 自动组合 ShadowDepth。
4. 其余普通 Opaque 使用唯一公共 Opaque Shadow pipeline。

不完整 `.shadow` 文件对只产生警告，然后继续正常自动路由。完整 override 或
自动生成 shader 的编译/反射合同错误会终止加载，不会静默回退。需要自动
ShadowDepth 的旧式 standalone shader 若没有 `shaderEvaluation`，同样是明确的
资产错误。

材质专用 ShadowDepth 复用对象已有 Set 0..2：Set 0/2 必须是 Base engine
reflection 的兼容子集，Set 1 使用完整 Material Schema，Set 3 禁止使用。

## Identity 与所有权

组合 shader identity 包含：

```text
material source + MaterialPass + MaterialFeatureKey + VertexFactoryKey
+ renderMode + shadingModel + static macros + compile target
```

`GraphicsShaderVariantArtifact` 保存实际 SPIR-V/debug 路径和 reflection，Pipeline
不再从 shader name 反推产物路径。Pipeline identity 继续额外包含 Pass Pipeline
Contract、cull、blend 和 descriptor layout contract。

`Material` 持有 Base pipeline、可选 ShadowDepth pipeline、完整 Set 1 Schema 和
两个 Pass 的 active binding 并集。`MaterialInstance` 只持有一份动态参数、贴图和
descriptor 资源。
