#include "render/offline/offlineParticleCompositor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "particles/particleEmitterCulling.h"
#include "particles/particleEmitterOrder.h"
#include "particles/particleLimits.h"
#include "particles/particleRibbon.h"

namespace VL
{
namespace
{

// 每个粒子固定的黄金分割相位；与 GPU 顶点/片元的 vSeed 一致。
constexpr float kParticlePhaseStep = 0.61803398875f;

float Mix(float first, float second, float weight)
{
    return first + (second - first) * weight;
}

Eigen::Vector4f Mix(
    const Eigen::Vector4f& first,
    const Eigen::Vector4f& second,
    float weight)
{
    return first + (second - first) * weight;
}

float Fract(float value)
{
    return value - std::floor(value);
}

float SmoothStep(float edge0, float edge1, float value)
{
    if (!(edge1 > edge0))
    {
        return value >= edge1 ? 1.0f : 0.0f;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0),
        0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 与 particle.frag / particleRibbon.frag 的 Hash21 / ValueNoise / Fbm
// 逐行一致；只使用 floor/fract/mix/smoothstep，跨平台保持确定性。
float Hash21(const Eigen::Vector2f& point)
{
    Eigen::Vector2f p(
        Fract(point.x() * 123.34f),
        Fract(point.y() * 456.21f));
    const float offset = p.x() * (p.x() + 45.32f) +
        p.y() * (p.y() + 45.32f);
    p.x() += offset;
    p.y() += offset;
    return Fract(p.x() * p.y());
}

float ValueNoise2D(const Eigen::Vector2f& point)
{
    const Eigen::Vector2f cell(std::floor(point.x()), std::floor(point.y()));
    const Eigen::Vector2f raw(
        Fract(point.x()), Fract(point.y()));
    const Eigen::Vector2f fraction(
        raw.x() * raw.x() * (3.0f - 2.0f * raw.x()),
        raw.y() * raw.y() * (3.0f - 2.0f * raw.y()));
    const float a = Hash21(cell);
    const float b = Hash21(cell + Eigen::Vector2f(1.0f, 0.0f));
    const float c = Hash21(cell + Eigen::Vector2f(0.0f, 1.0f));
    const float d = Hash21(cell + Eigen::Vector2f(1.0f, 1.0f));
    return Mix(
        Mix(a, b, fraction.x()),
        Mix(c, d, fraction.x()),
        fraction.y());
}

float Fbm2D(const Eigen::Vector2f& point)
{
    float value = 0.0f;
    float amplitude = 0.55f;
    Eigen::Vector2f p = point;
    for (int octave = 0; octave < 4; ++octave)
    {
        value += ValueNoise2D(p) * amplitude;
        p = p * 2.03f + Eigen::Vector2f(17.1f, 9.2f);
        amplitude *= 0.5f;
    }
    return value;
}

size_t ColorKeyCount(const ParticleEmitterAsset& asset)
{
    return asset.colorKeys.empty()
        ? 2
        : std::min<size_t>(asset.colorKeys.size(), kMaxParticleCurveKeys);
}

float ColorKeyTime(const ParticleEmitterAsset& asset, size_t index)
{
    if (asset.colorKeys.empty())
    {
        return index == 0 ? 0.0f : 1.0f;
    }
    return asset.colorKeys[index].time;
}

Eigen::Vector4f ColorKeyValue(
    const ParticleEmitterAsset& asset,
    size_t index)
{
    if (asset.colorKeys.empty())
    {
        return index == 0 ? asset.colorStart : asset.colorEnd;
    }
    return asset.colorKeys[index].color;
}

Eigen::Vector4f EvaluateColorCurve(
    const ParticleEmitterAsset& asset,
    float t)
{
    const size_t count = ColorKeyCount(asset);
    if (count <= 1)
    {
        return ColorKeyValue(asset, 0);
    }
    if (t <= ColorKeyTime(asset, 0))
    {
        return ColorKeyValue(asset, 0);
    }
    if (t >= ColorKeyTime(asset, count - 1))
    {
        return ColorKeyValue(asset, count - 1);
    }
    for (size_t index = 0; index + 1 < count; ++index)
    {
        const float t0 = ColorKeyTime(asset, index);
        const float t1 = ColorKeyTime(asset, index + 1);
        if (t >= t0 && t <= t1)
        {
            return Mix(
                ColorKeyValue(asset, index),
                ColorKeyValue(asset, index + 1),
                (t - t0) / std::max(t1 - t0, 1.0e-5f));
        }
    }
    return ColorKeyValue(asset, count - 1);
}

float SizeKeyTime(const ParticleEmitterAsset& asset, size_t index)
{
    if (asset.sizeKeys.empty())
    {
        return index == 0 ? 0.0f : 1.0f;
    }
    return asset.sizeKeys[index].time;
}

float SizeKeyValue(const ParticleEmitterAsset& asset, size_t index)
{
    if (asset.sizeKeys.empty())
    {
        return index == 0 ? asset.sizeStart : asset.sizeEnd;
    }
    return asset.sizeKeys[index].size;
}

float EvaluateSizeCurve(const ParticleEmitterAsset& asset, float t)
{
    const size_t count = asset.sizeKeys.empty()
        ? 2
        : std::min<size_t>(asset.sizeKeys.size(), kMaxParticleCurveKeys);
    if (count <= 1)
    {
        return SizeKeyValue(asset, 0);
    }
    if (t <= SizeKeyTime(asset, 0))
    {
        return SizeKeyValue(asset, 0);
    }
    if (t >= SizeKeyTime(asset, count - 1))
    {
        return SizeKeyValue(asset, count - 1);
    }
    for (size_t index = 0; index + 1 < count; ++index)
    {
        const float t0 = SizeKeyTime(asset, index);
        const float t1 = SizeKeyTime(asset, index + 1);
        if (t >= t0 && t <= t1)
        {
            return Mix(
                SizeKeyValue(asset, index),
                SizeKeyValue(asset, index + 1),
                (t - t0) / std::max(t1 - t0, 1.0e-5f));
        }
    }
    return SizeKeyValue(asset, count - 1);
}

Eigen::Vector3f SimulationPosition(
    const OfflineParticleEmitterState& emitter,
    const Eigen::Vector4f& positionAge)
{
    if (!emitter.asset.localSpace)
    {
        return positionAge.head<3>();
    }
    return (emitter.model * Eigen::Vector4f(
        positionAge.x(), positionAge.y(), positionAge.z(), 1.0f)).head<3>();
}

Eigen::Vector3f SimulationVelocity(
    const OfflineParticleEmitterState& emitter,
    const Eigen::Vector4f& velocityLife)
{
    if (!emitter.asset.localSpace)
    {
        return velocityLife.head<3>();
    }
    return emitter.model.block<3, 3>(0, 0) * velocityLife.head<3>();
}

float ViewDepth(
    const OfflineParticleCamera& camera,
    const Eigen::Vector3f& worldPosition)
{
    const Eigen::Vector4f viewPosition =
        camera.view * Eigen::Vector4f(
            worldPosition.x(), worldPosition.y(), worldPosition.z(), 1.0f);
    return -viewPosition.z();
}

bool ProjectPoint(
    const OfflineParticleCamera& camera,
    const Eigen::Vector3f& worldPosition,
    Eigen::Vector2f& pixel)
{
    const Eigen::Vector4f clip =
        camera.viewProjection * Eigen::Vector4f(
            worldPosition.x(), worldPosition.y(), worldPosition.z(), 1.0f);
    if (!std::isfinite(clip.w()) || !(clip.w() > 1.0e-6f) ||
        clip.z() < 0.0f || clip.z() > clip.w())
    {
        return false;
    }
    const Eigen::Vector3f ndc = clip.head<3>() / clip.w();
    pixel = Eigen::Vector2f(
        (ndc.x() * 0.5f + 0.5f) * static_cast<float>(camera.width),
        (ndc.y() * 0.5f + 0.5f) * static_cast<float>(camera.height));
    return true;
}

struct OfflineQuadVertex
{
    Eigen::Vector2f pixel = Eigen::Vector2f::Zero();
    // 屏幕四边形上的线性插值属性：billboard 用 quad 局部坐标，
    // ribbon 用 (along, across)。
    Eigen::Vector2f attribute = Eigen::Vector2f::Zero();
};

bool Barycentric(
    const Eigen::Vector2f& point,
    const Eigen::Vector2f& a,
    const Eigen::Vector2f& b,
    const Eigen::Vector2f& c,
    float& weightA,
    float& weightB,
    float& weightC)
{
    const float denominator =
        (b.y() - c.y()) * (a.x() - c.x()) +
        (c.x() - b.x()) * (a.y() - c.y());
    if (std::abs(denominator) < 1.0e-9f)
    {
        return false;
    }
    weightA =
        ((b.y() - c.y()) * (point.x() - c.x()) +
         (c.x() - b.x()) * (point.y() - c.y())) / denominator;
    weightB =
        ((c.y() - a.y()) * (point.x() - c.x()) +
         (a.x() - c.x()) * (point.y() - c.y())) / denominator;
    weightC = 1.0f - weightA - weightB;
    return weightA >= -1.0e-5f && weightB >= -1.0e-5f &&
        weightC >= -1.0e-5f;
}

// 四边形拆成两个三角形；属性按重心坐标插值，避免透视下的仿射误差。
bool SampleQuad(
    const OfflineQuadVertex (&quad)[4],
    const Eigen::Vector2f& pixel,
    Eigen::Vector2f& attribute)
{
    float weights[3] = {};
    if (Barycentric(pixel, quad[0].pixel, quad[1].pixel, quad[2].pixel,
        weights[0], weights[1], weights[2]))
    {
        attribute =
            quad[0].attribute * weights[0] +
            quad[1].attribute * weights[1] +
            quad[2].attribute * weights[2];
        return true;
    }
    if (Barycentric(pixel, quad[0].pixel, quad[2].pixel, quad[3].pixel,
        weights[0], weights[1], weights[2]))
    {
        attribute =
            quad[0].attribute * weights[0] +
            quad[2].attribute * weights[1] +
            quad[3].attribute * weights[2];
        return true;
    }
    return false;
}

struct QuadBounds
{
    int minimumX = 0;
    int minimumY = 0;
    int maximumX = -1;
    int maximumY = -1;
};

QuadBounds QuadPixelBounds(
    const OfflineQuadVertex (&quad)[4],
    const OfflineParticleCamera& camera)
{
    Eigen::Vector2f minimum = quad[0].pixel;
    Eigen::Vector2f maximum = quad[0].pixel;
    for (int index = 1; index < 4; ++index)
    {
        minimum = minimum.cwiseMin(quad[index].pixel);
        maximum = maximum.cwiseMax(quad[index].pixel);
    }
    QuadBounds bounds;
    bounds.minimumX = std::max(
        0, static_cast<int>(std::floor(minimum.x())));
    bounds.minimumY = std::max(
        0, static_cast<int>(std::floor(minimum.y())));
    bounds.maximumX = std::min(
        camera.width - 1, static_cast<int>(std::ceil(maximum.x())));
    bounds.maximumY = std::min(
        camera.height - 1, static_cast<int>(std::ceil(maximum.y())));
    return bounds;
}

void AccumulatePixel(
    OfflineParticleOverlay& overlay,
    int x,
    int y,
    const Eigen::Vector3f& rgb,
    float alpha,
    bool additive)
{
    if (x < 0 || y < 0 || x >= overlay.width || y >= overlay.height)
    {
        return;
    }
    const size_t index =
        static_cast<size_t>(y) * overlay.width + x;
    if (additive)
    {
        // Additive：srcAlpha 只缩放颜色，不减少背景可见比例。
        overlay.color[index].x += static_cast<double>(rgb.x() * alpha);
        overlay.color[index].y += static_cast<double>(rgb.y() * alpha);
        overlay.color[index].z += static_cast<double>(rgb.z() * alpha);
        return;
    }
    const float transmittance = overlay.transmittance[index];
    overlay.color[index].x +=
        static_cast<double>(rgb.x() * alpha * transmittance);
    overlay.color[index].y +=
        static_cast<double>(rgb.y() * alpha * transmittance);
    overlay.color[index].z +=
        static_cast<double>(rgb.z() * alpha * transmittance);
    overlay.transmittance[index] =
        transmittance * std::clamp(1.0f - alpha, 0.0f, 1.0f);
}

void RasterizeBillboard(
    const OfflineParticleEmitterState& emitter,
    uint32_t particleIndex,
    const OfflineParticle& particle,
    const OfflineParticleCamera& camera,
    float opacity,
    OfflineParticleOverlay& overlay)
{
    const ParticleEmitterAsset& asset = emitter.asset;
    const float lifetime = particle.velocityLife.w();
    const float lifeT = std::clamp(
        particle.positionAge.w() / std::max(lifetime, 1.0e-6f), 0.0f, 1.0f);
    const float size = EvaluateSizeCurve(asset, lifeT);
    if (!(size > 0.0f) || !std::isfinite(size))
    {
        return;
    }

    const Eigen::Vector3f center = SimulationPosition(
        emitter, particle.positionAge);
    const float depth = ViewDepth(camera, center);
    if (!(depth > 1.0e-4f))
    {
        return;
    }

    // GPU 使用 1/abs(invProjection[1][1]) 作为 pixel 比例；对透视/正交
    // 分别按深度或固定尺寸换算世界半边长到屏幕半边长。
    const float projectionScaleY = std::abs(camera.projection(1, 1));
    const bool orthographic = std::abs(camera.projection(3, 3)) > 1.0e-6f;
    float pixelHalfSize = orthographic
        ? size * projectionScaleY * static_cast<float>(camera.height) * 0.5f
        : size * projectionScaleY * static_cast<float>(camera.height) *
            0.5f / depth;
    if (!(pixelHalfSize > 0.0f) || !std::isfinite(pixelHalfSize))
    {
        return;
    }
    float worldHalfSize = size;
    if (asset.maxScreenSize > 0.0f &&
        pixelHalfSize > asset.maxScreenSize)
    {
        worldHalfSize *= asset.maxScreenSize / pixelHalfSize;
        pixelHalfSize = asset.maxScreenSize;
    }
    if (pixelHalfSize < 0.02f)
    {
        return;
    }

    Eigen::Vector3f billboardRight = camera.right;
    Eigen::Vector3f billboardUp = camera.up;
    if (asset.billboardAlignment == "velocity")
    {
        const Eigen::Vector3f velocity = SimulationVelocity(
            emitter, particle.velocityLife);
        const float velocityLengthSquared = velocity.squaredNorm();
        if (velocityLengthSquared > 1.0e-6f)
        {
            const Eigen::Vector3f forward = velocity.normalized();
            const Eigen::Vector3f candidateRight =
                Eigen::Vector3f(0.0f, 1.0f, 0.0f).cross(forward);
            if (candidateRight.squaredNorm() > 1.0e-6f)
            {
                billboardRight = candidateRight.normalized();
                const Eigen::Vector3f candidateUp =
                    forward.cross(billboardRight);
                if (candidateUp.squaredNorm() > 1.0e-6f)
                {
                    billboardUp = candidateUp.normalized();
                }
            }
        }
    }
    else if (asset.billboardAlignment == "fixed")
    {
        // fixed 基在发射器局部空间作者化；与 GPU 路径一样先乘 emitter.model
        // 线性部分，再构造 right 并用 normal × right 正交化 up。
        if (asset.billboardNormal.squaredNorm() > 1.0e-6f &&
            asset.billboardUp.squaredNorm() > 1.0e-6f)
        {
            const Eigen::Matrix3f linear =
                emitter.model.block<3, 3>(0, 0);
            const Eigen::Vector3f normal =
                (linear * asset.billboardNormal).normalized();
            const Eigen::Vector3f upHint =
                (linear * asset.billboardUp).normalized();
            const Eigen::Vector3f candidateRight =
                upHint.cross(normal);
            if (candidateRight.squaredNorm() > 1.0e-6f)
            {
                billboardRight = candidateRight.normalized();
                const Eigen::Vector3f candidateUp =
                    normal.cross(billboardRight);
                if (candidateUp.squaredNorm() > 1.0e-6f)
                {
                    billboardUp = candidateUp.normalized();
                }
            }
        }
    }

    float rotationDegrees =
        asset.rotation + asset.rotationSpeed * particle.positionAge.w();
    if (asset.rotationRandom)
    {
        rotationDegrees +=
            Fract(static_cast<float>(particleIndex) * kParticlePhaseStep) *
            360.0f;
    }
    const float rotationRadians =
        rotationDegrees * 3.14159265358979323846f / 180.0f;
    const float rotationCos = std::cos(rotationRadians);
    const float rotationSin = std::sin(rotationRadians);

    const Eigen::Vector2f corners[4] = {
        Eigen::Vector2f(-1.0f, -1.0f),
        Eigen::Vector2f(1.0f, -1.0f),
        Eigen::Vector2f(1.0f, 1.0f),
        Eigen::Vector2f(-1.0f, 1.0f)};
    OfflineQuadVertex quad[4];
    for (int index = 0; index < 4; ++index)
    {
        const Eigen::Vector2f corner = corners[index];
        const Eigen::Vector2f rotated(
            corner.x() * rotationCos - corner.y() * rotationSin,
            corner.x() * rotationSin + corner.y() * rotationCos);
        const Eigen::Vector3f world = center +
            (billboardRight * (rotated.x() * worldHalfSize) +
             billboardUp * (rotated.y() * worldHalfSize));
        if (!ProjectPoint(camera, world, quad[index].pixel))
        {
            // 近平面裁剪掉整片 quad 的边界情况：离线合成不拆分多边形，
            // 直接跳过该粒子，避免生成跨越相机背后的错误三角形。
            return;
        }
        quad[index].attribute = corner;
    }

    const QuadBounds bounds = QuadPixelBounds(quad, camera);
    const bool additive = asset.additive;
    const Eigen::Vector4f colorSample = EvaluateColorCurve(asset, lifeT);
    const Eigen::Vector3f rgb(
        colorSample.x() * emitter.colorTint.x(),
        colorSample.y() * emitter.colorTint.y(),
        colorSample.z() * emitter.colorTint.z());
    const float baseAlpha =
        colorSample.w() * opacity * emitter.colorTint.w();
    const float softness = std::clamp(asset.softness, 0.0f, 1.0f);
    const float seed =
        static_cast<float>(particleIndex) * kParticlePhaseStep;
    const float time = emitter.elapsedSeconds;

    for (int y = bounds.minimumY; y <= bounds.maximumY; ++y)
    {
        for (int x = bounds.minimumX; x <= bounds.maximumX; ++x)
        {
            const Eigen::Vector2f pixelCenter(
                static_cast<float>(x) + 0.5f,
                static_cast<float>(y) + 0.5f);
            Eigen::Vector2f corner;
            if (!SampleQuad(quad, pixelCenter, corner))
            {
                continue;
            }
            const float radius = corner.norm();
            if (radius >= 1.0f)
            {
                continue;
            }
            const float radial = std::pow(
                std::max(0.0f, 1.0f - radius),
                Mix(1.0f, 3.0f, softness));
            const Eigen::Vector2f uv = corner * 0.5f + Eigen::Vector2f(0.5f, 0.5f);
            const Eigen::Vector2f noiseUv =
                uv * 5.0f + Eigen::Vector2f(seed, -time * 0.35f);
            const float erosion = SmoothStep(
                0.18f, 0.82f,
                Fbm2D(noiseUv + Eigen::Vector2f(time * 0.2f, 0.0f)));
            const float alpha =
                baseAlpha * radial * Mix(1.0f, erosion, 0.72f);
            AccumulatePixel(overlay, x, y, rgb, alpha, additive);
        }
    }
}

void RasterizeRibbon(
    const OfflineParticleEmitterState& emitter,
    uint32_t particleIndex,
    const OfflineParticle& particle,
    const OfflineParticleCamera& camera,
    float opacity,
    OfflineParticleOverlay& overlay)
{
    const ParticleEmitterAsset& asset = emitter.asset;
    const float lifetime = particle.velocityLife.w();
    const float lifeT = std::clamp(
        particle.positionAge.w() / std::max(lifetime, 1.0e-6f), 0.0f, 1.0f);
    const uint32_t segments =
        ClampParticleRibbonSegments(asset.ribbonSegments);
    const Eigen::Vector3f points[4] = {
        SimulationPosition(emitter, particle.positionAge),
        SimulationPosition(emitter, particle.history0),
        SimulationPosition(emitter, particle.history1),
        SimulationPosition(emitter, particle.history2)};
    const Eigen::Vector4f colorSample = EvaluateColorCurve(asset, lifeT);
    const Eigen::Vector3f rgb(
        colorSample.x() * emitter.colorTint.x(),
        colorSample.y() * emitter.colorTint.y(),
        colorSample.z() * emitter.colorTint.z());
    const float baseAlpha =
        colorSample.w() * opacity * emitter.colorTint.w();
    const float edgeWidth = Mix(0.02f, 0.5f,
        std::clamp(asset.softness, 0.0f, 1.0f));
    const float fade = std::clamp(asset.ribbonFade, 0.0f, 1.0f);
    const float seed =
        static_cast<float>(particleIndex) * kParticlePhaseStep;
    const float time = emitter.elapsedSeconds;

    for (uint32_t segment = 0; segment < segments; ++segment)
    {
        const Eigen::Vector3f start = points[segment];
        const Eigen::Vector3f end = points[segment + 1];
        const Eigen::Vector3f tangent = end - start;
        const float tangentLength = tangent.norm();
        if (!(tangentLength > 1.0e-5f))
        {
            continue;
        }
        const Eigen::Vector3f midpoint = (start + end) * 0.5f;
        const Eigen::Vector3f viewOffset = camera.position - midpoint;
        Eigen::Vector3f viewDirection = camera.right;
        if (viewOffset.squaredNorm() > 1.0e-10f)
        {
            viewDirection = viewOffset.normalized();
        }
        Eigen::Vector3f side = tangent.cross(viewDirection);
        if (side.squaredNorm() > 1.0e-10f)
        {
            side.normalize();
        }
        else
        {
            side = camera.right;
        }
        side *= asset.ribbonWidth;

        // 世界空间条带四角 -> 屏幕空间四边形；属性 (along, across)。
        OfflineQuadVertex quad[4];
        const Eigen::Vector3f worldCorners[4] = {
            start - side, start + side, end + side, end - side};
        const Eigen::Vector2f attributes[4] = {
            Eigen::Vector2f(0.0f, -1.0f),
            Eigen::Vector2f(0.0f, 1.0f),
            Eigen::Vector2f(1.0f, 1.0f),
            Eigen::Vector2f(1.0f, -1.0f)};
        bool projected = true;
        for (int index = 0; index < 4; ++index)
        {
            if (!ProjectPoint(camera, worldCorners[index], quad[index].pixel))
            {
                projected = false;
                break;
            }
            quad[index].attribute = attributes[index];
        }
        if (!projected)
        {
            continue;
        }

        const QuadBounds bounds = QuadPixelBounds(quad, camera);
        const bool additive = asset.additive;
        for (int y = bounds.minimumY; y <= bounds.maximumY; ++y)
        {
            for (int x = bounds.minimumX; x <= bounds.maximumX; ++x)
            {
                const Eigen::Vector2f pixelCenter(
                    static_cast<float>(x) + 0.5f,
                    static_cast<float>(y) + 0.5f);
                Eigen::Vector2f attribute;
                if (!SampleQuad(quad, pixelCenter, attribute))
                {
                    continue;
                }
                const float along = std::clamp(attribute.x(), 0.0f, 1.0f);
                const float acrossUv = attribute.y() * 0.5f + 0.5f;
                const float edge =
                    SmoothStep(0.0f, edgeWidth, acrossUv) *
                    SmoothStep(0.0f, edgeWidth, 1.0f - acrossUv);
                const float tailT = ParticleRibbonTailParameter(
                    segment, along, segments);
                const float tailAlpha = Mix(1.0f, 1.0f - tailT, fade);

                // ribbon 的 vUV.x 就是段内 along（0..1），不要再加 0.5；
                // 尾端位置只通过 tailAlpha 进入，噪声相位仍按段内坐标。
                const Eigen::Vector2f noiseUv(
                    along * 5.0f,
                    acrossUv * 2.0f);
                const Eigen::Vector2f animated =
                    noiseUv + Eigen::Vector2f(seed, -time * 0.35f);
                const float erosion = SmoothStep(
                    0.18f, 0.82f,
                    Fbm2D(animated + Eigen::Vector2f(time * 0.2f, 0.0f)));
                const float alpha = baseAlpha * edge * tailAlpha *
                    Mix(1.0f, erosion, 0.6f);
                AccumulatePixel(overlay, x, y, rgb, alpha, additive);
            }
        }
    }
}

struct ParticleDepthKey
{
    float depth = 0.0f;
    uint32_t index = 0;
};

struct ParticleDepthDescending
{
    bool operator()(const ParticleDepthKey& left,
        const ParticleDepthKey& right) const
    {
        if (left.depth != right.depth) return left.depth > right.depth;
        // 深度相同按原始池索引升序，保证排序结果与线程/实现无关。
        return left.index < right.index;
    }
};

void RasterizeEmitter(
    const OfflineParticleEmitterState& emitter,
    const OfflineParticleCamera& camera,
    float opacity,
    OfflineParticleOverlay& overlay)
{
    if (!emitter.visible || !emitter.enabled)
    {
        return;
    }
    // 离线合成没有 distortion 附件，也没有 CPU 粒子灯/贴花消费路径；
    // 这些发射器都不应退化成可见 billboard。
    if (emitter.asset.renderMode == "distortion" ||
        emitter.asset.renderMode == "light" ||
        emitter.asset.renderMode == "decal")
    {
        return;
    }
    const bool ribbon = emitter.asset.renderMode == "ribbon";
    std::vector<ParticleDepthKey> order;
    if (emitter.asset.sortParticles)
    {
        order.reserve(emitter.particles.size());
        for (uint32_t index = 0;
             index < emitter.particles.size(); ++index)
        {
            const OfflineParticle& particle = emitter.particles[index];
            if (!particle.Alive()) continue;
            const Eigen::Vector3f center = SimulationPosition(
                emitter, particle.positionAge);
            order.push_back(ParticleDepthKey{
                ViewDepth(camera, center), index});
        }
        std::stable_sort(
            order.begin(), order.end(), ParticleDepthDescending{});
    }
    else
    {
        order.reserve(emitter.particles.size());
        for (uint32_t index = 0;
             index < emitter.particles.size(); ++index)
        {
            if (emitter.particles[index].Alive())
            {
                order.push_back(ParticleDepthKey{0.0f, index});
            }
        }
    }

    for (const ParticleDepthKey& key : order)
    {
        const OfflineParticle& particle = emitter.particles[key.index];
        if (ribbon)
        {
            RasterizeRibbon(
                emitter, key.index, particle, camera, opacity, overlay);
        }
        else
        {
            // mesh 渲染模式在离线合成里退化为 billboard；纹理/flipbook
            // 与静态网格实例化不在 CPU 合成边界内。
            RasterizeBillboard(
                emitter, key.index, particle, camera, opacity, overlay);
        }
    }
}

} // namespace

OfflineParticleCamera MakeOfflineParticleCamera(
    const CameraSnapshot& camera,
    int width,
    int height)
{
    OfflineParticleCamera result;
    result.position = camera.position;
    result.right = camera.right;
    result.up = camera.up;
    result.forward = camera.forward;
    result.view = camera.view;
    result.projection = camera.projection;
    result.viewProjection = camera.viewProjection;
    result.clipNear = camera.clipNear;
    result.clipFar = camera.clipFar;
    result.width = std::max(width, 0);
    result.height = std::max(height, 0);
    return result;
}

OfflineParticleOverlay CompositeOfflineParticles(
    const std::vector<OfflineParticleSystemState>& systems,
    const OfflineParticleCamera& camera)
{
    OfflineParticleOverlay overlay;
    overlay.width = camera.width;
    overlay.height = camera.height;
    if (overlay.width <= 0 || overlay.height <= 0)
    {
        return overlay;
    }
    const size_t pixelCount =
        static_cast<size_t>(overlay.width) * overlay.height;
    overlay.color.assign(pixelCount, Offline::Vec3{});
    overlay.transmittance.assign(pixelCount, 1.0f);

    // 跨系统/跨发射器仍按 GPU 的“远 -> 近 + 稳定 key”顺序；否则不同
    // unordered_map 迭代顺序会改变 alpha 合成的像素结果。
    std::vector<const OfflineParticleEmitterState*> emitters;
    std::vector<ParticleEmitterDepthKey> depthKeys;
    for (const OfflineParticleSystemState& system : systems)
    {
        for (size_t index = 0; index < system.emitters.size(); ++index)
        {
            const OfflineParticleEmitterState& emitter =
                system.emitters[index];
            if (!emitter.visible || !emitter.enabled) continue;
            emitters.push_back(&emitter);
            depthKeys.push_back(ParticleEmitterDepthKey{
                system.name + "|" + emitter.asset.name + "|" +
                    std::to_string(index),
                emitter.worldCenter,
                emitter.worldRadius});
        }
    }
    const std::vector<size_t> drawOrder =
        ComputeParticleEmitterDrawOrder(
            depthKeys, camera.position, camera.forward);
    const std::array<Eigen::Vector4f, 6> frustumPlanes =
        BuildParticleFrustumPlanes(camera.viewProjection);
    const float projectionScaleY =
        std::abs(camera.projection(1, 1));
    for (const size_t drawIndex : drawOrder)
    {
        const OfflineParticleEmitterState& emitter =
            *emitters[drawIndex];
        const ParticleEmitterCullingResult culling =
            CullParticleEmitter(
                ParticleEmitterCullingInput{
                    emitter.worldCenter,
                    emitter.worldRadius,
                    emitter.asset.maxDrawDistance,
                    emitter.asset.fadeDistance,
                    emitter.asset.minScreenSize},
                frustumPlanes,
                camera.position,
                camera.forward,
                projectionScaleY,
                static_cast<float>(camera.height));
        if (!culling.visible)
        {
            continue;
        }
        RasterizeEmitter(
            emitter,
            camera,
            emitter.asset.opacity * culling.opacityScale,
            overlay);
    }
    return overlay;
}

std::vector<Offline::Vec3> CompositeOfflineParticleOverBackground(
    const std::vector<Offline::Vec3>& background,
    const OfflineParticleOverlay& overlay)
{
    if (overlay.Empty() ||
        background.size() !=
            static_cast<size_t>(overlay.width) * overlay.height)
    {
        return background;
    }
    std::vector<Offline::Vec3> result(background.size());
    for (size_t index = 0; index < background.size(); ++index)
    {
        const float transmittance = overlay.transmittance[index];
        const Offline::Vec3& color = overlay.color[index];
        const Offline::Vec3& base = background[index];
        result[index] = Offline::Vec3{
            color.x + base.x * transmittance,
            color.y + base.y * transmittance,
            color.z + base.z * transmittance};
    }
    return result;
}

std::vector<Offline::Vec3> CompositeOfflineFilmPixels(
    const Offline::FilmSnapshot& film,
    const OfflineParticleOverlay& overlay)
{
    const size_t pixelCount =
        static_cast<size_t>(std::max(film.width, 0)) *
        static_cast<size_t>(std::max(film.height, 0));
    std::vector<Offline::Vec3> result(pixelCount);
    for (size_t index = 0; index < pixelCount; ++index)
    {
        if (index < film.direct.size() && index < film.indirect.size())
        {
            const Offline::Vec3& direct = film.direct[index];
            const Offline::Vec3& indirect = film.indirect[index];
            result[index] = Offline::Vec3{
                direct.x + indirect.x,
                direct.y + indirect.y,
                direct.z + indirect.z};
        }
    }
    if (overlay.width != film.width || overlay.height != film.height)
    {
        return result;
    }
    return CompositeOfflineParticleOverBackground(result, overlay);
}

OfflineParticleOverlay BuildOfflineParticleOverlay(
    const WorldSnapshot& snapshot,
    const OfflineParticlePreviewPolicy& policy,
    int width,
    int height,
    uint32_t seed)
{
    const std::vector<OfflineParticleSystemInput> inputs =
        BuildOfflineParticleInputs(snapshot, seed);
    std::vector<OfflineParticleSystemState> systems;
    systems.reserve(inputs.size());
    for (const OfflineParticleSystemInput& input : inputs)
    {
        systems.push_back(SimulateOfflineParticles(
            input, policy, snapshot.camera.position));
    }
    return CompositeOfflineParticles(
        systems,
        MakeOfflineParticleCamera(snapshot.camera, width, height));
}

} // namespace VL
