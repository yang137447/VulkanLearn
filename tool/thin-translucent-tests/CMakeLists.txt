add_executable(thin_translucent_tests
    thinTranslucentTests.cpp
)

target_include_directories(thin_translucent_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

vulkanlearn_add_gtest(thin_translucent_tests)
