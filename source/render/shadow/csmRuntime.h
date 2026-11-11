#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "render/shadow/csmSettings.h"
#include "render/shadow/shadowCasterCull.h"

namespace VL
{

struct RenderScene;

// 阴影计算策略
enum class ShadowStrategy
{
    DynamicTightBox,      // 原始方案：最小面积矩形 (利用率高，闪烁)
    StableBoundingSphere, // 基础稳定方案：外接球 (利用率低，极其稳定)
    StableRectangular     // 改良方案：稳定长方形 (利用率中，稳定)
};

struct ShadowProjectionParams
{
    Eigen::Matrix4f viewMatrix;
    Eigen::Matrix4f projectionMatrix;
    // 该级联接收面（相机 split）的光源空间 XY 范围，供 caster 预筛。
    VL::ShadowCasterReceiverBounds casterReceiverBounds;
};

struct ShadowCascadeFrameData
{
    std::array<Eigen::Matrix4f, 4> view{};
    std::array<Eigen::Matrix4f, 4> projection{};
    std::array<Eigen::Matrix4f, 4> lightViewProj{};
    // 逐级联接收面 XY 范围；与 lightViewProj 同帧同源。
    std::array<VL::ShadowCasterReceiverBounds, 4> casterReceiverBounds{};
    Eigen::Vector4f cascadeSplits = Eigen::Vector4f::Zero();
    std::array<Eigen::Vector4f, 4> bias{};
    Eigen::Vector4f csmParameters = Eigen::Vector4f::Zero();
    uint64_t frameIndex = std::numeric_limits<uint64_t>::max();
    uint64_t worldGeneration = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;
};

// caster 在光源空间的包围盒。投影只取决于光源朝向，同一帧的四个级联完全相同；
// 因此每帧投影一次缓存，逐级联只做 XY 过滤与 Z 合并，避免 N 级 × 全部 draw 的重复计算。
struct ShadowCasterLightSpaceBounds
{
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    float minZ = 0.0f;
    float maxZ = 0.0f;
};

// Owns the GT-authoritative CSM settings, the RT per-frame snapshot and the
// per-frame cascade cache. Synchronization is unchanged:
// - GT writers take csmSettingsMutex and bump csmSettingsRevision.
// - RT calls SnapshotFrameSettings() at frame start and only reads frameCsmSettings.
class CsmRuntime
{
public:
    void SetSettings(const CsmSettings& settings);
    CsmSettings GetSettings() const;
    void SnapshotFrameSettings();

    bool SetCastShadows(bool enabled, std::string& outMessage);
    bool SetDynamicShadowDistance(float distance, std::string& outMessage);
    bool SetCascadeCount(uint32_t cascadeCount, std::string& outMessage);
    bool SetCascadeDistributionExponent(float exponent, std::string& outMessage);
    bool SetCascadeTransitionFraction(float fraction, std::string& outMessage);
    bool SetShadowDistanceFadeoutFraction(float fraction, std::string& outMessage);
    bool SetShadowBias(float value, std::string& outMessage);
    bool SetShadowSlopeBias(float value, std::string& outMessage);
    bool SetShadowCascadeBiasDistribution(float value, std::string& outMessage);

    const CsmSettings& GetFrameSettings() const noexcept
    {
        return frameCsmSettings;
    }

    bool IsEnabled() const noexcept;
    bool IsCascadeActive(uint32_t cascadeIndex) const noexcept;
    bool GetCascadeLightViewProjection(
        uint32_t cascadeIndex,
        Eigen::Matrix4f& outLightViewProjection) const;
    bool GetCascadeCasterReceiverBounds(
        uint32_t cascadeIndex,
        VL::ShadowCasterReceiverBounds& outBounds) const;

    void BuildFrameData(
        const RenderScene& renderScene,
        uint64_t frameIndex,
        uint32_t passSizeWidth,
        uint32_t passSizeHeight);

    const ShadowCascadeFrameData& GetFrameData() const noexcept
    {
        return shadowCascadeFrameData;
    }

private:
    ShadowProjectionParams CalculateShadowMatrixForCameraRange(
        const RenderScene& renderScene,
        float splitNear,
        float splitFar,
        uint32_t passSizeWidth,
        uint32_t passSizeHeight);
    std::optional<std::pair<float, float>> ComputeCascadeLightSpaceZBounds(
        const RenderScene& renderScene,
        const Eigen::Matrix3f& worldToShadowMatrix,
        float minX,
        float maxX,
        float minY,
        float maxY);
    ShadowProjectionParams CalculateShadowMatrix_DynamicTight(
        const std::vector<Eigen::Vector3f>& pointsInShadowSys,
        const Eigen::Matrix3f& worldToShadowRotation,
        float sceneMaxZ,
        float sceneZRange);
    ShadowProjectionParams CalculateShadowMatrix_StableSphere(
        const std::vector<Eigen::Vector3f>& pointsInShadowSys,
        const Eigen::Matrix3f& worldToShadowRotation,
        float shadowMapResolution,
        float sceneMaxZ,
        float sceneZRange);
    ShadowProjectionParams CalculateShadowMatrix_StableRectangular(
        const std::vector<Eigen::Vector3f>& pointsInShadowSys,
        const Eigen::Matrix3f& worldToShadowRotation,
        float shadowMapResolution,
        float sceneMaxZ,
        float sceneZRange);

    // GT authority. Never read this field directly from RT code; RT consumes a
    // per-frame snapshot into frameCsmSettings at frame start.
    VL::CsmSettings csmSettings;
    mutable std::mutex csmSettingsMutex;
    std::atomic<uint64_t> csmSettingsRevision{0};
    // RT-owned per-frame snapshot; written only by SnapshotFrameSettings().
    VL::CsmSettings frameCsmSettings;
    uint64_t frameCsmSettingsRevision = 0;
    bool frameCsmSettingsInitialized = false;
    ShadowCascadeFrameData shadowCascadeFrameData;
    std::vector<ShadowCasterLightSpaceBounds> casterLightSpaceBounds;
    bool casterLightSpaceBoundsValid = false;
};

// 用于使用 boundingbox 加速
std::pair<float, float> ComputeMinMaxAlongAxis(
    const Eigen::Vector3f& aabbMin,
    const Eigen::Vector3f& aabbMax,
    const Eigen::Vector3f& axis);
// 将局部空间的 aabb 变换到世界空间
std::array<Eigen::Vector3f, 8> BuildWorldCorners(
    const Eigen::Vector3f& localMin,
    const Eigen::Vector3f& localMax,
    const Eigen::Matrix4f& modelMatrix);
// 计算某个空间下的 aabb
void ComputeAabbFromCorners(
    const std::array<Eigen::Vector3f, 8>& corners,
    Eigen::Vector3f& outMin,
    Eigen::Vector3f& outMax);

} // namespace VL
