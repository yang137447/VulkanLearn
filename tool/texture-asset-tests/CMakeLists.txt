add_executable(texture_asset_tests
    textureAssetTests.cpp
    textureArrayTests.cpp
    blockCompressedContainerTests.cpp
    blockCompressedGpuTests.cpp
    ${PROJECT_SOURCE_DIR}/source/textureAssetLoader.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/hostImage.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/textureIO.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/ddsTexture.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/ktx2Texture.cpp
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

vulkanlearn_add_gtest(texture_asset_tests)

# commonFunction.h 的路径/配置读取经 TracingIfstream/RuntimeFileTrace 记账，需要两者定义。
target_sources(texture_asset_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/source/core/runtimeFileTrace.cpp"
    "${PROJECT_SOURCE_DIR}/source/core/tracingIfstream.cpp")
