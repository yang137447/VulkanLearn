set(SPEEDTREE_WIND_VALIDATION_SOURCES
    speedtreeWindValidation.cpp
    ${PROJECT_SOURCE_DIR}/source/render/foliage/speedTreeWindSystem.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/loader/speedtree/speedTreeSourceAdapter.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/loader/common/mikkTSpaceMeshGenerator.cpp
)

add_executable(speedtree_wind_validation
    main.cpp
    ${SPEEDTREE_WIND_VALIDATION_SOURCES}
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

if(VULKANLEARN_BUILD_TESTS)
    add_executable(speedtree_wind_validation_tests
        speedTreeWindValidationTests.cpp
        ${SPEEDTREE_WIND_VALIDATION_SOURCES}
    )

    target_include_directories(speedtree_wind_validation_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/source
        ${SPEEDTREE_MODELER_ROOT}/standard_shaders
    )

    target_link_libraries(speedtree_wind_validation_tests
    PRIVATE
        Vulkan::Vulkan
        Eigen3
        Json
        MikkTSpace
    )

    vulkanlearn_add_gtest(
        speedtree_wind_validation_tests
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()
