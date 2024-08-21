set(SHADERC_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT TARGET Shaderc)
    if(WIN32)
        add_library(Shaderc SHARED IMPORTED GLOBAL)
        set_target_properties(Shaderc
            PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${SHADERC_DIR}/include"
                IMPORTED_LOCATION "${SHADERC_DIR}/bin/libshaderc_shared.dll"
                IMPORTED_IMPLIB "${SHADERC_DIR}/lib/libshaderc_shared.dll.a")
    elseif()
        find_package(Shaderc REQUIRED)
    endif()
endif()