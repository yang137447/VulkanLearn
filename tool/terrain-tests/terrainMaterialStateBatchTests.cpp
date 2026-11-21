#include <gtest/gtest.h>
#include "material/materialInstanceStateBatch.h"

namespace
{
std::shared_ptr<MaterialInstance> Instance()
{
    auto material = std::make_shared<MaterialInstance>();
    material->SetParameter("value", 1.f);
    material->SetParameter("pair", Eigen::Vector2f(2, 3));
    material->SetTexture("page", {});
    return material;
}

std::shared_ptr<Texture> Token(uintptr_t value)
{
    return std::shared_ptr<Texture>(reinterpret_cast<Texture*>(value), [](Texture*) {});
}
}

TEST(TerrainMaterialStateBatch, PrepareDoesNotChangeAnyCurrentBinding)
{
    auto material = Instance();
    VL::MaterialInstanceStateBatch batch({{material, {{"value", 4.f}}, {{"page", Token(1)}}}});
    EXPECT_FLOAT_EQ(material->GetParameter<float>("value"), 1.f);
    EXPECT_FALSE(material->GetTexture("page"));
    EXPECT_GT(batch.MetadataBytes(), 0u);
}

TEST(TerrainMaterialStateBatch, MultiMaterialCommitPublishesAllPrebuiltValues)
{
    auto first = Instance();
    auto second = Instance();
    const auto version = first->GetTextureBindingVersion();
    VL::MaterialInstanceStateBatch batch({
        {first, {{"value", 4.f}, {"pair", Eigen::Vector2f(5, 6)}}, {{"page", Token(1)}}},
        {second, {{"value", 7.f}}, {{"page", Token(2)}}}});
    ASSERT_TRUE(batch.TryLockNoThrow());
    batch.CommitNoThrow();
    EXPECT_FLOAT_EQ(first->GetParameter<float>("value"), 4.f);
    EXPECT_EQ(first->GetParameter<Eigen::Vector2f>("pair"), Eigen::Vector2f(5, 6));
    EXPECT_FLOAT_EQ(second->GetParameter<float>("value"), 7.f);
    EXPECT_EQ(first->GetTexture("page").get(), reinterpret_cast<Texture*>(1));
    EXPECT_EQ(second->GetTexture("page").get(), reinterpret_cast<Texture*>(2));
    EXPECT_EQ(first->GetTextureBindingVersion(), version + 1);
    EXPECT_FALSE(batch.TryLockNoThrow());
}

TEST(TerrainMaterialStateBatch, CancelAfterLocksKeepsWholeOldState)
{
    auto material = Instance();
    VL::MaterialInstanceStateBatch batch({{material, {{"value", 4.f}}, {{"page", Token(1)}}}});
    ASSERT_TRUE(batch.TryLockNoThrow());
    batch.CancelNoThrow();
    EXPECT_FLOAT_EQ(material->GetParameter<float>("value"), 1.f);
    EXPECT_FALSE(material->GetTexture("page"));
}

TEST(TerrainMaterialStateBatch, ConcurrentNumericChangeRejectsStalePreparation)
{
    auto material = Instance();
    VL::MaterialInstanceStateBatch batch({{material, {{"value", 4.f}}, {}}});
    material->SetParameter("value", 8.f);
    EXPECT_FALSE(batch.TryLockNoThrow());
    EXPECT_FLOAT_EQ(material->GetParameter<float>("value"), 8.f);
}

TEST(TerrainMaterialStateBatch, ConcurrentTextureChangeRejectsStalePreparation)
{
    auto material = Instance();
    VL::MaterialInstanceStateBatch batch({{material, {}, {{"page", Token(1)}}}});
    material->SetTexture("page", Token(2));
    EXPECT_FALSE(batch.TryLockNoThrow());
    EXPECT_EQ(material->GetTexture("page").get(), reinterpret_cast<Texture*>(2));
}

TEST(TerrainMaterialStateBatch, InvalidSecondMaterialLeavesFirstUntouched)
{
    auto first = Instance();
    auto second = Instance();
    EXPECT_THROW(VL::MaterialInstanceStateBatch({
        {first, {{"value", 4.f}}, {}},
        {second, {{"missing", 3.f}}, {}}}), std::invalid_argument);
    EXPECT_FLOAT_EQ(first->GetParameter<float>("value"), 1.f);
}

TEST(TerrainMaterialStateBatch, NoThrowCommitWithoutLocksDoesNothing)
{
    auto material = Instance();
    VL::MaterialInstanceStateBatch batch({{material, {{"value", 4.f}}, {}}});
    batch.CommitNoThrow();
    EXPECT_FLOAT_EQ(material->GetParameter<float>("value"), 1.f);
}

TEST(TerrainMaterialStateBatch, TextureAssetIdentitySurvivesRuntimeReplacement)
{
    auto material = Instance();
    material->SetTexture("page", {}, "Generated/T_texture.json", "key");
    VL::MaterialInstanceStateBatch batch({{material, {}, {{"page", Token(1)}}}});
    ASSERT_TRUE(batch.TryLockNoThrow());
    batch.CommitNoThrow();
    const auto snapshot = material->CaptureStateSnapshot();
    EXPECT_EQ(snapshot.textures.at("page").textureAssetIdentity, "Generated/T_texture.json");
    EXPECT_EQ(snapshot.textures.at("page").textureCacheIdentity, "key");
}

TEST(TerrainMaterialStateBatch, AbandonedLockedBatchReleasesLocksWithoutPublishing)
{
    auto material = Instance();
    {
        VL::MaterialInstanceStateBatch batch({{material, {{"value", 4.f}}, {}}});
        ASSERT_TRUE(batch.TryLockNoThrow());
    }
    material->SetParameter("value", 8.f);
    EXPECT_FLOAT_EQ(material->GetParameter<float>("value"), 8.f);
}
