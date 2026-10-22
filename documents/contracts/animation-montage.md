# 动画 Montage：数据与播放

> 状态：当前契约。描述 `AM_*.json`（`vl.anim-montage.v1`）的解析、跨文件绑定校验、
> 与 Skeleton/Sequence/SM 的引用关系、导入计划/`modelCacheKey`/`sourceInputs`，
> 以及 `PlayMontage` 的播放范围、入口 `startTime`、section/link 推进、notify 执行、
> warp（含旋转/Static）、catalog/feedback 与执行层拒绝矩阵。
> **非 `Slerp` 旋转方法、Montage 混合、section 内 rate/offset、additive 与 sync marker
> 执行不属于本契约**（数据保留，执行入口拒绝或不消费）。

## 所有权与入口

| 职责 | 所有者 | 可核对位置 |
| --- | --- | --- |
| 解析 `AM_*.json`；不读文件、不算摘要、不接触骨架/Sequence | `VL::Animation::ParseMontage` | `source/animation/animationMontage.h`、`animationMontage.cpp` |
| section link 时间换算（Absolute/Relative/Ratio） | `ResolveMontageSectionLinkTime` | `source/animation/animationMontage.cpp` |
| 跨文件绑定（sidecar/SK/clip id）并派生 clip id | `VL::Animation::BindMontage` | `source/animation/animationMontage.cpp` |
| 校验 `skinning.montages` 清单、扩展 `modelCacheKey` | `MeshAssetValidator::BuildLoadPlan` | `source/mesh/validation/meshAssetValidator.cpp` |
| 读取/哈希 Montage、调用 `ParseMontage`/`BindMontage`、填 `sourceInputs` | `MeshAssetLoader::ImportSections` | `source/mesh/loader/common/meshAssetLoader.cpp` |
| 持有导入后的 Montage 数据 | `SkeletalAsset::montages` | `source/animation/skeletalPlayback.h` |
| 按 id 或资源相对路径查找（不改变所有权） | `SkeletalAsset::FindMontage` | `source/animation/skeletalPlayback.cpp` |
| 执行校验、section/warp 装配、catalog 资产段与 notify feedback | `animationScriptData.*` | `source/animation/animationScriptData.h`、`animationScriptData.cpp` |
| section 链推进、播放范围切换、notify 扫描 | `SkeletalPlayback::TickMontageSections` / `SetMontageSections` | `source/animation/skeletalPlayback.cpp` |
| 按 modelPath 发布 catalog/feedback、接收 `playMontage` 请求 | `World::ScriptAnimationAssets` / `ApplyScriptAnimationRequests` | `source/world/worldAnimation.cpp` |
| C# 提交面：`PlayMontage`（位置 / `WarpTarget` 两个重载）、`Montage`/`CurveValue`、`State.Notifies`/`State.Montage` | `AnimationPlayback` | `script/VulkanLearn.Runtime/AnimationPlayback.cs` |

`ParseMontage(json, basePath)` 的 `basePath` 只用于诊断和资源路径展示；函数本身不读文件、
不计算 BLAKE3、不引用 Skeleton 或 Sequence。文件摘要由 loader 读取字节后写入
`Montage::sourceDigest`，资源相对路径写入 `Montage::sourcePath`；JSON 自身不携带这两个字段。
Montage 不复制骨骼表、不复制曲线值、不持有播放时钟或 notify 回调。

文件大小上限 4 MiB（`MaxMontageJsonBytes`）；结构上限为 `sections` ≤64、`notifies` ≤64、
warp 窗口 ≤8、名称 ≤64 字符、Montage id ≤256 字符、`length` ≤600 秒。上限只防止异常资产
拖垮导入，不参与播放语义。

## JSON schema

顶层字段：

| 字段 | 类型 | 必需 | 语义与校验 |
| --- | --- | --- | --- |
| `format` | string | 是 | 必须逐字为 `vl.anim-montage.v1` |
| `montage` | string | 是 | Montage id；非空、≤256；同一 SM 的 `skinning.montages[].id` 必须逐字相等 |
| `sequence` | string | 是 | **Sequence sidecar 的资源相对路径**；非空。clip id 不写在这里，由命中的 sidecar 派生 |
| `sequenceBlake3` | string | 否 | sidecar 文件 BLAKE3-256（64 hex）；给了就在绑定期与 sidecar 实际摘要比对 |
| `slot` | string | 是 | 非空、≤64；必须已在 SK `slots` 注册（GASP 为 `DefaultSlot`） |
| `length` | number | 是 | 秒、Montage-local；有限、>0、≤600 |
| `sections` | array | 是 | ≥1、≤64；元素见下 |
| `notifies` | array | 否 | ≤64；缺省为空。顺序即资产顺序，不按时间排序 |
| `source` | object | 否 | 追溯信息（T3D 文件名、UE 包路径、导出脚本）；`dump()` 后原样保留，不参与运行时身份 |

`sections[]`：

| 字段 | 类型 | 必需 | 语义与校验 |
| --- | --- | --- | --- |
| `name` | string | 是 | 非空、≤64；同一 Montage 内唯一 |
| `startTime` | number | 是 | 秒；有限、≥0 |
| `endTime` | number | 是 | 秒；有限、>`startTime`、≤`length + 1e-3` |
| `nextSection` | string | 否 | 空串表示不连接；非空必须命中同一 Montage 的某个 section（允许前向引用） |
| `nextTime` | number | 否 | 缺省 0；有限、在 `[0, length + 1e-3]` |
| `linkMethod` | string | 否 | 空串表示未设置；否则必须是 `Absolute`/`Relative`/`Ratio` |

`notifies[]`：

| 字段 | 类型 | 必需 | 语义与校验 |
| --- | --- | --- | --- |
| `kind` | string | 是 | `notify` 或 `notifyState` |
| `track` | string | 是 | 非空、≤64 |
| `name` | string | 是 | 非空、≤64 |
| `time` | number | 是 | 秒、Montage-local；有限；`[0, length]`（容差见下） |
| `duration` | number | 是 | 秒；有限、≥0；**普通 notify/notifyState 的区间不裁剪**（见「时间语义」） |
| `warp` | object | 否 | 只有 `MotionWarping` notifyState 才有；字段见下 |

`notifies[].warp`：

| 字段 | 类型 | 必需 | 语义与校验 |
| --- | --- | --- | --- |
| `target` | string | 是 | C# 动态 target 的键；非空、≤64；**不要求唯一**（同名窗口共享一个世界位置） |
| `provider` | string | 是 | UE `EWarpPointAnimProvider`：`None`/`Static`/`Bone`；未知枚举拒绝 |
| `bone` | string | provider=Bone 时是 | 非空、≤64；绑定期必须在 SK `joints` 中存在 |
| `subtractRemainingRootMotion` | bool | 是 | 原样保留；运行时对应 `MotionWarpWindow` 同名字段 |
| `ignoreZAxis` | bool | 是 | 原样保留；运行时对应 `MotionWarpWindow` 的忽略竖直分量；不影响旋转 warp |
| `warpRotation` | bool | 是 | 是否消费 target 提交的目标朝向；`false` 时保持原始根 yaw |
| `rotationTimeMultiplier` | number | 是 | 有限、>0；旋转时间按窗口剩余时长缩放 |
| `rotationMethod` | string | 否 | UE `EMotionWarpRotationMethod` 原名字符串；缺省 `Slerp`。数据层只校验非空/≤64；执行层只支持 `Slerp`，其它值在 `PlayMontage` 拒绝 |
| `easing` | string | 是 | 已知 UE `EEasingFunc`：`Linear`、`Step`、`SinusoidalIn/Out/InOut`、`EaseIn/Out/InOut`、`ExpoIn/Out/InOut`、`CircularIn/Out/InOut`；未知枚举拒绝 |
| `staticPose` | object | provider=Static 时是 | `location[3]` + `rotation[4]`（UE 四元数 `x,y,z,w`），全为有限数、旋转非退化；执行层按 `offset = Root(EndTime) * staticPose^-1` 求静态 warp point 偏移，只消费平移 |

## ParseMontage 校验矩阵

每条失败都抛出 `Animation montage asset '<basePath>': field "<字段>" <原因>`，字段路径带数组索引
（如 `notifies[2].warp.bone`）。

| 输入/字段 | 判定 |
| --- | --- |
| 顶层不是 JSON object | 拒绝（消息不带字段名） |
| `format` 缺失/不是字符串/不等于 `vl.anim-montage.v1` | 拒绝 |
| `montage` 缺失、非字符串、空、>256 | 拒绝 |
| `sequence` 缺失、非字符串、空 | 拒绝（解析器只查非空；能否命中已加载 sidecar 由 `BindMontage` 检查） |
| `sequenceBlake3` 存在但不是 64 hex 字符串 | 拒绝 |
| `slot` 缺失、非字符串、空、>64 | 拒绝 |
| `length` 缺失、非数、非有限、≤0、>600 | 拒绝 |
| `sections` 缺失、非数组、空、>64 | 拒绝 |
| section 非 object；`name` 空/重复/超长；`startTime < 0`；`endTime <= startTime`；`endTime > length+1e-3` | 拒绝 |
| `nextSection` 超长或指向不存在的 section | 拒绝（先收集全部名字，再校验引用） |
| `nextTime` 缺失时取 0；存在但非有限或不在 `[0,length+1e-3]` | 拒绝 |
| `linkMethod` 非空且不在 `Absolute`/`Relative`/`Ratio` | 拒绝 |
| `notifies` 存在但非数组或 >64 | 拒绝 |
| `kind` 不是 `notify`/`notifyState`；`track`/`name` 空或超长 | 拒绝 |
| notify `time < 0` 或超过上界 | 拒绝（上界见「时间语义」） |
| notify `duration < 0` | 拒绝 |
| 带 `warp` 的 notify：`duration <= 0` | 拒绝（warp 窗口必须有正长度） |
| 带 `warp` 的 notify：`time + duration > length + 1e-3` | 拒绝 |
| warp 窗口累计 >8 | 拒绝 |
| `warp.target` 空/超长；`provider` 未知；Bone 缺骨名或骨名超长 | 拒绝 |
| `rotationTimeMultiplier` 非有限或 ≤0；`easing` 未知 | 拒绝 |
| `rotationMethod` 存在但非字符串或 >64 字符 | 拒绝（空串按缺省 `Slerp`；未知枚举不在解析期拒绝，由执行层拒绝） |
| `provider=Static` 但缺 `staticPose`；`staticPose` 非 object、数组长度/元素非法或非有限 | 拒绝 |
| `source` 存在但不是 object | 拒绝（其余情况原样保留） |

**数据层允许保留、执行层按能力拒绝的字段**：多 section/`nextSection`/`nextTime`/`linkMethod`
（现在执行）、`provider=Static`（现在执行）、`warpRotation`/`rotationTimeMultiplier`
（现在执行）、`rotationMethod` 非 `Slerp`（执行拒绝）、非 `Linear` easing（执行拒绝）。

## BindMontage 跨文件校验

`BindMontage` 在**所有 clip 与 sidecar 加载完成之后**执行，输入是 Montage、当前已加载的
`SkeletonAsset`、本次 SM 导入的 sidecar 绑定列表（path → `SequenceData*` + 摘要）以及
`skinning.clips[].id` 清单。校验顺序固定：

1. 必须有已加载的 Skeleton（`skeleton.id` 非空）；否则报 `skeleton` 字段错误。
2. `montage.sequence` 必须**按字符串逐字命中**本次导入的某个 sidecar 路径；不按 clip id 猜测，
   也不做路径归一化。未命中报 `sequence` 路径错误。
3. sidecar 的 `skeleton` 必须等于已加载 SK 的 `skeleton` id。
4. sidecar 的 `skeletonBlake3` 与 SK 文件摘要都存在时，必须大小写不敏感相等。
5. `sequenceBlake3` 若声明，必须等于 sidecar 文件摘要；sidecar 没有可用摘要则拒绝。
6. `slot` 必须命中 SK `slots`。
7. 每条 `provider=Bone` 的 warp，骨名必须是 SK 关节（大小写敏感匹配，`SkeletonAsset::JointIndex`）。
8. sidecar 的 `sequence` 字段（派生的 clip id）必须已在本次 SM 的 `skinning.clips[].id` 中声明。

成功后返回派生 clip id，loader 写入 `Montage::derivedClipId`；`BindMontage` 本身不采样曲线、
不预计算窗口曲线值、不生成任何运行时窗口。

loader 另外校验 `montage.id == skinning.montages[].id`；不一致时报“id 与清单 id 不符”。
`Montage::windows` 由带 `warp` 的 notify 按数组顺序派生（`target` + `[time, time+duration]`），
不排序、不去重；同名 target 可以出现多次。

## 与 Skeleton / Sequence / SM 的引用关系

```text
SkeletalAsset
  skeleton  <- SM skinning.skeleton（Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json）
  clips[]   <- FBX + 可选 sidecar（<stem>.anim.json）
  montages[]<- SM skinning.montages[].path（AM_*.json）
                 └─ montage.sequence -> 必须命中 clips[] 已加载的某个 sidecar
```

| 层 | 提供什么 | Montage 如何使用 |
| --- | --- | --- |
| Skeleton `SK_*.json` | skeleton id、文件摘要、`joints`、`slots` | `slot` ∈ `slots`；Bone 骨名 ∈ `joints`；sidecar 的 skeleton id/摘要必须与它一致 |
| Sequence sidecar `<stem>.anim.json` | clip id（`sequence` 字段）、曲线、序列级 notify、文件摘要 | 只用路径定位、用摘要校验、用 `sequence` 字段派生 clip id；**曲线值不复制进 Montage** |
| SM `skinning.montages[]` | Montage 清单 `{id, path}` 与顺序 | 决定加载哪些 Montage、缓存键顺序；`id` 必须与 JSON 的 `montage` 相等 |

Montage 只是数据层：`SkeletalAsset::montages` 按值持有这些结构，`FindMontage(idOrPath)`
支持按 id 或 `sourcePath` 查找。retarget 目标通过 `AnimationAsset()` 从 sourceModel
查找 Montage/derived clip、构建 catalog 和执行 notify；Montage 的 SK/sidecar 绑定仍属于
source asset，不按目标骨名重新绑定。目标只接收 retarget 后的姿态；字段与拒绝规则见
[动画资产导入](animation-asset-import.md)。

资源侧已提交 **19 个** `Maps/SC_gasp/Animations/AM_*.json`
共 36,839 B：10 个 traversal（资源 `ebd7059`，46 条 notify、21 个 warp）+
9 个 Catch（资源 `3574a3a`，46 条 notify、12 个 warp），合计 188 条 notify、33 个窗口；
traversal 那批复制到 `tool/animation-tests/fixtures/montages/` 作为模块测试夹具。
`skeleton` 现在是骨骼 SM 的必需字段；`clips[].sequence` 与 `montages` 仍是可选：
`skinning.montages` 未写时导入计划里 `animationMontages` 为空，资产不含 Montage；
没有 sidecar 的 clip 仍可播放，但没有曲线、序列 notify 或 Montage 覆盖层。

## 时间语义：普通 notifyState 可越界，warp 窗口必须在 length 内

- 单位一律秒、Montage-local，与 Sequence/播放器/窗口同一时间轴；没有帧号字段。
- **普通 `notify`/`notifyState` 的 `duration` 独立于 Montage 长度**：它表示任意子区间，
  可以越过 `length`。导入保留作者原值，不裁剪、不 clamp；`time` 起点上界为
  `length + 1e-5`（给浮点序列化的末帧误差留余量）。这与 Sequence sidecar 的
  `SequenceNotify::EndTime()` 不 clamp 是同一条 UE 语义。
- **带 `warp` 的 notify 是播放窗口，不是普通 notifyState**：必须 `duration > 0`，且
  `time + duration ≤ length + 1e-3`（此处 `time` 上界同样用 `1e-3` 容差）。
  原因在 `MotionWarpRuntime`：窗口 `EndTime` 必须是片段内可采样的时刻。
  这条限制只作用于 warp 窗口，不影响普通 notifyState。
- section 的 `endTime` 与 `nextTime` 用同一个 `length + 1e-3` 容差；时间容差常量
  `TimeTolerance = 1e-3`、普通 notify 起点容差 `NotifyStartTolerance = 1e-5`。

## 导入计划与缓存键

`skinning.montages` 是可选数组，元素为 `{id, path}`：

| 校验 | 位置 |
| --- | --- |
| 必须是数组（不是则报 `skinning.montages must be an array`） | 规划层 |
| `skinning.skeleton` 是骨骼 SM 的必需字段（不是只在有 montages 时）；缺失即拒绝 | 规划层 |
| 元素必须是 object，`id`/`path` 均为非空字符串 | 规划层 |
| `id` 在同一 SM 内唯一 | 规划层 |
| `path` 必须是资源相对 `.json` 路径：非绝对、无根目录、无 `..`、扩展名 `.json` | `ReadOptionalAnimationJsonPath` |
| 加载前文件必须存在（`RequireImportFile`） | loader |

缓存键与身份：

```text
modelCacheKey（规划层逐项追加，保持清单顺序）
  |skeleton=<path>                            每个骨骼 SM 一条（不再有 |skinning=）
  |clip=<id>:<path>                           每个 clip 一项
  |sequence=<id>:<path>                       每个带 sidecar 的 clip 一项
  |montage=<path>                             每个 skinning.montages[] 一项
modelCacheKey（loader 在读取并哈希后追加）
  |skeletonBlake3=<64 hex>                    SK 文件摘要
  |sequenceBlake3=<id>:<64 hex>               每个 sidecar 一项
  |montageBlake3=<id>:<64 hex>                每个 Montage 一项

sourceInputs（loader 追加，按加载顺序）
  skeleton 路径 -> SK 文件 BLAKE3-256 hex
  modelDataPath -> FBX 摘要
  clip 路径 / sidecar 路径 / montage 路径 -> 各自文件摘要
```

导入结束前，loader 对 `sourceInputs` 的每一项重新哈希并比对；任何源文件在导入期间被替换都会
使整次导入失败（`Skeletal source changed during import`）。骨骼 SM 的最小缓存键现在是
`<modelDataPath>|skeleton=<SK path>|clip=<id>:<path>`；旧 `|skinning=<root>` 段随
`skinning.root` 删除一并消失。

## 播放入口与提交

原生播放入口是
`VL::Animation::PlayMontage(player, montage, targets, loop, rate, inPlace, startTime = 0)`
（`source/animation/animationScriptData.cpp`）；脚本侧请求是
`{"component", "mode":"playMontage", "montage", "targets", "loop"?, "rate"?, "inPlace"?, "startTime"?}`
（`source/world/worldAnimation.cpp` 的 `ApplyScriptAnimationRequests`）。

- `targets` 是“target 名 → 3 或 7 个有限数”的对象（`[x,y,z]` 或 `[x,y,z,qx,qy,qz,qw]`）；
  解析上限 32 项，键非空且 ≤64 字符，非单位四元数归一化。
  **键集合必须与 Montage 声明的 distinct warp target 名完全相等**：缺一个或多一个都拒绝，
  不静默使用默认位置。
- `startTime`（秒、clip-local，缺省 0）是入口时间：`0` 表示从 section 0 起点播放（旧调用
  不变）；正值必须落在 section 0 的可播放范围
  `[firstSection.startTime, min(firstSection.endTime, clipDuration) - 1e-3]` 内，
  否则带 montage id/section/范围拒绝；非有限或负数拒绝。
- **UE 对应语义**：`startTime` 对应 `UAnimNotifyState_PoseSearchBranchIn` 写进 PoseSearch
  DB 的**采样范围下界** `[Time, Time+Duration]`（`PoseSearchDatabase.cpp:1302-1315`）。
  该 notify 本身不选帧；真正选帧的是 Chooser 的 Pose Match 列
  （`OutputStartTime = BestSearchResult.GetAssetTime()`）。VL 取窗口下界，因此与 UE
  原生入口的差 **≤ 窗口长度**：9 个 Catch 窗口 33.3–34.9 ms（≈1 帧），差 ≤1 帧；
  非 Catch 家族窗口 `[0, 0.07–1.03]`（2–31 帧），VL 恒取 0（同样等于下界）。
- **只对 Montage notify 生效**：入口计算只读当前 `Montage(id).Notifies` 的第一个
  `PoseSearchBranchIn`；`Sequence`/clip 上同名 notify 不参与（locomotion 的
  Run/Sprint/Crouch/Jump_Land 也带该 notify，通用“扫 clip”会读错时间）。
  `MontageBranchInWindowsAreSamplingRanges` 锁定这条规则：9 个 Catch montage 窗口
  ≤1.2/30 s，10 个非 Catch montage `time==0`。
- 入口时间按**一次初始 seek** 执行：entryTime 之前的 notify 不补发；根位移/根 yaw 只从
  entryTime 起积分；warp 窗口用 `previousPosition=entryTime` 门控，**结束时间早于
  entryTime 的窗口不再激活**，包含 entryTime 的窗口从 entryTime 算 total。多 section
  链只截短第一段，后续段仍按 `nextSection` 推进。
- 提交顺序固定为 `Play(derivedClipId, false, rate, inPlace)` →（多 section 时）
  `SetMontageSections(sections)` 或（单 section）`SetPlaybackRange(first.start, first.end)` →
  `SetMontageNotifies(notifies, id, length)` → `Seek(entryTime)` →
  `SetMotionWarpWindows(按资产 notify 顺序装配的窗口)`。`Play`/`Select` 会清空旧播放范围、
  section 链与旧 Montage notify 覆盖层，所以这些必须在 `Play` 之后设置。
- 全部校验与候选构造先完成；失败时当前播放器保持不变（`PlayMontage` 在
  `SkeletalPlayback` 副本上操作，成功后才提交）。世界层同时关闭该组件的 foot placement、
  inertialization 与 root offset，并解除 Motion Matching 运行时绑定。
- Montage 播放的是 sidecar 派生的 clip：`derivedClipId` 必须已在
  `SkeletalAsset::clips` 中加载，且该 clip 必须有 sidecar（`clip.sequence.id == derivedClipId`）。
- C# 面有两个 `PlayMontage` 重载：旧 `Dictionary<string, Vector3>`（只提交位置）与
  `Dictionary<string, WarpTarget>`（位置 + 可选世界旋转），两者都新增
  `float startTime = 0`；C# 只做 wire 级校验（有限/非负），越界交由原生按资产范围拒绝。
  `Montage(id)` 读取 catalog 的 `MontageInfo`（`Sections`/`Notifies` 来自 catalog；
  `Windows` 由 notifies 在 C# 端派生）。
- **C# 入口**：`PlayMontageCore` 只校验执行层仍不支持的 provider/easing/rotation method
  枚举与 target 集合，不再按 `Sections.Count` 或 `Static` 提前拒绝；多 section 链与
  `staticPose` 的消费全部由原生执行层负责（C# 不读 `staticPose`）。
- Catch 的 9 个 Montage 已接入资源；C# 在 `MovementMode=Falling` 时提交
  `Catch_Hurdle/Mantle/Vault` 家族（Chooser 按顶面纵深自动选，或显式 action）与
  `Catch_Cliff_high`。
  入口时间是第一个 `PoseSearchBranchIn` notify 的 `Time`（即采样窗口下界，
  `entrySource=window-min`）；日志同时给出 `window=[t,end]` 与 `frames=duration*30`。
  没有该 notify 则 0；越界回退 0 并标 `entry=invalid`。`BackFloor` 仅 Hurdle Catch
  有窗口，按顶面落点解释（见
  [原生 MotionWarping](animation-motion-warping.md) 的消费路径）。
  Montage id 解析是**资源侧约定**：clip 为 `AM_` + `M_Neutral_Traversal_<action>`
  （Vault 无 `_stand`，其余带 `_stand`），Montage 先试 `AM_<clip>`、再试
  `AM_<clip>F`（Cliff 多一个 F）；都不存在时拒绝并输出
  `catch-montage-missing clip=... tried=...`。

## section 链与 link 推进

从 section 0 开始：单 section 的播放范围是 `[startTime, endTime]`；多 section（或单段
自连接）安装 section 链，播放范围始终是当前 section 的 `[startTime, endTime]`，段末按
`nextSection`/`linkMethod` 切到下一段。**只允许一次性播放**（`loop=true` 拒绝）；
链式 Montage 的每个 section 必须在装配期证明落在已加载 clip 内。

link 时间（`ResolveMontageSectionLinkTime`，来自 UE `FAnimLinkableElement`）：

```text
""（未设置，按 UE 默认）或 Absolute: nextTime
Relative:                          section.startTime + nextTime
Ratio（UE Proportional）:          section.startTime + nextTime * (endTime - startTime)
```

- `nextSection` 为空表示播放结束：停在段末、保留姿态，`Completed()` 为真。
- 链接落点必须落在目标 section 的 `[startTime, endTime)` 内（容差 `1e-3` s）；越界在
  `PlayMontage` 装配期拒绝。未知 `linkMethod` 同样拒绝。
- **UE 5.8 runtime 不再消费 NextTime**（段末直接跳到下一段起点，`AnimMontage.cpp:2722-2728`）；
  VL 执行的是数据层保留的 NextLink 语义，注释已在 `animationMontage.h` 写明。
- 段末推进最多 10 个 sub-step（UE `AnimMontage.cpp:2575`）；超过时放弃本 tick 剩余时间，
  下一 tick 继续，避免自连接死循环。
- 根运动只累加实际移动的子区间；段跳变本身不产生根位移/根旋转。连续过渡（链接时间 ==
  段末）不结束活动 notifyState；向后跳在段末补 `end(interrupted)`；向前跳过区间不重放
  被跳过的事件，活动 notifyState 若结束点落在被跳过区间，会在新段第一次扫描时补 `end`。
- 段边界按半开区间扫描：正好落在段末的即时 notify 属于下一段，只触发一次。
- 跳变（非连续段切换）推进 `discontinuity`，姿态历史按硬跳变处理。
- **近似**：同一 tick 内“多段 + 空洞 + 活动 warp 窗口”时，warp 仍按整 tick 的起点/终点
  调用一次 `Apply`，不按每个 sub-step 分别求值；当前没有资产同时触发这三个条件。

## Static provider

`provider=Static` 的窗口使用 Montage 资产里的 `staticPose`（mesh/组件空间、厘米）：

```text
offset = Root(EndTime) * staticPose^-1      （UE FTransform::GetRelativeTransform 约定）
targetModel += offset.translation           （只消费平移）
```

`staticPose` 由原生执行层从 `MontageWarp` 复制到 `MotionWarpWindow`，只加平移；
`offset` 的旋转不参与旋转 warp（旋转仍只由 target 的目标朝向决定）。`staticPose`
**不进入 catalog/C#**，C# 端没有对应字段；缺 `staticPose` 或旋转退化在装配期拒绝。

## 播放范围

`SkeletalPlayback::SetPlaybackRange(start, end)`（秒、clip-local）是 Montage 截断的执行手段：

- 参数必须有限、`start >= 0`、`end > start`；`end` 越过片段末尾时按片段时长 clamp
  （Montage `length` 与 sequence `duration` 的导出误差走这条路径），clamp 后为空仍拒绝。
- 范围只影响播放长度、根位移积分与 notify 有效域，不改变姿态采样；非 0 起点不移动当前
  时钟，调用方需随后 `Seek(start)` 对齐（`Seek` 不补发区间内历史事件）。
- `Duration()` 变成 `rangeEnd - rangeStart`；`Completed()` 比较 `time >= rangeEnd`；
  `RootMotionTotal/Remaining` 用范围端点积分；循环域是 `[rangeStart, rangeEnd)`。
- 直接调用 `SetPlaybackRange` 会清空 section 链（范围是播放段身份）；`PlayMontage` 是
  Montage 范围的唯一装配入口。
- `Play`/`Select`/`TransitionTo` 恢复默认范围并清空 Montage 身份；`Stop()` 保留时钟与姿态、
  补发活动 notifyState 的 `end(interrupted=true)`。

## notify 执行与 feedback

Sequence notifies 与（播放 Montage 时）Montage notifies 合并成一张按 `time` 稳定排序的
事件表：同刻事件保持“先 Sequence 后 Montage”的作者顺序；每个作者 notify 一个活动状态，
同名 notify 互不影响。

- 瞬时 `notify`：跨过其时间点时触发一次（区间半开，正好落在当前时刻的事件留到下一 tick）。
- `notifyState`：区间 `[time, time+duration)`，进入触发 `begin`、离开触发 `end`。
  作者区间不裁剪，可越过片段/播放范围末尾；`end` 的 `time` 是实际结束时刻，
  `startTime`/`duration` 仍是作者原值。
- 在 `Stop`、切片段、`TransitionTo`、循环回绕或一次性播放到达范围末尾时，仍活动的区间补
  `end(interrupted=true)`；循环回绕按“先尾部、后头部”处理，跨环区间得到 `end→begin`。
  `Seek` 清空活动状态且不补发被跳过的事件；暂停保留活动状态。
- 反馈字段：`notifies[]` 每项含 `name/track/kind/phase/time/startTime/duration/interrupted`，
  `montage` 含 `{id, length, range:[start,end]}`。C# 对应 `State.Notifies`、
  `State.Montage`、`State.MontageLength`、`State.MontageRange`。反馈在脚本 tick 之前发布，
  C# 看到的是上一动画 tick 的事件（既有反馈时序）。

## catalog 与曲线

`World::ScriptAnimationAssets()` 按 modelPath 发布一次性资产段：
`curves` 只包含“有 sidecar 且至少一条曲线”的 clip（`{name,times,values}`，键已排序）；
`montages` 发布全部 sections/notifies 与 warp 元数据（`warpRotation`、
`rotationTimeMultiplier`、`rotationMethod` 等，**不含 `staticPose`**——它只由原生
执行层消费）。它与旧 `catalog`（clip id 列表）一起随 `animation_catalog` 消息下发，
C# 按网格路径缓存为 `MontageInfo`/曲线键，不逐帧跨桥。C# 的 `WarpTarget`（位置 +
可选世界旋转）只用于 `PlayMontage` 提交，不进入 catalog。

`AnimationPlayback.CurveValue(clipId, curveName, time)` 是 C# 侧参考采样：秒、clip-local、
键间线性、区间外 clamp 到首/末值、单键常量；未知 clip/曲线与非有限时间都抛异常。
原生 `SampleFloatCurve` 仍是同一规范的参考实现；曲线值只在 Sequence sidecar 保存一份。

## 执行层拒绝矩阵

数据层保留的字段在播放入口按当前执行能力显式拒绝；原生与 C# 入口使用同一套执行
能力（C# 只把 provider/easing/rotation method 与 target 集合的校验前移给诊断）：

| 条件 | 结果 |
| --- | --- |
| `sections` 为空 | 拒绝：`has no sections`（原生） |
| `linkMethod` 不是 `Absolute`/`Relative`/`Ratio`（空串按 Absolute） | 拒绝：`supported link methods are ...` |
| `nextSection` 指向不存在的 section | 拒绝 |
| link 落点不在目标 section `[start,end)`（容差 1e-3） | 拒绝，诊断带落点与目标区间 |
| 链式 Montage 的 section 结束超出已加载 clip | 拒绝（装配期，避免 Tick 中途失败） |
| `provider` 不是 `None`/`Bone`/`Static` | 拒绝：provider 名带进诊断 |
| `Static` 缺 `staticPose`、location 非有限或旋转退化 | 拒绝 |
| `easing != "Linear"` | 拒绝：`only Linear is supported` |
| `warpRotation=true` 且 `rotationMethod != "Slerp"` | 拒绝：`only Slerp is supported`（C#/原生） |
| `loop == true` | 拒绝：Montage 不支持循环播放（C#/原生） |
| `rate` 非有限或不在 `(0,4]` | 拒绝（C#/原生） |
| `startTime` 非数字、非有限或负数 | 原生 `Script.InvalidMontageStartTime`；C# 抛 `ArgumentOutOfRangeException` |
| `startTime > 0` 且落在 section 0 可播放范围外（或剩余不足 1e-3 s） | 拒绝：带 montage id/section/范围（原生；C# 不复制资产时间轴判断） |
| `targets` 非 object、>32 项、键空/超 64、值不是 3/7 个数或非有限 | 原生 `Script.InvalidMontageTargets`；C# 空 targets 抛 `ArgumentNullException`、非有限/零旋转值抛 `ArgumentException` |
| 缺少 Montage 声明的 target，或多出未声明的 target | 拒绝，诊断带 montage id 与 target 名 |
| 未知 Montage id | 拒绝（C# `Unknown animation montage` / 原生 `Script.UnknownAnimationMontage`） |
| `derivedClipId` 为空、clip 未加载、或该 clip 没有 sidecar | 拒绝：Montage 必须绑定已加载的 sidecar |
| 旧 `clip` 请求携带 `warpTargets` | 拒绝：`Script.WarpTargetsDefinedByMontage`（warp target 由 Montage 资产声明） |

`warpRotation`/`rotationTimeMultiplier`/`rotationMethod` 与 `staticPose` 都已进入执行层；
非 `Slerp` 方法只在 `warpRotation=true` 时拒绝。
`AnimationPlayback.Play(clip, loop, rate, inPlace)` 已没有 warp 参数；`MotionWarpTarget`
类型与 `Play(..., warpTargets)` 重载已删除；`PlayMontage` 的 `WarpTarget` 重载可提交目标旋转。

## 未执行的数据（保留但不消费）

- **非 Slerp 旋转方法**：`rotationMethod` 的 `SlerpWithClampedRate`/`ConstantRate`/`Scale`
  等值原样保留，执行拒绝；`RotationType`/`AdditionalOffset`/`MaxRotationRate` 尚未进入
  VL 数据。
- **目标旋转的 pitch/roll**：旋转 warp 只消费 Y-up yaw，完整四元数的 pitch/roll 保留但不执行。
- **Static offset 的旋转**：`staticPose` 的旋转参与 offset 计算，但只把平移加到目标。
- **section 内 rate/offset、Montage 混合/blend-out**：未实现；资源侧用落地收口 +
  `ClearRootMotion` 替代 blend-out。
- **additive 与 sync marker**：只保留数据，不执行。
- 没有运行时 30/60/120 Hz 的 Montage/包内矩阵；只有模块测试与场景矩阵。

## 验证

`animation_preflight_tests` 相关套件（全量 255 项）：

- `AnimationMontage` 24 项（`tool/animation-tests/animationMontageTests.cpp`）：解析、
  GASP 夹具计数、数据保留、跨文件绑定拒绝、`FindMontage`、缓存键、
  `rotationMethod` 缺省/保留/类型拒绝。
- `AnimationScriptData` 30 项（`tool/animation-tests/animationScriptDataTests.cpp`）：
  catalog 曲线筛选与序列化、notify feedback 字段、窗口装配顺序与 flags、
  目标旋转透传、Static pose 映射与拒绝、未知 provider/非 Linear 拒绝、
  `PlayMontage` 范围/notify/Seek、warped yaw 写通道、失败不改播放器；
  section：Absolute/Relative/Ratio、空 nextSection 停止、未知/越界 link 拒绝、
  边界 notify 只触发一次、前向空洞跳过并补 end、后向跳变 interrupted；
  入口 `startTime`：跳过之前 notify/根位移、section 0 越界拒绝、多段链从入口开始。
- `NotifyPlayback` 11 项（`tool/animation-tests/notifyStateTests.cpp`）：瞬时/区间、
  越界 end interrupted、Stop/切片段/Transition、循环回绕、Seek 不补发、同名独立、
  范围 clamp、Montage 覆盖层合并与替换。
- `MotionWarping` 28 项（`tool/animation-tests/motionWarpingTests.cpp`）：见
  [原生 MotionWarping](animation-motion-warping.md)，含旋转/Static warp point。
- `scriptAnimationTests` 注册 48 项（`ScriptAnimationTest.*` 36 +
  `ScriptAnimationWorld.*` 12）；`MontageBranchInWindowsAreSamplingRanges` 锁定
  “只对 montage 取 branch-in 窗口”的规则：9 个 Catch 窗口 ≤1.2/30 s，10 个非 Catch
  montage `time==0`。

```powershell
.\build\bin\animation_preflight_tests --gtest_filter="AnimationMontage.*:AnimationScriptData.*:NotifyPlayback.*"
```

数据层解析与绑定以 `source/animation/animationMontage.*` 为准；播放入口、section 推进与
拒绝矩阵以 `source/animation/animationScriptData.cpp`、`source/animation/skeletalPlayback.cpp`、
`source/world/worldAnimation.cpp` 与 `script/VulkanLearn.Runtime/AnimationPlayback.cs` 为准。
