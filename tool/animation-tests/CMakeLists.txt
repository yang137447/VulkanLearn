add_executable(animation_preflight_tests
    ozzDependencyProbeTests.cpp
    skeletalImportTests.cpp
    animationSkeletonTests.cpp
    animationRetargetTests.cpp
    animationSequenceDataTests.cpp
    animationSyncGroupTests.cpp
    animationMontageTests.cpp
    animationScriptDataTests.cpp
    motionHistoryTests.cpp
    motionDatabaseTests.cpp
    motionMatchingTests.cpp
    skeletalBlendTests.cpp
    notifyStateTests.cpp
    visualCorrectionTests.cpp
    slopeWarpingTests.cpp
    poseDriverTests.cpp
    postProcessTests.cpp
    fbxInspectionTests.cpp
    motionWarpingTests.cpp
    ueProjectInventoryTests.cpp
    # Validator 是纯 CPU/无 I/O 的规划层；测试旧 SM 缓存键与新 skeleton/sequence 字段。
    ${PROJECT_SOURCE_DIR}/source/mesh/validation/meshAssetValidator.cpp)
target_link_libraries(animation_preflight_tests PRIVATE vl_animation_preflight vl_animation_assets assimp)
target_compile_definitions(animation_preflight_tests PRIVATE
    VL_ANIMATION_FBX_FIXTURE="${PROJECT_SOURCE_DIR}/extern/Assimp/Assimp/test/models/FBX/animation_with_skeleton.fbx"
    # 真实 GASP Montage 夹具（从资源仓库复制），避免模块测试依赖外部资源目录。
    VL_ANIMATION_MONTAGE_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tool/animation-tests/fixtures/montages"
    # Echo PostProcess PoseDriver 的真实资产副本（9 driver / 65 target），
    # 逐值与 T3D 对账，测试不依赖资源 worktree。
    VL_ANIMATION_POSE_DRIVER_FIXTURE="${PROJECT_SOURCE_DIR}/tool/animation-tests/fixtures/pose-driver/EPD_EchoPostProcess.json"
    # Echo Twist ControlRig 的原生替代资产副本（8 组 / 16 目标）。
    VL_ANIMATION_POST_PROCESS_FIXTURE="${PROJECT_SOURCE_DIR}/tool/animation-tests/fixtures/post-process/EPP_EchoPostProcess.json")
vulkanlearn_add_gtest(animation_preflight_tests)
