add_executable(terrain_tests
    terrainTests.cpp
    terrainPageManagerP31Tests.cpp
    terrainLayerPageManagerTests.cpp
    terrainLayerVisibilityTests.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainAssetLoader.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainFeedback.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightmap.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainLayerTable.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainLayerPageManager.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainLayerVisibility.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainMeshBuilder.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainPageManager.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainWeightMap.cpp
    ${PROJECT_SOURCE_DIR}/source/render/resource/rendererResourceCache.cpp
    ${PROJECT_SOURCE_DIR}/source/render/resource/resourceRetireQueue.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/hostImage.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/textureIO.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/ddsTexture.cpp
    ${PROJECT_SOURCE_DIR}/source/resource/image/ktx2Texture.cpp
)

target_include_directories(terrain_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(terrain_tests
    PRIVATE
        Vulkan::Vulkan
        Eigen3
        Json
        stb_image
        OpenEXR::OpenEXR
)

vulkanlearn_add_gtest(terrain_tests)
