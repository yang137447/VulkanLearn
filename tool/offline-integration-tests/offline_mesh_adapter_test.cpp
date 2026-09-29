#include <gtest/gtest.h>

#include "render/offline/offlineMeshAdapter.h"

namespace
{
MeshSection TriangleSection()
{
    MeshSection section;
    section.sectionName = "fabric";
    section.vertices.emplace_back(Eigen::Vector3f(0, 0, 0),
                                  Eigen::Vector3f(0, 0, 1),
                                  Eigen::Vector4f::Ones(), Eigen::Vector2f(0, 0));
    section.vertices.emplace_back(Eigen::Vector3f(1, 0, 0),
                                  Eigen::Vector3f(0, 0, 1),
                                  Eigen::Vector4f::Ones(), Eigen::Vector2f(1, 0));
    section.vertices.emplace_back(Eigen::Vector3f(0, 1, 0),
                                  Eigen::Vector3f(0, 0, 1),
                                  Eigen::Vector4f::Ones(), Eigen::Vector2f(0, 1));
    section.indices = {0, 1, 2};
    return section;
}

TEST(OfflineMeshAdapter, SectionUvAndNormalSurviveImport)
{
    const auto mesh = VL::BuildOfflineMesh(TriangleSection());
    ASSERT_EQ(mesh->triangles.size(), 1u);
    const auto& tri = mesh->triangles.front();
    EXPECT_DOUBLE_EQ(tri.dpdu.x, 1);
    EXPECT_DOUBLE_EQ(tri.dpdv.y, 1);
    EXPECT_DOUBLE_EQ(tri.uv[1].x, 1);
    EXPECT_DOUBLE_EQ(tri.normals[0].z, 1);
    EXPECT_FALSE(mesh->nodes.empty());
}

TEST(OfflineMeshAdapter, MirroredAndTranslatedInstancesShareMesh)
{
    const auto mesh = VL::BuildOfflineMesh(TriangleSection());
    Eigen::Matrix4f mirrored = Eigen::Matrix4f::Identity();
    mirrored(0, 0) = -2;
    mirrored(0, 3) = 3;
    Eigen::Matrix4f normal = Eigen::Matrix4f::Identity();
    normal(0, 3) = -3;

    VL::Offline::Scene scene;
    scene.instances.push_back(VL::BuildOfflineInstance(
        *mesh, mirrored, VL::Offline::SurfaceModel::Lambert));
    scene.instances.push_back(VL::BuildOfflineInstance(
        *mesh, normal, VL::Offline::SurfaceModel::ClothSheen));
    ASSERT_EQ(scene.instances[0].mesh, scene.instances[1].mesh);

    VL::Offline::Hit hit;
    ASSERT_TRUE(VL::Offline::intersect(
        scene, {{2.5, 0.25, 2}, {0, 0, -1}}, 100, hit));
    ASSERT_EQ(hit.instance, &scene.instances[0]);
    EXPECT_NEAR(hit.position.x, 2.5, 1e-6);
    EXPECT_GT(hit.normal.z, 0);
    EXPECT_GT(hit.geometricNormal.z, 0);
    EXPECT_NEAR(hit.uv.x, 0.25, 1e-6);
    EXPECT_NEAR(hit.dpdu.x, -2, 1e-6);
    ASSERT_TRUE(VL::Offline::intersect(
        scene, {{-2.75, 0.25, 2}, {0, 0, -1}}, 100, hit));
    EXPECT_EQ(hit.instance, &scene.instances[1]);
}

TEST(OfflineMeshAdapter, EmptyAuthoredSceneDoesNotRenderLegacySpheres)
{
    VL::Offline::Scene scene;
    VL::Offline::Hit hit;
    EXPECT_FALSE(VL::Offline::intersect(
        scene, {{0, 0, 4}, {0, 0, -1}}, 100, hit));
}

TEST(OfflineMeshAdapter, AuthoredLambertUsesSectionColorWithoutLegacyLighting)
{
    const auto mesh = VL::BuildOfflineMesh(TriangleSection());
    VL::Offline::Scene scene;
    scene.backdrop = {};
    scene.instances.push_back(VL::BuildOfflineInstance(
        *mesh, Eigen::Matrix4f::Identity(), VL::Offline::SurfaceModel::Lambert));
    scene.instances.front().baseColor = {0.2, 0.4, 0.6};
    const VL::Offline::Ray ray{{0.25, 0.25, 1}, {0, 0, -1}};
    VL::Offline::Random random{7};
    const VL::Offline::AlbedoTable albedo;
    auto unlit = VL::Offline::trace(scene, albedo, ray, 0, 1, random);
    EXPECT_DOUBLE_EQ(unlit.direct.x, 0);
    scene.directionalLight = true;
    scene.toLight = {0, 0, 1};
    scene.directionalIrradiance = VL::Offline::kPi;
    const auto lit = VL::Offline::trace(scene, albedo, ray, 0, 1, random);
    EXPECT_NEAR(lit.direct.x, 0.2, 1e-6);
    EXPECT_NEAR(lit.direct.y, 0.4, 1e-6);
    EXPECT_NEAR(lit.direct.z, 0.6, 1e-6);
}

TEST(OfflineMeshAdapter, RejectsInvalidTopologyAndSingularTransform)
{
    MeshSection section = TriangleSection();
    section.indices[2] = 3;
    EXPECT_THROW(VL::BuildOfflineMesh(section), std::invalid_argument);
    section = TriangleSection();
    const auto mesh = VL::BuildOfflineMesh(section);
    Eigen::Matrix4f singular = Eigen::Matrix4f::Identity();
    singular(1, 1) = 0;
    EXPECT_THROW(VL::BuildOfflineInstance(
        *mesh, singular, VL::Offline::SurfaceModel::Lambert), std::invalid_argument);
}

TEST(OfflineMeshAdapter, DegenerateUvFrameIsMarkedForFiberPreflight)
{
    MeshSection section = TriangleSection();
    section.vertices[2].texCoord = section.vertices[1].texCoord;
    const auto mesh = VL::BuildOfflineMesh(section);
    EXPECT_FALSE(mesh->hasValidUvFrames);
}

TEST(OfflineMeshAdapter, SupersededPrepareDoesNotPublishBvh)
{
    std::atomic<std::uint64_t> latest{3};
    EXPECT_THROW(VL::BuildOfflineMesh(TriangleSection(), &latest, 2),
        std::runtime_error);
    const auto mesh = VL::BuildOfflineMesh(TriangleSection(), &latest, 3);
    EXPECT_FALSE(mesh->nodes.empty());
}
} // namespace
