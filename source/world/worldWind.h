#pragma once

#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include <Eigen/Dense>

namespace VL
{

// 场景全局风的阵风参数。阵风是连续噪声调制，不是离散阵风事件：没有持续时间或上升/下降系数。
// 场景没有 gust 字段时 enabled 为 false，其余数值保持 0，渲染侧据此整体关闭阵风。
struct WorldWindGust
{
    bool enabled = false;
    // 相对强度，[0,1]：阵风对基准风强度的调制幅度。
    float amplitude = 0.0f;
    // 阵风噪声的时间频率（Hz 量级），>= 0。
    float frequency = 0.0f;
    // 噪声扰动比例，[0,1]：0 为平滑阵风，1 为最强的不规则扰动。
    float turbulence = 0.0f;
};

// 场景风的运行时数据。风场只描述环境状态：不属于任何材质或资产，树、草、头发只读它，
// 各自用自身的响应系数决定如何摆动。默认值即"无风"（strength 为 0）。
struct WorldWind
{
    // 已归一化的风向。作者向量的长度不进入方向，只用 strength 表示强度。
    Eigen::Vector3f direction = Eigen::Vector3f::UnitX();
    // 基准风强度，单位 cm/s。
    float strength = 0.0f;
    WorldWindGust gust;
};

namespace WorldWindDetail
{
inline void RejectUnknownKeys(const nlohmann::json& node, std::initializer_list<const char*> allowed,
    const char* where)
{
    const std::set<std::string> allowedKeys(allowed.begin(), allowed.end());
    for (const auto& item : node.items())
        if (!allowedKeys.count(item.key()))
            throw std::invalid_argument(std::string("Wind ") + where + " has unknown key: " + item.key());
}

// 作者数值必须是有限的 float；double 中的有限值在转 float 后可能溢出成 inf。
inline float ReadFiniteWindNumber(const nlohmann::json& node, const char* field)
{
    if (!node.contains(field) || !node.at(field).is_number())
        throw std::invalid_argument(std::string("Wind requires finite number: ") + field);
    const double value = node.at(field).get<double>();
    const float floatValue = static_cast<float>(value);
    if (!std::isfinite(value) || !std::isfinite(floatValue))
        throw std::invalid_argument(std::string("Wind requires finite number: ") + field);
    return floatValue;
}
} // namespace WorldWindDetail

// 校验场景 wind 对象的作者数据。只检查 JSON 合同，不创建 World 或 GPU 资源。
// 字段全部必填（阵风整体可选）；未知键直接拒绝，避免拼写错误被静默忽略。
inline void ValidateSceneWindJson(const nlohmann::json& object)
{
    using namespace WorldWindDetail;
    RejectUnknownKeys(object, {"type", "name", "direction", "strength", "gust"}, "object");

    if (!object.contains("direction") || !object.at("direction").is_array() || object.at("direction").size() != 3)
        throw std::invalid_argument("Wind requires vec3: direction");
    double lengthSquared = 0.0;
    for (const auto& component : object.at("direction"))
    {
        if (!component.is_number() || !std::isfinite(component.get<double>()))
            throw std::invalid_argument("Invalid wind direction");
        const double number = component.get<double>();
        lengthSquared += number * number;
    }
    // 与灯光 normal 的零长度判定使用同一阈值；作者向量长度不影响风向，但零向量没有方向。
    if (lengthSquared <= 1.0e-12)
        throw std::invalid_argument("Wind direction must be non-zero");

    const float strength = ReadFiniteWindNumber(object, "strength");
    if (strength < 0.0f)
        throw std::invalid_argument("Wind strength must be >= 0");

    if (!object.contains("gust")) return;
    const auto& gust = object.at("gust");
    if (!gust.is_object()) throw std::invalid_argument("Wind gust must be object");
    RejectUnknownKeys(gust, {"amplitude", "frequency", "turbulence"}, "gust");
    const float amplitude = ReadFiniteWindNumber(gust, "amplitude");
    const float frequency = ReadFiniteWindNumber(gust, "frequency");
    const float turbulence = ReadFiniteWindNumber(gust, "turbulence");
    if (amplitude < 0.0f || amplitude > 1.0f)
        throw std::invalid_argument("Wind gust amplitude must be in [0, 1]");
    if (frequency < 0.0f)
        throw std::invalid_argument("Wind gust frequency must be >= 0");
    if (turbulence < 0.0f || turbulence > 1.0f)
        throw std::invalid_argument("Wind gust turbulence must be in [0, 1]");
}

// 读取已通过 ValidateSceneWindJson 的 wind 对象。方向在这里归一化，之后的消费者不再重复归一化。
inline WorldWind ReadSceneWind(const nlohmann::json& object)
{
    using namespace WorldWindDetail;
    WorldWind wind;
    const Eigen::Vector3f direction(
        object.at("direction")[0].get<float>(),
        object.at("direction")[1].get<float>(),
        object.at("direction")[2].get<float>());
    wind.direction = direction.normalized();
    wind.strength = ReadFiniteWindNumber(object, "strength");
    if (object.contains("gust"))
    {
        const auto& gust = object.at("gust");
        wind.gust.enabled = true;
        wind.gust.amplitude = ReadFiniteWindNumber(gust, "amplitude");
        wind.gust.frequency = ReadFiniteWindNumber(gust, "frequency");
        wind.gust.turbulence = ReadFiniteWindNumber(gust, "turbulence");
    }
    return wind;
}

} // namespace VL
