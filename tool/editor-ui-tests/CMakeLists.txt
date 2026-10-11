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
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommand.cpp")
target_include_directories(editor_ui_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_compile_definitions(editor_ui_tests PRIVATE
    VL_EDITOR_ASSET_ROOT="${PROJECT_SOURCE_DIR}/ui")
target_link_libraries(editor_ui_tests PRIVATE
    vl_ui_widgets RmlUi::Core Eigen3 Vulkan::Vulkan Json vl_offline_core Threads::Threads)
vulkanlearn_add_gtest(editor_ui_tests)
