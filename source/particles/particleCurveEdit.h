#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "particles/particleLimits.h"

namespace VL::ParticleCurveEdit
{

// 编辑器与测试共用的纯函数层：只操作作者 JSON，不碰运行时 ParticleSystemAsset。
// key 数组始终规范化为 [{time, color:[r,g,b,a]}] 或 [{time, size}]，
// 这样面板、预览和校验看到的是同一份数据。
inline constexpr size_t kPreviewSamples = 24;
inline constexpr size_t kMaxKeys =
    static_cast<size_t>(kMaxParticleCurveKeys);
// 时间轴相邻 key 的最小间隔；SetKeyTime 拒绝任何会破坏严格递增的修改。
inline constexpr float kTimeEpsilon = 0.001f;

namespace detail
{

using Json = nlohmann::json;

inline float Clamp01(float value)
{
    if (!std::isfinite(value)) return 0.0f;
    return std::min(1.0f, std::max(0.0f, value));
}

// RGB 允许 HDR（>1），因此只拒绝非有限值与负数；alpha 仍使用 Clamp01。
inline float NonNegative(float value)
{
    if (!std::isfinite(value)) return 0.0f;
    return std::max(0.0f, value);
}

inline float ReadFloat(
    const Json& object,
    const char* key,
    float fallback)
{
    if (!object.is_object() || !object.contains(key)) return fallback;
    const Json& value = object.at(key);
    if (!value.is_number()) return fallback;
    const double number = value.get<double>();
    return std::isfinite(number)
        ? static_cast<float>(number) : fallback;
}

inline std::array<float, 4> ReadColor(
    const Json& value,
    const std::array<float, 4>& fallback)
{
    if (!value.is_array() ||
        (value.size() != 3 && value.size() != 4))
    {
        return fallback;
    }
    std::array<float, 4> color = fallback;
    for (size_t channel = 0; channel < 3; ++channel)
    {
        if (!value.at(channel).is_number()) return fallback;
        const double number = value.at(channel).get<double>();
        if (!std::isfinite(number)) return fallback;
        color[channel] = NonNegative(static_cast<float>(number));
    }
    // 三作者分量按不透明补 alpha，避免 UI 把缺省 alpha 显示成 0。
    if (value.size() == 4)
    {
        if (!value.at(3).is_number()) return fallback;
        const double alpha = value.at(3).get<double>();
        if (!std::isfinite(alpha)) return fallback;
        color[3] = Clamp01(static_cast<float>(alpha));
    }
    else
    {
        color[3] = 1.0f;
    }
    return color;
}

inline Json ColorJson(const std::array<float, 4>& color)
{
    return Json::array({color[0], color[1], color[2], color[3]});
}

inline void SortByTime(Json& keys)
{
    std::stable_sort(
        keys.begin(),
        keys.end(),
        [](const Json& lhs, const Json& rhs)
        {
            return ReadFloat(lhs, "time", 0.0f) <
                ReadFloat(rhs, "time", 0.0f);
        });
}

} // namespace detail

inline std::array<float, 4> SampleColor(
    const nlohmann::json& keys,
    float time);
inline float SampleSize(
    const nlohmann::json& keys,
    float time);

inline nlohmann::json ReadColorKeys(const nlohmann::json& emitter)
{
    using detail::Json;
    Json result = Json::array();
    const bool hasSource = emitter.is_object() &&
        emitter.contains("colorKeys") &&
        emitter.at("colorKeys").is_array();
    if (hasSource)
    {
        for (const Json& key : emitter.at("colorKeys"))
        {
            if (!key.is_object() || !key.contains("time") ||
                !key.at("time").is_number())
            {
                continue;
            }
            const double rawTime = key.at("time").get<double>();
            if (!std::isfinite(rawTime)) continue;
            const std::array<float, 4> fallback =
                {1.0f, 1.0f, 1.0f, 1.0f};
            Json normalized = Json::object();
            normalized["time"] = detail::Clamp01(
                static_cast<float>(rawTime));
            normalized["color"] = key.contains("color")
                ? detail::ColorJson(detail::ReadColor(
                    key.at("color"), fallback))
                : detail::ColorJson(fallback);
            result.push_back(std::move(normalized));
            if (result.size() >= kMaxKeys) break;
        }
    }
    if (result.empty())
    {
        const std::array<float, 4> startDefault =
            {1.0f, 0.6f, 0.15f, 1.0f};
        const std::array<float, 4> endDefault =
            {0.4f, 0.05f, 0.0f, 0.0f};
        const std::array<float, 4> start =
            emitter.is_object() && emitter.contains("colorStart")
            ? detail::ReadColor(
                emitter.at("colorStart"), startDefault)
            : startDefault;
        const std::array<float, 4> end =
            emitter.is_object() && emitter.contains("colorEnd")
            ? detail::ReadColor(emitter.at("colorEnd"), endDefault)
            : endDefault;
        Json first = Json::object();
        first["time"] = 0.0f;
        first["color"] = detail::ColorJson(start);
        Json last = Json::object();
        last["time"] = 1.0f;
        last["color"] = detail::ColorJson(end);
        result.push_back(std::move(first));
        result.push_back(std::move(last));
    }
    detail::SortByTime(result);
    return result;
}

inline nlohmann::json ReadSizeKeys(const nlohmann::json& emitter)
{
    using detail::Json;
    Json result = Json::array();
    const bool hasSource = emitter.is_object() &&
        emitter.contains("sizeKeys") &&
        emitter.at("sizeKeys").is_array();
    if (hasSource)
    {
        for (const Json& key : emitter.at("sizeKeys"))
        {
            if (!key.is_object() || !key.contains("time") ||
                !key.at("time").is_number())
            {
                continue;
            }
            const double rawTime = key.at("time").get<double>();
            if (!std::isfinite(rawTime)) continue;
            float size = detail::ReadFloat(key, "size", 1.0f);
            if (!std::isfinite(size)) size = 0.0f;
            Json normalized = Json::object();
            normalized["time"] = detail::Clamp01(
                static_cast<float>(rawTime));
            normalized["size"] = std::max(0.0f, size);
            result.push_back(std::move(normalized));
            if (result.size() >= kMaxKeys) break;
        }
    }
    if (result.empty())
    {
        float start = 20.0f;
        float end = 5.0f;
        if (emitter.is_object() && emitter.contains("size") &&
            emitter.at("size").is_array() &&
            emitter.at("size").size() >= 2)
        {
            const Json& size = emitter.at("size");
            if (size.at(0).is_number())
            {
                const double number = size.at(0).get<double>();
                if (std::isfinite(number))
                    start = std::max(0.0f, static_cast<float>(number));
            }
            if (size.at(1).is_number())
            {
                const double number = size.at(1).get<double>();
                if (std::isfinite(number))
                    end = std::max(0.0f, static_cast<float>(number));
            }
        }
        Json first = Json::object();
        first["time"] = 0.0f;
        first["size"] = start;
        Json last = Json::object();
        last["time"] = 1.0f;
        last["size"] = end;
        result.push_back(std::move(first));
        result.push_back(std::move(last));
    }
    detail::SortByTime(result);
    return result;
}

inline bool AddKey(nlohmann::json& keys, bool colorCurve)
{
    using detail::Json;
    if (!keys.is_array() || keys.size() >= kMaxKeys) return false;

    std::vector<float> times;
    times.reserve(keys.size());
    for (const Json& key : keys)
    {
        if (!key.is_object() || !key.contains("time") ||
            !key.at("time").is_number())
        {
            return false;
        }
        const double rawTime = key.at("time").get<double>();
        if (!std::isfinite(rawTime)) return false;
        times.push_back(detail::Clamp01(
            static_cast<float>(rawTime)));
    }
    std::sort(times.begin(), times.end());

    // 空隙包含首尾：[0,t0]、相邻 key、[t_last,1]。
    float bestStart = 0.0f;
    float bestEnd = 1.0f;
    if (!times.empty())
    {
        float previous = 0.0f;
        float bestGap = -1.0f;
        for (size_t index = 0; index <= times.size(); ++index)
        {
            const float boundary = index == times.size()
                ? 1.0f : times[index];
            const float gap = boundary - previous;
            if (gap > bestGap)
            {
                bestGap = gap;
                bestStart = previous;
                bestEnd = boundary;
            }
            previous = boundary;
        }
        if (bestGap <= 1.0f / 64.0f) return false;
    }

    const float time = (bestStart + bestEnd) * 0.5f;
    Json key = Json::object();
    key["time"] = time;
    if (colorCurve)
        key["color"] = detail::ColorJson(SampleColor(keys, time));
    else
        key["size"] = SampleSize(keys, time);
    keys.push_back(std::move(key));
    detail::SortByTime(keys);
    return true;
}

inline bool RemoveKey(nlohmann::json& keys, size_t index)
{
    if (!keys.is_array() || keys.size() < 2 ||
        index >= keys.size())
    {
        return false;
    }
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

inline bool SetKeyTime(
    nlohmann::json& keys,
    size_t index,
    float time)
{
    if (!keys.is_array() || index >= keys.size() ||
        !std::isfinite(time))
    {
        return false;
    }
    if (!keys.at(index).is_object()) return false;
    const float clamped = detail::Clamp01(time);
    if (index > 0)
    {
        if (!keys.at(index - 1).is_object()) return false;
        const float previous = detail::ReadFloat(
            keys.at(index - 1), "time", 0.0f);
        if (clamped < previous + kTimeEpsilon) return false;
    }
    if (index + 1 < keys.size())
    {
        if (!keys.at(index + 1).is_object()) return false;
        const float next = detail::ReadFloat(
            keys.at(index + 1), "time", 1.0f);
        if (clamped > next - kTimeEpsilon) return false;
    }
    keys.at(index)["time"] = clamped;
    return true;
}

inline void SetColorComponent(
    nlohmann::json& keys,
    size_t index,
    size_t channel,
    float value)
{
    if (!keys.is_array() || index >= keys.size() || channel > 3)
        return;
    if (!keys.at(index).is_object()) return;
    nlohmann::json& key = keys.at(index);
    nlohmann::json& color = key["color"];
    if (!color.is_array() || color.size() != 4)
    {
        std::array<float, 4> base = {0.0f, 0.0f, 0.0f, 1.0f};
        if (color.is_array())
        {
            for (size_t slot = 0;
                 slot < 3 && slot < color.size(); ++slot)
            {
                if (!color.at(slot).is_number()) continue;
                const double number = color.at(slot).get<double>();
                if (std::isfinite(number))
                    base[slot] = detail::NonNegative(
                        static_cast<float>(number));
            }
        }
        color = detail::ColorJson(base);
    }
    // alpha 是混合权重，必须留在 [0,1]；RGB 保持作者输入的 HDR 强度。
    color[channel] = channel == 3
        ? detail::Clamp01(value)
        : detail::NonNegative(value);
}

inline void SetSize(
    nlohmann::json& keys,
    size_t index,
    float value)
{
    if (!keys.is_array() || index >= keys.size()) return;
    if (!keys.at(index).is_object()) return;
    keys.at(index)["size"] = std::isfinite(value)
        ? std::max(0.0f, value) : 0.0f;
}

inline std::array<float, 4> SampleColor(
    const nlohmann::json& keys,
    float time)
{
    struct Point
    {
        float time = 0.0f;
        std::array<float, 4> color = {1.0f, 1.0f, 1.0f, 1.0f};
    };
    std::vector<Point> points;
    if (keys.is_array())
    {
        for (const nlohmann::json& key : keys)
        {
            if (!key.is_object() || !key.contains("time") ||
                !key.at("time").is_number())
            {
                continue;
            }
            const double rawTime = key.at("time").get<double>();
            if (!std::isfinite(rawTime)) continue;
            Point point;
            point.time = detail::Clamp01(
                static_cast<float>(rawTime));
            point.color = key.contains("color")
                ? detail::ReadColor(
                    key.at("color"),
                    {1.0f, 1.0f, 1.0f, 1.0f})
                : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
            points.push_back(point);
        }
    }
    if (points.empty())
        return {1.0f, 1.0f, 1.0f, 1.0f};
    std::stable_sort(
        points.begin(), points.end(),
        [](const Point& lhs, const Point& rhs)
        {
            return lhs.time < rhs.time;
        });
    if (!std::isfinite(time)) time = 0.0f;
    time = detail::Clamp01(time);
    if (time <= points.front().time) return points.front().color;
    if (time >= points.back().time) return points.back().color;
    for (size_t index = 1; index < points.size(); ++index)
    {
        if (time > points[index].time) continue;
        const Point& left = points[index - 1];
        const Point& right = points[index];
        const float span = right.time - left.time;
        if (span <= 0.0f) return left.color;
        const float amount = (time - left.time) / span;
        std::array<float, 4> color = left.color;
        for (size_t channel = 0; channel < 4; ++channel)
        {
            color[channel] = left.color[channel] +
                (right.color[channel] - left.color[channel]) *
                    amount;
        }
        return color;
    }
    return points.back().color;
}

inline float SampleSize(
    const nlohmann::json& keys,
    float time)
{
    struct Point
    {
        float time = 0.0f;
        float size = 1.0f;
    };
    std::vector<Point> points;
    if (keys.is_array())
    {
        for (const nlohmann::json& key : keys)
        {
            if (!key.is_object() || !key.contains("time") ||
                !key.at("time").is_number())
            {
                continue;
            }
            const double rawTime = key.at("time").get<double>();
            if (!std::isfinite(rawTime)) continue;
            Point point;
            point.time = detail::Clamp01(
                static_cast<float>(rawTime));
            point.size = detail::ReadFloat(key, "size", 1.0f);
            if (!std::isfinite(point.size)) point.size = 0.0f;
            point.size = std::max(0.0f, point.size);
            points.push_back(point);
        }
    }
    if (points.empty()) return 1.0f;
    std::stable_sort(
        points.begin(), points.end(),
        [](const Point& lhs, const Point& rhs)
        {
            return lhs.time < rhs.time;
        });
    if (!std::isfinite(time)) time = 0.0f;
    time = detail::Clamp01(time);
    if (time <= points.front().time) return points.front().size;
    if (time >= points.back().time) return points.back().size;
    for (size_t index = 1; index < points.size(); ++index)
    {
        if (time > points[index].time) continue;
        const Point& left = points[index - 1];
        const Point& right = points[index];
        const float span = right.time - left.time;
        if (span <= 0.0f) return left.size;
        const float amount = (time - left.time) / span;
        return left.size + (right.size - left.size) * amount;
    }
    return points.back().size;
}

inline std::string ColorHex(const std::array<float, 4>& rgba)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result = "#";
    result.reserve(9);
    for (float value : rgba)
    {
        const int channel = static_cast<int>(std::lround(
            detail::Clamp01(value) * 255.0f));
        result.push_back(digits[(channel >> 4) & 0xF]);
        result.push_back(digits[channel & 0xF]);
    }
    return result;
}

} // namespace VL::ParticleCurveEdit
