file(GLOB sources "*.h" "*.cpp")

add_executable(main ${sources})

target_link_libraries(main 
PRIVATE
    Vulkan
    SDL3::SDL3
    Shaderc
    Eigen3)