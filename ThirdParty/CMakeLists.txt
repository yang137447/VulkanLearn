cmake_minimum_required(VERSION 3.24)

# Machine-wide dependency superbuild. This project is configured separately
# from the engine (tool/build-deps.ps1) and installs the heavy third-party
# libraries once per toolchain/configuration.
project(vulkanlearn_deps LANGUAGES C CXX)

include(ExternalProject)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/VulkanLearnDeps.cmake)

option(VULKANLEARN_BUILD_ANIMATION_TOOLS "Match the engine animation-tools feature set." OFF)
option(VULKANLEARN_BUILD_TESTS "Match the engine test feature set." OFF)
option(VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS "Match the engine C# feature set." OFF)
option(VULKANLEARN_ENABLE_EDITOR "Match the engine editor feature set." OFF)

set(VULKANLEARN_DEPS_ROOT "" CACHE PATH
    "Machine-wide dependency root (<root>/<toolchain>/<config> is the prefix).")
set(VULKANLEARN_DEPS_CONFIG "" CACHE STRING
    "Configuration of the dependency prefix (defaults to CMAKE_BUILD_TYPE).")
set(VULKANLEARN_DEPS_BUILD_ROOT "" CACHE PATH
    "Per-dependency build trees.")
set(VULKANLEARN_DEPS_SOURCES "" CACHE PATH
    "Shared source checkouts for fetched dependencies.")
set(VULKANLEARN_DEPS_JOBS "4" CACHE STRING
    "Parallel build jobs passed to every dependency.")

if(NOT VULKANLEARN_DEPS_ROOT)
    message(FATAL_ERROR
        "VULKANLEARN_DEPS_ROOT is required. Run tool/build-deps.ps1 instead of configuring ThirdParty directly.")
endif()
file(TO_CMAKE_PATH "${VULKANLEARN_DEPS_ROOT}" VULKANLEARN_DEPS_ROOT)

if(NOT VULKANLEARN_DEPS_CONFIG)
    set(VULKANLEARN_DEPS_CONFIG "${CMAKE_BUILD_TYPE}")
endif()
if(NOT VULKANLEARN_DEPS_CONFIG)
    message(FATAL_ERROR "VULKANLEARN_DEPS_CONFIG or CMAKE_BUILD_TYPE is required.")
endif()
if(NOT VULKANLEARN_DEPS_CONFIG MATCHES "^(Debug|Release|RelWithDebInfo|MinSizeRel)$")
    message(FATAL_ERROR "Unsupported dependency configuration: ${VULKANLEARN_DEPS_CONFIG}")
endif()

get_filename_component(VL_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT VULKANLEARN_DEPS_BUILD_ROOT)
    set(VULKANLEARN_DEPS_BUILD_ROOT "${VULKANLEARN_DEPS_ROOT}/_build")
endif()
if(NOT VULKANLEARN_DEPS_SOURCES)
    set(VULKANLEARN_DEPS_SOURCES "${VULKANLEARN_DEPS_ROOT}/_sources")
endif()
file(TO_CMAKE_PATH "${VULKANLEARN_DEPS_BUILD_ROOT}" VULKANLEARN_DEPS_BUILD_ROOT)
file(TO_CMAKE_PATH "${VULKANLEARN_DEPS_SOURCES}" VULKANLEARN_DEPS_SOURCES)

vl_deps_resolve_prefix(
    "${VULKANLEARN_DEPS_ROOT}"
    "${VULKANLEARN_DEPS_CONFIG}"
    VL_DEPS_PREFIX)
set(VL_DEPS_BUILD_ROOT
    "${VULKANLEARN_DEPS_BUILD_ROOT}/${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}/${VULKANLEARN_DEPS_CONFIG}")

message(STATUS "Dependency prefix: ${VL_DEPS_PREFIX}")
message(STATUS "Dependency build root: ${VL_DEPS_BUILD_ROOT}")
message(STATUS "Dependency source root: ${VULKANLEARN_DEPS_SOURCES}")

file(MAKE_DIRECTORY "${VL_DEPS_PREFIX}" "${VL_DEPS_BUILD_ROOT}")

find_program(VL_DEPS_CCACHE_EXECUTABLE NAMES ccache)
set(VL_DEPS_COMMON_ARGS
    -DCMAKE_BUILD_TYPE=${VULKANLEARN_DEPS_CONFIG}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_PREFIX_PATH=${VL_DEPS_PREFIX}
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
    -DCMAKE_INSTALL_PREFIX=${VL_DEPS_PREFIX})
if(VL_DEPS_CCACHE_EXECUTABLE)
    list(APPEND VL_DEPS_COMMON_ARGS
        -DCMAKE_C_COMPILER_LAUNCHER=${VL_DEPS_CCACHE_EXECUTABLE}
        -DCMAKE_CXX_COMPILER_LAUNCHER=${VL_DEPS_CCACHE_EXECUTABLE})
endif()

function(vl_deps_add name)
    set(options "")
    set(one_value_args SOURCE_DIR BINARY_DIR SOURCE_SUBDIR)
    set(multi_value_args CMAKE_ARGS DEPENDS)
    cmake_parse_arguments(VL_DEP "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT VL_DEP_SOURCE_DIR)
        message(FATAL_ERROR "vl_deps_add(${name}): SOURCE_DIR is required.")
    endif()
    if(NOT EXISTS "${VL_DEP_SOURCE_DIR}/CMakeLists.txt" AND
        NOT VL_DEP_SOURCE_SUBDIR)
        message(FATAL_ERROR
            "vl_deps_add(${name}): source tree is missing at ${VL_DEP_SOURCE_DIR}. "
            "Run tool/build-deps.ps1 to populate the source cache.")
    endif()
    if(NOT VL_DEP_BINARY_DIR)
        set(VL_DEP_BINARY_DIR "${VL_DEPS_BUILD_ROOT}/${name}")
    endif()

    set(source_subdir_args "")
    if(VL_DEP_SOURCE_SUBDIR)
        set(source_subdir_args SOURCE_SUBDIR "${VL_DEP_SOURCE_SUBDIR}")
    endif()

    set(depends_args "")
    if(VL_DEP_DEPENDS)
        set(depends_args DEPENDS ${VL_DEP_DEPENDS})
    endif()

    ExternalProject_Add(${name}
        SOURCE_DIR "${VL_DEP_SOURCE_DIR}"
        BINARY_DIR "${VL_DEP_BINARY_DIR}"
        INSTALL_DIR "${VL_DEPS_PREFIX}"
        ${source_subdir_args}
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        PATCH_COMMAND ""
        CMAKE_ARGS ${VL_DEPS_COMMON_ARGS} ${VL_DEP_CMAKE_ARGS}
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
            --parallel "${VULKANLEARN_DEPS_JOBS}"
        INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
        BUILD_ALWAYS OFF
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
        USES_TERMINAL_INSTALL TRUE
        ${depends_args})
endfunction()

# ---------------------------------------------------------------- SDL3
vl_deps_add(vl_dep_sdl3
    SOURCE_DIR "${VL_REPO_ROOT}/extern/SDL3/SDL3"
    CMAKE_ARGS
        -DSDL_INSTALL=ON
        -DSDL_INSTALL_TESTS=OFF
        -DSDL_TESTS=OFF
        -DSDL_TEST_LIBRARY=OFF
        -DSDL_SHARED=ON
        -DSDL_STATIC=OFF
        -DSDL_CCACHE=OFF)

# ------------------------------------------------------------ FreeType
vl_deps_add(vl_dep_freetype
    SOURCE_DIR "${VL_REPO_ROOT}/extern/FreeType/freetype"
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=ON
        -DFT_DISABLE_BZIP2=ON
        -DFT_DISABLE_BROTLI=ON
        -DFT_DISABLE_HARFBUZZ=ON
        -DFT_DISABLE_PNG=ON
        -DFT_DISABLE_ZLIB=ON)

# -------------------------------------------------------------- Assimp
set(VL_DEPS_ASSIMP_ARGS
    -DASSIMP_BUILD_TESTS=OFF
    -DASSIMP_BUILD_SAMPLES=OFF
    -DASSIMP_BUILD_ASSIMP_TOOLS=OFF
    -DASSIMP_BUILD_USE_CCACHE=OFF
    -DASSIMP_WARNINGS_AS_ERRORS=OFF
    -DASSIMP_INSTALL=ON
    -DASSIMP_BUILD_ZLIB=ON
    -DASSIMP_BUILD_DRACO=OFF
    -DBUILD_SHARED_LIBS=ON)
if(VULKANLEARN_BUILD_ANIMATION_TOOLS)
    list(APPEND VL_DEPS_ASSIMP_ARGS
        -DASSIMP_NO_EXPORT=OFF
        -DASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT=OFF
        -DASSIMP_BUILD_FBX_EXPORTER=ON)
else()
    list(APPEND VL_DEPS_ASSIMP_ARGS -DASSIMP_NO_EXPORT=ON)
endif()

vl_deps_add(vl_dep_assimp
    SOURCE_DIR "${VL_REPO_ROOT}/extern/Assimp/Assimp"
    CMAKE_ARGS ${VL_DEPS_ASSIMP_ARGS})

# -------------------------------------------------------------- RmlUi
vl_deps_add(vl_dep_rmlui
    SOURCE_DIR "${VL_REPO_ROOT}/extern/RmlUi/RmlUi"
    DEPENDS vl_dep_freetype
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=ON
        -DBUILD_TESTING=OFF
        -DRMLUI_SAMPLES=OFF
        -DRMLUI_LUA_BINDINGS=OFF
        -DRMLUI_LOTTIE_PLUGIN=OFF
        -DRMLUI_SVG_PLUGIN=OFF
        -DRMLUI_HARFBUZZ_SAMPLE=OFF
        -DRMLUI_PRECOMPILED_HEADERS=OFF
        -DRMLUI_WARNINGS_AS_ERRORS=OFF
        # DLLs are deployed by the consumer; resolving them at install time
        # needs MinGW on PATH and fails on a clean machine.
        -DRMLUI_INSTALL_RUNTIME_DEPENDENCIES=OFF
        -DRMLUI_FONT_ENGINE=freetype)

# ------------------------------------------------------------- OpenEXR
set(VL_DEPS_OPENEXR_ARGS
    -DBUILD_SHARED_LIBS=ON
    -DBUILD_TESTING=OFF
    -DOPENEXR_BUILD_TOOLS=OFF
    -DOPENEXR_INSTALL_TOOLS=OFF
    -DOPENEXR_INSTALL_DEVELOPER_TOOLS=OFF
    -DOPENEXR_BUILD_EXAMPLES=OFF
    -DOPENEXR_BUILD_PYTHON=OFF
    -DOPENEXR_FORCE_INTERNAL_IMATH=ON
    -DOPENEXR_IMATH_TAG=v3.2.2
    -DOPENEXR_FORCE_INTERNAL_DEFLATE=ON
    -DOPENEXR_FORCE_INTERNAL_OPENJPH=ON
    -DOPENEXR_USE_TBB=OFF)
if(EXISTS "${VULKANLEARN_DEPS_SOURCES}/imath/CMakeLists.txt")
    list(APPEND VL_DEPS_OPENEXR_ARGS
        -DFETCHCONTENT_SOURCE_DIR_IMATH=${VULKANLEARN_DEPS_SOURCES}/imath)
endif()

vl_deps_add(vl_dep_openexr
    SOURCE_DIR "${VL_REPO_ROOT}/ThirdParty/wrappers/openexr"
    CMAKE_ARGS
        -DVL_OPENEXR_SOURCE_DIR=${VULKANLEARN_DEPS_SOURCES}/openexr
        ${VL_DEPS_OPENEXR_ARGS})

# --------------------------------------------------------------- Jolt
set(VL_DEPS_JOLT_ARGS
    -DENABLE_INSTALL=ON
    -DJPH_BUILD_SHARED_LIBS=OFF
    -DCPP_RTTI_ENABLED=ON
    -DCPP_EXCEPTIONS_ENABLED=ON
    -DOVERRIDE_CXX_FLAGS=OFF
    -DINTERPROCEDURAL_OPTIMIZATION=OFF
    -DENABLE_ALL_WARNINGS=OFF
    -DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=OFF
    -DPROFILER_IN_DEBUG_AND_RELEASE=OFF
    -DENABLE_OBJECT_STREAM=OFF
    -DTARGET_UNIT_TESTS=OFF
    -DTARGET_HELLO_WORLD=OFF
    -DTARGET_PERFORMANCE_TEST=OFF
    -DTARGET_SAMPLES=OFF
    -DTARGET_VIEWER=OFF)
foreach(feature SSE4_1 SSE4_2 AVX AVX2 AVX512 LZCNT TZCNT F16C FMADD)
    list(APPEND VL_DEPS_JOLT_ARGS -DUSE_${feature}=OFF)
endforeach()
foreach(backend DX12 VK MTL CPU_COMPUTE)
    list(APPEND VL_DEPS_JOLT_ARGS -DJPH_USE_${backend}=OFF)
endforeach()

vl_deps_add(vl_dep_jolt
    SOURCE_DIR "${VULKANLEARN_DEPS_SOURCES}/jolt"
    SOURCE_SUBDIR Build
    CMAKE_ARGS ${VL_DEPS_JOLT_ARGS})

# --------------------------------------------------- vl_deps package
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/vl_depsConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/vl_depsConfig.cmake"
    COPYONLY)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/vl_depsConfigVersion.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/vl_depsConfigVersion.cmake"
    COPYONLY)

add_custom_target(vl_deps_package ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${VL_DEPS_PREFIX}/share/vl_deps"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/vl_depsConfig.cmake"
        "${VL_DEPS_PREFIX}/share/vl_deps/vl_depsConfig.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/vl_depsConfigVersion.cmake"
        "${VL_DEPS_PREFIX}/share/vl_deps/vl_depsConfigVersion.cmake"
    DEPENDS vl_dep_sdl3 vl_dep_freetype vl_dep_assimp vl_dep_rmlui
        vl_dep_openexr vl_dep_jolt
    COMMENT "Writing vl_deps package config"
    VERBATIM)
