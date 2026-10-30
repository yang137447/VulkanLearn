add_executable(fracture_core_tests
    destructibleProfileTests.cpp
    fractureBudgetTests.cpp
    glassDamagePolicyTests.cpp
    plateFracturePolicyTests.cpp
    planarFractureTests.cpp
    fragmentMeshTests.cpp
    sphericalFractureTests.cpp
    rubberShellTests.cpp)

target_link_libraries(fracture_core_tests PRIVATE vl_fracture_core)
vulkanlearn_add_gtest(fracture_core_tests)
