set(Eigen3_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT TARGET Eigen3)
    if(WIN32)
        add_library(Eigen3 INTERFACE IMPORTED)
        set_target_properties(Eigen3
            PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}"
        )
    elseif()
        find_package(Eigen3 REQUIRED)
    endif()
endif()