add_library(vl_animation_preflight STATIC
    ozzDependencyProbe.cpp
    fbxInspection.cpp
    gaspImportReport.cpp
    motionDatabaseBuild.cpp
    ueProjectInventory.cpp)
target_include_directories(vl_animation_preflight PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(vl_animation_preflight
    PUBLIC ozz_animation_offline Json vl_animation_assets
    PRIVATE vl_ozz_probe_fixtures BLAKE3::blake3 assimp)

add_executable(vl_animation_probe main.cpp)
target_link_libraries(vl_animation_probe PRIVATE vl_animation_preflight)
if(MINGW)
    target_link_options(vl_animation_probe PRIVATE -municode)
endif()
