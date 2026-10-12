#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "render/camera/cameraExposure.h"

namespace VL
{
inline constexpr uint32_t AutoExposureBinCount = 64;
inline constexpr uint32_t AutoExposureLocalSize = 16;
inline constexpr float AutoExposureMinLogLuminance = -12.0f;
inline constexpr float AutoExposureMaxLogLuminance = 16.0f;

// 冻结相机到 std140 dispatch 参数的唯一转换；只管理记录序列，不读回 GPU 曝光。
struct alignas(16) AutoExposureDispatchParams
{
    std::array<float, 4> range{};
    std::array<float, 4> adaptation{};
    std::array<uint32_t, 4> control{};
    std::array<float, 4> manual{};
};
static_assert(sizeof(AutoExposureDispatchParams) == 64);
static_assert(offsetof(AutoExposureDispatchParams, adaptation) == 16);
static_assert(offsetof(AutoExposureDispatchParams, control) == 32);
static_assert(offsetof(AutoExposureDispatchParams, manual) == 48);

inline uint32_t AutoExposureDispatchGroups(uint32_t size)
{
    return size / AutoExposureLocalSize + (size % AutoExposureLocalSize != 0);
}

// 序列只在实时帧提交后推进，绝不以 frame-slot 或 imageIndex 推算历史。
// 新图/resize 和退出 Auto 后再次进入 Auto，首帧直接采用目标，不访问未初始化槽。
class AutoExposureHistorySequence
{
public:
    AutoExposureDispatchParams Prepare(
        const CameraExposure& camera, float deltaSeconds) const
    {
        AutoExposureDispatchParams params;
        params.range = {
            static_cast<float>(camera.autoTargetGray),
            static_cast<float>(std::exp2(-camera.autoMaxEv100) / 1.2),
            static_cast<float>(std::exp2(-camera.autoMinEv100) / 1.2),
            static_cast<float>(camera.compensation)};
        params.adaptation = {
            static_cast<float>(camera.autoSpeedUp),
            static_cast<float>(camera.autoSpeedDown), deltaSeconds, 0.0f};
        const bool automatic = camera.mode == ExposureMode::Auto;
        params.control = {automatic ? 1u : 0u, automatic && valid ? 1u : 0u,
            nextSlot ^ 1u, nextSlot};
        params.manual[0] = ExposureScale(camera);
        return params;
    }

    void Advance(ExposureMode mode) noexcept
    {
        valid = mode == ExposureMode::Auto;
        nextSlot ^= 1u;
    }

private:
    uint32_t nextSlot = 0;
    bool valid = false;
};
}
