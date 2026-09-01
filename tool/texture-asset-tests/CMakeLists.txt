add_executable(texture_asset_tests
    textureAssetTests.cpp
    ${PROJECT_SOURCE_DIR}/source/textureAssetLoader.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/hostImage.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/textureIO.cpp
)

target_include_directories(texture_asset_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(texture_asset_tests
    PRIVATE
        Vulkan::Vulkan
        Eigen3
        Json
        stb_image
        OpenEXR::OpenEXR
)

add_test(NAME texture_asset_contract COMMAND texture_asset_tests)
