#pragma once

#include <string>

#include "physics/fracture/fractureTypes.h"

namespace VL::Physics::Fracture
{

// 运行时只读的破坏参数。当前由内置注册表提供，后续可由资产 JSON 覆盖；
// 增加玻璃/陶瓷/冰/木头时优先新增 profile，而不是再写一套 runtime 常量。
struct DestructibleProfile
{
    std::string key;
    FractureMaterialProfile materialProfile = FractureMaterialProfile::Generic;

    // Plate/solid fracture tuning.
    double densityKgPerCm3 = 0.0025;
    double friction = 0.35;
    double restitution = 0.12;
    double damageRadiusBase = 0.06;
    double damageRadiusSpeedScale = 0.14;
    double damageRadiusMinFraction = 0.04;
    double damageRadiusMaxFraction = 0.22;
    int maxSubfractureDepth = 2;
    double minProjectileSubfractureArea = 8.0;
    double minImpactSubfractureArea = 60.0;
    float minSubfractureSpeed = 1.8f;
    double minAnchoredIslandArea = 300.0;
    double supportBorderTolerance = 0.5;
    double supportAdjacencyTolerance = 0.75;
    int radialCracksMin = 6;
    int radialCracksMax = 14;
    int ringCount = 3;
    double coreRadiusFraction = 0.03;
    double ringGrowth = 1.55;
    double angularJitter = 0.38;
    double ringJitter = 0.28;
    double dicingProbability = 0.70;
    double attachedDamageRadiusScale = 0.55;
    int attachedRadialCracksMin = 4;
    int attachedRadialCracksMax = 10;
    int attachedRingCount = 1;
    double attachedCoreRadiusFraction = 0.03;

    // Rubber balloon tuning.
    float burstSpeedMps = 4.0f;
    double burstRadialImpulse = 220.0;
    double burstTangentialBase = 28.0;
    double burstTangentialScale = 32.0;
    double burstEnergyBase = 0.75;
    double burstEnergySpeedScale = 1.0 / 12.0;
    double burstEnergyMin = 0.75;
    double burstEnergyMax = 1.45;
    double burstDamping = 4.0;
    double deflateGravity = -981.0;
    double deflateDamping = 3.0;
    double deflateThrust = 950.0;
    double deflateThrustDuration = 1.4;
    double deflateSpin = 7.0;
    double deflateHoleStart = 0.12;
    double deflateHoleEnd = 0.22;
    double deflateDuration = 1.2;

    // Budget/audio keys keep policy out of the runtime branches.
    std::string dynamicBudgetClass = "glass.dynamic";
    std::string attachedBudgetClass = "glass.attached";
    std::string shardBudgetClass = "balloon.shard";
    std::string audioProfile = "glass";
};

// Return nullptr for an unknown key; callers must report a diagnostic and use the
// type's default profile instead of silently guessing.
const DestructibleProfile* FindDestructibleProfile(const std::string& key);
const DestructibleProfile& DefaultGlassProfile();
const DestructibleProfile& DefaultBalloonProfile();

} // namespace VL::Physics::Fracture
