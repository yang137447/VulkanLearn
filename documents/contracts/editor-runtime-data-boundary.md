# Editor / Runtime 数据边界

> 状态：**当前契约**。runtime/editor 已按 CMake 目标拆分：
> `main_game` 只链接 `vl_runtime`，`main_editor` 链接 `vl_editor` /
> `vl_editor_asset_core`；新代码按本契约执行。

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
3. **runtime 结构不使用条件编译的 editor 字段。**
   编辑器与运行时的区分由 CMake 目标所有权完成（`main_editor`/`vl_editor` 依赖
   `vl_runtime`）；仓库不提供 `VL_WITH_EDITOR` / `VL_EDITOR_ONLY` 宏。
   编辑器专用字段一律放到 `source/editor/**` 的独立 DTO 或服务中。
4. **快照只发布 runtime 数据。**
   `WorldSnapshot`、`RenderScene`、GPU 结构不得携带编辑器草稿或 UI 状态。
   编辑器需要额外信息时，单独构建 editor snapshot，不扩大 runtime 快照。
5. **新增的粒子 runtime 不包含 `source/editor/**`。**
   `source/particles/**` 不得 include 编辑器头文件；architecture audit 已强制
   这一条。其它 runtime 模块同样由 CMake 源过滤与 configure 期 include 门禁约束。
6. **authoring JSON 不是 runtime 文档。**
   loader 在提交前把 JSON 冻结为 runtime 值。粒子 `ParticleSystemAsset`
   中的 `authoringJson` 只作为运行时 Data Channel / user parameter 重解析所需
   的最小参数模板保留，不承载 UI、draft、history 或编辑器选择状态。
7. **新引擎扩展先定义 runtime 语义。**
   例如 billboard 光照只把 `billboardLighting` / `billboardAmbient` 这类运行
   时行为字段放进 emitter asset；编辑器字段暴露、预览和验证反馈留在 editor
   模块。不得为了编辑器方便把 editor-only 字段塞进 asset 结构。

## 构建边界

- 编辑器目标 `main_editor` 链接 `vl_editor`、`vl_editor_asset_core`；纯运行时目标
  `main_game` 只链接 `vl_runtime`，两者不共享任何编译期编辑器宏。
- 当前已从 `source/particles/**` 移出 `particleModuleEdit.h`、
  `particleCurveEdit.h`、`particleEmitterEdit.h`、`particleFlipbookEdit.h`；
  runtime loader 只消费展平后的 `ParticleEmitterAsset`。这些 editor headers
  只放在 `source/editor/particles/**`，由 `vl_editor` 编译。
- `tool/architecture-boundary-audit.ps1` 的 `Particle runtime excludes editor`
  规则会拒绝 `source/particles/**` 中的 `#include "editor/`；新增粒子运行时
  依赖必须通过纯数据接口，而不是回引编辑器实现。

## 当前例外

运行时目标已由 CMake 源过滤和 configure 期 include 门禁守住；
`source/world/world.h` 仍持有编辑器预览用的 `editorCamera` /
`editorViewEnabled` 字段，这是当前唯一的 World 级编辑器状态例外。新增代码
不得扩大该例外；需要编辑器数据时使用稳定 runtime identity，在
`source/editor/**` 建独立 DTO 或服务。

## 评审清单

- runtime 头文件是否 include 了 `editor/`？
- 新字段是否真的在 runtime 模拟/渲染/脚本路径读取？
- 编辑器状态是否能用稳定 identity 外置，而不是嵌入 runtime 对象？
- 是否把 authoring JSON、draft、history、selection、thumbnail 带进了
  `WorldSnapshot` / `RenderScene` / GPU 结构？
- 编辑器字段是否可以外置到 `source/editor/**` 的 DTO/服务？
