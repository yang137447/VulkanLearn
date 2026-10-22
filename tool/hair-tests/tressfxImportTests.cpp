#include "mesh/loader/hair/tressfxModelImporter.h"
#include "mesh/validation/meshAssetValidator.h"
#include "tfxFixture.h"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace
{
nlohmann::json Descriptor(float radius = 0.05f)
{
    return {{"name", "SM_guides"}, {"type", "tressfx"},
        {"modelDataPath", "Maps/SC_probe/Source/guides.tfx"}, {"strandRadius", radius},
        {"materialSlots", {{{"name", "hair"}, {"materialInstancePath", "MI_probe.json"}}}}};
}

TEST(TressfxDescriptor, RequiresPositiveFiniteRepresentableRadius)
{
    for (double radius : {0.0, -1.0, 1e-300, 1e300, std::numeric_limits<double>::infinity()})
    {
        auto json = Descriptor();
        json["strandRadius"] = radius;
        EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_guides.json", json), std::runtime_error);
    }
    auto json = Descriptor();
    json.erase("strandRadius");
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_guides.json", json), std::runtime_error);
}

TEST(TressfxDescriptor, RadiusParticipatesInMeshIdentityWithoutDecimalTruncation)
{
    const auto first = MeshAssetValidator::BuildLoadPlan("SM_a.json", Descriptor(0.000001f));
    const auto second = MeshAssetValidator::BuildLoadPlan("SM_b.json", Descriptor(0.0000011f));
    EXPECT_NE(first.modelCacheKey, second.modelCacheKey);
}

TEST(TressfxDescriptor, RejectsAssimpImportOptions)
{
    auto json = Descriptor();
    json["importOptions"] = {{"generateSmoothNormals", false}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_guides.json", json), std::runtime_error);
}

TEST(TressfxDescriptor, RejectsMultipleMaterialSlots)
{
    auto json = Descriptor();
    json["materialSlots"].push_back({{"name", "extra"}, {"materialInstancePath", "MI_other.json"}});
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_guides.json", json), std::runtime_error);
}

TEST(TressfxDescriptor, RejectsNonTfxSource)
{
    auto json = Descriptor();
    json["modelDataPath"] = "hair.obj";
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_guides.json", json), std::runtime_error);
}

TEST(TressfxDescriptor, RejectsSkeletalDeclaration)
{
    auto json = Descriptor();
    json["skinning"] = {{"clips", nlohmann::json::array()}, {"skeleton", "SK_head.json"}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_guides.json", json), std::runtime_error);
}

class TressfxImport : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("vl-strand-import-" + std::to_string(stamp));
        ownsRoot = std::filesystem::create_directory(root);
        ASSERT_TRUE(ownsRoot);
        request.buildPlan = MeshAssetValidator::BuildLoadPlan("SM_guides.json", Descriptor());
        request.absoluteModelPath = (root / std::filesystem::u8path(u8"发丝.tfx")).u8string();
    }
    void TearDown() override
    {
        if (ownsRoot) { std::error_code error; std::filesystem::remove_all(root, error); }
    }
    ModelResource Import(const std::vector<uint8_t>& bytes)
    {
        {
            std::ofstream stream(std::filesystem::u8path(request.absoluteModelPath), std::ios::binary);
            stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!stream) throw std::runtime_error("Failed to write owned fixture");
        }
        return TressfxModelImporter().Import(request);
    }
    std::filesystem::path root;
    bool ownsRoot = false;
    ModelImportRequest request;
};

TEST_F(TressfxImport, BuildsOneSectionAndMapsExistingMaterialSlot)
{
    const auto resource = Import(TfxFixture::Guides(3));
    ASSERT_EQ(resource.sections.size(), 1u);
    EXPECT_EQ(resource.sections[0].vertices.size(), 24u);
    EXPECT_EQ(resource.sections[0].indices.size(), 54u);
    const auto slots = MeshAssetValidator::BuildSectionLoadPlans(request.buildPlan, resource);
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_EQ(slots[0].materialInstancePath, "MI_probe.json");
}

TEST_F(TressfxImport, KeepsCenterlinesAndEncodesSignedRadiusWithoutInverseMass)
{
    auto bytes = TfxFixture::Guides();
    TfxFixture::Float(bytes, 172, 200.0f);
    const auto section = Import(bytes).sections[0];
    EXPECT_TRUE(section.vertices[0].position.isApprox(Eigen::Vector3f(0, 0, -2)));
    EXPECT_TRUE(section.vertices[1].position.isApprox(section.vertices[0].position));
    EXPECT_FLOAT_EQ(section.vertices[0].texCoord1.x(), -0.05f);
    EXPECT_FLOAT_EQ(section.vertices[1].texCoord1.x(), 0.05f);
    EXPECT_FLOAT_EQ(section.vertices.back().texCoord1.y(), 1.0f);
    EXPECT_TRUE(section.vertices[0].tangent.isApprox(Eigen::Vector4f(0, 1, 0, 1)));
}

TEST_F(TressfxImport, AppliesPerGuideThicknessAndPadsBounds)
{
    auto bytes = TfxFixture::Guides(2);
    TfxFixture::Attribute(bytes, 24, {0.5f, 3.0f});
    const auto section = Import(bytes).sections[0];
    EXPECT_FLOAT_EQ(section.vertices[1].texCoord1.x(), 0.025f);
    EXPECT_FLOAT_EQ(section.vertices[9].texCoord1.x(), 0.15f);
    EXPECT_FLOAT_EQ(section.boundsPadding, 0.15f);
}

TEST_F(TressfxImport, CarriesExportedVertexUvAndColorUnchanged)
{
    auto bytes = TfxFixture::Guides();
    TfxFixture::Attribute(bytes, 16, {0.2f, 0.3f});
    TfxFixture::Attribute(bytes, 20, {2, -1, 3, -2, 4, -3, 5, -4});
    TfxFixture::Attribute(bytes, 28, {2, 3, 4, 0.5f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1});
    const auto section = Import(bytes).sections[0];
    EXPECT_TRUE(section.vertices[0].texCoord.isApprox(Eigen::Vector2f(2, -1)));
    EXPECT_TRUE(section.vertices[0].color.isApprox(Eigen::Vector4f(2, 3, 4, 0.5f)));
}

TEST_F(TressfxImport, MissingOptionalAttributesUseExplicitWhiteAndZeroUv)
{
    const auto section = Import(TfxFixture::Guides()).sections[0];
    EXPECT_TRUE(section.vertices[0].color.isApprox(Eigen::Vector4f::Ones()));
    EXPECT_TRUE(section.vertices[0].texCoord.isZero());
}

TEST_F(TressfxImport, RejectsUnrepresentableThicknessProduct)
{
    auto bytes = TfxFixture::Guides();
    request.buildPlan.strandRadius = std::numeric_limits<float>::max();
    TfxFixture::Attribute(bytes, 24, {2.0f});
    EXPECT_THROW(Import(bytes), std::runtime_error);
}

TEST_F(TressfxImport, RejectsInvalidCenterlineBeforePublishingGeometry)
{
    auto bytes = TfxFixture::Guides();
    TfxFixture::Point(bytes, 1, {0, 0, -2, 0});
    EXPECT_THROW(Import(bytes), std::runtime_error);
}
}
