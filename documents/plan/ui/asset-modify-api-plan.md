# 场景、模型与贴图标准修改接口

日期：2026-10-13。

状态：**设计记录，标准文档接口已落地**。下文保留原设计意图，不是当前实现契约。
当前能力以 [资产修改接口契约](../../contracts/asset-modify-api.md) 为准。
2026-10-13 落地范围：三类七种命令、共享校验、事务草稿、冲突保存、
JSONL CLI、运行中 Console 和 Save Content。Scene 标准修改暂存后通过
World/Graph 事务加载生效，加载前拒绝旧 World 的 UI actor 编辑；未把既有
UI actor 路径整体迁移成标准 modify，也没有新增 Mesh/Texture 编辑控件或 GPU 热替换。
现有行为以 [材质实例编辑器契约](../../contracts/material-instance-editor.md)、
[UI 组件契约](../../contracts/ui-component-system.md) 与源码为准。

## 目标与资产归属

提供可序列化、可查询能力、可校验、可追踪结果的标准 `modify`。
编辑器、AI、Console 与自动化工具使用同一个执行入口；各 producer 只负责产生请求。

| 接口 | 所属文档 | 修改范围 |
| --- | --- | --- |
| `scene.modify` | Scene JSON | 已有对象的作者参数、场景说明 |
| `mesh.modify` | SM JSON，包括 `type=speedtree` 的描述文件 | 模型源引用、导入设置、材质槽绑定 |
| `texture.modify` | T JSON | 图片源引用、colorSpace、mipmaps、filter、wrapMode、通道说明 |

场景中某个模型的位置、旋转、缩放属于 `scene.modify`，不能写入共享 SM。
某个对象的 `materialOverride` 也属于 Scene；SM 的材质槽绑定属于 Mesh；
MI 的参数与贴图槽仍由现有 MI 文档服务拥有。

这里的“模型修改”指资产描述与导入设置，不包括顶点、拓扑、UV 或骨骼编辑。
“贴图修改”指资产描述，不包括重新绘制像素、通道打包或图片格式转换。
几何/像素处理应有独立的离线处理操作，产出源文件后通过 `modify` 更新引用。
不得以接口名称暗示已经拥有这些内容生产能力。

## 统一生命周期

拟补齐每种资产的 `get_document`、`get_capabilities`、`modify`、`validate`、
`save`、`revert`、`reload`。命令名称使用 `scene.*` / `mesh.*` / `texture.*`。
已有 `scene.save` 复用原命令，不能创建另一份场景文档。
MI 保留已有命令；统一 facade 不重新实现其 Set/Clear 或保存逻辑。

`get_document` 返回规范资产路径、文档 revision、dirty、源摘要及可编辑投影，
同时完成未打开文档的加载。可编辑投影不得使用只读 inspector 的截断字符串，
也不展开内嵌 `data` 或读取引用的几何/像素数据。
`get_capabilities` 返回实际允许的字段、值类型、枚举、单位、可重置性、
对象/槽选择器及生效方式。AI 不需要从 UI 文案猜测有效字段。
两者均不修改草稿，不依赖资产是否在当前 World 中。

`modify` 修改当前文档的隔离候选。一次请求内所有字段应用完毕，再校验完整候选，
全部通过后才一次发布草稿与新 revision；任一字段失败，整条请求不改变草稿。
合法但等于当前值的修改不推进 revision。
例如同时修改 near/far 或 spot 的两种锥角，不能按单字段的临时中间态拒绝。

`validate` 不写盘。`save` 明确写盘，并使用文档基线的 BLAKE3 摘要检测外部修改、
沿用同目录暂存与原子替换。文件冲突与写入失败保留草稿和诊断。
`revert` 回到内存基线；`reload` 重新读取磁盘，脏文档沿用显式丢弃策略。
接口本身不弹确认框，UI 的 Save Content 确认只决定是否提交同一条 save 命令。

`modify`、`save`、`revert`、`reload` 均要求 `expectedDocumentRevision`。
revision 防止旧 producer 覆盖进程内新草稿；源摘要防止保存覆盖外部写入。
两者职责不同，不能互相替代，也不能以时间戳代替摘要。

## 命令形状

复用 `EditorCommandEnvelope`、`EditorCommandBus`、命令去重与结构化结果。
以下名称与 payload 是原方案的协议示例（字段名已按实际 codec 更正为 command）：

```json
{
  "protocolVersion": 1,
  "commandId": 1001,
  "source": "ai",
  "command": "scene.modify",
  "expectedDocumentRevision": 7,
  "payload": {
    "assetPath": "Maps/SC_example/SC_example.json",
    "changes": [
      {
        "objectIdentity": "mesh|example_model",
        "field": "position",
        "value": [0.0, 100.0, 0.0]
      },
      {
        "objectIdentity": "camera|Camera",
        "field": "exposure.ev100",
        "value": 9.0
      }
    ]
  }
}
```

Scene 根字段不携带 `objectIdentity`。对象字段必须携带唯一身份，复用现有
`type|name`；缺失、重名或类型不匹配拒绝，不以 objects 数组索引静默选中。
此身份只适用于已有对象编辑，不承诺改名后仍可使用。

```json
{
  "protocolVersion": 1,
  "commandId": 1002,
  "source": "ai",
  "command": "texture.modify",
  "expectedDocumentRevision": 3,
  "payload": {
    "assetPath": "Maps/SC_example/Textures/T_example.json",
    "changes": [
      {"field": "colorSpace", "value": "linear"},
      {"field": "wrapMode", "value": "clamp"}
    ]
  }
}
```

`changes` 为有界非空列表，重复目标字段拒绝，避免依赖列表顺序解释覆盖关系。
每种 field 解码为对应的 C++ 类型；向量要求完整分量，布尔不接受数字替代，
数值必须有限且可由消费端表示。未知字段、错误类型与不支持的操作明确拒绝。
重置可选字段使用显式 `reset` 操作，不用 `null` 同时表达删除、默认值和空内容。

Mesh 材质槽以 `slotIndex` 加 `expectedSlotName` 定位已有槽，
只改 `materialInstancePath`。现有运行时按源材质顺序映射槽，不能把槽名当成
导入器中的名称匹配规则，也不能在普通绑定修改中重排或增删槽。

结果复用既有 status/errorCode，增加可序列化的资产结果 payload：
规范路径、新 revision、dirty、发生变化的字段、字段诊断及生效方式。
字段诊断带对象/槽选择器，客户端不解析 message 判断失败类型。
读取命令必须能实际返回文档与能力数据，不能只返回一段成功提示。

## 第一版能力边界

| 资产 | 拟开放字段 | 关键约束 |
| --- | --- | --- |
| Scene 根 | name、description | description 非空，保留场景用途说明 |
| Scene mesh | position、rotation、scale、materialOverride | 世界长度沿用厘米；scale 非零，支持负值 |
| Scene 节点灯 | position、rotation、color、intensity、intensityUnit；spot 锥角 | 作者单位 lux/cd/lm；完整候选遵循共享物理光照校验 |
| Scene 面光 | position、normal、size、color、intensity、intensityUnit | 非零法线、正边长，保留现有单面语义与实时不支持限制 |
| Scene camera | position、rotation、look_at、fov、near_clip、far_clip、exposure 支持字段 | 有效投影与曝光；look_at 优先关系必须明确处理 |
| Mesh | name、modelDataPath、现有 importOptions、已有槽的 materialInstancePath | 引用和导入请求合法；不在 modifier 内写入几何数据 |
| Texture | name、source、colorSpace、mipmaps、filter、wrapMode、channelsDescription | 复用 T JSON 合同，拒绝 flipY 等未知字段 |

场景对象增删、改名、类型转换、环境/风编辑、临时可见性、编辑器观察相机、
terrain 运行时、Undo/Redo 不在第一版范围内。不要借通用 JSON Pointer 开放任意字段。

作者字段按 JSON 语义解释：只改 intensity 保留单位；同时改单位时以请求的最终
intensity/unit 对解释。spot 锥角与面光尺寸修改不默认执行 UI 现有“保持规范亮度”
换算，UI adapter 应将该意图转换为完整的作者候选。
Camera 明确修改 rotation 时去掉旧 look_at；同时提交 rotation 与 look_at 拒绝，
位置编辑保留 look_at 则表示仍朝向作者目标。这里的差异需写入能力结果与回归。

资产路径统一为 resource-root-relative 路径；引用在所属资产校验边界检查。
不允许绝对路径、资源根外逃逸或通过链接绕过根目录；保存前复核物理目标身份。
引用的文件存在不等于可导入或可解码，validate 结果应区分文档校验与内容预检，
不把仅通过 JSON 形状校验的结果报告成模型/贴图加载成功。

## 服务与运行时接入

```text
RmlUi / Console / AI / CLI adapter
    -> EditorCommandEnvelope
    -> EditorCommandBus
    -> GT asset-document facade
    -> SceneDocumentService / MeshDocumentService / TextureDocumentService
    -> shared asset validators + conflict-aware persistence
    -> structured result / immutable document snapshot
```

沿用已有 SceneDocumentService 的 baseline/draft/revision，新增 Mesh/Texture 文档 owner。
统一 facade 负责分派与查询；不要让当前 MI runtime 的名称成为新增功能的归属理由。
是否重命名 facade 由实现范围决定，不做无关的大规模迁移。

修改前先整理共享校验边界：Mesh 已有纯 JSON 的 MeshAssetValidator；
Scene 校验目前与 mesh 预检耦合，Texture 校验目前嵌在文件加载路径中。
需提取可消费隔离候选的校验入口，运行时 loader 与文档服务共同调用，
不能在 AI adapter 中复制字段、默认值、物理单位或导入规则。
字段能力投影与可编辑字段校验也应来自同一份定义，避免出现能发现却不能修改的字段。

Scene 的现有 actor 编辑采用“先写 World，再记录草稿与可重放历史”。
接入标准 modify 时必须统一该行为：UI actor 请求转成标准资产修改意图，
文档候选和可选 live 候选都准备成功后再发布。不能额外建一份 Scene 草稿，
也不能让新修改绕开旧历史，导致预览往返后重放出过时状态。
未加载场景仍可修改与保存；不能为了编辑文档而强制切换 World。

修改草稿、保存磁盘、应用运行时是三个独立动作。
第一版 Mesh/Texture 修改生效方式可以明确为“保存后经正式资源重建/World 加载生效”，
不得成功返回后暗示 GPU 已更新。已有 Scene live 编辑能力需与文档接入一起验证；
不支持的 live apply 返回能力诊断，资产修改结果仍独立成立。
新增 GPU 替换只能走现有隔离准备、稳定帧提交与 epoch 退休边界。

AI 调用必须有真实 host：先接收 JSON 请求、返回结构化 JSON 结果，
再宣称“AI 可用”。headless CLI 与运行中 host 复用相同文档 executor；
同一进程只有一份文档 owner。CLI 不能另写一套直接覆盖文件的逻辑。
独立进程之间通过保存时源摘要识别冲突，不能承诺共享未保存草稿。

## 落地顺序与验收

1. 提取候选校验入口与字段能力定义，补共享校验回归。
2. 增加三类命令、文档/能力结果和 codec；接入单资产原子 modify 与现有保存机制。
3. 统一 Scene actor 编辑、草稿与替换 World 重放；新增 Mesh/Texture owner，
   合入 modifiedDocuments 与现有 Save Content，不复制保存确认逻辑。
4. 接入 JSON host/CLI，验证 AI 从读取 revision 到 modify、validate、save 的完整流程。
5. 按实际能力接 UI 控件与可选 live apply；落地后再更新 contracts 与路由。

在最近的根 GoogleTest 模块套件补测试，覆盖：

- UI/AI/Console 同请求得到相同候选、revision 与结果；
- 多字段完整候选校验、失败零修改、重复字段拒绝、无变化不推进 revision；
- 陈旧 revision、错误对象身份/槽选择器、非法值和未知字段；
- 草稿与保存分离、外部源冲突、原子写失败和未管理 JSON 保留；
- 根外路径/链接拒绝、失效引用与内容预检失败；
- Scene UI 与 AI 连续编辑同文档、预览往返/替换 World 后保留最新草稿；
- headless 完整调用流程、运行中 host 复用文档，以及保存成功但尚未 live apply 的结果。

不新增 runtime test 标志，不用启动 Vulkan 替代纯文档模块测试。
按 [测试契约](../../contracts/testing.md) 完成相关编辑器套件、main 构建与边界审计；
有 UI/运行时变化时另做真实交互与出图验收。实际落地行为与验收记录见资产修改接口和测试契约。
