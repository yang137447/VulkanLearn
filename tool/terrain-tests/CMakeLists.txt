# 本目标自带 31 个 vl_runtime 也拥有的地形/资源/图像 IO TU，保持地形测试
# 不依赖图形运行时即可运行。P5 不为它链接 vl_editor_asset_core（本目标也
# 不消费资产核心），迁移留给让测试链接 vl_editor 的后续阶段。
add_executable(terrain_tests
    terrainTests.cpp
    terrainHeightmapIOTests.cpp
    terrainHeightmapEditorTests.cpp
    terrainPaintFileTests.cpp
    terrainPaintManifestTests.cpp
    terrainTressfxStubs.cpp
    terrainPageManagerP31Tests.cpp
    terrainCompositedVTTests.cpp
    terrainCompositedProductionTests.cpp
    terrainCompositedQualityClockTests.cpp
    terrainCompositedSourceSnapshotTests.cpp
    terrainCompositedSourceIntegrationTests.cpp
    terrainCompositedResourceSnapshotTests.cpp
    terrainCompositedProductionGpuTests.cpp
    terrainCompositedAdjacencyTests.cpp
    terrainCompositedDirtyTests.cpp
    terrainCompositedV2StoreTests.cpp
    terrainRvtBudgetTests.cpp
    terrainRvtSchedulerTests.cpp
    terrainCompositedAuditTests.cpp
    terrainCompositedAssetTests.cpp
    terrainCompositedAuthorMonitorTests.cpp
    terrainHeightAtlasTests.cpp
    terrainHeightAtlasStoreTests.cpp
    terrainCompositedResidencyTests.cpp
    terrainCompositedDependenciesTests.cpp
    terrainCompositedWeightOverlayTests.cpp
    terrainVertexUpdateQueueTests.cpp
    terrainChunkStreamerLifecycleTests.cpp
    terrainCompositedFeedbackTests.cpp
terrainLayerRegistryTests.cpp
    terrainWeightMapBindingTests.cpp
    terrainCollisionTests.cpp
    terrainCollisionInstanceTests.cpp
    terrainCollisionProxyTests.cpp
    terrainCollisionPatchTests.cpp
    terrainChunkResidencyTests.cpp
    vegetationAssetTests.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainAssetLoader.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCollision.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCollisionPatch.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCollisionProxy.cpp
    ${PROJECT_SOURCE_DIR}/source/engine/terrainCollisionPatchStreamer.cpp
    ${PROJECT_SOURCE_DIR}/source/engine/terrainChunkStreamerLifecycle.cpp
    ${PROJECT_SOURCE_DIR}/source/world/loading/terrainCollisionInstances.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainFeedback.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightmap.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightmapIO.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightmapEditor.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightmapWriter.cpp
    ${PROJECT_SOURCE_DIR}/source/editor/terrain/terrainPaintSession.cpp
    ${PROJECT_SOURCE_DIR}/source/editor/terrain/terrainCompositedAuthorMonitor.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainLayerTable.cpp
${PROJECT_SOURCE_DIR}/source/terrain/terrainLayerRegistry.cpp
${PROJECT_SOURCE_DIR}/source/terrain/terrainLayerSources.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedVT.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedVTStore.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedProduction.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedAdjacency.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedDirty.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainRvtBudget.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedSourceSnapshots.cpp
    ${PROJECT_SOURCE_DIR}/source/render/resource/terrainCompositedResourceSnapshot.cpp
    ${PROJECT_SOURCE_DIR}/source/render/resource/terrainCompositedWorldBindings.cpp
    ${PROJECT_SOURCE_DIR}/source/core/runtimeFileTrace.cpp
    ${PROJECT_SOURCE_DIR}/source/core/tracingIfstream.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedPageSourceBatch.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainRvtScheduler.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedResidency.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedDependencies.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedWeightOverlay.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainVertexUpdateQueue.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedFeedback.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightAtlasStore.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainMeshBuilder.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainChunkResidency.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainPageManager.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainWeightMap.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainWeightMapBinding.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainLayerPaint.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainPaintFile.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainPaintManifest.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainPaintSha256.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp
    ${PROJECT_SOURCE_DIR}/source/physics/joltRegistration.cpp
    ${PROJECT_SOURCE_DIR}/source/render/resource/rendererResourceCache.cpp
    ${PROJECT_SOURCE_DIR}/source/render/resource/tressfxGeometryCache.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/validation/meshAssetValidator.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/vegetationAsset.cpp
    ${PROJECT_SOURCE_DIR}/source/render/resource/resourceRetireQueue.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/hostImage.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/textureIO.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/ddsTexture.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/ktx2Texture.cpp
)

target_include_directories(terrain_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(terrain_tests
    PRIVATE
        Vulkan::Vulkan
        Eigen3
        Json
        stb_image
        OpenEXR::OpenEXR
        Jolt
        vl_hair_assets
        vl_fracture_core
        vl_animation_assets
        Threads::Threads
        BLAKE3::blake3
)

vulkanlearn_add_gtest(terrain_tests)
target_compile_definitions(terrain_tests PRIVATE VL_FILE_IO_TEST_HOOKS)

add_executable(terrain_composited_refresh_tests
    terrainCompositedRefreshTests.cpp
    terrainCompositedDirtyTests.cpp
    terrainCompositedAuthorMonitorTests.cpp
    ${PROJECT_SOURCE_DIR}/source/editor/terrain/terrainEditSession.cpp
    ${PROJECT_SOURCE_DIR}/source/editor/terrain/terrainPaintSession.cpp
    ${PROJECT_SOURCE_DIR}/source/editor/terrain/terrainCompositedAuthorMonitor.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedDirty.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedAdjacency.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedVT.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedVTStore.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedProduction.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedDependencies.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightmap.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightmapEditor.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightmapWriter.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainMeshBuilder.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainPaintManifest.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainPaintFile.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainPaintSha256.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp
    ${PROJECT_SOURCE_DIR}/source/core/runtimeFileTrace.cpp
    ${PROJECT_SOURCE_DIR}/source/core/tracingIfstream.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/hostImage.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/textureIO.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/ddsTexture.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/ktx2Texture.cpp
)
target_include_directories(terrain_composited_refresh_tests PRIVATE ${PROJECT_SOURCE_DIR}/source)
target_sources(terrain_composited_refresh_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainLayerRegistry.cpp)
target_link_libraries(terrain_composited_refresh_tests PRIVATE
    Vulkan::Vulkan Eigen3 Json stb_image OpenEXR::OpenEXR Threads::Threads BLAKE3::blake3)
vulkanlearn_add_gtest(terrain_composited_refresh_tests)

add_executable(terrain_material_state_tests terrainMaterialStateBatchTests.cpp)
target_include_directories(terrain_material_state_tests PRIVATE ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(terrain_material_state_tests PRIVATE vl_runtime)
vulkanlearn_add_gtest(terrain_material_state_tests)

add_executable(terrain_fault_owner_registration_tests terrainFaultOwnerRegistrationTests.cpp)
target_include_directories(terrain_fault_owner_registration_tests PRIVATE ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(terrain_fault_owner_registration_tests PRIVATE vl_runtime)
vulkanlearn_add_gtest(terrain_fault_owner_registration_tests)

add_executable(terrain_capture_frame_lifecycle_tests terrainCaptureFrameLifecycleTests.cpp)
target_include_directories(terrain_capture_frame_lifecycle_tests PRIVATE ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(terrain_capture_frame_lifecycle_tests PRIVATE vl_runtime)
vulkanlearn_add_gtest(terrain_capture_frame_lifecycle_tests)

add_executable(terrain_request_diagnostics_tests
    terrainCompositedRequestDiagnosticsTests.cpp
    ${PROJECT_SOURCE_DIR}/source/render/terrain/terrainCompositedRuntime.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedResidency.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainRvtBudget.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedAdjacency.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedProduction.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedVT.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedVTStore.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainPaintSha256.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp
    ${PROJECT_SOURCE_DIR}/source/core/runtimeFileTrace.cpp
    ${PROJECT_SOURCE_DIR}/source/core/tracingIfstream.cpp)
target_include_directories(terrain_request_diagnostics_tests BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain-composited-runtime-private
    ${PROJECT_SOURCE_DIR}/source)
target_compile_definitions(terrain_request_diagnostics_tests PRIVATE VL_FILE_IO_TEST_HOOKS)
target_link_libraries(terrain_request_diagnostics_tests PRIVATE
    Vulkan::Vulkan Eigen3 Json Threads::Threads BLAKE3::blake3)
vulkanlearn_add_gtest(terrain_request_diagnostics_tests)

get_target_property(terrain_priority_runtime_sources terrain_request_diagnostics_tests SOURCES)
list(REMOVE_ITEM terrain_priority_runtime_sources terrainCompositedRequestDiagnosticsTests.cpp)
add_executable(terrain_production_priority_tests
    terrainCompositedProductionPriorityTests.cpp
    ${terrain_priority_runtime_sources})
target_include_directories(terrain_production_priority_tests BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain-composited-runtime-private
    ${PROJECT_SOURCE_DIR}/source)
target_compile_definitions(terrain_production_priority_tests PRIVATE VL_FILE_IO_TEST_HOOKS
    VL_TERRAIN_SERVICE_SOURCE="${PROJECT_SOURCE_DIR}/source/render/resource/terrainCompositedService.cpp")
target_link_libraries(terrain_production_priority_tests PRIVATE
    Vulkan::Vulkan Eigen3 Json Threads::Threads BLAKE3::blake3)
vulkanlearn_add_gtest(terrain_production_priority_tests)

if(VULKANLEARN_BUILD_TERRAIN_CAPTURE_VALIDATION)
    add_executable(terrain_evidence_writer_tests
        ${PROJECT_SOURCE_DIR}/tool/validation/terrain/native-capture/terrainEvidenceWriterTests.cpp)
    target_include_directories(terrain_evidence_writer_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tool/validation/terrain/native-capture)
endif()
