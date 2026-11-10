#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <Eigen/Dense>

#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxGroupPaint.h"
#include "hair/tressfxRootCloud.h"
#include "hair/tressfxSurfaceBrush.h"

namespace
{

std::shared_ptr<const VL::Hair::TressfxRootCloud> BuildSphereCloud(
    size_t count, float radius)
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> growthDirections;
    positions.reserve(count);
    growthDirections.reserve(count);
    const float goldenAngle = 3.14159265358979323846f * (3.0f - std::sqrt(5.0f));
    for (size_t index = 0; index < count; ++index)
    {
        const float y = 1.0f - 2.0f * (static_cast<float>(index) + 0.5f) /
            static_cast<float>(count);
        const float ring = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float theta = goldenAngle * static_cast<float>(index);
        positions.emplace_back(radius * ring * std::cos(theta), radius * y,
            radius * ring * std::sin(theta));
        growthDirections.push_back(Eigen::Vector3f::UnitY());
    }
    return VL::Hair::TressfxRootCloud::Build(positions, growthDirections);
}

double Percentile(std::vector<double> samples, double fraction)
{
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t index = static_cast<size_t>(
        std::clamp(fraction, 0.0, 1.0) * static_cast<double>(samples.size() - 1));
    return samples[index];
}

} // namespace

int main()
{
    constexpr size_t kGuideCount = 40334;
    constexpr size_t kIterations = 240;
    const auto roots = BuildSphereCloud(kGuideCount, 10.0f);

    auto binding = std::make_shared<VL::Hair::TressfxGroupBinding>();
    binding->guideGroupIds.assign(kGuideCount, 0u);
    for (size_t guide = 0; guide < kGuideCount; guide += 3)
        binding->guideGroupIds[guide] = 1u;
    binding->groupNames = {"fringe", "side"};
    binding->key = "benchmark";

    VL::Hair::BrushCamera camera;
    camera.position = Eigen::Vector3f(0.0f, 0.0f, 100.0f);
    camera.forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    camera.right = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    camera.up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    camera.horizontalFovDegrees = 60.0f;
    camera.clipNear = 0.1f;
    camera.clipFar = 1000.0f;
    const VL::Hair::BrushViewport viewport{0, 0, 1000, 1000};

    VL::Hair::BrushSettings settings;
    settings.radiusPixels = 30.0f;
    settings.maxNormalAngleDegrees = 60.0f;
    settings.selectionThreshold = 0.05f;
    settings.maxRayDistanceWorld = 100000.0f;

    VL::Hair::GroupPaintController controller;
    const auto sessionStart = std::chrono::steady_clock::now();
    controller.BeginSession(binding);
    const double sessionMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - sessionStart).count();

    VL::Hair::SurfaceBrush brush;
    VL::Hair::RootCloudSelector selector;
    std::vector<double> frameMs;
    frameMs.reserve(kIterations);
    size_t validSamples = 0;
    double candidateTotal = 0.0;
    double hitTotal = 0.0;

    // 预热一次，避免首次触摸 Eigen/Vulkan 无关的缓存页影响第一帧样本。
    (void)brush.Sample(Eigen::Vector2f(500.0f, 500.0f), viewport, camera,
        *roots, Eigen::Matrix4f::Identity(), settings);

    for (size_t iteration = 0; iteration < kIterations; ++iteration)
    {
        const float t = static_cast<float>(iteration);
        const Eigen::Vector2f cursor(
            // 球面在 1000px 视口里半径约 96px；限制在轮廓内，避免大量空采样。
            500.0f + 70.0f * std::sin(t * 0.05f),
            500.0f + 50.0f * std::cos(t * 0.07f));
        const auto begin = std::chrono::steady_clock::now();
        const VL::Hair::BrushStamp stamp = brush.Sample(cursor, viewport, camera,
            *roots, Eigen::Matrix4f::Identity(), settings);
        const auto hits = selector.Select(*roots, stamp,
            Eigen::Matrix4f::Identity(), settings);
        const auto summary = controller.AnalyzeHits(hits, 1,
            VL::Hair::GroupPaintMode::Add, false);
        const auto end = std::chrono::steady_clock::now();

        frameMs.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        if (stamp.valid) ++validSamples;
        candidateTotal += static_cast<double>(stamp.candidateCount);
        hitTotal += static_cast<double>(hits.size());
        if (summary.selectedGuides != hits.size())
            std::fprintf(stderr, "summary mismatch at %zu\n", iteration);
    }

    double mean = 0.0;
    for (double value : frameMs) mean += value;
    mean /= static_cast<double>(frameMs.size());
    const auto meanOf = [&](double total)
    {
        return total / static_cast<double>(frameMs.size());
    };

    std::printf("guides=%zu iterations=%zu valid=%zu\n",
        kGuideCount, kIterations, validSamples);
    std::printf("session-begin=%.4f ms (shared binding, no guide copy)\n", sessionMs);
    std::printf("sample+select+analyze mean=%.4f ms p50=%.4f ms p95=%.4f ms max=%.4f ms\n",
        mean, Percentile(frameMs, 0.50), Percentile(frameMs, 0.95),
        Percentile(frameMs, 1.0));
    std::printf("mean ray candidates=%.1f mean selected roots=%.1f\n",
        meanOf(candidateTotal), meanOf(hitTotal));
    return validSamples == 0 || frameMs.empty() ? 1 : 0;
}
