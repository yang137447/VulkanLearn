set(Spirv_Reflect_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT TARGET Spirv_Reflect)
    if(WIN32)
        add_library(Spirv_Reflect STATIC IMPORTED GLOBAL)
        target_include_directories(Spirv_Reflect INTERFACE "${Spirv_Reflect_DIR}/include")
        set_property(TARGET Spirv_Reflect PROPERTY 
            IMPORTED_LOCATION "${Spirv_Reflect_DIR}/lib/libspirv-reflect-static.a")
    endif()
endif()