include(FetchContent)

# 只保留运行时读取 EXR 需要的核心库，避免把工具、样例和 python 模块一起拉进来。
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_DEVELOPER_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
# 在当前仓库内直接自举依赖，减少本机额外安装 Imath/libdeflate/OpenJPH 的要求。
set(OPENEXR_FORCE_INTERNAL_IMATH ON CACHE BOOL "" FORCE)
set(OPENEXR_USE_INTERNAL_DEFLATE ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_OPENJPH ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    openexr
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openexr.git
    GIT_TAG v3.4.10
    GIT_SHALLOW TRUE
)

# 通过官方 CMake target 暴露 OpenEXR::OpenEXR，供主工程直接链接。
FetchContent_MakeAvailable(openexr)
