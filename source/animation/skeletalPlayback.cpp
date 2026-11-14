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
    // 分支是图级预计算：先于主状态采样求值，UseCachedPose 才能在同一帧读到。
    EvaluateCachedPoseBranches(deltaSeconds);
    if (useCachedPoseActive)
    {
        TickCachedPose(deltaSeconds);
        return;
    }
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
    AdvanceAdditiveClock(deltaSeconds);
    AdvanceLayerClock(deltaSeconds);
    AdvanceSlotMontageClock(deltaSeconds);
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
    float blendSampleTime = -1;
    // BlendSpace1D：第二片段按 A 的归一化相位采样，再在局部 TRS 上加权混合。
    // 混合发生在一次性过渡混合之前，等价于 UE“状态输出 → 过渡混合”的顺序。
    if (blendSpaceActive && blendSpaceClip < asset->AnimationAsset().clips.size())
    {
        const auto& blendAsset = asset->AnimationAsset().clips[blendSpaceClip];
        const auto& blendClip = blendAsset.runtime;
        const float phase = rangeEnd > rangeStart
            ? std::clamp((sampleTime - rangeStart) / (rangeEnd - rangeStart), 0.0f, 1.0f)
            : 0.0f;
        const float blendDurationSeconds = blendClip ? blendClip->duration() : 0.0f;
        float blendTime = phase * blendDurationSeconds * blendSpaceRate;
        if (!blendSpaceSyncGroup.empty())
        {
            const auto& sourceAsset = asset->AnimationAsset().clips[selected];
            blendTime = MapBlendSpaceMarkerPhase(sourceAsset, sampleTime, blendAsset,
                blendSpaceSyncGroup, blendTime);
        }
        if (blendDurationSeconds > 0 && blendTime >= blendDurationSeconds)
            blendTime = looping ? std::fmod(blendTime, blendDurationSeconds)
                : std::max(0.0f, blendDurationSeconds - 1e-4f);
        else if (!looping)
            blendTime = std::min(blendTime, std::max(0.0f, blendDurationSeconds - 1e-4f));
        blendSampleTime = blendTime;
        auto blendPose = Sample(blendSpaceClip, blendTime);
        // 端点不走 ozz 混合，保证 0/1 与单独播放 A/B 的姿态逐元素一致。
        if (blendSpaceWeight >= 1.0f) locals = std::move(blendPose);
        else if (blendSpaceWeight > 0.0f)
            locals = Crossfade(locals, std::move(blendPose), blendSpaceWeight, asset->mesh);
    }
    // ApplyAdditive 属于本状态姿态的一部分：先加在状态局部姿态上，再参与过渡混合。
    locals = ApplyAdditive(std::move(locals));
    // LayeredBoneBlend 只改 mask 命中的关节，顺序固定在 additive 之后。
    locals = ApplyLayered(std::move(locals));
    // Masked Slot Montage 叠在基础姿态上，只覆盖 mask 命中的关节。
    locals = ApplySlotMontage(std::move(locals));
    RebuildFrameCurves(sampleTime, blendSampleTime);
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

void SkeletalPlayback::AdvanceAdditiveClock(float deltaSeconds)
{
    if (!additiveActive || deltaSeconds <= 0) return;
    const auto& animation = asset->AnimationAsset();
    if (additiveClip >= animation.clips.size()) return;
    const auto& clip = animation.clips[additiveClip];
    const float duration = clip.runtime ? clip.runtime->duration() : 0.0f;
    if (duration <= 0) return;
    additiveTime += deltaSeconds;
    if (additiveLoop)
        additiveTime = std::fmod(additiveTime, duration);
    else
        additiveTime = std::min(additiveTime, std::max(0.0f, duration - 1e-4f));
}

LocalPose SkeletalPlayback::ApplyAdditive(LocalPose locals) const
{
    if (!additiveActive || additiveAlpha <= 0) return locals;
    const auto& animation = asset->AnimationAsset();
    if (additiveClip >= animation.clips.size()) return locals;
    const auto rest = asset->mesh.runtime->joint_rest_poses();
    const LocalPose reference(rest.begin(), rest.end());
    const LocalPose sample = Sample(additiveClip, additiveTime);
    const auto& clip = animation.clips[additiveClip];
    if (clip.sequence.additiveAnimType == "RotationOffsetMeshSpace")
        return ApplyMeshSpaceRotationAdditive(locals, sample, reference, additiveAlpha,
            asset->mesh.joints);
    return ApplyLocalSpaceAdditive(locals, sample, reference, additiveAlpha,
        asset->mesh.joints.size());
}

namespace
{
float SampleFloatCurve(const FloatCurve& curve, float time)
{
    if (curve.times.empty()) return 0;
    if (time <= curve.times.front()) return curve.values.front();
    if (time >= curve.times.back()) return curve.values.back();
    const auto upper = std::upper_bound(curve.times.begin(), curve.times.end(), time);
    const size_t high = static_cast<size_t>(upper - curve.times.begin());
    const size_t low = high - 1;
    const float span = curve.times[high] - curve.times[low];
    const float alpha = span > 0 ? (time - curve.times[low]) / span : 0;
    return curve.values[low] + (curve.values[high] - curve.values[low]) * alpha;
}
}

void SkeletalPlayback::RebuildFrameCurves(float baseTime, float blendTime)
{
    frameCurves.clear();
    const auto& clips = asset->AnimationAsset().clips;
    auto accumulate = [&](size_t index, float sampleTime, float weight)
    {
        if (index >= clips.size() || weight == 0 || sampleTime < 0) return;
        for (const auto& curve : clips[index].sequence.curves)
            frameCurves[curve.name] += SampleFloatCurve(curve, sampleTime) * weight;
    };
    if (baseTime >= 0 && selected < clips.size())
    {
        if (blendSpaceActive && blendSpaceClip < clips.size())
        {
            accumulate(selected, baseTime, 1.0f - blendSpaceWeight);
            accumulate(blendSpaceClip, blendTime, blendSpaceWeight);
        }
        else accumulate(selected, baseTime, 1.0f);
    }
    if (additiveActive && additiveAlpha > 0)
        accumulate(additiveClip, additiveTime, additiveAlpha);
}

void SkeletalPlayback::AdvanceLayerClock(float deltaSeconds)
{
    if (!layeredActive || deltaSeconds <= 0) return;
    const auto& animation = asset->AnimationAsset();
    if (layeredClip >= animation.clips.size()) return;
    const auto& clip = animation.clips[layeredClip];
    const float duration = clip.runtime ? clip.runtime->duration() : 0.0f;
    if (duration <= 0) return;
    layeredTime += deltaSeconds;
    if (layeredLoop)
        layeredTime = std::fmod(layeredTime, duration);
    else
        layeredTime = std::min(layeredTime, std::max(0.0f, duration - 1e-4f));
}

LocalPose SkeletalPlayback::ApplyLayered(LocalPose locals) const
{
    if (!layeredActive || layeredAlpha <= 0) return locals;
    const auto& animation = asset->AnimationAsset();
    if (layeredClip >= animation.clips.size()) return locals;
    const LocalPose sample = Sample(layeredClip, layeredTime);
    if (!layeredWeights.empty())
        return ApplyLayeredWeights(locals, sample, layeredWeights, layeredAlpha,
            asset->mesh.joints.size());
    return ApplyLayeredPose(locals, sample, layeredMask, layeredAlpha,
        asset->mesh.joints.size());
}

void SkeletalPlayback::AdvanceSlotMontageClock(float deltaSeconds)
{
    if (!slotMontageActive || slotMontageCompleted || deltaSeconds <= 0) return;
    const auto& animation = asset->AnimationAsset();
    if (slotMontageClip >= animation.clips.size()) return;
    const auto& clip = animation.clips[slotMontageClip];
    const float duration = clip.runtime ? clip.runtime->duration() : 0.0f;
    if (duration <= 0) return;
    const float previousTime = slotMontageTime;
    float nextTime = previousTime + deltaSeconds * slotMontageRate;
    if (slotMontageLoop)
    {
        if (nextTime >= duration)
        {
            nextTime = std::fmod(nextTime, duration);
            // 回绕：先扫尾部，再扫新循环头部；与主播放器的“先尾后头”一致。
            ScanSlotNotifies(previousTime, duration, false);
            ScanSlotNotifies(0.0, nextTime, false);
        }
        else ScanSlotNotifies(previousTime, nextTime, false);
        slotMontageTime = nextTime;
    }
    else if (slotMontageTime >= duration)
    {
        slotMontageTime = std::max(0.0f, duration - 1e-4f);
        slotMontageCompleted = true;
    }
    else if (nextTime >= duration)
    {
        // 一次性 overlay 到末帧：允许 time==duration 的瞬时 notify，并结束活动区间。
        ScanSlotNotifies(previousTime, duration, true);
        EndActiveSlotNotifyStates(duration, false);
        slotMontageTime = std::max(0.0f, duration - 1e-4f);
        slotMontageCompleted = true;
    }
    else
    {
        ScanSlotNotifies(previousTime, nextTime, false);
        slotMontageTime = nextTime;
    }
}

LocalPose SkeletalPlayback::ApplySlotMontage(LocalPose locals) const
{
    if (!slotMontageActive || slotMontageAlpha <= 0) return locals;
    const auto& animation = asset->AnimationAsset();
    if (slotMontageClip >= animation.clips.size()) return locals;
    const LocalPose sample = Sample(slotMontageClip, slotMontageTime);
    if (!slotMontageWeights.empty())
        return ApplyLayeredWeights(locals, sample, slotMontageWeights, slotMontageAlpha,
            asset->mesh.joints.size());
    return ApplyLayeredPose(locals, sample, slotMontageMask, slotMontageAlpha,
        asset->mesh.joints.size());
}

namespace
{
float BranchSmoothstep(float axis, float from, float to)
{
    const float span = to - from;
    const float alpha = span > 0 ? std::clamp((axis - from) / span, 0.0f, 1.0f) : 0.0f;
    return alpha * alpha * (3 - 2 * alpha);
}
}

void SkeletalPlayback::EvaluateCachedPoseBranches(float deltaSeconds)
{
    // 暂停/停止时保留上一帧的同帧缓存；delta=0 仍按当前时间求值（首帧/暂停帧）。
    if (!playing) return;
    frameCachedPoses.clear();
    const auto& clips = asset->AnimationAsset().clips;
    for (CachedPoseBranchState& branch : cachedPoseBranches)
    {
        const auto& definition = branch.definition;
        if (branch.clipIndices.empty()) continue;
        const size_t baseIndex = definition.samples.size() >= 3 ? 1 : 0;
        const auto& baseClip = clips.at(branch.clipIndices[baseIndex]);
        const float baseDuration = baseClip.runtime ? baseClip.runtime->duration() : 0.0f;
        if (baseDuration <= 0) continue;
        if (deltaSeconds > 0)
        {
            branch.time += deltaSeconds * definition.rate;
            if (definition.loop)
                branch.time = std::fmod(branch.time, baseDuration);
            else
                branch.time = std::min(branch.time, std::max(0.0f, baseDuration - 1e-4f));
        }
        const auto axisEntry = cachedPoseBranchAxes.find(definition.name);
        const float axis = axisEntry != cachedPoseBranchAxes.end() ? axisEntry->second : 0.0f;
        size_t first = baseIndex;
        size_t second = baseIndex;
        float target = 0;
        if (definition.samples.size() == 1)
            first = second = 0;
        else if (baseIndex == 0)
        {
            first = 0;
            second = 1;
            target = BranchSmoothstep(axis,
                definition.samples[0].threshold, definition.samples[1].threshold);
        }
        else if (axis <= definition.samples[1].threshold)
        {
            first = 1;
            second = 0;
            target = 1 - BranchSmoothstep(axis,
                definition.samples[0].threshold, definition.samples[1].threshold);
        }
        else
        {
            first = 1;
            second = 2;
            target = BranchSmoothstep(axis,
                definition.samples[1].threshold, definition.samples[2].threshold);
        }
        if (!branch.weightValid)
        {
            branch.weight = target;
            branch.weightValid = true;
        }
        else
        {
            const float step = 5 * std::max(0.0f, deltaSeconds);
            branch.weight = std::clamp(target, branch.weight - step, branch.weight + step);
        }
        LocalPose pose = Sample(branch.clipIndices[first], branch.time);
        if (first != second && branch.weight > 0)
        {
            const auto& otherClip = clips.at(branch.clipIndices[second]);
            const float otherDuration = otherClip.runtime ? otherClip.runtime->duration() : 0.0f;
            const float phase = std::clamp(branch.time / baseDuration, 0.0f, 1.0f);
            float otherTime = phase * otherDuration;
            if (!definition.loop)
                otherTime = std::min(otherTime, std::max(0.0f, otherDuration - 1e-4f));
            LocalPose other = Sample(branch.clipIndices[second], otherTime);
            if (branch.weight >= 1.0f) pose = std::move(other);
            else pose = Crossfade(pose, std::move(other), branch.weight, asset->mesh);
        }
        frameCachedPoses[definition.name] = std::move(pose);
    }
}

void SkeletalPlayback::TickCachedPose(float deltaSeconds)
{
    firedNotifies = std::move(pendingNotifies);
    pendingNotifies.clear();
    const auto frame = frameCachedPoses.find(useCachedPoseName);
    const auto found = cachedPoses.find(useCachedPoseName);
    if (frame == frameCachedPoses.end() && found == cachedPoses.end())
        throw std::runtime_error("Cached pose disappeared: " + useCachedPoseName);
    AdvanceAdditiveClock(deltaSeconds);
    AdvanceLayerClock(deltaSeconds);
    AdvanceSlotMontageClock(deltaSeconds);
    auto locals = frame != frameCachedPoses.end() ? frame->second : found->second;
    locals = ApplyAdditive(std::move(locals));
    locals = ApplyLayered(std::move(locals));
    locals = ApplySlotMontage(std::move(locals));
    RebuildFrameCurves(-1, -1);
    const float elapsed = IsBlending() ? std::min(blendElapsed + deltaSeconds, blendDuration) : 0;
    if (!inertialization.Empty() && elapsed < blendDuration)
    {
        const auto target = ReadKinematics(locals, asset->mesh.joints.size());
        locals = WriteKinematics(std::move(locals), inertialization.Evaluate(target, elapsed));
    }
    else if (!blendSource.empty() && elapsed < blendDuration)
        locals = Crossfade(blendSource, std::move(locals), elapsed / blendDuration, asset->mesh);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto nextPoses = Evaluate(locals, discontinuity, nextModels);
    poses.swap(nextPoses);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    // 缓存姿态不推进片段时钟，也不贡献根位移；过渡混合仍按真实 delta 收尾。
    rootMotionDelta.setZero();
    rootMotionYawDelta = 0;
    blendElapsed = elapsed;
    if (IsBlending() && elapsed >= blendDuration)
    {
        blendSource.clear();
        inertialization.Clear();
        ClearSyncSourceClock();
    }
}
}
