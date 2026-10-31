include(FetchContent)
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v6.0.2
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Assimp
)
# 2. 设置 Assimp 构建选项
set(ASSIMP_BUILD_TESTS           OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES         OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS    OFF CACHE BOOL "" FORCE)
# 女性 Maya 离线转换复用 Assimp 的三角化/FBX exporter；运行时只继续使用 importer。
if(VULKANLEARN_BUILD_ANIMATION_TOOLS)
    set(ASSIMP_NO_EXPORT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_FBX_EXPORTER ON CACHE BOOL "" FORCE)
else()
    set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
endif()
set(ASSIMP_WARNINGS_AS_ERRORS    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(assimp)

#add_subdirectory(Assimp)
