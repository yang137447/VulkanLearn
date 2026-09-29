#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

#include "render/offline/offlineSceneAdapter.h"

namespace VL
{

// 只调度 GT 的离线相机请求；持久身份仍由 coordinator 对冻结内容计算。
class OfflineCameraMotionPolicy
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto SettleDelay = std::chrono::milliseconds(200);
    static constexpr auto PreviewBudget = std::chrono::milliseconds(30);
    // 升一档每轴像素翻倍，按像素工作量近似预测耗时约四倍；
    // 当前档 <5 ms 才升档，预计上一级 <20 ms，给 30 ms 预算留余量。
    static constexpr auto FastPreviewThreshold = std::chrono::milliseconds(5);

    void Reset() noexcept
    {
        observedCamera.reset();
        acceptedCamera.reset();
        acceptedAssets = nullptr;
        acceptedWorldGeneration = 0;
        moving = false;
        ResetAdaptation();
    }

    std::optional<OfflineRenderSettings> Decide(
        const CameraSnapshot& camera, uint64_t worldGeneration,
        const OfflineWorldAssets* assets, OfflineRenderSettings requested,
        Offline::SessionState sessionState, Clock::time_point now)
    {
        if (!observedCamera || worldGeneration != acceptedWorldGeneration)
        {
            OfflineRenderSettings target = requested;
            // 首次提交及 World 换代是完整目标，不能继承调用方的临时视口标记。
            target.interactivePreview = false;
            observedCamera = camera;
            acceptedCamera = camera;
            acceptedWorldGeneration = worldGeneration;
            acceptedAssets = assets;
            acceptedSettings = target;
            observedRequestedSettings = requested;
            moving = false;
            ResetAdaptation();
            return target;
        }

        // 用户输入换代时旧预览耗时不可用于新场景/尺寸/积分参数的档位判断。
        const bool changedRequestedInput = assets != acceptedAssets ||
            !SameSettings(requested, observedRequestedSettings);
        if (changedRequestedInput)
        {
            observedRequestedSettings = requested;
            ResetAdaptation();
        }
        if (!SameCamera(*observedCamera, camera))
        {
            observedCamera = camera;
            lastCameraChange = now;
            moving = true;
        }
        if (moving && now - lastCameraChange >= SettleDelay)
        {
            moving = false;
            ResetAdaptation();
        }

        if (sessionState == Offline::SessionState::Paused)
        {
            // 暂停时丢弃整次测量，恢复后不把用户等待时间算成渲染负载。
            previewInFlight = false;
            fastPreviewCount = 0;
        }
        if (moving && previewInFlight)
        {
            // 从 GT 接受请求计到首次观察 Completed，包含 worker 准备/积分
            // 及 GT 观察间隔；不使用样本数或 GPU 时间推测整张预览是否完成。
            const auto elapsed = now - previewStartedAt;
            if (sessionState == Offline::SessionState::Completed)
            {
                // 部分视口预览也以 Completed 结束计时；连续读取只计一次。
                previewInFlight = false;
                if (elapsed > PreviewBudget)
                {
                    if (previewDivisor < MaxPreviewDivisor &&
                        HasDifferentPreviewExtent(requested, previewDivisor * 2))
                        previewDivisor *= 2;
                    fastPreviewCount = 0;
                }
                else if (previewDivisor > MinPreviewDivisor)
                {
                    fastPreviewCount = elapsed < FastPreviewThreshold
                        ? fastPreviewCount + 1 : 0;
                    if (fastPreviewCount >= 3)
                    {
                        if (HasDifferentPreviewExtent(requested, previewDivisor / 2))
                            previewDivisor /= 2;
                        fastPreviewCount = 0;
                    }
                }
            }
            else if (sessionState == Offline::SessionState::Preparing &&
                     previewDivisor < MaxPreviewDivisor && elapsed > PreviewBudget &&
                     HasDifferentPreviewExtent(requested, previewDivisor * 2))
            {
                // 准备阶段超预算可以换档；已积分的 tile 则等待软截止发布，
                // 不在 30 ms 时取消即将完成的部分预览。
                previewInFlight = false;
                previewDivisor *= 2;
                fastPreviewCount = 0;
            }
            else if (sessionState == Offline::SessionState::Failed ||
                     sessionState == Offline::SessionState::Canceled ||
                     sessionState == Offline::SessionState::Idle)
            {
                // 失败或取消不是已完成样本，也打断连续快预览的统计。
                previewInFlight = false;
                fastPreviewCount = 0;
            }
        }

        OfflineRenderSettings target = requested;
        // 该标记仅描述本次视口交互目标，不进入停稳后的完整渲染。
        target.interactivePreview = moving;
        if (moving)
        {
            // 每轴向上取整并保底 1；只缩减冻结目标，停稳后恢复用户请求。
            target.width = PreviewExtent(requested.width, previewDivisor);
            target.height = PreviewExtent(requested.height, previewDivisor);
            target.samples = 1;
        }

        const bool changedEffectiveInput =
            assets != acceptedAssets ||
            target.width != acceptedSettings.width ||
            target.height != acceptedSettings.height ||
            target.bounces != acceptedSettings.bounces ||
            target.lightSamples != acceptedSettings.lightSamples ||
            target.seed != acceptedSettings.seed ||
            target.interactivePreview != acceptedSettings.interactivePreview;
        const bool changedCamera = !SameCamera(*acceptedCamera, camera);
        if (!changedRequestedInput && !changedEffectiveInput && !changedCamera &&
            target.samples == acceptedSettings.samples)
            return std::nullopt;

        // 原始输入变化立即交付，即使 spp-only/奇数 resize 的预览目标未变；
        // 只有纯相机请求合并等待完整样本，避免每帧取消而始终没有新视角。
        if (moving && changedCamera && !changedRequestedInput && !changedEffectiveInput &&
            acceptedSettings.samples == 1 &&
            sessionState != Offline::SessionState::Completed)
            return std::nullopt;

        // coordinator 会忽略完全相同的冻结目标，不能为这类通知虚构新计时。
        const bool changedSubmission = changedEffectiveInput || changedCamera ||
            target.samples != acceptedSettings.samples;
        acceptedCamera = camera;
        acceptedAssets = assets;
        acceptedSettings = target;
        previewInFlight = moving && changedSubmission &&
            sessionState != Offline::SessionState::Paused;
        if (previewInFlight) previewStartedAt = now;
        return target;
    }

private:
    static constexpr int MinPreviewDivisor = 2;
    static constexpr int MaxPreviewDivisor = 16;

    static int PreviewExtent(int extent, int divisor) noexcept
    {
        return std::max(1, extent / divisor + (extent % divisor != 0));
    }

    bool HasDifferentPreviewExtent(const OfflineRenderSettings& requested,
                                   int nextDivisor) const noexcept
    {
        // ceil 后相同的尺寸没有性能/画质变化，保留当前档及未完成任务。
        return PreviewExtent(requested.width, nextDivisor) !=
                PreviewExtent(requested.width, previewDivisor) ||
            PreviewExtent(requested.height, nextDivisor) !=
                PreviewExtent(requested.height, previewDivisor);
    }

    void ResetAdaptation() noexcept
    {
        previewDivisor = MinPreviewDivisor;
        fastPreviewCount = 0;
        previewInFlight = false;
        previewStartedAt = Clock::time_point{};
    }

    static bool SameSettings(const OfflineRenderSettings& left,
                             const OfflineRenderSettings& right) noexcept
    {
        return left.width == right.width && left.height == right.height &&
            left.samples == right.samples && left.bounces == right.bounces &&
            left.lightSamples == right.lightSamples && left.seed == right.seed &&
            left.interactivePreview == right.interactivePreview;
    }

    static bool SameCamera(const CameraSnapshot& left, const CameraSnapshot& right)
    {
        return (left.position.array() == right.position.array()).all() &&
            (left.forward.array() == right.forward.array()).all() &&
            (left.right.array() == right.right.array()).all() &&
            (left.up.array() == right.up.array()).all() &&
            left.horizontalFovDegrees == right.horizontalFovDegrees &&
            left.clipNear == right.clipNear && left.clipFar == right.clipFar;
    }

    std::optional<CameraSnapshot> observedCamera;
    std::optional<CameraSnapshot> acceptedCamera;
    Clock::time_point lastCameraChange{};
    uint64_t acceptedWorldGeneration = 0;
    const OfflineWorldAssets* acceptedAssets = nullptr;
    OfflineRenderSettings acceptedSettings;
    OfflineRenderSettings observedRequestedSettings;
    Clock::time_point previewStartedAt{};
    int previewDivisor = MinPreviewDivisor;
    int fastPreviewCount = 0;
    bool previewInFlight = false;
    bool moving = false;
};

} // namespace VL
