#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace VL::ParticleFlipbookEdit
{

// 16x16 图集上限。编辑器只生成 1..256 行的逐帧表；更大的区间交由
// 资产校验报错，而不是在这里悄悄截断作者数据。
inline constexpr size_t kMaxFrames = 256;
// 逐帧 UV rect 在 UBO 里每帧独占一个 vec4，GPU 表固定 64 项。
inline constexpr size_t kMaxUvFrames = 64;
// 逐帧旋转在 UBO 里每 vec4 打包 4 帧，GPU 表固定 16 项（最多 64 帧）。
inline constexpr size_t kMaxRotationFrames = 64;

namespace detail
{

using Json = nlohmann::json;

inline bool ReadInteger(
    const Json& object,
    const char* key,
    size_t& value)
{
    if (!object.is_object() || !object.contains(key)) return false;
    const Json& field = object.at(key);
    if (!field.is_number()) return false;
    const double number = field.get<double>();
    if (!std::isfinite(number) || number < 0.0 ||
        std::floor(number) != number ||
        number > static_cast<double>(
            std::numeric_limits<size_t>::max()))
    {
        return false;
    }
    value = static_cast<size_t>(number);
    return true;
}

// 帧区间同时可能来自 emitter.flipbook 或直接传入 flipbook 对象。
inline const Json* FlipbookObject(const Json& emitter)
{
    if (!emitter.is_object()) return nullptr;
    if (emitter.contains("flipbook") &&
        emitter.at("flipbook").is_object())
    {
        return &emitter.at("flipbook");
    }
    return &emitter;
}

inline bool ReadFinite(const Json& value, float& result)
{
    if (!value.is_number()) return false;
    const double number = value.get<double>();
    if (!std::isfinite(number)) return false;
    result = static_cast<float>(number);
    return std::isfinite(result);
}

} // namespace detail

inline size_t FlipbookFrameCount(const nlohmann::json& emitter)
{
    const nlohmann::json* flipbook =
        detail::FlipbookObject(emitter);
    if (flipbook == nullptr) return 0;

    size_t start = 0;
    detail::ReadInteger(*flipbook, "startFrame", start);

    size_t end = 0;
    if (!detail::ReadInteger(*flipbook, "endFrame", end))
    {
        // 与运行时 loader 一致：省略 endFrame 时使用图集最后一帧。
        size_t columns = 1;
        size_t rows = 1;
        detail::ReadInteger(*flipbook, "columns", columns);
        detail::ReadInteger(*flipbook, "rows", rows);
        if (columns == 0 || rows == 0 ||
            columns > std::numeric_limits<size_t>::max() / rows)
        {
            return 0;
        }
        end = columns * rows - 1;
    }
    if (end < start) return 0;
    if (end - start == std::numeric_limits<size_t>::max())
        return std::numeric_limits<size_t>::max();
    return end - start + 1;
}

inline bool HasFlipbookDurations(const nlohmann::json& emitter)
{
    const nlohmann::json* flipbook =
        detail::FlipbookObject(emitter);
    return flipbook != nullptr &&
        flipbook->contains("frameDurations") &&
        flipbook->at("frameDurations").is_array() &&
        !flipbook->at("frameDurations").empty();
}

inline bool HasFlipbookPivots(const nlohmann::json& emitter)
{
    const nlohmann::json* flipbook =
        detail::FlipbookObject(emitter);
    return flipbook != nullptr &&
        flipbook->contains("framePivots") &&
        flipbook->at("framePivots").is_array() &&
        !flipbook->at("framePivots").empty();
}

inline bool HasFlipbookUVs(const nlohmann::json& emitter)
{
    const nlohmann::json* flipbook =
        detail::FlipbookObject(emitter);
    return flipbook != nullptr &&
        flipbook->contains("frameUVs") &&
        flipbook->at("frameUVs").is_array() &&
        !flipbook->at("frameUVs").empty();
}

inline bool HasFlipbookRotations(const nlohmann::json& emitter)
{
    const nlohmann::json* flipbook =
        detail::FlipbookObject(emitter);
    return flipbook != nullptr &&
        flipbook->contains("frameRotations") &&
        flipbook->at("frameRotations").is_array() &&
        !flipbook->at("frameRotations").empty();
}

inline nlohmann::json EnsureFlipbookDurations(
    const nlohmann::json& emitter)
{
    const size_t frameCount = FlipbookFrameCount(emitter);
    if (frameCount == 0 || frameCount > kMaxFrames)
        return nlohmann::json();
    const nlohmann::json* flipbook =
        detail::FlipbookObject(emitter);
    const nlohmann::json* source = nullptr;
    if (flipbook != nullptr &&
        flipbook->contains("frameDurations") &&
        flipbook->at("frameDurations").is_array())
    {
        source = &flipbook->at("frameDurations");
    }

    nlohmann::json result = nlohmann::json::array();
    for (size_t index = 0; index < frameCount; ++index)
    {
        float value = 1.0f;
        if (source != nullptr && index < source->size())
            detail::ReadFinite(source->at(index), value);
        if (!std::isfinite(value) || value <= 0.0f)
            value = 1.0f;
        result.push_back(value);
    }
    return result;
}

inline nlohmann::json EnsureFlipbookPivots(
    const nlohmann::json& emitter)
{
    const size_t frameCount = FlipbookFrameCount(emitter);
    if (frameCount == 0 || frameCount > kMaxFrames)
        return nlohmann::json();
    const nlohmann::json* flipbook =
        detail::FlipbookObject(emitter);
    const nlohmann::json* source = nullptr;
    if (flipbook != nullptr &&
        flipbook->contains("framePivots") &&
        flipbook->at("framePivots").is_array())
    {
        source = &flipbook->at("framePivots");
    }

    nlohmann::json result = nlohmann::json::array();
    for (size_t index = 0; index < frameCount; ++index)
    {
        float u = 0.5f;
        float v = 0.5f;
        if (source != nullptr && index < source->size())
        {
            const nlohmann::json& entry = source->at(index);
            if (entry.is_array() && entry.size() == 2)
            {
                detail::ReadFinite(entry.at(0), u);
                detail::ReadFinite(entry.at(1), v);
            }
        }
        if (!std::isfinite(u)) u = 0.5f;
        if (!std::isfinite(v)) v = 0.5f;
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);
        result.push_back(nlohmann::json::array({u, v}));
    }
    return result;
}

inline bool SetFlipbookDuration(
    nlohmann::json& durations,
    size_t index,
    float value)
{
    if (!durations.is_array() || index >= durations.size() ||
        !std::isfinite(value) || value <= 0.0f)
    {
        return false;
    }
    durations[index] = value;
    return true;
}

inline bool SetFlipbookPivot(
    nlohmann::json& pivots,
    size_t index,
    char component,
    float value)
{
    if (!pivots.is_array() || index >= pivots.size() ||
        !std::isfinite(value))
    {
        return false;
    }
    nlohmann::json& pivot = pivots[index];
    if (!pivot.is_array() || pivot.size() != 2) return false;
    const char axis = component >= 'A' && component <= 'Z'
        ? static_cast<char>(component - 'A' + 'a')
        : component;
    if (axis != 'u' && axis != 'v') return false;
    pivot[axis == 'u' ? 0 : 1] =
        std::clamp(value, 0.0f, 1.0f);
    return true;
}

inline nlohmann::json UniformFlipbookDurations(
    size_t frameCount,
    float value = 1.0f)
{
    if (frameCount == 0 || frameCount > kMaxFrames ||
        !std::isfinite(value) || value <= 0.0f)
    {
        return nlohmann::json();
    }
    nlohmann::json result = nlohmann::json::array();
    for (size_t index = 0; index < frameCount; ++index)
        result.push_back(value);
    return result;
}

inline nlohmann::json UniformFlipbookPivots(
    size_t frameCount,
    float u = 0.5f,
    float v = 0.5f)
{
    if (frameCount == 0 || frameCount > kMaxFrames ||
        !std::isfinite(u) || !std::isfinite(v))
    {
        return nlohmann::json();
    }
    nlohmann::json result = nlohmann::json::array();
    for (size_t index = 0; index < frameCount; ++index)
    {
        result.push_back(nlohmann::json::array({
            std::clamp(u, 0.0f, 1.0f),
            std::clamp(v, 0.0f, 1.0f)}));
    }
    return result;
}

// UV rect 缺失时显示 sprite 空间的完整矩形 (0,0,1,1)。
inline nlohmann::json EnsureFlipbookUVs(
    const nlohmann::json& emitter)
{
    const size_t frameCount = FlipbookFrameCount(emitter);
    if (frameCount == 0 || frameCount > kMaxUvFrames)
        return nlohmann::json();
    const nlohmann::json* flipbook =
        detail::FlipbookObject(emitter);
    const nlohmann::json* source = nullptr;
    if (flipbook != nullptr &&
        flipbook->contains("frameUVs") &&
        flipbook->at("frameUVs").is_array())
    {
        source = &flipbook->at("frameUVs");
    }

    nlohmann::json result = nlohmann::json::array();
    for (size_t index = 0; index < frameCount; ++index)
    {
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 1.0f;
        float v1 = 1.0f;
        if (source != nullptr && index < source->size())
        {
            const nlohmann::json& entry = source->at(index);
            if (entry.is_array() && entry.size() == 4)
            {
                detail::ReadFinite(entry.at(0), u0);
                detail::ReadFinite(entry.at(1), v0);
                detail::ReadFinite(entry.at(2), u1);
                detail::ReadFinite(entry.at(3), v1);
            }
        }
        if (!std::isfinite(u0) || !std::isfinite(v0) ||
            !std::isfinite(u1) || !std::isfinite(v1) ||
            u0 < 0.0f || u0 > 1.0f || v0 < 0.0f || v0 > 1.0f ||
            u1 < 0.0f || u1 > 1.0f || v1 < 0.0f || v1 > 1.0f ||
            u0 >= u1 || v0 >= v1)
        {
            u0 = 0.0f;
            v0 = 0.0f;
            u1 = 1.0f;
            v1 = 1.0f;
        }
        result.push_back(
            nlohmann::json::array({u0, v0, u1, v1}));
    }
    return result;
}

// component 接受 u0/v0/u1/v1（大小写均可）。越界值按 [0,1] 夹取，
// 但 u0>=u1 或 v0>=v1 会拒绝，避免把非法 rect 提交给加载器。
inline bool SetFlipbookUV(
    nlohmann::json& uvs,
    size_t index,
    const std::string& component,
    float value)
{
    if (!uvs.is_array() || index >= uvs.size() ||
        !std::isfinite(value))
    {
        return false;
    }
    nlohmann::json& rect = uvs[index];
    if (!rect.is_array() || rect.size() != 4) return false;

    std::string axis = component;
    for (char& character : axis)
    {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    int slot = -1;
    if (axis == "u0") slot = 0;
    else if (axis == "v0") slot = 1;
    else if (axis == "u1") slot = 2;
    else if (axis == "v1") slot = 3;
    if (slot < 0) return false;

    rect[slot] = std::clamp(value, 0.0f, 1.0f);
    const float u0 = rect.at(0).get<float>();
    const float v0 = rect.at(1).get<float>();
    const float u1 = rect.at(2).get<float>();
    const float v1 = rect.at(3).get<float>();
    return u0 < u1 && v0 < v1;
}

inline nlohmann::json UniformFlipbookUVs(
    size_t frameCount,
    float u0 = 0.0f,
    float v0 = 0.0f,
    float u1 = 1.0f,
    float v1 = 1.0f)
{
    if (frameCount == 0 || frameCount > kMaxUvFrames ||
        !std::isfinite(u0) || !std::isfinite(v0) ||
        !std::isfinite(u1) || !std::isfinite(v1) ||
        u0 < 0.0f || u0 > 1.0f || v0 < 0.0f || v0 > 1.0f ||
        u1 < 0.0f || u1 > 1.0f || v1 < 0.0f || v1 > 1.0f ||
        u0 >= u1 || v0 >= v1)
    {
        return nlohmann::json();
    }
    nlohmann::json result = nlohmann::json::array();
    for (size_t index = 0; index < frameCount; ++index)
        result.push_back(nlohmann::json::array({u0, v0, u1, v1}));
    return result;
}

// 旋转表缺失时显示 0 度；长度固定等于帧区间。
inline nlohmann::json EnsureFlipbookRotations(
    const nlohmann::json& emitter)
{
    const size_t frameCount = FlipbookFrameCount(emitter);
    if (frameCount == 0 || frameCount > kMaxRotationFrames)
        return nlohmann::json();
    const nlohmann::json* flipbook =
        detail::FlipbookObject(emitter);
    const nlohmann::json* source = nullptr;
    if (flipbook != nullptr &&
        flipbook->contains("frameRotations") &&
        flipbook->at("frameRotations").is_array())
    {
        source = &flipbook->at("frameRotations");
    }

    nlohmann::json result = nlohmann::json::array();
    for (size_t index = 0; index < frameCount; ++index)
    {
        float value = 0.0f;
        if (source != nullptr && index < source->size())
            detail::ReadFinite(source->at(index), value);
        if (!std::isfinite(value)) value = 0.0f;
        result.push_back(value);
    }
    return result;
}

inline bool SetFlipbookRotation(
    nlohmann::json& rotations,
    size_t index,
    float value)
{
    if (!rotations.is_array() || index >= rotations.size() ||
        !std::isfinite(value))
    {
        return false;
    }
    rotations[index] = value;
    return true;
}

inline nlohmann::json UniformFlipbookRotations(
    size_t frameCount,
    float value = 0.0f)
{
    if (frameCount == 0 || frameCount > kMaxRotationFrames ||
        !std::isfinite(value))
    {
        return nlohmann::json();
    }
    nlohmann::json result = nlohmann::json::array();
    for (size_t index = 0; index < frameCount; ++index)
        result.push_back(value);
    return result;
}

} // namespace VL::ParticleFlipbookEdit
