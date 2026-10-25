add_library(vl_hair_assets STATIC tfxLoader.cpp tfxBoneLoader.cpp strandGeometry.cpp
    tressfxDynamics.cpp sdfBuilder.cpp
    collisionProxy.cpp collisionMeshObj.cpp tressfxSdfGrid.cpp tressfxProxySkinning.cpp
    tressfxRootWeights.cpp
    hairTraceFormat.cpp)
target_include_directories(vl_hair_assets PUBLIC "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(vl_hair_assets PRIVATE Eigen3 Threads::Threads)
