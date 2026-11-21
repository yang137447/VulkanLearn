#include "hair/rod/rodAnalytic.h"

#include <algorithm>
#include <cmath>

#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodMath.h"

namespace VL::Hair::Rod
{
ImplicitSystem AssembleImplicitSystemAnalytic(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input)
{
    ImplicitSystem system;
    system.lengthScale = rest.totalLength;
    double mass = 0.0;
    double stiffness = 0.0;
    for (size_t point = 0; point < rest.positions.size(); ++point)
    {
        mass += rest.masses[point];
        stiffness = std::max(stiffness, rest.supportStiffness[point]);
    }
    system.forceScale = mass * 9.8 + stiffness * rest.totalLength;
    system.torqueScale = system.forceScale * system.lengthScale;
    const size_t blocks = rest.positions.size();
    system.diagonal.assign(blocks, Matrix6::Zero());
    system.lower.assign(blocks, Matrix6::Zero());
    system.upper.assign(blocks, Matrix6::Zero());
    system.residual = EvaluateImplicitResidual(rest, old, candidate, input);
    auto add = [&](size_t row, size_t column, int rowOffset, int component, const Vec3& derivative)
    {
        Matrix6& matrix = row == column ? system.diagonal[row] :
            (row < column ? system.upper[row] : system.lower[row]);
        const double rowScale = rowOffset == 0 ? system.forceScale : system.torqueScale;
        const double columnScale = component < 3 ? system.lengthScale : 1.0;
        matrix.block<3, 1>(rowOffset, component) -= derivative * (columnScale / rowScale);
    };
    for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
    {
        const double length = rest.lengths[segment];
        const Mat3 rotation = candidate.orientations[segment].toRotationMatrix();
        const Vec3 strain = rotation.transpose() *
            (candidate.positions[segment + 1] - candidate.positions[segment]) / length;
        const Vec3 stress = rest.stretchShear[segment].cwiseProduct(strain - rest.restStrains[segment]);
        for (int degree = 0; degree < 9; ++degree)
        {
            const int axis = degree % 3;
            Vec3 edgeDelta = Vec3::Zero();
            Vec3 angleDelta = Vec3::Zero();
            const size_t column = degree >= 3 && degree < 6 ? segment + 1 : segment;
            const int component = degree < 6 ? axis : axis + 3;
            if (degree < 3)
                edgeDelta = -Vec3::Unit(axis);
            else if (degree < 6)
                edgeDelta = Vec3::Unit(axis);
            else
                angleDelta = Vec3::Unit(axis);
            const Vec3 deltaStrain = rotation.transpose() * edgeDelta / length +
                strain.cross(angleDelta);
            const Vec3 deltaStress = rest.stretchShear[segment].cwiseProduct(deltaStrain);
            const Vec3 deltaForce = rotation * (angleDelta.cross(stress) + deltaStress);
            const Vec3 deltaTorque = length *
                (deltaStrain.cross(stress) + strain.cross(deltaStress));
            add(segment, column, 0, component, deltaForce);
            add(segment + 1, column, 0, component, -deltaForce);
            add(segment, column, 3, component, deltaTorque);
        }
    }
    for (size_t joint = 0; joint < rest.restJointAngles.size(); ++joint)
    {
        const Mat3 relative = (candidate.orientations[joint].conjugate() *
            candidate.orientations[joint + 1]).toRotationMatrix();
        const Vec3 angle = Log(candidate.orientations[joint].conjugate() *
            candidate.orientations[joint + 1]);
        const Mat3 inverseJacobian = RightJacobianInverse(angle);
        const Mat3 cross = CrossMatrix(angle);
        const double squared = angle.squaredNorm();
        const double magnitude = std::sqrt(squared);
        double coefficient;
        double coefficientDerivative;
        if (magnitude < 1.0e-3)
        {
            coefficient = 1.0 / 12.0 + squared / 720.0 +
                squared * squared / 30240.0 + squared * squared * squared / 1209600.0;
            coefficientDerivative = 1.0 / 360.0 + squared / 7560.0 +
                squared * squared / 201600.0;
        }
        else
        {
            const double cotangent = 1.0 / std::tan(0.5 * magnitude);
            const double numerator = 1.0 - 0.5 * magnitude * cotangent;
            const double numeratorDerivative = -0.5 * cotangent +
                0.25 * magnitude * (1.0 + cotangent * cotangent);
            coefficient = numerator / squared;
            coefficientDerivative = numeratorDerivative / (squared * magnitude) -
                2.0 * numerator / (squared * squared);
        }
        const Vec3 jointStiffness = rest.bendTwist[joint] /
            (0.5 * (rest.lengths[joint] + rest.lengths[joint + 1]));
        const Vec3 stress = jointStiffness.cwiseProduct(angle - rest.restJointAngles[joint]);
        const Vec3 dual = inverseJacobian.transpose() * stress;
        for (int degree = 0; degree < 6; ++degree)
        {
            const int axis = degree % 3;
            const bool first = degree < 3;
            const size_t column = first ? joint : joint + 1;
            const Vec3 relativeDelta = first ? (-relative.transpose() * Vec3::Unit(axis)).eval() :
                Vec3::Unit(axis).eval();
            const Vec3 deltaAngle = inverseJacobian * relativeDelta;
            const Mat3 deltaCross = CrossMatrix(deltaAngle);
            const Mat3 deltaJacobian = 0.5 * deltaCross +
                coefficient * (deltaCross * cross + cross * deltaCross) +
                coefficientDerivative * angle.dot(deltaAngle) * cross * cross;
            const Vec3 deltaDual = deltaJacobian.transpose() * stress +
                inverseJacobian.transpose() * jointStiffness.cwiseProduct(deltaAngle);
            add(joint, column, 3, axis + 3,
                relative * (relativeDelta.cross(dual) + deltaDual));
            add(joint + 1, column, 3, axis + 3, -deltaDual);
        }
    }
    for (size_t block = 0; block < blocks; ++block)
    {
        const double step = input.stepSeconds;
        const double damping = input.supportEnabled ? rest.supportDamping[block] : 0.0;
        const double support = input.supportEnabled ? rest.supportStiffness[block] : 0.0;
        system.diagonal[block].topLeftCorner<3, 3>() +=
            ((rest.masses[block] / (step * step) + damping / step + support) *
                system.lengthScale / system.forceScale) * Mat3::Identity();
        if (block < rest.lengths.size())
        {
            const Vec3 netAngle = Log(old.orientations[block].conjugate() * candidate.orientations[block]);
            const Vec3 transported = candidate.orientations[block].conjugate() *
                (old.orientations[block] * rest.inertias[block].cwiseProduct(old.angularVelocities[block]));
            const Vec3 external = input.externalTorquesWorld.empty() ?
                Vec3::Zero().eval() : input.externalTorquesWorld[block];
            const Mat3 derivative = rest.inertias[block].asDiagonal().toDenseMatrix() *
                RightJacobianInverse(netAngle) / (step * step) -
                CrossMatrix(transported) / step -
                CrossMatrix(candidate.orientations[block].conjugate() * external);
            system.diagonal[block].bottomRightCorner<3, 3>() += derivative / system.torqueScale;
        }
        system.residual[block].head<3>() /= system.forceScale;
        system.residual[block].tail<3>() /= system.torqueScale;
    }
    for (size_t block = 0; block < blocks; ++block)
        for (int component = 0; component < 6; ++component)
        {
            const bool fixed = component < 3 ? rest.inverseMasses[block] == 0.0 :
                (block == 0 || block == rest.lengths.size());
            if (!fixed)
                continue;
            system.diagonal[block].row(component).setZero();
            system.diagonal[block].col(component).setZero();
            system.lower[block].row(component).setZero();
            system.upper[block].row(component).setZero();
            if (block > 0)
                system.upper[block - 1].col(component).setZero();
            if (block + 1 < blocks)
                system.lower[block + 1].col(component).setZero();
            system.diagonal[block](component, component) = 1.0;
            system.residual[block][component] = 0.0;
        }
    return system;
}
}
