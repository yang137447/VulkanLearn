include_guard(GLOBAL)

# Machine-wide dependency prefix shared by the root project and ThirdParty/.
# The toolchain id must match tool/deps-stamp.ps1 byte for byte.

function(vl_deps_compute_toolchain_id output)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(${output}
            "win64-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
            PARENT_SCOPE)
        return()
    endif()

    get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(crt "unknown")

    if(WIN32 AND EXISTS "${compiler_bin}/libstdc++-6.dll")
        find_program(VL_DEPS_OBJDUMP_EXECUTABLE
            NAMES objdump
            HINTS "${compiler_bin}"
            NO_DEFAULT_PATH)
        if(VL_DEPS_OBJDUMP_EXECUTABLE)
            execute_process(
                COMMAND "${VL_DEPS_OBJDUMP_EXECUTABLE}" -p "${compiler_bin}/libstdc++-6.dll"
                OUTPUT_VARIABLE imports
                ERROR_QUIET)
            if(imports MATCHES "api-ms-win-crt-")
                set(crt "ucrt")
            elseif(imports MATCHES "msvcrt\\.dll")
                set(crt "msvcrt")
            endif()
        endif()
    endif()

    if(crt STREQUAL "unknown")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -v
            OUTPUT_QUIET
            ERROR_VARIABLE verbose)
        if(verbose MATCHES "--with-default-msvcrt=([A-Za-z0-9_]+)")
            set(crt "${CMAKE_MATCH_1}")
        elseif(verbose MATCHES "ucrt")
            set(crt "ucrt")
        endif()
    endif()

    set(${output}
        "win64-mingw-${CMAKE_CXX_COMPILER_VERSION}-${crt}"
        PARENT_SCOPE)
endfunction()

function(vl_deps_resolve_prefix root config output)
    vl_deps_compute_toolchain_id(toolchain_id)
    file(TO_CMAKE_PATH "${root}" normalized_root)
    string(REGEX REPLACE "/+$" "" normalized_root "${normalized_root}")
    set(${output} "${normalized_root}/${toolchain_id}/${config}" PARENT_SCOPE)
endfunction()

function(vl_deps_require_prebuilt_variables)
    if(NOT VULKANLEARN_DEPS_ROOT)
        message(FATAL_ERROR
            "VULKANLEARN_DEPS_MODE=prebuilt requires -DVULKANLEARN_DEPS_ROOT=<machine-wide deps root>.")
    endif()

    set(known SDL3 Assimp OpenEXR RmlUi FreeType Jolt)
    foreach(entry IN LISTS VULKANLEARN_DEPS_PREBUILT)
        if(entry STREQUAL "")
            message(FATAL_ERROR
                "VULKANLEARN_DEPS_PREBUILT contains an empty entry.")
        endif()
        if(NOT entry IN_LIST known)
            message(FATAL_ERROR
                "VULKANLEARN_DEPS_PREBUILT contains unknown dependency '${entry}'. "
                "Expected a subset of: ${known}.")
        endif()
    endforeach()
endfunction()

# Configures prebuilt consumption: validates the prefix/stamp, imports
# vl_deps, and prepares the build-time stamp gate and DLL deployment.
function(vl_deps_configure_prebuilt)
    vl_deps_require_prebuilt_variables()

    if(NOT CMAKE_BUILD_TYPE)
        message(FATAL_ERROR
            "VULKANLEARN_DEPS_MODE=prebuilt requires a single-configuration build "
            "with CMAKE_BUILD_TYPE set.")
    endif()

    vl_deps_resolve_prefix(
        "${VULKANLEARN_DEPS_ROOT}"
        "${CMAKE_BUILD_TYPE}"
        resolved_prefix)
    set(VULKANLEARN_DEPS_PREFIX "${resolved_prefix}" CACHE PATH
        "Resolved machine-wide dependency prefix." FORCE)
    file(TO_CMAKE_PATH "${VULKANLEARN_DEPS_PREFIX}" VULKANLEARN_DEPS_PREFIX)

    set(stamp_path "${VULKANLEARN_DEPS_PREFIX}/deps-stamp.json")
    if(NOT EXISTS "${stamp_path}")
        message(FATAL_ERROR
            "Prebuilt dependency prefix has no deps-stamp.json: ${VULKANLEARN_DEPS_PREFIX}. "
            "Run tool/build-deps.ps1 -Config ${CMAKE_BUILD_TYPE}.")
    endif()

    file(READ "${stamp_path}" stamp_json)
    string(JSON stamp_schema GET "${stamp_json}" schema)
    if(NOT stamp_schema STREQUAL "vulkanlearn-deps-stamp/v1")
        message(FATAL_ERROR
            "Unsupported dependency stamp schema '${stamp_schema}' in ${stamp_path}.")
    endif()

    vl_deps_compute_toolchain_id(toolchain_id)
    string(JSON stamp_toolchain GET "${stamp_json}" identity toolchainId)
    string(JSON stamp_config GET "${stamp_json}" identity config)
    if(NOT stamp_toolchain STREQUAL toolchain_id)
        message(FATAL_ERROR
            "Prebuilt prefix toolchain '${stamp_toolchain}' does not match the current "
            "compiler '${toolchain_id}'.")
    endif()
    if(NOT stamp_config STREQUAL CMAKE_BUILD_TYPE)
        message(FATAL_ERROR
            "Prebuilt prefix configuration '${stamp_config}' does not match "
            "CMAKE_BUILD_TYPE '${CMAKE_BUILD_TYPE}'.")
    endif()

    set(VL_DEPS_REQUESTED "${VULKANLEARN_DEPS_PREBUILT}")
    find_package(vl_deps CONFIG REQUIRED
        PATHS "${VULKANLEARN_DEPS_PREFIX}/share/vl_deps"
        NO_DEFAULT_PATH)

    # Full stamp verification (effective flags, feature options, lock hashes)
    # needs a populated CMake cache, so it runs as a build-time gate.
    find_program(VL_DEPS_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(VL_DEPS_POWERSHELL_EXECUTABLE)
        add_custom_target(vl_deps_stamp_check ALL
            COMMAND "${VL_DEPS_POWERSHELL_EXECUTABLE}"
                -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/tool/deps-stamp.ps1"
                -BuildDir "${CMAKE_BINARY_DIR}"
                -Prefix "${VULKANLEARN_DEPS_PREFIX}"
                -Config "${CMAKE_BUILD_TYPE}"
                -Check -NoTouch
            COMMENT "Verifying prebuilt dependency stamp"
            VERBATIM)
    endif()

    # The prefix owns every shared library. Copy them next to the executable
    # (and the MinGW runtime trio) instead of relying on PATH.
    file(GLOB prebuilt_dlls "${VULKANLEARN_DEPS_PREFIX}/bin/*.dll")
    get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        if(EXISTS "${compiler_bin}/${runtime_dll}")
            list(APPEND prebuilt_dlls "${compiler_bin}/${runtime_dll}")
        endif()
    endforeach()

    set(copy_commands)
    foreach(dll IN LISTS prebuilt_dlls)
        list(APPEND copy_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${dll}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    endforeach()
    if(copy_commands)
        add_custom_target(vl_prebuilt_runtime_dlls ALL
            ${copy_commands}
            COMMENT "Deploying prebuilt dependency DLLs"
            VERBATIM)
    endif()

    # Ozz is not part of the six-dependency prefix, but prebuilt mode must
    # configure offline. Point FetchContent at the machine-wide source cache.
    set(ozz_source "${VULKANLEARN_DEPS_ROOT}/_sources/ozz")
    if(NOT FETCHCONTENT_SOURCE_DIR_VL_OZZ AND EXISTS "${ozz_source}/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_VL_OZZ "${ozz_source}" CACHE PATH
            "Machine-wide ozz-animation source cache." FORCE)
    endif()
    if(FETCHCONTENT_FULLY_DISCONNECTED AND
        NOT FETCHCONTENT_SOURCE_DIR_VL_OZZ)
        message(FATAL_ERROR
            "Prebuilt mode with FETCHCONTENT_FULLY_DISCONNECTED=ON needs the ozz source "
            "cache at ${ozz_source}.")
    endif()

    set(googletest_source "${VULKANLEARN_DEPS_ROOT}/_sources/googletest")
    if(NOT FETCHCONTENT_SOURCE_DIR_GOOGLETEST AND
        EXISTS "${googletest_source}/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${googletest_source}" CACHE PATH
            "Machine-wide googletest source cache." FORCE)
    endif()
    if(VULKANLEARN_BUILD_TESTS AND FETCHCONTENT_FULLY_DISCONNECTED AND
        NOT FETCHCONTENT_SOURCE_DIR_GOOGLETEST)
        message(FATAL_ERROR
            "Prebuilt mode builds tests but has no googletest source cache at "
            "${googletest_source}. Run tool/build-deps.ps1 -BuildTests.")
    endif()

    message(STATUS "Dependencies: prebuilt ${VULKANLEARN_DEPS_PREBUILT}")
    message(STATUS "Dependency prefix: ${VULKANLEARN_DEPS_PREFIX}")
endfunction()
