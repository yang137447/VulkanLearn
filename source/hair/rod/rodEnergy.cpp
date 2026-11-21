#include "hair/rod/rodEnergy.h"

#include "hair/rod/rodMath.h"

namespace VL::Hair::Rod
{
ShearBlock EvaluateShearBlock(const RestState& rest, const State& state, size_t segment)
{
    const Mat3 rotation = state.orientations[segment].toRotationMatrix();
    const Vec3 strain = rotation.transpose() *
        (state.positions[segment + 1] - state.positions[segment]) / rest.lengths[segment];
    ShearBlock block;
    block.constraint = strain - rest.restStrains[segment];
    block.firstPositionJacobian = -rotation.transpose() / rest.lengths[segment];
    block.secondPositionJacobian = rotation.transpose() / rest.lengths[segment];
    block.orientationJacobian = CrossMatrix(strain);
    block.compliance = (rest.lengths[segment] *
        rest.stretchShear[segment]).cwiseInverse().asDiagonal();
    return block;
}

BendBlock EvaluateBendBlock(const RestState& rest, const State& state, size_t joint)
{
    const Quat relative = state.orientations[joint].conjugate() *
        state.orientations[joint + 1];
    const Vec3 angle = Log(relative);
    const Mat3 logJacobian = RightJacobianInverse(angle);
    const double meanLength = 0.5 * (rest.lengths[joint] + rest.lengths[joint + 1]);
    BendBlock block;
    block.constraint = angle - rest.restJointAngles[joint];
    block.firstOrientationJacobian = -logJacobian * relative.toRotationMatrix().transpose();
    block.secondOrientationJacobian = logJacobian;
    block.compliance = (meanLength * rest.bendTwist[joint].cwiseInverse()).asDiagonal();
    return block;
}

EnergyEvaluation EvaluateEnergy(const RestState& rest, const State& state)
{
    EnergyEvaluation result;
    result.forces.assign(state.positions.size(), Vec3::Zero());
    result.bodyTorques.assign(state.orientations.size(), Vec3::Zero());
    for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
    {
        const ShearBlock block = EvaluateShearBlock(rest, state, segment);
        const Vec3 gradient = rest.lengths[segment] *
            rest.stretchShear[segment].cwiseProduct(block.constraint);
        result.stretchShear += 0.5 * block.constraint.dot(gradient);
        result.forces[segment] -= block.firstPositionJacobian.transpose() * gradient;
        result.forces[segment + 1] -= block.secondPositionJacobian.transpose() * gradient;
        result.bodyTorques[segment] -= block.orientationJacobian.transpose() * gradient;
    }
    for (size_t joint = 0; joint + 1 < rest.lengths.size(); ++joint)
    {
        const BendBlock block = EvaluateBendBlock(rest, state, joint);
        const double meanLength = 0.5 * (rest.lengths[joint] + rest.lengths[joint + 1]);
        const Vec3 gradient = rest.bendTwist[joint].cwiseProduct(block.constraint) / meanLength;
        result.bendTwist += 0.5 * block.constraint.dot(gradient);
        result.bodyTorques[joint] -= block.firstOrientationJacobian.transpose() * gradient;
        result.bodyTorques[joint + 1] -= block.secondOrientationJacobian.transpose() * gradient;
    }
    return result;
}
}
