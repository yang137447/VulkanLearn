#include "skeletalPlaybackInternal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace VL::Animation
{
namespace
{
std::atomic<uint64_t> nextPlaybackIdentity{1};
}

const Montage* SkeletalAsset::FindMontage(std::string_view idOrPath) const
{
    for (const Montage& montage : AnimationAsset().montages)
    {
        if (montage.id == idOrPath || montage.sourcePath == idOrPath)
        {
            return &montage;
        }
    }
    return nullptr;
}

SkeletalPlayback::SkeletalPlayback(std::shared_ptr<const SkeletalAsset> input)
    : asset(std::move(input))
{
    if (!asset || asset->mesh.sections.empty() || asset->mesh.joints.empty() || !asset->mesh.runtime)
        throw std::invalid_argument("Playback requires a skeletal mesh");
    // loader 和直接构造资产的调用方共用此边界：动作源/profile 必须成对，
    // 且不能把多层重定向或目标自带片段藏在播放器后面。
    if (static_cast<bool>(asset->animationSource) != static_cast<bool>(asset->retargetProfile) ||
        (asset->IsRetargeted() &&
            (asset->animationSource->IsRetargeted() ||
                asset->animationSource->clips.empty() || !asset->animationSource->mesh.runtime ||
                !asset->clips.empty() || !asset->montages.empty())))
        throw std::invalid_argument("Retargeted playback requires a valid animation source and profile");
    if (asset->IsRetargeted())
        compiledRetarget = std::make_shared<const CompiledRetarget>(
            asset->animationSource->mesh, asset->mesh, *asset->retargetProfile);
    playbackIdentity = nextPlaybackIdentity.fetch_add(1);
    displayedLocals = Sample(selected, time);
    poses = Evaluate(displayedLocals, discontinuity, jointModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    RebuildNotifyEvents();
}

float SkeletalPlayback::ClipDuration() const
{
    const auto& animation = asset->AnimationAsset();
    return animation.clips.empty() ? 0.0f : animation.clips.at(selected).runtime->duration();
}

float SkeletalPlayback::PlaybackRangeEnd() const
{
    // 多段 Montage 的合法上界是 Montage 时间轴长度，不是当前 clip 的 1.333 s。
    // rangeEnd 仍是“未设置”哨兵；section 推进只写内部范围，不改变段映射。
    if (!montageSegments.empty())
    {
        const float timelineEnd = std::max(0.0f, montageTimelineLength);
        if (playbackRangeEnd <= 0)
            return timelineEnd;
        return std::min(playbackRangeEnd, timelineEnd);
    }
    const float clipEnd = ClipDuration();
    // end==0 是“未显式设置”的哨兵：范围末尾跟随当前片段，切片段后无需重新设置。
    if (playbackRangeEnd <= 0) return clipEnd;
    return std::min(playbackRangeEnd, clipEnd);
}

float SkeletalPlayback::Duration() const
{
    // 播放长度按范围计算；time/notify 仍是 clip-local 绝对时间，所以 Completed 比较
    // PlaybackRangeEnd()，而不是 Duration()。
    return std::max(0.0f, PlaybackRangeEnd() - playbackRangeStart);
}

std::vector<std::shared_ptr<const SkinPose>> SkeletalPlayback::Evaluate(
    const LocalPose& locals, uint64_t epoch,
    std::shared_ptr<const std::vector<Eigen::Matrix4f>>& outputModels) const
{
    auto models = LocalToModels(asset->mesh, locals);
    // 所有 section 完成后才发布时钟、选择与姿态，异常时保持同一个旧状态。
    std::vector<std::shared_ptr<const SkinPose>> next;
    for (const auto& section : asset->mesh.sections)
        next.push_back(BuildSkinPose(section, models, epoch, playbackIdentity,
            &asset->mesh.joints));
    outputModels = std::make_shared<const std::vector<Eigen::Matrix4f>>(std::move(models));
    return next;
}

void SkeletalPlayback::Tick(float deltaSeconds, const Eigen::Matrix4f* meshTransform,
    const Eigen::Vector3f* actorPosition, float actorYaw)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0)
        throw std::invalid_argument("Playback delta must be finite and nonnegative");
    rootMotionDelta.setZero();
    rootMotionYawDelta = 0;
    // 同步组对齐先于本 tick 的时间推进应用：跳变本身不产生根位移，
    // 本帧根轨迹从对齐后的 time 到采样点，等价于 UE 的 follower 校正。
    bool syncJumped = false;
    if (syncPendingValid)
    {
        if (syncPendingHasTime && playing && deltaSeconds > 0)
        {
            const float rangeStart = PlaybackRangeStart();
            const float rangeEnd = PlaybackRangeEnd();
            float target = syncPendingTime;
            if (looping && rangeEnd > rangeStart)
            {
                target = rangeStart + std::fmod(target - rangeStart, rangeEnd - rangeStart);
                if (target < rangeStart) target += rangeEnd - rangeStart;
            }
            else
                target = std::clamp(target, rangeStart, std::max(rangeStart, rangeEnd - 1e-4f));
            if (std::abs(target - time) > 1e-5f)
            {
                // 相位跳变是硬跳变：递增 epoch，避免把跨相位差算成根位移。
                time = target;
                ++discontinuity;
                syncJumped = true;
            }
        }
        rate = std::clamp(syncPendingRate, AnimationSyncGroupRegistry::RateMin,
            AnimationSyncGroupRegistry::RateMax);
        syncPendingValid = false;
        syncPendingHasTime = false;
    }
    // 每个 Tick 重建“本反馈窗口”的事件：先并入控制面（Play/Stop/Select/TransitionTo）
    // 补发的 end，再追加本次位置穿越；未被读取的控制面事件不会在同帧 Tick 后丢失。
    firedNotifies = std::move(pendingNotifies);
    pendingNotifies.clear();
    if (!playing || Duration() == 0 || deltaSeconds == 0) return;
    AdvanceSyncSourceClock(deltaSeconds);
    if (syncJumped)
    {
        // 与 Seek 一样，相位跳变不补发被跳过的事件；只按新位置重建活动区间，
        // 避免旧 notifyState 在时间跳变后继续悬空。
        std::fill(notifyActive.begin(), notifyActive.end(), 0);
        RefreshActiveNotifyStates();
    }
    // 多 section Montage 走独立的推进器（见 TickMontageSections）；单 section/普通片段
    // 保持下面的原路径不变，避免既有行为因 section 支持发生位级漂移。
    if (!montageSegments.empty())
    {
        TickSegmentedMontage(deltaSeconds, meshTransform, actorPosition, actorYaw);
        return;
    }
    if (!montageSections.empty())
    {
        TickMontageSections(deltaSeconds, meshTransform, actorPosition, actorYaw);
        return;
    }
    const float rangeStart = PlaybackRangeStart();
    const float rangeEnd = PlaybackRangeEnd();
    const double nextPosition = static_cast<double>(time) + double(deltaSeconds) * rate;
    // 循环域是 [rangeStart,rangeEnd)；非循环在 rangeEnd 处 clamp。这里只求一次回绕，
    // 单个 tick 跨越多圈时事件只按“尾部+头部”各处理一次（与单次时钟采样一致）。
    bool wrapped = false;
    double next = nextPosition;
    if (looping)
    {
        if (next >= rangeEnd)
        {
            next = double(rangeStart) +
                std::fmod(next - double(rangeStart), double(rangeEnd - rangeStart));
            wrapped = true;
        }
    }
    else next = std::min(next, double(rangeEnd));
    const auto epoch = discontinuity + (looping && !inPlace && wrapped ? 1 : 0);
    const float sampleTime = static_cast<float>(next);
    const float elapsed = IsBlending() ? std::min(blendElapsed + deltaSeconds, blendDuration) : 0;
    if (sampleTime == time && epoch == discontinuity && !wrapped && !IsBlending()) return;
    const float previousTime = time;
    auto locals = Sample(selected, sampleTime);
    if (!inertialization.Empty() && elapsed < blendDuration)
    {
        const auto target = ReadKinematics(locals, asset->mesh.joints.size());
        locals = WriteKinematics(std::move(locals), inertialization.Evaluate(target, elapsed));
    }
    else if (!blendSource.empty() && elapsed < blendDuration)
        locals = Crossfade(blendSource, std::move(locals), elapsed / blendDuration, asset->mesh);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto nextPoses = Evaluate(locals, epoch, nextModels);
    poses.swap(nextPoses);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    if (epoch == discontinuity)
    {
        const auto& clip = asset->AnimationAsset().clips.at(selected);
        rootMotionDelta = RootTranslation(clip, sampleTime) - RootTranslation(clip, time);
        const auto rotation = RootRotation(clip, sampleTime) *
            RootRotation(clip, time).inverse();
        // 先放原始根 yaw；旋转 warp 只在本 tick 有激活窗口且提交了 target 旋转时
        // 覆盖它，平移结果（rootMotionDelta）与 yaw 通道互不覆盖。
        float yawDelta = YawFromQuaternion(rotation);
        // 原生 modifier 在模型空间改写本 tick 的根位移/根 yaw；C# 只消费最终
        // rootMotion/rootMotionYaw 交给 Jolt 与 Actor 旋转，不做窗口提取、
        // target 换算或 SkewWarp 数学。
        if (!motionWarp.Empty())
        {
            if (meshTransform && actorPosition)
                rootMotionDelta = motionWarp.Apply(asset->AnimationAsset().mesh, clip, time, sampleTime,
                    rootMotionDelta, *meshTransform, *actorPosition, actorYaw, &yawDelta);
            motionWarpState = motionWarp.DebugState(time);
        }
        else motionWarpState.clear();
        rootMotionYawDelta = yawDelta;
    }
    time = sampleTime;
    RefreshSyncMarker();
    // notify 只在姿态求值成功后提交：异常 tick 不改变活动区间。事件按实际位置穿越触发，
    // 显式 seek/切片段不补发历史（见 Seek/Select 的清理）。
    if (wrapped)
    {
        ScanNotifySegment(std::max(double(previousTime), double(rangeStart)), double(rangeEnd), false);
        // 回绕时仍活跃的区间先在循环末尾补 end(interrupted)，再扫描新循环头部；
        // 跨越循环点的 notifyState 在同一 tick 内得到 end→begin，固定“先尾部后头部”。
        EndActiveNotifyStates(rangeEnd, true, false);
        ScanNotifySegment(double(rangeStart), double(sampleTime), false);
    }
    else
    {
        // 一次性播放到达范围末尾的那一 tick 允许 time==rangeEnd 的瞬时 notify；
        // 越过末尾仍活跃的 notifyState 在 completed 时补 end(interrupted)。
        const bool terminal = !looping && sampleTime >= rangeEnd;
        ScanNotifySegment(std::max(double(previousTime), double(rangeStart)),
            double(sampleTime), terminal);
        if (terminal) EndActiveNotifyStates(rangeEnd, true, false);
    }
    RefreshActiveNotifyStates();
    blendElapsed = elapsed;
    if (IsBlending() && elapsed >= blendDuration)
    {
        blendSource.clear();
        inertialization.Clear();
        ClearSyncSourceClock();
    }
    if (Completed()) playing = false;
    discontinuity = epoch; // 仅保留原根的循环才会归零模型根轨迹。
}

std::shared_ptr<SkeletalPlayback> SkeletalPlayback::NewInstance() const
{
    return std::make_shared<SkeletalPlayback>(asset);
}
}
