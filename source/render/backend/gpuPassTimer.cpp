#include "render/backend/gpuPassTimer.h"

#include <array>
#include <iostream>
#include <sstream>

#include "core/logStore.h"
#include "render/backend/rendererBackendVulkan.h"

namespace VL
{

namespace
{

constexpr const char* kGpuStatsLogSource = "GPU";

// 与 DiagnosticsSubsystem::ReportInfo 相同的输出形态：结构化日志 + 控制台。
// RenderSystem 没有 DiagnosticsSubsystem 引用，统计输出保持在这两层即可。
void ReportGpuTiming(LogSeverity severity, const std::string& message)
{
    ProcessLog().Append(severity, kGpuStatsLogSource, message);
    std::ostream& output = severity == LogSeverity::Error ? std::cerr : std::cout;
    output << "[Diagnostics][" << LogSeverityName(severity) << "] " << message << std::endl;
}

} // namespace

void GpuPassTimer::Initialize(
    RendererBackendVulkan& rendererBackend,
    uint32_t frameSlotCount)
{
    ResetResources(rendererBackend);

    backend = &rendererBackend;
    timestampPeriodNanoseconds =
        static_cast<double>(rendererBackend.GetTimestampPeriodNanoseconds());
    timestampValidBits = rendererBackend.GetGraphicsTimestampValidBits();
    supportsTimestampQueries = rendererBackend.SupportsTimestampQueries();
    supported =
        supportsTimestampQueries &&
        timestampPeriodNanoseconds > 0.0 &&
        timestampValidBits > 0 &&
        frameSlotCount > 0;

    frameSlots.resize(frameSlotCount);
    for (FrameSlot& slot : frameSlots)
    {
        slot.pendingNames.reserve(kGpuPassTimingMaxEntries);
        slot.currentNames.reserve(kGpuPassTimingMaxEntries);
    }

    timingRing = GpuPassTimingRing(kGpuPassTimingWindowFrames);
    frameDurations.assign(kGpuPassTimingMaxEntries, std::nullopt);
    framesSinceLog = 0;
    collectedFrameCount = 0;
    unsupportedLogged = false;
    capacityWarningLogged = false;
    activeSlot = 0;
    frameActive = false;
    passOpen = false;
    initialized = true;

    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        snapshot = GpuPassStatsSnapshot{};
        snapshot.supported = supported;
    }

    if (!supported && loggingEnabled)
    {
        LogUnsupportedOnce();
    }
}

void GpuPassTimer::Shutdown(RendererBackendVulkan& rendererBackend)
{
    ResetResources(rendererBackend);
    initialized = false;
    backend = nullptr;
    supported = false;
    supportsTimestampQueries = false;
    timestampPeriodNanoseconds = 0.0;
    timestampValidBits = 0;
    frameActive = false;
    passOpen = false;
    activeSlot = 0;

    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        snapshot = GpuPassStatsSnapshot{};
    }
}

void GpuPassTimer::ResetResources(RendererBackendVulkan& rendererBackend)
{
    for (FrameSlot& slot : frameSlots)
    {
        if (slot.queryPool)
        {
            rendererBackend.DestroyQueryPool(slot.queryPool);
        }
    }
    frameSlots.clear();
    frameDurations.clear();
    scratchAverages.clear();
    scratchSampleCounts.clear();
    timingRing = GpuPassTimingRing(kGpuPassTimingWindowFrames);
}

GpuPassStatsSnapshot GpuPassTimer::Snapshot() const
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    return snapshot;
}

void GpuPassTimer::PrepareFrame(uint32_t frameSlot)
{
    if (!initialized || backend == nullptr || frameSlot >= frameSlots.size())
    {
        return;
    }
    if (!(loggingEnabled || collectionRequested.load(std::memory_order_acquire)) ||
        !supported)
    {
        return;
    }

    EnsureQueryPool(frameSlot);
}

void GpuPassTimer::BeginFrame(vk::CommandBuffer commandBuffer, uint32_t frameSlot)
{
    frameActive = false;
    passOpen = false;
    if (!initialized || backend == nullptr)
    {
        return;
    }
    if (frameSlot >= frameSlots.size())
    {
        // 槽位非法属于接线错误，但统计功能不该让渲染帧失败；直接跳过本帧。
        return;
    }

    const bool shouldCollect =
        loggingEnabled || collectionRequested.load(std::memory_order_acquire);
    if (!shouldCollect)
    {
        return;
    }
    if (!supported)
    {
        LogUnsupportedOnce();
        return;
    }

    EnsureQueryPool(frameSlot);
    // 上上次使用该槽位的提交已经由 backend BeginFrame 等待过 fence，结果可无阻塞读取。
    CollectSlotResults(frameSlot);

    FrameSlot& slot = frameSlots[frameSlot];
    // 在命令缓冲里重置该槽位的查询范围：上一批命令已随 fence 完成，不会与在飞工作重叠。
    commandBuffer.resetQueryPool(
        slot.queryPool,
        0,
        kGpuPassTimingMaxEntries * 2);
    activeSlot = frameSlot;
    frameActive = true;
}

void GpuPassTimer::EndFrame()
{
    if (!frameActive)
    {
        return;
    }

    FrameSlot& slot = frameSlots[activeSlot];
    if (passOpen)
    {
        // 未闭合的 pass 不产生 end timestamp，直接丢弃这一项，避免读回半个区间。
        --slot.currentCount;
        passOpen = false;
    }

    slot.pendingCount = slot.currentCount;
    slot.pendingValid = slot.pendingCount > 0;
    slot.currentNames.swap(slot.pendingNames);
    slot.currentCount = 0;
    frameActive = false;
}

bool GpuPassTimer::BeginPass(
    vk::CommandBuffer commandBuffer,
    const std::string& name)
{
    if (!frameActive || passOpen)
    {
        return false;
    }

    FrameSlot& slot = frameSlots[activeSlot];
    if (slot.currentCount >= kGpuPassTimingMaxEntries)
    {
        if (!capacityWarningLogged)
        {
            capacityWarningLogged = true;
            ReportGpuTiming(
                LogSeverity::Warning,
                "[GPU] pass timing capped at " +
                    std::to_string(kGpuPassTimingMaxEntries) +
                    " entries per frame; later passes in this frame are not timed.");
        }
        return false;
    }

    const uint32_t entryIndex = slot.currentCount;
    // 两端都用 bottom-of-pipe：begin 等前一个 pass 完整结束，end 等本 pass 完整结束，
    // 这样 duration 才是"该 pass 的串行 GPU 时间"。若 begin 用 top-of-pipe，上一 pass
    // 尚未完成的尾部会被记到下一个 pass 上。代价是每帧多出若干次全流水线 drain，
    // 这是 GPU 统计的常规代价，且只在显式开启采集时发生。
    commandBuffer.writeTimestamp(
        vk::PipelineStageFlagBits::eBottomOfPipe,
        slot.queryPool,
        entryIndex * 2);
    // 复用上一帧留下的字符串容量，避免逐帧分配；名字只在 pass 集合变化时改写。
    if (entryIndex < slot.currentNames.size())
    {
        slot.currentNames[entryIndex] = name;
    }
    else
    {
        slot.currentNames.push_back(name);
    }
    ++slot.currentCount;
    passOpen = true;
    return true;
}

void GpuPassTimer::EndPass(vk::CommandBuffer commandBuffer, bool began)
{
    if (!began || !frameActive || !passOpen)
    {
        return;
    }

    FrameSlot& slot = frameSlots[activeSlot];
    commandBuffer.writeTimestamp(
        vk::PipelineStageFlagBits::eBottomOfPipe,
        slot.queryPool,
        (slot.currentCount - 1) * 2 + 1);
    passOpen = false;
}

void GpuPassTimer::EnsureQueryPool(uint32_t frameSlot)
{
    FrameSlot& slot = frameSlots[frameSlot];
    if (slot.queryPool)
    {
        return;
    }

    slot.queryPool = backend->CreateTimestampQueryPool(
        kGpuPassTimingMaxEntries * 2,
        "GpuPassTimer.Slot" + std::to_string(frameSlot));
    slot.pendingNames.reserve(kGpuPassTimingMaxEntries);
    slot.currentNames.reserve(kGpuPassTimingMaxEntries);
    slot.pendingCount = 0;
    slot.currentCount = 0;
    slot.pendingValid = false;
}

void GpuPassTimer::CollectSlotResults(uint32_t frameSlot)
{
    FrameSlot& slot = frameSlots[frameSlot];
    if (!slot.pendingValid || slot.pendingCount == 0)
    {
        return;
    }

    frameDurations.assign(slot.pendingCount, std::nullopt);
    for (uint32_t i = 0; i < slot.pendingCount; ++i)
    {
        std::array<uint64_t, 2> timestamps{};
        if (!backend->TryReadTimestampQueryPair(
                slot.queryPool,
                i * 2,
                timestamps))
        {
            // 不带 WAIT 的读取在极端调度下可能尚未就绪；跳过该样本而不是阻塞。
            continue;
        }

        frameDurations[i] = ComputeGpuPassDurationMilliseconds(
            timestamps[0],
            timestamps[1],
            timestampPeriodNanoseconds);
    }

    timingRing.Configure(slot.pendingNames);
    timingRing.AddFrame(frameDurations);
    slot.pendingValid = false;
    ++collectedFrameCount;
    ++framesSinceLog;
    PublishSnapshot();
    if (printRequested.exchange(false, std::memory_order_acq_rel))
    {
        LogLatestTable();
    }
    LogTableIfDue();
}

void GpuPassTimer::PublishSnapshot()
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    const std::vector<std::string>& names = timingRing.Names();
    snapshot.supported = supported;
    snapshot.collecting = collectedFrameCount > 0;
    snapshot.table.frameCount = timingRing.WindowFrameCount();

    bool rebuildRows = snapshot.table.rows.size() != names.size();
    if (!rebuildRows)
    {
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (snapshot.table.rows[i].name != names[i])
            {
                rebuildRows = true;
                break;
            }
        }
    }
    if (rebuildRows)
    {
        snapshot.table.rows.clear();
        snapshot.table.rows.reserve(names.size());
        for (const std::string& name : names)
        {
            GpuPassTimingRow row;
            row.name = name;
            snapshot.table.rows.push_back(std::move(row));
        }
    }

    // 并行数组重算走成员缓冲，稳定后不在逐帧路径分配堆内存。
    timingRing.ComputeAverages(scratchAverages, scratchSampleCounts);
    for (size_t i = 0; i < snapshot.table.rows.size(); ++i)
    {
        snapshot.table.rows[i].averageMilliseconds = scratchAverages[i];
        snapshot.table.rows[i].sampleCount = scratchSampleCounts[i];
    }
}

void GpuPassTimer::LogTableIfDue()
{
    if (!loggingEnabled || framesSinceLog < kGpuPassTimingWindowFrames)
    {
        return;
    }
    framesSinceLog = 0;
    LogLatestTable();
}

void GpuPassTimer::LogLatestTable()
{
    GpuPassTimingTable table;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        table = snapshot.table;
    }
    ReportGpuTiming(LogSeverity::Info, FormatGpuPassTimingTable(table));
}

void GpuPassTimer::LogUnsupportedOnce()
{
    if (unsupportedLogged)
    {
        return;
    }
    unsupportedLogged = true;

    std::ostringstream message;
    message << "[GPU] pass timing disabled: timestamp queries unavailable "
            << "(timestampComputeAndGraphics=" << (supportsTimestampQueries ? 1 : 0)
            << ", timestampValidBits=" << timestampValidBits
            << ", timestampPeriod=" << timestampPeriodNanoseconds << ").";
    ReportGpuTiming(LogSeverity::Warning, message.str());
}

} // namespace VL
