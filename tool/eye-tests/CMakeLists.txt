add_executable(eye_tests
    eyeTests.cpp
    ${PROJECT_SOURCE_DIR}/source/render/eye/eyeAssets.cpp
    ${PROJECT_SOURCE_DIR}/source/render/eye/eyeGBufferCodec.cpp
    ${PROJECT_SOURCE_DIR}/source/render/eye/eyeLodContract.cpp
    ${PROJECT_SOURCE_DIR}/source/render/eye/eyePerformanceBudget.cpp
    ${PROJECT_SOURCE_DIR}/source/render/eye/eyeResourceSet.cpp
)

target_include_directories(eye_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/source
)

target_compile_definitions(eye_tests
    PRIVATE
        VULKANLEARN_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(eye_tests
    PRIVATE
        Json
)

add_test(NAME eye_contract COMMAND eye_tests)
