# 本目标自带 21 个 vl_runtime 也拥有的 TU（脚本宿主、World、地形碰撞等），
# 用于保持脚本测试的自包含切片。P5 不在此链接 vl_editor_asset_core，
# 否则运行时 TU 重复；迁移属于让脚本测试链接 vl_editor 的后续工作。
add_executable(script_tests scriptRuntimeTests.cpp tressfxComponentTests.cpp scriptBlueprintAuthoringTests.cpp
    scriptGameplayTests.cpp scriptAssetReloadTests.cpp scriptAssemblyReloadTests.cpp scriptBuildTests.cpp
    scriptPhysicsMeshGeometryTests.cpp
    scriptTerrainCollisionTests.cpp
    worldBuilderTerrainCollisionTests.cpp
    worldGameplayPlanTests.cpp
    scriptAnimationTests.cpp
    scriptTemplateExtensionTests.cpp
    scriptTimeTests.cpp
    scriptMixedLoadTests.cpp
    scriptHudTests.cpp
    scriptDebugDrawTests.cpp
    scriptLocomotionTests.cpp
    scriptTypeIdentityTests.cpp
    scriptSpringArmTests.cpp
    scriptLogicComponentTests.cpp
    scriptCustomDataTests.cpp
    scriptDuplicateMeshTests.cpp
    scriptPropertySchemaTests.cpp
    "${PROJECT_SOURCE_DIR}/source/editor/scene/scriptBlueprintAuthoring.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/service/sceneDocumentService.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/service/assetDocumentService.cpp"
    "${PROJECT_SOURCE_DIR}/source/particles/particleAssetJsonHelpers.cpp"
    "${PROJECT_SOURCE_DIR}/source/particles/particleAssetCollisionAssets.cpp"
    "${PROJECT_SOURCE_DIR}/source/particles/particleAssetParseEmitter.cpp"
    "${PROJECT_SOURCE_DIR}/source/particles/particleAssetValidateEmitter.cpp"
    "${PROJECT_SOURCE_DIR}/source/particles/particleAssetParseEmitterModules.cpp"
    "${PROJECT_SOURCE_DIR}/source/particles/particleAssetValidateEmitterModules.cpp"
    "${PROJECT_SOURCE_DIR}/source/particles/particleAssetModuleSchema.cpp"
    "${PROJECT_SOURCE_DIR}/source/particles/particleAssetLoader.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/assetDocumentFields.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommand.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/persistence/materialInstancePersistence.cpp"
    "${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp"
    "${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp"
    "${PROJECT_SOURCE_DIR}/source/mesh/validation/meshAssetValidator.cpp"
    "${PROJECT_SOURCE_DIR}/source/physics/fracture/destructibleProfile.cpp"
    "${PROJECT_SOURCE_DIR}/source/script/scriptRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/source/script/scriptAssemblyMigration.cpp"
    "${PROJECT_SOURCE_DIR}/source/script/scriptBuildService.cpp"
    "${PROJECT_SOURCE_DIR}/source/script/scriptPhysics.cpp"
    "${PROJECT_SOURCE_DIR}/source/terrain/terrainCollision.cpp"
    "${PROJECT_SOURCE_DIR}/source/terrain/terrainHeightmap.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/loading/terrainCollisionInstances.cpp"
    "${PROJECT_SOURCE_DIR}/source/resource/image/hostImage.cpp"
    "${PROJECT_SOURCE_DIR}/source/physics/joltRegistration.cpp"
    "${PROJECT_SOURCE_DIR}/source/physics/bonePhysics.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/world.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/worldParticleRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/worldParticleSystem.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/loading/worldBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/loading/worldGameplayPlanBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/source/render/shadow/csmSettings.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/worldAnimation.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/meshSceneObjectIdentity.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/worldSnapshotBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/source/sceneNode.cpp"
    "${PROJECT_SOURCE_DIR}/source/core/runtimeResult.cpp"
    "${PROJECT_SOURCE_DIR}/source/engine/diagnosticsSubsystem.cpp")
target_include_directories(script_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_compile_definitions(script_tests PRIVATE
    VL_SOURCE_DIR="${PROJECT_SOURCE_DIR}/source")
# 足部质量、放置行为与动态足部质量夹具超过 MinGW COFF section/文件上限；
# 只放宽这些目标文件格式。
if(MINGW)
    set_source_files_properties(scriptAnimationTests.cpp scriptRuntimeTests.cpp
        PROPERTIES COMPILE_OPTIONS "-Wa,-mbig-obj")
endif()
target_link_libraries(script_tests PRIVATE vl_script_host vl_animation_assets vl_debug_draw assimp Eigen3 Json Vulkan::Vulkan vl_offline_core BLAKE3::blake3 Jolt)
if(TARGET vl_gameplay)
    add_custom_target(vl_script_fixtures
        COMMAND "${VULKANLEARN_DOTNET_EXECUTABLE}" build
            "${CMAKE_CURRENT_SOURCE_DIR}/managed/ScriptFixtures.csproj" --configuration "${VULKANLEARN_SCRIPT_CONFIGURATION}"
            "-p:VulkanLearnResourceRoot=${VULKANLEARN_SCRIPT_RESOURCE_ROOT}"
            "-p:VulkanLearnSourceItems=${VULKANLEARN_SCRIPT_SOURCE_ITEMS}"
            --output "${VULKANLEARN_SCRIPT_OUTPUT}" --nologo
        VERBATIM)
    foreach(version 1 2)
        add_custom_target(vl_script_reload_fixtures_${version}
            COMMAND "${VULKANLEARN_DOTNET_EXECUTABLE}" build
                "${CMAKE_CURRENT_SOURCE_DIR}/reload/ReloadFixtures.csproj"
                --configuration "${VULKANLEARN_SCRIPT_CONFIGURATION}"
                "-p:DefineConstants=RELOAD_V${version}"
                --output "${VULKANLEARN_SCRIPT_OUTPUT}/reload-v${version}" --nologo
            VERBATIM)
        add_dependencies(vl_script_reload_fixtures_${version} vl_script_fixtures)
        add_dependencies(script_tests vl_script_reload_fixtures_${version})
    endforeach()
    # 同项目不同编译常量共享 obj，串行生成以免增量输出与符号交叉。
    add_dependencies(vl_script_reload_fixtures_2 vl_script_reload_fixtures_1)
    add_dependencies(vl_script_fixtures vl_gameplay)
    add_dependencies(script_tests vl_script_fixtures)
endif()
# 粒子/C# 资源路径从项目根解析；ctest 默认工作目录在 build 子目录，
# 会把 "../VukanLearnResources" 解析到错误位置。
vulkanlearn_add_gtest(script_tests WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
