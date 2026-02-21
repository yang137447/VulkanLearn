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
set(ASSIMP_NO_EXPORT             ON  CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(assimp)

#add_subdirectory(Assimp)