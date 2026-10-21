# 动画序列数据承载计划：Skeleton、Sequence sidecar 与 AnimMontage 资产

状态：计划，非契约。2026-10-20；已按 2026-10-20 四项决策修订（Sequence 唯一持有
曲线、C# `CurveValue` 同形 API、四动作一次迁移、数据/执行分层支持），并扩展
Skeleton 独立 JSON（第三层）；实施期追加 notifyState 原值保留与 §9.2 的 11 项决策。
本文不描述已实现行为；现状一律带源码/文档位置。

## 导读

| 你要做什么 | 先看 |
| --- | --- |
| 判断方向是否仍成立 | §0 结论摘要、§3 方案对比 |
| 写/改资产 JSON | §4（Skeleton §4.2、Sequence §4.3、Montage §4.4） |
| 接引擎/导入器 | §5 接入点、§6 身份与缓存 |
| 开一个 PR | §8.1 第一切片（文件级清单、测试、验证入口） |
| 查“为什么这么定” | §9.2 决策记录、§9.1 风险 |

**一句话口径**：骨骼 TRS 留在 FBX；SK 管层级/槽位/曲线名；Sequence 管曲线值与序列
notify；Montage 管窗口/区间/编排；C# 只提交目标、读取反馈，不复制插值。

**落地状态（2026-10-20）**：三层资产、数据层、播放与 C# 迁移全部落地——SK/22 个
sidecar/10 个 Montage（资源 `ebd7059`）；Skeleton/Sequence 数据层（`c300edb`）；
Montage 数据层（`72423ca`）；notify/notifyState 区间执行（`0356b7f`、`b3a5e79`）；
catalog/`playMontage`/C# `CurveValue`/`Montage`/`PlayMontage`/feedback（`e5a92f6`）；
清理 `skinning.root` 与脚本 `warpTargets`（`0de7bcc`）；资源 SM 接线（`aa78096`）与
删除残留 `root`（`ac0fe0c`）；旋转 warp（`36de338`）、section/link 与 Static provider
（`a74e29f`、`4d1475e`），资源 GASP 目标旋转接线（`b627bd0`）。剩余主要是非 `Slerp`
旋转方法/旋转参数、pitch/roll、Montage 混合与 section 内 rate/offset、热重载与烘焙缓存。
已落地部分的行为以 `contracts/animation-asset-import.md`、
`contracts/animation-montage.md`、`contracts/animation-motion-warping.md` 与源码为准；
验收矩阵、Climb walk 修复与旋转/section/Static 证据见
[P22 Montage 迁移记录](../../rationale/gasp-p22-montage-migration-record.md)、
[P23 旋转/section/Static 执行记录](../../rationale/gasp-p23-warp-execution-record.md)。

## 0. 结论摘要

- **三层资产**：Skeleton `SK_*.json` → Sequence sidecar `<stem>.anim.json` →
  AnimMontage `AM_*.json`。`Clip` 继续表示 UE `AnimSequence` 层（骨骼 TRS + 序列级
  数据）；Montage 用**Sequence 资产路径**引用（可带 BLAKE3 校验；clip id 由 loader
  从 sidecar 派生），不把 Montage 塞进 `Clip`。
- **Skeleton 独立 JSON**：`SK_*.json` 参考 UE `USkeleton`，保存 joints/restLocal、
  slots、曲线注册表 `curves`、虚拟骨/additive 参考姿态保留字段。推荐由 FBX 生成
  一次并提交，导入时作为**权威校验清单**（FBX 关节名称/父子/rest 必须一致）；
  FBX 仍是几何/权重/TRS 来源，第一切片不让手写 SK 反向驱动 FBX。
- **Sequence 用 JSON sidecar，并且是曲线的唯一持有者**：与 FBX 同目录的
  `<stem>.anim.json`，放帧率/帧数/长度、float 曲线、序列级 notify、sync marker、
  root motion 设置；`skeleton`/`skeletonBlake3` 指向 SK；骨骼 TRS 继续由 FBX 承载。
  **Montage 不复制任何曲线/键**。
- **Montage 用独立 JSON**：sections/slot、notify/notifyState 真实时间、link/next、
  MotionWarping modifier 的静态定义（窗口起止、provider、骨名、四个布尔/倍率/easing）。
  `slot` 必须在 SK `slots` 中注册。数据层完整保留这些字段并可 round-trip；执行层按
  切片逐步支持（见下）。
- **C# 曲线 API 与 UE Blueprint 同形**：`AnimationPlayback.CurveValue(clipId,
  curveName, time)`（秒、线性、区间外 clamp）。当前 C#↔原生桥是 native→C# 每帧 JSON
  回调，没有同步查询；第一切片用 catalog 一次性发布 `floatCurveKeys` + C# 线性采样
  实现同名 API。原生 `SampleFloatCurve` 仍是参考实现，C# 采样必须过 golden 测试。
- **warp 窗口不按“曲线名”直接绑定**：UE 的 modifier 接收的是位置，GASP 在 Blueprint
  `SetWarpTargets` 里用两条曲线采样算出 `BackFloor` 目标。VL 保持同一分层：C# 用
  `CurveValue` 自行算 `dOut` 后提交 target 世界位置；`MotionWarpWindow` 不加曲线字段。
- **分层支持**：已执行单/多 section、`Absolute`/`Relative`/`Ratio` link、`Linear` easing、
  provider `None`/`Bone`/`Static`、`ignoreZ`/`subtractRemaining` 与 yaw-only `Slerp`
  旋转 warp（P23）；仍未执行非 `Slerp` 旋转方法/旋转参数、非 `Linear` easing、
  Montage 混合与 section 内 rate/offset；additive、sync marker 只保留数据。只有无法
  无损表达的数据（未知 provider/easing/link 枚举、section 越界、多 slot track 等）才拒绝。
- **第一切片范围**：Skeleton JSON + Sequence sidecar + AnimMontage JSON 的
  导入/校验/读取，`PlayMontage`，notify 真实时间，C# 用 `CurveValue` 算 `dOut` 并只给
  targets；**四个动作（Hurdle/Climb/Mantle/Vault）一次迁完**。Climb 因 `attach` 首次
  真正生效、落点会变，单独用 UE 参考积分验收并单列结果。P23 已补 section 跳转、
  Static provider 与旋转 warp；仍不做非 `Slerp` 旋转方法、Montage 混合、
  PoseSearch、烘焙缓存。
- **体积基线**（实测，见 §6.2）：Walk 的完整审计侧车 592,248 B，裁剪成运行时序列数据
  只需 30,347 B；Walk FBX 1,678,800 B。Montage T3D 17,586 B，JSON 预计 4–8 KB。
  全 JSON 骨骼轨道约 0.54–1.28 MB/片段，且要自己承担精度/导入器，当前不做。
  SK 约 18–25 KB（91 骨 × 16 float + 名称/slots/curves），见 §6.2。

## 1. 背景与问题

### 1.1 当前 `Clip` 只有骨骼轨道

`source/animation/animationAssets.h:62-68`：

```cpp
struct Clip
{
    std::string id;
    std::vector<std::string> restFallbackJoints;
    ozz::animation::offline::RawAnimation raw;
    ozz::unique_ptr<ozz::animation::Animation> runtime;
};
```

没有曲线、notify、帧率/帧数、sync marker、root motion 设置或 Montage 数据；运行时
采样入口只有 `SampleLocals` / `SampleModels`（同文件 `:76-81`），都只处理 TRS。

导入器 `AssimpSkeletalImporter::BuildClip` 只读节点 TRS：
`source/mesh/loader/animation/assimpSkeletalImporter.cpp:529-537` 明确拒绝
`mNumMeshChannels`/`mNumMorphMeshChannels`，并要求每个通道映射到骨架关节、TRS 覆盖
整个区间（`:544-556`），最后用 ozz `AnimationBuilder` 构建压缩轨道（`:573-576`）。
FBX 的 ticksPerSecond 被换算成秒（`:541`），原始帧率/帧数没有保留。

结论：**导入器丢掉的不是“某个字段”，而是整类非 TRS 序列数据**；即使审计侧车已经
保存了曲线/notify，运行时 `Clip` 也没有承载位置。

### 1.2 GASP 现在需要的数据

- Hurdle 的 `BackFloor` 目标需要 `distance_from_ledge` 在两个窗口 EndTime 的采样值：
  `dOut = |curve(BackLedge.End) - curve(BackFloor.End)|`。当前两个值
  `34.098671` / `90.908951` cm 内联在资源 C#
  `Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs:490-496`，属于“资产数据转录”。
  证据：`artifacts/gasp-warp-hurdle/UE-EVIDENCE.md:58-79`、
  `artifacts/gasp-warp-hurdle/RESULTS.md:22-27`。
- 四个 traversal 动作的窗口、目标名、`attach` provider、四个布尔/倍率目前全部
  手写在 C# `BuildWarpTargets`（同上 `:479-553`）；换 gait/montage 就要改代码。
- `traversalNotifies` 的名称和 notify 时间也在 C#：名称按 clip 类型手写
  （`:463-473`），时间用 `duration*(index+1)/(n+1)` 均分（`:558-565`）。
  契约已记录这一点：
  `documents/contracts/animation-motion-warping.md:136-139`、
  `documents/rationale/gasp-p14-montage-runtime-record.md:27-38`。
- 当前运行时只给 C# 发布 clip id 列表：
  `source/world/worldAnimation.cpp:58-79`；没有曲线值、notify、帧信息或 Montage 目录。

### 1.3 要回答的问题

1. UE 的 Sequence/Montage/Skeleton 数据哪些可导出，T3D、UE Python、FBX 各能拿到什么；
2. Sequence 数据用 sidecar、扩展 FBX/自研容器、还是烘焙帧缓存；
3. Skeleton 是否独立资产，Montage 是否独立资产，如何被引用；
4. 运行时/C#/warp 窗口的接入点与所有权；
5. 第一个切片能做什么、明确不做什么。

## 2. 现状证据

### 2.1 VL 当前数据模型与缺口

| 数据 | 当前 VL 位置 | 现状 |
| --- | --- | --- |
| 骨骼 TRS 轨道 | FBX → `AssimpSkeletalImporter::BuildClip` → `Clip::raw/runtime` | 有；只读节点 TRS |
| 骨架/槽位/曲线注册表 | FBX 节点即骨架；无独立资产 | 无 SK JSON、无 slots、无曲线注册表；Montage slot/曲线名无处校验 |
| 曲线（float） | 无 | 审计侧车有，运行时 `Clip` 无 |
| 序列 notify | 无 | 审计侧车有，运行时不执行 |
| 帧率/帧数 | 无 | 只有 FBX `ticksPerSecond` 换算出的秒时长 |
| sync marker | 无 | 审计侧车有，运行时不执行 |
| root motion 设置 | 无 | 运行时总是保留原始根轨道（`animation-asset-import.md:57`；`animation-runtime-skinning.md:11`） |
| Montage sections/slot | C# 日志常量 `Default`；slot 只在 UE 资产里 | 资产无 |
| Montage notify/notifyState | C# 手写名称 + 均分时间 | 资产无 |
| Montage warp modifier | C# `MotionWarpTarget[]` 手写 | 资产无 |

契约明确写了当前边界：

- `documents/contracts/animation-asset-import.md:66-79`：`vl_animation_probe --import-gasp`
  的**报告**保留 curves/notifies/syncMarkers/settings/rateScale，并做长度/排序/有限值
  校验；“没有承诺曲线插值、事件执行、虚拟骨重建或 UE 蓝图等价”。这是工具报告，
  不是运行时 `Clip`。
- `documents/contracts/animation-runtime-skinning.md:218-221`：“没有源动画曲线/notify 执行。”
- `documents/contracts/animation-motion-warping.md:122-146`：当前契约文本仍记录
  Hurdle 三个窗口、`Distance_From_Ledge` 未接入、notify 时间均分、没有旋转
  warp/section/NotifyState；但 `artifacts/gasp-warp-hurdle/RESULTS.md:22-27` 记录
  资源侧已经提交四个窗口（含两个 `FrontLedge` + `attach`）。**契约限制段落后于
  资源实现**，本计划以资产/Montage 数据为迁移目标，不把旧限制当现状。

### 2.2 现有侧车：数据已经在，但只用于审计

`audit_gasp.py` 的 `clip_record`（`tool/animation/audit_gasp.py:160-212`）为固定 12 段
locomotion 导出：

- `duration` / `numFrames` / `numKeys` / `rateScale`；
- `tracks`（作者轨道名）；
- `settings`（`enable_root_motion`、`root_motion_root_lock`、`force_root_lock`、
  `additive_anim_type`、`retarget_source`）；
- `floatCurveKeys`：每条曲线 `{name, times, values}`；
- `syncMarkers`；
- `notifies`：`{track, time, duration, fields}`（`fields.ueStructText` 里含 notify 名）；
- `rawRootSamples`、`referenceFrames`（审计用，运行时不需要）。

这 12 份侧车已经随正式资源落在
`<resourcePath>/Maps/SC_gasp/Source/M_Neutral_*.json`，每份约 534–734 KB。它们没有
被任何 SM JSON 引用，运行时也不读取；`vl_animation_probe --import-gasp` 只是把它们
当作审计输入（`tool/animation/gaspImportReport.cpp:44-75,108-115`）。

实测 Walk Loop 侧车各段 JSON 字节数：

```text
total                 592,248
floatCurveKeys         16,908   (5 条曲线；contact_l/r 各 242 keys)
notifies               11,661   (9 条)
tracks                  1,236   (88 条作者轨道)
referenceFrames       193,264   (审计用)
rawRootSamples         32,559   (审计用)
只保留运行时需要的头+curves+notifies  30,347  (约 5.1%)
```

曲线键不保证落在序列帧上，也不保证在 `[0,duration]` 内：Walk 的 `Phase` 首末键是
`-0.394025` / `4.026487`，`enable_warping` 只有 `0.1` 一个键；`movedata_speed`
从 `0.033333` 开始。这是“时间必须按秒存、不能按帧取整”的直接证据。

Traversal 侧没有对应的完整曲线导出：`audit_gasp_traversal.py:67-80` 只记录
`curves` 的**名字**和 `notifyTracks` 名字，没有 `get_float_keys` 的键值。Hurdle 的
`distance_from_ledge` 采样值来自另一个一次性脚本
`artifacts/gasp-warp-hurdle/export-ue-hurdle.py:156-167`，只采了 10 个时刻，不是完整
键集。第一切片必须先补这次导出（见 §8/§9）。

### 2.3 UE 侧数据来源对照

下表是“能否拿到”的结论，括号内是证据或待验证说明。

| 数据 | T3D `.copy` | UE Python（UE 5.8.2） | FBX + Assimp 6.0.2 |
| --- | --- | --- | --- |
| 骨骼 TRS 轨道 | 不直接给关键帧 | 只能按时刻采样姿态，不给原始键 | **有**：`aiAnimation` 的 TRS 通道 |
| float 曲线键 | `RawCurveData(FloatCurves=...)` 含完整键、`LeaveTangent`、`CurveName`（`artifacts/gasp-p0/ue-audit-05/M_Neutral_Walk_Loop_F.copy`） | `AnimationLibrary.get_float_keys` 给 time/value，**无 tangent/interp**（12 段 locomotion 已成功） | 无 |
| 序列 notify | `Notifies(i)` 含 `NotifyName`、`LinkValue`、`Duration`、`NotifyStateClass`（Walk `.copy`） | `get_animation_notify_track_names` + `get_animation_notify_events_for_track` 成功（Walk 9 条） | 无 |
| notifyState 起止 | `Notifies(i)` 的 `LinkValue`+`Duration`+`EndLink.LinkValue` | 同一接口给 `trigger_time`/`duration` | 无 |
| sync marker | 待验证（`.copy` 未见 `SyncMarkers` 段） | `get_animation_sync_markers`（Walk 为 0） | 无 |
| 帧率/帧数 | 只有 `SequenceLength` | `get_sequence_length` / `get_num_frames` / `get_num_keys` | 只有 `ticksPerSecond`/`durationTicks` |
| root motion 设置 | 有（`enable_root_motion` 等属性） | `get_editor_property("enable_root_motion"/"force_root_lock"/"root_motion_root_lock")` | 无 |
| additive 设置 | 有 | `get_editor_property("additive_anim_type")` | 无 |
| Montage sections/slot/长度 | `CompositeSections`、`SlotAnimTracks`、`SequenceLength`（`artifacts/gasp-warp-audit/montages/AM_*.copy`） | P14 记录用 UE Python 读到 sections=[Default]、slot、length（`gasp-p14-montage-runtime-record.md:8-25`） | 无 |
| Montage notify/notifyState | **有**：`Notifies(i)` 精确时间 + `Duration`；嵌套对象含 `WarpTargetName`/`WarpPointAnimProvider`/`WarpPointAnimBoneName`/`bIgnoreZAxis`/`bWarpRotation`/`AddTranslationEasingFunc` | `AnimMontage.Notifies` 是 protected，不能读；`MotionWarpingUtilities.GetMotionWarpingWindowsFromAnimation` 只给窗口起止，modifier 布尔读不到 | 无 |

证据细节：

- `artifacts/gasp-warp-hurdle/UE-EVIDENCE.md:18-19` 记录
  `UAnimMontage::Notifies` 对 Python protected；窗口用
  `UMotionWarpingUtilities::GetMotionWarpingWindowsFromAnimation` 读出，与 T3D 逐位一致。
- 同一文件 `:20-40` 给出 Hurdle 四个窗口的精确时间/顺序；`:80-108` 给出
  `WarpPointAnimProvider=Bone` 的 `attach` 语义。
- Hurdle Montage T3D 里可直接读到
  `Notifies(1)=(NotifyName="MotionWarping",...,Duration=0.156754,EndLink=(...LinkValue=0.234904),LinkValue=0.078149,...
  )`，以及嵌套 modifier 的 `WarpTargetName="FrontLedge"`、
  `WarpPointAnimProvider=Bone`、`WarpPointAnimBoneName="attach"`、
  `bIgnoreZAxis=False`、`AddTranslationEasingFunc=Linear`。
- T3D 只写出与默认值不同的属性：`bIgnoreZAxis`、`bSubtractRemainingRootMotion`
  默认 `False`，`bWarpRotation` 默认 `True`，`WarpPointAnimProvider` 缺省为
  `None`。转换器必须按 UE 默认补齐，**不能把省略的布尔一律当 False**；Hurdle 的
  FrontLedge 窗口正是省略 `bWarpRotation` 而取默认 True。

Montage 长度不等于 sequence 时长（实测；sequence 来自 `traversal-audit.json`，
Montage 来自 T3D `SequenceLength`）：

| clip（stand Lfoot） | sequence duration | Montage length |
| --- | --- | --- |
| Climb_Start_2_5 | 3.333333 | 2.750000 |
| Vault_1_0 | 2.566667 | 2.200000 |
| Mantle_1_0 | 2.000000 | 2.000000 |
| Hurdle_1_0_V2 | 1.666667 | 1.666667 |

这说明 Montage 至少存在 **section 截断**；是否还带 rate/segment offset 未知
（`SlotAnimTracks` 的 `CachedPlayLength` 与 section 长度不一致，见 §9.3）。当前
C# 用 Montage 时长作为 `traversalDuration`，但播放器仍按 clip 时长跑；第一切片要把
Montage section end 变成原生 playback range，否则 `Completed`/`RootMotionRemaining`
仍按 clip 末尾算。

### 2.4 FBX/Assimp 的能力边界

- 依赖版本：`extern/assimp/Assimp` 的 git tag 为 `v6.0.2`
  （`fb375dd8c0a032106a2122815fb18dffe0283721`，2025-06-08）。
- Assimp 的 FBX 动画只保留白名单属性：`FBXConverter.cpp:2668-2679` 的
  `prop_whitelist = {"Lcl Scaling","Lcl Rotation","Lcl Translation","DeformPercent"}`，
  再经 `FBXAnimation.cpp:107-119` 校验；不在白名单的 `AnimationCurveNode` 直接
  `throw std::range_error("AnimationCurveNode target property is not in whitelist")`
  或被跳过。也就是说，UE 导出的自定义 float 曲线即使以 FBX `AnimationCurveNode`
  存在，Assimp 也不会作为 `aiAnimation` 通道交给调用方。
- `artifacts/gasp-warp-hurdle/hurdle-fbx-inspect.json` 的实际动画只有 88 个通道，
  每个通道字段只有 `positions`/`rotations`/`scales`，各 4,488 个键；没有任何曲线通道。
- 现有导入器进一步拒绝 mesh/morph 通道
  （`source/mesh/loader/animation/assimpSkeletalImporter.cpp:529-537`），所以
  即使 Assimp 暴露 morph 曲线也不会进入 `Clip`。

结论：**FBX 只适合继续承载骨骼 TRS；曲线/notify/Montage 数据必须另找承载。**

### 2.5 运行时当前消费路径

- 原生 warp 队列在 `SkeletalPlayback`：`source/animation/skeletalPlayback.h:102-104`；
  tick 时用窗口改写本 tick 根位移
  `source/animation/skeletalPlayback.cpp:317-330`；窗口门控/顺序在
  `source/animation/motionWarping.cpp:138-210`。
- C# 只提交 `target/window`：`script/VulkanLearn.Runtime/AnimationPlayback.cs:9-14,49-69`；
  原生 JSON 请求在 `source/world/worldAnimation.cpp:255-295` 解析并调用
  `SetMotionWarpWindows`（`:294`）。
- 播放器时长只取 clip 时长：
  `source/animation/skeletalPlayback.cpp:212-215`；`RootMotionTotal/Remaining`
  也按 clip 末尾算（`:217-229`），不知道 Montage section 的截断。
- catalog 只发布 clip id（`worldAnimation.cpp:58-79`），feedback 没有 notify
  （`:81-162`）。
- game-export 对资源路径的处理：`Asset()` 复制文件并扫描 JSON
  （`tool/game-export/gameExporter.cpp:113-125`）；`modelDataPath` 会整目录复制
  （`:118-122`）；扫描时只要字符串以 `Common/`、`Maps/`、`Generated/` 开头就当作
  资源复制，不要求字段名在白名单里（`:146-157`）。

## 3. 承载方案对比

### 3.1 候选方案

| 方案 | 数据完整性 | 身份/缓存 | 可选性/热重载 | 打包闭包 | 体积 | 精度 | 主要风险 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **a. FBX + JSON sidecar（推荐）** | 曲线/notify/帧信息完整；骨骼仍在 FBX | sidecar 文件 BLAKE3，进 `sourceInputs`；路径进 `modelCacheKey` | sidecar 可缺省；改 sidecar 需重载 World；可单独重新导出 | sidecar 在 `Source/` 由整目录复制；或显式 `Maps/` 路径被扫描 | 30 KB/片段（Walk 实测） | 时间秒（源 float32），值 float32；键间线性插值 | 插值与 UE 曲线可能不同；导出链路要补 |
| b. 扩展 FBX/自研二进制容器 | 可完整，但需要自己写容器/版本 | 容器 BLAKE3；需要新工具链 | 不透明，热重载/审查差 | 需要导入器/导出器同时改 | 中 | 可精确 | Assimp 不读自定义曲线；FBX SDK 不可用；自研容器无生态，后续工具成本高 |
| c. 烘焙帧缓存（每帧姿态+曲线值） | 运行时最快，数据与 UE 脱钩 | 缓存 BLAKE3，作为生成物 | 不能单独改曲线；改源要重烘 | 生成物要进包/缓存 | 原始姿态约 0.43 MB/4 s/88 骨，压缩后更小 | 逐帧精确，但帧率固定 | 失去 UE 源语义；换 gait/重定向要重烘；当前没有烘焙工具 |
| d. 全部 JSON（含骨骼轨道） | 完整，可脱离 FBX | 规范 JSON BLAKE3；浮点格式化影响身份 | 可读可 diff，但片段大 | 需把 skeleton/bind 也从 FBX 拆出 | 估算 0.54–1.28 MB/片段 | 需 9 位有效数字才能 round-trip float32 | 新导入器、骨架/绑定来源分裂、仓库膨胀 |

### 3.2 全 JSON 骨骼轨道的估算

以 Walk 为例：88 条作者轨道 × 121 帧。每个骨骼每帧 T(3)+Q(4)+S(3) 共 10 个 float，
原始 40 B；JSON 文本按每数约 12 字符（9 位有效数字 + 分隔符）估约 120 B/骨/帧：

```text
原始二进制：88 × 121 × 40  = 425,920 B  ≈ 0.43 MB
JSON 估算：  88 × 121 × 120 = 1,277,760 B ≈ 1.28 MB
Walk FBX 实测：1,678,800 B
Hurdle（51 帧）：JSON 估算 88 × 51 × 120 ≈ 0.54 MB；FBX 实测 1,008,544 B
按 91 根物理骨（含无作者轨道的绑定回退骨）：
  91 × 121 × 40  = 440,440 B  ≈ 0.44 MB（原始）
  91 × 121 × 120 = 1,321,320 B ≈ 1.32 MB（JSON）
```

实际 FBX 作者轨道是 88 条（Walk 侧车 `tracks` = 88），所以上表按 88 算；91 是
“全部物理骨”的上界。30 Hz × 4 s 的关键帧密度下，全 JSON 约 1.3 MB/片段；这还没有
包括 `skeleton`/bind 数据和 JSON 缩进。

JSON 文本甚至比 FBX 小，但这不是选它的理由：FBX 已经携带骨架、绑定和轨道，且
导入器/坐标/绑定校验都已围绕它建立；把轨道搬进 JSON 会新增一套骨架/绑定/坐标
导入路径，并把“资产身份”从文件字节变成“规范 JSON 序列化 + 浮点格式”。全 JSON 的
BLAKE3 必须对规范字节（固定键序、固定浮点格式）取摘要，否则导出器换一个键序/
小数位就产生新身份；sidecar 只对“曲线/notify 这半部分”取文件字节摘要，爆炸半径
小得多。打包闭包上，全 JSON 还要把 skeleton/bind 源一并纳入，不能再靠
`modelDataPath` 的整目录复制覆盖。当前没有脱离开 FBX 的需求，所以全 JSON 只作为
未来选项。

### 3.3 为什么不选 b/c

- **b 扩展 FBX/自研容器**：Assimp 6.0.2 对自定义 `AnimationCurveNode` 是白名单
  过滤（§2.4），要读就得改/替换 Assimp 或引入 FBX SDK；自研二进制容器则要自己
  承担版本、端序、工具、审查和热重载。收益只是“少一个文件”，成本远高于 sidecar。
- **c 烘焙缓存**：它把“源数据”和“运行时数据”合并，失去按曲线/窗口重新导出的能力；
  换 gait、改 notify、改 section 都要重烘。适合未来发行包的性能优化，不适合当前
  还在对齐 UE 语义的阶段。

### 3.4 推荐：Skeleton JSON + sidecar + Montage JSON

推荐 a，并把 Skeleton、Montage 都独立成 JSON。理由：

1. **身份简单**：三个 JSON 各自 BLAKE3；FBX 的身份/导入路径不变；SK 用
   `skeletonBlake3` 把 Sequence 绑到具体骨架版本。
2. **可选性**：sidecar/montage 可缺省；SK 对 skeletal mesh 是第一切片必需项
   （Montage 的 slot 与曲线的注册表都需要它），这是有意的迁移，不是隐式默认。
3. **审查/热重载**：JSON 可 diff、可手改、可单独重新导出；World 重载即可生效，
   不需要重导 1.5 MB FBX。
4. **打包**：SK/sidecar/Montage 都走显式 `Maps/...` 路径；sidecar 另有
   `Source/` 整目录复制兜底（`gameExporter.cpp:118-122,146-157`）。
5. **体积**：30 KB sidecar 对 1.5 MB FBX，约 50 倍；Montage 4–8 KB 对 17.6 KB
   T3D；SK 约 18–25 KB。
6. **Skeleton 不手写**：SK 由 FBX 生成一次并作为导入校验清单，避免第三套作者源。

## 4. 三层资产模型与 schema

### 4.1 模型

```text
SM_mannequin.json
  skinning.skeleton   -> Skeletons/SK_*.json（Skeleton 资产）
  skinning.clips[]    -> FBX（几何/权重/TRS） + <stem>.anim.json（Sequence 数据；曲线唯一来源）
  skinning.montages[] -> Animations/AM_*.json（Montage 数据）
                         Montage.sequence -> Sequence 资产路径（可带 BLAKE3）

Skeleton JSON（每个骨架一份）
  skeleton id / root
  joints[]（name/parent/restLocal；拓扑序、父空间、厘米）
  slots[]（至少 DefaultSlot；Montage slot 必须在这里注册）
  curves {name: "Float"}（曲线注册表；值仍在 Sequence）
  virtualBones / additiveReferencePose（保留，第一切片不执行）

Sequence sidecar（每个 clip 一份）
  skeleton id + skeletonBlake3（指向 SK）
  frameRate / numFrames / numKeys / duration / rateScale
  rootMotion 设置 / additive
  floatCurveKeys[]（name + times + values；必须在 SK curves 注册；Montage 不复制）
  notifies[]（track/name/time/duration）
  syncMarkers[]（保留，不执行）

AnimMontage JSON（每个动作一份）
  montage id / sequence（Sequence 资产路径 + BLAKE3）/ slot / length
  sections[]（数据层保留多 section/link/next；执行层分切片）
  notifies[]（真实时间；warp notify 还带静态 modifier 定义）
```

三层关系与校验链：

| 层 | 资产 | 权威内容 | 被谁引用 | 导入校验 |
| --- | --- | --- | --- | --- |
| 0（源） | FBX | 几何、权重、绑定 TRS | SM `modelDataPath`；SK 的 `source.fbx` | 现有导入器 |
| 1 | Skeleton `SK_*.json` | 关节名/父子/restLocal、slots、曲线注册表 | SM `skinning.skeleton`；Sequence `skeleton` | FBX 关节名称/父子/rest 与 SK 逐项一致，否则拒绝 |
| 2 | Sequence `<stem>.anim.json` | 帧信息、曲线、序列 notify、root 设置 | SM `skinning.clips[].sequence`；Montage `sequence` | `skeleton`/`skeletonBlake3` 等于 SK；曲线名已在 SK `curves` 注册 |
| 3 | Montage `AM_*.json` | sections/slot/notify/warp 静态定义 | SM `skinning.montages`；C# `PlayMontage` | `slot` ∈ SK `slots`；`sequence` 路径可解析；解析出的 Sequence 必须同 skeleton |

数据流：

```text
FBX（几何+权重+TRS）
  └─ export_gasp_skeleton.py ─> SK_*.json（提交；权威校验清单）
       └─ SM skinning.skeleton
FBX + SK
  └─ audit/export ─> <stem>.anim.json（skeleton id/hash + floatCurveKeys）
       └─ AM_*.json（sequence 路径 + sequenceBlake3；slot ∈ SK slots）
```

**为什么三层不扁平**：

- Skeleton 是“骨架身份/槽位/曲线注册表”，可以被同一角色的 Sequence 与 Montage
  共同引用；它不属于某一个 clip，也不属于某一个动作。
- Sequence 是“姿态数据”，可以被 locomotion/Motion Matching/预览复用；Montage 是
  “动作编排”。三者变更频率和所有者不同：改窗口/notify 不应重导 FBX，改曲线不应
  重写 Montage，改骨架不应悄悄改 clip 的曲线契约。
- `Clip` 已经承担 ozz 压缩轨道和 `restFallbackJoints`；把 skeleton/sections/窗口塞进
  `Clip` 会把身份/gameplay 数据混进骨骼资产，破坏 Motion Matching 的 clip 身份。
- **曲线只存 Sequence 一份**，**关节/槽位/曲线名只在 SK 一份**；Montage 只引用
  Sequence 资产路径，不复制 `floatCurveKeys` 或关节表。

### 4.2 Skeleton JSON

#### 命名与位置

- 文件：`Maps/<map>/Skeletons/SK_<name>.json`；跨地图共享的骨架放
  `Common/Skeletons/SK_<name>.json`。GASP 使用
  `Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json`。
- 引用：SM JSON 增加 `skinning.skeleton`：

```json
"skinning": {
  "root": "root",
  "skeleton": "Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json",
  "clips": [
    { "id": "M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot",
      "path": "Maps/SC_gasp/Source/M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.fbx",
      "sequence": "Maps/SC_gasp/Source/M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.anim.json" }
  ],
  "montages": [
    { "id": "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot",
      "path": "Maps/SC_gasp/Animations/AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.json" }
  ]
}
```

`skinning.root` 保留为兼容字段：若存在必须等于 SK 的 `root`；新资源可以只写
`skinning.skeleton`，根骨名以 SK 为准。

#### schema 草案

```json
{
  "format": "vl.skeleton.v1",
  "skeleton": "SK_UEFN_Mannequin",
  "root": "root",
  "joints": [
    {
      "name": "root",
      "parent": -1,
      "restLocal": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    },
    {
      "name": "pelvis",
      "parent": 0,
      "restLocal": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    }
  ],
  "slots": ["DefaultSlot"],
  "curves": {
    "distance_from_ledge": "Float",
    "contact_l": "Float",
    "contact_r": "Float"
  },
  "virtualBones": [],
  "additiveReferencePose": null,
  "source": {
    "fbx": "Maps/SC_gasp/Source/SKM_UEFN_Mannequin.fbx",
    "fbxBlake3": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "tool": "export_gasp_skeleton.py"
  }
}
```

字段说明（示例中的 `restLocal` 数值仅示范形状，实际来自 FBX；`fbxBlake3` 为占位）：

| 字段 | 类型 | 必需 | 说明 |
| --- | --- | --- | --- |
| `format` | string | 是 | 必须 `vl.skeleton.v1` |
| `skeleton` | string | 是 | 骨架 id；Sequence 的 `skeleton` 必须等于它 |
| `root` | string | 是 | 根骨名；必须对应 `joints` 中 `parent=-1` 的唯一项 |
| `joints` | array | 是 | 拓扑序（父在子前）；≤骨架上限；名称唯一 |
| `joints[].name` | string | 是 | 与 FBX 关节名逐字一致（大小写敏感；导入器当前按字符串匹配） |
| `joints[].parent` | integer | 是 | `-1` 为根；否则是 `[0, i-1]` 的父索引 |
| `joints[].restLocal` | number[16] | 是 | 父空间绑定变换，**row-major 4×4**，厘米；平移在 `[3],[7],[11]`；导入时转成 Eigen 列主序 |
| `slots` | string[] | 是 | 至少 `DefaultSlot`；名称唯一；Montage 的 `slot` 必须在此注册 |
| `curves` | object | 是 | `{curveName: "Float"}` 注册表；对应 UE `USkeleton::CurveMetaData`；**值仍在 Sequence** |
| `virtualBones` | array | 否 | 虚拟骨保留字段；第一切片只解析/保留，不重建 |
| `additiveReferencePose` | object/null | 否 | additive 参考姿态保留字段；第一切片不执行 |
| `source` | object | 是 | `fbx` 路径、`fbxBlake3`、生成工具；供追溯 |

校验规则（导入时，原生）：

- 文件 ≤ 4 MB；关节数 ≤ 1024；`joints` 非空；`name` 非空、≤64、唯一（当前按
  大小写敏感匹配 FBX）；`root` 必须是 `parent=-1` 的唯一关节。
- `parent` 必须 `< i`（拓扑序），不能越界；从根到每个关节的父链无环。
- `restLocal` 16 个有限数；末行 `(0,0,0,1)`；线性部分无 shear/反射/奇异（与现有
  导入器的拒绝条件一致）；平移单位厘米。
- `slots` 非空、唯一、≤64；必须含 `DefaultSlot`；Montage 的 `slot` 必须命中。
- `curves` 名称非空、≤64、大小写不敏感唯一；值只接受 `"Float"`（第一切片）；
  未知类型拒绝（无法执行/无损表达）。
- `virtualBones`/`additiveReferencePose` 只做结构校验与保留；不执行、不参与
  `Clip` 采样。
- `source.fbx` 必须存在；`fbxBlake3`（若给）必须与导入 FBX 的实际摘要一致；
  `source.tool` 非空。

#### 权威性取舍

**推荐：FBX 生成一次 SK、提交为独立资产；导入时 SK 作为权威校验清单。**

```text
export_gasp_skeleton.py（读 FBX）
  -> SK_*.json（提交）
  -> 导入时：ReadMesh(FBX) 的 joints vs SK joints
       name/parent/order 逐项相等
       restLocal 逐元素 |diff| <= 1e-5
       不一致 -> 拒绝整个 skeletal mesh
```

- FBX 仍是几何/权重/绑定 TRS 的导入源；SK 不驱动 FBX 导入，只做校验和注册表。
- 不推荐第一切片“手写 SK 反向驱动 FBX”：两套作者源会漂移，且当前导入器的坐标/
  绑定/权重校验都围绕 FBX 建立。
- 后续方向（不在第一切片）：SK 成为唯一 skeleton 源，FBX 只供几何/权重；届时
  `ReadMesh` 按 SK 建立骨架，FBX 节点只用于蒙皮绑定与绑定姿态交叉校验。
- `skinning.skeleton` 在第一切片对 skeletal mesh 必需；旧资源需在同一资源 PR 补
  SK，否则拒绝并给出诊断（不能静默退回“从 FBX 猜骨架”）。

#### 引用与校验链

1. SM `skinning.skeleton` → SK 路径；SK 的 BLAKE3 进入
   `modelCacheKey`/`sourceInputs`。
2. SM `skinning.clips[]` 仍是 `id/path/sequence`；`path` 是 FBX，`sequence` 是
   Sequence sidecar。`skinning.root`（若写）必须等于 SK `root`。
3. Sequence sidecar 的 `skeleton` 必须等于 SK `skeleton`；`skeletonBlake3` 必须等于
   SK 文件摘要；`floatCurveKeys[].name` 必须已在 SK `curves` 注册，未注册默认拒绝
   并报告 clip/曲线名（避免拼写漂移）。
4. Montage `slot` 必须在 SK `slots` 中；Montage `sequence` 路径解析出的 Sequence
   sidecar 的 `skeleton`/`skeletonBlake3` 必须等于当前 SM 的 SK；否则拒绝。
5. 曲线值仍只在 Sequence；SK 只注册名字/类型，不复制键值。

### 4.3 Sequence sidecar

#### 命名与位置

- 文件：与 FBX 同目录，`<FBX stem>.anim.json`。例如
  `Maps/SC_gasp/Source/M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.anim.json`。
- 为什么不复用现有 `<stem>.json`：正式 `Source/` 已有 12 份 500 KB 级审计侧车
  （含 `referenceFrames`/`rawRootSamples`），复用会迫使运行时读审计数据，且同名
  冲突；`.anim.json` 后缀把“运行时序列数据”和“审计报告”分开。
- 引用方式：SM JSON 的 clip 条目显式写
  `"sequence": "Maps/SC_gasp/Source/M_....anim.json"`。不靠文件名自动发现；缺失
  该字段表示“这个 clip 没有曲线/notify”，行为与现在一致。

#### schema 草案

```json
{
  "format": "vl.anim-sequence.v1",
  "sequence": "M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot",
  "skeleton": "SK_UEFN_Mannequin",
  "skeletonBlake3": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "duration": 1.6666666,
  "frameRate": { "numerator": 30, "denominator": 1 },
  "numFrames": 50,
  "numKeys": 51,
  "rateScale": 1.0,
  "rootMotion": {
    "enableRootMotion": true,
    "forceRootLock": true,
    "rootMotionRootLock": "RefPose",
    "additiveAnimType": "None"
  },
  "tracks": ["root", "pelvis", "spine_01"],
  "floatCurveKeys": [
    {
      "name": "distance_from_ledge",
      "times": [0.0, 0.033333, 0.066667],
      "values": [-75.0, -74.2, -73.4]
    },
    {
      "name": "contact_l",
      "times": [0.0, 0.016667],
      "values": [1.0, 0.997594]
    }
  ],
  "notifies": [
    {
      "track": "Footstep Left",
      "name": "Footstep Left",
      "time": 0.0726406,
      "duration": 0.0
    },
    {
      "track": "1",
      "name": "PoseSearchBranchIn",
      "time": 0.0,
      "duration": 1.0030003
    }
  ],
  "syncMarkers": [],
  "sampling": { "interpolation": "linear", "outside": "clamp" },
  "source": {
    "fbx": "M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.fbx",
    "uePackage": "/Game/Characters/UEFN_Mannequin/Animations/Traversal/Hurdle/M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot",
    "exportedBy": "audit_gasp_traversal.py"
  }
}
```

字段说明：

| 字段 | 类型 | 必需 | 说明 |
| --- | --- | --- | --- |
| `format` | string | 是 | 必须 `vl.anim-sequence.v1` |
| `sequence` | string | 是 | clip id；必须存在于 SM `skinning.clips[]`，且 FBX 与 SK 一致 |
| `skeleton` | string | 是 | SK 的 `skeleton` id；必须等于加载的 SK id |
| `skeletonBlake3` | string | 是 | SK 文件 BLAKE3-256（64 hex）；必须等于加载摘要，防止 Sequence/SK 串配 |
| `duration` | number | 是 | 秒；必须与 FBX `raw.duration` 差 ≤ `1e-5` |
| `frameRate` | rational | 是 | UE `FFrameRate`；当前 GASP 均为 `30/1`，由 `numFrames/duration` 核对 |
| `numFrames` / `numKeys` | integer | 是 | 保留 UE 值；`numKeys` 不用于采样 |
| `rateScale` | number | 是 | 保留 UE 值；运行时当前不消费 |
| `rootMotion` | object | 是 | 四个设置原样保留；运行时根策略不变 |
| `tracks` | string[] | 否 | 作者轨道名，诊断/缺轨校验用；名字必须在 SK `joints` 中 |
| `floatCurveKeys` | array | 否 | 每条曲线 `name/times/values`；空数组合法；名字必须在 SK `curves` 注册 |
| `notifies` | array | 否 | `track/name/time/duration`；空数组合法 |
| `syncMarkers` | array | 否 | 原样保留，不执行 |
| `sampling` | object | 是 | 固定 `linear`/`clamp`；与实现一致 |
| `source` | object | 是 | FBX 文件名、UE 包路径、导出脚本；不含自身哈希 |

校验规则（第一切片）：

- 文件 ≤ 8 MB；曲线 ≤ 64 条；每条 keys ≤ 100,000；notify ≤ 4,096。
- `skeleton` 必须等于 SM `skinning.skeleton` 加载出的 SK id；`skeletonBlake3` 必须
  等于该 SK 文件的 BLAKE3。不一致拒绝（防止 Sequence 与骨架串配）。
- 曲线名非空、≤64 字符、**大小写不敏感唯一**；`times`/`values` 等长且 ≥1。
- **曲线名必须在 SK `curves` 注册**；未注册默认拒绝，诊断里带上 clip id 和曲线名，
  避免 `Distance_From_Ledge`/`distance_from_ledge` 这类拼写漂移悄悄变成空曲线。
- `tracks` 若给，每个名字必须在 SK `joints` 中；用于诊断作者轨道与 SK 的偏差。
- `times` 严格递增、有限；`values` 有限。允许 keys 在 `[0,duration]` 外（保留
  pre/post roll；证据：Walk `Phase`）。
- notify 的 `time`/`duration` 有限，`time ∈ [0, duration+1e-5]`，
  `duration ≥ 0`，`time+duration ≤ duration+1e-5`；`name`/`track` 非空。
- `duration` 与 FBX 一致；`numFrames/frameRate` 与 `duration` 差 ≤ `1e-3`。
- `additiveAnimType` 已知 UE 值保留、不因非 None 拒绝；第一切片不播放 additive
  （执行入口给诊断）。未知枚举拒绝。
- `rateScale` 有限且 >0；当前不消费，只拒绝非法值。

采样规则：

- `SampleFloatCurve(sequence, name, time)`：按名查找（大小写不敏感），
  `time` 在首键前取首值、末键后取末值，键间线性插值；单键曲线为常量；
  找不到返回 `false`。不分配内存、不修改时钟。
- 端点外 clamp 是显式选择（不是 ozz 的 extrapolate）；避免把作者 pre/post roll
  当作线性外推。
- 线性插值是**近似**：UE `FRichCurve` 默认可能是 cubic，而 Python
  `get_float_keys` 不返回 tangent/interp。Hurdle 的 `dOut` 采样点
  `0.598017`/`0.831335` 不在 30 Hz 帧上（帧点约 `0.6`/`0.8333`），所以键间插值
  会参与结果；验收需要用 UE 参考值设容差（§7.2），不能宣称与 UE cubic 等价。

#### 时间单位：秒，帧只作元数据

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| 秒（推荐） | 与 UE 采样 API、窗口 EndTime、T3D `LinkValue` 同单位；支持稀疏/子帧/区间外键 | 需要 float32→double 的精度说明 | **采用** |
| 帧 + 帧率 | 整数精确，便于逐帧工具 | 曲线键可不在帧上；`Phase` 有 `-0.394025`；窗口 EndTime `0.598017` 会被取整丢失 | 不采用 |
| 秒 + 冗余 frame 索引 | 两者都有 | 双份数据易不一致，需额外校验 | 不采用 |

保留 `frameRate`/`numFrames` 是为了证明“这段数据的原始采样率是 30 Hz”、做
一致性校验和未来帧工具；所有采样/窗口/notify 时间仍用秒。

### 4.4 AnimMontage JSON

#### 命名与位置

- 文件：`Maps/SC_gasp/Animations/AM_<sequence stem>.json`。
- 共享动作可放 `Common/Animations/`；GASP traversal 只属于 `SC_gasp`，放
  `Maps/SC_gasp/Animations/`，避免跨地图耦合。
- 引用：SM JSON 增加 `skinning.montages`：

```json
"skinning": {
  "root": "root",
  "clips": [
    { "id": "M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot",
      "path": "Maps/SC_gasp/Source/M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.fbx",
      "sequence": "Maps/SC_gasp/Source/M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.anim.json" }
  ],
  "montages": [
    { "id": "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot",
      "path": "Maps/SC_gasp/Animations/AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.json" }
  ]
}
```

不选“蓝图属性直接给 Montage 路径”的原因：蓝图属性是脚本侧数据，原生无法在
`SkeletalAsset` 导入时校验 sequence/骨名/窗口时间；也不选“全局资产注册表”，
因为当前运行时是引用驱动的加载，没有全局注册表。SM 已经拥有 clip catalog，
把 Montage 挂在同一个骨骼资产下最自然；game-export 也会顺着 `Maps/` 路径复制。

#### schema 草案

```json
{
  "format": "vl.anim-montage.v1",
  "montage": "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot",
  "sequence": "Maps/SC_gasp/Source/M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.anim.json",
  "sequenceBlake3": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "slot": "DefaultSlot",
  "length": 1.6666666,
  "sections": [
    {
      "name": "Default",
      "startTime": 0.0,
      "endTime": 1.6666666,
      "nextSection": "",
      "nextTime": 0.0,
      "linkMethod": ""
    }
  ],
  "notifies": [
    {
      "kind": "notifyState",
      "name": "MotionWarping",
      "track": "MotionWarping",
      "time": 0.078149,
      "duration": 0.156754,
      "warp": {
        "target": "FrontLedge",
        "provider": "Bone",
        "bone": "attach",
        "subtractRemainingRootMotion": false,
        "ignoreZAxis": false,
        "warpRotation": true,
        "rotationTimeMultiplier": 1.0,
        "easing": "Linear"
      }
    },
    {
      "kind": "notifyState",
      "name": "MotionWarping",
      "track": "MotionWarping",
      "time": 0.599556,
      "duration": 0.231779,
      "warp": {
        "target": "BackFloor",
        "provider": "None",
        "bone": "",
        "subtractRemainingRootMotion": false,
        "ignoreZAxis": false,
        "warpRotation": false,
        "rotationTimeMultiplier": 1.0,
        "easing": "Linear"
      }
    }
  ],
  "source": {
    "t3d": "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.copy",
    "uePackage": "/Game/Characters/UEFN_Mannequin/Animations/Traversal/Hurdle/AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot",
    "exportedBy": "import_gasp_montages.py"
  }
}
```

字段说明（示例里的 `sequenceBlake3` 是占位 64 hex，不是真实摘要）：

| 字段 | 类型 | 必需 | 说明 |
| --- | --- | --- | --- |
| `format` | string | 是 | `vl.anim-montage.v1` |
| `montage` | string | 是 | 全局唯一 id（同一 SM 内）；C# 用它播放 |
| `sequence` | string | 是 | **Sequence 资产路径**（sidecar `.anim.json`），路径是唯一权威；loader 从 sidecar 的 `sequence` 字段读 clip id，并校验它在 SM `skinning.clips[]` 中存在。**Montage 不写单独的 clip id 字段。** |
| `sequenceBlake3` | string | 否 | sidecar BLAKE3-256（64 hex）；给了必须与加载摘要一致。用于 Montage 与 Sequence 的跨文件内容校验 |
| `slot` | string | 是 | 必须是 SK `slots` 中的名字（GASP 为 `DefaultSlot`）；第一切片只执行单 slot |
| `length` | number | 是 | >0；每个 section `endTime <= length+1e-3`；单 section 时等于其 `endTime` |
| `sections` | array | 是 | ≥1 个；数据层完整保留 name/startTime/endTime/nextSection/nextTime/linkMethod，执行层分切片 |
| `sections[].linkMethod` | string | 否 | UE link 方法（如 `Absolute`/`Relative`/`Ratio`）原样保留；第一切片不执行 |
| `notifies` | array | 否 | 按 UE `Notifies(i)` 原顺序；`kind` 为 `notify` 或 `notifyState` |
| `notifies[].time` / `duration` | number | 是 | 秒；真实资产时间，不做均分 |
| `notifies[].warp` | object | 否 | 只有 MotionWarping notify 有；静态 modifier 定义；**不含曲线** |
| `warp.target` | string | 是 | C# 动态 target 的键；非空 ≤64 |
| `warp.provider` | string | 是 | `None`/`Static`/`Bone` 全保留；执行层第一切片只 `None`/`Bone` |
| `warp.bone` | string | provider=Bone 时是 | 必须在骨架中；≤64 |
| `warp.staticPose` | object | provider=Static 时是 | `{location:[x,y,z], rotation:[x,y,z,w]}`；UE 静态 provider 的 transform，原样保留，Slice 3 执行 |
| `warp.subtractRemainingRootMotion` / `ignoreZAxis` | bool | 是 | 第一切片执行，对应现有 `MotionWarpWindow` 两字段 |
| `warp.warpRotation` | bool | 是 | 保留；第一切片不执行，Slice 3 执行 |
| `warp.rotationTimeMultiplier` | number | 是 | 有限、>0；保留，Slice 3 执行 |
| `warp.easing` | string | 是 | 保留已知 UE `EEasingFunc` 值；第一切片只执行 `Linear`，Slice 2 执行其余 |
| `source` | object | 是 | T3D 输入名、UE 包路径、转换脚本；不含自身哈希 |

校验规则（导入时，原生）：

- `sequence` 必须是已加载的 sidecar 路径（同 SM `skinning.clips[].sequence`）；
  loader 从 sidecar 的 `sequence` 字段读 clip id，并校验它在 SM `skinning.clips[]`
  中存在；`sequenceBlake3` 给了就必须匹配 sidecar 的实际 BLAKE3。**不读单独的
  clip id 字段，路径是唯一权威。**
- 解析出的 Sequence sidecar 的 `skeleton`/`skeletonBlake3` 必须等于当前 SM 的
  SK id/摘要；Montage 引用的 Sequence 必须同 skeleton，否则拒绝。
- `slot` 必须已在 SK `slots` 中注册（GASP 为 `DefaultSlot`）；未注册拒绝并给出
  Montage/slot/SK 诊断。
- `length > 0`；`sections` ≥1；每个 section：`name` 非空且唯一、`startTime >= 0`、
  `endTime > startTime`、`endTime <= length + 1e-3`；`nextSection` 为空或指向存在的
  section；`nextTime` 在目标 section 或 `[0,length]` 内；`linkMethod` 为空或是已知
  UE link 方法。**多 section/link 在导入时保留，不因“第一切片不执行”而拒绝。**
- `slot` 非空；`notifies` ≤64；窗口总数 ≤8（与现有 `Play` 的 8 窗口预算一致）；
  按数组顺序保存，不按时间排序（Hurdle 的 `Notifies` 顺序就是 UE modifier 添加顺序）。
- 每个 `warp`：`time >= 0`、`duration > 0`、`time+duration <= length+1e-3`；
  `target` 非空；`provider` 是已知枚举 `None`/`Static`/`Bone`；Bone 名存在于
  `mesh.joints`；`easing` 是已知枚举；`rotationTimeMultiplier` 有限且 >0。
  **未知 provider/easing/linkMethod 字符串拒绝**（无法映射到 UE 语义）；已知但
  第一切片不执行的值保留并给诊断。`provider=Static` 必须带有限的 `staticPose`，
  否则无法无损表达，拒绝。Sequence 的 `rootMotionRootLock`/`additiveAnimType`
  同理：已知 UE 枚举保留，未知值拒绝。
- `warpRotation`、`Static` provider、非 `Linear` easing 都不在导入时拒绝；执行层
  按 `PlayMontage` 的请求决定是否支持（见下表）。
- 目标名可以不唯一（Hurdle 两个 `FrontLedge` 窗口）；C# 只按名字提供**一个**
  世界位置，多个同名窗口共享同一个 target。
- `slot` 当前是单字符串（GASP 只有 `DefaultSlot`）；如果源 Montage 有多个
  `SlotAnimTracks`，本 schema 无法无损表达，转换器/导入明确拒绝并报告，不静默丢掉
  其余 slot；单 slot 必须在 SK `slots` 注册。
- **Montage 不复制曲线**：schema 里没有 `floatCurveKeys`/`curveValues`；
  需要曲线时按 `sequence` 路径去 Sequence 资产取。

#### 执行层切片（数据保留，执行分层）

> 2026-10-21 注：P23 已落地多 section/link、`Static` provider 与 yaw-only `Slerp` 旋转；
> 下表保留第一切片时的规划状态。当前仍在执行层拒绝的只有非 `Linear` easing、非
> `Slerp` 旋转方法与不可用静态 pose 等。

| 字段/能力 | 数据层 | 第一切片执行 | Slice 2 | Slice 3+ |
| --- | --- | --- | --- | --- |
| 单 `Default` section | 保留 | 是 | 是 | 是 |
| 多 section / `nextSection` / `nextTime` / `linkMethod` | 保留并可 round-trip | 否；播放请求给诊断并拒绝 | 是（跳转/link/next） | 是 |
| `slot` | 保留（单串） | 只 `DefaultSlot` | 单 slot | 多 slot 需 schema 扩展（未来） |
| `Linear` easing | 保留 | 是 | 是 | 是 |
| 非 `Linear` easing | 保留 | 否；播放请求给诊断并拒绝 | 是 | 是 |
| provider `None` / `Bone` | 保留 | 是 | 是 | 是 |
| provider `Static` | 保留 | 否；播放请求给诊断并拒绝 | 否 | 是 |
| `ignoreZAxis` / `subtractRemainingRootMotion` | 保留 | 是 | 是 | 是 |
| `warpRotation` / `rotationTimeMultiplier` | 保留 | 否（GASP 的 FrontLedge 为 true，但只做平移） | 否 | 旋转 warp 是 |
| additive 序列 | 保留（`additiveAnimType` 已知值） | 不播放；请求给诊断 | 不播放 | 按 additive 运行时另定 |
| sync marker | 保留 | 不执行 | 不执行 | 不执行（只作数据） |

原则：**只有无法无损表达的数据才拒绝**（未知 provider/easing 枚举、section 越界、
Bone 不存在、notify 越界）；已知但暂不执行的数据一律保留，执行入口给明确诊断，
不静默降级、也不在导入时丢掉。

UE 的 `EWarpPointAnimProvider` 实际枚举是 `None`/`Static`/`Bone`；GASP 的 traversal
只使用 `None` 与 `Bone`。用户口径里的“Model”如果指“目标用模型空间/根位置”，不需要
新增 provider，仍由 `None`（不加骨偏移）或 `Bone`（加骨偏移）表达。`Static` 数据层
保留（`provider="Static"` + `warp.staticPose`），第一切片不执行。

### 4.5 与 UE 字段的对应

| UE | VL schema | 说明 |
| --- | --- | --- |
| `USkeleton.ReferenceSkeleton`（骨骼名/父/参考姿态） | `SK.joints[]`（name/parent/restLocal） | 由 FBX 生成；导入作为权威校验 |
| `USkeleton.Slots` | `SK.slots[]` | Montage `slot` 必须注册 |
| `USkeleton.CurveMetaData` | `SK.curves{name:"Float"}` | 只注册名字/类型；值在 Sequence |
| `USkeleton.VirtualBones`（待核对属性名） | `SK.virtualBones` | 保留，第一切片不重建 |
| Additive 参考姿态（USkeleton/AnimSequence） | `SK.additiveReferencePose` | 保留，第一切片不执行 |
| `AnimSequence.SequenceLength` | `duration` | 秒 |
| `AnimSequence.SamplingFrameRate` | `frameRate` | 有理数；由 `numFrames/duration` 核对 |
| `AnimSequence.GetNumberOfFrames()` | `numFrames` | 原值 |
| `FRawCurveTracks.FloatCurves[].CurveName` | `floatCurveKeys[].name` | 保留大小写；查找大小写不敏感 |
| `FRichCurve.Keys[].Time/Value` | `times`/`values` | 不搬 tangent/interp |
| `AnimSequence.Notifies` | `notifies` | `track/name/time/duration` |
| `AnimSequence.SyncMarkers` | `syncMarkers` | 保留不执行 |
| `AnimSequence.bEnableRootMotion` | `rootMotion.enableRootMotion` | 保留 |
| `AnimSequence.RootMotionRootLock` | `rootMotion.rootMotionRootLock` | 保留 |
| `AnimSequence.bForceRootLock` | `rootMotion.forceRootLock` | 保留 |
| `AnimSequence.AdditiveAnimType` | `rootMotion.additiveAnimType` | 已知值保留；数据层不拒绝；第一切片不播放 additive |
| `AnimMontage.SequenceLength` | `length` | 秒；不等于 sequence `duration` 时第一切片按 section 截断处理；是否含 retime 见 §9.3 |
| `SlotAnimTracks[0].AnimSegments[].AnimReference` | `sequence` + `sequenceBlake3` | 只写 Sequence 资产路径；clip id 由 sidecar 的 `sequence` 字段派生，不写单独的 clip id 字段 |
| `CompositeSections` | `sections[]` | 完整保留 name/start/end；第一切片执行单 `Default` |
| `CompositeSections.NextSection` / `NextLink` / link method | `sections[].nextSection` / `nextTime` / `linkMethod` | 数据层保留可 round-trip；Slice 2 执行 |
| `SlotAnimTracks[0].SlotName` | `slot` | 保留 |
| `Notifies(i).NotifyName` | `notifies[].name` | |
| `Notifies(i).LinkValue` / `Duration` | `notifies[].time` / `duration` | 真实时间 |
| `RootMotionModifier_SkewWarp.WarpTargetName` | `warp.target` | 动态 target 的键 |
| `WarpPointAnimProvider` | `warp.provider` | `None`/`Static`/`Bone` 全保留；执行见 §4.4 表 |
| `WarpPointAnimBoneName` | `warp.bone` | |
| `WarpPointAnimStaticTransform`（UE 属性名待核对） | `warp.staticPose` | `Static` provider 的静态 transform；原样保留，Slice 3 执行 |
| `bIgnoreZAxis` / `bSubtractRemainingRootMotion` | `warp.ignoreZAxis` / `warp.subtractRemainingRootMotion` | 第一切片执行 |
| `bWarpRotation` / `WarpRotationTimeMultiplier` | `warp.warpRotation` / `warp.rotationTimeMultiplier` | 保留；Slice 3 执行 |
| `AddTranslationEasingFunc` | `warp.easing` | 全保留；第一切片只 Linear |

## 5. 运行时与导入器接入点

### 5.1 C++ 数据模型

拟新增/修改（不含实现细节，只给形状）：

```text
source/animation/animationSkeleton.h
  struct SkeletonJoint { std::string name; int parent = -1; Eigen::Matrix4f restLocal; };
  struct SkeletonAsset
  {
      std::string id, root;
      std::vector<SkeletonJoint> joints;
      std::vector<std::string> slots;
      std::map<std::string, std::string> curves;   // name -> "Float"
      // virtualBones / additiveReferencePose 原样保留，第一切片不执行
      std::string virtualBonesJson, additiveReferencePoseJson;
      std::string sourceFbx; std::string sourceFbxDigest;
      std::string sourceDigest;                    // SK 文件 BLAKE3
  };

source/animation/animationSequenceData.h
  struct FloatCurve { std::string name; std::vector<float> times, values; };
  struct SequenceNotify { std::string track, name; float time, duration; };
  struct SequenceData
  {
      std::string skeletonId, skeletonDigest;      // 必须等于 SkeletonAsset
      float duration = 0;
      int frameRateNumerator = 0, frameRateDenominator = 1;
      int numFrames = 0, numKeys = 0;
      float rateScale = 1;
      bool enableRootMotion = false, forceRootLock = false;
      std::string rootMotionRootLock, additiveAnimType;
      std::vector<std::string> tracks;
      std::vector<FloatCurve> curves;
      std::vector<SequenceNotify> notifies;
      std::string sourceDigest;   // sidecar 文件 BLAKE3
  };
  bool SampleFloatCurve(const SequenceData&, std::string_view name, float time, float& value);

source/animation/animationMontage.h
  struct MontageSection { std::string name; float startTime, endTime; std::string nextSection; float nextTime; std::string linkMethod; };
  struct MontageStaticPose { std::array<float,3> location; std::array<float,4> rotation; };
  struct MontageWarp { std::string target, provider, bone; bool subtractRemaining, ignoreZ, warpRotation; float rotationTimeMultiplier; std::string easing; std::optional<MontageStaticPose> staticPose; };
  struct MontageNotify { std::string kind, name, track; float time, duration; std::optional<MontageWarp> warp; };
  struct MontageWindow { std::string target; float startTime, endTime; }; // 无 curveValues
  struct Montage { std::string id, sequencePath, sequenceDigest, slot; std::string derivedClipId; float length; std::vector<MontageSection> sections; std::vector<MontageNotify> notifies; std::vector<MontageWindow> windows; std::string sourceDigest; };

source/animation/animationAssets.h
  struct Clip { ...; SequenceData sequence; };

source/animation/skeletalPlayback.h
  struct SkeletalAsset { ...; SkeletonAsset skeleton; std::vector<Montage> montages; };

source/animation/skeletalPlayback.h
  void SetPlaybackRange(float endTime);              // Montage section end
  const std::vector<SequenceNotify>& FiredNotifies() const; // 本 tick 触发
```

**Montage 不持有曲线**：`MontageWarp`/`MontageWindow` 没有 `curveValues` 或
`floatCurveKeys`。`sequencePath` 指向 Sequence sidecar，`sequenceDigest` 用于内容
绑定；clip id 由 loader 从 sidecar 的 `sequence` 字段派生（`derivedClipId`，仅运行时
诊断/播放用，不写进 Montage JSON、不参与身份）。需要曲线值时 C# 通过 catalog 的
`floatCurveKeys` + `CurveValue` API 读取，避免窗口 EndTime 预采样成为第二份数据源。

### 5.2 导入器/校验器

`source/mesh/validation/meshAssetValidator.cpp:249-275` 当前校验
`skinning.root/clips` 并拼 `modelCacheKey`。拟扩展：

1. `skinning.skeleton`（新必需）：资源相对 `.json` 路径；`skinning.root` 变为可选
   兼容字段，若存在必须等于 SK `root`；两者都拼进 `modelCacheKey`。
2. `skinning.clips[]`：`sequence` 仍为可选（字符串，非空、资源相对路径、`.json`）；
   与 `id`/`path` 一样做重复/空值校验，并拼进 `modelCacheKey`。
3. `skinning.montages[]`：`id`/`path` 非空、id 唯一、path 为资源相对 `.json`；
   Montage JSON 自己的 `sequence` 指向同 SM 的 sidecar 路径；拼进 `modelCacheKey`。
4. `MeshAssetBuildPlan`（`source/mesh/meshAssetTypes.h:141-151`）新增
   `animationSkeleton`/`animationSequences`/`animationMontages`，供 loader 使用。
5. `source/mesh/loader/common/meshAssetLoader.cpp:119-142` 当前在
   `ImportSections` 里建 `SkeletalAsset`、哈希 FBX、读 clip。拟：
   - 先加载/哈希 SK JSON，保存 `SkeletonAsset` 与摘要；SK 必须在任何 clip 之前；
     `buildPlan.skeletonRoot = SK.root`，`ReadMesh` 用 SK root 而不是 `skinning.root`；
   - `ReadMesh(FBX, SK.root)` 后，用 SK 校验关节数/顺序/名称/父子/`restLocal`（§4.2），
     不一致拒绝；`asset->skeleton = std::move(skeleton)`；
   - 哈希 sidecar/montage 文件，写入 `sourceInputs`；
   - `ReadClip` 后调用 `LoadSequenceData(sidecar)`，校验 `skeleton`/`skeletonBlake3`
     与 SK 一致、曲线名已在 SK `curves` 注册、duration/帧率，再挂到
     `asset->clips.back().sequence`；
   - 所有 clip 就绪后 `LoadMontage(montageJson, asset)`：解析 `sequence` 路径，找到
     同一 SM 里已加载的 sidecar；从其 `sequence` 字段派生 clip id 并确认在
     `skinning.clips[]` 中；核对 `sequenceBlake3`、`slot ∈ SK slots`、解析出的
     Sequence 同 SK；再校验 sections/link/骨名/窗口/枚举。**加载时不采样曲线、
     不生成窗口曲线值**。
   - 仍用“导入后重新哈希全部 `sourceInputs`”防止导入期间源被替换
     （现有逻辑 `:139-142`）。

侧车不能靠“同名文件存在就加载”：SM 显式给 `skeleton`/`sequence` 路径，缺失即
无曲线/无法校验，避免隐式发现和路径猜测。第一切片对 skeletal mesh 要求
`skinning.skeleton` 存在；旧资源需在同一资源 PR 补 SK。

### 5.3 `SkeletalPlayback`

- `Duration()`（`skeletalPlayback.cpp:212-215`）改为：
  `playbackRangeEnd > 0 ? min(playbackRangeEnd, clip.duration) : clip.duration`。
  `SetPlaybackRange(0)` 恢复 clip 时长；`Play`/`Select` 清零。
- `RootMotionTotal/Remaining`（`:217-229`）与 `PreviewPose` 的时长同样使用
  playback range；否则 Montage 截断后 remaining 会把 section 之后的根位移算进去，
  `subtractRemaining` 语义会错。
- `Tick`（`:287-330`）在推进 `time -> sampleTime` 后扫描
  `clip.sequence.notifies`：每 tick 开头清空 `firedNotifies`；
  `previousTime <= notify.time < currentTime` 的
  **零时长 notify** 追加到 `firedNotifies`；`Seek`/`Select`/`Play`/循环回绕时
  重置游标、不补发历史。第一切片 Montage 非循环，循环回绕的 notify 语义明确不做。
- `SetMotionWarpWindows`/`ClearMotionWarp`（`:434-444`）保留；`PlayMontage` 在
  `worldAnimation` 里组装窗口后调用它们。
- `SkeletalAsset` 增加 `skeleton`/`montages`；`Play` 不变，新增
  `SetPlaybackRange` 只影响后续 tick，不重采样当前姿态。

### 5.4 `worldAnimation`

`source/world/worldAnimation.cpp`：

1. **catalog**（`:58-79`）：保留现有 `ScriptAnimationCatalog()` 的 clip id 列表，
   新增 `ScriptAnimationAssets()`：
   ```json
   {
     "Maps/SC_gasp/Meshes/SM_mannequin.json": {
       "montages": [
         { "id": "AM_...",
           "sequence": "Maps/SC_gasp/Source/M_....anim.json",
           "clipId": "M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot",
           "length": 1.6666666,
           "windows": [
             { "target": "FrontLedge", "start": 0.078149, "end": 0.234904 }
           ] }
       ],
       "curves": {
         "M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot": [
           { "name": "distance_from_ledge",
             "times": [0.0, 0.033333, 0.066667],
             "values": [-75.0, -74.2, -73.4] }
         ]
       }
     }
   }
   ```
   `curves` 是 Sequence sidecar 的 `floatCurveKeys` **一次性**发布（context 创建/
   `animation_catalog` 更新时，不是每帧）；C# 缓存后由 `CurveValue` 线性采样。
   `montages[].clipId` 是从 sidecar `sequence` 派生的运行时字段，**不是 Montage JSON
   字段、不参与身份**；Montage JSON 只有 `sequence` 路径。
   Montage 的 `windows` 只有 target/start/end，没有 `curveValues`。
2. **请求**（`:255-295`）：新增 `mode:"playMontage"`：
   ```json
   { "component": "...", "mode": "playMontage",
     "montage": "AM_...", "targets": { "FrontLedge": [x,y,z], "BackLedge": [...], "BackFloor": [...] },
     "loop": false, "rate": 1, "inPlace": true }
   ```
   校验：montage 存在；`targets` 是对象、≤32 项、每项 3 个有限数；
   montage 的所有 distinct `warp.target` 都必须有 target；多余 target 拒绝；
   `loop` 第一切片必须 false；`rate ∈ (0,4]`。
   在 candidate 上：解析 Montage 的 `sequence` 路径 → 读取 sidecar 的 `sequence`
   字段得到 clip id → 找到已加载的 `Clip`（必须带 sidecar）→
   `Play(resolvedClipId, false, rate, inPlace)` → `SetPlaybackRange(section.endTime)`
   → 按 Montage `notifies` 顺序为每个 `warp` 建 `MotionWarpWindow{ targetName,
   targetLocation, startTime, endTime, subtractRemainingRootMotion, ignoreVertical,
   warpPointBone }` → `SetMotionWarpWindows`。执行层遇到不支持的
   section/provider/easing 时，请求失败并给出诊断，不改变当前播放器。全部成功后
   一次提交（沿用现有 candidate 模式 `:169,297-302`）。
3. **feedback**（`:81-162`）：新增
   `"notifies": [{"name","track","time","duration"}]`（本 tick 触发）；
   `"montage": "AM_..."`（当前 Montage，非 Montage 时为空）；
   `"duration"` 已是 playback range（`player.Duration()`）。
4. **Tick**（`:305-347`）：不变；`SkeletalPlayback::Tick` 内部收集 notify。

### 5.5 C# 提交面与 `CurveValue`

#### 为什么第一切片是 “catalog + C# 采样”

当前 C#↔原生桥是**回调式**：原生在脚本 tick 里调用 `NativeBridge.Invoke`
（`script/VulkanLearn.Runtime/NativeBridge.cs:60-90`；`source/script/scriptHost.cpp:73-92`
只加载并调用这一个入口），C# 通过请求 JSON 提交命令、通过反馈 JSON 读状态。
`source/script/scriptRuntime.cpp:507-512` 在 `TickScriptAnimations`（`:529`）之前
把 `animationFeedback` 交给 C#，所以 **C# 回调期间没有同步 native 查询能力**，
反馈还是上一动画 tick 的快照。

UE 给 Blueprint 的是 `GetCurveValueFromAnimation(AnimationAsset, CurveName, Time)`
（任意时刻取值）。第一切片用等价形状在 C# 侧实现：

- catalog 一次性发布每个 clip 的 `floatCurveKeys`（§5.4）；
- C# 在 `BindMeshAnimation`/`UpdateAnimationCatalog` 时缓存按 clip/曲线名索引的
  数组，不逐帧跨桥；
- `AnimationPlayback.CurveValue(clipId, curveName, time)` 在 C# 内做线性 clamp
  采样。原生 `SampleFloatCurve` 是参考实现，C# 采样必须过 golden 测试（§7.1/§7.2）。
- 原生同步查询（新 ABI/函数指针）列为**后续可选**，第一切片不做。

#### API 形状

```csharp
public readonly record struct AnimationNotify(string Name, string Track, float Time, float Duration);
public readonly record struct MontageWindowInfo(string Target, float Start, float End);
public readonly record struct MontageInfo(string Id, string SequencePath, string ClipId,
    float Length, IReadOnlyList<MontageWindowInfo> Windows);

// MontageInfo.ClipId 由 loader 从 sidecar 的 `sequence` 字段派生，仅运行时诊断/播放用；
// 它不在 Montage JSON 里、不参与身份。Montage 资产只有 `sequence` 路径。

// AnimationState 末尾追加（默认空，旧构造不破坏）：
//   string Montage = "", AnimationNotify[]? Notifies = null

public float CurveValue(string clipId, string curveName, float time);
public void PlayMontage(string montage, IReadOnlyDictionary<string, Vector3> targets,
    bool loop = false, float rate = 1, bool inPlace = true);
public MontageInfo? Montage(string id);
```

`CurveValue` 采样规范（写死）：

| 项 | 行为 |
| --- | --- |
| 时间单位 | 秒；片段本地时间，与 `SampleLocals`/`AnimationState.Time` 同源 |
| 查找 | `clipId` 必须属于当前 mesh 的 clip catalog；曲线名大小写不敏感 |
| 键间 | 相邻 key 线性插值：`v0 + (t-t0)/(t1-t0)*(v1-v0)` |
| 区间外 | clamp 到首/末值；不 extrapolate，不因 `t` 为负而报错（曲线可有 pre-roll key） |
| 单键 | 常量 |
| 缺 clip/曲线 | `ArgumentException`（不静默返回 0） |
| 非有限 `time`（NaN/±∞） | `ArgumentOutOfRangeException` |
| 时钟/状态 | 纯读取；不推进时钟、不改姿态、不触发 notify |
| 调用时机 | 与 `AnimationPlayback` 其它读 API 相同：当前线程、存活组件、活动 gameplay 回调 |

- `Play` 保留，`warpTargets` 参数保留用于兼容/其他调用；traversal 迁移到
  `PlayMontage` 后不再构造窗口。
- `BindMeshAnimation` 从 catalog 设置 `ClipIds`/`Montages`/曲线键缓存；
  `PlayMontage` 校验 montage id、`SequencePath`、targets 键集合、有限值；请求
  序列化到 `TakeAnimationCommands`。**没有单独的 clip id 参数/字段**；clip id
  由原生/catalog 从 sidecar 的 `sequence` 派生。
- `SynchronizeAnimations` 解析 `notifies`/`montage`/`duration` 到 `AnimationState`。
- C# 不再需要 `traversalDuration` 常量、`traversalNotifies` 数组、
  `traversalNotifyIndex` 或 `MotionWarpTarget[]` 窗口；`State.Duration`/`State.Time`
  /`State.Completed`/`State.Notifies` 都由原生提供。
- `BuildTraversalTargets` 从 `Montage(id).Windows` 取窗口 EndTime，再用
  `CurveValue(montage.ClipId, "distance_from_ledge", endTime)` 取两个值算 `dOut`
  （`ClipId` 是 loader 从 sidecar `sequence` 派生的诊断字段，不是 Montage JSON 的
  独立字段）；任一曲线缺失时捕获 `ArgumentException` 并**不得内联回退**，
  直接 `AbortTraversal("missing-curve")` 并打印缺的 clip/曲线名。这样资产缺失是
  显式失败，不会悄悄退回旧常数。

### 5.6 warp 窗口：Montage 静态 + C# 动态

```text
Montage JSON 静态：target=FrontLedge, start/end, provider=Bone, bone=attach,
                   subtractRemaining=false, ignoreZ=false, ...
C# 动态：targets = { FrontLedge: world, BackLedge: world, BackFloor: world }
原生：为每个静态窗口建 MotionWarpWindow(targetName, targetLocation=targets[name], ...)
      MotionWarpRuntime.Apply 仍只做平移 SkewWarp
```

**为什么不在 `MotionWarpWindow` 加 `curveName`**：

- UE 的 modifier 接收的是 `Location`；GASP 在 Blueprint `SetWarpTargets` 里做
  `BackLedgeLocation + BackLedgeNormal * |d_ledge - d_floor|`（`UE-EVIDENCE.md:42-51`）。
  `curveName` 字段会把“两条曲线、两个窗口、一个方向”的 gameplay 公式塞进通用
  warp 结构，无法表达成单一曲线/单一轴。
- 曲线不在 Montage 里复制，也不在 `MotionWarpWindow` 里引用；C# 用
  `CurveValue(montage.ClipId, "distance_from_ledge", window.EndTime)` 直接取任意时刻值，
  再算 `dOut` 并提交 target 位置。这与 UE Blueprint 的 “GetCurveValueFromAnimation +
  SetWarpTarget” 同形。
- **第一切片是 C# 采样**（catalog 完整 `floatCurveKeys` + 线性 clamp），原因是桥没有
  同步 native 查询（§5.5）；代价是 C# 有一份线性插值实现。缓解：
  - 原生 `SampleFloatCurve` 保持为参考实现；
  - C#/原生 golden 测试（§7.1/§7.2）固定同一组 `distance_from_ledge` 采样值；
  - 采样规范（线性/clamp/秒/缺曲线抛错）写死在 §5.5 表里，两边都按它实现。
- **原生同步查询（新 ABI）列为后续可选**：等桥具备同步 C#→native 调用后再把
  `CurveValue` 的服务端移回原生；第一切片不做，也不为它改 ABI。
- 原生“窗口按曲线名算 target”仍不做：等于把 `SetWarpTargets` 的 BP 逻辑编码进
  modifier，超出 UE 语义。

### 5.7 notify 时间取代均分近似

- 数据：sequence sidecar 的 `notifies` 是真实时间（例如 Walk 侧车 `Footstep Left`
  的 `0.0726406`；Hurdle 以实际导出的 sidecar 为准），不是
  `duration*(i+1)/(n+1)`。
- 执行：`SkeletalPlayback::Tick` 只负责“本 tick 是否跨过 notify 时间”，
  发布 `firedNotifies`；C# 在 `Update` 里消费 `State.Notifies`，日志/后续事件用
  `notify.Time`（资产时间），不用 `State.Time` 反推。
- 第一切片只执行 `duration == 0` 的序列 notify（Footstep/Handstep）；`notifyState`
  （`PoseSearchBranchIn`、`BlendOut`）和 Montage 的 MotionWarping notifyState 不
  回调 C#——warp 窗口由原生按时间消费，不需要事件。
- 循环/seek：Montage 第一切片非循环；循环 clip 的跨回绕 notify 明确不做，避免双发。
- 反馈时序：现有 `source/script/scriptRuntime.cpp:507-512` 在
  `world->TickScriptAnimations`（`:529`）**之前**把 `animationFeedback` 交给 C#，
  所以 `AnimationState`（含新的 `Notifies`）是上一动画 tick 的快照；notify 会晚一个
  脚本 tick 到达。这是既有反馈延迟，本计划不引入第二个 notify 队列，也不补发。

### 5.8 所有权表

| 数据/行为 | 所有者 | 落点 |
| --- | --- | --- |
| Sequence 曲线/notify 解析、校验 | 原生 | `animationSequenceData.*` + loader |
| 曲线键发布 | 原生 | `ScriptAnimationAssets().curves`（一次性 catalog） |
| 曲线采样（第一切片） | C# | `CurveValue` 线性 clamp；原生 `SampleFloatCurve` 为参考实现 |
| Montage 解析/校验 | 原生 | `animationMontage.*` + loader |
| 窗口时间/顺序/provider/静态布尔 | Montage 资产 | `AM_*.json` |
| 动态 target 世界位置 | C# | `PlayMontage(..., targets)`；`dOut` 由 `CurveValue` 算 |
| 窗口门控、target 换算、SkewWarp 平移、根位移 | 原生 | `MotionWarpRuntime`（现状不变） |
| 播放时钟 | 原生 | `SkeletalPlayback` |
| notify 跨越检测 | 原生 | `SkeletalPlayback::Tick` |
| notify 业务反应/日志 | C# | `State.Notifies` |
| section 跳转/link/混合 | Slice 2 | 数据已保留；第一切片播放请求给诊断 |
| 非 Linear easing | Slice 2 | 数据已保留；第一切片播放请求给诊断 |
| `Static` provider / 旋转 warp | Slice 3 | 字段已保留；第一/二切片不执行 |
| additive / sync marker | 只保留数据 | 第一切片不执行 |

## 6. 身份、缓存、热重载、打包

### 6.1 身份

- **Skeleton**：SK 文件 BLAKE3-256 写入 `sourceInputs`，`SkeletonAsset::sourceDigest`
  保存；`skinning.skeleton` 路径拼进 `modelCacheKey`。Sequence 的 `skeletonBlake3`
  与 Montage 的 `slot` 校验都读这一份摘要。
- **sidecar**：`ContentHasher::HashFile` 的 BLAKE3-256 写入
  `SkeletalAsset::sourceInputs`（`meshAssetLoader.cpp:127-133` 的现有模式），
  同时 `Clip::sequence.sourceDigest` 保存同一摘要。
- **Montage**：文件 BLAKE3 写入 `sourceInputs`，`Montage::sourceDigest` 保存。
  Montage 的 `sequence` 路径必须是同一 SM 已加载的 sidecar；`sequenceBlake3`
  （若给）必须与 sidecar 摘要一致；loader 从 sidecar 的 `sequence` 字段派生 clip
  id，并确认它在 SM `skinning.clips[]` 中。这样 Montage 身份绑定“自己的字节 +
  引用哪一份 Sequence”，且没有第二份 clip id 权威字段。
- **`modelCacheKey`**（`meshAssetValidator.cpp:248-275`）追加
  `|skeleton=<path>`、`|sequence=<id>:<path>`、`|montage=<path>`；loader 再追加内容摘要段
  `|skeletonBlake3=…`、`|sequenceBlake3=<id>:…`、`|montageBlake3=<id>:…`；与现有 FBX clip
  一样是路径级 key，不是内容 hash。内容变化在同一进程内的缓存失效仍依赖 World
  重载（见 §6.3）。
- **Motion Matching 数据库**：`motionRuntimeDatabase.cpp:97-105` 只要求数据库
  自己的输入逐项出现在 `asset.sourceInputs` 中；新增 skeleton/sidecar/montage 不会
  破坏现有绑定（它只做子集匹配），也不会把曲线算进特征。
- **运行时身份**：`playbackIdentity`（pose/时钟代际）不因 skeleton/sidecar/montage
  改变；Montage 变更只改窗口/notify 与 `sequence` 绑定，不改姿态采样。

### 6.2 体积

| 资产 | 实测/估算 | 说明 |
| --- | --- | --- |
| Skeleton SK JSON | 约 18–25 KB（估算） | 91 骨 × 16 float + 名称/slots/curves；见下 |
| Walk FBX | 1,678,800 B | 不变 |
| Hurdle FBX | 1,008,544 B | 不变 |
| Walk 审计侧车 | 592,248 B | 现有，运行时不用 |
| Walk 运行时 sidecar | 30,347 B | 头+curves+notifies（转成新 schema 后同量级） |
| Hurdle 运行时 sidecar | 约 15–25 KB（估算） | 8–10 条曲线 × 50–100 keys + notifies |
| Hurdle Montage JSON | 约 4–8 KB（估算） | 4 窗口 + sections；T3D 17,586 B |
| catalog `curves` | 17 KB/5 条曲线（Walk 实测 `floatCurveKeys`） | 一次性发布；22 个 clip 约 400–600 KB，不逐帧 |
| 全 JSON 骨骼 | 0.54–1.28 MB/片段（估算） | 不采用 |
| 烘焙姿态 | 0.43 MB/4 s/88 骨（原始） | 不采用 |

22 个 clip 的 sidecar 合计估算 400–600 KB（locomotion 5 条曲线、traversal 8–10 条），
相比现有 22 个 FBX 的实测 ~29.6 MB 可忽略。SK 的体积：91 骨 × 16 float = 1,456 个
数值，按 9 位有效数字 + 名称/括号估约 18–25 KB；比单个 FBX（1–1.7 MB）小两个数量级。

### 6.3 热重载

当前没有动画资产文件监视；`source/shader/reload/shaderFileMonitor.*` 只服务
shader，World/M_ 重载是既有路径。第一切片：

- 改 SK/sidecar/Montage 后需要重新加载 World（或重启）；`sourceInputs` 的“导入后
  重哈希”会拒绝导入期间被替换的源，但不会在运行中自动重载。
- 不在第一切片新增 watcher；热重载是后续项。编辑器预览重新打开模型即可重建
  `SkeletalAsset`。
- 同一路径内容变化而进程不重载，`modelCacheKey` 不会变；这是现有 FBX 也存在的
  限制，不在本计划里伪装成已解决。

### 6.4 打包导出

- Skeleton 放在 `Maps/SC_gasp/Skeletons/`：SM JSON 的 `skinning.skeleton` 字符串以
  `Maps/` 开头，`gameExporter.cpp:146-157` 的 `resourcePrefix` 分支会 `Asset()`
  复制并继续 `Scan`；SK 的 `source.fbx` 同样以 `Maps/` 开头，会一并复制。
- sidecar 放在 `Maps/SC_gasp/Source/`：`gameExporter.cpp:118-122` 对
  `modelDataPath` 的父目录做 `Tree`，整目录复制，无需改导出器。
- Montage 放在 `Maps/SC_gasp/Animations/`：SM JSON 里的字符串以 `Maps/` 开头，
  `gameExporter.cpp:146-157` 的 `resourcePrefix` 分支会 `Asset()` 复制并继续
  `Scan`；无需新字段白名单。
- 显式 `skeleton`/`sequence`/`montages` 路径同时让“SK/sidecar 不在 Source 目录”时
  仍能被复制；这是不采用“文件名自动发现”的另一个理由。
- manifest 会记录 SK/sidecar/Montage 的 BLAKE3（`gameExporter.cpp:285-298`），
  包内身份可核。

## 7. 验证与测试

### 7.1 模块测试（GoogleTest，根 helper 注册）

新增 `tool/animation-tests/animationSkeletonTests.cpp` 与
`tool/animation-tests/animationSequenceDataTests.cpp`，并在
`tool/animation-tests/CMakeLists.txt` 注册；扩展 `skeletalImportTests.cpp`。
最小用例：

0. **Skeleton 解析/权威校验**
   - 合法 SK：root/joints 拓扑序、row-major `restLocal`、`DefaultSlot`、`Float` 曲线
     注册表读入；virtualBones/additiveReferencePose 原样保留。
   - 拒绝：root 不唯一、parent 越界/成环、joint 重名、restLocal 非有限/末行非
     `(0,0,0,1)`/shear/反射、slot 空/重复、曲线类型非 `Float`/重名、source.fbx 缺失。
   - **FBX 与 SK 不一致拒绝**：关节数/顺序/名称/父子/restLocal（`1e-5`）任一不符。
   - `skinning.skeleton` 缺失、SK 摘要变化、`skinning.root` 与 SK root 不符 → 拒绝。
1. **Sequence 解析/校验**
   - 合法 sidecar：duration/帧率/numFrames 一致；曲线/notify 读入。
   - duration 与 FBX 不符、`numFrames/frameRate` 不符 → 拒绝。
   - `skeleton`/`skeletonBlake3` 与 SK 不符 → 拒绝。
   - 曲线名未在 SK `curves` 注册 → 拒绝并带 clip/curve 诊断。
   - 曲线名大小写重复、times 非递增、NaN、空 keys → 拒绝。
   - notify 负时间/超 duration/负 duration → 拒绝。
   - additive 已知值保留、不拒绝；执行层不播放。
   - 缺 sidecar → `Clip::sequence` 为空，导入仍成功（可选性）。
2. **曲线采样**
   - 精确键、键间中点线性、首键前 clamp、末键后 clamp、单键、缺失名。
   - `Phase` 式区间外键被原样保留且采样 clamp。
3. **Montage 校验/round-trip**
   - 合法（GASP）：单 section、窗口在区间内、Bone `attach` 存在、provider None。
   - 拒绝：`sequence` 路径不存在、sidecar 的 `sequence` 字段派生出的 clip id 不在
     SM `skinning.clips[]`、`sequenceBlake3` 不匹配、`slot` 未在 SK `slots` 注册、
     解析出的 Sequence 与当前 SM 不同 skeleton、section 越界、Bone 不存在、
     未知 provider/easing 枚举、notify/窗口越界、duration=0。
   - **保留**：多 section + `nextSection`/`nextTime`、`Static` provider、非
     `Linear` easing、additive 值都写入并读出，不因第一切片不执行而丢弃。
   - 执行入口：带多 section/`Static`/非 Linear 的 Montage 调 `PlayMontage` 时
     返回诊断并保持当前播放器不变；不是导入失败。
   - notify 顺序按数组保留（Hurdle 的 0/3/2/1 对象名顺序不能被排序改写）。
4. **播放/notify**
   - `SetPlaybackRange(2.75)` 后 `Duration()==2.75`，`Completed` 在 2.75；
     `RootMotionRemaining` 用 2.75 而非 clip 3.333。
   - tick 0.2→0.3 跨过 0.25 notify 只发一次；Seek 后不补发；
     循环回绕第一切片不发。
5. **catalog/身份**
   - SM JSON 的 `skeleton`/`sequence`/`montages` 进入 `modelCacheKey` 和
     `sourceInputs`；改 SK/sidecar 字节 → 摘要变化；缺文件 → 导入失败。
   - Montage `sequenceBlake3` 与 sidecar 摘要一致才通过；不一致拒绝；
     `slot` 未注册 SK 时拒绝并给诊断。
6. **原生曲线参考**
   - 以正式 `SM_mannequin.json` + 新导出的 Hurdle sidecar 为夹具：
     `SampleFloatCurve("distance_from_ledge", 0.598017)` 与
     `artifacts/gasp-warp-hurdle/ue-hurdle-seq.json` 的 `34.098671` 比，
     `0.831335` 与 `90.908951` 比；容差先设 `0.5 cm`（线性 vs UE cubic 的
     未验证差），并输出 `dOut` 与 `56.810280` 比。

### 7.2 C# 测试

`tool/script-tests/scriptAnimationTests.cpp` 增加：

- `PlayMontage` 请求序列化：montage id、`SequencePath`、targets 对象、
  loop/rate/inPlace；不序列化任何 clip id 字段；
- 缺 target/多余 target/非有限 target 被 C# 入口拒绝；
- feedback 的 `notifies`/`montage`/`duration` 解析进 `AnimationState`；
- `CurveValue` golden（与原生/UE 同值）：正式 Hurdle sidecar 上
  `CurveValue(clip, "distance_from_ledge", 0.598017) == 34.098671 ± 0.5 cm`，
  `0.831335 == 90.908951 ± 0.5 cm`，`|dOut-56.810280| <= 1.0 cm`；
  另测键间线性、首/末 clamp、单键、大小写不敏感、缺 clip/曲线抛
  `ArgumentException`、NaN/∞ 抛 `ArgumentOutOfRangeException`；
- `Montage(id)` 返回的窗口 target/start/end 与 catalog 一致；`MontageInfo.ClipId`
  是 loader 从 sidecar `sequence` 派生的值（与 SM clips 命中），不是 Montage JSON
  字段；`CurveValue` 的曲线键来自同一 clip 的 sidecar，不在 Montage 里重复。

### 7.3 运行期场景

沿用现有 traversal 场景，不新增 runtime test flag：

```powershell
.\build\bin\main.exe --initial-scene Maps/SC_gasp/SC_gasp_traversal_auto.json
.\build\bin\main.exe --initial-scene Maps/SC_gasp/SC_gasp_traversal_climb.json
.\build\bin\main.exe --initial-scene Maps/SC_gasp/SC_gasp_traversal.json
.\build\bin\main.exe --initial-scene Maps/SC_gasp/SC_gasp_traversal_vault.json
```

检查：

- `P8_MONTAGE_START` 的窗口数来自 Montage（Hurdle 4 个）；
- `P8_MONTAGE_NOTIFY` 的 `t=` 等于 sidecar 里的资产时间（以实际导出的
  `<stem>.anim.json` 为准；不能再出现 `duration*(i+1)/(n+1)` 的均分值，
  例如 4 条 notify 时旧的 `0.333/0.667/1.0/1.333`）；
- Hurdle END 仍 grounded，落点相对 `RESULTS.md` 的 `-333..-354` 不出现数量级
  漂移；`dOut` 用曲线值复算后与 `56.810280` 一致；
- Climb 因为 Montage 的 `attach` provider 首次真正生效，落点预计会相对当前
  `-207` 变化；必须与 `UE-EVIDENCE.md` 的参考积分对照后记录，不能静默接受。

### 7.4 命令与门禁

```powershell
cmake -S . -B build -DVULKANLEARN_BUILD_ANIMATION_TOOLS=ON -DVULKANLEARN_BUILD_TESTS=ON
cmake --build build --target vl_animation_probe animation_preflight_tests -j8
.\build\bin\animation_preflight_tests --gtest_filter=AnimationSequenceData.*:SkeletalImport.*:MotionWarping.*
py -3 tool\validate-docs.py
```

不新增 `--animation-*` runtime test 标志；运行期验证沿用现有 Console/场景日志。

## 8. 分阶段切片

### 8.1 第一切片（本计划的最小可落地范围）

范围：Skeleton JSON + Sequence sidecar + AnimMontage JSON 的导入/校验/读取；
`CurveValue` 曲线读取；notify 真实时间；`PlayMontage`；C# 计算 `dOut` 并只提供
targets；**Hurdle/Climb/Mantle/Vault 四个动作一次迁完**。数据层保留多
section/link/Static/非 Linear/additive；执行层第一切片只跑单 Default/Linear/
None|Bone/ignoreZ/subtractRemaining。不做：section 跳转/混合、旋转 warp、
PoseSearch、烘焙缓存、全 JSON、热重载、虚拟骨/retargeting/additive 执行。

#### 8.1.1 引擎仓库文件清单

状态列按 2026-10-20 工作树核对；“已落地”表示文件/字段已在源码中存在，“未落地”是剩余范围。

| 文件 | 改动 | 状态 |
| --- | --- | --- |
| `source/animation/animationSkeleton.h` | 新增：`SkeletonJoint`/`SkeletonAsset`；slots/curves/保留字段 | 已落地（`c300edb`） |
| `source/animation/animationSkeleton.cpp` | 新增：SK JSON 解析、拓扑/restLocal/slots/curves 校验、FBX 与 SK 对照 | 已落地（`c300edb`） |
| `source/animation/animationSequenceData.h` | 新增：`FloatCurve`/`SequenceNotify`/`SequenceData`、`SampleFloatCurve` 声明 | 已落地（`c300edb`） |
| `source/animation/animationSequenceData.cpp` | 新增：JSON 解析、校验、线性 clamp 采样 | 已落地（`c300edb`） |
| `source/animation/animationMontage.h` | 新增：`MontageSection`/`MontageWarp`/`MontageNotify`/`MontageWindow`/`Montage`；`sequencePath`/`sequenceDigest`/`derivedClipId`；无曲线字段 | 已落地（`72423ca`） |
| `source/animation/animationMontage.cpp` | 新增：JSON 解析、`sequence` 路径/摘要/slot ∈ SK/骨名/sections/窗口校验；**不做窗口曲线预采样** | 已落地（`72423ca`） |
| `source/animation/animationAssets.h` | `Clip` 增加 `SequenceData sequence` | 已落地（`c300edb`） |
| `source/animation/CMakeLists.txt` | 加入三个新 `.cpp` | 已落地 |
| `source/animation/skeletalPlayback.h` | `SkeletalAsset::skeleton/montages`；`SetPlaybackRange`；`FiredNotifies`；`SetMontageSections` | 已落地：`skeleton`/`montages`（`c300edb`、`72423ca`）、range/notify API（`0356b7f`）、catalog 与 C# 消费（`e5a92f6`）、section 链（`a74e29f`） |
| `source/animation/skeletalPlayback.cpp` | `Duration/RootMotionTotal/Remaining` 用 range；notify 跨越；section 推进；旋转 yaw 通道 | 已落地（`0356b7f`、`b3a5e79`、`a74e29f`、`36de338`）：`NotifyPlayback.*` 11 项 + section 用例 |
| `source/mesh/meshAssetTypes.h` | build plan 增加 skeleton/clip sequence/montage 字段 | 已落地（`c300edb`、`72423ca`） |
| `source/mesh/validation/meshAssetValidator.cpp` | 强制 `skinning.skeleton`、拒绝 `skinning.root`、`skinning.clips[].sequence`、`skinning.montages` 校验；`modelCacheKey` | 已落地（`c300edb`、`72423ca`、`0de7bcc`） |
| `source/mesh/loader/common/meshAssetLoader.cpp` | 加载/哈希 SK、FBX↔SK 对照、加载 sidecar/Montage；`sourceInputs` | 已落地（`c300edb`、`72423ca`、`0de7bcc`） |
| `source/world/world.h` | 声明 `ScriptAnimationAssets()`（若需要独立 catalog） | 已落地（`e5a92f6`） |
| `source/world/worldAnimation.cpp` | catalog 增加 montages + `curves`（完整 `floatCurveKeys`）；`playMontage` 请求；feedback `notifies`/`montage`；拒绝 clip 请求的 `warpTargets` | 已落地（`e5a92f6`、`0de7bcc`） |
| `source/script/scriptRuntime.cpp` | context 创建/`animation_catalog` 消息带 `animationAssets`（含 curves） | 已落地（`e5a92f6`） |
| `script/VulkanLearn.Runtime/AnimationPlayback.cs` | `PlayMontage`、`MontageInfo`、`AnimationNotify`、`CurveValue`（catalog 键缓存 + 线性 clamp）、feedback 解析 | 已落地（`e5a92f6`）；`0de7bcc` 删除 `MotionWarpTarget`/`Play(warpTargets)` |
| `tool/animation-tests/animationSkeletonTests.cpp` | 新增：SK 解析/权威校验/round-trip | 已落地（17 项） |
| `tool/animation-tests/animationSequenceDataTests.cpp` | 新增：Sequence 校验、采样、notify 测试 | 已落地（14 项） |
| `tool/animation-tests/animationMontageTests.cpp` | 新增：Montage 解析、跨文件绑定、缓存键、`rotationMethod` | 已落地（24 项） |
| `tool/animation-tests/animationScriptDataTests.cpp` | 新增：catalog/feedback/窗口装配/`PlayMontage` 拒绝矩阵、旋转/Static/section link/入口 startTime | 已落地（30 项） |
| `tool/animation-tests/notifyStateTests.cpp` | 新增：notify/notifyState 区间执行 | 已落地（11 项） |
| `tool/animation-tests/skeletalImportTests.cpp` | sidecar/Montage 导入/round-trip 回归 | 已落地（30 项） |
| `tool/animation-tests/CMakeLists.txt` | 注册新测试文件 | 已落地 |
| `tool/script-tests/scriptAnimationTests.cpp` | `PlayMontage`/`CurveValue`/notify feedback/拒绝/旋转/startTime 用例 | 已落地（`ScriptAnimationTest.*` 36 + `ScriptAnimationWorld.*` 11） |

#### 8.1.2 工具文件清单

实际落地的导出链是 `export_gasp_ue_raw.py`（UE 只读 raw dump）→
`export_gasp_skeleton.py` + `export_gasp_sequences.py` → `import_gasp_montages.py`；
计划里的 `audit_gasp_traversal.py` 扩展没有采用（该脚本保持审计用途）。

| 文件 | 改动 | 状态 |
| --- | --- | --- |
| `tool/animation/export_gasp_ue_raw.py` | 新增（计划外）：在隔离的 UE 工程副本里只读导出 skeleton/curves/notifies 的 `vl.gasp.ue-animation-raw.v1` 中间 dump | 已落地 |
| `tool/animation/export_gasp_skeleton.py` | 新增：读 raw dump 生成 `SK_*.json`；写 root/joints/restLocal/slots/`curves` 并集/source；拒绝覆盖 | 已落地（SK 35457 B、13 条曲线） |
| `tool/animation/export_gasp_sequences.py` | 新增：读 raw dump + SK + SM clip 清单，写 22 个 `<stem>.anim.json`；校验 Hurdle `dOut=56.810280`（0.5 cm 容差）；notifyState 原值保留；拒绝覆盖 | 已落地（22 个 sidecar） |
| `tool/animation/audit_gasp_traversal.py` | 原计划扩展它导出 sidecar；实际未改，保持审计用途 | 未采用 |
| `tool/animation/import_gasp_montages.py` | 新增：解析 `artifacts/gasp-warp-audit/montages/*.copy`，写 `AM_*.json`；写入 `sequence` 路径/`sequenceBlake3`；clip id 由 loader 从 sidecar 派生，**不写进 Montage**；保留多 section/link/`staticPose`/非 Linear；拒绝覆盖；只拒绝无法无损表达的数据 | 已落地（10 个 Montage） |
| `tool/animation/README.md` | 记录三条导出/转换命令（SK/Sequence/Montage）与输入目录要求 | 未落地（README 还没有这三套命令） |
| `tool/animation/gaspImportReport.cpp`（可选） | 复用新校验器，报告 curve/notify 数量与采样值，便于对照 | 未落地（仍只读审计侧车） |

#### 8.1.3 资源仓库配套（单独提交，不在本仓库）

- 已落地（资源 `ebd7059` + `aa78096` + `ac0fe0c`）：
  - `Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json`：91 物理骨 + 2 虚拟骨、
    13 条曲线注册、`DefaultSlot`，35457 B。
  - `Maps/SC_gasp/Source/M_*.anim.json`：22 个 sidecar，130 条曲线、11078 个键、
    142 条 notify，共 552209 B。
  - `Maps/SC_gasp/Animations/AM_*.json`：10 个 traversal Montage，10 个 section、
    46 条 notify、21 个 warp 窗口，共 21096 B；每个带 `sequenceBlake3`，
    `slot=DefaultSlot`，不写 clip id。
  - `Maps/SC_gasp/Meshes/SM_mannequin.json`：`skinning.skeleton`；22 个 clip 各加
    `sequence`；10 个 `montages`；`skinning.root` 已删除（`ac0fe0c`）。
  - `Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs`：
    - `SetTraversalMontage` 只保留 `AM_` + clip id，手写 notify 名数组已删除；
    - `BuildTraversalTargets` 从 `animation.Montage(id).Windows` 取 target 名，用
      `animation.CurveValue(montage.ClipId, "distance_from_ledge", endTime)` 算 `dOut`；
    - `animation.PlayMontage(traversalMontage, targets, false, 1, true)`；
    - 时间/notify 用 `animation.State.Duration/Time/Completed/Notifies/MontageRange`；
    - Climb+Walk 竖直闭环从首个真实窗口 Start 开始。

#### 8.1.4 测试与验收

已落地（2026-10-21，`animation_preflight_tests` 255 项）：

- `AnimationSkeleton.*`：17 项，SK 解析/权威校验/round-trip/FBX 对照/root 字段拒绝。
- `AnimationSequenceData.*`：14 项，Sequence 解析/校验/采样/notify 保留。
- `AnimationMontage.*`：24 项，Montage 解析、跨文件绑定、重复 id、缓存键、`rotationMethod`。
- `AnimationScriptData.*`：30 项，catalog/feedback/窗口装配/`PlayMontage` 拒绝矩阵、
  目标旋转/Static pose、section link（Absolute/Relative/Ratio、边界/空洞/回跳）、
  入口 `startTime`。
- `NotifyPlayback.*`：11 项，瞬时/区间/越界/Stop/循环/Seek/范围。
- `SkeletalImport.*`：30 项；含 `sourceInputs`、`skinning.skeleton` 强制与缓存键。
- `MotionWarping.*`：28 项；平移 + 旋转（90°/180°、倍率、门控、独立通道、
  subtractRemaining、非 Slerp 跳过）+ Static/Bone warp point。
- `scriptAnimationTests`：`ScriptAnimationTest.*` 36 + `ScriptAnimationWorld.*` 11；
  含旋转/`WarpTarget` 重载、`startTime` 3 项；
  `PlayMontageRejectsUnsupportedExecutionWithDiagnostics` 为 6 项拒绝。
- 运行期：P22 清理后矩阵 10 个 traversal 场景 ×2 全部 grounded、无 ABORT；
  P23 旋转接线 4 场景 ×2 全部 grounded、END yaw 0.0/x=0，+20° 诊断 END yaw 20.0；
  P24/P25 Catch：Hurdle/Cliff/Mantle/Vault + Falling 自动触发，入口 `startTime=0.877`
  （Hurdle high），落地 y=290；P26 家族 Chooser（纵深 2/3 个胶囊直径）、跳跃/走落
  gating、GASP 关卡视觉与 branch-in 采样窗口研究：入口 `startTime` = UE 采样范围下界
  （Catch 窗口≈1 帧，差 ≤1 帧；非 Catch 窗口 2–31 帧）。
- 文档：数据层、播放与迁移已写入 `contracts/animation-asset-import.md`、
  `contracts/animation-montage.md`、`contracts/animation-motion-warping.md`；
  验收与对照见 [P22 Montage 迁移记录](../../rationale/gasp-p22-montage-migration-record.md)、
  [P23 旋转/section/Static 执行记录](../../rationale/gasp-p23-warp-execution-record.md)、
  [P25 Catch 入口记录](../../rationale/gasp-p25-catch-entry-record.md) 与
  [P26 家族/研究记录](../../rationale/gasp-p26-chooser-families-record.md)。

剩余（§8.2 方向）：

- 非 `Slerp` 旋转方法（`SlerpWithClampedRate`/`ConstantRate`/`Scale`）与
  `RotationType`/`AdditionalOffset`/`MaxRotationRate`；目标旋转的 pitch/roll；
- Montage 混合/blend-out、section 内 rate/offset、非 `Linear` easing；
- 原生 PoseSearch 选帧（非 Catch 家族 B1；Catch 差 ≤1 帧、无收益）；
- 动画资产热重载/文件监视、发行包烘焙缓存；
- 原生同步 `CurveValue` 查询/新 ABI（当前 catalog + C# 采样）；
- 30/60/120 Hz 场景矩阵（现有缺口）。

#### 8.1.5 第一切片不执行（不是不保留）

- 二进制烘焙缓存、全 JSON 骨骼轨道、FBX 容器扩展；
- SK 的 `virtualBones` 重建、retargeting、additive 参考姿态执行：数据保留，第一切片
  不执行；SK 反向驱动 FBX 导入（SK 成为唯一 skeleton 源）也留到后续；
- Montage 混合/blend-out、section 内 rate/offset、多 slot track：数据保留或 schema 外，
  当前不做；
- 非 `Linear` easing：数据保留，播放请求给诊断并拒绝；
- 非 `Slerp` 旋转方法、`RotationType`/`AdditionalOffset`/`MaxRotationRate`、目标旋转
  pitch/roll：数据保留，执行拒绝或不消费；
- PoseSearch/Chooser；
- additive、sync marker 执行：数据保留，不执行（Sequence/Montage notify 与
  notifyState 执行已落地，见 `contracts/animation-montage.md`）；
- 动画资产热重载/文件监视；
- 原生同步 `CurveValue` 查询/新 ABI：当前用 catalog + C# 采样；
- 30/60/120 Hz 场景矩阵（现有缺口，不在本切片补）。

### 8.2 后续切片（仅方向，不展开）

1. **Slice 2**：Montage 混合、section 内 rate/offset、非 `Linear` easing；
   用 UE 参考根轨迹验证 `SlotAnimTracks` 的 retiming。
2. **Slice 3**：非 `Slerp` 旋转方法（`SlerpWithClampedRate`/`ConstantRate`/`Scale`）、
   `RotationType`/`AdditionalOffset`/`MaxRotationRate`、目标旋转 pitch/roll。
3. **Slice 4**：SK 成为唯一 skeleton 源（FBX 只供几何/权重）；虚拟骨重建、
   retargeting、additive 参考姿态执行。
4. **Slice 5**：动画资产热重载、编辑器时间轴/曲线预览。
5. **Slice 6**（可选，取决于桥演进）：原生同步 `CurveValue` 查询/新 ABI，把曲线
   服务端移回原生；现有 `CurveValue` 签名保持不变。
6. **Slice 7**（可选）：发行包烘焙缓存；sidecar 仍是源，缓存是生成物；
   用 BLAKE3 绑定源。
7. **Slice 8**：Catch 家族、入口时间与 gating 已落地（P24–P26）；剩余原生 PoseSearch
   选帧（非 Catch B1）与其它新动作族。

## 9. 风险与未决问题

### 9.1 风险

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| Traversal 完整曲线键尚未导出 | Hurdle `dOut` 无法从资产复算，第一切片的核心验收缺输入 | 先在 UE 5.8.2 项目副本跑扩展后的 `audit_gasp_traversal.py`；把 `<stem>.anim.json` 纳入同一资源 PR |
| SK 与 FBX 生成源漂移 | 关节名/父子/rest 不一致会让整个 skeletal mesh 导入失败 | SK 由 `export_gasp_skeleton.py` 从同一 FBX 生成并提交；导入 strict 对照（`1e-5`）；改 FBX 必须重生成 SK |
| SK `curves` 注册表漏项 | 对应 clip 的 sidecar 被拒绝，整段动画无法加载 | 生成器取 22 个 clip 曲线名并集；导入诊断带 clip/curve；新增曲线先注册 SK |
| 旧资源没有 SK | `skinning.skeleton` 变为必需后旧场景直接失败 | 资源 PR 与引擎 PR 同步补 SK；或按 §9.2 决策保留一个过渡期（本计划推荐同 PR） |
| SK restLocal 布局/精度 | row-major/column-major 误读会让绑定姿态整体错位 | schema 固定 row-major 4×4、平移 `[3],[7],[11]`；导入转 Eigen；与 FBX 对照 `1e-5`；生成器写 9 位有效数字 |
| UE 曲线可能是 cubic，VL 只做线性 | C# `CurveValue` 在任意时刻与 UE 的 `GetCurveValueFromAnimation` 有未知差 | 用 `0.598017`/`0.831335` 的 UE 参考值与 `0.5 cm` 容差验证；若超差，下一版导出 tangent/interp（T3D 有 `LeaveTangent`）或改用逐键 cubic；不直接用 UE 采样值内联 |
| C# 与原生两份线性采样实现漂移 | `CurveValue` 与 `SampleFloatCurve` 对同一 sidecar 给出不同值，warp 目标不可复现 | 采样规范写死（线性/clamp/秒/缺曲线抛错）；同一 golden 在 `AnimationSequenceData.*` 与 `scriptAnimationTests` 各跑一遍；第一切片只允许一份规范实现 |
| 桥无同步 native 查询 | `CurveValue` 只能由 catalog 键在 C# 采样；完整键 payload 变大 | catalog 只发布有 sidecar 的 clip 的 `floatCurveKeys`（实测 17 KB/5 条曲线，22 个 clip 约 400–600 KB 一次性）；不逐帧发布；原生同步 ABI 列为后续可选 |
| Montage 的 `SlotAnimTracks` 可能带 rate/offset，T3D 未给全 | Climb/Vault 的根轨迹与 UE 不一致；当前 C# 的“从 0 播到 montage 长度”只是近似 | 第一切片显式假设 rate=1、start=0、只截断；用 `UE-EVIDENCE` 的根轨迹对照，超差则列为已知限制，不宣称 UE 等价 |
| Climb 首次应用 `attach` provider | 现落点 `-207` 会变化，可能被误判回归 | 把变化写进验收；对照 `UE-EVIDENCE.md` 的参考积分；不静默删 provider |
| Montage modifier 只能从 T3D 读 | 没有稳定的 UE Python 导出链；T3D 文件在 `artifacts/`（gitignored） | 转换器的输入 `.copy` 留在 `artifacts/` 或 `Generated/Import/`，不要放 `Source/`（会被 game-export 打进包）；转换器记录输入 BLAKE3 |
| 曲线名大小写不一致（文档 `Distance_From_Ledge` vs 资产 `distance_from_ledge`） | C#/schema 写错大小写会找不到曲线 | 查找大小写不敏感；导入时按大小写不敏感拒绝重名；schema 保留原拼写 |
| notify 顺序/重叠窗口 | Hurdle 4 窗口有重叠；排序会改变“最后生效窗口” | JSON 数组顺序 = UE `Notifies` 顺序；runtime 不排序；单测固定 Hurdle 顺序 |
| `Duration()` 改为 Montage range | 影响 `RootMotionRemaining`、`subtractRemaining`、预览、MM | 只在 Montage range 非零时改；补 `RootMotionRemaining`/`Completed` 测试；非 Montage 路径逐位不变 |
| 同路径内容变化不触发热重载 | 改 sidecar 后运行中看不到新值 | 第一切片要求 World 重载；文档明说；不把 `modelCacheKey` 说成内容 hash |
| 资源 PR 与引擎 PR 耦合 | 引擎先合入时旧资源没有 `skinning.skeleton`，skeletal mesh 导入失败 | 同一资源 PR 同步补 SK；若不能同发，只能临时允许无 SK 回退（不推荐，违背“SK 权威校验”）；sequence/montages 仍可缺省 |
| Montage 的 `sequence` 路径与 sidecar 摘要不一致 | 加载到错误/旧曲线，`dOut` 不可复现 | `sequenceBlake3` 校验；sidecar 的 `sequence` 派生 clip id 必须在同一 SM 的 `skinning.clips[]` 里 |

### 9.2 决策记录

#### 已确认 2026-10-20

1. **三层模型**：Skeleton `SK_*.json` + Sequence sidecar + Montage JSON；`Clip` =
   Sequence；**关节/槽位/曲线名只在 SK 一份，曲线值只在 Sequence 一份**；Montage
   用 Sequence 资产路径引用（可带 BLAKE3），不复制曲线/键，也不写单独的 clip id。
2. **C# 曲线 API 与 UE Blueprint 同形**：`AnimationPlayback.CurveValue(clipId,
   curveName, time)`；当前桥无同步查询，第一切片用 catalog 一次性发布
   `floatCurveKeys` + C# 线性 clamp 采样；原生同步查询/新 ABI 为后续可选。
   采样规范：秒、线性、区间外 clamp、缺 clip/曲线抛错、非有限时间抛错。
3. **第一切片四个动作一次迁完**（Hurdle/Climb/Mantle/Vault）；Climb 因 `attach`
   首次真正生效、落点会变，单独用 UE 参考积分验收并在结果里单列。
4. **分层支持**：数据层保留 sections/slot/link/easing/Static provider/additive 并可
   round-trip（GASP 单 slot；多 slot track 属于本 schema 无法无损表达，明确拒绝）；
   执行层第一切片只执行单 `Default`/`Linear`/`None|Bone`/`ignoreZ`/
   `subtractRemaining`；Slice 2 执行 section 跳转/link/next 与非 Linear easing；
   Slice 3 执行 `Static` provider 与旋转 warp；additive、sync marker 只保留数据。
   只有无法无损表达的数据（未知 provider/easing 枚举、section 越界、Bone 不存在、
   多 slot track）才拒绝。
5. **Skeleton 独立 JSON + FBX 生成**：SK 由 FBX 生成一次、提交为独立资产；导入时
   作为权威校验清单（FBX 关节名称/父子/rest 必须一致，不一致拒绝）；第一切片不让
   手写 SK 反向驱动 FBX。SK 成为唯一 skeleton 源、FBX 只供几何/权重列为 Slice 4。

#### 已确认（实施期追加，2026-10-20）

6. **Montage 引用位置**：SM `skinning.montages` 数组（资源相对 `.json` 路径）；Montage
   自身只写 `sequence` 路径 + 可选 `sequenceBlake3`，不写 clip id。
7. **`sequenceBlake3`**：可选但推荐；给了就必须与 sidecar 一致，缺失不阻止加载。
8. **T3D 输入**：只保留在 gitignored `artifacts/`，不进资源仓库；转换器输入不打包。
9. **C#/原生 golden 容差**：曲线 `0.5 cm`、`dOut` `1.0 cm`；当前实测差在 `1e-5` 量级。
10. **Climb 落点变化**：接受，按 UE 参考积分验收（已随 `a18840e` 落地）。
11. **未执行数据的运行时策略**：`PlayMontage` 对非 `Linear` easing、非 `Slerp` 旋转方法、
    不可用 `staticPose`/未知 provider/link 的 **执行**请求拒绝并保持当前播放器、给诊断；
    数据层不拒绝、原样保留。P23 后多 section 与 `Static` 已执行（C# 入口由 `8e61949` 接受两者）。
12. **native 同步查询 ABI**：第一切片不做；Slice 6 前再评估。
13. **`restLocal` 布局**：row-major 4×4（平移位于 `[3]/[7]/[11]`）；生成器与解析器统一。
14. **SK 生成顺序**：先 `export_gasp_skeleton.py` 生成 SK，再导出 sidecar 并校验曲线
    注册；资源 PR 固定此顺序。
15. **`skinning.skeleton` 强制时机**：`0de7bcc` 已改为强制并删除 `skinning.root`。
16. **notifyState 原值**：`time`/`duration` 按 UE 原值保留，允许 `time+duration` 越过
    序列/Montage 长度；运行时按播放范围 clamp、结束/中断补 End（`8857d05`、`31d99b5`）。

#### 仍需确认

- 当前无阻塞项；剩余为待验证事项，见 §9.3（Montage retiming、sync marker、sidecar
  覆盖范围、打包闭包等）。

### 9.3 待验证清单

- `audit_gasp_traversal.py` 对 traversal clip 调 `AnimationLibrary.get_float_keys`
  是否与 12 段 locomotion 一样成功；notify event 是否能取到显式 `name`
  （当前只有 `fields.ueStructText`）。
- SK 生成：`SKM_UEFN_Mannequin.fbx` 的 91 个物理骨（`VB ` 虚拟骨单列）与 SK
  `joints` 数量/名称/父子/restLocal（`1e-5`）是否逐项一致；SK 体积是否落在
  18–25 KB。
- SK `curves` 注册表：22 个 clip 的曲线名并集（含 `distance_from_ledge`、
  `contact_l/r`、`disable*pelvis/footik`、`Phase` 等）是否完整；未注册曲线在导入时
  是否给出 clip/curve 诊断而不是静默空曲线。
- `skinning.skeleton` 的 game-export 闭包：SK 路径、SK `source.fbx`、SM
  `modelDataPath` 是否都被复制，且 manifest 含 SK 的 BLAKE3。
- Hurdle `distance_from_ledge` 的完整键集；C# `CurveValue` 与原生
  `SampleFloatCurve` 在 `0.598017`/`0.831335` 的 golden 是否同时落在
  `34.098671`/`90.908951` 的 `0.5 cm` 容差内。
- catalog 完整 `floatCurveKeys` 的实际大小（按 Walk 17 KB/5 条曲线推算 22 个 clip
  约 400–600 KB）与 context 创建/`animation_catalog` 的耗时；若过大，是否改成
  “只发布有曲线的 clip”或按需请求（后者需要桥改动）。
- Montage `SlotAnimTracks` 是否隐含 rate/segment offset；`CachedPlayLength`
  （Climb stand 2.333333、Vault 2.833333）与 section 长度不一致的原因。
- `syncMarkers` 在 T3D 中的位置/格式（Python 接口可用，但当前 GASP 为 0）。
- 资源侧 `SM_mannequin.json` 的 22 个 clip 是否都需要 sidecar，还是只给
  traversal/需要曲线的 clip。
- `MotionWarpingUtilities` 对 Montage 的窗口列表是否覆盖所有 provider 类型
  （当前只验证 None/Bone；`Static` 的数据 schema 需从 T3D 补齐）。
- `game-export` 对 `Animations/` 目录的复制是否只需 SM 里的 `Maps/` 字符串
  （按 `gameExporter.cpp:146-157` 推断，需一次实际打包验证）。
- `sequenceBlake3` 在 T3D 转换器里是否总能从 sidecar 取到（若 sidecar 尚未生成，
  转换器需要明确的输入顺序）。

## 10. 参考位置

- 导入/采样现状：[动画资产导入](../../contracts/animation-asset-import.md)；
  [动画播放与蒙皮](../../contracts/animation-runtime-skinning.md)。
- warp 现状与限制：[原生 MotionWarping](../../contracts/animation-motion-warping.md)。
- 工具现状：[动画预检工具](../../contracts/animation-preflight-tools.md)；
  [工具入口](../../../tool/animation/README.md)。
- GASP 记录：[P0 资产基线](../../rationale/gasp-p0-asset-baseline-record.md)；
  [P14 Montage 运行时](../../rationale/gasp-p14-montage-runtime-record.md)；
  [P17 UE Montage 时间](../../rationale/gasp-p17-ue-montage-timing-record.md)；
  [P18 Ledge warp](../../rationale/gasp-p18-runtime-ledge-warp-record.md)；
  [P19 原生 MotionWarping](../../rationale/gasp-p19-native-motion-warping-runtime.md)。
- 证据文件（gitignored，不作为契约）：`artifacts/gasp-warp-hurdle/UE-EVIDENCE.md`、
  `artifacts/gasp-warp-hurdle/ue-hurdle-seq.json`、
  `artifacts/gasp-warp-audit/montages/AM_*.copy`、
  `artifacts/gasp-p0/ue-audit-05/M_Neutral_Walk_Loop_F.copy`。
- 文档分层规则：[documents/README](../../README.md)。
