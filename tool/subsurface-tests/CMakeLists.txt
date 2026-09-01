add_executable(subsurface_tests
    subsurfaceTests.cpp
    ${PROJECT_SOURCE_DIR}/source/render/subsurface/subsurfaceAssets.cpp
    ${PROJECT_SOURCE_DIR}/source/render/subsurface/subsurfaceGBufferCodec.cpp
)

target_include_directories(subsurface_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_compile_definitions(subsurface_tests
    PRIVATE
        VULKANLEARN_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(subsurface_tests
    PRIVATE
        Json
)

vulkanlearn_add_gtest(subsurface_tests)
