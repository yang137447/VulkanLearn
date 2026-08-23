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

add_test(
    NAME hair_lut_generator_contract
    COMMAND hair_lut_generator
        --validate
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures_hair_author_metadata.json)

add_test(
    NAME hair_lut_generator_dispatch
    COMMAND hair_lut_generator
        --dispatch
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures_hair_author_metadata.json)
