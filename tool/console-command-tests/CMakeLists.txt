add_executable(console_command_tests
    consoleCommandTests.cpp
)

target_include_directories(console_command_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(console_command_tests
    PRIVATE
        Json
)

vulkanlearn_add_gtest(console_command_tests)
