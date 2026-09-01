add_executable(cloth_tests
    clothTests.cpp
)

target_include_directories(cloth_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(cloth_tests
    PRIVATE
        Json
)

add_test(NAME cloth_contract COMMAND cloth_tests)
