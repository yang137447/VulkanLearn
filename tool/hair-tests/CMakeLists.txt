add_executable(hair_tests
    hairTests.cpp
    support/hairReference.cpp
    support/hairGBufferCodec.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairConventions.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairAssets.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairLutCoordinates.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairMaterialContract.cpp
)

target_include_directories(hair_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
        ${PROJECT_SOURCE_DIR}/source
)

target_compile_definitions(hair_tests
    PRIVATE
        VULKANLEARN_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(hair_tests
    PRIVATE
        Json
)

vulkanlearn_add_gtest(hair_tests)
