add_executable(editor_ui_tests
    editorUiTests.cpp
    actorPropertyTests.cpp
    sceneDocumentTests.cpp
    assetDocumentTests.cpp
    blueprintViewportInteractionTests.cpp
    modelPreviewCameraTests.cpp
    tressfxHairPreviewTests.cpp
    tressfxHairSimulationTests.cpp
    tressfxComponentUiTests.cpp
    tressfxGroupEditorTests.cpp
    debugDrawOverlayTests.cpp
    "${PROJECT_SOURCE_DIR}/source/ui/debugDrawOverlay.cpp"
    "${PROJECT_SOURCE_DIR}/source/engine/modelPreviewCamera.cpp"
    resourceEditorControllerTests.cpp
    widgetTests.cpp
    dockingTests.cpp
    performancePanelTests.cpp
    uiViewModelRefreshTests.cpp
    uiViewModelSectionsTests.cpp
    uiUpdateSchedulerTests.cpp
    "${PROJECT_SOURCE_DIR}/source/ui/uiRenderSnapshotQueue.cpp"
    logPanelTests.cpp
    gameplayHudTests.cpp
    contentBrowserTests.cpp
    contentBrowserPerformanceTests.cpp
    jsonAssetLibraryTests.cpp
    retargetPreviewTests.cpp
    thumbnailLibraryTests.cpp
    rmlUiTextureTests.cpp
    "${PROJECT_SOURCE_DIR}/source/ui/rmlUiInterfaces.cpp"
    "${PROJECT_SOURCE_DIR}/source/platform/platformWindow.cpp"
    "${PROJECT_SOURCE_DIR}/source/engine/diagnosticsSubsystem.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/assets/editorThumbnailLibrary.cpp"
    "${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp"
    "${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp"
    "${PROJECT_SOURCE_DIR}/source/resource/image/textureIO.cpp"
    "${PROJECT_SOURCE_DIR}/source/resource/image/hostImage.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorContentBrowserController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorDockingController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorLogPanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorPerformancePanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorJsonInspectorController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorResourceEditorController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorSceneActorTree.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/assets/editorJsonAssetLibrary.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/assets/editorJsonAssetTypes.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/assets/editorRetargetPreview.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/retargetAssetEditorPanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/materialInstanceAssetEditorPanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/tressfxGroupEditor.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/blueprintAssetEditorPanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/blueprintLogicComponentsPanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/sceneLogicComponentsPanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/scriptPropertyControls.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/blueprintViewportPanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommand.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommandJsonCodec.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/service/sceneDocumentService.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/service/assetDocumentService.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/service/assetCommandExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/assetDocumentFields.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommandBus.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/assetJsonCommandHost.cpp"
    "${PROJECT_SOURCE_DIR}/source/mesh/validation/meshAssetValidator.cpp"
    "${PROJECT_SOURCE_DIR}/source/physics/fracture/destructibleProfile.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/persistence/materialInstancePersistence.cpp"
    # 逐 actor 可见性的 World/快照回归（editorUiTests.cpp）：渲染侧过滤发生在
    # WorldSnapshotBuilder，因此这里要编进真实的 World 与快照构建器，
    # 而不是用测试替身证明"标志位被读到了"。
    # meshSceneObjectIdentity.cpp 也编进来：读写两端共用同一份身份推导，
    # 测试必须证明"大纲身份"就是 World 认得的那个键。
    "${PROJECT_SOURCE_DIR}/source/core/runtimeResult.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/world.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/meshSceneObjectIdentity.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/worldSnapshotBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/scene/editorSceneActorBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/scene/editorActorProperty.cpp"
    "${PROJECT_SOURCE_DIR}/source/sceneNode.cpp")
target_include_directories(editor_ui_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
if(MINGW)
    # 主 RmlUi 套件的模板实例数已超过 MinGW 标准 COFF section 上限。
    set_source_files_properties(editorUiTests.cpp blueprintViewportInteractionTests.cpp
        "${PROJECT_SOURCE_DIR}/source/editor/ui/materialInstanceAssetEditorPanel.cpp"
        "${PROJECT_SOURCE_DIR}/source/editor/ui/blueprintViewportPanel.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wa,-mbig-obj")
endif()
target_compile_definitions(editor_ui_tests PRIVATE
    VL_EDITOR_ASSET_ROOT="${PROJECT_SOURCE_DIR}/ui")
target_link_libraries(editor_ui_tests PRIVATE
    vl_ui_widgets vl_hair_assets RmlUi::Core SDL3::SDL3 Eigen3 Vulkan::Vulkan Json vl_offline_core vl_animation_assets vl_debug_draw Threads::Threads stb_image OpenEXR::OpenEXR BLAKE3::blake3)
vulkanlearn_add_gtest(editor_ui_tests)
