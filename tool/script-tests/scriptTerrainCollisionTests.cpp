#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "resource/image/hostImage.h"
#include "script/scriptPhysics.h"
#include "script/scriptRuntime.h"
#include "terrain/terrainCollisionTypes.h"
#include "terrain/terrainDisplacementSampler.h"
#include "terrain/terrainHeightmap.h"
#include "world/world.h"

namespace
{

using Json = nlohmann::json;

std::shared_ptr<const HostImage> FlatHeightmap(float value = 1.0f)
{
    HostImage image;
    image.width = 4;
    image.height = 4;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = image.width * image.GetPixelSize();
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * image.height);
    float* pixels = reinterpret_cast<float*>(image.data.data());
    for (size_t index = 0; index < image.width * image.height; ++index)
    {
        pixels[index * 4 + 0] = value;
        pixels[index * 4 + 1] = value;
        pixels[index * 4 + 2] = value;
        pixels[index * 4 + 3] = 1.0f;
    }
    return std::make_shared<const HostImage>(std::move(image));
}

VL::TerrainBuildPlan Plan(const std::string& placementId)
{
    VL::TerrainBuildPlan plan;
    plan.desc.assetPath = "Generated/Validation/TerrainTests/TR_script_collision.json";
    plan.desc.size = Eigen::Vector2f(10000.0f, 10000.0f);
    plan.desc.heightScale = 500.0f;
    plan.desc.gridResolution = 64;
    plan.desc.chunkQuads = 16;
    plan.heightmap = FlatHeightmap();
    plan.placementId = placementId;
    return plan;
}

VL::TerrainCollisionInstance Instance(
    const std::string& placementId,
    const Eigen::Vector3f& position)
{
    VL::TerrainCollisionInstance instance;
    instance.placementId = placementId;
    instance.plan = Plan(placementId);
    instance.placement.position = position;
    return instance;
}

Json StaticProbeState()
{
    return {
        {"id", "probe"},
        {"position", {1000.0, 6000.0, -2000.0}},
        {"rotation", {0.0, 0.0, 0.0}},
        {"scale", {1.0, 1.0, 1.0}}};
}

Json StaticProbeDeclarations()
{
    return {
        {"probe", {
            {"id", "probe"},
            {"position", {1000.0, 6000.0, -2000.0}},
            {"rotation", {0.0, 0.0, 0.0}},
            {"scale", {1.0, 1.0, 1.0}},
            {"physics", {
                {"shape", "box"},
                {"motion", "static"},
                {"halfExtents", {50.0, 50.0, 50.0}}}}}}};
}

uint32_t Crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < size; ++index)
    {
        crc ^= data[index];
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void AppendBigEndian32(std::vector<uint8_t>& output, uint32_t value)
{
    output.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
    output.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
    output.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    output.push_back(static_cast<uint8_t>(value & 0xFFu));
}

void AppendPngChunk(
    std::vector<uint8_t>& output,
    const char* type,
    const std::vector<uint8_t>& data)
{
    AppendBigEndian32(output, static_cast<uint32_t>(data.size()));
    const size_t typeOffset = output.size();
    output.insert(output.end(), type, type + 4);
    output.insert(output.end(), data.begin(), data.end());
    AppendBigEndian32(
        output, Crc32(output.data() + typeOffset, output.size() - typeOffset));
}

bool WritePng16(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    const std::vector<uint16_t>& samples)
{
    if (samples.size() != static_cast<size_t>(width) * height)
        return false;
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint16_t value =
                samples[static_cast<size_t>(y) * width + x];
            raw.push_back(static_cast<uint8_t>(value >> 8u));
            raw.push_back(static_cast<uint8_t>(value & 0xFFu));
        }
    }
    std::vector<uint8_t> compressed = {0x78, 0x01};
    const uint16_t length = static_cast<uint16_t>(raw.size());
    const uint16_t inverse = static_cast<uint16_t>(~length);
    compressed.push_back(0x01);
    compressed.push_back(static_cast<uint8_t>(length & 0xFFu));
    compressed.push_back(static_cast<uint8_t>((length >> 8u) & 0xFFu));
    compressed.push_back(static_cast<uint8_t>(inverse & 0xFFu));
    compressed.push_back(static_cast<uint8_t>((inverse >> 8u) & 0xFFu));
    compressed.insert(compressed.end(), raw.begin(), raw.end());
    uint32_t first = 1;
    uint32_t second = 0;
    for (const uint8_t byte : raw)
    {
        first = (first + byte) % 65521u;
        second = (second + first) % 65521u;
    }
    AppendBigEndian32(compressed, (second << 16u) | first);

    std::vector<uint8_t> image = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> header(13, 0);
    header[0] = static_cast<uint8_t>(width >> 24u);
    header[1] = static_cast<uint8_t>(width >> 16u);
    header[2] = static_cast<uint8_t>(width >> 8u);
    header[3] = static_cast<uint8_t>(width);
    header[4] = static_cast<uint8_t>(height >> 24u);
    header[5] = static_cast<uint8_t>(height >> 16u);
    header[6] = static_cast<uint8_t>(height >> 8u);
    header[7] = static_cast<uint8_t>(height);
    header[8] = 16;
    header[9] = 0;
    AppendPngChunk(image, "IHDR", header);
    AppendPngChunk(image, "IDAT", compressed);
    AppendPngChunk(image, "IEND", {});

    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
        return false;
    stream.write(
        reinterpret_cast<const char*>(image.data()),
        static_cast<std::streamsize>(image.size()));
    return static_cast<bool>(stream);
}

void WriteJson(const std::filesystem::path& path, const Json& json)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream << json.dump(2);
    ASSERT_TRUE(static_cast<bool>(stream));
}

std::shared_ptr<const HostImage> FlatHeightmap32(float value)
{
    HostImage image;
    image.width = 33;
    image.height = 33;
    image.channels = 1;
    image.format = HostImage::PixelFormat::R32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = image.width * sizeof(float);
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * image.height);
    float* pixels = reinterpret_cast<float*>(image.data.data());
    for (size_t index = 0; index < static_cast<size_t>(image.width) * image.height;
         ++index)
        pixels[index] = value;
    return std::make_shared<const HostImage>(std::move(image));
}

VL::TerrainCollisionInstance DisplacementInstance(
    const std::filesystem::path& resourceRoot,
    const char* materialInstancePath,
    float displacementM)
{
    VL::TerrainCollisionInstance instance;
    instance.placementId = "terrain|Displacement";
    instance.plan.desc.assetPath = "Generated/Validation/TerrainTests/TR_displacement.json";
    instance.plan.desc.size = Eigen::Vector2f(10000.0f, 10000.0f);
    instance.plan.desc.heightScale = 1000.0f;
    instance.plan.desc.gridResolution = 32;
    instance.plan.desc.chunkQuads = 16;
    instance.plan.desc.pageTilesPerSide = 1;
    instance.plan.desc.materialInstancePath = materialInstancePath;
    VL::TerrainLayerDesc layer;
    layer.name = "land";
    layer.heightMin = 0.0f;
    layer.heightMax = 1.0f;
    layer.slopeMin = 0.0f;
    layer.slopeMax = 90.0f;
    layer.uvScale = 1.0f;
    layer.displacementM = displacementM;
    instance.plan.desc.layers = {layer};
    instance.plan.heightmap = FlatHeightmap32(0.5f);
    instance.placement.position = Eigen::Vector3f(1000.0f, 3000.0f, -2000.0f);
    (void)resourceRoot;
    return instance;
}

} // namespace

TEST(ScriptTerrainCollision, SetTerrainCollisionsCreatesOneBodyPerPlacement)
{
    VL::ScriptPhysicsWorld physics;
    physics.SetTerrainCollisions({
        Instance("terrain|A", Eigen::Vector3f(-6000.0f, 0.0f, 0.0f)),
        Instance("terrain|B", Eigen::Vector3f(6000.0f, 100.0f, 0.0f))});

    EXPECT_EQ(physics.TerrainCollisionBodyCount(), 2u);
    const auto placements = physics.TerrainCollisionPlacementIds();
    ASSERT_EQ(placements.size(), 2u);
    EXPECT_NE(std::find(placements.begin(), placements.end(), "terrain|A"), placements.end());
    EXPECT_NE(std::find(placements.begin(), placements.end(), "terrain|B"), placements.end());
}

TEST(ScriptTerrainCollision, SetTerrainCollisionsReplacesInsteadOfAppending)
{
    VL::ScriptPhysicsWorld physics;
    physics.SetTerrainCollisions({Instance("terrain|A", Eigen::Vector3f::Zero())});
    EXPECT_EQ(physics.TerrainCollisionBodyCount(), 1u);

    physics.SetTerrainCollisions({
        Instance("terrain|A", Eigen::Vector3f::Zero()),
        Instance("terrain|B", Eigen::Vector3f(1000.0f, 0.0f, 0.0f))});
    EXPECT_EQ(physics.TerrainCollisionBodyCount(), 2u);

    physics.SetTerrainCollisions({});
    EXPECT_EQ(physics.TerrainCollisionBodyCount(), 0u);
    EXPECT_TRUE(physics.TerrainCollisionPlacementIds().empty());
}

TEST(ScriptTerrainCollision, RaycastHitsHeightfieldAtCpuSampledHeight)
{
    VL::ScriptPhysicsWorld physics;
    const VL::TerrainCollisionInstance instance =
        Instance("terrain|Probe", Eigen::Vector3f(1000.0f, 3000.0f, -2000.0f));
    physics.SetTerrainCollisions({instance});

    const float expected = VL::TerrainHeightmap::SampleHeightCm(
        instance.plan.desc, *instance.plan.heightmap, 0.5f, 0.5f) +
        instance.placement.position.y();
    const Json request = {
        {"id", "ground"},
        {"owner", "none"},
        {"from", {1000.0, 6000.0, -2000.0}},
        {"to", {1000.0, 0.0, -2000.0}}};
    const Json hits = physics.Raycast(Json::array({request}));
    ASSERT_EQ(hits.size(), 1u);
    ASSERT_TRUE(hits[0].contains("point")) << hits[0].dump();
    EXPECT_NEAR(hits[0].at("point")[1].get<float>(), expected, 0.5f);
    EXPECT_FALSE(hits[0].contains("blueprint"));
    EXPECT_EQ(hits[0].at("terrain").get<std::string>(), "terrain|Probe");
}

TEST(ScriptTerrainCollision, ProbeGroundFindsTerrainBelowAScriptBody)
{
    VL::ScriptPhysicsWorld physics;
    physics.SetTerrainCollisions({
        Instance("terrain|Probe", Eigen::Vector3f(1000.0f, 3000.0f, -2000.0f))});

    Json states = Json::array({StaticProbeState()});
    const Json declarations = StaticProbeDeclarations();
    physics.Step(states, declarations, 0.0f);

    const VL::Animation::FootGroundHit hit = physics.ProbeGround(
        "probe",
        Eigen::Vector3f(1000.0f, 6000.0f, -2000.0f),
        Eigen::Vector3f(0.0f, -3000.0f, 0.0f));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.y(), 3500.0f, 0.5f);
    EXPECT_EQ(hit.supportIdentity, "terrain|Probe");
}

TEST(ScriptTerrainCollision, PlainWorldDoesNotCreateAPhysicsWorld)
{
    VL::ScriptRuntimeSubsystem runtime;
    auto world = std::make_shared<VL::World>(1, "scene.json");
    runtime.SynchronizeWorld(world);
    EXPECT_FALSE(runtime.HasPhysicsWorld());
    runtime.SynchronizeWorld(nullptr);
    EXPECT_FALSE(runtime.HasPhysicsWorld());
}

TEST(ScriptTerrainCollision, ProbeGroundReturnsVisualDisplacementSurface)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "vl_script_terrain_displacement";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    WriteJson(
        root / "MI_terrain_synthetic.json",
        Json{
            {"name", "MI_terrain_synthetic"},
            {"type", "materialInstance"},
            {"material", "shader/glsl/M_terrain.json"},
            {"parameters", {
                {"u_uvScale", 4.0},
                {"u_detailUvScale", 2.0},
                {"u_layerDisplacementScale", 1.0},
                {"u_terrainWeightMapMode", 0.0}}},
            {"textures", {
                {"layerHeight", "height_array.json"}}}});
    WriteJson(
        root / "height_array.json",
        Json{
            {"name", "height_array"},
            {"type", "textureArray"},
            {"sources", Json::array({"layer0.png"})},
            {"colorSpace", "linear"},
            {"mipmaps", true},
            {"filter", "linear"},
            {"wrapMode", "repeat"},
            {"bitDepth", 16}});
    ASSERT_TRUE(WritePng16(root / "layer0.png", 4, 4, std::vector<uint16_t>(16, 0xFFFFu)));

    // 宏高度 = 0.5 * 1000 = 500，placement y=3000；主导层 displacementM=0.4，
    // 16-bit 层高度为 1.0，所以视觉表面应比宏高度高 20 cm。
    VL::ScriptPhysicsWorld physics;
    const VL::TerrainCollisionInstance instance =
        DisplacementInstance(root, "MI_terrain_synthetic.json", 0.4f);
    physics.SetTerrainCollisions({instance}, root);
    EXPECT_EQ(physics.TerrainDisplacementSamplerCount(), 1u);

    Json states = Json::array({StaticProbeState()});
    physics.Step(states, StaticProbeDeclarations(), 0.0f);
    const VL::Animation::FootGroundHit hit = physics.ProbeGround(
        "probe",
        Eigen::Vector3f(1000.0f, 6000.0f, -2000.0f),
        Eigen::Vector3f(0.0f, -3000.0f, 0.0f));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.y(), 3520.0f, 1.0f);
    EXPECT_NEAR(hit.normal.y(), 1.0f, 1.0e-3f);
    EXPECT_EQ(hit.supportIdentity, "terrain|Displacement");

    // 同一份资产把 u_layerDisplacementScale 关掉后，视觉表面必须回到宏高度。
    WriteJson(
        root / "MI_terrain_synthetic_zero.json",
        Json{
            {"name", "MI_terrain_synthetic_zero"},
            {"type", "materialInstance"},
            {"material", "shader/glsl/M_terrain.json"},
            {"parameters", {
                {"u_uvScale", 4.0},
                {"u_detailUvScale", 2.0},
                {"u_layerDisplacementScale", 0.0},
                {"u_terrainWeightMapMode", 0.0}}},
            {"textures", {
                {"layerHeight", "height_array.json"}}}});
    const VL::TerrainCollisionInstance zero =
        DisplacementInstance(root, "MI_terrain_synthetic_zero.json", 0.4f);
    physics.SetTerrainCollisions({zero}, root);
    const VL::Animation::FootGroundHit flat = physics.ProbeGround(
        "probe",
        Eigen::Vector3f(1000.0f, 6000.0f, -2000.0f),
        Eigen::Vector3f(0.0f, -3000.0f, 0.0f));
    ASSERT_TRUE(flat.hit);
    EXPECT_NEAR(flat.point.y(), 3500.0f, 1.0f);

    std::filesystem::remove_all(root);
}

TEST(ScriptTerrainCollision, ProbeGroundReturnsLowerVisualSurface)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "vl_script_terrain_displacement_miss";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    WriteJson(
        root / "MI_terrain_synthetic.json",
        Json{
            {"name", "MI_terrain_synthetic"},
            {"type", "materialInstance"},
            {"material", "shader/glsl/M_terrain.json"},
            {"parameters", {
                {"u_uvScale", 4.0},
                {"u_detailUvScale", 2.0},
                {"u_layerDisplacementScale", 1.0},
                {"u_terrainWeightMapMode", 0.0}}},
            {"textures", {
                {"layerHeight", "height_array.json"}}}});
    WriteJson(
        root / "height_array.json",
        Json{
            {"name", "height_array"},
            {"type", "textureArray"},
            {"sources", Json::array({"layer0.png"})},
            {"colorSpace", "linear"},
            {"mipmaps", true},
            {"filter", "linear"},
            {"wrapMode", "repeat"},
            {"bitDepth", 16}});
    // 层高度 0 -> (0-0.5)*0.4*100 = -20 cm；视觉表面低于 Jolt 宏高度。
    ASSERT_TRUE(WritePng16(root / "layer0.png", 4, 4, std::vector<uint16_t>(16, 0x0000u)));

    VL::ScriptPhysicsWorld physics;
    physics.SetTerrainCollisions(
        {DisplacementInstance(root, "MI_terrain_synthetic.json", 0.4f)}, root);
    Json states = Json::array({StaticProbeState()});
    physics.Step(states, StaticProbeDeclarations(), 0.0f);

    // Jolt 在 3500 命中宏高度；地形命中后必须改报 CPU 采样器的 3480 视觉表面。
    const VL::Animation::FootGroundHit hit = physics.ProbeGround(
        "probe",
        Eigen::Vector3f(1000.0f, 3510.0f, -2000.0f),
        Eigen::Vector3f(0.0f, -100.0f, 0.0f));
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.point.y(), 3480.0f, 1.0f);
    EXPECT_EQ(hit.supportIdentity, "terrain|Displacement");

    std::filesystem::remove_all(root);
}
