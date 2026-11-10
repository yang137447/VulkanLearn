#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "editor/terrain/terrainPaintSession.h"
#include "resource/image/hostImage.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainHeightmapEditor.h"
#include "terrain/terrainMeshBuilder.h"

namespace
{

HostImage MakeHeightmap(
    uint32_t width,
    uint32_t height,
    const std::function<float(uint32_t, uint32_t)>& value)
{
    HostImage image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = width * image.GetPixelSize();
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * height);
    float* pixels = reinterpret_cast<float*>(image.data.data());
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const float sample = value(x, y);
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            pixels[index + 0] = sample;
            pixels[index + 1] = sample;
            pixels[index + 2] = sample;
            pixels[index + 3] = 1.0f;
        }
    }
    return image;
}

VL::TerrainAssetDesc MakeDesc()
{
    VL::TerrainAssetDesc desc;
    desc.assetPath = "Generated/Validation/TerrainEditor/TR_editor.json";
    desc.name = "TR_editor";
    desc.heightmapPath = "Generated/Validation/TerrainEditor/heightmap.exr";
    desc.size = Eigen::Vector2f(1000.0f, 500.0f);
    desc.heightScale = 200.0f;
    desc.heightOffset = 10.0f;
    desc.gridResolution = 4;
    desc.chunkQuads = 2;
    desc.materialInstancePath =
        "Generated/Validation/TerrainEditor/MI_editor.json";
    return desc;
}

VL::TerrainBuildPlan MakePlan(const HostImage& image)
{
    VL::TerrainBuildPlan plan;
    plan.desc = MakeDesc();
    plan.heightmap = std::make_shared<const HostImage>(image);
    plan.placementId = "terrain|Landscape";
    return plan;
}

VL::TerrainHeightmapEditor MakeEditor(
    uint32_t width,
    uint32_t height,
    const std::function<float(uint32_t, uint32_t)>& value)
{
    return VL::TerrainHeightmapEditor(
        MakePlan(MakeHeightmap(width, height, value)));
}

VL::TerrainSculptBrush MakeBrush(
    VL::TerrainSculptMode mode,
    float centerUv,
    float radiusUv,
    float strength,
    float falloff = 1.0f)
{
    VL::TerrainSculptBrush brush;
    brush.mode = mode;
    brush.centerUv = Eigen::Vector2f(centerUv, centerUv);
    brush.radiusUv = radiusUv;
    brush.strength = strength;
    brush.falloff = falloff;
    return brush;
}

VL::TerrainCollisionInstance MakePaintInstance()
{
    VL::TerrainCollisionInstance instance;
    instance.placementId = "terrain|Landscape";
    instance.plan.desc.assetPath =
        "Generated/Validation/TerrainEditor/TR_editor.json";
    instance.plan.desc.name = "TR_editor";
    instance.plan.desc.size = Eigen::Vector2f(1000.0f, 500.0f);
    instance.plan.desc.gridResolution = 4;
    instance.plan.desc.chunkQuads = 2;
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    instance.plan.desc.layers.push_back(VL::TerrainLayerDesc{});
    instance.plan.placementId = instance.placementId;

    auto paint = std::make_shared<VL::TerrainLayerPaintData>();
    paint->width = 11;
    paint->height = 11;
    paint->layerCount = 2;
    paint->weights.assign(11u * 11u * 2u, 0.0f);
    paint->opacity.assign(11u * 11u * 2u, 0u);
    instance.plan.layerPaint = std::move(paint);
    return instance;
}

VL::Editor::TerrainPaintBrush MakePaintBrush(
    float opacity = 1.0f,
    float flow = 1.0f,
    float noise = 0.0f,
    uint32_t noiseSeed = 0)
{
    VL::Editor::TerrainPaintBrush brush;
    brush.layerIndex = 0;
    brush.centerUv = Eigen::Vector2f(0.5f, 0.5f);
    brush.radiusUv = 0.25f;
    brush.strength = 1.0f;
    brush.falloff = 0.0f;
    brush.opacity = opacity;
    brush.flow = flow;
    brush.noise = noise;
    brush.noiseSeed = noiseSeed;
    return brush;
}

} // namespace

TEST(TerrainHeightmapEditorTest, SamplingMatchesRuntimeBilinearConvention)
{
    const HostImage image = MakeHeightmap(4, 4, [](uint32_t x, uint32_t y) {
        return (static_cast<float>(x) * 0.13f +
            static_cast<float>(y) * 0.29f);
    });
    const VL::TerrainBuildPlan plan = MakePlan(image);
    const VL::TerrainHeightmapEditor editor(plan);

    const float uvPairs[][2] = {
        {0.0f, 0.0f},
        {0.5f, 0.5f},
        {1.0f, 1.0f},
        {0.23f, 0.71f},
        {0.91f, 0.08f}};
    for (const auto& uv : uvPairs)
    {
        EXPECT_FLOAT_EQ(
            editor.SampleNormalized(uv[0], uv[1]),
            VL::TerrainHeightmap::SampleNormalized(image, uv[0], uv[1]));
        EXPECT_FLOAT_EQ(
            editor.SampleHeightCm(uv[0], uv[1]),
            VL::TerrainHeightmap::SampleHeightCm(
                plan.desc, image, uv[0], uv[1]));
    }
}

TEST(TerrainHeightmapEditorTest, SamplingPreservesSourceValuesOutsideUnitRange)
{
    const HostImage image = MakeHeightmap(2, 2, [](uint32_t x, uint32_t) {
        return x == 0 ? 1.5f : -0.25f;
    });
    const VL::TerrainBuildPlan plan = MakePlan(image);
    const VL::TerrainHeightmapEditor editor(plan);

    EXPECT_FLOAT_EQ(editor.SampleNormalized(0.0f, 0.5f), 1.5f);
    EXPECT_FLOAT_EQ(editor.SampleNormalized(1.0f, 0.5f), -0.25f);
}

TEST(TerrainHeightmapEditorTest, RaiseAndLowerAreSymmetricBelowClamp)
{
    VL::TerrainHeightmapEditor raised = MakeEditor(
        4, 4, [](uint32_t, uint32_t) { return 0.5f; });
    VL::TerrainHeightmapEditor lowered = MakeEditor(
        4, 4, [](uint32_t, uint32_t) { return 0.5f; });

    const VL::TerrainSculptResult raiseResult = raised.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.5f, 0.5f, 0.2f, 0.0f));
    const VL::TerrainSculptResult lowerResult = lowered.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Lower, 0.5f, 0.5f, 0.2f, 0.0f));

    ASSERT_TRUE(raiseResult.applied);
    ASSERT_TRUE(lowerResult.applied);
    EXPECT_EQ(raiseResult.dirtyRect.minX, lowerResult.dirtyRect.minX);
    EXPECT_EQ(raiseResult.dirtyRect.maxX, lowerResult.dirtyRect.maxX);
    EXPECT_EQ(raiseResult.dirtyRect.minY, lowerResult.dirtyRect.minY);
    EXPECT_EQ(raiseResult.dirtyRect.maxY, lowerResult.dirtyRect.maxY);

    const std::vector<float>& high = raised.NormalizedSamples();
    const std::vector<float>& low = lowered.NormalizedSamples();
    ASSERT_EQ(high.size(), low.size());
    for (size_t index = 0; index < high.size(); ++index)
        EXPECT_FLOAT_EQ(high[index] + low[index], 1.0f);
}

TEST(TerrainHeightmapEditorTest, BrushClampsAtUnitBounds)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        4, 4, [](uint32_t, uint32_t) { return 0.95f; });
    const VL::TerrainSculptResult result = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.5f, 0.8f, 0.5f, 0.0f));
    ASSERT_TRUE(result.applied);
    EXPECT_FLOAT_EQ(result.changedMax, 1.0f);
    for (float sample : editor.NormalizedSamples())
        EXPECT_LE(sample, 1.0f);

    VL::TerrainHeightmapEditor lowered = MakeEditor(
        4, 4, [](uint32_t, uint32_t) { return 0.05f; });
    const VL::TerrainSculptResult lowerResult = lowered.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Lower, 0.5f, 0.8f, 0.5f, 0.0f));
    ASSERT_TRUE(lowerResult.applied);
    EXPECT_FLOAT_EQ(lowerResult.changedMin, 0.0f);
    for (float sample : lowered.NormalizedSamples())
        EXPECT_GE(sample, 0.0f);

    // 已经到上限后继续 Raise 不再产生变化，也不推进 revision。
    const uint64_t revision = editor.Revision();
    const VL::TerrainSculptResult repeated = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.5f, 0.8f, 0.5f, 0.0f));
    EXPECT_FALSE(repeated.applied);
    EXPECT_EQ(repeated.revision, revision);
    EXPECT_EQ(editor.Revision(), revision);
}

TEST(TerrainHeightmapEditorTest, FalloffIsMonotonicAndReachesZeroAtEdge)
{
    float previous = VL::TerrainHeightmapEditor::BrushFalloff(0.0f, 0.5f);
    EXPECT_FLOAT_EQ(previous, 1.0f);
    for (int step = 1; step <= 100; ++step)
    {
        const float distance = static_cast<float>(step) / 100.0f;
        const float current =
            VL::TerrainHeightmapEditor::BrushFalloff(distance, 0.5f);
        EXPECT_LE(current, previous);
        EXPECT_GE(current, 0.0f);
        EXPECT_LE(current, 1.0f);
        previous = current;
    }
    EXPECT_FLOAT_EQ(previous, 0.0f);
}

TEST(TerrainHeightmapEditorTest, FalloffDecaysFromCenterToRadius)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        5, 5, [](uint32_t, uint32_t) { return 0.5f; });
    const VL::TerrainSculptResult result = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.5f, 0.6f, 0.2f, 1.0f));
    ASSERT_TRUE(result.applied);

    // 5x5 的四个角与中心；中心被提升得最多，角点位于半径外保持原值。
    const std::vector<float>& samples = editor.NormalizedSamples();
    const float center = samples[2 * 5 + 2];
    const float nearCenter = samples[2 * 5 + 1];
    const float corner = samples[0];
    EXPECT_GT(center, nearCenter);
    EXPECT_GT(nearCenter, 0.5f);
    EXPECT_FLOAT_EQ(corner, 0.5f);
}

TEST(TerrainHeightmapEditorTest, PixelsOutsideBrushRadiusStayUnchanged)
{
    const auto source = [](uint32_t x, uint32_t y) {
        return 0.2f + static_cast<float>(x + y) * 0.01f;
    };
    VL::TerrainHeightmapEditor editor = MakeEditor(8, 8, source);
    const std::vector<float> before = editor.NormalizedSamples();
    const VL::TerrainSculptResult result = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.5f, 0.2f, 0.1f));
    ASSERT_TRUE(result.applied);
    ASSERT_TRUE(result.dirtyRect.valid);

    for (uint32_t y = 0; y < editor.Height(); ++y)
    {
        for (uint32_t x = 0; x < editor.Width(); ++x)
        {
            const size_t index = static_cast<size_t>(y) * editor.Width() + x;
            if (!result.dirtyRect.Contains(x, y))
            {
                EXPECT_FLOAT_EQ(editor.NormalizedSamples()[index], before[index]);
            }
        }
    }
    EXPECT_FLOAT_EQ(editor.NormalizedSamples()[0], before[0]);
}

TEST(TerrainHeightmapEditorTest, SquareBrushCoversCornerOutsideCircleFootprint)
{
    const auto flat = [](uint32_t, uint32_t) { return 0.5f; };
    VL::TerrainHeightmapEditor circleEditor = MakeEditor(11, 11, flat);
    VL::TerrainHeightmapEditor squareEditor = MakeEditor(11, 11, flat);

    VL::TerrainSculptBrush brush;
    brush.mode = VL::TerrainSculptMode::Raise;
    brush.centerUv = Eigen::Vector2f(0.5f, 0.5f);
    brush.radiusUv = 0.25f;
    brush.strength = 0.2f;
    brush.falloff = 0.0f;

    brush.shape = VL::TerrainBrushShape::Circle;
    circleEditor.ApplyBrush(brush);
    brush.shape = VL::TerrainBrushShape::Square;
    squareEditor.ApplyBrush(brush);

    // texel (7,7) 的归一化偏移为 (0.8,0.8)：Chebyshev 距离 0.8 在方形笔刷内，
    // 欧氏距离 1.131 已在圆形笔刷外。
    const size_t corner = static_cast<size_t>(7) * 11 + 7;
    EXPECT_FLOAT_EQ(circleEditor.NormalizedSamples()[corner], 0.5f);
    EXPECT_GT(squareEditor.NormalizedSamples()[corner], 0.5f);
}

TEST(TerrainPaintSessionTest, SquareBrushCoversCornerOutsideCircleFootprint)
{
    const VL::TerrainCollisionInstance instance = MakePaintInstance();
    VL::Editor::TerrainPaintSession circleSession;
    VL::Editor::TerrainPaintSession squareSession;
    std::string error;
    ASSERT_TRUE(
        VL::Editor::TerrainPaintSession::Create(instance, circleSession, error))
        << error;
    ASSERT_TRUE(
        VL::Editor::TerrainPaintSession::Create(instance, squareSession, error))
        << error;

    VL::Editor::TerrainPaintBrush brush;
    brush.layerIndex = 0;
    brush.centerUv = Eigen::Vector2f(0.5f, 0.5f);
    brush.radiusUv = 0.25f;
    brush.strength = 1.0f;
    brush.falloff = 0.0f;

    brush.shape = VL::TerrainBrushShape::Circle;
    ASSERT_TRUE(circleSession.BeginStroke());
    ASSERT_TRUE(circleSession.ApplyBrush(brush));
    ASSERT_TRUE(circleSession.EndStroke());

    brush.shape = VL::TerrainBrushShape::Square;
    ASSERT_TRUE(squareSession.BeginStroke());
    ASSERT_TRUE(squareSession.ApplyBrush(brush));
    ASSERT_TRUE(squareSession.EndStroke());

    // 与 Sculpt 使用同一组归一化坐标：方形笔刷必须覆盖 (7,7)，圆形笔刷不能。
    const size_t corner = static_cast<size_t>(7) * 11 + 7;
    EXPECT_FLOAT_EQ(circleSession.PaintData()->weights[corner], 0.0f);
    EXPECT_GT(squareSession.PaintData()->weights[corner], 0.0f);
}

TEST(TerrainPaintSessionTest, DefaultOpacityFlowAndNoisePreserveLegacyAccumulation)
{
    const VL::TerrainCollisionInstance instance = MakePaintInstance();
    VL::Editor::TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(
        VL::Editor::TerrainPaintSession::Create(instance, session, error))
        << error;

    VL::Editor::TerrainPaintBrush brush = MakePaintBrush();
    brush.strength = 0.5f;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(brush));
    ASSERT_TRUE(session.EndStroke());

    // 旧调用只设置 strength/falloff；新字段取默认值时必须仍得到
    // opacity += (1-opacity) * strength * falloff 的结果。
    const size_t center = static_cast<size_t>(5) * 11 + 5;
    EXPECT_EQ(session.PaintData()->opacity[center], 128u);
    EXPECT_FLOAT_EQ(session.PaintData()->weights[center], 0.5f);
}

TEST(TerrainPaintSessionTest, OpacityCapsCoverageAcrossRepeatedStamps)
{
    const VL::TerrainCollisionInstance instance = MakePaintInstance();
    VL::Editor::TerrainPaintSession capped;
    VL::Editor::TerrainPaintSession full;
    std::string error;
    ASSERT_TRUE(VL::Editor::TerrainPaintSession::Create(instance, capped, error))
        << error;
    ASSERT_TRUE(VL::Editor::TerrainPaintSession::Create(instance, full, error))
        << error;

    VL::Editor::TerrainPaintBrush cappedBrush = MakePaintBrush(0.4f, 1.0f);
    ASSERT_TRUE(capped.BeginStroke());
    ASSERT_TRUE(capped.ApplyBrush(cappedBrush));
    for (int stamp = 0; stamp < 8; ++stamp)
        capped.ApplyBrush(cappedBrush);
    ASSERT_TRUE(capped.EndStroke());

    VL::Editor::TerrainPaintBrush fullBrush = MakePaintBrush(1.0f, 1.0f);
    ASSERT_TRUE(full.BeginStroke());
    ASSERT_TRUE(full.ApplyBrush(fullBrush));
    ASSERT_TRUE(full.EndStroke());

    const size_t center = static_cast<size_t>(5) * 11 + 5;
    EXPECT_EQ(capped.PaintData()->opacity[center], 102u);
    for (uint8_t value : capped.PaintData()->opacity)
        EXPECT_LE(value, 102u);
    EXPECT_EQ(full.PaintData()->opacity[center], 255u);
}

TEST(TerrainPaintSessionTest, FlowControlsPerStampAdvanceAndCapSpeed)
{
    const VL::TerrainCollisionInstance instance = MakePaintInstance();
    const size_t center = static_cast<size_t>(5) * 11 + 5;

    const auto stampCountToCap = [&instance, center](float flow)
    {
        VL::Editor::TerrainPaintSession session;
        std::string error;
        if (!VL::Editor::TerrainPaintSession::Create(
                instance, session, error))
            return 0;
        const VL::Editor::TerrainPaintBrush brush =
            MakePaintBrush(0.4f, flow);
        if (!session.BeginStroke())
            return 0;
        int stamps = 0;
        for (; stamps < 32; ++stamps)
        {
            session.ApplyBrush(brush);
            if (session.PaintData()->opacity[center] >= 102u)
            {
                ++stamps;
                break;
            }
        }
        return session.EndStroke() || stamps != 0 ? stamps : 0;
    };

    VL::Editor::TerrainPaintSession low;
    VL::Editor::TerrainPaintSession high;
    std::string error;
    ASSERT_TRUE(VL::Editor::TerrainPaintSession::Create(instance, low, error))
        << error;
    ASSERT_TRUE(VL::Editor::TerrainPaintSession::Create(instance, high, error))
        << error;
    const VL::Editor::TerrainPaintBrush lowBrush = MakePaintBrush(1.0f, 0.2f);
    const VL::Editor::TerrainPaintBrush highBrush = MakePaintBrush(1.0f, 0.8f);
    ASSERT_TRUE(low.BeginStroke());
    ASSERT_TRUE(low.ApplyBrush(lowBrush));
    ASSERT_TRUE(low.EndStroke());
    ASSERT_TRUE(high.BeginStroke());
    ASSERT_TRUE(high.ApplyBrush(highBrush));
    ASSERT_TRUE(high.EndStroke());

    EXPECT_EQ(low.PaintData()->opacity[center], 51u);
    EXPECT_EQ(high.PaintData()->opacity[center], 204u);
    const int lowStamps = stampCountToCap(0.2f);
    const int highStamps = stampCountToCap(0.8f);
    EXPECT_GT(lowStamps, 0);
    EXPECT_GT(highStamps, 0);
    EXPECT_LT(highStamps, lowStamps);
}

TEST(TerrainPaintSessionTest, SprayNoiseIsDeterministicAndModulatesCoverage)
{
    const VL::TerrainCollisionInstance instance = MakePaintInstance();
    VL::Editor::TerrainPaintSession noiseFree;
    VL::Editor::TerrainPaintSession sprayA;
    VL::Editor::TerrainPaintSession sprayB;
    VL::Editor::TerrainPaintSession otherSeed;
    std::string error;
    ASSERT_TRUE(
        VL::Editor::TerrainPaintSession::Create(instance, noiseFree, error))
        << error;
    ASSERT_TRUE(
        VL::Editor::TerrainPaintSession::Create(instance, sprayA, error))
        << error;
    ASSERT_TRUE(
        VL::Editor::TerrainPaintSession::Create(instance, sprayB, error))
        << error;
    ASSERT_TRUE(
        VL::Editor::TerrainPaintSession::Create(instance, otherSeed, error))
        << error;

    const auto stamp = [](VL::Editor::TerrainPaintSession& session,
                          const VL::Editor::TerrainPaintBrush& brush)
    {
        return session.BeginStroke() &&
            session.ApplyBrush(brush) &&
            session.EndStroke();
    };
    ASSERT_TRUE(stamp(noiseFree, MakePaintBrush(1.0f, 1.0f, 0.0f, 0u)));
    ASSERT_TRUE(stamp(sprayA, MakePaintBrush(1.0f, 1.0f, 0.8f, 123u)));
    ASSERT_TRUE(stamp(sprayB, MakePaintBrush(1.0f, 1.0f, 0.8f, 123u)));
    ASSERT_TRUE(stamp(otherSeed, MakePaintBrush(1.0f, 1.0f, 0.8f, 456u)));

    const auto weightSum = [](const VL::Editor::TerrainPaintSession& session)
    {
        float sum = 0.0f;
        for (float weight : session.PaintData()->weights)
            sum += weight;
        return sum;
    };
    EXPECT_LT(weightSum(sprayA), weightSum(noiseFree));
    EXPECT_EQ(sprayA.PaintData()->weights, sprayB.PaintData()->weights);
    EXPECT_EQ(sprayA.PaintData()->opacity, sprayB.PaintData()->opacity);
    EXPECT_NE(sprayA.PaintData()->weights, otherSeed.PaintData()->weights);
}

TEST(TerrainPaintSessionTest, EraseReducesCoverageAndRespectsFlow)
{
    const VL::TerrainCollisionInstance instance = MakePaintInstance();
    const size_t center = static_cast<size_t>(5) * 11 + 5;

    const auto opacityAfterErase = [&instance, center](float flow)
    {
        VL::Editor::TerrainPaintSession session;
        std::string error;
        if (!VL::Editor::TerrainPaintSession::Create(
                instance, session, error))
            return uint8_t{0};
        const VL::Editor::TerrainPaintBrush paint =
            MakePaintBrush(1.0f, 1.0f);
        if (!session.BeginStroke() || !session.ApplyBrush(paint) ||
            !session.EndStroke())
            return uint8_t{0};
        VL::Editor::TerrainPaintBrush erase = MakePaintBrush(1.0f, flow);
        erase.erase = true;
        if (!session.BeginStroke() || !session.ApplyBrush(erase) ||
            !session.EndStroke())
            return uint8_t{0};
        return session.PaintData()->opacity[center];
    };

    const uint8_t lowFlow = opacityAfterErase(0.2f);
    const uint8_t highFlow = opacityAfterErase(0.8f);
    EXPECT_EQ(lowFlow, 204u);
    EXPECT_EQ(highFlow, 51u);
    EXPECT_LT(highFlow, lowFlow);
}

TEST(TerrainPaintSessionTest, InvalidPaintRangeFieldsAreRejectedWithoutChange)
{
    const VL::TerrainCollisionInstance instance = MakePaintInstance();
    VL::Editor::TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(
        VL::Editor::TerrainPaintSession::Create(instance, session, error))
        << error;
    ASSERT_TRUE(session.BeginStroke());

    VL::Editor::TerrainPaintBrush brush = MakePaintBrush();
    brush.opacity = 1.1f;
    EXPECT_FALSE(session.ApplyBrush(brush));
    brush = MakePaintBrush();
    brush.flow = -0.1f;
    EXPECT_FALSE(session.ApplyBrush(brush));
    brush = MakePaintBrush();
    brush.noise = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(session.ApplyBrush(brush));
    EXPECT_EQ(session.Revision(), 0u);
    EXPECT_FALSE(session.EndStroke());
}

TEST(TerrainHeightmapEditorTest, SmoothReducesSpikeAndLeavesFlatTerrainUntouched)
{
    VL::TerrainHeightmapEditor flat = MakeEditor(
        5, 5, [](uint32_t, uint32_t) { return 0.5f; });
    const VL::TerrainSculptResult flatResult = flat.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Smooth, 0.5f, 0.5f, 1.0f));
    EXPECT_FALSE(flatResult.applied);
    EXPECT_EQ(flat.Revision(), 0u);

    VL::TerrainHeightmapEditor spiked = MakeEditor(
        5, 5, [](uint32_t x, uint32_t y) {
            return (x == 2 && y == 2) ? 1.0f : 0.0f;
        });
    const VL::TerrainSculptResult result = spiked.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Smooth, 0.5f, 0.25f, 1.0f));
    ASSERT_TRUE(result.applied);
    EXPECT_LT(spiked.NormalizedSamples()[2 * 5 + 2], 1.0f);
}

TEST(TerrainHeightmapEditorTest, SmoothPreservesSymmetricHeightProfile)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        5, 5, [](uint32_t x, uint32_t) {
            return x == 2 ? 1.0f : 0.0f;
        });
    const VL::TerrainSculptResult result = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Smooth, 0.5f, 0.5f, 1.0f, 0.0f));
    ASSERT_TRUE(result.applied);

    // 输入关于 u=0.5 对称，3x3 盒式平滑也必须保持左右对称。
    const std::vector<float>& samples = editor.NormalizedSamples();
    for (uint32_t y = 0; y < editor.Height(); ++y)
    {
        for (uint32_t x = 0; x < editor.Width(); ++x)
        {
            EXPECT_FLOAT_EQ(
                samples[static_cast<size_t>(y) * editor.Width() + x],
                samples[static_cast<size_t>(y) * editor.Width() +
                    (editor.Width() - 1 - x)]);
        }
    }
}

TEST(TerrainHeightmapEditorTest, DirtyRectMapsToChunksAcrossBoundary)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        4, 4, [](uint32_t, uint32_t) { return 0.5f; });
    // 4x4 高度图、gridResolution=4、chunkQuads=2：UV=0.5 正好在四个
    // chunk 共享的边界上，笔刷覆盖边界两侧的 texel。
    const VL::TerrainSculptResult result = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.5f, 0.4f, 0.1f, 0.0f));
    ASSERT_TRUE(result.applied);
    ASSERT_TRUE(result.dirtyRect.valid);
    EXPECT_EQ(result.dirtyRect.minX, 1u);
    EXPECT_EQ(result.dirtyRect.maxX, 2u);
    EXPECT_EQ(result.dirtyRect.minY, 1u);
    EXPECT_EQ(result.dirtyRect.maxY, 2u);

    EXPECT_EQ(editor.DirtyChunkCount(), 4u);
    EXPECT_TRUE(editor.IsChunkDirty(0, 0));
    EXPECT_TRUE(editor.IsChunkDirty(1, 0));
    EXPECT_TRUE(editor.IsChunkDirty(0, 1));
    EXPECT_TRUE(editor.IsChunkDirty(1, 1));
}

TEST(TerrainHeightmapEditorTest, SingleTexelDirtyRegionMapsToOneChunk)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        8, 8, [](uint32_t, uint32_t) { return 0.5f; });
    // UV=0 只命中 texel (0,0)；扩张后的双线性影响仍完全落在第一个 chunk。
    const VL::TerrainSculptResult result = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.0f, 0.01f, 0.1f, 0.0f));
    ASSERT_TRUE(result.applied);
    EXPECT_EQ(result.dirtyRect.minX, 0u);
    EXPECT_EQ(result.dirtyRect.maxX, 0u);
    EXPECT_EQ(result.dirtyRect.minY, 0u);
    EXPECT_EQ(result.dirtyRect.maxY, 0u);

    EXPECT_EQ(editor.DirtyChunkCount(), 1u);
    EXPECT_TRUE(editor.IsChunkDirty(0, 0));
}

TEST(TerrainHeightmapEditorTest, ResetDirtyKeepsRevisionAndSamples)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        4, 4, [](uint32_t, uint32_t) { return 0.5f; });
    const VL::TerrainSculptResult result = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.5f, 0.5f, 0.1f));
    ASSERT_TRUE(result.applied);
    const std::vector<float> samples = editor.NormalizedSamples();

    editor.ResetDirty();
    EXPECT_FALSE(editor.HasDirty());
    EXPECT_EQ(editor.DirtyChunkCount(), 0u);
    EXPECT_EQ(editor.Revision(), 1u);
    EXPECT_EQ(editor.NormalizedSamples(), samples);
}

TEST(TerrainHeightmapEditorTest, MarkAllDirtyReportsEveryChunk)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        4, 4, [](uint32_t, uint32_t) { return 0.5f; });
    editor.MarkAllDirty();
    EXPECT_TRUE(editor.HasDirty());
    EXPECT_TRUE(editor.DirtyRect().valid);
    EXPECT_EQ(editor.DirtyRect().minX, 0u);
    EXPECT_EQ(editor.DirtyRect().maxX, 3u);
    EXPECT_EQ(editor.DirtyChunkCount(), 4u);
    EXPECT_TRUE(editor.IsChunkDirty(1, 1));

    editor.ResetDirty();
    EXPECT_FALSE(editor.HasDirty());
    EXPECT_EQ(editor.DirtyChunkCount(), 0u);
}

TEST(TerrainHeightmapEditorTest, InvalidBrushesAreRejected)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        4, 4, [](uint32_t, uint32_t) { return 0.5f; });
    EXPECT_THROW(
        editor.ApplyBrush(MakeBrush(
            VL::TerrainSculptMode::Raise, 0.5f, 0.0f, 0.1f)),
        std::invalid_argument);
    EXPECT_THROW(
        editor.ApplyBrush(MakeBrush(
            VL::TerrainSculptMode::Raise, 0.5f, -0.1f, 0.1f)),
        std::invalid_argument);
    EXPECT_THROW(
        editor.ApplyBrush(MakeBrush(
            VL::TerrainSculptMode::Raise, 0.5f, 0.5f, 0.0f)),
        std::invalid_argument);
    EXPECT_THROW(
        editor.ApplyBrush(MakeBrush(
            VL::TerrainSculptMode::Raise, 0.5f, 0.5f, -0.1f)),
        std::invalid_argument);
    EXPECT_THROW(
        editor.ApplyBrush(MakeBrush(
            VL::TerrainSculptMode::Raise, 0.5f, 0.5f, 0.1f, 1.1f)),
        std::invalid_argument);

    VL::TerrainSculptBrush nonFinite = MakeBrush(
        VL::TerrainSculptMode::Raise, 0.5f, 0.5f, 0.1f);
    nonFinite.centerUv.x() = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(editor.ApplyBrush(nonFinite), std::invalid_argument);

    VL::TerrainSculptBrush outside = MakeBrush(
        VL::TerrainSculptMode::Raise, 0.5f, 0.5f, 0.1f);
    outside.centerUv.x() = 1.01f;
    EXPECT_THROW(editor.ApplyBrush(outside), std::invalid_argument);
}

TEST(TerrainHeightmapEditorTest, BrushWithNoAffectedTexelsIsANoOp)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        8, 8, [](uint32_t, uint32_t) { return 0.5f; });
    // 0.01 的 UV 半径小于半个 texel 间距，且中心落在 texel 之间。
    const VL::TerrainSculptResult result = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.5f, 0.01f, 0.1f));
    EXPECT_FALSE(result.applied);
    EXPECT_EQ(result.revision, 0u);
    EXPECT_FALSE(editor.HasDirty());
}

TEST(TerrainHeightmapEditorTest, RepeatedIdenticalBrushesAreDeterministic)
{
    VL::TerrainHeightmapEditor first = MakeEditor(
        8, 8, [](uint32_t x, uint32_t y) {
            return 0.25f + static_cast<float>(x + y) * 0.01f;
        });
    VL::TerrainHeightmapEditor second = MakeEditor(
        8, 8, [](uint32_t x, uint32_t y) {
            return 0.25f + static_cast<float>(x + y) * 0.01f;
        });
    const VL::TerrainSculptBrush brush =
        MakeBrush(VL::TerrainSculptMode::Raise, 0.4f, 0.3f, 0.1f, 0.6f);

    for (int stamp = 0; stamp < 4; ++stamp)
    {
        const VL::TerrainSculptResult firstResult = first.ApplyBrush(brush);
        const VL::TerrainSculptResult secondResult = second.ApplyBrush(brush);
        EXPECT_EQ(firstResult.revision, secondResult.revision);
        EXPECT_EQ(firstResult.dirtyRect.minX, secondResult.dirtyRect.minX);
        EXPECT_EQ(firstResult.dirtyRect.maxY, secondResult.dirtyRect.maxY);
        EXPECT_FLOAT_EQ(firstResult.changedMin, secondResult.changedMin);
        EXPECT_FLOAT_EQ(firstResult.changedMax, secondResult.changedMax);
    }
    EXPECT_EQ(first.NormalizedSamples(), second.NormalizedSamples());
}

TEST(TerrainHeightmapEditorTest, ExportProducesFloatHeightmapConsumableByRuntime)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        4, 4, [](uint32_t x, uint32_t y) {
            return 0.3f + static_cast<float>(x + y) * 0.02f;
        });
    editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.5f, 0.5f, 0.1f));

    const HostImage exported = editor.ExportNormalizedHeightmap();
    EXPECT_EQ(exported.width, 4u);
    EXPECT_EQ(exported.height, 4u);
    EXPECT_EQ(exported.format, HostImage::PixelFormat::R32_FLOAT);
    EXPECT_EQ(exported.mipLevels, 1u);
    exported.ValidateStorage();

    for (const auto& uv : std::vector<std::pair<float, float>>{
        {0.0f, 0.0f}, {0.37f, 0.62f}, {1.0f, 1.0f}})
    {
        EXPECT_FLOAT_EQ(
            VL::TerrainHeightmap::SampleNormalized(exported, uv.first, uv.second),
            editor.SampleNormalized(uv.first, uv.second));
    }
}

TEST(TerrainHeightmapEditorTest, BuildDirtyChunkMeshesReusesRuntimeMeshFormula)
{
    VL::TerrainHeightmapEditor editor = MakeEditor(
        4, 4, [](uint32_t x, uint32_t y) {
            return 0.2f + static_cast<float>(x * 3 + y) * 0.05f;
        });
    const VL::TerrainSculptResult result = editor.ApplyBrush(
        MakeBrush(VL::TerrainSculptMode::Raise, 0.0f, 0.01f, 0.1f, 0.0f));
    ASSERT_TRUE(result.applied);

    const std::vector<VL::TerrainMeshChunk> dirtyMeshes =
        editor.BuildDirtyChunkMeshes();
    ASSERT_EQ(dirtyMeshes.size(), 1u);
    ASSERT_EQ(dirtyMeshes[0].chunkX, 0u);
    ASSERT_EQ(dirtyMeshes[0].chunkZ, 0u);

    VL::TerrainBuildPlan exportedPlan = MakePlan(
        editor.ExportNormalizedHeightmap());
    const std::vector<VL::TerrainMeshChunk> allMeshes =
        VL::TerrainMeshBuilder::BuildChunks(exportedPlan);
    ASSERT_EQ(allMeshes.size(), 4u);
    ASSERT_EQ(dirtyMeshes[0].vertices.size(), allMeshes[0].vertices.size());
    for (size_t index = 0; index < dirtyMeshes[0].vertices.size(); ++index)
    {
        EXPECT_TRUE(dirtyMeshes[0].vertices[index].position.isApprox(
            allMeshes[0].vertices[index].position));
        EXPECT_TRUE(dirtyMeshes[0].vertices[index].normal.isApprox(
            allMeshes[0].vertices[index].normal));
    }
    EXPECT_EQ(dirtyMeshes[0].indices, allMeshes[0].indices);
}
