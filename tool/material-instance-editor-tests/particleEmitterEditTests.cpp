#include <string>

#include <gtest/gtest.h>

#include "particles/particleAsset.h"
#include "particles/particleEmitterEdit.h"

namespace
{

using namespace VL;
using Json = nlohmann::json;

const std::string kSourcePath = "Maps/SC_test/PS_emitter_edit.json";

Json LegacyDocument()
{
    return Json{
        {"name", "PS_legacy"},
        {"type", "particleSystem"},
        {"position", {100.0, 0.0, 0.0}},
        {"maxParticles", 256},
        {"spawnRate", 40.0},
        {"lifetime", {1.0, 2.0}}};
}

Json MultiEmitterDocument()
{
    return Json{
        {"name", "PS_multi"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{{"name", "fire"},
                {"maxParticles", 512},
                {"spawnRate", 120.0},
                {"position", {5.0, 0.0, 0.0}}},
            Json{{"name", "smoke"},
                {"maxParticles", 2048},
                {"spawnRate", 30.0}}})}};
}

} // namespace

TEST(ParticleEmitterEdit, ConvertLegacyDocumentToExplicitEmitters)
{
    Json document = LegacyDocument();
    ASSERT_TRUE(EnsureExplicitParticleEmitters(document));
    ASSERT_TRUE(document.contains("emitters"));
    ASSERT_TRUE(document.at("emitters").is_array());
    ASSERT_EQ(document.at("emitters").size(), 1u);

    const Json& emitter = document.at("emitters").at(0);
    EXPECT_EQ(emitter.at("name"), "emitter0");
    EXPECT_FALSE(emitter.contains("type"));
    EXPECT_FALSE(emitter.contains("emitters"));
    // 隐式 emitter 没有局部 position 语义，升级时必须丢弃根 position。
    EXPECT_FALSE(emitter.contains("position"));
    EXPECT_EQ(emitter.at("maxParticles"), 256);
    EXPECT_EQ(emitter.at("spawnRate"), 40.0);

    // 根对象仍是系统容器，保留 name/type 与其余根字段。
    EXPECT_EQ(document.at("name"), "PS_legacy");
    EXPECT_EQ(document.at("type"), "particleSystem");
    EXPECT_EQ(document.at("position"), Json({100.0, 0.0, 0.0}));

    // 已经是显式文档时保持原样。
    const Json before = document;
    EXPECT_TRUE(EnsureExplicitParticleEmitters(document));
    EXPECT_EQ(document, before);
}

TEST(ParticleEmitterEdit, EnsureRejectsNonObjectDocument)
{
    Json document = Json::array();
    EXPECT_FALSE(EnsureExplicitParticleEmitters(document));
    EXPECT_TRUE(document.is_array());
}

TEST(ParticleEmitterEdit, AddOnLegacyDocumentCopiesRootFields)
{
    Json document = LegacyDocument();
    size_t selected = 0;
    EXPECT_EQ(AddParticleEmitter(document, selected),
        ParticleEmitterEditStatus::Ok);
    ASSERT_TRUE(document.contains("emitters"));
    const Json& emitters = document.at("emitters");
    ASSERT_EQ(emitters.size(), 2u);
    EXPECT_EQ(emitters.at(0).at("name"), "emitter0");
    EXPECT_EQ(emitters.at(1).at("name"), "emitter0_copy");
    EXPECT_EQ(selected, 1u);

    EXPECT_EQ(emitters.at(1).at("maxParticles"), 256);
    EXPECT_EQ(emitters.at(1).at("spawnRate"), 40.0);
    EXPECT_EQ(emitters.at(1).at("lifetime"), Json({1.0, 2.0}));
    EXPECT_FALSE(emitters.at(1).contains("position"));
    EXPECT_NO_THROW((void)ParticleAssetLoader::ParseJson(
        document, kSourcePath));
}

TEST(ParticleEmitterEdit, AddInsertsAfterSelectionAndKeepsNamesUnique)
{
    Json document = MultiEmitterDocument();
    size_t selected = 0;
    ASSERT_EQ(AddParticleEmitter(document, selected),
        ParticleEmitterEditStatus::Ok);
    ASSERT_EQ(document.at("emitters").size(), 3u);
    EXPECT_EQ(selected, 1u);
    EXPECT_EQ(document.at("emitters").at(1).at("name"), "fire_copy");
    // 复制必须保留原发射器的全部作者字段，包括局部 position。
    EXPECT_EQ(document.at("emitters").at(1).at("maxParticles"), 512);
    EXPECT_EQ(document.at("emitters").at(1).at("spawnRate"), 120.0);
    EXPECT_EQ(document.at("emitters").at(1).at("position"),
        Json({5.0, 0.0, 0.0}));
    EXPECT_EQ(document.at("emitters").at(2).at("name"), "smoke");
    EXPECT_NO_THROW((void)ParticleAssetLoader::ParseJson(
        document, kSourcePath));

    // fire_copy 已被占用时按 _2 递增。
    selected = 0;
    ASSERT_EQ(AddParticleEmitter(document, selected),
        ParticleEmitterEditStatus::Ok);
    EXPECT_EQ(document.at("emitters").at(1).at("name"),
        "fire_copy_2");
    EXPECT_EQ(document.at("emitters").at(2).at("name"), "fire_copy");
    EXPECT_NO_THROW((void)ParticleAssetLoader::ParseJson(
        document, kSourcePath));
}

TEST(ParticleEmitterEdit, AddUsesDefaultNamesForUnnamedEntries)
{
    Json document = Json{
        {"name", "PS_unnamed"},
        {"type", "particleSystem"},
        {"emitters", Json::array({Json::object(), Json{{"name", "x"}}})}};
    size_t selected = 0;
    ASSERT_EQ(AddParticleEmitter(document, selected),
        ParticleEmitterEditStatus::Ok);
    EXPECT_EQ(document.at("emitters").at(1).at("name"),
        "emitter0_copy");

    selected = 0;
    ASSERT_EQ(AddParticleEmitter(document, selected),
        ParticleEmitterEditStatus::Ok);
    EXPECT_EQ(document.at("emitters").at(1).at("name"),
        "emitter0_copy_2");
}

TEST(ParticleEmitterEdit, AddRejectsWhenAtLimit)
{
    Json emitters = Json::array();
    for (size_t index = 0; index < 16; ++index)
        emitters.push_back(Json{{"name", "e" + std::to_string(index)}});
    Json document = Json{
        {"name", "PS_full"},
        {"type", "particleSystem"},
        {"emitters", emitters}};
    const Json before = document;
    size_t selected = 3;
    EXPECT_EQ(AddParticleEmitter(document, selected),
        ParticleEmitterEditStatus::MaxEmitters);
    EXPECT_EQ(document, before);
    EXPECT_EQ(selected, 3u);
}

TEST(ParticleEmitterEdit, AddRejectsOutOfRangeSelection)
{
    Json document = MultiEmitterDocument();
    const Json before = document;
    size_t selected = 2;
    EXPECT_EQ(AddParticleEmitter(document, selected),
        ParticleEmitterEditStatus::InvalidIndex);
    EXPECT_EQ(document, before);
    EXPECT_EQ(selected, 2u);
}

TEST(ParticleEmitterEdit, AddRejectsOutOfRangeSelectionWithoutUpgradingLegacy)
{
    Json document = LegacyDocument();
    const Json before = document;
    size_t selected = 5;
    EXPECT_EQ(AddParticleEmitter(document, selected),
        ParticleEmitterEditStatus::InvalidIndex);
    EXPECT_FALSE(document.contains("emitters"));
    EXPECT_EQ(document, before);
    EXPECT_EQ(selected, 5u);
}

TEST(ParticleEmitterEdit, RemoveKeepsAtLeastOneEmitter)
{
    Json legacy = LegacyDocument();
    const Json before = legacy;
    size_t selected = 0;
    EXPECT_EQ(RemoveParticleEmitter(legacy, selected),
        ParticleEmitterEditStatus::MinEmitters);
    EXPECT_EQ(legacy, before);

    Json single = Json{
        {"name", "PS_single"},
        {"type", "particleSystem"},
        {"emitters", Json::array({Json{{"name", "only"}}})}};
    const Json singleBefore = single;
    EXPECT_EQ(RemoveParticleEmitter(single, selected),
        ParticleEmitterEditStatus::MinEmitters);
    EXPECT_EQ(single, singleBefore);
    EXPECT_EQ(selected, 0u);
}

TEST(ParticleEmitterEdit, RemoveClampsSelection)
{
    Json document = MultiEmitterDocument();
    size_t selected = 1;
    ASSERT_EQ(RemoveParticleEmitter(document, selected),
        ParticleEmitterEditStatus::Ok);
    ASSERT_EQ(document.at("emitters").size(), 1u);
    EXPECT_EQ(document.at("emitters").at(0).at("name"), "fire");
    EXPECT_EQ(selected, 0u);
    EXPECT_NO_THROW((void)ParticleAssetLoader::ParseJson(
        document, kSourcePath));

    Json three = MultiEmitterDocument();
    three["emitters"].push_back(Json{{"name", "sparks"}});
    selected = 2;
    ASSERT_EQ(RemoveParticleEmitter(three, selected),
        ParticleEmitterEditStatus::Ok);
    ASSERT_EQ(three.at("emitters").size(), 2u);
    EXPECT_EQ(selected, 1u);
    EXPECT_EQ(three.at("emitters").at(1).at("name"), "smoke");
    EXPECT_NO_THROW((void)ParticleAssetLoader::ParseJson(
        three, kSourcePath));
}

TEST(ParticleEmitterEdit, MoveSwapsNeighborsAndFollowsSelection)
{
    Json document = MultiEmitterDocument();
    document["emitters"].push_back(Json{{"name", "sparks"}});
    size_t selected = 1;
    ASSERT_EQ(MoveParticleEmitter(document, selected, -1),
        ParticleEmitterEditStatus::Ok);
    EXPECT_EQ(selected, 0u);
    EXPECT_EQ(document.at("emitters").at(0).at("name"), "smoke");
    EXPECT_EQ(document.at("emitters").at(1).at("name"), "fire");
    EXPECT_EQ(document.at("emitters").at(2).at("name"), "sparks");

    ASSERT_EQ(MoveParticleEmitter(document, selected, 1),
        ParticleEmitterEditStatus::Ok);
    EXPECT_EQ(selected, 1u);
    EXPECT_EQ(document.at("emitters").at(0).at("name"), "fire");
    EXPECT_EQ(document.at("emitters").at(1).at("name"), "smoke");
    EXPECT_NO_THROW((void)ParticleAssetLoader::ParseJson(
        document, kSourcePath));
}

TEST(ParticleEmitterEdit, MoveOutOfRangeLeavesDocumentUntouched)
{
    Json document = MultiEmitterDocument();
    const Json before = document;
    size_t selected = 0;
    EXPECT_EQ(MoveParticleEmitter(document, selected, -1),
        ParticleEmitterEditStatus::NoMove);
    EXPECT_EQ(document, before);
    EXPECT_EQ(selected, 0u);

    selected = 1;
    EXPECT_EQ(MoveParticleEmitter(document, selected, 1),
        ParticleEmitterEditStatus::NoMove);
    EXPECT_EQ(document, before);
    EXPECT_EQ(selected, 1u);

    EXPECT_EQ(MoveParticleEmitter(document, selected, 0),
        ParticleEmitterEditStatus::NoMove);
    EXPECT_EQ(document, before);

    Json single = LegacyDocument();
    const Json singleBefore = single;
    selected = 0;
    EXPECT_EQ(MoveParticleEmitter(single, selected, 1),
        ParticleEmitterEditStatus::NoMove);
    EXPECT_EQ(single, singleBefore);
}

TEST(ParticleEmitterEdit, MoveRejectsOutOfRangeSelection)
{
    Json document = MultiEmitterDocument();
    const Json before = document;
    size_t selected = 5;
    EXPECT_EQ(MoveParticleEmitter(document, selected, -1),
        ParticleEmitterEditStatus::InvalidIndex);
    EXPECT_EQ(document, before);
    EXPECT_EQ(selected, 5u);
}
