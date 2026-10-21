include(FetchContent)

if(CMAKE_VERSION VERSION_LESS 3.24)
    message(FATAL_ERROR "Animation assets require CMake 3.24 or later for ozz-animation.")
endif()

# 上游样例、转换器和测试不进入 VL 构建；运行时采样不依赖 Autodesk FBX SDK。
set(BUILD_SHARED_LIBS OFF)
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_data OFF CACHE BOOL "" FORCE)
set(ozz_build_postfix OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vl_ozz
    URL https://github.com/guillaumeblanc/ozz-animation/archive/6cbdc790123aa4731d82e255df187b3a8a808256.tar.gz
    URL_HASH SHA256=ef52ed59de92cfbe352417683a254a4fdcab2dffd92967df680f5194bdd8332a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(vl_ozz)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/licenses")
configure_file("${vl_ozz_SOURCE_DIR}/LICENSE.md"
    "${CMAKE_BINARY_DIR}/bin/licenses/ozz-animation.txt" COPYONLY)

# 依赖夹具只供开发工具使用，不进入运行时内容或充当 GASP 资产。
add_library(vl_ozz_probe_fixtures INTERFACE)
target_compile_definitions(vl_ozz_probe_fixtures INTERFACE
    VL_OZZ_MEDIA_ROOT="${vl_ozz_SOURCE_DIR}/media/bin")
