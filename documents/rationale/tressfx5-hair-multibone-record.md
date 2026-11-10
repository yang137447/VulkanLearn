# TressFX .tfxbone 多骨数据路径记录

状态：2026-10-24 的解析、映射与 GPU 接入记录，**不是实现契约**。当前只有
声明 `simulation.skinning.boneWeights` 的资产才走多骨；正式 Female 资产没有
`.tfxbone`，仍走单根骨。本文记录源格式、VL 数据路径、GPU 资源与验证证据。

## 1. 源行为

固定 revision `61f8dd9b51b88d41937ef8d73618aac778642f41`。源
`Engine/Plugins/AMD/TressFX/Source/TressFXCore/Private/TressFXAsset.cpp:417-501`
的 `FTressFXAsset::LoadBoneData(USkeletalMesh&, FMemoryReader)` 读取
`.tfxbone`（小端）：

```text
int32 numBones
repeat numBones:
    int32 index            // 未使用
    int32 charLen          // 包含 null 终止
    char[charLen] name
int32 numStrandsInStream
repeat numStrandsInStream:
    int32 index            // 未使用
    int32 count
    repeat count:
        int32 boneIndex    // 指向 boneNames
        float weight
```

源行为要点：

- 若 `numGuideStrands < numStrandsInStream` 直接失败；更少的 strand 允许，
  剩余 `boneIndexData` 为零（运行期回退）。
- 每个影响按 `boneNames[boneIndex]` 找目标骨架 joint；找不到时置 `-1`。
- `boneIndex == -1` 或 `weight == 0` 的项 `continue`，不进入
  `boneSkinningData`；其余项累加 `BoneMatrix += Matrix[bone] * weight`，最后
  shader 里 `BoneMatrix /= WeightSum`。
- 源 shader 的 `ApplyVertexBoneSkinning` 用这些权重对 guide 顶点做
  `InitialPos = BoneMatrix * RestPos`（再 `*0.01`）。

## 2. VL 数据路径

新增 `source/hair/tfxBoneLoader.{h,cpp}`，纯 CPU、不依赖 Vulkan/World：

- `ParseTfxBone(bytes, path)` / `LoadTfxBone(path)`：解析上述字节流，校验
  骨数、名字长度、strand 数、影响数、boneIndex 范围、权重有限且非负；
  错误使用 `RuntimeError`（`Hair.TfxBoneTruncated` / `...Index` / `...Weight`
  等），带 path 与字段。
- `MapTfxBoneToSkeleton(asset, jointNames, guideCount, unmappedBones, path)`：
  - 骨名映射到目标 joint 下标；未知骨按源行为跳过并记录到 `unmappedBones`。
  - `boneIndex < 0` 或 `weight == 0` 跳过。
  - 同一 joint 的重复影响累加，权重归一化到 1；输出 `joints` 按 joint 排序，
    保证确定性。
  - `strands.size() > guideCount` 硬错误；缺失 strand 返回空权重，调用方回退
    根骨/rest。
- 资产字段 `simulation.skinning.boneWeights`（资源仓库相对路径）由
  `TressfxModelImporter` 解析成 `TfxBoneAsset`，随 `MeshSection`/`MeshObjectBuildPlan`
  传递；`rendererResourceLoadCoordinator` 在加载期用源角色 jointNames 映射，
  结果放入 `TressfxSkinningBinding::strandBones`。路径参与 mesh cache identity。
- 该库已加入 `vl_hair_assets`；模块测试 `TfxBoneLoader.*` 7 项覆盖成功解析、
  截断、越界/NaN、strand 数超出、骨名映射/归一化、`-1`/零权重跳过、未知骨
  记录与空权重回退；另有 2 项描述符测试（`boneWeights` 字段/身份）与 1 项
  加权 `InitialPos` 数学测试。

## 3. GPU 接入

- `RenderableObject` 为每个 GPU 发丝对象创建：
  - `TressFXBoneWeights`：每 strand 4 个 `(joint, weight)`，32 bytes/strand；
  - `TressFXSkinningPalette`：每 frame-in-flight 槽 256 个 `mat4` 的全骨架
    bind→pose palette。
- `SetTressfxSkinningBinding` 把 `strandBones` 打包成 GPU 格式：最多取权重
  最大的 4 个影响并重新归一化；空权重 strand 用 `rootBone` 的 joint/weight=1
  回退，避免加权矩阵变成零矩阵。
- `HairSimulationPass` 新增 binding 14（bone weights）与 15（palette），只加在
  Integration/StrandLevel 的 descriptor set；`params.motion.w` bit2 表示启用。
  每帧写 palette（pose 可用时），Integration 用加权矩阵算 `InitialPos`，
  StrandLevel 用 `TressfxQuatFromMatrix` 重算 `SkinningQuat`；LocalShape 继续
  消费 strand-level quat。
- 没有声明 `.tfxbone`、pose 不可用或权重为空时 bit2=0，完全走原单根骨路径。

## 4. 验证

- 正式场景（无 `boneWeights`）：Release 截图，dispatch `objects=1 guides=40334
  points=1290688 sdfObjects=1`，stderr 0；Debug startup shader `compiled=28,
  failed=0`，无 VUID。
- 临时多骨场景（两个临时资产，采集后已删除）：
  - 75% Head_M + 25% Neck1_M：`[TressFX] boneWeights: strands=40334 joints=105`，
    dispatch 正常，Debug 无 VUID、stderr 0，截图与单骨外观一致。
  - 100% Chest_M：截图显示发丝根点明显移到胸/肩，证明 GPU 加权 `InitialPos`
    真正生效，而不是只读了绑定数据。
  - 60 帧 trace（79 guides × 32 points）：finite 151680、bad 0；segment
    min/mean/max = 0.0493/1.2907/5.785 cm。根/发梢步长最大 16.78/29.41 cm；
    这是故意把全部权重压到 Chest_M 的人造输入，只作为路径烟测，不能当正式
    物理质量指标。
- `hair_asset_tests`：254 tests / 25 suites 全通过。
- 文档校验：`py -3 tool\validate-docs.py`。未运行 UE；正式资产未改。

## 5. 边界

- 源导出器按“每个顶点最多四个骨影响”设计；VL 也只上传 4 个，超过时取权重
  最大的四个并重新归一化。
- 本地没有真实 `.tfxbone` 资产；正式 Female 仍单根骨，真实资产验收后置。
- CPU backend 仍不支持 skinning（描述符校验要求 `backend=gpu`）；多骨只存在于
  GPU 路径。
- `.tfxmesh`/follow 仍未实现；MT/blend-shape 根三角变体不在当前资产面内。
