# Vegetation Instancing

> 状态：**当前契约**。本文件描述 `VEG_*.json`、`mesh.instancesPath`、实例顶点流和
> 实时 instanced draw 的现有行为。风、逐实例 LOD、逐实例剔除、实例烘焙和实例物理
> 不在当前实现内。

## 资产与场景入口

`VEG_*.json` 是资源根相对的静态实例清单：

```json
{
  "version": 1,
  "mesh": "Generated/BlenderPegasusGN/Vegetation/SM_pegasus_cypress.json",
  "instances": [
    {
      "position": [0, 0, 0],
      "rotation": [0, 0, 0, 1],
      "scale": [100, 100, 100],
      "windPhase": 0.5,
      "lodBias": 0.0,
      "variant": 0,
      "seed": 0
    }
  ]
}
```

- `mesh` 必填且非空；它必须与场景 mesh 对象的 `modelPath` 指向同一资源。
- `instances` 必须有 1..100000 项。`position` 必填；`rotation` 是 `xyzw` 四元数，
  有限且非零，加载时归一化；`scale` 接受单数字或三分量，任一分量接近 0 都失败；
  数值必须有限。
- `windPhase`、`lodBias` 是有限浮点数；`variant`、`seed` 是 `uint32`。
- 场景 mesh 对象通过可选 `instancesPath` 引用 VEG。预检阶段读取并校验 VEG，
  渲染加载阶段直接消费已解析的实例数组。

## 实例顶点 ABI

普通网格仍使用 binding 0 的 `Vertex`。binding 1 使用 `eInstance` 步进率，
每实例 80 字节：

- location 8..11：实例模型矩阵的四列，各为 `vec4`，偏移 0/16/32/48。
- location 12：`vec4 params`，偏移 64；`x=windPhase`、`y=lodBias`、
  `z=variant`、`w=seed`。

顶点着色器显式按四个 `vec4` 接收矩阵列，再构造 `mat4`。实例矩阵按列主序写入，
最终模型矩阵是场景对象 `uboM.model * instanceModel`；上一帧裁剪位置和世界位置
使用同一实例矩阵。

`params` 目前只透传到材质顶点上下文，没有运行时风、颜色或动画求值器读取它。
后续风系统可以从 `windPhase` / `seed` 扩展，不需要改变顶点 ABI。

## 渲染行为

`RendererMeshLoader` 为网格的每个 section 生成一个 `MeshObjectBuildPlan`；
同一 section 的 `DrawIndexed(..., instanceCount)` 一次提交全部 VEG 实例。
实例化路径与普通路径使用不同的 renderable 缓存键，键包含归一化后的
`instancesPath`，所以同一 SM 可以同时作为普通 mesh 和批量实例使用。

实例包围盒在加载期合并：每个实例矩阵作用到 section 局部 AABB，再取并集。
World snapshot、视锥剔除和阴影 caster 预筛消费合并后的对象局部包围盒；
当前没有逐实例视锥剔除。

Pegasus cypress 切片实测：122 棵树、叶/干两个 section，加上 16 个 terrain chunk，
`terrainchunk stats` 报告 `meshDraws=18`。

## 当前限制

- 实例清单是只读加载资产，没有运行时增删改、GPU 回读或实例流压缩。
- 没有每实例 LOD、每实例材质、每实例可见性、GPU culling 或空间分区。
- 没有实例动画/骨骼；VEG 只适用于静态 mesh section。
- 离线渲染器不展开 VEG 实例流；离线加载带 `instancesPath` 的 mesh 会显式失败，
  避免静默只画一个实例。
- 实例不创建碰撞体；地形碰撞仍由 `terrainCollision` 单独提供。

## 验证

```powershell
cmake --build build --target terrain_tests -j 8
.\build\bin\terrain_tests.exe
py -3 tool\validate_scene_schema.py
```

`terrain_tests` 的 `VegetationAssetTest.*` 覆盖 VEG 解析、四元数归一化、实例参数打包、
变换后包围盒以及缺字段/零缩放/非法参数拒绝。Pegasus grove 的 mesh 对象按
`schema/scene.schema.json` 的 `$defs.meshObject` 直接校验为 0 问题；资源 `Maps/**`
全量扫描仍受 5 个既有 `wind` 对象 schema 缺口影响。
