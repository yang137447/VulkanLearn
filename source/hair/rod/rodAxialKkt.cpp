#include "hair/rod/rodAxialKkt.h"

#include <cmath>
#include <limits>
#include <stdexcept>

#include <Eigen/LU>

namespace VL::Hair::Rod
{
namespace
{
class EquilibratedPivot
{
public:
    explicit EquilibratedPivot(const Eigen::MatrixXd& matrix)
        : rows_(matrix.rows()), columns_(matrix.cols())
    {
        Eigen::MatrixXd scaled = matrix;
        for (Eigen::Index row = 0; row < matrix.rows(); ++row)
        {
            const double maximum = scaled.row(row).cwiseAbs().maxCoeff();
            rows_[row] = maximum > 0.0 ? 1.0 / maximum : 1.0;
            scaled.row(row) *= rows_[row];
        }
        for (Eigen::Index column = 0; column < matrix.cols(); ++column)
        {
            const double maximum = scaled.col(column).cwiseAbs().maxCoeff();
            columns_[column] = maximum > 0.0 ? 1.0 / maximum : 1.0;
            scaled.col(column) *= columns_[column];
        }
        factor_.compute(scaled);
        factor_.setThreshold(64.0 * static_cast<double>(matrix.rows()) *
            std::numeric_limits<double>::epsilon());
    }

    bool IsInvertible() const
    {
        return factor_.isInvertible();
    }

    Eigen::MatrixXd Solve(const Eigen::MatrixXd& right) const
    {
        const Eigen::MatrixXd scaledRight = rows_.asDiagonal() * right;
        return columns_.asDiagonal() * factor_.solve(scaledRight);
    }

private:
    Eigen::VectorXd rows_;
    Eigen::VectorXd columns_;
    Eigen::FullPivLU<Eigen::MatrixXd> factor_;
};
}

AxialContactSystem AssembleAxialContactSystem(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, const AxialContactActiveSet& active,
    const FrozenTangentAir* air)
{
    const size_t count = rest.positions.size();
    if (count < 2 || active.contacts.size() != count ||
        active.tensileActive.size() != count - 1 || active.tensileMultipliers.size() != count - 1)
        throw std::invalid_argument("axial contact active set dimensions are invalid");
    const auto contact = AssembleContactSystem(rest, old, candidate, input, profile, constraints, active.contacts);
    AxialContactSystem system;
    system.forceScale = contact.forceScale;
    system.lengthScale = contact.lengthScale;
    system.lower.assign(count, AxialContactMatrix::Zero());
    system.diagonal.assign(count, AxialContactMatrix::Zero());
    system.upper.assign(count, AxialContactMatrix::Zero());
    system.residual.assign(count, AxialContactVector::Zero());
    for (size_t point = 0; point < count; ++point)
    {
        system.lower[point].topLeftCorner<9, 9>() = contact.lower[point];
        system.diagonal[point].topLeftCorner<9, 9>() = contact.diagonal[point];
        system.upper[point].topLeftCorner<9, 9>() = contact.upper[point];
        system.residual[point].head<9>() = contact.residual[point];
        system.diagonal[point](9, 9) = 1.0;
    }
    if (air)
    {
        const auto original = AssembleProgressiveImplicitSystem(rest, old, candidate, input, profile);
        const auto implicitAir = AssembleProgressiveAirSystem(rest, old, candidate, input, profile, *air);
        for (size_t point = 0; point < count; ++point)
        {
            system.residual[point].head<6>() += implicitAir.residual[point] - original.residual[point];
            system.diagonal[point].topLeftCorner<6, 6>() +=
                implicitAir.diagonal[point] - original.diagonal[point];
        }
    }
    for (size_t edge = 0; edge + 1 < count; ++edge)
    {
        const size_t right = edge + 1;
        const auto value = EvaluateAxialLimit(old.positions[edge], old.positions[right],
            candidate.positions[edge], candidate.positions[right], rest.lengths[edge],
            active.tensileMultipliers[edge]);
        const bool leftFree = rest.inverseMasses[edge] != 0.0;
        const bool rightFree = rest.inverseMasses[right] != 0.0;
        if (!leftFree && !rightFree)
        {
            if (value.gap < -1.0e-9 * system.lengthScale)
                throw std::domain_error("prescribed endpoints violate tensile limit");
            continue;
        }
        if (!active.tensileActive[edge])
            continue;
        if (!std::isfinite(active.tensileMultipliers[edge]))
            throw std::invalid_argument("axial multiplier is not finite");
        system.diagonal[right](9, 9) = 0.0;
        system.residual[right][9] = value.gap / system.lengthScale;
        if (leftFree)
        {
            system.residual[edge].head<3>() -= value.force.head<3>() / system.forceScale;
            system.upper[edge].block<3, 1>(0, 9) = -value.direction;
            system.lower[right].block<1, 3>(9, 0) = value.direction.transpose();
            system.diagonal[edge].topLeftCorner<3, 3>() +=
                value.negativeForceDerivative.topLeftCorner<3, 3>() *
                (system.lengthScale / system.forceScale);
        }
        if (rightFree)
        {
            system.residual[right].head<3>() -= value.force.tail<3>() / system.forceScale;
            system.diagonal[right].block<3, 1>(0, 9) = value.direction;
            system.diagonal[right].block<1, 3>(9, 0) = -value.direction.transpose();
            system.diagonal[right].topLeftCorner<3, 3>() +=
                value.negativeForceDerivative.bottomRightCorner<3, 3>() *
                (system.lengthScale / system.forceScale);
        }
        if (leftFree && rightFree)
        {
            system.upper[edge].topLeftCorner<3, 3>() +=
                value.negativeForceDerivative.topRightCorner<3, 3>() *
                (system.lengthScale / system.forceScale);
            system.lower[right].topLeftCorner<3, 3>() +=
                value.negativeForceDerivative.bottomLeftCorner<3, 3>() *
                (system.lengthScale / system.forceScale);
        }
    }
    return system;
}

std::vector<AxialContactVector> SolveAxialContactSystem(const AxialContactSystem& system)
{
    const size_t count = system.diagonal.size();
    if (count == 0 || system.lower.size() != count ||
        system.upper.size() != count || system.residual.size() != count)
        throw std::invalid_argument("axial contact band dimensions are invalid");
    struct Supernode
    {
        size_t first;
        size_t nodes;
        Eigen::MatrixXd upper;
        Eigen::VectorXd right;
    };
    std::vector<Supernode> factors;
    for (size_t point = 0; point < count;)
    {
        AxialContactMatrix diagonal = system.diagonal[point];
        AxialContactVector value = -system.residual[point];
        if (!factors.empty())
        {
            diagonal -= system.lower[point] * factors.back().upper.bottomRows(10);
            value -= system.lower[point] * factors.back().right.tail(10);
        }
        const EquilibratedPivot factorization(diagonal);
        if (factorization.IsInvertible())
        {
            factors.push_back({point, 1, factorization.Solve(system.upper[point]), factorization.Solve(value)});
            ++point;
            continue;
        }
        if (point + 1 >= count)
            throw std::runtime_error("axial contact unsupported terminal local rank domain");
        Eigen::Matrix<double, 20, 20> joined = Eigen::Matrix<double, 20, 20>::Zero();
        joined.topLeftCorner<10, 10>() = diagonal;
        joined.topRightCorner<10, 10>() = system.upper[point];
        joined.bottomLeftCorner<10, 10>() = system.lower[point + 1];
        joined.bottomRightCorner<10, 10>() = system.diagonal[point + 1];
        Eigen::Matrix<double, 20, 1> joinedRight;
        joinedRight.head<10>() = value;
        joinedRight.tail<10>() = -system.residual[point + 1];
        Eigen::Matrix<double, 20, 10> joinedUpper = Eigen::Matrix<double, 20, 10>::Zero();
        joinedUpper.bottomRows<10>() = system.upper[point + 1];
        const EquilibratedPivot joinedFactorization(joined);
        if (!joinedFactorization.IsInvertible())
            throw std::runtime_error("axial contact unsupported two-node local rank domain");
        factors.push_back({point, 2, joinedFactorization.Solve(joinedUpper),
            joinedFactorization.Solve(joinedRight)});
        point += 2;
    }
    std::vector<AxialContactVector> direction(count, AxialContactVector::Zero());
    for (size_t remaining = factors.size(); remaining > 0; --remaining)
    {
        const auto& factor = factors[remaining - 1];
        Eigen::VectorXd value = factor.right;
        if (factor.first + factor.nodes < count)
            value -= factor.upper * direction[factor.first + factor.nodes];
        for (size_t node = 0; node < factor.nodes; ++node)
            direction[factor.first + node] = value.segment<10>(static_cast<Eigen::Index>(10 * node));
    }
    for (size_t point = 0; point < count; ++point)
    {
        AxialContactVector remaining = system.diagonal[point] * direction[point] + system.residual[point];
        if (point > 0)
            remaining += system.lower[point] * direction[point - 1];
        if (point + 1 < count)
            remaining += system.upper[point] * direction[point + 1];
        if (!remaining.allFinite() || remaining.cwiseAbs().maxCoeff() > 1.0e-10)
            throw std::runtime_error("axial contact original linear residual rejected");
    }
    return direction;
}
}
