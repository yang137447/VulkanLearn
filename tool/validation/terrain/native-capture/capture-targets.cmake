if(NOT TARGET vl_runtime)
    message(FATAL_ERROR "Include capture-targets.cmake after ordinary vl_runtime target creation")
endif()
get_filename_component(VL_TERRAIN_CAPTURE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT EXISTS "${VL_TERRAIN_CAPTURE_ROOT}/source/render/diagnostics/terrainCaptureHooks.h")
    message(FATAL_ERROR "Promote coherent R10 hook/frame patches before configuring test NativeGame")
endif()
add_executable(native_terrain_capture_game
    "${CMAKE_CURRENT_LIST_DIR}/nativeTerrainGame.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/nativeTerrainCaptureAdapter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainTesCapture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainTesCapturePolicy.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainTesCapturePipeline.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainReferenceAtlas.cpp")
target_compile_features(native_terrain_capture_game PRIVATE cxx_std_17)
target_include_directories(native_terrain_capture_game PRIVATE
    "${VL_TERRAIN_CAPTURE_ROOT}/source"
    "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(native_terrain_capture_game PRIVATE vl_runtime)
if(TARGET main_game)
    add_dependencies(native_terrain_capture_game main_game)
endif()
set_target_properties(native_terrain_capture_game PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(native_terrain_lit_capture_game
    "${CMAKE_CURRENT_LIST_DIR}/nativeTerrainLitGame.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/nativeTerrainCaptureAdapter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainTesCapture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainTesCapturePolicy.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainTesCapturePipeline.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainReferenceAtlas.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainLitTarget.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainLitReplaySession.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainLitCaptureAdapter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrainLitCase.cpp")
target_compile_features(native_terrain_lit_capture_game PRIVATE cxx_std_17)
target_include_directories(native_terrain_lit_capture_game PRIVATE
    "${VL_TERRAIN_CAPTURE_ROOT}/source" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(native_terrain_lit_capture_game PRIVATE vl_runtime)
vl_apply_executable_dependencies(native_terrain_lit_capture_game)
set_target_properties(native_terrain_lit_capture_game PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
