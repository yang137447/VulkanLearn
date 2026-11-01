#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/command/assetDocumentFields.h"
#include "particles/particleAsset.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;
using namespace VL::Editor;

Json MultiEmitterDocument()
{
    return Json{
        {"name", "PS_test"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{
                {"name", "fire"},
                {"position", {0.0, 0.0, 0.0}},
                {"maxParticles", 128},
                {"lifetime", {0.5, 1.0}}
            },
            Json{
                {"name", "smoke"},
                {"position", {0.0, 55.0, 0.0}},
                {"maxParticles", 64},
                {"lifetime", {2.0, 3.0}}
            }
        })}
    };
}

} // namespace

TEST(ParticleAsset, MultiEmitterParsesNamesAndLocalPositions)
{
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            MultiEmitterDocument(), "Maps/SC_test/PS_test.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(asset.emitters.at(0).name, "fire");
    EXPECT_EQ(asset.emitters.at(1).name, "smoke");
    EXPECT_FLOAT_EQ(asset.emitters.at(1).localPosition.y(), 55.0f);
    EXPECT_EQ(asset.emitters.at(0).maxParticles, 128u);
    EXPECT_EQ(asset.emitters.at(1).maxParticles, 64u);
}

TEST(ParticleAsset, LegacyRootBecomesOneImplicitEmitter)
{
    const Json document{
        {"name", "PS_legacy"},
        {"type", "particleSystem"},
        {"maxParticles", 32},
        {"spawnRate", 10.0}
    };
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_legacy.json");

    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_EQ(asset.name, "PS_legacy");
    EXPECT_EQ(asset.emitters.at(0).name, "emitter0");
    EXPECT_FLOAT_EQ(asset.emitters.at(0).localPosition.squaredNorm(), 0.0f);
    EXPECT_EQ(asset.emitters.at(0).maxParticles, 32u);
}

namespace
{

Json V2ModuleDocument()
{
    return Json{
        {"name", "PS_v2"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", {{"Wind", 2.5}}},
        {"emitters", Json::array({
            Json{
                {"name", "fire"},
                {"position", {1.0, 2.0, 3.0}},
                {"modules", Json::array({
                    Json{{"type", "emitter"}, {"fields", {
                        {"maxParticles", 128},
                        {"renderMode", "billboard"}}}},
                    Json{{"type", "spawn.rate"}, {"fields", {
                        {"spawnRate", 50.0},
                        {"burst", false}}}},
                    Json{{"type", "forces.gravity"}, {"fields", {
                        {"gravity", {0.0, -50.0, 0.0}}}}},
                    Json{{"type", "update.lifetime"}, {"fields", {
                        {"lifetime", {1.0, 2.0}}}}}
                })}
            }
        })}
    };
}

} // namespace

TEST(ParticleAsset, V2ModulesFlattenIntoRuntimeEmitter)
{
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            V2ModuleDocument(), "Maps/SC_test/PS_v2.json");

    ASSERT_EQ(asset.version, 2u);
    ASSERT_EQ(asset.userParameters.size(), 1u);
    EXPECT_FLOAT_EQ(asset.userParameters.at("Wind"), 2.5f);
    ASSERT_EQ(asset.emitters.size(), 1u);
    const ParticleEmitterAsset& emitter = asset.emitters.front();
    EXPECT_EQ(emitter.name, "fire");
    EXPECT_EQ(emitter.maxParticles, 128u);
    EXPECT_FLOAT_EQ(emitter.spawnRate, 50.0f);
    EXPECT_EQ(emitter.gravity, Eigen::Vector3f(0.0f, -50.0f, 0.0f));
    EXPECT_FLOAT_EQ(emitter.lifetimeMin, 1.0f);
    EXPECT_FLOAT_EQ(emitter.lifetimeMax, 2.0f);
}

TEST(ParticleAsset, V2DisabledModulesLeaveRuntimeDefaults)
{
    Json document = V2ModuleDocument();
    document["emitters"][0]["modules"][1]["enabled"] = false;
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_v2_disabled.json");
    EXPECT_FLOAT_EQ(asset.emitters.front().spawnRate, 200.0f);
}

TEST(ParticleAsset, V2ModuleFieldConflictsAndUnknownTypesAreRejected)
{
    Json conflict = V2ModuleDocument();
    conflict["emitters"][0]["modules"].push_back(Json{
        {"type", "spawn.rate"},
        {"fields", {{"spawnRate", 10.0}}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            conflict, "Maps/SC_test/PS_v2_conflict.json"),
        std::invalid_argument);

    Json unknown = V2ModuleDocument();
    unknown["emitters"][0]["modules"][0]["type"] = "spawn.typo";
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            unknown, "Maps/SC_test/PS_v2_unknown.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, V2UserParametersRejectNonNumbers)
{
    Json document = V2ModuleDocument();
    document["userParameters"]["Wind"] = "fast";
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_v2_params.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, DuplicateEmitterNamesAreRejected)
{
    Json document = MultiEmitterDocument();
    document["emitters"][1]["name"] = "fire";

    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_duplicate.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, EmptyAndOversizedEmitterArraysAreRejected)
{
    Json empty = MultiEmitterDocument();
    empty["emitters"] = Json::array();
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            empty, "Maps/SC_test/PS_empty.json"),
        std::invalid_argument);

    Json oversized = MultiEmitterDocument();
    oversized["emitters"] = Json::array();
    for (size_t index = 0; index < 17; ++index)
        oversized["emitters"].push_back(Json{
            {"name", "emitter" + std::to_string(index)},
            {"maxParticles", 8}
        });
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            oversized, "Maps/SC_test/PS_oversized.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, SortLimitIsAppliedPerEmitter)
{
    Json document = MultiEmitterDocument();
    document["emitters"][1]["maxParticles"] = 4097;
    document["emitters"][1]["sortParticles"] = true;

    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_sort.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, EditorPathsWriteSelectedEmitterField)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange change;
    change.field = "emitters.1.spawnRate";
    change.value = 123.0;
    changes.push_back(change);

    const Json changed = ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);

    ASSERT_EQ(changed.size(), 1u);
    EXPECT_DOUBLE_EQ(
        document["emitters"][1]["spawnRate"].get<double>(), 123.0);
    EXPECT_FALSE(document["emitters"][0].contains("spawnRate"));
    (void)ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_modified.json");
}

TEST(ParticleAsset, EditorRejectsOutOfRangeEmitterPath)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange change;
    change.field = "emitters.2.spawnRate";
    change.value = 10.0;
    changes.push_back(change);

    EXPECT_THROW(
        ApplyAssetChanges(
            document, EditorAssetDocumentKind::ParticleSystem, changes),
        std::invalid_argument);
}

TEST(ParticleAsset, CapabilitiesAndProjectionExposeEmitterSelection)
{
    const Json document = MultiEmitterDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const Json projection = ProjectEditableAsset(
        EditorAssetDocumentKind::ParticleSystem, document);

    ASSERT_TRUE(capabilities["emitters"].is_array());
    ASSERT_EQ(capabilities["emitters"].size(), 2u);
    EXPECT_EQ(capabilities["emitters"][1]["index"], 1);
    EXPECT_EQ(capabilities["emitters"][1]["name"], "smoke");
    ASSERT_TRUE(projection["emitters"].is_array());
    EXPECT_EQ(projection["emitters"].size(), 2u);
    EXPECT_EQ(projection["emitters"][0]["name"], "fire");
    EXPECT_EQ(projection["emitters"][1]["position"][1], 55.0);
}

TEST(ParticleAsset, CapabilitiesExposeRotationFields)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["rotation"] = 15.0;
    document["emitters"][0]["rotationSpeed"] = 30.0;
    document["emitters"][0]["rotationRandom"] = true;
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const Json projection = ProjectEditableAsset(
        EditorAssetDocumentKind::ParticleSystem, document);

    ASSERT_TRUE(capabilities["emitters"].is_array());
    ASSERT_EQ(capabilities["emitters"].size(), 2u);
    bool hasRotation = false;
    bool hasRotationSpeed = false;
    bool hasRotationRandom = false;
    for (const Json& field : capabilities["emitters"][0]["fields"])
    {
        const std::string name = field.value("field", std::string());
        if (name == "rotation")
        {
            hasRotation = true;
            EXPECT_EQ(field.value("unit", std::string()), "degrees");
        }
        else if (name == "rotationSpeed")
        {
            hasRotationSpeed = true;
            EXPECT_EQ(field.value("unit", std::string()), "degrees/s");
        }
        else if (name == "rotationRandom")
        {
            hasRotationRandom = true;
            EXPECT_EQ(field.value("type", std::string()), "boolean");
        }
    }
    EXPECT_TRUE(hasRotation);
    EXPECT_TRUE(hasRotationSpeed);
    EXPECT_TRUE(hasRotationRandom);
    EXPECT_EQ(projection["emitters"][0]["rotation"], 15.0);
    EXPECT_EQ(projection["emitters"][0]["rotationSpeed"], 30.0);
    EXPECT_EQ(projection["emitters"][0]["rotationRandom"], true);
}

TEST(ParticleAsset, VisibilityFieldsParseAndRoundTrip)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["maxDrawDistance"] = 4000.0;
    document["emitters"][0]["fadeDistance"] = 800.0;
    document["emitters"][0]["maxScreenSize"] = 180.0;
    document["emitters"][1]["minScreenSize"] = 1.5;

    const Json roundTripped = Json::parse(document.dump());
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            roundTripped, "Maps/SC_test/PS_visibility.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].maxDrawDistance, 4000.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].fadeDistance, 800.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].maxScreenSize, 180.0f);
    EXPECT_FLOAT_EQ(asset.emitters[1].minScreenSize, 1.5f);
    EXPECT_FLOAT_EQ(asset.emitters[1].maxDrawDistance, 0.0f);
}

TEST(ParticleAsset, VisibilityFieldsRejectNegativeValues)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["maxDrawDistance"] = -1.0;

    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_negative_distance.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["minScreenSize"] = -0.1;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_negative_screen.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, VisibilityFieldsValidateFadeRange)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["maxDrawDistance"] = 100.0;
    document["emitters"][0]["fadeDistance"] = 101.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_fade_too_large.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["fadeDistance"] = 10.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_fade_without_distance.json"),
        std::invalid_argument);
}

namespace
{

Json FlipbookDocument(
    uint32_t startFrame,
    uint32_t endFrame,
    uint32_t columns = 4,
    uint32_t rows = 4)
{
    Json document = MultiEmitterDocument();
    Json& flipbook = document["emitters"][0]["flipbook"];
    flipbook = Json::object();
    flipbook["columns"] = columns;
    flipbook["rows"] = rows;
    flipbook["startFrame"] = startFrame;
    flipbook["endFrame"] = endFrame;
    return document;
}

} // namespace

TEST(ParticleAsset, FlipbookFrameDurationsAndPivotsRoundTrip)
{
    Json document = FlipbookDocument(4, 6);
    document["emitters"][0]["flipbook"]["frameDurations"] =
        Json::array({0.1, 0.25, 0.4});
    document["emitters"][0]["flipbook"]["framePivots"] =
        Json::array({
            Json::array({0.5, 0.35}),
            Json::array({0.25, 1.0}),
            Json::array({0.0, 0.0})});

    const Json roundTripped = Json::parse(document.dump());
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        roundTripped, "Maps/SC_test/PS_flipbook_frames.json");

    ASSERT_EQ(asset.emitters[0].flipbookFrameDurations.size(), 3u);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFrameDurations[0], 0.1f);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFrameDurations[2], 0.4f);
    ASSERT_EQ(asset.emitters[0].flipbookFramePivots.size(), 3u);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFramePivots[0].x(), 0.5f);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFramePivots[0].y(), 0.35f);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFramePivots[1].x(), 0.25f);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFramePivots[2].y(), 0.0f);
}

TEST(ParticleAsset, FlipbookFrameTablesRequireMatchingLength)
{
    Json document = FlipbookDocument(0, 3);
    document["emitters"][0]["flipbook"]["frameDurations"] =
        Json::array({0.1, 0.2});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_duration_mismatch.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 3);
    document["emitters"][0]["flipbook"]["framePivots"] =
        Json::array({Json::array({0.5, 0.5})});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_pivot_mismatch.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, FlipbookFrameDurationsRejectInvalidEntries)
{
    Json document = FlipbookDocument(0, 1);
    document["emitters"][0]["flipbook"]["frameDurations"] =
        Json::array({0.1, 0.0});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_duration_zero.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 1);
    document["emitters"][0]["flipbook"]["frameDurations"] =
        Json::array({0.1, "fast"});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_duration_text.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 1);
    document["emitters"][0]["flipbook"]["frameDurations"] = 0.1;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_duration_not_array.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, FlipbookFramePivotsRejectOutOfRange)
{
    Json document = FlipbookDocument(0, 0);
    document["emitters"][0]["flipbook"]["framePivots"] =
        Json::array({Json::array({1.5, 0.5})});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_pivot_above_one.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 0);
    document["emitters"][0]["flipbook"]["framePivots"] =
        Json::array({Json::array({-0.1, 0.5})});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_pivot_negative.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 0);
    document["emitters"][0]["flipbook"]["framePivots"] =
        Json::array({Json::array({0.5})});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_pivot_short.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, FlipbookSupportsAtlasUpTo256Frames)
{
    Json document = FlipbookDocument(0, 255, 16, 16);
    Json durations = Json::array();
    for (uint32_t index = 0; index < 256; ++index)
        durations.push_back(0.05);
    document["emitters"][0]["flipbook"]["frameDurations"] = durations;

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_flipbook_256.json");
    EXPECT_EQ(asset.emitters[0].flipbookFrameDurations.size(), 256u);

    Json oversized = FlipbookDocument(0, 15, 17, 16);
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            oversized, "Maps/SC_test/PS_flipbook_17x16.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, FlipbookFrameUVsRoundTrip)
{
    Json document = FlipbookDocument(2, 3);
    document["emitters"][0]["flipbook"]["frameUVs"] =
        Json::array({
            Json::array({0.0, 0.0, 0.5, 1.0}),
            Json::array({0.5, 0.0, 1.0, 1.0})});

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_flipbook_uvs.json");

    ASSERT_EQ(asset.emitters[0].flipbookFrameUVs.size(), 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFrameUVs[0].x(), 0.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFrameUVs[0].z(), 0.5f);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFrameUVs[1].y(), 0.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookFrameUVs[1].w(), 1.0f);
}

TEST(ParticleAsset, FlipbookFrameUVsRejectInvalidRects)
{
    Json document = FlipbookDocument(0, 1);
    document["emitters"][0]["flipbook"]["frameUVs"] =
        Json::array({Json::array({0.0, 0.0, 1.0, 1.0})});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_uv_mismatch.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 0);
    document["emitters"][0]["flipbook"]["frameUVs"] =
        Json::array({Json::array({0.6, 0.0, 0.4, 1.0})});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_uv_reversed_u.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 0);
    document["emitters"][0]["flipbook"]["frameUVs"] =
        Json::array({Json::array({0.0, 0.5, 1.0, 0.5})});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_uv_reversed_v.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 0);
    document["emitters"][0]["flipbook"]["frameUVs"] =
        Json::array({Json::array({0.0, 0.0, 1.2, 1.0})});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_uv_above_one.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 0);
    document["emitters"][0]["flipbook"]["frameUVs"] =
        Json::array({Json::array({0.0, 0.0, 1.0})});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_uv_short.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, FlipbookFrameUVsRequireAtMost64Frames)
{
    Json document = FlipbookDocument(0, 255, 16, 16);
    Json uvs = Json::array();
    for (uint32_t index = 0; index < 256; ++index)
        uvs.push_back(Json::array({0.0, 0.0, 1.0, 1.0}));
    document["emitters"][0]["flipbook"]["frameUVs"] = uvs;

    try
    {
        (void)ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_uv_256.json");
        FAIL() << "UV rects beyond the 64-frame GPU table must be rejected";
    }
    catch (const std::invalid_argument& exception)
    {
        EXPECT_NE(
            std::string(exception.what()).find("at most 64 frames"),
            std::string::npos);
    }
}

TEST(ParticleAsset, FlipbookFrameRotationsRoundTrip)
{
    Json document = FlipbookDocument(2, 4);
    document["emitters"][0]["flipbook"]["frameRotations"] =
        Json::array({-15.0, 0.0, 30.5});

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_flipbook_rotations.json");

    ASSERT_EQ(asset.emitters[0].flipbookFrameRotations.size(), 3u);
    EXPECT_FLOAT_EQ(
        asset.emitters[0].flipbookFrameRotations[0], -15.0f);
    EXPECT_FLOAT_EQ(
        asset.emitters[0].flipbookFrameRotations[2], 30.5f);
}

TEST(ParticleAsset, FlipbookFrameRotationsRejectBadLengthOrValues)
{
    Json document = FlipbookDocument(0, 1);
    document["emitters"][0]["flipbook"]["frameRotations"] =
        Json::array({0.0});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_rotation_mismatch.json"),
        std::invalid_argument);

    document = FlipbookDocument(0, 0);
    document["emitters"][0]["flipbook"]["frameRotations"] =
        Json::array({"spin"});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_rotation_string.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, FlipbookFrameRotationsRequireAtMost64Frames)
{
    Json document = FlipbookDocument(0, 255, 16, 16);
    Json rotations = Json::array();
    for (uint32_t index = 0; index < 256; ++index)
        rotations.push_back(0.0);
    document["emitters"][0]["flipbook"]["frameRotations"] = rotations;

    try
    {
        (void)ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_rotation_256.json");
        FAIL() << "Frame rotations beyond the 64-frame GPU table must "
                  "be rejected";
    }
    catch (const std::invalid_argument& exception)
    {
        EXPECT_NE(
            std::string(exception.what()).find("at most 64 frames"),
            std::string::npos);
    }
}

TEST(ParticleAsset, BurstIntervalParsesAndValidates)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["burst"] = true;
    document["emitters"][0]["burstInterval"] = 2.5;
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_burst_interval.json");
    EXPECT_FLOAT_EQ(asset.emitters[0].burstInterval, 2.5f);
    EXPECT_FLOAT_EQ(asset.emitters[1].burstInterval, 0.0f);

    // 旧根格式同样支持该字段（PS_explosion 走这条路径）。
    const Json legacy{
        {"name", "PS_legacy_burst"},
        {"type", "particleSystem"},
        {"burst", true},
        {"burstInterval", 1.25}};
    const ParticleSystemAsset legacyAsset =
        ParticleAssetLoader::ParseJson(
            legacy, "Maps/SC_test/PS_legacy_burst.json");
    EXPECT_FLOAT_EQ(
        legacyAsset.emitters[0].burstInterval, 1.25f);

    document["emitters"][0]["burstInterval"] = -1.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_burst_negative.json"),
        std::invalid_argument);

    document["emitters"][0]["burstInterval"] = 61.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_burst_too_long.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, LodFieldsParseAndValidate)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["lodDistance"] = 3000.0;
    document["emitters"][0]["lodSpawnRateScale"] = 0.4;
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_lod.json");
    EXPECT_FLOAT_EQ(asset.emitters[0].lodDistance, 3000.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodSpawnRateScale, 0.4f);
    // 未作者化时 LOD 默认关闭且不缩放。
    EXPECT_FLOAT_EQ(asset.emitters[1].lodDistance, 0.0f);
    EXPECT_FLOAT_EQ(asset.emitters[1].lodSpawnRateScale, 1.0f);

    document["emitters"][0]["lodDistance"] = -1.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lod_negative.json"),
        std::invalid_argument);

    document["emitters"][0]["lodDistance"] = 1000.0;
    document["emitters"][0]["lodSpawnRateScale"] = 1.5;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lod_scale.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, RotationFieldsParseAndRoundTrip)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["rotation"] = 45.5;
    document["emitters"][0]["rotationSpeed"] = -720.0;
    document["emitters"][0]["rotationRandom"] = true;

    const Json roundTripped = Json::parse(document.dump());
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        roundTripped, "Maps/SC_test/PS_rotation.json");

    EXPECT_FLOAT_EQ(asset.emitters[0].rotation, 45.5f);
    EXPECT_FLOAT_EQ(asset.emitters[0].rotationSpeed, -720.0f);
    EXPECT_TRUE(asset.emitters[0].rotationRandom);
    EXPECT_FLOAT_EQ(asset.emitters[1].rotation, 0.0f);
    EXPECT_FLOAT_EQ(asset.emitters[1].rotationSpeed, 0.0f);
    EXPECT_FALSE(asset.emitters[1].rotationRandom);
}

TEST(ParticleAsset, LodTiersParseAndKeepLegacyFallback)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["lodDistance"] = 500.0;
    document["emitters"][0]["lodSpawnRateScale"] = 0.9;
    document["emitters"][0]["lod"] = Json{
        {"tiers", Json::array({
            Json{{"distance", 1000.0}, {"spawnRateScale", 0.6}},
            Json{{"distance", 3000.0}, {"spawnRateScale", 0.25}}
        })}};

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_lod_tiers.json");
    ASSERT_EQ(asset.emitters[0].lodTiers.size(), 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[0].distance, 1000.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[0].spawnRateScale, 0.6f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[1].distance, 3000.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[1].spawnRateScale, 0.25f);
    // 兼容字段仍被解析，运行时只在 tiers 为空时回退使用。
    EXPECT_FLOAT_EQ(asset.emitters[0].lodDistance, 500.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodSpawnRateScale, 0.9f);
    EXPECT_TRUE(asset.emitters[1].lodTiers.empty());
}

TEST(ParticleAsset, LodTiersRejectInvalidOrderingAndRanges)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["lod"] = Json{
        {"tiers", Json::array({
            Json{{"distance", 3000.0}, {"spawnRateScale", 0.6}},
            Json{{"distance", 1000.0}, {"spawnRateScale", 0.3}}
        })}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lod_order.json"),
        std::invalid_argument);

    document["emitters"][0]["lod"]["tiers"] = Json::array({
        Json{{"distance", 1000.0}, {"spawnRateScale", 0.6}},
        Json{{"distance", 1000.0}, {"spawnRateScale", 0.3}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lod_duplicate.json"),
        std::invalid_argument);

    document["emitters"][0]["lod"]["tiers"] = Json::array({
        Json{{"distance", -1.0}, {"spawnRateScale", 0.6}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lod_negative_tier.json"),
        std::invalid_argument);

    document["emitters"][0]["lod"]["tiers"] = Json::array({
        Json{{"distance", 1000.0}, {"spawnRateScale", 1.5}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lod_scale_tier.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, LodTiersRejectMoreThanFourEntries)
{
    Json document = MultiEmitterDocument();
    Json tiers = Json::array();
    for (int index = 0; index < 5; ++index)
    {
        tiers.push_back(Json{
            {"distance", 1000.0 * (index + 1)},
            {"spawnRateScale", 0.5}});
    }
    document["emitters"][0]["lod"] = Json{{"tiers", tiers}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lod_five.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, RotationSpeedValidationRejectsNonFiniteAndOutOfRange)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["rotationSpeed"] = 3600.0;
    EXPECT_NO_THROW((void)ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_rotation_boundary.json"));

    document["emitters"][0]["rotationSpeed"] = -3600.0;
    EXPECT_NO_THROW((void)ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_rotation_boundary_negative.json"));

    document["emitters"][0]["rotationSpeed"] = 3600.1;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_rotation_too_fast.json"),
        std::invalid_argument);

    ParticleSystemAsset asset;
    asset.name = "PS_nonfinite_rotation";
    ParticleEmitterAsset emitter;
    emitter.name = "fire";
    emitter.rotationSpeed =
        std::numeric_limits<float>::infinity();
    asset.emitters.push_back(emitter);
    EXPECT_THROW(
        ParticleAssetLoader::Validate(asset),
        std::invalid_argument);

    asset.emitters[0].rotationSpeed = 0.0f;
    asset.emitters[0].rotation =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(
        ParticleAssetLoader::Validate(asset),
        std::invalid_argument);
}

TEST(ParticleAsset, AlphaBlendRequiresSortParticles)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["blend"] = "alpha";

    try
    {
        (void)ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_alpha_unsorted.json");
        FAIL() << "alpha emitter without sorting must be rejected";
    }
    catch (const std::invalid_argument& exception)
    {
        EXPECT_NE(
            std::string(exception.what()).find(
                "alpha blend requires sortParticles=true"),
            std::string::npos);
    }

    document["emitters"][0]["sortParticles"] = true;
    EXPECT_NO_THROW(
        (void)ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_alpha_sorted.json"));
}

TEST(ParticleAsset, NonFiniteNumbersAreRejected)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity =
        std::numeric_limits<float>::infinity();

    Json document = MultiEmitterDocument();
    document["emitters"][0]["spawnRate"] = infinity;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_inf_spawn.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["lifetime"] = Json::array({nan, 1.0});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_nan_lifetime.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["size"] = Json::array({infinity, 1.0});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_inf_size.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["gravity"] =
        Json::array({0.0, -infinity, 0.0});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_inf_gravity.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "sphere"}, {"radius", nan}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_nan_radius.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["sizeKeys"] = Json::array({
        Json{{"time", 0.0}, {"size", infinity}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_inf_size_key.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["colorKeys"] = Json::array({
        Json{{"time", 0.0},
             {"color", Json::array({1.0, nan, 0.0, 1.0})}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_nan_color_key.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["flipbook"] = Json{
        {"columns", 1},
        {"rows", 2},
        {"frameDurations", Json::array({infinity, 0.1})}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_inf_duration.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["flipbook"] = Json{
        {"columns", 1},
        {"rows", 1},
        {"framePivots", Json::array({Json::array({nan, 0.5})})}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_nan_pivot.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, SpawnShapeDefaultsToSphereWithDocumentedDefaults)
{
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            MultiEmitterDocument(),
            "Maps/SC_test/PS_shape_default.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(asset.emitters[0].shapeType, "sphere");
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeRadius, 10.0f);
    EXPECT_EQ(
        asset.emitters[0].shapeExtent,
        Eigen::Vector3f(50.0f, 50.0f, 50.0f));
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeHeight, 100.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeInnerRadius, 50.0f);
}

TEST(ParticleAsset, SpawnShapeTypesParseWithTheirParameters)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "box"}, {"extent", {40.0, 50.0, 60.0}}};
    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_shape_box.json");
    EXPECT_EQ(asset.emitters[0].shapeType, "box");
    EXPECT_EQ(
        asset.emitters[0].shapeExtent,
        Eigen::Vector3f(40.0f, 50.0f, 60.0f));

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "cone"},
        {"radius", 30.0},
        {"height", 120.0},
        {"extent", {1.0, 2.0, 3.0}}};
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_shape_cone.json");
    EXPECT_EQ(asset.emitters[0].shapeType, "cone");
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeRadius, 30.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeHeight, 120.0f);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "cylinder"},
        {"radius", 25.0},
        {"height", 80.0}};
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_shape_cylinder.json");
    EXPECT_EQ(asset.emitters[0].shapeType, "cylinder");
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeRadius, 25.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeHeight, 80.0f);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "disc"}, {"radius", 35.0}};
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_shape_disc.json");
    EXPECT_EQ(asset.emitters[0].shapeType, "disc");
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeRadius, 35.0f);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "ring"},
        {"radius", 60.0},
        {"innerRadius", 20.0}};
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_shape_ring.json");
    EXPECT_EQ(asset.emitters[0].shapeType, "ring");
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeRadius, 60.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeInnerRadius, 20.0f);
}

TEST(ParticleAsset, SpawnShapeFieldsRoundTripThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange type;
    type.field = "emitters.0.shape.type";
    type.value = std::string("box");
    changes.push_back(type);
    EditorAssetFieldChange extent;
    extent.field = "emitters.0.shape.extent";
    extent.value = std::array<double, 3>{40.0, 50.0, 60.0};
    changes.push_back(extent);
    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        Json::parse(document.dump()),
        "Maps/SC_test/PS_shape_box_editor.json");
    EXPECT_EQ(asset.emitters[0].shapeType, "box");
    EXPECT_EQ(
        asset.emitters[0].shapeExtent,
        Eigen::Vector3f(40.0f, 50.0f, 60.0f));

    document = MultiEmitterDocument();
    changes.clear();
    EditorAssetFieldChange coneType;
    coneType.field = "emitters.0.shape.type";
    coneType.value = std::string("cone");
    changes.push_back(coneType);
    EditorAssetFieldChange coneHeight;
    coneHeight.field = "emitters.0.shape.height";
    coneHeight.value = 120.0;
    changes.push_back(coneHeight);
    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    asset = ParticleAssetLoader::ParseJson(
        Json::parse(document.dump()),
        "Maps/SC_test/PS_shape_cone_editor.json");
    EXPECT_EQ(asset.emitters[0].shapeType, "cone");
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeHeight, 120.0f);

    document = MultiEmitterDocument();
    changes.clear();
    EditorAssetFieldChange cylinderType;
    cylinderType.field = "emitters.1.shape.type";
    cylinderType.value = std::string("cylinder");
    changes.push_back(cylinderType);
    EditorAssetFieldChange cylinderHeight;
    cylinderHeight.field = "emitters.1.shape.height";
    cylinderHeight.value = 70.0;
    changes.push_back(cylinderHeight);
    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    asset = ParticleAssetLoader::ParseJson(
        Json::parse(document.dump()),
        "Maps/SC_test/PS_shape_cylinder_editor.json");
    EXPECT_EQ(asset.emitters[1].shapeType, "cylinder");
    EXPECT_FLOAT_EQ(asset.emitters[1].shapeHeight, 70.0f);

    document = MultiEmitterDocument();
    changes.clear();
    EditorAssetFieldChange ringType;
    ringType.field = "emitters.0.shape.type";
    ringType.value = std::string("ring");
    changes.push_back(ringType);
    EditorAssetFieldChange ringRadius;
    ringRadius.field = "emitters.0.shape.radius";
    ringRadius.value = 60.0;
    changes.push_back(ringRadius);
    EditorAssetFieldChange ringInnerRadius;
    ringInnerRadius.field = "emitters.0.shape.innerRadius";
    ringInnerRadius.value = 20.0;
    changes.push_back(ringInnerRadius);
    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    asset = ParticleAssetLoader::ParseJson(
        Json::parse(document.dump()),
        "Maps/SC_test/PS_shape_ring_editor.json");
    EXPECT_EQ(asset.emitters[0].shapeType, "ring");
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeRadius, 60.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeInnerRadius, 20.0f);
}

TEST(ParticleAsset, SpawnShapeFieldsExposedByEditorCapabilities)
{
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, MultiEmitterDocument());

    ASSERT_TRUE(capabilities["emitters"].is_array());
    ASSERT_FALSE(capabilities["emitters"].empty());
    bool hasType = false;
    bool hasExtent = false;
    bool hasHeight = false;
    bool hasInnerRadius = false;
    for (const Json& field : capabilities["emitters"][0]["fields"])
    {
        const std::string name =
            field.value("field", std::string());
        if (name == "shape.type")
        {
            hasType = true;
            const Json& values = field.at("enum");
            EXPECT_EQ(values.size(), 6u);
            for (const char* expected :
                {"sphere", "box", "cone", "cylinder", "disc", "ring"})
            {
                bool found = false;
                for (const Json& value : values)
                    found = found || value == expected;
                EXPECT_TRUE(found) << expected;
            }
        }
        else if (name == "shape.extent")
        {
            hasExtent = true;
            EXPECT_EQ(field.value("type", std::string()), "vec3");
            EXPECT_EQ(field.value("unit", std::string()), "cm");
            EXPECT_TRUE(field.value("resettable", false));
        }
        else if (name == "shape.height")
        {
            hasHeight = true;
            EXPECT_EQ(field.value("type", std::string()), "number");
            EXPECT_EQ(field.value("unit", std::string()), "cm");
            EXPECT_TRUE(field.value("resettable", false));
        }
        else if (name == "shape.innerRadius")
        {
            hasInnerRadius = true;
            EXPECT_EQ(field.value("type", std::string()), "number");
            EXPECT_EQ(field.value("unit", std::string()), "cm");
            EXPECT_TRUE(field.value("resettable", false));
        }
    }
    EXPECT_TRUE(hasType);
    EXPECT_TRUE(hasExtent);
    EXPECT_TRUE(hasHeight);
    EXPECT_TRUE(hasInnerRadius);
}

TEST(ParticleAsset, SpawnShapeRejectsInvalidTypeAndDimensions)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{{"type", "capsule"}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_shape_invalid_type.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "box"}, {"extent", {40.0, 0.0, 60.0}}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_shape_box_zero.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "box"}, {"extent", {40.0, -1.0, 60.0}}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_shape_box_negative.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "cone"}, {"height", 0.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_shape_cone_zero_height.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "cylinder"}, {"height", -5.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_shape_cylinder_negative_height.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "ring"},
        {"radius", 20.0},
        {"innerRadius", 20.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_shape_ring_equal_radius.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["shape"] = Json{
        {"type", "ring"},
        {"radius", 20.0},
        {"innerRadius", 30.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_shape_ring_inner_outside.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, NonIntegerCountsAreRejected)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["maxParticles"] = 128.5;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_fractional_count.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["flipbook"] = Json{
        {"columns", 1.5}, {"rows", 1}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_fractional_columns.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["flipbook"] = Json{
        {"columns", 1}, {"rows", 1}, {"startFrame", 0.5}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_fractional_start.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["flipbook"] = Json{
        {"columns", 1}, {"rows", 1},
        {"startFrame", 0}, {"endFrame", 0.5}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_fractional_end.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, CollisionDefaultsToNone)
{
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        MultiEmitterDocument(), "Maps/SC_test/PS_collision_default.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(asset.emitters[0].collisionType, "none");
    EXPECT_EQ(asset.emitters[0].collisionPlaneNormal,
        Eigen::Vector3f(0.0f, 1.0f, 0.0f));
    EXPECT_FLOAT_EQ(asset.emitters[0].collisionBounce, 0.3f);
    EXPECT_FLOAT_EQ(asset.emitters[0].collisionFriction, 0.2f);
    EXPECT_FLOAT_EQ(asset.emitters[0].collisionRadius, 0.0f);
    EXPECT_FALSE(asset.emitters[0].killOnCollision);
}

TEST(ParticleAsset, CollisionPlaneRoundTripsAndNormalizesNormal)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "plane"},
        {"plane", Json{{"normal", {0.0, 0.0, 2.0}},
            {"offset", 50.0}}},
        {"bounce", 0.65},
        {"friction", 0.15},
        {"radius", 2.5},
        {"killOnCollision", true}};

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        Json::parse(document.dump()),
        "Maps/SC_test/PS_collision_plane.json");

    const ParticleEmitterAsset& emitter = asset.emitters[0];
    EXPECT_EQ(emitter.collisionType, "plane");
    EXPECT_NEAR(emitter.collisionPlaneNormal.z(), 1.0f, 1.0e-6f);
    EXPECT_NEAR(emitter.collisionPlaneNormal.norm(), 1.0f, 1.0e-6f);
    EXPECT_FLOAT_EQ(emitter.collisionPlaneOffset, 50.0f);
    EXPECT_FLOAT_EQ(emitter.collisionBounce, 0.65f);
    EXPECT_FLOAT_EQ(emitter.collisionFriction, 0.15f);
    EXPECT_FLOAT_EQ(emitter.collisionRadius, 2.5f);
    EXPECT_TRUE(emitter.killOnCollision);
}

TEST(ParticleAsset, CollisionBoxRoundTripsThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "box"},
        {"box", Json{
            {"min", {-10.0, -20.0, -30.0}},
            {"max", {10.0, 20.0, 30.0}}}},
        {"bounce", 0.4},
        {"friction", 0.25}};

    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange change;
    change.field = "emitters.0.collision.box.max";
    change.value = std::array<double, 3>{12.0, 22.0, 32.0};
    changes.push_back(change);
    change.field = "emitters.0.collision.killOnCollision";
    change.value = true;
    changes.push_back(change);

    const Json changed = ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    EXPECT_EQ(changed.size(), 2u);

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_collision_box.json");
    const ParticleEmitterAsset& emitter = asset.emitters[0];
    EXPECT_EQ(emitter.collisionType, "box");
    EXPECT_FLOAT_EQ(emitter.collisionBoxMin.x(), -10.0f);
    EXPECT_FLOAT_EQ(emitter.collisionBoxMin.y(), -20.0f);
    EXPECT_FLOAT_EQ(emitter.collisionBoxMax.x(), 12.0f);
    EXPECT_FLOAT_EQ(emitter.collisionBoxMax.y(), 22.0f);
    EXPECT_FLOAT_EQ(emitter.collisionBoxMax.z(), 32.0f);
    EXPECT_TRUE(emitter.killOnCollision);
}

TEST(ParticleAsset, CollisionCapabilitiesAndProjectionExposeNestedFields)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "plane"},
        {"plane", Json{{"normal", {0.0, 1.0, 0.0}}, {"offset", 4.0}}},
        {"bounce", 0.5}};

    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const Json projection = ProjectEditableAsset(
        EditorAssetDocumentKind::ParticleSystem, document);
    bool foundNormal = false;
    for (const Json& field : capabilities["emitters"][0]["fields"])
    {
        if (field.value("field", std::string()) ==
            "collision.plane.normal")
        {
            foundNormal = true;
            EXPECT_EQ(field.value("type", std::string()), "vec3");
        }
    }
    EXPECT_TRUE(foundNormal);
    EXPECT_EQ(projection["emitters"][0]["collision"]["type"], "plane");
    EXPECT_DOUBLE_EQ(
        projection["emitters"][0]["collision"]["plane"]["offset"],
        4.0);
}

TEST(ParticleAsset, LegacyCollisionNestedFieldWritesAtRoot)
{
    Json document{
        {"name", "PS_legacy_collision"},
        {"type", "particleSystem"},
        {"maxParticles", 32}};
    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange change;
    change.field = "collision.type";
    change.value = std::string("plane");
    changes.push_back(change);
    change.field = "collision.plane.normal";
    change.value = std::array<double, 3>{0.0, 0.0, 1.0};
    changes.push_back(change);

    const Json changed = ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    EXPECT_EQ(changed.size(), 2u);
    EXPECT_EQ(document["collision"]["type"], "plane");
    ASSERT_TRUE(document["collision"]["plane"]["normal"].is_array());
    EXPECT_DOUBLE_EQ(
        document["collision"]["plane"]["normal"][2].get<double>(), 1.0);
    EXPECT_FALSE(document["collision"].contains("plane.normal"));

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_legacy_collision.json");
    EXPECT_EQ(asset.emitters[0].collisionType, "plane");
    EXPECT_FLOAT_EQ(asset.emitters[0].collisionPlaneNormal.z(), 1.0f);
}

TEST(ParticleAsset, CollisionRejectsInvalidPlaneNormal)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "plane"},
        {"plane", Json{{"normal", {0.0, 0.0, 0.0}},
            {"offset", 0.0}}}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_collision_zero_normal.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, CollisionRejectsInvalidBoxAndRanges)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "box"},
        {"box", Json{{"min", {0.0, 0.0, 0.0}},
            {"max", {-1.0, 1.0, 1.0}}}}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_collision_bad_box.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "plane"}, {"bounce", 1.1}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_collision_bounce.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "plane"}, {"friction", -0.1}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_collision_friction.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "plane"}, {"radius", -1.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_collision_radius.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, CollisionRejectsInvalidTypeAndNonFiniteValues)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "mesh"}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_collision_type.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "plane"},
        {"plane", Json{{"normal", {
            0.0,
            std::numeric_limits<float>::infinity(),
            1.0}},
            {"offset", 0.0}}}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_collision_inf_normal.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "box"},
        {"box", Json{{"min", {-1.0, -1.0, -1.0}},
            {"max", {1.0, 1.0, 1.0}}}},
        {"bounce", std::numeric_limits<float>::quiet_NaN()}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_collision_nan_bounce.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, CollisionCapsuleRoundTripsWithCcd)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "capsule"},
        {"capsule", Json{
            {"a", {0.0, -30.0, 10.0}},
            {"b", {0.0, 40.0, 10.0}},
            {"radius", 12.5}}},
        {"bounce", 0.6},
        {"friction", 0.1},
        {"radius", 2.0},
        {"ccd", true}};

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        Json::parse(document.dump()),
        "Maps/SC_test/PS_collision_capsule.json");

    const ParticleEmitterAsset& emitter = asset.emitters[0];
    EXPECT_EQ(emitter.collisionType, "capsule");
    EXPECT_FLOAT_EQ(emitter.collisionCapsuleA.x(), 0.0f);
    EXPECT_FLOAT_EQ(emitter.collisionCapsuleA.y(), -30.0f);
    EXPECT_FLOAT_EQ(emitter.collisionCapsuleA.z(), 10.0f);
    EXPECT_FLOAT_EQ(emitter.collisionCapsuleB.y(), 40.0f);
    EXPECT_FLOAT_EQ(emitter.collisionCapsuleRadius, 12.5f);
    EXPECT_FLOAT_EQ(emitter.collisionRadius, 2.0f);
    EXPECT_TRUE(emitter.collisionCcd);
}

TEST(ParticleAsset, CollisionCapsuleAndCcdRoundTripThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "capsule"},
        {"capsule", Json{
            {"a", {0.0, -10.0, 0.0}},
            {"b", {0.0, 10.0, 0.0}},
            {"radius", 20.0}}}};

    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange change;
    change.field = "emitters.0.collision.capsule.b";
    change.value = std::array<double, 3>{5.0, 60.0, -5.0};
    changes.push_back(change);
    change.field = "emitters.0.collision.capsule.radius";
    change.value = 8.0;
    changes.push_back(change);
    change.field = "emitters.0.collision.ccd";
    change.value = true;
    changes.push_back(change);

    const Json changed = ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    EXPECT_EQ(changed.size(), 3u);

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_collision_capsule_edit.json");
    const ParticleEmitterAsset& emitter = asset.emitters[0];
    EXPECT_EQ(emitter.collisionType, "capsule");
    EXPECT_FLOAT_EQ(emitter.collisionCapsuleB.x(), 5.0f);
    EXPECT_FLOAT_EQ(emitter.collisionCapsuleB.y(), 60.0f);
    EXPECT_FLOAT_EQ(emitter.collisionCapsuleB.z(), -5.0f);
    EXPECT_FLOAT_EQ(emitter.collisionCapsuleRadius, 8.0f);
    EXPECT_TRUE(emitter.collisionCcd);
}

TEST(ParticleAsset, CollisionCapsuleRejectsInvalidRadiusAndNonFiniteEndpoints)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "capsule"},
        {"capsule", Json{
            {"a", {0.0, -10.0, 0.0}},
            {"b", {0.0, 10.0, 0.0}},
            {"radius", -1.0}}}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_capsule_negative_radius.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["collision"] = Json{
        {"type", "capsule"},
        {"capsule", Json{
            {"a", {0.0, std::numeric_limits<float>::infinity(), 0.0}},
            {"b", {0.0, 10.0, 0.0}},
            {"radius", 5.0}}}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_capsule_inf_endpoint.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, CollisionCapabilitiesExposeCapsuleAndCcd)
{
    const Json document = MultiEmitterDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);

    bool foundType = false;
    bool foundCapsuleA = false;
    bool foundCapsuleB = false;
    bool foundCapsuleRadius = false;
    bool foundCcd = false;
    for (const Json& field : capabilities["emitters"][0]["fields"])
    {
        const std::string name = field.value("field", std::string());
        if (name == "collision.type")
        {
            foundType = true;
            const Json& values = field.at("enum");
            EXPECT_NE(std::find(values.begin(), values.end(), "capsule"),
                values.end());
        }
        else if (name == "collision.capsule.a") foundCapsuleA = true;
        else if (name == "collision.capsule.b") foundCapsuleB = true;
        else if (name == "collision.capsule.radius") foundCapsuleRadius = true;
        else if (name == "collision.ccd") foundCcd = true;
    }
    EXPECT_TRUE(foundType);
    EXPECT_TRUE(foundCapsuleA);
    EXPECT_TRUE(foundCapsuleB);
    EXPECT_TRUE(foundCapsuleRadius);
    EXPECT_TRUE(foundCcd);
}

TEST(ParticleAsset, LocalSpaceAndFixedBoundsParse)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["localSpace"] = true;
    document["emitters"][0]["fixedBounds"] = Json{
        {"center", {10.0, -20.0, 30.0}},
        {"extent", {40.0, 50.0, 60.0}}
    };

    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_space_bounds.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_TRUE(asset.emitters[0].localSpace);
    // 省略 useFixedBounds 时，fixedBounds 对象自动启用固定包围球。
    EXPECT_TRUE(asset.emitters[0].useFixedBounds);
    EXPECT_FLOAT_EQ(asset.emitters[0].fixedBoundsCenter.x(), 10.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].fixedBoundsCenter.y(), -20.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].fixedBoundsCenter.z(), 30.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].fixedBoundsExtent.x(), 40.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].fixedBoundsExtent.z(), 60.0f);
    EXPECT_FALSE(asset.emitters[1].localSpace);
    EXPECT_FALSE(asset.emitters[1].useFixedBounds);
}

TEST(ParticleAsset, ExplicitFalseDisablesFixedBounds)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["useFixedBounds"] = false;
    document["emitters"][0]["fixedBounds"] = Json{
        {"center", {1.0, 2.0, 3.0}},
        {"extent", {4.0, 5.0, 6.0}}
    };

    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_fixed_disabled.json");

    EXPECT_FALSE(asset.emitters[0].useFixedBounds);
    // 显式关闭只是不消费数据；作者字段仍然保留。
    EXPECT_FLOAT_EQ(asset.emitters[0].fixedBoundsExtent.y(), 5.0f);
}

TEST(ParticleAsset, LocalSpaceRejectsWorldCollision)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["localSpace"] = true;
    document["emitters"][0]["collision"] = Json{{"type", "plane"}};

    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_local_collision.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, FixedBoundsRejectsInvalidExtent)
{
    Json negative = MultiEmitterDocument();
    negative["emitters"][0]["useFixedBounds"] = true;
    negative["emitters"][0]["fixedBounds"] = Json{
        {"extent", {10.0, -1.0, 10.0}}
    };
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            negative, "Maps/SC_test/PS_bounds_negative.json"),
        std::invalid_argument);

    Json zero = MultiEmitterDocument();
    zero["emitters"][0]["useFixedBounds"] = true;
    zero["emitters"][0]["fixedBounds"] = Json{
        {"extent", {0.0, 0.0, 0.0}}
    };
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            zero, "Maps/SC_test/PS_bounds_zero.json"),
        std::invalid_argument);

    Json nonFinite = MultiEmitterDocument();
    nonFinite["emitters"][0]["useFixedBounds"] = true;
    nonFinite["emitters"][0]["fixedBounds"] = Json{
        {"center", {0.0, "bad", 0.0}}
    };
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            nonFinite, "Maps/SC_test/PS_bounds_center.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, EditorPathsWriteLocalSpaceAndFixedBounds)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;

    EditorAssetFieldChange localSpace;
    localSpace.field = "emitters.0.localSpace";
    localSpace.value = true;
    changes.push_back(localSpace);

    EditorAssetFieldChange fixedBounds;
    fixedBounds.field = "emitters.1.useFixedBounds";
    fixedBounds.value = true;
    changes.push_back(fixedBounds);

    EditorAssetFieldChange extent;
    extent.field = "emitters.1.fixedBounds.extent";
    extent.value = std::array<double, 3>{20.0, 30.0, 40.0};
    changes.push_back(extent);

    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_editor_space_bounds.json");

    EXPECT_TRUE(asset.emitters[0].localSpace);
    EXPECT_TRUE(asset.emitters[1].useFixedBounds);
    EXPECT_FLOAT_EQ(asset.emitters[1].fixedBoundsExtent.y(), 30.0f);
}

TEST(ParticleAsset, PrepareOpenFailureIncludesSourcePath)
{
    const std::string path =
        "Maps/SC_test/PS_missing_for_error_context.json";
    try
    {
        (void)ParticleAssetLoader::Prepare(path);
        FAIL() << "Expected Prepare to reject a missing asset";
    }
    catch (const std::runtime_error& exception)
    {
        const std::string message = exception.what();
        EXPECT_NE(message.find(path), std::string::npos) << message;
        EXPECT_NE(message.find("Failed to open"), std::string::npos)
            << message;
    }
}

TEST(ParticleAsset, PrepareParseFailureIncludesSourcePathAndReason)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "vl_particle_invalid_syntax.json";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "{\"type\": \"particleSystem\",";
    }
    const std::string source = path.generic_string();
    try
    {
        (void)ParticleAssetLoader::Prepare(source);
        std::filesystem::remove(path);
        FAIL() << "Expected Prepare to reject malformed JSON";
    }
    catch (const std::runtime_error& exception)
    {
        std::filesystem::remove(path);
        const std::string message = exception.what();
        EXPECT_NE(message.find(source), std::string::npos) << message;
        EXPECT_NE(message.find("Failed to parse"), std::string::npos)
            << message;
        EXPECT_NE(message.find("parse error"), std::string::npos)
            << message;
    }
}

TEST(ParticleAsset, RibbonFieldsParseAndRoundTrip)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "ribbon";
    document["emitters"][0]["ribbonWidth"] = 3.0;
    document["emitters"][0]["ribbonSegments"] = 2;
    document["emitters"][0]["ribbonFade"] = 0.8;

    const Json roundTripped = Json::parse(document.dump());
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            roundTripped, "Maps/SC_test/PS_ribbon.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(asset.emitters[0].renderMode, "ribbon");
    EXPECT_FLOAT_EQ(asset.emitters[0].ribbonWidth, 3.0f);
    EXPECT_EQ(asset.emitters[0].ribbonSegments, 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].ribbonFade, 0.8f);
    EXPECT_EQ(asset.emitters[1].renderMode, "billboard");
}

TEST(ParticleAsset, RibbonFieldsDefaultToBillboard)
{
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            MultiEmitterDocument(),
            "Maps/SC_test/PS_ribbon_defaults.json");

    EXPECT_EQ(asset.emitters[0].renderMode, "billboard");
    EXPECT_FLOAT_EQ(asset.emitters[0].ribbonWidth, 5.0f);
    EXPECT_EQ(asset.emitters[0].ribbonSegments, 3u);
    EXPECT_FLOAT_EQ(asset.emitters[0].ribbonFade, 1.0f);
}

TEST(ParticleAsset, RibbonFieldsRoundTripThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    const auto addChange = [&changes](
        const char* field,
        EditorAssetFieldValue value)
    {
        EditorAssetFieldChange change;
        change.field = field;
        change.value = std::move(value);
        changes.push_back(std::move(change));
    };
    addChange("emitters.1.renderMode", std::string("ribbon"));
    addChange("emitters.1.ribbonWidth", 7.5);
    addChange("emitters.1.ribbonSegments", 1.0);
    addChange("emitters.1.ribbonFade", 0.25);

    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_ribbon_editor.json");

    EXPECT_EQ(asset.emitters[1].renderMode, "ribbon");
    EXPECT_FLOAT_EQ(asset.emitters[1].ribbonWidth, 7.5f);
    EXPECT_EQ(asset.emitters[1].ribbonSegments, 1u);
    EXPECT_FLOAT_EQ(asset.emitters[1].ribbonFade, 0.25f);
    EXPECT_EQ(asset.emitters[0].renderMode, "billboard");
}

TEST(ParticleAsset, RibbonFieldsRejectInvalidValues)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "trail";
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_ribbon_mode.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["ribbonWidth"] = 0.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_ribbon_width.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["ribbonSegments"] = 4;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_ribbon_segments.json"),
        std::invalid_argument);

    // 计数字段必须是整数，不能静默把 2.5 截断成 2。
    document = MultiEmitterDocument();
    document["emitters"][0]["ribbonSegments"] = 2.5;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_ribbon_fraction.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["ribbonFade"] = 1.5;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_ribbon_fade.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, CapabilitiesExposeRibbonFields)
{
    const Json document = MultiEmitterDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const auto hasField = [&capabilities](const char* name)
    {
        for (const Json& field : capabilities["emitters"][0]["fields"])
        {
            if (field.value("field", std::string()) == name)
                return true;
        }
        return false;
    };

    EXPECT_TRUE(hasField("renderMode"));
    EXPECT_TRUE(hasField("ribbonWidth"));
    EXPECT_TRUE(hasField("ribbonSegments"));
    EXPECT_TRUE(hasField("ribbonFade"));
}

TEST(ParticleAsset, BillboardAlignmentDefaultsToCamera)
{
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            MultiEmitterDocument(),
            "Maps/SC_test/PS_billboard_default.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(asset.emitters[0].billboardAlignment, "camera");
    EXPECT_EQ(asset.emitters[1].billboardAlignment, "camera");
}

TEST(ParticleAsset, BillboardAlignmentVelocityRoundTripsThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange change;
    change.field = "emitters.0.billboardAlignment";
    change.value = std::string("velocity");
    changes.push_back(change);

    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            Json::parse(document.dump()),
            "Maps/SC_test/PS_billboard_velocity.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(asset.emitters[0].billboardAlignment, "velocity");
    EXPECT_EQ(asset.emitters[1].billboardAlignment, "camera");
}

TEST(ParticleAsset, BillboardAlignmentRejectsInvalidValue)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["billboardAlignment"] = "normal";

    try
    {
        (void)ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_invalid.json");
        FAIL() << "billboardAlignment outside camera|velocity must be rejected";
    }
    catch (const std::invalid_argument& exception)
    {
        EXPECT_NE(
            std::string(exception.what()).find("billboardAlignment"),
            std::string::npos);
    }
}

TEST(ParticleAsset, CapabilitiesAndProjectionExposeBillboardAlignment)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["billboardAlignment"] = "velocity";
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const Json projection = ProjectEditableAsset(
        EditorAssetDocumentKind::ParticleSystem, document);

    const Json* alignmentField = nullptr;
    for (const Json& field : capabilities["emitters"][0]["fields"])
        if (field.value("field", std::string()) == "billboardAlignment")
            alignmentField = &field;

    ASSERT_NE(alignmentField, nullptr);
    EXPECT_EQ(alignmentField->value("type", std::string()), "string");
    EXPECT_TRUE(alignmentField->value("resettable", false));
    ASSERT_TRUE(alignmentField->contains("enum"));
    EXPECT_EQ(alignmentField->at("enum"), Json::array({"camera", "velocity"}));
    EXPECT_EQ(
        projection["emitters"][0]["billboardAlignment"], "velocity");
}

TEST(ParticleAsset, MeshFieldsParseAndRoundTrip)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["mesh"] = Json{
        {"path", "Common/Meshes/SM_cube.json"},
        {"scale", {2.0, 3.0, 4.0}},
        {"alignment", "velocity"}};

    const Json roundTripped = Json::parse(document.dump());
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            roundTripped, "Maps/SC_test/PS_mesh.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(asset.emitters[0].renderMode, "mesh");
    EXPECT_EQ(
        asset.emitters[0].meshPath,
        "Common/Meshes/SM_cube.json");
    EXPECT_FLOAT_EQ(asset.emitters[0].meshScale.x(), 2.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].meshScale.y(), 3.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].meshScale.z(), 4.0f);
    EXPECT_EQ(asset.emitters[0].meshAlignment, "velocity");
    EXPECT_EQ(asset.emitters[1].renderMode, "billboard");
    EXPECT_TRUE(asset.emitters[1].meshPath.empty());
}

TEST(ParticleAsset, MeshFieldsRoundTripThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    const auto addChange = [&changes](
        const char* field,
        EditorAssetFieldValue value)
    {
        EditorAssetFieldChange change;
        change.field = field;
        change.value = std::move(value);
        changes.push_back(std::move(change));
    };
    addChange("emitters.1.renderMode", std::string("mesh"));
    addChange(
        "emitters.1.mesh.path",
        std::string("Common/Meshes/SM_cube.json"));
    addChange(
        "emitters.1.mesh.scale",
        std::array<double, 3>{0.5, 1.5, 2.5});
    addChange(
        "emitters.1.mesh.alignment",
        std::string("camera"));

    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_editor.json");

    EXPECT_EQ(asset.emitters[1].renderMode, "mesh");
    EXPECT_EQ(
        asset.emitters[1].meshPath,
        "Common/Meshes/SM_cube.json");
    EXPECT_FLOAT_EQ(asset.emitters[1].meshScale.x(), 0.5f);
    EXPECT_FLOAT_EQ(asset.emitters[1].meshScale.y(), 1.5f);
    EXPECT_FLOAT_EQ(asset.emitters[1].meshScale.z(), 2.5f);
    EXPECT_EQ(asset.emitters[1].meshAlignment, "camera");
}

TEST(ParticleAsset, MeshFieldsRejectInvalidConfiguration)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_missing.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["mesh"] = Json{
        {"path", "../SM_cube.json"}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_traversal.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["mesh"] = Json{
        {"path", "C:/Meshes/SM_cube.json"}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_absolute.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["mesh"] = Json{
        {"path", "Common/Meshes/cube.obj"}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_extension.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["mesh"] = Json{
        {"path", "Common/Meshes/SM_cube.json"},
        {"scale", {0.0, 0.0, 0.0}}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_zero_scale.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["mesh"] = Json{
        {"path", "Common/Meshes/SM_cube.json"},
        {"alignment", "normal"}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_alignment.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, MeshRejectsLocalSpace)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["localSpace"] = true;
    document["emitters"][0]["mesh"] = Json{
        {"path", "Common/Meshes/SM_cube.json"}};

    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_local_space.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, CapabilitiesExposeMeshFields)
{
    const Json document = MultiEmitterDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const auto hasField = [&capabilities](const char* name)
    {
        for (const Json& field : capabilities["emitters"][0]["fields"])
        {
            if (field.value("field", std::string()) == name)
                return true;
        }
        return false;
    };

    EXPECT_TRUE(hasField("mesh.path"));
    EXPECT_TRUE(hasField("mesh.scale"));
    EXPECT_TRUE(hasField("mesh.alignment"));
}

TEST(ParticleAsset, ForcesDefaultToDisabledNeutralValues)
{
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        MultiEmitterDocument(), "Maps/SC_test/PS_forces_default.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    const ParticleEmitterAsset& emitter = asset.emitters[0];
    EXPECT_FLOAT_EQ(emitter.curlNoiseStrength, 0.0f);
    EXPECT_FLOAT_EQ(emitter.curlNoiseFrequency, 0.1f);
    EXPECT_FLOAT_EQ(emitter.curlNoiseSpeed, 0.0f);
    EXPECT_FLOAT_EQ(emitter.vortexStrength, 0.0f);
    EXPECT_FLOAT_EQ(emitter.vortexRadius, 50.0f);
    EXPECT_EQ(emitter.vortexAxis, Eigen::Vector3f(0.0f, 1.0f, 0.0f));
    EXPECT_EQ(emitter.attractorPosition, Eigen::Vector3f::Zero());
    EXPECT_FLOAT_EQ(emitter.attractorStrength, 0.0f);
    EXPECT_FLOAT_EQ(emitter.attractorRadius, 100.0f);
    EXPECT_EQ(emitter.wind, Eigen::Vector3f::Zero());
}

TEST(ParticleAsset, ForcesParseWithTheirParameters)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["curlNoise"] = Json{
        {"strength", 12.5},
        {"frequency", 0.25},
        {"speed", 3.0}};
    document["emitters"][0]["vortex"] = Json{
        {"strength", 20.0},
        {"radius", 75.0},
        {"axis", {0.0, 0.0, 2.0}}};
    document["emitters"][0]["attractor"] = Json{
        {"position", {10.0, 20.0, 30.0}},
        {"strength", 15.0},
        {"radius", 110.0}};
    document["emitters"][0]["wind"] = Json{1.0, 2.0, 3.0};

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_forces.json");
    const ParticleEmitterAsset& emitter = asset.emitters[0];
    EXPECT_FLOAT_EQ(emitter.curlNoiseStrength, 12.5f);
    EXPECT_FLOAT_EQ(emitter.curlNoiseFrequency, 0.25f);
    EXPECT_FLOAT_EQ(emitter.curlNoiseSpeed, 3.0f);
    EXPECT_FLOAT_EQ(emitter.vortexStrength, 20.0f);
    EXPECT_FLOAT_EQ(emitter.vortexRadius, 75.0f);
    EXPECT_EQ(emitter.vortexAxis, Eigen::Vector3f(0.0f, 0.0f, 2.0f));
    EXPECT_EQ(
        emitter.attractorPosition,
        Eigen::Vector3f(10.0f, 20.0f, 30.0f));
    EXPECT_FLOAT_EQ(emitter.attractorStrength, 15.0f);
    EXPECT_FLOAT_EQ(emitter.attractorRadius, 110.0f);
    EXPECT_EQ(emitter.wind, Eigen::Vector3f(1.0f, 2.0f, 3.0f));
}

TEST(ParticleAsset, ForcesRoundTripThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    const auto addChange = [&changes](
        const char* field,
        auto value)
    {
        EditorAssetFieldChange change;
        change.field = field;
        change.value = std::move(value);
        changes.push_back(std::move(change));
    };
    addChange("emitters.0.curlNoise.strength", 8.0);
    addChange("emitters.0.curlNoise.frequency", 0.5);
    addChange("emitters.0.curlNoise.speed", 2.0);
    addChange("emitters.0.vortex.strength", 9.0);
    addChange("emitters.0.vortex.radius", 60.0);
    addChange("emitters.0.vortex.axis",
        std::array<double, 3>{0.0, 0.0, 1.0});
    addChange("emitters.0.attractor.position",
        std::array<double, 3>{3.0, 4.0, 5.0});
    addChange("emitters.0.attractor.strength", 7.0);
    addChange("emitters.0.attractor.radius", 90.0);
    addChange("emitters.0.wind",
        std::array<double, 3>{0.5, 0.0, -1.5});

    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_forces_editor.json");
    const ParticleEmitterAsset& emitter = asset.emitters[0];
    EXPECT_FLOAT_EQ(emitter.curlNoiseStrength, 8.0f);
    EXPECT_FLOAT_EQ(emitter.curlNoiseFrequency, 0.5f);
    EXPECT_FLOAT_EQ(emitter.curlNoiseSpeed, 2.0f);
    EXPECT_FLOAT_EQ(emitter.vortexStrength, 9.0f);
    EXPECT_FLOAT_EQ(emitter.vortexRadius, 60.0f);
    EXPECT_EQ(emitter.vortexAxis, Eigen::Vector3f(0.0f, 0.0f, 1.0f));
    EXPECT_EQ(
        emitter.attractorPosition,
        Eigen::Vector3f(3.0f, 4.0f, 5.0f));
    EXPECT_FLOAT_EQ(emitter.attractorStrength, 7.0f);
    EXPECT_FLOAT_EQ(emitter.attractorRadius, 90.0f);
    EXPECT_EQ(emitter.wind, Eigen::Vector3f(0.5f, 0.0f, -1.5f));
}

TEST(ParticleAsset, ForcesCapabilitiesExposeFields)
{
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, MultiEmitterDocument());
    const auto findField = [&capabilities](const char* name) -> Json
    {
        for (const Json& field : capabilities["emitters"][0]["fields"])
        {
            if (field.value("field", std::string()) == name)
                return field;
        }
        return Json();
    };

    const Json curlStrength = findField("curlNoise.strength");
    ASSERT_TRUE(curlStrength.is_object());
    EXPECT_EQ(curlStrength.value("type", std::string()), "number");
    EXPECT_EQ(curlStrength.value("unit", std::string()), "cm/s^2");
    EXPECT_TRUE(curlStrength.value("resettable", false));
    const Json curlFrequency = findField("curlNoise.frequency");
    ASSERT_TRUE(curlFrequency.is_object());
    EXPECT_EQ(curlFrequency.value("unit", std::string()), "1/cm");
    const Json curlSpeed = findField("curlNoise.speed");
    ASSERT_TRUE(curlSpeed.is_object());
    EXPECT_EQ(curlSpeed.value("unit", std::string()), "1/s");

    const Json vortexStrength = findField("vortex.strength");
    ASSERT_TRUE(vortexStrength.is_object());
    EXPECT_EQ(vortexStrength.value("type", std::string()), "number");
    EXPECT_EQ(vortexStrength.value("unit", std::string()), "cm/s^2");
    const Json vortexRadius = findField("vortex.radius");
    ASSERT_TRUE(vortexRadius.is_object());
    EXPECT_EQ(vortexRadius.value("unit", std::string()), "cm");
    const Json vortexAxis = findField("vortex.axis");
    ASSERT_TRUE(vortexAxis.is_object());
    EXPECT_EQ(vortexAxis.value("type", std::string()), "vec3");
    const Json attractorPosition = findField("attractor.position");
    ASSERT_TRUE(attractorPosition.is_object());
    EXPECT_EQ(attractorPosition.value("type", std::string()), "vec3");
    EXPECT_EQ(attractorPosition.value("unit", std::string()), "cm");
    const Json attractorStrength = findField("attractor.strength");
    ASSERT_TRUE(attractorStrength.is_object());
    EXPECT_EQ(attractorStrength.value("type", std::string()), "number");
    EXPECT_EQ(attractorStrength.value("unit", std::string()), "cm/s^2");
    const Json attractorRadius = findField("attractor.radius");
    ASSERT_TRUE(attractorRadius.is_object());
    EXPECT_EQ(attractorRadius.value("type", std::string()), "number");
    EXPECT_EQ(attractorRadius.value("unit", std::string()), "cm");
    const Json wind = findField("wind");
    ASSERT_TRUE(wind.is_object());
    EXPECT_EQ(wind.value("type", std::string()), "vec3");
    EXPECT_EQ(wind.value("unit", std::string()), "cm/s^2");
}

TEST(ParticleAsset, ForcesRejectInvalidRangesAndNonFiniteValues)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity =
        std::numeric_limits<float>::infinity();

    Json document = MultiEmitterDocument();
    document["emitters"][0]["curlNoise"] = Json{
        {"strength", -1.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_curl_negative.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["curlNoise"] = Json{
        {"frequency", -0.1}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_curl_frequency.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["curlNoise"] = Json{
        {"speed", infinity}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_curl_speed_inf.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["vortex"] = Json{
        {"strength", -1.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_vortex_negative.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["vortex"] = Json{
        {"radius", -1.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_vortex_radius.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["vortex"] = Json{
        {"strength", 1.0},
        {"axis", {0.0, 0.0, 0.0}}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_vortex_zero_axis.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["attractor"] = Json{
        {"strength", -1.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_attractor_negative.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["attractor"] = Json{
        {"strength", 1.0},
        {"radius", 0.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_attractor_zero_radius.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["wind"] = Json{nan, 0.0, 0.0};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_wind_nan.json"),
        std::invalid_argument);
}
