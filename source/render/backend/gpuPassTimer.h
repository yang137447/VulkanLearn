#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "render/backend/gpuPassTimings.h"

namespace VL
{

class RendererBackendVulkan;

struct GpuPassStatsSnapshot
{
    bool supported = false;
    bool collecting = false;
    GpuPassTimingTable table;
};

// 逐 pass GPU 计时器：每个 frame-in-flight 槽位独占一个 timestamp query pool。
// 查询结果在下一次复用该槽位时读取——此时 backend 的 BeginFrame 已经等待过该槽位
// 的 fence，所以结果必然就绪；读取用不带 WAIT 的 vkGetQueryPoolResults，绝不为
// 统计功能阻塞渲染。仅记录 pass 边界，不进入 render pass 实例内部。
class GpuPassTimer
{
public:
    void Initialize(RendererBackendVulkan& rendererBackend, uint32_t frameSlotCount);
    void Shutdown(RendererBackendVulkan& rendererBackend);

    // --gpu-stats 打开后每 120 帧向日志打印一张表；关闭时完全不创建 query pool。
    void SetLoggingEnabled(bool enabled) { loggingEnabled = enabled; }

    // 控制台按需采集：即使 --gpu-stats 未开启也会激活采集，但不改变周期日志行为。
    void RequestCollection() { collectionRequested.store(true, std::memory_order_release); }
    // 采集已开启但还没有可打印的表时，请求在下一次读回后补打一张（仅一次）。
    void RequestPrint() { printRequested.store(true, std::memory_order_release); }

    GpuPassStatsSnapshot Snapshot() const;

    // 在 backend BeginFrame 之前调用：只创建本帧 slot 需要的 query pool。
    // 把 pool 创建放在命令缓冲开始录制之前，避免在同一帧的录制中途创建查询对象。
    void PrepareFrame(uint32_t frameSlot);
    // 在 backend BeginFrame 返回后、任何被计时命令之前调用；frameSlot 是
    // frame-in-flight 索引，不是 swapchain image 索引。
    void BeginFrame(vk::CommandBuffer commandBuffer, uint32_t frameSlot);
    // 在 SubmitFrame 之前调用，把本帧查询交给 2 帧后的读回。
    void EndFrame();

    // 当前帧是否正在采集（BeginFrame 已激活且尚未 EndFrame）。关闭采集时为 false，
    // 调用方据此跳过只为统计服务的场景扫描（例如 hair HasWork）。
    bool IsFrameActive() const { return frameActive; }
    // 返回是否真的写入了 timestamp（未采集、容量耗尽或已有未闭合的 pass 时为 false）。
    bool BeginPass(vk::CommandBuffer commandBuffer, const std::string& name);
    void EndPass(vk::CommandBuffer commandBuffer, bool began);

private:
    struct FrameSlot
    {
        vk::QueryPool queryPool;
        // pendingNames 对应已经提交、尚未读回的查询；currentNames 正在被本帧覆盖。
        // EndFrame 交换两者，因此待读回的名字不会被下一帧录制覆盖。
        std::vector<std::string> pendingNames;
        std::vector<std::string> currentNames;
        uint32_t pendingCount = 0;
        uint32_t currentCount = 0;
        bool pendingValid = false;
    };

    void ResetResources(RendererBackendVulkan& rendererBackend);
    void EnsureQueryPool(uint32_t frameSlot);
    void CollectSlotResults(uint32_t frameSlot);
    void PublishSnapshot();
    void LogTableIfDue();
    void LogLatestTable();
    void LogUnsupportedOnce();

    RendererBackendVulkan* backend = nullptr;
    bool initialized = false;
    bool supported = false;
    bool supportsTimestampQueries = false;
    bool unsupportedLogged = false;
    bool capacityWarningLogged = false;
    bool loggingEnabled = false;
    std::atomic<bool> collectionRequested{false};
    std::atomic<bool> printRequested{false};

    double timestampPeriodNanoseconds = 0.0;
    uint32_t timestampValidBits = 0;

    std::vector<FrameSlot> frameSlots;
    uint32_t activeSlot = 0;
    bool frameActive = false;
    bool passOpen = false;

    GpuPassTimingRing timingRing;
    std::vector<std::optional<double>> frameDurations;
    std::vector<std::optional<double>> scratchAverages;
    std::vector<uint32_t> scratchSampleCounts;
    size_t framesSinceLog = 0;
    uint64_t collectedFrameCount = 0;

    mutable std::mutex snapshotMutex;
    GpuPassStatsSnapshot snapshot;
};

} // namespace VL
