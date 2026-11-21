#pragma once

#include <cstdint>

#include "hair/rod/rodMath.h"

namespace VL::Hair::Rod
{
inline constexpr std::uint32_t SectionInertiaKineticMathVersion = 2;

Vec3 SectionRotaryInertia(double segmentMass, double radius);
RestState BuildSectionInertiaRestState(const std::vector<Vec3>& positions,
    const Material& material = {}, const std::vector<Quat>& orientations = {},
    const std::vector<double>& inverseMassMarkers = {});
}
