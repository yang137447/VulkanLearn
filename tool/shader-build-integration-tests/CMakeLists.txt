add_executable(shader_build_integration_tests
    shaderBuildIntegrationTests.cpp
    ${PROJECT_SOURCE_DIR}/source/pipeline/shaderReflectionService.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/shaderBuildManifest.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/shaderAbiSignature.cpp
    ${PROJECT_SOURCE_DIR}/source/shaderCompiler.cpp
    ${PROJECT_SOURCE_DIR}/source/shaderReflect.cpp
)

if(TARGET copy_shaderc_dll)
    add_dependencies(shader_build_integration_tests copy_shaderc_dll)
endif()

target_include_directories(shader_build_integration_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(shader_build_integration_tests
    PRIVATE
        Vulkan::Vulkan
        Vulkan::shaderc_combined
        Spirv_Reflect
        BLAKE3::blake3
        Eigen3
        Json
)

vulkanlearn_add_gtest(shader_build_integration_tests)
