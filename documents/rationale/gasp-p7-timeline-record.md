# GASP P7 时间轴、查询回放与独立包记录

状态：执行记录，非当前契约。P7 的时间轴、查询回放、独立包固定动作矩阵/正常退出
与生命周期回归分项已完成技术验收；无测试负载分项性能、真实转向/受阻键鼠矩阵、
多人回放与历史姿态重演列为后续增强，不阻塞本次 P7 技术验收。
接续 [P6 动态质量记录](gasp-p6-dynamic-quality-record.md)；
计划见 [GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)。

## 范围

P7 验收要求“可暂停/拖时间轴，查看状态、权重、轨迹和分项代价”。
状态、查询、候选与代价在 P5 已有冻结显示；本记录覆盖：

- 被聚焦脚本控制器的原生播放时间轴。
- 最近 600 帧 Motion 调试快照的查询回放（查询、候选、代价、轨迹）。
- 正式 GASP playground 的独立包导出与仓库外启动。

不覆盖完整 P7 的生命周期/导出性能分项、固定动作矩阵或多人同时回放。

- 时间轴显示唯一原生播放器的片段、当前时间、时长与播放状态。
- Play/Pause/Seek 只改被聚焦的 `sceneObjectIdentity`，不遍历或改写其它角色。
- Seek 先显式暂停，Motion Matching 下一帧不能把定位覆盖为继续搜索。
- 回放只选择历史快照；进入时暂停目标播放器，不重搜、不写 Actor/Jolt。
- 无控制器、离线或时长为零时禁用；拖动期间不覆盖焦点控件。
- 不维护第二份时钟，不改变动画资产、pre-IK/查询姿态或 Actor/Jolt 位移。

## 实现

- `Animation::MotionDebugView` 增加 `playbackClipId/playbackTime/playbackDuration/
  playbackPlaying`，由 `World::CaptureMotionDebug` 从被聚焦播放器填充。
- `UiActionType::ControlScriptAnimation` 携带网格身份、动作（0=seek、1=play）
  和秒值；`RuntimeCommandType::ControlScriptAnimation` 只在 GT 执行。
- `World::ControlScriptAnimation` 按身份找到唯一播放器，校验 World 代数、
  动作与片长；`seek` 执行 `SetPlaying(false)` + `Seek`，`play` 恢复。
- RmlUi Motion 面板新增 play/pause 按钮、range 与状态文本；
  绑定只读原生快照，事件经现有 typed action sink 提交。
- 面板刷新键包含播放状态，使暂停/恢复和拖动立即反映，同时保留既有 10 Hz
  候选表节流；时间轴值在同一刷新周期内更新。
- `World` 增加被聚焦组件的 600 帧 `MotionDebugView` 历史；实时捕获去重后追加，
  `SetMotionDebugReplayFrame(index)` 进入回放并暂停目标播放器，`-1` 退出。
  回放期间 `CaptureMotionDebug()` 直接返回历史副本，不再追加。
- `MotionDebugView` 增加 `replayActive/replayFrame/replayFrames`；Motion 面板
  增加 Replay 复选框、历史帧 range 与状态，经 `ControlMotionReplay` 在 GT 执行。
- 独立包使用 `vl_export_game` 收集配置/资源/shader/原生 DLL/私有 .NET 运行时；
  Motion DB/PS 通过 `--dynamic-assets` 显式列入，因为 C# 字符串不做静态分析。

## 验证

- `editor_ui_tests` 668 项（Motion 11 项）连续两遍：663 通过、
  5 项符号链接权限跳过、零失败（91.012 s/遍）；
  Motion 11 项全部通过，全量日志 `artifacts/gasp-p7-replay-editor-full.log/xml`。
  新用例 `MotionTimelineSeeksAndPlaysOnlyTheFocusedPlayback` 覆盖：
  时长/时间绑定、播放态按钮禁用、seek 动作的身份/代数/秒值、
  pause/play 动作、无播放器与离线禁用且不产生命令。
- `script_tests` 新增 `ScriptAnimationTest.ScriptTimelineControlsOnlyTheFocusedPlayback`
  连续两遍通过：只暂停/定位目标播放器，另一独立实例的时钟与播放态不变；
  错误身份、旧 World 代数、越界时间与非法动作全部拒绝；play 恢复后时间保持。
- 新增 `EditorUiTest.MotionReplaySelectsFrozenHistoryWithoutPlaybackSeek`：
  历史帧 range/复选框/状态、进入回放发送帧索引、无历史时禁用且不发命令。
  `EditorUiTest.Motion*` 11 项通过（全量 668 见证据索引）。
- 新增 `ScriptAnimationTest.MotionDebugHistoryReplaysAndDoesNotAppendWhileFrozen`：
  回放返回对应历史帧的查询/候选，回放期间不追加，退出恢复实时快照。
- 全量 C# ON `script_tests` 384 项一轮通过（1236.254 s，日志
  `artifacts/gasp-p7-full-script-04.log/xml`）；
  时间轴/回放定向 2 项连续两遍通过；
  动画 `animation_preflight_tests` 129 项连续两遍通过；
  C# OFF Release `script_tests` 79 项通过。
- Debug `main`、Development `main`、`editor_ui_tests`、`script_tests` 构建退出 0。
- 独立包：Debug `artifacts/gasp-p7-export-01` 验证闭包；Development
  `artifacts/gasp-p7-export-dev-01` 从仓库外启动，World 提交成功，
  空闲 clip=0、按 W 切到 clip=5（WalkLoop）并推进，松开回 clip=0；
  窗口 93 FPS；`CloseMainWindow` 后正常 teardown，stderr 为空。
- 固定动作矩阵探针包 `artifacts/gasp-p7-probe-export-01`（Development，
  1706 文件）从仓库外启动，连续记录 Idle→Walk→Run→JumpStart→Fall→Land→Idle、
  `yaw=180`、位置 z≈-1304、跳跃 grounded=False、落地 grounded=True、
  `P7_PROBE_FINISHED`；探针场景 451 FPS，正常 teardown，stderr 为空；
  正式 MM 探针包 `artifacts/gasp-p7-mm-probe-export-01` 另记录
  Walk→Run→Jump→Fall→Land→180°→Idle、z≈-1000、459 FPS；
  42 个场景 schema 检查通过。
- 实景采集见下节。

## 查询回放

World 为被聚焦组件保留最近 600 帧的不可变 `MotionDebugView`：
查询/归一化查询、候选与分项代价、轨迹线、搜索次数、决策和播放状态。
实时捕获按 `componentIdentity` 切换清空，重复帧去重后追加；进入 Replay 时
`SetMotionDebugReplayFrame(index)` 暂停目标播放器，面板只读历史副本，
不再追加。退出 Replay 返回实时快照；回放不重新搜索、不写 Actor/Jolt。

限制：回放只重演**查询/候选/轨迹/代价**，角色姿态保持暂停时刻，不按历史
候选重演骨骼姿态；时间轴播放器的 seek 与查询回放是两个独立控件。
多角色同时回放未实现，一次只保留一个被聚焦组件的历史。

## 独立包启动

Debug 包 `artifacts/gasp-p7-export-01`（1878 文件、1.5 GB）先验证了导出闭包；
随后用 `--binary-directory build-vscode/development/bin` 导出
`artifacts/gasp-p7-export-dev-01`（1876 文件、1.577 GB），从仓库外
`C:\Users\Public\gasp-p7-run-dev` 直接启动：

- World/graph transaction committed，场景指向包内 `resources/Maps/SC_gasp/SC_gasp_playground.json`；
- `animation get` 空闲为 clip=0/time 推进/duration=10 s/playing=1；
  按下 W 后变为 clip=5/duration=3.467 s 的 WalkLoop，时间持续推进，
  松开后回到 clip=0；证明实际输入进入原生 MM/播放器；
- 窗口 93 FPS（同轮 C# 全量测试占 CPU，仍非隔离性能分项）；
- `CloseMainWindow` 后日志出现 `Quit event received` 与
  `EngineLoop teardown complete`，stderr 为空。

限制：相机是场景固定相机，未独立记录世界位移；动作矩阵只做了 W 走循环开/停。
`artifacts/gasp-p7-export-dev-02` 另验证了按 identity 的 `animation get`
（`__blueprint/player/visual` 与模板各一条），窗口 108 FPS 并正常 teardown；
键盘注入在无焦点窗口下不稳定，因此没有继续依赖键盘扩展动作矩阵。

### 固定动作矩阵探针

为摆脱键盘焦点依赖，新增资源侧
`Maps/SC_gasp/Blueprints/PackageProbe.blueprint.cs`、`BP_gasp_package_probe.json`
和 `SC_gasp_package_probe.json`：固定时序驱动 Idle(1 s)→Walk(2 s)→Run(2 s)
→JumpStart→Fall→Land→Idle，并在 7.2 s 把 Actor yaw 设为 180°。
探针只做包验收，不替代正式 `GaspCharacter` 的输入/状态机路径。

仓库内 Development 运行和独立包 `artifacts/gasp-p7-probe-export-01`
（1706 文件，Development 二进制）均记录到同一序列：

```text
P7_PROBE_CLIP M_Neutral_Stand_Idle_Loop
P7_PROBE_CLIP M_Neutral_Walk_Loop_F
P7_PROBE_CLIP M_Neutral_Run_Loop_F
P7_PROBE_CLIP M_Neutral_Jump_F_Start_Stand_Lfoot
P7_PROBE_CLIP M_Neutral_Jump_Loop_Fall
P7_PROBE_CLIP M_Neutral_Jump_F_Land_Stand_Light_Lfoot
P7_PROBE_CLIP M_Neutral_Stand_Idle_Loop
P7_PROBE_FINISHED
```

包内位置从 `z=0` 推进到 `z≈-1304`，跳跃时 `grounded=False`、y≈113，
落地后回到 y=90；结束时 `yaw=180`、`grounded=True`。探针场景 451 FPS，
`CloseMainWindow` 后正常 teardown，stderr 为空。当前 42 个场景 schema 检查通过。

该显式片段矩阵不经过正式 MM 状态机，且没有程序集重载/World 换代；
正式 playground 的分项性能仍需单独验收。

### 正式 GaspCharacter MM 探针

为覆盖正式状态机/MM 路径，在 `CharacterBlueprint` 增加默认关闭的
`PackageProbe` 实例属性；测试 BP `BP_gasp_mm_probe.json` 开启它并开启
`MatchSpeed`，场景 `SC_gasp_mm_probe.json` 使用正式胶囊、相机、Jolt 与
`DB_gasp.json`。开启后固定时序替代 Move/Sprint/Jump 输入，默认路径不变。

仓库内 Development 与独立包 `artifacts/gasp-p7-mm-probe-export-01`
（1706 文件）记录到正式 MM/状态机序列：

```text
Idle → Walk (WalkLoop/Start/Stop MM 选帧) → Run (Run_Loop_F)
→ Jump → Fall → Land → 180° 转向 → Idle
```

包内读数：Walk 以 200 cm/s 从 z=0 走到 z≈-407；Run 以 450 cm/s
到 z≈-1300；跳跃 y=98→213、grounded=False；落地 y=90、grounded=True；
转向后 yaw=180、位置回到 z≈-1000；窗口 459 FPS；
`CloseMainWindow` 后正常 teardown，stderr 为空。42 个场景 schema 检查通过。

这次矩阵走正式 `LocomotionState`/`Animation.Match`，但仍由测试属性驱动，
不是真实键鼠输入；程序集重载/World 换代与分项性能仍未验收。

### 真实键鼠、性能与生命周期冒烟

- 隐藏 `PseudoConsoleWindow` 后把焦点交给 SDL，正式 playground 包可接收真实
  `keybd_event`：空闲 clip=0，W 后 clip=6，W+LeftShift 后 clip=7（2.5 s），
  Space 后 clip=9，松开回 clip=0；日志 `artifacts/gasp-p7-input-run.log`。
- 无测试负载的正式 playground 包采样 97–117 FPS（`gasp-p7-perf-run.log`）；
  探针场景 451/459 FPS。仅为帧率冒烟，不是逐 pass 分项分析。
- 生命周期回归：`ScriptAssemblyReloadTest.*`/`ScriptAssetReloadTest.*` 29 项、
  World replacement 6 项通过（`gasp-p7-lifecycle-tests.log`、
  `gasp-p7-world-lifecycle-tests.log`）。

## 实景与限制

采集使用正式 C#/Jolt/MM 夹具，Motion 调试面板由 `motiondebug 1` 打开；
截图应显示新增时间轴。它验证面板布局与出图，不把 UI 事件在真实窗口里逐项手点。

Development 1280×720 的 walk 采集报告为
`artifacts/gasp-p7-timeline-walk-20261019/capture.json`（1/1 通过，config 恢复）。
截图 `walk-t1-1280x720.bmp` 的面板读回（视觉模型两次一致，非像素测量）：
标题 `Motion Matching | #41 | DB 9b6ae5f7668b`，同一行有 play（空心三角）与
pause（双竖条）两个按钮、一条横向轨道，右侧时间文本 `0.3435 / 4 s >`。
裁剪范围偏左导致按钮左缘与滑块 thumb 被切掉，因此只登记“控件存在且文本正确”，
不把 thumb 位置当作布局验收。

后续增强（不阻塞本次 P7 技术验收）：

- 回放只重演查询/候选/代价，不按历史候选重演骨骼姿态；逐帧步进按钮未做。
- 正式 MM 探针已覆盖状态机/选帧，但由 `PackageProbe` 测试属性驱动，真实转向/受阻键鼠矩阵未做。
- 无测试负载的正式 playground 分项性能（仅记录 97–117 FPS 的帧率冒烟）。
- 程序集重载/World 换代的包内实测（本轮引用 29 项 reload + 6 项 World replacement 回归）。
- 多角色同时调试时的时间轴/回放选择；当前一次只显示一个被聚焦组件。
- 真实窗口中的长时间拖动、键盘焦点和不同 DPI 未做像素级矩阵。

因此本次 P7 技术验收分项完成；上述项目保留为后续增强。

## 证据

- 全量：`artifacts/gasp-p7-full-script-04.log/xml`（384 项，1236.254 s）；
  `gasp-p7-full-script-02.log/xml` 为 382 项基线。
- 定向脚本：`artifacts/gasp-p7-timeline-script-tests-01.log`、
  `gasp-p7-replay-script-tests-01.log`。
- UI：`artifacts/gasp-p7-replay-editor-full.log/xml`、
  `gasp-p7-replay-ui-tests-01.log`、`gasp-p7-timeline-ui-tests-02.log`。
- 动画/C# OFF：`artifacts/gasp-p7-replay-animation-full.log/xml`、
  `gasp-p7-replay-csharp-off-tests.log/xml`。
- 构建：`artifacts/gasp-p7-replay-editor-build-01.log`、
  `gasp-p7-replay-script-build-01.log`、`gasp-p7-replay-main-build-01.log`、
  `gasp-p7-replay-development-build.log`、`gasp-p7-replay-csharp-off-build.log`、
  `gasp-p7-probe-build.log`、`gasp-p7-probe-build2.log`、
  `gasp-p7-mm-probe-build.log`、`gasp-p7-mm-probe-build2.log`。
- 独立包：`artifacts/gasp-p7-export-01.log`（Debug 闭包）、
  `gasp-p7-export-dev-01.log`、`gasp-p7-export-dev-02.log`、
  `gasp-p7-export-dev-run.log/.err`、`gasp-p7-action2-run.log/.err`、
  `gasp-p7-move-idle.bmp`、`gasp-p7-move-walk.bmp`；
  包目录 `artifacts/gasp-p7-export-01/`、`gasp-p7-export-dev-01/`、
  `artifacts/gasp-p7-export-dev-02/`。
- 动作矩阵探针：`artifacts/gasp-p7-probe-scene3.log`、
  `gasp-p7-probe-export-01.log`、`gasp-p7-probe-package.log/.err`、
  `gasp-p7-probe-scene-schema.log`；包目录 `artifacts/gasp-p7-probe-export-01/`；
  资源源 `Maps/SC_gasp/Blueprints/PackageProbe.blueprint.cs`、
  `BP_gasp_package_probe.json`、`SC_gasp_package_probe.json`。
- 正式 MM 探针：`artifacts/gasp-p7-mm-probe-scene3.log`、
  `gasp-p7-mm-probe-export-01.log`、`gasp-p7-mm-probe-package.log/.err`、
  `gasp-p7-mm-probe-scene-schema.log`；包目录
  `artifacts/gasp-p7-mm-probe-export-01/`；资源源
  `Maps/SC_gasp/Blueprints/BP_gasp_mm_probe.json`、
  `SC_gasp_mm_probe.json`，以及 `GaspCharacter.blueprint.cs` 的
  `PackageProbe` 默认关闭属性。
- 真实键鼠/性能/生命周期：`artifacts/gasp-p7-input-run.log`、
  `gasp-p7-input3-run.log`、`gasp-p7-perf-run.log`、
  `gasp-p7-lifecycle-tests.log/xml`、`gasp-p7-world-lifecycle-tests.log/xml`。
- 实景：`artifacts/gasp-p7-timeline-walk-20261019/capture.json` 及
  `walk-t1-1280x720.bmp`、`walk-t1-1280x720.log/.trace.json`。

契约见 [动画运行时契约](../contracts/animation-runtime-skinning.md#c-片段控制)，
测试登记见 [测试契约](../contracts/testing.md)。
