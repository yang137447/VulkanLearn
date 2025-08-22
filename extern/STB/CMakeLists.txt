set(STB_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT TARGET stb_image)
    if(WIN32)
        add_library(stb_image INTERFACE)
        set_target_properties(stb_image
            PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${STB_DIR}"
        )
    elseif()
        find_package(stb_image REQUIRED)
    endif()
endif()