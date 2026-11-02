# 描边壳运行时法线平滑合同

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、
> 资源字段或运行日志；不含对未来行为的承诺。

本文描述倒置壳描边在硬边处闭合裂口所使用的一次性 compute 平滑。它只修改被资产显式
标记的描边 section 自己的顶点缓冲，不改变本体 section 的着色法线，也不引入第二套
材质求值路径。

## 资产触发

静态 mesh 资产（`type: "mesh"`）可选声明：

```json
"smoothNormalsSections": ["Outline"],
"outlineMaskSections": ["Body"]
```

- 数组元素按 **`materialSlots[].name`** 匹配，而不是源模型 group / section 名。
  OBJ 的 `g gun_outline` 不是资产契约。
- 元素必须非空、不可重复，并且必须对应已声明的 material slot。
- `type: "speedtree"` 或带 `skinning` 的骨骼 mesh 拒绝该字段；
  平滑只支持静态网格。
- 字段只影响运行时 Renderable 的创建与 compute 入队，不改变 `modelCacheKey` 的
  几何导入缓存身份：同一 OBJ 的 A/B/C 资产仍共享 CPU 导入结果。
- `outlineMaskSections` 标记本体 section：该 draw 的 `UBOModel.selectionData.y = 1`，
  GBuffer velocity 的 `.w` 通道写 1，forward 描边据此裁掉本体覆盖。

校验落点：

- `source/mesh/validation/meshAssetValidator.cpp`（`ReadMaterialSlotNameList`）
- `source/mesh/meshAssetTypes.h`（`MeshAssetBuildPlan::smoothNormalsSections` /
  `outlineMaskSections`）

## Renderable 与顶点缓冲

`RendererMeshLoader` 只为命中的 section 在 renderable key 后追加
`|outlineSmooth`。这是必需的：多个 mesh 资产常常共享同一 `modelDataPath` 和
`modelCacheKey`，若共用同一个 `RenderableObject`，compute 写回的平滑法线会污染
未声明该标记的实例。

静态描边 section 的顶点缓冲同时带 `eVertexBuffer | eStorageBuffer | eTransferDst`，
索引缓冲带 `eIndexBuffer | eStorageBuffer`。compute 把整份缓冲当裸 `float` /
`uint` 数组访问：

- 位置：`f[base + 0..2]`
- 法线：`f[base + 3..5]`
- `base = vertexIndex * (sizeof(Vertex) / sizeof(float))`

本体 section 不声明该字段，因此其顶点缓冲不会被 compute 绑定或写入。

## Compute 三阶段

`source/render/outline/outlineNormalSmoother.*` 拥有三条
`CreateComputePipelineCandidate` 管线，shader 位于 `shader/glsl/outline/`：

1. `outlineSmoothWeld.comp`
   - 位置乘 `quantScale = 1000`（0.001 cm）后取整；
   - 槽里存**完整 32 位 key**，key 0 保留为空槽；同 key 的复制顶点复用同一槽；
   - 线性探测处理桶冲突，避免把不同位置误焊在一起。
2. `outlineSmoothAccumulate.comp`
   - 每个三角形按 `0.5 * cross(p1-p0, p2-p0)` 做面积加权；
   - 用三角形三个源顶点法线之和给几何面法线定向；点积为负时翻转，
     防止少量反绕序面（如弹夹薄壁）把壳推向内部；
   - 用 `atomicAdd(int)` 定点（`4096`）累加，不依赖 float 原子扩展。
3. `outlineSmoothApply.comp`
   - 归一化面积加权结果并写回单位法线；
   - 累加长度过小（两侧法线几乎相消）时保留源法线，避免壳塌陷。

参数块是 `std140` uniform，不是 `std430`；后者会要求
`GL_EXT_scalar_block_layout`。

## C 的材质来源与一致性

C 的描边宽度、颜色、fade、bias 和材质参数与经典 `M_gunOutlineB` 变体完全一致：

- `MI_gun_outline_C.json` 引用 `M_gunOutlineSmooth.json`；它是 `M_gunOutlineB.json`
  的副本，参数、shader 和 render mode 相同，只把 `cullMode` 改成 `None`；
- 场景 C 的 `OutlineWidth` 为 1.0，与经典 B 变体相同；
- `u_viewBias` 使用材质默认值 0，C 没有额外偏置。

C 只比经典 B 变体多两件事：描边 section 的顶点法线被 compute 焊接平滑，以及本体 section
在 GBuffer mask 中写 1。`cullMode = None` 是 mask 技术的一部分：`cull Front` 会把
平滑后圆角外沿的部分三角当作正面剔掉，造成轮廓局部断续；`cull None` + 本体 mask
才能保留完整外沿。`M_gunOutlineB` 仍保持自己的 `cull Front`；当前 demo 场景只保留
A/C 两把枪，B 资产作为参考变体保留。

## 本体 mask：GBuffer 而不是 stencil buffer

当前渲染图没有启用带 stencil 的深度格式；`sceneDepth` 是 `D32_SFLOAT`，
`graphicsPipelineBuilder.cpp` 固定 `stencilTestEnable(false)`。直接给
`forwardTransparent` 加 stencil 还会遇到非 MSAA 前向 pass 与 geometry stencil
未 resolve 的边界。

因此本体 mask 复用已有的 GBuffer 保留通道：

- `outlineMaskSections` 命中的本体 draw 在 `UBOModel.selectionData.y` 写 1；
- `base.frag.glsl` 对非 Eye 表面把 `selectionData.y` 写进 `gbufferVelocity.w`
  （`.z` 继续留给编辑器选中，避免触发 toneMapping 的选中外扩轮廓）；
- `config/renderGraphConfig.json` 的 `forwardTransparent.input` 增加
  `gbufferVelocity` binding 1；
- `M_gunOutlineSmooth.surface.glsl` 采样 `gbufferVelocity.a`，大于 0.5 时 `discard`；
- 所有 transparent 材质在 `materialSurface.glsl` 声明该 sampler，`base.frag`
  以一个恒 0 权重引用它，保证 set 3 binding 1 进入所有透明材质的 descriptor layout。

这等价于 stencil mask 的可见效果：本体像素上的壳被裁掉，壳只在本体轮廓之外
可见。A 的本体与经典 B 变体都不写 `outlineMaskSections`，所以它们的
`gbufferVelocity.w = 0`，描边行为不变。

## 法线一致性验证

`VL_OUTLINE_DEBUG_READBACK=1` 时，`OutlineNormalSmoother` 会把 compute 写回的顶点
缓冲复制到 host-visible 缓冲，并在下一次使用该 swapchain image 时逐顶点与 CPU
面积加权参考比对。2026-11-02 在 C 的 16086 个描边顶点上实测：

```text
[OutlineSmooth][Debug] gpuVsCpu vertices=16086 negativeDot=0 dotBelow0.9=0
  nonUnit=0 minDot=0.999533 maxAngleDeg=1.75178
```

这表示 compute 输出与 CPU 参考方向一致（最大误差来自定点累加/舍入），没有反法线。
如果画面再出现局部缺边，应先检查壳的 cull/mask，而不是继续怀疑 compute 法线。

## 帧内位置与资源生命周期

- `RendererMeshLoader` 在实时 World 加载期只把 job 入队；缩略图/预览候选不提供
  `OutlineNormalSmoother`，不会修改候选缓存对象。
- `RenderSystem::RecordAndSubmitCurrentRenderScene()` 在
  `BeginFrame` 成功、环境 IBL compute 之后、任何图形 pass 之前调用
  `OutlineNormalSmoother::RecordPending()`。
- 每个 swapchain image 独占 hash / rep / accum / params 缓冲和 descriptor pool。
  `BeginFrame` 已等待该 image 的 fence，所以复用前不需要 device/queue 访问器，
  也不需要 `waitIdle()`。
- 每个 job 用独立的 descriptor set 和 params 偏移；同一帧多个 job 不会因为
  录制期改写描述符而互相覆盖。
- 最后一个 job 后插入 `compute -> vertex input / vertex shader` 内存屏障，
  同帧后续 shadow / gbuffer / transparent pass 能读到新法线。

## 日志与边界

运行时输出带 `[OutlineSmooth]` 前缀：

```text
[OutlineSmooth] enqueue <renderable> vertices=... triangles=... hashSlots=... uniquePositions=... occupancy=...
[OutlineSmooth] record image=... jobs=... dispatches=... threads=[...] groups=[...] cpuRecordMs=...
[OutlineSmooth] gpuMs=... image=... jobs=...
```

`uniquePositions` 是量化位置元组的精确去重数；`occupiedSlots` 用与 shader 完全相同的
32 位 key 在 CPU 复算，因此 `occupancy = occupiedSlots / hashSlots` 精确对应 GPU
哈希表实际占用（含 key 碰撞合并），不需要读回。`threads` 是三次 dispatch 的
invocation 数：weld/apply 为顶点数，accumulate 为三角形数。

当前已知边界：

- **一次性**：`RenderableObject::IsOutlineNormalsReady()` 置位后不再重复 dispatch；
  资源缓存命中且已平滑的缓冲也不会重跑。
- **按位置焊接**：只共享位置、没有共享边的两个表面也会合并。模型在接触/穿插处
  可能因此得到更厚的圆角壳；这是位置焊接的当前行为，不是管线错误。
- **平滑只作用于描边 section**：它不修复本体硬表面法线，也不改变材质的宽度曲线。
  壳宽、颜色、fade 仍由 `M_*` / `MI_*` 参数控制。
- **静态网格**：骨骼网格的描边壳需要额外的蒙皮空间处理，当前拒绝该字段。
