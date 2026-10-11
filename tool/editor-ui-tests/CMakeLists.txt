add_executable(editor_ui_tests
    editorUiTests.cpp
    resourceEditorControllerTests.cpp
    widgetTests.cpp
    dockingTests.cpp
    contentBrowserTests.cpp
    contentBrowserPerformanceTests.cpp
    jsonAssetLibraryTests.cpp
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorContentBrowserController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorDockingController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorJsonInspectorController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorResourceEditorController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorSceneActorTree.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/assets/editorJsonAssetLibrary.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/assets/editorJsonAssetTypes.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/materialInstanceAssetEditorPanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommand.cpp"
    # 逐 actor 可见性的 World/快照回归（worldVisibilityTests.cpp）：渲染侧过滤发生在
    # WorldSnapshotBuilder，因此这里要编进真实的 World 与快照构建器，
    # 而不是用测试替身证明"标志位被读到了"。
    "${PROJECT_SOURCE_DIR}/source/core/runtimeResult.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/world.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/worldSnapshotBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/scene/editorSceneActorBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/source/sceneNode.cpp")
target_include_directories(editor_ui_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_compile_definitions(editor_ui_tests PRIVATE
    VL_EDITOR_ASSET_ROOT="${PROJECT_SOURCE_DIR}/ui")
target_link_libraries(editor_ui_tests PRIVATE
    vl_ui_widgets RmlUi::Core Eigen3 Vulkan::Vulkan Json vl_offline_core Threads::Threads)
vulkanlearn_add_gtest(editor_ui_tests)
