set(Spirv_Reflect_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT TARGET Spirv_Reflect)
    if(WIN32)
        add_library(Spirv_Reflect INTERFACE)
        set_target_properties(Spirv_Reflect
            PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Spirv_Reflect_DIR}/include"
        )
    elseif()
        find_package(Spirv_Reflect REQUIRED)
    endif()
endif()