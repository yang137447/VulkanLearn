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
| `source/resource/image/hostImage.*`、`source/resource/image/textureIO.*`、`source/resource/image/ddsTexture.cpp`、`source/resource/image/ktx2Texture.cpp`、`source/resource/image/hostImageVulkanPlan.h`、`source/resource/device/deviceTextureFactory.*`、`tool/texture-asset-tests/**` | `contracts/texture-asset-json-v1.md`、`contracts/testing.md` | — |
| `source/terrain/**`、`source/physics/joltRegistration.*`、`source/render/resource/rendererTerrainLoader.*`、`source/render/resource/terrainFeedbackGpu.*`、`shader/glsl/M_terrain*.json`、`shader/glsl/M_terrain.*.glsl`、`shader/glsl/materialFunction/mf_terrainLayer*.glsl`、`shader/glsl/pass/M_terrainFeedback.json`、`shader/glsl/pass/terrainFeedback.*`、`tool/terrain-tests/**` | `contracts/terrain.md`、`contracts/testing.md`、`contracts/texture-asset-json-v1.md`（VT atlas/压缩格式） | — |
| `source/script/scriptPhysics.*`、`source/script/scriptRuntime.cpp`、`source/world/loading/terrainCollisionInstances.*`、`source/world/world.h` | `contracts/terrain.md`、`contracts/csharp-blueprint-runtime.md`、`contracts/testing.md` | — |
| `source/scene/validation/**` | `contracts/asset-modify-api.md`、`contracts/terrain.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/render/offline/**` | `contracts/vl-offline-rendering.md` | — |
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
- 地形：运行时已支持 `terrain` 场景对象、TR 高度图网格与 chunk mesh 渲染；
  M_terrain 已支持最多 256 层的层表、tile 候选层、World 级 32×32 global page table + ID/权重
  atlas 与 top-4 数组混合；page manager 已有 CPU resident/eviction、虚拟页到物理
  atlas 槽位的分配与批量 region 上传；注册身份是场景 `placementId`，同一 TR 可多处摆放，
  场景级 `pageOrigin`/`pageTilesPerSide` 可覆盖虚拟区域，fallback 只占物理槽位且缺页
  输出中性表面。P3 后 renderer 使用 `preload=false`：feedback miss 驱动
  `RequestPageAtEpoch`，每帧限流请求/上传，物理槽位淘汰后进入 CPU quarantine 并到期回收；
  `terrain stats`/`terrain capacity N` 提供驻留与预算观测。P3.1 起只有同时具备
  `TR.layers` 与 `layerTable` 材质的地形进入 feedback，epoch-aware LRU 使用
  `lastTouchedEpoch`，降预算遵守 `max(2, framesInFlight+1)` 安全窗。独立的 128×128
  `terrainFeedback` pass（D32 depth）与 host-visible 回读 ring 仍在。
  L1 已把层 catalog 扩成 256×4（row2 页数/页表基址、row3 平均色）并新增纯 CPU 的
  `source/terrain/terrainLayerPageManager.*`：层页 `(layerId, mip, pageX, pageY)` 的
  物理页分配/淘汰/quarantine/上传批次。L2 已把 catalog、每 mip 的 RGBA32F 页表与
  三张 4096² atlas（4096²/slice、无 mip）建进 GPU，加载期用 GPU→GPU `vkCmdCopyImage`
  上传每层尾页，并输出淘汰失效项；`terrainlayerstats` 提供层页/atlas 观测。
  L3 已给 `M_terrain` 加 `u_terrainLayerMode`（默认 0）：mode 0 保持旧三数组
  快速路径；mode 1 走 top-4 → catalog → 每 mip 页表 → atlas 的采样，含 mip
  选择、页内半 texel inset、catalog avgColor 缺页 fallback 与 sourceResolution
  一致性校验，并在选中 mip 无效时回退到更粗的 mip。L4a 由
  `source/terrain/terrainLayerVisibility.*` 把覆盖页 feedback 推导成
  “可见层 → mip → 该 mip 全部页”，逐帧限量请求/上传/淘汰；mode 1 不再加载期
  全量预取（`terrainlayer preload` 只作对照）。L4c 在同一 resolver 里对目标 mip
  缺页的页追加 +1/+2 级粗 mip 预取（优先级递减、受层最大 mip 与每帧预算约束），
  `terrainlayer stats` 输出 `fallbackSlots/fallbackPages/fallbackLevels/targetMips`
  等回退分布；shader 侧 `avgColor` 淡入仍未实现。默认视觉路径仍与 L1/L2 逐字节一致。
  `source/terrain/terrainCollision.*` 提供 Jolt 静态 heightfield 碰撞的 RAII body：cm→m、
  placement 平移/旋转/缩放、16 位高度压缩，并复用调用方已有的 PhysicsSystem；
  `source/physics/joltRegistration.*` 是地形与 `ScriptPhysicsWorld` 共用的进程级
  Jolt 注册。World 通过 `world/loading/terrainCollisionInstances.*` 保留每个
  terrain placement 的计划与 TRS，Play 时 `ScriptPhysicsWorld::SetTerrainCollisions`
  创建/替换 body，Stop/World 换代销毁；编辑器与普通加载不创建物理世界。
  `TerrainCollisionWorld` 只是测试/离线夹具。CPU 侧已有 LOD/skirt 生成；`lodLevels>1` 时
  `RendererTerrainLoader` 生成全部级，`WorldSnapshotBuilder` 每帧按相机到
  chunk 世界 AABB 的距离与 10% 滞回选择一级，阴影/拾取/材质上下文共享同一快照，
  `terrainlod stats` 可观测。`lodLevels=1` 旧资产仍逐字节等同 LOD0。
  S1 新增 World 级 `source/terrain/terrainLayerRegistry.*`：层身份是
  `layer.key`（缺省 `<assetPath>#<name>`），同 key 参数一致跨 TR 去重成同一个
  globalId，参数冲突/同 TR 重复/超过 256 层都在加载期失败；单 TR 的
  globalId == 局部下标，旧输出逐字节不变。ID/权重页写 globalId，
  `TerrainPageManager::RegisterTerrain` 保存 placement 的 local→global 映射。
  S2 把层源数组改成 `source/terrain/terrainLayerSources.h` 的
  `map<sourceSet, TerrainSourceSetRecord>`：同 set 共享同一组 shared_ptr，
  不同 set 可绑定不同数组，层页上传按 `layerSlices` 还原源 slice，三张 atlas
  仍只有一组（格式必须一致）。`terrainlayerstats` 输出
  `globalLayers/sourceSets/dedupedLayers/capacity`。
  当前没有低分辨率 fallback/淡入、相机/视锥预取、地形编辑，也没有
  分块或编辑后的碰撞重建。行为见
  [terrain 契约](contracts/terrain.md)；后续计划见
  `plan/rendering/terrain-openworld-demo-plan.md`（非契约）。

## 组合规则

- 精确规则与"改动类型"规则**叠加**，不采用首条命中。
- 命中 `contracts/` 与 `rationale/` 同时覆盖某主题时，以 `contracts/` 为准，并把 `rationale/` 中与之冲突的
  描述当作待修正项报告。

## 规划入口（非契约）

- [地形系统：Open World Demo Collection 复刻计划](plan/rendering/terrain-openworld-demo-plan.md)：
  当前地形目标。CPU 高度图网格 + 最多 32 层 + tile 候选层 + ID/权重图 top-4；
  GPU-driven/VT 与几何 streaming 仍搁置；CPU 几何 LOD 已接运行时选择
  （大世界层表另见下一项）。
  M2 的基础 TR/场景/网格/World 渲染已落地并写入 `contracts/terrain.md`；M3+ 的层混合与
  画面复刻仍未实现。计划不替代当前契约。
- [地形大世界与上百层地表架构](plan/rendering/terrain-large-world-layers-plan.md)：
  从上百层/大世界目标倒推的层表、tile 候选层、ID/权重图、VT 与流式阶段。未实现，
  不替代 `contracts/terrain.md`；先用于确认层数、世界尺寸和显存预算。
- [地形 VT 流式闭环设计](plan/rendering/terrain-vt-streaming-design.md)：
  GPU feedback → CPU 驻留/LRU → 子区域上传的下一阶段闭环；含 feedback 载体对比、
  texel/ring 格式、frame fence/epoch 保护和分步验收。P0/P1/P2/P4 的 World-local state、
  batch 上传、feedback pass/回读统计、placementId/fallback 解耦已落地；P3 的
  `ApplyFeedback`、feedback 驱动请求/淘汰、每帧上传 ring 与 quarantine 也已落地。
  仍不替代 `contracts/terrain.md`。
- [地形层纹理流式设计](plan/rendering/terrain-layer-texture-streaming-design.md)：
  256 层 albedo/normal/pbr 的显存预算、layer catalog、第二级 VT/数组页方案对比与
  L0–L4 分步实施。L0（BC7/BC5）、L1（catalog v2 + CPU 层页管理器）与 L2
  （GPU 层 atlas/页表/加载期上传）与 L3（`u_terrainLayerMode=1` 的 shader VT
  采样、材质 schema、sourceResolution 校验）已落地；L4a/b/c 的覆盖页+精确层
  feedback、逐帧上传/淘汰、粗 mip 预取与 feedback 饥饿保护也已落地。多 biome
  `sourceSet`/全局 layer catalog 仍未实现，不替代 `contracts/terrain.md`。
- [全局 layer catalog / 多 biome sourceSet 设计](plan/rendering/terrain-global-layer-catalog-design.md)：
  把 `layerId = TR.layers` 下标、World 单组源纹理的限制扩展到全局稳定 ID、
  source set table、统一 atlas 与分步迁移。未实现，不替代 `contracts/terrain.md`。
- [开放世界场景栈路线图](plan/rendering/open-world-scene-stack-roadmap.md)：
  地形碰撞/GASP、植被撒点与石头对齐、PP2 分层风、3D 风力点、LOD、自定义 shading 的依赖与阶段。
  未实现，不替代当前契约。
- [艺术地球仪 Demo 计划](plan/rendering/artistic-globe-demo-plan.md)：
  经典支架与浮雕地球、冰雪、云和水汽的艺术展示方向。以整件摆件的观赏效果为目标，
  不做地表近景；包含功能缺口、阶段验收与生图提示词，尚未实施。
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
