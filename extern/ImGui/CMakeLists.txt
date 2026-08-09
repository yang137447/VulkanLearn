cmake_minimum_required(VERSION 3.20)

include(FetchContent)

FetchContent_Declare(
    imgui_source
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b-docking
    GIT_SHALLOW TRUE
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui
)

FetchContent_GetProperties(imgui_source)
if(NOT imgui_source_POPULATED)
    FetchContent_Populate(imgui_source)
endif()

add_library(imgui STATIC
    ${imgui_source_SOURCE_DIR}/imgui.cpp
    ${imgui_source_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_source_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_source_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC ${imgui_source_SOURCE_DIR})
add_library(ImGui::ImGui ALIAS imgui)
