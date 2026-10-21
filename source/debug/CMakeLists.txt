add_library(vl_debug_draw STATIC drawDebug.cpp debugProjection.cpp debugDrawControl.cpp)
target_include_directories(vl_debug_draw PUBLIC "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(vl_debug_draw PUBLIC Eigen3)
