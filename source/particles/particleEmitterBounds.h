#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include <Eigen/Dense>

#include "particles/particleAsset.h"

namespace VL
{

// Hermite 会超出端点值的包络（例如 size 曲线 0 -> 1 带正切线时中段可
// 能 >1）。剔除半径必须按采样最大值而不是只看 key 值，否则大尺寸粒子
// 会被提前剔除。linear 路径保持原“逐 key 取最大”的字节等价行为。
struct ParticleScalarCurveSample
{
    float time = 0.0f;
    float value = 0.0f;
    float tangentIn = 0.0f;
    float tangentOut = 0.0f;
};

inline float MaxParticleScalarCurveValue(
    const std::vector<ParticleScalarCurveSample>& keys,
    ParticleCurveInterpolation interpolation,
    float initial)
{
    float maximum = initial;
    for (const ParticleScalarCurveSample& key : keys)
    {
        maximum = std::max(maximum, key.value);
    }
    if (keys.size() < 2 ||
        interpolation == ParticleCurveInterpolation::Linear)
    {
        return maximum;
    }
    const float tangentScale =
        ParticleCurveTangentScale(interpolation);
    // 每段 16 个等距样本；Hermite 是三次多项式，这个密度对包围球
    // 的保守性足够，且只发生在资产/发射器重建路径，不在逐帧热点。
    constexpr int samplesPerSegment = 16;
    for (size_t index = 0; index + 1 < keys.size(); ++index)
    {
        const ParticleScalarCurveSample& left = keys[index];
        const ParticleScalarCurveSample& right = keys[index + 1];
        const float span = right.time - left.time;
        if (!(span > 0.0f))
        {
            continue;
        }
        for (int sample = 0; sample <= samplesPerSegment; ++sample)
        {
            const float u = static_cast<float>(sample) /
                static_cast<float>(samplesPerSegment);
            maximum = std::max(
                maximum,
                ParticleHermiteScalar(
                    left.value,
                    right.value,
                    left.tangentOut * tangentScale,
                    right.tangentIn * tangentScale,
                    u,
                    span));
        }
    }
    return maximum;
}

inline float EvaluateParticleScalarCurveValue(
    const std::vector<ParticleScalarCurveSample>& keys,
    ParticleCurveInterpolation interpolation,
    float t)
{
    if (keys.empty())
    {
        return 0.0f;
    }
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    if (clamped <= keys.front().time)
    {
        return keys.front().value;
    }
    if (clamped >= keys.back().time)
    {
        return keys.back().value;
    }
    const float tangentScale =
        ParticleCurveTangentScale(interpolation);
    for (size_t index = 0; index + 1 < keys.size(); ++index)
    {
        const ParticleScalarCurveSample& left = keys[index];
        const ParticleScalarCurveSample& right = keys[index + 1];
        if (clamped < left.time || clamped > right.time)
        {
            continue;
        }
        const float span = right.time - left.time;
        if (!(span > 1.0e-6f))
        {
            return right.value;
        }
        const float u = (clamped - left.time) / span;
        if (interpolation == ParticleCurveInterpolation::Linear)
        {
            return left.value +
                (right.value - left.value) * u;
        }
        return ParticleHermiteScalar(
            left.value,
            right.value,
            left.tangentOut * tangentScale,
            right.tangentIn * tangentScale,
            u,
            span);
    }
    return keys.back().value;
}

// 发射器包围球半径的纯 CPU 估算。剔除、距离淡出、minScreenSize 与跨发射器
// 透明排序都消费同一个值，因此必须把“粒子能走多远”和“billboard 有多大”
// 一起算进去，否则大尺寸低速烟雾会提前消失。
inline float ComputeParticleEmitterLocalRadius(
    const ParticleEmitterAsset& asset,
    float meshLocalBoundsRadius = 0.0f)
{
    // LOD renderScale 可能放大视觉尺寸；包围球按最大档位保守外扩。
    float maxLodRenderScale = 1.0f;
    for (const ParticleLodTier& tier : asset.lodTiers)
    {
        maxLodRenderScale = std::max(
            maxLodRenderScale, tier.renderScale);
    }
    // beam 不消费粒子池；包围球只需覆盖端点、横向半宽、噪声包络和
    // 端点抖动。继续叠加寿命/速度/重力会得到与画面无关的巨大球体。
    if (asset.renderMode == "beam")
    {
        const float mainLength =
            (asset.beam.end - asset.beam.start).norm();
        // 分支从主弧内部出发，最长可再走 branchLength * 主线长度，
        // 末端还有横向 spread；root 本身仍落在线段内，不会超出半长。
        const float branchReach =
            asset.beam.branchCount > 0
                ? std::max(asset.beam.branchLength, 0.0f) *
                        mainLength +
                    std::max(asset.beam.branchSpread, 0.0f) +
                    // 分支端点也带同一 jitter；两个端点各自外扩，
                    // 因此再计一份两端的抖动余量。
                    2.0f *
                        std::max(asset.beam.branchLength, 0.0f) *
                        std::sqrt(3.0f) *
                        std::max(asset.beam.endpointJitter, 0.0f)
                : 0.0f;
        std::vector<ParticleScalarCurveSample> beamWidthKeys;
        beamWidthKeys.reserve(asset.beam.widthKeys.size());
        for (const ParticleBeamWidthKey& key : asset.beam.widthKeys)
        {
            beamWidthKeys.push_back({
                key.time,
                key.width,
                key.tangentIn,
                key.tangentOut});
        }
        const float beamWidthScale = MaxParticleScalarCurveValue(
            beamWidthKeys, asset.curveInterpolation, 1.0f);
        return 0.5f *
                mainLength +
            branchReach +
            // 两个正交噪声波最坏可叠加成 sqrt(2) 倍幅度；
            // 端点抖动是三个独立轴，最坏 sqrt(3) 倍。
            std::sqrt(2.0f) *
                std::max(asset.beam.noiseAmplitude, 0.0f) +
            std::max(asset.beam.width, 0.0f) *
                std::max(1.0f, beamWidthScale) *
                maxLodRenderScale +
            std::sqrt(3.0f) *
                std::max(asset.beam.endpointJitter, 0.0f);
    }

    // 速度区间逐轴取绝对值最大者再求范数：非对称区间
    // (例如 [-400, 20]) 也必须按最远可达速度外扩。
    const Eigen::Vector3f maxSpeed(
        std::max(
            std::abs(asset.velocityMin.x()),
            std::abs(asset.velocityMax.x())),
        std::max(
            std::abs(asset.velocityMin.y()),
            std::abs(asset.velocityMax.y())),
        std::max(
            std::abs(asset.velocityMin.z()),
            std::abs(asset.velocityMax.z())));

    float maxBillboardHalfSize =
        std::max(asset.sizeStart, asset.sizeEnd);
    std::vector<ParticleScalarCurveSample> sizeKeys;
    sizeKeys.reserve(asset.sizeKeys.size());
    for (const ParticleSizeKey& key : asset.sizeKeys)
    {
        sizeKeys.push_back({
            key.time,
            key.size,
            key.tangentIn,
            key.tangentOut});
    }
    maxBillboardHalfSize = MaxParticleScalarCurveValue(
        sizeKeys, asset.curveInterpolation, maxBillboardHalfSize);
    const bool meshVisual =
        asset.renderMode == "mesh" ||
        asset.renderMode == "skeletalMeshTris" ||
        asset.renderMode == "skeletalMeshBones";
    float meshSizeFactor = 1.0f;
    if (meshVisual && asset.meshUseSizeCurve)
    {
        const float baseSize = sizeKeys.empty()
            ? std::max(asset.sizeStart, 1.0e-4f)
            : EvaluateParticleScalarCurveValue(
                sizeKeys, asset.curveInterpolation, 0.0f);
        const float maxSize = sizeKeys.empty()
            ? std::max(asset.sizeStart, asset.sizeEnd)
            : MaxParticleScalarCurveValue(
                sizeKeys, asset.curveInterpolation, 0.0f);
        meshSizeFactor = std::max(
            1.0f,
            maxSize / std::max(baseSize, 1.0e-4f) *
                asset.meshSizeScale);
    }

    const float lifetime = std::max(asset.lifetimeMax, 0.0f);
    // ribbon 条带在切线两侧各外扩 ribbonWidth；历史点都在运动轨迹内，
    // 因此只需要额外加上横向半宽。mesh 的视觉半径来自导入网格顶点，
    // 而不是 billboard size 曲线。
    float ribbonHalfWidth = 0.0f;
    if (asset.renderMode == "ribbon")
    {
        std::vector<ParticleScalarCurveSample> widthKeys;
        widthKeys.reserve(asset.ribbonWidthKeys.size());
        for (const ParticleRibbonWidthKey& key : asset.ribbonWidthKeys)
        {
            widthKeys.push_back({
                key.time,
                key.width,
                key.tangentIn,
                key.tangentOut});
        }
        const float maxWidthScale = MaxParticleScalarCurveValue(
            widthKeys, asset.curveInterpolation, 1.0f);
        ribbonHalfWidth =
            std::max(asset.ribbonWidth, 0.0f) *
            maxWidthScale * maxLodRenderScale *
            (1.0f + std::abs(asset.ribbonPivot));
    }
    const float visualRadius =
        (meshVisual && meshLocalBoundsRadius > 0.0f
        ? std::max(meshLocalBoundsRadius, 0.0f)
        : maxBillboardHalfSize) * maxLodRenderScale *
            (meshVisual ? meshSizeFactor : 1.0f);
    return asset.shapeRadius +
        lifetime *
            (maxSpeed.norm() +
             0.5f * asset.gravity.norm() * lifetime) +
        visualRadius +
        ribbonHalfWidth;
}

inline float ComputeParticleEmitterBoundsRadius(
    const ParticleEmitterAsset& asset,
    const Eigen::Matrix4f& model,
    float meshLocalBoundsRadius = 0.0f)
{
    const Eigen::Matrix3f linear = model.block<3, 3>(0, 0);
    float maximumScale = 0.0f;
    for (int column = 0; column < 3; ++column)
    {
        maximumScale =
            std::max(maximumScale, linear.col(column).norm());
    }
    return ComputeParticleEmitterLocalRadius(
        asset, meshLocalBoundsRadius) * maximumScale;
}

// 统一的发射器世界包围球：剔除、距离淡出、minScreenSize 与跨发射器透明
// 排序都消费这一份 center/radius。fixed 模式仍以发射器局部空间作者化，
// 由 model 变换到世界；半径使用半尺寸向量长度近似外接球。
inline void ComputeParticleEmitterBounds(
    const ParticleEmitterAsset& asset,
    const Eigen::Matrix4f& model,
    Eigen::Vector3f& worldCenter,
    float& worldRadius,
    float meshLocalBoundsRadius = 0.0f)
{
    const Eigen::Matrix3f linear = model.block<3, 3>(0, 0);
    float maximumScale = 0.0f;
    for (int column = 0; column < 3; ++column)
    {
        maximumScale =
            std::max(maximumScale, linear.col(column).norm());
    }

    Eigen::Vector3f localCenter = Eigen::Vector3f::Zero();
    if (asset.useFixedBounds)
    {
        localCenter = asset.fixedBoundsCenter;
    }
    else if (asset.renderMode == "beam")
    {
        // beam 的自动包围球必须围绕线段中点，而不是发射器原点；
        // 否则跨屏幕的 Tesla 弧会被按原点剔除。
        localCenter = (asset.beam.start + asset.beam.end) * 0.5f;
    }
    worldCenter =
        (model * Eigen::Vector4f(localCenter.x(),
             localCenter.y(), localCenter.z(), 1.0f)).head<3>();
    const float localRadius = asset.useFixedBounds
        ? asset.fixedBoundsExtent.norm()
        : ComputeParticleEmitterLocalRadius(
            asset, meshLocalBoundsRadius);
    worldRadius = localRadius * maximumScale;
}

} // namespace VL
