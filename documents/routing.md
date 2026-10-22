# 源码到文档路由

改代码前按**源码路径**匹配**全部**适用规则（不是首条命中）。命中的 `contracts/` 条目可以直接当事实；
命中的 `rationale/` 条目只解释意图，技术描述可能已过时。

本表的引用完整性由 `py -3 tool/validate-docs.py` 校验：文档必须存在、路径 glob 必须匹配到真实文件、
本表列出的门禁规则名必须**逐字**存在于 `tool/architecture-boundary-audit.ps1`。

## 精确规则

"守护门禁"列写的是脚本里 `-RuleName` 的**字面值**（不是别名、不是概括）。规则内容以脚本为唯一真相，
不要把规则正文抄进文档。

| 源码路径 | 必读文档 | 守护门禁（逐字 `-RuleName`） |
| --- | --- | --- |
| `source/ui/widgets/**` | `contracts/ui-component-system.md` | — |
| `source/ui/**` | `contracts/ui-component-system.md`、`contracts/game-ui-stack.md` | — |
| `source/editor/**` | `contracts/ui-component-system.md` | — |
| `source/editor/assets/editorRetargetPreview.*`、`source/editor/ui/retargetAssetEditorPanel.*`、`tool/editor-ui-tests/retargetPreviewTests.cpp` | `contracts/animation-asset-import.md`、`contracts/animation-runtime-skinning.md`、`contracts/ui-component-system.md`、`contracts/testing.md` | — |
| `source/script/**`、`script/**`、`tool/script-tests/**`、`source/editor/scene/scriptBlueprintAuthoring.*` | `contracts/csharp-blueprint-runtime.md`、`contracts/testing.md` | — |
| `source/editor/service/**`、`source/editor/command/**` | `contracts/asset-modify-api.md`、`contracts/material-instance-editor.md`、`contracts/material-param-authoring-and-reflection.md` | — |
| `tool/asset-edit/**` | `contracts/asset-modify-api.md`、`contracts/testing.md` | — |
| `tool/game-export/**`、`tool/game-export-tests/**` | `contracts/csharp-blueprint-runtime.md`、`contracts/asset-organization.md`、`contracts/testing.md` | — |
| `tool/animation/**`、`tool/animation-tests/**` | `contracts/animation-preflight-tools.md`、`contracts/testing.md` | — |
| `source/animation/**`、`source/mesh/loader/animation/**`、`tool/animation/gaspImportReport.*`、`tool/animation/compare_gasp_import.py`、`tool/animation-tests/skeletalImportTests.cpp` | `contracts/animation-asset-import.md`、`contracts/testing.md` | — |
| `source/animation/animationMontage.*`、`source/animation/animationSkeleton.*`、`source/animation/animationSequenceData.*`、`source/mesh/loader/common/meshAssetLoader.*`、`source/mesh/validation/meshAssetValidator.*`、`source/mesh/meshAssetTypes.h`、`tool/animation-tests/animationMontageTests.cpp` | `contracts/animation-montage.md`、`contracts/animation-asset-import.md`、`contracts/testing.md` | — |
| `source/animation/animationScriptData.*`、`source/world/worldAnimation.cpp`、`source/script/scriptRuntime.cpp`、`tool/animation-tests/animationScriptDataTests.cpp`、`tool/animation-tests/notifyStateTests.cpp` | `contracts/animation-montage.md`、`contracts/animation-runtime-skinning.md`、`contracts/testing.md` | — |
| `source/animation/skeletalPlayback.*`、`source/animation/footPlacement.*`、`source/animation/skinPose.h`、`source/world/worldAnimation.cpp`、`shader/glsl/engine/skinnedVertexFactory.glsl`、`tool/animation/build_gasp*.py` | `contracts/animation-runtime-skinning.md`、`contracts/animation-motion-warping.md`、`contracts/animation-montage.md` | — |
| `source/animation/motionWarping.*`、`source/script/scriptPhysics.cpp`、`script/VulkanLearn.Runtime/AnimationPlayback.cs`、`tool/animation-tests/motionWarpingTests.cpp` | `contracts/animation-motion-warping.md`、`contracts/animation-montage.md`、`contracts/testing.md` | — |
| `source/animation/motionDatabase.*`、`source/animation/motionDebug.h`、`source/animation/motionFeatures.*`、`source/animation/motionHistory.*`、`source/animation/motionMatching.*`、`source/animation/motionRuntime.*`、`source/world/worldAnimation.cpp`、`tool/animation/motionDatabaseBuild.*`、`tool/animation/browse_motion_database.py`、`tool/animation/verify_gasp_motion_database.py`、`tool/animation/capture_gasp_p4.py`、`tool/animation/capture_gasp_p5.py`、`tool/animation-tests/motionDatabaseTests.cpp`、`tool/animation-tests/motionHistoryTests.cpp`、`tool/animation-tests/motionMatchingTests.cpp` | `contracts/animation-motion-features.md`、`contracts/testing.md` | — |
| `source/animation/motionRuntimeDatabase.*` | `contracts/animation-asset-import.md`、`contracts/animation-motion-features.md`、`contracts/testing.md` | — |
| `source/debug/**`、`source/ui/debugDrawOverlay.*`、`source/script/scriptDebugDraw.h`、`script/VulkanLearn.Runtime/DebugDraw.cs`、`tool/debug-draw-tests/**`、`tool/editor-ui-tests/debugDrawOverlayTests.cpp`、`tool/script-tests/scriptDebugDrawTests.cpp`、`tool/script-tests/managed/DebugDrawFixtures.cs`、`tool/debug/capture_draw_debug.py` | `contracts/debug-draw.md`、`contracts/testing.md` | — |
| `source/resource/image/textureAssetValidation.h`、`source/textureAssetLoader.cpp` | `contracts/texture-asset-json-v1.md`、`contracts/asset-modify-api.md` | — |
| `source/scene/validation/**` | `contracts/asset-modify-api.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/render/offline/**` | `contracts/vl-offline-rendering.md` | — |
| `source/hair/**`、`tool/hair/**`、`tool/hair-tests/**` | `contracts/tressfx-asset-import.md`、`contracts/testing.md` | — |
| `source/mesh/loader/hair/**`、`shader/glsl/engine/tressfxVertexFactory.glsl`、`shader/glsl/engine/tressfxCoverage.glsl`、`shader/glsl/hair/**`、`tool/material-schema-tests/tressfxMaterialTests.cpp` | `contracts/tressfx-asset-import.md`、`contracts/material-mesh-pass-composition.md`、`contracts/testing.md` | — |
| `source/world/**` | `contracts/vulkanlearn-architecture.html` | World mesh object cutover、Mesh object build plan contract、World build plan mesh object contract、WorldBuilder mesh object plan consumption、World loader runtime error code contract、World reload failure state machine validation |
| `source/render/backend/**` | `contracts/vulkanlearn-architecture.html` | Redundant Vulkan device abstraction cutover、Vulkan device boundary concrete-only contract、RendererBackend device boundary escape hatch、RendererBackend hides concrete device boundary、RendererBackend device boundary forward declaration、Backend texture handle bridge contract、Swapchain resize production lifecycle |
| `source/render/resource/**` | `contracts/descriptor-imageinfo-management.md` | Buffer resource package lifecycle handle adoption、Texture resource package lifecycle handle adoption、Renderer resource load result contract、Renderer resource load runtime error code contract |
| `source/renderGraph.*` | `contracts/vulkanlearn-architecture.html` | Render graph reload production lifecycle、Render graph resource retire ownership、World graph retire queue validation、RenderGraph resource package lifecycle handle adoption |
| `source/passBarrier.*`、`shader/glsl/uiOverlay.*` | `contracts/debug-draw.md`、`contracts/descriptor-imageinfo-management.md`、`contracts/shader-hot-reload.md` | — |
| `source/renderSystem.*` | `contracts/vulkanlearn-architecture.html` | RenderSystem frame boundary、Draw frame boundary、Resolved draw resource ownership |
| `source/render/pass/**` | `contracts/vulkanlearn-architecture.html` | Render pass gameplay boundary |
| `source/shaderCompiler.*`、`source/shader/build/**` | `contracts/shader-build-cache.md` | — |
| `source/shader/reload/**` | `contracts/shader-hot-reload.md` | — |
| `source/material/**` | `contracts/material-param-authoring-and-reflection.md` | Descriptor writer explicit context boundary、Vulkan-native descriptor write type、Vulkan-native descriptor write contract |
| `shader/glsl/**` | `contracts/material-param-authoring-and-reflection.md`、`contracts/gbuffer-channel-ownership.md` | — |
| 论文 case 验证：`tool/validation/**` | `rationale/shading-model-case-records.md`（证据归档）、`plan/rendering/shading-model-alignment-plan.md` | — |
| `ui/**/*.rcss`、任何写 UI 样式处 | `rationale/pitfalls.md` 的 `[rcss-alpha]` 条 | — |
| 改右键上下文菜单（`ui/editor-workspace.rml` 的 `#editor-context-menu`、面板的 `mouseup/button==1` 处理、菜单目标解析） | `contracts/ui-component-system.md` 的「右键上下文菜单」节 | — |
| `source/render/editor/**`、`shader/glsl/pass/editorGrid.*`、`config/renderGraphConfig.json` 的 `editorGrid` pass | `contracts/material-mesh-pass-composition.md` 的「编辑器网格 pass」节 | — |
| `source/editor/ui/editorViewportAxisIndicator.h`、`#viewport-axis-indicator` 叠层 | `contracts/ui-component-system.md` 的「角落坐标轴指示器」节 | — |
| `source/pipeline/**` | `contracts/vulkanlearn-architecture.html` | Pipeline Vulkan creation diagnostics、Pipeline Vulkan creation diagnostic context |
| `source/commonFunction.h` | `contracts/coding-guidelines.md` | CommonFunction depth helper cleanup、Source comment clarity |
| `source/engine/**` | `contracts/vulkanlearn-architecture.html` | Runtime test hook command boundary、Launch runtime tests use CommandBus、Runtime test state stays out of EngineLoop、LoadWorld path resolve runtime error contract |
| `schema/*.schema.json` | `contracts/texture-asset-json-v1.md` | — |
| `tool/architecture-boundary-audit.ps1`、`tool/architecture-final-validation.ps1` | `contracts/vulkanlearn-architecture.html` | Architecture final validation script contract |
| `documents/**` | `README.md`（分层规则） | Vulkan-native architecture wording |

`extern/**` 是第三方依赖，除 `extern/BLAKE3` 的锁定说明外不在本表覆盖范围（见"未覆盖路径"）。

warp 消费点：`skeletalPlayback.*` 在 GT tick 内调用原生窗口队列；C# 通过
`PlayMontage(montage, targets)` 只提交 Montage id 与 target 世界位置，
`worldAnimation.cpp` 解析 `targets` 后由 `animationScriptData.cpp` 按 Montage 资产窗口
装配并回传 `warp`/`rootMotion`；`scriptPhysics.cpp` 在 Jolt 更新前用
`rootMotionActive` 速度替代 walk/jump。窗口门控、target 模型空间换算、Bone provider
骨骼偏移与单 tick 根位移以 `contracts/animation-motion-warping.md` 为准；Montage 窗口/
notify/playMontage 请求与拒绝矩阵以 `contracts/animation-montage.md` 为准；资源侧的
提交/消费实现
（`Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs`）在资源仓库，不在本表 glob 校验范围。

## 主题型契约（不绑定单一源码路径）

这些契约按**改动性质**触发，而不是按路径。列出它们是为了让"没有路径路由的契约"显式可见，
而不是被当成孤儿。

| 契约 | 何时必读 |
| --- | --- |
| `contracts/asset-organization.md` | 新增/移动资源目录，或改 `resourcePath` 布局时 |
| `contracts/lookdev-scene.md` | 改文章资产审阅场景（`SC_lookdev_article`）、UE 资源导入、固定曝光/显示预设或截图时 |
| `contracts/physical-lighting-and-exposure.md` | 改厘米世界长度、光源物理单位（lux/candela）、相机 EV100 或曝光模式时 |
| `contracts/csm-shadow-map-m1.md` | 改 CSM 阴影贴图资源、分层 framebuffer、级联设置或相关 debug view 时 |
| `contracts/debug-runtime-commands.md` | 新增/修改控制台调试命令（含相机控制与截图）时 |
| `contracts/environment-update-scheduler.md` | 改环境贴图更新调度、SH 广播、dirty generation 或 IBL barrier 时 |
| `contracts/material-mesh-pass-composition.md` | 改材质求值、Base/ShadowDepth 模板、Composer 身份或 Shadow 路由时 |
| `contracts/animation-runtime-skinning.md` | 改骨骼 SM 加载、动画时钟/快照、蒙皮 SSBO、姿态/速度历史或动画预览控件时 |
| `contracts/shader-structure-and-material-function.md` | 改 Shader Structure、Material Inputs/Function 组合或 Shading Model 边界时 |
| `contracts/testing.md` | 改测试入口、测试目标或新增测试时 |
| `contracts/boundary-rules.generated.md` | 想知道 64 条边界规则各是什么、作用于哪些路径时（**生成物，勿手工编辑**） |
| `contracts/README.md` | 想知道契约层的写作与校验约定时 |
| `contracts/diagrams/README.md` | 改架构图、或改图所描述的那条链路时 |

## 架构图

图源是**契约**：IR 里的组件、边界与有向连接是拓扑真相，逐节点 `sources` 钉在某个 40 位 revision
上（由 archify 的 `finalize` 校验文件存在与行号范围）。产物 HTML 由 IR 生成，**不要手工编辑产物**。

| 源码路径 | 必读图源 | 图说明什么 |
| --- | --- | --- |
| `source/engine/engineLoop.cpp`、`source/ui/uiSubsystem.cpp` | `contracts/diagrams/editor-frame-sequence.json` | 一帧内从平台事件到上屏的顺序：UI 输入独占、GT 稳定帧边界、快照发布、可见性收口 |
| `source/world/loading/world*.cpp`、`source/world/loading/worldGraphTransactionCoordinator.cpp` | `contracts/diagrams/world-graph-transaction-lifecycle.json` | World/RenderGraph 事务：隔离准备、提交前校验、无抛提交、GPU epoch 退休 |

`py -3 tool/validate-docs.py` 会检查每张图都能从本表到达，且它引用的源码证据仍然成立
（路径存在、行号在文件范围内）—— 图与代码漂移时门禁失败，而不是让人以为图还是对的。

## 门禁规则

`tool/architecture-boundary-audit.ps1` 定义代码**不得违反什么**：64 条命名规则，由
`tool/architecture-final-validation.ps1` 调用。

- **规则清单不在本文件里。** 它是生成物：
  `documents/contracts/boundary-rules.generated.md`，由
  `tool/generate_boundary_rule_index.py` 从脚本生成，`tool/validate-docs.py` 会在生成物过期时判失败。
  改规则请改脚本再重新生成 —— 手工维护第二份清单正是会漂移的东西。
- 规则**正文**以脚本为唯一真相。不要把它抄进任何文档。
- 上表的"守护门禁"列写的是脚本里 `-RuleName` 的**字面值**（不是概括、不是别名），
  由 `tool/validate-docs.py` 逐字核对。

## 改动类型 → 必读

按**改动性质**匹配，与上面的路径规则叠加。

| 改动类型 | 必读文档 |
| --- | --- |
| 新增/修改 shader（任何 `shader/glsl/**`） | `contracts/material-param-authoring-and-reflection.md`、`contracts/gbuffer-channel-ownership.md` |
| 改材质参数 schema 或 MI 格式 | `contracts/material-param-authoring-and-reflection.md`、`schema/README.md` |
| 改编辑器命令协议 | `contracts/material-instance-editor.md` |
| 修改 Scene/Mesh/Texture 标准修改接口 | `contracts/asset-modify-api.md`；设计背景见 `plan/ui/asset-modify-api-plan.md`（非契约） |
| 加/改渲染 pass 或 RenderGraph 资源 | `contracts/vulkanlearn-architecture.html`、`config/renderGraphConfig.json` |
| 改 UI 组件或停靠布局 | `contracts/ui-component-system.md` |
| `source/ui/widgets/workspace/**`、`source/editor/ui/editorDockingController.*`（面板集合、默认布局、标签组归属、布局持久化格式） | `contracts/ui-component-system.md` 的「面板与标签组」节 | — |
| 改场景对象校验/加载 | `contracts/vulkanlearn-architecture.html`、`schema/scene.schema.json` |
| 改测试入口或新增测试目标 | `contracts/testing.md` |
| 移动/新建/删除文档 | `README.md`（分层与硬纪律）、本表 |

## 未覆盖路径

以下模块**当前没有契约文档**。改动时按源码与相邻模块说明处理，并显式报告缺口，不要拿相邻专题替代：

- `source/platform/**`（SDL 平台层与输入仲裁）
- `source/engine/testing/**`（runtime validation services）
- `source/core/**`
- `extern/**`（第三方依赖；除 `extern/BLAKE3` 的锁定说明外不写文档）
- 地形：**不存在运行时实现**。只有编辑器资产层的 `EditorAssetKind::Terrain` 与
  `objectType == "terrain"` 的 MI 导航解析；`SceneAssetValidator` 不接受 `terrain` 场景对象。

## 组合规则

- 精确规则与"改动类型"规则**叠加**，不采用首条命中。
- 命中 `contracts/` 与 `rationale/` 同时覆盖某主题时，以 `contracts/` 为准，并把 `rationale/` 中与之冲突的
  描述当作待修正项报告。

## 规划入口（非契约）

- [GASP 与 C# 动画控制开发计划](plan/architecture/gasp-csharp-animation-plan.md)：
  当前开发目标。C# 高层状态机与数据库筛选，原生 Motion Matching、轨迹/姿态历史、
  蒙皮、预览调试及障碍翻越的分阶段方案，每阶段提供可验收效果。
  这是未实现功能的规划，不替代当前契约；新模块落地时再补真实源码路由。
- [动画序列数据承载计划](plan/architecture/animation-sequence-data-plan.md)：
  Skeleton、Sequence sidecar 与 AnimMontage 三层资产；曲线/notify 真实时间与
  warp 窗口迁移目标。计划，非契约；当前行为仍以
  `contracts/animation-motion-warping.md`、`contracts/animation-asset-import.md` 为准。
- [原 ALS 计划](plan/architecture/als-csharp-animation-plan.md)：
  已由 GASP 计划取代，仅保留历史决策，不作为后续实施依据。

## 维护

新增、移动或删除模块与专题时同步更新本表；源码覆盖范围变化时更新"未覆盖路径"。
根 `AGENTS.md` 只保留到本文件的入口，不复制映射。
