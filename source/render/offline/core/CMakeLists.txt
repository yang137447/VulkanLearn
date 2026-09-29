add_library(vl_offline_core STATIC
    renderer.cpp
    renderSession.cpp
    groomPfm.cpp
    colorMap.cpp
    models/clothSheen.cpp
    models/clothFiber.cpp)

target_compile_features(vl_offline_core PUBLIC cxx_std_17)
# 保留现有 core/renderer.h 等接口路径，且不依赖 tool/ 中的旧 CLI。
target_include_directories(vl_offline_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(MSVC)
    target_compile_options(vl_offline_core PRIVATE /W4)
else()
    target_compile_options(vl_offline_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
