#include "skeletalPlaybackInternal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace VL::Animation
{
float SkeletalPlayback::MontageSourceTime(const MontageSegment& segment,
    float timeline, float clipDuration) const
{
    const float clipStart = std::clamp(segment.clipStartTime, 0.0f,
        std::max(0.0f, clipDuration));
    const float declaredEnd = segment.clipEndTime > 0 ?
        segment.clipEndTime : clipDuration;
    const float clipEnd = std::max(clipStart,
        std::min(declaredEnd, std::max(clipStart, clipDuration)));
    const float local = (timeline - segment.startTime) * segment.rateScale;
    if (!segment.loop)
        return std::clamp(clipStart + local, clipStart, clipEnd);

    const float span = clipEnd - clipStart;
    if (!(span > 0) || !std::isfinite(local))
        return clipStart;
    float wrapped = std::fmod(local, span);
    if (wrapped < 0) wrapped += span;
    return clipStart + wrapped;
}

SkeletalPlayback::MontageSample SkeletalPlayback::SampleMontageTimeline(
    float timeline, bool atEnd) const
{
    MontageSample result;
    if (montageSegments.empty() || montageSegmentClips.size() != montageSegments.size())
    {
        result.clip = selected;
        result.clipTime = timeline;
        return result;
    }

    constexpr float tolerance = 1e-5f;
    size_t segmentIndex = montageSegments.size() - 1;
    for (size_t index = 0; index < montageSegments.size(); ++index)
    {
        const MontageSegment& segment = montageSegments[index];
        if (timeline < segment.endTime - tolerance)
        {
            segmentIndex = index;
            break;
        }
        if ((atEnd || index + 1 == montageSegments.size()) &&
            timeline <= segment.endTime + tolerance)
        {
            segmentIndex = index;
            break;
        }
    }
    const MontageSegment& segment = montageSegments[segmentIndex];
    result.segment = segmentIndex;
    result.clip = montageSegmentClips[segmentIndex];
    const float duration = asset->AnimationAsset().clips.at(result.clip).runtime->duration();
    result.clipTime = MontageSourceTime(segment, timeline, duration);
    return result;
}

Eigen::Vector3f SkeletalPlayback::MontageRootDelta(float from, float to) const
{
    if (!(to > from) || montageSegments.empty())
        return Eigen::Vector3f::Zero();

    Eigen::Vector3f total = Eigen::Vector3f::Zero();
    float cursor = from;
    for (size_t index = 0; index < montageSegments.size() && cursor < to; ++index)
    {
        const MontageSegment& segment = montageSegments[index];
        const float begin = std::max(cursor, segment.startTime);
        const float end = std::min(to, segment.endTime);
        if (!(end > begin)) continue;
        const Clip& clip = asset->AnimationAsset().clips.at(montageSegmentClips[index]);
        const float duration = clip.runtime->duration();
        total += RootTranslation(clip, MontageSourceTime(segment, end, duration)) -
            RootTranslation(clip, MontageSourceTime(segment, begin, duration));
        cursor = end;
    }
    return total;
}

void SkeletalPlayback::ClearMontageSegments()
{
    montageSegments.clear();
    montageSegmentClips.clear();
    montageSegment = 0;
    montageTimelineLength = 0;
}

std::optional<float> SkeletalPlayback::SampleCurve(std::string_view name, float seconds) const
{
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return std::nullopt;
    // 曲线归当前播放片段所有；Montage 只改变播放范围，不复制 Sequence 曲线。
    return SampleFloatCurve(animation.clips.at(selected).sequence, name, seconds);
}

std::optional<float> SkeletalPlayback::SampleCurve(std::string_view name) const
{
    if (!montageSegments.empty())
    {
        const MontageSample sample = SampleMontageTimeline(time);
        return SampleCurve(name, sample.clipTime);
    }
    return SampleCurve(name, time);
}

Eigen::Vector3f SkeletalPlayback::RootMotionTotal() const
{
    if (!montageSegments.empty())
        return MontageRootDelta(PlaybackRangeStart(), PlaybackRangeEnd());
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return Eigen::Vector3f::Zero();
    const auto& clip = animation.clips.at(selected);
    // 根位移积分同样限制在播放范围：Montage 截断后不能把 section 之后的根轨迹算进来，
    // 否则 subtractRemainingRootMotion 会消费到范围外的位移。
    return RootTranslation(clip, PlaybackRangeEnd()) - RootTranslation(clip, playbackRangeStart);
}

Eigen::Vector3f SkeletalPlayback::RootMotionRemaining() const
{
    if (!montageSegments.empty())
        return MontageRootDelta(std::min(time, PlaybackRangeEnd()), PlaybackRangeEnd());
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return Eigen::Vector3f::Zero();
    const auto& clip = animation.clips.at(selected);
    return RootTranslation(clip, PlaybackRangeEnd()) -
        RootTranslation(clip, std::min(time, PlaybackRangeEnd()));
}

void SkeletalPlayback::SetPlaybackRange(float start, float end)
{
    SetPlaybackRangeInternal(start, end);
    // section 链依赖范围；直接改范围时停止旧链。多段映射是独立的时间轴身份，
    // 由 SetMontageSections 内部的 range 写入保留，不能在这里清掉。
    montageSections.clear();
    montageSection = 0;
}

void SkeletalPlayback::SetPlaybackRangeInternal(float start, float end)
{
    const float clipDuration = montageSegments.empty() ?
        ClipDuration() : montageTimelineLength;
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || !(end > start))
        throw std::invalid_argument("Playback range must be finite and increasing from a nonnegative start");
    // 播放范围不能延长资源：end 越过片段末尾时按片段末尾 clamp（Montage section 的
    // 导出误差也走这条路径），clamp 后为空的范围仍拒绝。
    const float clampedEnd = std::min(end, clipDuration);
    if (!(clampedEnd > start))
        throw std::invalid_argument("Playback range is empty inside the clip");
    // 只影响后续 tick 的播放长度、根位移积分与 notify 有效域，不重采样当前姿态；
    // 非 0 起点也不移动时钟，调用方需要随后 Seek(start) 对齐（见头文件注释）。
    playbackRangeStart = start;
    playbackRangeEnd = clampedEnd;
}

void SkeletalPlayback::SetMontageSections(std::vector<MontageSection> sections,
    std::optional<float> entryStartTime)
{
    if (sections.empty())
        throw std::invalid_argument("Montage playback requires at least one section");
    if (looping)
        throw std::invalid_argument("Montage section playback requires one-shot playback");
    // 入口时间只截短 section 0：起点不能早于作者起点（缺省 = 作者起点）。越界拦截在
    // PlayMontage 装配期完成（诊断带 montage id）；这里只保证范围本身合法。
    const float entry = std::max(sections.front().startTime,
        entryStartTime.value_or(sections.front().startTime));
    // 先按第一段装配范围（同时清掉旧链），再安装链；两段之间没有可失败操作。
    SetPlaybackRange(entry, sections.front().endTime);
    montageSections = std::move(sections);
    montageSection = 0;
}

void SkeletalPlayback::SetMontageSegments(std::vector<MontageSegment> segments,
    float length)
{
    if (segments.empty())
    {
        ClearMontageSegments();
        RebuildNotifyEvents();
        return;
    }
    if (!std::isfinite(length) || length <= 0)
        throw std::invalid_argument("Montage segment timeline length must be positive");

    std::vector<size_t> clipIndices;
    clipIndices.reserve(segments.size());
    float previousEnd = 0;
    for (size_t index = 0; index < segments.size(); ++index)
    {
        const MontageSegment& segment = segments[index];
        if (segment.derivedClipId.empty())
            throw std::invalid_argument("Montage segment '" + segment.id +
                "' has no derived clip id");
        if (!std::isfinite(segment.startTime) || !std::isfinite(segment.endTime) ||
            segment.startTime < 0 || !(segment.endTime > segment.startTime) ||
            segment.endTime > length + 1e-3f ||
            std::abs(segment.startTime - previousEnd) > 1e-3f)
            throw std::invalid_argument("Montage segment timeline is not contiguous at '" +
                segment.id + "'");
        if (!std::isfinite(segment.rateScale) || segment.rateScale <= 0 ||
            segment.rateScale > 4)
            throw std::invalid_argument("Montage segment '" + segment.id +
                "' has an invalid rateScale");
        size_t clip = 0;
        const auto& clips = asset->AnimationAsset().clips;
        while (clip < clips.size() && clips[clip].id != segment.derivedClipId)
            ++clip;
        if (clip == clips.size() || clips[clip].sequence.id != segment.derivedClipId)
            throw std::invalid_argument("Montage segment '" + segment.id +
                "' requires a loaded sidecar clip '" + segment.derivedClipId + "'");
        clipIndices.push_back(clip);
        previousEnd = segment.endTime;
    }
    if (std::abs(previousEnd - length) > 1e-3f)
        throw std::invalid_argument(
            "Montage segments must cover the complete timeline");

    montageSegments = std::move(segments);
    montageSegmentClips = std::move(clipIndices);
    montageTimelineLength = length;
    const MontageSample sample = SampleMontageTimeline(std::clamp(time, 0.0f, length));
    montageSegment = sample.segment;
    selected = sample.clip;
    RebuildNotifyEvents();
}

std::string SkeletalPlayback::MontageSectionName() const
{
    return montageSections.empty() ? std::string() :
        montageSections.at(montageSection).name;
}

std::string SkeletalPlayback::MontageSegmentId() const
{
    return montageSegments.empty() ? std::string() :
        montageSegments.at(montageSegment).id;
}

int SkeletalPlayback::MontageNextSectionIndex() const
{
    if (montageSections.empty()) return -1;
    const std::string& next = montageSections.at(montageSection).nextSection;
    if (next.empty()) return -1;
    for (size_t index = 0; index < montageSections.size(); ++index)
        if (montageSections[index].name == next) return static_cast<int>(index);
    // 未命中引用已在 PlayMontage 的 ValidateMontageExecution 拒绝；运行期不抛异常，
    // 退化为“没有下一段”的停止语义。
    return -1;
}

void SkeletalPlayback::Stop()
{
    // Stop 与暂停不同：活动 notifyState 必须补 end(interrupted)，让 C# 清理窗口状态。
    // 时钟/姿态保留，下一次 Play 会重置；暂停（SetPlaying(false)）保留活动区间继续。
    EndActiveNotifyStates(time, true, true);
    playing = false;
    explicitlyPaused = false;
    ClearBlendSpace();
    ClearCachedPose();
    ClearSyncSourceClock();
    syncDecisionValid = false;
    syncLeader.clear();
    syncPendingValid = false;
    syncPendingHasTime = false;
}

void SkeletalPlayback::SetBlendSpace1D(const std::string& clipA, const std::string& clipB,
    float weight, float rate, const std::string& syncGroup)
{
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty())
        throw std::invalid_argument("Blend space requires animation clips");
    if (!std::isfinite(weight))
        throw std::invalid_argument("Blend space weight must be finite");
    if (!std::isfinite(rate) || rate <= 0 || rate > 4)
        throw std::invalid_argument("Blend space rate must be in (0,4]");
    if (syncGroup.size() > 64 || syncGroup.find('\0') != std::string::npos)
        throw std::invalid_argument("Blend space sync group is invalid");
    if (clipB.empty() || clipB.size() > 256)
        throw std::invalid_argument("Blend space target clip is invalid");
    if (clipA != SelectedClipId())
        throw std::invalid_argument("Blend space source clip must match the selected clip");
    size_t index = 0;
    while (index < animation.clips.size() && animation.clips[index].id != clipB) ++index;
    if (index == animation.clips.size())
        throw std::invalid_argument("Unknown animation blend clip: " + clipB);
    // 同片段没有混合意义；显式清空可让调用方用同一入口退回单片段。
    if (index == selected)
    {
        ClearBlendSpace();
        return;
    }
    // 先完成全部校验再改状态；失败时保持上一帧的姿态输出。
    blendSpaceClip = index;
    blendSpaceWeight = std::clamp(weight, 0.0f, 1.0f);
    blendSpaceRate = rate;
    blendSpaceSyncGroup = syncGroup;
    blendSpaceActive = true;
}

void SkeletalPlayback::ClearBlendSpace()
{
    blendSpaceActive = false;
    blendSpaceWeight = 0;
    blendSpaceRate = 1;
    blendSpaceSyncGroup.clear();
}

std::string SkeletalPlayback::BlendSpaceClipId() const
{
    const auto& animation = asset->AnimationAsset();
    if (!blendSpaceActive || blendSpaceClip >= animation.clips.size()) return {};
    return animation.clips[blendSpaceClip].id;
}

void SkeletalPlayback::SetAdditive(const std::string& clipId, float alpha, bool loop)
{
    const auto& animation = asset->AnimationAsset();
    if (!std::isfinite(alpha))
        throw std::invalid_argument("Additive alpha must be finite");
    size_t index = 0;
    while (index < animation.clips.size() && animation.clips[index].id != clipId) ++index;
    if (index == animation.clips.size())
        throw std::invalid_argument("Unknown animation additive clip: " + clipId);
    const auto& clip = animation.clips[index];
    // 只有 LocalSpaceBase 增量能按 UE 的局部 TRS 复合执行；MeshSpace/RotationOffset
    // 需要另一套数学，显式拒绝而不是静默降级。
    if (clip.sequence.additiveAnimType != "LocalSpaceBase" &&
        clip.sequence.additiveAnimType != "RotationOffsetMeshSpace")
        throw std::invalid_argument(
            "Additive clip must be LocalSpaceBase or RotationOffsetMeshSpace: " + clipId);
    // 同一片段保持自己的时钟（权重/循环可在不重启的情况下更新）。
    if (!additiveActive || additiveClip != index)
    {
        additiveTime = 0;
        additiveClip = index;
    }
    additiveAlpha = std::clamp(alpha, 0.0f, 1.0f);
    additiveLoop = loop;
    additiveActive = true;
}

void SkeletalPlayback::ClearAdditive()
{
    additiveActive = false;
    additiveAlpha = 0;
    additiveTime = 0;
}

std::string SkeletalPlayback::AdditiveClipId() const
{
    const auto& animation = asset->AnimationAsset();
    if (!additiveActive || additiveClip >= animation.clips.size()) return {};
    return animation.clips[additiveClip].id;
}

void SkeletalPlayback::SetLayeredPose(const std::string& clipId, float alpha, bool loop,
    const std::vector<std::string>& boneMask)
{
    const auto& animation = asset->AnimationAsset();
    if (!std::isfinite(alpha))
        throw std::invalid_argument("Layered alpha must be finite");
    if (boneMask.size() > 64)
        throw std::invalid_argument("Layered bone mask exceeds 64 names");
    size_t index = 0;
    while (index < animation.clips.size() && animation.clips[index].id != clipId) ++index;
    if (index == animation.clips.size())
        throw std::invalid_argument("Unknown animation layered clip: " + clipId);
    const auto jointCount = asset->mesh.joints.size();
    std::vector<uint8_t> mask(jointCount, 0);
    if (boneMask.empty())
        std::fill(mask.begin(), mask.end(), static_cast<uint8_t>(1));
    else
    {
        for (const auto& name : boneMask)
        {
            size_t joint = 0;
            while (joint < jointCount && asset->mesh.joints[joint].name != name) ++joint;
            if (joint == jointCount)
                throw std::invalid_argument("Unknown layered bone: " + name);
            mask[joint] = 1;
        }
    }
    // 同一片段保留自己的时钟；权重/mask 可在不重启的情况下更新。
    if (!layeredActive || layeredClip != index)
    {
        layeredTime = 0;
        layeredClip = index;
    }
    layeredAlpha = std::clamp(alpha, 0.0f, 1.0f);
    layeredLoop = loop;
    layeredMask = std::move(mask);
    layeredMaskBones = boneMask.empty() ? jointCount : boneMask.size();
    layeredWeights.clear();
    layeredActive = true;
}

void SkeletalPlayback::SetLayeredPoseWeights(const std::string& clipId, float alpha, bool loop,
    const std::vector<BlendProfileBone>& boneWeights)
{
    const auto& animation = asset->AnimationAsset();
    if (!std::isfinite(alpha))
        throw std::invalid_argument("Layered alpha must be finite");
    if (boneWeights.size() > 256)
        throw std::invalid_argument("Layered blend profile exceeds 256 bones");
    size_t index = 0;
    while (index < animation.clips.size() && animation.clips[index].id != clipId) ++index;
    if (index == animation.clips.size())
        throw std::invalid_argument("Unknown animation layered clip: " + clipId);
    const auto jointCount = asset->mesh.joints.size();
    // VL 白名单语义：未列出的骨骼权重 0（UE BlendProfile 未列出按 1.0；这里更接近
    // mask，避免局部动作意外覆盖全身）。契约写明这一有意差异。
    std::vector<float> weights(jointCount, 0.0f);
    for (const BlendProfileBone& bone : boneWeights)
    {
        if (!std::isfinite(bone.weight))
            throw std::invalid_argument("Layered blend profile weight must be finite");
        size_t joint = 0;
        while (joint < jointCount && asset->mesh.joints[joint].name != bone.name) ++joint;
        if (joint == jointCount)
            throw std::invalid_argument("Unknown layered bone: " + bone.name);
        weights[joint] = std::clamp(bone.weight, 0.0f, 1.0f);
    }
    if (!layeredActive || layeredClip != index)
    {
        layeredTime = 0;
        layeredClip = index;
    }
    layeredAlpha = std::clamp(alpha, 0.0f, 1.0f);
    layeredLoop = loop;
    layeredMask.clear();
    layeredMaskBones = boneWeights.size();
    layeredWeights = std::move(weights);
    layeredActive = true;
}

void SkeletalPlayback::ClearLayeredPose()
{
    layeredActive = false;
    layeredAlpha = 0;
    layeredTime = 0;
    layeredMask.clear();
    layeredMaskBones = 0;
    layeredWeights.clear();
}

std::string SkeletalPlayback::LayeredClipId() const
{
    const auto& animation = asset->AnimationAsset();
    if (!layeredActive || layeredClip >= animation.clips.size()) return {};
    return animation.clips[layeredClip].id;
}

void SkeletalPlayback::PlaySlotMontage(const std::string& clipId, float alpha, bool loop,
    float rate, const std::vector<std::string>& boneMask)
{
    const auto& animation = asset->AnimationAsset();
    if (!std::isfinite(alpha) || !std::isfinite(rate) || rate <= 0 || rate > 4)
        throw std::invalid_argument("Slot montage alpha/rate is invalid");
    if (boneMask.size() > 64)
        throw std::invalid_argument("Slot montage bone mask exceeds 64 names");
    size_t index = 0;
    while (index < animation.clips.size() && animation.clips[index].id != clipId) ++index;
    if (index == animation.clips.size())
        throw std::invalid_argument("Unknown slot montage clip: " + clipId);
    const auto jointCount = asset->mesh.joints.size();
    std::vector<uint8_t> mask(jointCount, 0);
    if (boneMask.empty())
        std::fill(mask.begin(), mask.end(), static_cast<uint8_t>(1));
    else
        for (const auto& name : boneMask)
        {
            size_t joint = 0;
            while (joint < jointCount && asset->mesh.joints[joint].name != name) ++joint;
            if (joint == jointCount)
                throw std::invalid_argument("Unknown slot montage bone: " + name);
            mask[joint] = 1;
        }
    slotMontageClip = index;
    slotMontageAlpha = std::clamp(alpha, 0.0f, 1.0f);
    slotMontageLoop = loop;
    slotMontageRate = rate;
    slotMontageTime = 0;
    slotMontageCompleted = false;
    slotMontageMask = std::move(mask);
    slotMontageMaskBones = boneMask.empty() ? jointCount : boneMask.size();
    slotMontageWeights.clear();
    // 换 overlay 片段前结束上一段的 notifyState，再按新片段重建事件表。
    EndActiveSlotNotifyStates(slotMontageTime, true);
    slotMontageActive = true;
    RebuildSlotNotifyEvents();
}

void SkeletalPlayback::PlaySlotMontageWeights(const std::string& clipId, float alpha, bool loop,
    float rate, const std::vector<BlendProfileBone>& boneWeights)
{
    const auto& animation = asset->AnimationAsset();
    if (!std::isfinite(alpha) || !std::isfinite(rate) || rate <= 0 || rate > 4)
        throw std::invalid_argument("Slot montage alpha/rate is invalid");
    if (boneWeights.size() > 256)
        throw std::invalid_argument("Slot montage blend profile exceeds 256 bones");
    size_t index = 0;
    while (index < animation.clips.size() && animation.clips[index].id != clipId) ++index;
    if (index == animation.clips.size())
        throw std::invalid_argument("Unknown slot montage clip: " + clipId);
    const auto jointCount = asset->mesh.joints.size();
    std::vector<float> weights(jointCount, 0.0f);
    for (const BlendProfileBone& bone : boneWeights)
    {
        if (!std::isfinite(bone.weight))
            throw std::invalid_argument("Slot montage blend profile weight must be finite");
        size_t joint = 0;
        while (joint < jointCount && asset->mesh.joints[joint].name != bone.name) ++joint;
        if (joint == jointCount)
            throw std::invalid_argument("Unknown slot montage bone: " + bone.name);
        weights[joint] = std::clamp(bone.weight, 0.0f, 1.0f);
    }
    slotMontageClip = index;
    slotMontageAlpha = std::clamp(alpha, 0.0f, 1.0f);
    slotMontageLoop = loop;
    slotMontageRate = rate;
    slotMontageTime = 0;
    slotMontageCompleted = false;
    slotMontageMask.clear();
    slotMontageMaskBones = boneWeights.size();
    slotMontageWeights = std::move(weights);
    EndActiveSlotNotifyStates(slotMontageTime, true);
    slotMontageActive = true;
    RebuildSlotNotifyEvents();
}

void SkeletalPlayback::StopSlotMontage()
{
    EndActiveSlotNotifyStates(slotMontageTime, true);
    slotMontageActive = false;
    slotMontageAlpha = 0;
    slotMontageTime = 0;
    slotMontageCompleted = false;
    slotMontageMask.clear();
    slotMontageMaskBones = 0;
    slotMontageWeights.clear();
    slotNotifyEvents.clear();
    slotNotifyActive.clear();
}

std::string SkeletalPlayback::SlotMontageClipId() const
{
    const auto& animation = asset->AnimationAsset();
    if (!slotMontageActive || slotMontageClip >= animation.clips.size()) return {};
    return animation.clips[slotMontageClip].id;
}

void SkeletalPlayback::CaptureCachedPose(const std::string& name)
{
    if (name.empty() || name.size() > 128)
        throw std::invalid_argument("Cached pose name is invalid");
    // 首帧尚未求值：UE 的 SaveCachedPose 也只在有 pose 后才有内容；静默跳过，
    // 不能让一条捕获命令把同一批播放命令整体作废。
    if (displayedLocals.empty())
        return;
    // 缓存态下 displayedLocals 已包含 additive；若继续写回缓存，下一帧会把它
    // 当成 Base 再次叠加（缩放/位移会逐帧累积）。Land 期间缓存必须保持冻结。
    if (useCachedPoseActive)
        return;
    cachedPoses[name] = displayedLocals;
}

void SkeletalPlayback::UseCachedPose(const std::string& name, float blendSeconds)
{
    if (name.empty() || name.size() > 128)
        throw std::invalid_argument("Cached pose name is invalid");
    if (!std::isfinite(blendSeconds) || blendSeconds < 0 || blendSeconds > 10)
        throw std::invalid_argument("Cached pose blend must be in [0,10]");
    // 宿主每帧都会重复提交 UseCachedPose；同一状态只允许初始化一次混合。
    if (useCachedPoseActive && useCachedPoseName == name) return;
    const bool isBranch = std::any_of(cachedPoseBranches.begin(), cachedPoseBranches.end(),
        [&](const CachedPoseBranchState& branch) { return branch.definition.name == name; });
    if (!isBranch && cachedPoses.find(name) == cachedPoses.end())
        throw std::invalid_argument("Unknown cached pose: " + name);
    useCachedPoseName = name;
    useCachedPoseActive = true;
    // 进入缓存姿态（如 Fall→Land）必须能把当前姿态作为混合源，否则会硬切。
    if (blendSeconds > 0 && !displayedLocals.empty())
    {
        blendSource = displayedLocals;
        blendDuration = blendSeconds;
        blendElapsed = 0;
        inertialization.Clear();
    }
    else
    {
        blendSource.clear();
        inertialization.Clear();
    }
}

void SkeletalPlayback::ClearCachedPose()
{
    useCachedPoseActive = false;
    useCachedPoseName.clear();
}

void SkeletalPlayback::SetCachedPoseBranches(std::vector<CachedPoseBranch> branches)
{
    const auto& animation = asset->AnimationAsset();
    std::vector<CachedPoseBranchState> next;
    next.reserve(branches.size());
    std::set<std::string> names;
    for (CachedPoseBranch& branch : branches)
    {
        if (branch.name.empty() || branch.name.size() > 128)
            throw std::invalid_argument("Cached pose branch name is invalid");
        if (!names.insert(branch.name).second)
            throw std::invalid_argument("Duplicate cached pose branch: " + branch.name);
        if (branch.samples.empty() || branch.samples.size() > 3)
            throw std::invalid_argument(
                "Cached pose branch requires 1..3 samples: " + branch.name);
        if (!std::isfinite(branch.rate) || branch.rate <= 0 || branch.rate > 4)
            throw std::invalid_argument("Cached pose branch rate is invalid: " + branch.name);
        if (!std::isfinite(branch.idleThreshold) || branch.idleThreshold < 0)
            throw std::invalid_argument("Cached pose branch idle threshold is invalid: " + branch.name);
        CachedPoseBranchState state;
        state.definition = std::move(branch);
        if (!state.definition.idleClip.empty())
        {
            size_t idle = 0;
            while (idle < animation.clips.size() &&
                animation.clips[idle].id != state.definition.idleClip)
                ++idle;
            if (idle == animation.clips.size())
                throw std::invalid_argument(
                    "Unknown cached pose branch idle clip: " + state.definition.idleClip);
            state.idleClipIndex = idle;
        }
        for (const CachedPoseBranchSample& sample : state.definition.samples)
        {
            size_t index = 0;
            while (index < animation.clips.size() && animation.clips[index].id != sample.clip)
                ++index;
            if (index == animation.clips.size())
                throw std::invalid_argument(
                    "Unknown cached pose branch clip: " + sample.clip);
            state.clipIndices.push_back(index);
        }
        for (size_t index = 1; index < state.definition.samples.size(); ++index)
            if (!(state.definition.samples[index].threshold >
                state.definition.samples[index - 1].threshold))
                throw std::invalid_argument(
                    "Cached pose branch thresholds must increase: " + state.definition.name);
        next.push_back(std::move(state));
    }
    cachedPoseBranches = std::move(next);
    frameCachedPoses.clear();
}

void SkeletalPlayback::ClearCachedPoseBranches()
{
    cachedPoseBranches.clear();
    frameCachedPoses.clear();
}

void SkeletalPlayback::SetCachedPoseBranchAxis(const std::string& name, float value)
{
    if (name.empty() || name.size() > 128)
        throw std::invalid_argument("Cached pose branch name is invalid");
    if (!std::isfinite(value))
        throw std::invalid_argument("Cached pose branch axis must be finite");
    cachedPoseBranchAxes[name] = value;
}

LocalPose SkeletalPlayback::Sample(size_t clipIndex, float sampleTime) const
{
    const auto rest = asset->mesh.runtime->joint_rest_poses();
    const auto& animation = asset->AnimationAsset();
    LocalPose locals;
    if (animation.clips.empty())
    {
        locals = LocalPose(rest.begin(), rest.end());
    }
    else
    {
        locals = SampleLocals(animation.mesh, animation.clips.at(clipIndex), sampleTime);
        // 先在 source skeleton 上采样，再转换模型空间旋转增量；普通资产复用
        // 原采样结果，target 骨长/未映射骨骼不被动作源的比例污染。
        if (compiledRetarget)
            locals = compiledRetarget->Apply(locals);
    }
    if (inPlace)
    {
        // 根是唯一的骨架入口；只替换 lane 0 的完整绑定 TRS，等价于旧模型空间
        // restRoot * inverse(sampleRoot) 校正，并保留 pelvis 相对运动。混合前移除，
        // 避免原根位移/缩放污染交叉混合；Actor/Jolt 始终独占胶囊位移。
        auto& root = locals.front();
        const auto& bind = rest.front();
        root.translation.x = ozz::math::SetX(root.translation.x, bind.translation.x);
        root.translation.y = ozz::math::SetX(root.translation.y, bind.translation.y);
        root.translation.z = ozz::math::SetX(root.translation.z, bind.translation.z);
        root.rotation.x = ozz::math::SetX(root.rotation.x, bind.rotation.x);
        root.rotation.y = ozz::math::SetX(root.rotation.y, bind.rotation.y);
        root.rotation.z = ozz::math::SetX(root.rotation.z, bind.rotation.z);
        root.rotation.w = ozz::math::SetX(root.rotation.w, bind.rotation.w);
        root.scale.x = ozz::math::SetX(root.scale.x, bind.scale.x);
        root.scale.y = ozz::math::SetX(root.scale.y, bind.scale.y);
        root.scale.z = ozz::math::SetX(root.scale.z, bind.scale.z);
    }
    return locals;
}

LocalPose SkeletalPlayback::PreviewPose(float aheadSeconds) const
{
    // 缓存姿态没有自己的片段时钟：离开 Land 时若仍按旧片段采样，会把“缓存姿态 vs
    // 旧片段”的差异当成巨大速度，惯性化导致落地后骨骼单帧乱飞。
    if (useCachedPoseActive) return displayedLocals;
    if (!playing || Duration() == 0) return displayedLocals;
    const float rangeStart = PlaybackRangeStart();
    const float rangeEnd = PlaybackRangeEnd();
    const double next = double(time) + double(aheadSeconds) * rate;
    float sampleTime;
    if (looping && next >= rangeEnd)
    {
        // 预览与 Tick 用同一个循环域 [rangeStart,rangeEnd)：回绕点不是片段的 0。
        sampleTime = float(double(rangeStart) +
            std::fmod(next - double(rangeStart), double(rangeEnd - rangeStart)));
    }
    else sampleTime = float(std::min(next, double(rangeEnd)));
    size_t sampleClip = selected;
    float clipSampleTime = sampleTime;
    if (!montageSegments.empty())
    {
        const MontageSample sample = SampleMontageTimeline(sampleTime, sampleTime >= rangeEnd);
        sampleClip = sample.clip;
        clipSampleTime = sample.clipTime;
    }
    auto locals = Sample(sampleClip, clipSampleTime);
    // 惯性化源速度必须沿“实际输出姿态曲线”估计；BlendSpace 的 B 片段也属于该曲线，
    // 否则从 Walk/Run 混合态停下时会把 A/B 差异误当成巨大速度，导致骨骼单帧乱飞。
    if (blendSpaceActive && blendSpaceClip < asset->AnimationAsset().clips.size())
    {
        const auto& blendClip = asset->AnimationAsset().clips[blendSpaceClip].runtime;
        const float phase = rangeEnd > rangeStart
            ? std::clamp((sampleTime - rangeStart) / (rangeEnd - rangeStart), 0.0f, 1.0f)
            : 0.0f;
        const float blendDurationSeconds = blendClip ? blendClip->duration() : 0.0f;
        float blendTime = phase * blendDurationSeconds;
        if (!looping)
            blendTime = std::min(blendTime, std::max(0.0f, blendDurationSeconds - 1e-4f));
        LocalPose blendPose = Sample(blendSpaceClip, blendTime);
        if (blendSpaceWeight >= 1.0f) locals = std::move(blendPose);
        else if (blendSpaceWeight > 0.0f)
            locals = Crossfade(locals, std::move(blendPose), blendSpaceWeight, asset->mesh);
    }
    const float elapsed = std::min(blendElapsed + aheadSeconds, blendDuration);
    if (!inertialization.Empty() && elapsed < blendDuration)
    {
        const auto target = ReadKinematics(locals, asset->mesh.joints.size());
        locals = WriteKinematics(std::move(locals), inertialization.Evaluate(target, elapsed));
    }
    else if (!blendSource.empty() && elapsed < blendDuration)
        locals = Crossfade(blendSource, std::move(locals), elapsed / blendDuration, asset->mesh);
    return locals;
}

void SkeletalPlayback::TransitionTo(const std::string& clipId, float seconds, bool loop,
    float blendSeconds, bool removeRoot)
{
    size_t index = 0;
    const auto& animation = asset->AnimationAsset();
    while (index < animation.clips.size() && animation.clips[index].id != clipId) ++index;
    if (index == animation.clips.size()) throw std::invalid_argument("Unknown animation clip: " + clipId);
    if (!std::isfinite(seconds) || seconds < 0 || seconds > animation.clips[index].runtime->duration())
        throw std::invalid_argument("Transition time must be inside the clip");
    if (!std::isfinite(blendSeconds) || blendSeconds < 0)
        throw std::invalid_argument("Transition duration must be finite and nonnegative");
    // 首帧尚无 displayed 姿态：任何混合/速度估计都没有源，退化为硬切。
    if (displayedLocals.empty()) blendSeconds = 0;
    // 根消费策略属于位移所有权边界。平滑过渡不能同时保证首帧连续和立即移除旧根，
    // 因而策略切换必须由调用方显式选零时长硬切；普通 MM 的 in-place 策略保持不变。
    if (blendSeconds > 0 && removeRoot != inPlace)
        throw std::invalid_argument("Changing root policy requires a zero-duration transition");
    SkeletalPlayback candidate(*this);
    const float previousTime = time;
    candidate.selected = index;
    candidate.time = seconds;
    candidate.inPlace = removeRoot;
    candidate.looping = loop;
    candidate.rate = 1;
    candidate.requestedRate = 1;
    candidate.playing = true;
    candidate.explicitlyPaused = false;
    // 过渡开启新的播放段：旧 Montage 的播放范围/notify 覆盖不能带到新片段，
    // 否则 MM transition 会继承被 Montage section 截短的 Duration。
    candidate.playbackRangeStart = 0;
    candidate.playbackRangeEnd = 0;
    candidate.montageNotifies.clear();
    candidate.montageId.clear();
    candidate.montageLength = 0;
    candidate.montageSections.clear();
    candidate.montageSection = 0;
    candidate.ClearMontageSegments();
    candidate.blendElapsed = 0;
    candidate.blendDuration = blendSeconds;
    // 过渡开启新的目标片段；旧的混合源属于上一播放段，不能带到新时钟上。
    candidate.ClearBlendSpace();
    candidate.ClearCachedPose();
    auto target = candidate.Sample(index, seconds);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> targetModels;
    auto targetPoses = candidate.Evaluate(target, discontinuity + (blendSeconds == 0), targetModels);
    if (blendSeconds == 0)
    {
        candidate.blendSource.clear();
        candidate.inertialization.Clear();
        candidate.displayedLocals.swap(target);
        candidate.poses.swap(targetPoses);
        candidate.jointModels.swap(targetModels);
        candidate.preIkModels = candidate.jointModels;
        candidate.motionModels = candidate.jointModels;
        candidate.visualLocals.clear();
        candidate.slopeWarpedLocals.clear();
        candidate.footPlacementSource.reset();
        candidate.rootOffset.Reset("hard-cut");
        candidate.ResetFootPlacement("hard-cut");
        ++candidate.discontinuity;
    }
    else
    {
        // 首次求值 alpha=0，完全保留已发布的姿态。任意次数中断仍只有一份冻结源，
        // 正常混合不改变 epoch，使当帧 post-blend 姿态继续参与速度历史。
        if (inertializationEnabled)
        {
            // 速度只在过渡边界前看 1ms 的同一条姿态曲线，不推进第二个动画时钟。
            // ozz 压缩采样的导数为数值估计；有限 Hermite 对该估计满足 C1，不承诺任意源 C2。
            constexpr float step = 0.001f;
            // 一次性片段播完时 playing=false，PreviewPose 会原样返回当前姿态，测出的速度是 0；
            // 惯性化于是以零速度起步（整个过渡先“停一下”再加速），只在切换瞬间表现为一次顿挫。
            // 这种情况回看一小步取片段自身的出帧速度：此时 displayedLocals 就是 rangeEnd 姿态，
            // Completed() 又要求混合已结束，所以两者是同一条未混合曲线。
            const float sourceFrom = std::max(PlaybackRangeStart(), time - step);
            const float sourceDelta = time - sourceFrom;
            std::vector<JointKinematics> sourceMotion;
            // 只有“播完停在末尾”才回看；显式 Pause 停在片段中间时速度必须是 0。
            if (!playing && !explicitlyPaused && !looping && sourceDelta > 1e-5f)
                sourceMotion = MeasureKinematics(Sample(selected, sourceFrom), displayedLocals,
                    asset->mesh.joints.size(), sourceDelta);
            else
                sourceMotion = MeasureKinematics(displayedLocals, PreviewPose(step),
                    asset->mesh.joints.size(), step);
            const auto targetMotion = MeasureKinematics(target, candidate.Sample(index,
                loop ? float(std::fmod(double(seconds) + step, candidate.Duration())) :
                    std::min(seconds + step, candidate.Duration())), asset->mesh.joints.size(), step);
            candidate.inertialization.Begin(sourceMotion, targetMotion, blendSeconds);
            candidate.blendSource.clear();
        }
        else
        {
            candidate.inertialization.Clear();
            candidate.blendSource = displayedLocals;
        }
    }
    // 过渡源保留一份只读影子相位时钟：follower 持续对齐需要知道旧 leader 的
    // 归一化时间，但这里不复制姿态播放器，也不产生第二份 notify/根轨迹。
    if (blendSeconds > 0)
    {
        candidate.syncSourceValid = true;
        candidate.syncSourceSelected = selected;
        candidate.syncSourceTime = time;
        candidate.syncSourceRangeStart = PlaybackRangeStart();
        candidate.syncSourceRangeEnd = PlaybackRangeEnd();
        candidate.syncSourceRate = rate;
        candidate.syncSourceLooping = looping;
    }
    else candidate.ClearSyncSourceClock();
    candidate.syncDecisionValid = false;
    candidate.syncLeader.clear();
    candidate.syncPendingValid = false;
    candidate.syncPendingHasTime = false;
    // 过渡是时钟跳变：先结束旧播放段的活动区间（interrupted），再按新片段重建事件表；
    // 新片段不补发目标位置之前的历史（与 seek 的“硬跳变不重放”语义一致）。
    candidate.EndActiveNotifyStates(previousTime, true, true);
    candidate.RebuildNotifyEvents();
    if (candidate.Completed()) candidate.playing = false;
    *this = std::move(candidate);
    activeSyncGroup.clear();
    RefreshSyncMarker();
}

void SkeletalPlayback::TransitionToSynced(const std::string& clipId, float seconds, bool loop,
    float blendSeconds, bool removeRoot, const std::string& syncGroup, float syncPhase)
{
    float resolvedSeconds = seconds;
    std::string matchedGroup;
    if (!syncGroup.empty() && std::isfinite(syncPhase) && syncPhase >= 0)
    {
        const auto& clips = asset->AnimationAsset().clips;
        size_t index = 0;
        while (index < clips.size() && clips[index].id != clipId) ++index;
        if (index < clips.size())
        {
            const SequenceData& sequence = clips[index].sequence;
            for (const PlayerBinding& player : sequence.players)
            {
                if (player.syncGroup != syncGroup ||
                    player.syncMethod == SyncMethod::DoNotSync ||
                    player.syncMethod == SyncMethod::Graph) continue;
                matchedGroup = syncGroup;
                const float duration = clips[index].runtime->duration();
                resolvedSeconds = std::clamp(syncPhase, 0.0f, 1.0f) * duration;
                if (loop)
                    resolvedSeconds = duration > 0 ? std::fmod(resolvedSeconds, duration) : 0;
                else
                    resolvedSeconds = std::min(resolvedSeconds, std::max(0.0f, duration - 1e-4f));
                break;
            }
        }
    }
    TransitionTo(clipId, resolvedSeconds, loop, blendSeconds, removeRoot);
    activeSyncGroup = std::move(matchedGroup);
    RefreshSyncMarker();
}

void SkeletalPlayback::Play(const std::string& clipId, bool loop, float speed, bool removeRoot)
{
    if (!std::isfinite(speed) || speed <= 0 || speed > 4)
        throw std::invalid_argument("Animation rate must be in (0,4]");
    size_t index = 0;
    const auto& animation = asset->AnimationAsset();
    while (index < animation.clips.size() && animation.clips[index].id != clipId) ++index;
    if (index == animation.clips.size()) throw std::invalid_argument("Unknown animation clip: " + clipId);
    // 请求先在候选值上完成全部采样，失败不改变活动时钟/根策略。
    SkeletalPlayback candidate(*this);
    candidate.inPlace = removeRoot;
    candidate.looping = loop;
    candidate.rate = speed;
    candidate.requestedRate = speed;
    candidate.playing = true;
    candidate.explicitlyPaused = false;
    candidate.inertializationEnabled = false;
    candidate.SetRootOffsetEnabled(false);
    candidate.Select(index);
    *this = std::move(candidate);
}

void SkeletalPlayback::Seek(float seconds)
{
    if (!std::isfinite(seconds) || seconds < PlaybackRangeStart() || seconds > PlaybackRangeEnd())
        throw std::invalid_argument("Seek must be inside the playback range");
    size_t sampleClip = selected;
    float clipTime = seconds;
    size_t segmentIndex = montageSegment;
    if (!montageSegments.empty())
    {
        const MontageSample sample = SampleMontageTimeline(seconds, seconds >= PlaybackRangeEnd());
        sampleClip = sample.clip;
        clipTime = sample.clipTime;
        segmentIndex = sample.segment;
    }
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto locals = Sample(sampleClip, clipTime);
    auto next = Evaluate(locals, discontinuity + 1, nextModels);
    // 显式 seek 是硬跳变：清空活动区间且不补发被跳过的 begin/end（与 UE 的 seek 语义一致）。
    // 事件缓冲也一并丢弃，下一 Tick 只处理 seconds 之后的位置穿越。
    std::fill(notifyActive.begin(), notifyActive.end(), 0);
    RefreshActiveNotifyStates();
    firedNotifies.clear();
    pendingNotifies.clear();
    poses.swap(next);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    blendSource.clear();
    inertialization.Clear();
    ClearBlendSpace();
    ClearCachedPose();
    time = seconds;
    selected = sampleClip;
    montageSegment = segmentIndex;
    activeSyncGroup.clear();
    ClearSyncSourceClock();
    syncDecisionValid = false;
    syncLeader.clear();
    syncPendingValid = false;
    syncPendingHasTime = false;
    RefreshSyncMarker();
    ++discontinuity;
    rootOffset.Reset("seek");
    ResetFootPlacement("seek");
}

void SkeletalPlayback::Select(size_t clipIndex)
{
    if (clipIndex >= asset->AnimationAsset().clips.size())
        throw std::out_of_range("Unknown animation clip");
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto locals = Sample(clipIndex, 0);
    auto next = Evaluate(locals, discontinuity + 1, nextModels);
    // 切片段：旧片段的活动 notifyState 补 end(interrupted)，事件延到下一 Tick 被反馈读取。
    // Montage 覆盖层、section 链与播放范围都属于上一播放段，随 Select 复位到整段片段。
    EndActiveNotifyStates(time, true, true);
    montageNotifies.clear();
    montageId.clear();
    montageLength = 0;
    montageSections.clear();
    montageSection = 0;
    ClearMontageSegments();
    playbackRangeStart = 0;
    playbackRangeEnd = 0;
    poses.swap(next);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    blendSource.clear();
    inertialization.Clear();
    ClearBlendSpace();
    ClearCachedPose();
    selected = clipIndex;
    time = 0;
    activeSyncGroup.clear();
    ClearSyncSourceClock();
    syncDecisionValid = false;
    syncLeader.clear();
    syncPendingValid = false;
    syncPendingHasTime = false;
    RefreshSyncMarker();
    ++discontinuity;
    rootOffset.Reset("select");
    ResetFootPlacement("select");
    RebuildNotifyEvents();
}
}
