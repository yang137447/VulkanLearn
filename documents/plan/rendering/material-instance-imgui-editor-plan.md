# Material Instance Asset Editor 开发计划

> **当前实施状态（2026-08-29）**：renderer-independent 的 MI 资产编辑基线已经落地，`renderMode`/`shadingModel`/`cullMode` 三项 render-state override 已接入命令、service、UI 快照和稀疏保存；renderer-owned preview 已支持这三项状态的隔离候选加载、variant/pass/material/pipeline 与对象 descriptor 重建、原子 live swap 和 GPU epoch 退休，数值 live preview 继续走 MaterialInstance 原地更新。纹理 live preview 尚未完成，`texture.open`、事件流和少数 runtime 路由仍有缺口。
>
> 本文件以下正文保留计划编写时的原始内容，不以删除或改写历史计划的方式更新状态。当前实现事实、已完成阶段和剩余门槛以文末“实施收尾记录”为准。

## 文档状态

- 原始状态（计划创建时）：规划，尚未实现
- 当前状态：部分落地；renderer-independent 资产编辑链、三项 render-state override 编辑、renderer-owned 数值 live preview 和三项状态的事务化 live preview 已具备；纹理 live preview 和少数 runtime 路由仍未完成
- 目标：在 Dear ImGui 开发者 UI 中提供 UE 风格的 MI 资产配置编辑器；场景只用于定位资产，资产文档负责编辑和保存，运行时 World 只提供可选预览。
- 首版范围：`float`、`vec2`、`vec3`、`vec4` 参数、MI texture slot 对已有 `T_*.json` 纹理资产的引用配置，以及 `renderMode`、`shadingModel`、`cullMode` 三项 MI render-state override；这些编辑均已有 renderer-free 代码路径。
- 当前契约：稳定的 UI/线程边界已迁移到 `documents/architecture/material-instance-editor.md`；本文保留原始设计、阶段目标和验收矩阵，并在文末记录实际落地状态。

正式合同现位于 `documents/architecture/material-instance-editor.md`，并继续遵循
`documents/architecture/game-ui-stack.md` 与 `documents/rendering/material-param-authoring-and-reflection.md`
中的宿主 UI、线程和参数真相源约束。

## 背景与基础

当前工程已具备以下能力：

- `M_*.json` 定义参数类型、默认值和 vector 可选的 `channels` 范围元数据；
- `MI_*.json` 只保存显式 override，有效参数按 `MI override > M_ default` 合成；
- `MaterialDescriptorSchema` 保存参数名、GLSL 类型、std140 layout 与通道信息；
- SPIR-V reflection 描述当前 Base/Shadow variant 实际使用的 Set 1 成员；
- `MaterialInstance` 支持 `float`、`vec2`、`vec3`、`vec4` 的读写；
- ImGui 运行在游戏线程，渲染线程只消费不可变 UI draw snapshot；
- 现有 V1 GT/RT 交接会在下一轮引擎资源修改前等待渲染线程完成。

计划创建时缺少的是从场景引用链定位 MI 资产、独立于 World 的资产文档会话、类型化预览桥接、纹理资产引用配置、稀疏保存、原子替换和外部修改冲突处理；本轮已落地其中的命令/codec、renderer-free 文档服务、纹理引用配置、稀疏保存和 UI facade，剩余边界见文末实施收尾记录。

## P0 目标

1. 从 3D 视口选择模型，在选中模型的材质槽列表中打开对应 `MI_*.json`。
2. 使用 UE Details 风格配置 MI 的数值 override 与 texture asset binding。
3. 编辑 `float`、`vec2`、`vec3`、`vec4`，并为 texture slot 选择已有 `T_*.json`。
4. 资产文档可以在 MI 未被当前 World 加载时独立打开、编辑、校验和保存。
5. 当前 World 存在同路径 live MI 时，可通过 Preview Bridge 实时预览草稿；预览失败不阻塞资产保存。
6. 显示当前导航上下文中引用该 MI 的场景对象、mesh/terrain 资产与 material slot。
7. 支持单项 Reset、Reset All、Revert、Save 与 `Ctrl+S`。
8. 以经过校验的稀疏 JSON 原子写回原 `MI_*.json`。
9. World 切换、双线程运行、预览失败、保存失败和外部文件修改时不访问旧资源、不损坏资产。
10. 所有资产导航、查询、草稿修改、校验、保存和预览都通过统一、可序列化的 Command 协议执行，使 ImGui、Console、自动化测试和 AI 共用同一入口。

## P0 非目标

- `T_*.json` 内部的 `source/colorSpace/mipmaps/filter/wrapMode` 编辑；该能力属于独立 Texture Asset Editor；
- 直接把 PNG/TGA/EXR 路径写入 MI；MI texture slot 只能引用 `T_*.json`；
- 宏、未列入支持范围的 Render State、直接 pipeline variant 编辑；`renderMode`、`shadingModel`、`cullMode` 的 MI override 与 renderer-owned live preview 已纳入本轮范围；
- `Make Unique`、复制 MI、对象引用改写；
- RenderGraph Pass MI、独立 preview scene、视口 picking、undo/redo；
- 依据名称把 vec3/vec4 猜测为颜色。颜色控件后续必须由 M_ 显式元数据声明；
- 全项目反向依赖数据库。首版只保证当前导航来源和当前已打开资产上下文的引用信息准确。

以下做法明确禁止：

- ImGui callback 直接修改 Asset Document、写文件或调用 live `MaterialInstance`；
- AI/Console 绕过 CommandBus 直接调用 editor service；
- 为 UI、Console、测试和 AI 分别实现不同的参数校验或保存路径；
- 把不可序列化指针、lambda 或 Vulkan handle 放入命令载荷。

## 真相源与所有权

### 资产引用链

场景对象不是 MI 编辑器的资产所有者。首版必须保留并展示实际引用来源：

```text
scene*.json object
  -> modelPath / terrainPath
  -> SM_*.json / TR_*.json asset
  -> mesh section / terrain material slot
  -> materialInstancePath
  -> MI_*.json
  -> M_*.json + T_*.json references
```

点击场景对象的语义是：

```text
EditorCommand.ResolveSceneMaterialAsset(scene object identity)
  -> EditorCommandResult(normalized MI path, reference origin)
  -> EditorCommand.OpenMaterialInstanceAsset(normalized MI path, reference origin)
```

它不是“选中一个 live `MaterialInstance` 指针进行配置”。同一个 MI 可以被多个对象、多个 section、
多个场景甚至当前未加载的资产引用；编辑器修改的是这份共享 MI 资产本身。

### 三层架构

#### Asset Navigation

- 通过查询命令从当前场景对象解析到 mesh/terrain 资产、slot 和 MI 路径；
- 选中模型材质列表通过 Open 命令按规范化路径打开对应 `MI_*.json`，不要求重复搜索全局资产；
- 导航结果携带只读 `reference origin`，用于面包屑、References 和返回源对象；
- Navigation 不加载 Vulkan 资源，不创建或修改 live MI。

#### Asset Document

- 以规范化 `MI_*.json` 路径作为唯一文档身份；
- 从磁盘读取原始 MI，解析其 M_ schema、默认参数和 texture slot；
- 拥有 baseline、working overrides、dirty、文件摘要、校验和 Save；
- 文档生命周期独立于 World、RenderGraph 和 renderer resource cache；
- Save 的正确性不依赖当前是否存在可预览的 live instance。
- Asset Document 只能由 Material Asset Command Executor 修改；Panel 和其他 producer 只读 snapshot/result。

#### Runtime Preview Bridge

- 只把 Asset Document 的 working draft 映射到当前 World 中同资产路径的 live MI；
- World identity、live instance、descriptor package 和 GPU epoch 只属于 Preview Bridge；
- 没有匹配 live MI 时显示 `Preview unavailable`，资产仍可编辑和保存；
- World 换代时断开旧 bridge，并按新 World 重新匹配，Asset Document 不关闭、不换身份；
- Preview Bridge 的任何失败都不能修改资产 baseline，也不能阻止 Save。
- Preview Bridge 只消费 Preview Command，不接受 UI 或 AI 的直接方法调用。

### 参数与纹理真相源

完整可编辑参数列表来自 M_ schema：

- schema 决定参数全集、类型、默认值与 vector 通道说明；
- MI 原始 JSON 和草稿决定显式 override；
- reflection 只标记当前 variant 的参数是 `Active` 还是 `Inactive`；
- inactive 参数仍可编辑与保存，因为可能被其他 Pass 或兼容 variant 使用。

编辑器不得用 reflection 代替 schema 生成参数列表，也不得重新实现一套 M_/MI 有效值合成规则。

纹理配置同样按资产职责分层：

- M_ schema 定义 texture slot 全集与默认 `T_*.json`；
- MI 文档只保存 texture slot 的显式 `T_*.json` override；
- `T_*.json` 决定原始图片 `source`、色彩空间、mipmap、filter 和 wrapMode；
- shader reflection 只说明当前 variant 实际使用哪些 texture binding；
- MI Asset Editor 不直接编辑原始图片路径和采样设置，只提供 `Open Texture Asset` 跳转。

Render-state 编辑使用 M_ 定义作为默认值来源：`shadingModel` 来自 M_ 根字段，
`renderMode`/`cullMode` 来自 M_ 的 `renderStates`。MI 只保存非默认的
`renderStateOverrides`，当前明确支持 `renderMode`、`shadingModel`、`cullMode`。
字段和值的合法性、默认值去重和稀疏化由共享 command/service/persistence 链路负责；
有效 `renderMode`/`shadingModel` 的组合约束由共享 material validator 提前执行，
pass/pipeline 的具体合同约束仍由 renderer 侧的 `MaterialInstanceValidator` 负责。

### 文档身份与预览身份

Asset Document 的稳定身份只有：

```text
normalized MI asset path
```

场景导航来源是附加上下文，不参与文档唯一身份。Preview Bridge 才使用：

```text
active World identity + normalized MI asset path + live resource generation
```

Asset Document 和 UI 不保存 `MaterialInstance*`、`Material*`、Vulkan handle、descriptor/buffer/texture
handle 或可变 resource-cache 引用。Preview Bridge 每次提交都按身份重新解析 live resource。

## 统一 Editor Command 协议

### 单一外部入口

MI Asset Editor 对外只暴露 `EditorCommandBus` 与只读结果/快照接口：

```text
ImGui / Console / AI / Runtime Test
  -> EditorCommandEnvelope
  -> EditorCommandBus
  -> MaterialEditorCommandExecutor
       -> AssetNavigationExecutor
       -> MaterialAssetDocumentExecutor
       -> MaterialPreviewCommandExecutor
  -> EditorCommandResult
  -> EditorEvent / EditorSnapshot
```

producer 只能构造命令和消费结果。所有实际状态变更，包括打开/关闭文档、选择参数、修改 override、
Reset、Revert、Save、Reload、连接预览和断开预览，都由 executor 完成。

这里的“所有操作”指所有有业务语义的查询和修改。ImGui 的 hover、滚动位置、窗口大小、折叠状态等
纯展示状态不进入业务 Command；AI 通过 List/Get/Resolve 查询获得同等业务信息，不需要模拟 UI 像素操作。

现有 `RuntimeCommand` 可继续承载运行时/渲染调试命令，但本编辑器不得再经 `UiAction -> RuntimeCommand`
二次翻译。ImGui、Console、自动化测试和 AI 应直接生产同一种 `EditorCommandEnvelope`。`CommandBus` 可扩展为
多 domain 队列，或增加独立 `EditorCommandBus`；无论实现形式如何，不能维护多套行为实现。

### Command-only 硬约束

- 所有有业务语义的操作都必须是 Command：查询、搜索、选择、打开、关闭、修改、Reset、Revert、Reload、Validate、Save、Preview 和获取结果；
- ImGui callback、Console parser、AI adapter 和测试 fixture 只能构造 Command，不能直接调用 executor/service、修改 document 或触碰 live renderer；
- Asset Document、Preview Bridge 和 Navigation 的实际状态只能由对应 Command Executor 修改；
- `List/Get/Resolve` 也必须经过 CommandBus；返回的 snapshot/result 是 producer 唯一可读取的业务状态；
- 搜索框文本、hover、滚动、停靠布局和折叠状态是纯展示状态，可以留在 ImGui 本地；搜索提交仍发送查询 Command；
- 同一 Command 在不同 producer 下必须得到相同的校验、revision、错误码和副作用；`source` 只用于审计；
- 未经 CommandResult 确认，producer 不得假设操作成功，也不能通过读取内部对象补猜结果。

### Command Envelope

建议命令公共字段：

```cpp
struct EditorCommandEnvelope
{
    uint32_t protocolVersion;
    uint64_t commandId;
    std::optional<uint64_t> correlationId;
    EditorCommandSource source;
    EditorCommandType type;
    std::optional<uint64_t> expectedDocumentRevision;
    EditorCommandPayload payload;
};
```

- `protocolVersion`：命令 JSON 和 payload schema 版本；不兼容版本在进入 executor 前拒绝；
- `commandId`：调用方生成或由入口分配，用于结果关联、日志、去重和 AI 重试；同一 ID 重复提交返回原结果而不重复执行；
- `correlationId`：可选上层任务/批处理 ID，用于将 AI 多步操作归并到同一审计链；
- `source`：`ImGui`、`Console`、`AI`、`RuntimeTest`，只用于审计/诊断，不改变行为；
- `type`：稳定的操作类型，不用自由文本决定执行逻辑；
- `expectedDocumentRevision`：写命令的乐观并发保护；revision 不匹配时拒绝旧命令；
- `payload`：可序列化的 typed value，不包含指针、callback 或 Vulkan 对象；
- 命令进入统一队列后按 owner 线程顺序执行；异步任务先返回受理结果，最终状态仍通过同一 commandId 查询。

结构化接口和 Console 文本 parser 必须生成同一种 envelope。AI 优先使用 JSON/typed API，Console 文本只是
adapter；parser 不拥有任何业务校验。

### 稳定序列化与 AI 接入

每种 `EditorCommandType` 必须有稳定的外部命令名和 JSON schema。外部命令名使用小写 namespace，
内部可映射到 enum，例如：

```text
material.resolve_scene_reference
material.list_assets
material.open
texture.open
material.select
material.close
material.get_document
material.set_parameter
material.clear_parameter
material.set_texture
material.clear_texture
material.set_render_state
material.clear_render_state
material.validate
material.save
material.revert
material.reload
material.preview.connect
material.preview.apply
material.preview.restore_baseline
material.preview.disconnect
editor.get_command_result
editor.list_events
editor.execute_batch
```

示例：

```json
{
  "protocolVersion": 1,
  "commandId": 1042,
  "source": "ai",
  "command": "material.set_parameter",
  "expectedDocumentRevision": 7,
  "payload": {
    "assetPath": "materials/MI_car_paint.json",
    "parameter": "u_clearCoat",
    "type": "float",
    "value": [0.85]
  }
}
```

```json
{
  "protocolVersion": 1,
  "commandId": 1043,
  "source": "ai",
  "command": "material.set_texture",
  "expectedDocumentRevision": 8,
  "payload": {
    "assetPath": "materials/MI_car_paint.json",
    "slot": "baseColorMap",
    "textureAssetPath": "textures/T_car_red_basecolor.json"
  }
}
```

- JSON 字段、command 名、error code 和 value 数组顺序必须版本化并有 schema 测试；
- 路径进入 executor 前统一规范化，结果始终返回规范化路径；
- vec2/3/4 分别要求 2/3/4 个有限 float，不能接受缺省分量；
- AI adapter、Console adapter、ImGui adapter 都调用同一 `EditorCommandCodec/Factory`；
- 首版应提供可被自动化/AI 调用的结构化入口，例如 JSON-lines stdin、进程内 API 或后续 MCP adapter；
- transport 不拥有业务逻辑，替换 transport 不得改变命令语义。
- AI 不需要模拟 ImGui；标准流程是 `resolve/list -> open/select -> get -> set/validate -> save -> get_result`；
- 异步 Save/Preview 必须能通过 `editor.get_command_result(commandId)` 查询，并可通过事件流获取状态变化。

### P0 Command Types

#### Navigation 与查询

- `ResolveSceneMaterialAsset`
  - 输入 scene/world object identity，可选 section；
  - 输出 scene、mesh/terrain asset、section/slot 和 MI path breadcrumb；
- `ListMaterialInstanceAssets`
  - 输入搜索词和分页条件；输出可打开的 MI 资产条目；
- `OpenMaterialInstanceAsset`
  - 输入 MI path 和可选 navigation origin；打开或聚焦同路径文档；
- `SelectMaterialInstanceDocument`
  - 输入已打开的 MI path；改变当前编辑文档选择，不改变资产内容；
- `CloseMaterialInstanceAsset`
  - 输入 MI path、expected revision 和 dirty policy；
- `GetMaterialInstanceDocument`
  - 返回文档 revision、schema、baseline、working values、dirty 和校验状态；
- `GetMaterialInstanceReferenceContext`
  - 返回当前已知 navigation origins，不承诺全项目反向依赖。

#### 文档编辑

- `SetMaterialParameterOverride`
  - 输入 MI path、parameter name、明确 `ParamType` 和完整值；
- `ClearMaterialParameterOverride`
  - 删除一个参数 override，恢复 M_ default；
- `SetMaterialTextureOverride`
  - 输入 MI path、slot name 和规范化 `T_*.json` path；
- `ClearMaterialTextureOverride`
  - 删除一个纹理 override，恢复 M_ default；
- `SetMaterialRenderStateOverride`
  - 输入 MI path、字段 `renderMode`/`shadingModel`/`cullMode` 和对应枚举值；
- `ClearMaterialRenderStateOverride`
  - 删除一个 render-state override，恢复 M_ default；
- `ResetMaterialInstanceOverrides`
  - 输入作用域 `Parameters/Textures/RenderStates/All`；
- `RevertMaterialInstanceDocument`
  - working draft 恢复 baseline；
- `ReloadMaterialInstanceDocument`
  - 从磁盘重读 MI/M_，处理 Source Changed；首版 dirty 时要求明确 discard policy；
- `ValidateMaterialInstanceDocument`
  - 只运行候选资产校验，不写文件；
- `SaveMaterialInstanceDocument`
  - 校验、冲突检测、原子保存并更新 baseline。

#### Preview

- `ConnectMaterialInstancePreview`
  - 尝试连接 active World 中同路径 live MI；
- `DisconnectMaterialInstancePreview`
  - 断开 bridge，不关闭文档；
- `ApplyMaterialInstancePreview`
  - 将指定 document revision 的完整 working draft 作为一次预览事务提交；
- `RestoreMaterialInstancePreviewBaseline`
  - 尝试把 live MI 恢复至文档 baseline；失败只影响 preview 状态。

UI 不应为每个 vec 分量发送命令。一个控件完成一次有效变化时发送完整 typed value。连续拖动可由
CommandBus 按 `(document, parameter, revision)` 合并尚未执行的预览更新，但不得丢失最终值或改变 Save 草稿。

### Command Result

每条命令必须产生可查询的结构化结果：

```cpp
struct EditorCommandResult
{
    uint32_t protocolVersion;
    uint64_t commandId;
    EditorCommandStatus status;
    EditorErrorCode errorCode;
    std::string message;
    std::optional<uint64_t> documentRevision;
    EditorCommandResultPayload payload;
};
```

- `status`：`Accepted`、`Running`、`Succeeded`、`Rejected`、`Failed`；
- `Accepted/Running` 只用于异步 Save/Preview 等操作，表示命令尚未到达最终状态；
- `Rejected` 表示 stale revision、dirty policy 未声明、目标不存在等前置条件不满足；
- `Failed` 表示已进入执行但资产读取、校验、写盘或 preview prepare 失败；
- `errorCode` 必须稳定，AI 和测试不得解析自然语言判断结果；
- `message` 面向人类诊断；
- 成功结果返回最新 document revision 和必要 payload；
- 命令结果写入 bounded result store，并可通过 `GetEditorCommandResult(commandId)` 查询；
- ImGui 状态栏、Console 输出、AI response 和自动化断言消费同一 result。
- 只有 `Succeeded/Rejected/Failed` 是最终状态；同一 commandId 的最终结果必须稳定、可重复查询。

建议 P0 error code 至少覆盖：`AssetNotFound`、`InvalidAssetType`、`ReferenceResolutionFailed`、
`DocumentNotOpen`、`DocumentDirty`、`StaleDocumentRevision`、`UnknownParameter`、`ParameterTypeMismatch`、
`UnknownTextureSlot`、`InvalidTextureAssetReference`、`SourceChanged`、`ValidationFailed`、`AtomicWriteFailed`、
`PreviewUnavailable`、`PreviewGenerationChanged`、`PreviewPrepareFailed`、`PreviewCommitFailed`。

### Revision 与批处理

- 文档每次成功写命令后递增 `documentRevision`；只读命令不递增；
- Save 成功更新 baseline revision 和文件摘要，但文档仍保持同一 path 身份；
- 写命令携带 `expectedDocumentRevision`，防止 AI、UI 和 Console 的旧操作覆盖新草稿；
- 需要一次修改多个参数/纹理时使用 `ExecuteEditorCommandBatch`；
- batch 先在临时 draft 上全部校验，通过后一次提交 document revision；任一子命令失败则零修改；
- batch 的 Preview 在 document commit 后按完整 draft 单独执行，Preview 失败不回滚资产草稿；
- Save 可作为 batch 末尾的显式命令，但不能默认自动保存。

### 实时预览命令链

```text
ImGui / Console / AI
  -> ApplyMaterialInstancePreview(document path, revision)
  -> EditorCommandBus
  -> EngineLoop stable frame boundary
  -> Preview Bridge validates World / MI path / generation / schema
  -> numeric: MaterialInstance::SetParameter(full value)
  -> render state: prepare shader variant/pass/material/pipeline rebuild
  -> texture: prepare Texture + descriptor replacement
  -> no-throw live swap + GPU epoch retirement
  -> next frame preview
```

vec2/vec3/vec4 必须单次提交完整值，不能通过多个标量 action 拼接。校验失败仅报告诊断并保持草稿 dirty；
不得调用全局 `device.waitIdle()`。数值预览不重建 pipeline/descriptor；`renderMode`、`shadingModel`、
`cullMode` 的 render-state 草稿必须在 preview prepare 阶段完成 variant/pass/material/pipeline、对象
descriptor 与 RenderScene 分组的隔离候选构建，并在校验通过后作为一个事务提交，不能只因 JSON 解析成功就报告
live apply 成功。纹理预览只重建受影响对象的 descriptor package，并保持加载、校验、swap 和旧资源退役事务化。

## UE 风格面板布局

```text
+--------------------------------------------------------------------------------+
| Save | Revert | Reset All | MI Asset *          [asset status] [preview status]|
+----------------------+--------------------------------------+------------------+
| Asset Navigation     |      Existing Scene / Preview         | Asset Details    |
| Scene Outliner       |      Preview is optional              | General          |
| Reference Breadcrumb|                                      | Scalar Parameters|
| Selected Model Materials |                                  | Vector Parameters|
| Reference Context   |                                      | Texture Bindings |
+----------------------+--------------------------------------+------------------+
| MI path | Modified | Asset validation | Preview connection | Last operation     |
+--------------------------------------------------------------------------------+
```

- 新建可停靠 `Material Instance Asset Editor`；`BuildDeveloperPanels()` 只调用独立 panel。
- `Scene Outliner` 只负责发出 `ResolveSceneMaterialAsset` 和 `OpenMaterialInstanceAsset` 命令；它不直接解析 World 或访问 Asset Document。
- `Reference Breadcrumb` 显示 `scene object -> mesh/terrain asset -> section/slot -> MI asset`，允许返回来源。
- `Selected Model Materials` 显示当前 3D 视口选中模型的全部材质槽和对应 MI；未选中模型时提示从视口选择。
- `Reference Context` 只保证当前场景/导航来源准确；未建立项目级反向依赖索引时不得伪称完整引用统计。
- `Asset Details` 至少包含 `General`、`Render States`、`Scalar Parameters`、`Vector Parameters`、`Texture Bindings`。
- 中间场景只用于 preview；没有匹配 live MI 时仍保持完整 Asset Details 和 Save 能力。

### 参数行

```text
ParameterName  [Editor Control]  [Reset]
```

- 未 override：显示 M_ 默认有效值，编辑控件可直接操作，Reset 禁用；
- 修改继承值：用编辑后的值创建草稿 override；稀疏规则不保存“等于默认值”的伪 override；
- 已 override：Reset 启用并使用强调色高亮；点击后删除草稿 override，恢复 M_ 默认值并实时预览；
- tooltip 显示类型、默认值、通道名称/说明/范围和 active 状态；
- `Show only active parameters` 可作为显示过滤，不可改变保存语义。

### 控件规则

- `float`：有未来顶层 range 时使用 `SliderFloat`，否则 `DragFloat`；
- `vec2`：无 channels 用 `DragFloat2`；有完整 x/y channels 时分量独立显示和限值；
- `vec3`：无 channels 用 `DragFloat3`；有完整 x/y/z channels 时分量独立显示和限值；
- `vec4`：无 channels 用 `DragFloat4`；有完整 x/y/z/w channels 时分量独立显示和限值；
- 不能按参数名称自动使用 `ColorEdit3/4`；后续添加 `editor.widget = "color"` 才允许使用。

### Texture Binding 行

```text
[Override] SlotName  [T_*.json asset path] [Browse] [Open Asset] [Reset]
```

- slot 全集和默认值来自 M_ schema；
- `Browse` 只能选择通过校验的 `T_*.json`，不能选择 PNG/TGA/EXR 等原始图片；
- `Open Asset` 跳转到独立 Texture Asset Inspector/Editor；MI 编辑器不修改纹理导入和 sampler 设置；
- 取消 Override 或 Reset 恢复 M_ 默认 `T_*.json`；
- 选择等于 M_ 默认纹理时不保留 MI override；
- reflection inactive texture slot 仍可配置和保存，只在 UI 中标记 inactive；
- 缩略图属于后续增强，不是资产引用配置成立的前提。

### Render State 行

```text
StateName  [Enum Control]  [Reset]
```

- 当前支持 `renderMode`、`shadingModel`、`cullMode`，控件值来自稳定的枚举名称；
- 未 override 时显示 M_ 默认值，修改后创建 working override；改回默认值或点击 Reset 删除 override；
- Reset 仅改变 Asset Document 草稿并高亮 dirty/reset 状态，不承诺立即改变当前 live pipeline；
- renderMode/shadingModel 的 pass 组合不兼容时由共享 runtime/material validator 拒绝；
- 不把 render-state override 当作 numeric UBO 更新，也不在 numeric preview session 中静默忽略它。

## 编辑会话

每个打开的 MI Asset Document 包含：

- 规范化 MI 路径、M_ 路径和 schema 内容摘要；
- 原始 MI JSON、M_ 参数/纹理默认值、baseline overrides、working overrides；
- 源文件 BLAKE3-256 摘要；
- dirty 状态、资产校验和保存/冲突诊断；
- 可选 navigation origin，但该字段不参与文档身份和保存。

Preview Connection 是独立临时状态，包含 active World identity、live resource generation、连接状态和
最近预览诊断；它不进入 Asset Document baseline，也不决定文档能否保存。

状态机：

```text
Closed -> Clean -> Dirty -> Saving -> Clean
                     |          -> SaveFailed
                     -> SourceChanged
```

- 切换 MI 或关闭文档遇到 dirty 时显示 `Save / Discard / Cancel`；World 切换不关闭资产文档；
- `Revert` 恢复 Asset Document baseline，并在 Preview Bridge 已连接时尝试同步预览；
- `Discard` 只丢弃资产草稿；preview 恢复失败只报告连接错误，不阻塞文档关闭；
- M_ schema 摘要变化时要求 Reload/Merge Document，不能把旧参数类型或旧 slot 写入新 schema；
- World 换代只断开旧 Preview Connection，并尝试连接新 World 中相同路径 MI。

## 稀疏保存

### 候选 JSON

Save 必须基于会话打开时的原始 MI JSON 定点 patch：

1. 复制原始 JSON；
2. 仅修改编辑器管理的 `parameters`、`textures` 和 `renderStateOverrides` 键；
3. working parameter 与 M_ default 完全相同则删除该参数 override；
4. working texture asset path 与 M_ default 相同则删除该纹理 override；
5. working render-state value 与 M_ default 相同则删除该 render-state override；
6. `parameters`、`textures` 或 `renderStateOverrides` 变空时分别删除整个对象；
7. 保留 `type`、`name`、`material`；原样保留 `macros`、接受的扩展字段和其他未知字段。

首版按解析后的 float 分量精确比较 default 与 working value，不引入隐藏 epsilon 静默删除用户有效输入。

### 校验、冲突与写入

写盘前必须：

1. 校验 MI header、M_ 引用、override key/type/range；
2. 校验每个 texture override 都引用合法、可加载的 `T_*.json`，拒绝原始图片路径；
3. 使用 `MaterialInstanceResolver` 合成有效参数和纹理绑定；
4. 使用 `MaterialDescriptorSchema::ValidateInstanceValues()` 校验；
5. 重读 MI 文件并验证 BLAKE3-256 摘要；变化则进入 `Source Changed`，默认拒绝覆盖；
6. 写入 sibling 临时文件，flush/close 后原子替换原 MI；成功才更新 baseline 和摘要。

失败时原文件必须保持不变。首版冲突只提供 `Reload / Cancel`；`Force Save` 与 `Save As` 以后独立评审。
保存成功不要求 World 存在或重载。若 Preview Bridge 已连接且预览草稿已提交，live 状态应与 working
document 一致；下一次 World 加载必须从保存文件得到相同参数和纹理绑定。

## 建议职责划分（原计划角色）

> 下列路径和角色是计划编写时的拆分草案，保留用于说明设计意图；本轮实际落地路径以文末“实施收尾记录”和正式架构合同为准。

- `source/editor/command/editorCommandBus.*`：统一 envelope、codec、队列、commandId 去重、result store 和事件；
- `source/editor/command/materialEditorCommandExecutor.*`：路由所有 MI editor 命令并保证 owner-thread 顺序；
- `source/editor/asset/assetNavigationService.*`：场景/mesh/terrain 引用追溯，只由 Navigation Command Executor 调用；
- `source/editor/material/materialInstanceAssetDocument.*`：磁盘文档、baseline/working overrides、dirty、校验与保存；
- `source/ui/materialEditor/materialInstanceAssetEditorPanel.*`：ImGui 导航、资产 Details、控件和反馈；
- `source/material/editor/materialInstancePreviewBridge.*`：连接当前 World live MI、数值预览和事务化纹理预览；
- 所有查询、Open/Select、Set/Clear、Reset、Validate、Save/Revert/Reload 和 Preview 都是 EditorCommand；只有 Preview Executor 的 Vulkan 变更进入 EngineLoop 稳定帧边界。

复用 `MaterialInstanceResolver`、`MaterialAssetValidator`、`MaterialDescriptorSchema`、
`TextureAssetLoader`、`MaterialInstance::SetParameter()`、World-local `RendererResourceCache`、BLAKE3 与原子文件工具。

资产保存层不得依赖 `RendererBackendVulkan`、RenderGraph 或 active World。Preview Bridge 可以依赖 renderer，
但不能拥有 Asset Document，也不能直接修改其 baseline/dirty 状态。

## 分阶段交付

### P0.0 Command 基础设施与协议

- 定义 `EditorCommandEnvelope`、typed payload、稳定 command name、protocol version、commandId/correlationId；
- 实现 ImGui、Console、Runtime Test 和 AI 共用的 codec/factory、CommandBus、result store、事件流和错误码；
- 实现重复 commandId 去重、expected revision、batch 原子语义和异步结果查询；
- 验收：四类 producer 对同一命令得到相同结果；非法命令在 executor 前得到稳定错误；producer 不可直接访问 editor service。

### P0.1 Asset Navigation 与只读文档

- 通过 `ResolveSceneMaterialAsset` 实现场景对象到 mesh/terrain slot 再到 MI 的引用追溯和 breadcrumb；
- 通过 `ListMaterialInstanceAssets`、`OpenMaterialInstanceAsset`、`SelectMaterialInstanceDocument` 实现可独立于 World 打开的 MI Asset Browser 与 Asset Document；
- 显示 General、Render States、四类参数、texture slot、默认值、override 和资源路径；
- 验收：关闭/切换 World 后文档仍可阅读和保存；导航不创建 Vulkan 资源；同一路径只打开一个文档。

### P0.2 数值资产编辑与保存

- float/vec2/vec3/vec4 控件、Override、单项 Reset、Reset All；
- baseline/working 文档、dirty 确认、Save、`Ctrl+S`、Revert，全部通过 EditorCommand；
- 候选校验、BLAKE3 冲突检测与原子替换；
- 验收：不启动 renderer 也能完成资产编辑和保存；重启/World reload 后值不丢失；默认 override 删除。

### P0.2a Render-state 资产编辑

- 支持 `renderMode`、`shadingModel`、`cullMode` 的枚举控件、单项 Reset 和 `RenderStates`/`All` Reset；
- 通过统一 EditorCommand 写入 working `renderStateOverrides`，回到 M_ 默认值时删除稀疏 override；
- 验收：不启动 renderer 也能完成 render-state 草稿编辑、校验和保存；保存文件保留非托管字段，重载后得到相同 override；不把该能力等同于 live pipeline preview。

### P0.3 Texture Asset Binding 配置

- Texture Bindings 分组、`T_*.json` Asset Picker、Open Texture Asset、Override/Reset，全部通过 EditorCommand；
- 保存时稀疏 patch `textures`，并校验 T_ asset 及其原始图片资源可加载；
- 验收：拒绝直接图片路径；M_ 默认绑定不重复写入 MI；无需 live World 即可保存纹理配置。

### P0.4 Runtime Preview Bridge

- 通过 Preview Command 将指定 document revision 的数值/纹理/render-state 草稿连接到当前 World 同路径 live MI；当前已接通数值草稿的 runtime adapter 路径；
- numeric adapter 只提交完整数值参数；当前遇到 render-state-only 或混合 render-state 草稿时会在 prepare 阶段明确失败，不会静默忽略或返回假成功；
- render-state 草稿需要按 `renderMode`、`shadingModel`、`cullMode` 影响准备 shader variant、pass/material 选择、descriptor contract 和 graphics pipeline，并通过 no-throw live swap 原子提交；
- 纹理草稿仍需准备新 Texture 和受影响 descriptor package，no-throw swap 后按 GPU epoch 退役旧资源；
- Preview unavailable/failed 与 Asset validation/save 状态分离；
- 验收：World 换代只重连 bridge，不关闭文档；预览失败不阻塞 Save；数值路径已由 focused public-API 测试覆盖；`renderMode`、`shadingModel`、`cullMode` 的 focused 测试覆盖 draft 转发、prepare failure、脏草稿保留和 baseline restore；真实 owner/session 的 capture、旧资源不变、pass/pipeline/descriptor 不变及 GPU epoch 退役仍待 Vulkan 应用集成测试，render-state live rebuild、纹理路径及单/双线程一致性仍待应用 smoke，无全局 wait idle。

### P0.5 验证与文档迁移

- 定向测试：Command codec/schema（含 render-state 命令和 batch）、导航解析、候选 JSON、参数/纹理/render-state 稀疏保存、资产校验、preview request 和 result 查询；
- 单线程/双线程短帧 smoke 与手动 ImGui 检查；真实 `IRendererMaterialInstancePreviewOwner`/`IRendererMaterialInstancePreviewSession` 矩阵已形成测试设计，但当前 focused target 不执行 Vulkan owner/session 实例化；
- 将落地合同迁移到正式 architecture/rendering 文档，并标记本文为完成记录。

## 验证矩阵

- 命令一致性：ImGui/Console/AI/Runtime Test 使用同一 command name、payload、revision、error code 和 result；
- 资产导航：通过 command 完成 scene -> model/terrain -> section/slot -> MI breadcrumb；当前 World 未加载 MI 时直接从 Browser 打开；
- 参数：float；无/有 channels 的 vec2、vec3、vec4；active/inactive 显示和保存；
- 纹理：T_ Asset Picker、默认/override、inactive slot、无 live preview 保存、拒绝图片直链、T_ 或 source 缺失；
- Override：创建、修改、取消、单项 Reset、Reset All、Revert、Save 后 baseline 更新；
- 文档：同路径单例、多个 navigation origin、World 切换保持打开、schema 外部变化、Source Changed；
- 保存：parameters/textures/renderStateOverrides 变空、保留 macros/未知字段、校验/replace 失败、外部修改、重启/World reload；
- 预览：无匹配 live MI、同路径换代、render-state-only 草稿不得假成功、render-state variant/pipeline prepare 失败、纹理加载/descriptor prepare 失败、异步 command result 查询、`workerThreadCount == 1/2`、UI/renderer shutdown。

## P1 后续项

- 显式颜色 widget 元数据；
- Texture Asset Editor（编辑 T_ source/colorSpace/mipmap/filter/wrapMode）和缩略图缓存；
- Make Unique 和对象引用事务；
- 宏编辑；
- Render-state live preview 与 variant/pass/material/pipeline rebuild；需要覆盖 `renderMode`、`shadingModel`、`cullMode` 的事务化资源替换；
- viewport picking、独立 preview、undo/redo、Pass MI 编辑。

## 完成定义

P0 完成时，所有业务操作均通过统一 EditorCommand；场景对象只作为资产导航入口，MI Asset Document 可脱离 World 独立配置和保存；用户可编辑
四种数值类型、`renderMode`/`shadingModel`/`cullMode` 三项 render-state override 并为 texture slot 选择 `T_*.json`；Asset validation/save 与 Runtime Preview Bridge 状态明确
分离；MI 以校验后的稀疏原子写入持久化；World 换代、预览失败、外部修改和保存失败均不造成资产损坏
或旧资源访问。

---

## 实施收尾记录（2026-08-27）

本节是对上方原始计划的追加记录，不删除、不覆盖原有目标、非目标、架构约束、阶段说明和验证矩阵。它只标记当前代码已经落地的范围，以及仍未接通的运行时边界。

### 已落地的实现

- **Command 与 transport**：`source/editor/command/` 已提供 typed envelope、稳定 lower-case command name、协议校验、向量 arity/有限值校验、JSON encode/decode、`commandId` 去重、`expectedDocumentRevision` 门禁和 bounded result store。
- **Renderer-free Asset Document**：`source/editor/service/materialInstanceDocumentService.*` 已提供 MI 资产列表、打开/选择、文档快照、reference context、参数/纹理/render-state override、Reset、Revert、Reload、Validate、Save，以及场景 mesh/terrain 引用解析和 batch 的服务层实现。
- **Render-state 编辑**：`renderMode`、`shadingModel`、`cullMode` 已具备 typed command payload、JSON codec、字段和值校验、默认值清理、`RenderStates` reset scope、UI 快照和稀疏 `renderStateOverrides` 保存；renderer-owned preview 还会在隔离 World-local package 中重建受影响资源并原子替换 active World 的 render scene。
- **持久化**：`source/editor/persistence/` 已提供参数、纹理和三项 render-state 的稀疏 candidate、默认值比较、空对象清理、BLAKE3 source-digest 冲突检测和原子替换；候选校验只验证 editor-managed projection，同时保留原始 JSON 中的非托管字段。
- **校验边界**：当前 service 已覆盖文档级类型、有限值、channel range、override key、三项 render-state 枚举和值、有效 `renderMode`/`shadingModel` 组合以及 `T_*.json` source 检查；pass-specific 路由、`MaterialDescriptorSchema`/shader reflection 和 pipeline 合同校验仍属于 renderer/live preview 接入阶段。
- **ImGui 与宿主 UI**：`source/editor/ui/materialInstanceAssetEditorPanel.*` 已接通资产浏览、参数/纹理/render-state 编辑、单项 Reset、Reset All、Revert、Validate、Save 和 `Ctrl+S` 命令生产；`source/editor/runtime/materialInstanceEditorRuntime.*` 已接入 `UiSubsystem` 的初始化、每帧 drain/tick、snapshot 更新和 World 变化诊断，World 变化不会关闭文档。
- **Preview 边界**：`source/editor/preview/` 已提供值语义类型、World/代际检查的 controller 和显式 `Unavailable` adapter；renderer-owned numeric adapter 已接入稳定帧路径，三项 render-state 变化会进入隔离资源候选事务，完成 Material/variant/pass/graphics pipeline、对象 descriptor、RenderScene 分组的原子替换，并将旧 World-local package 按 GPU epoch 退休。纹理变更仍需要独立的 descriptor replacement 事务。
- **测试与正式合同**：`tool/material-instance-editor-tests/` 已包含 fixture contract tests 与 landed-interface tests，并由根 `CMakeLists.txt` 注册；正式合同已迁移到 `documents/architecture/material-instance-editor.md`。

### P0 阶段状态

| 阶段 | 当前状态 | 事实边界 |
| --- | --- | --- |
| P0.0 Command 基础设施与协议 | 部分完成 | 协议、codec、bus、去重和 revision 门禁已落地；事件发布尚未接入，batch 虽有 service 实现但尚未由 runtime facade 路由。 |
| P0.1 Asset Navigation 与只读文档 | 部分完成 | renderer-free document service、MI browser、同路径文档单例和快照已落地；scene resolver 的 service 方法存在，但当前 UI/runtime 路径尚未接通 Scene Outliner reveal。 |
| P0.2 数值资产编辑与保存 | 已落地（renderer-free；待最终构建/运行验证） | 四种数值类型、override/reset/revert、文档级校验、稀疏保存、摘要冲突和原子写入已有实现；不依赖 live World。 |
| P0.2a Render-state 资产编辑 | 已落地（编辑与 live preview 均已接入） | `renderMode`、`shadingModel`、`cullMode` 的 typed command、service/UI 快照、字段和值校验、`RenderStates` reset scope 和稀疏 `renderStateOverrides` 保存已实现；renderer-owned preview 通过隔离候选完成 variant/pass/material/pipeline、对象 descriptor 和 RenderScene 分组的原子替换，并按 GPU epoch 退休旧资源。 |
| P0.3 Texture Asset Binding 配置 | 部分完成 | `T_*.json` 列表、选择、override/reset 和 source 校验已落地；`texture.open` 尚未连接独立 Texture Asset Editor。 |
| P0.4 Runtime Preview Bridge | 数值与 render-state 路径已接入，纹理路径待完成 | runtime 通过公开 `Config::previewAdapter` 接入 renderer-owned 数值 live preview；三项 render-state 通过隔离 candidate package 完成 variant/pass/material/pipeline、对象 descriptor 和 RenderScene 分组的原子替换，并按 GPU epoch 退休旧资源；纹理 descriptor replacement、`texture.open` 和 Vulkan 资源事务仍未完成。 |
| P0.5 验证与文档迁移 | focused 与应用 smoke 已完成，手动验收待执行 | baseline regression、production service/runtime 的 render-state 编辑与三项 preview prepare-failure/restore 回归、public fake-adapter connect/apply/restore/generation 测试、landed protocol-boundary 测试和三 target 注册已落地；focused tests 于 2026-08-29 通过，单/双线程、开发者 UI 开/关的短帧 smoke 于 2026-08-27 通过，手动 ImGui 验收未执行。 |

### 当前可用路径与未接通路径

启用 developer UI 后，当前面板可以通过统一命令路径浏览 MI 资产、打开/选择文档、编辑 `float`/`vec2`/`vec3`/`vec4` 参数、纹理 override 和 `renderMode`/`shadingModel`/`cullMode` render-state override，并执行 Reset、Revert、Validate、Save 与 `Ctrl+S`。这些操作由 renderer-free 文档服务完成，当前 World 是否存在不影响资产文档的保存生命周期。

以下能力仍属于实现收口，而不是已完成的 runtime 功能：

- Scene Outliner 到 `ResolveSceneMaterialAsset` 的 UI/runtime 路由；
- `texture.open` 到独立 Texture Asset Editor 的路由；
- 数值 `material.preview.*` 已接入 runtime adapter；`renderMode`、`shadingModel`、`cullMode` 的 preview 已路由到 variant/pass/material/pipeline、对象 descriptor 和 RenderScene 分组的事务化替换；纹理 `material.preview.*` 到 descriptor transaction 的路由仍未完成；
- `editor.list_events` 和 `editor.execute_batch` 在 runtime facade 中的完整接线；
- 手动 ImGui 验收。

因此，本次实现已经提供“运行时可提交资产草稿并安全保存”、三项 render-state 资产编辑、三项状态 live preview 以及数值 live `MaterialInstance` 调整；纹理 live preview、`texture.open` 或事件流仍未完成，后续继续遵守 Preview Bridge 的稳定帧、pass-specific variant/descriptor 校验、资源事务和 GPU epoch 退役约束。

### 文档与测试使用方式

- 当前正式架构合同：`documents/architecture/material-instance-editor.md`；
- focused test 说明与覆盖范围：`tool/material-instance-editor-tests/README.md`；
- 根构建下可运行：`cmake --build build -j2 --target material_instance_editor_tests material_instance_editor_landed_tests material_instance_editor_production_tests`，随后执行 `ctest --test-dir build -R "^material_instance_editor_(contract|landed|production)$" --output-on-failure`；本轮三个 target 均已执行并通过；
- fixture standalone 构建仍可使用 `tool/material-instance-editor-tests/CMakeLists.txt`，用于不依赖完整主程序的协议、持久化和预览值语义回归。
- 本轮已执行应用级单线程/双线程短帧 smoke，并分别覆盖开发者 UI 开启/关闭；未执行手动 ImGui 验收。focused target 不链接真实 Vulkan renderer adapter，不以 focused tests 冒充 Vulkan 集成证明。

### 本轮验证补充

 renderer-owned numeric preview 对 working/baseline 草稿中的 unchanged texture override 采用 active MI
捕获的规范化 asset identity 做门禁；纹理字段 malformed 或发生变化时返回 preview prepare failure，
不会静默忽略。2026-08-27 已完成 workerThreadCount=1/2、开发者 UI 开启/关闭的应用短帧 smoke；
仍未执行手动 ImGui 交互验收。

### 2026-08-29 Runtime Preview 回归补充

`tool/material-instance-editor-tests/materialInstanceEditorProductionTests.cpp`
新增三项 render-state preview boundary cases：分别修改 `renderMode`、
`shadingModel`、`cullMode`，确认完整 working draft 进入公开 preview adapter，
focused production test 继续使用 CPU fake adapter 验证完整草稿、脏状态和 baseline
协议边界；真实 renderer-owned adapter 现已实现三项状态的隔离 prepare/commit，
完成 variant/pass/material/pipeline 与对象 descriptor 重建，交换 RenderScene 分组，
并把旧 World-local package 按最后提交的 GPU epoch 送入退休队列。纹理 descriptor
替换仍未接通，真实 Vulkan 交互验收仍需手动执行。
