option(VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS "Build and host C# gameplay Blueprints (requires .NET 8 SDK)." OFF)
add_library(vl_script_host STATIC "${PROJECT_SOURCE_DIR}/source/script/scriptHost.cpp")
target_include_directories(vl_script_host PUBLIC "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(vl_script_host PUBLIC Json)

if(VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS)
    include(FetchContent)
    # 函数隔离第三方编译选项；只编译 CPU 库，不引入样例、GPU 后端或另一套 Vulkan owner。
    function(vl_add_jolt_cpu)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        set(OVERRIDE_CXX_FLAGS OFF)
        set(INTERPROCEDURAL_OPTIMIZATION OFF)
        set(ENABLE_ALL_WARNINGS OFF)
        set(ENABLE_INSTALL OFF)
        set(JPH_BUILD_SHARED_LIBS OFF)
        set(CPP_RTTI_ENABLED ON)
        set(CPP_EXCEPTIONS_ENABLED ON)
        set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
        set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
        set(ENABLE_OBJECT_STREAM OFF)
        foreach(feature SSE4_1 SSE4_2 AVX AVX2 AVX512 LZCNT TZCNT F16C FMADD)
            set(USE_${feature} OFF)
        endforeach()
        foreach(backend DX12 VK MTL CPU_COMPUTE)
            set(JPH_USE_${backend} OFF)
        endforeach()
        FetchContent_Declare(vl_jolt
            URL https://codeload.github.com/jrouwe/JoltPhysics/tar.gz/e77f175595e64cb44218cc9d9d56fc365ad0e36a
            URL_HASH SHA256=1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_SUBDIR Build)
        FetchContent_MakeAvailable(vl_jolt)
    endfunction()
    vl_add_jolt_cpu()
    target_link_libraries(vl_script_host PUBLIC Jolt)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "C# Blueprint hosting currently requires Windows x64.")
    endif()
    find_program(VULKANLEARN_DOTNET_EXECUTABLE dotnet
        HINTS "${PROJECT_SOURCE_DIR}/artifacts/dotnet-sdk" REQUIRED)
    execute_process(COMMAND "${VULKANLEARN_DOTNET_EXECUTABLE}" --list-sdks
        OUTPUT_VARIABLE dotnet_sdks RESULT_VARIABLE dotnet_status)
    if(NOT dotnet_status EQUAL 0 OR NOT dotnet_sdks MATCHES "8\\.0\\.")
        message(FATAL_ERROR "C# Blueprints require a .NET 8 SDK. Set VULKANLEARN_DOTNET_EXECUTABLE to its dotnet.exe.")
    endif()
    get_filename_component(dotnet_root "${VULKANLEARN_DOTNET_EXECUTABLE}" DIRECTORY)
    file(GLOB hostfxr_candidates "${dotnet_root}/host/fxr/8.*/hostfxr.dll")
    list(SORT hostfxr_candidates COMPARE NATURAL ORDER DESCENDING)
    if(NOT hostfxr_candidates)
        message(FATAL_ERROR "hostfxr.dll was not found alongside the .NET SDK.")
    endif()
    list(GET hostfxr_candidates 0 hostfxr)
    file(GLOB host_headers "${dotnet_root}/packs/Microsoft.NETCore.App.Host.win-x64/8.*/runtimes/win-x64/native")
    list(SORT host_headers COMPARE NATURAL ORDER DESCENDING)
    list(GET host_headers 0 host_include)
    target_include_directories(vl_script_host PRIVATE "${host_include}")
    set(VULKANLEARN_SCRIPT_OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/managed")
    set(VULKANLEARN_SCRIPT_CONFIGURATION "$<IF:$<CONFIG:Debug>,Debug,Release>")
    set(VULKANLEARN_SCRIPT_OUTPUT "${VULKANLEARN_SCRIPT_OUTPUT}" PARENT_SCOPE)
    set(VULKANLEARN_SCRIPT_CONFIGURATION "${VULKANLEARN_SCRIPT_CONFIGURATION}" PARENT_SCOPE)
    # 引擎只提供 API 与项目模板，用户源码始终从运行时资源根收集。
    # 生成项目供 IDE 使用；删除 Generated 后重新配置即可恢复，不修改任何作者源码。
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/config/config.json")
    file(READ "${PROJECT_SOURCE_DIR}/config/config.json" vl_runtime_config)
    string(JSON vl_resource_relative GET "${vl_runtime_config}" resourcePath)
    get_filename_component(vl_default_resource_root "${vl_resource_relative}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    set(VULKANLEARN_SCRIPT_RESOURCE_ROOT "" CACHE PATH "Override the C# resource root (empty uses config.json resourcePath).")
    if(NOT VULKANLEARN_SCRIPT_RESOURCE_ROOT)
        set(VULKANLEARN_SCRIPT_RESOURCE_ROOT "${vl_default_resource_root}")
    endif()
    get_filename_component(VULKANLEARN_SCRIPT_RESOURCE_ROOT "${VULKANLEARN_SCRIPT_RESOURCE_ROOT}" ABSOLUTE)
    set(VULKANLEARN_SCRIPT_GAMEPLAY_PROJECT
        "${VULKANLEARN_SCRIPT_RESOURCE_ROOT}/Generated/Runtime/CSharp/Gameplay.csproj")
    function(vl_script_xml_text output value)
        string(REPLACE "&" "&amp;" value "${value}")
        string(REPLACE "<" "&lt;" value "${value}")
        string(REPLACE "\"" "&quot;" value "${value}")
        set(${output} "${value}" PARENT_SCOPE)
    endfunction()
    vl_script_xml_text(VL_RESOURCE_ROOT_XML "${VULKANLEARN_SCRIPT_RESOURCE_ROOT}")
    vl_script_xml_text(VL_RUNTIME_PROJECT_XML "${CMAKE_CURRENT_SOURCE_DIR}/VulkanLearn.Runtime/VulkanLearn.Runtime.csproj")
    configure_file(Gameplay/Gameplay.csproj.in "${VULKANLEARN_SCRIPT_GAMEPLAY_PROJECT}" @ONLY)
    set(VULKANLEARN_SCRIPT_RESOURCE_ROOT "${VULKANLEARN_SCRIPT_RESOURCE_ROOT}" PARENT_SCOPE)
    add_executable(vl_script_sources sourceManifest.cpp "${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp")
    target_include_directories(vl_script_sources PRIVATE "${PROJECT_SOURCE_DIR}/source")
    if(MINGW)
        target_link_options(vl_script_sources PRIVATE -municode)
    endif()
    set(VULKANLEARN_SCRIPT_SOURCE_ITEMS "${CMAKE_CURRENT_BINARY_DIR}/ResourceSources.props")
    set(VULKANLEARN_SCRIPT_SOURCE_ITEMS "${VULKANLEARN_SCRIPT_SOURCE_ITEMS}" PARENT_SCOPE)
    add_custom_target(vl_gameplay
        COMMAND vl_script_sources "${VULKANLEARN_SCRIPT_RESOURCE_ROOT}" "${VULKANLEARN_SCRIPT_SOURCE_ITEMS}"
        COMMAND "${VULKANLEARN_DOTNET_EXECUTABLE}" build
            "${VULKANLEARN_SCRIPT_GAMEPLAY_PROJECT}" --configuration "${VULKANLEARN_SCRIPT_CONFIGURATION}"
            "-p:VulkanLearnSourceItems=${VULKANLEARN_SCRIPT_SOURCE_ITEMS}"
            --artifacts-path "${CMAKE_CURRENT_BINARY_DIR}/managed-intermediate"
            --output "${VULKANLEARN_SCRIPT_OUTPUT}" --nologo
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${hostfxr}" "${VULKANLEARN_SCRIPT_OUTPUT}/hostfxr.dll"
        DEPENDS vl_script_sources VERBATIM)
    # 显式打包目标携带 SDK 随附的运行时，不把 SDK 或全局 .NET 安装当成部署前提。
    file(GLOB runtime_candidates LIST_DIRECTORIES TRUE "${dotnet_root}/shared/Microsoft.NETCore.App/8.*")
    list(SORT runtime_candidates COMPARE NATURAL ORDER DESCENDING)
    if(NOT runtime_candidates)
        message(FATAL_ERROR "The .NET 8 runtime was not found alongside the SDK.")
    endif()
    list(GET runtime_candidates 0 dotnet_runtime)
    get_filename_component(runtime_version "${dotnet_runtime}" NAME)
    get_filename_component(hostfxr_version_directory "${hostfxr}" DIRECTORY)
    get_filename_component(hostfxr_version "${hostfxr_version_directory}" NAME)
    add_custom_target(vl_script_package
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${dotnet_runtime}"
            "${VULKANLEARN_SCRIPT_OUTPUT}/dotnet/shared/Microsoft.NETCore.App/${runtime_version}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${hostfxr_version_directory}"
            "${VULKANLEARN_SCRIPT_OUTPUT}/dotnet/host/fxr/${hostfxr_version}"
        DEPENDS vl_gameplay VERBATIM)
    target_compile_definitions(vl_script_host PUBLIC VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
        VL_SCRIPT_OUTPUT_DIRECTORY="${VULKANLEARN_SCRIPT_OUTPUT}"
        VL_SCRIPT_DOTNET_EXECUTABLE="${VULKANLEARN_DOTNET_EXECUTABLE}"
        VL_SCRIPT_GAMEPLAY_PROJECT="${VULKANLEARN_SCRIPT_GAMEPLAY_PROJECT}"
        VL_SCRIPT_RESOURCE_ROOT="${VULKANLEARN_SCRIPT_RESOURCE_ROOT}"
        VL_SCRIPT_RUNTIME_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/VulkanLearn.Runtime"
        VL_SCRIPT_BUILD_CONFIGURATION="${VULKANLEARN_SCRIPT_CONFIGURATION}")
    target_compile_definitions(vl_script_host PRIVATE VL_DOTNET_ROOT="${dotnet_root}")
endif()
