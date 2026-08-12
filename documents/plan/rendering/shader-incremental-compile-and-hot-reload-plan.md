# Shader 增量编译与热重载专项计划

## 文档状态

- 类型：当前专项计划
- 日期：2026-08-11
- 状态：规划，尚未实现
- 范围：GLSL 源码、生成材质 include、shaderc 编译、SPIR-V 缓存、反射、Graphics/Compute Pipeline 更新与安全退休
- 第一目标：启动阶段只编译缺失或过期的 Shader 产物
- 第二目标：运行时支持不改变 Shader ABI 的 Graphics Shader 安全热重载
- 非当前契约：本文描述的缓存 manifest、运行时命令和热重载接口在实现完成前不能被资产或其他模块依赖

本文档是 VulkanLearn 的项目专项实施计划，不替代以下当前合同：

- `documents/rendering/material-param-authoring-and-reflection.md`
- `documents/rendering/material-mesh-pass-composition.md`
- `documents/rendering/descriptor-imageinfo-management.md`
- `documents/architecture/vulkanlearn-architecture.html`

如果后续阶段完成实现，应把稳定下来的行为迁入 `documents/rendering/` 或
`documents/architecture/`，本文保留实施过程、阶段边界和历史决策。

## 背景

当前启动阶段执行以下流程：

```text
MaterialParameterIncludeGenerator::GenerateAllIncludes
    -> ShaderCompiler::StartCompile
    -> RendererBackendVulkan 初始化
    -> PipelineFactory 创建
    -> RenderGraph / World / Material 加载
```

现有编译链有三类入口：

1. `ShaderCompiler::StartCompile()` 递归扫描 `shader/glsl/`，编译所有 `.vert`、
   `.frag` 和 `.comp` 入口文件。
2. `ShaderCompiler::EnsureGraphicsVariantCompiled()` 根据 `ShaderVariantKey` 编译普通
   Graphics Shader Pair。
3. `ShaderCompiler::EnsureMaterialGraphicsVariantCompiled()` 编译 Material Composer
   装配后的 Base/ShadowDepth Shader Pair。

当前实现已经具备以下基础：

- shaderc 进程内编译
- 相对引用者目录解析 `#include`
- runtime SPIR-V 与 debug reflection SPIR-V 双产物
- Graphics Shader Variant 规范化 key
- Material Composer 完整源码装配
- SPIRV-Reflect descriptor binding 反射
- `PipelineFactory` 进程内 artifact/pipeline 缓存
- `Material` 对 Surface/Shadow Pipeline 的 `shared_ptr` 所有权
- EngineLoop 的 Render Thread idle 边界
- `ResourceRetireQueue` 基于 frame epoch 的延迟释放

但当前不存在跨启动的编译有效性判断。只要进入启动或首次请求某个 variant，就会重新
调用 shaderc 并覆盖 SPIR-V 文件。`PipelineFactory` 的缓存也只在当前进程内有效。

## 当前问题

### 1. 全量启动编译

`ShaderCompiler::StartCompile()` 对所有入口文件无条件编译。Release 构建还需要额外生成
启用 Debug View 和 debug info 的 reflection SPIR-V，因此同一入口可能发生两次编译。

### 2. Ensure 不是磁盘缓存 Ensure

两个 `Ensure*Compiled()` 接口目前只是统一编译入口，并不会判断现有产物是否仍然有效。
只有 `PipelineFactory` 会在单次进程生命周期内避免重复准备同一个 variant。

### 3. 持久化身份不稳定

当前 `ShaderVariantKey::GetVariantHash()` 和
`MaterialShaderCompileRequest::GetRequestHash()` 使用 `std::hash<std::string>`。

`std::hash` 可以用于单进程内 `unordered_map`，但标准不保证其跨编译器、标准库版本或
程序版本稳定，不应作为长期磁盘缓存和产物文件名的正式身份算法。

### 4. 没有传递 include 依赖

当前 `Include::GetInclude()` 只读取内容并返回给 shaderc，不记录：

- 入口文件包含了哪些文件
- include 文件又传递包含了哪些文件
- 一个公共 include 变化后会影响哪些 Shader Variant

因此无法只失效真正受影响的编译单元。

### 5. 生成 include 每次重写

`MaterialParameterIncludeGenerator::GenerateAllIncludes()` 每次启动扫描全部 `M_*.json`，
`GenerateInclude()` 即使生成内容没有变化也会重写目标文件。

这会制造无意义的文件修改时间变化，也会让未来 FileMonitor 重复触发。

### 6. Pipeline 缓存没有源码代际

当前 Graphics Pipeline key 使用逻辑 Shader artifact key。Shader 源码改变但宏、
render mode、shading model 等逻辑 identity 不变时，现有缓存仍会认为它是同一份 artifact。

热重载需要同时表达：

```text
Logical Shader Variant
    表示“这是哪个 Shader”

Artifact Generation
    表示“这个 Shader 当前是哪一版源码产物”
```

### 7. Material 缺少完整重建配方

`Material` 当前保存 Shader Variant、Material Feature、Descriptor Schema、Pass Contract 和
创建后的 Pipeline，但没有完整保存所有 Pipeline 创建输入和 Material Composer 请求。

热重载不能只替换 SPIR-V 文件，还必须能够从现有 Material 恢复出一份可重复执行的
Pipeline build request。

## 目标

### 增量编译目标

- 未修改 Shader 的连续两次启动不调用 shaderc。
- 修改叶子 `.vert/.frag/.comp` 时，只重新编译对应入口和受影响 variant。
- 修改公共 `.glsl` include 时，只重新编译传递依赖它的入口和 variant。
- 修改 Material Composer template/function 时，只失效受影响的组合 Shader。
- 编译选项、目标环境或缓存 schema 变化时，可靠地触发重编译。
- 删除、缺失或不完整的产物能够自动恢复。
- Debug 和 Release reflection 产物继续满足当前反射合同。
- 输出清晰的 cache hit、cache miss、编译数量与耗时诊断。

### 热重载目标

- Shader 编译失败时继续使用旧 Pipeline。
- 新 Shader 完成编译、反射和 Pipeline 创建后才允许提交替换。
- V1 只接受 Shader ABI 完全兼容的代码修改。
- 单线程和 `workerThreadCount == 2` 模式使用同一套安全提交语义。
- 旧 Pipeline 在 GPU 不再使用前保持存活。
- 修改一个公共 include 时，同一批受影响的 live variant 事务式更新。
- World reload、RenderGraph reload、resize 和 shutdown 不持有已失效 Pipeline 引用。

## 非目标

以下内容不属于第一轮实现：

- Shader Graph 或可视化材质编辑器
- 独立 Asset Daemon、远程编译或分布式编译
- 多进程同时写同一份 `shader/spv/` 缓存
- 自动修复不兼容 descriptor、vertex input 或 render target 合同
- V1 运行时热重载 `M_*.json` 后完整重建 Material/MI descriptor 资源
- V1 后台线程创建 Vulkan Pipeline
- V1 热重载 UI Overlay 自有 Pipeline
- V1 热重载环境生成器等长期持有的 Compute Pipeline
- 直接编辑 `shader/spv/` 作为热重载输入
- 把文件时间戳作为最终正确性依据

## 核心决策

### 1. 增量编译与热重载分阶段实现

两者共享依赖图、内容哈希和 artifact cache，但运行时风险不同：

- 增量编译解决“是否需要调用 shaderc”。
- 热重载解决“新产物如何替换正在使用的 Vulkan 对象”。

第一阶段不改变运行时资源所有权，只优化启动和首次 variant 准备。热重载在缓存正确性
稳定后单独接入。

### 2. 使用 MD5 作为内容变更判定

V1 使用 MD5 计算 Shader 编译输入的稳定内容摘要。

理由：

- Shader 和 JSON/include 文件规模小，MD5 性能不是瓶颈。
- 128 位摘要足够处理本地非对抗性构建缓存。
- 结果稳定，适合写入磁盘 manifest 和诊断日志。
- 相比时间戳，不受 Git checkout、文件复制、时间戳精度和编辑器保存策略影响。
- MD5 的密码学碰撞弱点不影响本地 Shader cache invalidation 场景。

V1 规则：

- 对文件原始字节计算 MD5，不做换行符或 BOM 归一化。
- 换行符变化会触发一次保守重编译。
- 不使用 `std::hash` 生成持久化 ID。
- 不使用 SDL test 模块中的 MD5 作为引擎正式依赖。
- 增加独立、可测试的内容哈希工具。
- 时间戳和文件大小未来可以作为性能快速路径，但不能替代最终 MD5 判断。

### 3. 区分逻辑身份与源码代际

```text
LogicalBuildKey
    决定它是哪一个编译单元

SourceFingerprint
    决定当前全部编译输入是哪一版

ArtifactGeneration
    LogicalBuildKey + SourceFingerprint 对应的有效产物代际
```

逻辑身份变化表示新的 variant；源码代际变化表示同一个 variant 需要生成新产物和新
Pipeline。

### 4. `variants.json` 与 build cache manifest 分离

现有 `shader/spv/variants.json` 继续描述人类可读的 Shader Variant 注册信息，不承担
增量编译正确性。

新增：

```text
shader/spv/shader-build-cache.json
```

其职责仅包括：

- cache schema 和 compile policy 版本
- LogicalBuildKey 的稳定 ID
- SourceFingerprint
- 已记录依赖及其 MD5
- runtime/debug SPIR-V 输出路径和输出摘要
- 最近一次成功反射得到的 ABI 摘要

### 5. 启动失败与热重载失败语义不同

启动阶段保持当前严格语义：

```text
缓存 miss / dirty
    -> 编译失败
    -> 启动失败
```

不能因为磁盘上残留旧 SPIR-V 就静默使用过期产物。

运行时热重载采用回滚语义：

```text
热重载编译 / 反射 / Pipeline 创建失败
    -> 丢弃候选结果
    -> 保留当前 Pipeline
    -> 输出诊断
    -> 等待下一次修改后重试
```

### 6. V1 热重载只接受 ABI 完全一致

只修改 Shader 内部逻辑时，可以复用现有 descriptor package、Material Instance 和 draw
资源。任何 ABI 差异在 V1 都拒绝提交，而不是尝试局部修补。

### 7. CPU 编译和 Vulkan 提交分离

最终线程模型：

```text
CPU 编译阶段
    读取文件、生成源码、shaderc、反射、构建候选描述

Engine/Render 安全提交阶段
    创建 Vulkan Pipeline、替换运行时引用、退休旧资源
```

V1 可先同步执行 CPU 编译以建立正确性基线。后续异步化只移动纯 CPU 阶段，不改变提交
事务和资源所有权。

## 术语

### Shader Entry

可独立传给 shaderc 的 `.vert`、`.frag` 或 `.comp` 文件。

### Shader Pair

由 vertex 和 fragment 两个 stage 组成的 Graphics 编译目标。

### Composed Shader Pair

由 Material Composer 使用 pass template、material evaluation module、vertex factory 和
parameter include 装配出的完整 vertex/fragment 源码。

### LogicalBuildKey

一份编译目标的稳定逻辑描述。至少包含：

```text
build kind
stage 或 graphics pair
source/virtual source identity
normalized macros
render mode
shading model
material pass
vertex factory
Vulkan target
compile policy version
```

### SourceFingerprint

LogicalBuildKey 对应的全部真实编译输入内容摘要。

### Shader ABI Signature

从新旧 SPIR-V 反射结果生成的稳定接口描述，用于判断是否允许热替换。

### Live Variant

当前被 Material、RenderGraph Pass Material、Compute owner 或 UI owner 持有并可能参与后续
帧渲染的 Shader Variant。

## Shader 类型与第一阶段覆盖范围

| Shader 类型 | 当前入口 | 启动增量编译 | 热重载 V1 |
| --- | --- | --- | --- |
| 独立 `.vert/.frag` | `StartCompile()` | 支持 | PipelineFactory 管理的 Material Graphics 支持 |
| 独立 `.comp` | `StartCompile()` / `ComputePipeline` | 支持 | 暂不支持 |
| 普通 Graphics Variant | `EnsureGraphicsVariantCompiled()` | 支持 | 支持 |
| Material Composer Pair | `EnsureMaterialGraphicsVariantCompiled()` | 支持 | 支持 |
| RenderGraph Pass Material | Material/PipelineFactory | 支持 | 随 Material Graphics 支持 |
| Material ShadowDepth | Material Composer / Shadow builder | 支持 | 与 Surface 同批事务更新 |
| UI Overlay | 自有 Pipeline 创建逻辑 | 支持编译缓存 | 暂不支持运行时替换 |

## 总体结构

```text
Material Include Generation
    -> write-if-changed

Shader Build Request
    -> LogicalBuildKey
    -> Previous Dependency Snapshot
    -> MD5 Validation
          |
          +-- cache hit --> load artifact metadata
          |
          +-- cache miss
                 -> shaderc + dependency collection
                 -> runtime/debug SPIR-V
                 -> reflection
                 -> temporary artifact write
                 -> manifest commit

Runtime Reload Request
    -> changed source paths
    -> reverse dependency lookup
    -> affected live variants
    -> compile all candidates
    -> ABI validation
    -> create all candidate pipelines
    -> render-thread safe point
    -> swap all live references
    -> retire old pipelines
```

## 编译身份设计

### 普通 Stage Entry

LogicalBuildKey 示例：

```text
kind=StageEntry
path=pass/toneMapping.frag
stage=fragment
macros=[]
target=Vulkan1.4
policy=ShaderCompilePolicyV1
```

### 普通 Graphics Variant

LogicalBuildKey 示例：

```text
kind=GraphicsVariant
shaderName=unlit
renderMode=Opaque
shadingModel=Unlit
macros=[...normalized...]
target=Vulkan1.4
policy=ShaderCompilePolicyV1
```

### Material Composer Variant

LogicalBuildKey 使用现有 `MaterialShaderCompileRequest::GetNormalizedKey()` 的语义输入，
但持久化 ID 改为稳定 MD5：

```text
logicalId = MD5(normalizedLogicalKey)
```

源码内容不进入 logical ID，而进入 SourceFingerprint。这样源码变化后仍能识别为同一个
逻辑 variant 的新代际。

## SourceFingerprint 设计

SourceFingerprint 对一份规范化输入流计算 MD5：

```text
MD5(
    cacheSchemaVersion
    + compilePolicyVersion
    + targetEnvironment
    + optimizationMode
    + debugReflectionPolicy
    + shaderStage
    + normalizedMacros
    + primarySourceIdentity
    + primarySourceMD5
    + sortedDependencyPathAndMD5
)
```

依赖条目格式：

```text
normalized/path/to/include.glsl:<md5>
```

规则：

- 路径相对 `shader/glsl/` 保存。
- 路径使用 `/` 分隔符。
- 依赖按规范化路径排序后进入 fingerprint。
- 同一路径只记录一次。
- 主源码单独记录，同时也允许出现在统一依赖集合中。
- 宏先按当前项目规则规范化，再参与 fingerprint。
- Material Composer 生成的完整 vertex/fragment 字符串直接计算 MD5。
- Composer 源码中的递归 include 继续由 Includer 收集。

## 依赖收集与失效

### 编译时收集

扩展现有 shaderc Includer，使每次 include 成功读取后记录：

- 规范化源路径
- 文件原始内容 MD5
- requesting source
- include depth

ShaderCompiler 完成后返回 Dependency Snapshot，不让 Includer 直接写全局 manifest。

### 缓存命中检查

已有成功 manifest 时：

1. 重新计算主源码 MD5。
2. 按 manifest 中记录的依赖路径重新计算每个依赖 MD5。
3. 比较 compile policy 和 LogicalBuildKey。
4. 检查全部 runtime/debug 输出存在。
5. 检查输出摘要与 manifest 一致。
6. 全部一致才允许 cache hit。

如果主源码或任一旧依赖改变，就重新编译并重新收集依赖。因此 include 指令新增、删除或
改变后，新的依赖集合会在本次成功编译后替换旧记录。

### 反向依赖索引

运行时从 manifest 和本次进程编译结果构建：

```text
source path -> logical build IDs
```

公共 include 改变时，通过该索引取得全部受影响编译单元。

未被当前进程使用的 artifact 只标记 dirty，后续按需准备时再编译；当前 live variant 才进入
热重载事务。

## Cache Manifest 草案

```json
{
  "schemaVersion": 1,
  "compilePolicy": "ShaderCompilePolicyV1",
  "targetEnvironment": "Vulkan1.4",
  "artifacts": {
    "<logicalBuildId>": {
      "normalizedKey": "...",
      "sourceFingerprint": "<md5>",
      "dependencies": [
        {
          "path": "common/lighting.glsl",
          "md5": "<md5>"
        }
      ],
      "outputs": {
        "runtimeVertex": {
          "path": "...",
          "md5": "<md5>"
        },
        "runtimeFragment": {
          "path": "...",
          "md5": "<md5>"
        },
        "debugVertex": {
          "path": "...",
          "md5": "<md5>"
        },
        "debugFragment": {
          "path": "...",
          "md5": "<md5>"
        }
      },
      "abiFingerprint": "<stable-md5>"
    }
  }
}
```

Stage Entry 和 Compute artifact 只记录自身实际存在的输出字段。

## 产物提交语义

一份 Graphics Pair 的以下结果视为一个整体：

- runtime vertex SPIR-V
- runtime fragment SPIR-V
- debug vertex SPIR-V
- debug fragment SPIR-V
- dependency snapshot
- reflection result
- ABI fingerprint

提交规则：

1. shaderc 先把结果保存在内存。
2. runtime/debug 两个 stage 全部编译成功。
3. reflection 和当前 Material Schema 校验成功。
4. 写入同目录临时文件。
5. 临时文件完成后替换正式输出。
6. 最后更新 manifest。

manifest 是缓存有效性的最终提交标记。进程异常导致输出和 manifest 不一致时，下次启动通过
输出 MD5 不一致发现问题并重新编译。

启动编译可以在严格失败语义下完成临时文件替换并最后提交 manifest。热重载候选则必须继续
保存在内存或候选临时文件中；在 Pipeline 创建和批次提交成功前，不能覆盖当前正式输出，
也不能更新当前有效 manifest。否则运行时回滚后，磁盘缓存会错误地声称新代际已经生效。

## 生成 Material Include

`MaterialParameterIncludeGenerator` 改为：

```text
读取 M_*.json
    -> 构建目标 GLSL 字符串
    -> 比较目标文件当前内容
    -> 内容相同：不写文件
    -> 内容不同：原子替换文件
```

FileMonitor 只观察 source-of-truth：

- 手写 `.vert/.frag/.comp/.glsl`
- 后续支持的 `M_*.json`

不直接观察：

- `shader/glsl/**/generate/`
- `shader/spv/`
- 临时输出

这样 `M_*.json` 修改只产生一次上游事件，不会因生成 include 再触发第二次 reload。

## 启动增量编译流程

第一阶段保持当前启动顺序和“扫描全部入口”的行为，只改变每个入口是否实际调用 shaderc：

```text
GenerateAllIncludes(write-if-changed)
    -> Load shader-build-cache.json
    -> Scan shader/glsl entry files
    -> Build LogicalBuildKey
    -> Validate source/dependency MD5
       -> hit: skip shaderc
       -> miss: compile and commit
    -> continue renderer initialization
```

第一阶段不立即切换为完全 lazy compile，避免一次同时改变：

- Compute Shader 可用时机
- UI Overlay Shader 可用时机
- RenderGraph Pass Material 加载顺序
- 启动失败语义

增量链稳定后，可以评估把普通 Graphics Variant 改为纯按需，并保留显式全量验证命令。

## 热重载 V1 边界

### 支持的修改

- 函数内部计算逻辑
- 光照、颜色、采样和控制流
- 已存在 UBO/texture binding 的使用方式
- 不改变接口的公共 include
- Material Composer template/function 的内部逻辑

### 拒绝的修改

- 新增、删除或移动 descriptor set/binding
- 改变 descriptor type 或 array count
- 改变 descriptor stage visibility
- 改变 UBO block size、member offset、member size、member type 或 member name
- 改变 push constant range
- 改变 vertex input location/type
- 改变 fragment output location/type
- 改变 specialization constant 合同
- 修改 `M_*.json` 导致 Material Descriptor Schema 或静态 Feature 路由变化
- 修改 RenderPass/RenderGraph compatibility

V1 拒绝不兼容更新时只输出 ABI diff，不销毁或修改当前运行时资源。

## Shader ABI Signature

当前 reflection 主要覆盖 descriptor binding。热重载 V1 前需要补齐稳定 ABI 描述，至少包含：

```text
descriptor set / binding / type / count / stage flags
UBO block size
UBO member name / offset / size / type
push constant offset / size / stage flags
vertex input location / type
fragment output location / type
specialization constant id / type
```

生成 ABI fingerprint 前按 set、binding、location 或 constant id 排序，避免枚举顺序影响结果。

Material Set 1 仍然先通过 `MaterialDescriptorSchema::ValidateShaderBindings()` 验证。ABI 比较不是
替代现有 schema，而是比较新旧运行时 Pipeline 接口是否完全一致。

## 热重载事务

### 触发方式

P2 先增加手动调试命令，不立即增加自动文件监听：

```text
shaderreload changed
shaderreload all
shadercache stats
```

命令通过现有 RuntimeCommand 队列进入 EngineLoop，不从控制台线程直接操作 Shader 或 Vulkan
对象。

### 一次 Reload Batch

```text
1. 扫描当前内容 MD5，得到 changed source paths
2. 通过反向依赖索引找出 affected logical variants
3. 过滤当前 live variants
4. 为全部 live variants 生成候选源码和编译请求
5. 编译全部 runtime/debug SPIR-V
6. 反射并生成 ABI signature
7. 校验新旧 ABI 完全一致
8. 为全部 variant 构建候选 Pipeline
9. 等待 Render Thread idle 安全点
10. 一次性替换全部 live references
11. 把旧 Pipeline 放入 ResourceRetireQueue
12. 提交 artifact manifest
```

批次默认 all-or-nothing。公共 include 同时影响 Surface、ShadowDepth 或多个 Material 时，不能
只提交部分成功 variant，否则同一套共享代码会在一帧中出现混合代际。

互不相关的 changed dependency component 后续可以拆成多个事务，V1 不做此优化。

### Pipeline 创建与切换

CPU 编译成功不代表热重载成功。只有以下条件全部满足才提交：

- 所有 SPIR-V 成功
- 所有 reflection 成功
- 所有 Material schema 校验成功
- 所有 ABI 完全兼容
- 所有 Vulkan Pipeline 创建成功
- Render Thread 已到达安全点

Pipeline 切换不能在 command buffer 正在录制或提交过程中修改其引用。

P2 为保证正确性，可以在 `WaitForRenderThreadIdle()` 后同步创建候选 Pipeline。后续若 profiling
证明 Pipeline 创建停顿明显，再设计带外部同步的后台 Pipeline Cache 和创建线程。

### 旧 Pipeline 退休

Render Thread idle 只表示 CPU 侧不再录制当前帧，不表示 GPU 已经执行完所有已提交命令。

替换后：

```text
old shared_ptr<PipelineBase>
    -> ResourceRetireQueue::RetireShared
    -> last used frame epoch complete
    -> shared_ptr 释放
    -> GraphicsPipeline destructor 销毁 Vulkan 对象
```

不能在 swap 后立即析构旧 Pipeline，也不应为每次 Shader reload 调用全局 device idle。

## 运行时对象调整

### GraphicsShaderVariantArtifact

增加：

- `logicalBuildId`
- `sourceFingerprint`
- `artifactGenerationKey`
- runtime/debug SPIR-V 内存结果或可验证路径
- `ShaderAbiSignature`

`normalizedKey` 继续表达逻辑 variant，不表达源码代际。

### PipelineFactory

需要支持：

- artifact cache hit 时同时比较 source fingerprint
- 显式准备候选 artifact，不立即覆盖 active artifact
- Graphics Pipeline key 包含 artifact generation
- 为 reload batch 创建候选 Pipeline
- commit/rollback candidate artifact 与 Pipeline
- 查询逻辑 variant 当前 active generation

不能简单 `erase()` 当前缓存后原地重建，因为 live Material 仍可能持有旧 Pipeline。

### Material

需要保存或引用不可变 Pipeline rebuild recipe，至少覆盖：

- Surface Shader compile request
- ShadowDepth Shader compile request 或公共 Shadow 路由
- Pass Pipeline Contract
- Material Descriptor Schema
- cull/blend 固定状态
- Graphics Pipeline Layout override
- 当前 active Surface/Shadow Pipeline generation

替换接口应一次提交 Surface 和 Shadow 结果，更新 `activeShaderBindings`，并返回需要退休的旧
Pipeline 引用。

### Resolved Draw References

当前 Shadow 计划已经要求 shader reload 失效 Surface/Shadow variant 和 resolved draw
references。专项实现需要确认 Draw Packet/Snapshot 是：

- 每帧从 Material 读取 Pipeline；或
- 保存了 Pipeline 快照，需要在提交新 generation 时重新发布。

不能只替换 `Material::renderPipeline`，却让 Render Thread Snapshot 长期持有旧的裸 Vulkan
handle 或旧 Pipeline pointer。

## Compute 与 UI 后续处理

### Compute Pipeline

Compute Shader 通常由环境生成器或 baker 长期持有 `shared_ptr<ComputePipeline>`。支持热重载前
需要明确每个 owner 的：

- descriptor layout compatibility
- descriptor set rebuild 规则
- dispatch 录制安全点
- Pipeline 替换接口

P4 再纳入，不阻塞 Graphics Material Shader V1。

### UI Overlay

UI Overlay 当前不完全通过 `PipelineFactory` 管理。P4 需要让它实现统一 reload participant
接口，或者迁移到 PipelineFactory artifact/pipeline 管理路径。

## FileMonitor 设计

P3 增加自动监听。Windows-first V1 优先采用简单、可调试的轮询，而不是立即引入
`ReadDirectoryChangesW`：

```text
每 200~500ms 扫描 source-of-truth 文件
    -> 比较缓存的 size/mtime 快速候选
    -> 对候选计算 MD5
    -> debounce
    -> 提交稳定 changed path 集合
```

MD5 仍是最终变更判定。mtime/size 只减少不必要的文件读取。

监听规则：

- 忽略 `shader/spv/`
- 忽略 `shader/glsl/**/generate/`
- 忽略临时文件和编辑器交换文件
- 同一路径在 debounce 窗口内只保留最后一次状态
- 文件删除、重命名和新增都形成明确事件
- 文件仍在写入或暂时不可读时延迟到下一轮，不提交半写内容

## 多线程边界

### P1/P2

- Shader 编译仍由 EngineLoop 所在线程同步执行。
- RuntimeCommand 只在 EngineLoop 更新边界消费。
- Vulkan Pipeline 创建和 live 引用替换发生在 Render Thread idle 后。
- manifest 写入由单一 coordinator 串行执行。

### P3

- FileMonitor 和 shaderc 编译可进入独立 CPU worker。
- 每个编译任务使用独立 `shaderc::Compiler` 和 CompileOptions。
- worker 不访问 `Material`、`PipelineFactory` 的 live cache 或 Vulkan device。
- worker 只产生不可变 candidate batch。
- EngineLoop 消费 candidate batch 并完成 Vulkan 创建、commit 或 rollback。

项目当前仍要求运行时测试串行执行，因为多个进程共享 `shader/spv/` 输出。跨进程文件锁和
共享 cache 协调不在本专项 V1 范围内。

## 诊断设计

### 启动摘要

```text
Shader build: entries=24, hits=21, compiled=3, failed=0, elapsed=84ms
```

每个 cache miss 至少报告原因：

- artifact missing
- manifest missing/corrupt
- schema version changed
- compile policy changed
- primary source changed
- dependency changed
- output missing
- output digest mismatch

### 热重载摘要

```text
Shader reload batch 7:
  changedSources=1
  affectedVariants=6
  liveVariants=4
  compiled=4
  pipelinesCreated=4
  committed=true
  retiredPipelines=4
  elapsed=132ms
```

ABI 拒绝必须输出精确差异，例如：

```text
Shader reload rejected: M_speedtree.Base
  Set 1 Binding 3 descriptor count: 1 -> 4
  Vertex input location 5: vec4 -> vec3
Current pipeline remains active.
```

## 实施阶段

### P1：稳定哈希与启动增量编译

目标：不改变运行时资源行为，连续启动跳过未修改 Shader。

- [ ] 增加独立 MD5 内容哈希工具和测试。
- [ ] 用稳定 MD5 替换持久化 variant/request 文件名中的 `std::hash`。
- [ ] 为旧 `std::hash` 产物提供一次自然重建，不做长期双格式兼容。
- [ ] 扩展 shaderc Includer，返回 Dependency Snapshot。
- [ ] 增加 `shader-build-cache.json` 读写和 schema version。
- [ ] 实现 Stage Entry、普通 Graphics Variant、Composer Variant 的统一 cache lookup。
- [ ] 把 runtime/debug Graphics Pair 作为一个提交单元。
- [ ] 保存并验证输出文件 MD5。
- [ ] Material include 生成改为 write-if-changed。
- [ ] 保持启动 dirty compile 失败即失败。
- [ ] 输出 cache hit/miss 和耗时摘要。
- [ ] 保留显式 force rebuild 入口，用于验证全部 Shader。

验收：

- 连续两次无修改启动，第二次 `compiled=0`。
- 修改一个叶子 fragment，只重编译对应入口/variant。
- 修改公共 include，只重编译真实依赖者。
- 删除任一 runtime/debug 产物后自动重建完整编译单元。
- 修改 compile policy version 后全部相关 artifact 重建。

### P2：手动、ABI 兼容的 Graphics 热重载

目标：通过控制台命令安全更新 PipelineFactory 管理的 Material Graphics Pipeline。

- [ ] 扩展 reflection，生成完整 Shader ABI Signature。
- [ ] 给 Graphics artifact 增加 source generation。
- [ ] Pipeline key 纳入 artifact generation。
- [ ] 定义不可变 Graphics Pipeline rebuild recipe。
- [ ] Material 保存 Surface/Shadow reload 所需配方。
- [ ] 增加 live variant/reload participant 注册。
- [ ] 增加 source path 反向依赖索引。
- [ ] 增加 `shaderreload changed/all` RuntimeCommand。
- [ ] 编译、反射、ABI 检查全部候选。
- [ ] 在 Render Thread idle 后创建全部候选 Pipeline。
- [ ] 批次 all-or-nothing 替换 Surface/Shadow Pipeline。
- [ ] 失效或重新发布 resolved draw references。
- [ ] 旧 Pipeline 进入 ResourceRetireQueue。
- [ ] 失败时保留旧 Pipeline 和旧 manifest generation。

验收：

- 修改颜色计算后不重启即可看到结果。
- 保存语法错误时画面继续使用旧 Pipeline。
- 修复错误再次保存后可以成功更新。
- descriptor binding 变化被拒绝并输出 ABI diff。
- Surface/Shadow 共同依赖改变时同批提交。
- 两线程模式连续 reload 无 data race、use-after-free 或 validation error。

### P3：FileMonitor 与异步 CPU 编译

目标：自动发现变化，避免 shaderc 阻塞正常帧循环。

- [ ] 增加轮询 FileMonitor。
- [ ] 增加 debounce 和文件稳定性判断。
- [ ] 忽略生成目录、SPIR-V 目录和临时文件。
- [ ] 增加独立 Shader Compile Worker。
- [ ] worker 输出不可变 candidate batch。
- [ ] EngineLoop 在更新边界消费完成 batch。
- [ ] 增加 reload 请求合并和过时代际丢弃。
- [ ] 同一 Shader 新修改到达时，不提交较旧的异步编译结果。
- [ ] 增加编译中、成功、失败状态到开发 UI/诊断系统。

验收：

- 编辑器一次保存产生多个文件事件时只编译一次。
- 编译期间渲染继续使用旧 Pipeline。
- 连续快速保存时只允许最新 source generation 提交。
- shutdown 会停止 worker，不在 Vulkan teardown 后回调提交。

### P4：Compute、UI 与 Layout-changing Reload

目标：扩展覆盖面，并支持需要重建绑定资源的高级更新。

- [ ] 为 Compute owner 定义 reload participant 和 descriptor rebuild recipe。
- [ ] 接入环境生成器、IBL baker 等 Compute Pipeline。
- [ ] UI Overlay 接入统一 artifact/reload 管理。
- [ ] `M_*.json` 变化触发单材质 include/schema rebuild。
- [ ] descriptor layout 变化时创建新的 descriptor package。
- [ ] MaterialInstance 参数/贴图按新 schema 迁移和校验。
- [ ] RenderGraph Pass Material 输入变化时重新校验 pass contract。
- [ ] 以新旧资源包事务切换，旧包进入 ResourceRetireQueue。

这一阶段必须单独设计，不在 P2 中通过临时判断逐步放宽 ABI 限制。

## 预计文件改动

P1 主要涉及：

- `source/shaderCompiler.h`
- `source/shaderCompiler.cpp`
- `source/shaderVariant.h`
- `source/material/compiler/materialShaderCompileRequest.h`
- `source/material/generator/materialParameterIncludeGenerator.cpp`
- 新增稳定哈希、依赖快照和 build cache manifest 文件

P2 主要涉及：

- `source/pipeline/graphicsShaderVariantArtifact.h`
- `source/pipeline/pipelineFactory.h`
- `source/pipeline/pipelineFactory.cpp`
- `source/pipeline/shaderReflectionService.h`
- `source/pipeline/shaderReflectionService.cpp`
- `source/shaderReflect.h`
- `source/shaderReflect.cpp`
- `source/material.h`
- `source/material.cpp`
- `source/render/shadow/materialShadowPipelineBuilder.*`
- `source/engine/runtimeCommand.*`
- `source/engine/runtimeCommandExecutor.*`
- `source/engine/engineLoop.*`
- `source/debugConsole.cpp`

P3/P4 再新增 FileMonitor、compile worker 和其他 reload participant，不提前把这些职责塞进
`ShaderCompiler` 或 `EngineLoop`。

## 测试矩阵

| 用例 | 预期 |
| --- | --- |
| 无修改连续启动 | 第二次全部 cache hit，不调用 shaderc |
| 修改叶子 `.frag` | 只重编译对应入口和引用 variant |
| 修改 `common/lighting.glsl` | 只重编译传递依赖 lighting 的 variant |
| 修改 Composer Base template | 所有 live Base composed variant dirty，ShadowDepth 不误伤 |
| 修改共享 material function | 依赖它的 Base/Shadow variant 同批更新 |
| Material include 生成内容不变 | 文件不重写，不触发 reload |
| 删除 debug SPIR-V | 完整 artifact 重建，reflection 正常 |
| manifest 损坏 | 报告原因并安全全量重建 |
| cache schema 升级 | 旧记录统一失效 |
| 启动 dirty Shader 编译失败 | 启动失败，不使用旧 artifact |
| 热重载编译失败 | 旧 Pipeline 保持，下一次可重试 |
| 热重载 ABI 变化 | 拒绝提交并输出精确 diff |
| 热重载 Pipeline 创建失败 | 候选全部回滚，旧 Pipeline 保持 |
| 公共 include 影响多个 live variant | all-or-nothing 提交 |
| 快速连续保存 | 旧 generation 结果不能覆盖新 generation |
| 单线程模式连续 reload | 无悬空引用和 Vulkan validation error |
| 两线程模式连续 reload | 安全点正确，无 GT/RT data race |
| reload 后 world reload | 新 Pipeline generation 正常进入新 world |
| reload 后 graph reload | compatibility 相同可重建/复用，无旧引用泄漏 |
| reload 后 resize/minimize | Pipeline 生命周期和 framebuffer/render pass 合同正确 |
| shutdown 时存在编译任务 | worker 停止，候选清理，不访问已销毁 device |

## 风险与约束

### 1. `std::hash` 迁移改变现有产物路径

旧 variant artifact 会成为孤立生成文件。P1 允许自然重建，新 manifest 不读取旧格式。
清理策略只处理明确属于旧缓存的文件，不进行模糊目录删除。

### 2. 公共 include 造成大批量重编译

依赖图只能避免无关 Shader 重编译，不能避免真正依赖公共文件的所有 variant 重编译。
P3 通过异步 CPU 编译降低帧阻塞，不能通过漏编译换性能。

### 3. Debug reflection 与 runtime SPIR-V 宏不同

Release 当前会为 reflection 额外启用 `ENABLE_DEBUG_VIEW`。缓存 key 和 compile policy 必须明确
记录该规则，不能只按主源码 MD5 判断两个输出都有效。

### 4. PipelineFactory 弱缓存和 Material 强所有权

旧 Pipeline 可能同时被 Material、draw snapshot 和 retire queue 持有。替换设计必须明确每个
强引用来源，不能假设从 PipelineFactory cache erase 后对象就会析构。

### 5. Render Thread idle 不等于 GPU idle

CPU 安全点负责停止新引用产生，GPU 完成 epoch 负责最终销毁。两者缺一不可。

### 6. RenderPass compatibility

Shader reload 不负责修改 attachment、sample count、color output count 或其他 RenderGraph
合同。fragment output 改变在 V1 直接拒绝。

### 7. 多进程共享输出

当前运行时测试已经要求串行执行。P1 的原子写入可以避免单进程半文件，但不解决两个进程
同时提交 manifest 的最后写入覆盖问题。

## 文档跟进

P1 实现完成后：

- 新增 `documents/rendering/shader-build-cache.md`，记录正式缓存 key、manifest schema 和失败语义。
- 更新 `README.md` 的启动说明，注明 clean/miss 与 warm/hit 行为。

P2/P3 实现完成后：

- 新增 `documents/rendering/shader-hot-reload.md`，记录正式运行时命令、ABI 边界和安全提交合同。
- 更新 `documents/architecture/vulkanlearn-architecture.html` 的线程/数据流和资源生命周期。
- 更新 `AGENTS.md`，说明 Shader 修改后的增量编译、热重载和生成产物规则。

## 一句话结论

本专项的核心不是“监听文件后重新调用 shaderc”，而是建立一条可验证的事务链：

```text
稳定逻辑身份
    + MD5 内容与传递依赖
    + 原子 artifact cache
    + 完整 Shader ABI 比较
    + Pipeline 候选提交
    + Render Thread 安全切换
    + GPU epoch 延迟退休
```

实施顺序固定为：先保证启动增量编译正确，再支持手动兼容热重载，最后增加自动监听、异步
编译和 layout-changing 资源重建。
