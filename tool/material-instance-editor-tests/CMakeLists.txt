cmake_minimum_required(VERSION 3.20)

project(material_instance_editor_contract_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VULKANLEARN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    CACHE PATH
    "VulkanLearn source tree used by the contract tests")
get_filename_component(VULKANLEARN_SOURCE_DIR
    "${VULKANLEARN_SOURCE_DIR}"
    ABSOLUTE)

if(NOT EXISTS "${VULKANLEARN_SOURCE_DIR}/source/material/validation/materialAssetValidator.cpp")
    message(FATAL_ERROR
        "VULKANLEARN_SOURCE_DIR does not point at the VulkanLearn repository: "
        "${VULKANLEARN_SOURCE_DIR}")
endif()

if(NOT TARGET Json)
    add_subdirectory(
        "${VULKANLEARN_SOURCE_DIR}/extern/Json"
        "${CMAKE_CURRENT_BINARY_DIR}/vulkanlearn-json")
endif()

if(NOT TARGET BLAKE3::blake3)
    add_subdirectory(
        "${VULKANLEARN_SOURCE_DIR}/extern/BLAKE3"
        "${CMAKE_CURRENT_BINARY_DIR}/vulkanlearn-blake3")
endif()

if(NOT TARGET Vulkan::Vulkan)
    add_subdirectory(
        "${VULKANLEARN_SOURCE_DIR}/extern/VulkanSDK"
        "${CMAKE_CURRENT_BINARY_DIR}/vulkanlearn-vulkan-sdk")
endif()

if(NOT TARGET Eigen3)
    add_subdirectory(
        "${VULKANLEARN_SOURCE_DIR}/extern/Eigen3"
        "${CMAKE_CURRENT_BINARY_DIR}/vulkanlearn-eigen3")
endif()

if(NOT TARGET Spirv_Reflect)
    add_subdirectory(
        "${VULKANLEARN_SOURCE_DIR}/extern/Spirv-Reflect"
        "${CMAKE_CURRENT_BINARY_DIR}/vulkanlearn-spirv-reflect")
endif()

# 主构建里命令/服务/持久化由 vl_editor_asset_core 拥有，测试只链接它，
# 并从源列表里删掉随 vl_runtime 传递的运行时 TU；否则同一个 TU 会既进
# 归档又被目标编译，归档成员被拉取时出现重复定义。
# 独立配置（本目录自己声明 project()）没有这两个目标，只能继续内联编译；
# 两条路径必须互斥。
if(TARGET vl_editor_asset_core)
    set(VL_ASSET_CORE_TARGET vl_editor_asset_core)
    set(VL_ASSET_CORE_BASIC_SOURCES)
    set(VL_ASSET_CORE_FULL_SOURCES)
    set(VL_RUNTIME_EDITOR_TESTS_SOURCES)
    set(VL_RUNTIME_LANDED_TESTS_SOURCES)
    set(VL_RUNTIME_PRODUCTION_TESTS_SOURCES)
else()
    set(VL_ASSET_CORE_TARGET)
    set(VL_ASSET_CORE_BASIC_SOURCES
        "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommand.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommandJsonCodec.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/command/assetDocumentFields.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommandBus.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/persistence/materialInstancePersistence.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/persistence/materialInstanceSparseCandidate.cpp")
    set(VL_ASSET_CORE_FULL_SOURCES
        ${VL_ASSET_CORE_BASIC_SOURCES}
        "${VULKANLEARN_SOURCE_DIR}/source/editor/command/assetJsonCommandHost.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/assetDocumentService.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/assetCommandExecutor.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/sceneDocumentService.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/materialInstanceDocumentService.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/material/materialInstanceAssetCatalog.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/material/materialInstanceDocumentBatch.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/material/materialInstanceDocumentIO.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/material/materialInstanceDocumentOverrides.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/material/materialInstanceDocumentPaths.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/material/materialInstanceDocumentSnapshot.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/editor/service/material/materialInstanceDocumentValidation.cpp")
    # 独立配置必须逐一复现各目标实际需要的运行时切片：
    # editor/landed 只做契约与文档层验证，production 才需要场景解析、
    # 网格校验与破碎配置。多编或少编都会改变独立构建的结果。
    set(VL_RUNTIME_SLICE_COMMON_SOURCES
        "${VULKANLEARN_SOURCE_DIR}/source/material/validation/materialAssetValidator.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/shader/build/atomicFile.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/shader/build/contentHash.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/render/editorBridge/materialInstancePreviewTypes.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/render/editorBridge/materialInstancePreviewAdapter.cpp")
    set(VL_RUNTIME_EDITOR_TESTS_SOURCES
        ${VL_RUNTIME_SLICE_COMMON_SOURCES}
        "${VULKANLEARN_SOURCE_DIR}/source/material/materialDescriptorSchema.cpp")
    set(VL_RUNTIME_LANDED_TESTS_SOURCES
        ${VL_RUNTIME_SLICE_COMMON_SOURCES})
    set(VL_RUNTIME_PRODUCTION_TESTS_SOURCES
        ${VL_RUNTIME_SLICE_COMMON_SOURCES}
        "${VULKANLEARN_SOURCE_DIR}/source/material/materialDescriptorSchema.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/core/runtimeResult.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/mesh/validation/meshAssetValidator.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/render/frontend/scenePicking.cpp"
        "${VULKANLEARN_SOURCE_DIR}/source/physics/fracture/destructibleProfile.cpp")
endif()

add_executable(material_instance_editor_tests
    materialInstanceEditorTests.cpp
    support/materialInstanceEditorContract.cpp
    ${VL_RUNTIME_EDITOR_TESTS_SOURCES}
    ${VL_ASSET_CORE_BASIC_SOURCES}
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewController.cpp")

target_include_directories(material_instance_editor_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${VULKANLEARN_SOURCE_DIR}/source"
        "${VULKANLEARN_SOURCE_DIR}/extern/Eigen3"
        "${VULKANLEARN_SOURCE_DIR}/extern/Json/include"
        "${VULKANLEARN_SOURCE_DIR}/extern/VulkanSDK/include"
        "${VULKANLEARN_SOURCE_DIR}/extern/Spirv-Reflect/include")

target_compile_definitions(material_instance_editor_tests
    PRIVATE
        VULKANLEARN_MATERIAL_EDITOR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

target_link_libraries(material_instance_editor_tests
    PRIVATE
        ${VL_ASSET_CORE_TARGET}
        Vulkan::Vulkan
        Eigen3
        Json
        Spirv_Reflect
        BLAKE3::blake3)

vulkanlearn_add_gtest(
    material_instance_editor_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(material_instance_editor_landed_tests
    materialInstanceEditorLandedTests.cpp
    ${VL_RUNTIME_LANDED_TESTS_SOURCES}
    ${VL_ASSET_CORE_BASIC_SOURCES}
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewController.cpp")

target_include_directories(material_instance_editor_landed_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${VULKANLEARN_SOURCE_DIR}/source"
        "${VULKANLEARN_SOURCE_DIR}/extern/Eigen3"
        "${VULKANLEARN_SOURCE_DIR}/extern/Json/include"
        "${VULKANLEARN_SOURCE_DIR}/extern/VulkanSDK/include"
        "${VULKANLEARN_SOURCE_DIR}/extern/Spirv-Reflect/include")

target_compile_definitions(material_instance_editor_landed_tests
    PRIVATE
        VULKANLEARN_MATERIAL_EDITOR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

target_link_libraries(material_instance_editor_landed_tests
    PRIVATE
        ${VL_ASSET_CORE_TARGET}
        Vulkan::Vulkan
        Eigen3
        Json
        Spirv_Reflect
        BLAKE3::blake3)

vulkanlearn_add_gtest(
    material_instance_editor_landed_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(material_instance_editor_production_tests
    materialInstanceEditorProductionTests.cpp
    particleAssetTests.cpp
    particleBudgetOverrideTests.cpp
    particleBurstTests.cpp
    particleCollisionTests.cpp
    particleCurveEditTests.cpp
    particleDocumentHistoryTests.cpp
    particleDumpTests.cpp
    particleFlipbookEditTests.cpp
    particleFrameStatsTests.cpp
    particleIndirectTests.cpp
    particleLightRuntimeTests.cpp
    particleDecalRuntimeTests.cpp
    particleEmitterBoundsTests.cpp
    particleEmitterCullingTests.cpp
    particleEmitterEditTests.cpp
    particleEmitterKeyTests.cpp
    particleEmitterOrderTests.cpp
    particleFlipbookTests.cpp
    particleHitScanTests.cpp
    particleLodTests.cpp
    particleMaterialSdfTests.cpp
    particleMeshCollisionTests.cpp
    particleMeshTests.cpp
    particleMeshReferenceTests.cpp
    particleModuleEditTests.cpp
    particlePreviewUpdateTests.cpp
    particleRenderIdentityTests.cpp
    particleRibbonTests.cpp
    particleSoftDepthTests.cpp
    particleRuntimeCommandTests.cpp
    particleSkeletalPreflightTests.cpp
    particleSimulationDeltaTests.cpp
    particleTextureReferenceTests.cpp
    ${VL_RUNTIME_PRODUCTION_TESTS_SOURCES}
    ${VL_ASSET_CORE_FULL_SOURCES}
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewController.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/runtime/materialInstanceEditorRuntime.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleSkeletalPreflight.cpp"
)
target_sources(material_instance_editor_production_tests PRIVATE
    "${VULKANLEARN_SOURCE_DIR}/source/editor/selection/materialInstanceSelection.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetJsonHelpers.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetCollisionAssets.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetParseEmitter.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetValidateEmitter.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetModuleSchema.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetLoader.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleCpuEmitterRuntime.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleDecalRuntime.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleLightRuntime.cpp")

target_include_directories(material_instance_editor_production_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${VULKANLEARN_SOURCE_DIR}/source"
        "${VULKANLEARN_SOURCE_DIR}/extern/Eigen3"
        "${VULKANLEARN_SOURCE_DIR}/extern/Json/include"
        "${VULKANLEARN_SOURCE_DIR}/extern/VulkanSDK/include"
        "${VULKANLEARN_SOURCE_DIR}/extern/Spirv-Reflect/include")

target_compile_definitions(material_instance_editor_production_tests
    PRIVATE
        VULKANLEARN_MATERIAL_EDITOR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

target_compile_options(material_instance_editor_production_tests
    PRIVATE
        -fweak)

target_link_libraries(material_instance_editor_production_tests
    PRIVATE
        ${VL_ASSET_CORE_TARGET}
        Vulkan::Vulkan
        Eigen3
        Json
        Spirv_Reflect
        BLAKE3::blake3)

vulkanlearn_add_gtest(
    material_instance_editor_production_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(particle_mesh_collision_tests
    particleMeshCollisionTests.cpp
    particleCollisionTests.cpp
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetJsonHelpers.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetCollisionAssets.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetParseEmitter.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetValidateEmitter.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetModuleSchema.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/particles/particleAssetLoader.cpp")

target_include_directories(particle_mesh_collision_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${VULKANLEARN_SOURCE_DIR}/source"
        "${VULKANLEARN_SOURCE_DIR}/extern/Eigen3"
        "${VULKANLEARN_SOURCE_DIR}/extern/Json/include"
        "${VULKANLEARN_SOURCE_DIR}/extern/VulkanSDK/include"
        "${VULKANLEARN_SOURCE_DIR}/extern/Spirv-Reflect/include")

target_compile_options(particle_mesh_collision_tests
    PRIVATE
        -fweak)

target_link_libraries(particle_mesh_collision_tests
    PRIVATE
        Vulkan::Vulkan
        Eigen3
        Json
        Spirv_Reflect
        BLAKE3::blake3)

vulkanlearn_add_gtest(
    particle_mesh_collision_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
