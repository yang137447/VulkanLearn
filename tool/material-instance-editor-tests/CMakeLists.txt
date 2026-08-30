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

add_executable(material_instance_editor_tests
    materialInstanceEditorTests.cpp
    support/materialInstanceEditorContract.cpp
    "${VULKANLEARN_SOURCE_DIR}/source/material/materialDescriptorSchema.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/material/validation/materialAssetValidator.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/shader/build/atomicFile.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/shader/build/contentHash.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommand.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommandJsonCodec.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommandBus.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/persistence/materialInstancePersistence.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/persistence/materialInstanceSparseCandidate.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewTypes.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewAdapter.cpp"
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
        Vulkan::Vulkan
        Eigen3
        Json
        Spirv_Reflect
        BLAKE3::blake3)

enable_testing()
add_test(
    NAME material_instance_editor_contract
    COMMAND material_instance_editor_tests)
set_tests_properties(material_instance_editor_contract PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(material_instance_editor_landed_tests
    materialInstanceEditorLandedTests.cpp
    "${VULKANLEARN_SOURCE_DIR}/source/material/validation/materialAssetValidator.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/shader/build/atomicFile.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/shader/build/contentHash.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommand.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommandJsonCodec.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommandBus.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/persistence/materialInstancePersistence.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/persistence/materialInstanceSparseCandidate.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewTypes.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewAdapter.cpp"
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
        Vulkan::Vulkan
        Eigen3
        Json
        Spirv_Reflect
        BLAKE3::blake3)

add_test(
    NAME material_instance_editor_landed
    COMMAND material_instance_editor_landed_tests)
set_tests_properties(material_instance_editor_landed PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(material_instance_editor_production_tests
    materialInstanceEditorProductionTests.cpp
    "${VULKANLEARN_SOURCE_DIR}/source/material/materialDescriptorSchema.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/material/validation/materialAssetValidator.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/shader/build/atomicFile.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/shader/build/contentHash.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommand.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/command/editorCommandBus.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/persistence/materialInstancePersistence.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/persistence/materialInstanceSparseCandidate.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/service/materialInstanceDocumentService.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/core/runtimeResult.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewTypes.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewAdapter.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/preview/materialInstancePreviewController.cpp"
    "${VULKANLEARN_SOURCE_DIR}/source/editor/runtime/materialInstanceEditorRuntime.cpp")

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
        Vulkan::Vulkan
        Eigen3
        Json
        Spirv_Reflect
        BLAKE3::blake3)

add_test(
    NAME material_instance_editor_production
    COMMAND material_instance_editor_production_tests)
set_tests_properties(material_instance_editor_production PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
