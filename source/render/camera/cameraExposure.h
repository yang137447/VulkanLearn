#pragma once

#include <cmath>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace VL
{
enum class ExposureMode { Ev100, PhysicalCamera, Auto };

// Camera 拥有作者状态，快照冻结后供实时/离线显示共用；不参与离线积分或改写 film。
// Auto 模式的测光与历史曝光由实时 RenderGraph 持有，离线渲染仍使用 ev100 作为显示基线。
struct CameraExposure
{
    ExposureMode mode = ExposureMode::Ev100;
    double ev100 = 9.0;
    double aperture = 4.0;
    double shutterSeconds = 1.0 / 125.0;
    double iso = 100.0;
    double compensation = 0.0;
    double autoMinEv100 = -10.0;
    double autoMaxEv100 = 20.0;
    double autoSpeedUp = 3.0;
    double autoSpeedDown = 1.0;
    double autoTargetGray = 0.18;
};

inline double EffectiveEv100(const CameraExposure& value)
{
    return value.mode == ExposureMode::Ev100 || value.mode == ExposureMode::Auto ? value.ev100 :
        std::log2(value.aperture * value.aperture / value.shutterSeconds * 100.0 / value.iso);
}

inline float ExposureScale(const CameraExposure& value)
{
    // 本轮明确采用 saturation-based 标定 1.2，不宣称它是所有 UE 版本的默认值。
    // 正补偿增加曝光；与相机 EV 不同，不能放在分母里同号相加。
    return static_cast<float>(std::exp2(value.compensation - EffectiveEv100(value)) / 1.2);
}

inline void ValidateCameraExposure(const CameraExposure& value)
{
    if ((value.mode != ExposureMode::Ev100 && value.mode != ExposureMode::PhysicalCamera &&
         value.mode != ExposureMode::Auto) ||
        !std::isfinite(value.ev100) || value.ev100 < -24 || value.ev100 > 32 ||
        !std::isfinite(value.aperture) || value.aperture < 0.1 || value.aperture > 128 ||
        !std::isfinite(value.shutterSeconds) || value.shutterSeconds < 0.000001 ||
        value.shutterSeconds > 3600 ||
        !std::isfinite(value.iso) || value.iso < 1 || value.iso > 10000000 ||
        !std::isfinite(value.compensation) || value.compensation < -24 || value.compensation > 24 ||
        !std::isfinite(value.autoMinEv100) || !std::isfinite(value.autoMaxEv100) ||
        value.autoMinEv100 < -24 || value.autoMaxEv100 > 32 ||
        value.autoMinEv100 >= value.autoMaxEv100 ||
        !std::isfinite(value.autoSpeedUp) || value.autoSpeedUp <= 0 || value.autoSpeedUp > 32 ||
        !std::isfinite(value.autoSpeedDown) || value.autoSpeedDown <= 0 || value.autoSpeedDown > 32 ||
        !std::isfinite(value.autoTargetGray) || value.autoTargetGray <= 0 || value.autoTargetGray > 1)
        throw std::invalid_argument("Invalid camera exposure parameter/range");
}

inline CameraExposure ReadCameraExposure(const nlohmann::json& camera)
{
    if (!camera.contains("exposure") || !camera.at("exposure").is_object())
        throw std::invalid_argument("Camera requires explicit exposure settings");
    const auto& node = camera.at("exposure");
    for (const auto& item : node.items())
        if (item.key() != "mode" && item.key() != "ev100" && item.key() != "aperture" &&
            item.key() != "shutterSeconds" && item.key() != "iso" && item.key() != "compensation" &&
            item.key() != "autoMinEv100" && item.key() != "autoMaxEv100" &&
            item.key() != "autoSpeedUp" && item.key() != "autoSpeedDown" &&
            item.key() != "autoTargetGray")
            throw std::invalid_argument("Unknown exposure field: " + item.key());
    CameraExposure value;
    const auto mode = node.at("mode").get<std::string>();
    if (mode != "ev100" && mode != "physical" && mode != "auto")
        throw std::invalid_argument("Exposure mode must be ev100, physical, or auto");
    value.mode = mode == "ev100" ? ExposureMode::Ev100 :
        mode == "physical" ? ExposureMode::PhysicalCamera : ExposureMode::Auto;
    value.ev100 = node.at("ev100").get<double>();
    value.aperture = node.at("aperture").get<double>();
    value.shutterSeconds = node.at("shutterSeconds").get<double>();
    value.iso = node.at("iso").get<double>();
    value.compensation = node.at("compensation").get<double>();
    value.autoMinEv100 = node.value("autoMinEv100", value.autoMinEv100);
    value.autoMaxEv100 = node.value("autoMaxEv100", value.autoMaxEv100);
    value.autoSpeedUp = node.value("autoSpeedUp", value.autoSpeedUp);
    value.autoSpeedDown = node.value("autoSpeedDown", value.autoSpeedDown);
    value.autoTargetGray = node.value("autoTargetGray", value.autoTargetGray);
    ValidateCameraExposure(value);
    return value;
}

inline nlohmann::json DescribeCameraExposure(const CameraExposure& value)
{
    return {{"mode", value.mode == ExposureMode::Ev100 ? "ev100" :
                value.mode == ExposureMode::PhysicalCamera ? "physical" : "auto"},
            {"ev100", value.ev100}, {"aperture", value.aperture},
            {"shutterSeconds", value.shutterSeconds}, {"iso", value.iso},
            {"compensation", value.compensation},
            {"autoMinEv100", value.autoMinEv100},
            {"autoMaxEv100", value.autoMaxEv100},
            {"autoSpeedUp", value.autoSpeedUp},
            {"autoSpeedDown", value.autoSpeedDown},
            {"autoTargetGray", value.autoTargetGray}};
}

inline CameraExposure EditCameraExposure(CameraExposure value, const std::string& field, double number)
{
    if (field == "mode")
    {
        if (number != 0 && number != 1 && number != 2)
            throw std::invalid_argument("Exposure mode must be 0 (ev100), 1 (physical), or 2 (auto)");
        value.mode = number == 0 ? ExposureMode::Ev100 :
            number == 1 ? ExposureMode::PhysicalCamera : ExposureMode::Auto;
    }
    else if (field == "ev100") value.ev100 = number;
    else if (field == "aperture") value.aperture = number;
    else if (field == "shutterSeconds") value.shutterSeconds = number;
    else if (field == "iso") value.iso = number;
    else if (field == "compensation") value.compensation = number;
    else if (field == "autoMinEv100") value.autoMinEv100 = number;
    else if (field == "autoMaxEv100") value.autoMaxEv100 = number;
    else if (field == "autoSpeedUp") value.autoSpeedUp = number;
    else if (field == "autoSpeedDown") value.autoSpeedDown = number;
    else if (field == "autoTargetGray") value.autoTargetGray = number;
    else throw std::invalid_argument("Unknown exposure parameter: " + field);
    ValidateCameraExposure(value);
    return value;
}
}
