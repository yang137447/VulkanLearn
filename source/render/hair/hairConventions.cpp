#include "render/hair/hairConventions.h"

namespace VL::Hair
{

float WrapAngle(float angle) noexcept
{
    while (angle > HairPi)
    {
        angle -= 2.0f * HairPi;
    }
    while (angle < -HairPi)
    {
        angle += 2.0f * HairPi;
    }
    return angle;
}

} // namespace VL::Hair
