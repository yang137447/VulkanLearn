#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <vector>
#include <cstdint>

namespace VL
{
// GT 单帧桥接工作量；只计值传输，不持有请求或托管对象。
struct ScriptBridgeMetrics
{
    double serializeMs = 0, managedMs = 0, parseMs = 0;
    uint64_t requestBytes = 0, responseBytes = 0, calls = 0;
};
// GT 的 CoreCLR 宿主；只交换有界 UTF-8 值，不向托管程序集暴露 World 或 Vulkan 指针。
class ScriptHost
{
public:
    void Initialize(const std::filesystem::path& runtimeDirectory);
    nlohmann::json Invoke(nlohmann::json request) const;
    bool IsReady() const { return invoke != nullptr; }
    static std::filesystem::path DefaultRuntimeDirectory();
    void ResetMetrics() { metrics = {}; }
    const ScriptBridgeMetrics& Metrics() const { return metrics; }

private:
    using Entry = int (*)(const char*, int32_t, char*, int32_t);
    Entry invoke = nullptr;
    // 同一宿主只由 GT 串行调用；结果解析后不保留缓冲区指针，跨帧复用避免重复清零大块内存。
    mutable std::vector<char> responseBuffer;
    mutable ScriptBridgeMetrics metrics;
};
} // namespace VL
