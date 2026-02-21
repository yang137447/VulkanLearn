#pragma once

// Profiler Configuration
// TRACY_ENABLE: Enable Tracy Profiler
// NVTX_ENABLE: Enable NVIDIA Nsight Systems (NVTX)
// These should be defined in CMakeLists.txt or project settings

#if defined(TRACY_ENABLE)
#include "tracy/Tracy.hpp"
#endif

#if defined(NVTX_ENABLE)
#include "nvtx3/nvToolsExt.h"
#endif

#define PROFILE_CONCAT_INTERNAL(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_INTERNAL(a, b)

// ------------------------------------------------------------------------------------------------
// NVTX Implementation
// ------------------------------------------------------------------------------------------------
#if defined(NVTX_ENABLE)
class NvtxScopedRange {
public:
    NvtxScopedRange(const char* name) {
        nvtxRangePushA(name);
    }
    ~NvtxScopedRange() {
        nvtxRangePop();
    }
};
#define PROFILE_NVTX_SCOPE(name) NvtxScopedRange PROFILE_CONCAT(nvtx_scope_, __LINE__)(name)
#else
#define PROFILE_NVTX_SCOPE(name)
#endif

// ------------------------------------------------------------------------------------------------
// Tracy Implementation
// ------------------------------------------------------------------------------------------------
#if defined(TRACY_ENABLE)
#define PROFILE_TRACY_SCOPE(name) \
    ZoneTransient(PROFILE_CONCAT(tracy_zone_, __LINE__), true); \
    ZoneNameV(PROFILE_CONCAT(tracy_zone_, __LINE__), name, strlen(name))

#define PROFILE_TRACY_FUNCTION() ZoneScoped

#define PROFILE_TRACY_FRAME() FrameMark
#else
#define PROFILE_TRACY_SCOPE(name)
#define PROFILE_TRACY_FUNCTION()
#define PROFILE_TRACY_FRAME()
#endif

// ------------------------------------------------------------------------------------------------
// Unified Profiling Macros
// ------------------------------------------------------------------------------------------------

// Scope-based profiling (blocks, loops, specific sections)
// Usage: { PROFILE_SCOPE("MySection"); ... }
#define PROFILE_SCOPE(name) \
    PROFILE_TRACY_SCOPE(name); \
    PROFILE_NVTX_SCOPE(name)

// Function-level profiling
// Usage: void MyFunc() { PROFILE_FUNCTION(); ... }
#define PROFILE_FUNCTION() \
    PROFILE_TRACY_FUNCTION(); \
    PROFILE_NVTX_SCOPE(__FUNCTION__)

// Frame marking (usually at end of frame)
#define PROFILE_FRAME() PROFILE_TRACY_FRAME()

// Legacy compatibility
class Profiler {
public:
    static Profiler& Instance() {
        static Profiler instance;
        return instance;
    }
    void BeginSession(const std::string& name, const std::string& filepath = "results.json") {
    }
    void EndSession() {}
};
