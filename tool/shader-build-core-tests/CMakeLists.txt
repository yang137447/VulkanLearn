add_executable(shader_build_core_tests
    shaderBuildCoreTests.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/reload/shaderFileMonitor.cpp
)

target_include_directories(shader_build_core_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(shader_build_core_tests
    PRIVATE
        BLAKE3::blake3
)

vulkanlearn_add_gtest(shader_build_core_tests)
