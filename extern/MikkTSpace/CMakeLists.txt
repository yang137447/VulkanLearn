set(MikkTSpace_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT TARGET MikkTSpace)
    add_library(MikkTSpace STATIC
        "${MikkTSpace_DIR}/mikktspace.cpp"
    )
    target_include_directories(MikkTSpace PUBLIC "${MikkTSpace_DIR}")
endif()
