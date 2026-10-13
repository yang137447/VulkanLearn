add_library(vl_ui_widgets STATIC
    widgetDocument.cpp
    input/inputWidgets.cpp
    layout/layoutWidgets.cpp
    navigation/tabWidgets.cpp
    property/propertyWidgets.cpp
    text/textWidgets.cpp
    views/viewWidgets.cpp
    workspace/dockWorkspace.cpp)
target_include_directories(vl_ui_widgets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(vl_ui_widgets PUBLIC RmlUi::Core)
