# Shader Incremental Compile And Hot Reload Completion Fix Plan

## Status

- 类型：已完成审核修复历史
- 创建日期：2026-08-13
- 上游计划：
  `documents/plan/rendering/shader-incremental-compile-and-hot-reload-plan.md`
- 当前正式合同：
  - `documents/rendering/shader-build-cache.md`
  - `documents/rendering/shader-hot-reload.md`
- 完成日期：2026-08-13
- 当前结论：R1-R4 已全部实现并通过最终矩阵；本文保留原始 blocker、修复原则和
  验收要求作为审核历史，当前稳定合同以 `documents/rendering/` 与架构文档为准。

本文只负责关闭 2026-08-13 审核发现的完成度缺口。不得借此扩大到无关渲染重构。

## 完成证据摘要

- R1：`latestObservedSourceEpoch` 快速拒绝、pending source identity union、frozen
  primary/include snapshot 与 commit-time digest validation 已落地；in-flight
  A1/A2/A3、多独立 source、include 删除和 manual/auto 交错测试通过。
- R2：M_ candidate include overlay、批量原子发布和
  `MaterialInstanceStateSnapshot` 迁移已落地；参数、贴图、字段删除、类型冲突、
  多 M_ all-or-nothing、失败后恢复与 retirement drain 测试通过。
- R3：World-local resource package、`PreparedRenderGraphState`、runtime binding
  preflight 和 no-throw owner swap 已落地；全部 prepare fault 保持旧指纹，成功提交时
  World/graph/RenderSystem/Controller generation 同步前进。
- R4：publication mutex、包含 cache hit 的物理路径预检、stale cache-hit 写前复核、
  prepared retirement 激活边界与 shutdown-in-flight 已落地。
- Debug/Release、`workerThreadCount=1/2` 的六项 Shader 测试和 World/Graph 矩阵、
  stress、实际启动缓存回归均通过；第二次 warm start 为 `compiled=0, shaderc=0`。
- 最终日志未发现 Vulkan validation error、data race、use-after-free、heap
  corruption、device lost 或退休队列未 drain。最新二进制反复未复现旧 teardown
  heap after-free/SIGTRAP；这不是对所有未来环境不可复现的证明。
- 无已知未关闭的本专项限制。

## 新线程任务目标

新线程应把本文作为持续执行合同：

```text
/goal 完整修复 shader-incremental-compile-and-hot-reload-completion-fix-plan.md
中 R1-R4 的代码、测试、运行时验证和正式文档同步。按 R1 -> R2 -> R3 -> R4
顺序推进，不得在只补接口、只补测试、只完成 happy path 或仍存在已知事务窗口时停止。
```

只有以下条件全部成立，才能把目标标记为完成：

1. 编译在途时出现任何更新源码，旧 candidate 都不能提交到 live runtime 或正式 artifact。
2. 多个独立 source 的稳定变化不会因 pending plan 覆盖而丢失。
3. `M_*.json` 热更会迁移兼容的 live MaterialInstance 参数和贴图。
4. M_ 解析、include 生成、编译、资源创建、World/RenderGraph 绑定任一步失败，旧整包继续工作。
5. World、renderer resource cache、RenderGraph、RenderSystem 和 Controller 不会出现半切换状态。
6. Debug/Release、`workerThreadCount=1/2`、全部专项测试和压力矩阵通过。
7. 没有 Vulkan validation error、data race、use-after-free、悬空引用或资源泄漏。
8. 正式合同与实际实现一致，不再声明尚未成立的保证。

## 开始前必读

1. `AGENTS.md`
2. `documents/plan/rendering/shader-incremental-compile-and-hot-reload-plan.md`
3. `documents/rendering/shader-build-cache.md`
4. `documents/rendering/shader-hot-reload.md`
5. `source/engine/engineLoop.*`
6. `source/engine/runtimeTestHooks.*`
7. `source/shader/reload/*`
8. `source/shaderCompiler.*`
9. `source/materialInstance.*`
10. `source/render/resource/rendererMaterialLoader.*`
11. `source/render/resource/rendererResourceCache.*`
12. `source/world/loading/worldTransitionCoordinator.*`
13. `source/world/worldManager.*`
14. `source/renderGraph.*`
15. `source/render/resource/resourceRetireQueue.*`
16. 当前工作区 `git status` 和 `git diff`

保留并兼容用户已有修改。禁止 reset、覆盖或清理无关文件。除非用户另行要求，不提交
Git commit。

## 审核阻断项

### B1：旧异步 generation 仍可能覆盖更新源码

当前 `EngineLoop::ProcessAutomaticShaderReloads()` 先消费 worker 完成结果，再调用
`ShaderFileMonitor::Poll()`：

- `source/engine/engineLoop.cpp:846-920`
- `source/engine/engineLoop.cpp:931-932`

因此可能出现：

```text
A 已捕获并在 worker 编译
    -> 用户保存 B
    -> B 尚未经过本帧 Poll/debounce
    -> A 完成并通过 latestAutoReloadGeneration 检查
    -> A 被提交
    -> 下一帧才发现 B
```

此外，worker 忙碌时 `pendingAutoReloadPlan` 被新 plan 直接覆盖：

- `source/engine/engineLoop.cpp:1037-1042`

如果两个独立 source 先后产生稳定事件，前一个事件已经被 FileMonitor 消费，却可能随旧
pending plan 一起丢失，之后不会自动重试。

当前快速保存测试只在一次 Poll 前连续写同一个文件：

- `source/engine/runtimeTestHooks.cpp:2591-2601`

它验证了 debounce 合并，但没有验证“已有编译在途时再次保存”或“多个独立 source
事件合并”。

### B2：M_ 热更重新构造 MI，没有迁移旧 live 状态

当前 M_ 变化只执行：

```text
重写生成 include -> Queue LoadWorld(active scene)
```

对应：

- `source/engine/engineLoop.cpp:961-999`
- `source/render/resource/rendererResourceCache.cpp:42-85`
- `source/render/resource/rendererMaterialLoader.cpp:54-214`

新 MaterialInstance 完全从 M_/MI JSON 和默认值构造，没有读取旧 live MI。运行时通过
`RenderSystem` 或其他系统修改、且未写回 JSON 的参数和贴图会被重置。

当前 `--shader-definition-reload-test` 只验证新增参数得到默认值：

- `source/engine/runtimeTestHooks.cpp:3151-3165`

它没有先修改已有参数/贴图，再验证兼容迁移。

### B3：World/RenderGraph 后续失败会留下半切换状态

当前 World 在 runtime bind 和 graph reload 之前已经激活：

- `source/world/loading/worldTransitionCoordinator.cpp:153-157`
- `source/engine/engineLoop.cpp:340-364`

`BindActiveWorldRuntimeObjects()` 先修改 RenderSystem active World，再检查 view target 和
Controller：

- `source/engine/engineLoop.cpp:1074-1093`

RenderGraph reload 先销毁当前 graph，再原地创建新 graph：

- `source/engine/engineLoop.cpp:1170-1178`
- `source/renderGraph.cpp:560-604`

如果 Vulkan graph object 创建、pass material restore、descriptor rebuild 或 runtime bind
失败，现有路径只报告错误，没有恢复旧 World、旧 RenderGraph 和旧 runtime binding。

这不满足正式合同：

- `documents/rendering/shader-hot-reload.md:111-117`

### B4：必须一并关闭的事务加固问题

以下问题与三个主阻断项共享同一失败窗口，应在本专项内关闭：

1. `TrackingIncluder` 在查询 frozen source snapshot 前先读取磁盘。include 在捕获后被删除
   或短暂不可读时，worker 仍会观察磁盘状态：
   - `source/shaderCompiler.cpp:321-333`
2. 某个 M_ include 生成失败后，当前循环仍可能继续排队 World rebind：
   - `source/engine/engineLoop.cpp:961-999`
3. 正式 artifact 提交后调用的 Compute/UI 虚拟 commit 接口没有 `noexcept` 合同：
   - `source/shader/reload/computePipelineReloadParticipant.h`
   - `source/shader/reload/uiOverlayReloadParticipant.h`
   - `source/shader/reload/shaderReloadCoordinator.cpp:963-1028`
4. 旧日志 `build/teardown-novalidation.log` 曾出现进程退出期 heap after-free/SIGTRAP。
   最新 Debug 二进制未复现，但 shutdown-in-flight 必须有正式可重复测试，不能只依赖旧日志。

## 修复原则

### Source generation

- debounce 决定“何时开始编译”，不能作为“旧结果是否仍可提交”的唯一依据。
- 只要当前磁盘 BLAKE3-256 与 candidate 捕获值不同，candidate 就不得提交。
- generation 计数用于调度和诊断；最终正确性必须由完整内容 digest 复核。
- pending 变化必须按 source identity 求并集，禁止覆盖后丢事件。
- frozen snapshot 编译不得先读取对应磁盘文件。

### Runtime transaction

- prepare 阶段允许失败，但不得修改 live owner。
- commit 前完成全部解析、编译、反射、schema、ABI、Vulkan 创建和绑定校验。
- commit 阶段只允许预验证后的 ownership move/swap，必须为 `noexcept`。
- 任何无法做成 `noexcept` 的步骤必须移回 prepare 阶段，或提供真实回滚。
- RT idle 只负责阻止新 CPU 引用；旧 GPU 资源仍通过 frame epoch 退休。
- Shader/M_ 热更禁止使用 `device.waitIdle()`。

### M_ migration

- 相同 MI 资产身份、相同参数名且相同类型：保留旧 live 值。
- 相同贴图 binding 且新 schema/descriptor 合同兼容：保留旧 live 贴图。
- 新增参数或贴图：使用新 M_/MI resolved value 或默认值。
- 已从新 schema 删除的旧 runtime 字段可以丢弃。
- 同名类型改变、贴图 binding 合同不兼容、缺失必需值：整个事务失败。
- MI JSON 中的未知字段仍按现有严格校验失败，不得静默忽略。

## R1：关闭 P3 generation 和 pending 事件缺口

### R1.1 统一自动 reload 状态

不要继续让 `latestAutoReloadGeneration` 同时承担“调度顺序”和“源码仍为最新版”两个职责。
引入明确状态，命名可遵循代码库现有风格：

```text
latestObservedSourceEpoch
inFlightGeneration
inFlightCapturedDigests
pendingStableSources: set<normalized source identity>
pendingSourceEpoch
```

建议把 pending 状态保存为 source identity 集合，而不是提前捕获的完整 plan：

```text
稳定事件到达
    -> pendingStableSources 求并集
    -> source epoch 前进
worker idle
    -> 从 pendingStableSources 捕获最新 live recipe 和 frozen snapshot
    -> 成功 Submit 后才从 pending 集合移除
```

这样多个独立 source 的事件不会因 plan replacement 丢失，plan 也总是基于真正开始编译时
的最新 live owner。

### R1.2 调整每帧顺序

自动路径每帧至少遵守：

```text
1. Poll FileMonitor，收集 candidate/stable content changes
2. 记录 source epoch，并失效受影响的 in-flight generation
3. 合并 stable source identities 到 pending set
4. 消费 worker result
5. 对 result 的全部 primary/dependency digest 做提交前复核
6. current -> commit；stale -> discard，不改 artifact/live state
7. worker idle 时捕获并提交最新 pending union
```

即使仍保留 generation 快速判断，第 5 步也不能省略。

### R1.3 提交前 source digest 复核

增加可独立测试的 helper，例如：

```cpp
ValidateCandidateSourcesStillCurrent(...)
```

它必须覆盖 candidate 的：

- 所有 primary source
- 全部传递 include
- 文件缺失/删除
- M_ candidate 对应的生成 include 内容
- frozen snapshot 中实际参与编译的 digest

结果不一致时：

- 不调用 `CommitArtifacts`
- 不创建或不提交 live Pipeline
- 不刷新 resolved draw references
- 不修改正式 manifest
- 输出 generation、source identity、captured/current digest 和 discard reason

### R1.4 修复 frozen include

`TrackingIncluder::GetInclude()` 必须先查询 `sourceSnapshot`：

```text
snapshot 有 identity -> 直接使用 snapshot bytes
snapshot 无 identity -> 才读取磁盘
```

捕获后的 include 即使被删除、替换或短暂锁定，worker 也只能编译捕获时的 immutable bytes。
当前磁盘是否已经更新由提交前 digest 复核负责。

### R1.5 manual/auto 协调

- manual reload 必须使更旧的 auto generation 失效。
- manual reload 成功或失败后，FileMonitor baseline 与 pending source 集合不能吞掉之后的新保存。
- manual `all` 不得清除尚未稳定或刚刚稳定的自动变化。
- M_ change 必须使依赖其生成 include 的旧 in-flight candidate 失效。

### R1 验收测试

扩展 `--shader-auto-reload-test` 或增加等价串行入口，必须可重复控制 worker 编译窗口。
可以使用测试 gate/condition variable 或明确的 compile delay fault injection，禁止依赖机器快慢碰运气。

- [x] A 编译在途时保存 B，A 完成后不得提交。
- [x] A1 编译在途时稳定保存 A2，再保存 A3，只允许 A3 提交。
- [x] X 编译在途时先修改独立 source Y，再修改独立 source Z；下一批必须包含 Y+Z。
- [x] 一个公共 include 与一个叶子 shader 连续变化，两者真实依赖者都更新。
- [x] include 在捕获后被删除，旧 candidate 不提交，下一稳定批明确失败且旧 Pipeline 保持。
- [x] 只改 mtime、不改内容，不增加 source generation，不调用 shaderc。
- [x] stale discard 前后 manifest digest、Pipeline generation 和 resolved draw generation 不变。
- [x] manual reload 与 auto in-flight 交错时，旧 auto 结果不能覆盖 manual 结果。
- [x] 单线程和双线程模式均通过。

R1 完成后立即运行 Debug/Release 构建、shader core/integration tests 和自动 reload runtime test。
失败未修复前不得进入 R2。

## R2：实现真正的 MaterialInstance live 状态迁移

### R2.1 增加不可变 CPU 状态快照

为 MaterialInstance 增加只读快照 DTO，不暴露可写内部 map：

```text
MaterialInstanceStateSnapshot
    normalized MI asset identity
    typed parameter values
    texture bindings
    optional source asset identity / texture cache identity
```

参数值建议使用明确 variant：

```cpp
std::variant<float, Eigen::Vector2f, Eigen::Vector3f, Eigen::Vector4f>
```

贴图快照必须让旧 `shared_ptr<Texture>` 在事务 prepare/rollback 窗口内继续存活。不得只保存
裸 Vulkan handle 或 `vk::DescriptorImageInfo`。

### R2.2 在新 schema 上执行迁移

迁移顺序：

```text
1. 按新 M_ + MI JSON 构造并验证新 MaterialDescriptorSchema
2. 创建新 MI CPU 默认状态
3. 按 normalized MI asset identity 查找旧 snapshot
4. 迁移同名同类型参数
5. 迁移合同兼容的同名贴图
6. 新字段保留新 resolved/default value
7. 对完整新 MI 再做 schema + active shader binding 校验
8. 创建新 UBO/descriptor package
```

任何迁移或 descriptor 创建失败都只销毁 candidate，不修改旧 MI。

### R2.3 M_ include 也必须事务化

把 include 生成拆成：

```text
BuildGeneratedIncludeContent(M_.json) -> immutable bytes
CommitGeneratedIncludeIfChanged(bytes) -> atomic write
```

prepare 阶段使用 candidate include bytes 进入 frozen source overlay，不要先覆盖正式
`shader/glsl/generate/` 文件。

以下全部成功后，才允许提交正式 include：

- 所有 M_ JSON 解析
- 所有 candidate include 内容生成
- Shader 编译/反射/schema 校验
- Material/MI/descriptor package 创建
- RenderGraph pass contract 校验
- World/runtime transaction 已进入 no-throw commit 边界

如果一个 M_ 失败：

- 不继续排队 World rebind
- 不提交其他 M_ include
- 不改变 manifest
- 不改变旧 runtime
- 输出失败资产和 batch id

### R2 验收测试

扩展 `--shader-definition-reload-test`：

- [x] 初始 MI 参数改为与 JSON 不同的 runtime 值。
- [x] 初始 MI 贴图改为另一份兼容 live Texture。
- [x] M_ 新增参数后，旧参数 runtime 值保持，新参数取得新默认值。
- [x] 旧贴图对象/资产身份保持，descriptor package 为新 layout 创建。
- [x] 删除 schema 字段时，已删除字段不进入新 MI，其他 runtime 值保持。
- [x] 同名参数类型改变时整批失败，旧 MI 值、贴图、Pipeline、descriptor、World generation 不变。
- [x] 新增必需贴图但无可解析默认值时整批失败。
- [x] M_ JSON 损坏或 include 生成失败时不排队 World reload。
- [x] 多个 M_ 同批变化时 all-or-nothing。
- [x] 失败后修复文件，下一稳定 generation 可以成功恢复。
- [x] 旧 Material、MI、Texture 和 descriptor package 最终经 GPU epoch 退休并 drain。

R2 完成后立即运行 Debug/Release、单线程/双线程 definition reload tests。失败未修复前不得
进入 R3。

## R3：建立 World/RenderGraph prepare + commit 事务

### R3.1 禁止 prepare 阶段修改 active owner

World reload/M_ rebind 的 prepare 阶段不得调用：

- `WorldManager::ActivateLoadedWorld`
- `RenderSystem::SetActiveWorld`
- `Controller::SetViewTarget`
- 清空 active `RendererResourceCache`
- `RenderGraph::Shutdown` 当前 live graph

候选必须存在独立 staging owner 中。

### R3.2 World-local resource package

把 active cache 的原地清空/重建改成候选包，具体类型名可按代码库风格选择：

```text
PreparedWorldLocalResources
    textures
    materials
    materialInstances
    renderableObjects
    objectResources
    world textures
    owner generation
```

loader 在 staging package/context 中解析和创建资源。只有最终 commit 才把 active package 与
candidate package swap。旧 package 整包进入 `ResourceRetireQueue`。

禁止把 candidate 一项项提前写入 singleton active map。

### R3.3 RenderGraph state/package

把 RenderGraph 可替换状态收敛到可 staging 的 package：

```text
RenderGraphState
    CompiledRenderGraph
    resourcesMsaa/resourcesResolve
    renderpasses/renderpassesOrdered
    canonical shadow pass
    descriptor plan/cache/package
    pass material bindings
```

提供语义等价接口：

```text
PrepareReload(json, backend, candidate material bindings)
ValidatePreparedReload(...)
CommitPreparedReload(...) noexcept
RetireOldState(...)
```

`PrepareReload` 在临时 state 中完成：

- graph JSON compile
- attachment/render pass/framebuffer 创建
- pass material pipeline contract 校验
- descriptor layout/pool/set 创建与写入
- RenderSystem graph-dependent candidate 引用解析

任一步失败，只清理 candidate state，live graph 不变。

### R3.4 Runtime binding 预检

在修改 active runtime 前预检：

- candidate World handle 有效
- candidate view target 可锁定
- Controller 存在
- RenderSystem 能为 candidate World 构建/验证 resolved scene
- candidate graph/pass material 输入完整
- candidate World generation 与资源 owner generation 一致

`BindActiveWorldRuntimeObjects()` 至少要先完成全部校验，再执行修改。更推荐把它拆成
`PrepareRuntimeBinding` 和 `CommitRuntimeBinding noexcept`。

### R3.5 最终 commit 顺序

在 `WaitForRenderThreadIdle()` 安全点完成：

```text
1. 最后一次验证 candidate generation/source digests
2. 提交必要的原子文件/artifact
3. swap RendererResourceCache world-local package
4. swap RenderGraph state/package
5. activate candidate World
6. swap RenderSystem active/resolved references
7. 设置 Controller view target
8. 发布新 render snapshot
9. 将旧 World/resource/graph/descriptor package 放入 retire queue
```

第 3-8 步必须为预验证后的 `noexcept` ownership move/swap。若现有接口可能抛异常，把可失败
工作移到 prepare，不能用 catch 后继续运行半切换状态。

### R3.6 独立 graph reload 和 resize

- graph-only reload 必须使用同一 `PrepareReload -> Commit` 路径。
- graph reload 失败时旧 graph 仍可继续渲染。
- resize 可以保留现有 GPU idle 策略，但 graph/swapchain-dependent package 创建失败时不得
  返回主循环继续使用部分新状态；要么可恢复旧完整 package，要么明确终止运行。
- minimize/zero drawable size 不创建空资源，也不破坏当前 package。

### R3 验收测试

增加确定性的 fault injection，不依赖真实 OOM：

- [x] candidate World 构建完成后、commit 前注入失败，旧 active World 不变。
- [x] view target/runtime binding 预检失败，RenderSystem/Controller 都未改变。
- [x] graph resource 创建第 N 项失败，旧 graph generation 和 GPU handles 仍工作。
- [x] render pass/framebuffer 创建失败，旧 graph 仍可继续至少渲染若干帧。
- [x] pass material contract 或 descriptor 创建失败，旧 pass material bindings 不变。
- [x] M_ schema rebuild 成功但 graph rebind 失败，旧 World/Material/MI/Pipeline/descriptor 全部保持。
- [x] 成功事务中所有 owner generation 同时前进，不存在新 World + 旧 graph 或旧 World + 新 cache。
- [x] rollback candidate 资源无泄漏。
- [x] 成功后旧整包进入 retire queue，并在 completed epoch 后 drain。
- [x] world reload、graph reload、resize、minimize、shader reload 交错运行无悬空引用。

建议新增可重复 runtime 入口：

```text
--world-graph-transaction-test
```

或者扩展现有 rollback tests，但必须能分别覆盖“资源加载前失败”和“World 已构建后 graph/runtime
prepare 失败”，不能只覆盖当前已有的 scene/material/mesh/texture load failure。

R3 完成后立即运行全部 world/graph/resize rollback 与 stress tests。失败未修复前不得进入 R4。

## R4：提交边界、shutdown 和文档收口

### R4.1 强制 no-throw live swap

正式 artifact 提交后的所有 live swap 必须由类型系统保证不抛异常：

- `Material::CommitPipelineReload`
- `ComputePipelineReloadParticipant::CommitReplacement`
- `UiOverlayReloadParticipant::CommitReplacement`
- `PipelineFactory::PublishValidatedGraphicsShaderVariantArtifacts`
- prepared graph/world/resource package commit
- prepared retire queue enqueue

给接口和 override 增加 `noexcept`，并保证调用链内部只有预留容量后的 move/swap。若实现无法
满足 `noexcept`，必须调整提交顺序或增加完整回滚，禁止只靠注释声称 no-throw。

### R4.2 shutdown-in-flight

增加正式测试：

```text
1. 用 gate 阻塞 worker candidate compile
2. 请求应用 shutdown
3. 释放 gate
4. 验证 worker join
5. 验证 completed candidate 被丢弃
6. 验证 Vulkan teardown 后无回调、无 manifest/live commit
```

建议新增：

```text
--shader-shutdown-inflight-test
```

测试需分别运行 `workerThreadCount=1` 和 `2`。检查正常进程退出，不得只检查测试状态在
EngineLoop 内变成 succeeded。

### R4.3 回归旧 teardown 证据

针对 `build/teardown-novalidation.log` 的旧 heap after-free/SIGTRAP：

- 使用最新 Debug 二进制重复 shutdown-in-flight 和短 frame smoke。
- 如能复现，修复根因并增加回归断言。
- 如无法复现，在最终报告记录旧二进制时间、最新二进制时间、重复次数和日志位置。
- 不得把“当前没复现”写成已经证明不存在。

### R4.4 正式文档同步

代码和测试全部通过后再更新：

- `documents/rendering/shader-hot-reload.md`
- `documents/rendering/shader-build-cache.md`
- `documents/plan/rendering/shader-incremental-compile-and-hot-reload-plan.md`
- `README.md`
- `AGENTS.md`
- `documents/architecture/vulkanlearn-architecture.html`
- `documents/README.md`

文档必须明确：

- observed source epoch、stable event 和 commit-time digest validation 的区别
- pending source union 规则
- M_ live 参数/贴图迁移规则
- World/RenderGraph staging 与 no-throw commit
- graph/runtime prepare 失败的回滚语义
- shutdown-in-flight 验证入口

完成后把本文状态改为“已完成”，保留为审核修复历史；稳定合同只留在
`documents/rendering/` 和架构文档中。

## 强制验证顺序

### 1. Baseline

```powershell
cmake --build build -j 4
cmake --build build-release -j 4
ctest --test-dir build -R "shader_build_(core|integration)" --output-on-failure
ctest --test-dir build-release -R "shader_build_(core|integration)" --output-on-failure
```

### 2. R1

```powershell
build/bin/main.exe --shader-auto-reload-test --exit-after-tests --no-dev-ui
build-release/bin/main.exe --shader-auto-reload-test --exit-after-tests --no-dev-ui
```

运行单线程和双线程配置，并确认测试包含真正的 worker in-flight 保存。

### 3. R2

```powershell
build/bin/main.exe --shader-definition-reload-test --exit-after-tests --no-dev-ui
build-release/bin/main.exe --shader-definition-reload-test --exit-after-tests --no-dev-ui
```

### 4. R3

```powershell
build/bin/main.exe --reloadstress scenes/SC_uds_mountain_range.json 20 --exit-after-tests --no-dev-ui
build/bin/main.exe --graphreloadstress 6 --exit-after-tests --no-dev-ui
build/bin/main.exe --resizestress 6 --exit-after-tests --no-dev-ui
build/bin/main.exe --framesmoke 120 --exit-after-tests --no-dev-ui
```

同时运行新增的 World/Graph rollback fault-injection test。

### 5. R4 和完整 Shader 矩阵

```powershell
build/bin/main.exe --shader-reload-test --exit-after-tests --no-dev-ui
build/bin/main.exe --shader-auto-reload-test --exit-after-tests --no-dev-ui
build/bin/main.exe --shader-compute-reload-test --exit-after-tests --no-dev-ui
build/bin/main.exe --shader-definition-reload-test --exit-after-tests --no-dev-ui
build/bin/main.exe --shader-ui-reload-test --exit-after-tests --no-dev-ui
```

Debug/Release、单线程/双线程全部串行执行。运行时测试共享 `shader/spv/`，禁止并行。

### 6. 启动缓存回归

最终至少验证：

- [x] cold/force rebuild
- [x] 连续两次 warm start，第二次 `compiled=0, shaderc=0`
- [x] 叶子 Shader 修改
- [x] 公共 include 修改
- [x] Composer Base 修改
- [x] Composer Shadow 修改
- [x] 缺失 runtime/debug pair 任一产物
- [x] 损坏 manifest
- [x] cache schema/compile policy 升级
- [x] dirty Shader 启动编译失败

## 日志判定

每个 runtime test 保存独立日志，并扫描：

```text
VUID-
Validation Error
data race
use-after-free
heap corruption
after it was freed
SIGTRAP
device lost
```

成功日志至少应包含：

- cache hit/miss 与 shaderc 次数
- observed/stable source epoch
- pending source identities
- stale generation discard 原因
- candidate source digest 复核结果
- reload batch 和 owner 类型
- Pipeline/descriptor/world/graph commit generation
- rollback 原因
- retired resource owner、epoch 和 pending/drain 数量

## 检查点报告格式

每个 R 阶段完成后报告：

```text
阶段：
完成内容：
关键代码路径：
新增/修改测试：
运行命令：
结果：
validation/retire 诊断：
剩余工作：
阻塞情况：
```

不得把以下做法当作完成：

- 用全量 Shader 重编译掩盖 dependency/generation 错误
- 用每次 `device.waitIdle()` 实现热更
- 删除 `shader/spv/` 规避缓存问题
- 关闭 validation
- 忽略、覆盖或清空 pending source 事件
- 在失败后静默使用部分 candidate 状态
- 只新增接口、注释、测试 fixture 或演示路径
- 只证明 happy path 成功，不注入 commit 前失败

## 最终验收证据

最终报告必须逐项给出：

1. 修改文件列表。
2. R1 stale generation 和多 source coalescing 的确定性测试证据。
3. R2 runtime 参数/贴图迁移前后值和资源身份证据。
4. R3 每个 fault injection 点的 old-state fingerprint 保持证据。
5. World/graph/resource/package 成功提交时 generation 同步前进证据。
6. Pipeline、descriptor、材质、graph 旧资源的 epoch retirement/drain 证据。
7. Debug/Release、单线程/双线程完整命令和退出码。
8. warm start `shaderc=0` 证据。
9. validation/UAF/data-race/heap 扫描结果。
10. shutdown-in-flight 正常进程退出证据。
11. 正式文档迁移和索引同步证据。
12. 已知限制；如果没有，明确写“无已知未关闭的本专项限制”。
