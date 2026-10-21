# Material Evaluation 与 Mesh Pass 组合合同

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或`tool/architecture-boundary-audit.ps1` 的门禁规则；不含对未来行为的承诺——那些在 `documents/rationale/`。

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

当前支持 `vertexFactoryKey = StaticMesh` 和 `SkinnedMesh`，共享固定顶点输入 locations。
SkinnedMesh 在材质求值前通过独立 Set 2 SSBO 执行完整 CSR LBS，
不改变静态 Vertex stride；key 进入 shader identity。
蒙皮材质要求 shaderEvaluation，拒绝材质 WPO 和完整 standalone shadow override。
其资源与历史合同见 [动画播放与蒙皮](animation-runtime-skinning.md)。

## 编辑器网格 pass：材质组合的显式例外

`config/renderGraphConfig.json` 里的 `editorGrid` pass（`type: "editorGrid"`）**不参与**
上面这条材质组合链：它的 shader 直接输出顶点色，没有 `shaderEvaluation`、没有
`MaterialInputs`、不接 Material Function、也不进 GBuffer / 阴影 / IBL。它仍然声明
`needCreateMaterial: true` 与 `materialInstancePath`，只是为了走既有的 pass 材质所有权与
热重载事务路径，不是因为需要材质求值。

它存在的理由与边界：

- **尺寸按 cm 定义**：小格 10 cm、每 10 格一条主线（1 m），XZ 各覆盖 -240 到 +240 cm。
  线半宽为 0.12/0.2 cm（次线/主线），Y 轴高 40 cm，地面抬升 0.02 cm。
  这是固定十进制参考网格，不是 UE 随视距自适应的完整实现，也不是 HDRI 自带的棋盘纹理。

- **它是场景几何，不是 UI 叠层。** 画在 `sceneColor` / `sceneDepth` 上、`loadOp: load`，
  因此深度测试能正确遮挡它。做成 UI 叠层反而要手工复现相机投影 —— 而相机的 aspect 已经
  由 `EngineLoop::SynchronizeSceneViewport` 按视口 rect 设好，跟着场景走就自动正确。
- **它画在 `forwardTransparent` 之后**，`depthWriteEnable: false`、`depthCompareOp: lessOrEqual`，
  所以共面时网格胜出、且它不写深度去影响后续 pass。
- **每个四边形输出正反两份绕序，这是必需的不是冗余。** 材质是 `cullMode: Back`
  （改成 TwoSided 会被要求生成材质资产无法提供的 ShadowDepth 组合），而地面条带的可见朝向
  随相机方位变化：只出一份绕序时，编辑器默认的俯视视角下整个地面网格都是背面、被整族剔除。
  2026-10-12 实测：开/关该 pass 只差 **66 px**；只把 `cullMode` 临时改成 `Front`，同一测量变成
  **40343 px**（611 倍）。所以这里的双绕序是"让可见性由深度测试决定，而不是由相机方向决定"。
  顶点数因此是逻辑四边形数 × 2（绕序）× 6：`EditorGridExpectedVertexCount()` 与其测试共用这一口径。
- **顶点流复用共享 `Vertex`**（`source/VertexDataStruct.h`：position=0、normal=1、color=2）。
  网格 shader 只消费 0 与 2，**跳过 normal** —— normal 的取值只为满足共享布局，不表达光照信息。
- 几何在 XZ 地面上抬升 `kGroundLift`，避免与场景自身的 y=0 地面在共面像素上随机归属；
  颜色是 tone mapping 前的线性值且全部低于 bloom 阈值，因此后处理不会把网格线当成高光。

几何所有权在 `source/render/editor/editorViewportGrid.*`（只管"网格长什么样"），
pass 状态与深度策略在配置与 `shader/glsl/pass/editorGrid.*`（管"怎么被画出来"）。

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

SkinnedMesh 是显式例外：强制自动组合 ShadowDepth，完整 standalone override 拒绝，
防止描述符虽兼容却只读绑定姿态位置。Base/ShadowDepth 共用同一蒙皮 factory。

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
