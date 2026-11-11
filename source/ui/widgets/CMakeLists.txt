add_library(vl_ui_widgets STATIC
    widgetDocument.cpp
    input/inputWidgets.cpp
    layout/layoutWidgets.cpp
    navigation/tabWidgets.cpp
    property/propertyWidgets.cpp
    text/textWidgets.cpp
    views/viewWidgets.cpp)
target_include_directories(vl_ui_widgets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(vl_ui_widgets PUBLIC RmlUi::Core)

if(VULKANLEARN_ENABLE_EDITOR)
    add_library(vl_editor_widgets STATIC
        editor/brushLibraryPanel.cpp
        workspace/dockWorkspace.cpp)
    target_include_directories(vl_editor_widgets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_link_libraries(vl_editor_widgets PUBLIC vl_ui_widgets RmlUi::Core)
endif()
