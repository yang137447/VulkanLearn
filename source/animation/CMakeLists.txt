add_library(vl_animation_assets STATIC
    animationAssets.cpp
    animationSkeleton.cpp
    animationSequenceData.cpp
    animationMontage.cpp
    animationScriptData.cpp
    skeletalPlayback.cpp
    inertialization.cpp
    rootOffset.cpp
    footPlacement.cpp
    motionFeatures.cpp
    motionHistory.cpp
    motionDatabase.cpp
    motionMatching.cpp
    motionRuntimeDatabase.cpp
    motionRuntime.cpp
    motionWarping.cpp
    ../shader/build/contentHash.cpp
    ../shader/build/atomicFile.cpp
    ../mesh/loader/animation/assimpSkeletalImporter.cpp
    ../mesh/loader/common/mikkTSpaceMeshGenerator.cpp)
# Eigen/ozz 求解模板在 MinGW Debug 超过旧 COFF section 上限，只放宽该源的目标文件格式。
if(MINGW)
    set_source_files_properties(footPlacement.cpp PROPERTIES COMPILE_OPTIONS "-Wa,-mbig-obj")
endif()
target_include_directories(vl_animation_assets PUBLIC ${PROJECT_SOURCE_DIR}/source)
target_link_libraries(vl_animation_assets PUBLIC ozz_animation_offline Eigen3 Vulkan::Vulkan Json
    PRIVATE assimp MikkTSpace BLAKE3::blake3)
