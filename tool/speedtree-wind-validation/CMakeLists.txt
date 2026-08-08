add_executable(speedtree_wind_validation
    speedtreeWindValidation.cpp
    ${PROJECT_SOURCE_DIR}/source/render/foliage/speedTreeWindSystem.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/loader/speedtree/speedTreeSourceAdapter.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/loader/common/mikkTSpaceMeshGenerator.cpp
)

target_include_directories(speedtree_wind_validation
PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/source
    ${SPEEDTREE_MODELER_ROOT}/standard_shaders
)

target_link_libraries(speedtree_wind_validation
PRIVATE
    Vulkan::Vulkan
    Eigen3
    Json
    MikkTSpace
)

add_test(
    NAME speedtree_wind_numeric
    COMMAND speedtree_wind_validation
)
set_tests_properties(speedtree_wind_numeric PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
)
