#pragma once

#include <string>

#include "hair/tressfxLiveParams.h"
#include "mesh/meshAssetTypes.h"

namespace VL::Hair
{
// 把一条实时覆盖请求写到渲染对象自己的 TressfxSimulationConfig 副本。
// 只改目标字段，其他字段保持不变；值域非法时整条请求被拒绝，config 不留半改状态。
// 调用方必须保证这是渲染线程对单个 RenderableObject 副本的写入（不碰 imported/
// 共享 section，否则会污染其他 placement）。
inline bool ApplyHairLiveParameter(::TressfxSimulationConfig& config,
    const HairLiveParameterRequest& request, std::string& error)
{
    error.clear();
    // 这里只校验参数本身：对象名属于命令/目标边界，不参与“写 config”的语义。
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
    const float* values = request.values;
    if (request.field == "gravity")
    {
        config.gravity = Eigen::Vector3f(values[0], values[1], values[2]);
        return true;
    }
    if (request.field == "wind")
    {
        config.wind = Eigen::Vector3f(values[0], values[1], values[2]);
        return true;
    }
    if (request.field == "damping")
    {
        config.damping = values[0];
        return true;
    }
    if (request.field == "iterations")
    {
        // 校验已保证是 [1,16] 内的整数；这里只做窄化，不再夹取。
        config.iterations = static_cast<uint32_t>(values[0]);
        return true;
    }
    if (request.field == "bendStiffness")
    {
        config.bendStiffness = values[0];
        return true;
    }
    if (request.field == "globalShapeStiffness")
    {
        config.globalShapeStiffness = values[0];
        return true;
    }
    if (request.field == "globalShapeEffectiveRange")
    {
        config.globalShapeEffectiveRange = values[0];
        return true;
    }
    if (request.field == "vspCoefficient")
    {
        config.vspCoefficient = values[0];
        return true;
    }
    if (request.field == "vspAccelThresholdMax")
    {
        config.vspAccelThresholdMax = values[0];
        return true;
    }
    // FindHairLiveParameterSpec 与这里必须同步；走到这里说明表被改漏了。
    error = "unhandled hair parameter: " + request.field;
    return false;
}
} // namespace VL::Hair
