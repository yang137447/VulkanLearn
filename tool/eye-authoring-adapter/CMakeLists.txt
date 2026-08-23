add_executable(eye_authoring_adapter
    main.cpp
    eyeAuthoringAdapter.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp
)

target_include_directories(eye_authoring_adapter
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(eye_authoring_adapter
    PRIVATE
        Json
        BLAKE3::blake3
)

add_executable(eye_authoring_adapter_tests
    eyeAuthoringAdapterTests.cpp
    eyeAuthoringAdapter.cpp
    ${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp
)

target_include_directories(eye_authoring_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/source
)

target_link_libraries(eye_authoring_adapter_tests
    PRIVATE
        Json
        BLAKE3::blake3
)

add_test(NAME eye_authoring_adapter_contract COMMAND eye_authoring_adapter_tests)
