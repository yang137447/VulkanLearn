#include "animation/motionDatabase.h"
#include "motionDatabaseBuild.h"
#include "shader/build/contentHash.h"
#include <fstream>
#include <gtest/gtest.h>

namespace
{
using namespace VL::Animation;

TEST(MotionDatabase, RejectsCorruptLayoutAndInvalidStatistics)
{
    MotionDatabase database;
    database.schema.joints = {"root"};
    database.clipIds = {"walk"};
    database.looping = {true};
    const auto dimensions = FeatureChannelNames(database.schema).size();
    database.means.assign(dimensions, 0);
    database.scales.assign(dimensions, 1);
    database.frames.push_back({0, 0, std::vector<float>(dimensions)});
    auto json = MotionDatabaseJson(database);
    EXPECT_NO_THROW(ParseMotionDatabase(json));
    json["scales"][0] = 0;
    EXPECT_THROW(ParseMotionDatabase(json), std::invalid_argument);
    json = MotionDatabaseJson(database);
    json["channels"][0] = "wrong-axis";
    EXPECT_THROW(ParseMotionDatabase(json), std::invalid_argument);
    json = MotionDatabaseJson(database);
    json["frames"][0]["clip"] = 1;
    EXPECT_THROW(ParseMotionDatabase(json), std::invalid_argument);
}

class MotionDatabaseInputs : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() /
            ("vl-motion-inputs-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(root);
        FeatureSchema schema;
        schema.joints = {"root"};
        Write("schema.json", FeatureSchemaJson(schema).dump());
        Write("mesh.fbx", "mesh bytes");
        Write("walk.fbx", "walk bytes");
        Write("run.fbx", "run bytes");
        Write("skeleton.json", R"({"format":"vl.skeleton.v1","skeleton":"SK_body","root":"root",
            "joints":[]})");
        Write("model.json", R"({"type":"mesh","modelDataPath":"mesh.fbx","skinning":{
            "skeleton":"skeleton.json",
            "clips":[{"id":"walk","path":"walk.fbx"},{"id":"run","path":"run.fbx"}]}})");
        Write("source.json", R"({"type":"motionDatabaseSource","version":1,
            "boundaryPolicy":"exclude-nonloop-extend-loop-v1","schema":"schema.json",
            "model":"model.json","members":[{"clip":"walk","loop":true},{"clip":"run","loop":true}]})");
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    void Write(const std::string& path, const std::string& bytes)
    {
        std::ofstream file(root / path, std::ios::binary);
        file << bytes;
    }
    nlohmann::json Capture() { return VL::AnimationPreflight::CaptureMotionDatabaseInputs(root, "source.json"); }
    std::filesystem::path root;
};

TEST_F(MotionDatabaseInputs, StableBytesProduceStableIdentityAndFbxChangesInvalidate)
{
    const auto first = Capture();
    EXPECT_EQ(first, Capture());
    Write("walk.fbx", "changed motion");
    EXPECT_NE(first.at("identity"), Capture().at("identity"));
}

TEST_F(MotionDatabaseInputs, SkeletonBytesArePartOfIdentity)
{
    const auto first = Capture();
    // 根骨与关节来自 SK；改 SK 必须让旧 motion 缓存失效。
    Write("skeleton.json", R"({"format":"vl.skeleton.v1","skeleton":"SK_body","root":"root",
        "joints":[],"note":"changed"})");
    EXPECT_NE(first.at("identity"), Capture().at("identity"));
}

TEST_F(MotionDatabaseInputs, RejectsModelWithoutSkeletonAsset)
{
    Write("model.json", R"({"type":"mesh","modelDataPath":"mesh.fbx","skinning":{
        "clips":[{"id":"walk","path":"walk.fbx"},{"id":"run","path":"run.fbx"}]}})");
    EXPECT_THROW(Capture(), std::invalid_argument);
}

TEST_F(MotionDatabaseInputs, SchemaWeightsAndMemberOrderInvalidateIdentity)
{
    const auto first = Capture();
    auto schema = first.at("schema");
    schema["weights"]["posePosition"] = 2;
    Write("schema.json", schema.dump());
    EXPECT_NE(first.at("identity"), Capture().at("identity"));
    Write("schema.json", first.at("schema").dump());
    const auto restored = Capture();
    auto source = restored.at("source");
    std::swap(source["members"][0], source["members"][1]);
    Write("source.json", source.dump());
    EXPECT_NE(restored.at("identity"), Capture().at("identity"));
}

TEST_F(MotionDatabaseInputs, RejectsPathEscapeAndImplicitLoopPolicy)
{
    auto source = Capture().at("source");
    source["schema"] = "../schema.json";
    Write("source.json", source.dump());
    EXPECT_THROW(Capture(), std::invalid_argument);
    source["schema"] = "schema.json";
    source["members"][0].erase("loop");
    Write("source.json", source.dump());
    EXPECT_ANY_THROW(Capture());
}

TEST_F(MotionDatabaseInputs, RejectsSelfConsistentCacheWithDifferentSourceSchema)
{
    const auto captured = Capture();
    MotionDatabase database;
    database.schema = ParseFeatureSchema(captured.at("schema"));
    database.clipIds = {"walk", "run"};
    database.looping = {true, true};
    const auto dimensions = FeatureChannelNames(database.schema).size();
    database.means.assign(dimensions, 0); database.scales.assign(dimensions, 1);
    database.frames.push_back({0, 0, std::vector<float>(dimensions)});
    database.frames.push_back({1, 0, std::vector<float>(dimensions)});
    const auto path = "Generated/MotionMatching/" + captured.at("identity").get<std::string>() + ".json";
    std::filesystem::create_directories((root / path).parent_path());
    auto cache = nlohmann::json{{"identity", captured.at("identity")}, {"inputs", captured.at("inputs")},
        {"database", MotionDatabaseJson(database)}};
    cache["payloadBlake3"] = VL::ContentHasher::HashString(cache.at("database").dump()).ToHex();
    Write(path, cache.dump());
    EXPECT_EQ(VL::AnimationPreflight::BuildMotionDatabaseReport(root, "source.json").at("cacheStatus"), "verified-hit");
    database.schema.posePositionWeight = 2;
    cache["database"] = MotionDatabaseJson(database);
    cache["payloadBlake3"] = VL::ContentHasher::HashString(cache.at("database").dump()).ToHex();
    Write(path, cache.dump());
    // 摘要自洽仍不是同源 schema。拒绝命中后会尝试导入；夹具的伪 FBX 使重建明确失败。
    EXPECT_ANY_THROW(VL::AnimationPreflight::BuildMotionDatabaseReport(root, "source.json"));
}
}
