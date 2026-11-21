#include "hair/rod/rodRootSupport.h"

namespace VL::Hair::Rod
{
SupportSample SampleRootSupport(const RootFrame& root, const Vec3& rootLocalPosition)
{
    SupportSample result;
    const Vec3 offset = root.orientation * rootLocalPosition;
    result.position = root.position + offset;
    result.velocity = root.linearVelocity + root.angularVelocityWorld.cross(offset);
    return result;
}

SupportEvaluation EvaluateRootSupport(
    const RestState& rest, const State& state, const RootFrame& root)
{
    SupportEvaluation result;
    result.forces.assign(state.positions.size(), Vec3::Zero());
    for (size_t point = 0; point < state.positions.size(); ++point)
    {
        const SupportSample sample = SampleRootSupport(root, rest.rootLocalPositions[point]);
        const Vec3 displacement = state.positions[point] - sample.position;
        const Vec3 relativeVelocity = state.velocities[point] - sample.velocity;
        const Vec3 springForce = -rest.supportStiffness[point] * displacement;
        const Vec3 dampingForce = -rest.supportDamping[point] * relativeVelocity;
        result.forces[point] = springForce + dampingForce;
        result.potential += 0.5 * rest.supportStiffness[point] * displacement.squaredNorm();
        result.drivePower += result.forces[point].dot(sample.velocity);
        result.dissipation += rest.supportDamping[point] * relativeVelocity.squaredNorm();
    }
    return result;
}
}
