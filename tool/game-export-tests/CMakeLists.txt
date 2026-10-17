add_executable(game_export_tests gameExporterTests.cpp)
target_include_directories(game_export_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(game_export_tests PRIVATE vl_game_export BLAKE3::blake3)
vulkanlearn_add_gtest(game_export_tests)
