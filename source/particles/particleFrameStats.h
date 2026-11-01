#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace VL
{

// 主线程可读的帧级预算快照。CPU 字段描述当前帧的录制估算；GPU 字段来自
// particle graphics pass 的 timestamp 查询，是最近一次已完成样本的耗时。
// 两者都只读，不改变渲染行为。
struct ParticleFrameStats
{
    // 逐发射器 GPU 绘制区间；全局排序池使用 key "__global__"。
    struct EmitterGpu
    {
        std::string key;
        bool supported = false;
        double lastMilliseconds = 0.0;
        double averageMilliseconds = 0.0;
        double maxMilliseconds = 0.0;
        uint64_t sampleCount = 0;
    };

    uint32_t totalEmitters = 0;
    uint32_t visibleEmitters = 0;
    uint32_t culledEmitters = 0;
    uint32_t sortedEmitters = 0;
    uint32_t estimatedDrawCalls = 0;
    uint64_t estimatedDrawnVertices = 0;
    uint64_t totalParticles = 0;
    uint64_t sortDispatchCount = 0;
    uint64_t sortCapacityTotal = 0;
    uint64_t frameIndex = 0;
    double elapsedSeconds = 0.0;
    // 后端未暴露 graphics timestamp 或计时被禁用时为 false，数值保持 0。
    // GPU 数值只覆盖 particle graphics pass，不含 compute 模拟与排序 dispatch。
    bool gpuTimingSupported = false;
    double gpuMilliseconds = 0.0;
    double gpuAverageMilliseconds = 0.0;
    double gpuMaxMilliseconds = 0.0;
    uint64_t gpuSampleCount = 0;
    std::vector<EmitterGpu> emitterGpu;

    nlohmann::ordered_json ToJson() const
    {
        nlohmann::ordered_json result;
        result["totalEmitters"] = totalEmitters;
        result["visibleEmitters"] = visibleEmitters;
        result["culledEmitters"] = culledEmitters;
        result["sortedEmitters"] = sortedEmitters;
        result["estimatedDrawCalls"] = estimatedDrawCalls;
        result["estimatedDrawnVertices"] = estimatedDrawnVertices;
        result["totalParticles"] = totalParticles;
        result["sortDispatchCount"] = sortDispatchCount;
        result["sortCapacityTotal"] = sortCapacityTotal;
        result["frameIndex"] = frameIndex;
        result["elapsedSeconds"] = elapsedSeconds;
        result["gpuTimingSupported"] = gpuTimingSupported;
        result["gpuMilliseconds"] = gpuMilliseconds;
        result["gpuAverageMilliseconds"] = gpuAverageMilliseconds;
        result["gpuMaxMilliseconds"] = gpuMaxMilliseconds;
        result["gpuSampleCount"] = gpuSampleCount;
        result["emitterGpu"] = nlohmann::ordered_json::array();
        for (const EmitterGpu& emitter : emitterGpu)
        {
            result["emitterGpu"].push_back({
                {"key", emitter.key},
                {"supported", emitter.supported},
                {"milliseconds", emitter.lastMilliseconds},
                {"averageMilliseconds", emitter.averageMilliseconds},
                {"maxMilliseconds", emitter.maxMilliseconds},
                {"sampleCount", emitter.sampleCount}});
        }
        return result;
    }

    std::string FormatLine() const
    {
        std::ostringstream stream;
        stream << "particles frame=" << frameIndex
            << " elapsed=" << std::fixed << std::setprecision(2)
            << elapsedSeconds << "s"
            << " emitters=" << totalEmitters
            << " visible=" << visibleEmitters
            << " culled=" << culledEmitters
            << " sorted=" << sortedEmitters
            << " drawCalls=" << estimatedDrawCalls
            << " vertices=" << estimatedDrawnVertices
            << " particles=" << totalParticles
            << " sortDispatches=" << sortDispatchCount
            << " sortCapacity=" << sortCapacityTotal
            << " gpuTimingSupported=" << (gpuTimingSupported ? 1 : 0)
            << " gpuMs=" << std::fixed << std::setprecision(3)
            << gpuMilliseconds
            << " gpuAvgMs=" << gpuAverageMilliseconds
            << " gpuMaxMs=" << gpuMaxMilliseconds
            << " gpuSamples=" << gpuSampleCount;
        return stream.str();
    }
};

// bitonic 每级 (k) 需要 k 的一半数量 pass，总 pass 数为
// levels*(levels+1)/2；capacity<=1 没有排序 pass。
inline uint32_t ParticleSortDispatchCount(uint32_t capacity)
{
    uint32_t levels = 0;
    while (capacity > 1)
    {
        capacity >>= 1;
        ++levels;
    }
    return levels * (levels + 1) / 2;
}

} // namespace VL
