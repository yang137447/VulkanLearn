#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

// M0 CPU 原型：位置 + 拉伸 + 弯曲 + 标量扭转，XPBD。
// 只用于验证 sag-free rest 反解的数值行为，不进入运行时。
namespace
{
using Vec = Eigen::Vector3d;

enum class SweepOrder
{
    Forward,
    Reverse,
    Interleaved
};

const char* SweepName(SweepOrder order)
{
    switch (order)
    {
    case SweepOrder::Reverse:
        return "reverse";
    case SweepOrder::Interleaved:
        return "interleaved";
    case SweepOrder::Forward:
    default:
        return "forward";
    }
}

struct Params
{
    double EA = 1.0e4;
    double EI = 100.0;
    double GJ = 100.0;
    double GA = 1.0e4 / 2.6;
    double gravity = -9.8;
    double gravityScale = 1.0;
    double dt = 1.0 / 240.0;
    int substeps = 4;
    int iterations = 8;
    int rootFixed = 2;
    double damping = 0.002;
    SweepOrder sweepOrder = SweepOrder::Forward;
    bool useVectorStretchShear = false;
};

struct Rest
{
    std::vector<double> lengths;
    std::vector<Vec> bend;
    std::vector<double> twistDelta;
    std::vector<Eigen::Matrix3d> frames;
};

struct State
{
    std::vector<Vec> x;
    std::vector<Vec> prev;
    std::vector<double> phi;
    std::vector<double> phiPrev;
};

std::vector<Eigen::Matrix3d> BuildSegmentFrames(
    const std::vector<Vec>& positions)
{
    std::vector<Eigen::Matrix3d> frames(positions.size() - 1);
    for (size_t segment = 0; segment + 1 < positions.size(); ++segment)
    {
        const Vec tangent =
            (positions[segment + 1] - positions[segment]).normalized();
        Vec reference = std::abs(tangent.y()) < 0.9
            ? Vec(0.0, 1.0, 0.0) : Vec(1.0, 0.0, 0.0);
        Vec first = reference.cross(tangent);
        if (first.squaredNorm() < 1.0e-16)
            first = Vec(0.0, 0.0, 1.0).cross(tangent);
        first.normalize();
        const Vec second = tangent.cross(first).normalized();
        Eigen::Matrix3d frame;
        frame.col(0) = first;
        frame.col(1) = second;
        frame.col(2) = tangent;
        frames[segment] = frame;
    }
    return frames;
}

Rest BuildRest(const std::vector<Vec>& x, double twistStep)
{
    Rest rest;
    const size_t count = x.size();
    rest.lengths.resize(count - 1);
    for (size_t i = 0; i + 1 < count; ++i)
        rest.lengths[i] = (x[i + 1] - x[i]).norm();
    rest.bend.resize(count - 2);
    for (size_t i = 0; i + 2 < count; ++i)
    {
        const Vec t0 = (x[i + 1] - x[i]).normalized();
        const Vec t1 = (x[i + 2] - x[i + 1]).normalized();
        rest.bend[i] = t0.cross(t1);
    }
    rest.twistDelta.assign(count - 2, twistStep);
    rest.frames = BuildSegmentFrames(x);
    return rest;
}

double BendComponent(const std::vector<Vec>& x, size_t i, int axis)
{
    const Vec t0 = (x[i + 1] - x[i]).normalized();
    const Vec t1 = (x[i + 2] - x[i + 1]).normalized();
    return t0.cross(t1)[axis];
}

Eigen::Matrix3d CoRotatedFrame(const Eigen::Matrix3d& authoredFrame,
    const Vec& currentDelta)
{
    const Vec from = authoredFrame.col(2);
    const Vec to = currentDelta.normalized();
    const double cosine = std::clamp(from.dot(to), -1.0, 1.0);
    Eigen::Matrix3d transport = Eigen::Matrix3d::Identity();
    if (cosine < 1.0 - 1.0e-12)
    {
        Vec axis = from.cross(to);
        const double sine = axis.norm();
        if (sine < 1.0e-12)
        {
            axis = from.unitOrthogonal();
            transport =
                2.0 * axis * axis.transpose() -
                Eigen::Matrix3d::Identity();
        }
        else
        {
            axis /= sine;
            Eigen::Matrix3d skew;
            skew << 0.0, -axis.z(), axis.y(),
                axis.z(), 0.0, -axis.x(),
                -axis.y(), axis.x(), 0.0;
            transport = Eigen::Matrix3d::Identity() +
                sine * skew + (1.0 - cosine) * skew * skew;
        }
    }
    return transport * authoredFrame;
}

State ResetState(const std::vector<Vec>& x, const Rest& rest)
{
    State state;
    state.x = x;
    state.prev = x;
    state.phi.assign(rest.twistDelta.size(), 0.0);
    for (size_t i = 0; i < state.phi.size(); ++i)
        state.phi[i] = rest.twistDelta[i] * static_cast<double>(i);
    state.phiPrev = state.phi;
    return state;
}

void Step(State& state, const Rest& rest, const Params& params)
{
    const size_t count = state.x.size();
    const int substeps = std::max(1, params.substeps);
    const double h = params.dt / substeps;
    for (int substep = 0; substep < substeps; ++substep)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const Vec old = state.x[i];
            if (i < static_cast<size_t>(params.rootFixed))
            {
                state.x[i] = old;
            }
            else
            {
                const Vec velocity =
                    (state.x[i] - state.prev[i]) * (1.0 - params.damping);
                state.x[i] += velocity +
                    Vec(0.0, params.gravity * params.gravityScale * h * h, 0.0);
            }
            state.prev[i] = old;
        }
        for (size_t i = 0; i < state.phi.size(); ++i)
        {
            const double velocity =
                (state.phi[i] - state.phiPrev[i]) * (1.0 - params.damping);
            state.phiPrev[i] = state.phi[i];
            state.phi[i] += velocity;
        }

        std::vector<double> lambdaStretchScalar(count - 1, 0.0);
        std::vector<Vec> lambdaStretchVector(
            count - 1, Vec::Zero());
        std::vector<Eigen::Vector3d> lambdaBend(count - 2,
            Eigen::Vector3d::Zero());
        std::vector<double> lambdaTwist(count - 2, 0.0);
        const auto applyStretch = [&](size_t i) {
            const double w0 =
                i < static_cast<size_t>(params.rootFixed) ? 0.0 : 1.0;
            const double w1 =
                (i + 1) < static_cast<size_t>(params.rootFixed)
                ? 0.0 : 1.0;
            if (params.useVectorStretchShear)
            {
                const Vec delta = state.x[i + 1] - state.x[i];
                const double restLength = std::max(
                    rest.lengths[i], 1.0e-12);
                const Eigen::Matrix3d frame =
                    CoRotatedFrame(rest.frames[i], delta);
                const Vec local =
                    frame.transpose() * delta / restLength;
                const Vec C = local - Vec(0.0, 0.0, 1.0);
                const Vec alphaTilde(
                    1.0 / (params.GA * restLength * h * h),
                    1.0 / (params.GA * restLength * h * h),
                    1.0 / (params.EA * restLength * h * h));
                const Vec denominator =
                    Vec::Ones() *
                        ((w0 + w1) / (restLength * restLength)) +
                    alphaTilde;
                const Vec deltaLambda =
                    -(C + alphaTilde.cwiseProduct(
                        lambdaStretchVector[i])).cwiseQuotient(
                        denominator);
                lambdaStretchVector[i] += deltaLambda;
                const Vec worldDelta =
                    frame * deltaLambda / restLength;
                state.x[i] -= w0 * worldDelta;
                state.x[i + 1] += w1 * worldDelta;
                return;
            }
            const Vec delta = state.x[i + 1] - state.x[i];
            const double length = delta.norm();
            const double C = length - rest.lengths[i];
            const Vec tangent = delta / length;
            const double alphaTilde =
                (rest.lengths[i] / params.EA) / (h * h);
            const double deltaLambda =
                -(C + alphaTilde * lambdaStretchScalar[i]) /
                (w0 + w1 + alphaTilde);
            lambdaStretchScalar[i] += deltaLambda;
            state.x[i] -= tangent * (w0 * deltaLambda);
            state.x[i + 1] += tangent * (w1 * deltaLambda);
        };
        const auto applyBend = [&](size_t i) {
            Eigen::Vector3d C;
            for (int axis = 0; axis < 3; ++axis)
            {
                C[axis] =
                    BendComponent(state.x, i, axis) - rest.bend[i][axis];
            }
            Eigen::Matrix<double, 9, 3> gradient;
            constexpr double epsilon = 1.0e-6;
            for (int dof = 0; dof < 9; ++dof)
            {
                const size_t point = i + dof / 3;
                const int axis = dof % 3;
                if (point < static_cast<size_t>(params.rootFixed))
                {
                    gradient.row(dof).setZero();
                    continue;
                }
                for (int component = 0; component < 3; ++component)
                {
                    std::vector<Vec> plus = state.x;
                    std::vector<Vec> minus = state.x;
                    plus[point][axis] += epsilon;
                    minus[point][axis] -= epsilon;
                    gradient(dof, component) =
                        (BendComponent(plus, i, component) -
                            BendComponent(minus, i, component)) /
                        (2.0 * epsilon);
                }
            }
            const double alphaTilde =
                (rest.lengths[i] / params.EI) / (h * h);
            Eigen::Matrix3d denominator =
                alphaTilde * Eigen::Matrix3d::Identity();
            for (int dof = 0; dof < 9; ++dof)
            {
                denominator +=
                    gradient.row(dof).transpose() *
                    gradient.row(dof);
            }
            const Eigen::Vector3d deltaLambda =
                -denominator.ldlt().solve(
                    C + alphaTilde * lambdaBend[i]);
            lambdaBend[i] += deltaLambda;
            for (int dof = 0; dof < 9; ++dof)
            {
                state.x[i + dof / 3][dof % 3] +=
                    gradient.row(dof).dot(deltaLambda);
            }
        };
        const auto applyTwist = [&](size_t i) {
            const double C = (state.phi[i + 1] - state.phi[i]) -
                rest.twistDelta[i];
            const double alphaTilde =
                (rest.lengths[i] / params.GJ) / (h * h);
            const double deltaLambda =
                -(C + alphaTilde * lambdaTwist[i]) /
                (2.0 + alphaTilde);
            lambdaTwist[i] += deltaLambda;
            state.phi[i] -= deltaLambda;
            state.phi[i + 1] += deltaLambda;
        };

        const int segmentCount = static_cast<int>(count) - 1;
        const int jointCount = static_cast<int>(count) - 2;
        for (int iteration = 0; iteration < params.iterations; ++iteration)
        {
            switch (params.sweepOrder)
            {
            case SweepOrder::Forward:
                for (int i = 0; i < segmentCount; ++i)
                    applyStretch(static_cast<size_t>(i));
                for (int i = 0; i < jointCount; ++i)
                    applyBend(static_cast<size_t>(i));
                for (int i = 0; i < jointCount; ++i)
                    applyTwist(static_cast<size_t>(i));
                break;
            case SweepOrder::Reverse:
                for (int i = segmentCount - 1; i >= 0; --i)
                    applyStretch(static_cast<size_t>(i));
                for (int i = jointCount - 1; i >= 0; --i)
                    applyBend(static_cast<size_t>(i));
                for (int i = jointCount - 1; i >= 0; --i)
                    applyTwist(static_cast<size_t>(i));
                break;
            case SweepOrder::Interleaved:
                for (int i = 0; i < segmentCount; ++i)
                {
                    applyStretch(static_cast<size_t>(i));
                    if (i < jointCount)
                    {
                        applyBend(static_cast<size_t>(i));
                        applyTwist(static_cast<size_t>(i));
                    }
                }
                for (int i = segmentCount - 1; i >= 0; --i)
                {
                    applyStretch(static_cast<size_t>(i));
                    if (i < jointCount)
                    {
                        applyTwist(static_cast<size_t>(i));
                        applyBend(static_cast<size_t>(i));
                    }
                }
                break;
            }
        }
    }
}

std::vector<Vec> Helix(int count, double radius, double height, int turns)
{
    std::vector<Vec> x;
    x.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        const double s = static_cast<double>(i) / (count - 1);
        const double angle = 2.0 * 3.141592653589793 * turns * s;
        x.emplace_back(radius * std::cos(angle), height * s,
            radius * std::sin(angle));
    }
    return x;
}

double SettleResidual(const std::vector<Vec>& authored, const Rest& rest,
    const Params& params, int settleSteps, State* settled = nullptr)
{
    State state = ResetState(authored, rest);
    for (int i = 0; i < settleSteps; ++i)
        Step(state, rest, params);
    double maxError = 0.0;
    for (size_t i = 0; i < authored.size(); ++i)
        maxError = std::max(maxError, (authored[i] - state.x[i]).norm());
    if (settled != nullptr)
        *settled = state;
    return maxError;
}

void PrintMeasure(const char* label, const std::vector<Vec>& authored,
    const State& state)
{
    double maxDistance = 0.0;
    double sumDistance = 0.0;
    for (size_t i = 0; i < authored.size(); ++i)
    {
        const double distance = (state.x[i] - authored[i]).norm();
        maxDistance = std::max(maxDistance, distance);
        sumDistance += distance;
    }
    std::printf("%-14s max=%8.5f mean=%8.5f tip=%8.5f\n", label,
        maxDistance, sumDistance / authored.size(),
        (state.x.back() - authored.back()).norm());
}

std::vector<Vec> Settle(const std::vector<Vec>& authored, const Rest& rest,
    const Params& params, int settleSteps)
{
    State state = ResetState(authored, rest);
    for (int i = 0; i < settleSteps; ++i)
        Step(state, rest, params);
    return state.x;
}

Eigen::VectorXd ResidualVector(const std::vector<Vec>& authored,
    const std::vector<Vec>& settled, int rootFixed)
{
    const size_t count = authored.size() - static_cast<size_t>(rootFixed);
    Eigen::VectorXd residual(static_cast<Eigen::Index>(count * 3));
    for (size_t i = 0; i < count; ++i)
        for (int axis = 0; axis < 3; ++axis)
            residual[static_cast<Eigen::Index>(i * 3 + axis)] =
                settled[i + static_cast<size_t>(rootFixed)][axis] -
                authored[i + static_cast<size_t>(rootFixed)][axis];
    return residual;
}

Eigen::VectorXd StaticResidualVector(const std::vector<Vec>& authored,
    const Rest& rest, const Params& params)
{
    Params probe = params;
    probe.substeps = 1;
    State state = ResetState(authored, rest);
    Step(state, rest, probe);
    const double inverseDtSquared = 1.0 / (probe.dt * probe.dt);
    const size_t count = authored.size() - static_cast<size_t>(params.rootFixed);
    Eigen::VectorXd residual(static_cast<Eigen::Index>(count * 3));
    for (size_t i = 0; i < count; ++i)
        for (int axis = 0; axis < 3; ++axis)
            residual[static_cast<Eigen::Index>(i * 3 + axis)] =
                (state.x[i + static_cast<size_t>(params.rootFixed)][axis] -
                    authored[i + static_cast<size_t>(params.rootFixed)][axis]) *
                inverseDtSquared;
    return residual;
}

int RunNewton(int points, int newtonIterations, int settleSteps, double lambda)
{
    Params params;
    params.rootFixed = 2;
    const std::vector<Vec> authored = Helix(points, 3.0, 20.0, 1);
    Rest rest = BuildRest(authored, 0.02);

    const size_t joints = authored.size() - 2;
    const Eigen::Index unknownCount = static_cast<Eigen::Index>(joints * 3);
    const auto evaluate = [&](const Rest& candidate) {
        return StaticResidualVector(authored, candidate, params);
    };

    Eigen::VectorXd residual = evaluate(rest);
    std::printf("newton start: points=%d unknowns=%lld residual=%.6f\n",
        points, static_cast<long long>(unknownCount), residual.norm());
    constexpr double delta = 1.0e-4;
    for (int iteration = 0; iteration < newtonIterations; ++iteration)
    {
        Eigen::MatrixXd jacobian(unknownCount, unknownCount);
        for (Eigen::Index column = 0; column < unknownCount; ++column)
        {
            Rest candidate = rest;
            const size_t joint = static_cast<size_t>(column / 3);
            candidate.bend[joint][column % 3] += delta;
            const Eigen::VectorXd shifted = evaluate(candidate);
            jacobian.col(column) = (shifted - residual) / delta;
        }

        const Eigen::MatrixXd normal = jacobian.transpose() * jacobian;
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(normal);
        const double minSingular = std::sqrt(std::max(0.0, solver.eigenvalues().minCoeff()));
        const double maxSingular = std::sqrt(std::max(0.0, solver.eigenvalues().maxCoeff()));
        const Eigen::MatrixXd damped = normal +
            lambda * Eigen::MatrixXd::Identity(unknownCount, unknownCount);
        const Eigen::VectorXd step = damped.ldlt().solve(
            -(jacobian.transpose() * residual));

        double alpha = 1.0;
        Eigen::VectorXd candidateResidual;
        std::vector<Vec> candidatePositions;
        bool accepted = false;
        for (int lineSearch = 0; lineSearch < 8; ++lineSearch)
        {
            Rest candidate = rest;
            for (size_t i = 0; i < joints; ++i)
                for (int axis = 0; axis < 3; ++axis)
                    candidate.bend[i][axis] +=
                        alpha * step[static_cast<Eigen::Index>(i * 3 + axis)];
            candidateResidual = evaluate(candidate);
            if (candidateResidual.norm() < residual.norm())
            {
                accepted = true;
                break;
            }
            alpha *= 0.5;
        }
        std::printf(
            "newton %2d residual %.6f -> %.6f lambda %.2e alpha %.3f sigma [%.3e, %.3e]\n",
            iteration, residual.norm(), candidateResidual.norm(), lambda, alpha,
            minSingular, maxSingular);
        if (!accepted)
        {
            lambda *= 5.0;
            continue;
        }
        Rest acceptedRest = rest;
        for (size_t i = 0; i < joints; ++i)
            for (int axis = 0; axis < 3; ++axis)
                acceptedRest.bend[i][axis] +=
                    alpha * step[static_cast<Eigen::Index>(i * 3 + axis)];
        rest = acceptedRest;
        residual = candidateResidual;
        lambda = std::max(1.0e-6, lambda / 3.0);
        if (residual.norm() < 1.0e-5)
            break;
    }
    const std::vector<Vec> finalPositions =
        Settle(authored, rest, params, settleSteps);
    PrintMeasure("newton final", authored, State{finalPositions, finalPositions, {}, {}});
    return 0;
}

void BendGradient(const std::vector<Vec>& x, size_t joint,
    Eigen::Matrix<double, 9, 3>& gradient)
{
    constexpr double epsilon = 1.0e-6;
    for (int dof = 0; dof < 9; ++dof)
    {
        const size_t point = joint + static_cast<size_t>(dof / 3);
        const int axis = dof % 3;
        for (int component = 0; component < 3; ++component)
        {
            std::vector<Vec> plus = x;
            std::vector<Vec> minus = x;
            plus[point][axis] += epsilon;
            minus[point][axis] -= epsilon;
            gradient(dof, component) =
                (BendComponent(plus, joint, component) -
                    BendComponent(minus, joint, component)) /
                (2.0 * epsilon);
        }
    }
}

Eigen::VectorXd ForceResidual(const std::vector<Vec>& authored,
    const Rest& rest, const Params& params)
{
    const size_t count = authored.size() - static_cast<size_t>(params.rootFixed);
    Eigen::VectorXd force = Eigen::VectorXd::Zero(
        static_cast<Eigen::Index>(count * 3));
    for (size_t i = 0; i < count; ++i)
        force[static_cast<Eigen::Index>(i * 3 + 1)] = params.gravity;

    for (size_t edge = 0; edge + 1 < authored.size(); ++edge)
    {
        const Vec delta = authored[edge + 1] - authored[edge];
        const double length = delta.norm();
        const double C = length - rest.lengths[edge];
        const Vec tangent = delta / length;
        const double stiffness = params.EA / std::max(rest.lengths[edge], 1.0e-12);
        const Vec edgeForce = -stiffness * C * tangent;
        for (int endpoint = 0; endpoint < 2; ++endpoint)
        {
            const size_t point = edge + static_cast<size_t>(endpoint);
            if (point < static_cast<size_t>(params.rootFixed))
                continue;
            const size_t local = point - static_cast<size_t>(params.rootFixed);
            for (int axis = 0; axis < 3; ++axis)
                force[static_cast<Eigen::Index>(local * 3 + axis)] +=
                    (endpoint == 0 ? -1.0 : 1.0) * edgeForce[axis];
        }
    }

    for (size_t joint = 0; joint + 2 < authored.size(); ++joint)
    {
        const Vec t0 = (authored[joint + 1] - authored[joint]).normalized();
        const Vec t1 = (authored[joint + 2] - authored[joint + 1]).normalized();
        const Vec C = t0.cross(t1) - rest.bend[joint];
        Eigen::Matrix<double, 9, 3> gradient;
        BendGradient(authored, joint, gradient);
        const double stiffness = params.EI / std::max(rest.lengths[joint], 1.0e-12);
        for (int dof = 0; dof < 9; ++dof)
        {
            const size_t point = joint + static_cast<size_t>(dof / 3);
            if (point < static_cast<size_t>(params.rootFixed))
                continue;
            const size_t local = point - static_cast<size_t>(params.rootFixed);
            const double scalar = -stiffness * gradient.row(dof).dot(C);
            force[static_cast<Eigen::Index>(local * 3 + dof % 3)] += scalar;
        }
    }
    return force;
}

int RunEnergyNewton(int points, int newtonIterations, int settleSteps, double lambda)
{
    Params params;
    params.rootFixed = 2;
    const std::vector<Vec> authored = Helix(points, 3.0, 20.0, 1);
    Rest rest = BuildRest(authored, 0.02);
    const size_t joints = authored.size() - 2;
    const Eigen::Index unknownCount = static_cast<Eigen::Index>(joints * 3);
    const auto evaluate = [&](const Rest& candidate) {
        return ForceResidual(authored, candidate, params);
    };
    Eigen::VectorXd residual = evaluate(rest);
    std::printf("energy newton start: points=%d unknowns=%lld residual=%.6f\n",
        points, static_cast<long long>(unknownCount), residual.norm());
    constexpr double delta = 1.0e-4;
    for (int iteration = 0; iteration < newtonIterations; ++iteration)
    {
        Eigen::MatrixXd jacobian(unknownCount, unknownCount);
        for (Eigen::Index column = 0; column < unknownCount; ++column)
        {
            Rest candidate = rest;
            const size_t joint = static_cast<size_t>(column / 3);
            candidate.bend[joint][column % 3] += delta;
            jacobian.col(column) = (evaluate(candidate) - residual) / delta;
        }
        const Eigen::MatrixXd normal = jacobian.transpose() * jacobian;
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(normal);
        const double minSingular = std::sqrt(std::max(0.0, solver.eigenvalues().minCoeff()));
        const double maxSingular = std::sqrt(std::max(0.0, solver.eigenvalues().maxCoeff()));
        const Eigen::MatrixXd damped = normal +
            lambda * Eigen::MatrixXd::Identity(unknownCount, unknownCount);
        const Eigen::VectorXd step = damped.ldlt().solve(
            -(jacobian.transpose() * residual));

        double alpha = 1.0;
        Eigen::VectorXd candidateResidual;
        Rest accepted = rest;
        bool acceptedStep = false;
        for (int lineSearch = 0; lineSearch < 12; ++lineSearch)
        {
            Rest candidate = rest;
            for (size_t i = 0; i < joints; ++i)
                for (int axis = 0; axis < 3; ++axis)
                    candidate.bend[i][axis] +=
                        alpha * step[static_cast<Eigen::Index>(i * 3 + axis)];
            candidateResidual = evaluate(candidate);
            if (candidateResidual.norm() < residual.norm())
            {
                accepted = candidate;
                acceptedStep = true;
                break;
            }
            alpha *= 0.5;
        }
        std::printf(
            "energy newton %2d residual %.6f -> %.6f lambda %.2e alpha %.3f sigma [%.3e, %.3e]\n",
            iteration, residual.norm(), candidateResidual.norm(), lambda, alpha,
            minSingular, maxSingular);
        if (!acceptedStep)
        {
            lambda *= 5.0;
            continue;
        }
        rest = accepted;
        residual = candidateResidual;
        lambda = std::max(1.0e-6, lambda / 3.0);
        if (residual.norm() < 1.0e-6)
            break;
    }
    const std::vector<Vec> finalPositions = Settle(authored, rest, params, settleSteps);
    PrintMeasure("energy final", authored,
        State{finalPositions, finalPositions, {}, {}});
    return 0;
}

struct BendBasis
{
    Vec along;
    Vec binormal;
    Vec third;
};

std::vector<BendBasis> BuildBendBasis(const std::vector<Vec>& authored)
{
    std::vector<BendBasis> basis(authored.size() - 2);
    for (size_t joint = 0; joint + 2 < authored.size(); ++joint)
    {
        const Vec t0 = (authored[joint + 1] - authored[joint]).normalized();
        const Vec t1 = (authored[joint + 2] - authored[joint + 1]).normalized();
        const Vec bisector = (t0 + t1).normalized();
        Vec binormal = t0.cross(t1);
        if (binormal.squaredNorm() < 1.0e-16)
        {
            const Vec fallback = std::abs(bisector.z()) < 0.9
                ? Vec(0.0, 0.0, 1.0) : Vec(1.0, 0.0, 0.0);
            binormal = bisector.cross(fallback);
        }
        binormal.normalize();
        Vec along = t0 - t1;
        along -= bisector * along.dot(bisector);
        if (along.squaredNorm() < 1.0e-16)
            along = bisector.cross(binormal);
        along.normalize();
        Vec third = along.cross(binormal);
        if (third.squaredNorm() < 1.0e-16)
            third = bisector;
        third.normalize();
        basis[joint] = {along, binormal, third};
    }
    return basis;
}

int RunSvdNewton(int points, int newtonIterations, int settleSteps)
{
    Params params;
    params.rootFixed = 2;
    const std::vector<Vec> authored = Helix(points, 3.0, 20.0, 1);
    Rest rest = BuildRest(authored, 0.02);
    const std::vector<BendBasis> basis = BuildBendBasis(authored);
    const size_t joints = authored.size() - 2;
    const size_t lengthCount = authored.size() - 1;
    const Eigen::Index unknownCount =
        static_cast<Eigen::Index>(lengthCount + joints * 2);
    const auto evaluate = [&](const Rest& candidate) {
        return ForceResidual(authored, candidate, params);
    };
    Eigen::VectorXd residual = evaluate(rest);
    std::printf("svd newton start: points=%d unknowns=%lld residual=%.6f\n",
        points, static_cast<long long>(unknownCount), residual.norm());
    constexpr double delta = 1.0e-4;
    for (int iteration = 0; iteration < newtonIterations; ++iteration)
    {
        Eigen::MatrixXd jacobian(
            residual.size(), unknownCount);
        for (size_t i = 0; i < lengthCount; ++i)
        {
            Rest candidate = rest;
            candidate.lengths[i] += delta;
            jacobian.col(static_cast<Eigen::Index>(i)) =
                (evaluate(candidate) - residual) / delta;
        }
        for (size_t joint = 0; joint < joints; ++joint)
        {
            for (int axis = 0; axis < 2; ++axis)
            {
                Rest candidate = rest;
                candidate.bend[joint] += delta * (axis == 0
                    ? basis[joint].along : basis[joint].binormal);
                jacobian.col(static_cast<Eigen::Index>(lengthCount + joint * 2 + axis)) =
                    (evaluate(candidate) - residual) / delta;
            }
        }
        Eigen::JacobiSVD<Eigen::MatrixXd> svd(
            jacobian, Eigen::ComputeThinU | Eigen::ComputeFullV);
        const Eigen::VectorXd singular = svd.singularValues();
        const double sigmaMax = singular.maxCoeff();
        const double tolerance = std::max(1.0e-10, sigmaMax * 1.0e-6);
        Eigen::VectorXd inverse = Eigen::VectorXd::Zero(singular.size());
        int rank = 0;
        for (Eigen::Index i = 0; i < singular.size(); ++i)
        {
            if (singular[i] > tolerance)
            {
                inverse[i] = 1.0 / singular[i];
                ++rank;
            }
        }
        const Eigen::VectorXd step = -svd.matrixV().leftCols(singular.size()) *
            inverse.asDiagonal() * svd.matrixU().transpose() * residual;
        {
            const Eigen::VectorXd nullVector =
                svd.matrixV().col(unknownCount - 1);
            std::printf("  null vector: |v|=%.3f lengths=[",
                nullVector.norm());
            for (size_t i = 0; i < lengthCount; ++i)
                std::printf("%s%.3f", i == 0 ? "" : ",",
                    nullVector[static_cast<Eigen::Index>(i)]);
            std::printf("] bends=[");
            for (size_t joint = 0; joint < joints; ++joint)
                std::printf("%s(%.3f,%.3f)", joint == 0 ? "" : ",",
                    nullVector[static_cast<Eigen::Index>(lengthCount + joint * 2)],
                    nullVector[static_cast<Eigen::Index>(lengthCount + joint * 2 + 1)]);
            std::printf("]\n");
        }

        double alpha = 1.0;
        Eigen::VectorXd candidateResidual;
        Rest accepted = rest;
        bool acceptedStep = false;
        for (int lineSearch = 0; lineSearch < 12; ++lineSearch)
        {
            Rest candidate = rest;
            for (size_t i = 0; i < lengthCount; ++i)
                candidate.lengths[i] +=
                    alpha * step[static_cast<Eigen::Index>(i)];
            for (size_t joint = 0; joint < joints; ++joint)
                for (int axis = 0; axis < 2; ++axis)
                    candidate.bend[joint] += alpha *
                        step[static_cast<Eigen::Index>(lengthCount + joint * 2 + axis)] *
                        (axis == 0 ? basis[joint].along : basis[joint].binormal);
            candidateResidual = evaluate(candidate);
            if (candidateResidual.norm() < residual.norm())
            {
                accepted = candidate;
                acceptedStep = true;
                break;
            }
            alpha *= 0.5;
        }
        std::printf(
            "svd newton %2d residual %.6f -> %.6f rank %d/%lld sigma max %.3e tol %.3e alpha %.3f\n",
            iteration, residual.norm(), candidateResidual.norm(), rank,
            static_cast<long long>(singular.size()), sigmaMax, tolerance, alpha);
        if (!acceptedStep)
            break;
        rest = accepted;
        residual = candidateResidual;
        if (residual.norm() < 1.0e-6)
            break;
    }
    const std::vector<Vec> finalPositions = Settle(authored, rest, params, settleSteps);
    PrintMeasure("svd final", authored,
        State{finalPositions, finalPositions, {}, {}});
    {
        const size_t movable = authored.size() - static_cast<size_t>(params.rootFixed);
        const Eigen::Index dofCount = static_cast<Eigen::Index>(movable * 3);
        Eigen::MatrixXd hessian(dofCount, dofCount);
        const Eigen::VectorXd base = ForceResidual(authored, rest, params);
        constexpr double epsilon = 1.0e-4;
        for (Eigen::Index column = 0; column < dofCount; ++column)
        {
            std::vector<Vec> perturbed = authored;
            const size_t point =
                static_cast<size_t>(column / 3) + static_cast<size_t>(params.rootFixed);
            perturbed[point][column % 3] += epsilon;
            hessian.col(column) =
                (ForceResidual(perturbed, rest, params) - base) / epsilon;
        }
        const Eigen::MatrixXd symmetric =
            0.5 * (hessian + hessian.transpose());
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eigen(symmetric);
        std::printf("stability eigenvalues: min %.6e max %.6e\n",
            eigen.eigenvalues().minCoeff(), eigen.eigenvalues().maxCoeff());
    }
    for (size_t i = 0; i < authored.size(); ++i)
    {
        const double deviation = (finalPositions[i] - authored[i]).norm();
        if (deviation > 0.2)
            std::printf("  point %zu deviation %.4f\n", i, deviation);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// M0-006: gauge-free rest inverse solve with a static-Hessian stability gate.
//
// The position model is still the M0 chain: stretch + 2-DOF rest bend + scalar
// twist.  M0-006 changes only the inverse problem:
//   * rest lengths whose two endpoints are both fixed are gauges and stay at
//     the authored value instead of being solved;
//   * rest bend is parameterized in the two authored tangent-plane directions;
//   * a force-balance root is accepted only when the position Hessian at the
//     authored shape is positive definite.
// Twist is intentionally outside this position Hessian: phi is a scalar state
// constrained independently of x in this prototype, and rest twist is not a
// position unknown.  A full angular stability check would need a separate
// orientation Hessian and is not claimed here.
//
// Paper mapping note (2026-11-14 review):
// This remains a node-based small-angle prototype.  The 3-DOF rest bend adds a
// tangential/twist component in the authored tangent-plane basis; it is not a
// segment-center quaternion.  Kugelstadt 2016 uses a segment-center q_s with
//   Omega_s = (2/Lbar_s) Im(q_s^{-1} q_{s+1}),
// Hsu uses 2 log(q_s^{-1} q_{s+1})/Lbar_s, and Bergou uses 2 tan(phi/2).
// A node-quaternion variant must define the q_i -> q_s map explicitly, e.g.
//   c_s = (x_s + x_{s+1})/2,
//   q_s = slerp(q_s, q_{s+1}, 1/2) (or the relative-rotation midpoint),
//   Lbar_s = (L_s + L_{s+1})/2.
// That segment-center variant is not implemented in this pass.
// ---------------------------------------------------------------------------

double TotalLength(const std::vector<Vec>& points)
{
    double length = 0.0;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        length += (points[i + 1] - points[i]).norm();
    return length;
}

std::vector<Vec> StraightChain(int count, double length)
{
    std::vector<Vec> points;
    points.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const double s = static_cast<double>(i) /
            static_cast<double>(std::max(1, count - 1));
        points.emplace_back(0.0, -length * s, 0.0);
    }
    return points;
}

std::vector<Vec> InitialBendChain(int count, double length, double totalAngle)
{
    std::vector<Vec> points;
    points.reserve(static_cast<size_t>(count));
    const double radius = length / totalAngle;
    for (int i = 0; i < count; ++i)
    {
        const double s = static_cast<double>(i) /
            static_cast<double>(std::max(1, count - 1));
        const double angle = totalAngle * s;
        points.emplace_back(radius * std::sin(angle),
            -radius * (1.0 - std::cos(angle)), 0.0);
    }
    return points;
}

struct RodModel
{
    Params params;
    std::vector<Vec> authored;
    Rest reference;
    std::vector<BendBasis> basis;
    std::vector<size_t> lengthEdges;
    std::vector<size_t> bendJoints;
    std::vector<Eigen::Matrix3d> segmentFrames;
    double lengthScale = 1.0;
    bool useThreeDofBend = false;
    bool useVectorStretchShear = false;

    Eigen::Index UnknownCount() const
    {
        const size_t bendDofs = useThreeDofBend ? 3 : 2;
        return static_cast<Eigen::Index>(
            lengthEdges.size() + bendJoints.size() * bendDofs);
    }

    Eigen::Index ForceCount() const
    {
        return static_cast<Eigen::Index>(
            (authored.size() - static_cast<size_t>(params.rootFixed)) * 3);
    }
};

RodModel BuildRodModel(const std::vector<Vec>& authored, const Params& params,
    bool threeDofBend = false, bool vectorStretchShear = false)
{
    RodModel model;
    model.params = params;
    model.authored = authored;
    model.useThreeDofBend = threeDofBend;
    model.useVectorStretchShear = vectorStretchShear;
    model.params.useVectorStretchShear = vectorStretchShear;
    model.reference = BuildRest(authored, 0.02);
    model.basis = BuildBendBasis(authored);
    const double totalLength = TotalLength(authored);
    model.lengthScale =
        totalLength / static_cast<double>(authored.size() - 1);
    model.segmentFrames = model.reference.frames;
    for (size_t edge = 0; edge + 1 < authored.size(); ++edge)
    {
        const bool bothFixed =
            edge + 1 < static_cast<size_t>(params.rootFixed);
        if (!bothFixed)
            model.lengthEdges.push_back(edge);
    }
    for (size_t joint = 0; joint + 2 < authored.size(); ++joint)
        model.bendJoints.push_back(joint);
    return model;
}

Eigen::VectorXd Pack(const RodModel& model, const Rest& rest)
{
    Eigen::VectorXd packed(model.UnknownCount());
    Eigen::Index index = 0;
    for (size_t edge : model.lengthEdges)
    {
        packed[index++] =
            (rest.lengths[edge] - model.reference.lengths[edge]) /
            model.lengthScale;
    }
    for (size_t joint : model.bendJoints)
    {
        const Vec delta = rest.bend[joint] - model.reference.bend[joint];
        packed[index++] = delta.dot(model.basis[joint].along);
        packed[index++] = delta.dot(model.basis[joint].binormal);
        if (model.useThreeDofBend)
            packed[index++] = delta.dot(model.basis[joint].third);
    }
    return packed;
}

bool Unpack(const RodModel& model, const Eigen::VectorXd& packed, Rest& rest)
{
    if (packed.size() != model.UnknownCount() || !packed.allFinite())
        return false;
    rest = model.reference;
    Eigen::Index index = 0;
    for (size_t edge : model.lengthEdges)
    {
        const double reference = model.reference.lengths[edge];
        const double length =
            reference + model.lengthScale * packed[index++];
        if (!std::isfinite(length) || length <= 0.05 * reference ||
            length >= 20.0 * reference)
        {
            return false;
        }
        rest.lengths[edge] = length;
    }
    for (size_t joint : model.bendJoints)
    {
        const double along = packed[index++];
        const double binormal = packed[index++];
        const double third =
            model.useThreeDofBend ? packed[index++] : 0.0;
        rest.bend[joint] = model.reference.bend[joint] +
            along * model.basis[joint].along +
            binormal * model.basis[joint].binormal +
            third * model.basis[joint].third;
    }
    return true;
}

void BendGradientAnalytic(const std::vector<Vec>& x, size_t joint,
    Eigen::Matrix<double, 9, 3>& gradient)
{
    const Vec e0 = x[joint + 1] - x[joint];
    const Vec e1 = x[joint + 2] - x[joint + 1];
    const double length0 = e0.norm();
    const double length1 = e1.norm();
    const Vec t0 = e0 / length0;
    const Vec t1 = e1 / length1;
    const Eigen::Matrix3d identity = Eigen::Matrix3d::Identity();
    const Eigen::Matrix3d p0 =
        (identity - t0 * t0.transpose()) / length0;
    const Eigen::Matrix3d p1 =
        (identity - t1 * t1.transpose()) / length1;
    Eigen::Matrix3d s0;
    s0 << 0.0, -t0.z(), t0.y(),
        t0.z(), 0.0, -t0.x(),
        -t0.y(), t0.x(), 0.0;
    Eigen::Matrix3d s1;
    s1 << 0.0, -t1.z(), t1.y(),
        t1.z(), 0.0, -t1.x(),
        -t1.y(), t1.x(), 0.0;
    const Eigen::Matrix3d g0 = s1 * p0;
    const Eigen::Matrix3d g1 = -s1 * p0 - s0 * p1;
    const Eigen::Matrix3d g2 = s0 * p1;
    for (int component = 0; component < 3; ++component)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            gradient(axis, component) = g0(component, axis);
            gradient(3 + axis, component) = g1(component, axis);
            gradient(6 + axis, component) = g2(component, axis);
        }
    }
}

Eigen::VectorXd StretchForceAt(const RodModel& model,
    const std::vector<Vec>& positions, const Rest& rest)
{
    const Params& params = model.params;
    const size_t movable =
        positions.size() - static_cast<size_t>(params.rootFixed);
    Eigen::VectorXd force = Eigen::VectorXd::Zero(
        static_cast<Eigen::Index>(movable * 3));
    for (size_t edge = 0; edge + 1 < positions.size(); ++edge)
    {
        const Vec delta = positions[edge + 1] - positions[edge];
        const double length = delta.norm();
        if (!(length > 1.0e-12))
        {
            force.setConstant(
                std::numeric_limits<double>::quiet_NaN());
            return force;
        }
          Vec edgeForce;
          if (model.useVectorStretchShear)
          {
              const double restLength = std::max(
                  rest.lengths[edge], 1.0e-12);
                const Eigen::Matrix3d frame =
                    CoRotatedFrame(
                        model.segmentFrames[edge], delta);
              const Vec local =
                  frame.transpose() * delta / restLength;
              const Vec strain = local - Vec(0.0, 0.0, 1.0);
              const Vec localForce = -Vec(
                  params.GA * restLength * strain.x(),
                  params.GA * restLength * strain.y(),
                  params.EA * restLength * strain.z()) / restLength;
              edgeForce = frame * localForce;
          }
          else
          {
              const double C = length - rest.lengths[edge];
              const Vec tangent = delta / length;
              const double stiffness = params.EA / std::max(
                  rest.lengths[edge], 1.0e-12);
              edgeForce = -stiffness * C * tangent;
          }
        for (int endpoint = 0; endpoint < 2; ++endpoint)
        {
            const size_t point = edge + static_cast<size_t>(endpoint);
            if (point < static_cast<size_t>(params.rootFixed))
                continue;
            const size_t local =
                point - static_cast<size_t>(params.rootFixed);
            for (int axis = 0; axis < 3; ++axis)
            {
                force[static_cast<Eigen::Index>(local * 3 + axis)] +=
                    (endpoint == 0 ? -1.0 : 1.0) * edgeForce[axis];
            }
        }
    }
    return force;
}

Eigen::VectorXd BendForceAt(const RodModel& model,
    const std::vector<Vec>& positions, const Rest& rest)
{
    const Params& params = model.params;
    const size_t movable =
        positions.size() - static_cast<size_t>(params.rootFixed);
    Eigen::VectorXd force = Eigen::VectorXd::Zero(
        static_cast<Eigen::Index>(movable * 3));
    for (size_t joint = 0; joint + 2 < positions.size(); ++joint)
    {
        const Vec t0 =
            (positions[joint + 1] - positions[joint]).normalized();
        const Vec t1 =
            (positions[joint + 2] - positions[joint + 1]).normalized();
        const Vec C = t0.cross(t1) - rest.bend[joint];
        Eigen::Matrix<double, 9, 3> gradient;
        BendGradientAnalytic(positions, joint, gradient);
        const double stiffness =
            params.EI / std::max(rest.lengths[joint], 1.0e-12);
        for (int dof = 0; dof < 9; ++dof)
        {
            const size_t point =
                joint + static_cast<size_t>(dof / 3);
            if (point < static_cast<size_t>(params.rootFixed))
                continue;
            const size_t local =
                point - static_cast<size_t>(params.rootFixed);
            const double scalar =
                -stiffness * gradient.row(dof).dot(C);
            force[static_cast<Eigen::Index>(local * 3 + dof % 3)] +=
                scalar;
        }
    }
    return force;
}

Eigen::VectorXd BendJointForceAt(const RodModel& model,
    const std::vector<Vec>& positions, const Rest& rest, size_t joint)
{
    const Params& params = model.params;
    Eigen::VectorXd force = Eigen::VectorXd::Zero(
        static_cast<Eigen::Index>(positions.size() * 3));
    const Vec t0 =
        (positions[joint + 1] - positions[joint]).normalized();
    const Vec t1 =
        (positions[joint + 2] - positions[joint + 1]).normalized();
    const Vec C = t0.cross(t1) - rest.bend[joint];
    Eigen::Matrix<double, 9, 3> gradient;
    BendGradientAnalytic(positions, joint, gradient);
    const double stiffness =
        params.EI / std::max(rest.lengths[joint], 1.0e-12);
    for (int dof = 0; dof < 9; ++dof)
    {
        const size_t point =
            joint + static_cast<size_t>(dof / 3);
        const double scalar =
            -stiffness * gradient.row(dof).dot(C);
        force[static_cast<Eigen::Index>(point * 3 + dof % 3)] +=
            scalar;
    }
    return force;
}

double MaxBendTorqueResidual(const RodModel& model,
    const std::vector<Vec>& positions, const Rest& rest,
    int* worstJoint = nullptr)
{
    double maxTorque = 0.0;
    int worst = -1;
    for (size_t joint = 0; joint + 2 < positions.size(); ++joint)
    {
        const Eigen::VectorXd force =
            BendJointForceAt(model, positions, rest, joint);
        Vec torque = Vec::Zero();
        for (int localPoint = 0; localPoint < 3; ++localPoint)
        {
            const size_t point =
                joint + static_cast<size_t>(localPoint);
            const Vec forcePoint(
                  force[static_cast<Eigen::Index>(point * 3 + 0)],
                  force[static_cast<Eigen::Index>(point * 3 + 1)],
                  force[static_cast<Eigen::Index>(point * 3 + 2)]);
            const Vec lever = positions[point] - positions[joint + 1];
            torque += lever.cross(forcePoint);
        }
        const double magnitude = torque.norm();
        if (magnitude > maxTorque)
        {
            maxTorque = magnitude;
            worst = static_cast<int>(joint);
        }
    }
    if (worstJoint != nullptr)
        *worstJoint = worst;
    return maxTorque;
}

Eigen::VectorXd GravityForceAt(const RodModel& model,
    const std::vector<Vec>& positions, double gravityScale)
{
    const size_t movable = positions.size() -
        static_cast<size_t>(model.params.rootFixed);
    Eigen::VectorXd force = Eigen::VectorXd::Zero(
        static_cast<Eigen::Index>(movable * 3));
    for (size_t i = 0; i < movable; ++i)
    {
        force[static_cast<Eigen::Index>(i * 3 + 1)] =
            model.params.gravity * gravityScale;
    }
    return force;
}

Eigen::VectorXd ForceResidualExactAt(const RodModel& model,
    const std::vector<Vec>& positions, const Rest& rest, double gravityScale)
{
    return StretchForceAt(model, positions, rest) +
        BendForceAt(model, positions, rest) +
        GravityForceAt(model, positions, gravityScale);
}

Eigen::VectorXd ForceResidualExact(const RodModel& model, const Rest& rest,
    double gravityScale)
{
    return ForceResidualExactAt(model, model.authored, rest, gravityScale);
}

Eigen::VectorXd GravityVector(const RodModel& model, double gravityScale)
{
    const size_t movable = model.authored.size() -
        static_cast<size_t>(model.params.rootFixed);
    Eigen::VectorXd gravity = Eigen::VectorXd::Zero(
        static_cast<Eigen::Index>(movable * 3));
    for (size_t i = 0; i < movable; ++i)
    {
        gravity[static_cast<Eigen::Index>(i * 3 + 1)] =
            model.params.gravity * gravityScale;
    }
    return gravity;
}

Eigen::MatrixXd ComputeJacobian(const RodModel& model, const Rest& rest,
    double gravityScale)
{
    const Eigen::Index unknowns = model.UnknownCount();
    const Eigen::Index forces = model.ForceCount();
    Eigen::MatrixXd jacobian(forces, unknowns);
    const Eigen::VectorXd base = Pack(model, rest);
    const double step = 1.0e-6;
    for (Eigen::Index column = 0; column < unknowns; ++column)
    {
        Eigen::VectorXd plus = base;
        Eigen::VectorXd minus = base;
        plus[column] += step;
        minus[column] -= step;
        Rest plusRest;
        Rest minusRest;
        if (!Unpack(model, plus, plusRest) ||
            !Unpack(model, minus, minusRest))
        {
            jacobian.col(column).setZero();
            continue;
        }
        const Eigen::VectorXd forcePlus =
            ForceResidualExact(model, plusRest, gravityScale);
        const Eigen::VectorXd forceMinus =
            ForceResidualExact(model, minusRest, gravityScale);
        if (!forcePlus.allFinite() || !forceMinus.allFinite())
        {
            jacobian.col(column).setZero();
            continue;
        }
        jacobian.col(column) =
            (forcePlus - forceMinus) / (2.0 * step);
    }
    return jacobian;
}

Eigen::VectorXd MinimumNormSolve(const Eigen::MatrixXd& matrix,
    const Eigen::VectorXd& rhs)
{
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(matrix,
        Eigen::ComputeThinU | Eigen::ComputeFullV);
    const Eigen::VectorXd singular = svd.singularValues();
    const double tolerance =
        static_cast<double>(std::max(matrix.rows(), matrix.cols())) *
        std::numeric_limits<double>::epsilon() *
        std::max(1.0, singular.maxCoeff()) * 100.0;
    Eigen::VectorXd inverse = Eigen::VectorXd::Zero(singular.size());
    for (Eigen::Index i = 0; i < singular.size(); ++i)
    {
        if (singular[i] > tolerance)
            inverse[i] = 1.0 / singular[i];
    }
    return svd.matrixV().leftCols(singular.size()) *
        inverse.asDiagonal() * svd.matrixU().transpose() * rhs;
}

enum class HessianComponent
{
    Total,
    Stretch,
    Bend,
    Gravity
};

Eigen::MatrixXd ComputePositionHessian(const RodModel& model,
    const Rest& rest, double gravityScale, HessianComponent component)
{
    const std::vector<Vec>& authored = model.authored;
    const size_t movable = authored.size() -
        static_cast<size_t>(model.params.rootFixed);
    const Eigen::Index dof = static_cast<Eigen::Index>(movable * 3);
    Eigen::MatrixXd hessian(dof, dof);
    const double step = 1.0e-5 * model.lengthScale;
    for (Eigen::Index column = 0; column < dof; ++column)
    {
        const size_t point =
            static_cast<size_t>(column / 3) +
            static_cast<size_t>(model.params.rootFixed);
        const int axis = static_cast<int>(column % 3);
        std::vector<Vec> plus = authored;
        std::vector<Vec> minus = authored;
        plus[point][axis] += step;
        minus[point][axis] -= step;
        Eigen::VectorXd forcePlus;
        Eigen::VectorXd forceMinus;
        switch (component)
        {
        case HessianComponent::Stretch:
            forcePlus = StretchForceAt(model, plus, rest);
            forceMinus = StretchForceAt(model, minus, rest);
            break;
        case HessianComponent::Bend:
            forcePlus = BendForceAt(model, plus, rest);
            forceMinus = BendForceAt(model, minus, rest);
            break;
        case HessianComponent::Gravity:
            forcePlus = GravityForceAt(model, plus, gravityScale);
            forceMinus = GravityForceAt(model, minus, gravityScale);
            break;
        case HessianComponent::Total:
        default:
            forcePlus = ForceResidualExactAt(
                model, plus, rest, gravityScale);
            forceMinus = ForceResidualExactAt(
                model, minus, rest, gravityScale);
            break;
        }
        if (!forcePlus.allFinite() || !forceMinus.allFinite())
        {
            hessian.setConstant(
                std::numeric_limits<double>::quiet_NaN());
            return hessian;
        }
        hessian.col(column) =
            -(forcePlus - forceMinus) / (2.0 * step);
    }
    return 0.5 * (hessian + hessian.transpose());
}

Eigen::MatrixXd ComputeStretchMaterialHessian(const RodModel& model,
    const Rest& rest)
{
    const std::vector<Vec>& authored = model.authored;
    const size_t movable = authored.size() -
        static_cast<size_t>(model.params.rootFixed);
    const Eigen::Index dof = static_cast<Eigen::Index>(movable * 3);
    Eigen::MatrixXd hessian = Eigen::MatrixXd::Zero(dof, dof);
    for (size_t edge = 0; edge + 1 < authored.size(); ++edge)
    {
        const Vec delta = authored[edge + 1] - authored[edge];
        const double length = delta.norm();
        if (!(length > 1.0e-12))
            continue;
        const Vec tangent = delta / length;
          Eigen::Matrix3d block;
          if (model.useVectorStretchShear)
          {
              const double restLength = std::max(
                  rest.lengths[edge], 1.0e-12);
              Eigen::Matrix3d stiffness =
                  Eigen::Matrix3d::Zero();
              stiffness(0, 0) = model.params.GA * restLength;
              stiffness(1, 1) = model.params.GA * restLength;
              stiffness(2, 2) = model.params.EA * restLength;
              const Eigen::Matrix3d frame =
                  CoRotatedFrame(
                      model.segmentFrames[edge], delta);
              block = frame * stiffness * frame.transpose() /
                  (restLength * restLength);
          }
          else
          {
              const double stiffness = model.params.EA /
                  std::max(rest.lengths[edge], 1.0e-12);
              block = stiffness * tangent * tangent.transpose();
          }
        for (int endpointA = 0; endpointA < 2; ++endpointA)
        {
            const size_t pointA =
                edge + static_cast<size_t>(endpointA);
            if (pointA < static_cast<size_t>(model.params.rootFixed))
                continue;
            const Eigen::Index localA = static_cast<Eigen::Index>(
                (pointA - static_cast<size_t>(model.params.rootFixed)) * 3);
            for (int endpointB = 0; endpointB < 2; ++endpointB)
            {
                const size_t pointB =
                    edge + static_cast<size_t>(endpointB);
                if (pointB <
                    static_cast<size_t>(model.params.rootFixed))
                {
                    continue;
                }
                const Eigen::Index localB = static_cast<Eigen::Index>(
                    (pointB -
                        static_cast<size_t>(model.params.rootFixed)) * 3);
                const double sign =
                    endpointA == endpointB ? 1.0 : -1.0;
                hessian.block<3, 3>(localA, localB) += sign * block;
            }
        }
    }
    return hessian;
}

Eigen::MatrixXd ComputeBendMaterialHessian(const RodModel& model,
    const Rest& rest)
{
    const std::vector<Vec>& authored = model.authored;
    const size_t movable = authored.size() -
        static_cast<size_t>(model.params.rootFixed);
    const Eigen::Index dof = static_cast<Eigen::Index>(movable * 3);
    Eigen::MatrixXd hessian = Eigen::MatrixXd::Zero(dof, dof);
    for (size_t joint = 0; joint + 2 < authored.size(); ++joint)
    {
        Eigen::Matrix<double, 9, 3> gradient;
        BendGradientAnalytic(authored, joint, gradient);
        const double stiffness =
            model.params.EI /
            std::max(rest.lengths[joint], 1.0e-12);
        int index[9];
        for (int local = 0; local < 9; ++local)
        {
            const size_t point =
                joint + static_cast<size_t>(local / 3);
            index[local] = point <
                static_cast<size_t>(model.params.rootFixed)
                ? -1
                : static_cast<int>(
                    (point -
                        static_cast<size_t>(model.params.rootFixed)) * 3 +
                    static_cast<size_t>(local % 3));
        }
        for (int a = 0; a < 9; ++a)
        {
            if (index[a] < 0)
                continue;
            for (int b = 0; b < 9; ++b)
            {
                if (index[b] < 0)
                    continue;
                hessian(index[a], index[b]) +=
                    stiffness * gradient.row(a).dot(gradient.row(b));
            }
        }
    }
    return hessian;
}

struct StabilityResult
{
    bool finite = true;
    double minEigenvalue = 0.0;
    double maxEigenvalue = 0.0;
    std::vector<double> smallestEigenvalues;
    Eigen::VectorXd minEigenvector;
    int minPoint = -1;
    int minAxis = -1;
    double minComponent = 0.0;
};

StabilityResult ComputeStability(const RodModel& model, const Rest& rest,
    double gravityScale)
{
    StabilityResult result;
    const Eigen::MatrixXd symmetric = ComputePositionHessian(
        model, rest, gravityScale, HessianComponent::Total);
    if (!symmetric.allFinite())
    {
        result.finite = false;
        return result;
    }
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(symmetric);
    if (solver.info() != Eigen::Success)
    {
        result.finite = false;
        return result;
    }
    result.minEigenvalue = solver.eigenvalues().minCoeff();
    result.maxEigenvalue = solver.eigenvalues().maxCoeff();
    result.minEigenvector = solver.eigenvectors().col(0);
    Eigen::Index index = 0;
    result.minEigenvector.cwiseAbs().maxCoeff(&index);
    result.minPoint =
        static_cast<int>(index / 3) + model.params.rootFixed;
    result.minAxis = static_cast<int>(index % 3);
    result.minComponent = result.minEigenvector[index];
    const Eigen::Index first = std::max<Eigen::Index>(
        0, solver.eigenvalues().size() - 3);
    for (Eigen::Index i = first; i < solver.eigenvalues().size(); ++i)
        result.smallestEigenvalues.push_back(solver.eigenvalues()[i]);
    return result;
}

void PrintStability(const char* label, const StabilityResult& stability)
{
    if (!stability.finite)
    {
        std::printf("%s: non-finite Hessian\n", label);
        return;
    }
    std::printf(
        "%s: lambda_min=%.6e lambda_max=%.6e %s; smallest:",
        label, stability.minEigenvalue, stability.maxEigenvalue,
        stability.minEigenvalue > 0.0 ? "PD" : "INDEFINITE");
    for (double value : stability.smallestEigenvalues)
        std::printf(" %.6e", value);
    std::printf("\n");
    std::printf(
        "  min eigenvector max at point %d axis %d component %.6e\n",
        stability.minPoint, stability.minAxis,
        stability.minComponent);
}

void PrintEnergyDecomposition(const RodModel& model, const Rest& rest,
    const StabilityResult& stability)
{
    if (!stability.finite)
        return;
    const Eigen::VectorXd& direction = stability.minEigenvector;
    const Eigen::MatrixXd stretch = ComputePositionHessian(
        model, rest, 1.0, HessianComponent::Stretch);
    const Eigen::MatrixXd bend = ComputePositionHessian(
        model, rest, 1.0, HessianComponent::Bend);
    const Eigen::MatrixXd stretchMaterial =
        ComputeStretchMaterialHessian(model, rest);
    const Eigen::MatrixXd bendMaterial =
        ComputeBendMaterialHessian(model, rest);
    const double stretchEnergy = direction.dot(stretch * direction);
    const double bendEnergy = direction.dot(bend * direction);
    const double gravityEnergy = 0.0;
    const double stretchMaterialEnergy =
        direction.dot(stretchMaterial * direction);
    const double bendMaterialEnergy =
        direction.dot(bendMaterial * direction);
    const double stretchGeometricEnergy =
        stretchEnergy - stretchMaterialEnergy;
    const double bendGeometricEnergy =
        bendEnergy - bendMaterialEnergy;
    const double total =
        stretchEnergy + bendEnergy + gravityEnergy;
    const double magnitude = std::max(std::abs(total), 1.0e-30);
    std::printf(
        "negative-mode energy decomposition (v^T H v, |v|=1):\n");
    std::printf(
        "  total %.6e = stretch %.6e + bend %.6e + gravity %.6e\n",
        total, stretchEnergy, bendEnergy, gravityEnergy);
    std::printf(
        "  stretch: material %.6e + geometric %.6e\n",
        stretchMaterialEnergy, stretchGeometricEnergy);
    std::printf(
        "  bend:    material %.6e + geometric %.6e\n",
        bendMaterialEnergy, bendGeometricEnergy);
    std::printf(
        "  signed share of |total|: stretch %+6.1f%%, bend %+6.1f%%, "
        "gravity %+6.1f%%\n",
        100.0 * stretchEnergy / magnitude,
        100.0 * bendEnergy / magnitude,
        100.0 * gravityEnergy / magnitude);
    std::printf(
        "  check: v^T H v - lambda_min = %.3e\n",
        total - stability.minEigenvalue);
    std::printf(
        "  note: direct gravity Hessian is zero; gravity acts through "
        "the pre-strain/geometric terms.\n");
}

std::string ComponentLabel(const RodModel& model, Eigen::Index index)
{
    Eigen::Index cursor = 0;
    for (size_t edge : model.lengthEdges)
    {
        if (cursor == index)
            return "L[" + std::to_string(edge) + "]";
        ++cursor;
    }
    for (size_t joint : model.bendJoints)
    {
        if (cursor == index)
            return "b[" + std::to_string(joint) + "].along";
        ++cursor;
        if (cursor == index)
            return "b[" + std::to_string(joint) + "].binormal";
        ++cursor;
        if (model.useThreeDofBend)
        {
            if (cursor == index)
                return "b[" + std::to_string(joint) + "].third";
            ++cursor;
        }
    }
    return "?";
}

void PrintSparseVector(const char* label, const Eigen::VectorXd& vector,
    const RodModel& model)
{
    std::vector<std::pair<double, Eigen::Index>> entries;
    entries.reserve(static_cast<size_t>(vector.size()));
    for (Eigen::Index i = 0; i < vector.size(); ++i)
        entries.emplace_back(std::abs(vector[i]), i);
    std::sort(entries.begin(), entries.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.first > rhs.first;
        });
    std::printf("%s: |v|=%.6e; top:", label, vector.norm());
    const size_t count = std::min<size_t>(8, entries.size());
    for (size_t i = 0; i < count; ++i)
    {
        const Eigen::Index index = entries[i].second;
        std::printf(" %s=%.3e",
            ComponentLabel(model, index).c_str(), vector[index]);
    }
    std::printf("\n");
}

void PrintRankAnalysis(const char* label, const RodModel& model,
    const Rest& rest, double gravityScale)
{
    const Eigen::MatrixXd jacobian =
        ComputeJacobian(model, rest, gravityScale) /
        std::abs(model.params.gravity);
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(jacobian,
        Eigen::ComputeThinU | Eigen::ComputeFullV);
    const Eigen::VectorXd singular = svd.singularValues();
    const double sigmaMax = singular.maxCoeff();
    const double sigmaMin = singular.minCoeff();
    const Eigen::Index unknownCount = jacobian.cols();
    const Eigen::Index maxRank = singular.size();
    const double machineTolerance =
        static_cast<double>(
            std::max(jacobian.rows(), jacobian.cols())) *
        std::numeric_limits<double>::epsilon() * sigmaMax * 100.0;
    const double softTolerance = sigmaMax * 1.0e-6;
    int machineRank = 0;
    int softRank = 0;
    for (Eigen::Index i = 0; i < singular.size(); ++i)
    {
        if (singular[i] > machineTolerance)
            ++machineRank;
        if (singular[i] > softTolerance)
            ++softRank;
    }
    std::printf(
        "%s: rank %d/%lld (unknowns %lld, equations %lld) at "
        "machine tol %.3e; rank %d/%lld at 1e-6*sigma_max; "
        "nullity %lld; sigma [%.3e, %.3e] cond %.3e\n",
        label, machineRank, static_cast<long long>(maxRank),
        static_cast<long long>(unknownCount),
        static_cast<long long>(jacobian.rows()),
        machineTolerance, softRank,
        static_cast<long long>(maxRank),
        static_cast<long long>(unknownCount - machineRank),
        sigmaMin, sigmaMax,
        sigmaMin > 0.0 ? sigmaMax / sigmaMin
                       : std::numeric_limits<double>::infinity());
    std::printf("  smallest singular values:");
    const Eigen::Index first = std::max<Eigen::Index>(
        0, singular.size() - 5);
    for (Eigen::Index i = first; i < singular.size(); ++i)
        std::printf(" %.3e", singular[i]);
    std::printf("\n");
    if (machineRank < unknownCount)
    {
        for (Eigen::Index k = machineRank; k < unknownCount; ++k)
            PrintSparseVector("  null vector",
                svd.matrixV().col(k), model);
    }
}

struct SolveReport
{
    bool converged = false;
    bool finite = true;
    int iterations = 0;
    double residualInf = 0.0;
    double residualL2 = 0.0;
    Rest rest;
};

SolveReport SolveForceBalance(const RodModel& model, const Rest& initial,
    double gravityScale, int maxIterations, double targetInf)
{
    SolveReport report;
    report.rest = initial;
    Eigen::VectorXd force =
        ForceResidualExact(model, report.rest, gravityScale);
    if (!force.allFinite())
    {
        report.finite = false;
        return report;
    }
    const double normalization = std::abs(model.params.gravity);
    double damping = 1.0e-10;
    double trust = 0.5;
    for (int iteration = 0; iteration < maxIterations; ++iteration)
    {
        report.iterations = iteration;
        report.residualL2 = force.norm();
        report.residualInf =
            force.cwiseAbs().maxCoeff() / normalization;
        if (report.residualInf < targetInf)
        {
            report.converged = true;
            return report;
        }
        const Eigen::MatrixXd jacobian =
            ComputeJacobian(model, report.rest, gravityScale);
        const Eigen::VectorXd packed = Pack(model, report.rest);
        bool accepted = false;
        for (int attempt = 0; attempt < 10 && !accepted; ++attempt)
        {
            Eigen::MatrixXd normal =
                jacobian.transpose() * jacobian;
            normal.diagonal().array() += damping;
            Eigen::VectorXd step =
                normal.ldlt().solve(-jacobian.transpose() * force);
            if (!step.allFinite())
            {
                damping *= 10.0;
                continue;
            }
            const double stepNorm = step.norm();
            if (stepNorm > trust)
                step *= trust / stepNorm;
            double alpha = 1.0;
            for (int lineSearch = 0; lineSearch < 24; ++lineSearch)
            {
                const Eigen::VectorXd candidatePacked =
                    packed + alpha * step;
                Rest candidate;
                if (Unpack(model, candidatePacked, candidate))
                {
                    const Eigen::VectorXd candidateForce =
                        ForceResidualExact(
                            model, candidate, gravityScale);
                    if (candidateForce.allFinite() &&
                        candidateForce.norm() <
                            (1.0 - 1.0e-4 * alpha) * force.norm())
                    {
                        report.rest = candidate;
                        force = candidateForce;
                        accepted = true;
                        break;
                    }
                }
                alpha *= 0.5;
            }
            if (!accepted)
            {
                damping *= 10.0;
                trust *= 0.5;
            }
        }
        if (!accepted)
            break;
        damping = std::max(1.0e-12, damping / 3.0);
        trust = std::min(2.0, trust * 1.5);
    }
    report.residualL2 = force.norm();
    report.residualInf =
        force.cwiseAbs().maxCoeff() / normalization;
    report.converged = report.residualInf < targetInf;
    return report;
}

void PrintTwistColumn(const RodModel& model, const Rest& rest)
{
    if (rest.twistDelta.empty())
        return;
    const Eigen::VectorXd base =
        ForceResidualExact(model, rest, 1.0);
    Rest perturbed = rest;
    perturbed.twistDelta[0] += 1.0;
    const Eigen::VectorXd shifted =
        ForceResidualExact(model, perturbed, 1.0);
    const double change = (shifted - base).norm();
    std::printf(
        "rest twist column: ||dG/d twistDelta|| = %.3e "
        "(position-only model: zero column)\n", change);
}

struct MultiStartReport
{
    bool stableFound = false;
    bool physicalStableFound = false;
    int attempts = 0;
    int converged = 0;
    int uniqueRoots = 0;
    double bestLambdaMin =
        -std::numeric_limits<double>::infinity();
    double bestPhysicalLambdaMin =
        -std::numeric_limits<double>::infinity();
    double bestResidual =
        std::numeric_limits<double>::infinity();
    Rest bestRest;
    Rest bestPhysicalRest;
};

MultiStartReport MultiStartSearch(const RodModel& model, int attempts,
    double lengthPerturb, double bendPerturb, unsigned seed)
{
    MultiStartReport report;
    std::mt19937 generator(seed);
    std::uniform_real_distribution<double> distribution(-1.0, 1.0);
    std::vector<Eigen::VectorXd> roots;
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        Rest guess = model.reference;
        for (size_t edge : model.lengthEdges)
        {
            guess.lengths[edge] *= 1.0 +
                lengthPerturb * distribution(generator);
        }
        for (size_t joint : model.bendJoints)
        {
            const double along =
                bendPerturb * distribution(generator);
            const double binormal =
                bendPerturb * distribution(generator);
            const double third = model.useThreeDofBend
                ? bendPerturb * distribution(generator) : 0.0;
            guess.bend[joint] += along * model.basis[joint].along +
                binormal * model.basis[joint].binormal +
                third * model.basis[joint].third;
        }
        const SolveReport solved = SolveForceBalance(
            model, guess, 1.0, 100, 1.0e-8);
        ++report.attempts;
        if (!solved.converged)
        {
            std::printf(
                "  multistart %2d: not converged (residual %.3e)\n",
                attempt, solved.residualInf);
            continue;
        }
        ++report.converged;
        const StabilityResult stability =
            ComputeStability(model, solved.rest, 1.0);
        if (!stability.finite)
        {
            std::printf(
                "  multistart %2d: non-finite Hessian\n", attempt);
            continue;
        }
        const Eigen::VectorXd packed = Pack(model, solved.rest);
        bool isNew = true;
        for (const Eigen::VectorXd& root : roots)
        {
            if ((root - packed).norm() < 1.0e-2)
            {
                isNew = false;
                break;
            }
        }
        if (isNew)
        {
            roots.push_back(packed);
            ++report.uniqueRoots;
        }
        double maxBend = 0.0;
        for (size_t joint : model.bendJoints)
        {
            maxBend = std::max(
                maxBend, solved.rest.bend[joint].norm());
        }
        const bool physicalBend = maxBend <= 1.0 + 1.0e-6;
        std::printf(
            "  multistart %2d: residual %.3e lambda_min %.6e "
            "max|b| %.3e %s%s%s\n",
            attempt, solved.residualInf, stability.minEigenvalue,
            maxBend,
            stability.minEigenvalue > 0.0 ? "PD" : "indefinite",
            physicalBend ? " physical" : " unphysical",
            isNew ? " (new root)" : "");
        if (stability.minEigenvalue > report.bestLambdaMin)
        {
            report.bestLambdaMin = stability.minEigenvalue;
            report.bestResidual = solved.residualInf;
            report.bestRest = solved.rest;
        }
        if (stability.minEigenvalue > 0.0)
        {
            report.stableFound = true;
            if (physicalBend)
            {
                report.physicalStableFound = true;
                report.bestPhysicalLambdaMin =
                    stability.minEigenvalue;
                report.bestPhysicalRest = solved.rest;
                break;
            }
        }
    }
    return report;
}

struct CurvatureReport
{
    bool attempted = false;
    bool improved = false;
    double before = 0.0;
    double after = 0.0;
    double nullGradientNorm = 0.0;
    int steps = 0;
    Rest bestRest;
};

CurvatureReport CurvatureCorrection(const RodModel& model,
    const Rest& start, int maxSteps, double trust)
{
    CurvatureReport report;
    report.attempted = true;
    Rest rest = start;
    StabilityResult stability = ComputeStability(model, rest, 1.0);
    report.before = stability.minEigenvalue;
    report.after = report.before;
    report.bestRest = rest;
    if (!stability.finite)
        return report;

    for (int step = 0; step < maxSteps; ++step)
    {
        const Eigen::VectorXd packed = Pack(model, rest);
        const Eigen::VectorXd direction = stability.minEigenvector;
        Eigen::VectorXd gradient(model.UnknownCount());
        const double finiteDifference = 1.0e-4;
        for (Eigen::Index k = 0; k < gradient.size(); ++k)
        {
            Eigen::VectorXd plus = packed;
            Eigen::VectorXd minus = packed;
            plus[k] += finiteDifference;
            minus[k] -= finiteDifference;
            Rest plusRest;
            Rest minusRest;
            if (!Unpack(model, plus, plusRest) ||
                !Unpack(model, minus, minusRest))
            {
                gradient[k] = 0.0;
                continue;
            }
            const Eigen::MatrixXd hessianPlus = ComputePositionHessian(
                model, plusRest, 1.0, HessianComponent::Total);
            const Eigen::MatrixXd hessianMinus = ComputePositionHessian(
                model, minusRest, 1.0, HessianComponent::Total);
            if (!hessianPlus.allFinite() ||
                !hessianMinus.allFinite())
            {
                gradient[k] = 0.0;
                continue;
            }
            const double valuePlus =
                direction.dot(hessianPlus * direction);
            const double valueMinus =
                direction.dot(hessianMinus * direction);
            gradient[k] = (valuePlus - valueMinus) /
                (2.0 * finiteDifference);
        }
        const Eigen::MatrixXd jacobian =
            ComputeJacobian(model, rest, 1.0);
        const Eigen::VectorXd rowSpaceStep =
            MinimumNormSolve(jacobian, jacobian * gradient);
        const Eigen::VectorXd nullGradient =
            gradient - rowSpaceStep;
        report.nullGradientNorm = nullGradient.norm();
        if (report.nullGradientNorm < 1.0e-12)
        {
            std::printf(
                "  curvature correction %d: null-space gradient "
                "%.3e (QP gives delta_r=0)\n",
                step, report.nullGradientNorm);
            break;
        }
        const Eigen::VectorXd delta =
            trust * nullGradient / report.nullGradientNorm;
        Eigen::VectorXd candidatePacked = packed + delta;
        Rest candidate;
        if (!Unpack(model, candidatePacked, candidate))
            break;
        const SolveReport solved = SolveForceBalance(
            model, candidate, 1.0, 80, 1.0e-10);
        if (!solved.converged)
            break;
        const StabilityResult candidateStability =
            ComputeStability(model, solved.rest, 1.0);
        if (!candidateStability.finite)
            break;
        std::printf(
            "  curvature correction %d: lambda_min %.6e -> %.6e "
            "(null-space gradient %.3e)\n",
            step, stability.minEigenvalue,
            candidateStability.minEigenvalue,
            report.nullGradientNorm);
        if (candidateStability.minEigenvalue <=
            stability.minEigenvalue)
        {
            break;
        }
        rest = solved.rest;
        stability = candidateStability;
        report.after = stability.minEigenvalue;
        report.bestRest = rest;
        report.improved = true;
        ++report.steps;
        if (stability.minEigenvalue > 0.0)
            break;
    }
    report.after = stability.minEigenvalue;
    return report;
}

struct BranchReport
{
    bool reachedFullGravity = false;
    bool stableAtFullGravity = false;
    bool finite = true;
    double firstUnstableLambda = -1.0;
    double lambdaMinAtFull = 0.0;
    double lambdaMaxAtFull = 0.0;
    Rest rest;
};

BranchReport TraceStableBranch(const RodModel& model)
{
    BranchReport report;
    Rest rest = model.reference;
    double lambda = 0.0;
    StabilityResult stability = ComputeStability(model, rest, 0.0);
    std::printf("continuation:\n");
    std::printf("  lambda 0.000000: force residual 0.000e+00; ");
    PrintStability("H", stability);
    if (!(stability.minEigenvalue > 0.0))
    {
        std::printf(
            "  warning: lambda_min(H) is not positive at lambda=0\n");
    }

    const Eigen::VectorXd gravity = GravityVector(model, 1.0);
    Eigen::MatrixXd jacobian = ComputeJacobian(model, rest, 0.0);
    Eigen::VectorXd tangent = MinimumNormSolve(jacobian, -gravity);
    if (!tangent.allFinite())
    {
        report.finite = false;
        report.rest = rest;
        return report;
    }

    double step = 0.05;
    int acceptedSteps = 0;
    while (lambda < 1.0 - 1.0e-12 && acceptedSteps < 200)
    {
        const double next = std::min(1.0, lambda + step);
        const Eigen::VectorXd predicted =
            Pack(model, rest) + (next - lambda) * tangent;
        Rest guess;
        if (!Unpack(model, predicted, guess))
        {
            step *= 0.5;
            if (step < 1.0e-5)
                break;
            continue;
        }
        const SolveReport solved =
            SolveForceBalance(model, guess, next, 60, 1.0e-8);
        if (!solved.converged)
        {
            step *= 0.5;
            if (step < 1.0e-5)
                break;
            continue;
        }
        stability = ComputeStability(model, solved.rest, next);
        if (!stability.finite)
        {
            report.finite = false;
            break;
        }
        if (!(stability.minEigenvalue > 0.0) &&
            report.firstUnstableLambda < 0.0)
        {
            report.firstUnstableLambda = next;
        }
        std::printf(
            "  lambda %.6f: force residual %.3e; iter %d; ",
            next, solved.residualInf, solved.iterations);
        PrintStability("H", stability);
        rest = solved.rest;
        lambda = next;
        if (next >= 1.0 - 1.0e-12)
        {
            report.lambdaMinAtFull = stability.minEigenvalue;
            report.lambdaMaxAtFull = stability.maxEigenvalue;
        }
        jacobian = ComputeJacobian(model, rest, lambda);
        tangent = MinimumNormSolve(jacobian, -gravity);
        if (!tangent.allFinite())
        {
            report.finite = false;
            break;
        }
        if (solved.iterations <= 6)
            step = std::min(0.25, step * 1.5);
        else
            step = std::max(0.01, step * 0.7);
        ++acceptedSteps;
    }
    report.rest = rest;
    report.reachedFullGravity = lambda >= 1.0 - 1.0e-12;
    if (report.reachedFullGravity)
        report.stableAtFullGravity = report.lambdaMinAtFull > 0.0;
    if (report.firstUnstableLambda > 0.0)
    {
        std::printf(
            "  first stability loss at lambda=%.6f\n",
            report.firstUnstableLambda);
    }
    else
    {
        std::printf("  first stability loss: none\n");
    }
    return report;
}

struct StiffnessReport
{
    bool reachedTarget = false;
    bool stableAtTarget = false;
    bool finite = true;
    double firstUnstableEI = -1.0;
    Rest rest;
};

// Alternate path to the target material: hold gravity at 1 and ramp EI down
// from a stiff regime where the authored shape is obviously stable.  This is a
// search for a stable root, not a claim that the stiff root is physical.
StiffnessReport TraceStiffnessPath(const RodModel& model, double startEI,
    double targetEI)
{
    StiffnessReport report;
    RodModel path = model;
    path.params.EI = startEI;
    Rest rest = path.reference;
    SolveReport solved =
        SolveForceBalance(path, rest, 1.0, 100, 1.0e-10);
    if (!solved.converged)
    {
        report.finite = false;
        report.rest = rest;
        return report;
    }
    rest = solved.rest;
    StabilityResult stability = ComputeStability(path, rest, 1.0);
    if (!stability.finite)
    {
        report.finite = false;
        report.rest = rest;
        return report;
    }
    std::printf(
        "stiffness continuation (lambda=1):\n"
        "  EI %.6e: force residual %.3e; ",
        startEI, solved.residualInf);
    PrintStability("H", stability);
    if (!(stability.minEigenvalue > 0.0))
        report.firstUnstableEI = startEI;

    double currentEI = startEI;
    while (currentEI > targetEI * (1.0 + 1.0e-12))
    {
        double nextEI = std::max(targetEI, currentEI * 0.5);
        path.params.EI = nextEI;
        solved = SolveForceBalance(path, rest, 1.0, 100, 1.0e-10);
        if (!solved.converged)
        {
            nextEI = std::max(targetEI, currentEI * 0.8);
            path.params.EI = nextEI;
            solved = SolveForceBalance(path, rest, 1.0, 100, 1.0e-10);
        }
        if (!solved.converged)
        {
            std::printf(
                "  stiffness path stopped at EI %.6e "
                "(corrector did not converge)\n", currentEI);
            break;
        }
        rest = solved.rest;
        currentEI = nextEI;
        stability = ComputeStability(path, rest, 1.0);
        if (!stability.finite)
        {
            report.finite = false;
            break;
        }
        std::printf(
            "  EI %.6e: force residual %.3e; ",
            currentEI, solved.residualInf);
        PrintStability("H", stability);
        if (!(stability.minEigenvalue > 0.0) &&
            report.firstUnstableEI < 0.0)
        {
            report.firstUnstableEI = currentEI;
        }
        if (currentEI <= targetEI * (1.0 + 1.0e-12))
        {
            report.reachedTarget = true;
            report.stableAtTarget = stability.minEigenvalue > 0.0;
        }
    }
    report.rest = rest;
    if (report.firstUnstableEI > 0.0)
    {
        std::printf(
            "  first stiffness stability loss at EI=%.6e\n",
            report.firstUnstableEI);
    }
    else
    {
        std::printf("  first stiffness stability loss: none\n");
    }
    return report;
}

struct SimMetrics
{
    bool finite = true;
    double finalMaxDeviation = 0.0;
    double finalTipDeviation = 0.0;
    double trajectoryMaxDeviation = 0.0;
    double trajectoryMaxTip = 0.0;
    double maxSegmentStretch = 0.0;
    std::vector<double> finalDeviations;
};

SimMetrics Simulate(const RodModel& model, const Rest& rest, int steps)
{
    SimMetrics metrics;
    State state = ResetState(model.authored, rest);
    const double totalLength = TotalLength(model.authored);
    for (int step = 0; step < steps; ++step)
    {
        Step(state, rest, model.params);
        for (const Vec& point : state.x)
        {
            if (!point.allFinite())
                metrics.finite = false;
        }
        for (double phi : state.phi)
        {
            if (!std::isfinite(phi))
                metrics.finite = false;
        }
        if (!metrics.finite)
        {
            metrics.finalMaxDeviation =
                std::numeric_limits<double>::infinity();
            metrics.finalTipDeviation =
                std::numeric_limits<double>::infinity();
            metrics.trajectoryMaxDeviation =
                std::numeric_limits<double>::infinity();
            metrics.trajectoryMaxTip =
                std::numeric_limits<double>::infinity();
            metrics.maxSegmentStretch =
                std::numeric_limits<double>::infinity();
            return metrics;
        }
        double maxDeviation = 0.0;
        for (size_t i = 0; i < model.authored.size(); ++i)
        {
            maxDeviation = std::max(maxDeviation,
                (state.x[i] - model.authored[i]).norm());
        }
        const double tipDeviation =
            (state.x.back() - model.authored.back()).norm();
        metrics.trajectoryMaxDeviation = std::max(
            metrics.trajectoryMaxDeviation,
            maxDeviation / totalLength);
        metrics.trajectoryMaxTip = std::max(
            metrics.trajectoryMaxTip,
            tipDeviation / totalLength);
        for (size_t edge = 0;
            edge + 1 < model.authored.size(); ++edge)
        {
            const double length =
                (state.x[edge + 1] - state.x[edge]).norm();
            const double stretch = std::abs(
                length / rest.lengths[edge] - 1.0);
            metrics.maxSegmentStretch =
                std::max(metrics.maxSegmentStretch, stretch);
        }
        if (step == steps - 1)
        {
            metrics.finalMaxDeviation =
                maxDeviation / totalLength;
            metrics.finalTipDeviation =
                tipDeviation / totalLength;
            metrics.finalDeviations.resize(model.authored.size());
            for (size_t i = 0; i < model.authored.size(); ++i)
            {
                metrics.finalDeviations[i] =
                    (state.x[i] - model.authored[i]).norm();
            }
        }
    }
    return metrics;
}

void PrintSimMetrics(const char* label, const SimMetrics& metrics)
{
    std::printf(
        "%s: final max deviation %.6e L, final tip %.6e L, "
        "trajectory max %.6e L, max segment stretch %.6e, %s\n",
        label, metrics.finalMaxDeviation,
        metrics.finalTipDeviation,
        metrics.trajectoryMaxDeviation,
        metrics.maxSegmentStretch,
        metrics.finite ? "finite" : "NON-FINITE");
    if (!metrics.finalDeviations.empty())
    {
        std::vector<std::pair<double, size_t>> deviations;
        deviations.reserve(metrics.finalDeviations.size());
        for (size_t i = 0; i < metrics.finalDeviations.size(); ++i)
            deviations.emplace_back(metrics.finalDeviations[i], i);
        std::sort(deviations.begin(), deviations.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.first > rhs.first;
            });
        std::printf("  largest final point deviations:");
        const size_t count =
            std::min<size_t>(5, deviations.size());
        for (size_t i = 0; i < count; ++i)
        {
            std::printf(" p%zu=%.4f",
                deviations[i].second, deviations[i].first);
        }
        std::printf("\n");
    }
}

void PrintRestSummary(const char* label, const RodModel& model,
    const Rest& rest)
{
    double maxLengthChange = 0.0;
    for (size_t edge : model.lengthEdges)
    {
        maxLengthChange = std::max(maxLengthChange,
            std::abs(rest.lengths[edge] /
                model.reference.lengths[edge] - 1.0));
    }
    double maxBendChange = 0.0;
    double maxBendMagnitude = 0.0;
    for (size_t joint : model.bendJoints)
    {
        maxBendMagnitude = std::max(
            maxBendMagnitude, rest.bend[joint].norm());
        const Vec delta =
            rest.bend[joint] - model.reference.bend[joint];
        const double along =
            delta.dot(model.basis[joint].along);
        const double binormal =
            delta.dot(model.basis[joint].binormal);
        maxBendChange = std::max(maxBendChange,
            std::sqrt(along * along + binormal * binormal));
    }
    std::printf(
        "%s: max |dL|/L=%.6e max |db|=%.6e max |b|=%.6e\n",
        label, maxLengthChange, maxBendChange, maxBendMagnitude);
}

double MaxRestBendMagnitude(const RodModel& model, const Rest& rest)
{
    double magnitude = 0.0;
    for (size_t joint : model.bendJoints)
        magnitude = std::max(magnitude, rest.bend[joint].norm());
    return magnitude;
}

void PrintTorqueResidual(const char* label, const RodModel& model,
    const Rest& rest)
{
    int worstJoint = -1;
    const double torque = MaxBendTorqueResidual(
        model, model.authored, rest, &worstJoint);
    const double normalized = torque /
        (std::abs(model.params.gravity) *
            TotalLength(model.authored));
    std::printf(
        "%s: max per-joint bend torque %.6e "
        "(normalized %.3e, joint %d)\n",
        label, torque, normalized, worstJoint);
}

void PrintRestQuaternion(const RodModel& model, const Rest& rest)
{
    constexpr double pi = 3.141592653589793;
    double maxNormalization = 0.0;
    double maxAngle = 0.0;
    double minFlipMargin = pi;
    for (size_t joint : model.bendJoints)
    {
        const double half = 0.5 * rest.bend[joint].norm();
        const double norm = std::sqrt(1.0 + half * half);
        maxNormalization = std::max(
            maxNormalization, std::abs(norm - 1.0));
        const double angle = 2.0 * std::atan2(half, 1.0);
        maxAngle = std::max(maxAngle, angle);
        minFlipMargin = std::min(minFlipMargin, pi - angle);
    }
    std::printf(
          "rest quaternion proxy (small-angle q=(1,b/2), normalized; "
          "NOT a true material quaternion): max |q|-1 before "
          "normalize %.6e, max angle %.6e rad, min flip margin "
          "%.6e rad\n",
        maxNormalization, maxAngle, minFlipMargin);
}

void PrintVectorConstraintCompliance(const RodModel& model)
{
    const size_t edge = model.lengthEdges.empty()
        ? 0 : model.lengthEdges.front();
    const double length = model.reference.lengths[edge];
    const double scalarAlpha = length / model.params.EA;
    const double vectorAxial = 1.0 / (model.params.EA * length);
    const double vectorShear = 1.0 / (model.params.GA * length);
    std::printf(
        "vector C_ss compliance check (L[%zu]=%.6e): "
        "axial 1/(EA L)=%.6e vs scalar L/EA=%.6e (ratio %.3e); "
        "shear 1/(GA L)=%.6e\n",
        edge, length, vectorAxial, scalarAlpha,
        vectorAxial / scalarAlpha, vectorShear);
    std::printf(
        "  active: %s\n",
        model.useVectorStretchShear
            ? "vector C_ss uses diag(1/(GA L),1/(GA L),1/(EA L))"
            : "scalar |e|-L uses L/EA; vector C_ss would need "
              "diag(1/(GA L),1/(GA L),1/(EA L))");
}

void PrintSweepComparison(const RodModel& model, const Rest& rest,
    int settleSteps)
{
    std::printf(
        "sweep-order comparison (same rest, %d steps):\n",
        settleSteps);
    const SweepOrder orders[] = {
        SweepOrder::Forward,
        SweepOrder::Reverse,
        SweepOrder::Interleaved
    };
    for (SweepOrder order : orders)
    {
        RodModel local = model;
        local.params.sweepOrder = order;
        const SimMetrics metrics =
            Simulate(local, rest, settleSteps);
        int worstJoint = -1;
        const double torque = MaxBendTorqueResidual(
            local, local.authored, rest, &worstJoint);
        const double normalizedTorque = torque /
            (std::abs(local.params.gravity) *
                TotalLength(local.authored));
        std::printf(
            "  %-11s final max %.6e L, tip %.6e L, "
            "stretch %.6e, torque %.6e (norm %.3e, joint %d), %s\n",
            SweepName(order), metrics.finalMaxDeviation,
            metrics.finalTipDeviation, metrics.maxSegmentStretch,
            torque, normalizedTorque, worstJoint,
            metrics.finite ? "finite" : "NON-FINITE");
    }
}

int RunM0_006(const std::string& sceneName, int points, int settleSteps,
    int bendDof, int multistartCount, bool vectorConstraint)
{
    Params params;
    params.rootFixed = 2;
    std::vector<Vec> authored;
    if (sceneName == "straight")
    {
        authored = StraightChain(points, 20.0);
    }
    else if (sceneName == "helix")
    {
        authored = Helix(points, 3.0, 20.0, 1);
    }
    else if (sceneName == "initial-bend")
    {
        authored = InitialBendChain(
            points, 20.0, 0.5 * 3.141592653589793);
    }
    else
    {
        std::fprintf(stderr, "unknown scene: %s\n",
            sceneName.c_str());
        return 2;
    }
    const bool threeDof = bendDof == 3;
    const RodModel model =
        BuildRodModel(authored, params, threeDof, vectorConstraint);
    const double totalLength = TotalLength(authored);
    std::printf(
        "=== M0-006 scene=%s points=%d rootFixed=%d bendDof=%d "
        "settle=%d multistart=%d constraint=%s ===\n",
        sceneName.c_str(), points, params.rootFixed, bendDof,
        settleSteps, multistartCount,
        vectorConstraint ? "vector-C_ss" : "scalar-length");
    std::printf(
        "parameters: EA=%.6e EI=%.6e GJ=%.6e g=%.6f dt=%.6e "
        "substeps=%d iterations=%d damping=%.6e\n",
        params.EA, params.EI, params.GJ, params.gravity, params.dt,
        params.substeps, params.iterations, params.damping);
    if (sceneName == "straight")
    {
        std::printf(
            "scene geometry: straight vertical chain, length %.3f\n",
            totalLength);
    }
    else if (sceneName == "helix")
    {
        std::printf(
            "scene geometry: helix radius %.3f height %.3f turns %d\n",
            3.0, 20.0, 1);
    }
    else
    {
        std::printf(
            "scene geometry: planar circular arc, length %.3f, "
            "total angle %.3f rad\n",
            totalLength, 0.5 * 3.141592653589793);
    }
    std::printf(
        "model: total length %.6f, mean segment %.6f, "
        "movable points %zu\n",
        totalLength, model.lengthScale,
        authored.size() - static_cast<size_t>(params.rootFixed));
    std::printf("gauge: excluded fixed-fixed edges:");
    for (size_t edge = 0; edge + 1 < authored.size(); ++edge)
    {
        if (edge + 1 < static_cast<size_t>(params.rootFixed))
        {
            std::printf(" L[%zu]=%.6f",
                edge, model.reference.lengths[edge]);
        }
    }
    std::printf("\n");
    std::printf(
        "parameterization: %zu active rest lengths + %zu joints * "
        "%d bend DOF = %lld unknowns; equations %lld\n",
        model.lengthEdges.size(), model.bendJoints.size(), bendDof,
        static_cast<long long>(model.UnknownCount()),
        static_cast<long long>(model.ForceCount()));
    std::printf(
        "twist: scalar phi/rest twist is excluded from the position "
        "Hessian; no angular stability is claimed\n");
    std::printf(
        "discrete definition: node-based small-angle prototype; "
        "Kugelstadt/Hsu use segment-center q_s and Lbar_s; "
        "no equivalence is claimed\n");
    std::printf(
        "dynamics stretch: %s\n",
        vectorConstraint
            ? "vector C_ss with diag(1/(GA L),1/(GA L),1/(EA L))"
            : "scalar |e|-L with alpha_s=L/EA");
    PrintTwistColumn(model, model.reference);
    PrintVectorConstraintCompliance(model);
    PrintRestSummary("reference rest", model, model.reference);
    PrintRankAnalysis(
        "rank(reference)", model, model.reference, 0.0);

    const BranchReport branch = TraceStableBranch(model);

    const SolveReport direct = SolveForceBalance(
        model, model.reference, 1.0, 80, 1.0e-10);
    StabilityResult directStability;
    if (direct.converged)
    {
        directStability =
            ComputeStability(model, direct.rest, 1.0);
        std::printf(
            "direct root from authored rest at lambda=1: "
            "residual %.3e, iterations %d\n",
            direct.residualInf, direct.iterations);
        PrintStability("direct root H", directStability);
        PrintRankAnalysis(
            "rank(direct root)", model, direct.rest, 1.0);
        PrintRestSummary("direct root rest", model, direct.rest);
    }
    else
    {
        std::printf(
            "direct root from authored rest at lambda=1: "
            "not converged (residual %.3e, iterations %d)\n",
            direct.residualInf, direct.iterations);
    }

    Rest candidate = model.reference;
    bool haveCandidate = false;
    std::string candidateSource = "none";
    if (branch.reachedFullGravity && branch.stableAtFullGravity)
    {
        candidate = branch.rest;
        haveCandidate = true;
        candidateSource = "gravity continuation";
    }
    else if (direct.converged && directStability.finite &&
        directStability.minEigenvalue > 0.0)
    {
        candidate = direct.rest;
        haveCandidate = true;
        candidateSource = "direct Newton";
    }
    else
    {
        const StiffnessReport stiffness =
            TraceStiffnessPath(model, 1.0e5, model.params.EI);
        if (stiffness.reachedTarget && stiffness.stableAtTarget)
        {
            candidate = stiffness.rest;
            haveCandidate = true;
            candidateSource = "stiffness continuation";
        }
        else if (branch.reachedFullGravity)
        {
            candidate = branch.rest;
            haveCandidate = true;
            candidateSource = "gravity continuation (unstable)";
        }
        else if (direct.converged)
        {
            candidate = direct.rest;
            haveCandidate = true;
            candidateSource = "direct Newton (unstable)";
        }
    }
    if (!haveCandidate)
    {
        std::printf("no force-balance candidate found\n");
        std::printf("verdict: FAIL (no candidate)\n");
        return 1;
    }

    const SolveReport deterministicPolish = SolveForceBalance(
        model, candidate, 1.0, 80, 1.0e-10);
    if (deterministicPolish.converged)
    {
        candidate = deterministicPolish.rest;
    }
    else
    {
        std::printf(
            "warning: deterministic polish did not converge "
            "(residual %.3e)\n", deterministicPolish.residualInf);
    }

    StabilityResult deterministicStability =
        ComputeStability(model, candidate, 1.0);
    std::printf("deterministic candidate source: %s\n",
        candidateSource.c_str());
    PrintRestSummary("deterministic candidate rest", model, candidate);
    PrintStability(
        "deterministic candidate H", deterministicStability);
    if (deterministicStability.finite &&
        deterministicStability.minEigenvalue <= 0.0)
    {
        PrintEnergyDecomposition(
            model, candidate, deterministicStability);
    }

    std::printf("curvature correction:\n");
    const int curvatureSteps = points <= 8 ? 4 : 2;
    const CurvatureReport curvature = CurvatureCorrection(
        model, candidate, curvatureSteps, 0.05);
    StabilityResult currentStability = deterministicStability;
    if (curvature.improved)
    {
        candidate = curvature.bestRest;
        candidateSource += " + curvature correction";
        currentStability = ComputeStability(model, candidate, 1.0);
        std::printf("  accepted: lambda_min %.6e -> %.6e\n",
            curvature.before, curvature.after);
    }
    else
    {
        std::printf(
            "  no improvement: lambda_min %.6e -> %.6e, "
            "null-space gradient %.3e\n",
            curvature.before, curvature.after,
            curvature.nullGradientNorm);
    }

    if (multistartCount > 0)
    {
        std::printf(
            "multi-start search (%d attempts):\n", multistartCount);
        const unsigned seed =
            20261114u + static_cast<unsigned>(points) +
            (threeDof ? 1u : 0u) +
            static_cast<unsigned>(bendDof);
        const MultiStartReport multistart = MultiStartSearch(
            model, multistartCount, 0.03, 0.3, seed);
        std::printf(
            "  summary: attempts %d, converged %d, unique roots %d, "
            "best lambda_min %.6e, physical stable %s\n",
            multistart.attempts, multistart.converged,
            multistart.uniqueRoots, multistart.bestLambdaMin,
            multistart.physicalStableFound ? "yes" : "no");
        if (multistart.physicalStableFound)
        {
            candidate = multistart.bestPhysicalRest;
            candidateSource += " + physical multistart";
            currentStability =
                ComputeStability(model, candidate, 1.0);
            std::printf(
                "  accepted physical stable root: lambda_min %.6e\n",
                currentStability.minEigenvalue);
        }
        else if (multistart.converged > 0 &&
            (multistart.stableFound ||
                multistart.bestLambdaMin >
                    currentStability.minEigenvalue))
        {
            candidate = multistart.bestRest;
            candidateSource += " + multistart";
            currentStability =
                ComputeStability(model, candidate, 1.0);
            std::printf("  accepted: lambda_min %.6e\n",
                currentStability.minEigenvalue);
        }
        else
        {
            std::printf("  no better root accepted\n");
        }
    }

    const SolveReport finalPolish = SolveForceBalance(
        model, candidate, 1.0, 80, 1.0e-10);
    if (finalPolish.converged)
    {
        candidate = finalPolish.rest;
    }
    else
    {
        std::printf(
            "warning: final polish did not converge "
            "(residual %.3e)\n", finalPolish.residualInf);
    }
    std::printf("final candidate source: %s\n",
        candidateSource.c_str());
    PrintRestSummary("final candidate rest", model, candidate);
    PrintTorqueResidual("final candidate torque", model, candidate);
    PrintRestQuaternion(model, candidate);
    PrintRankAnalysis(
        "rank(final candidate)", model, candidate, 1.0);

    const Eigen::VectorXd force =
        ForceResidualExact(model, candidate, 1.0);
    const bool forceFinite = force.allFinite();
    const double residualInf = forceFinite
        ? force.cwiseAbs().maxCoeff() / std::abs(params.gravity)
        : std::numeric_limits<double>::infinity();
    const double residualL2 = forceFinite
        ? force.norm()
        : std::numeric_limits<double>::infinity();
    const StabilityResult stability =
        ComputeStability(model, candidate, 1.0);
    std::printf(
        "final candidate force residual: ||G||inf=%.6e, "
        "||G||inf/(m|g|)=%.6e, ||G||2=%.6e\n",
        forceFinite ? force.cwiseAbs().maxCoeff() : -1.0,
        residualInf, residualL2);
    PrintStability("final candidate H", stability);
    if (stability.finite && stability.minEigenvalue <= 0.0)
    {
        PrintEnergyDecomposition(model, candidate, stability);
    }

    const double maxBendMagnitude =
        MaxRestBendMagnitude(model, candidate);
    const bool bendPhysical = maxBendMagnitude <= 1.0 + 1.0e-6;
    std::printf(
        "physical rest bend: max |b|=%.6e, |b|<=1 %s\n",
        maxBendMagnitude, bendPhysical ? "PASS" : "FAIL");

    const SimMetrics referenceMetrics =
        Simulate(model, model.reference, settleSteps);
    const SimMetrics candidateMetrics =
        Simulate(model, candidate, settleSteps);
    PrintSimMetrics("reference 200-step", referenceMetrics);
    PrintSimMetrics("candidate 200-step", candidateMetrics);
    PrintSweepComparison(model, candidate, settleSteps);

    const bool residualPass = residualInf < 1.0e-5;
    const bool stabilityPass =
        stability.finite && stability.minEigenvalue > 0.0;
    const bool deviationPass =
        candidateMetrics.finalMaxDeviation < 0.05;
    const bool tipPass =
        candidateMetrics.finalTipDeviation < 0.01;
    const bool stretchPass =
        candidateMetrics.maxSegmentStretch < 0.05;
    const bool finitePass =
        candidateMetrics.finite && stability.finite && forceFinite;
    const bool numericalPass = residualPass && stabilityPass &&
        deviationPass && tipPass && stretchPass && finitePass;
    const bool pass = numericalPass && bendPhysical;
    std::printf(
        "thresholds: residual %s, Hessian %s, 200-step max %s, "
        "tip %s, stretch %s, finite %s\n",
        residualPass ? "PASS" : "FAIL",
        stabilityPass ? "PASS" : "FAIL",
        deviationPass ? "PASS" : "FAIL",
        tipPass ? "PASS" : "FAIL",
        stretchPass ? "PASS" : "FAIL",
        finitePass ? "PASS" : "FAIL");
    std::printf("physical bend gate: %s\n",
        bendPhysical ? "PASS" : "FAIL");
    if (numericalPass && !bendPhysical)
    {
        std::printf(
            "verdict: FAIL (numerical thresholds pass, but rest bend "
            "%.3e exceeds |b|<=1; model not physically feasible)\n",
            maxBendMagnitude);
        return 1;
    }
    std::printf("verdict: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int Run(const std::string& mode, double omega, int iterations, int settleSteps)
{
    const Params params;
    const std::vector<Vec> authored = Helix(32, 3.0, 20.0, 1);
    const Rest rest = BuildRest(authored, 0.02);

    State baseline;
    const double baselineResidual = SettleResidual(
        authored, rest, params, settleSteps, &baseline);
    PrintMeasure("baseline", authored, baseline);
    std::printf("baseline residual %.6f\n", baselineResidual);

    Rest solved = rest;
    std::vector<Vec> restPositions = authored;
    for (int k = 0; k < iterations; ++k)
    {
        const State probe = [&]() {
            State state = ResetState(authored, solved);
            for (int i = 0; i < settleSteps; ++i)
                Step(state, solved, params);
            return state;
        }();
        double maxError = 0.0;
        for (size_t i = 0; i < authored.size(); ++i)
            maxError = std::max(maxError, (authored[i] - probe.x[i]).norm());
        std::printf("inverse %2d residual %.6f\n", k, maxError);

        if (mode == "position")
        {
            for (size_t i = 0; i < authored.size(); ++i)
                if (i >= static_cast<size_t>(params.rootFixed))
                    restPositions[i] += omega * (authored[i] - probe.x[i]);
            solved = BuildRest(restPositions, 0.02);
        }
        else if (mode == "bend")
        {
            for (size_t i = 0; i + 2 < authored.size(); ++i)
            {
                const Vec t0 = (probe.x[i + 1] - probe.x[i]).normalized();
                const Vec t1 = (probe.x[i + 2] - probe.x[i + 1]).normalized();
                solved.bend[i] += omega * (rest.bend[i] - t0.cross(t1));
            }
        }
        else
        {
            std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
            return 2;
        }
    }

    State finalState = ResetState(authored, solved);
    for (int i = 0; i < settleSteps; ++i)
        Step(finalState, solved, params);
    PrintMeasure("sag-free", authored, finalState);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    std::string mode = "position";
    std::string scene = "straight";
    double omega = 0.5;
    int iterations = 20;
    int settleSteps = 120;
    int points = 8;
    int newtonIterations = 10;
    double lambda = 1.0e-3;
    int bendDof = 2;
    int multistartCount = 0;
    bool vectorConstraint = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--mode" && i + 1 < argc)
            mode = argv[++i];
        else if (argument == "--scene" && i + 1 < argc)
            scene = argv[++i];
        else if (argument == "--bend-dof" && i + 1 < argc)
            bendDof = std::atoi(argv[++i]);
        else if (argument == "--multistart" && i + 1 < argc)
            multistartCount = std::atoi(argv[++i]);
        else if (argument == "--constraint" && i + 1 < argc)
            vectorConstraint = std::string(argv[++i]) == "vector";
        else if (argument == "--omega" && i + 1 < argc)
            omega = std::atof(argv[++i]);
        else if (argument == "--iterations" && i + 1 < argc)
            iterations = std::atoi(argv[++i]);
        else if (argument == "--settle" && i + 1 < argc)
            settleSteps = std::atoi(argv[++i]);
        else if (argument == "--points" && i + 1 < argc)
            points = std::atoi(argv[++i]);
        else if (argument == "--newton" && i + 1 < argc)
            newtonIterations = std::atoi(argv[++i]);
        else if (argument == "--lambda" && i + 1 < argc)
            lambda = std::atof(argv[++i]);
    }
    if (mode == "newton")
        return RunNewton(points, newtonIterations, settleSteps, lambda);
    if (mode == "energy")
        return RunEnergyNewton(points, newtonIterations, settleSteps, lambda);
    if (mode == "svd")
        return RunSvdNewton(points, newtonIterations, settleSteps);
    if (mode == "m0-006")
        return RunM0_006(
            scene, points, settleSteps, bendDof, multistartCount,
            vectorConstraint);
    std::printf("mode=%s omega=%.4f iterations=%d settle=%d\n",
        mode.c_str(), omega, iterations, settleSteps);
    return Run(mode, omega, iterations, settleSteps);
}
