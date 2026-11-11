include_guard(GLOBAL)

# Optional machine-wide source cache produced by tool/build-deps.ps1.
# Source mode can then configure offline with FETCHCONTENT_FULLY_DISCONNECTED=ON.
set(VULKANLEARN_DEPS_SOURCE_CACHE "" CACHE PATH
    "Shared FetchContent source cache (for example <deps-root>/_sources).")

if(NOT VULKANLEARN_DEPS_SOURCE_CACHE)
    return()
endif()

file(TO_CMAKE_PATH "${VULKANLEARN_DEPS_SOURCE_CACHE}" VULKANLEARN_DEPS_SOURCE_CACHE)
foreach(entry
    "OPENEXR:openexr"
    "IMATH:imath"
    "VL_JOLT:jolt"
    "VL_OZZ:ozz"
    "GOOGLETEST:googletest")
    string(REPLACE ":" ";" pair "${entry}")
    list(GET pair 0 fetch_name)
    list(GET pair 1 directory)
    if(EXISTS "${VULKANLEARN_DEPS_SOURCE_CACHE}/${directory}/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_${fetch_name}
            "${VULKANLEARN_DEPS_SOURCE_CACHE}/${directory}"
            CACHE PATH "" FORCE)
    endif()
endforeach()
