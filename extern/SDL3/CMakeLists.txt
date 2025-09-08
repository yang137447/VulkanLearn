include(fetchcontent)      # 照写，不需要修改
fetchcontent_declare(	
    SDL3	#库名字
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git	# 仓库地址
    GIT_TAG release-3.2.22 # 库版本
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/SDL3 # 指定库下载地址
    )
fetchcontent_makeavailable(SDL3)

#add_subdirectory(SDL3)