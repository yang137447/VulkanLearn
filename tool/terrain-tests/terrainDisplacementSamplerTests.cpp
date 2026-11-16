#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include "resource/image/textureIO.h"
#include "terrain/terrainDisplacementSampler.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainWeightMap.h"

namespace
{

constexpr float Pi = 3.14159265358979323846f;

HostImage MakeHeightImage(
    uint32_t width,
    uint32_t height,
    const std::function<float(float, float)>& value)
{
    HostImage image;
    image.width = width;
    image.height = height;
    image.channels = 1;
    image.format = HostImage::PixelFormat::R32_FLOAT;
    image.rowStrideBytes = width * sizeof(float);
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * height);
    for (uint32_t y = 0; y < height; ++y)
    {
        float* row = reinterpret_cast<float*>(
            image.data.data() + static_cast<size_t>(y) * image.rowStrideBytes);
        for (uint32_t x = 0; x < width; ++x)
        {
            const float u = width <= 1 ? 0.5f :
                static_cast<float>(x) / static_cast<float>(width - 1);
            const float v = height <= 1 ? 0.5f :
                static_cast<float>(y) / static_cast<float>(height - 1);
            row[x] = value(u, v);
        }
    }
    return image;
}

VL::TerrainLayerDesc MakeLayer(
    const char* name,
    float heightMin,
    float heightMax,
    float displacementM,
    float uvScale = 1.0f)
{
    VL::TerrainLayerDesc layer;
    layer.name = name;
    layer.heightMin = heightMin;
    layer.heightMax = heightMax;
    layer.slopeMin = 0.0f;
    layer.slopeMax = 90.0f;
    layer.noise = 0.03f;
    layer.uvScale = uvScale;
    layer.displacementM = displacementM;
    layer.hasHeight = displacementM > 0.0f;
    return layer;
}

VL::TerrainBuildPlan MakePlan(
    HostImage heightmap,
    std::vector<VL::TerrainLayerDesc> layers,
    float heightScale,
    float heightOffset = 0.0f,
    float sizeCm = 10000.0f,
    uint32_t gridResolution = 128,
    uint32_t pageTilesPerSide = 2)
{
    VL::TerrainBuildPlan plan;
    plan.desc.assetPath = "Generated/Validation/TerrainDisplacement/TR_synthetic.json";
    plan.desc.name = "TR_synthetic";
    plan.desc.size = Eigen::Vector2f(sizeCm, sizeCm);
    plan.desc.heightScale = heightScale;
    plan.desc.heightOffset = heightOffset;
    plan.desc.gridResolution = std::min(
        gridResolution,
        heightmap.width > 1 ? heightmap.width - 1 : 1u);
    plan.desc.chunkQuads = plan.desc.gridResolution;
    plan.desc.pageTilesPerSide = pageTilesPerSide;
    plan.desc.layers = std::move(layers);
    plan.heightmap = std::make_shared<const HostImage>(std::move(heightmap));
    plan.placementId = "terrain|synthetic";
    return plan;
}

std::shared_ptr<const VL::TerrainHeightTexture> MakeTexture(
    uint32_t width,
    uint32_t height,
    const std::function<float(float, float)>& value,
    bool buildMips = true)
{
    return VL::TerrainHeightTexture::FromHostImage(
        MakeHeightImage(width, height, value), false, buildMips);
}

VL::TerrainDisplacementSourceDesc MakeSource(
    VL::TerrainBuildPlan plan,
    std::vector<std::shared_ptr<const VL::TerrainHeightTexture>> layerHeights,
    std::shared_ptr<const VL::TerrainHeightTexture> detailHeight,
    float uvScale,
    float detailUvScale,
    float layerDisplacementScale,
    float detailAmplitudeCm)
{
    VL::TerrainDisplacementSourceDesc source;
    source.plan = std::move(plan);
    source.material.uvScale = uvScale;
    source.material.detailUvScale = detailUvScale;
    source.material.layerDisplacementScale = layerDisplacementScale;
    source.material.detailAmplitudeCm = detailAmplitudeCm;
    source.material.weightMapMode = 0;
    source.layerHeights = std::move(layerHeights);
    source.detailHeight = std::move(detailHeight);
    return source;
}

float RepeatBilinear(
    const VL::TerrainHeightTexture::MipLevel& mip,
    float u,
    float v)
{
    if (mip.width == 0 || mip.height == 0 || mip.texels.empty())
        return 0.5f;
    const auto wrap = [](int value, int size)
    {
        int result = value % size;
        return result < 0 ? result + size : result;
    };
    const float x = u * static_cast<float>(mip.width) - 0.5f;
    const float y = v * static_cast<float>(mip.height) - 0.5f;
    const float floorX = std::floor(x);
    const float floorY = std::floor(y);
    const int x0 = static_cast<int>(floorX);
    const int y0 = static_cast<int>(floorY);
    const float fx = x - floorX;
    const float fy = y - floorY;
    const auto at = [&](int px, int py)
    {
        return mip.texels[
            static_cast<size_t>(wrap(py, static_cast<int>(mip.height))) *
                mip.width +
            wrap(px, static_cast<int>(mip.width))];
    };
    const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
    const float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
    return top + (bottom - top) * fy;
}

float SmoothStep(float edge0, float edge1, float value)
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct PageSample
{
    uint8_t ids[4] = {0, 0, 0, 0};
    float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

PageSample SampleBakedPage(
    const VL::TerrainPageData& page,
    float localU,
    float localV)
{
    const float resolution = static_cast<float>(page.pageResolution);
    const float halfTexel = 0.5f / resolution;
    localU = std::clamp(localU, halfTexel, 1.0f - halfTexel);
    localV = std::clamp(localV, halfTexel, 1.0f - halfTexel);
    const float x = localU * resolution - 0.5f;
    const float y = localV * resolution - 0.5f;
    const float floorX = std::floor(x);
    const float floorY = std::floor(y);
    const float fx = x - floorX;
    const float fy = y - floorY;
    const int x0 = std::clamp(
        static_cast<int>(floorX), 0, static_cast<int>(page.pageResolution) - 1);
    const int y0 = std::clamp(
        static_cast<int>(floorY), 0, static_cast<int>(page.pageResolution) - 1);
    const int x1 = std::min(x0 + 1, static_cast<int>(page.pageResolution) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(page.pageResolution) - 1);
    const int xs[2] = {x0, x1};
    const int ys[2] = {y0, y1};
    const float weights[4] = {
        (1.0f - fx) * (1.0f - fy),
        fx * (1.0f - fy),
        (1.0f - fx) * fy,
        fx * fy};
    PageSample result;
    float weightBlend[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int sample = 0; sample < 4; ++sample)
    {
        const int px = xs[sample & 1];
        const int py = ys[sample >> 1];
        const size_t offset =
            (static_cast<size_t>(py) * page.pageResolution +
                static_cast<size_t>(px)) * 4;
        for (int slot = 0; slot < 4; ++slot)
        {
            weightBlend[slot] += weights[sample] *
                static_cast<float>(page.weights[offset + slot]) / 255.0f;
        }
    }
    // layerIds atlas 是 nearest，只有权重做双线性。
    const int nearestX = std::clamp(
        static_cast<int>(std::floor(localU * resolution)),
        0,
        static_cast<int>(page.pageResolution) - 1);
    const int nearestY = std::clamp(
        static_cast<int>(std::floor(localV * resolution)),
        0,
        static_cast<int>(page.pageResolution) - 1);
    const size_t nearestOffset =
        (static_cast<size_t>(nearestY) * page.pageResolution +
            static_cast<size_t>(nearestX)) * 4;
    float sum = 0.0f;
    for (int slot = 0; slot < 4; ++slot)
    {
        result.ids[slot] = page.ids[nearestOffset + slot];
        result.weights[slot] = weightBlend[slot];
        sum += weightBlend[slot];
    }
    if (sum < 1.0e-4f)
    {
        result.weights[0] = 1.0f;
        return result;
    }
    for (int slot = 0; slot < 4; ++slot)
        result.weights[slot] /= sum;
    return result;
}

// 测试侧手写 16-bit grayscale PNG：仓库没有 16-bit 写入器，stored deflate
// 能让 stbi_load_16 读到原始样本而不是 8-bit 截断值。
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

} // namespace

TEST(TerrainDisplacementSampler, HeightTextureDecodesNativeSixteenBitPng)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "vl_terrain_displacement_sampler_test";
    const std::filesystem::path path = root / "height16.png";
    const std::vector<uint16_t> samples = {0x8000u, 0x8001u, 0xFFFFu, 0x0001u};
    ASSERT_TRUE(WritePng16(path, 2, 2, samples));

    VL::TerrainHeightTextureCache cache;
    const auto texture = cache.Load(path, false, true);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->Width(), 2u);
    EXPECT_EQ(texture->Height(), 2u);
    EXPECT_GE(texture->MipCount(), 2u);
    EXPECT_NEAR(texture->Sample(0.25f, 0.25f, 0), 0x8000u / 65535.0f, 1.0e-7f);
    EXPECT_NEAR(texture->Sample(0.75f, 0.25f, 0), 0x8001u / 65535.0f, 1.0e-7f);
    EXPECT_NEAR(texture->Sample(0.25f, 0.75f, 0), 0xFFFFu / 65535.0f, 1.0e-7f);
    EXPECT_NEAR(texture->Sample(0.75f, 0.75f, 0), 0x0001u / 65535.0f, 1.0e-7f);
    // 0x8000/65535 与 8-bit 截断的 128/255 不同；这条断言防止静默降位。
    EXPECT_GT(std::abs(
        texture->Sample(0.25f, 0.25f, 0) - 128.0f / 255.0f), 1.0e-5f);

    std::filesystem::remove_all(root);
}

TEST(TerrainDisplacementSampler, HeightTextureHonoursFlipY)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "vl_terrain_displacement_sampler_flip";
    const std::filesystem::path path = root / "height16.png";
    const std::vector<uint16_t> samples = {0x1000u, 0x2000u, 0x3000u, 0x4000u};
    ASSERT_TRUE(WritePng16(path, 2, 2, samples));

    VL::TerrainHeightTextureCache cache;
    const auto unflipped = cache.Load(path, false, false);
    const auto flipped = cache.Load(path, true, false);
    EXPECT_NEAR(unflipped->Sample(0.25f, 0.25f, 0), 0x1000u / 65535.0f, 1.0e-7f);
    EXPECT_NEAR(flipped->Sample(0.25f, 0.25f, 0), 0x3000u / 65535.0f, 1.0e-7f);

    std::filesystem::remove_all(root);
}

TEST(TerrainDisplacementSampler, SampleTop4MatchesBakedPageTexels)
{
    const HostImage heightmap = MakeHeightImage(
        129,
        129,
        [](float u, float v)
        {
            return 0.5f + 0.35f * std::sin(2.0f * Pi * u) *
                std::cos(2.0f * Pi * v);
        });
    VL::TerrainBuildPlan plan = MakePlan(
        heightmap,
        {MakeLayer("low", 0.0f, 0.45f, 0.4f),
            MakeLayer("mid", 0.35f, 0.7f, 0.15f),
            MakeLayer("high", 0.6f, 1.0f, 0.0f)},
        10000.0f);
    const auto layerHeights = std::vector<std::shared_ptr<const VL::TerrainHeightTexture>>{
        MakeTexture(32, 32, [](float u, float v) { return 0.5f + 0.25f * u; }),
        MakeTexture(32, 32, [](float u, float v) { return 0.5f + 0.25f * v; }),
        MakeTexture(32, 32, [](float u, float v) { return 0.5f; })};
    const VL::TerrainDisplacementSampler sampler(MakeSource(
        plan, layerHeights, nullptr, 4.0f, 2.0f, 0.5f, 0.0f));

    const uint32_t tileX = 1;
    const uint32_t tileZ = 0;
    const VL::TerrainPageData page = VL::TerrainWeightMapBuilder::BuildPage(
        plan, plan.desc.pageTilesPerSide, VL::TerrainPageResolution, tileX, tileZ);
    float maximumIdError = 0.0f;
    float maximumWeightError = 0.0f;
    for (int index = 1; index < 64; ++index)
    {
        const float localU = static_cast<float>(index) / 65.0f;
        const float localV = 1.0f - localU;
        const float u = (static_cast<float>(tileX) + localU) /
            static_cast<float>(plan.desc.pageTilesPerSide);
        const float v = (static_cast<float>(tileZ) + localV) /
            static_cast<float>(plan.desc.pageTilesPerSide);
        std::array<uint8_t, 4> ids{};
        std::array<float, 4> weights{};
        sampler.SampleTop4(u, v, ids, weights);
        const PageSample reference = SampleBakedPage(page, localU, localV);
        for (int slot = 0; slot < 4; ++slot)
        {
            maximumIdError = std::max(
                maximumIdError,
                std::abs(static_cast<float>(ids[slot]) -
                    static_cast<float>(reference.ids[slot])));
            maximumWeightError = std::max(
                maximumWeightError,
                std::abs(weights[slot] - reference.weights[slot]));
        }
    }
    EXPECT_LE(maximumIdError, 1.0f);
    EXPECT_LE(maximumWeightError, 1.0e-3f);
}

TEST(TerrainDisplacementSampler, LayerAndDetailDisplacementMatchTesFormula)
{
    const float heightScale = 8000.0f;
    const HostImage heightmap = MakeHeightImage(
        129,
        129,
        [](float u, float v)
        {
            return 0.5f + 0.2f * std::sin(2.0f * Pi * u + 0.4f) +
                0.12f * std::cos(2.0f * Pi * v - 0.3f);
        });
    VL::TerrainBuildPlan plan = MakePlan(
        heightmap,
        {MakeLayer("land", 0.0f, 1.0f, 0.6f, 1.25f)},
        heightScale);
    const auto layerHeight = MakeTexture(
        64,
        64,
        [](float u, float v)
        {
            return 0.5f + 0.3f * std::sin(2.0f * Pi * u * 3.0f) *
                std::cos(2.0f * Pi * v * 2.0f);
        });
    const auto detailHeight = MakeTexture(
        32,
        32,
        [](float u, float v)
        {
            return 0.5f + 0.4f * std::cos(2.0f * Pi * (u + v) * 5.0f);
        });
    const float uvScale = 3.0f;
    const float detailUvScale = 5.0f;
    const float layerDisplacementScale = 0.5f;
    const float detailAmplitudeCm = 20.0f;
    const VL::TerrainDisplacementSampler sampler(MakeSource(
        plan,
        {layerHeight},
        detailHeight,
        uvScale,
        detailUvScale,
        layerDisplacementScale,
        detailAmplitudeCm));

    double squaredError = 0.0;
    float maximumError = 0.0f;
    int samples = 0;
    for (int y = 2; y < 18; ++y)
    {
        for (int x = 3; x < 17; ++x)
        {
            const float u = static_cast<float>(x) / 20.0f;
            const float v = static_cast<float>(y) / 20.0f;
            const VL::TerrainDisplacementSample sample =
                sampler.SampleAtUv(u, v);
            const float du = 1.0f / static_cast<float>(plan.desc.gridResolution);
            const auto macroAt = [&](float su, float sv)
            {
                return VL::TerrainHeightmap::SampleHeightCm(
                    plan.desc, *plan.heightmap, su, sv);
            };
            const float left = macroAt(u - du, v);
            const float right = macroAt(u + du, v);
            const float down = macroAt(u, v - du);
            const float up = macroAt(u, v + du);
            const Eigen::Vector3f normal = Eigen::Vector3f(
                -(right - left) / (2.0f * du * plan.desc.size.x()),
                1.0f,
                -(up - down) / (2.0f * du * plan.desc.size.y())).normalized();

            const float layerUv = uvScale * 1.25f * detailUvScale;
            const float layerValue = RepeatBilinear(
                layerHeight->Mip(2), u * layerUv, v * layerUv);
            const float referenceLayer = (layerValue - 0.5f) * 0.6f *
                layerDisplacementScale * 100.0f;
            const float detailUv = uvScale * detailUvScale;
            const float detailValue = RepeatBilinear(
                detailHeight->Mip(0), u * detailUv, v * detailUv);
            const float solidity = SmoothStep(0.004f, 0.03f, 0.6f);
            const float referenceDetail = (detailValue - 0.5f) *
                detailAmplitudeCm * solidity;
            const float referenceHeight =
                macroAt(u, v) + normal.y() * (referenceLayer + referenceDetail);
            const float error = std::abs(sample.heightCm - referenceHeight);
            maximumError = std::max(maximumError, error);
            squaredError += static_cast<double>(error) * error;
            ++samples;
        }
    }
    ASSERT_GT(samples, 0);
    const double rms = std::sqrt(squaredError / samples);
    std::cout << "[TerrainDisplacementParity] samples=" << samples
              << " rms=" << rms << " max=" << maximumError << std::endl;
    EXPECT_LE(rms, 1.0e-3);
    EXPECT_LE(maximumError, 5.0e-3f);
}

TEST(TerrainDisplacementSampler, WaterLayerKeepsMacroSurfaceFlat)
{
    const float heightScale = 10000.0f;
    // 海平面：归一化高度 0.75 以上全部是 water；detail 幅度很大也不能
    // 让水面出现 tiling 起伏。
    const HostImage heightmap = MakeHeightImage(
        65,
        65,
        [](float u, float v) { return 0.75f + 0.0f * u + 0.0f * v; });
    VL::TerrainBuildPlan plan = MakePlan(
        heightmap,
        {MakeLayer("sea", 0.55f, 1.0f, 0.0f), MakeLayer("land", 0.0f, 0.6f, 0.35f)},
        heightScale);
    const auto layerHeights = std::vector<std::shared_ptr<const VL::TerrainHeightTexture>>{
        MakeTexture(32, 32, [](float u, float v) { return 0.9f; }),
        MakeTexture(32, 32, [](float u, float v) { return 0.1f; })};
    const auto detail = MakeTexture(
        32, 32, [](float u, float v) { return 0.95f; }, false);
    const VL::TerrainDisplacementSampler sampler(MakeSource(
        plan, layerHeights, detail, 8.0f, 4.0f, 1.0f, 40.0f));

    std::array<uint8_t, 4> ids{};
    std::array<float, 4> weights{};
    sampler.SampleTop4(0.31f, 0.67f, ids, weights);
    ASSERT_EQ(ids[0], 0u);
    const VL::TerrainDisplacementSample sample = sampler.SampleAtUv(0.31f, 0.67f);
    EXPECT_NEAR(sample.layerDisplacementCm, 0.0f, 1.0e-5f);
    EXPECT_NEAR(sample.detailDisplacementCm, 0.0f, 1.0e-5f);
    EXPECT_NEAR(
        sample.heightCm,
        VL::TerrainHeightmap::SampleHeightCm(
            plan.desc, *plan.heightmap, 0.31f, 0.67f),
        1.0e-3f);
}

TEST(TerrainDisplacementSampler, SampleWorldSolvesNormalDisplacedXz)
{
    const HostImage heightmap = MakeHeightImage(
        129,
        129,
        [](float u, float v) { return 0.4f + 0.25f * u + 0.12f * v; });
    VL::TerrainBuildPlan plan = MakePlan(
        heightmap,
        {MakeLayer("slope", 0.0f, 1.0f, 0.8f, 0.75f)},
        12000.0f);
    const auto layerHeight = MakeTexture(
        64,
        64,
        [](float u, float v)
        {
            return 0.5f + 0.45f * std::sin(2.0f * Pi * u * 2.0f) *
                std::sin(2.0f * Pi * v * 3.0f);
        });
    const VL::TerrainDisplacementSampler sampler(MakeSource(
        plan, {layerHeight}, nullptr, 6.0f, 3.0f, 1.0f, 0.0f));

    float maximumXzError = 0.0f;
    for (int y = 20; y <= 80; y += 12)
    {
        for (int x = 15; x <= 85; x += 10)
        {
            const float worldX =
                (static_cast<float>(x) / 100.0f - 0.5f) * plan.desc.size.x();
            const float worldZ =
                (static_cast<float>(y) / 100.0f - 0.5f) * plan.desc.size.y();
            const VL::TerrainDisplacementSample sample =
                sampler.SampleWorld(worldX, worldZ, 10.0f);
            ASSERT_TRUE(sample.valid);
            ASSERT_TRUE(sample.insideFootprint);
            maximumXzError = std::max(
                maximumXzError,
                std::max(
                    std::abs(sample.worldPosition.x() - worldX),
                    std::abs(sample.worldPosition.z() - worldZ)));
        }
    }
    std::cout << "[TerrainDisplacementSolve] maxXzErrorCm=" << maximumXzError
              << std::endl;
    // 位移沿世界法线在 XZ 上产生的偏移必须被固定点迭代收回；
    // 1 cm 仍小于脚 IK 的接触误差预算（≤2 cm）。
    EXPECT_LE(maximumXzError, 1.0f);
}

TEST(TerrainDisplacementSampler, SmoothNormalFollowsSlopeDirection)
{
    const HostImage heightmap = MakeHeightImage(
        65,
        65,
        [](float u, float v) { return 0.2f + 0.6f * u + 0.0f * v; });
    VL::TerrainBuildPlan plan = MakePlan(
        heightmap,
        {MakeLayer("slope", 0.0f, 1.0f, 0.0f)},
        10000.0f);
    const VL::TerrainDisplacementSampler sampler(MakeSource(
        plan, {}, nullptr, 8.0f, 2.0f, 1.0f, 0.0f));
    const Eigen::Vector3f normal = sampler.SampleNormal(0.0f, 0.0f, 20.0f);
    EXPECT_LT(normal.x(), -0.1f);
    EXPECT_NEAR(normal.z(), 0.0f, 0.05f);
    EXPECT_NEAR(normal.norm(), 1.0f, 1.0e-5f);
}
