#include "skeletalPlaybackInternal.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace VL::Animation
{
void SkeletalPlayback::RebuildNotifyEvents()
{
    notifyEvents.clear();
    const auto& animation = asset->AnimationAsset();
    if (!montageSegments.empty())
    {
        // 多段资产把每段 sidecar 的 notify 映射到 Montage 时间轴。这样切 clip 时
        // 事件表不需要重建，段边界也不会因为 clip 本地时间归零而重复/漏发。
        for (size_t segmentIndex = 0; segmentIndex < montageSegments.size(); ++segmentIndex)
        {
            const MontageSegment& segment = montageSegments[segmentIndex];
            const Clip& clip = animation.clips.at(montageSegmentClips.at(segmentIndex));
            const float clipDuration = clip.runtime->duration();
            const float clipStart = std::clamp(segment.clipStartTime, 0.0f,
                std::max(0.0f, clipDuration));
            const float clipEnd = std::max(clipStart, std::min(
                segment.clipEndTime > 0 ? segment.clipEndTime : clipDuration,
                std::max(clipStart, clipDuration)));
            const float sourceSpan = clipEnd - clipStart;
            const float timelineSpan = segment.endTime - segment.startTime;
            const float rateScale = segment.rateScale;
            const size_t loops = segment.loop && sourceSpan > 1e-5f && rateScale > 0 ?
                static_cast<size_t>(std::ceil(timelineSpan * rateScale / sourceSpan)) : 1;
            // 段内循环只重复 sidecar 事件；上限防止恶意短 clip 生成无限表。
            for (size_t loop = 0; loop < std::min<size_t>(loops, 64); ++loop)
            {
                const float loopOffset = static_cast<float>(loop) * sourceSpan / rateScale;
                for (const auto& notify : clip.sequence.notifies)
                {
                    const float sourceTime = notify.time;
                    if (sourceTime < clipStart - 1e-5f ||
                        sourceTime > clipEnd + 1e-5f)
                        continue;
                    NotifyEvent event;
                    event.name = notify.name;
                    event.track = notify.track;
                    event.kind = notify.duration > 0 ? "notifyState" : "notify";
                    event.time = segment.startTime + loopOffset +
                        (sourceTime - clipStart) / rateScale;
                    event.duration = notify.duration / rateScale;
                    if (event.time < segment.startTime - 1e-5f ||
                        event.time > segment.endTime + 1e-5f)
                        continue;
                    event.segmentIndex = static_cast<int>(segmentIndex);
                    event.segmentId = segment.id;
                    notifyEvents.push_back(std::move(event));
                }
            }
        }
    }
    else if (!animation.clips.empty())
    {
        // Sequence notify 没有 kind 字段：duration==0 是 UE 瞬时 notify，duration>0 是
        // notifyState。time/duration 已经由解析器保留作者原值（可越过片段末尾），这里不裁剪。
        for (const auto& notify : animation.clips.at(selected).sequence.notifies)
        {
            NotifyEvent event;
            event.name = notify.name;
            event.track = notify.track;
            event.kind = notify.duration > 0 ? "notifyState" : "notify";
            event.time = notify.time;
            event.duration = notify.duration;
            notifyEvents.push_back(std::move(event));
        }
    }
    // Montage JSON 与 sidecar 是两份数据：Montage 提供 warp/PoseSearch 等 notifyState，
    // sidecar 提供 Footstep 等序列级瞬时 notify；播放 Montage 时两者都要执行，不能互相覆盖。
    for (const auto& notify : montageNotifies)
    {
        NotifyEvent event;
        event.name = notify.name;
        event.track = notify.track;
        // Montage 的 notify 即使带 duration 也不执行区间；notifyState 只有 duration>0 才有区间语义。
        event.kind = notify.kind == "notifyState" && notify.duration > 0 ? "notifyState" : "notify";
        event.time = notify.time;
        event.duration = event.kind == "notifyState" ? notify.duration : 0.0f;
        if (!montageSegments.empty())
        {
            const MontageSample sample = SampleMontageTimeline(notify.time);
            event.segmentIndex = static_cast<int>(sample.segment);
            event.segmentId = montageSegments.at(sample.segment).id;
        }
        notifyEvents.push_back(std::move(event));
    }
    // 运行时事件表按时间排序；同刻事件用 stable_sort 保留作者顺序（Sequence 在前）。
    std::stable_sort(notifyEvents.begin(), notifyEvents.end(),
        [](const NotifyEvent& left, const NotifyEvent& right) { return left.time < right.time; });
    notifyActive.assign(notifyEvents.size(), 0);
    RefreshActiveNotifyStates();
}

void SkeletalPlayback::RefreshActiveNotifyStates()
{
    activeNotifyStates.clear();
    for (size_t index = 0; index < notifyEvents.size(); ++index)
    {
        if (!notifyActive[index]) continue;
        ActiveNotifyState state;
        state.name = notifyEvents[index].name;
        state.track = notifyEvents[index].track;
        state.startTime = notifyEvents[index].time;
        state.endTime = notifyEvents[index].EndTime();
        activeNotifyStates.push_back(std::move(state));
    }
}

void SkeletalPlayback::PushFired(const NotifyEvent& notify, const char* phase, float eventTime,
    bool interrupted)
{
    FiredNotify event;
    event.name = notify.name;
    event.track = notify.track;
    event.kind = notify.kind;
    event.phase = phase;
    event.time = eventTime;
    // startTime/duration 始终是作者原值，不因播放范围裁剪而改写；回调方据此还原完整区间。
    event.startTime = notify.time;
    event.duration = notify.duration;
    event.interrupted = interrupted;
    event.segmentIndex = notify.segmentIndex;
    event.segmentId = notify.segmentId;
    firedNotifies.push_back(std::move(event));
}

void SkeletalPlayback::ScanNotifySegment(double from, double to, bool includeTerminalEnd)
{
    if (!(to >= from)) return;
    struct Point
    {
        double time = 0;
        size_t index = 0;
        bool end = false;
    };
    std::vector<Point> points;
    points.reserve(notifyEvents.size() * 2);
    for (size_t index = 0; index < notifyEvents.size(); ++index)
    {
        const NotifyEvent& notify = notifyEvents[index];
        const double start = static_cast<double>(notify.time);
        if (!notify.IsState())
        {
            // [from,to) 半开：正好落在当前时刻的事件留给下一 tick，避免两 tick 重复触发；
            // 一次性播放到达末尾的那一 tick 例外，允许 time==to 的瞬时 notify。
            // 循环尾部以 rangeEnd 为上界（to 不含），因此 time==rangeEnd 的瞬时 notify
            // 在循环播放中不触发；需要它在最后一次循环结束时补发时用一次性播放。
            const bool inSegment = start >= from &&
                (start < to || (includeTerminalEnd && start <= to));
            if (inSegment) points.push_back({start, index, false});
            continue;
        }
        // notifyState 的 begin：区间起点进入扫描窗口。terminal 上不开启新区间，
        // 否则播放范围末尾会补出一个立刻中断的零长度状态。
        const bool beginsHere = start >= from && start < to;
        if (beginsHere) points.push_back({start, index, false});
        const double end = static_cast<double>(notify.EndTime());
        // end 含上界：到达作者 EndTime 的那一 tick 结束，不需要再等下一 tick。
        // 若“起点和终点同一 tick 内”（大 delta 跨过整个区间），begin/end 都会进点表，
        // 按时间排序后依次执行，回调方仍能看到完整的 begin→end。
        if ((notifyActive[index] || beginsHere) && end <= to)
            points.push_back({std::max(end, from), index, true});
    }
    std::stable_sort(points.begin(), points.end(),
        [](const Point& left, const Point& right) { return left.time < right.time; });
    for (const Point& point : points)
    {
        const NotifyEvent& notify = notifyEvents[point.index];
        if (point.end)
        {
            if (!notifyActive[point.index]) continue;
            notifyActive[point.index] = 0;
            PushFired(notify, "end", static_cast<float>(point.time), false);
        }
        else if (!notify.IsState())
        {
            PushFired(notify, "instant", static_cast<float>(point.time), false);
        }
        else if (!notifyActive[point.index])
        {
            notifyActive[point.index] = 1;
            PushFired(notify, "begin", static_cast<float>(point.time), false);
        }
    }
}

void SkeletalPlayback::EndActiveNotifyStates(float atTime, bool interrupted, bool deferToNextTick)
{
    for (size_t index = 0; index < notifyEvents.size(); ++index)
    {
        if (!notifyActive[index]) continue;
        notifyActive[index] = 0;
        PushFired(notifyEvents[index], "end", atTime, interrupted);
        // 控制面事件可能在本帧 Tick 之前产生；复制一份留到下一 Tick 的事件窗口，
        // 否则同帧 Tick 重建 firedNotifies 时会把还没被反馈读取的 end 丢掉。
        if (deferToNextTick) pendingNotifies.push_back(firedNotifies.back());
    }
    RefreshActiveNotifyStates();
}

void SkeletalPlayback::SetMontageNotifies(std::vector<MontageNotify> notifies,
    std::string id, float length)
{
    // 换事件源前先结束旧源的活动区间；旧 notify 表仍是解释 active 下标的唯一依据。
    EndActiveNotifyStates(time, true, true);
    montageNotifies = std::move(notifies);
    montageId = std::move(id);
    montageLength = length;
    RebuildNotifyEvents();
}
}
