add_executable(audio_runtime_tests
    audioClipTests.cpp
    audioMixerTests.cpp
    "${PROJECT_SOURCE_DIR}/source/audio/audioClip.cpp"
    "${PROJECT_SOURCE_DIR}/source/audio/audioMixer.cpp")
target_include_directories(audio_runtime_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(audio_runtime_tests PRIVATE Eigen3)
vulkanlearn_add_gtest(audio_runtime_tests)

# commonFunction.h 的路径/配置读取经 TracingIfstream/RuntimeFileTrace 记账，需要两者定义。
target_sources(audio_runtime_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/source/core/runtimeFileTrace.cpp"
    "${PROJECT_SOURCE_DIR}/source/core/tracingIfstream.cpp")
