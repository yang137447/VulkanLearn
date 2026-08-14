# Shader Build Cache Contract

## Status

- 类型：当前渲染契约
- 落地状态：已实现
- 上游规划：`documents/plan/rendering/shader-incremental-compile-and-hot-reload-plan.md`（P1 稳定后迁入本文）
- 关联：`documents/rendering/shader-hot-reload.md`

本文描述跨启动 Shader 增量编译的身份、缓存 manifest、提交语义和失败语义。运行
时热重载复用同一套身份与 manifest，但提交语义见 `shader-hot-reload.md`。

## 身份模型

```text
LogicalBuildKey
    决定“这是哪一份编译目标”
SourceFingerprint
    决定“当前全部编译输入是哪一版”
ArtifactGenerationKey
    LogicalBuildId + SourceFingerprint + 输出摘要 + ABI 摘要
    决定“这份产物属于哪个有效代际”
```

- `LogicalBuildId = BLAKE3-256(kind + normalizedKey)`。normalizedKey 只含逻辑
  语义（stage/pair、shader 名、归一化宏、render mode、shading model、pass、
  vertex factory、target environment），不含源码内容。
- `SourceFingerprint` 覆盖 cache schema、compile policy、target environment、
  优化模式、debug reflection 策略、stage 主源码 identity+digest、排序后的传递
  include identity+digest。
- `ArtifactGenerationKey` 进一步混合输出角色摘要与 ABI fingerprint，因此同一
  逻辑变体在源码或 ABI 变化后会得到新代际。

## 稳定哈希

- 使用固定锁版官方 BLAKE3 C 实现（`extern/BLAKE3/`），完整 256-bit 输出。
- 内存为 32 字节 digest；manifest、日志使用 64 字符小写十六进制。
- 业务层只通过 `VL::ContentHasher` / `VL::CanonicalFieldHasher` 使用哈希，不直接
  依赖 BLAKE3 C API。
- 规范化字段流写 type-tag、定长 little-endian 长度、字段名、字段值，禁止无边界
  字符串拼接。
- 禁止 `std::hash`、单纯时间戳、截断 digest 作为持久化身份。
- 对文件原始字节计算，不做换行符/BOM 归一化。

## Manifest

- 路径：`shader/spv/shader-build-cache.json`
- 字段：`schemaVersion`、`hashAlgorithm`、`compilePolicy`、`targetEnvironment`、
  `artifacts`。
- 每个 artifact 记录：logicalBuildId、kind、normalizedKey、sourceFingerprint、
  primarySources、dependencies（含 requestingSources 与最小 include 深度）、
  outputs（路径相对 shader root + digest）、abiFingerprint。
- Material Composer 的 Base 与 ShadowDepth pair 都是独立、真实的 manifest-backed
  artifact；它们按各自模板、生成源码、依赖和输出摘要命中或失效。
- `shader/spv/variants.json` 仍是人类可读变体注册表，不承担增量正确性。

## Cache Hit 校验顺序

1. manifest 可解析且 schema/hashAlgorithm/compilePolicy/target 一致。
2. logicalBuildId 与 kind/normalizedKey 一致。
3. 主源码 digest 与记录一致。
4. 每个记录依赖仍存在且 digest 一致（BLAKE3-256 重新计算）。
5. 输出集合、路径和逐文件 digest 与记录一致。
6. 从 debug SPIR-V 重新反射，ABI fingerprint 与记录一致。

任何一步失败都是 cache miss，并输出具体原因（artifact missing、compile policy
changed、dependency changed、output digest mismatch、ABI fingerprint changed 等）。

## 提交语义

- runtime/debug Graphics Pair 是一份完整 artifact：两个 stage 全部成功、反射
  成功、schema 校验成功后才一起提交。
- `CommitArtifactsWithAdditionalFiles` 用进程内 publication mutex 覆盖完整发布事务：
  路径预检、cache-hit 复核、临时写/替换、提交后摘要校验、manifest commit 和失败回滚
  不会与同一 `ShaderCompiler` 的另一发布交错。
- 任何写入前先按规范化物理路径检查整个 batch。检查范围包含 cache miss 输出、
  cache hit 的正式输出以及 candidate generated include；即使两个逻辑 artifact 中有
  一个是 cache hit，输出别名也会被拒绝。
- cache hit 在参与混合发布时必须重新读取正式输出并复核捕获 digest。文件消失或摘要
  已变化会在任何写入前拒绝整批，其他待发布输出和 manifest 保持字节不变。
- 先写同目录临时文件，再原子替换正式输出，最后提交 manifest。manifest 是缓存
  有效性的最终提交标记。
- 完成原子替换后仍逐一重算正式输出 digest；post-write verification 失败与
  manifest commit 失败都走同一输出回滚路径。
- `CommitArtifacts` 在输出替换或 manifest 提交失败时回滚输出文件，正式
  artifact 保持上一个成功代际。
- 进程内 `PipelineFactory` 的 artifact 缓存按逻辑 key 索引，但命中时必须重新
  确认当前磁盘代际；代际不一致时替换为当前产物。

## 启动失败语义

- 启动阶段 dirty 编译失败即启动失败，禁止因磁盘残留旧 SPIR-V 静默使用过期产物。
- 运行时热重载失败采用回滚语义，旧 Pipeline 与旧 manifest 代际保持不变。

## 编译策略版本

- Debug：runtime 与 reflection 均带 debug info + `ENABLE_DEBUG_VIEW`，同一 stage
  一次 shaderc 调用。
- Release：runtime 优化无 debug info，reflection 额外启用 `ENABLE_DEBUG_VIEW` 与
  debug info，同一 stage 两次 shaderc 调用。
- `compilePolicy` 内含 `configuration=`、`reflection=` 与 `abi=ShaderAbiSignatureV2`；
  ABI 描述格式变化必须递增该版本，触发全量重建。

## 诊断

启动摘要：

```text
Shader build: entries=29, artifacts=17, hits=17, misses=0, compiled=0,
shaderc=0, failed=0, elapsedMs=47.6
```

- 每个 cache miss 输出原因与逻辑 key。
- `--shader-force-rebuild` 强制重建全部启动 artifact。
- 运行时 `shadercache stats` 输出上次启动统计与当前 manifest artifact 数。

## 文件责任

- `source/shader/build/contentHash.*`：BLAKE3-256 封装与规范化字段流。
- `source/shader/build/atomicFile.*`：同目录临时写、原子替换、按需重写。
- `source/shader/build/shaderBuildManifest.*`：manifest 读写与反向依赖索引。
- `source/shaderCompiler.*`：编译请求、依赖收集、缓存命中、原子提交。
- `source/material/generator/materialParameterIncludeGenerator.*`：M_ include
  生成，内容不变时不重写。
