add_executable(hair_asset_tests
    tfxLoaderTests.cpp strandGeometryTests.cpp tressfxImportTests.cpp tressfxDynamicsTests.cpp
    tressfxSkinningTests.cpp
    sdfBuilderTests.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/loader/hair/tressfxModelImporter.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/validation/meshAssetValidator.cpp)
target_link_libraries(hair_asset_tests PRIVATE vl_hair_assets Vulkan::Vulkan Eigen3 Json)
vulkanlearn_add_gtest(hair_asset_tests)
