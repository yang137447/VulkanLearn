add_library(vl_hair_assets STATIC tfxLoader.cpp tfxBoneLoader.cpp strandGeometry.cpp
    tressfxDynamics.cpp sdfBuilder.cpp
    collisionProxy.cpp collisionMeshObj.cpp tressfxSdfGrid.cpp tressfxProxySkinning.cpp
    tressfxRootWeights.cpp
    hairTraceFormat.cpp tressfxRootCloud.cpp tressfxSurfaceBrush.cpp
    tressfxGroupPaint.cpp tressfxGroupBinding.cpp
    tressfxFollowBinding.cpp
    rod/rodMath.cpp rod/rodEnergy.cpp rod/rodRootSupport.cpp
    rod/rodXpbd.cpp rod/rodCollision.cpp
    rod/rodImplicit.cpp rod/rodAnalytic.cpp rod/rodProgressiveSupport.cpp
    rod/rodContactImplicit.cpp rod/rodSectionInertia.cpp rod/rodWorkCertificate.cpp
    rod/rodAirDrag.cpp
    rod/rodAxialLimit.cpp rod/rodAxialKkt.cpp rod/rodAxialStep.cpp
    rod/rodSmoothSdf.cpp)
target_include_directories(vl_hair_assets PUBLIC "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(vl_hair_assets PRIVATE Eigen3 Threads::Threads)
