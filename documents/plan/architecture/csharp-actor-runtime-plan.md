# C# Actor Runtime Plan

## 用户源码资源化增补（2026-10-18）

原计划之后补充用户资源归属：常驻 Runtime/API 留在引擎，
用户 `.blueprint.cs`、普通 C# 数据及辅助代码进入资源仓库的 Common/Maps 内容域。
现有示例按共享或关卡专属归属迁移，不再从引擎 Gameplay 目录编译游戏逻辑。
Gameplay 项目由引擎模板生成到 Generated，初始构建与编辑器候选编译均消费资源源码。
Outliner 新建蓝图同时创建源码与配对 BP JSON，随后编译并沿既有事务发布类目录。
文件发布不覆盖已有作者文件，编译失败保留文件与活动程序集。
这项增补不引入逐地图程序集或蓝图继承。当前精确行为见蓝图契约；
下列矩阵及历史数据保留原计划收尾时的语境。

## 原计划收尾矩阵

原始 Phase 0–5 的功能范围已完成。当前名称统一为 C# 蓝图；
实现和边界以 [蓝图契约](../../contracts/csharp-blueprint-runtime.md) 为准，
最终验证以 [测试契约的收尾记录](../../contracts/testing.md#c-原计划收尾验证) 为准。
本节优先于下面保留的历史 Status、未实现清单和各轮测量。

| 阶段 | 完成内容 | 主要证据 |
| --- | --- | --- |
| Phase 0：宿主与边界 | Windows x64/.NET 8、hostfxr、ABI v1 有界 UTF-8、分代 Gameplay 和私有运行时部署；启动失败不发布半初始化宿主 | ScriptStartup、ScriptDeployment、直接 ScriptHost/CLR 用例 |
| Phase 1：垂直链路 | 场景蓝图实例、生命周期、Transform/Input/Time/Log、故障隔离、Play/Pause/Stop 与真实 World 视觉值发布 | ScriptRuntimeTest、ScriptGameplayTest、既有 Vulkan Play/Stop 实窗记录 |
| Phase 2：World/组件 | 稳定身份、空间父链、逻辑组件、运行时增删挂接、代际句柄、候选 World 事务及冻结 WorldSnapshot | 场景/作者用例、运行时组件测试、ScriptMixedLoadTest、既有 World 事务测试 |
| Phase 3：游戏服务 | action/axis、计时器、事件、Spawn/Destroy、类型化 Expose/内联自定义数据、显式实例权限、相机/灯光、HUD 文本与集中诊断 | ScriptGameplayTest、ScriptLogicComponentTest、ScriptCustomDataTest、ScriptHudTest、真实 RmlUi HUD 绑定 |
| Phase 4：构建/部署/重载 | CMake Debug/Release、候选独占编译、DLL/PDB/依赖确定性、手动/监视编译、重载失败保留、委托/上下文及旧程序集回收、独立导出 | ScriptBuildTest、ScriptAssemblyReloadTest、game_export_tests、独立目录私有 CLR 新进程验收 |
| Phase 5：加固 | 显式深值迁移、异步资产完成/取消/租约、FixedTick/Jolt、时间倍率、诊断/性能采样、增量同步和混合负载 | 迁移/异步资产/Jolt/时钟套件、千实例性能和混合 CLR/Jolt/WorldSnapshot 回归 |

本轮补齐最后的实际缺口：

- C# 用 `Hud.SetText/Remove` 发布有界文本，按根/组件隔离、回调回滚和生命周期清理；
  生产弹药示例只用 C# 输入和状态，UI 消费冻结文本，不暴露 DOM 或 Vulkan。
- Blueprint 和逻辑组件支持普通类型名、短程序集限定名及真实完整程序集身份；
  默认值、权限、作者/UI 与 CLR 解析一致，错误程序集/版本被拒绝，作者文本不被改写。
- 两个独占编译候选逐字节比较 Gameplay DLL/PDB/deps 清单与桥接 DLL/PDB。
  Windows Roslyn 的 PathMap 分隔符缺陷由失败回归定位并修正。
- 脚本错误携带异常类型、处置、程序集/场景和代际；性能明细定频采样，
  故障和生命周期即时发布。增量导出先比较再分配，保留一次性命令语义。

性能收尾沿用预热 3 帧、测量 30 帧，在两个独立 Release 新进程中各运行两遍：
千空实例平均 0.5070–0.6729 ms、p95 1.6779–1.8419 ms，四次明确启用的
5 ms p95 预算均通过；100 移动/900 空实例平均 1.0253–1.2424 ms，
千移动实例平均 5.1690–5.9794 ms。空实例分配约 29.5 KB/帧，
全移动约 543 KB/帧。它们是 Tick/JSON/World 同步/诊断耗时，不含渲染。
另一个包含 64 动态球、139 个 CPU draw sections、计时器/事件/相机/灯光的
混合测试，Tick + WorldSnapshot 平均 6.04–6.21 ms；不代表 GPU 帧率。
下面历史上的 211 ms、57–66 ms 和首遍 p95 失败记录保留为优化证据，
不再代表当前测量。

原提案中的托管单元与新增 runtime test 设想按现行仓库治理落实：
托管 fixture 由根 GoogleTest 驱动真实 hostfxr/CLR，不另建测试 main；
Vulkan 事务沿用获准的既有测试，不新增 runtime 标志。
同步和独立 RT 模式共用输入后、快照前的 GT 脚本路径，RT 只消费冻结值。
本轮没有恢复桌面自动操作，HUD 等新增交互的模块覆盖不记为新实窗验收。
最终门禁：Debug 脚本 288 项连续两遍全通过，UI 每遍 624 通过/5 环境跳过；
Release 脚本每遍 287 通过/1 部署前提跳过，当前 Debug 独立目录私有 CLR 实测通过。
C# OFF main/脚本构建及 69 项两遍通过；根 CTest 1286 项中 1280 通过/6 环境跳过，
零失败。单线程与双线程 World 事务串行通过，均退出码 0、stderr 为空并正常 teardown。

原计划完成不表示所有游戏引擎功能齐备。独立数据资产/表、存档、蓝图资产继承、
动画、音频、网络与毛发模拟属于独立扩展；二进制 ABI、更多规模/硬件的性能预算及
补充桌面交互验收也不由本次有限样本自动证明。脚本仍是可信进程内代码，
任意静态字段、文件和自建线程副作用不具备事务回滚保证。

## 历史提案与逐轮记录

下文保留当时的命名、未实现项和验收设想，不作为当前完成度或实现契约。

2026-10-17 命名决议：本计划保留历史 Actor/ACT 讨论，当前功能统一称为蓝图（Blueprint）。
当前实现与格式以 [C# 蓝图契约](../../contracts/csharp-blueprint-runtime.md) 为准：
`Blueprints/BP_*.json`、`*.blueprint.cs`、`Blueprint`、`BlueprintHandle`、
`BlueprintReference`、`blueprintInstance`、`blueprintPath` 与 `blueprint.*`。
旧格式直接迁移，不保留类型、命令或 API 别名。

2026-10-17 性能增量补记：播放阶段已使用 JSON 稀疏状态同步、原生 ID 状态索引和
物理实例子集；未重写的基类帧回调跳过，继承 override、委托和生命周期保留。
千空实例 Release 平均从分段基线 61–68 ms 降到 1.33–1.69 ms；
100 移动/900 空实例约 2.7–2.9 ms，全千实例移动约 8.1–9.5 ms。
仍有约 6 ms 的新进程首遍 p95，显式 5 ms 尾延迟预算未达标；
全活跃托管分配、刚体同步、JSON 和逐帧诊断仍有优化余量。
二进制 ABI、独立数据资产和存档接口没有包含在此增量中。
详细测量及验证以当前蓝图/测试契约为准。下面保留增量前的状态和测量记录。

最新集成状态（2026-10-17）：蓝图作者工作流、运行时与自定义逻辑组件、
固定更新/时钟、显式播放中重载和集中 Log 已实现。最终全量回归零失败，
证据见 [测试契约](../../contracts/testing.md)。千空蓝图 Release 整帧脚本路径
平均约 211 ms，仍需优化 JSON 桥接、World 同步和诊断开销，不能据此宣称大规模
游戏逻辑性能达标。下面原始 Status 与逐轮增量保留历史语境；
当前命名和实现边界以上述蓝图契约为准。

## Status

- Type: executable architecture and implementation plan
- Created: 2026-09-03
- State: first vertical slice implemented (2026-10-17); full plan incomplete
- Naming decision: gameplay source files use `*.actor.cs`
- Scope: an in-process C# gameplay layer with code-authored Actors; no node-based Blueprint editor or graph runtime

This plan defines a usable C# Actor runtime for VulkanLearn. Gameplay authors
write normal C# classes, compile them into a managed gameplay assembly, and
attach those classes to scene-authored Actor instances. C++ keeps ownership of
the engine, World, resources, Vulkan, and frame boundaries. C# owns gameplay
behavior through a narrow, explicit API.

The current architecture intentionally excluded reflection, GC, editor, and
Blueprint infrastructure from V1. This plan is a post-V1 extension and must
preserve the existing Game Thread / Render Thread boundary, World transaction
rules, resource lifetime rules, and renderer snapshot contract.

## Target Outcome

```text
Player.actor.cs
    -> Gameplay.csproj
    -> Gameplay.dll + PDB
    -> native ScriptRuntime loads the assembly
    -> scene creates Gameplay.PlayerActor
    -> OnCreate / BeginPlay / Tick / EndPlay
    -> C# reads Input and changes Actor Transform
    -> C++ World is updated on the Game Thread
    -> WorldSnapshot is built
    -> Vulkan renderer displays the result
```

The first usable release must support Actor identity, lifecycle, components,
transforms, input actions, timers, events, spawning, destruction, scene
properties, diagnostics, test coverage, and a controlled reload story. It is
not merely a P/Invoke proof of concept.

## Current Baseline

2026-10-17 update: the implemented subset is documented in
[C# Blueprint runtime contract](../../contracts/csharp-blueprint-runtime.md).
It includes Windows x64/.NET 8 hosting, lifecycle, scalar Expose properties,
scene identity/TRS, one root mesh, input/time, fault isolation, Scene drafts and
editor placement/Play/Pause/Stop. Assemblies reload by process restart.
The next landed slice adds single-level ACT_ assets, sparse scene overrides,
property reset, new asset creation and native viewport drag/drop onto y=0.
The gameplay increment adds scene/mesh component trees and local editing, preloaded
ACT Spawn/Destroy, Bullet box/sphere root bodies and contact events, and Editing-only
ACT content reload through the existing World transaction. Timers, general events,
nested prefabs, state migration and assembly hot reload remain unimplemented.
2026-10-17 后续增量将 CPU 后端替换为锁定的 Jolt 5.6.0，增加 capsule、
Trigger、批量射线与 CharacterVirtual 控制器；输入映射、owner 计时器、
Actor 查找/只读快照和事件投递的当前边界见上述契约。
前段 Bullet/未实现计时器的描述记录当时进度，不作为当前行为。
Jolt GPU/毛发模块没有接入；毛发资产、模拟与 RenderGraph 集成是独立扩展。
2026-10-17 作者工作流增量：Actor 改名/复制/删除、inline scene/mesh/camera
组件增删及父链编辑、根物理 Inspector 与碰撞轮廓、Scene 有界 Undo/Redo 已接入。
脚本相机与作者 Camera 独立，随物理结果发布，异常回滚镜头投影与活动选择。
新增 SC_actor_playground 学习场景和 ThirdPersonCharacterActor/SpawnTriggerActor，
用于角色移动跳跃、跟随镜头、Trigger 内事件交互及预加载 Spawn。
2026-10-17 Actor 资产编辑增量：Content Browser 双击 ACT 打开独立配置标签，
支持类默认值、组件树/相机与物理配置，沿用 revision/冲突检查和显式保存。
ACT 的 instanceEditable 白名单决定场景可覆写属性；inline 由
`[Expose(InstanceEditable = true)]` 开放，单独 `[Expose]` 只表示可配置默认值。
未开放属性在 Details 只读，GT 与加载端都拒绝越权覆写。
这轮没有节点图或 3D Actor 预览。
2026-10-17 预览增量：补齐三栏中央真实 3D 视口，复用 `SC_model_preview`、
正式 World 事务和既有环绕相机。GT 仅消费路径/revision 对应的权威 ACT 视觉草稿，
不写临时 ACT，不执行 OnCreate 或物理；组件修改稳定后自动刷新，返回关卡恢复 Scene 草稿。
该增量仍不含组件拾取、gizmo 或非网格组件图示，Vulkan 实窗验收因用户 Esc 中止未恢复。
2026-10-17 布局增量：按 UE 组件编辑习惯将长表单拆成左侧 Components 树与右侧 Details，
Actor (Self) 配置类默认值/实例白名单/根物理，选中组件配置其父级/TRS/网格或相机。
修复根 rotation/scale 与物理尺寸向量行收缩，选择按稳定 ID 保留，不进入作者 JSON。
桌面验收在初次默认速度编辑后被用户 Esc 中止；后续布局验证使用真实 RmlUi 模块，
没有恢复桌面自动操作，保存/关闭/Undo/Redo 的实窗验收仍未完成。
这不等同于完成全计划：组件实例覆写、通用 gizmo、碰撞层/约束、
程序集热重载、可部署包、typed Expose 和运行时组件增删仍是后续范围。
2026-10-17 本轮增量：恢复桌面验收，修复场景选择器漏掉 actor 导致返回关卡请求被丢弃的问题。
真实 Vulkan 窗口已核对中央网格、环绕/缩放、组件选择、局部变换保存/重开、
保存后关闭/取消关闭、关卡 Scene 草稿在预览往返保留、Undo/Redo 和最大化布局。
正常 Alt+F4 完成 teardown；丢弃分支和中键平移仅有模块覆盖，没有记为桌面通过。
typed Expose 增加 Vector2/3/4、非 Flags 枚举、AssetId 和 ActorReference，
继续要求显式 Instance Editable。新增点光/聚光/方向光组件，GT World 统一冻结灯值，
父缩放不改变物理强度，组件方向光不取代 CSM 主光。
增加停止播放后的手动 Gameplay 程序集重载：隔离候选验证、失败保留、分代卸载，
及可执行文件相对 managed 输出、配置感知构建和私有 .NET 运行时打包目标。
源码自动编译/监视、播放中状态迁移、通用 gizmo、组件实例覆写、运行时组件增删、
碰撞层/约束和完整游戏导出器仍属未完成范围。前文未实现列表记录此前状态。
2026-10-17 本轮收尾证据：118 项脚本、528 项编辑器回归均连续两遍通过
（编辑器 5 项符号链接环境跳过）；独立目录新进程确认使用随包私有 CoreCLR，
World/RenderGraph 事务测试退出码 0。灯光实窗已核对脉冲播放、Pause/Stop、
手动程序集重载、组件强度编辑和 Expose 向量单分量编辑，正常 Alt+F4 teardown。
枚举下拉、引用控件、关闭丢弃和中键平移仍只有模块覆盖；完整证据见 testing 契约。
2026-10-17 蓝图视口交互增量：蓝图独立标签支持正式导入网格的组件包围盒拾取，
选择同步到 Components/Details，并提供移动、旋转和缩放轴。移动沿父坐标轴，
旋转编辑局部 YXZ Euler 分量，缩放沿局部轴；拖动只更新候选子树轮廓，
释放提交一次完整组件编辑，正式模型沿用 350ms 稳定后的预览事务刷新。
失焦、Esc、切页、模态和 revision/generation/视口布局变化取消手势。
新增每文档 32 条且总计 16 MiB 的 Undo/Redo，保存保留历史，
revert/reload 清空；失败或陈旧请求不破坏历史，完成反馈解除 UI 等待。
三个子代理分别完成历史、几何和界面，主线程完成集成修复与验收：
编辑器 567 项连续两遍均 562 通过、5 项符号链接环境跳过，
脚本 120 项及生产集成 11 项连续两遍通过，既有视口 41 项通过。
Vulkan 实窗核对包围盒拾取、三种变换、Undo/Redo 和最大化布局，
逐步撤销恢复原始草稿后正常关闭，未保存验收修改。
通用关卡 gizmo、三角形/深度拾取、非网格组件图示、组件实例覆写、
运行时组件增删、碰撞层/约束、播放中状态迁移和完整游戏导出器仍未完成。

2026-10-17 P1/P2 集成增量：以上历史清单中的七项已完成实现与集成验证：
蓝图视口实时预览、Self 与辅助图示/吸附；按 BP 白名单控制的组件实例覆写；
内置组件运行时 Add/Remove/Attach 与代际句柄；Jolt 层/掩码、compound/静态 mesh、
Fixed/Hinge、扫球镜头避障；编辑器独立 Gameplay 编译/诊断/源码监视；
同 World 异步 CPU 准备、分帧 GPU 上传、完成/取消/租约卸载；独立游戏导出器。
完整当前格式和边界见蓝图契约，测试与实窗证据集中记录于 testing 契约。
独立导出包已从工程目录之外、无有效系统 DOTNET_ROOT 的环境使用私有 CoreCLR，
自动进入 Play、生成刚体并正常关闭。
最终 C# ON 全目标和 C# OFF Release 构建通过；脚本 177 项连续两遍通过，
编辑器 587 项连续两遍均 582 通过、5 项环境跳过。完整 CTest 1133 项仍只有
历史控制页的 6 项失败，World 事务测试退出码 0。
动态加载实窗已核对真实网格 Spawn、Destroy 后资源释放与同 World 保持；
编译按钮成功重载。最终游戏包包含 1614 个文件及显式动态 BP 清单。
Snap/组件权限的真实鼠标回归修复了 click 截断；用户 Esc 停止桌面自动操作后，
没有继续实窗复验这些控件，模块与最终构建验证通过。详细证据与测量限制见 testing 契约。

这些增量不代表 UE 全功能对齐：通用关卡 gizmo、三角形/深度拾取、资产继承、
用户自定义 C# Component 生命周期、播放中的程序集状态迁移、物理 worker/GPU 毛发、
安装器和平台签名继续作为独立后续议题，不计入本轮七项验收。

2026-10-17 后续基础增量：自定义 C# `Component` 已接入独立类目录、
`scriptComponents` 作者声明、BP 组件属性白名单、Scene 稀疏覆写及 Details，
提供独立生命周期/故障隔离、运行时增删与代际句柄；逻辑组件不占空间父链，
共享所属 Blueprint 的根 Transform。Expose 补齐单位 Quaternion 的 XYZW 值格式。
原生 GT 统一调度 Tick、60 Hz FixedTick/Jolt 子步、LateTick，增加可控 Time Scale、
非倍率时钟、八步追赶/丢步统计及暂停余量语义。
Blueprint/组件生命周期与异常文本、回调耗时、服务/程序集/GC 值快照已接入，
中央 Log/性能页消费冻结数据，具体 UI 和容量以契约为准。
Playing/Paused 程序集重载支持 `[Migrate]` 显式值属性、运行时空间/逻辑状态恢复及
OnReload 服务重建；保留原生物理与时钟，拒绝在途资源/物理请求、旧句柄和任意对象图迁移。
编辑器编译候选在原 World generation 的 Playing/Paused 也经该事务尝试发布。
以上说明实现状态；本增量最终构建、重复回归及性能测量结果仍由集成验收记录，
不沿用上一轮测试数字作为本轮通过证据。此前的未完成列表保留为历史，
其中自定义逻辑组件、显式迁移、固定回调和性能诊断已不属于当前缺项。
通用关卡 gizmo、深度拾取、资产继承、动画/音频/存档/网络、
多线程物理与毛发模拟、安装器/签名仍未包含在这次基础增量中。

本次基础增量最终验收补记：C# ON 全目标 Debug、C# OFF Release main/script_tests
构建通过；完整 CTest 1197 项中 1191 通过、6 项符号链接环境跳过、零失败，
已修复历史控制页的六项失效测试。Release 千刚体整步平均 4.219402 ms；
千空蓝图经过 Tick/JSON/World 同步/诊断的平均耗时 211.152667 ms，
p95 222.5 ms。后续先定位桥接与同步成本，再决定批量数据通道和快照采样策略；
该性能工作尚未完成。Log 本轮有模块覆盖，没有新增实窗验收。

2026-10-17 自定义数据方案确认：普通游戏状态留在 C#，不因支持用户类型而逐帧复制到 C++。
作者配置使用程序集加载时缓存的递归类型描述，覆盖无循环的自定义数据类/结构体、
一维数组、List 和字符串键 Dictionary；编辑器、原生校验和显式重载迁移共用描述。
内联数据按值合成，实例权限由根 Expose 属性授权，集合先整体覆写/Reset。
本次先实现内联数据闭环；独立共享数据资产与存档接口属于后续增量，
配置权限、迁移标记和存档策略不隐式绑定。高频引擎状态的增量批量通道独立推进，
不能将当前 JSON 桥接或本次描述缓存写成二进制通道已经完成。

内联数据增量验收补记：自定义 class/struct、数组/List/字符串键 Dictionary、
四处递归编辑、对象稀疏覆写与集合整体覆写、实例权限及深值迁移已接通。
新增武器配置逻辑组件作为生产示例，详细边界见当前 C# 蓝图契约。
完整 CTest 1237 项中 1231 通过、6 项环境跳过；最终脚本 241 项及 UI 627 项
均连续两遍通过（UI 每遍含 5 项环境跳过），ON/OFF 构建及原生 World 事务通过。
身份索引与复用响应缓冲后，Release 千空蓝图同用例两遍均值为
65.7463 / 57.4162 ms，历史均值为 211.152667 ms；测量仍包含 JSON、
World 同步和诊断，不代表渲染帧率，也不代表高频增量通道已完成。
独立数据资产、存档接口和高频二进制增量桥接仍是后续增量。

The baseline below records the
2026-09-03 starting point; it is historical, not a current source inventory.

The repository already provides useful boundaries but not the runtime itself:

- `EngineLoop` owns startup, per-frame dispatch, World transitions, and render
  submission in `source/engine/engineLoop.*`.
- `World` is the Game Thread-owned mutable gameplay state in
  `source/world/world.*`.
- `WorldSnapshot` is an immutable render-facing DTO; the renderer must not read
  mutable World or managed objects directly.
- `InputSubsystem` converts SDL input into engine-facing intent, but currently
  exposes only hard-coded movement keys and mouse deltas.
- `CommandBus` and `RuntimeCommandExecutor` demonstrate deferred owner-side
  commands, but are not a C# ABI or a general gameplay command system.
- `WorldLoader` and `WorldBuilder` support fixed scene object types. The scene
  schema has no Actor class, script property, or component declaration.
- `RuntimeConfig` has no script assembly, script root, reload, or execution
  policy configuration.
- The CMake project builds C/C++ only. There are no managed projects, runtime
  host, binding layer, or script tests.

## Design Principles

1. **C# is gameplay, not renderer control.** Managed code cannot access Vulkan,
   `RenderSystem`, `RenderGraph`, renderer caches, descriptor sets, or native
   GPU objects.
2. **C++ owns identity and lifetime.** C# receives opaque handles and cannot
   delete native objects or keep ownership through raw pointers.
3. **Game Thread is the first execution model.** Actor callbacks and World API
   calls run on the Game Thread. Render Thread code only consumes snapshots.
4. **World transitions are transactional.** Candidate World and candidate
   script context are prepared together and published together.
5. **Frame mutation is deterministic.** Spawn, destroy, attach, and component
   changes are queued and flushed at defined frame boundaries.
6. **The public managed API is small and versioned.** Eigen, STL containers,
   Vulkan-Hpp types, and C++ exceptions never become the ABI.
7. **Normal C# tooling remains usable.** `*.actor.cs` is an engine convention,
   not a requirement for a custom IDE.
8. **Failures are contained.** A script exception is diagnosed with Actor and
   callback context and cannot silently corrupt World state.

## Scope And Non-Goals

### In scope

- In-process .NET/CoreCLR-hosted C# runtime for the current Windows-first target.
- One or more managed gameplay assemblies, with `Gameplay.dll` as the default.
- `Actor` and `Component` base types.
- Scene binding by managed type name and serialized properties.
- Transform, input, time, timer, event, logging, asset-reference, and basic
  World APIs.
- Deferred structural commands and stable opaque handles.
- World-load integration and deterministic shutdown.
- Build, deploy, diagnostics, tests, and an initial manual reload path.

### Explicitly out of scope for the first release

- Blueprint graph editor or node graph format.
- General-purpose C++ reflection/GC/UObject system.
- Direct C# authoring of Vulkan pipelines, shaders, descriptors, or render passes.
- Untrusted Mod sandboxing inside the engine process.
- Multi-threaded Actor Tick or arbitrary managed access from the Render Thread.
- Full physics, animation, audio, networking, replication, or save-game systems.
- Hot replacement of arbitrary managed code while preserving every object field.

## Proposed Runtime Architecture

```text
EngineLoop (GT)
  ├─ InputSubsystem
  ├─ ScriptRuntimeSubsystem
  │    ├─ ScriptHost / .NET boundary
  │    ├─ AssemblyCatalog
  │    ├─ ScriptTypeRegistry
  │    ├─ ScriptWorldContext
  │    ├─ ActorInstanceRegistry
  │    ├─ ScriptScheduler
  │    ├─ GameplayCommandBuffer
  │    └─ ScriptDiagnostics
  ├─ WorldManager / WorldTransitionCoordinator
  └─ WorldSnapshotBuilder -> RenderSystem

Managed side
  ├─ VulkanLearn.Runtime.dll
  │    ├─ Actor / Component
  │    ├─ Handles and math value types
  │    ├─ Transform / World / Input / Time APIs
  │    ├─ Timer / Event APIs
  │    └─ NativeApi P/Invoke declarations
  └─ Gameplay.dll
       └─ user-authored `*.actor.cs` classes
```

### Ownership matrix

| Object | Owner | Managed view |
|---|---|---|
| Actor identity and enabled state | C++ World / ActorRegistry | `ActorHandle` |
| Actor Transform | C++ World | `Transform` value/proxy API |
| Native component data | C++ ComponentRegistry | typed component handle |
| C# object instance | ScriptRuntime | managed reference, never native owner |
| GPU mesh/material/texture | renderer resource owners | logical `AssetId` |
| Frame render input | `WorldSnapshotBuilder` | not exposed as mutable object |
| callback scheduling | ScriptScheduler | callback token/wrapper |

## Managed API Shape

The following is the intended shape, not a frozen implementation signature:

```csharp
public abstract class Actor
{
    public ActorHandle Handle { get; }
    public Transform Transform { get; }
    public bool Enabled { get; set; }

    protected virtual void OnCreate() { }
    protected virtual void BeginPlay() { }
    protected virtual void Tick(float deltaTime) { }
    protected virtual void LateTick(float deltaTime) { }
    protected virtual void EndPlay(EndPlayReason reason) { }
}

public abstract class Component
{
    public ComponentHandle Handle { get; }
    public Actor Owner { get; }
    public bool Enabled { get; set; }

    protected virtual void OnCreate() { }
    protected virtual void BeginPlay() { }
    protected virtual void Tick(float deltaTime) { }
    protected virtual void EndPlay(EndPlayReason reason) { }
}
```

Example authoring style:

```csharp
public sealed class PlayerActor : Actor
{
    [Expose]
    public float MoveSpeed { get; set; } = 5.0f;

    protected override void BeginPlay()
    {
        Log.Info($"Player started: {Name}");
    }

    protected override void Tick(float deltaTime)
    {
        var movement = Input.GetVector2("Move");
        Transform.Translate(new Vector3(movement.X, 0.0f, movement.Y) * MoveSpeed * deltaTime);
    }
}
```

The final API must document whether property access is a cached managed value,
a native proxy, or a command submission. Prefer batched native calls and avoid
one P/Invoke transition per scalar field per frame.

## Native/Managed ABI

Use a dedicated C-compatible layer, for example:

```text
source/script/native/scriptNativeApi.h
source/script/native/scriptNativeApi.cpp
source/script/native/scriptAbiTypes.h
```

Required rules:

- Use fixed-width integers, POD structs, UTF-8 strings, caller-provided buffers,
  and explicit release functions.
- Use opaque `uint64_t` IDs plus generation/context tokens to reject stale
  handles.
- Never export C++ name-mangled functions, STL containers, Eigen objects,
  `shared_ptr`, exceptions, or Vulkan handles.
- Every call returns a status or writes a structured error into an engine-owned
  error channel; exceptions never cross the ABI.
- ABI and managed runtime versions are checked before loading `Gameplay.dll`.
- Group functions by ownership: runtime, actor, component, transform, input,
  time, timer, event, asset, diagnostics, and command submission.

The first ABI version must be narrow. Expanding it requires an explicit version
decision and managed wrapper update, not ad hoc exported helper functions.

## Actor And Component Model

Every Actor and Component needs:

- stable runtime ID within the active World;
- World generation or context ID;
- debug name and managed type name;
- enabled/started/pending-destroy state;
- optional tags and parent Actor ID.

Handles become invalid after destruction or World replacement. Native code must
validate generation before every mutating operation.

### Lifecycle

```text
Construct managed object
    -> bind native handle and serialized properties
    -> OnCreate
    -> BeginPlay
    -> Tick / Component Tick
    -> LateTick
    -> EndPlay
    -> release managed instance and native registration
```

Required decisions:

- Actor creation follows scene declaration order.
- Component creation follows declaration order.
- `BeginPlay` occurs only after the candidate World is valid.
- `Destroy` marks for end-of-frame removal and does not invalidate iteration.
- `Spawn` becomes visible at the next documented phase.
- A callback exception disables the failing Actor according to configured policy
  and records a structured diagnostic.

### Components

Start with adapters for current runtime needs:

- `TransformComponent`;
- `MeshRendererComponent`;
- `CameraComponent`;
- `DirectionalLightComponent`;
- `PointLightComponent`;
- `SpotLightComponent`;
- `EnvironmentComponent` only if gameplay requires it.

Do not force an immediate rewrite of current camera/light/mesh loading. Add the
component layer first, then migrate individual scene types when their ownership
and snapshot representation are ready.

## Frame And Thread Contract

The target frame order is:

```text
1. Process platform events
2. Update input action state
3. Advance RuntimeClock
4. Publish input/time to ScriptRuntime
5. Run Actor / Component Tick on the Game Thread
6. Flush gameplay structural commands
7. Run LateTick and final transform propagation
8. Build WorldSnapshot
9. Submit/render snapshot
10. Retire timers, callbacks, and destroyed instances
```

The exact placement must be reconciled with the current `EngineLoop::Tick()`
ordering before implementation. Scripts run after input is available and before
`WorldSnapshotBuilder` reads World.

First-version restrictions:

- C# callbacks run only on the Game Thread.
- C# never reads mutable World from the Render Thread.
- Structural changes use `GameplayCommandBuffer`.
- Background managed work may calculate plain data only; applying it requires a
  Game Thread continuation or explicit command.
- Shutdown stops script callbacks before renderer and World owners are torn down.

## Scene And Configuration Contract

Add an Actor form without breaking existing fixed object forms:

```json
{
    "name": "Player",
    "type": "actor",
    "class": "Gameplay.PlayerActor",
    "position": [0, 0, 0],
    "rotation": [0, 0, 0],
    "scale": [1, 1, 1],
    "components": [
        { "type": "mesh", "modelPath": "models/SM_player.json" }
    ],
    "properties": {
        "MoveSpeed": 5.0
    }
}
```

The schema must define managed type and assembly identity, transform defaults,
component properties, property conversion/validation, tags, parent reference,
enabled state, and spawn policy. Exact field names are deferred until the native
Actor/component ownership model is implemented.

Runtime configuration should add a `scripts` block:

```json
{
    "scripts": {
        "enabled": true,
        "projectRoot": "script",
        "runtimeAssembly": "Gameplay/bin/Debug/net8.0/Gameplay.dll",
        "runtimeConfig": "Gameplay.runtime.json",
        "reloadMode": "manual",
        "exceptionPolicy": "disable_actor"
    }
}
```

Paths are resolved through `RuntimeConfig`; the script subsystem must not
rediscover the project root independently.

## Build And Deployment Plan

Recommended managed layout:

```text
script/
├── VulkanLearn.Runtime/
│   ├── VulkanLearn.Runtime.csproj
│   └── ...
└── Gameplay/
    ├── Gameplay.csproj
    ├── Gameplay.runtime.json
    └── Actors/
        ├── Player.actor.cs
        └── CameraController.actor.cs
```

`*.actor.cs` is a source naming convention. The project file must explicitly
include the files if SDK globs do not include them, while preserving standard
`dotnet`, Rider, and Visual Studio language-service behavior.

CMake should:

- expose `VULKANLEARN_ENABLE_CSHARP_ACTORS`;
- locate or validate the required .NET SDK/runtime;
- invoke the managed project as an explicit target;
- stage the assembly, dependencies, runtime config, and PDB predictably;
- make `main` depend on managed output only when enabled;
- report a clear configuration error rather than silently disabling scripts.

The C++ build remains valid without the .NET SDK when the option is disabled.
The initial native target is Windows/MinGW-compatible hosting; C++/CLI is not a
dependency or integration route.

## Runtime Hosting And Assembly Loading

`ScriptHost` owns managed runtime lifetime:

1. Validate host/runtime ABI versions.
2. Initialize the managed runtime before loading the initial World.
3. Load `VulkanLearn.Runtime.dll` and the configured gameplay assembly.
4. Discover Actor types through a controlled registration/reflection boundary.
5. Cache type metadata and lifecycle entry points.
6. Create a `ScriptWorldContext` for each active/candidate World.
7. Stop callbacks before unloading or replacing a context.
8. Shutdown only after all instances and callbacks are gone.

Use a generated registry or managed registration method where practical.
Reflection may support managed authoring properties, but must not become a broad
native reflection system.

## Properties And Serialization

Use explicit authoring attributes:

```csharp
[Expose]
public float MoveSpeed { get; set; } = 5.0f;
```

Initial property types:

```text
bool, int, uint, float, double, string,
Vector2, Vector3, Vector4, Quaternion,
AssetId, ActorReference, enum
```

The property pipeline needs one source of truth for defaults, JSON conversion,
validation, unknown fields, editor/runtime visibility, reset behavior, and
serialization. Do not serialize arbitrary managed object graphs initially.

## Input, Time, Timer, And Events

### Input

Keep SDL ownership in `InputSubsystem`, but replace fixed movement fields with
an action/axis map:

```csharp
Input.IsPressed("Jump")
Input.WasPressed("Jump")
Input.WasReleased("Jump")
Input.GetAxis("MoveForward")
Input.GetVector2("Move")
```

Input contexts must let UI capture disable gameplay input without exposing SDL.

### Time

Expose explicit semantics:

```text
Time.DeltaTime
Time.UnscaledDeltaTime
Time.FixedDeltaTime (only when fixed update exists)
Time.TimeSinceStartup
Time.TimeScale
Time.FrameIndex
```

The first implementation may omit physics-driven fixed update, but must not
pretend variable frame delta is fixed time.

### Timers and events

Engine-owned timers/events must provide one-shot and repeating timers,
cancellation by token and owner, subscription tokens, deterministic callback
phase/order, and automatic cleanup on Actor destruction or World replacement.
No callback may remain reachable through a timer or event after its owner ends.

## Asset And Rendering Integration

C# refers to resources logically:

```csharp
var mesh = Assets.Get<MeshAsset>("Meshes/Player");
```

The first release may expose references without complete asynchronous loading,
but must define identity, normalized paths, status/failure, World ownership,
renderer cache lookup, and retirement behavior. GPU resource creation remains in
the existing renderer resource loaders and transactional World/Graph path.

## World Transition Integration

Extend the existing prepare/commit path rather than adding a parallel script
loader:

```text
WorldTransitionCoordinator
  -> load and validate scene
  -> prepare renderer resource package
  -> build candidate World
  -> prepare candidate ScriptWorldContext
  -> create and validate candidate Actors
  -> prepare runtime binding
  -> commit World + script context + renderer owners
  -> begin play candidate scripts
  -> retire old script context after callbacks stop
```

The safe default is to construct and validate before commit, publish ownership,
then call `BeginPlay` at the next stable Game Thread phase. The post-commit
`BeginPlay` failure policy must be explicit and must not partially roll back
Vulkan ownership.

## Error Handling And Diagnostics

Every script failure should include assembly/type, Actor name and handle, World
scene path and generation, callback, exception type/message/stack, frame index,
and the resulting disable/destroy/load-failure action.

Use `DiagnosticsSubsystem` as the initial report sink. Start with this policy:

```text
construction / property validation failure -> reject candidate World
BeginPlay failure -> reject before activation when possible
Tick failure -> disable the Actor and continue
fatal host/runtime failure -> request controlled shutdown
```

## Reload Strategy

### Level 0: restart-only

Rebuild `Gameplay.dll` and restart the process. This is mandatory for the first
vertical slice and provides a reliable baseline.

### Level 1: manual context reload

At a safe Game Thread point, stop callbacks, create a new context/assembly,
recreate Actors from the active World and serialized properties, then resume.
Timers, events, delegates, native callbacks, and static state must not retain the
old context.

### Level 2: state-preserving reload

Only after Level 1 is stable, add explicit migration for fields marked with a
migration attribute or serialized property name. Do not promise arbitrary object
graph preservation.

Do not claim AssemblyLoadContext unload correctness until a test proves that old
contexts and delegates become collectible after reload.

## Developer Tooling

Eventually expose through the existing developer UI/console:

- runtime enabled state and loaded assembly/version;
- Actor count and type/name/ID;
- lifecycle state and last exception;
- per-Actor Tick time;
- pending timer/event/command counts;
- manual script reload;
- stale-handle and ABI diagnostics.

The first milestone can use diagnostics and console commands only; a full editor
is not required.

## Test Plan

### Managed unit tests

Use a normal .NET test project for lifecycle ordering, property conversion,
input edge semantics, timer cancellation/owner cleanup, event cleanup, and handle
wrappers.

### Native/module tests

Register through the repository GoogleTest helper for ABI layout/version checks,
stale-handle rejection, command ordering, Actor registry behavior, exception
conversion, scene Actor validation, and ScriptWorldContext isolation.

### Runtime test

Add a serial runtime test only when module tests cannot cover the behavior. It
must load `Gameplay.dll`, instantiate a scene Actor, observe `BeginPlay`, run
several frames, verify script-driven Transform and WorldSnapshot changes, load a
second World, verify old `EndPlay` and handle invalidation, and verify no old
timer or callback fires.

## Implementation Phases

### Phase 0: contracts and host spike

- Confirm supported .NET runtime/SDK and Windows deployment model.
- Add an isolated managed hello-world host spike outside gameplay integration.
- Freeze ABI versioning, handle generation, UTF-8, error, and shutdown rules.
- Decide one default assembly versus multiple assemblies.

Exit criteria: native code loads and calls managed code, reports a managed
exception, and shuts down without Vulkan or World involvement.

### Phase 1: vertical slice

- Add `ScriptRuntimeSubsystem` to `SubsystemCollection`.
- Add managed runtime library and `Gameplay.csproj`.
- Load `Gameplay.dll` during startup.
- Add Actor registration, handles, `OnCreate`, `BeginPlay`, `Tick`, and `EndPlay`.
- Add minimal Transform, Time, Log, and Input APIs.
- Bind one scene Actor and migrate the camera/player movement proof from
  `Controller` to `Player.actor.cs`.

Exit criteria: a `.actor.cs` Actor moves an existing World object in the Vulkan
application, and a failing script does not crash the process.

### Phase 2: World and component foundation

- Add native Actor/Component registries and stable IDs.
- Add scene schema and loader/build-plan support for Actor entries.
- Add component creation/lookup and parent/child Transform hierarchy.
- Integrate candidate ScriptWorldContext into World transactions.
- Define spawn/destroy/add/remove command flush phases.

Exit criteria: World replacement is atomic from the script and renderer
perspective; old handles cannot modify the new World.

### Phase 3: usable gameplay services

- Replace fixed input fields with action/axis maps.
- Add timers and owner-scoped events.
- Add exposed property metadata and scene overrides.
- Add logical asset references and first renderer-facing components.
- Add console/developer diagnostics and per-Actor timing.

Exit criteria: a small sample implements a controller, trigger, spawner, light
controller, and UI-facing state without C++ changes.

### Phase 4: build, deploy, and manual reload

- Integrate managed builds into CMake and runtime output staging.
- Add startup validation for missing SDK/runtime/assembly.
- Add manual script context reload at a Game Thread safe point.
- Prove cleanup of timers, events, callbacks, handles, and old assemblies.

Exit criteria: reload failures preserve the last valid running context or follow
an explicitly documented fallback policy.

### Phase 5: hardening and extensions

- Add explicit state migration.
- Add async asset completion routed to Game Thread.
- Add fixed-step update if physics or deterministic gameplay needs it.
- Add profiling and broader managed API coverage.

Every extension needs module tests, lifecycle ownership rules, and no weakening
of the WorldSnapshot or Vulkan resource boundaries.

## Acceptance Matrix

The first usable release is not complete until:

- C++ builds with the C# feature disabled.
- C# build produces deterministic assembly, dependencies, and PDB output.
- Missing SDK/runtime/assembly gives a structured startup error.
- A scene instantiates an Actor by assembly-qualified type name.
- Lifecycle ordering is tested.
- Input, Transform, Time, Log, Spawn, and Destroy work without raw pointers.
- Actor and Component handles reject stale World generations.
- Public C# API cannot mutate renderer/Vulkan objects.
- Script exceptions are isolated and diagnosable.
- Failed candidate World/script preparation leaves active World unchanged.
- World replacement stops old callbacks and owner-scoped timers.
- WorldSnapshot contains script-driven World changes.
- Existing synchronous and optional render-thread modes retain behavior.
- Manual reload commits a valid replacement or preserves the last valid context.
- GoogleTest, managed tests, and the approved serial runtime test pass.

## Main Risks And Mitigations

| Risk | Mitigation |
|---|---|
| ABI exposes C++ implementation types | Dedicated C ABI and layout tests |
| Old World remains reachable | Explicit context ownership and generation checks |
| Tick mutates containers | Deferred structural command buffer |
| World reload mixes old/new script state | Candidate ScriptWorldContext and one commit boundary |
| C# exception crashes native process | Catch at every managed callback boundary |
| P/Invoke overhead grows per frame | Cached proxies, value snapshots, batching, profiling |
| `.actor.cs` breaks tooling | Explicit project inclusion and standard SDK project support |
| Runtime deployment differs | Runtime validation and staged output policy |
| Reload leaves old assembly loaded | Collectibility tests and callback/timer cleanup audit |
| C# gains renderer authority | API review rule forbidding render-owner/Vulkan references |

## Deferred Decisions

Phase 0 must decide .NET version and deployment mode, hostfxr/CoreCLR details,
assembly topology, registration versus reflection, managed-only versus native
Actor representation, post-commit `BeginPlay` failure policy, property storage
location, fixed-step requirements, and the manual reload command.

## Documentation Follow-Ups

When Phase 2 becomes current behavior, update:

- `documents/contracts/vulkanlearn-architecture.html` with Script/Gameplay
  ownership and thread rules;
- `documents/contracts/coding-guidelines.md` with ABI and managed API rules;
- `schema/scene.schema.json` and schema documentation;
- `README.md` with C# SDK setup, build, run, and deployment instructions.

Until then, this plan is the future-facing proposal; the existing V1
architecture document remains the current implementation contract.
