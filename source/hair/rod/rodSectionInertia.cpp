#include "hair/rod/rodSectionInertia.h"

#include <cmath>
#include <stdexcept>

namespace VL::Hair::Rod
{
Vec3 SectionRotaryInertia(double segmentMass, double radius)
{
    if (!std::isfinite(segmentMass) || segmentMass <= 0.0 ||
        !std::isfinite(radius) || radius <= 0.0)
        throw std::invalid_argument("section rotary inertia requires positive finite mass and radius");
    const double transverse = 0.25 * segmentMass * radius * radius;
    const Vec3 inertia(transverse, transverse, 2.0 * transverse);
    if (!inertia.allFinite() || inertia.minCoeff() <= 0.0)
        throw std::invalid_argument("section rotary inertia is not representable");
    return inertia;
}

RestState BuildSectionInertiaRestState(const std::vector<Vec3>& positions,
    const Material& material, const std::vector<Quat>& orientations,
    const std::vector<double>& inverseMassMarkers)
{
    RestState rest = BuildRestState(positions, material, orientations, inverseMassMarkers);
    for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
    {
        const double mass = material.density * Pi * material.radius * material.radius *
            rest.lengths[segment];
        rest.inertias[segment] = SectionRotaryInertia(mass, material.radius);
        rest.inverseInertias[segment] = segment == 0 ? Vec3::Zero().eval() :
            rest.inertias[segment].cwiseInverse().eval();
    }
    return rest;
}
}
