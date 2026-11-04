include_guard(GLOBAL)

# Jolt 只保留一份 CPU 构建；C# Blueprint 与原生软体冲击共享同一个 target。
function(vl_add_jolt_cpu)
    if(TARGET Jolt)
        return()
    endif()
    include(FetchContent)
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
