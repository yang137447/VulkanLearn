set(SDL3_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT TARGET SDL3)
    if(WIN32)
        add_library(SDL3 SHARED IMPORTED GLOBAL)
        set_target_properties(SDL3
            PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${SDL3_DIR}/include"
                IMPORTED_LOCATION "${SDL3_DIR}/bin/SDL3.dll"
                IMPORTED_IMPLIB "${SDL3_DIR}/lib/libSDL3.lib")
    else()
        find_package(SDL3 REQUIRED)
    endif()
endif()