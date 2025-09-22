set(SHADERC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT TARGET Vulkan::shaderc_combined)
    if(WIN32)
        add_library(Vulkan::shaderc_combined STATIC IMPORTED GLOBAL)
        target_include_directories(Vulkan::shaderc_combined INTERFACE "${SHADERC_DIR}/include")
        set_property(TARGET Vulkan::shaderc_combined PROPERTY 
            IMPORTED_LOCATION "${SHADERC_DIR}/lib/libshaderc_shared.dll.a")
        
        # 定义 DLL 文件路径
        set(SHADERC_DLL "${SHADERC_DIR}/bin/libshaderc_shared.dll" CACHE PATH "Path to shaderc_shared.dll")
        
        # 在构建后复制 DLL 文件到输出目录 CMAKE_RUNTIME_OUTPUT_DIRECTORY
        if(DEFINED SHADERC_DLL AND EXISTS "${SHADERC_DLL}")
            add_custom_target(copy_shaderc_dll ALL
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SHADERC_DLL}"
                    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
        endif()
        
    else()
        find_package(Vulkan REQUIRED COMPONENTS shaderc)
    endif()
endif()
