set(Json_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT TARGET Json)
    if(WIN32)
        add_library(Json INTERFACE)
        set_target_properties(Json
            PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${Json_DIR}/include"
        )
    elseif()
        find_package(Json REQUIRED)
    endif()
endif()