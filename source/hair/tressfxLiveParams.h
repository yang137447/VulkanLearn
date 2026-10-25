#pragma once

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "hair/hairConsoleCommand.h"

namespace VL::Hair
{
// 编辑器/Console 的毛发模拟参数实时覆盖请求。只携带值语义：目标渲染对象名、
// 作者字段名和 1..3 个 float；不持有 World、RenderableObject 或 GPU 资源。
// 字段名与 `documents/contracts/tressfx-asset-import.md` 的作者字段一致。
struct HairLiveParameterRequest
{
    std::string objectName;
    std::string field;
    float values[3] = {0.0f, 0.0f, 0.0f};
    uint32_t valueCount = 0;
};

// 可实时覆盖的字段集合。数量与契约里的“可会话预览”一致：它们都只进入每帧
// 重打的 params UBO，不参与 buffer 分配、几何或渲染对象身份。
enum class HairLiveParameterKind
{
    Vector3,
    UnitInterval,
    Iterations,
    PositiveThreshold
};

struct HairLiveParameterSpec
{
    const char* name;
    HairLiveParameterKind kind;
    uint32_t componentCount;
};

inline const HairLiveParameterSpec* FindHairLiveParameterSpec(std::string_view field)
{
    static const HairLiveParameterSpec specs[] = {
        {"gravity", HairLiveParameterKind::Vector3, 3},
        {"wind", HairLiveParameterKind::Vector3, 3},
        {"damping", HairLiveParameterKind::UnitInterval, 1},
        {"iterations", HairLiveParameterKind::Iterations, 1},
        {"bendStiffness", HairLiveParameterKind::UnitInterval, 1},
        {"globalShapeStiffness", HairLiveParameterKind::UnitInterval, 1},
        {"globalShapeEffectiveRange", HairLiveParameterKind::UnitInterval, 1},
        {"vspCoefficient", HairLiveParameterKind::UnitInterval, 1},
        {"vspAccelThresholdMax", HairLiveParameterKind::PositiveThreshold, 1},
    };
    for (const HairLiveParameterSpec& spec : specs)
        if (field == spec.name) return &spec;
    return nullptr;
}

// 需要重载 World 才能生效的作者字段：它们决定 buffer 分配、pass 录制、几何或
// 渲染对象身份，不能通过实时覆盖修改。列出它们是为了给作者明确原因，而不是
// 让命令落到 “unknown parameter”。
inline bool IsHairReloadOnlyField(std::string_view field)
{
    return field == "enabled" || field == "backend" || field == "rootFixedPoints" ||
        field == "maxDt" || field == "vspStiffness" || field == "localShapeStiffness" ||
        field == "collision" || field == "strandRadius" || field == "skinning" ||
        field == "capsules" || field == "boneWeights" || field == "weights";
}

// 值域校验。语义是**拒绝**而不是夹取：越界输入应在命令边界报错，不能把作者的
// 笔误静默变成另一个参数值。返回错误消息，合法返回 nullptr。
inline const char* ValidateHairLiveParameterValues(const HairLiveParameterSpec& spec,
    const float* values, uint32_t count)
{
    if (count != spec.componentCount) return "wrong number of values";
    for (uint32_t index = 0; index < count; ++index)
        if (!std::isfinite(values[index])) return "value must be finite";
    switch (spec.kind)
    {
    case HairLiveParameterKind::Vector3:
        return nullptr;
    case HairLiveParameterKind::UnitInterval:
        return values[0] >= 0.0f && values[0] <= 1.0f
            ? nullptr : "value must be in [0,1]";
    case HairLiveParameterKind::Iterations:
        if (values[0] < 1.0f || values[0] > 16.0f)
            return "iterations must be in [1,16]";
        if (values[0] != std::trunc(values[0]))
            return "iterations must be an integer";
        return nullptr;
    case HairLiveParameterKind::PositiveThreshold:
        return values[0] > 0.0f && values[0] <= 1.0e7f
            ? nullptr : "value must be in (0,1e7]";
    }
    return "unknown hair parameter";
}

// 校验整条请求（目标对象名、字段是否存在、是否需要重载、值域）。`hair set` 没有
// “全部对象”语义：必须给出明确的渲染对象名，避免一次笔误改到多根毛发。
inline bool ValidateHairLiveParameterRequest(const HairLiveParameterRequest& request,
    std::string& error)
{
    error.clear();
    if (request.objectName.empty())
    {
        error = "hair parameter requires an object name";
        return false;
    }
    const HairLiveParameterSpec* spec = FindHairLiveParameterSpec(request.field);
    if (spec == nullptr)
    {
        error = IsHairReloadOnlyField(request.field)
            ? "hair parameter requires a world reload: " + request.field
            : "unknown hair parameter: " + request.field;
        return false;
    }
    if (const char* reason = ValidateHairLiveParameterValues(
            *spec, request.values, request.valueCount))
    {
        error = reason;
        return false;
    }
    return true;
}

// 解析 `hair set` 之后的文本：`<objectName> <field> <value...>`。
// objectName 与字段名各一个 token（≤128 bytes），值个数由字段决定。
// 失败不发布半解析结果；成功时 request.objectName/valueCount 才被写入。
inline const char* ParseHairSetArguments(std::string_view arguments,
    HairLiveParameterRequest& request)
{
    request = HairLiveParameterRequest{};
    size_t cursor = 0;
    const auto nextToken = [&arguments, &cursor](std::string_view& token)
    {
        while (cursor < arguments.size() && IsHairCommandSpace(arguments[cursor]))
            ++cursor;
        if (cursor >= arguments.size()) return false;
        const size_t begin = cursor;
        while (cursor < arguments.size() && !IsHairCommandSpace(arguments[cursor]))
            ++cursor;
        token = arguments.substr(begin, cursor - begin);
        return true;
    };
    std::string_view objectName;
    std::string_view field;
    if (!nextToken(objectName) || !nextToken(field))
        return "Usage: hair set <objectName> <field> <value...>";
    if (objectName.size() > 128)
        return "hair set object name is too long (max 128 bytes)";
    const HairLiveParameterSpec* spec = FindHairLiveParameterSpec(field);
    if (spec == nullptr)
        return IsHairReloadOnlyField(field)
            ? "this hair parameter requires a world reload"
            : "unknown hair parameter";
    HairLiveParameterRequest parsed;
    parsed.field.assign(field);
    for (uint32_t index = 0; index < spec->componentCount; ++index)
    {
        std::string_view token;
        if (!nextToken(token)) return "missing value for hair parameter";
        const std::string text(token);
        char* end = nullptr;
        errno = 0;
        const float value = std::strtof(text.c_str(), &end);
        // 整段 token 必须被消费；"0.02x" 或空 token 不能被当成合法数字。
        if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
            !std::isfinite(value))
            return "value must be a finite number";
        parsed.values[index] = value;
    }
    std::string_view extra;
    if (nextToken(extra)) return "too many values for hair parameter";
    parsed.valueCount = spec->componentCount;
    if (const char* reason = ValidateHairLiveParameterValues(
            *spec, parsed.values, parsed.valueCount))
        return reason;
    parsed.objectName.assign(objectName);
    request = std::move(parsed);
    return nullptr;
}
} // namespace VL::Hair
