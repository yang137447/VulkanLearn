#pragma once

#include <cstdint>
#include <string>

namespace VL::Physics
{

// 目标材质配置。它是运行时分派的主键，不携带任何几何或玩法假设。
enum class FractureMaterialProfile
{
    Generic = 0,
    Glass,
    Balloon
};

// 冲击世界只描述“接触双方在结构中的角色”，不描述玻璃/气球等材料名。
enum class FractureBodyRole
{
    StaticTarget,
    AttachedPart,
    FreeShard,
    Projectile,
    Ground
};

enum class FractureContactKind
{
    Impact,
    Resting
};

const char* FractureMaterialProfileName(FractureMaterialProfile profile);

} // namespace VL::Physics
