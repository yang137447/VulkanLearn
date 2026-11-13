#include "skeletalPlaybackInternal.h"

#include <algorithm>
#include <cmath>
#include <optional>
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
    ClearSyncSourceClock();
    syncDecisionValid = false;
    syncLeader.clear();
    syncPendingValid = false;
    syncPendingHasTime = false;
}

void SkeletalPlayback::SetBlendSpace1D(const std::string& clipA, const std::string& clipB, float weight)
{
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty())
        throw std::invalid_argument("Blend space requires animation clips");
    if (!std::isfinite(weight))
        throw std::invalid_argument("Blend space weight must be finite");
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
    blendSpaceActive = true;
}

void SkeletalPlayback::ClearBlendSpace()
{
    blendSpaceActive = false;
    blendSpaceWeight = 0;
}

std::string SkeletalPlayback::BlendSpaceClipId() const
{
    const auto& animation = asset->AnimationAsset();
    if (!blendSpaceActive || blendSpaceClip >= animation.clips.size()) return {};
    return animation.clips[blendSpaceClip].id;
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
