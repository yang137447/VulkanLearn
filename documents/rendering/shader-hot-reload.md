# Shader Hot Reload Contract

## Status

- 类型：当前渲染契约
- 落地状态：已实现（P2/P3/P4，R1-R4 完成审核已关闭）
- 上游规划：`documents/plan/rendering/shader-incremental-compile-and-hot-reload-plan.md`
- 关联：`documents/rendering/shader-build-cache.md`

本文描述运行时 Shader 热重载的 ABI 边界、事务提交、异步编译、参与者和资源退休。

## 触发方式

控制台命令：

```text
shaderreload changed
shaderreload all
shadercache stats
```

命令通过 RuntimeCommand 队列进入 EngineLoop，再由 GT-owned
`ShaderReloadRuntime` 处理，不从控制台线程直接触碰 Shader 或 Vulkan 对象。

自动监听：

- `ShaderFileMonitor` 轮询 `shader/glsl` source-of-truth：
  `.vert/.frag/.comp/.glsl` 与 `M_*.json`。
- 忽略 `**/generate/`、`shader/spv/`、临时/编辑器交换文件。
- mtime/size 只做快速候选；BLAKE3-256 是最终变更判定。每 4 次扫描强制全量
  重哈希，兜底时间戳粒度或时间戳被还原的编辑。
- debounce：同一路径必须连续稳定指定次数才提交事件；文件不可读时延迟到下一轮。
- monitor 观察到内容过渡时推进 `latestObservedSourceEpoch`；只有通过 debounce 的
  stable event 才进入待处理集合。epoch 是过时结果的快速拒绝条件，不替代提交时
  source digest 复核。

## ABI 边界

`ShaderAbiSignature` 覆盖：

- descriptor set/binding/type/count/stage + UBO block size + member name/offset/size/type
- push constant offset/size/stage
- vertex input location/component/format/type
- fragment output location/component/format/type
- specialization constant id/type/name
- compute local workgroup size

指纹前按 set/binding、location、constant id 排序。

V1 策略：

- Material Surface/Shadow、Compute、UI 的原地管线替换只接受 ABI 完全兼容候选；
  拒绝时输出精确 diff，旧资源不变。
- Compute/UI 每次提交仍重建 descriptor pool/set 到新 layout 对象（旧 layout
  对象随旧管线退休）。
- `M_*.json` 的 Material Descriptor Schema 变化走独立事务（见下文），允许
  layout 变化并重建 Material/MI descriptor package。

## Reload 事务（Material Surface/Shadow）

```text
1. 扫描 changed source paths
2. 反向依赖索引得到受影响 logical builds
3. 捕获 live Material 的不可变 reload recipe 与 active artifact
4. 冻结当前源码快照到 build request（include 编译期间不再读盘）
5. CPU 编译全部候选 + 反射 + schema 校验 + ABI 校验
6. 提交前复核全部 primary/dependency 的当前 digest
7. 在 Render Thread idle 安全点创建全部候选 Pipeline
8. 事务性替换 Surface/Shadow 引用
9. 旧 Pipeline 进入 ResourceRetireQueue，等待 GPU frame epoch 完成
```

- Surface 与 ShadowDepth 按 reload batch all-or-nothing 提交。
- 编译、反射、schema、ABI、Pipeline 创建任一失败：丢弃候选，旧 Pipeline 与旧
  正式 artifact 保持。
- Pipeline 替换发生在 `WaitForRenderThreadIdle()` 安全点，禁止每次 `device.waitIdle()`。
- 提交后 `RefreshResolvedSceneAfterShaderReload` 重新发布 resolved draw 引用。

## 异步编译（FileMonitor + Compile Worker）

- `ShaderReloadRuntime` 拥有 `ShaderFileMonitor`、`ShaderCompileWorker`、
  pending source union、source epoch、generation 和自动/手动提交诊断状态。
  `EngineLoop` 只提供稳定 safe point、active World generation、resolved scene
  refresh 和 M_ World transaction host 操作。
- GT 侧由 `ShaderReloadRuntime` 捕获 plan（读 live 注册与 active artifact），worker
  只执行纯 CPU 编译并发布不可变 candidate batch。
- worker 不访问 Vulkan device、live Material/PipelineFactory cache 或 manifest。
- 每个编译任务使用独立 `shaderc::Compiler` 与 CompileOptions。
- stable source identity 进入 `pendingAutoReloadSources` 并求并集；worker 空闲时才按
  当前 live owner 捕获最新 plan。只有成功 submit 的 source 才从 pending union 移除，
  多个独立 source 不会因 plan replacement 丢失。
- `sourceEpoch`/generation 负责快速拒绝已被新观察或手动 reload 取代的结果；即使
  epoch 仍匹配，GT 在正式 artifact/live commit 前仍调用
  `ValidateCandidateSourcesStillCurrent` 复核全部 primary、传递 include 和候选生成
  include digest。任一不一致都丢弃 candidate，不修改 manifest、Pipeline 或 resolved
  draw generation。
- build request 携带 frozen `sourceSnapshot`；`TrackingIncluder` 优先读取快照字节，
  因而编译在途时 include 的修改、删除或短暂不可读不会混入该 candidate。
- shutdown 先停止并 join worker，再进入 Vulkan teardown；worker 无 Vulkan 回调。

## Compute Reload Participants

`ComputePipelineReloadParticipant`：

- 提供 shader name、active pipeline/artifact、ABI 校验、replacement pipeline
  创建、descriptor rebuild recipe、事务提交。
- 已接入 `ProceduralSkyCubeGenerator`（skyToCubemap）与 `EnvironmentIblBaker`
  （skySHGenerate、prefilterEnvMap），注册/注销随 RenderSystem 帧资源生命周期。
- descriptor package 的替换在新 pool/set 全部预分配成功后 swap；旧 pool/set 与
  旧 pipeline 统一进 GPU epoch 退休队列。

## UI Overlay Reload Participant

- `UiOverlayRendererVulkan` 实现统一 participant：同一次事务替换 straight 与
  premultiplied alpha 两个 pipeline 变体。
- 活跃 artifact 在 UI 初始化时由 `PipelineFactory::PrepareGraphicsShaderVariant`
  记录，不在捕获时从磁盘重新推导。

## M_*.json 定义热更新

```text
M_*.json 稳定变化
    -> 批量解析并生成不可变 candidate include overlay
    -> WorldGraphTransactionCoordinator 在独立 World-local resource package
       与 PreparedRenderGraphState 中编译/校验
    -> 保留合同兼容的 live MaterialInstance 参数与贴图状态
    -> 原子发布 artifact + generated include
    -> no-throw swap World/resource/graph/runtime owner
```

- prepare 阶段不覆盖正式 `shader/glsl/generate/` include；候选字节作为 source overlay
  进入 frozen 编译请求。全部 JSON、include、编译、反射、资源和绑定校验成功后，
  generated include 才与 artifact 一起原子发布。
- 新 schema 会重建 Surface/Shadow 编译请求、pipeline layout 与 MI descriptor package。
  同名同类型参数保留 live 值，合同兼容的同名贴图保留其 `shared_ptr<Texture>`/资产身份；
  新字段采用新 M_/MI resolved value 或默认值，已删除字段不迁移。
- 同名参数类型变化、贴图 binding 不兼容、未知字段或缺失必需值会拒绝整批。多个
  `M_*.json` 同批变化也是 all-or-nothing。
- RenderGraph Pass Material 在候选 graph 中重新校验 pipeline contract 与输入。
- 初始 World、运行时 World reload 和 M_ 定义 reload 都调用
  `WorldTransitionCoordinator::PrepareWorldLoad()`，再由
  `WorldGraphTransactionCoordinator` 协调 candidate graph、pipeline cache、
  runtime binding、正式文件发布、live owner swap 与 retire activation。旧的
  mutable cache snapshot/restore World 加载路径已删除。
- 启动在第一次事务前调用
  `RenderSystem::InitializeWorldTransactionResources()`，事务提交后再调用
  `FinalizeInitialRenderObjectInitialization()` 初始化 UI overlay。初始 candidate
  lazy 创建的 process-global BRDF LUT binding 会随 World-local package 一起发布；
  后续候选只能继承该稳定 identity，不能替换或删除现有 global binding。
- 任一 prepare/发布前步骤失败时，active World、World-local resources、RenderGraph、
  RenderSystem、Controller、正式 include/manifest 与旧材质整包均不变。
- 旧资源统一进入 ResourceRetireQueue。不满足“单材质”粒度的资源重建是可接受的
  事务实现：未受影响的 shader 变体走缓存命中，不重复 shaderc。

## 退休语义

- Render Thread idle 只停止新引用产生；GPU 完成 frame epoch 才允许销毁。
- 旧 `shared_ptr<PipelineBase>`、旧 descriptor pool/set、旧 UI pipeline/cache
  全部经 `ResourceRetireQueue::RetireShared/RetirePrepared` 延迟释放。
- live swap 前先准备 `PreparedResourceRetirements`；Compute/UI 的
  `PreparedRetiredResourcePackage` 保持 disarmed，只有所有 live 引用成功 swap 后才
  调用 `Activate()`。候选失败不会退休仍在使用的旧资源。
- 退休诊断含 owner generation、lastUsedEpoch、pending 数量。

## 诊断

热重载提交摘要：

```text
Shader reload batch 7: changedSources=1, affectedBuilds=6, liveMaterials=4,
compiled=4, shaderc=8, pipelinesCreated=4, committed=true, retiredPipelines=4
```

自动路径额外报告 `compileMs`、supersede/丢弃、worker 失败保留旧管线、以及
`retirePending`。

## 验证入口

```text
--shader-reload-test            手动 Graphics Surface/Shadow 回滚矩阵
--shader-auto-reload-test       FileMonitor + Compile Worker 矩阵
--shader-compute-reload-test    Compute participant 矩阵
--shader-definition-reload-test M_*.json schema 重建/回滚矩阵
--shader-ui-reload-test         UI Overlay pipeline pair 矩阵
--shader-shutdown-inflight-test worker 阻塞期间 shutdown/join/丢弃矩阵
--world-graph-transaction-test  World/Graph prepare fault/原子 commit 矩阵
```

全部使用 `--exit-after-tests`，退出码 0 成功、2 失败；测试共享 `shader/spv/`
输出，必须串行执行。
