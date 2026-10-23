#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace VL
{

// 一个 pass 的冻结样本。milliseconds 为空表示该 pass 在统计窗口内没有有效样本
// （查询未就绪、计数器回绕或采集尚未开始），不是 0 ms。
struct PerformancePassSample
{
    std::string name;
    std::optional<double> milliseconds;
    uint32_t sampleCount = 0;
};

// Performance 页与 perf JSON/JSONL 共用的唯一快照。
//
// 只包含值语义的 CPU/GPU 数字，不携带 World、Material、Vulkan 句柄或渲染线程指针：
// GT 每帧从渲染侧复制一份，UI 与机器通道随后都只读这份副本。CPU 帧时长来自
// 渲染器自动曝光路径使用的同一个 frame delta，因此 fps/cpuFrameMs 与曝光适应
// 看到的是同一个时钟，这里不再新建第二个帧率计时器。
struct PerformanceSnapshot
{
    bool gpuTimingSupported = false;
    bool collecting = false;
    std::optional<double> fps;
    std::optional<double> cpuFrameMs;
    // 所有有效 pass 样本之和；全部缺失时为空。它不是完整帧时间。
    std::optional<double> gpuTotalMs;
    std::vector<PerformancePassSample> passes;
    // 读回窗口内已采集的帧数；只用于表格/状态文案，不进入 JSON。
    size_t windowFrameCount = 0;

    // valid 只描述 GPU 通道：设备不支持 timestamp、尚未读回任何样本、
    // 或窗口内没有任何有效 pass 时都为 false。CPU 数字仍可独立显示。
    bool IsValid() const
    {
        return gpuTimingSupported && collecting && gpuTotalMs.has_value() &&
            !passes.empty();
    }
};

inline bool IsFiniteNonNegative(double value)
{
    return std::isfinite(value) && value >= 0.0;
}

// 把非法/负值样本收敛成"缺失"，绝不把 NaN/Inf 写进 UI 或 JSON。
inline std::optional<double> SanitizePerformanceMilliseconds(std::optional<double> value)
{
    if (!value.has_value() || !IsFiniteNonNegative(*value))
    {
        return std::nullopt;
    }
    return value;
}

// 重算 gpuTotalMs 与非法样本；调用方可以原地更新 passes 后再调用它，
// 因此渲染侧读回路径复用同一份 vector，不逐帧重建字符串。
inline void RecomputePerformanceSnapshotTotals(PerformanceSnapshot& snapshot)
{
    double total = 0.0;
    bool hasValidSample = false;
    for (PerformancePassSample& pass : snapshot.passes)
    {
        pass.milliseconds = SanitizePerformanceMilliseconds(pass.milliseconds);
        if (!pass.milliseconds.has_value())
        {
            continue;
        }
        total += *pass.milliseconds;
        hasValidSample = true;
    }
    snapshot.gpuTotalMs = hasValidSample ? std::optional<double>(total) : std::nullopt;
}

// CPU frame delta（秒）与 GPU 读回表合并成一份快照。delta 非法时 fps/cpuFrameMs
// 都留空；面板会显示占位符而不是伪造的 0。
inline PerformanceSnapshot BuildPerformanceSnapshot(
    std::vector<PerformancePassSample> passes,
    size_t windowFrameCount,
    bool gpuTimingSupported,
    bool collecting,
    std::optional<double> cpuFrameSeconds)
{
    PerformanceSnapshot snapshot;
    snapshot.gpuTimingSupported = gpuTimingSupported;
    snapshot.collecting = collecting;
    snapshot.windowFrameCount = windowFrameCount;
    snapshot.passes = std::move(passes);
    if (cpuFrameSeconds.has_value() && std::isfinite(*cpuFrameSeconds) &&
        *cpuFrameSeconds > 0.0)
    {
        snapshot.cpuFrameMs = *cpuFrameSeconds * 1000.0;
        snapshot.fps = 1.0 / *cpuFrameSeconds;
    }
    RecomputePerformanceSnapshotTotals(snapshot);
    return snapshot;
}

// 固定 3 位小数，跨通道（面板/JSON/JSONL）共用；把 -0.0 归一成 0.000。
inline std::string FormatPerformanceNumber(double value, int precision = 3)
{
    if (value == 0.0)
    {
        value = 0.0;
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

inline std::optional<double> SerializedPerformanceValue(
    const std::optional<double>& value,
    bool requirePositive = false)
{
    if (!value.has_value() || !std::isfinite(*value))
    {
        return std::nullopt;
    }
    if (requirePositive ? *value <= 0.0 : *value < 0.0)
    {
        return std::nullopt;
    }
    return value;
}

inline std::string EscapePerformanceJsonString(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20)
            {
                const char digits[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += digits[(static_cast<unsigned char>(character) >> 4) & 0xf];
                escaped += digits[static_cast<unsigned char>(character) & 0xf];
            }
            else
            {
                escaped += character;
            }
            break;
        }
    }
    return escaped;
}

inline void AppendPerformanceNumber(
    std::string& output,
    std::optional<double> value,
    bool requirePositive = false,
    int precision = 3)
{
    value = SerializedPerformanceValue(value, requirePositive);
    if (!value.has_value())
    {
        output += "null";
        return;
    }
    output += FormatPerformanceNumber(*value, precision);
}

// 机器可读通道的唯一序列化实现。键顺序固定为
// fps、cpuMs、gpuTotalMs、valid、passes；pass 内固定为 name、ms。
// 无效值一律写 null，绝不输出 NaN/Inf，也不使用指数记法。
inline std::string FormatPerformanceJson(const PerformanceSnapshot& snapshot)
{
    std::string output;
    output.reserve(128 + snapshot.passes.size() * 48);
    output += "{\"fps\":";
    AppendPerformanceNumber(output, snapshot.fps, true);
    output += ",\"cpuMs\":";
    AppendPerformanceNumber(output, snapshot.cpuFrameMs);
    output += ",\"gpuTotalMs\":";
    AppendPerformanceNumber(output, snapshot.gpuTotalMs);
    output += ",\"valid\":";
    output += snapshot.IsValid() ? "true" : "false";
    output += ",\"passes\":[";
    for (size_t index = 0; index < snapshot.passes.size(); ++index)
    {
        if (index != 0)
        {
            output += ',';
        }
        const PerformancePassSample& pass = snapshot.passes[index];
        output += "{\"name\":\"";
        output += EscapePerformanceJsonString(pass.name);
        output += "\",\"ms\":";
        AppendPerformanceNumber(output, SanitizePerformanceMilliseconds(pass.milliseconds));
        output += '}';
    }
    output += "]}";
    return output;
}

// 面板展示百分比：GPU 总和有效时才计算，避免除零；结果为 0..100。
inline std::optional<double> PerformancePassPercent(
    const PerformanceSnapshot& snapshot,
    const PerformancePassSample& pass)
{
    if (!snapshot.gpuTotalMs.has_value() || *snapshot.gpuTotalMs <= 0.0 ||
        !pass.milliseconds.has_value())
    {
        return std::nullopt;
    }
    return *pass.milliseconds * 100.0 / *snapshot.gpuTotalMs;
}

} // namespace VL
