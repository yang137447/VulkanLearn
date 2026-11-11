# Editor / Runtime 数据边界

> 状态：**当前契约**。本文件约束新增代码如何区分编辑器数据和运行时数据；
> 现有单体 `main` 仍同时链接编辑器与运行时，迁移是渐进的，但新代码按本契约执行。

## 目标

运行时对象只拥有模拟、渲染、脚本和网络实际需要的数据。编辑器需要的文档、
草稿、历史、选择、缩略图、面板状态、验证反馈不能混入 runtime 对象、快照或
GPU 结构。目标结构对齐 UE 的 `WITH_EDITORONLY_DATA` / editor module 思路：
runtime 可以独立存在，editor 是构建在其上的附加层。

## 术语

- **runtime data**：`World`、`WorldSnapshot`、`RenderScene`、粒子
  `ParticleEmitterAsset` / `ParticleSystemAsset`、GPU UBO/SSBO、渲染资源。
  这些结构在运行时构建中必须可独立编译和运行。
- **editor data**：JSON 文档、draft、undo/redo、selection、inspector 状态、
  thumbnail、资源库缓存、验证 UI 状态、面板布局。
- **authoring source**：磁盘上的 `SC_*.json` / `PS_*.json` / `MI_*.json`。
  它是输入格式，不是 runtime 对象；loader 把它冻结成 runtime 值。

## 规则

1. **runtime 对象不得保存编辑器所有权。**
   不把 `Document`、`History`、`Selection`、UI 指针或编辑器回调放进 `World`、
   `WorldSnapshot`、`RenderScene`、`ParticleEmitterAsset` 等 runtime 结构。
2. **编辑器状态放在 `source/editor/**`。**
   编辑器 DTO 以稳定的 runtime identity（scene path、object identity、asset
   path 等）为 key，通过服务/表格与 runtime 对象关联，而不是反向嵌入。
   粒子的模块栈、曲线、Flipbook 与发射器结构编辑 helper 已放在
   `source/editor/particles/**`；它们只操作作者 JSON，不属于 runtime。
3. **只在必要时使用编译期 editor 字段。**
   `source/core/editorBuild.h` 提供 `VL_WITH_EDITOR` 与 `VL_EDITOR_ONLY(...)`。
   只有字段确实必须与某个 runtime 对象同生命周期、且仅在编辑器构建中需要时，
   才把字段包在 `VL_EDITOR_ONLY` 中；优先使用独立 editor DTO。
4. **快照只发布 runtime 数据。**
   `WorldSnapshot`、`RenderScene`、GPU 结构不得携带编辑器草稿或 UI 状态。
   编辑器需要额外信息时，单独构建 editor snapshot，不扩大 runtime 快照。
5. **新增的粒子 runtime 不包含 `source/editor/**`。**
   `source/particles/**` 不得 include 编辑器头文件；architecture audit 已强制
   这一条。其它模块当前仍有单体依赖，见下文「迁移中的已知例外」；它们是
   迁移债务，不得成为新增代码的先例。
6. **authoring JSON 不是 runtime 文档。**
   loader 在提交前把 JSON 冻结为 runtime 值。粒子 `ParticleSystemAsset`
   中的 `authoringJson` 只作为运行时 Data Channel / user parameter 重解析所需
   的最小参数模板保留，不承载 UI、draft、history 或编辑器选择状态。
7. **新引擎扩展先定义 runtime 语义。**
   例如 billboard 光照只把 `billboardLighting` / `billboardAmbient` 这类运行
   时行为字段放进 emitter asset；编辑器字段暴露、预览和验证反馈留在 editor
   模块。不得为了编辑器方便把 editor-only 字段塞进 asset 结构。

## 构建边界

- 编辑器可执行文件 `main` 定义 `VL_WITH_EDITOR=1`。
- 编译编辑器实现的测试目标（`material_instance_editor_production_tests`、
  `editor_ui_tests`）也定义同一个宏；任何包含 `VL_EDITOR_ONLY` 结构体的
  runtime-only 目标必须保持宏为 0，避免布局不一致。
- 纯运行时目标不定义该宏，`VL_EDITOR_ONLY` 展开为空。
- 当前 `main` 仍是编辑器/运行时单体；完整 runtime-only target 拆分尚未完成，
  但新代码必须按上述边界设计，避免继续扩大混合面。
- 当前已从 `source/particles/**` 移出 `particleModuleEdit.h`、
  `particleCurveEdit.h`、`particleEmitterEdit.h`、`particleFlipbookEdit.h`；
  runtime loader 只消费展平后的 `ParticleEmitterAsset`。这些 editor headers
  用 `static_assert(VL_WITH_EDITOR != 0)` 拒绝被 runtime-only 目标包含。
- `tool/architecture-boundary-audit.ps1` 的 `Particle runtime excludes editor`
  规则会拒绝 `source/particles/**` 中的 `#include "editor/`；新增粒子运行时
  依赖必须通过纯数据接口，而不是回引编辑器实现。

## 迁移中的已知例外

当前仍有以下 runtime/world 单元反向依赖编辑器实现，完整拆分属于后续工作：

- `source/renderSystem.h` include editor selection/preview。
- `source/engine/engineLoop.cpp`、`source/engine/runtimeCommandExecutor.cpp`
  include editor scene/service/runtime。
- `source/world/world.h` 仍持有 `editorCamera` / `editorViewEnabled` 状态。

这些例外不改变本契约对新增粒子和后续新模块的要求：新代码不得继续扩大
反向依赖；需要编辑器数据时，用稳定 runtime identity 在 `source/editor/**`
建独立 DTO 或服务。

## 评审清单

- runtime 头文件是否 include 了 `editor/`？
- 新字段是否真的在 runtime 模拟/渲染/脚本路径读取？
- 编辑器状态是否能用稳定 identity 外置，而不是嵌入 runtime 对象？
- 是否把 authoring JSON、draft、history、selection、thumbnail 带进了
  `WorldSnapshot` / `RenderScene` / GPU 结构？
- `VL_EDITOR_ONLY` 字段是否有明确的“无法外置”的理由？
