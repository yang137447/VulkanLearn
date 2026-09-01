add_executable(hair_lut_generator
    hairLutGenerator.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairAssets.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairConventions.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairLutCoordinates.cpp
)

target_include_directories(hair_lut_generator
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(hair_lut_generator
    PRIVATE
        Json
)

add_executable(hair_lut_generator_tests
    hairLutGeneratorTests.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairAssets.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairConventions.cpp
    ${PROJECT_SOURCE_DIR}/source/render/hair/hairLutCoordinates.cpp
)

target_include_directories(hair_lut_generator_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(hair_lut_generator_tests
    PRIVATE
        Json
)

vulkanlearn_add_gtest(
    hair_lut_generator_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
