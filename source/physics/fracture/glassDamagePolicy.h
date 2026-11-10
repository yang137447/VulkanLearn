#pragma once

namespace VL::Physics::Fracture
{

// 玻璃冲击的“开裂 → 破碎”纯策略。它只接收相对速度并维护累计损伤，
// 不依赖 Jolt、World、Vulkan 或调试绘制；因此裂纹阈值和增长曲线可以被
// GoogleTest 直接覆盖。裂纹拓扑与命中点由调用方用现有径向图案生成。
struct GlassDamageSettings
{
    // 低于该相对速度只开裂，不产生碎片；与气球漏气阈值同一量级，
    // 但保持独立常量：玻璃板和橡皮膜的失效速度不应互相耦合。
    float crackOnlySpeed = 4.0f;
    // 归一化累计能量达到该值后进入完整破碎。
    float breakEnergy = 1.0f;
    // 单次开裂冲击的能量系数：contribution = energyPerHit * (v/vc)^2。
    // 接近阈值的低能量命中约 3 次达到破碎，微弱命中需要更多次。
    float energyPerHit = 0.4f;
    // 裂纹图案随累计能量从小变密：母裂纹数和环数线性增长。
    int minRadialCracks = 6;
    int maxRadialCracks = 12;
    int minRingCount = 1;
    int maxRingCount = 3;
    // 开裂图案的 stopRadius 相对完整伤害半径的倍率区间。
    // 开裂只是局部浅层损伤，一开始不会覆盖完整破碎范围。
    float minCrackRadiusScale = 0.5f;
    float maxCrackRadiusScale = 0.8f;
};

enum class GlassHitOutcome
{
    Crack,
    Fracture
};

struct GlassHitDecision
{
    GlassHitOutcome outcome = GlassHitOutcome::Crack;
    float addedEnergy = 0.0f;
    // 本次之后的累计能量（已 clamp 到 breakEnergy）。
    float accumulatedEnergy = 0.0f;
    // accumulatedEnergy / breakEnergy，范围 [0, 1]。
    float normalizedEnergy = 0.0f;
    // 开裂图案的 stopRadius 倍率；Fracture 时无实际意义。
    float crackRadiusScale = 0.0f;
    int radialCracks = 0;
    int ringCount = 0;
};

class GlassDamagePolicy
{
public:
    GlassDamagePolicy() = default;
    explicit GlassDamagePolicy(const GlassDamageSettings& settings);

    // relativeSpeed 单位为米/秒，与 Jolt 接触速度一致。
    // 强命中（>= crackOnlySpeed）立即返回 Fracture；低能量命中累计能量，
    // 达到 breakEnergy 的那一次返回 Fracture。非有限或负速度按 0 能量处理。
    GlassHitDecision RegisterImpact(float relativeSpeed);

    float AccumulatedEnergy() const { return accumulatedEnergy; }
    bool IsBroken() const { return broken; }
    const GlassDamageSettings& Settings() const { return settings; }
    void Reset();

private:
    GlassDamageSettings settings;
    float accumulatedEnergy = 0.0f;
    bool broken = false;
};

} // namespace VL::Physics::Fracture
