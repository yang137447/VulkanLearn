# TressFX 毛发编辑器：视口、蓝图组件与网格派生根点权重（计划）

> 状态：**计划**（非契约，2026-10-25 定稿）。本文只记录决策、证据与任务拆解，
> 不描述当前实现。当前行为以 `documents/contracts/tressfx-asset-import.md` 与源码为准。

## 1. 背景

毛发运行时（CPU/GPU guide 模拟、逐帧 SDF 碰撞、骨骼蒙皮）已在 TressFX 移植线上落地，
但作者侧只有两条互相割裂的路：

- 场景 JSON 里的独立 `mesh` 对象，绑定靠 `simulation.skinning.source` 写死**场景对象名**；
- 编辑器里只能改 `.tfx` 描述符 JSON，无法在视口里看物理响应，也无法调模拟参数。

本轮要解决的是**作者体验**，不是渲染模型：不做 Marschner / 覆盖积分 / OIT / 发丝阴影。

## 2. 已定决策

| 决策 | 内容 | 依据 |
| --- | --- | --- |
| 视口范围 | 毛发资产 tab **只支持无 `simulation.skinning` 的资产**；单对象预览、原地转模型 | 用户决定；带 skinning 的资产需要源角色姿态，视口不承担 |
| 绑定归属 | 角色身上的毛发由**蓝图 TressFX 组件**承载；组件挂载时自动派生绑定与权重 | 用户决定；比在资产里写场景对象名更符合放置语义 |
| 权重来源 | 从**父角色绑定姿态网格**派生：每个 guide 根点取最近身体顶点，复制其完整权重列表 | 与碰撞代理同一套实现（`BuildTressfxProxySkinTransfer`），确定性已有测试 |
| 是否插值 | **不插值**。实测发根到最近顶点最大 0.516 cm，头部顶点间距中位 0.067 cm | 见 §3；插值只会优化看不见的误差，跨接缝加权反而更假 |
| 距离上限 | 2 cm，只作为“错空间”检测器；个别超距回退 `rootBone` 并统计，全部超距硬错误 | 与代理同一常量；本资产实测最大 0.516 cm |
| 资产命名 | **2026-10-26 更新：毛发使用独立 `TF_*.json` + 自己的文档 kind 与专用 tab**（此前“继续 `SM_` + `type: "tressfx"`”的结论作废） | 用户决定；双击打开毛发 tab，复用已实现的视口与模拟面板 |
| 运行时调参 | **不做**。游戏读资产固定值，任何调参入口都属于编辑器会话 | 用户决定 |
| GPU 侧 | **零改动**：per-strand 多骨通道（binding 14/15、`motion.w` bit2、4 骨裁剪归一化、palette 上传）已就绪 | 现状可核对 `tressfxSimIntegration.comp` / `tressfxSimStrandLevel.comp` / `renderableObject.cpp` |

## 3. 测量证据（2026-10-25，正式 Female 资产）

用 `Maps/SC_tressfx_female/Source/female_body.obj` 与 `female_guides.tfx`，
头部盒 x±22 / y124–180 / z±22 cm：

| 指标 | 实测 |
| --- | --- |
| 头部区域身体顶点 | 76,448 |
| 身体顶点最近邻间距 | p50 0.067 / p90 0.42 / max 1.17 cm |
| 发根 → 最近顶点 | p50 0.218 / p99 0.428 / **max 0.516 cm** |
| 发根 → 次近顶点 | p50 0.364 / p99 0.572 cm |
| 距离 > 1 cm 的发根 | 0 根 |
| `.tfx` 内容 | 40,334 guides × 32 点，**仅 positions**（strandUV/vertexUV/thickness/vertexColor 的 offset 全为 0） |

结论：最近顶点足够；插值的触发条件是换成低模绑定源（头皮间距到 1–2 cm 量级），
届时用已有的 `SignedDistancePointToTriangle` 做“最近面片重心插值”，不是默认路径。

## 4. P0：毛发资产视口

**目标**：打开 `type: "tressfx"` 的 `SM_*.json` → 专用视口 → 转动模型看物理响应 → 可复位重放。

- 预览世界：复用 `SC_model_preview` + `extraSceneObjects`，只注入毛发对象；不需要关卡上下文。
- 交互：左键拖 = 旋转预览对象（`RuntimeCommandType::SetMeshPlacement` 绝对 rotation）；
  中键/右键 = 相机 orbit；滚轮 = 缩放。旋转只作用于预览世界，不写 SC/SM。
- 复位：新增一条“请求一帧 ResetPositions”的命令；**不要用重载实现**（重载要重新解析 20 MB TFX）。
  shader 侧 `params.motion.y` 的 ResetPositions 分支已经存在。
- 守卫：资产声明了 `simulation.skinning` 时，视口显示“由蓝图 TressFX 组件承载”的说明，
  不静默剥离 `skinning`（剥离会让 `collision.capsules[].bone` 校验失效并给出误导性预览）。
- 验收：
  - 无 skinning 资产打开即可进视口，不依赖已打开关卡；
  - 拖动旋转时发丝相对头部滞后并回摆，`hair trace` 能证明根点跟随、梢部位移非零；
  - 同一次复位后重复同样的旋转，轨迹一致（确定性）；
  - 带 skinning 的资产给出明确提示，不冒充可预览。

## 5. 蓝图 TressFX 组件

现有 BP 组件体系是 `scene`/`mesh`/`camera`/`springArm`/三种灯，带 `id`/`parent`/局部 TRS，
身份 `__blueprint/<actor>/<component>`。新增一类 `tressfx` 组件：

| 字段 | 说明 |
| --- | --- |
| `modelPath` | tressfx 的 `SM_*.json` |
| `parent` | 默认指向角色 `mesh` 组件；加载期校验父链最终落在带骨架的 mesh 组件上 |
| `rootBone` | 可选，覆盖 SM 的 `rootBone`（回退骨） |
| `weights` | 默认 `"auto"`；显式 `.tfxbone`（`weights: "file"`）优先于自动派生 |

**触发时机**：世界构建期、父 mesh 的 renderable 建好之后。现有
`ResolveHairSkinningBindings` 已在所有 mesh 加载完成后执行，组件路径只需把
“source 场景对象名”换成父组件身份，其余（关节解析、胶囊骨映射、代理权重、绑定空间校验）复用。

**自动派生**（加载期一次，可缓存）：

1. 逐 strand 根点权重（绑姿最近顶点、2 cm、回退统计）；
2. 胶囊 `bone` → 目标骨架关节下标；
3. `collision.mesh` 代理的逐顶点权重（从同一具身体转移）；
4. 绑定空间一致性 + `maxNearestDistanceCm` 日志。

**不自动做**：胶囊摆放、SDF 代理生成（离线 `vl_build_collision_proxy`）、follow 发丝生成。

**规则变更**：

- `simulation.skinning.source` 变为可选：组件挂载时由组件提供；场景 JSON 独立毛发仍需写，
  缺失时加载期报“缺少绑定来源”。
- 胶囊 `bone` 的校验条件从“必须声明 `skinning`”改为“声明 `skinning` 即可”，不再要求 `source`。

## 5.5 TF 独立资产与专用 Tab（2026-10-26 用户决定）

毛发不再是 Mesh 的一种，而是**独立资产类型**：

- 文件前缀 `TF_*.json`；`type` 仍保留 `"tressfx"`（格式标识），独立性由**文档 kind**
  表达：新增 `EditorAssetDocumentKind::Hair` / `EditorAssetKind::Hair`，内容浏览器显示为
  独立类别（TressFX Hair），不再归入 Mesh。
- `AssetDocumentService` 为 Hair kind 接受 `TF_` 前缀 + `type: "tressfx"`，校验仍复用
  `MeshAssetValidator::BuildLoadPlan` 的 tressfx 分支；迁移期可同时接受旧的 `SM_` 前缀，
  迁移完成后收紧。
- **双击 `TF_*.json` 直接打开专用 Tab**（不是 Mesh 的 JSON/预览页）：复用已实现的
  `tressfxHairPreview.h`（skinning 守卫 + 旋转值类型）、`tressfxHairSimulationControls.h`
  （9 参数表/范围/格式化）、`materialInstanceAssetEditorPanel` 里的 `#resource-hair-tools`
  （左键转模型、Reset）与 `Simulation` 折叠段、`ui/editor-workspace.rml|.rcss` 的宿主。
  这些代码原样复用，只把入口从“Mesh kind + type 判定”改为“Hair kind”。
- 不改写 tab 实现：新增 Hair kind 的打开分支（`OpenResourceAsset` / content browser kind
  映射 / 双击分发），其余沿用现有预览世界（`SC_model_preview` + 单对象注入）与保存流程。
- 迁移顺序：**先做 TF kind + 专用 tab（资产身份）**，再做 A 迁移（行为下沉到放置）：
  A 的默认值会写进 TF 资产，先改身份可以避免两次改文件名。
- 迁移内容：3 个 `SM_female_hair*.json` → `TF_female_hair*.json`（A 之后收敛为 1 个），
  更新 `SC_tressfx_female*.json` 与蓝图组件里的 `modelPath`；`asset-organization.md`
  新增 `TF_` 行并从 `SM_` 行移除 tressfx 说明。
- 验收：
  - 内容浏览器中毛发是独立类别，双击打开毛发 Tab（视口 + 转模型 + Reset + Simulation 段）；
  - Mesh 类别下不再出现 tressfx 资产，误把 `TF_` 当 Mesh 打开会被拒绝；
  - 场景/蓝图引用 `TF_` 资产正常加载，`binding=component weights=mesh` 行为不变；
  - `editor_ui_tests` 覆盖 kind 映射、双击分发、Tab 复用与 `TF_` 校验。

## 6. P1：模拟参数面板

同一毛发视口内的折叠段，只覆盖可会话预览的标量：`gravity`、`wind`、`damping`、
`iterations`、`bendStiffness`、`globalShapeStiffness`+`Range`、`vspCoefficient`、
`vspAccelThresholdMax`；标出“需重载”的字段（`enabled`/`backend`/`skinning`/
`collision.mesh`/曲线/`strandRadius`）。双轨：会话覆盖即时预览；`Save to asset`
走既有 capability → validate → save，并提示保存后重载会复位模拟。

成本点：现有 capability 只有一级点路径与整对象/整数组替换，胶囊/曲线逐字段编辑需要
扩展索引路径模型。

## 7. P2：碰撞胶囊与曲线

- 胶囊列表（`a`/`b`/`radius`/`bone`，≤8）+ 视口 gizmo；
- 逐顶点曲线编辑器——等正式资产真的导出曲线数据再做。

## 8. 非目标

- 不新增 `GR_*` groom 资产类型，不做 UE 式 BindingAsset 源→目标网格重定向；
- 不做运行时调参入口；
- 不做 Marschner 发丝着色、像素覆盖积分、OIT、发丝阴影——那是渲染模型，不是本计划；
- 不自动摆放碰撞胶囊。

## 9. 任务拆解

| 序 | 任务 | 主要落点 | 验证 |
| --- | --- | --- | --- |
| T1 | P0 视口：单对象预览 + 转模型 + 复位 + skinning 守卫 | `source/editor/**`、`source/engine/**`、预览世界注入 | 编辑 UI 用例 + 手工烟测 |
| T2 | 复位命令 | `runtimeCommand*`、`hairSimulationPass.*`、console `hair` | 模块测试 + 一次 run |
| T3 | 根点权重派生（纯 CPU + 描述符） | `source/hair/**`、`source/mesh/**` | GTest（转移/回退/裁剪归一化/身份） |
| T4 | 蓝图 `tressfx` 组件 | `source/script/**`、世界构建、绑定注入 | BP 校验用例 + 加载烟测 |
| T5 | 参数面板 | editor capability + RmlUi | 编辑 UI 用例 + 保存往返 |

门禁：改 `documents/**` 后跑 `py -3 tool/validate-docs.py`；改 `source/**` 后跑
`pwsh -File tool/architecture-boundary-audit.ps1`；测试登记遵守 `contracts/testing.md`
（只用 GoogleTest + 根 `vulkanlearn_add_gtest`，不新增 runtime test 标志）。

## 通用性：per-placement renderable 与 placement binding

毛发是第一个需要**逐放置 GPU 状态**的网格类型（三份位置历史、params UBO、绑定），
但机制不应只为 tressfx 写：

- **per-placement renderable**：mesh key 显式区分 shared / placement-scoped；渲染侧在
  `ResolvedRenderSceneBuilder::Build` 之前有一步通用的“确保实例存在”，输入
  `(templateKey, instanceKey, placementBinding)`。hair 只是第一个 provider。
- **placement binding**：场景对象只带一个结构化记录（`kind` + 参数），沿
  scene object → `WorldMeshObject` → `RenderDrawPacket` 传递；校验与解析按 `kind`
  分派，不再新增 `__tressfx*` 专用字段（旧字段可保留兼容读取）。
- **handler**：`kind` 的 handler 负责解析源对象/关节、派生权重（调 A1 helper）、写绑定；
  缓存键为 `(template model key, kind + 参数, source asset identity)`。
- **生命周期**：实例 renderable 的创建与退休走同一套 resource cache / World 世代，
  不为某一类型写单独回收路径。
- **接入成本**：一个 enum + 一处 handler 注册/分派即可，不引入框架；新增类型只需
  提供自己的 clone 与 binding 解析，不修改 reconcile 主流程。

## 编辑边界：关卡 vs 资产

两类修改必须分开，UI 与保存路径都按这个边界执行：

| 层 | 能改什么 | 在哪改 | 落盘到 |
| --- | --- | --- | --- |
| 关卡（level） | actor/组件实例的 TRS、可见性、BP 实例属性覆写与组件覆写（仅资产白名单授权的字段） | 关卡编辑器 Outliner/Details | `SC_*.json` 或实例覆写块 |
| 资产（asset） | 毛发描述符（`strandRadius`/`simulation.*`/`collision.*`/材质槽）、BP 组件树与 `tressfx` 字段、`MI_*.json` 材质参数 | 对应资产 tab | `SM_*.json` / `BP_*.json` / `MI_*.json` |

- 关卡侧**不提供**毛发资产字段与模拟参数编辑：`Hair Asset`/`Root Bone`/`Root Weights`
  与 `Simulation` 面板只读，提示“在蓝图资产/毛发资产里编辑”。`tressfx` 组件不进入
  `componentInstanceEditable`，因此关卡没有合法的实例覆写通道。
- 资产 tab 的 `Simulation` 面板保存的是**资产**：一次保存影响所有放置该资产的实例；
  保存后需要重新加载才生效，并且会复位模拟。UI 必须写明这一点。
- 会话级预览（转模型、`hair set` 实时覆盖）既不是关卡修改也不是资产修改：不写文件，
  退出/重载即丢弃。
- 不允许“借当前关卡实例去改资产”；资产修改只能经资产 tab 的标准
  `modify → validate → save`（带 revision）。

## 10. 进度

**2026-10-25：T1/T2 落地（未提交）。**

- 视口：`type: "tressfx"` 的 `SM_*.json` 打开单对象预览（`SC_model_preview` + 注入，
  scale=1），kind 显示 TressFX Hair；带 `simulation.skinning` 的资产不预览，状态区提示
  由蓝图组件承载（目录摘要 → 作者文档快照 → `MeshAssetLoader` 三层守卫）；左键拖转模型
  （`RotatePreviewModel` → `SetMeshPlacement(mesh|preview_model)`），中/右键仍是相机；
  工具条 Reset 按钮发 `ResetHairSimulation`。
- 复位：`RuntimeCommandType::ResetHairSimulation` + `RenderSystem` 单槽 →
  `HairSimulationPass::RequestReset`，只在该对象下一个有 substep 的帧消费，复用
  `motion.y` 分支并关闭 rebase，reason=`requested`；Console `hair reset [objectName]`。
- 验证：`cmake --build build -j8` 通过；`hair_asset_tests` 261/261；
  `editor_ui_tests` 770 passed / 5 skipped（既有 symlink 用例）；
  architecture-boundary audit 通过。GUI 烟测仍待人工执行（步骤见子任务报告与
  `contracts/game-ui-stack.md` 的 TressFX Hair Asset Viewport 节）。
- 当时未做：T3 权重派生、T4 蓝图组件、T5 参数面板；旋转枢轴仍是对象原点（绕 Y≈原地转，
  绕 X 会绕脚底画弧）。

**2026-10-25：T3 落地（未提交）。**

- 描述符：`simulation.skinning.weights: "rootBone" | "mesh"`（缺省 rootBone）；未知值、
  与 `boneWeights` 并存都拒绝；`mesh` 需要 GPU backend；缓存身份只对 `mesh` 追加
  `|rootweights=mesh`（既有资产身份不变）。
- 导入：`MeshSection::tressfxRootPositions`，每 guide 一个 model 空间根点（≈0.48 MB）。
- 转移：`TressfxSkinTransferPolicy::{RejectUnmatched, RecordUnmatched}`；碰撞代理保持
  拒绝策略，发丝根点用记录策略并在加载期逐根回退 `rootBone`，全部超距才硬错误。
- 纯函数：`TressfxPackStrandBoneWeights`（前 4 + 归一化 + 回退）、
  `TressfxStrandBoneWeightsFromTransfer`（CSR → 逐 strand）。
- 加载：`ResolveHairSkinningBindings` 在 `weights=="mesh"` 时复用
  `BuildProxySkinBodyData` + 同一套最近顶点转移，打印
  `[TressFX] rootWeights: strands=… joints=… maxNearest=… fallback=…`。
- 验证：`hair_asset_tests` 273/273、`material_schema_tests` 29 项、architecture audit
  通过、`main` 构建通过。正式资产实机 A/B（mesh vs rootBone）未跑，需要 GUI。
- 边界：`weights = "mesh"` 仍要求 `simulation.skinning.source`，组件化（T4）再放宽。

**2026-10-25：T4a 落地（未提交，运行时与校验；编辑器 UI 未做）。**

- 组件：`type: "tressfx"`，字段 `modelPath`/`rootBone`/`weights`（`auto`/`rootBone`/`mesh`），
  必须有 `mesh` 祖先；校验在组件树拓扑期拒绝无 mesh 祖先、缺 modelPath、非法权重值。
- 展开：生成场景 `mesh` 对象，放置级绑定写在生成对象上
  （`__tressfxSkinningSource`/`__tressfxParentModelPath`/`__tressfxRootBone`/`__tressfxWeights`），
  不写作者 JSON、不生成临时 SM；模板键与结构签名包含父组件身份。
- 加载：`ResolveHairSkinningBindings` 组件绑定优先于 SM `source`；两者都没有则硬错误。
- 权重：组件显式 > SM 显式 > 组件默认 `mesh`；`.tfxbone` 优先于自动升级，显式 `mesh`
  与文件并存按冲突拒绝（纯函数 `ResolveTressfxRootWeightChoice`）。启动打印一次
  `[TressFX] hair binding: … binding=component|scene weights=mesh|bone|file`。
- 验证：`hair_asset_tests` 277/277、`ScriptTressfxComponent` 5/5、`material_schema_tests`
  29/29、architecture audit 通过、`main` 重新链接。`script_tests` 全量在**本工作树资源
  副本**缺 `Maps/SC_gasp/**` 时有 65 项动画/gameplay 失败，与本次改动无关。
- 残留：运行时 Spawn 蓝图仍未准备 tressfx 组件、也不走绑定解析（T4c）；组件局部 TRS
  必须是单位阵，没有“相对角色偏移”语义；编辑器 UI 未做（T4b）。

**2026-10-25：T4b 落地（未提交）。**

- 蓝图资产页组件下拉新增 `TressFX`（Scene 侧不提供）；添加时父级自动取最近 mesh
  组件，无 mesh 祖先或空 `modelPath` 时拒绝并提示。
- Details：`Hair Asset`、`Root Bone`、`Root Weights`（`auto`/`rootBone`/`mesh`，
  缺省 `auto`）；父级下拉只列 mesh；局部 TRS 与图元碰撞控件隐藏且写入被忽略；
  不进入 `componentInstanceEditable`。
- 数据流仍是“改本帧 JSON 草稿 → `ModifyAssetDocument` 整字段变更”，不直接碰 World。
- 验证：`editor_ui_tests` 782 项（777 passed / 5 既有 symlink skip），新增 7 项；
  `main` 构建通过；architecture audit 通过。GUI 烟测未跑。
- 残留：运行时 Spawn 仍不带毛发（T4c）；Scene 侧 TressFX 组件无实例白名单，
  是否只读待定。

**2026-10-25：T5a 落地（未提交，实时覆盖通道；面板与保存未做）。**

- 命令：`hair set <object> <field> <value...>` → `RuntimeCommandType::SetHairSimulationParameter`
  → GT 校验（名称/字段/值域，拒绝不夹取）→ `RenderSystem` 有界队列（64）→
  `HairSimulationPass` 在下一个有步进的帧按名匹配 → 写该 `RenderableObject` 自己的
  config 副本 → 同帧 UBO 生效。不写资产、不改缓存身份、重载后回作者值。
- 字段：`gravity`/`wind`（3 分量）、`damping`、`iterations`、`bendStiffness`、
  `globalShapeStiffness`、`globalShapeEffectiveRange`、`vspCoefficient`、
  `vspAccelThresholdMax`；其余字段（`enabled`/`backend`/`rootFixedPoints`/曲线/碰撞/
  `skinning`/`strandRadius`）明确按“需重载”拒绝。仅 GPU backend。
- 分工：GT 不判断对象存在性（毛发 renderable 不在 World 里），由渲染线程判定并报告。
- 验证：`hair_asset_tests` 287/287（新增 `TressfxLiveParams` 10 项）、audit 通过、
  `main` 构建通过。GUI 烟测未跑。
- 已知缺口：队列不绑定 World 世代（早到的命令可能命中后续同名对象）；T5b 可收紧。

**2026-10-25：T6 取证（蓝图预览 / 关卡未运行可见性）。**

- 结论：链路本身已通，没有发现“看不见”的代码缺口。三个无 GUI 探针（`Generated/Validation/SC_tressfx_component_probe/`）分别验证：
  1) 关卡里 `blueprintPath` 放置 BP（body + hair）→ `binding=component weights=mesh rootWeights … fallback=0`；
  2) 内联 `~preview` actor（等价蓝图 tab 预览注入）→ 同上；
  3) 静态毛发组件（无 simulation/skinning）→ 正常提交、无 sim dispatch、stderr 空。
- 绘制不受模拟门控：draw 是普通 transparent pass，只有 compute 模拟被 `enabled/backend` 决定；
  不存在“没 pose 就不画”的短路。蓝图纸预览与关卡放置共用同一条组件展开链。
- 唯一改动：`blueprintAssetLoadSession` 的预准备集合从 `type=="mesh"` 改为
  `IsScriptMeshComponent`（mesh + tressfx），为后续 spawn/缩略图预准备铺路；该路径当前没有消费者。
- 残留：GUI 肉眼确认未做；探针资产留在 Generated（可删）。

**2026-10-25：T6 结论后的下一步。**

- 待办：T5b（参数面板 + 保存回资产）、T4c（运行时 Spawn 带毛发）、缩略图/预准备路径的 tressfx 性能与失败验证。

**2026-10-25：用户对三个待定的答复。**

- **运行时 Spawn 会用到** → T4c 要做：spawn 出来的蓝图演员要自动准备 tressfx 组件、重算绑定
  （父组件身份随新 actor id 变化）并走同一条网格模板路径；当前只有“场景里摆好的实例 +
  既有实例的组件增删”覆盖。
- **资源副本已同步** `Maps/SC_gasp`（365 文件 / 225.6 MB，从主资源目录 robocopy 补齐）；
  全量 `script_tests` 的基线要在 T5b 构建结束后重跑，之前的 65 项失败属于缺资源。
- **关卡侧 TressFX 组件字段改只读**：Scene/关卡 Details 里的 `Hair Asset`/`Root Bone`/
  `Root Weights` 不再假装可编辑，显示只读并提示“在蓝图资产里编辑”。

**2026-10-25：③ 落地（未提交）。**

- Scene/关卡 Details 的 tressfx 字段改为 `PropertyInfo` 只读行 + 提示
  “TressFX binding is authored in the Blueprint asset … level instances cannot override”；
  不再生成输入控件。
- 事件处理层对 `type=="tressfx"` 的 `modelPath`/`rootBone`/`weights` 写入直接丢弃，
  旧控件或脚本残留事件也不会产生 `componentOverrides`。
- 验证：`editor_ui_tests` 791 项（786 passed / 5 既有 skip，新增 2 项
  `EditorUiTest.SceneTressfx*`）、audit 通过、`main` 构建通过。GUI 烟测未跑。

**2026-10-25：T4c 调研结论（未改代码，按停止条件停下）。**

- 现状：spawn 不会崩，但毛发**借用模板的 renderable**——`World::CloneScriptBlueprintMeshes`
  整份复制 `WorldMeshObject`（含 `meshKey`），而 GPU 毛发 key 带 `|instance=<身份>`；
  spawn 只改写了 `sceneObjectIdentity`/`debugName`。结果是绑定、三份位置历史、params UBO
  全是模板的：单实例时头发跟着隐藏模板身体（原点），多实例共享同一份模拟历史。
- 不能只重写 key：`ResolvedRenderSceneBuilder` 对 cache 里没有的 `mesh.key` 直接硬失败
  （`ResolvedRenderScene.MissingRenderableObject`），必须在同帧 resolve 前把新 renderable
  建出来。
- 方案 A（推荐）：`WorldMeshObject` 携带 placement binding（spawn 时由 GT 按新 id 生成），
  `RenderDrawPacket` 带 binding + 模板 key；`RenderSystem` 在 resolve 前对账——未知 hair key
  从模板 renderable 克隆（共享 imported、新建自己的 sim buffer/UBO）→ 解析 joint →
  派生权重（按 (hair modelKey, source skeletal 身份) 缓存）→ 绑定。约 10–12 个文件；
  风险在“resolve 前完成对账”“权重缓存”“spawn renderable 的销毁/退休接入 World 世代”。
- 方案 B（预建 N 个 per-placement 槽）快但受 N 限制、显存浪费；方案 C（spawn 毛发共享静态）
  会跟随错误身体，不建议。
- 需要用户决定：按 A 拆两步（A1 抽可复用权重 helper + 单测；A2 binding 进 packet + 帧前
  reconcile + clone + 缓存 + 回收），还是先只加“spawn 毛发暂时跳过 + 告警”的 interim 保护。

**2026-10-25：T5b 落地（未提交）。**

- 毛发资产页 Details 新增 `Simulation` 折叠段（仅可预览 tressfx，即无 `skinning`）：
  9 个控件，范围/单位/默认值来自共享表，初值取资产草稿。
- 拖动 = **会话预览**：`SetHairSimulationParameter` 目标 `preview_model`，不写资产/关卡；
  预览世界未发布时不投递。
- `Save to Hair Asset` = `mesh.modify`（带快照 revision）→ Save Content 确认 → `mesh.save`；
  文案写明“影响所有放置、重载生效并复位模拟”。
- Mesh 能力表新增 9 个 `simulation.*` 一级字段（不按文档类型过滤，非 tressfx 由加载器忽略）；
  值域在 modify/validate/save 阶段由 `BuildLoadPlan` 校验。
- 关卡侧没有毛发参数入口（测试锁定）；③（组件字段只读）随后单独落地。
- 验证：`editor_ui_tests` 789 项（784 passed / 5 既有 skip）、`hair_asset_tests` 287/287、
  audit 通过、`main` 构建通过。GUI 烟测未跑。

## 11. 未决

- 组件指定身体：默认 `parent` 链；若出现“毛发不是角色子节点”的作者需求，再加显式 `attachTo`。
- `.tfxbone` 与自动权重的优先级：已定为显式文件优先，需要一次 A/B 证据确认不会掩盖自动派生的问题。
- 参数面板的保存粒度：整段 `simulation` 一次性提交，还是逐字段提交（受 capability 扩展影响）。

## 12. 交付记录（2026-10-25）

- 代码：工作树 `codex/tressfx5-hair-port` 全量提交（包含此前未提交的物理/调度/trace 工作与本轮的 T1–T5、T6、③）。
- 验证：`hair_asset_tests` 287/287、`editor_ui_tests` 791（786 passed / 5 skipped）、
  `material_schema_tests` 29/29、`script_tests` 455（454 passed / 1 skipped）、
  `validate-docs.py` 通过、`architecture-boundary-audit.ps1` 通过。
- GUI 烟测（无人工交互，截图在 `artifacts/delivery/`）：关卡蒙皮毛发、蓝图组件毛发、
  编辑器关卡视口、毛发资产预览；日志确认 `binding=component weights=mesh` 与
  `rootWeights … fallback=0`。
- 已知缺口：运行时 Spawn 的蓝图演员尚未生成 per-instance 毛发 renderable（T4c，方案 A 待排期）。

**2026-10-26：T4c 落地（未提交）。**

- 运行时 Spawn 的蓝图演员拥有独立毛发 renderable：placement binding 沿 scene object →
  `WorldMeshObject` → `RenderDrawPacket` 传递；`RenderSystem` 在 resolve 前对账，
  `CloneForPlacement` + `ApplyPlacementBinding` 按 kind 分派；模板未解析时用来源 draw
  的骨架现场解析并用 A1 helper 派生权重（按 template/kind/source 缓存）。
- spawn 克隆重写放置级绑定身份并建立该 actor 的组件网格模板；`ToWorldMesh` 补齐
  `animation`/`skinPose`/`skinSection`，修复 spawn 骨骼首帧无 pose 的 SSBO 缺失。
- 修复 `PrunePlacementRenderables` 的真实泄漏：键存活时把缺失计数清零，不能 erase 跟踪项。
- 新增 Console `script <play|pause|stop>`（与编辑器 Play 同一个 `ScriptRuntime::Control`
  入口），使 spawn 可以无 GUI 验证；不是 runtime test flag。
- 验证：探针 `Generated/Validation/SC_hair_spawn_probe/` 日志显示
  `[Placement] instance renderable`、`[Placement] tressfx root weights … fallback=0`、
  `runtime placement renderables: 1→0`、`placement-scoped renderables in cache: 2→1→0`，
  stderr 空；截图 `artifacts/delivery/spawn_final.png`。
- 回归：hair 297/297、script 456（455 passed / 1 skipped）、editor 791（786 passed / 5 skip）、
  material 29/29、architecture audit 与 validate-docs 通过。
- 已知限制：spawn 复制模板已解析绑定（同蓝图同骨架）；`rootBone` 覆盖不一致报错；
  `weights` 覆盖不重新派生；每次 spawn 复制约 2 MB `strandBones`。

**交付记录（2026-10-26）。**

- 本次交付提交：`【Mod】support per-placement hair renderables for spawned blueprints`，
  包含 A2 的通用 placement 机制、三个 spawn 缺口修复、prune 泄漏修复与 `script` 控制台命令。
- 未包含：曲线/胶囊编辑、UE 式 BindingAsset 重定向、毛发着色模型。
