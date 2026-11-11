add_executable(material_schema_tests
    materialSchemaTests.cpp
    tressfxMaterialTests.cpp
    skinDeformMaterialTests.cpp
    ${PROJECT_SOURCE_DIR}/source/materialInstanceValidator.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp
    ${PROJECT_SOURCE_DIR}/source/material/materialDescriptorSchema.cpp
    ${PROJECT_SOURCE_DIR}/source/material/loader/materialInstanceResolver.cpp
    ${PROJECT_SOURCE_DIR}/source/material/validation/materialAssetValidator.cpp
)

target_include_directories(material_schema_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(material_schema_tests
    PRIVATE
        Vulkan::Vulkan
        Eigen3
        Json
        Spirv_Reflect
        BLAKE3::blake3
)
target_compile_definitions(material_schema_tests PRIVATE VL_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")

vulkanlearn_add_gtest(material_schema_tests)

# MaterialInstance CPU state only: no Vulkan device, no renderer backend. The
# backend symbols needed by materialInstance.cpp are stubbed in the test TU.
add_executable(material_instance_thread_safety_tests
    materialInstanceThreadSafetyTests.cpp
    ${PROJECT_SOURCE_DIR}/source/materialInstance.cpp
    ${PROJECT_SOURCE_DIR}/source/material/materialDescriptorSchema.cpp
)

target_include_directories(material_instance_thread_safety_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(material_instance_thread_safety_tests
    PRIVATE
        Vulkan::Vulkan
        Vulkan::shaderc_combined
        Eigen3
        Json
        Spirv_Reflect
        BLAKE3::blake3
)

vulkanlearn_add_gtest(material_instance_thread_safety_tests)
