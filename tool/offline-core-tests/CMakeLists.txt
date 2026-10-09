add_executable(vl_offline_core_tests
    core_test.cpp
    reference_gtest.cpp
    render_session_test.cpp
    sampling_gtest.cpp
    lighting_gtest.cpp
    cylinder_gtest.cpp)
target_sources(vl_offline_core_tests PRIVATE ashikhmin_velvet_gtest.cpp ashikhmin_satin_gtest.cpp)
target_link_libraries(vl_offline_core_tests PRIVATE vl_offline_core)
vulkanlearn_add_gtest(vl_offline_core_tests)
