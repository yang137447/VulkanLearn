#pragma once

#include <string>

namespace VL
{

// Scene-authored projectile recipe. Runtime no longer infers arrow material or
// ballistics from whichever scene object happens to exist.
struct ProjectileSettings
{
    std::string materialKey;
    std::string materialInstanceKey;
    float speedCmPerSecond = 3500.0f;
    float radiusCm = 1.5f;
    float massKg = 0.08f;
    float cooldownSeconds = 0.15f;

    bool IsConfigured() const { return !materialKey.empty(); }
};

} // namespace VL
