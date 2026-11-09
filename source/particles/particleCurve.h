#pragma once

#include <cmath>
#include <string_view>

#include <Eigen/Dense>

namespace VL
{

// 作者曲线插值模式。GPU 只实现 Hermite；Bezier 在 CPU 上传边界按
// m = 3 * handle 转成 Hermite 斜率，避免 CPU/GPU 两套数学。
enum class ParticleCurveInterpolation
{
    Linear = 0,
    Hermite = 1,
    Bezier = 2
};

inline bool ParseParticleCurveInterpolation(
    std::string_view text,
    ParticleCurveInterpolation& result)
{
    if (text == "linear")
    {
        result = ParticleCurveInterpolation::Linear;
        return true;
    }
    if (text == "hermite")
    {
        result = ParticleCurveInterpolation::Hermite;
        return true;
    }
    if (text == "bezier")
    {
        result = ParticleCurveInterpolation::Bezier;
        return true;
    }
    return false;
}

inline const char* ParticleCurveInterpolationName(
    ParticleCurveInterpolation interpolation)
{
    switch (interpolation)
    {
    case ParticleCurveInterpolation::Hermite:
        return "hermite";
    case ParticleCurveInterpolation::Bezier:
        return "bezier";
    case ParticleCurveInterpolation::Linear:
    default:
        return "linear";
    }
}

inline int ParticleCurveInterpolationCode(
    ParticleCurveInterpolation interpolation)
{
    return static_cast<int>(interpolation);
}

// 作者切线的语义：
//   hermite: tangentIn/tangentOut 是 dValue/dTime（归一化时间）；
//   bezier : tangentIn/tangentOut 是控制柄相对端点的偏移，
//            即 c1 = p0 + tangentOut、c2 = p1 - tangentIn。
// 两种模式最终上传的都是 Hermite 斜率：
//   hermite 直接使用，bezier 使用 3 * handle（m0 = 3*(c1-p0)、
//   m1 = 3*(p1-c2)）。linear 不读取切线。
inline float ParticleCurveTangentScale(
    ParticleCurveInterpolation interpolation)
{
    switch (interpolation)
    {
    case ParticleCurveInterpolation::Hermite:
        return 1.0f;
    case ParticleCurveInterpolation::Bezier:
        return 3.0f;
    case ParticleCurveInterpolation::Linear:
    default:
        return 0.0f;
    }
}

// Hermite 基函数；u 是段内 [0,1] 参数，span 是归一化时间跨度。
// GPU 端 particleCommon.glsl 的 ParticleHermite* 必须保持同一公式。
inline float ParticleHermiteScalar(
    float p0,
    float p1,
    float m0,
    float m1,
    float u,
    float span)
{
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    return h00 * p0 + h10 * span * m0 +
        h01 * p1 + h11 * span * m1;
}

inline Eigen::Vector4f ParticleHermiteVector4(
    const Eigen::Vector4f& p0,
    const Eigen::Vector4f& p1,
    const Eigen::Vector4f& m0,
    const Eigen::Vector4f& m1,
    float u,
    float span)
{
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    return h00 * p0 + h10 * span * m0 +
        h01 * p1 + h11 * span * m1;
}

} // namespace VL
