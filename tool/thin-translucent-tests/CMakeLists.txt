add_executable(thin_translucent_tests
    thinTranslucentTests.cpp
)

target_include_directories(thin_translucent_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

add_test(NAME thin_translucent COMMAND thin_translucent_tests)
