# 资产标准修改接口

> 状态：**当前契约**（实现已落地，2026-10-13）。本文描述源码与模块测试可核对的行为。

## 入口与归属

Scene、Mesh、TressFX、Texture、Blueprint 采用同一协议和 executor。运行中的 `editor <JSON>` Console 命令
通过 `MaterialInstanceEditorRuntime::SubmitJson` 投递到现有 `EditorCommandBus`；
无窗口工具 `vl_asset_edit` 使用同一 codec、bus、`AssetCommandExecutor` 与文档服务。
Console 只借用 facade，不操作 World 或文件。运行中入口需要启用编辑器。

| 命令前缀 | 文档归属与可编辑内容 |
| --- | --- |
| `scene.*` | Scene 根 name/description；已有 mesh 的 TRS/materialOverride；灯光作者参数；相机变换、投影、曝光 |
| `mesh.*` | `SM_*.json`，type 为 mesh/speedtree；源模型引用、适用的导入选项、已有材质槽的 MI 绑定；拒绝 type 为 tressfx 的文档 |
| `tressfx.*` | `TF_*.json`，type 为 tressfx；源 TFX 引用、已有材质槽的 MI 绑定及 `simulation.*` 模拟标量；迁移期也接受旧 SM_ 前缀，不接受 importOptions，`strandRadius`/`collision.*` 不在修改能力表内 |
| `texture.*` | `T_*.json`；源图片引用、colorSpace、mipmaps、filter、wrapMode、channelsDescription |
| `blueprint.*` | `BP_*.json`；名称/说明/类、Expose 类型化默认值、instanceEditable 名称数组、componentInstanceEditable 权限对象、完整组件数组、根物理对象及初始 rotation/scale |

场景中的模型变换属于 Scene，共享模型的槽绑定属于 Mesh；MI 参数和贴图槽由原 MI 服务拥有。
接口不编辑几何或图片像素，不支持 Scene 对象增删/改名/类型转换、槽增删/重排、任意 JSON Pointer、
环境/风参数、terrain、通用跨资产历史。临时 `SC_model_preview.json` 不能成为可编辑资产文档。
具体可写字段以 `get_capabilities` 为准，字段定义、typed codec 与能力投影共用
`assetDocumentFields.cpp` 的一张表。

TressFX 能力表包含 9 个 `simulation.*` 一级字段：`gravity`、`wind`（向量）、`damping`、
`iterations`、`bendStiffness`、`globalShapeStiffness`、`globalShapeEffectiveRange`、
`vspCoefficient`、`vspAccelThresholdMax`（数值）。这些字段只对 Tressfx 文档 kind 开放，
Mesh 能力表不包含模拟字段。值域由
`MeshAssetValidator::BuildLoadPlan` 在 modify/validate/save 阶段校验，越界直接拒绝；
`iterations` 必须是整数。逐顶点曲线、`collision.*`、`strandRadius`、`skinning.*` 仍不在
能力表内，属于资产直接编辑。
BP 文档支持完整替换组件数组，包括增删和父链调整。可选
[C# 蓝图](csharp-blueprint-runtime.md) 的场景新增/改名/复制/删除、实例属性与 inline
组件/物理编辑、获准的继承组件字段编辑/重置由 GT 作者适配器复用 Scene 草稿与保存 owner，
不在此 JSON facade 的能力表内。
编辑器的 Scene Undo/Redo 也属于该作者路径；本 facade 只为 BP 提供 `blueprint.undo` / `blueprint.redo`。
标准修改导致草稿与 Scene 历史当前值不符时，旧历史不可回放。

## 生命周期

五种前缀各支持 `get_document`、`get_capabilities`、`modify`、`validate`、
`save`、`revert`、`reload`。已有 `scene.save` 复用同一 Scene owner。
Blueprint 另支持 `undo`、`redo`，都要求当前 revision；文档投影的 `canUndo` / `canRedo` 标明可用性。
两个历史栈合计最多 32 步、16 MiB 序列化 JSON，保存保留、revert/reload 清空，新编辑清空 redo。
历史只修改草稿，候选引用/类校验失败不消耗记录；保存后的撤销重新变脏，不自动改磁盘。

读取命令打开文档并返回 `assetPath`、`documentRevision`、`dirty`、BLAKE3 `sourceDigest`、
完整可编辑投影及能力。投影不展开未管理字段或内嵌 `data`；原始 JSON 草稿保留未管理内容，
保存只改变已提交字段的语义，整份文件按四空格重新序列化。读取不等于通过完整引用校验。

`modify`、`save`、`revert`、`reload` 必须带当前 `expectedDocumentRevision`。
每次 modify 接受 1..128 项：在隔离副本上应用全部修改，然后验证最终候选。
near/far、spot 内外锥等组合只看最终状态。失败不改变任何字段或 revision；
无变化不推进 revision。重复目标、未知字段/属性、错误类型、非有限或 float 不可表示数值拒绝。
向量必须给完整三个分量。可选字段删除采用 `{"field":"mipmaps","reset":true}`，
reset 不能同时带 value；必填字段不能 reset。

BP 的 `properties`、`physics`、`componentInstanceEditable` 使用完整对象，
`components`、`instanceEditable` 使用完整数组，
不能用任意嵌套路径局部写入。运行中的宿主发布递归类目录，候选校验属性名称、
标量/向量/枚举/稳定引用、自定义对象/数组/List/字符串键字典类型与
实例白名单名称；CLI 不启动 CLR，只校验结构、引用类型及内容归属。保存不构造托管 Blueprint，
不执行 OnCreate；正式加载仍负责完整 CLR 校验。

`componentInstanceEditable` 将稳定组件 ID 映射到可覆写字段名数组，例如
`{"visual":["position","materialOverride"]}`；最多 128 个组件条目，每项最多 32 个不重复字段，
Scene 的每组件覆写同为最多 32 个字段，覆盖 SpringArm 的完整参数集合。
未知组件、错误类型和该组件类型不支持的字段拒绝；TRS 按完整向量授权，
ID/type/parent、结构增删、碰撞及根物理配置不开放实例覆写。
支持字段由共享组件权限表校验。Scene 作者路径只写稀疏 `componentOverrides`，
重置删除字段键，未覆写项继承 BP；这不是 `scene.modify` 的额外 JSON 能力。

`validate` 不写盘；`save` 重新校验并用打开时的源摘要检查外部修改，
沿用 `MaterialInstancePersistence` 的同目录暂存、替换前摘要复核及原子替换。
冲突/写入失败保留草稿，modifiedDocuments 显示 SourceChanged/SaveFailed。
save 成功推进 revision。`revert` 回内存基线；`reload` 读取磁盘并推进 revision，
脏文档默认拒绝，显式 `dirtyPolicy:"discard_changes"` 才丢弃草稿。
失败候选的 `diagnostics` 带 code、message 和请求字段/对象/槽 selectors；
scope 为 candidate，targets 表示整个请求范围，不声称定位到其中唯一错误字段。
客户端以 status/errorCode 判断成功与失败，不解析自然语言 message。

## 选择器与作者语义

Scene 根字段不带选择器；对象字段以唯一的 `objectIdentity:"type|name"` 定位，
不按数组下标猜对象。完整场景校验拒绝重复身份。Mesh 槽必须同时带
`slotIndex` 与 `expectedSlotName`，仅修改 `materialInstancePath`，顺序身份变化即拒绝。

位置、长度和裁剪面使用厘米，rotation 与锥角使用度。
灯光 intensity 使用最终 intensityUnit 的作者值（directional 为 lux，局部光为 cd/lm）。
修改锥角/面光 size 不自动保持旧规范亮度；同时修改单位不自动换算旧 intensity。
UI 的保持亮度换算仍属于 UI actor adapter。Camera rotation 明确清除旧 look_at，
同时提交 rotation/look_at 拒绝；只修改 position 保留 look_at，表示继续朝向该作者目标。

场景 JSON 与 Texture JSON 分别由 `sceneJsonValidator.h`、`textureAssetValidation.h`
共享给正式 loader 和标准资产候选；Mesh 使用现有 `MeshAssetValidator`。
校验检查文档及引用文件存在/类型/资源根边界，结果标记
`validationLevel:"document_and_references"`、`contentPreflight:false`。
这不导入模型、不验证源材质槽与几何对应关系、不解码图片，也不准备 Vulkan 资源。
retarget 骨骼 SM 的文档验证检查 `sourceModel`/`retarget` 引用存在，
sourceModel 的 descriptor 规划合法且为非 retarget 的骨骼 SM；profile 内容和两套 SK 的
identity/骨名映射由正式导入验证。`skinning` 不在标准修改能力表内，普通字段保存保留它。
正式 World 加载仍执行完整导入与资源事务；加载失败不回滚已经明确保存的作者文件。
旧 UI actor 保存沿用已有轻量验证，不为了保存一个灯光重新导入未修改的模型。

## 草稿与运行时

标准 modify 只暂存，返回 `application:"save_and_reload"`。
Scene 草稿与已有 UI actor 编辑使用同一 baseline/draft/revision，进入同一 Save Content 列表。
标准 Scene 修改后，基于旧 World 的 UI actor 编辑暂时拒绝，直到该场景完成正式加载；
保存成功本身不能解除这一限制。未保存 Scene 草稿在下次 World 加载前冻结，
经既有 World/Graph 事务验证、准备和发布；此时跳过旧逐字段重放，避免旧历史覆盖新草稿。
源摘要/物理目标冲突拒绝草稿加载。保存后的场景从磁盘加载。

脚本作者适配器的连续请求直接合成作者 JSON；当前草稿仍匹配脚本历史记录时，
可读取最新草稿继续编辑，不必等待前一个分量触发的重载。标准 API 改写使历史不匹配时，
仍受上述 staged Scene 门禁约束；该例外不授权对旧 World 做 live 参数补丁。

Mesh/TressFX/Texture 草稿也进入 Save Content，按资产类型提交 mesh.save/tressfx.save/texture.save。
本接口没有 GPU 热替换，保存后需经正式资源重建/World 加载消费新描述；
texture.reload/mesh.reload/tressfx.reload 只重读作者文档。TressFX 拥有独立草稿 owner，
通过冻结的 `tressfxDocuments` 快照向其资产页提供投影和 revision；同一文件不会同时进入
Mesh 与 TressFX 两份草稿。API 没有新增模型/贴图 Details 编辑控件。
MI 的现有预览协议保持独立。

BP 标签页有独立草稿与 revision，也进入 Save Content，提交 `blueprint.save`。
修改默认值、属性白名单或组件权限不修改 Scene 草稿；未保存的 BP 不参与运行时加载。
撤回权限后已有越权覆写在下次正式加载时拒绝，不会静默删改 Scene 文件。
显式保存后既有内容观察器通过正式 World 事务消费新配置。`blueprint.reload` 只重读作者文档。

路径只接受资源根相对地址，拒绝绝对地址、根外遍历和链接逃逸。
规范化别名、资源根内链接别名以及 Windows 大小写别名复用已打开 owner；
保存前重新解析首次逻辑地址，核对物理目标。每个 host 会话拥有自己的未保存草稿，
独立 CLI 进程不共享运行中编辑器草稿；并发保存通过摘要发现冲突，
不提供跨进程锁或通用数据库事务。

## AI 调用

从仓库根构建/启动；默认 resource-root 取 `config/config.json` 的 resourcePath：

```powershell
cmake --build build --target vl_asset_edit -j6
.\build\bin\vl_asset_edit.exe --resource-root ..\VukanLearnResources --input requests.jsonl
```

省略 `--input` 从 stdin 持续读 JSON Lines。整次读/改/校验/保存放在同一进程中，
每行一个请求、stdout 每行一个结果。退出码 0 表示所有命令成功，2 表示有命令拒绝/失败，
1 表示配置或输入文件错误。CLI 仅支持上述五种标准资产命令。

```jsonl
{"protocolVersion":1,"commandId":1,"source":"ai","command":"texture.get_document","payload":{"assetPath":"Maps/SC_example/Textures/T_example.json"}}
{"protocolVersion":1,"commandId":2,"source":"ai","command":"texture.modify","expectedDocumentRevision":1,"payload":{"assetPath":"Maps/SC_example/Textures/T_example.json","changes":[{"field":"colorSpace","value":"linear"}]}}
{"protocolVersion":1,"commandId":3,"source":"ai","command":"texture.validate","payload":{"assetPath":"Maps/SC_example/Textures/T_example.json"}}
{"protocolVersion":1,"commandId":4,"source":"ai","command":"texture.save","expectedDocumentRevision":2,"payload":{"assetPath":"Maps/SC_example/Textures/T_example.json"}}
```

示例 revision 假设首次打开且 modify 确实改变颜色空间。交互客户端必须使用返回的 revision；
重试同一命令复用 commandId，新的意图使用新 ID。总线去重在有界结果缓存保留期间有效。
运行中 Console 用 `editor ` 加同一行 JSON，立即打印接收状态，后续帧打印终态结构化结果。
解析上限为单请求 1 MiB、资产 JSON 4 MiB、嵌套 64 层，重复 JSON 键拒绝。
JSONL 语法/协议错误未能取得有效 ID 时，transport 返回 commandId 0 的 rejected 诊断；
该诊断不是可投递的 EditorCommandEnvelope。

测试位于 `tool/editor-ui-tests/assetDocumentTests.cpp`、
`sceneDocumentTests.cpp` 与 `tool/material-instance-editor-tests/materialInstanceEditorProductionTests.cpp`。
验证入口见 [testing.md](testing.md)。
