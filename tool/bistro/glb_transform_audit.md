# Godot Bistro GLB Transform Audit

审计对象：`VukanLearnResources/generated/bistro_godot_source/Meshes/**/*.glb`

## 结论

- 38 个渲染 GLB 共包含 1094 个 glTF 节点、953 个带 mesh 节点、953 个 mesh、953 个 primitive。
- 953/953 个 mesh 节点只使用 `translation`；没有 mesh 节点使用 `rotation`、`scale` 或 `matrix`。
- 519 个 mesh 节点是根节点，434 个 mesh 节点是深度 1 的子节点。后者必须累加父节点 translation，不能只把 mesh 节点自身 translation 写入顶点。
- 每个 mesh id 只被一个节点使用；没有 GLB 内部的 mesh/accessor 共享实例可利用。GLB 的模块化粒度是一个 GLB 一个 `SM_*`，而不是 GLB 内部节点实例化。
- 当前 `AssimpSourceAdapter::ProcessNode` 只递归 `aiNode::mMeshes`，不读取 `aiNode::mTransformation`。直接让引擎读取这些 GLB 会丢失所有 953 个节点的位移，所有部件会回到 mesh 局部坐标。

## 变换烘焙

推荐在资源转换阶段烘焙：

1. 按 glTF 规则从 scene roots 遍历节点。
2. 计算 `world = parentWorld * localNodeTransform`。
3. 用 world 矩阵变换 POSITION；用 inverse-transpose 的 3x3 变换 NORMAL/TANGENT，并重新归一化。
4. 处理完后移除节点上的 `translation/rotation/scale/matrix`，让导入器看到的是已经在场景坐标中的 section。
5. 保留一个 GLB 对应一个模型描述；不要合并 GLB 内 section，也不要把节点拆成大量 `SM_*`。

这些 GLB 目前只有 translation，因此不会触发旋转/缩放或负 determinant 的切线 handedness 问题；转换器仍应保留通用 inverse-transpose 逻辑。

## Godot wrapper / 材质风险

常规模块的 `.tscn` 会以 GLB PackedScene 为根，并通过 `surface_material_override/0` 覆盖材质。节点名中的 `.` 在 tscn 中通常被规范化为 `_`，因此使用 canonical name 或显式 node-index 映射。

有 10 个 `_Props.glb` 没有对应的 GLB-based wrapper：它们的 Godot `.tscn` 使用 `ArrayMesh` `.res`（例如 `S1Lamps_Props_Mesh_001.res`），而不是 `res://Meshes/.../*.glb`。因此不能从这些 wrapper 直接恢复材质槽：

```text
Section01/S1Lamps_Props.glb
Section02/Bistro_Lanterns.glb
Section02/BistroProps_Props.glb
Section02/S2Lamps_Props.glb
Section03/S3Lamps_Props.glb
Section04/S4B3_Props.glb
Section05/S5B1_Props.glb
Section05/S5B2_Props.glb
Section05/S5Lamps_Props.glb
```

这些 `_Props.glb` 应当暂时跳过，或者另写 `.res`/Godot import 解析器；不能选择一个“最相似”的普通 wrapper 代替，否则会错配材质。常规 GLB wrapper 需要逐 mesh 覆盖映射，并验证每个 mesh 都有 material slot。

## 边界

`MainScene.tscn` 中 38 个模块实例大多没有额外 Transform3D，布局位移已经位于 GLB 节点中。`PotPlants.glb` 例外：它在 MainScene 中有多个 PackedScene 实例，转换后应保留多个 scene objects 指向同一个 `SM_PotPlants.json`，并把 MainScene 的 Transform3D 分解成 scene JSON 的 position/rotation/scale。
