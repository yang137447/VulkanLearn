add_library(vl_game_export STATIC gameExporter.cpp nativeDependencies.cpp
    "${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp")
target_include_directories(vl_game_export PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(vl_game_export PUBLIC Json PRIVATE BLAKE3::blake3)

add_executable(vl_export_game main.cpp)
target_link_libraries(vl_export_game PRIVATE vl_game_export)
if(VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS)
    get_filename_component(export_dotnet_root "${VULKANLEARN_DOTNET_EXECUTABLE}" DIRECTORY)
    target_compile_definitions(vl_export_game PRIVATE VL_EXPORT_CSHARP_ENABLED
        VL_EXPORT_DOTNET_ROOT="${export_dotnet_root}")
endif()
