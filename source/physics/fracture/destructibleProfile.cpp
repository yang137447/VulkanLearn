#include "physics/fracture/destructibleProfile.h"

namespace VL::Physics::Fracture
{
namespace
{

DestructibleProfile MakeGlassDefault()
{
    DestructibleProfile profile;
    profile.key = "glass.default";
    profile.materialProfile = FractureMaterialProfile::Glass;
    profile.dynamicBudgetClass = "glass.dynamic";
    profile.attachedBudgetClass = "glass.attached";
    profile.shardBudgetClass = "glass.dynamic";
    profile.audioProfile = "glass";
    return profile;
}

DestructibleProfile MakeGlassTempered()
{
    DestructibleProfile profile = MakeGlassDefault();
    profile.key = "glass.tempered";
    profile.maxSubfractureDepth = 3;
    profile.minProjectileSubfractureArea = 4.0;
    profile.minImpactSubfractureArea = 30.0;
    profile.radialCracksMax = 18;
    profile.ringCount = 4;
    profile.attachedDamageRadiusScale = 0.42;
    return profile;
}

DestructibleProfile MakeBalloonDefault()
{
    DestructibleProfile profile;
    profile.key = "balloon.default";
    profile.materialProfile = FractureMaterialProfile::Balloon;
    profile.dynamicBudgetClass = "balloon.shard";
    profile.attachedBudgetClass = "balloon.shard";
    profile.shardBudgetClass = "balloon.shard";
    profile.audioProfile = "balloon";
    return profile;
}

} // namespace

const DestructibleProfile* FindDestructibleProfile(const std::string& key)
{
    static const DestructibleProfile glassDefault = MakeGlassDefault();
    static const DestructibleProfile glassTempered = MakeGlassTempered();
    static const DestructibleProfile balloonDefault = MakeBalloonDefault();
    if (key == glassDefault.key) return &glassDefault;
    if (key == glassTempered.key) return &glassTempered;
    if (key == balloonDefault.key) return &balloonDefault;
    return nullptr;
}

const DestructibleProfile& DefaultGlassProfile()
{
    return *FindDestructibleProfile("glass.default");
}

const DestructibleProfile& DefaultBalloonProfile()
{
    return *FindDestructibleProfile("balloon.default");
}

} // namespace VL::Physics::Fracture
