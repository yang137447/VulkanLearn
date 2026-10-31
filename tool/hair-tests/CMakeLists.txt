add_executable(hair_asset_tests
    tfxLoaderTests.cpp tfxBoneLoaderTests.cpp strandGeometryTests.cpp tressfxImportTests.cpp tressfxDynamicsTests.cpp
    tressfxSolverMathTests.cpp
    tressfxSourceScheduleTests.cpp
    tressfxSkinningTests.cpp
    tressfxSimulationHistoryTests.cpp
    tressfxFrameContextTests.cpp
    tressfxWorldHistoryTests.cpp
    sdfBuilderTests.cpp
    collisionProxyTests.cpp
    collisionSdfTests.cpp
    hairTraceTests.cpp
    hairConsoleCommandTests.cpp
    tressfxLiveParamsTests.cpp
    tressfxComponentBindingTests.cpp
    tressfxRootWeightsTests.cpp
    tressfxGroupGpuDataTests.cpp
    tressfxGroupSimulationTests.cpp
    tressfxGroupBindingTests.cpp
    tressfxGeometryCacheTests.cpp
    tressfxSurfaceBrushTests.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/loader/hair/tressfxModelImporter.cpp
    ${PROJECT_SOURCE_DIR}/source/mesh/validation/meshAssetValidator.cpp
    ${PROJECT_SOURCE_DIR}/source/hair/tressfxGroupSimulation.cpp
    ${PROJECT_SOURCE_DIR}/source/render/resource/tressfxGeometryCache.cpp
    ${PROJECT_SOURCE_DIR}/source/core/runtimeResult.cpp)
target_link_libraries(hair_asset_tests PRIVATE vl_hair_assets Vulkan::Vulkan Eigen3 Json)
# MinGW 的 COFF 对象默认最多 32767 个 section；Debug 下 Eigen/JSON 重模板的单个
# 测试 TU 会超过该上限并以 “too many sections” 拒绝汇编。big-obj 只放宽对象
# 文件格式，不改变代码或断言；其他编译器没有这个上限，不添加该参数。
if(MINGW)
    target_compile_options(hair_asset_tests PRIVATE -Wa,-mbig-obj)
endif()
vulkanlearn_add_gtest(hair_asset_tests)
