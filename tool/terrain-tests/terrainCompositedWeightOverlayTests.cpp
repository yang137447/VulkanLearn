#include <gtest/gtest.h>

#include "terrain/terrainCompositedWeightOverlay.h"
#include "terrain/terrainHeightmap.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
VL::TerrainBuildPlan Plan()
{
    VL::TerrainBuildPlan plan;
    plan.desc.heightmapMargin = 1;
    plan.desc.gridResolution = 2;
    plan.desc.layers.resize(2);
    auto heightmap = std::make_shared<HostImage>();
    heightmap->width = heightmap->height = 5;
    plan.heightmap = heightmap;
    return plan;
}

HostImage Imported()
{
    HostImage image;
    image.width = image.height = 5;
    image.arrayLayers = 2;
    image.format = HostImage::PixelFormat::RGBA8_UNORM;
    image.data.resize(5 * 5 * 2 * 4, 0);
    for (size_t pixel = 0; pixel < 50; ++pixel)
        image.data[pixel * 4] = pixel < 25 ? 51 : 204;
    return image;
}

std::shared_ptr<VL::TerrainLayerPaintData> Paint()
{
    auto paint = std::make_shared<VL::TerrainLayerPaintData>();
    paint->width = paint->height = 3;
    paint->layerCount = 2;
    paint->weights.resize(18, 1.f);
    paint->opacity.resize(18, 0);
    return paint;
}

float Weight(const HostImage& image, uint32_t layer, uint32_t pixelX = 2, uint32_t pixelY = 2)
{
    float value;
    const size_t offset = ((size_t(layer) * image.height + pixelY) * image.width + pixelX) * sizeof(value);
    std::memcpy(&value, image.data.data() + offset, sizeof(value));
    return value;
}

float SampleLocal(const HostImage& image, uint32_t layer, float surfaceU, float surfaceV)
{
    const float span = float(image.width - 1);
    const float positionX = (1.f + surfaceU * (span - 2.f)) / span * image.width - 0.5f;
    const float positionY = (1.f + surfaceV * (span - 2.f)) / span * image.height - 0.5f;
    const int startX = int(std::floor(positionX));
    const int startY = int(std::floor(positionY));
    const float fractionX = positionX - startX;
    const float fractionY = positionY - startY;
    auto read = [&](int pixelX, int pixelY)
    {
        return Weight(image, layer, std::clamp(pixelX, 0, int(image.width) - 1),
            std::clamp(pixelY, 0, int(image.height) - 1));
    };
    return (read(startX, startY) * (1 - fractionX) +
        read(startX + 1, startY) * fractionX) * (1 - fractionY) +
        (read(startX, startY + 1) * (1 - fractionX) +
        read(startX + 1, startY + 1) * fractionX) * fractionY;
}

std::array<uint8_t, 4> TopIds(
    const HostImage& image, const std::vector<uint8_t>& mapping, float surfaceU, float surfaceV)
{
    std::array<float, 4> weights{};
    std::array<uint8_t, 4> ids{};
    for (uint32_t layer = 0; layer < image.arrayLayers; ++layer)
    {
        const float weight = SampleLocal(image, layer, surfaceU, surfaceV);
        for (size_t slot = 0; slot < weights.size(); ++slot)
            if (weight > weights[slot])
            {
                for (size_t move = 3; move > slot; --move)
                {
                    weights[move] = weights[move - 1];
                    ids[move] = ids[move - 1];
                }
                weights[slot] = weight;
                ids[slot] = mapping[layer];
                break;
            }
    }
    return ids;
}

HostImage NineRawLayers()
{
    HostImage image;
    image.width = image.height = 5;
    image.arrayLayers = 9;
    image.channels = 1;
    image.format = HostImage::PixelFormat::R32_FLOAT;
    image.data.resize(5 * 5 * 9 * sizeof(float));
    for (uint32_t layer = 0; layer < 9; ++layer)
        for (uint32_t pixelY = 0; pixelY < 5; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < 5; ++pixelX)
            {
                const float value = 0.1f + float(layer) * 0.03f +
                    float(pixelX) * 0.01f + float(pixelY) * 0.02f;
                const size_t offset = ((size_t(layer) * 5 + pixelY) * 5 + pixelX) * sizeof(float);
                std::memcpy(image.data.data() + offset, &value, sizeof(value));
            }
    return image;
}
}

TEST(TerrainCompositedWeightOverlay, PreservesEveryImportedLayerWithoutPaint)
{
    const auto image = VL::BuildTerrainCompositedWeightOverlay(Plan(), Imported(), {17, 4});
    EXPECT_EQ(image.format, HostImage::PixelFormat::R32_FLOAT);
    EXPECT_EQ(image.arrayLayers, 2u);
    for (uint32_t pixelY = 0; pixelY < 5; ++pixelY)
        for (uint32_t pixelX = 0; pixelX < 5; ++pixelX)
        {
            EXPECT_FLOAT_EQ(Weight(image, 0, pixelX, pixelY), 0.2f);
            EXPECT_FLOAT_EQ(Weight(image, 1, pixelX, pixelY), 0.8f);
        }
}

TEST(TerrainCompositedWeightOverlay, RearLayerCanEnterTopFourAfterInterpolationWithoutFlattening)
{
    auto plan = Plan();
    plan.desc.layers.resize(9);
    HostImage image;
    image.width = image.height = 5;
    image.arrayLayers = 9;
    image.channels = 1;
    image.format = HostImage::PixelFormat::R32_FLOAT;
    image.data.resize(5 * 5 * 9 * sizeof(float));
    for (uint32_t layer = 0; layer < 9; ++layer)
        for (uint32_t pixelY = 0; pixelY < 5; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < 5; ++pixelX)
            {
                const float horizontal = float(pixelX) / 4.f;
                const float value = layer == 8 ? 0.6f :
                    layer < 4 ? 0.9f - 0.8f * horizontal : 0.1f + 0.8f * horizontal;
                std::memcpy(image.data.data() + ((layer * 5 + pixelY) * 5 + pixelX) * 4, &value, 4);
            }
    const std::vector<uint8_t> mapping{201, 17, 4, 98, 65, 2, 7, 35, 250};
    const auto frozen = VL::BuildTerrainCompositedWeightOverlay(plan, image, mapping);
    EXPECT_EQ(frozen.data, image.data);
    for (uint32_t layer = 0; layer < 4; ++layer)
        EXPECT_GT(Weight(frozen, layer, 0), Weight(frozen, 8, 0));
    for (uint32_t layer = 4; layer < 8; ++layer)
        EXPECT_GT(Weight(frozen, layer, 4), Weight(frozen, 8, 4));
    for (uint32_t layer = 0; layer < 8; ++layer)
        EXPECT_GT(Weight(frozen, 8, 2), Weight(frozen, layer, 2));
}

TEST(TerrainCompositedWeightOverlay, PaintUsesOriginalBaseAndEraseRestoresIt)
{
    auto plan = Plan();
    auto paint = Paint();
    paint->opacity[4] = 128;
    plan.layerPaint = paint;
    const auto imported = Imported();
    const auto edited = VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4});
    EXPECT_FLOAT_EQ(Weight(edited, 0), paint->Blend(0, 0.5f, 0.5f, 0.2f));
    EXPECT_FLOAT_EQ(Weight(edited, 1), 0.8f);
    const auto repeated = VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4});
    EXPECT_EQ(edited.data, repeated.data);
    paint->opacity[4] = 0;
    const auto erased = VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4});
    EXPECT_FLOAT_EQ(Weight(erased, 0), 0.2f);
}

TEST(TerrainCompositedWeightOverlay, SnapshotUndoRedoProducesIdenticalArrays)
{
    auto plan = Plan();
    const auto imported = Imported();
    const auto baseline = VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4});
    auto paint = Paint();
    paint->opacity.assign(18, 255);
    plan.layerPaint = paint;
    const auto edited = VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4});
    plan.layerPaint.reset();
    EXPECT_EQ(VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4}).data, baseline.data);
    plan.layerPaint = std::make_shared<const VL::TerrainLayerPaintData>(*paint);
    EXPECT_EQ(VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4}).data, edited.data);
}

TEST(TerrainCompositedWeightOverlay, PaintUsesMeshNormalizedUvAtTextureCenters)
{
    auto plan = Plan();
    auto paint = Paint();
    paint->opacity.assign(18, 255);
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t pixelY = 0; pixelY < 3; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < 3; ++pixelX)
                paint->weights[(layer * 3 + pixelY) * 3 + pixelX] = float(pixelX) * 0.5f;
    plan.layerPaint = paint;
    const auto image = VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {17, 4});
    EXPECT_NEAR(Weight(image, 0, 0), 0.1f, 1e-6f);
    EXPECT_NEAR(Weight(image, 0, 1), 0.3f, 1e-6f);
    EXPECT_FLOAT_EQ(Weight(image, 0, 2), 0.5f);
    EXPECT_NEAR(Weight(image, 0, 3), 0.7f, 1e-6f);
    EXPECT_NEAR(Weight(image, 0, 4), 0.9f, 1e-6f);
}

TEST(TerrainCompositedWeightOverlay, ReadsPaddedLayerMajorStorage)
{
    auto imported = Imported();
    imported.rowStrideBytes = 24;
    imported.data.assign(24 * 5 * 2, 0);
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t pixelY = 0; pixelY < 5; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < 5; ++pixelX)
                imported.data[(layer * 5 + pixelY) * 24 + pixelX * 4] = layer ? 204 : 51;
    const auto image = VL::BuildTerrainCompositedWeightOverlay(Plan(), imported, {17, 4});
    EXPECT_FLOAT_EQ(Weight(image, 0), 0.2f);
    EXPECT_FLOAT_EQ(Weight(image, 1), 0.8f);
}

TEST(TerrainCompositedWeightOverlay, PreservesFloatAndSixteenBitPrecision)
{
    auto imported = Imported();
    imported.format = HostImage::PixelFormat::R16_UNORM;
    imported.data.resize(100);
    const uint16_t value = 32769;
    for (size_t offset = 0; offset < imported.data.size(); offset += sizeof(value))
        std::memcpy(imported.data.data() + offset, &value, sizeof(value));
    auto image = VL::BuildTerrainCompositedWeightOverlay(Plan(), imported, {17, 4});
    EXPECT_FLOAT_EQ(Weight(image, 0), float(value) / 65535.f);
    const auto repeated = VL::BuildTerrainCompositedWeightOverlay(Plan(), image, {17, 4});
    EXPECT_EQ(repeated.data, image.data);
}

TEST(TerrainCompositedWeightOverlay, RejectsInvalidMappingsAndMargin)
{
    auto plan = Plan();
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {}), std::runtime_error);
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {4, 4}), std::runtime_error);
    plan.desc.heightmapMargin = 0;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {17, 4}), std::runtime_error);
}

TEST(TerrainCompositedWeightOverlay, RejectsInvalidImportStorageAndShape)
{
    auto imported = Imported();
    imported.data.pop_back();
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(Plan(), imported, {17, 4}), std::exception);
    imported = Imported();
    imported.height = 4;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(Plan(), imported, {17, 4}), std::runtime_error);
    imported = Imported();
    imported.arrayLayers = 1;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(Plan(), imported, {17, 4}), std::runtime_error);
    imported = Imported();
    imported.mipLevels = 2;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(Plan(), imported, {17, 4}), std::runtime_error);
}

TEST(TerrainCompositedWeightOverlay, RejectsSrgbCompressedAndSwizzledWeights)
{
    auto imported = Imported();
    imported.format = HostImage::PixelFormat::RGBA8_SRGB;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(Plan(), imported, {17, 4}), std::runtime_error);
    imported = Imported();
    imported.format = HostImage::PixelFormat::BC7_UNORM;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(Plan(), imported, {17, 4}), std::exception);
    imported = Imported();
    imported.swizzle.r = 1;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(Plan(), imported, {17, 4}), std::runtime_error);
}

TEST(TerrainCompositedWeightOverlay, RejectsMalformedPaintRatherThanDiscardingIt)
{
    auto plan = Plan();
    auto paint = Paint();
    plan.layerPaint = paint;
    paint->weights.pop_back();
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {17, 4}), std::runtime_error);
    paint = Paint();
    plan.layerPaint = paint;
    paint->weights[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {17, 4}), std::runtime_error);
    paint->weights[0] = 2.f;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {17, 4}), std::runtime_error);
}

TEST(TerrainCompositedWeightOverlay, FailedPreparationLeavesOriginalSnapshotUnchanged)
{
    const auto original = Imported();
    const auto bytes = original.data;
    auto plan = Plan();
    auto paint = Paint();
    paint->layerCount = 3;
    plan.layerPaint = paint;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, original, {17, 4}), std::runtime_error);
    EXPECT_EQ(original.data, bytes);
    EXPECT_EQ(VL::BuildTerrainCompositedWeightOverlay(Plan(), original, {17, 4}).arrayLayers, 2u);
}

TEST(TerrainCompositedWeightOverlay, NineLayerPaintKeepsRawValuesAndUnsortedMapping)
{
    auto plan = Plan();
    plan.desc.layers.resize(9);
    const auto imported = NineRawLayers();
    const auto originalBytes = imported.data;
    const std::vector<uint8_t> mapping{201, 17, 4, 98, 65, 2, 7, 35, 250};
    auto paint = std::make_shared<VL::TerrainLayerPaintData>();
    paint->width = paint->height = 3;
    paint->layerCount = 9;
    paint->weights.resize(81);
    paint->opacity.assign(81, 255);
    for (uint32_t layer = 0; layer < 9; ++layer)
        for (uint32_t pixelY = 0; pixelY < 3; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < 3; ++pixelX)
                paint->weights[(layer * 3 + pixelY) * 3 + pixelX] =
                    0.1f + 0.02f * layer + 0.1f * pixelX + 0.15f * pixelY;
    plan.layerPaint = paint;
    const auto image = VL::BuildTerrainCompositedWeightOverlay(plan, imported, mapping);
    ASSERT_EQ(image.arrayLayers, 9u);
    EXPECT_EQ(image.format, HostImage::PixelFormat::R32_FLOAT);
    for (uint32_t layer = 0; layer < 9; ++layer)
        for (float surfaceU : {0.f, 0.17f, 0.5f, 0.83f, 1.f})
            for (float surfaceV : {0.f, 0.29f, 0.5f, 0.71f, 1.f})
                EXPECT_NEAR(SampleLocal(image, layer, surfaceU, surfaceV),
                    0.1f + 0.02f * layer + 0.2f * (0.25f + 0.5f * surfaceU) +
                    0.3f * (0.25f + 0.5f * surfaceV), 1e-6f);
    float rawSum = 0;
    for (uint32_t layer = 0; layer < 9; ++layer)
        rawSum += Weight(image, layer);
    EXPECT_GT(rawSum, 1.f);
    EXPECT_EQ(TopIds(image, mapping, 0.5f, 0.5f),
        (std::array<uint8_t, 4>{250, 35, 7, 2}));
    const auto remapped = VL::BuildTerrainCompositedWeightOverlay(
        plan, imported, {250, 35, 7, 2, 65, 98, 4, 17, 201});
    EXPECT_EQ(remapped.data, image.data);
    auto unpainted = plan;
    unpainted.layerPaint.reset();
    EXPECT_EQ(VL::BuildTerrainCompositedWeightOverlay(unpainted, image, mapping).data, image.data);
    EXPECT_EQ(imported.data, originalBytes);
    EXPECT_EQ(mapping, (std::vector<uint8_t>{201, 17, 4, 98, 65, 2, 7, 35, 250}));
}

TEST(TerrainCompositedWeightOverlay, OpacityHolesRetainOriginalPerLayerBase)
{
    auto plan = Plan();
    plan.desc.layers.resize(9);
    const auto imported = NineRawLayers();
    auto paint = std::make_shared<VL::TerrainLayerPaintData>();
    paint->width = paint->height = 3;
    paint->layerCount = 9;
    paint->weights.assign(81, 0.93f);
    paint->opacity.assign(81, 255);
    for (uint32_t layer = 0; layer < 9; ++layer)
    {
        paint->opacity[layer * 9 + 4] = 0;
        paint->opacity[layer * 9 + 1] = 128;
        paint->opacity[layer * 9 + 7] = 64;
    }
    plan.layerPaint = paint;
    const std::vector<uint8_t> mapping{201, 17, 4, 98, 65, 2, 7, 35, 250};
    const auto image = VL::BuildTerrainCompositedWeightOverlay(plan, imported, mapping);
    for (uint32_t layer = 0; layer < 9; ++layer)
    {
        EXPECT_FLOAT_EQ(Weight(image, layer), Weight(imported, layer));
        for (uint32_t pixelY = 0; pixelY < 5; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < 5; ++pixelX)
                EXPECT_NEAR(Weight(image, layer, pixelX, pixelY),
                    paint->Blend(layer, (float(pixelX) + 0.5f) / 5.f,
                        (float(pixelY) + 0.5f) / 5.f, Weight(imported, layer, pixelX, pixelY)), 1e-6f);
    }
    paint->opacity.assign(81, 0);
    EXPECT_EQ(VL::BuildTerrainCompositedWeightOverlay(plan, imported, mapping).data, imported.data);
}

TEST(TerrainCompositedWeightOverlay, RevisionsDoNotMutatePreviouslyPublishedSnapshots)
{
    auto plan = Plan();
    const auto imported = Imported();
    auto draft = Paint();
    draft->opacity.assign(18, 128);
    draft->weights.assign(18, 0.9f);
    const auto revisionOne = std::make_shared<const VL::TerrainLayerPaintData>(*draft);
    plan.layerPaint = revisionOne;
    const auto first = VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4});
    const auto frozenBytes = first.data;
    draft->weights.assign(18, 0.1f);
    draft->opacity.assign(18, 255);
    const auto revisionTwo = std::make_shared<const VL::TerrainLayerPaintData>(*draft);
    plan.layerPaint = revisionTwo;
    const auto second = VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4});
    EXPECT_NE(second.data, first.data);
    EXPECT_EQ(first.data, frozenBytes);
    plan.layerPaint = revisionOne;
    EXPECT_EQ(VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4}).data, first.data);
    plan.layerPaint = revisionTwo;
    EXPECT_EQ(VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4}).data, second.data);
    EXPECT_FLOAT_EQ(revisionOne->weights[4], 0.9f);
    EXPECT_EQ(revisionOne->opacity[4], 128);
}

TEST(TerrainCompositedWeightOverlay, MissingLayerOrMappingIsRejectedInsteadOfSilentlyDroppingLayers)
{
    auto plan = Plan();
    plan.desc.layers.resize(9);
    auto imported = NineRawLayers();
    const std::vector<uint8_t> mapping{201, 17, 4, 98, 65, 2, 7, 35, 250};
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, imported,
        {201, 17, 4, 98, 65, 2, 7, 35}), std::runtime_error);
    imported.arrayLayers = 8;
    imported.data.resize(5 * 5 * 8 * sizeof(float));
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, imported, mapping), std::runtime_error);
    imported = NineRawLayers();
    auto paint = Paint();
    paint->layerCount = 8;
    paint->weights.resize(72);
    paint->opacity.resize(72);
    plan.layerPaint = paint;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, imported, mapping), std::runtime_error);
}

TEST(TerrainCompositedWeightOverlay, TopFourUsesFilteredRawLayersAndStableLocalTieOrder)
{
    auto plan = Plan();
    plan.desc.layers.resize(9);
    auto imported = NineRawLayers();
    const std::vector<uint8_t> mapping{201, 17, 4, 98, 65, 2, 7, 35, 250};
    for (uint32_t layer = 0; layer < 9; ++layer)
        for (uint32_t pixelY = 0; pixelY < 5; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < 5; ++pixelX)
            {
                const float horizontal = float(pixelX) / 4.f;
                const float value = layer == 8 ? 0.6f :
                    layer < 4 ? 0.875f - 0.75f * horizontal : 0.125f + 0.75f * horizontal;
                std::memcpy(imported.data.data() + ((layer * 5 + pixelY) * 5 + pixelX) * 4,
                    &value, sizeof(value));
            }
    const auto frozen = VL::BuildTerrainCompositedWeightOverlay(plan, imported, mapping);
    EXPECT_EQ(TopIds(frozen, mapping, 0.f, 0.5f),
        (std::array<uint8_t, 4>{201, 17, 4, 98}));
    EXPECT_EQ(TopIds(frozen, mapping, 1.f, 0.5f),
        (std::array<uint8_t, 4>{65, 2, 7, 35}));
    EXPECT_EQ(TopIds(frozen, mapping, 0.5f, 0.5f),
        (std::array<uint8_t, 4>{250, 201, 17, 4}));
    for (float surfaceU : {0.f, 0.17f, 0.5f, 0.83f, 1.f})
        EXPECT_EQ(TopIds(frozen, mapping, surfaceU, 0.5f),
            TopIds(imported, mapping, surfaceU, 0.5f));
}

TEST(TerrainCompositedWeightOverlay, PaintRequiresValidMeshUvTransform)
{
    auto plan = Plan();
    plan.layerPaint = Paint();
    plan.heightmap.reset();
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {17, 4}), std::runtime_error);
    plan = Plan();
    plan.layerPaint = Paint();
    plan.desc.gridResolution = 512;
    EXPECT_THROW(VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {17, 4}), std::runtime_error);
}

TEST(TerrainCompositedWeightOverlay, FrozenLinearResamplingIsNotDirectPaintEquivalence)
{
    auto plan = Plan();
    auto paint = Paint();
    paint->width = paint->height = 4;
    paint->weights.assign(32, 0.f);
    paint->opacity.assign(32, 255);
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t pixelY = 0; pixelY < 4; ++pixelY)
            paint->weights[(layer * 4 + pixelY) * 4 + 1] = 1.f;
    plan.layerPaint = paint;
    const auto frozen = VL::BuildTerrainCompositedWeightOverlay(plan, Imported(), {17, 4});
    const float direct = paint->Blend(0, 0.375f, 0.5f, 0.2f);
    const float resampled = SampleLocal(frozen, 0, 0.25f, 0.5f);
    EXPECT_NEAR(direct, 0.875f, 1e-6f);
    EXPECT_NEAR(resampled, 0.75f, 1e-6f);
    EXPECT_GT(std::abs(direct - resampled), 0.1f);
}

TEST(TerrainCompositedWeightOverlay, DifferentImportExtentUsesMeshMarginInsteadOfImportMargin)
{
    auto plan = Plan();
    auto paint = Paint();
    paint->opacity.assign(18, 255);
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t pixelY = 0; pixelY < 3; ++pixelY)
            for (uint32_t pixelX = 0; pixelX < 3; ++pixelX)
                paint->weights[(layer * 3 + pixelY) * 3 + pixelX] = float(pixelX) * 0.5f;
    plan.layerPaint = paint;
    auto imported = Imported();
    imported.width = imported.height = 9;
    imported.data.assign(9 * 9 * 2 * 4, 51);
    const auto image = VL::BuildTerrainCompositedWeightOverlay(plan, imported, {17, 4});
    for (float surfaceU : {0.f, 0.1f, 0.5f, 0.9f, 1.f})
        EXPECT_NEAR(SampleLocal(image, 0, surfaceU, 0.5f), 0.25f + 0.5f * surfaceU, 1e-6f);
}
