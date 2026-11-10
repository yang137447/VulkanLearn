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
#include "particles/particleEmitterBounds.h"
#include "particles/particleLifetime.h"
#include "particles/particleShadowCaster.h"
#include "render/rendergraph/renderGraphPassType.h"

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

Json ParticleLightDocument()
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "light";
    document["emitters"][0]["maxParticles"] = 64;
    document["emitters"][0]["light"] = Json{
        {"intensity", 300.0},
        {"radius", 700.0}};
    return document;
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

TEST(ParticleAsset, MeshCastShadowsParsesBooleanAndDefaultsFalse)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["mesh"] = Json{
        {"path", "Maps/SC_test/Meshes/SM_debris.json"}};
    document["emitters"][1]["renderMode"] = "mesh";
    document["emitters"][1]["mesh"] = Json{
        {"path", "Maps/SC_test/Meshes/SM_chip.json"}};
    document["emitters"][1]["castShadows"] = true;

    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_caster.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_FALSE(asset.emitters.at(0).castShadows);
    EXPECT_TRUE(asset.emitters.at(1).castShadows);
    EXPECT_TRUE(VL::ParticleEmitterCastsMeshShadow(asset.emitters.at(1)));

    Json invalid = document;
    invalid["emitters"][0]["castShadows"] = 1.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_caster_invalid.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, EmitterSortOrderParsesAndValidatesRange)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["sortOrder"] = -7;
    document["emitters"][1]["sortOrder"] = 1000;
    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_sort_order.json");
    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(asset.emitters.at(0).sortOrder, -7);
    EXPECT_EQ(asset.emitters.at(1).sortOrder, 1000);

    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    Json sortOrderField;
    for (const Json& field : capabilities["emitters"][0]["fields"])
    {
        if (field.value("field", std::string()) == "sortOrder")
        {
            sortOrderField = field;
        }
    }
    ASSERT_TRUE(sortOrderField.is_object());
    EXPECT_EQ(sortOrderField.value("type", std::string()), "number");
    EXPECT_TRUE(sortOrderField.value("integer", false));
    EXPECT_DOUBLE_EQ(sortOrderField.value("minimum", 0.0), -1000.0);
    EXPECT_DOUBLE_EQ(sortOrderField.value("maximum", 0.0), 1000.0);

    // 小数不能静默截断，越界也不能留到运行时才发现。
    document["emitters"][0]["sortOrder"] = 1.5;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_sort_order_fraction.json"),
        std::invalid_argument);
    document["emitters"][0]["sortOrder"] = -1001;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_sort_order_low.json"),
        std::invalid_argument);
    document["emitters"][0]["sortOrder"] = 1001;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_sort_order_high.json"),
        std::invalid_argument);
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

TEST(ParticleAsset, V2RendererAdditiveControlsBlend)
{
    Json document = V2ModuleDocument();
    document["emitters"][0]["modules"][0]["fields"]["sortParticles"] = true;
    // 根级 blend 仍可作者化；renderer.additive 必须优先于它。
    document["emitters"][0]["blend"] = "additive";
    document["emitters"][0]["modules"].push_back(Json{
        {"type", "renderer"},
        {"fields", {{"additive", false}}}});
    const ParticleSystemAsset alpha =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_v2_alpha.json");
    EXPECT_FALSE(alpha.emitters.front().additive);

    document["emitters"][0]["blend"] = "alpha";
    document["emitters"][0]["modules"].back()["fields"]["additive"] = true;
    const ParticleSystemAsset additive =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_v2_additive.json");
    EXPECT_TRUE(additive.emitters.front().additive);
}

TEST(ParticleAsset, V2EmitterModuleCarriesSortOrder)
{
    // sortParticles 与 sortOrder 同属 emitter 模块；展平后运行时只看到
    // 根级字段，因此 v2 作者层不能被遗漏。
    Json document = V2ModuleDocument();
    document["emitters"][0]["modules"][0]["fields"]["sortOrder"] = 12;
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_v2_sort_order.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_EQ(asset.emitters.front().sortOrder, 12);
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

TEST(ParticleAsset, BurstCountAndSpawnPerUnitParseAndValidate)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["burst"] = true;
    document["emitters"][0]["burstCount"] = 64;
    document["emitters"][0]["spawnPerUnit"] = 0.5;
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_burst_count.json");
    EXPECT_EQ(asset.emitters[0].burstCount, 64u);
    EXPECT_FLOAT_EQ(asset.emitters[0].spawnPerUnit, 0.5f);
    EXPECT_EQ(asset.emitters[1].burstCount, 0u);
    EXPECT_FLOAT_EQ(asset.emitters[1].spawnPerUnit, 0.0f);

    const uint32_t maxParticles = asset.emitters[0].maxParticles;
    Json invalid = MultiEmitterDocument();
    invalid["emitters"][0]["burstCount"] = maxParticles + 1;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_burst_count_too_large.json"),
        std::invalid_argument);

    invalid = MultiEmitterDocument();
    invalid["emitters"][0]["spawnPerUnit"] = -1.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_spawn_unit_negative.json"),
        std::invalid_argument);

    invalid["emitters"][0]["spawnPerUnit"] = 100001.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_spawn_unit_too_large.json"),
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
            Json{{"distance", 3000.0}, {"spawnRateScale", 0.25},
                 {"renderScale", 0.5}, {"fadeRange", 400.0}}
        })}};

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_lod_tiers.json");
    ASSERT_EQ(asset.emitters[0].lodTiers.size(), 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[0].distance, 1000.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[0].spawnRateScale, 0.6f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[0].renderScale, 1.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[0].fadeRange, 0.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[1].distance, 3000.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[1].spawnRateScale, 0.25f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[1].renderScale, 0.5f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lodTiers[1].fadeRange, 400.0f);
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

    document["emitters"][0]["lod"]["tiers"] = Json::array({
        Json{{"distance", 1000.0}, {"spawnRateScale", 0.5},
             {"renderScale", 4.5}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lod_render_scale.json"),
        std::invalid_argument);

    document["emitters"][0]["lod"]["tiers"] = Json::array({
        Json{{"distance", 1000.0}, {"spawnRateScale", 0.5},
             {"fadeRange", -1.0}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lod_fade_range.json"),
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

TEST(ParticleAsset, LocalSpaceAcceptsCollisionInLocalSpace)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["localSpace"] = true;
    document["emitters"][0]["collision"] = Json{{"type", "plane"}};

    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_local_collision.json");
    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_TRUE(asset.emitters[0].localSpace);
    EXPECT_EQ(asset.emitters[0].collisionType, "plane");
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

TEST(ParticleAsset, DistortionModeParsesStrengthAndDefaults)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "distortion";

    ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_distortion_defaults.json");
    EXPECT_EQ(asset.emitters[0].renderMode, "distortion");
    EXPECT_FLOAT_EQ(asset.emitters[0].distortionStrength, 0.02f);

    document["emitters"][0]["distortionStrength"] = 0.08;
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_distortion_custom.json");
    EXPECT_FLOAT_EQ(asset.emitters[0].distortionStrength, 0.08f);
    // distortion 走独立加法缓冲，不要求 alpha billboard 的排序开关。
    EXPECT_FALSE(asset.emitters[0].sortParticles);
}

TEST(RenderGraphPassType, DistortionNameIsRegistered)
{
    const auto parsed = ParseRenderGraphPassType("distortion");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, RenderGraphPassType::Distortion);
    EXPECT_EQ(RenderGraphPassTypeToString(*parsed), "distortion");
    // 表里必须保留 particle，distortion 是追加而不是替换。
    const auto particle = ParseRenderGraphPassType("particle");
    ASSERT_TRUE(particle.has_value());
    EXPECT_EQ(*particle, RenderGraphPassType::Particle);
}

TEST(ParticleAsset, DistortionStrengthRejectsOutOfRange)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "distortion";
    document["emitters"][0]["distortionStrength"] = -0.01;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_distortion_negative.json"),
        std::invalid_argument);

    document["emitters"][0]["distortionStrength"] = 0.251;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_distortion_large.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, DistortionFieldsRoundTripThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange mode;
    mode.field = "emitters.1.renderMode";
    mode.value = std::string("distortion");
    changes.push_back(std::move(mode));
    EditorAssetFieldChange strength;
    strength.field = "emitters.1.distortionStrength";
    strength.value = 0.05;
    changes.push_back(std::move(strength));

    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_distortion_editor.json");

    EXPECT_EQ(asset.emitters[1].renderMode, "distortion");
    EXPECT_FLOAT_EQ(asset.emitters[1].distortionStrength, 0.05f);
    EXPECT_EQ(asset.emitters[0].renderMode, "billboard");
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
    document["emitters"][0]["ribbonSegments"] = 33;
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

TEST(ParticleAsset, SubUvSoftParticleAndEooFieldsRoundTrip)
{
    Json document = MultiEmitterDocument();
    Json& emitter = document["emitters"][0];
    emitter["flipbook"] = Json{
        {"perParticle", true},
        {"blend", true},
        {"playRate", 1.5}};
    emitter["softParticleTapRadius"] = 6.0;
    emitter["billboardDensity"] = 0.01;
    emitter["eooPacked"] = true;
    emitter["eooEmissiveStrength"] = 2.5;
    emitter["ribbonUVTiling"] = 3.0;
    emitter["ribbonWidthKeys"] = Json::array({
        Json{{"time", 0.0}, {"width", 1.0}},
        Json{{"time", 0.5}, {"width", 0.4}},
        Json{{"time", 1.0}, {"width", 0.05}}});

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_subuv_soft_eoo.json");
    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_TRUE(asset.emitters[0].flipbookPerParticle);
    EXPECT_TRUE(asset.emitters[0].flipbookBlend);
    EXPECT_FLOAT_EQ(asset.emitters[0].flipbookPlayRate, 1.5f);
    EXPECT_FLOAT_EQ(asset.emitters[0].softParticleTapRadius, 6.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].billboardDensity, 0.01f);
    EXPECT_TRUE(asset.emitters[0].eooPacked);
    EXPECT_FLOAT_EQ(asset.emitters[0].eooEmissiveStrength, 2.5f);
    EXPECT_FLOAT_EQ(asset.emitters[0].ribbonUVTiling, 3.0f);
    ASSERT_EQ(asset.emitters[0].ribbonWidthKeys.size(), 3u);
    EXPECT_FLOAT_EQ(
        asset.emitters[0].ribbonWidthKeys[1].width, 0.4f);

    Json invalid = MultiEmitterDocument();
    invalid["emitters"][0]["flipbook"] = Json{
        {"perParticle", true}, {"playRate", 17.0}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_subuv_rate.json"),
        std::invalid_argument);

    invalid = MultiEmitterDocument();
    invalid["emitters"][0]["softParticleTapRadius"] = 33.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_soft_radius.json"),
        std::invalid_argument);

    invalid = MultiEmitterDocument();
    invalid["emitters"][0]["billboardDensity"] = 0.3;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_density.json"),
        std::invalid_argument);

    invalid = MultiEmitterDocument();
    invalid["emitters"][0]["eooEmissiveStrength"] = 17.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_eoo_strength.json"),
        std::invalid_argument);

    invalid = MultiEmitterDocument();
    invalid["emitters"][0]["ribbonUVTiling"] = 0.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_ribbon_uv.json"),
        std::invalid_argument);

    invalid = MultiEmitterDocument();
    invalid["emitters"][0]["ribbonWidthKeys"] = Json::array({
        Json{{"time", 0.5}, {"width", 1.0}},
        Json{{"time", 0.2}, {"width", 0.5}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            invalid, "Maps/SC_test/PS_ribbon_width_keys.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, BeamFieldsDefaultAndParseCustomValues)
{
    Json document = MultiEmitterDocument();
    const ParticleSystemAsset defaults =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_beam_defaults.json");
    ASSERT_EQ(defaults.emitters.size(), 2u);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.start.x(), -200.0f);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.end.x(), 200.0f);
    EXPECT_EQ(defaults.emitters[0].beam.segments, 16u);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.width, 3.0f);
    EXPECT_EQ(defaults.emitters[0].beam.count, 4u);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.noiseAmplitude, 18.0f);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.noiseFrequency, 3.0f);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.noiseSpeed, 5.0f);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.endpointJitter, 0.0f);
    EXPECT_EQ(defaults.emitters[0].beam.branchCount, 0u);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.branchStart, 0.5f);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.branchLength, 0.3f);
    EXPECT_FLOAT_EQ(defaults.emitters[0].beam.branchSpread, 20.0f);
    EXPECT_TRUE(defaults.emitters[0].beam.colorKeys.empty());
    EXPECT_TRUE(defaults.emitters[0].beam.widthKeys.empty());
    EXPECT_TRUE(defaults.emitters[0].beam.arcs.empty());

    document["emitters"][0]["renderMode"] = "beam";
    document["emitters"][0]["beam"] = Json{
        {"start", {-1.0, 2.0, 3.0}},
        {"end", {4.0, 5.0, 6.0}},
        {"segments", 24},
        {"width", 2.5},
        {"count", 2},
        {"noiseAmplitude", 25.0},
        {"noiseFrequency", 4.0},
        {"noiseSpeed", 6.0},
        {"endpointJitter", 7.0},
        {"branchCount", 2},
        {"branchStart", 0.45},
        {"branchLength", 0.35},
        {"branchSpread", 33.0}};
    document["emitters"][0]["beam"]["colorKeys"] = Json::array({
        Json{{"time", 0.0}, {"color", {1.0, 2.0, 3.0, 1.0}}},
        Json{{"time", 0.5}, {"color", {0.5, 1.0, 2.0, 0.8}}},
        Json{{"time", 1.0}, {"color", {0.1, 0.2, 0.3, 0.0}}}});
    document["emitters"][0]["beam"]["widthKeys"] = Json::array({
        Json{{"time", 0.0}, {"width", 0.25}},
        Json{{"time", 0.7}, {"width", 1.0}},
        Json{{"time", 1.0}, {"width", 0.1}}});
    document["emitters"][0]["beam"]["arcs"] = Json::array({
        Json{{"colorStart", {1.0, 0.7, 0.2}},
            {"colorEnd", {0.4, 0.1, 0.0}},
            {"widthScale", 0.8},
            {"intensity", 1.5},
            {"noisePhase", 1.2}},
        Json{{"colorStart", {0.2, 0.7, 1.0}},
            {"colorEnd", {0.0, 0.1, 0.4}},
            {"widthScale", 0.5},
            {"intensity", 0.75},
            {"noisePhase", -2.4}}});
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_beam_custom.json");

    EXPECT_EQ(asset.emitters[0].renderMode, "beam");
    EXPECT_EQ(asset.emitters[0].beam.start,
        Eigen::Vector3f(-1.0f, 2.0f, 3.0f));
    EXPECT_EQ(asset.emitters[0].beam.end,
        Eigen::Vector3f(4.0f, 5.0f, 6.0f));
    EXPECT_EQ(asset.emitters[0].beam.segments, 24u);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.width, 2.5f);
    EXPECT_EQ(asset.emitters[0].beam.count, 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.noiseAmplitude, 25.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.noiseFrequency, 4.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.noiseSpeed, 6.0f);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.endpointJitter, 7.0f);
    EXPECT_EQ(asset.emitters[0].beam.branchCount, 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.branchStart, 0.45f);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.branchLength, 0.35f);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.branchSpread, 33.0f);
    ASSERT_EQ(asset.emitters[0].beam.colorKeys.size(), 3u);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.colorKeys[1].time, 0.5f);
    EXPECT_EQ(asset.emitters[0].beam.colorKeys[1].color,
        Eigen::Vector4f(0.5f, 1.0f, 2.0f, 0.8f));
    ASSERT_EQ(asset.emitters[0].beam.widthKeys.size(), 3u);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.widthKeys[1].width, 1.0f);
    ASSERT_EQ(asset.emitters[0].beam.arcs.size(), 2u);
    EXPECT_EQ(asset.emitters[0].beam.arcs[0].colorStart,
        Eigen::Vector3f(1.0f, 0.7f, 0.2f));
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.arcs[0].widthScale, 0.8f);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.arcs[0].intensity, 1.5f);
    EXPECT_FLOAT_EQ(asset.emitters[0].beam.arcs[1].noisePhase, -2.4f);

    // beam 不参与逐粒子排序，alpha 路径不强制 sortParticles=true。
    document["emitters"][0]["blend"] = "alpha";
    document["emitters"][0]["sortParticles"] = false;
    EXPECT_NO_THROW(
        (void)ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_beam_alpha.json"));
}

TEST(ParticleAsset, BeamFieldsRejectInvalidValues)
{
    const auto expectReject = [](
        Json document,
        const char* path)
    {
        EXPECT_THROW(
            ParticleAssetLoader::ParseJson(document, path),
            std::invalid_argument);
    };
    const auto beamDocument = []()
    {
        Json document = MultiEmitterDocument();
        document["emitters"][0]["renderMode"] = "beam";
        document["emitters"][0]["beam"] = Json{
            {"start", {-200.0, 0.0, 0.0}},
            {"end", {200.0, 0.0, 0.0}},
            {"segments", 16},
            {"width", 3.0},
            {"count", 4},
            {"noiseAmplitude", 18.0},
            {"noiseFrequency", 3.0},
            {"noiseSpeed", 5.0},
            {"endpointJitter", 0.0}};
        return document;
    };

    Json document = beamDocument();
    document["emitters"][0]["beam"]["end"] = {-200.0, 0.0, 0.0};
    expectReject(document, "Maps/SC_test/PS_beam_degenerate.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["segments"] = 1;
    expectReject(document, "Maps/SC_test/PS_beam_segments_low.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["segments"] = 65;
    expectReject(document, "Maps/SC_test/PS_beam_segments_high.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["count"] = 0;
    expectReject(document, "Maps/SC_test/PS_beam_count_low.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["count"] = 17;
    expectReject(document, "Maps/SC_test/PS_beam_count_high.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["width"] = 0.0;
    expectReject(document, "Maps/SC_test/PS_beam_width.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["noiseAmplitude"] = -1.0;
    expectReject(document, "Maps/SC_test/PS_beam_amplitude.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["noiseFrequency"] = 0.0;
    expectReject(document, "Maps/SC_test/PS_beam_frequency.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["endpointJitter"] = -1.0;
    expectReject(document, "Maps/SC_test/PS_beam_jitter.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["branchCount"] = 5;
    expectReject(document, "Maps/SC_test/PS_beam_branch_count_high.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["branchStart"] = -0.1;
    expectReject(document, "Maps/SC_test/PS_beam_branch_start_low.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["branchStart"] = 1.1;
    expectReject(document, "Maps/SC_test/PS_beam_branch_start_high.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["branchLength"] = 0.0;
    expectReject(document, "Maps/SC_test/PS_beam_branch_length_low.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["branchLength"] = 1.1;
    expectReject(document, "Maps/SC_test/PS_beam_branch_length_high.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["branchSpread"] = -1.0;
    expectReject(document, "Maps/SC_test/PS_beam_branch_spread.json");

    document = beamDocument();
    document["emitters"][0]["beam"] = "not-an-object";
    expectReject(document, "Maps/SC_test/PS_beam_object.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["start"] = {0.0, 0.0};
    expectReject(document, "Maps/SC_test/PS_beam_start.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["colorKeys"] = Json::array();
    expectReject(document, "Maps/SC_test/PS_beam_color_keys_empty.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["colorKeys"] = Json::array({
        Json{{"time", 0.5}, {"color", {1.0, 1.0, 1.0, 1.0}}},
        Json{{"time", 0.5}, {"color", {1.0, 0.0, 0.0, 1.0}}}});
    expectReject(document, "Maps/SC_test/PS_beam_color_keys_duplicate.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["colorKeys"] = Json::array({
        Json{{"time", -0.1}, {"color", {1.0, 1.0, 1.0, 1.0}}}});
    expectReject(document, "Maps/SC_test/PS_beam_color_keys_time.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["colorKeys"] = Json::array({
        Json{{"time", 0.0}, {"color", {65.0, 1.0, 1.0, 1.0}}}});
    expectReject(document, "Maps/SC_test/PS_beam_color_keys_range.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["widthKeys"] = Json::array({
        Json{{"time", 0.0}, {"width", 1.1}}});
    expectReject(document, "Maps/SC_test/PS_beam_width_keys_range.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["widthKeys"] = Json::array({
        Json{{"time", 0.8}, {"width", 1.0}},
        Json{{"time", 0.2}, {"width", 0.5}}});
    expectReject(document, "Maps/SC_test/PS_beam_width_keys_order.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["arcs"] = Json::array({
        Json{{"widthScale", 0.5}}});
    expectReject(document, "Maps/SC_test/PS_beam_arcs_count.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["arcs"] = Json::array({
        Json{{"widthScale", 1.1}},
        Json{{"widthScale", 0.5}},
        Json{{"widthScale", 0.5}},
        Json{{"widthScale", 0.5}}});
    expectReject(document, "Maps/SC_test/PS_beam_arcs_width.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["arcs"] = Json::array({
        Json{{"intensity", -1.0}},
        Json{{"intensity", 1.0}},
        Json{{"intensity", 1.0}},
        Json{{"intensity", 1.0}}});
    expectReject(document, "Maps/SC_test/PS_beam_arcs_intensity.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["arcs"] = Json::array({
        Json{{"noisePhase", 1001.0}},
        Json{{"noisePhase", 0.0}},
        Json{{"noisePhase", 0.0}},
        Json{{"noisePhase", 0.0}}});
    expectReject(document, "Maps/SC_test/PS_beam_arcs_phase.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["arcs"] = Json::array({
        "not-an-object"});
    expectReject(document, "Maps/SC_test/PS_beam_arcs_object.json");

    document = beamDocument();
    document["emitters"][0]["beam"]["hitScan"] = Json{
        {"planeNormal", {0.0, 1.0, 0.0}},
        {"planeOffset", 0.0},
        {"maxDistance", 2000.0}};
    const ParticleSystemAsset hitScanAsset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_beam_hitscan.json");
    ASSERT_EQ(hitScanAsset.emitters.size(), 2u);
    EXPECT_TRUE(hitScanAsset.emitters[0].beam.hitScan.enabled);
    EXPECT_TRUE(hitScanAsset.emitters[0].beam.hitScan.planeNormal
        .isApprox(Eigen::Vector3f::UnitY()));
    EXPECT_FLOAT_EQ(
        hitScanAsset.emitters[0].beam.hitScan.maxDistance, 2000.0f);

    document["emitters"][0]["beam"]["hitScan"]["planeNormal"] =
        {0.0, 0.0, 0.0};
    expectReject(document, "Maps/SC_test/PS_beam_hitscan_normal.json");
    document = beamDocument();
    document["emitters"][0]["beam"]["hitScan"] = Json{
        {"maxDistance", 0.0}};
    expectReject(document, "Maps/SC_test/PS_beam_hitscan_distance.json");
    document = beamDocument();
    document["emitters"][0]["beam"]["hitScan"] = 1;
    expectReject(document, "Maps/SC_test/PS_beam_hitscan_object.json");
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
    EXPECT_TRUE(hasField("beam.branchCount"));
    EXPECT_TRUE(hasField("beam.branchStart"));
    EXPECT_TRUE(hasField("beam.branchLength"));
    EXPECT_TRUE(hasField("beam.branchSpread"));
    EXPECT_TRUE(hasField("beam.colorKeys"));
    EXPECT_TRUE(hasField("beam.widthKeys"));
    EXPECT_TRUE(hasField("beam.arcs"));
    EXPECT_TRUE(hasField("beam.hitScan.enabled"));
    EXPECT_TRUE(hasField("beam.hitScan.planeNormal"));
    EXPECT_TRUE(hasField("beam.hitScan.planeOffset"));
    EXPECT_TRUE(hasField("beam.hitScan.maxDistance"));
}

TEST(ParticleAsset, BeamCurvesRoundTripThroughV2RendererModule)
{
    Json document = V2ModuleDocument();
    auto& modules = document["emitters"][0]["modules"];
    // renderer 字段只能由一个模块拥有；旧 fixture 把 renderMode 放在
    // emitter 模块里，这里先移除再建立规范 owner。
    modules[0]["fields"].erase("renderMode");
    modules.push_back(Json{
        {"type", "renderer"},
        {"enabled", true},
        {"fields", Json{
            {"renderMode", "beam"},
            {"beam", Json{
                {"start", {-200.0, 0.0, 0.0}},
                {"end", {200.0, 0.0, 0.0}},
                {"segments", 24},
                {"width", 3.0},
                {"count", 2},
                {"noiseAmplitude", 18.0},
                {"noiseFrequency", 3.0},
                {"noiseSpeed", 5.0}}}}}});

    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange colorChange;
    colorChange.field = "emitters.0.beam.colorKeys";
    colorChange.value = Json::array({
        Json{{"time", 0.0}, {"color", {2.0, 4.0, 8.0, 1.0}}},
        Json{{"time", 1.0}, {"color", {0.5, 1.0, 2.0, 0.0}}}});
    changes.push_back(colorChange);
    EditorAssetFieldChange widthChange;
    widthChange.field = "emitters.0.beam.widthKeys";
    widthChange.value = Json::array({
        Json{{"time", 0.0}, {"width", 0.2}},
        Json{{"time", 0.5}, {"width", 1.0}},
        Json{{"time", 1.0}, {"width", 0.4}}});
    changes.push_back(widthChange);
    EditorAssetFieldChange arcChange;
    arcChange.field = "emitters.0.beam.arcs";
    arcChange.value = Json::array({
        Json{{"colorStart", {1.0, 0.6, 0.2}},
            {"widthScale", 0.9},
            {"intensity", 1.25},
            {"noisePhase", 0.7}},
        Json{{"colorStart", {0.2, 0.6, 1.0}},
            {"widthScale", 0.45},
            {"intensity", 0.8},
            {"noisePhase", -1.3}}});
    changes.push_back(arcChange);
    ApplyAssetChanges(
        document,
        EditorAssetDocumentKind::ParticleSystem,
        changes);

    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            Json::parse(document.dump()),
            "Maps/SC_test/PS_beam_v2_curves.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    const ParticleBeamAsset& beam = asset.emitters[0].beam;
    ASSERT_EQ(beam.colorKeys.size(), 2u);
    EXPECT_FLOAT_EQ(beam.colorKeys[0].color.y(), 4.0f);
    ASSERT_EQ(beam.widthKeys.size(), 3u);
    EXPECT_FLOAT_EQ(beam.widthKeys[1].width, 1.0f);
    ASSERT_EQ(beam.arcs.size(), 2u);
    EXPECT_FLOAT_EQ(beam.arcs[1].widthScale, 0.45f);
    EXPECT_FLOAT_EQ(beam.arcs[1].noisePhase, -1.3f);

    // 字段必须落在 renderer 模块里，而不是根对象旁路。
    const Json& rendererModule =
        document["emitters"][0]["modules"].back();
    EXPECT_EQ(rendererModule["type"], "renderer");
    EXPECT_TRUE(rendererModule["fields"].contains("beam"));
    EXPECT_TRUE(rendererModule["fields"]["beam"].contains("colorKeys"));
    EXPECT_TRUE(rendererModule["fields"]["beam"].contains("widthKeys"));
    EXPECT_TRUE(rendererModule["fields"]["beam"].contains("arcs"));
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
    EXPECT_EQ(
        alignmentField->at("enum"),
        Json::array({"camera", "velocity", "fixed"}));
    EXPECT_EQ(
        projection["emitters"][0]["billboardAlignment"], "velocity");
}

TEST(ParticleAsset, BillboardAlignmentFixedParsesWorldBasis)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["billboardAlignment"] = "fixed";
    document["emitters"][0]["billboardNormal"] = {0.0, 0.0, 1.0};
    document["emitters"][0]["billboardUp"] = {0.0, 1.0, 0.0};

    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_fixed.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(asset.emitters[0].billboardAlignment, "fixed");
    EXPECT_TRUE(asset.emitters[0].billboardNormal.isApprox(
        Eigen::Vector3f(0.0f, 0.0f, 1.0f)));
    EXPECT_TRUE(asset.emitters[0].billboardUp.isApprox(
        Eigen::Vector3f(0.0f, 1.0f, 0.0f)));
    // 非 fixed 发射器保留默认基，渲染端不会消费它们。
    EXPECT_EQ(asset.emitters[1].billboardAlignment, "camera");
    EXPECT_TRUE(asset.emitters[1].billboardNormal.isApprox(
        Eigen::Vector3f(0.0f, 0.0f, 1.0f)));
    EXPECT_TRUE(asset.emitters[1].billboardUp.isApprox(
        Eigen::Vector3f(0.0f, 1.0f, 0.0f)));
}

TEST(ParticleAsset, BillboardAlignmentFixedRoundTripsThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange change;
    change.field = "emitters.0.billboardAlignment";
    change.value = std::string("fixed");
    changes.push_back(change);
    change.field = "emitters.0.billboardNormal";
    change.value = std::array<double, 3>{1.0, 0.0, 0.0};
    changes.push_back(change);
    change.field = "emitters.0.billboardUp";
    change.value = std::array<double, 3>{0.0, 1.0, 0.0};
    changes.push_back(change);

    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            Json::parse(document.dump()),
            "Maps/SC_test/PS_billboard_fixed.json");

    EXPECT_EQ(asset.emitters[0].billboardAlignment, "fixed");
    EXPECT_TRUE(asset.emitters[0].billboardNormal.isApprox(
        Eigen::Vector3f(1.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(asset.emitters[0].billboardUp.isApprox(
        Eigen::Vector3f(0.0f, 1.0f, 0.0f)));
}

TEST(ParticleAsset, BillboardAlignmentFixedRejectsDegenerateBasis)
{
    const auto parseWith = [](const Json& document) {
        return ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_fixed_invalid.json");
    };

    Json zeroNormal = MultiEmitterDocument();
    zeroNormal["emitters"][0]["billboardAlignment"] = "fixed";
    zeroNormal["emitters"][0]["billboardNormal"] = {0.0, 0.0, 0.0};
    zeroNormal["emitters"][0]["billboardUp"] = {0.0, 1.0, 0.0};
    EXPECT_THROW(parseWith(zeroNormal), std::invalid_argument);

    Json zeroUp = MultiEmitterDocument();
    zeroUp["emitters"][0]["billboardAlignment"] = "fixed";
    zeroUp["emitters"][0]["billboardNormal"] = {0.0, 0.0, 1.0};
    zeroUp["emitters"][0]["billboardUp"] = {0.0, 0.0, 0.0};
    EXPECT_THROW(parseWith(zeroUp), std::invalid_argument);

    Json parallel = MultiEmitterDocument();
    parallel["emitters"][0]["billboardAlignment"] = "fixed";
    parallel["emitters"][0]["billboardNormal"] = {0.0, 2.0, 0.0};
    parallel["emitters"][0]["billboardUp"] = {0.0, -3.0, 0.0};
    EXPECT_THROW(parseWith(parallel), std::invalid_argument);

    Json nonFinite = MultiEmitterDocument();
    nonFinite["emitters"][0]["billboardAlignment"] = "fixed";
    nonFinite["emitters"][0]["billboardNormal"] =
        Json::array({0.0, 0.0, std::numeric_limits<double>::infinity()});
    nonFinite["emitters"][0]["billboardUp"] = {0.0, 1.0, 0.0};
    EXPECT_THROW(parseWith(nonFinite), std::invalid_argument);
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

TEST(ParticleAsset, MeshAndSkeletalAcceptLocalSpace)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["localSpace"] = true;
    document["emitters"][0]["mesh"] = Json{
        {"path", "Common/Meshes/SM_cube.json"}};
    document["emitters"][1]["renderMode"] = "skeletalMeshTris";
    document["emitters"][1]["localSpace"] = true;
    document["emitters"][1]["mesh"] = Json{
        {"path", "Common/Meshes/SK_cube.json"}};

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_mesh_local_space.json");
    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_TRUE(asset.emitters[0].localSpace);
    EXPECT_TRUE(asset.emitters[1].localSpace);
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
    EXPECT_TRUE(hasField("mesh.useSizeCurve"));
    EXPECT_TRUE(hasField("mesh.sizeScale"));
}

TEST(ParticleAsset, MeshSizeCurveFieldsParseAndValidate)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["renderMode"] = "mesh";
    document["emitters"][0]["mesh"] = Json{
        {"path", "Common/Meshes/SM_cube.json"}};
    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_mesh_size_default.json");
    EXPECT_FALSE(asset.emitters[0].meshUseSizeCurve);
    EXPECT_FLOAT_EQ(asset.emitters[0].meshSizeScale, 1.0f);

    document["emitters"][0]["mesh"]["useSizeCurve"] = true;
    document["emitters"][0]["mesh"]["sizeScale"] = 2.5;
    document["emitters"][0]["sizeKeys"] = Json::array({
        Json{{"time", 0.0}, {"size", 10.0}},
        Json{{"time", 1.0}, {"size", 30.0}}});
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_mesh_size.json");
    EXPECT_TRUE(asset.emitters[0].meshUseSizeCurve);
    EXPECT_FLOAT_EQ(asset.emitters[0].meshSizeScale, 2.5f);

    document["emitters"][0]["mesh"]["sizeScale"] = 0.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_size_zero.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, MeshLightingDefaultsToEnabledWithAmbient)
{
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        MultiEmitterDocument(), "Maps/SC_test/PS_mesh_lighting_default.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_TRUE(asset.emitters[0].meshLighting);
    EXPECT_TRUE(asset.emitters[1].meshLighting);
    EXPECT_FLOAT_EQ(asset.emitters[0].meshAmbient, 0.2f);
    EXPECT_FLOAT_EQ(asset.emitters[1].meshAmbient, 0.2f);
}

TEST(ParticleAsset, BillboardLightingDefaultsToUnlitWithAmbient)
{
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        MultiEmitterDocument(),
        "Maps/SC_test/PS_billboard_lighting_default.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_FALSE(asset.emitters[0].billboardLighting);
    EXPECT_FALSE(asset.emitters[1].billboardLighting);
    EXPECT_FLOAT_EQ(asset.emitters[0].billboardAmbient, 0.2f);
    EXPECT_FLOAT_EQ(asset.emitters[1].billboardAmbient, 0.2f);
    EXPECT_FLOAT_EQ(
        asset.emitters[0].billboardLightingStrength,
        1.0f);
    EXPECT_FLOAT_EQ(
        asset.emitters[1].billboardLightingStrength,
        1.0f);
}

TEST(ParticleAsset, BillboardLightingStrengthParsesAndValidates)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["billboardLighting"] = true;
    document["emitters"][0]["billboardLightingStrength"] = 0.35;
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document,
        "Maps/SC_test/PS_billboard_lighting_strength.json");
    EXPECT_FLOAT_EQ(
        asset.emitters[0].billboardLightingStrength,
        0.35f);

    document["emitters"][0]["billboardLightingStrength"] = -0.01;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document,
            "Maps/SC_test/PS_billboard_lighting_strength_low.json"),
        std::invalid_argument);

    document["emitters"][0]["billboardLightingStrength"] = 1.01;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document,
            "Maps/SC_test/PS_billboard_lighting_strength_high.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, BillboardLightingParsesAndRejectsInvalidAmbient)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["billboardLighting"] = true;
    document["emitters"][0]["billboardAmbient"] = 0.65;
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_billboard_lighting.json");
    EXPECT_TRUE(asset.emitters[0].billboardLighting);
    EXPECT_FLOAT_EQ(asset.emitters[0].billboardAmbient, 0.65f);

    document["emitters"][0]["billboardAmbient"] = -0.1;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document,
            "Maps/SC_test/PS_billboard_lighting_negative.json"),
        std::invalid_argument);

    document["emitters"][0]["billboardAmbient"] = 1.5;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document,
            "Maps/SC_test/PS_billboard_lighting_too_large.json"),
        std::invalid_argument);

    document["emitters"][0]["billboardAmbient"] =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document,
            "Maps/SC_test/PS_billboard_lighting_nan.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, BillboardMaterialLobeDefaults)
{
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        MultiEmitterDocument(),
        "Maps/SC_test/PS_billboard_lobes_default.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].billboardSpecular, 0.1f);
    EXPECT_FLOAT_EQ(asset.emitters[1].billboardSpecular, 0.1f);
    EXPECT_FLOAT_EQ(asset.emitters[0].billboardRoughness, 0.6f);
    EXPECT_FLOAT_EQ(asset.emitters[1].billboardRoughness, 0.6f);
    EXPECT_FLOAT_EQ(
        asset.emitters[0].billboardForwardScattering,
        0.35f);
    EXPECT_FLOAT_EQ(
        asset.emitters[1].billboardForwardScattering,
        0.35f);
}

TEST(ParticleAsset, BillboardMaterialLobesParseAndValidateRanges)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["billboardSpecular"] = 0.45;
    document["emitters"][0]["billboardRoughness"] = 0.25;
    document["emitters"][0]["billboardForwardScattering"] = 1.5;
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document,
        "Maps/SC_test/PS_billboard_lobes.json");
    EXPECT_FLOAT_EQ(asset.emitters[0].billboardSpecular, 0.45f);
    EXPECT_FLOAT_EQ(asset.emitters[0].billboardRoughness, 0.25f);
    EXPECT_FLOAT_EQ(
        asset.emitters[0].billboardForwardScattering,
        1.5f);

    // 镜面强度
    document["emitters"][0]["billboardSpecular"] = -0.01;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_specular_low.json"),
        std::invalid_argument);
    document["emitters"][0]["billboardSpecular"] = 1.01;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_specular_high.json"),
        std::invalid_argument);
    document["emitters"][0]["billboardSpecular"] = 0.4;

    // 粗糙度下限是 0.02：0 会让 GGX 退化成 delta 高光。
    document["emitters"][0]["billboardRoughness"] = 0.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_roughness_low.json"),
        std::invalid_argument);
    document["emitters"][0]["billboardRoughness"] = 1.01;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_roughness_high.json"),
        std::invalid_argument);
    document["emitters"][0]["billboardRoughness"] = 0.6;

    document["emitters"][0]["billboardForwardScattering"] = -0.01;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_scatter_low.json"),
        std::invalid_argument);
    document["emitters"][0]["billboardForwardScattering"] = 2.01;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_scatter_high.json"),
        std::invalid_argument);
    document["emitters"][0]["billboardForwardScattering"] =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_billboard_scatter_nan.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, MeshLightingFieldsRoundTripThroughEditorPath)
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
    addChange("emitters.1.meshLighting", false);
    addChange("emitters.1.meshAmbient", 0.6);

    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_mesh_lighting_editor.json");

    EXPECT_TRUE(asset.emitters[0].meshLighting);
    EXPECT_FLOAT_EQ(asset.emitters[0].meshAmbient, 0.2f);
    EXPECT_FALSE(asset.emitters[1].meshLighting);
    EXPECT_FLOAT_EQ(asset.emitters[1].meshAmbient, 0.6f);
}

TEST(ParticleAsset, MeshAmbientRejectsOutOfRangeAndNonFinite)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["meshAmbient"] = -0.1;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_ambient_negative.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["meshAmbient"] = 1.5;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_ambient_too_large.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["meshAmbient"] =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_ambient_nan.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["meshAmbient"] =
        std::numeric_limits<float>::infinity();
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_mesh_ambient_infinity.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, CapabilitiesExposeMeshLightingFields)
{
    const Json document = MultiEmitterDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const auto findField = [&capabilities](const char* name) -> const Json*
    {
        for (const Json& field : capabilities["emitters"][0]["fields"])
            if (field.value("field", std::string()) == name)
                return &field;
        return nullptr;
    };

    const Json* lighting = findField("meshLighting");
    const Json* ambient = findField("meshAmbient");
    ASSERT_NE(lighting, nullptr);
    ASSERT_NE(ambient, nullptr);
    EXPECT_EQ(lighting->value("type", std::string()), "boolean");
    EXPECT_TRUE(lighting->value("resettable", false));
    EXPECT_EQ(ambient->value("type", std::string()), "number");
    EXPECT_TRUE(ambient->value("resettable", false));
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

TEST(ParticleAsset, LifetimeScaleKeysDefaultToEmpty)
{
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        MultiEmitterDocument(),
        "Maps/SC_test/PS_lifetime_scale_default.json");

    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_TRUE(asset.emitters[0].lifetimeScaleKeys.empty());
    EXPECT_TRUE(asset.emitters[1].lifetimeScaleKeys.empty());
}

TEST(ParticleAsset, LifetimeScaleKeysParseSortsByTimeAndKeepsDuplicates)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["lifetimeScaleKeys"] = Json::array({
        Json{{"time", 1.0}, {"scale", 0.0}},
        Json{{"time", 0.5}, {"scale", 2.0}},
        Json{{"time", 0.0}, {"scale", 0.25}},
        Json{{"time", 0.5}, {"scale", 3.0}}});

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_lifetime_scale_parse.json");

    const std::vector<ParticleLifetimeScaleKey>& keys =
        asset.emitters[0].lifetimeScaleKeys;
    ASSERT_EQ(keys.size(), 4u);
    EXPECT_FLOAT_EQ(keys[0].time, 0.0f);
    EXPECT_FLOAT_EQ(keys[0].scale, 0.25f);
    EXPECT_FLOAT_EQ(keys[1].time, 0.5f);
    EXPECT_FLOAT_EQ(keys[1].scale, 2.0f);
    EXPECT_FLOAT_EQ(keys[2].time, 0.5f);
    EXPECT_FLOAT_EQ(keys[2].scale, 3.0f);
    EXPECT_FLOAT_EQ(keys[3].time, 1.0f);
    EXPECT_FLOAT_EQ(keys[3].scale, 0.0f);
}

TEST(ParticleAsset, LifetimeScaleKeysRoundTripThroughEditorPath)
{
    Json document = MultiEmitterDocument();
    std::vector<EditorAssetFieldChange> changes;
    EditorAssetFieldChange change;
    change.field = "emitters.0.lifetimeScaleKeys";
    change.value = Json::array({
        Json{{"time", 0.0}, {"scale", 0.5}},
        Json{{"time", 0.75}, {"scale", 2.0}}});
    changes.push_back(change);

    ApplyAssetChanges(
        document,
        EditorAssetDocumentKind::ParticleSystem,
        changes);
    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_lifetime_scale_editor.json");
    ASSERT_EQ(asset.emitters[0].lifetimeScaleKeys.size(), 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].lifetimeScaleKeys[0].scale, 0.5f);
    EXPECT_FLOAT_EQ(asset.emitters[0].lifetimeScaleKeys[1].time, 0.75f);

    EditorAssetFieldChange resetChange;
    resetChange.field = "emitters.0.lifetimeScaleKeys";
    resetChange.reset = true;
    changes.clear();
    changes.push_back(resetChange);
    ApplyAssetChanges(
        document,
        EditorAssetDocumentKind::ParticleSystem,
        changes);
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_lifetime_scale_reset.json");
    EXPECT_TRUE(asset.emitters[0].lifetimeScaleKeys.empty());
}

TEST(ParticleAsset, LifetimeScaleKeysExposedByEditorCapabilities)
{
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem,
        MultiEmitterDocument());
    Json found;
    for (const Json& field : capabilities["emitters"][0]["fields"])
    {
        if (field.value("field", std::string()) == "lifetimeScaleKeys")
        {
            found = field;
            break;
        }
    }
    ASSERT_TRUE(found.is_object());
    EXPECT_EQ(found.value("type", std::string()), "array");
    EXPECT_TRUE(found.value("resettable", false));
}

TEST(ParticleAsset, LifetimeScaleKeysRejectInvalidCountRangeAndNonFinite)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity =
        std::numeric_limits<float>::infinity();

    Json document = MultiEmitterDocument();
    Json keys = Json::array();
    for (int index = 0; index < 9; ++index)
    {
        keys.push_back(Json{{"time", index / 10.0}, {"scale", 1.0}});
    }
    document["emitters"][0]["lifetimeScaleKeys"] = keys;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lifetime_scale_count.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["lifetimeScaleKeys"] = Json::array({
        Json{{"time", 1.5}, {"scale", 1.0}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lifetime_scale_time.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["lifetimeScaleKeys"] = Json::array({
        Json{{"time", 0.5}, {"scale", -0.1}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lifetime_scale_low.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["lifetimeScaleKeys"] = Json::array({
        Json{{"time", 0.5}, {"scale", 4.1}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lifetime_scale_high.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["lifetimeScaleKeys"] = Json::array({
        Json{{"time", nan}, {"scale", 1.0}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lifetime_scale_time_nan.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["lifetimeScaleKeys"] = Json::array({
        Json{{"time", 0.5}, {"scale", infinity}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_lifetime_scale_scale_inf.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, LifetimeScaleEvaluateEmptyAndClamp)
{
    const std::vector<ParticleLifetimeScaleKey> empty;
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(empty, 0.5f), 1.0f);

    const std::vector<ParticleLifetimeScaleKey> keys = {
        ParticleLifetimeScaleKey{0.25f, 0.5f},
        ParticleLifetimeScaleKey{0.75f, 2.0f}};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, -1.0f), 0.5f);
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, 0.0f), 0.5f);
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, nan), 0.5f);
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, 1.0f), 2.0f);
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, 2.0f), 2.0f);
}

TEST(ParticleAsset, LifetimeScaleEvaluatePiecewiseLinear)
{
    const std::vector<ParticleLifetimeScaleKey> keys = {
        ParticleLifetimeScaleKey{0.0f, 0.0f},
        ParticleLifetimeScaleKey{0.5f, 1.0f},
        ParticleLifetimeScaleKey{1.0f, 4.0f}};
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, 0.25f), 0.5f);
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, 0.75f), 2.5f);
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(keys, 1.0f), 4.0f);

    const std::vector<ParticleLifetimeScaleKey> duplicate = {
        ParticleLifetimeScaleKey{0.0f, 0.5f},
        ParticleLifetimeScaleKey{0.5f, 1.0f},
        ParticleLifetimeScaleKey{0.5f, 3.0f},
        ParticleLifetimeScaleKey{1.0f, 0.0f}};
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(duplicate, 0.5f), 3.0f);
    EXPECT_FLOAT_EQ(EvaluateParticleLifetimeScale(duplicate, 0.75f), 1.5f);
}

TEST(ParticleAsset, V2UserParametersSubstituteIntoModuleNumbers)
{
    const Json document = Json{
        {"name", "PS_params"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", Json{
            {"Rate", 42.5},
            {"Radius", 8.0}}},
        {"modules", Json::array({
            Json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", Json{{"maxParticles", 64}}}},
            Json{
                {"type", "spawn.rate"},
                {"enabled", true},
                {"fields", Json{{"spawnRate", "$Rate"}}}},
            Json{
                {"type", "spawn.shapeSphere"},
                {"enabled", true},
                {"fields", Json{{"shape", Json{
                    {"type", "sphere"},
                    {"radius", "$Radius"}}}}}},
            Json{
                {"type", "update.lifetime"},
                {"enabled", true},
                {"fields", Json{{"lifetime", Json::array({0.5, 1.0})}}}}
        })}
    };

    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_params.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_FLOAT_EQ(asset.emitters[0].spawnRate, 42.5f);
    EXPECT_FLOAT_EQ(asset.emitters[0].shapeRadius, 8.0f);
}

TEST(ParticleAsset, V2UnknownUserParameterReferenceIsRejected)
{
    const Json document = Json{
        {"name", "PS_params"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", Json{{"Rate", 1.0}}},
        {"modules", Json::array({
            Json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", Json{{"maxParticles", 64}}}},
            Json{
                {"type", "spawn.rate"},
                {"enabled", true},
                {"fields", Json{{"spawnRate", "$Missing"}}}}
        })}
    };
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_params.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, EventsDeterminismAndFixedStepParse)
{
    const Json document = Json{
        {"name", "PS_events"},
        {"type", "particleSystem"},
        {"emitters", Json::array({Json{
            {"name", "sparks"},
            {"events", Json{
                {"spawn", true},
                {"death", true},
                {"collision", true}}},
            {"randomSeed", 7u},
            {"deterministicRandom", true},
            {"fixedTimeStep", 0.0166667f}
        }})}
    };
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_events.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_TRUE(asset.emitters[0].emitSpawnEvent);
    EXPECT_TRUE(asset.emitters[0].emitDeathEvent);
    EXPECT_TRUE(asset.emitters[0].emitCollisionEvent);
    EXPECT_EQ(asset.emitters[0].randomSeed, 7u);
    EXPECT_TRUE(asset.emitters[0].deterministicRandom);
    EXPECT_FLOAT_EQ(asset.emitters[0].fixedTimeStep, 0.0166667f);
}

TEST(ParticleAsset, LocalSpaceCanCombineWithAnalyticCollision)
{
    const Json document = Json{
        {"name", "PS_local_collision"},
        {"type", "particleSystem"},
        {"emitters", Json::array({Json{
            {"name", "local"},
            {"localSpace", true},
            {"collision", Json{
                {"type", "box"},
                {"box", Json{
                    {"min", {-1.0, -1.0, -1.0}},
                    {"max", {1.0, 1.0, 1.0}}}}}}
        }})}
    };
    const ParticleSystemAsset asset =
        ParticleAssetLoader::ParseJson(
            document,
            "Maps/SC_test/PS_local_collision.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_TRUE(asset.emitters[0].localSpace);
    EXPECT_EQ(asset.emitters[0].collisionType, "box");
}

TEST(ParticleAsset, MaterialInstancePathMustBeRelativeJson)
{
    const Json document = Json{
        {"name", "PS_material"},
        {"type", "particleSystem"},
        {"emitters", Json::array({Json{
            {"name", "mesh"},
            {"renderMode", "mesh"},
            {"mesh", Json{{"path", "Maps/SC_test/Meshes/SM_x.json"}}},
            {"materialInstancePath", "../escape.json"}
        }})}
    };
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_material.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, AuthoringJsonIsFrozenOnlyWhenUserParametersMatter)
{
    const Json withParameters = Json{
        {"name", "PS_params"},
        {"type", "particleSystem"},
        {"version", 2},
        {"userParameters", Json{{"Speed", 2.0}}},
        {"modules", Json::array({
            Json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", Json{{"maxParticles", 32}}}},
            Json{
                {"type", "spawn.rate"},
                {"enabled", true},
                {"fields", Json{{"spawnRate", "$Speed"}}}}
        })}
    };
    const ParticleSystemAsset parsed =
        ParticleAssetLoader::ParseJson(
            withParameters, "Maps/SC_test/PS_params.json");
    ASSERT_TRUE(parsed.authoringJson != nullptr);
    EXPECT_EQ(parsed.authoringJson->at("name"), "PS_params");

    const Json withoutParameters = Json{
        {"name", "PS_plain"},
        {"type", "particleSystem"},
        {"version", 2},
        {"modules", Json::array({
            Json{
                {"type", "emitter"},
                {"enabled", true},
                {"fields", Json{{"maxParticles", 32}}}},
            Json{
                {"type", "spawn.rate"},
                {"enabled", true},
                {"fields", Json{{"spawnRate", 20.0}}}}
        })}
    };
    const ParticleSystemAsset plain =
        ParticleAssetLoader::ParseJson(
            withoutParameters, "Maps/SC_test/PS_plain.json");
    EXPECT_TRUE(plain.authoringJson == nullptr);
}

namespace
{

EditorAssetFieldChange ParticleEditorChange(
    std::string field,
    EditorAssetFieldValue value)
{
    EditorAssetFieldChange change;
    change.field = std::move(field);
    change.value = std::move(value);
    return change;
}

EditorAssetFieldChange ParticleEditorReset(std::string field)
{
    EditorAssetFieldChange change;
    change.field = std::move(field);
    change.reset = true;
    return change;
}

const Json* FindParticleCapability(
    const Json& fields,
    const std::string& name)
{
    for (const Json& field : fields)
        if (field.value("field", std::string()) == name)
            return &field;
    return nullptr;
}

} // namespace

TEST(ParticleEditorFields, RendererEventsDeterminismAndSdfAreExposed)
{
    Json document = MultiEmitterDocument();
    document["version"] = 2;
    document["userParameters"] = Json{{"Speed", 2.0}, {"Radius", 8.0}};
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    ASSERT_TRUE(capabilities["fields"].is_array());
    ASSERT_TRUE(capabilities["emitters"].is_array());
    ASSERT_EQ(capabilities["emitters"].size(), 2u);

    const Json& rootFields = capabilities["fields"];
    const Json* rootParameters =
        FindParticleCapability(rootFields, "userParameters");
    ASSERT_NE(rootParameters, nullptr);
    EXPECT_EQ(rootParameters->at("type"), "object");
    EXPECT_TRUE(rootParameters->at("resettable").get<bool>());

    const Json& emitterFields = capabilities["emitters"][0]["fields"];
    EXPECT_EQ(FindParticleCapability(emitterFields, "userParameters"),
        nullptr);

    const Json* material = FindParticleCapability(
        emitterFields, "mesh.materialInstancePath");
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->at("type"), "string");
    EXPECT_EQ(material->at("unit"), "asset_path");
    EXPECT_TRUE(material->at("resettable").get<bool>());
    // 根别名仍可写入，但能力表只公开规范位置，避免两行写同一值。
    EXPECT_EQ(FindParticleCapability(emitterFields, "materialInstancePath"),
        nullptr);

    const Json* shadows =
        FindParticleCapability(emitterFields, "billboardShadows");
    ASSERT_NE(shadows, nullptr);
    EXPECT_EQ(shadows->at("type"), "boolean");
    EXPECT_TRUE(shadows->at("resettable").get<bool>());

    const Json* billboardLighting =
        FindParticleCapability(emitterFields, "billboardLighting");
    const Json* billboardAmbient =
        FindParticleCapability(emitterFields, "billboardAmbient");
    ASSERT_NE(billboardLighting, nullptr);
    ASSERT_NE(billboardAmbient, nullptr);
    EXPECT_EQ(billboardLighting->at("type"), "boolean");
    EXPECT_TRUE(billboardLighting->at("resettable").get<bool>());
    EXPECT_EQ(billboardAmbient->at("type"), "number");
    EXPECT_TRUE(billboardAmbient->at("resettable").get<bool>());

    for (const char* name : {"events.spawn", "events.death",
             "events.collision"})
    {
        const Json* event = FindParticleCapability(emitterFields, name);
        ASSERT_NE(event, nullptr) << name;
        EXPECT_EQ(event->at("type"), "boolean");
        EXPECT_TRUE(event->at("resettable").get<bool>());
    }

    const Json* seed = FindParticleCapability(emitterFields, "randomSeed");
    ASSERT_NE(seed, nullptr);
    EXPECT_EQ(seed->at("type"), "number");
    EXPECT_TRUE(seed->at("integer").get<bool>());
    EXPECT_DOUBLE_EQ(seed->at("minimum").get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(seed->at("maximum").get<double>(), 4294967295.0);
    EXPECT_TRUE(seed->at("resettable").get<bool>());

    const Json* deterministic = FindParticleCapability(
        emitterFields, "deterministicRandom");
    ASSERT_NE(deterministic, nullptr);
    EXPECT_EQ(deterministic->at("type"), "boolean");
    EXPECT_TRUE(deterministic->at("resettable").get<bool>());

    const Json* step =
        FindParticleCapability(emitterFields, "fixedTimeStep");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->at("type"), "number");
    EXPECT_DOUBLE_EQ(step->at("minimum").get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(step->at("maximum").get<double>(), 0.25);
    EXPECT_TRUE(step->at("resettable").get<bool>());

    const Json* collisionType =
        FindParticleCapability(emitterFields, "collision.type");
    ASSERT_NE(collisionType, nullptr);
    const Json& enumValues = collisionType->at("enum");
    EXPECT_NE(
        std::find(enumValues.begin(), enumValues.end(), "sdf"),
        enumValues.end());
    EXPECT_NE(
        std::find(enumValues.begin(), enumValues.end(), "mesh"),
        enumValues.end());

    const Json* sdfPath =
        FindParticleCapability(emitterFields, "collision.sdf.path");
    ASSERT_NE(sdfPath, nullptr);
    EXPECT_EQ(sdfPath->at("type"), "string");
    EXPECT_EQ(sdfPath->at("unit"), "asset_path");
    EXPECT_TRUE(sdfPath->at("resettable").get<bool>());

    const Json* meshPath =
        FindParticleCapability(emitterFields, "collision.mesh.path");
    ASSERT_NE(meshPath, nullptr);
    EXPECT_EQ(meshPath->at("type"), "string");
    EXPECT_EQ(meshPath->at("unit"), "asset_path");
    EXPECT_TRUE(meshPath->at("resettable").get<bool>());
}

TEST(ParticleEditorFields, UserParametersAreV2OnlyAndBounded)
{
    Json v1 = MultiEmitterDocument();
    const Json v1Capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, v1);
    EXPECT_EQ(FindParticleCapability(v1Capabilities["fields"],
        "userParameters"), nullptr);
    EXPECT_EQ(FindParticleCapability(
        v1Capabilities["emitters"][0]["fields"], "userParameters"),
        nullptr);

    Json v2 = MultiEmitterDocument();
    v2["version"] = 2;
    const Json v2Capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, v2);
    ASSERT_NE(FindParticleCapability(v2Capabilities["fields"],
        "userParameters"), nullptr);

    const std::vector<EditorAssetFieldChange> valid{
        ParticleEditorChange(
            "userParameters", Json{{"Speed", 2.0}, {"Count", 3}})};
    ApplyAssetChanges(v2, EditorAssetDocumentKind::ParticleSystem, valid);
    EXPECT_DOUBLE_EQ(v2.at("userParameters").at("Speed").get<double>(),
        2.0);
    EXPECT_DOUBLE_EQ(v2.at("userParameters").at("Count").get<double>(),
        3.0);

    Json tooMany = Json::object();
    for (int index = 0; index < 65; ++index)
        tooMany["P" + std::to_string(index)] = 1.0;
    EXPECT_THROW(
        ApplyAssetChanges(v2, EditorAssetDocumentKind::ParticleSystem,
            {ParticleEditorChange("userParameters", tooMany)}),
        std::invalid_argument);
    EXPECT_THROW(
        ApplyAssetChanges(v2, EditorAssetDocumentKind::ParticleSystem,
            {ParticleEditorChange(
                "userParameters", Json{{"Speed", "fast"}})}),
        std::invalid_argument);
    EXPECT_THROW(
        ApplyAssetChanges(v2, EditorAssetDocumentKind::ParticleSystem,
            {ParticleEditorChange(
                "userParameters", Json{{"", 1.0}})}),
        std::invalid_argument);

    Json exactlyLimit = Json::object();
    for (int index = 0; index < 64; ++index)
        exactlyLimit["P" + std::to_string(index)] = 0.5;
    EXPECT_NO_THROW(ApplyAssetChanges(
        v2, EditorAssetDocumentKind::ParticleSystem,
        {ParticleEditorChange("userParameters", exactlyLimit)}));
    EXPECT_EQ(v2.at("userParameters").size(), 64u);

    EXPECT_THROW(
        ApplyAssetChanges(v1, EditorAssetDocumentKind::ParticleSystem,
            {ParticleEditorChange("userParameters", Json{{"Speed", 1.0}})}),
        std::invalid_argument);

    ApplyAssetChanges(v2, EditorAssetDocumentKind::ParticleSystem,
        {ParticleEditorReset("userParameters")});
    EXPECT_FALSE(v2.contains("userParameters"));
}

TEST(ParticleEditorFields, NewFieldsRoundTripAndResetToLoaderDefaults)
{
    Json document = MultiEmitterDocument();
    const std::vector<EditorAssetFieldChange> changes{
        ParticleEditorChange("emitters.0.billboardShadows", true),
        ParticleEditorChange("emitters.0.billboardLighting", true),
        ParticleEditorChange("emitters.0.billboardAmbient", 0.55),
        ParticleEditorChange("emitters.0.mesh.materialInstancePath",
            std::string("Maps/SC_test/Materials/MI_spark.json")),
        ParticleEditorChange("emitters.0.events.spawn", true),
        ParticleEditorChange("emitters.0.events.death", true),
        ParticleEditorChange("emitters.0.events.collision", true),
        ParticleEditorChange("emitters.0.randomSeed", 12345.0),
        ParticleEditorChange("emitters.0.deterministicRandom", true),
        ParticleEditorChange("emitters.0.fixedTimeStep", 0.01),
        ParticleEditorChange("emitters.0.collision.sdf.path",
            std::string("Maps/SC_test/Sdf/sdf_field.json"))};
    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, changes);

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_new_fields.json");
    ASSERT_EQ(asset.emitters.size(), 2u);
    const ParticleEmitterAsset& emitter = asset.emitters[0];
    EXPECT_TRUE(emitter.billboardShadows);
    EXPECT_TRUE(emitter.billboardLighting);
    EXPECT_FLOAT_EQ(emitter.billboardAmbient, 0.55f);
    EXPECT_EQ(emitter.materialInstancePath,
        "Maps/SC_test/Materials/MI_spark.json");
    EXPECT_TRUE(emitter.emitSpawnEvent);
    EXPECT_TRUE(emitter.emitDeathEvent);
    EXPECT_TRUE(emitter.emitCollisionEvent);
    EXPECT_EQ(emitter.randomSeed, 12345u);
    EXPECT_TRUE(emitter.deterministicRandom);
    EXPECT_FLOAT_EQ(emitter.fixedTimeStep, 0.01f);
    EXPECT_EQ(emitter.collisionSdfPath,
        "Maps/SC_test/Sdf/sdf_field.json");

    std::vector<EditorAssetFieldChange> resets;
    for (const EditorAssetFieldChange& change : changes)
        resets.push_back(ParticleEditorReset(change.field));
    ApplyAssetChanges(
        document, EditorAssetDocumentKind::ParticleSystem, resets);
    const ParticleSystemAsset defaults = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_new_fields_reset.json");
    const ParticleEmitterAsset& reset = defaults.emitters[0];
    EXPECT_FALSE(reset.billboardShadows);
    EXPECT_FALSE(reset.billboardLighting);
    EXPECT_FLOAT_EQ(reset.billboardAmbient, 0.2f);
    EXPECT_TRUE(reset.materialInstancePath.empty());
    EXPECT_FALSE(reset.emitSpawnEvent);
    EXPECT_FALSE(reset.emitDeathEvent);
    EXPECT_FALSE(reset.emitCollisionEvent);
    EXPECT_EQ(reset.randomSeed, 0u);
    EXPECT_FALSE(reset.deterministicRandom);
    EXPECT_FLOAT_EQ(reset.fixedTimeStep, 0.0f);
    EXPECT_TRUE(reset.collisionSdfPath.empty());
}

TEST(ParticleEditorFields, MaterialInstancePathRootAliasNormalizesToMesh)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["materialInstancePath"] =
        "Maps/SC_test/Materials/MI_old.json";

    const Json projected = ProjectEditableAsset(
        EditorAssetDocumentKind::ParticleSystem, document);
    ASSERT_TRUE(projected["emitters"][0].contains("mesh"));
    EXPECT_EQ(projected["emitters"][0]["mesh"]["materialInstancePath"],
        "Maps/SC_test/Materials/MI_old.json");
    EXPECT_FALSE(projected["emitters"][0].contains("materialInstancePath"));

    ApplyAssetChanges(document, EditorAssetDocumentKind::ParticleSystem,
        {ParticleEditorChange("emitters.0.materialInstancePath",
            std::string("Maps/SC_test/Materials/MI_new.json"))});
    EXPECT_FALSE(document["emitters"][0].contains("materialInstancePath"));
    EXPECT_EQ(document["emitters"][0]["mesh"]["materialInstancePath"],
        "Maps/SC_test/Materials/MI_new.json");

    ApplyAssetChanges(document, EditorAssetDocumentKind::ParticleSystem,
        {ParticleEditorReset("emitters.0.mesh.materialInstancePath")});
    EXPECT_FALSE(document["emitters"][0].contains("materialInstancePath"));
    EXPECT_FALSE(document["emitters"][0].contains("mesh"));
}

TEST(ParticleEditorFields, NewFieldRangesAndEnumsAreEnforced)
{
    Json document = MultiEmitterDocument();
    const auto apply = [&document](EditorAssetFieldChange change)
    {
        return ApplyAssetChanges(
            document, EditorAssetDocumentKind::ParticleSystem,
            {std::move(change)});
    };
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.randomSeed", -1.0)), std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.randomSeed", 1.5)), std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.randomSeed", 4294967296.0)), std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.fixedTimeStep", -0.01)), std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.fixedTimeStep", 0.2501)), std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.collision.type", std::string("sdf "))),
        std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.collision.type", std::string("mesh "))),
        std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.billboardShadows", std::string("true"))),
        std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.billboardLighting", std::string("true"))),
        std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.billboardAmbient", -0.01)),
        std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.billboardAmbient", 1.01)),
        std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.events.spawn", 1.0)), std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.mesh.materialInstancePath", std::string())),
        std::invalid_argument);
    EXPECT_THROW(apply(ParticleEditorChange(
        "emitters.0.collision.sdf.path", std::string())),
        std::invalid_argument);

    // 合法边界值必须通过。
    EXPECT_NO_THROW(apply(ParticleEditorChange(
        "emitters.0.randomSeed", 4294967295.0)));
    EXPECT_NO_THROW(apply(ParticleEditorChange(
        "emitters.0.fixedTimeStep", 0.25)));
    EXPECT_NO_THROW(apply(ParticleEditorChange(
        "emitters.0.collision.type", std::string("sdf"))));
    EXPECT_NO_THROW(apply(ParticleEditorChange(
        "emitters.0.collision.type", std::string("mesh"))));
}

TEST(ParticleAsset, LightBrightnessKeysParseColorAndEnforceBounds)
{
    Json document = ParticleLightDocument();
    Json keys = Json::array();
    for (int key = 0; key < 8; ++key)
    {
        keys.push_back(Json{
            {"time", static_cast<double>(key) / 7.0},
            {"brightness", static_cast<double>(key)},
            {"color", Json::array({0.5, 1.0, 2.0})}});
    }
    document["emitters"][0]["light"]["brightnessKeys"] = keys;

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_light_curve_bounds.json");
    ASSERT_EQ(asset.emitters.size(), 2u);
    const ParticleLightAsset& light = asset.emitters[0].light;
    ASSERT_EQ(light.brightnessKeys.size(), 8u);
    EXPECT_FLOAT_EQ(light.brightnessKeys.back().time, 1.0f);
    EXPECT_FLOAT_EQ(light.brightnessKeys.back().brightness, 7.0f);
    EXPECT_TRUE(light.brightnessKeys.back().color.isApprox(
        Eigen::Vector3f(0.5f, 1.0f, 2.0f)));

    Json tooMany = document;
    tooMany["emitters"][0]["light"]["brightnessKeys"].push_back(
        Json{{"time", 1.0}, {"brightness", 1.0}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            tooMany, "Maps/SC_test/PS_light_curve_too_many.json"),
        std::invalid_argument);

    const auto parseKeys = [](Json keysToParse)
    {
        Json candidate = ParticleLightDocument();
        candidate["emitters"][0]["light"]["brightnessKeys"] =
            std::move(keysToParse);
        return ParticleAssetLoader::ParseJson(
            candidate, "Maps/SC_test/PS_light_curve_invalid.json");
    };

    EXPECT_THROW(parseKeys(Json::array({
        Json{{"time", 0.0}, {"brightness", 1.0}},
        Json{{"time", 0.5}, {"brightness", 1.0}},
        Json{{"time", 0.5}, {"brightness", 1.0}}})),
        std::invalid_argument);
    EXPECT_THROW(parseKeys(Json::array({
        Json{{"time", 0.5}, {"brightness", 1.0}},
        Json{{"time", 0.25}, {"brightness", 1.0}}})),
        std::invalid_argument);
    EXPECT_THROW(parseKeys(Json::array({
        Json{{"time", -0.01}, {"brightness", 1.0}}})),
        std::invalid_argument);
    EXPECT_THROW(parseKeys(Json::array({
        Json{{"time", 1.01}, {"brightness", 1.0}}})),
        std::invalid_argument);
    EXPECT_THROW(parseKeys(Json::array({
        Json{{"time", 0.0}, {"brightness", -0.01}}})),
        std::invalid_argument);
    EXPECT_THROW(parseKeys(Json::array({
        Json{{"time", 0.0}, {"brightness", 64.01}}})),
        std::invalid_argument);
    EXPECT_THROW(parseKeys(Json::array({
        Json{{"time", 0.0}, {"brightness", 1.0},
            {"color", Json::array({-0.01, 1.0, 1.0})}}})),
        std::invalid_argument);
    EXPECT_THROW(parseKeys(Json::array({
        Json{{"time", 0.0}, {"brightness", 1.0},
            {"color", Json::array({1.0, 64.01, 1.0})}}})),
        std::invalid_argument);
}

TEST(ParticleAsset, LightIesTextureIsIesProfileAliasButBothAreRejected)
{
    Json document = ParticleLightDocument();
    document["emitters"][0]["light"]["iesTexture"] =
        Json::array({1.0, 0.5, 0.25});
    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_light_ies_texture.json");
    ASSERT_EQ(asset.emitters.size(), 2u);
    ASSERT_EQ(asset.emitters[0].light.iesTexture.size(), 3u);
    ASSERT_EQ(asset.emitters[0].light.iesProfile.size(), 3u);
    EXPECT_FLOAT_EQ(asset.emitters[0].light.iesProfile[1], 0.5f);

    document = ParticleLightDocument();
    document["emitters"][0]["light"]["iesProfile"] =
        Json::array({1.0, 0.75});
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_light_ies_profile.json");
    ASSERT_EQ(asset.emitters[0].light.iesProfile.size(), 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].light.iesProfile[1], 0.75f);

    document["emitters"][0]["light"]["iesTexture"] =
        Json::array({1.0, 0.5});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_light_ies_conflict.json"),
        std::invalid_argument);
}

TEST(ParticleEditorFields, LightCurveCapabilityAndV2ModuleWriteBack)
{
    Json document = ParticleLightDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    ASSERT_TRUE(capabilities["emitters"].is_array());
    ASSERT_EQ(capabilities["emitters"].size(), 2u);
    const Json& emitterFields = capabilities["emitters"][0]["fields"];
    const Json* brightnessKeys = FindParticleCapability(
        emitterFields, "light.brightnessKeys");
    ASSERT_NE(brightnessKeys, nullptr);
    EXPECT_EQ(brightnessKeys->value("type", std::string()), "array");
    EXPECT_TRUE(brightnessKeys->value("resettable", false));
    // iesTexture 只是 iesProfile 的兼容别名，不在能力表里暴露第二行。
    EXPECT_EQ(FindParticleCapability(emitterFields, "light.iesTexture"),
        nullptr);

    Json v2 = Json{
        {"name", "PS_v2_light_curve"},
        {"type", "particleSystem"},
        {"version", 2},
        {"emitters", Json::array({Json{
            {"name", "glow"},
            {"modules", Json::array({
                Json{{"type", "emitter"}, {"fields", Json{
                    {"maxParticles", 64}}}},
                Json{{"type", "renderer"}, {"fields", Json{
                    {"renderMode", "light"},
                    {"light", Json{
                        {"intensity", 300.0},
                        {"radius", 700.0}}}}}}})}
        }})}
    };
    ApplyAssetChanges(
        v2, EditorAssetDocumentKind::ParticleSystem,
        {ParticleEditorChange(
            "emitters.0.light.brightnessKeys",
            Json::array({
                Json{{"time", 0.0}, {"brightness", 0.25},
                    {"color", Json::array({1.0, 0.5, 0.5})}},
                Json{{"time", 1.0}, {"brightness", 2.0},
                    {"color", Json::array({0.5, 0.5, 1.0})}}}))});

    // 字段必须写进 renderer 模块的 fields.light，而不是 emitter 根对象。
    EXPECT_FALSE(v2["emitters"][0].contains("light"));
    const Json& modules = v2["emitters"][0]["modules"];
    const Json* renderer = nullptr;
    for (const Json& module : modules)
    {
        if (module.value("type", std::string()) == "renderer")
            renderer = &module;
    }
    ASSERT_NE(renderer, nullptr);
    ASSERT_TRUE(renderer->at("fields").contains("light"));
    ASSERT_TRUE(renderer->at("fields").at("light").contains(
        "brightnessKeys"));

    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        v2, "Maps/SC_test/PS_v2_light_curve.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    ASSERT_EQ(asset.emitters[0].light.brightnessKeys.size(), 2u);
    EXPECT_FLOAT_EQ(asset.emitters[0].light.brightnessKeys[0].brightness,
        0.25f);
    EXPECT_TRUE(asset.emitters[0].light.brightnessKeys[1].color.isApprox(
        Eigen::Vector3f(0.5f, 0.5f, 1.0f)));

    ApplyAssetChanges(
        v2, EditorAssetDocumentKind::ParticleSystem,
        {ParticleEditorReset("emitters.0.light.brightnessKeys")});
    asset = ParticleAssetLoader::ParseJson(
        v2, "Maps/SC_test/PS_v2_light_curve_reset.json");
    EXPECT_TRUE(asset.emitters[0].light.brightnessKeys.empty());
}

TEST(ParticleAsset, V2EventsDeterminismAndRendererModulesFlatten)
{
    const Json document = Json{
        {"name", "PS_v2_new_fields"},
        {"type", "particleSystem"},
        {"version", 2},
        {"emitters", Json::array({Json{
            {"name", "sparks"},
            {"modules", Json::array({
                Json{{"type", "events"}, {"fields", Json{
                    {"events", Json{
                        {"spawn", true},
                        {"death", true},
                        {"collision", false}}}}}},
                Json{{"type", "emitter"}, {"fields", Json{
                    {"randomSeed", 9},
                    {"deterministicRandom", true},
                    {"fixedTimeStep", 0.02}}}},
                Json{{"type", "renderer"}, {"fields", Json{
                    {"billboardShadows", true},
                    {"mesh", Json{{"materialInstancePath",
                        "Maps/SC_test/Materials/MI_z.json"}}}}}}
            })}
        }})}
    };
    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_v2_new_fields.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    const ParticleEmitterAsset& emitter = asset.emitters[0];
    EXPECT_TRUE(emitter.emitSpawnEvent);
    EXPECT_TRUE(emitter.emitDeathEvent);
    EXPECT_FALSE(emitter.emitCollisionEvent);
    EXPECT_EQ(emitter.randomSeed, 9u);
    EXPECT_TRUE(emitter.deterministicRandom);
    EXPECT_FLOAT_EQ(emitter.fixedTimeStep, 0.02f);
    EXPECT_TRUE(emitter.billboardShadows);
    EXPECT_EQ(emitter.materialInstancePath,
        "Maps/SC_test/Materials/MI_z.json");
}

TEST(ParticleAsset, CurveInterpolationDefaultsAndRejectsUnknown)
{
    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        MultiEmitterDocument(),
        "Maps/SC_test/PS_curve_default.json");
    ASSERT_EQ(asset.emitters.size(), 2u);
    EXPECT_EQ(
        asset.emitters[0].curveInterpolation,
        ParticleCurveInterpolation::Linear);

    Json document = MultiEmitterDocument();
    document["emitters"][0]["curveInterpolation"] = "hermite";
    document["emitters"][1]["curveInterpolation"] = "bezier";
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_curve_modes.json");
    EXPECT_EQ(
        asset.emitters[0].curveInterpolation,
        ParticleCurveInterpolation::Hermite);
    EXPECT_EQ(
        asset.emitters[1].curveInterpolation,
        ParticleCurveInterpolation::Bezier);

    document["emitters"][0]["curveInterpolation"] = "spline";
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_curve_unknown.json"),
        std::invalid_argument);
    document["emitters"][0]["curveInterpolation"] = 1.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_curve_wrong_type.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, CurveTangentsParseForColorAndSize)
{
    Json document = MultiEmitterDocument();
    Json& emitter = document["emitters"][0];
    emitter["curveInterpolation"] = "bezier";
    emitter["colorKeys"] = Json::array({
        Json{{"time", 0.0},
             {"color", Json::array({1.0, 1.0, 1.0, 1.0})},
             {"tangentOut", Json::array({0.1, 0.2, 0.3})}},
        Json{{"time", 1.0},
             {"color", Json::array({0.0, 0.0, 0.0, 0.0})},
             {"tangentIn", Json::array({-0.4, -0.5, -0.6, -0.7})}}});
    emitter["sizeKeys"] = Json::array({
        Json{{"time", 0.0}, {"size", 5.0},
             {"tangentOut", 2.5}},
        Json{{"time", 1.0}, {"size", 20.0},
             {"tangentIn", -1.5}}});

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_curve_tangents.json");
    const ParticleEmitterAsset& parsed = asset.emitters[0];
    EXPECT_EQ(
        parsed.curveInterpolation,
        ParticleCurveInterpolation::Bezier);
    ASSERT_EQ(parsed.colorKeys.size(), 2u);
    EXPECT_TRUE(parsed.colorKeys[0].tangentOut.isApprox(
        Eigen::Vector4f(0.1f, 0.2f, 0.3f, 0.0f)));
    EXPECT_TRUE(parsed.colorKeys[1].tangentIn.isApprox(
        Eigen::Vector4f(-0.4f, -0.5f, -0.6f, -0.7f)));
    ASSERT_EQ(parsed.sizeKeys.size(), 2u);
    EXPECT_FLOAT_EQ(parsed.sizeKeys[0].tangentOut, 2.5f);
    EXPECT_FLOAT_EQ(parsed.sizeKeys[1].tangentIn, -1.5f);
}

TEST(ParticleAsset, CurveTangentsRejectNonFiniteValues)
{
    const double infinity =
        std::numeric_limits<double>::infinity();
    Json document = MultiEmitterDocument();
    document["emitters"][0]["colorKeys"] = Json::array({
        Json{{"time", 0.0},
             {"color", Json::array({1.0, 1.0, 1.0, 1.0})},
             {"tangentOut", Json::array({0.0, infinity, 0.0})}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_curve_inf_color.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["sizeKeys"] = Json::array({
        Json{{"time", 0.0}, {"size", 1.0},
             {"tangentIn", infinity}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_curve_inf_size.json"),
        std::invalid_argument);
}

TEST(ParticleAsset, HermiteOvershootWidensBoundsConservatively)
{
    const std::vector<ParticleScalarCurveSample> keys{
        {0.0f, 1.0f, 0.0f, 8.0f},
        {1.0f, 1.0f, 0.0f, 0.0f}};
    EXPECT_FLOAT_EQ(
        MaxParticleScalarCurveValue(
            keys, ParticleCurveInterpolation::Linear, 1.0f),
        1.0f);
    EXPECT_GT(
        MaxParticleScalarCurveValue(
            keys, ParticleCurveInterpolation::Hermite, 1.0f),
        1.0f);
    EXPECT_GT(
        MaxParticleScalarCurveValue(
            keys, ParticleCurveInterpolation::Bezier, 1.0f),
        2.0f);
}

TEST(ParticleEditorFields, CurveInterpolationAndTangentFieldsAreExposed)
{
    const Json document = MultiEmitterDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const Json& fields = capabilities["emitters"][0]["fields"];

    const Json* interpolation =
        FindParticleCapability(fields, "curveInterpolation");
    ASSERT_NE(interpolation, nullptr);
    EXPECT_EQ(interpolation->value("type", std::string()), "string");
    EXPECT_TRUE(interpolation->value("resettable", false));
    ASSERT_TRUE(interpolation->at("enum").is_array());
    EXPECT_NE(std::find(
        interpolation->at("enum").begin(),
        interpolation->at("enum").end(),
        Json("hermite")),
        interpolation->at("enum").end());
    EXPECT_NE(std::find(
        interpolation->at("enum").begin(),
        interpolation->at("enum").end(),
        Json("bezier")),
        interpolation->at("enum").end());

    for (const char* name : {"colorKeys", "sizeKeys"})
    {
        const Json* curve = FindParticleCapability(fields, name);
        ASSERT_NE(curve, nullptr) << name;
        ASSERT_TRUE(curve->contains("entryFields")) << name;
        EXPECT_NE(std::find(
            curve->at("entryFields").begin(),
            curve->at("entryFields").end(),
            Json("tangentIn")),
            curve->at("entryFields").end()) << name;
        EXPECT_NE(std::find(
            curve->at("entryFields").begin(),
            curve->at("entryFields").end(),
            Json("tangentOut")),
            curve->at("entryFields").end()) << name;
    }
}

TEST(ParticleEditorFields, CurveInterpolationV2WritesCurvesModuleAndResets)
{
    Json document = Json{
        {"name", "PS_v2_curve_interpolation"},
        {"type", "particleSystem"},
        {"version", 2},
        {"emitters", Json::array({Json{
            {"name", "spark"},
            {"modules", Json::array({
                Json{{"type", "curves"}, {"fields", Json::object()}}})}
        }})}
    };
    ApplyAssetChanges(
        document,
        EditorAssetDocumentKind::ParticleSystem,
        {ParticleEditorChange(
            "emitters.0.curveInterpolation", std::string("hermite"))});

    const Json& modules = document["emitters"][0]["modules"];
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].value("type", std::string()), "curves");
    EXPECT_EQ(
        modules[0].at("fields").value("curveInterpolation", std::string()),
        "hermite");

    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_v2_curve_interpolation.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_EQ(
        asset.emitters[0].curveInterpolation,
        ParticleCurveInterpolation::Hermite);

    ApplyAssetChanges(
        document,
        EditorAssetDocumentKind::ParticleSystem,
        {ParticleEditorReset("emitters.0.curveInterpolation")});
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_v2_curve_interpolation_reset.json");
    ASSERT_EQ(asset.emitters.size(), 1u);
    EXPECT_EQ(
        asset.emitters[0].curveInterpolation,
        ParticleCurveInterpolation::Linear);
}

TEST(ParticleAsset, FlipbookTimelineKeysParseAndValidate)
{
    Json document = MultiEmitterDocument();
    Json& flipbook = document["emitters"][0]["flipbook"];
    flipbook = Json{
        {"columns", 4},
        {"rows", 4},
        {"framePivotKeys", Json::array({
            Json{{"time", 0.0},
                 {"pivot", Json::array({0.25, 0.75})},
                 {"tangentOut", Json::array({0.5, -0.5})}},
            Json{{"time", 1.0},
                 {"pivot", Json::array({0.5, 0.5})},
                 {"tangentIn", Json::array({-0.5, 0.5})}}})},
        {"frameRotationKeys", Json::array({
            Json{{"time", 0.0}, {"degrees", -15.0},
                 {"tangentOut", 2.0}},
            Json{{"time", 1.0}, {"degrees", 45.0},
                 {"tangentIn", -2.0}}})}};

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_flipbook_timeline.json");
    ASSERT_EQ(asset.emitters.size(), 2u);
    const ParticleEmitterAsset& emitter = asset.emitters[0];
    ASSERT_EQ(emitter.flipbookFramePivotKeys.size(), 2u);
    EXPECT_FLOAT_EQ(
        emitter.flipbookFramePivotKeys[0].pivot.x(), 0.25f);
    EXPECT_FLOAT_EQ(
        emitter.flipbookFramePivotKeys[0].tangentOut.y(), -0.5f);
    ASSERT_EQ(emitter.flipbookFrameRotationKeys.size(), 2u);
    EXPECT_FLOAT_EQ(
        emitter.flipbookFrameRotationKeys[1].degrees, 45.0f);
    EXPECT_FLOAT_EQ(
        emitter.flipbookFrameRotationKeys[1].tangentIn, -2.0f);
}

TEST(ParticleAsset, FlipbookTimelineKeysRejectInvalidAuthoring)
{
    auto documentWithPivotKeys = [](const Json& keys)
    {
        Json document = MultiEmitterDocument();
        document["emitters"][0]["flipbook"] = Json{
            {"columns", 4},
            {"rows", 4},
            {"framePivotKeys", keys}};
        return document;
    };

    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            documentWithPivotKeys(Json::array()),
            "Maps/SC_test/PS_flipbook_empty_keys.json"),
        std::invalid_argument);
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            documentWithPivotKeys(Json::array({
                Json{{"time", 0.5}, {"pivot", Json::array({0.5, 0.5})}},
                Json{{"time", 0.5}, {"pivot", Json::array({0.5, 0.5})}}})),
            "Maps/SC_test/PS_flipbook_duplicate_times.json"),
        std::invalid_argument);
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            documentWithPivotKeys(Json::array({
                Json{{"time", 0.0},
                     {"pivot", Json::array({9.0, 0.5})}}})),
            "Maps/SC_test/PS_flipbook_pivot_range.json"),
        std::invalid_argument);
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            documentWithPivotKeys(Json::array({
                Json{{"time", 0.0},
                     {"pivot", Json::array({0.5, 0.5})},
                     {"tangentOut",
                      Json::array({0.0,
                          std::numeric_limits<double>::infinity()})}}})),
            "Maps/SC_test/PS_flipbook_inf_tangent.json"),
        std::invalid_argument);

    Json tooMany = MultiEmitterDocument();
    Json keys = Json::array();
    for (int index = 0; index < 9; ++index)
    {
        keys.push_back(Json{
            {"time", static_cast<double>(index) / 9.0},
            {"pivot", Json::array({0.5, 0.5})}});
    }
    tooMany["emitters"][0]["flipbook"] = Json{
        {"columns", 4}, {"rows", 4},
        {"framePivotKeys", keys}};
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            tooMany, "Maps/SC_test/PS_flipbook_many_keys.json"),
        std::invalid_argument);
}

TEST(ParticleEditorFields, FlipbookTimelineKeysAreExposed)
{
    const Json document = MultiEmitterDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const Json& fields = capabilities["emitters"][0]["fields"];
    for (const char* name : {
        "flipbook.framePivotKeys",
        "flipbook.frameRotationKeys"})
    {
        const Json* curve = FindParticleCapability(fields, name);
        ASSERT_NE(curve, nullptr) << name;
        ASSERT_TRUE(curve->contains("entryFields")) << name;
        EXPECT_NE(std::find(
            curve->at("entryFields").begin(),
            curve->at("entryFields").end(),
            Json("tangentIn")),
            curve->at("entryFields").end()) << name;
        EXPECT_NE(std::find(
            curve->at("entryFields").begin(),
            curve->at("entryFields").end(),
            Json("tangentOut")),
            curve->at("entryFields").end()) << name;
    }
}

TEST(ParticleEditorFields, LodTierRenderFieldsAreExposed)
{
    const Json document = MultiEmitterDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const Json& fields = capabilities["emitters"][0]["fields"];
    const Json* tiers = FindParticleCapability(fields, "lod.tiers");
    ASSERT_NE(tiers, nullptr);
    ASSERT_TRUE(tiers->contains("entryFields"));
    for (const char* name : {
        "distance", "spawnRateScale", "renderScale", "fadeRange"})
    {
        EXPECT_NE(std::find(
            tiers->at("entryFields").begin(),
            tiers->at("entryFields").end(),
            Json(name)),
            tiers->at("entryFields").end()) << name;
    }
}

TEST(ParticleAsset, RibbonBeamCurveTangentsAndShapeParse)
{
    Json document = MultiEmitterDocument();
    Json& emitter = document["emitters"][0];
    emitter["curveInterpolation"] = "bezier";
    emitter["ribbonPivot"] = 0.25;
    emitter["ribbonTwist"] = 45.0;
    emitter["ribbonWidthKeys"] = Json::array({
        Json{{"time", 0.0}, {"width", 1.0},
             {"tangentOut", 0.5}},
        Json{{"time", 1.0}, {"width", 0.5},
             {"tangentIn", -0.5}}});
    emitter["ribbonColorKeys"] = Json::array({
        Json{{"time", 0.0},
             {"color", Json::array({1.0, 0.0, 0.0, 1.0})},
             {"tangentOut", Json::array({0.1, 0.2, 0.3, 0.0})}}});
    emitter["beam"] = Json{
        {"colorKeys", Json::array({
            Json{{"time", 0.0},
                 {"color", Json::array({1.0, 1.0, 1.0, 1.0})},
                 {"tangentOut", Json::array({0.3, 0.3, 0.3, 0.0})}},
            Json{{"time", 1.0},
                 {"color", Json::array({0.0, 0.0, 1.0, 1.0})},
                 {"tangentIn", Json::array({-0.3, -0.3, 0.0, 0.0})}}})},
        {"widthKeys", Json::array({
            Json{{"time", 0.0}, {"width", 1.0},
                 {"tangentOut", 0.5}},
            Json{{"time", 1.0}, {"width", 0.2},
                 {"tangentIn", -0.5}}})}};

    const ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_ribbon_curve_tangents.json");
    const ParticleEmitterAsset& parsed = asset.emitters[0];
    EXPECT_FLOAT_EQ(parsed.ribbonPivot, 0.25f);
    EXPECT_FLOAT_EQ(parsed.ribbonTwist, 45.0f);
    ASSERT_EQ(parsed.ribbonWidthKeys.size(), 2u);
    EXPECT_FLOAT_EQ(parsed.ribbonWidthKeys[0].tangentOut, 0.5f);
    ASSERT_EQ(parsed.ribbonColorKeys.size(), 1u);
    EXPECT_FLOAT_EQ(parsed.ribbonColorKeys[0].tangentOut.x(), 0.1f);
    ASSERT_EQ(parsed.beam.colorKeys.size(), 2u);
    EXPECT_FLOAT_EQ(parsed.beam.colorKeys[0].tangentOut.x(), 0.3f);
    ASSERT_EQ(parsed.beam.widthKeys.size(), 2u);
    EXPECT_FLOAT_EQ(parsed.beam.widthKeys[1].tangentIn, -0.5f);
}

TEST(ParticleAsset, RibbonShapeAndCurveTangentRangesAreValidated)
{
    Json document = MultiEmitterDocument();
    document["emitters"][0]["ribbonPivot"] = 1.5;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_ribbon_pivot_range.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["ribbonTwist"] = 4000.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_ribbon_twist_range.json"),
        std::invalid_argument);

    document = MultiEmitterDocument();
    document["emitters"][0]["ribbonWidthKeys"] = Json::array({
        Json{{"time", 0.0}, {"width", 1.0},
             {"tangentOut",
              std::numeric_limits<double>::infinity()}}});
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_ribbon_inf_tangent.json"),
        std::invalid_argument);
}

TEST(ParticleEditorFields, RibbonAndBeamCurveTangentFieldsAreExposed)
{
    const Json document = MultiEmitterDocument();
    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem, document);
    const Json& fields = capabilities["emitters"][0]["fields"];
    for (const char* name : {
        "ribbonWidthKeys", "ribbonColorKeys",
        "beam.colorKeys", "beam.widthKeys"})
    {
        const Json* curve = FindParticleCapability(fields, name);
        ASSERT_NE(curve, nullptr) << name;
        ASSERT_TRUE(curve->contains("entryFields")) << name;
        EXPECT_NE(std::find(
            curve->at("entryFields").begin(),
            curve->at("entryFields").end(),
            Json("tangentIn")),
            curve->at("entryFields").end()) << name;
    }
    EXPECT_NE(FindParticleCapability(fields, "ribbonPivot"), nullptr);
    EXPECT_NE(FindParticleCapability(fields, "ribbonTwist"), nullptr);
}

TEST(ParticleAsset, SoftParticleThicknessParsesAndValidates)
{
    ParticleSystemAsset asset = ParticleAssetLoader::ParseJson(
        MultiEmitterDocument(), "Maps/SC_test/PS_soft_default.json");
    EXPECT_FLOAT_EQ(asset.emitters[0].softParticleThickness, 0.0f);

    Json document = MultiEmitterDocument();
    document["emitters"][0]["softParticleThickness"] = 40.0;
    asset = ParticleAssetLoader::ParseJson(
        document, "Maps/SC_test/PS_soft_thickness.json");
    EXPECT_FLOAT_EQ(asset.emitters[0].softParticleThickness, 40.0f);

    document["emitters"][0]["softParticleThickness"] = -1.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_soft_thickness_negative.json"),
        std::invalid_argument);
    document["emitters"][0]["softParticleThickness"] = 10001.0;
    EXPECT_THROW(
        ParticleAssetLoader::ParseJson(
            document, "Maps/SC_test/PS_soft_thickness_large.json"),
        std::invalid_argument);

    const Json capabilities = DescribeAssetCapabilities(
        EditorAssetDocumentKind::ParticleSystem,
        MultiEmitterDocument());
    EXPECT_NE(
        FindParticleCapability(
            capabilities["emitters"][0]["fields"],
            "softParticleThickness"),
        nullptr);
}
