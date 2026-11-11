add_executable(physics_runtime_tests fractureImpactWorldTests.cpp bonePhysicsTests.cpp)
target_include_directories(physics_runtime_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(physics_runtime_tests PRIVATE vl_physics_runtime)
target_compile_definitions(physics_runtime_tests PRIVATE
    VL_BONE_PHYSICS_FIXTURE="${PROJECT_SOURCE_DIR}/tool/physics-runtime-tests/fixtures/BPH_EchoPostProcess.json")
vulkanlearn_add_gtest(physics_runtime_tests)
