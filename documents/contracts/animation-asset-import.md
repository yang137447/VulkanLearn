# 动画 CPU 资产导入

> 状态：当前契约。描述 CPU 导入与采样；World 播放和 GPU 蒙皮见动画运行时契约。

## 所有权与入口

`vl_animation_assets` 随引擎构建，`main` 链接该库；CMake 最低版本为 3.24。
入口为 `AssimpSkeletalImporter::ReadMesh/ReadClip`，对应的 `BuildMesh/BuildClip`
供已解析 Assimp 场景和模块测试共用。结果拥有全部 CPU 数据，不引用 Assimp 场景内存。
`main` 的递归源码列表排除动画子目录，避免重复编译库源码。
静态 Assimp 导入路径保持独立。运行时消费见
[动画播放与蒙皮](animation-runtime-skinning.md)。

仅接受固定 `ue-fbx-cm-yup-v1` 策略：FBX metadata 必须为厘米、Z-up、
FrontAxis=-Y、CoordAxis=+X。缺失或不匹配直接拒绝，不根据文件名推测。
不启用 GlobalScale、LimitBoneWeights、pivot 保留或自动轴向校正。

## 空间与骨架

统一基底 `VL = (FBX.X, FBX.Z, -FBX.Y)`，单位厘米，Y 向上。
结合已核对的 UE 导出基底，等于 `VL = (UE.X, UE.Z, UE.Y)`。
位置乘基底；局部/模型矩阵与 inverse bind 共同采用基底共轭，
四元数与缩放按相同轴映射转换。源 Walk_F 的 UE +Y 对应模型 VL +Z。
这里没有施加 Actor 或组件朝向；既有第三人称示例输入前向 -Z 不是资产基底。

显式指定骨架根，并保留其全部子树，包括没有权重的父骨、IK/道具骨。
骨架名必须唯一，`VB ` 虚拟骨不作为物理骨进入此导入器。
节点/深度与 ozz 关节上限在导入边界检查；shear、反射、奇异与非有限变换拒绝。
包装静态变换折入网格骨架根；片段首版要求 identity 根包装，避免隐式烘焙错误。
ozz SkeletonBuilder 的输出顺序逐项与导入的深度优先索引核对。
片段必须具有相同物理骨名字和父关系。

## 蒙皮流与切线

`SkinSection` 保存独立 CSR 权重流（offsets + joint/weight），不扩大静态 `Vertex`。
保留所有正权重，包括九影响顶点；不截断、不排序丢失，也不归一化修复。
负值、非有限、重复骨/顶点权重、越界索引、无权重顶点和权重和误差大于
0.0001 的输入拒绝。

需要三角形、法线和 UV0。复用现有 `MikkTSpaceMeshGenerator` 及其源顶点映射，
在角点展开后同步复制完整权重；随后才翻转三角形绕序适配当前 Vulkan CW。
保留源颜色/UV0/可选 UV1，不复制 UV0 伪造 UV1，不额外翻 V。

按 section 校验 `restModel * inverseBind == meshToModel`，
平移容差 0.1 cm、线性矩阵容差 0.0001。
对展开后的每个顶点实际计算绑定姿态 CPU 蒙皮，最大位置残差超过 0.1 cm 拒绝。
无权重骨的 offset 不参与绑定残差判定；其层级仍保留。
这些是导入校验，不是逐帧 shader 防御。

## 片段采样

片段保留原始 TRS 与根轨道，再由固定 ozz AnimationBuilder 构建压缩轨道。
单位时间必须显式有效；时长大于 600 秒、非有限/无序/重复/越界 key、
未知/重复通道、非正缩放、非单位四元数、mesh/morph 通道和非默认外推行为拒绝。
非空分量要求从 0 开始，多 key 分量必须覆盖到片段末尾；不隐式改变区间外采样语义。
缺轨或缺少某个 TRS 分量时填入网格骨架的绑定分量，不使用空通道的 identity。
根位移不删除，不施加给角色或物理。

`SampleModels` 接受显式片段内时间，支持端点和倒序 seek，不拥有播放时钟。
身体使用 ozz 压缩采样与 LocalToModelJob。
根使用 ozz 原始 SampleTrack 覆盖首个 SoA lane 后再做层级求值：
SamplingJob 的 RcpEst 插值系数会放大长位移误差，根作为轨迹输入保留原始精度。
调用方必须传入语义匹配的导入骨架和片段；相同轨道数量本身不证明兼容。
此入口不拥有时钟；World 的 `SkeletalPlayback` 调用它进行片段内采样。

## 三层动画资产：加载顺序与校验

骨骼 SM 的 `skinning` 引用三层数据资产：

```json
"skinning": {
  "skeleton": "Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json",
  "clips": [
    { "id": "M_Neutral_Walk_Loop_F",
      "path": "Maps/SC_gasp/Source/M_Neutral_Walk_Loop_F.fbx",
      "sequence": "Maps/SC_gasp/Source/M_Neutral_Walk_Loop_F.anim.json" }
  ],
  "montages": [
    { "id": "AM_...",
      "path": "Maps/SC_gasp/Animations/AM_....json" }
  ]
}
```

需要把 GASP 动作复用到另一套角色骨架时，目标 SM 可以把动作源和显式重定向
profile 成对声明：

```json
"skinning": {
  "skeleton": "Maps/SC_cc5/Skeletons/SK_CC5.json",
  "clips": [],
  "sourceModel": "Maps/SC_gasp/Meshes/SM_gasp.json",
  "retarget": "Maps/SC_cc5/Retarget/RT_CC5_From_GASP.json"
}
```

这种资产中，目标 `skeleton`、网格 sections、权重、inverse bind、渲染骨架、IK
和 bounds 归目标 SM 所有；`sourceModel` 只提供 source skeleton 上的 clips 和
montages。`sourceModel` 必须是非 retarget 的骨骼 SM，不能再套一层
`sourceModel`；目标也不能同时声明自己的 `clips` 或 `montages`。`retarget` 必须是
`vl.retarget.v1`，使用显式 source/target joint mapping，不自动猜名，并且
`sourceSkeleton`/`targetSkeleton` 必须分别等于两份 SK 的 `id`。

```json
{
  "format": "vl.retarget.v1",
  "sourceSkeleton": "SK_UEFN_Mannequin",
  "targetSkeleton": "SK_CC5",
  "sourceRoot": "root",
  "targetRoot": "CC_Base_BoneRoot",
  "joints": [
    {"source": "root", "target": "CC_Base_BoneRoot"},
    {"source": "pelvis", "target": "CC_Base_Hip"}
  ]
}
```

示例骨名仅说明格式；每个角色必须按实际导入的 SK 名称填写，root pair 必须显式包含
在 `joints` 中，同一 source 或 target joint 不允许重复映射。

profile 可选 `sourcePose` / `targetPose`；省略任一项表示该侧直接使用 bind pose。
每侧只有一个具名校准姿态，名称是作者标签，不是可切换的姿态库。例如：

```json
"targetPose": {
  "name": "AlignedToGasp",
  "rotationOffsets": [
    {"joint": "Shoulder_L", "rotation": [0, 0, 0.1305261922, 0.9914448614]}
  ]
}
```

上例骨名及角度只是格式示例。`rotation` 为有限单位四元数 `[x,y,z,w]`，
模长误差容差 `0.0001`；校准局部旋转为 **`R_bindLocal * R_offset`**，
即在原 bind 骨轴中右乘偏移，再由父链传播到模型空间。每个偏移须引用本侧已存在的
非根 joint，同一侧 joint 不可重复；偏移可用于未映射的骨骼。对象要求非空 `name`
及数组 `rotationOffsets`，拒绝该对象/偏移条目内的未知字段、缺字段或错误类型。
索引 0 的 locomotion root 禁止作者旋转偏移，避免改变播放器根 TRS 和根运动的消费基底。
源/目标原始 bind、权重及 inverse bind 均保持不变，偏移只构建单独的参考姿态副本。
编辑器 JSON 形状提示由 `schema/retarget_profile.schema.json` 提供。
独立查看页的可选 `preview` 保存资源根相对 `sourceSkeletonPath` /
`targetSkeletonPath`，worker 检查两个 SK 身份与 profile 相同；正式运行时仍从 SM
加载自己的 SK/FBX，不由 preview 路径重定向。
可选 `preview.targetModelPath` 启用真实源/目标动画预览；GT 核对该目标 SM 使用当前
RT，再从其 `sourceModel` 取得源模型，复用正式导入/重定向/蒙皮。
标签页的测量语义见
[UI 组件体系](ui-component-system.md#retarget-参考姿态标签)。
骨名、重复、根约束和单位四元数仍由生产解析/加载边界核对。

运行时先在 source skeleton 上采样，再按
`R_target(t) = R_source(t) * inverse(R_sourceReference) * R_targetReference`
求 target 模型空间旋转并还原局部姿态；source 恰为校准参考时输出 target 校准参考。
引用模型空间参考旋转在播放器创建时编译，不逐帧重读 JSON 或重新求校准姿态。
目标骨长、目标局部平移/缩放保留，未映射目标 joint 保留其**校准后的局部旋转**
并继承父骨动作；未配置偏移时等于原局部 bind。
只把 source root 相对原始 bind 的局部平移差传给 target root，
不做身高比例缩放。两套
骨架都要求单根、root 为 joint 0 且 parent-first 拓扑。

`skinning.skeleton` 是骨骼 SM 的**必需字段**：根骨、关节表、槽位、曲线注册表都以这份 SK 为
唯一来源。旧 `skinning.root` 已删除且没有兼容分支——出现即拒绝
（`Mesh skinning.root is no longer supported; use skinning.skeleton`），即使同时给了
`skinning.skeleton`。`clips[].sequence` 与 `montages` 仍是可选：没有 sidecar 的 clip 可播放，
但没有曲线/序列 notify；没有 `montages` 的资产不含 Montage。

固定加载顺序（`MeshAssetLoader::ImportSections`，顺序本身就是契约）：

1. **规划**（`MeshAssetValidator::BuildLoadPlan`）：读必需的 `skinning.skeleton`、每个
   `clips[]` 的 `id`/`path`/可选 `sequence`、可选 `montages[]`。所有 JSON 路径必须是资源
   相对路径：非绝对、无根目录、无 `..`、扩展名 `.json`；clip id 与 montage id 在各自清单内
   唯一。规划期只拼接路径级缓存键，不读文件。
2. **Skeleton 先于任何 clip**：读取 SK（≤4 MiB）→ `ParseSkeleton` → 记文件摘要；SK 提供
   根骨、槽位、曲线注册表与 FBX 对照清单。
3. **FBX 网格**：`ReadMesh` 以 SK 的 `root` 为根；随后 `ValidateSkeletonAgainstMesh` 逐项
   对照关节数量、顺序、名称、父子与 `restLocal`（容差 `1e-5`）。FBX 仍是几何/权重/绑定 TRS
   的来源，SK 是权威校验清单，不反向驱动导入。
   若声明 `sourceModel`，此时先导入非 retarget 的源 SM，再读 profile（≤4 MiB）并校验
   两份 SK id、映射、root 和拓扑；profile 摘要及源 SM 的完整输入清单纳入目标身份。
4. **逐 clip 加载**：先读 FBX 片段，再读可选 sidecar（≤8 MiB）→ `ParseSequence`。sidecar 的
   `sequence` 字段必须等于 `clips[].id`；曲线名必须在 SK `curves` 注册、`tracks` 必须是 SK
   关节；sidecar `duration` 与 FBX `raw.duration` 差 ≤`1e-5`。摘要与路径进入缓存键与
   `sourceInputs`。侧车字段与采样语义以 `source/animation/animationSequenceData.h` 为准。
5. **全部 clip 完成后才加载 Montage**：逐项读取（≤4 MiB）→ `ParseMontage` → 校验
   `montage.id == skinning.montages[].id` → `BindMontage`（sidecar 路径命中、sidecar 的
   SK id/摘要、可选 `sequenceBlake3`、slot、Bone 骨名、派生 clip id）。Montage 只引用
   sidecar 路径，不复制曲线、不在导入期采样窗口。完整规则与播放入口见
   [动画 Montage：数据与播放](animation-montage.md)。
6. **整份清单复核**：导入结束前对 `sourceInputs` 的每一项重新哈希并比对；任一源文件在
   导入期间被替换（含 FBX、SK、sidecar、Montage）都拒绝发布结果。

`ParseSkeleton`/`ParseSequence`/`ParseMontage` 都是纯函数：只接收已解析 JSON 与 `basePath`，
不读文件、不算摘要；BLAKE3 与存在性检查只发生在 loader。

`sourceInputs` 是导入身份清单（`std::vector<std::pair<std::string,std::string>>`，
第一项为描述符身份，其后每项是“资源路径 → BLAKE3-256 hex”）：

| 顺序 | 条目 | 写入者 |
| --- | --- | --- |
| 0（可选） | SM 描述符路径 → `descriptorIdentity` | `ImportSections`（非空时） |
| 1 | `skinning.skeleton` → SK 文件摘要 | loader |
| 2 | `modelDataPath` → FBX 摘要 | loader |
| 3（retarget 分支） | `skinning.retarget` → retarget profile 摘要 | loader |
| 4（retarget 分支） | `skinning.sourceModel` 的完整 `sourceInputs` | loader |
| 3 起（普通分支） | 每个 `clips[].path` → 片段 FBX 摘要，其后紧跟可选 sidecar 摘要 | loader |
| 逐 clip 之后（普通分支） | 每个 `montages[].path` → Montage 摘要 | loader |

Motion Matching 数据库的 `CaptureMotionDatabaseInputs` 也把 `skinning.skeleton` 指向的 SK
文件摘要加入输入清单（在源/schema/model/FBX 之后、member FBX 之前）；改 SK 会改变
`vl.motion-database.v1` identity，旧缓存不会被错误命中，缺失时自动重建。

## 缓存键

`modelCacheKey` 由规划层与 loader 分两段拼接（各段以 `|` 连接）。规划层先写
sourceModel/profile 路径，再写本地 clip/Montage 路径；loader 先写 SK 摘要，
随后写 retarget 分支或普通分支的摘要：

| 段 | 内容 | 写入时机 |
| --- | --- | --- |
| 基段 | `modelDataPath` | 规划层 |
| `skeleton=<path>` | SK 资源路径（骨骼 SM 必需） | 规划层 |
| `clip=<id>:<path>` | 每个 clip 一项，按 `clips[]` 顺序 | 规划层 |
| `sequence=<id>:<path>` | 每个带 `sequence` 的 clip 一项 | 规划层 |
| `montage=<path>` | 每个 Montage 一项，按 `montages[]` 顺序 | 规划层 |
| `animationSourceModel=<path>` | retarget 目标的 sourceModel 路径 | 规划层 |
| `retarget=<path>` | retarget profile 路径 | 规划层 |
| `skeletonBlake3=<hex>` | SK 文件摘要 | loader |
| `sequenceBlake3=<id>:<hex>` | 每个 sidecar 一项 | loader |
| `montageBlake3=<id>:<hex>` | 每个 Montage 一项 | loader |
| `retargetBlake3=<hex>` | retarget profile 摘要 | loader |
| `animationInput=<path>:<hex>` | sourceModel 的每个 source input | loader |
| `generateSmoothNormals`、`section=<name>` | 静态导入选项（骨骼导入拒绝这些选项） | 规划层 |

旧 `|skinning=<root>` 段随 `skinning.root` 一起删除；无片段骨骼 SM 的最小规划键是
`<modelDataPath>|skeleton=<SK path>`，普通动画资产再按序追加 clip 项。

`MeshAnimationClipPlan::sequenceDigest`、`MeshAnimationMontagePlan::digest` 由 loader 回填，
随导入计划一起发布，供调用方核对同一代资产；渲染资源缓存以 `modelCacheKey` 为键。

## 固定 GASP 数据验收

`vl_animation_probe --import-gasp <P0-audit-directory> --output <new.json>`
只读固定的 mannequin 与 12 段片段，输出骨架、绑定/权重统计、5 个模型空间姿态、
侧车原根采样时刻对应的采样结果，以及 BLAKE3 输入身份。
输出路径必须在输入目录之外，且不可覆盖已有报告。
元数据保留 curves/notifies/syncMarkers/settings/rateScale 与原始 `.copy` 输入摘要；
曲线时间/值长度、排序与有限值，以及 notify 时间/时长在工具边界检查。
曲线端点外作者 key 原样保留。没有承诺曲线插值、事件执行、虚拟骨重建或 UE 蓝图等价。

`compare_gasp_import.py` 使用固定 UE→VL 基底独立对照 UE RAW 数据。
阈值为 0.1 cm、0.1 degree、0.00001 s，并检查元数据逐值保留和缺轨名单。
它输出数据报告和可选投影图；不是 Vulkan 角色渲染证据。
操作命令见 [工具入口](../../tool/animation/README.md)。

## 验证

根 GoogleTest 目标 `animation_preflight_tests` 中，`SkeletalImport`（30 项）覆盖
手算轴向、父骨保留、九影响权重、MikkTSpace 展开、绑定姿态、缺轨、根位移、
旋转/缩放和损坏资产拒绝；`AnimationSkeleton`（17 项）覆盖 SK 拓扑/槽位/曲线注册表、
FBX 对照、`skinning.root` 拒绝与强制 `skinning.skeleton`；`AnimationSequenceData`（14 项）
覆盖 sidecar 帧信息/曲线/notify 与采样；`AnimationMontage`（24 项）覆盖 Montage 解析、
跨文件绑定、缓存键与 `rotationMethod`；`AnimationScriptData`（30 项）覆盖 catalog/窗口
装配/target 旋转/Static pose/section link/入口 `startTime`；`MotionWarping`（28 项）覆盖
平移、旋转与 Static/Bone warp point；`NotifyPlayback`（11 项）覆盖 notify 执行。测试通过
根 helper 注册，没有新增 runtime test 标志。

`AnimationRetarget` 覆盖显式映射/骨架拓扑拒绝、不同 rest axes/骨长、未映射骨、
根平移差、source 采样与 target palette、in-place/混合、source Montage/catalog、
不完整 source/profile 与嵌套 source 拒绝、descriptor 配对/cache key，
以及 retarget Motion Matching 在磁盘访问前拒绝。

资源侧三层资产已在 `Maps/SC_gasp/` 提交：SK 35457 B（91 物理骨 + 2 虚拟骨、13 条曲线
注册、`DefaultSlot`）；**31 个** `Source/*.anim.json` 共 829,022 B，**19 个**
`Animations/AM_*.json` 共 36,839 B（10 个 traversal + 9 个 Catch）。全量 notify 合计
**188** 条（142 traversal + 46 Catch），warp 窗口合计 **33** 个（21 + 12）。
SM 为 **31 clip / 19 montage**（资源 `3574a3a`）；接线见资源 `aa78096`（`skeleton` +
sequence + `montages`）；资源 `ac0fe0c` 删除残留的 `skinning.root`；资源 `b627bd0`
让 GASP `FrontLedge` 提交目标旋转（`BackLedge`/`BackFloor` 仍只提交位置）。
Catch 9 个 clip 的时长为 3.333–5.000 s、5,708 个曲线键、46 条 notify、12 个 warp；
逐 clip 的 duration/frames/curves/keys/notify 见
`artifacts/gasp-catch-export/validation.json`（`issues=0`，FBX 与 sidecar 的 duration
最大差 1.59e-7 s）。

Catch 资产复用同一导出链，新增可选参数（`6963a9d`）：
`export_gasp_ue_raw.py` 的 `VL_GASP_ANIM_CLIP_IDS` 只导出指定 clip；
`export_gasp_sequences.py` 的 `--clips` 限制批量、`--continue-on-error` 让单 clip 失败
不终止整批；`import_gasp_montages.py` 同样支持 `--clips`。`audit_gasp_catch.py` 只读枚举
9 个 Catch montage、导出 T3D 与引用序列 FBX（环境变量与命令见
[工具入口](../../tool/animation/README.md)）。全量重导必须先用当前 31 clip 的 SM 生成
raw dump（不设 `VL_GASP_ANIM_CLIP_IDS`），否则会漏掉新增 clip。
Montage 的 schema、绑定与播放见 [动画 Montage：数据与播放](animation-montage.md)。

固定 GASP 正式入口为资源仓库 `Maps/SC_gasp/SC_gasp.json`，
原始导出与审计证据仍保留在隔离目录。正式目录与限制见动画运行时契约。
