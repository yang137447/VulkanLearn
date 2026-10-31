#pragma once

#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "hair/tressfxRootCloud.h"

namespace VL::Hair
{

// 与 UiViewportRect / UiCameraProjection 同值的纯数据副本，避免 CPU 组件依赖 RmlUi。
struct BrushViewport
{
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct BrushCamera
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    Eigen::Vector3f right = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    Eigen::Vector3f up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    float horizontalFovDegrees = 60.0f;
    float clipNear = 0.1f;
    float clipFar = 1000.0f;
};

struct BrushSettings
{
    // UI 手感参数：只用于换算 r_world，不参与命中判定。
    float radiusPixels = 20.0f;
    // 发根法线与笔刷平面法线的最大夹角；normalGate 的主判据。
    float maxNormalAngleDegrees = 60.0f;
    // 0 表示由根点云 cellSize 自动推导薄层厚度。
    float planeThicknessWorld = 0.0f;
    // 0 表示由根点云 cellSize 自动推导 ray tube 半径。
    float planeSearchRadiusWorld = 0.0f;
    float selectionThreshold = 0.05f;
    float maxRayDistanceWorld = 100000.0f;
    size_t maxPlaneCandidates = 256;
};

struct BrushStamp
{
    bool valid = false;
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f normal = Eigen::Vector3f::UnitY();
    Eigen::Vector3f rayDirection = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    float radiusWorld = 0.0f;
    float depth = 0.0f;
    // true 表示法线退化为 -viewDir / 最近根点，UI 必须显示降级提示。
    bool fallback = false;
    uint32_t candidateCount = 0;
};

// 屏幕坐标只负责生成 camera ray；拟合平面、落点、半径全部在世界空间。
class SurfaceBrush
{
public:
    BrushStamp Sample(
        const Eigen::Vector2f& cursor,
        const BrushViewport& viewport,
        const BrushCamera& camera,
        const TressfxRootCloud& roots,
        const Eigen::Matrix4f& rootLocalToWorld,
        const BrushSettings& settings) const;
};

struct RootHit
{
    uint32_t guideIndex = 0;
    float weight = 0.0f;
};

// normalGate 是主判据；planeGate 限制薄层；discGate 使用平面内世界距离。
// rootGrowthDir 不参与普通分组笔刷，留给后续 comb 模式。
class RootCloudSelector
{
public:
    std::vector<RootHit> Select(
        const TressfxRootCloud& roots,
        const BrushStamp& stamp,
        const Eigen::Matrix4f& rootLocalToWorld,
        const BrushSettings& settings) const;

    // 套索：屏幕多边形先投到 brush plane，再把发根投到同一平面做 point-in-polygon。
    // 选择度量完全在世界平面内，不使用屏幕距离；normalGate/planeGate 与圆形笔刷一致。
    std::vector<RootHit> SelectLasso(
        const TressfxRootCloud& roots,
        const BrushStamp& stamp,
        const Eigen::Matrix4f& rootLocalToWorld,
        const BrushViewport& viewport,
        const BrushCamera& camera,
        const std::vector<Eigen::Vector2f>& screenPolygon,
        const BrushSettings& settings) const;
};

} // namespace VL::Hair
