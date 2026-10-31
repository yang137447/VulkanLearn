#include <stdexcept>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "particles/particleAssetOverride.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;

Json DraftDocument()
{
    return Json{
        {"name", "PS_draft"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{{"name", "fire"},
                {"maxParticles", 128},
                {"spawnRate", 301.0}}
        })}
    };
}

TEST(ParticlePreviewUpdate, DraftOverrideBypassesDisk)
{
    const std::string path =
        "Maps/__particle_preview_missing__/PS_draft.json";
    std::unordered_map<std::string, Json> documents;
    documents.emplace(path, DraftDocument());

    const ParticleSystemAsset asset =
        PrepareParticleAssetWithOverride(path, documents);
    EXPECT_EQ(asset.name, "PS_draft");
    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_EQ(asset.emitters[0].name, "fire");
    EXPECT_FLOAT_EQ(asset.emitters[0].spawnRate, 301.0f);
}

TEST(ParticlePreviewUpdate, NonMatchingDraftStillReadsDisk)
{
    std::unordered_map<std::string, Json> documents;
    documents.emplace(
        "Maps/__particle_preview_missing__/PS_other.json",
        DraftDocument());
    EXPECT_THROW(
        PrepareParticleAssetWithOverride(
            "Maps/__particle_preview_missing__/PS_absent.json",
            documents),
        std::exception);
}

TEST(ParticlePreviewUpdate, InvalidDraftIsRejectedBeforeWorldLoad)
{
    Json document = DraftDocument();
    document["emitters"][0]["spawnRate"] = -1.0;
    std::unordered_map<std::string, Json> documents;
    const std::string path =
        "Maps/__particle_preview_missing__/PS_invalid.json";
    documents.emplace(path, std::move(document));
    EXPECT_THROW(
        PrepareParticleAssetWithOverride(path, documents),
        std::invalid_argument);
}

} // namespace
