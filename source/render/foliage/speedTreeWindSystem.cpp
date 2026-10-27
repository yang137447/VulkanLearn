#include "speedTreeWindSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "world/worldWindSampling.h"

namespace
{
    // Fixed inspection value for the current Oak validation pass. The public
    // SetStrength API can override this once an interactive control is wired.
    constexpr float InspectionStrength = 0.5f;
    // Modeler v10.2.0's Runtime SDK invoke path enables every authored layer
    // and passes 1.0 for the per-instance noise decorrelation scalar.
    constexpr float RuntimeSdkWindIndependence = 1.0f;
    constexpr float RuntimeSdkGuiSpeedScale = 0.1f;
    constexpr float ImportScaling = 1.0f;
    constexpr bool UseFixedInspectionStrength = false;
    constexpr bool FreezeInspectionTime = false;

    float Clamp01(float value)
    {
        return std::max(0.0f, std::min(1.0f, value));
    }

    float Interpolate(float start, float end, float amount)
    {
        return start + (end - start) * amount;
    }

    float LinearSigmoid(float value, float linearity)
    {
        const float sigmoid = 1.0f /
            (1.0f + std::exp(-Interpolate(-6.0f, 6.0f, value)));
        return Interpolate(sigmoid, value, linearity);
    }

    float SampleCurve(const WindResponseCurve& curve, float strength)
    {
        const float position = Clamp01(strength) * 19.0f;
        const size_t before = static_cast<size_t>(position);
        const size_t after = std::min<size_t>(before + 1, curve.values.size() - 1);
        const float amount = position - static_cast<float>(before);
        return curve.values[before] + (curve.values[after] - curve.values[before]) * amount;
    }

    std::pair<Eigen::Vector3f, Eigen::Vector3f> ConvertSourceBounds(
        const Eigen::Vector3f& sourceBoundsMin,
        const Eigen::Vector3f& sourceBoundsMax)
    {
        // The -Y handedness conversion reverses the source Y interval. Do not
        // independently convert min/max or the engine-space Z bounds become
        // inverted, which corrupts packed noise offsets and height weights.
        return {
            Eigen::Vector3f(
                sourceBoundsMin.x(),
                sourceBoundsMin.z(),
                -sourceBoundsMax.y()),
            Eigen::Vector3f(
                sourceBoundsMax.x(),
                sourceBoundsMax.z(),
                -sourceBoundsMin.y())};
    }

    Eigen::Vector3f NormalizeIfNonZero(const Eigen::Vector3f& value)
    {
        const float length = value.norm();
        if (length == 0.0f)
        {
            return value;
        }
        return value / length;
    }

    Eigen::Quaternionf BuildWindDirectionRotation(
        const Eigen::Vector3f& startDirection,
        const Eigen::Vector3f& targetDirection)
    {
        constexpr float OppositeDirectionThreshold = -0.9999f;
        constexpr float AxisLengthSquaredThreshold = 1.0e-8f;
        const float directionDot = startDirection.dot(targetDirection);
        if (directionDot > OppositeDirectionThreshold)
        {
            return Eigen::Quaternionf::FromTwoVectors(startDirection, targetDirection);
        }

        Eigen::Vector3f rotationAxis = Eigen::Vector3f::UnitY() -
            startDirection * startDirection.dot(Eigen::Vector3f::UnitY());
        if (rotationAxis.squaredNorm() <= AxisLengthSquaredThreshold)
        {
            rotationAxis = Eigen::Vector3f::UnitX() -
                startDirection * startDirection.dot(Eigen::Vector3f::UnitX());
        }
        rotationAxis.normalize();
        return Eigen::Quaternionf(
            0.0f,
            rotationAxis.x(),
            rotationAxis.y(),
            rotationAxis.z());
    }
}

void SpeedTreeWindSystem::ResetGustState()
{
    gustState = GustState();
}

void SpeedTreeWindSystem::Reset()
{
    config = WindResponse();
    gpuState = SpeedTreeWindStateGPU();
    sourceBoundsMin.setZero();
    sourceBoundsMax.setZero();
    treeExtents = Eigen::Vector3f::Ones();
    windDirection = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    currentWindDirection = windDirection;
    windDirectionAtStart = windDirection;
    windDirectionRotation = Eigen::Quaternionf::Identity();
    targetStrength = InspectionStrength;
    currentStrength = targetStrength;
    strengthAtStart = currentStrength;
    strengthChangeStartTime = 0.0;
    strengthChangeEndTime = 0.0;
    directionChangeStartTime = 0.0;
    directionChangeEndTime = 0.0;
    ResetGustState();
    gustRandomState = 137u;
    gustingEnabled = true;
    sceneGustScale = 1.0f;
    sharedNoisePosition.setZero();
    branch1NoisePosition.setZero();
    branch2NoisePosition.setZero();
    rippleNoisePosition.setZero();
    lastAdvanceTime = -1.0;
    configured = false;
}

void SpeedTreeWindSystem::Configure(
    const WindResponse& windConfig,
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax)
{
    config = windConfig;
    sourceBoundsMin = boundsMin;
    sourceBoundsMax = boundsMax;
    const auto convertedBounds = ConvertSourceBounds(sourceBoundsMin, sourceBoundsMax);
    treeExtents = convertedBounds.second - convertedBounds.first;
    // The Games 9 schema exposes CurrentStrength, but Oak v10's twelve-field
    // Common table omits it; Unity also does not copy it into runtime profiles.
    // Strength is live weather state, so use an inspection default until a
    // renderer or interactive controller supplies one.
    targetStrength = InspectionStrength;
    windDirection = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    currentWindDirection = windDirection;
    windDirectionAtStart = windDirection;
    windDirectionRotation = Eigen::Quaternionf::Identity();
    currentStrength = targetStrength;
    strengthAtStart = currentStrength;
    strengthChangeStartTime = 0.0;
    strengthChangeEndTime = 0.0;
    directionChangeStartTime = 0.0;
    directionChangeEndTime = 0.0;
    ResetGustState();
    gustRandomState = 137u;
    gustingEnabled = true;
    sceneGustScale = 1.0f;
    sharedNoisePosition.setZero();
    branch1NoisePosition.setZero();
    branch2NoisePosition.setZero();
    rippleNoisePosition.setZero();
    lastAdvanceTime = -1.0;
    configured = true;
    // 不能调用 AdvanceTo(0.0)：它会把 lastAdvanceTime 置为 0，使随后的场景风推进
    // 在 t=0 被早退，GPU 状态停留在 SDK 参考值。这里直接写入零风状态（方向为零，
    // 着色器不形变），首次推进会覆盖它。
    UpdateGpuState(Eigen::Vector3f::Zero(), 0.0f, 0.0f);
}

void SpeedTreeWindSystem::SetStrength(float strength)
{
    if (strength == currentStrength)
    {
        return;
    }

    targetStrength = strength;
    const double startTime = lastAdvanceTime;
    const float responseTime = config.common.strengthResponse;
    const float responseAmount = Interpolate(
        responseTime * 0.5f,
        responseTime,
        std::abs(targetStrength - currentStrength));
    strengthAtStart = currentStrength;
    strengthChangeStartTime = startTime;
    strengthChangeEndTime = startTime + responseAmount;
}

void SpeedTreeWindSystem::SetDirection(const Eigen::Vector3f& direction)
{
    if (direction == windDirection)
    {
        return;
    }

    windDirection = direction;
    const double startTime = lastAdvanceTime;
    const float responseTime = config.common.directionResponse;
    const float dot = currentWindDirection.dot(windDirection);
    const float distance = 1.0f - ((dot + 1.0f) * 0.5f);
    const float responseAmount = Interpolate(
        responseTime * 0.5f,
        responseTime,
        distance);
    windDirectionAtStart = currentWindDirection;
    windDirectionRotation = BuildWindDirectionRotation(
        windDirectionAtStart,
        windDirection);
    directionChangeStartTime = startTime;
    directionChangeEndTime = startTime + responseAmount;
}

void SpeedTreeWindSystem::SetGustingEnabled(bool enabled)
{
    gustingEnabled = enabled;
    if (!enabled)
    {
        ResetGustState();
    }
}

void SpeedTreeWindSystem::SetSceneGustScale(float scale)
{
    sceneGustScale = Clamp01(scale);
}

void SpeedTreeWindSystem::ForceGust()
{
    if (gustingEnabled)
    {
        gustState.forceRequested = true;
    }
}

float SpeedTreeWindSystem::RandomFloat(float minimum, float maximum)
{
    // 使用 xorshift32 根据当前状态生成下一个伪随机状态。
    // 这里的左移、右移和按位异或组合起来可以快速打散状态位；
    // gustRandomState 是 uint32_t，计算会自然保持在 32 位范围内。
    gustRandomState ^= gustRandomState << 13;
    gustRandomState ^= gustRandomState >> 17;
    gustRandomState ^= gustRandomState << 5;

    // 将 32 位随机整数归一化为约 [0, 1] 的比例值。
    const float percent = static_cast<float>(gustRandomState) /
        static_cast<float>(UINT32_MAX);

    // 将比例值线性映射到调用方指定的 [minimum, maximum] 区间。
    return Interpolate(minimum, maximum, percent);
}

void SpeedTreeWindSystem::AdvanceTo(double timeSeconds)
{
    if (!configured)
    {
        return;
    }

    const float time = std::max(0.0f, static_cast<float>(timeSeconds));
    if (time == lastAdvanceTime)
    {
        return;
    }

    const float deltaTime = lastAdvanceTime >= 0.0
        ? time - static_cast<float>(lastAdvanceTime)
        : 0.0f;
    lastAdvanceTime = time;

    if (gustingEnabled)
    {
        const bool canStartGust =
            gustState.forceRequested ||
            time > gustState.fallTargetTime ||
            (time < gustState.fallStartTime && time > gustState.riseTargetTime);
        if (canStartGust)
        {
            const float gustThreshold =
                deltaTime * config.common.gustFrequency *
                config.common.gustFrequency * 0.1f;
            if (gustState.forceRequested ||
                RandomFloat(0.0f, deltaTime) < gustThreshold)
            {
                gustState.startTime = time;
                gustState.strengthAtStart = gustState.strength;
                gustState.targetStrength = RandomFloat(
                    config.common.gustStrengthMin,
                    config.common.gustStrengthMax);
                gustState.targetStrength = std::min(
                    1.0f - currentStrength,
                    gustState.targetStrength);

                const float responseAmount = Interpolate(
                    config.common.strengthResponse * 0.5f,
                    config.common.strengthResponse,
                    std::abs(gustState.targetStrength - currentStrength));
                if (gustState.targetStrength > gustState.strength)
                {
                    gustState.riseTargetTime = time +
                        config.common.gustRiseScalar *
                        RandomFloat(responseAmount, responseAmount * 2.0f);
                }
                else
                {
                    gustState.riseTargetTime = time +
                        config.common.gustFallScalar *
                        RandomFloat(responseAmount, responseAmount * 2.0f);
                }

                gustState.fallStartTime = gustState.riseTargetTime + RandomFloat(
                    config.common.gustDurationMin,
                    config.common.gustDurationMax);
                gustState.fallTargetTime = gustState.fallStartTime +
                    config.common.gustFallScalar *
                    RandomFloat(responseAmount * 2.0f, responseAmount * 3.0f);
            }
        }

        if (time < gustState.riseTargetTime)
        {
            gustState.strength = Interpolate(
                gustState.strengthAtStart,
                gustState.targetStrength,
                LinearSigmoid(
                    static_cast<float>((time - gustState.startTime) /
                        (gustState.riseTargetTime - gustState.startTime)),
                    0.0f));
        }
        else if (time > gustState.fallStartTime &&
            gustState.fallTargetTime > 0.0 &&
            gustState.fallTargetTime > gustState.fallStartTime)
        {
            gustState.strength = Interpolate(
                gustState.targetStrength,
                0.0f,
                LinearSigmoid(
                    static_cast<float>((time - gustState.fallStartTime) /
                        (gustState.fallTargetTime - gustState.fallStartTime)),
                    0.5f));
        }

        gustState.strength = Clamp01(gustState.strength);
        gustState.forceRequested = false;
    }
    else
    {
        ResetGustState();
    }

    float directionFactor = 1.0f;
    if (directionChangeEndTime != directionChangeStartTime)
    {
        directionFactor = Clamp01(static_cast<float>(
            (time - directionChangeStartTime) /
            (directionChangeEndTime - directionChangeStartTime)));
    }
    if (directionFactor >= 1.0f)
    {
        currentWindDirection = windDirection;
    }
    else
    {
        directionFactor = LinearSigmoid(directionFactor, 0.5f);
        currentWindDirection = NormalizeIfNonZero(
            Eigen::Quaternionf::Identity()
                .slerp(directionFactor, windDirectionRotation) *
            windDirectionAtStart);
    }

    UpdateEasedStrength(time);

    const float strength = UseFixedInspectionStrength
        ? InspectionStrength
        : Clamp01(currentStrength + gustState.strength * sceneGustScale);
    UpdateGpuState(currentWindDirection, strength, deltaTime);
}

// 强度缓动：目标在 [strengthChangeStartTime, strengthChangeEndTime] 内由 sigmoid 过渡到
// targetStrength。零时长过渡（响应时间为 0）直接到达目标。原先的 0 因子会停在两端中点，
// 导致强度永远停在半路，所以这里取 1。
// SDK 参考状态机和场景目标路径共用；响应时间来自当前 profile，而不是场景风。
void SpeedTreeWindSystem::UpdateEasedStrength(double time)
{
    float strengthFactor = 1.0f;
    if (strengthChangeEndTime != strengthChangeStartTime)
    {
        strengthFactor = Clamp01(static_cast<float>(
            (time - strengthChangeStartTime) /
            (strengthChangeEndTime - strengthChangeStartTime)));
    }
    currentStrength = Interpolate(
        strengthAtStart,
        targetStrength,
        LinearSigmoid(strengthFactor, 0.0f));
}

void SpeedTreeWindSystem::AdvanceToSceneWind(
    double timeSeconds,
    const VL::WorldWind& wind)
{
    if (!configured)
    {
        return;
    }

    const double time = std::max(0.0, timeSeconds);
    if (time == lastAdvanceTime)
    {
        return;
    }

    // 场景 wind 是作者目标，不是最终风。每个 profile 在这里像 CWindStateMgr 一样
    // 独立缓动强度/方向，并用 profile 自己的 gust 参数推进离散阵风；场景 gust 只
    // 提供开关和整体幅度缩放，frequency/turbulence 留给场级消费者（头发）。
    const float target = Clamp01(VL::SceneWindCurveAxis(wind.strength));
    const Eigen::Vector3f direction = wind.direction.squaredNorm() > 1.0e-12f
        ? wind.direction.normalized()
        : Eigen::Vector3f::UnitX();

    if (lastAdvanceTime < 0.0)
    {
        // 首帧直接采用目标值，避免从 Configure 的 inspection 值慢启动。
        targetStrength = target;
        currentStrength = target;
        strengthAtStart = target;
        strengthChangeStartTime = time;
        strengthChangeEndTime = time;
        windDirection = direction;
        currentWindDirection = direction;
        windDirectionAtStart = direction;
        windDirectionRotation = Eigen::Quaternionf::Identity();
        directionChangeStartTime = time;
        directionChangeEndTime = time;
    }
    else
    {
        SetStrength(target);
        SetDirection(direction);
    }

    SetGustingEnabled(wind.gust.enabled);
    SetSceneGustScale(wind.gust.enabled ? wind.gust.amplitude : 0.0f);
    // AdvanceTo 仍负责 SDK 语义的 gust/响应/噪声推进和 GPU 打包；这里只是把
    // 目标值换成场景 wind，而不是另写一套时间积分。
    AdvanceTo(time);
}

void SpeedTreeWindSystem::UpdateGpuState(
    const Eigen::Vector3f& direction,
    float strength,
    float deltaTime)
{
    const float sharedSpeed = SampleCurve(config.shared.speed, strength);
    const float branch1Speed = SampleCurve(config.branch1.speed, strength);
    const float branch2Speed = SampleCurve(config.branch2.speed, strength);
    const float rippleSpeed = SampleCurve(config.ripple.speed, strength);

    const float noiseDelta = FreezeInspectionTime ? 0.0f : deltaTime;
    sharedNoisePosition -= direction *
        noiseDelta * RuntimeSdkGuiSpeedScale * sharedSpeed;
    branch1NoisePosition -= direction *
        noiseDelta * RuntimeSdkGuiSpeedScale * branch1Speed;
    branch2NoisePosition -= direction *
        noiseDelta * RuntimeSdkGuiSpeedScale * branch2Speed;
    rippleNoisePosition -= direction *
        noiseDelta * RuntimeSdkGuiSpeedScale * rippleSpeed;

    gpuState.windVector = Eigen::Vector4f(
        direction.x(),
        direction.y(),
        direction.z(),
        strength);
    gpuState.treeExtentsSharedHeightStart = Eigen::Vector4f(
        treeExtents.x(),
        treeExtents.y(),
        treeExtents.z(),
        config.sharedStartHeight);
    const auto convertedBounds = ConvertSourceBounds(sourceBoundsMin, sourceBoundsMax);
    const Eigen::Vector3f& convertedBoundsMin = convertedBounds.first;
    const Eigen::Vector3f& convertedBoundsMax = convertedBounds.second;
    gpuState.treeBoundsMin = Eigen::Vector4f(
        convertedBoundsMin.x(), convertedBoundsMin.y(), convertedBoundsMin.z(), 0.0f);
    gpuState.treeBoundsMax = Eigen::Vector4f(
        convertedBoundsMax.x(), convertedBoundsMax.y(), convertedBoundsMax.z(), 0.0f);
    gpuState.branchStretchLimits = Eigen::Vector4f(
        config.branch1StretchLimit,
        config.branch2StretchLimit,
        RuntimeSdkWindIndependence,
        ImportScaling);

    gpuState.sharedNoisePosTurbulenceIndependence = Eigen::Vector4f(
        sharedNoisePosition.x(),
        sharedNoisePosition.y(),
        sharedNoisePosition.z(),
        config.doShared ? config.shared.independence : 0.0f);
    gpuState.sharedBendOscillationTurbulenceFlexibility = Eigen::Vector4f(
        config.doShared ? SampleCurve(config.shared.bend, strength) : 0.0f,
        config.doShared ? SampleCurve(config.shared.oscillation, strength) : 0.0f,
        config.doShared ? SampleCurve(config.shared.turbulence, strength) : 0.0f,
        config.doShared ? SampleCurve(config.shared.flexibility, strength) : 0.0f);

    gpuState.branch1NoisePosTurbulenceIndependence = Eigen::Vector4f(
        branch1NoisePosition.x(),
        branch1NoisePosition.y(),
        branch1NoisePosition.z(),
        config.doBranch1 ? config.branch1.independence : 0.0f);
    gpuState.branch1BendOscillationTurbulenceFlexibility = Eigen::Vector4f(
        config.doBranch1 ? SampleCurve(config.branch1.bend, strength) : 0.0f,
        config.doBranch1 ? SampleCurve(config.branch1.oscillation, strength) : 0.0f,
        config.doBranch1 ? SampleCurve(config.branch1.turbulence, strength) : 0.0f,
        config.doBranch1 ? SampleCurve(config.branch1.flexibility, strength) : 0.0f);

    gpuState.branch2NoisePosTurbulenceIndependence = Eigen::Vector4f(
        branch2NoisePosition.x(),
        branch2NoisePosition.y(),
        branch2NoisePosition.z(),
        config.doBranch2 ? config.branch2.independence : 0.0f);
    gpuState.branch2BendOscillationTurbulenceFlexibility = Eigen::Vector4f(
        config.doBranch2 ? SampleCurve(config.branch2.bend, strength) : 0.0f,
        config.doBranch2 ? SampleCurve(config.branch2.oscillation, strength) : 0.0f,
        config.doBranch2 ? SampleCurve(config.branch2.turbulence, strength) : 0.0f,
        config.doBranch2 ? SampleCurve(config.branch2.flexibility, strength) : 0.0f);

    gpuState.rippleNoisePosTurbulenceIndependence = Eigen::Vector4f(
        rippleNoisePosition.x(),
        rippleNoisePosition.y(),
        rippleNoisePosition.z(),
        config.doRipple ? config.ripple.independence : 0.0f);
    gpuState.ripplePlanarDirectionalFlexibilityShimmer = Eigen::Vector4f(
        config.doRipple ? SampleCurve(config.ripple.planar, strength) : 0.0f,
        config.doRipple ? SampleCurve(config.ripple.directional, strength) : 0.0f,
        config.doRipple ? SampleCurve(config.ripple.flexibility, strength) : 0.0f,
        config.doShimmer && config.doRipple ? config.ripple.shimmer : 0.0f);
}

void WindResponseProfileSet::Configure(
    const std::unordered_map<std::string, WindResponseProfile>& profiles)
{
    systems.clear();
    systems.reserve(profiles.size());
    for (const auto& [profileKey, profile] : profiles)
    {
        SpeedTreeWindSystem system;
        system.Configure(
            profile.config,
            profile.sourceBoundsMin,
            profile.sourceBoundsMax);
        systems.emplace(profileKey, std::move(system));
    }
}

void WindResponseProfileSet::Reset()
{
    systems.clear();
}

void WindResponseProfileSet::Swap(
    WindResponseProfileSet& other) noexcept
{
    systems.swap(other.systems);
}

void WindResponseProfileSet::AdvanceTo(double timeSeconds)
{
    for (auto& [profileKey, system] : systems)
    {
        system.AdvanceTo(timeSeconds);
    }
}

void WindResponseProfileSet::AdvanceToSceneWind(
    double timeSeconds,
    const VL::WorldWind& wind)
{
    // 所有物种共享场景作者目标；每个 profile 仍保留自己的响应时间、gust 包络和
    // 噪声状态，因此阵风相位/节奏可以不同。
    for (auto& [profileKey, system] : systems)
    {
        system.AdvanceToSceneWind(timeSeconds, wind);
    }
}

bool WindResponseProfileSet::SetStrength(float strength)
{
    if (systems.empty())
    {
        return false;
    }
    for (auto& [profileKey, system] : systems)
    {
        system.SetStrength(strength);
    }
    return true;
}

bool WindResponseProfileSet::SetDirection(const Eigen::Vector3f& direction)
{
    if (systems.empty())
    {
        return false;
    }
    for (auto& [profileKey, system] : systems)
    {
        system.SetDirection(direction);
    }
    return true;
}

bool WindResponseProfileSet::SetGustingEnabled(bool enabled)
{
    if (systems.empty())
    {
        return false;
    }
    for (auto& [profileKey, system] : systems)
    {
        system.SetGustingEnabled(enabled);
    }
    return true;
}

bool WindResponseProfileSet::ForceGust()
{
    if (systems.empty() || !IsGustingEnabled())
    {
        return false;
    }
    for (auto& [profileKey, system] : systems)
    {
        system.ForceGust();
    }
    return true;
}

const SpeedTreeWindStateGPU* WindResponseProfileSet::FindGpuState(
    std::string_view profileKey) const
{
    // Current limitation: profile lookup still constructs a string and hashes
    // it on the object UBO path. World/RenderScene preparation can remove that
    // cost by resolving profile keys to stable numeric handles.
    const auto systemIt = systems.find(std::string(profileKey));
    if (systemIt == systems.end())
    {
        return nullptr;
    }
    return &systemIt->second.GetGpuState();
}

bool WindResponseProfileSet::IsGustingEnabled() const
{
    if (systems.empty())
    {
        return false;
    }
    for (const auto& [profileKey, system] : systems)
    {
        if (!system.IsGustingEnabled())
        {
            return false;
        }
    }
    return true;
}
