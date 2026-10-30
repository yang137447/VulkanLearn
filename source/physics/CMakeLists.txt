add_library(vl_fracture_core STATIC
    fracture/fractureBudget.cpp
    fracture/glassDamagePolicy.cpp
    fracture/planarFracture.cpp
    fracture/fragmentMesh.cpp
    fracture/sphericalFracture.cpp
    softbody/rubberShell.cpp)

target_include_directories(vl_fracture_core PUBLIC "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(vl_fracture_core PUBLIC Eigen3)

if(VULKANLEARN_ENABLE_NATIVE_PHYSICS OR VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS)
    add_library(vl_physics_runtime STATIC
        runtime/joltRuntime.cpp
        runtime/fractureImpactWorld.cpp)
    target_include_directories(vl_physics_runtime PUBLIC "${PROJECT_SOURCE_DIR}/source")
    target_link_libraries(vl_physics_runtime PUBLIC Eigen3 Jolt)
endif()
