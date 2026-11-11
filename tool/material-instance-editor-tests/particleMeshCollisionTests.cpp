#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "particles/particleAsset.h"
#include "particles/particleCollision.h"
#include "particles/particleMeshCollision.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;

Json CubeMeshDocument()
{
    return Json{
        {"type", "particleCollisionMesh"},
        {"vertices", Json::array({
            Json::array({-10.0, -10.0, -10.0}),
            Json::array({10.0, -10.0, -10.0}),
            Json::array({10.0, 10.0, -10.0}),
            Json::array({-10.0, 10.0, -10.0}),
            Json::array({-10.0, -10.0, 10.0}),
            Json::array({10.0, -10.0, 10.0}),
            Json::array({10.0, 10.0, 10.0}),
            Json::array({-10.0, 10.0, 10.0})})},
        {"indices", Json::array({
            0, 3, 2, 0, 2, 1,
            4, 5, 6, 4, 6, 7,
            0, 1, 5, 0, 5, 4,
            3, 7, 6, 3, 6, 2,
            0, 4, 7, 0, 7, 3,
            1, 2, 6, 1, 6, 5})},
        {"resolution", Json::array({24, 24, 24})},
        {"padding", 5.0}};
}

Json TetrahedronMeshDocument()
{
    return Json{
        {"type", "particleCollisionMesh"},
        {"vertices", Json::array({
            Json::array({0.0, 0.0, 0.0}),
            Json::array({10.0, 0.0, 0.0}),
            Json::array({0.0, 10.0, 0.0}),
            Json::array({0.0, 0.0, 10.0})})},
        {"indices", Json::array({
            0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3})},
        {"resolution", Json::array({16, 16, 16})},
        {"padding", 5.0}};
}

ParticleEmitterAsset MeshEmitter(
    const std::shared_ptr<const ParticleSdfAsset>& sdf)
{
    ParticleEmitterAsset emitter;
    emitter.collisionType = "mesh";
    emitter.collisionMeshPath = "Meshes/PS_collision_cube.json";
    emitter.collisionSdf = sdf;
    emitter.collisionBounce = 0.5f;
    emitter.collisionFriction = 0.0f;
    return emitter;
}

class ParticleMeshCollisionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        root = std::filesystem::temp_directory_path() /
            ("vl-particle-collision-mesh-" + std::to_string(stamp));
        std::filesystem::create_directories(root / "Meshes");
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void WriteJson(
        const std::filesystem::path& path,
        const Json& document)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file << document.dump(2);
    }

    std::filesystem::path root;
};

} // namespace

TEST(ParticleCollisionMeshParser, ParsesCubeAndAppliesDefaults)
{
    const ParticleCollisionMeshAsset mesh =
        ParseParticleCollisionMeshJson(CubeMeshDocument());
    EXPECT_EQ(mesh.vertices.size(), 8u);
    EXPECT_EQ(mesh.indices.size(), 36u);
    EXPECT_EQ(mesh.resolution, Eigen::Vector3i(24, 24, 24));
    EXPECT_FLOAT_EQ(mesh.padding, 5.0f);

    Json defaults = CubeMeshDocument();
    defaults.erase("resolution");
    defaults.erase("padding");
    const ParticleCollisionMeshAsset defaulted =
        ParseParticleCollisionMeshJson(defaults);
    EXPECT_EQ(defaulted.resolution, Eigen::Vector3i(24, 24, 24));
    EXPECT_FLOAT_EQ(defaulted.padding, 5.0f);
}

TEST(ParticleCollisionMeshParser, RejectsMalformedVerticesAndIndices)
{
    {
        Json document = CubeMeshDocument();
        document["type"] = "particleSdf";
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["type"] = Json::array({"particleCollisionMesh"});
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document.erase("vertices");
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["vertices"] = Json::array();
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["vertices"][0] = Json::array({1.0, 2.0});
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["vertices"][0][0] =
            std::numeric_limits<double>::quiet_NaN();
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document.erase("indices");
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["indices"] = Json::array({0, 1, 2, 0});
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["indices"][0] = 1.5;
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["indices"][0] = -1;
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["indices"][0] = 8;
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
}

TEST(ParticleCollisionMeshParser, RejectsResolutionAndPaddingOutOfRange)
{
    {
        Json document = CubeMeshDocument();
        document["resolution"] = Json::array({1, 24, 24});
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["resolution"] = Json::array({24, 49, 24});
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["resolution"] = Json::array({24, 24});
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["resolution"] = Json::array({24, 24.5, 24});
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["padding"] = -0.5;
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        document["padding"] =
            std::numeric_limits<double>::quiet_NaN();
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
}

TEST(ParticleCollisionMeshParser, EnforcesVertexAndTriangleBudget)
{
    {
        Json document = CubeMeshDocument();
        Json vertices = Json::array();
        for (size_t index = 0;
             index <= kMaxParticleCollisionMeshVertices;
             ++index)
        {
            vertices.push_back(Json::array({0.0, 0.0, 0.0}));
        }
        document["vertices"] = std::move(vertices);
        document["indices"] = Json::array({0, 0, 0});
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        Json document = CubeMeshDocument();
        Json indices = Json::array();
        for (size_t index = 0;
             index <= kMaxParticleCollisionMeshTriangles;
             ++index)
        {
            indices.push_back(0);
            indices.push_back(1);
            indices.push_back(2);
        }
        document["indices"] = std::move(indices);
        EXPECT_THROW(
            ParseParticleCollisionMeshJson(document),
            std::invalid_argument);
    }
    {
        // 三角形数量与分辨率各自合法，但组合工作量超预算。
        Json document = CubeMeshDocument();
        Json indices = Json::array();
        for (size_t index = 0;
             index < kMaxParticleCollisionMeshTriangles;
             ++index)
        {
            indices.push_back(0);
            indices.push_back(1);
            indices.push_back(2);
        }
        document["indices"] = std::move(indices);
        document["resolution"] = Json::array({48, 48, 48});
        const ParticleCollisionMeshAsset mesh =
            ParseParticleCollisionMeshJson(document);
        EXPECT_THROW(
            BuildParticleSdfFromCollisionMesh(mesh),
            std::invalid_argument);
    }
}

TEST(ParticleCollisionMeshSdf, ClosedCubeHasNegativeInterior)
{
    const ParticleCollisionMeshAsset mesh =
        ParseParticleCollisionMeshJson(CubeMeshDocument());
    const ParticleSdfAsset sdf =
        BuildParticleSdfFromCollisionMesh(mesh);

    EXPECT_EQ(sdf.resolution, Eigen::Vector3i(24, 24, 24));
    EXPECT_TRUE(sdf.boundsMin.isApprox(
        Eigen::Vector3f(-15.0f, -15.0f, -15.0f)));
    EXPECT_TRUE(sdf.boundsMax.isApprox(
        Eigen::Vector3f(15.0f, 15.0f, 15.0f)));

    const float center =
        sdf.SampleDistance(Eigen::Vector3f(0.0f, 0.0f, 0.0f));
    EXPECT_LT(center, -5.0f);
    const float outside =
        sdf.SampleDistance(Eigen::Vector3f(11.0f, 0.0f, 0.0f));
    EXPECT_GT(outside, 0.0f);
    EXPECT_LT(outside, 3.0f);
    EXPECT_FALSE(std::isfinite(
        sdf.SampleDistance(Eigen::Vector3f(100.0f, 0.0f, 0.0f))));

    // 靠近 +X 面时梯度应指向 +X；中心点六面等距，梯度可能退化，
    // 不作为断言。
    const Eigen::Vector3f gradient =
        sdf.SampleGradient(Eigen::Vector3f(8.0f, 0.0f, 0.0f));
    EXPECT_GT(gradient.x(), 0.5f);
    EXPECT_NEAR(gradient.y(), 0.0f, 0.1f);
    EXPECT_NEAR(gradient.z(), 0.0f, 0.1f);
}

TEST(ParticleCollisionMeshSdf, ClosedTetrahedronHasSignedDistance)
{
    const ParticleCollisionMeshAsset mesh =
        ParseParticleCollisionMeshJson(TetrahedronMeshDocument());
    const ParticleSdfAsset sdf =
        BuildParticleSdfFromCollisionMesh(mesh);

    EXPECT_LT(
        sdf.SampleDistance(Eigen::Vector3f(1.0f, 1.0f, 1.0f)),
        0.0f);
    EXPECT_GT(
        sdf.SampleDistance(Eigen::Vector3f(-3.0f, -3.0f, -3.0f)),
        0.0f);
    EXPECT_GT(
        sdf.SampleDistance(Eigen::Vector3f(9.0f, 9.0f, 9.0f)),
        0.0f);
}

TEST(ParticleCollisionMeshSdf, RejectsOpenNonManifoldAndDegenerateFaces)
{
    {
        Json open = CubeMeshDocument();
        for (int index = 0; index < 3; ++index)
        {
            open["indices"].erase(open["indices"].end() - 1);
        }
        const ParticleCollisionMeshAsset mesh =
            ParseParticleCollisionMeshJson(open);
        EXPECT_THROW(
            BuildParticleSdfFromCollisionMesh(mesh),
            std::invalid_argument);
    }
    {
        Json duplicated = CubeMeshDocument();
        for (const int index : {0, 3, 2})
        {
            duplicated["indices"].push_back(index);
        }
        const ParticleCollisionMeshAsset mesh =
            ParseParticleCollisionMeshJson(duplicated);
        EXPECT_THROW(
            BuildParticleSdfFromCollisionMesh(mesh),
            std::invalid_argument);
    }
    {
        Json flipped = CubeMeshDocument();
        std::swap(flipped["indices"][0], flipped["indices"][1]);
        const ParticleCollisionMeshAsset mesh =
            ParseParticleCollisionMeshJson(flipped);
        EXPECT_THROW(
            BuildParticleSdfFromCollisionMesh(mesh),
            std::invalid_argument);
    }
    {
        Json degenerate = CubeMeshDocument();
        // 三个不同但共线的顶点触发面积阈值，而不是重复索引的退化边。
        degenerate["vertices"][2] = Json::array({-10.0, 0.0, -10.0});
        const ParticleCollisionMeshAsset mesh =
            ParseParticleCollisionMeshJson(degenerate);
        EXPECT_THROW(
            BuildParticleSdfFromCollisionMesh(mesh),
            std::invalid_argument);
    }
    {
        Json sharedVertex = Json{
            {"type", "particleCollisionMesh"},
            {"vertices", Json::array({
                Json::array({0.0, 0.0, 0.0}),
                Json::array({10.0, 0.0, 0.0}),
                Json::array({0.0, 10.0, 0.0}),
                Json::array({0.0, 0.0, 10.0}),
                Json::array({-10.0, 0.0, 0.0}),
                Json::array({0.0, -10.0, 0.0}),
                Json::array({0.0, 0.0, -10.0})})},
            {"indices", Json::array({
                0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3,
                0, 5, 4, 0, 4, 6, 0, 6, 5, 4, 5, 6})},
            {"resolution", Json::array({12, 12, 12})},
            {"padding", 5.0}};
        const ParticleCollisionMeshAsset mesh =
            ParseParticleCollisionMeshJson(sharedVertex);
        EXPECT_THROW(
            BuildParticleSdfFromCollisionMesh(mesh),
            std::invalid_argument);
    }
}

TEST(ParticleCollisionMeshSdf, RejectsDegenerateBounds)
{
    Json document = CubeMeshDocument();
    for (Json& vertex : document["vertices"])
    {
        vertex = Json::array({0.0, 0.0, 0.0});
    }
    document["padding"] = 0.0;
    const ParticleCollisionMeshAsset mesh =
        ParseParticleCollisionMeshJson(document);
    EXPECT_THROW(
        BuildParticleSdfFromCollisionMesh(mesh),
        std::invalid_argument);
}

TEST(ParticleCollisionMeshRuntime, MeshMapsToSdfGpuCode)
{
    EXPECT_EQ(ParticleCollisionTypeValue("mesh"), 4);
    EXPECT_EQ(ParticleCollisionTypeValue("sdf"), 4);
    EXPECT_EQ(ParticleCollisionTypeValue("plane"), 1);
    EXPECT_EQ(ParticleCollisionTypeValue("none"), 0);
}

TEST(ParticleCollisionMeshRuntime, ResourceIdentityTracksTypeAndSdfData)
{
    ParticleEmitterAsset current;
    current.collisionType = "sdf";
    current.collisionSdfPath = "Meshes/sdf.json";
    current.collisionMeshPath = "Meshes/mesh.json";
    current.collisionSdf =
        std::make_shared<const ParticleSdfAsset>();
    ParticleEmitterAsset next = current;
    EXPECT_FALSE(
        ParticleCollisionResourceIdentityChanged(current, next));

    next.collisionType = "mesh";
    EXPECT_TRUE(
        ParticleCollisionResourceIdentityChanged(current, next));

    next = current;
    next.collisionSdf =
        std::make_shared<const ParticleSdfAsset>();
    EXPECT_TRUE(
        ParticleCollisionResourceIdentityChanged(current, next));

    auto stableCurrent =
        std::make_shared<ParticleSdfAsset>();
    stableCurrent->sourceIdentity = "mesh|cube|100";
    current.collisionSdf = stableCurrent;
    auto stableNext =
        std::make_shared<ParticleSdfAsset>();
    stableNext->sourceIdentity = "mesh|cube|100";
    next = current;
    next.collisionSdf = stableNext;
    EXPECT_FALSE(
        ParticleCollisionResourceIdentityChanged(current, next));
    stableNext->sourceIdentity = "mesh|cube|200";
    EXPECT_TRUE(
        ParticleCollisionResourceIdentityChanged(current, next));
}

TEST(ParticleCollisionMeshRuntime, PushesParticleOutAndReflectsNormal)
{
    const ParticleCollisionMeshAsset mesh =
        ParseParticleCollisionMeshJson(CubeMeshDocument());
    const auto sdf = std::make_shared<const ParticleSdfAsset>(
        BuildParticleSdfFromCollisionMesh(mesh));
    ParticleEmitterAsset emitter = MeshEmitter(sdf);
    emitter.collisionRadius = 2.0f;

    ParticleCollisionState state;
    state.position = Eigen::Vector3f(9.0f, 0.0f, 0.0f);
    state.velocity = Eigen::Vector3f(-10.0f, 0.0f, 0.0f);

    Eigen::Vector3f normal = Eigen::Vector3f::Zero();
    ASSERT_TRUE(ResolveParticleCollision(state, emitter, &normal));
    EXPECT_NEAR(state.position.x(), 12.0f, 1.0f);
    EXPECT_GT(normal.x(), 0.9f);
    EXPECT_GT(state.velocity.x(), 0.0f);
}

TEST(ParticleCollisionMeshRuntime, CcdStopsParticleCrossingFace)
{
    const ParticleCollisionMeshAsset mesh =
        ParseParticleCollisionMeshJson(CubeMeshDocument());
    const auto sdf = std::make_shared<const ParticleSdfAsset>(
        BuildParticleSdfFromCollisionMesh(mesh));
    ParticleEmitterAsset emitter = MeshEmitter(sdf);
    emitter.collisionRadius = 0.5f;
    emitter.collisionCcd = true;

    ParticleCollisionState state;
    state.position = Eigen::Vector3f(14.0f, 0.0f, 0.0f);
    state.velocity = Eigen::Vector3f(-300.0f, 0.0f, 0.0f);

    ASSERT_TRUE(ResolveParticleCollisionSteps(
        state,
        emitter,
        Eigen::Vector3f::Zero(),
        0.0f,
        0.05f));
    EXPECT_GT(state.position.x(), 10.0f);
    EXPECT_GT(state.velocity.x(), 0.0f);
    EXPECT_GT(sdf->SampleDistance(state.position), 0.0f);
}

TEST_F(ParticleMeshCollisionTest, EmitterAssetLoadsMeshPathRoundTrip)
{
    const std::string relativePath =
        "Meshes/PS_collision_cube.json";
    WriteJson(root / relativePath, CubeMeshDocument());

    const Json document = Json{
        {"name", "PS_mesh_collision"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{
                {"name", "debris"},
                {"localSpace", true},
                {"maxParticles", 16},
                {"collision", Json{
                    {"type", "mesh"},
                    {"mesh", Json{{"path", relativePath}}},
                    {"radius", 1.0}}}}})}};

    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document,
            "Maps/SC_test/PS_mesh_collision.json",
            root);
    ASSERT_EQ(asset.emitters.size(), 1u);
    const ParticleEmitterAsset& emitter = asset.emitters.front();
    EXPECT_EQ(emitter.collisionType, "mesh");
    EXPECT_TRUE(emitter.localSpace);
    EXPECT_EQ(emitter.collisionMeshPath, relativePath);
    ASSERT_NE(emitter.collisionSdf, nullptr);
    EXPECT_FALSE(emitter.collisionSdf->Empty());
    EXPECT_LT(
        emitter.collisionSdf->SampleDistance(
            Eigen::Vector3f(0.0f, 0.0f, 0.0f)),
        0.0f);
    EXPECT_NO_THROW(ParticleAssetLoader::Validate(asset));

    // JSON round-trip 后路径与烘好的 SDF 必须保持一致。
    const ParticleSystemAsset reparsed =
        ParticleAssetLoader::ParseJson(
            Json::parse(document.dump()),
            "Maps/SC_test/PS_mesh_collision.json",
            root);
    EXPECT_EQ(
        reparsed.emitters.front().collisionMeshPath,
        relativePath);
    ASSERT_NE(reparsed.emitters.front().collisionSdf, nullptr);
    EXPECT_FLOAT_EQ(
        reparsed.emitters.front().collisionSdf->SampleDistance(
            Eigen::Vector3f(11.0f, 0.0f, 0.0f)),
        emitter.collisionSdf->SampleDistance(
            Eigen::Vector3f(11.0f, 0.0f, 0.0f)));
}

TEST_F(ParticleMeshCollisionTest, MeshTypeNeverFallsBackSilently)
{
    const Json missingPath = Json{
        {"name", "PS_mesh_missing_path"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{
                {"name", "debris"},
                {"collision", Json{{"type", "mesh"}}}}})}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            missingPath,
            "Maps/SC_test/PS_mesh_missing_path.json",
            root),
        std::invalid_argument);

    const Json missingFile = Json{
        {"name", "PS_mesh_missing_file"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{
                {"name", "debris"},
                {"collision", Json{
                    {"type", "mesh"},
                    {"mesh", Json{{"path", "Meshes/missing.json"}}}}}}})}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            missingFile,
            "Maps/SC_test/PS_mesh_missing_file.json",
            root),
        std::runtime_error);

    WriteJson(
        root / "Meshes" / "PS_wrong_type.json",
        Json{{"type", "particleSdf"}});
    const Json wrongType = Json{
        {"name", "PS_mesh_wrong_type"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{
                {"name", "debris"},
                {"collision", Json{
                    {"type", "mesh"},
                    {"mesh", Json{{"path", "Meshes/PS_wrong_type.json"}}}}}}})}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            wrongType,
            "Maps/SC_test/PS_mesh_wrong_type.json",
            root),
        std::invalid_argument);
}

TEST_F(ParticleMeshCollisionTest, InactiveMeshPathStillValidated)
{
    Json document = Json{
        {"name", "PS_plane_with_bad_mesh_path"},
        {"type", "particleSystem"}};
    const Json emitter = Json{
        {"name", "debris"},
        {"collision", Json{
            {"type", "plane"},
            {"mesh", Json{{"path", "../Meshes/cube.json"}}}}}};
    document["emitters"] = Json::array({emitter});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document,
            "Maps/SC_test/PS_plane_with_bad_mesh_path.json",
            root),
        std::invalid_argument);
}

TEST_F(ParticleMeshCollisionTest, PaddingMustCoverCollisionRadius)
{
    const std::string relativePath =
        "Meshes/PS_collision_cube.json";
    WriteJson(root / relativePath, CubeMeshDocument());
    const Json document = Json{
        {"name", "PS_mesh_padding"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{
                {"name", "debris"},
                {"collision", Json{
                    {"type", "mesh"},
                    {"mesh", Json{{"path", relativePath}}},
                    {"radius", 6.0}}}}})}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document,
            "Maps/SC_test/PS_mesh_padding.json",
            root),
        std::invalid_argument);

    Json covered = document;
    covered["emitters"][0]["collision"]["radius"] = 5.0;
    EXPECT_NO_THROW(
        (void)ParticleAssetLoader::ParseJson(
            covered,
            "Maps/SC_test/PS_mesh_padding.json",
            root));
}
