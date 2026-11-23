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
| `CMakeLists.txt`、`source/CMakeLists.txt`、`source/main_editor.cpp`、`source/main_game.cpp`、`source/engine/application.*` | `contracts/vulkanlearn-architecture.html`、`contracts/testing.md` | Runtime editor target ownership、Game executable link boundary |
| `source/engine/editorHost.h`、`source/engine/nullEditorHost.*`、`source/editor/editorHost.*` | `contracts/vulkanlearn-architecture.html` | Runtime editor target ownership、Game executable link boundary、Runtime UI editor include boundary |
| `source/ui/iEditorUiShell.h`、`source/editor/ui/editorUiShell.*`、`source/ui/uiSubsystem*` | `contracts/game-ui-stack.md` | Runtime UI editor include boundary |
| `source/ui/widgets/**` | `contracts/ui-component-system.md` | — |
| `source/ui/**` | `contracts/ui-component-system.md`、`contracts/game-ui-stack.md` | — |
| `source/editor/**` | `contracts/ui-component-system.md`、`contracts/editor-runtime-data-boundary.md` | — |
| `script/VulkanLearn.Runtime/AnimationGraph*.cs`、`script/VulkanLearn.Runtime/LocomotionAnimationBlueprint.cs`、`script/VulkanLearn.Runtime/LocomotionAnimationGraph.cs`、`tool/script-tests/scriptLocomotionTests.cpp`、`tool/script-tests/managed/AnimationGraphFixtures.cs` | `contracts/animation-runtime-skinning.md`、`contracts/csharp-blueprint-runtime.md`、`contracts/testing.md` | — |
| `source/editor/ui/panel/**` | `contracts/ui-component-system.md`、`contracts/game-ui-stack.md` | Runtime editor target ownership |
| `source/editor/service/material/**` | `contracts/asset-modify-api.md`、`contracts/material-instance-editor.md`、`contracts/material-param-authoring-and-reflection.md` | Runtime editor target ownership |
| `source/script/**`、`script/**`、`tool/script-tests/**`、`source/editor/scene/scriptBlueprintAuthoring.*` | `contracts/csharp-blueprint-runtime.md`、`contracts/testing.md` | — |
| `source/editor/service/**`、`source/editor/command/**` | `contracts/asset-modify-api.md`、`contracts/material-instance-editor.md`、`contracts/material-param-authoring-and-reflection.md` | — |
| `tool/asset-edit/**` | `contracts/asset-modify-api.md`、`contracts/testing.md` | — |
| `tool/game-export/**`、`tool/game-export-tests/**` | `contracts/csharp-blueprint-runtime.md`、`contracts/asset-organization.md`、`contracts/testing.md` | Game packaging manifest closure、Game shader cache closure |
| `cmake/VulkanLearnDeps.cmake`、`ThirdParty/**`、`extern/CMakeLists.txt`、`tool/build-deps.ps1`、`tool/deps-stamp.ps1`、`tool/deps-gc.ps1`、`tool/clean-project.ps1` | `contracts/testing.md` | — |
| `tool/validate-game-package.ps1`、`ui/runtime-ui.manifest.json` | `contracts/game-ui-stack.md`、`contracts/testing.md` | Game packaging manifest closure、Game shader cache closure |
| `tool/animation/**`、`tool/animation-tests/**` | `contracts/animation-preflight-tools.md`、`contracts/testing.md` | — |
| `source/animation/**`、`source/mesh/loader/animation/**`、`tool/animation/gaspImportReport.*`、`tool/animation/compare_gasp_import.py`、`tool/animation-tests/skeletalImportTests.cpp` | `contracts/animation-asset-import.md`、`contracts/testing.md` | — |
| `source/animation/animationMontage.*`、`source/animation/animationSkeleton.*`、`source/animation/animationSequenceData.*`、`source/mesh/loader/common/meshAssetLoader.*`、`source/mesh/validation/meshAssetValidator.*`、`source/mesh/meshAssetTypes.h`、`tool/animation-tests/animationMontageTests.cpp` | `contracts/animation-montage.md`、`contracts/animation-asset-import.md`、`contracts/testing.md` | — |
| `source/animation/animationScriptData.*`、`source/world/worldAnimation.cpp`、`source/script/scriptRuntime.cpp`、`tool/animation-tests/animationScriptDataTests.cpp`、`tool/animation-tests/notifyStateTests.cpp` | `contracts/animation-montage.md`、`contracts/animation-runtime-skinning.md`、`contracts/testing.md` | — |
| `source/animation/skeletalPlayback*`、`source/animation/animationSyncGroup.*`、`source/animation/footPlacement.*`、`source/animation/slopeWarping.*`、`source/animation/skinPose.h`、`source/world/worldAnimation.cpp`、`shader/glsl/engine/skinnedVertexFactory.glsl`、`tool/animation/build_gasp*.py`、`tool/animation-tests/animationSyncGroupTests.cpp`、`tool/animation-tests/slopeWarpingTests.cpp` | `contracts/animation-runtime-skinning.md`、`contracts/animation-motion-warping.md`、`contracts/animation-montage.md` | — |
| `source/animation/poseDriver.*`、`source/world/worldAnimation.cpp`、`tool/animation/extract_pose_driver.py`、`tool/animation-tests/poseDriverTests.cpp` | `contracts/animation-pose-driver.md`、`contracts/animation-runtime-skinning.md`、`contracts/testing.md` | — |
| `source/animation/postProcess.*`、`source/world/worldAnimation.cpp`、`tool/animation/extract_post_process.py`、`tool/animation-tests/postProcessTests.cpp` | `contracts/animation-post-process.md`、`contracts/animation-runtime-skinning.md`、`contracts/testing.md` | — |
| `source/physics/bonePhysics.*`、`source/world/worldAnimation.cpp`、`tool/animation/extract_bone_physics.py`、`tool/physics-runtime-tests/bonePhysicsTests.cpp` | `contracts/animation-bone-physics.md`、`contracts/animation-post-process.md`、`contracts/testing.md` | — |
| `source/animation/motionWarping.*`、`source/script/scriptPhysics.cpp`、`script/VulkanLearn.Runtime/AnimationPlayback.cs`、`tool/animation-tests/motionWarpingTests.cpp` | `contracts/animation-motion-warping.md`、`contracts/animation-montage.md`、`contracts/testing.md` | — |
| `source/animation/motionDatabase.*`、`source/animation/motionDebug.h`、`source/animation/motionFeatures.*`、`source/animation/motionHistory.*`、`source/animation/motionMatching.*`、`source/animation/motionRuntime.*`、`source/world/worldAnimation.cpp`、`tool/animation/motionDatabaseBuild.*`、`tool/animation/browse_motion_database.py`、`tool/animation/verify_gasp_motion_database.py`、`tool/animation/capture_gasp_p4.py`、`tool/animation/capture_gasp_p5.py`、`tool/animation-tests/motionDatabaseTests.cpp`、`tool/animation-tests/motionHistoryTests.cpp`、`tool/animation-tests/motionMatchingTests.cpp` | `contracts/animation-motion-features.md`、`contracts/testing.md` | — |
| `source/animation/motionRuntimeDatabase.*` | `contracts/animation-asset-import.md`、`contracts/animation-motion-features.md`、`contracts/testing.md` | — |
| `source/debug/**`、`source/ui/debugDrawOverlay.*`、`source/script/scriptDebugDraw.h`、`script/VulkanLearn.Runtime/DebugDraw.cs`、`tool/debug-draw-tests/**`、`tool/editor-ui-tests/debugDrawOverlayTests.cpp`、`tool/script-tests/scriptDebugDrawTests.cpp`、`tool/script-tests/managed/DebugDrawFixtures.cs`、`tool/debug/capture_draw_debug.py` | `contracts/debug-draw.md`、`contracts/testing.md` | — |
| `source/resource/image/textureAssetValidation.h`、`source/textureAssetLoader.cpp` | `contracts/texture-asset-json-v1.md`、`contracts/asset-modify-api.md` | — |
| `source/resource/image/hostImage.*`、`source/resource/image/textureIO.*`、`source/resource/image/ddsTexture.cpp`、`source/resource/image/ktx2Texture.cpp`、`source/resource/image/hostImageVulkanPlan.h`、`source/resource/device/deviceTextureFactory.*`、`tool/texture-asset-tests/**` | `contracts/texture-asset-json-v1.md`、`contracts/testing.md` | — |
| `source/terrain/**`、`source/physics/joltRegistration.*`、`source/render/resource/rendererTerrainLoader.*`、`source/render/resource/terrainFeedbackGpu.*`、`shader/glsl/M_terrain*.json`、`shader/glsl/M_terrain.*.glsl`、`shader/glsl/materialFunction/mf_terrainLayer*.glsl`、`shader/glsl/pass/M_terrainFeedback.json`、`shader/glsl/pass/terrainFeedback.*`、`tool/terrain-tests/**` | `contracts/terrain.md`、`contracts/testing.md`、`contracts/texture-asset-json-v1.md`（VT atlas/压缩格式） | — |
| `source/terrain/terrainCompositedVT.*`、`source/terrain/terrainCompositedVTStore.*`、`source/terrain/terrainCompositedDependencies.*`、`source/terrain/terrainCompositedResidency.*`、`source/terrain/terrainCompositedFeedback.*` | `contracts/terrain-composited-vt.md`、`contracts/terrain.md` | — |
| `source/terrain/terrainCompositedProduction.*`、`source/terrain/terrainCompositedAdjacency.*`、`source/terrain/terrainCompositedIdentity.h`、`source/terrain/terrainCompositedAuthorRecipe.*`、`source/terrain/terrainCompositedAuthorDependency.h` | `contracts/terrain-composited-vt.md`、`contracts/terrain.md`、`contracts/texture-asset-json-v1.md` | — |
| `source/terrain/terrainRvtBudget.*`、`source/terrain/terrainRvtScheduler.*` | `contracts/terrain-composited-vt.md`、`contracts/testing.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/terrain/terrainCompositedDirty.*`、`source/terrain/terrainCompositedRefresh.h`、`source/terrain/terrainCompositedWeightOverlay.*`、`source/editor/terrain/terrainCompositedAuthorMonitor.*` | `contracts/terrain-composited-vt.md`、`contracts/terrain.md`、`contracts/editor-runtime-data-boundary.md` | — |
| `source/render/terrain/terrainCompositedRuntime.*`、`source/render/resource/terrainCompositedService.*`、`source/render/resource/terrainCompositedBakeGpu.*` | `contracts/terrain-composited-vt.md`、`contracts/descriptor-imageinfo-management.md`、`contracts/vulkanlearn-architecture.html` | Terrain composited-only cutover |
| `source/render/resource/terrainCompositedProductionGpu.*`、`source/render/resource/terrainCompositedFeedbackGpu.*`、`source/render/resource/terrainCompositedBakeAdjacency.h` | `contracts/terrain-composited-vt.md`、`contracts/descriptor-imageinfo-management.md`、`contracts/texture-asset-json-v1.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/render/resource/terrainCompositedQualityGpu.*`、`source/render/resource/terrainCompositedQualityReplay.*`、`source/render/resource/terrainCompositedQualityData.h`、`source/render/frame/terrainCompositedQualityFrame.*` | `contracts/terrain-composited-vt.md`、`contracts/testing.md`、`contracts/descriptor-imageinfo-management.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/render/frame/terrainCompositedLoading.*`、`source/render/frame/terrainCompositedReadiness.*`、`source/render/frame/terrainCompositedQualityValidation.*`、`source/render/frame/terrainCompositedFrameLifetime.h`、`source/terrain/terrainCompositedQualityClock.h` | `contracts/terrain-composited-vt.md`、`contracts/testing.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/terrain/terrainCompositedSourceSnapshots.*`、`source/terrain/terrainCompositedShaderPath.h`、`source/render/resource/terrainCompositedResourceSnapshot.*` | `contracts/terrain-composited-vt.md`、`contracts/descriptor-imageinfo-management.md`、`contracts/shader-build-cache.md` | — |
| `source/engine/terrainChunkStreamerLifecycle.cpp`、`tool/terrain-tests/terrainChunkStreamerLifecycleTests.cpp` | `contracts/terrain.md`、`contracts/terrain-composited-vt.md`、`contracts/testing.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/render/resource/terrainHeightAtlas.*`、`source/terrain/terrainHeightAtlasLayout.h`、`source/terrain/terrainHeightAtlasStore.*` | `contracts/terrain-composited-vt.md`、`contracts/terrain.md`、`contracts/texture-asset-json-v1.md` | — |
| `source/terrain/terrainVertexUpdateQueue.*`、`source/editor/terrain/terrainCompositedSculptApply.*`、`source/editor/terrain/terrainCompositedSculptCommit.cpp` | `contracts/terrain-composited-vt.md`、`contracts/terrain.md`、`contracts/editor-runtime-data-boundary.md` | — |
| `shader/glsl/materialFunction/terrainCompositedEvaluation.glsl`、`shader/glsl/materialFunction/terrainCompositedFeedback.glsl`、`shader/glsl/materialFunction/mf_terrainCompositedVT.glsl`、`shader/glsl/materialFunction/terrainHeightAtlas.glsl`、`shader/glsl/materialFunction/mf_terrainLayerBlend.glsl`、`shader/glsl/M_terrain.json`、`shader/glsl/pass/terrainFeedback.frag` | `contracts/terrain-composited-vt.md`、`contracts/terrain.md`、`contracts/material-param-authoring-and-reflection.md` | Terrain composited-only cutover |
| `shader/glsl/materialFunction/terrainCompositedFootprint.glsl` | `contracts/terrain-composited-vt.md`、`contracts/material-param-authoring-and-reflection.md` | — |
| `shader/glsl/terrain/terrainCompositedBakeWorld.comp`、`shader/glsl/terrain/terrainCompositedBakeWorldPadding.comp`、`shader/glsl/terrain/terrainCompositedBakeWorldTypes.glsl`、`shader/glsl/terrain/terrainCompositedBakeWorldEvaluator.glsl`、`shader/glsl/terrain/terrainCompositedBakeCoverage.glsl`、`shader/glsl/terrain/terrainCompositedBakeRoute.glsl` | `contracts/terrain-composited-vt.md`、`contracts/terrain.md`、`contracts/material-param-authoring-and-reflection.md`、`contracts/shader-build-cache.md` | — |
| `shader/glsl/terrain/terrainCompositedBakeSamplingDiagnostic.comp`、`shader/glsl/terrain/terrainCompositedSourceFilterCandidate.glsl` | `contracts/terrain-composited-vt.md`、`contracts/testing.md`、`contracts/material-param-authoring-and-reflection.md`、`rationale/terrain-rvt-source-filter-proposal.md` | — |
| `shader/glsl/terrain/terrainCompositedQuality.comp`、`shader/glsl/terrain/terrainCompositedQualityVisible.glsl` | `contracts/terrain-composited-vt.md`、`contracts/testing.md`、`contracts/material-param-authoring-and-reflection.md` | — |
| `shader/glsl/materialFunction/terrainCoverage.glsl`、`shader/glsl/engine/passTemplate/base.tesc.glsl`、`shader/glsl/engine/passTemplate/base.tese.glsl` | `contracts/terrain.md`、`contracts/terrain-composited-vt.md`、`contracts/material-mesh-pass-composition.md` | — |
| `source/engine/launchOptions.*`、`source/engine/application.cpp`、`source/render/resource/rendererTerrainLoader.cpp`、`source/editor/editorHost.cpp`、`tool/game-export/terrainPackageClosure.h` | `contracts/terrain-composited-vt.md`、`contracts/terrain.md` | Terrain composited-only cutover |
| `source/debugConsoleProcessCommand.cpp`、`source/engine/runtimeCommand.h`、`source/engine/runtimeCommandExecutorExecuteCommand.cpp`、`shader/glsl/pass/M_terrainFeedback.json` | `contracts/terrain-composited-vt.md`、`contracts/debug-runtime-commands.md` | Terrain composited-only cutover |
| `tool/terrain-tests/terrainComposited*Tests.cpp`、`tool/terrain-tests/terrainHeightAtlasTests.cpp`、`tool/terrain-tests/terrainHeightAtlasStoreTests.cpp` | `contracts/terrain-composited-vt.md`、`contracts/testing.md` | — |
| `tool/terrain-tests/terrainRvt*Tests.cpp`、`tool/terrain-tests/terrainVertexUpdateQueueTests.cpp`、`tool/editor-ui-tests/terrainCompositedSculptApplyTests.cpp`、`tool/editor-ui-tests/terrain-composited-sculpt-private/**` | `contracts/terrain-composited-vt.md`、`contracts/terrain.md`、`contracts/testing.md` | — |
| `config/terrainRvtP0Frozen.json`、`config/terrainRvtP0Frozen.json.sha256`、`config/terrainRvtP0Frozen.json.review.json`、`tool/validation/terrain/**` | `contracts/terrain-composited-vt.md`、`contracts/testing.md` | — |
| `source/script/scriptPhysics.*`、`source/script/scriptRuntime.cpp`、`source/world/loading/terrainCollisionInstances.*`、`source/world/world.h` | `contracts/terrain.md`、`contracts/csharp-blueprint-runtime.md`、`contracts/testing.md` | — |
| `source/scene/validation/**` | `contracts/asset-modify-api.md`、`contracts/terrain.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/render/offline/**` | `contracts/vl-offline-rendering.md` | — |
| `source/hair/**`、`tool/hair/**`、`tool/hair-tests/**` | `contracts/tressfx-asset-import.md`、`contracts/gpu-pass-timing.md`、`contracts/scene-wind.md`、`contracts/testing.md` | — |
| `source/mesh/loader/hair/**`、`shader/glsl/engine/tressfxVertexFactory.glsl`、`shader/glsl/engine/tressfxCoverage.glsl`、`shader/glsl/hair/**`、`tool/material-schema-tests/tressfxMaterialTests.cpp` | `contracts/tressfx-asset-import.md`、`contracts/material-mesh-pass-composition.md`、`contracts/testing.md` | — |
| `source/physics/**` | `contracts/fracture-core.md` | — |
| `source/physics/fracture/fractureBudget.*` | `contracts/fracture-budget.md` | — |
| `source/physics/fracture/destructibleProfile.*`、`plateFracturePolicy.*` | `contracts/fracture-core.md`、`contracts/fracture-budget.md`、`contracts/glass-fracture-runtime.md` | — |
| `tool/fracture-core-tests/**` | `contracts/fracture-core.md`、`contracts/fracture-budget.md`、`contracts/testing.md` | — |
| `source/engine/softBodyRuntime.*` | `contracts/softbody-runtime.md`、`contracts/fracture-budget.md` | — |
| `source/engine/glassRuntime.*` | `contracts/glass-fracture-runtime.md`、`contracts/fracture-budget.md` | — |
| `source/engine/projectileRuntime.*` | `contracts/projectile-shooting-runtime.md` | — |
| `source/engine/destructibleRuntime.*` | `contracts/projectile-shooting-runtime.md`、`contracts/glass-fracture-runtime.md`、`contracts/softbody-runtime.md` | — |
| `source/physics/runtime/**` | `contracts/softbody-runtime.md`、`contracts/glass-fracture-runtime.md` | — |
| `source/audio/**` | `contracts/audio-runtime.md`、`contracts/softbody-runtime.md`、`contracts/glass-fracture-runtime.md` | — |
| `tool/physics-runtime-tests/**` | `contracts/softbody-runtime.md`、`contracts/glass-fracture-runtime.md`、`contracts/testing.md` | — |
| `source/ui/sceneObjectTypeSupport.h` | `contracts/softbody-runtime.md`、`contracts/glass-fracture-runtime.md`、`contracts/game-ui-stack.md` | — |
| `source/world/**` | `contracts/vulkanlearn-architecture.html` | World mesh object cutover、Mesh object build plan contract、World build plan pure data contract、World gameplay plan builder contract、WorldBuilder assembly assets contract、WorldBuilder mesh object plan consumption、World loader runtime error code contract、World reload failure state machine validation |
| `source/render/backend/**` | `contracts/vulkanlearn-architecture.html`、`contracts/gpu-pass-timing.md` | Redundant Vulkan device abstraction cutover、Vulkan device boundary concrete-only contract、RendererBackend device boundary escape hatch、RendererBackend hides concrete device boundary、RendererBackend device boundary forward declaration、Backend texture handle bridge contract、Swapchain resize production lifecycle |
| `source/renderableObject.*` | `contracts/vulkanlearn-architecture.html`、`contracts/vegetation-instancing.md` | — |
| `source/VertexDataStruct.h`、`source/mesh/vegetationAsset.*`、`source/render/resource/rendererMeshLoader.*`、`schema/scene.schema.json` | `contracts/vegetation-instancing.md` | — |
| `source/render/resource/**` | `contracts/descriptor-imageinfo-management.md`、`contracts/vulkanlearn-architecture.html` | Buffer resource package lifecycle handle adoption、Texture resource package lifecycle handle adoption、Renderer resource load result contract、Renderer gameplay plan boundary、Renderer world loader include boundary、Renderer resource load runtime error code contract |
| `source/renderGraph.h`、`source/renderGraph*.cpp` | `contracts/vulkanlearn-architecture.html` | Render graph reload production lifecycle、Render graph resource retire ownership、World graph retire queue validation、RenderGraph resource package lifecycle handle adoption |
| `source/passBarrier.*`、`shader/glsl/uiOverlay.*` | `contracts/debug-draw.md`、`contracts/descriptor-imageinfo-management.md`、`contracts/shader-hot-reload.md` | — |
| `source/renderSystem.*` | `contracts/vulkanlearn-architecture.html`、`contracts/gpu-pass-timing.md`、`contracts/scene-wind.md` | RenderSystem frame boundary、Draw frame boundary、Resolved draw resource ownership |
| `source/VulkanManager.*`、`source/vulkanDebug.*` | `contracts/gpu-pass-timing.md` | — |
| `source/render/frame/**` | `contracts/vulkanlearn-architecture.html`、`contracts/gpu-pass-timing.md` | — |
| `source/render/shadow/**` | `contracts/vulkanlearn-architecture.html`、`contracts/csm-shadow-map-m1.md` | — |
| `source/render/postprocess/**` | `contracts/vulkanlearn-architecture.html` | — |
| `source/render/environment/**` | `contracts/vulkanlearn-architecture.html`、`contracts/environment-update-scheduler.md` | — |
| `source/render/terrain/**`、`source/render/resource/terrain/**` | `contracts/terrain.md`、`contracts/vulkanlearn-architecture.html`、`contracts/testing.md` | — |
| `source/render/hair/**` | `contracts/tressfx-asset-import.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/render/picking/**` | `contracts/vulkanlearn-architecture.html`、`contracts/material-instance-editor.md` | — |
| `source/render/preview/**` | `contracts/material-instance-editor.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/render/world/**` | `contracts/vulkanlearn-architecture.html`、`contracts/csharp-blueprint-runtime.md` | — |
| `source/render/renderable/**` | `contracts/vulkanlearn-architecture.html`、`contracts/vegetation-instancing.md` | — |
| `source/render/pass/**` | `contracts/vulkanlearn-architecture.html` | Render pass gameplay boundary |
| `source/particles/**`、`shader/glsl/particle/**` | `contracts/particle-system.md`、`contracts/editor-runtime-data-boundary.md` | Particle runtime excludes editor |
| `source/render/decal/**`、`shader/glsl/decal/**` | `contracts/decal-runtime.md` | — |
| `source/shaderCompiler*`、`source/shader/build/**` | `contracts/shader-build-cache.md` | — |
| `source/shader/reload/**` | `contracts/shader-hot-reload.md` | — |
| `source/material/**` | `contracts/material-param-authoring-and-reflection.md` | Descriptor writer explicit context boundary、Vulkan-native descriptor write type、Vulkan-native descriptor write contract |
| `shader/glsl/**` | `contracts/material-param-authoring-and-reflection.md`、`contracts/gbuffer-channel-ownership.md` | — |
| `shader/glsl/M_glassRefraction.*`、`shader/glsl/M_glassRefraction.surface.glsl`、`config/renderGraphConfig.json` 的 `sceneColorCopy` / `forwardTransparent`、`tool/renderer-backend-tests/glass_refraction_graph_test.cpp` | `contracts/glass-refraction.md`、`contracts/vulkanlearn-architecture.html` | — |
| 论文 case 验证：`tool/validation/**` | `rationale/shading-model-case-records.md`（证据归档）、`plan/rendering/shading-model-alignment-plan.md` | — |
| 粒子特效验收：`tool/validation/particle/**` | `plan/rendering/niagara-effects-alignment-plan.md` | — |
| `ui/**/*.rcss`、任何写 UI 样式处 | `rationale/pitfalls.md` 的 `[rcss-alpha]`、`[rmlui-shrink-to-fit-max-width]` 条 | — |
| 改右键上下文菜单（`ui/editor-workspace.rml` 的 `#editor-context-menu`、面板的 `mouseup/button==1` 处理、菜单目标解析） | `contracts/ui-component-system.md` 的「右键上下文菜单」节 | — |
| `source/render/editor/**`、`shader/glsl/pass/editorGrid.*`、`config/renderGraphConfig.json` 的 `editorGrid` pass | `contracts/material-mesh-pass-composition.md` 的「编辑器网格 pass」节 | — |
| `source/editor/ui/editorViewportAxisIndicator.h`、`#viewport-axis-indicator` 叠层 | `contracts/ui-component-system.md` 的「角落坐标轴指示器」节 | — |
| `source/pipeline/**` | `contracts/vulkanlearn-architecture.html` | Pipeline Vulkan creation diagnostics、Pipeline Vulkan creation diagnostic context |
| `source/commonFunction.h` | `contracts/coding-guidelines.md` | CommonFunction depth helper cleanup、Source comment clarity |
| `source/engine/**` | `contracts/vulkanlearn-architecture.html` | Runtime test hook command boundary、Launch runtime tests use CommandBus、Runtime test state stays out of EngineLoop、LoadWorld path resolve runtime error contract、World transaction engine ownership contract、Prepared world transition contract、World transaction ownership cutover、Bounded async render loop contract、Bounded async render loop no immediate idle wait |
| `source/engine/frame/**` | `contracts/vulkanlearn-architecture.html`、`contracts/gpu-pass-timing.md` | Bounded async render loop contract、Bounded async render loop submit contract、Bounded async render loop no immediate idle wait |
| `source/engine/window/**` | `contracts/vulkanlearn-architecture.html` | Swapchain resize production lifecycle resources、Render graph reload production lifecycle |
| `source/engine/ui/**`、`source/engine/script/**`、`source/engine/perf/**` | `contracts/vulkanlearn-architecture.html`、`contracts/game-ui-stack.md`、`contracts/testing.md` | — |
| `schema/*.schema.json` | `contracts/texture-asset-json-v1.md` | — |
| `tool/architecture-boundary-audit.ps1`、`tool/architecture-final-validation.ps1` | `contracts/vulkanlearn-architecture.html` | Architecture final validation script contract |
| `documents/**` | `README.md`（分层规则） | Vulkan-native architecture wording |
| `source/particles/particleRenderer*`、`source/particles/particleAsset*` | `contracts/particle-system.md`、`contracts/editor-runtime-data-boundary.md` | Particle runtime excludes editor |
| `source/animation/skeletalPlayback*` | `contracts/animation-runtime-skinning.md`、`contracts/animation-motion-warping.md`、`contracts/animation-montage.md` | — |
| `source/renderGraph*`、`source/render/rendergraph/renderGraphCompiler*` | `contracts/vulkanlearn-architecture.html` | Render graph reload production lifecycle、Render graph resource retire ownership、World graph retire queue validation、RenderGraph resource package lifecycle handle adoption |
| `source/render/backend/rendererBackendVulkan*` | `contracts/vulkanlearn-architecture.html`、`contracts/gpu-pass-timing.md` | Redundant Vulkan device abstraction cutover、Vulkan device boundary concrete-only contract、RendererBackend device boundary escape hatch、RendererBackend hides concrete device boundary、RendererBackend device boundary forward declaration、Backend texture handle bridge contract、Swapchain resize production lifecycle |
| `source/ui/uiSubsystem*` | `contracts/game-ui-stack.md` | Runtime UI editor include boundary |
| `source/shaderCompiler*` | `contracts/shader-build-cache.md` | — |
| `source/engine/runtimeCommandExecutor*` | `contracts/debug-runtime-commands.md`、`contracts/vulkanlearn-architecture.html` | Runtime test hook command boundary、Launch runtime tests use CommandBus、Runtime test state stays out of EngineLoop |
| `source/debugConsole*` | `contracts/debug-runtime-commands.md` | — |
| `source/hair/hairSimulationPass*` | `contracts/tressfx-asset-import.md`、`contracts/gpu-pass-timing.md` | — |
| `source/editor/runtime/materialInstanceEditorRuntime*` | `contracts/material-instance-editor.md`、`contracts/ui-component-system.md` | Runtime editor target ownership |
| `source/editor/ui/panel/particlePanel*` | `contracts/ui-component-system.md`、`contracts/game-ui-stack.md` | Runtime editor target ownership |
| `source/editor/ui/materialInstanceAssetEditorPanel*` | `contracts/ui-component-system.md`、`contracts/material-instance-editor.md` | Runtime editor target ownership |
| `source/world/worldParticle*` | `contracts/particle-system.md`、`contracts/vulkanlearn-architecture.html` | — |
| `source/editor/command/editorCommandJsonCodec*` | `contracts/asset-modify-api.md`、`contracts/material-instance-editor.md` | — |

`extern/**` 是第三方依赖，除 `extern/BLAKE3` 的锁定说明外不在本表覆盖范围（见"未覆盖路径"）。

warp 消费点：`skeletalPlayback*` 在 GT tick 内调用原生窗口队列；C# 通过
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
| `contracts/vegetation-instancing.md` | 改 `VEG_*.json`、静态实例顶点 ABI、instanced draw 或实例包围盒时 |
| `contracts/testing.md` | 改测试入口、测试目标或新增测试时 |
| `contracts/boundary-rules.generated.md` | 想知道 90 条边界规则各是什么、作用于哪些路径时（**生成物，勿手工编辑**） |
| `contracts/README.md` | 想知道契约层的写作与校验约定时 |
| `contracts/diagrams/README.md` | 改架构图、或改图所描述的那条链路时 |

## 架构图

图源是**契约**：IR 里的组件、边界与有向连接是拓扑真相，逐节点 `sources` 钉在某个 40 位 revision
上（由 archify 的 `finalize` 校验文件存在与行号范围）。产物 HTML 由 IR 生成，**不要手工编辑产物**。

| 源码路径 | 必读图源 | 图说明什么 |
| --- | --- | --- |
| `source/engine/engineLoop.cpp`、`source/ui/uiSubsystem.cpp` | `contracts/diagrams/editor-frame-sequence.json` | 一帧内从平台事件到上屏的顺序：UI 输入独占、GT 稳定帧边界、快照发布、可见性收口 |
| `source/world/loading/world*.cpp`、`source/engine/worldGraphTransactionCoordinator.cpp` | `contracts/diagrams/world-graph-transaction-lifecycle.json` | World/RenderGraph 事务：隔离准备、提交前校验、无抛提交、GPU epoch 退休 |

`py -3 tool/validate-docs.py` 会检查每张图都能从本表到达，且它引用的源码证据仍然成立
（路径存在、行号在文件范围内）—— 图与代码漂移时门禁失败，而不是让人以为图还是对的。

## 门禁规则

`tool/architecture-boundary-audit.ps1` 定义代码**不得违反什么**：90 条规则检查（81 个命名规则），由
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

地形已有精确路由，不属于未覆盖模块：CPU 几何、Editor、Paint/coverage 与碰撞见
[terrain 契约](contracts/terrain.md)；唯一材质运行时标准 compositedVT + 常驻 height
见 [合成 VT 契约](contracts/terrain-composited-vt.md)。
被删除的层源页管理、层页可见性与旧双路径说明只作为
[历史记录](rationale/terrain-legacy-runtime-contract-snapshot.md)，不作为源码入口或验收证据。

## 组合规则

- 精确规则与"改动类型"规则**叠加**，不采用首条命中。
- 命中 `contracts/` 与 `rationale/` 同时覆盖某主题时，以 `contracts/` 为准，并把 `rationale/` 中与之冲突的
  描述当作待修正项报告。

## 规划入口（非契约）

- [地形 RVT 按需生产方案](plan/rendering/terrain-runtime-virtual-texture-production-plan.md)：
  P0–P6 退出要求和设计背景，不是本轮完成证明。阶段证据见
  [执行台账](rationale/terrain-rvt-production-execution.md)，当前行为以 V2/gen3 契约和源码为准。
- [地形合成 VT 资源生命周期计划](plan/rendering/terrain-composited-vt-lifecycle-plan.md)：
  待审阅设计，非当前能力。梳理 Editor 编辑快照、共享构建/Cook、合成材质虚拟页、
  Runtime 驻留、版本与增量刷新，以及从层源 VT 迁移的阶段验收。
- [实时破碎物理与声音研究计划](plan/physics/realtime-fracture-and-audio-plan.md)：
  玻璃冲击/二次碎裂、气球破裂和事件驱动声音的混合路线、数据模型、里程碑与风险；
  当前已落地运行时平面/球面破碎、橡皮膜软壳、Jolt 冲击、动态网格、落地音和气球爆裂读感；
  玻璃、复杂物体和高级空间音频仍在计划中。
- [气球爆裂手感研究](plan/physics/balloon-burst-feel-research.md)：
  气球橡胶膜充气/撕裂/回缩、实时碎片条带、宽带爆裂声与视觉时序的证据和调参建议；
  非契约研究记录，具体实现以 `contracts/softbody-runtime.md` 和源码为准。
- [地形系统：Open World Demo Collection 复刻计划](plan/rendering/terrain-openworld-demo-plan.md)：
  早期 CPU 网格、层混合与画面复刻的阶段计划；其中的阶段实现状态不是当前合同。
- [地形大世界与上百层地表架构](plan/rendering/terrain-large-world-layers-plan.md)：
  层表、tile 候选层、ID/权重与流式预算的历史设计，不替代当前合同。
- [地形 VT 流式闭环设计](plan/rendering/terrain-vt-streaming-design.md)：
  覆盖率反馈、LRU、上传 ring 与 epoch 保护的历史分阶段设计；
  coverage 的现存职责见 `contracts/terrain.md`。
- [地形层纹理流式设计](plan/rendering/terrain-layer-texture-streaming-design.md)：
  已退役 layer source VT 的方案比较与阶段记录，不是可选运行时入口；
  当前唯一材质合同为 `contracts/terrain-composited-vt.md`。
- [全局 layer catalog / 多 biome sourceSet 设计](plan/rendering/terrain-global-layer-catalog-design.md)：
  层身份与 sourceSet 的设计背景；现存作者源、coverage 与烘焙依赖以当前合同为准。
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
- [Valley Echo 角色运动复刻计划](plan/architecture/valley-echo-locomotion-replication-plan.md)：
  Valley of the Ancient 的 Echo 地面步态状态机、方向起步/停止、跳跃落地、SpringArm
  控制与一次性动作的资产导出和分阶段门禁。计划，非契约；当前运行时能力仍以
  `contracts/animation-motion-features.md`、`contracts/animation-motion-warping.md`、
  `contracts/animation-montage.md` 为准。
- [原 ALS 计划](plan/architecture/als-csharp-animation-plan.md)：
  已由 GASP 计划取代，仅保留历史决策，不作为后续实施依据。

## 维护

新增、移动或删除模块与专题时同步更新本表；源码覆盖范围变化时更新"未覆盖路径"。
根 `AGENTS.md` 只保留到本文件的入口，不复制映射。
