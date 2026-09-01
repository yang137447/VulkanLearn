add_executable(material_schema_tests
    materialSchemaTests.cpp
    ${PROJECT_SOURCE_DIR}/source/material/materialDescriptorSchema.cpp
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
)

vulkanlearn_add_gtest(material_schema_tests)
