add_library(blake3 STATIC
    blake3.c
    blake3_dispatch.c
    blake3_portable.c
)

add_library(BLAKE3::blake3 ALIAS blake3)

target_include_directories(blake3
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# VulkanLearn's first MinGW integration deliberately uses the official
# portable C path. SIMD/assembly variants can be enabled separately after
# profiling proves they matter for shader-sized inputs.
target_compile_definitions(blake3
    PRIVATE
        BLAKE3_NO_SSE2
        BLAKE3_NO_SSE41
        BLAKE3_NO_AVX2
        BLAKE3_NO_AVX512
)
