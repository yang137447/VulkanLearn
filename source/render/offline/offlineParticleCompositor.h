#pragma once

#include <cstdint>
#include <vector>

#include "render/offline/core/renderSession.h"
#include "render/offline/offlineParticleSimulation.h"
#include "world/worldSnapshot.h"

namespace VL
{

// 复用 WorldSnapshot 的相机约定：viewProjection 已包含 Vulkan NDC 的 Y 翻转，
// projection(1,1) 与 GPU 的 1/abs(invProjection[1][1]) 等价。
struct OfflineParticleCamera
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f right = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    Eigen::Vector3f up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    Eigen::Vector3f forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f projection = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f viewProjection = Eigen::Matrix4f::Identity();
    float clipNear = 0.1f;
    float clipFar = 1000.0f;
    int width = 0;
    int height = 0;
};

OfflineParticleCamera MakeOfflineParticleCamera(
    const CameraSnapshot& camera,
    int width,
    int height);

// 预乘颜色 + 背景可见比例。合成公式：
//   final = particleColor + background * transmittance
// alpha 图层累乘 transmittance；additive 图层只累加颜色，不遮挡背景。
struct OfflineParticleOverlay
{
    int width = 0;
    int height = 0;
    std::vector<Offline::Vec3> color;
    std::vector<float> transmittance;

    bool Empty() const noexcept
    {
        return width <= 0 || height <= 0 || color.empty();
    }
};

// CPU 软件合成：billboard 与 ribbon 走真实粒子路径；mesh 渲染模式在离线
// 边界内按 billboard 处理。flipbook 纹理、软粒子深度与场景遮挡不在
// 离线合成的输入范围内，使用程序化圆盘/条带形状。
OfflineParticleOverlay CompositeOfflineParticles(
    const std::vector<OfflineParticleSystemState>& systems,
    const OfflineParticleCamera& camera);

std::vector<Offline::Vec3> CompositeOfflineParticleOverBackground(
    const std::vector<Offline::Vec3>& background,
    const OfflineParticleOverlay& overlay);

// 供离线显示/导出消费；几何 AOV 保持不变，只返回合成后的组合颜色。
std::vector<Offline::Vec3> CompositeOfflineFilmPixels(
    const Offline::FilmSnapshot& film,
    const OfflineParticleOverlay& overlay);

// 从冻结 WorldSnapshot 构建并推进粒子，再合成到离线尺寸。
OfflineParticleOverlay BuildOfflineParticleOverlay(
    const WorldSnapshot& snapshot,
    const OfflineParticlePreviewPolicy& policy,
    int width,
    int height,
    uint32_t seed);

} // namespace VL
