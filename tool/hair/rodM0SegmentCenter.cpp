#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <tuple>
#include <vector>

// M0-007 CPU prototype: segment-center material quaternions + rest twist.
//
// This file is intentionally separate from rodM0Prototype.cpp.  M0-006 is a
// node-based small-angle model; M0-007 is the paper-shaped model:
//   * node positions x_i carry stretch/shear and gravity;
//   * each segment s has a unit material quaternion q_s at its center
//     c_s = (x_s + x_{s+1}) / 2;
//   * C_ss = R_s^T (x_{s+1}-x_s)/L_s - e3 with compliance
//     diag(1/(GA_1 L), 1/(GA_2 L), 1/(EA L));
//   * relative rotation r_s = q_s^{-1} q_{s+1}, sign-fixed to r_s.w >= 0,
//     theta_s = 2 atan2(|r_s.v|, r_s.w), rotation vector = theta_s * axis;
//   * curvature/twist omega_s = rotation_vector(r_s) / Lbar_s with
//     Lbar_s = (L_s + L_{s+1})/2 for the joint between segments s and s+1;
//   * the same log map is used to recover angular velocity from quaternions.
//
// The rest parameters are rest segment lengths and unit rest relative
// quaternions.  Rest relative rotations are parameterized by rotation vectors
// with |v| <= pi - flipMargin; this makes every rest quaternion unit and gives
// an explicit flip-margin check.  Rest twist is the third component of v in the
// segment body frame.

namespace
{
using Vec = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;

constexpr double Pi = 3.14159265358979323846;
constexpr double TargetDeviationBound = 1.0;
constexpr double TorqueEquationWeight = 10.0;

struct Quat
{
    double w = 1.0;
    Vec v = Vec::Zero();
};

Quat operator*(const Quat& a, const Quat& b)
{
    return {
        a.w * b.w - a.v.dot(b.v),
        a.w * b.v + b.w * a.v + a.v.cross(b.v)};
}

Quat Conjugate(const Quat& q)
{
    return {q.w, -q.v};
}

Quat Normalize(const Quat& q)
{
    const double norm = std::sqrt(q.w * q.w + q.v.squaredNorm());
    if (!(norm > 1.0e-12))
        return {};
    return {q.w / norm, q.v / norm};
}

Quat Exp(const Vec& rotationVector)
{
    const double angle = rotationVector.norm();
    if (!(angle > 1.0e-12))
        return Normalize({1.0, 0.5 * rotationVector});
    const Vec axis = rotationVector / angle;
    return {std::cos(0.5 * angle), std::sin(0.5 * angle) * axis};
}

Vec Log(const Quat& input)
{
    Quat q = Normalize(input);
    if (q.w < 0.0)
        q = {-q.w, -q.v};
    const double vectorNorm = q.v.norm();
    if (!(vectorNorm > 1.0e-12))
        return 2.0 * q.v;
    const double angle = 2.0 * std::atan2(vectorNorm, q.w);
    return angle * q.v / vectorNorm;
}

Mat3 ToMatrix(const Quat& input)
{
    const Quat q = Normalize(input);
    const double w = q.w;
    const double x = q.v.x();
    const double y = q.v.y();
    const double z = q.v.z();
    Mat3 rotation;
    rotation <<
        1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - w * z),
        2.0 * (x * z + w * y),
        2.0 * (x * y + w * z), 1.0 - 2.0 * (x * x + z * z),
        2.0 * (y * z - w * x),
        2.0 * (x * z - w * y), 2.0 * (y * z + w * x),
        1.0 - 2.0 * (x * x + y * y);
    return rotation;
}

Quat FromMatrix(const Mat3& rotation)
{
    Eigen::Quaterniond q(rotation);
    q.normalize();
    return {q.w(), Vec(q.x(), q.y(), q.z())};
}

Quat FromTwoVectors(const Vec& from, const Vec& to)
{
    const Vec a = from.normalized();
    const Vec b = to.normalized();
    const double cosine = std::clamp(a.dot(b), -1.0, 1.0);
    if (cosine > 1.0 - 1.0e-12)
        return {};
    if (cosine < -1.0 + 1.0e-12)
        return Exp(a.unitOrthogonal() * Pi);
    const Vec axis = a.cross(b).normalized();
    return Exp(axis * std::acos(cosine));
}

struct Params
{
    double EA = 1.0e4;
    double EI = 100.0;
    double GJ = 100.0;
    double GA = 1.0e4 / 2.6;
    double gravity = -9.8;
    double dt = 1.0 / 240.0;
    int substeps = 8;
    int rootFixed = 2;
    double damping = 0.002;
    double restLengthBound = 0.05;
    double flipMargin = 0.10;
    double clumpStiffness = 0.0;
    int clumpGroupSize = 4;
    bool clumpWholeGuide = false;
    double globalTetherStiffness = 0.0;
    double localTetherStiffness = 0.0;
    int localTetherStride = 0;
    // M0-011: radial dead zone around the authored rest point.  Inside
    // |x-x_rest| <= tetherDeadZone the shape term is inactive, so wind can
    // move the strand without fighting a linear spring.  Outside it the
    // restoring force is k*(d-eps), optionally plus k2*(d-eps)^3.
    double tetherDeadZone = 0.0;
    double tetherProgressive = 0.0;
};

struct Scene
{
    std::vector<Vec> x;
    std::vector<Quat> q;
    std::vector<double> restLengths;
    double totalLength = 0.0;
};

struct Rest
{
    std::vector<double> lengths;
    std::vector<Vec> relativeRotation;
};

struct State
{
    std::vector<Vec> x;
    std::vector<Quat> q;
    std::vector<Vec> velocity;
    std::vector<Vec> angularVelocity;
};

struct StateLayout
{
    size_t positionCount = 0;
    size_t quaternionCount = 0;
    size_t rootFixed = 0;

    size_t Dof() const { return positionCount + quaternionCount; }

    size_t PositionIndex(size_t point, int axis) const
    {
        return (point - rootFixed) * 3 + static_cast<size_t>(axis);
    }

    size_t QuaternionIndex(size_t segment, int axis) const
    {
        return positionCount +
            (segment - 1) * 3 + static_cast<size_t>(axis);
    }
};

StateLayout BuildLayout(const Scene& scene, const Params& params)
{
    StateLayout layout;
    layout.rootFixed = static_cast<size_t>(params.rootFixed);
    layout.positionCount =
        (scene.x.size() - layout.rootFixed) * 3;
    layout.quaternionCount = (scene.q.size() - 1) * 3;
    return layout;
}

double TotalLength(const std::vector<Vec>& points)
{
    double length = 0.0;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        length += (points[i + 1] - points[i]).norm();
    return length;
}

std::vector<Vec> Helix(int count, double radius, double height, int turns)
{
    std::vector<Vec> points;
    points.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const double s = static_cast<double>(i) /
            static_cast<double>(std::max(1, count - 1));
        const double angle = 2.0 * Pi * turns * s;
        points.emplace_back(radius * std::cos(angle),
            height * s, radius * std::sin(angle));
    }
    return points;
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

std::vector<Vec> InitialBendChain(
    int count, double length, double totalAngle)
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

Scene BuildScene(const std::string& name, int points)
{
    Scene scene;
    if (name == "straight")
        scene.x = StraightChain(points, 20.0);
    else if (name == "helix")
        scene.x = Helix(points, 3.0, 20.0, 1);
    else if (name == "initial-bend")
        scene.x = InitialBendChain(points, 20.0, 0.5 * Pi);
    else
        scene.x.clear();

    const size_t segmentCount = scene.x.size() - 1;
    scene.q.resize(segmentCount);
    scene.restLengths.resize(segmentCount);
    for (size_t s = 0; s < segmentCount; ++s)
    {
        scene.restLengths[s] =
            (scene.x[s + 1] - scene.x[s]).norm();
    }
    scene.totalLength = TotalLength(scene.x);

    const Vec firstTangent =
        (scene.x[1] - scene.x[0]).normalized();
    const Vec reference = std::abs(firstTangent.y()) < 0.9
        ? Vec(0.0, 1.0, 0.0) : Vec(1.0, 0.0, 0.0);
    Vec first = reference.cross(firstTangent);
    if (first.squaredNorm() < 1.0e-16)
        first = Vec(0.0, 0.0, 1.0).cross(firstTangent);
    first.normalize();
    const Vec second = firstTangent.cross(first).normalized();
    Mat3 initialFrame;
    initialFrame.col(0) = first;
    initialFrame.col(1) = second;
    initialFrame.col(2) = firstTangent;
    scene.q[0] = FromMatrix(initialFrame);

    for (size_t s = 1; s < segmentCount; ++s)
    {
        const Vec previousTangent =
            (scene.x[s] - scene.x[s - 1]).normalized();
        const Vec currentTangent =
            (scene.x[s + 1] - scene.x[s]).normalized();
        const Quat delta =
            FromTwoVectors(previousTangent, currentTangent);
        scene.q[s] = Normalize(delta * scene.q[s - 1]);
    }
    return scene;
}

Rest AuthoredRest(const Scene& scene)
{
    Rest rest;
    rest.lengths = scene.restLengths;
    rest.relativeRotation.resize(scene.q.size() - 1);
    for (size_t s = 0; s + 1 < scene.q.size(); ++s)
    {
        rest.relativeRotation[s] =
            Log(Conjugate(scene.q[s]) * scene.q[s + 1]);
    }
    return rest;
}

Eigen::VectorXd PackRest(const Scene& scene, const Rest& rest)
{
    const size_t segmentCount = scene.restLengths.size();
    Eigen::VectorXd variables(
        static_cast<Eigen::Index>(
            segmentCount + (segmentCount - 1) * 3));
    Eigen::Index index = 0;
    for (size_t s = 0; s < segmentCount; ++s)
    {
        variables[index++] =
            rest.lengths[s] / scene.restLengths[s] - 1.0;
    }
    for (size_t s = 0; s + 1 < segmentCount; ++s)
    {
        variables[index++] = rest.relativeRotation[s].x();
        variables[index++] = rest.relativeRotation[s].y();
        variables[index++] = rest.relativeRotation[s].z();
    }
    return variables;
}

bool UnpackRest(const Scene& scene, const Params& params,
    const Eigen::VectorXd& variables, Rest& rest)
{
    const size_t segmentCount = scene.restLengths.size();
    const Eigen::Index expected =
        static_cast<Eigen::Index>(
            segmentCount + (segmentCount - 1) * 3);
    if (variables.size() != expected || !variables.allFinite())
        return false;
    rest.lengths.resize(segmentCount);
    rest.relativeRotation.resize(segmentCount - 1);
    Eigen::Index index = 0;
    for (size_t s = 0; s < segmentCount; ++s)
    {
        const double relativeScale = variables[index++];
        if (std::abs(relativeScale) > 1.0)
            return false;
        rest.lengths[s] = scene.restLengths[s] *
            (1.0 + params.restLengthBound * relativeScale);
    }
    const double rotationScale = Pi - params.flipMargin;
    for (size_t s = 0; s + 1 < segmentCount; ++s)
    {
        Vec rotation;
        rotation.x() = rotationScale * variables[index++];
        rotation.y() = rotationScale * variables[index++];
        rotation.z() = rotationScale * variables[index++];
        if (rotation.norm() > rotationScale)
            return false;
        rest.relativeRotation[s] = rotation;
    }
    return true;
}

struct EnergyBreakdown
{
    double stretch = 0.0;
    double shear = 0.0;
    double bend = 0.0;
    double twist = 0.0;
    double gravity = 0.0;
    double clump = 0.0;
    double tether = 0.0;

    double Total() const
    {
        return stretch + shear + bend + twist + gravity + clump + tether;
    }
};

std::vector<std::vector<size_t>> BuildClumpGroups(
    const Scene& scene, const Params& params)
{
    std::vector<std::vector<size_t>> groups;
    if (!(params.clumpStiffness > 0.0))
        return groups;
    const size_t count = scene.x.size();
    if (params.clumpWholeGuide)
    {
        std::vector<size_t> group;
        for (size_t i = 0; i < count; ++i)
            group.push_back(i);
        if (group.size() >= 2)
            groups.push_back(group);
        return groups;
    }
    const size_t groupSize = static_cast<size_t>(
        std::max(2, params.clumpGroupSize));
    for (size_t start = 0; start < count; start += groupSize)
    {
        std::vector<size_t> group;
        const size_t end = std::min(count, start + groupSize);
        for (size_t i = start; i < end; ++i)
            group.push_back(i);
        if (group.size() >= 2)
            groups.push_back(group);
    }
    return groups;
}

double ClumpEnergyAndResidual(const Scene& scene,
    const Params& params, const State& state,
    double* maxResidual = nullptr)
{
    if (maxResidual != nullptr)
        *maxResidual = 0.0;
    if (!(params.clumpStiffness > 0.0))
        return 0.0;
    const auto groups = BuildClumpGroups(scene, params);
    double energy = 0.0;
    double maxC = 0.0;
    for (const std::vector<size_t>& group : groups)
    {
        Vec authoredCentroid = Vec::Zero();
        Vec currentCentroid = Vec::Zero();
        for (size_t point : group)
        {
            authoredCentroid += scene.x[point];
            currentCentroid += state.x[point];
        }
        authoredCentroid /= static_cast<double>(group.size());
        currentCentroid /= static_cast<double>(group.size());
        for (size_t point : group)
        {
            const double restDistance =
                (scene.x[point] - authoredCentroid).norm();
            const double C =
                (state.x[point] - currentCentroid).norm() -
                restDistance;
            energy += 0.5 * params.clumpStiffness * C * C;
            maxC = std::max(maxC, std::abs(C));
        }
    }
    if (maxResidual != nullptr)
        *maxResidual = maxC;
    return energy;
}

EnergyBreakdown EnergyAt(const Scene& scene, const Params& params,
    const Rest& rest, const State& state)
{
    EnergyBreakdown energy;
    const size_t segmentCount = scene.restLengths.size();
    for (size_t s = 0; s < segmentCount; ++s)
    {
        const Vec delta = state.x[s + 1] - state.x[s];
        const double length =
            std::max(rest.lengths[s], 1.0e-12);
        const Mat3 frame = ToMatrix(state.q[s]);
        const Vec strain =
            frame.transpose() * delta / length - Vec(0.0, 0.0, 1.0);
        energy.stretch +=
            0.5 * params.EA * length * strain.z() * strain.z();
        energy.shear += 0.5 * params.GA * length *
            (strain.x() * strain.x() + strain.y() * strain.y());
    }
    for (size_t s = 0; s + 1 < segmentCount; ++s)
    {
        // Use the rest Lbar for the angular energy.  This keeps the force
        // block (x, q_target) and torque block (q, rest rotation) exactly
        // decoupled; a current-length Lbar would make the angular energy
        // depend on x and reintroduce the ill-conditioned cross-coupling.
        const double lengthBar = std::max(
            0.5 * (rest.lengths[s] + rest.lengths[s + 1]),
            1.0e-12);
        const Quat relative =
            Normalize(Conjugate(state.q[s]) * state.q[s + 1]);
        const Vec curvature = Log(relative) / lengthBar;
        const Vec restCurvature =
            rest.relativeRotation[s] / lengthBar;
        const Vec difference = curvature - restCurvature;
        energy.bend += 0.5 * params.EI / lengthBar *
            (difference.x() * difference.x() +
                difference.y() * difference.y());
        energy.twist += 0.5 * params.GJ / lengthBar *
            difference.z() * difference.z();
    }
    double sumY = 0.0;
    for (const Vec& point : state.x)
        sumY += point.y();
    energy.gravity = -params.gravity * sumY;
    energy.clump =
        ClumpEnergyAndResidual(scene, params, state, nullptr);
    const size_t rootFixed =
        static_cast<size_t>(params.rootFixed);
    for (size_t point = rootFixed; point < state.x.size(); ++point)
    {
        double stiffness = params.globalTetherStiffness;
        if (params.localTetherStride > 0 &&
            (point - rootFixed) %
                static_cast<size_t>(params.localTetherStride) == 0)
        {
            stiffness += params.localTetherStiffness;
        }
        if (stiffness > 0.0 || params.tetherProgressive > 0.0)
        {
            const double distance =
                (state.x[point] - scene.x[point]).norm();
            const double excess =
                std::max(0.0, distance - params.tetherDeadZone);
            energy.tether +=
                0.5 * stiffness * excess * excess;
            if (params.tetherProgressive > 0.0)
            {
                const double excessSquared = excess * excess;
                energy.tether +=
                    0.25 * params.tetherProgressive *
                    excessSquared * excessSquared;
            }
        }
    }
    return energy;
}

State AuthoredState(const Scene& scene)
{
    State state;
    state.x = scene.x;
    state.q = scene.q;
    state.velocity.assign(scene.x.size(), Vec::Zero());
    state.angularVelocity.assign(scene.q.size(), Vec::Zero());
    return state;
}

Eigen::Index InverseVariableCount(const Scene& scene)
{
    const size_t segmentCount = scene.restLengths.size();
    return static_cast<Eigen::Index>(
        segmentCount + 6 * (segmentCount - 1));
}

Eigen::VectorXd PackInverse(const Scene& scene, const Params& params,
    const Rest& rest, const State& target)
{
    const size_t segmentCount = scene.restLengths.size();
    Eigen::VectorXd variables(InverseVariableCount(scene));
    Eigen::Index index = 0;
    for (size_t s = 0; s < segmentCount; ++s)
    {
        variables[index++] =
            (rest.lengths[s] / scene.restLengths[s] - 1.0) /
            params.restLengthBound;
    }
    const double rotationScale = Pi - params.flipMargin;
    for (size_t s = 0; s + 1 < segmentCount; ++s)
    {
        variables[index++] =
            rest.relativeRotation[s].x() / rotationScale;
        variables[index++] =
            rest.relativeRotation[s].y() / rotationScale;
        variables[index++] =
            rest.relativeRotation[s].z() / rotationScale;
    }
    for (size_t s = 1; s < segmentCount; ++s)
    {
        const Vec delta =
            2.0 * Log(Conjugate(scene.q[s]) * target.q[s]);
        variables[index++] = delta.x() / TargetDeviationBound;
        variables[index++] = delta.y() / TargetDeviationBound;
        variables[index++] = delta.z() / TargetDeviationBound;
    }
    return variables;
}

bool UnpackInverse(const Scene& scene, const Params& params,
    const Eigen::VectorXd& variables, Rest& rest, State& target)
{
    const size_t segmentCount = scene.restLengths.size();
    if (variables.size() != InverseVariableCount(scene) ||
        !variables.allFinite())
    {
        return false;
    }
    const Eigen::Index restVariableCount =
        static_cast<Eigen::Index>(
            segmentCount + 3 * (segmentCount - 1));
    if (!UnpackRest(scene, params,
        variables.head(restVariableCount), rest))
    {
        return false;
    }
    target = AuthoredState(scene);
    Eigen::Index index = restVariableCount;
    for (size_t s = 1; s < segmentCount; ++s)
    {
        Vec delta;
        delta.x() = TargetDeviationBound * variables[index++];
        delta.y() = TargetDeviationBound * variables[index++];
        delta.z() = TargetDeviationBound * variables[index++];
        if (delta.norm() > TargetDeviationBound)
            return false;
        target.q[s] = Normalize(
            scene.q[s] * Exp(0.5 * delta));
    }
    return true;
}

void ApplyDelta(const Scene& scene, const Params& params,
    const State& base, const Eigen::VectorXd& delta, double scale,
    State& result)
{
    result = base;
    const StateLayout layout = BuildLayout(scene, params);
    for (size_t point = layout.rootFixed;
        point < base.x.size(); ++point)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            result.x[point][axis] += scale *
                delta[static_cast<Eigen::Index>(
                    layout.PositionIndex(point, axis))];
        }
    }
    for (size_t segment = 1; segment < base.q.size(); ++segment)
    {
        Vec rotation;
        for (int axis = 0; axis < 3; ++axis)
        {
            rotation[axis] = scale *
                delta[static_cast<Eigen::Index>(
                    layout.QuaternionIndex(segment, axis))];
        }
        result.q[segment] = Normalize(
            base.q[segment] * Exp(0.5 * rotation));
    }
}

Eigen::VectorXd StateGradient(const Scene& scene, const Params& params,
    const Rest& rest, const State& state)
{
    const StateLayout layout = BuildLayout(scene, params);
    Eigen::VectorXd gradient(layout.Dof());
    Eigen::VectorXd delta = Eigen::VectorXd::Zero(layout.Dof());
    const double positionStep =
        1.0e-5 * scene.totalLength /
        static_cast<double>(scene.restLengths.size());
    const double quaternionStep = 1.0e-5;
    State plus;
    State minus;
    for (size_t point = layout.rootFixed;
        point < state.x.size(); ++point)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const Eigen::Index index =
                static_cast<Eigen::Index>(
                    layout.PositionIndex(point, axis));
            delta[index] = 1.0;
            ApplyDelta(scene, params, state, delta,
                positionStep, plus);
            ApplyDelta(scene, params, state, delta,
                -positionStep, minus);
            gradient[index] =
                (EnergyAt(scene, params, rest, plus).Total() -
                    EnergyAt(scene, params, rest, minus).Total()) /
                (2.0 * positionStep);
            delta[index] = 0.0;
        }
    }
    for (size_t segment = 1;
        segment < state.q.size(); ++segment)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const Eigen::Index index =
                static_cast<Eigen::Index>(
                    layout.QuaternionIndex(segment, axis));
            delta[index] = 1.0;
            ApplyDelta(scene, params, state, delta,
                quaternionStep, plus);
            ApplyDelta(scene, params, state, delta,
                -quaternionStep, minus);
            gradient[index] =
                (EnergyAt(scene, params, rest, plus).Total() -
                    EnergyAt(scene, params, rest, minus).Total()) /
                (2.0 * quaternionStep);
            delta[index] = 0.0;
        }
    }
    return gradient;
}

Eigen::VectorXd ScaledResidual(const Scene& scene, const Params& params,
    const Rest& rest, const State& target)
{
    const Eigen::VectorXd gradient =
        StateGradient(scene, params, rest, target);
    const StateLayout layout = BuildLayout(scene, params);
    const Eigen::Index primaryCount = layout.Dof();
    const size_t segmentCount = scene.restLengths.size();
    Eigen::VectorXd residual(
        primaryCount + 4 * static_cast<Eigen::Index>(segmentCount - 1));
    residual.head(primaryCount) = -gradient;
    const double forceScale = std::abs(params.gravity);
    const double torqueScale =
        std::abs(params.gravity) * scene.totalLength;
    for (size_t point = layout.rootFixed;
        point < scene.x.size(); ++point)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            residual[static_cast<Eigen::Index>(
                layout.PositionIndex(point, axis))] /= forceScale;
        }
    }
    for (size_t segment = 1;
        segment < scene.q.size(); ++segment)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            residual[static_cast<Eigen::Index>(
                layout.QuaternionIndex(segment, axis))] /=
                torqueScale;
            residual[static_cast<Eigen::Index>(
                layout.QuaternionIndex(segment, axis))] *=
                TorqueEquationWeight;
        }
    }
    Eigen::Index index = primaryCount;
    constexpr double targetWeight = 1.0e-3;
    for (size_t s = 1; s < segmentCount; ++s)
    {
        const Vec delta =
            2.0 * Log(Conjugate(scene.q[s]) * target.q[s]);
        residual[index++] = targetWeight * delta.x();
        residual[index++] = targetWeight * delta.y();
        residual[index++] = targetWeight * delta.z();
    }
    constexpr double twistWeight = 1.0e-3;
    for (size_t s = 0; s + 1 < segmentCount; ++s)
    {
        residual[index++] =
            twistWeight * rest.relativeRotation[s].z();
    }
    return residual;
}

Eigen::MatrixXd InverseJacobian(const Scene& scene, const Params& params,
    const Rest& rest, const State& target)
{
    const size_t segmentCount = scene.restLengths.size();
    const Eigen::Index variableCount = InverseVariableCount(scene);
    const Eigen::VectorXd base =
        PackInverse(scene, params, rest, target);
    const Eigen::Index residualCount =
        ScaledResidual(scene, params, rest, target).size();
    Eigen::MatrixXd jacobian(residualCount, variableCount);
    const double step = 1.0e-6;
    for (Eigen::Index column = 0;
        column < variableCount; ++column)
    {
        Eigen::VectorXd plus = base;
        Eigen::VectorXd minus = base;
        plus[column] += step;
        minus[column] -= step;
        Rest plusRest;
        Rest minusRest;
        State plusTarget;
        State minusTarget;
        if (!UnpackInverse(scene, params, plus,
                plusRest, plusTarget) ||
            !UnpackInverse(scene, params, minus,
                minusRest, minusTarget))
        {
            jacobian.col(column).setZero();
            continue;
        }
        const Eigen::VectorXd plusResidual =
            ScaledResidual(scene, params, plusRest, plusTarget);
        const Eigen::VectorXd minusResidual =
            ScaledResidual(scene, params, minusRest, minusTarget);
        if (!plusResidual.allFinite() ||
            !minusResidual.allFinite())
        {
            jacobian.col(column).setZero();
            continue;
        }
        jacobian.col(column) =
            (plusResidual - minusResidual) / (2.0 * step);
    }
    return jacobian;
}

struct StabilityResult
{
    bool finite = true;
    double minEigenvalue = 0.0;
    double maxEigenvalue = 0.0;
    std::vector<double> smallestEigenvalues;
    Eigen::VectorXd minEigenvector;
};

Eigen::MatrixXd ComputeStateHessianMatrix(const Scene& scene,
    const Params& params, const Rest& rest, const State& target)
{
    const StateLayout layout = BuildLayout(scene, params);
    const State& state = target;
    Eigen::MatrixXd hessian(layout.Dof(), layout.Dof());
    Eigen::VectorXd delta = Eigen::VectorXd::Zero(layout.Dof());
    const double positionStep =
        1.0e-4 * scene.totalLength /
        static_cast<double>(scene.restLengths.size());
    const double quaternionStep = 1.0e-4;
    State plus;
    State minus;
    for (size_t point = layout.rootFixed;
        point < state.x.size(); ++point)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const Eigen::Index index =
                static_cast<Eigen::Index>(
                    layout.PositionIndex(point, axis));
            delta[index] = 1.0;
            ApplyDelta(scene, params, state, delta,
                positionStep, plus);
            ApplyDelta(scene, params, state, delta,
                -positionStep, minus);
            const Eigen::VectorXd plusGradient =
                StateGradient(scene, params, rest, plus);
            const Eigen::VectorXd minusGradient =
                StateGradient(scene, params, rest, minus);
            if (!plusGradient.allFinite() ||
                !minusGradient.allFinite())
            {
                return Eigen::MatrixXd::Constant(
                    layout.Dof(), layout.Dof(),
                    std::numeric_limits<double>::quiet_NaN());
            }
            hessian.col(index) =
                (plusGradient - minusGradient) /
                (2.0 * positionStep);
            delta[index] = 0.0;
        }
    }
    for (size_t segment = 1;
        segment < state.q.size(); ++segment)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const Eigen::Index index =
                static_cast<Eigen::Index>(
                    layout.QuaternionIndex(segment, axis));
            delta[index] = 1.0;
            ApplyDelta(scene, params, state, delta,
                quaternionStep, plus);
            ApplyDelta(scene, params, state, delta,
                -quaternionStep, minus);
            const Eigen::VectorXd plusGradient =
                StateGradient(scene, params, rest, plus);
            const Eigen::VectorXd minusGradient =
                StateGradient(scene, params, rest, minus);
            if (!plusGradient.allFinite() ||
                !minusGradient.allFinite())
            {
                return Eigen::MatrixXd::Constant(
                    layout.Dof(), layout.Dof(),
                    std::numeric_limits<double>::quiet_NaN());
            }
            hessian.col(index) =
                (plusGradient - minusGradient) /
                (2.0 * quaternionStep);
            delta[index] = 0.0;
        }
    }
    return 0.5 * (hessian + hessian.transpose());
}

StabilityResult ComputeStateHessian(const Scene& scene,
    const Params& params, const Rest& rest, const State& target)
{
    StabilityResult result;
    const Eigen::MatrixXd symmetric =
        ComputeStateHessianMatrix(scene, params, rest, target);
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
    const Eigen::Index first = std::max<Eigen::Index>(
        0, solver.eigenvalues().size() - 3);
    for (Eigen::Index i = first; i < solver.eigenvalues().size(); ++i)
        result.smallestEigenvalues.push_back(solver.eigenvalues()[i]);
    return result;
}

EnergyBreakdown StateQuadraticForm(const Scene& scene,
    const Params& params, const Rest& rest, const State& target,
    const Eigen::VectorXd& direction)
{
    const State& state = target;
    const double step = 1.0e-4;
    State plus;
    State minus;
    ApplyDelta(scene, params, state, direction, step, plus);
    ApplyDelta(scene, params, state, direction, -step, minus);
    const EnergyBreakdown base =
        EnergyAt(scene, params, rest, state);
    const EnergyBreakdown positive =
        EnergyAt(scene, params, rest, plus);
    const EnergyBreakdown negative =
        EnergyAt(scene, params, rest, minus);
    const double inverseStepSquared = 1.0 / (step * step);
    EnergyBreakdown quadratic;
    quadratic.stretch = (positive.stretch -
        2.0 * base.stretch + negative.stretch) * inverseStepSquared;
    quadratic.shear = (positive.shear -
        2.0 * base.shear + negative.shear) * inverseStepSquared;
    quadratic.bend = (positive.bend -
        2.0 * base.bend + negative.bend) * inverseStepSquared;
    quadratic.twist = (positive.twist -
        2.0 * base.twist + negative.twist) * inverseStepSquared;
    quadratic.gravity = (positive.gravity -
        2.0 * base.gravity + negative.gravity) * inverseStepSquared;
    quadratic.clump = (positive.clump -
        2.0 * base.clump + negative.clump) * inverseStepSquared;
    return quadratic;
}

struct SimMetrics
{
    bool finite = true;
    double finalMaxDeviation = 0.0;
    double finalTipDeviation = 0.0;
    double trajectoryMaxDeviation = 0.0;
    double trajectoryMaxTip = 0.0;
    double maxSegmentStretch = 0.0;
    double maxClumpResidual = 0.0;
    double maxTetherResidual = 0.0;
    std::vector<double> finalDeviations;
};

bool AdvanceOneStep(const Scene& scene, const Params& params,
    const Rest& rest, State& state,
    const Vec& windAcceleration)
{
    const double h = params.dt /
        static_cast<double>(std::max(1, params.substeps));
    const StateLayout layout = BuildLayout(scene, params);
    for (int substep = 0; substep < params.substeps; ++substep)
    {
        const Eigen::VectorXd gradient =
            StateGradient(scene, params, rest, state);
        if (!gradient.allFinite())
            return false;
        for (size_t point = layout.rootFixed;
            point < state.x.size(); ++point)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                const Eigen::Index index =
                    static_cast<Eigen::Index>(
                        layout.PositionIndex(point, axis));
                state.velocity[point][axis] =
                    (1.0 - params.damping) *
                        state.velocity[point][axis] -
                    h * gradient[index] +
                    h * windAcceleration[axis];
                state.x[point][axis] +=
                    h * state.velocity[point][axis];
            }
        }
        for (size_t segment = 1;
            segment < state.q.size(); ++segment)
        {
            Vec torque;
            for (int axis = 0; axis < 3; ++axis)
            {
                torque[axis] = -gradient[
                    static_cast<Eigen::Index>(
                        layout.QuaternionIndex(segment, axis))];
            }
            state.angularVelocity[segment] =
                (1.0 - params.damping) *
                    state.angularVelocity[segment] +
                h * torque;
            const Quat previous = state.q[segment];
            state.q[segment] = Normalize(
                state.q[segment] *
                Exp(0.5 * h *
                    state.angularVelocity[segment]));
            state.angularVelocity[segment] =
                (2.0 / h) *
                Log(Conjugate(previous) *
                    state.q[segment]);
        }
    }
    return true;
}

SimMetrics Simulate(const Scene& scene, const Params& params,
    const Rest& rest, const State& initial, int steps)
{
    SimMetrics metrics;
    State state = initial;
    state.velocity.assign(scene.x.size(), Vec::Zero());
    state.angularVelocity.assign(scene.q.size(), Vec::Zero());
    const StateLayout layout = BuildLayout(scene, params);
    for (int step = 0; step < steps; ++step)
    {
        if (!AdvanceOneStep(
            scene, params, rest, state, Vec::Zero()))
        {
            metrics.finite = false;
            break;
        }
        if (!metrics.finite)
            break;
        for (const Vec& point : state.x)
        {
            if (!point.allFinite())
                metrics.finite = false;
        }
        for (const Quat& orientation : state.q)
        {
            if (!std::isfinite(orientation.w) ||
                !orientation.v.allFinite())
            {
                metrics.finite = false;
            }
        }
        if (!metrics.finite)
            break;
        double maxDeviation = 0.0;
        for (size_t i = 0; i < scene.x.size(); ++i)
        {
            maxDeviation = std::max(maxDeviation,
                (state.x[i] - scene.x[i]).norm());
        }
        const double tipDeviation =
            (state.x.back() - scene.x.back()).norm();
        metrics.trajectoryMaxDeviation = std::max(
            metrics.trajectoryMaxDeviation,
            maxDeviation / scene.totalLength);
        metrics.trajectoryMaxTip = std::max(
            metrics.trajectoryMaxTip,
            tipDeviation / scene.totalLength);
        double clumpResidual = 0.0;
        ClumpEnergyAndResidual(
            scene, params, state, &clumpResidual);
        metrics.maxClumpResidual = std::max(
            metrics.maxClumpResidual, clumpResidual);
        double tetherResidual = 0.0;
        for (size_t point = layout.rootFixed;
            point < scene.x.size(); ++point)
        {
            const bool tethered =
                params.globalTetherStiffness > 0.0 ||
                (params.localTetherStride > 0 &&
                    (point - layout.rootFixed) %
                        static_cast<size_t>(
                            params.localTetherStride) == 0);
            if (tethered)
            {
                tetherResidual = std::max(tetherResidual,
                    (state.x[point] - scene.x[point]).norm());
            }
        }
        metrics.maxTetherResidual = std::max(
            metrics.maxTetherResidual, tetherResidual);
        for (size_t s = 0; s < scene.restLengths.size(); ++s)
        {
            const double length =
                (state.x[s + 1] - state.x[s]).norm();
            const double stretch = std::abs(
                length / rest.lengths[s] - 1.0);
            metrics.maxSegmentStretch =
                std::max(metrics.maxSegmentStretch, stretch);
        }
        if (step == steps - 1)
        {
            metrics.finalMaxDeviation =
                maxDeviation / scene.totalLength;
            metrics.finalTipDeviation =
                tipDeviation / scene.totalLength;
            metrics.finalDeviations.resize(scene.x.size());
            for (size_t i = 0; i < scene.x.size(); ++i)
            {
                metrics.finalDeviations[i] =
                    (state.x[i] - scene.x[i]).norm();
            }
        }
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
        metrics.maxClumpResidual =
            std::numeric_limits<double>::infinity();
        metrics.maxTetherResidual =
            std::numeric_limits<double>::infinity();
    }
    return metrics;
}

struct SolveReport
{
    bool converged = false;
    bool finite = true;
    int iterations = 0;
    double forceNorm = std::numeric_limits<double>::infinity();
    double torqueNorm = std::numeric_limits<double>::infinity();
    double residualNorm = std::numeric_limits<double>::infinity();
    Rest rest;
    State target;
};

SolveReport SolveInverse(const Scene& scene, const Params& params,
    const Rest& initialRest, const State& initialTarget,
    int maxIterations, double target)
{
    SolveReport report;
    report.rest = initialRest;
    report.target = initialTarget;
    Eigen::VectorXd residual =
        ScaledResidual(scene, params, report.rest, report.target);
    const StateLayout layout = BuildLayout(scene, params);
    double damping = 1.0e-2;
    double trust = 0.5;
    for (int iteration = 0; iteration < maxIterations; ++iteration)
    {
        report.iterations = iteration;
        report.forceNorm = 0.0;
        report.torqueNorm = 0.0;
        for (size_t point = layout.rootFixed;
            point < scene.x.size(); ++point)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                report.forceNorm = std::max(report.forceNorm,
                    std::abs(residual[static_cast<Eigen::Index>(
                        layout.PositionIndex(point, axis))]));
            }
        }
        for (size_t segment = 1;
            segment < scene.q.size(); ++segment)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                report.torqueNorm = std::max(report.torqueNorm,
                    std::abs(residual[static_cast<Eigen::Index>(
                        layout.QuaternionIndex(segment, axis))]));
            }
        }
        report.torqueNorm /= TorqueEquationWeight;
        report.residualNorm = residual.norm();
        if (!residual.allFinite())
        {
            report.finite = false;
            return report;
        }
        if (report.forceNorm < target &&
            report.torqueNorm < target)
        {
            report.converged = true;
            return report;
        }
        const Eigen::MatrixXd jacobian =
            InverseJacobian(
                scene, params, report.rest, report.target);
        if (iteration < 3)
        {
            const Eigen::Index targetStart =
                InverseVariableCount(scene) -
                3 * static_cast<Eigen::Index>(
                    scene.restLengths.size() - 1);
            const Eigen::Index lengthCount =
                static_cast<Eigen::Index>(
                    scene.restLengths.size());
            const double lengthNorm =
                jacobian.leftCols(lengthCount).norm();
            const double restRotationNorm =
                jacobian.middleCols(
                    lengthCount, targetStart - lengthCount).norm();
            const double targetNorm =
                jacobian.rightCols(
                    jacobian.cols() - targetStart).norm();
            const Eigen::Index primaryCount = layout.Dof();
            std::printf(
                "    iter %d force %.3e torque %.3e res %.3e "
                "primary %.3e reg %.3e | Jlen %.3e Jrot %.3e "
                "Jtarget %.3e\n",
                iteration, report.forceNorm, report.torqueNorm,
                report.residualNorm,
                residual.head(primaryCount).norm(),
                residual.tail(
                    residual.size() - primaryCount).norm(),
                lengthNorm, restRotationNorm, targetNorm);
            const Eigen::Index positionRows = layout.positionCount;
            const Eigen::Index torqueRows = layout.quaternionCount;
            const Eigen::Index restRotationStart =
                static_cast<Eigen::Index>(
                    scene.restLengths.size());
            const Eigen::Index restRotationCount =
                3 * static_cast<Eigen::Index>(
                    scene.restLengths.size() - 1);
            std::printf(
                "      blocks: force/restRot %.3e "
                "torque/restRot %.3e force/target %.3e "
                "torque/target %.3e\n",
                jacobian.topRows(positionRows).middleCols(
                    restRotationStart, restRotationCount).norm(),
                jacobian.middleRows(
                    positionRows, torqueRows).middleCols(
                        restRotationStart, restRotationCount).norm(),
                jacobian.topRows(positionRows).rightCols(
                    jacobian.cols() - targetStart).norm(),
                jacobian.middleRows(
                    positionRows, torqueRows).rightCols(
                        jacobian.cols() - targetStart).norm());
        }
        const Eigen::VectorXd base =
            PackInverse(scene, params,
                report.rest, report.target);
        bool accepted = false;
        for (int attempt = 0; attempt < 8 && !accepted; ++attempt)
        {
            Eigen::MatrixXd normal =
                jacobian.transpose() * jacobian;
            normal.diagonal().array() += damping;
            Eigen::VectorXd step =
                normal.ldlt().solve(-jacobian.transpose() * residual);
            if (!step.allFinite())
            {
                damping *= 10.0;
                continue;
            }
            const double stepNorm = step.norm();
            if (stepNorm > trust)
                step *= trust / stepNorm;
            double alpha = 1.0;
            for (int lineSearch = 0;
                lineSearch < 20; ++lineSearch)
            {
                const Eigen::VectorXd candidate =
                    base + alpha * step;
                Rest candidateRest;
                State candidateTarget;
                if (UnpackInverse(scene, params, candidate,
                    candidateRest, candidateTarget))
                {
                    const Eigen::VectorXd candidateResidual =
                        ScaledResidual(scene, params,
                            candidateRest, candidateTarget);
                    if (candidateResidual.allFinite() &&
                        candidateResidual.norm() <
                            (1.0 - 1.0e-4 * alpha) *
                                residual.norm())
                    {
                        report.rest = candidateRest;
                        report.target = candidateTarget;
                        residual = candidateResidual;
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
            if (iteration < 3)
            {
                std::printf(
                    "      attempt %d damping %.3e trust %.3e "
                    "step %.3e alpha %.3e accepted %d\n",
                    attempt, damping, trust, stepNorm,
                    alpha, accepted ? 1 : 0);
            }
        }
        if (!accepted)
            break;
        damping = std::max(1.0e-10, damping / 3.0);
        trust = std::min(2.0, trust * 1.5);
    }
    report.residualNorm = residual.norm();
    report.converged =
        report.forceNorm < target && report.torqueNorm < target;
    return report;
}

struct BlockReport
{
    bool converged = false;
    double primaryNorm = std::numeric_limits<double>::infinity();
    int iterations = 0;
};

BlockReport SolveBlock(const Scene& scene, const Params& params,
    Rest& rest, State& target,
    const std::vector<Eigen::Index>& variables,
    const std::vector<Eigen::Index>& primaryRows,
    const std::vector<Eigen::Index>& regularizationRows,
    int maxIterations, double tolerance)
{
    BlockReport report;
    std::vector<Eigen::Index> rows = primaryRows;
    rows.insert(rows.end(),
        regularizationRows.begin(), regularizationRows.end());
    const auto primaryNorm = [&](const Eigen::VectorXd& full)
    {
        double value = 0.0;
        for (Eigen::Index row : primaryRows)
            value = std::max(value, std::abs(full[row]));
        return value;
    };
    const auto blockResidual = [&](const Eigen::VectorXd& full)
    {
        Eigen::VectorXd block(rows.size());
        for (size_t i = 0; i < rows.size(); ++i)
            block[static_cast<Eigen::Index>(i)] = full[rows[i]];
        return block;
    };
    Eigen::VectorXd full =
        ScaledResidual(scene, params, rest, target);
    double damping = 1.0e-2;
    double trust = 0.1;
    double previousNorm =
        std::numeric_limits<double>::infinity();
    int stagnantIterations = 0;
    for (int iteration = 0; iteration < maxIterations; ++iteration)
    {
        report.iterations = iteration;
        report.primaryNorm = primaryNorm(full);
        if (std::isfinite(previousNorm) &&
            report.primaryNorm >
                previousNorm * (1.0 - 1.0e-6))
        {
            ++stagnantIterations;
        }
        else
        {
            stagnantIterations = 0;
        }
        previousNorm = report.primaryNorm;
        if (report.primaryNorm < tolerance)
        {
            report.converged = true;
            return report;
        }
        if (stagnantIterations >= 5)
            break;
        const Eigen::MatrixXd fullJacobian =
            InverseJacobian(scene, params, rest, target);
        Eigen::MatrixXd jacobian(
            rows.size(), variables.size());
        for (size_t row = 0; row < rows.size(); ++row)
        {
            for (size_t column = 0;
                column < variables.size(); ++column)
            {
                jacobian(
                    static_cast<Eigen::Index>(row),
                    static_cast<Eigen::Index>(column)) =
                    fullJacobian(
                        rows[row], variables[column]);
            }
        }
        const Eigen::VectorXd residual = blockResidual(full);
        bool accepted = false;
        for (int attempt = 0; attempt < 8 && !accepted; ++attempt)
        {
            Eigen::MatrixXd normal =
                jacobian.transpose() * jacobian;
            normal.diagonal().array() += damping;
            Eigen::VectorXd step = normal.ldlt().solve(
                -jacobian.transpose() * residual);
            if (!step.allFinite())
            {
                damping *= 10.0;
                continue;
            }
            const double stepNorm = step.norm();
            if (stepNorm > trust)
                step *= trust / stepNorm;
            const Eigen::VectorXd base =
                PackInverse(scene, params, rest, target);
            const double currentNorm = residual.norm();
            double alpha = 1.0;
            for (int lineSearch = 0;
                lineSearch < 20; ++lineSearch)
            {
                Eigen::VectorXd candidate = base;
                for (size_t i = 0; i < variables.size(); ++i)
                {
                    candidate[variables[i]] +=
                        alpha * step[static_cast<Eigen::Index>(i)];
                }
                Rest candidateRest;
                State candidateTarget;
                if (UnpackInverse(scene, params, candidate,
                    candidateRest, candidateTarget))
                {
                    const Eigen::VectorXd candidateFull =
                        ScaledResidual(scene, params,
                            candidateRest, candidateTarget);
                    const Eigen::VectorXd candidateResidual =
                        blockResidual(candidateFull);
                    if (candidateResidual.allFinite() &&
                        candidateResidual.norm() <
                            (1.0 - 1.0e-4 * alpha) *
                                currentNorm)
                    {
                        rest = candidateRest;
                        target = candidateTarget;
                        full = candidateFull;
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
            if (iteration < 5)
            {
                std::printf(
                    "      block iter %d norm %.3e step %.3e "
                    "alpha %.3e accepted %d damping %.3e\n",
                    iteration, report.primaryNorm, stepNorm,
                    alpha, accepted ? 1 : 0, damping);
            }
        }
        if (!accepted)
            break;
        damping = std::max(1.0e-10, damping / 3.0);
        trust = std::min(1.0, trust * 1.5);
    }
    report.primaryNorm = primaryNorm(full);
    report.converged = report.primaryNorm < tolerance;
    return report;
}

SolveReport SolveBlockInverse(const Scene& scene,
    const Params& params, const Rest& initialRest,
    const State& initialTarget, int maxIterations, double target)
{
    SolveReport report;
    report.rest = initialRest;
    report.target = initialTarget;
    const size_t segmentCount = scene.restLengths.size();
    const StateLayout layout = BuildLayout(scene, params);
    const Eigen::Index variableCount =
        InverseVariableCount(scene);
    const Eigen::Index targetStart =
        variableCount - 3 * static_cast<Eigen::Index>(
            segmentCount - 1);
    const Eigen::Index primaryCount = layout.Dof();
    const Eigen::Index positionCount = layout.positionCount;
    const Eigen::Index quaternionCount = layout.quaternionCount;

    std::vector<Eigen::Index> forceVariables;
    for (size_t s = 0; s < segmentCount; ++s)
    {
        forceVariables.push_back(
            static_cast<Eigen::Index>(s));
    }
    for (Eigen::Index i = targetStart; i < variableCount; ++i)
        forceVariables.push_back(i);

    std::vector<Eigen::Index> forceRows;
    for (Eigen::Index i = 0; i < positionCount; ++i)
        forceRows.push_back(i);

    std::vector<Eigen::Index> forceRegularizationRows;
    for (Eigen::Index i = primaryCount;
        i < primaryCount + 3 * static_cast<Eigen::Index>(
            segmentCount - 1);
        ++i)
    {
        forceRegularizationRows.push_back(i);
    }

    std::vector<Eigen::Index> torqueVariables;
    for (Eigen::Index i = static_cast<Eigen::Index>(segmentCount);
        i < targetStart; ++i)
    {
        torqueVariables.push_back(i);
    }

    std::vector<Eigen::Index> torqueRows;
    for (Eigen::Index i = positionCount;
        i < positionCount + quaternionCount; ++i)
    {
        torqueRows.push_back(i);
    }

    const BlockReport force = SolveBlock(
        scene, params, report.rest, report.target,
        forceVariables, forceRows, forceRegularizationRows,
        maxIterations, target);
    const BlockReport torque = SolveBlock(
        scene, params, report.rest, report.target,
        torqueVariables, torqueRows, {},
        std::min(maxIterations, 20), target);
    std::printf(
        "    blocks: force iter %d norm %.3e %s; "
        "torque iter %d norm %.3e %s\n",
        force.iterations, force.primaryNorm,
        force.converged ? "converged" : "not-converged",
        torque.iterations, torque.primaryNorm,
        torque.converged ? "converged" : "not-converged");

    {
        const Eigen::MatrixXd fullJacobian =
            InverseJacobian(
                scene, params, report.rest, report.target);
        Eigen::MatrixXd torqueJacobian(
            torqueRows.size(), torqueVariables.size());
        for (size_t row = 0; row < torqueRows.size(); ++row)
        {
            for (size_t column = 0;
                column < torqueVariables.size(); ++column)
            {
                torqueJacobian(
                    static_cast<Eigen::Index>(row),
                    static_cast<Eigen::Index>(column)) =
                    fullJacobian(
                        torqueRows[row], torqueVariables[column]);
            }
        }
        Eigen::JacobiSVD<Eigen::MatrixXd> svd(
            torqueJacobian,
            Eigen::ComputeThinU | Eigen::ComputeFullV);
        const Eigen::VectorXd singular = svd.singularValues();
        const double sigmaMax = singular.maxCoeff();
        const double sigmaMin = singular.minCoeff();
        const double tolerance =
            static_cast<double>(
                std::max(torqueJacobian.rows(),
                    torqueJacobian.cols())) *
            std::numeric_limits<double>::epsilon() *
            std::max(1.0, sigmaMax) * 100.0;
        int rank = 0;
        for (Eigen::Index i = 0; i < singular.size(); ++i)
        {
            if (singular[i] > tolerance)
                ++rank;
        }
        const Eigen::VectorXd residual =
            ScaledResidual(
                scene, params, report.rest, report.target);
        Eigen::VectorXd torqueResidual(torqueRows.size());
        for (size_t row = 0; row < torqueRows.size(); ++row)
            torqueResidual[static_cast<Eigen::Index>(row)] =
                residual[torqueRows[row]];
        Eigen::VectorXd leftNull = torqueResidual;
        for (int i = 0; i < rank; ++i)
        {
            const Eigen::VectorXd column =
                svd.matrixU().col(i);
            leftNull -= column * column.dot(torqueResidual);
        }
        std::printf(
            "    torque block: rank %d/%lld sigma [%.3e, %.3e] "
            "cond %.3e; residual %.3e; left-null %.3e\n",
            rank, static_cast<long long>(singular.size()),
            sigmaMin, sigmaMax,
            sigmaMin > 0.0 ? sigmaMax / sigmaMin
                           : std::numeric_limits<double>::infinity(),
            torqueResidual.norm(), leftNull.norm());
        double maxAngle = 0.0;
        for (const Vec& rotation : report.rest.relativeRotation)
            maxAngle = std::max(maxAngle, rotation.norm());
        std::printf(
            "    rest angle after torque: max %.6e rad, "
            "flip margin %.6e rad\n",
            maxAngle, Pi - maxAngle);
    }

    const Eigen::VectorXd residual =
        ScaledResidual(scene, params, report.rest, report.target);
    report.forceNorm = 0.0;
    report.torqueNorm = 0.0;
    for (Eigen::Index i = 0; i < positionCount; ++i)
    {
        report.forceNorm = std::max(
            report.forceNorm, std::abs(residual[i]));
    }
    for (Eigen::Index i = positionCount;
        i < positionCount + quaternionCount; ++i)
    {
        report.torqueNorm = std::max(
            report.torqueNorm, std::abs(residual[i]));
    }
    report.torqueNorm /= TorqueEquationWeight;
    report.residualNorm = residual.norm();
    report.converged =
        report.forceNorm < target &&
        report.torqueNorm < target;
    report.finite = residual.allFinite();
    return report;
}

struct StaticMetrics
{
    double forceNorm = 0.0;
    double torqueNorm = 0.0;
    double maxRestLengthChange = 0.0;
    double maxRestAngle = 0.0;
    double minFlipMargin = Pi;
    double maxTargetDeviation = 0.0;
    double maxTargetShear = 0.0;
    double clumpResidual = 0.0;
    StabilityResult stability;
};

StaticMetrics EvaluateStatic(const Scene& scene, const Params& params,
    const Rest& rest, const State& target)
{
    StaticMetrics metrics;
    const Eigen::VectorXd residual =
        ScaledResidual(scene, params, rest, target);
    const StateLayout layout = BuildLayout(scene, params);
    for (size_t point = layout.rootFixed;
        point < scene.x.size(); ++point)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            metrics.forceNorm = std::max(metrics.forceNorm,
                std::abs(residual[static_cast<Eigen::Index>(
                    layout.PositionIndex(point, axis))]));
        }
    }
    for (size_t segment = 1;
        segment < scene.q.size(); ++segment)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            metrics.torqueNorm = std::max(metrics.torqueNorm,
                std::abs(residual[static_cast<Eigen::Index>(
                    layout.QuaternionIndex(segment, axis))]));
        }
    }
    metrics.torqueNorm /= TorqueEquationWeight;
    for (size_t s = 0; s < rest.lengths.size(); ++s)
    {
        metrics.maxRestLengthChange = std::max(
            metrics.maxRestLengthChange,
            std::abs(rest.lengths[s] /
                scene.restLengths[s] - 1.0));
    }
    for (const Vec& rotation : rest.relativeRotation)
    {
        const double angle = rotation.norm();
        metrics.maxRestAngle = std::max(
            metrics.maxRestAngle, angle);
        metrics.minFlipMargin = std::min(
            metrics.minFlipMargin, Pi - angle);
    }
    for (size_t s = 1; s < scene.q.size(); ++s)
    {
        const Vec delta =
            2.0 * Log(Conjugate(scene.q[s]) * target.q[s]);
        metrics.maxTargetDeviation = std::max(
            metrics.maxTargetDeviation, delta.norm());
    }
    for (size_t s = 0; s < scene.q.size(); ++s)
    {
        const Vec material = ToMatrix(target.q[s]).col(2);
        const Vec tangent =
            (scene.x[s + 1] - scene.x[s]).normalized();
        const double cosine =
            std::clamp(material.dot(tangent), -1.0, 1.0);
        metrics.maxTargetShear = std::max(
            metrics.maxTargetShear, std::acos(cosine));
    }
    ClumpEnergyAndResidual(
        scene, params, AuthoredState(scene),
        &metrics.clumpResidual);
    metrics.stability =
        ComputeStateHessian(scene, params, rest, target);
    return metrics;
}

void PrintRestSummary(const char* label, const Scene& scene,
    const Rest& rest)
{
    double maxLength = 0.0;
    double maxAngle = 0.0;
    double minFlip = Pi;
    for (size_t s = 0; s < rest.lengths.size(); ++s)
    {
        maxLength = std::max(maxLength,
            std::abs(rest.lengths[s] /
                scene.restLengths[s] - 1.0));
    }
    for (const Vec& rotation : rest.relativeRotation)
    {
        const double angle = rotation.norm();
        maxAngle = std::max(maxAngle, angle);
        minFlip = std::min(minFlip, Pi - angle);
    }
    std::printf(
        "%s: max |dL|/L=%.6e max rest angle=%.6e rad "
        "min flip margin=%.6e rad\n",
        label, maxLength, maxAngle, minFlip);
}

void PrintTargetSummary(const char* label, const Scene& scene,
    const State& target)
{
    double maxDeviation = 0.0;
    double maxShear = 0.0;
    for (size_t s = 1; s < scene.q.size(); ++s)
    {
        const Vec delta =
            2.0 * Log(Conjugate(scene.q[s]) * target.q[s]);
        maxDeviation = std::max(maxDeviation, delta.norm());
    }
    for (size_t s = 0; s < scene.q.size(); ++s)
    {
        const Vec material = ToMatrix(target.q[s]).col(2);
        const Vec tangent =
            (scene.x[s + 1] - scene.x[s]).normalized();
        const double cosine =
            std::clamp(material.dot(tangent), -1.0, 1.0);
        maxShear = std::max(maxShear, std::acos(cosine));
    }
    std::printf(
        "%s: max target frame deviation=%.6e rad "
        "max target shear=%.6e rad\n",
        label, maxDeviation, maxShear);
}

void PrintRank(const Scene& scene, const Params& params,
    const Rest& rest, const State& target)
{
    const Eigen::MatrixXd jacobian =
        InverseJacobian(scene, params, rest, target);
    const Eigen::Index primaryRows = BuildLayout(scene, params).Dof();
    const Eigen::MatrixXd primary = jacobian.topRows(primaryRows);
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(jacobian,
        Eigen::ComputeThinU | Eigen::ComputeFullV);
    const Eigen::VectorXd singular = svd.singularValues();
    const double sigmaMax = singular.maxCoeff();
    const double sigmaMin = singular.minCoeff();
    const double tolerance =
        static_cast<double>(
            std::max(jacobian.rows(), jacobian.cols())) *
        std::numeric_limits<double>::epsilon() *
        std::max(1.0, sigmaMax) * 100.0;
    int rank = 0;
    for (Eigen::Index i = 0; i < singular.size(); ++i)
    {
        if (singular[i] > tolerance)
            ++rank;
    }
    std::printf(
        "augmented Jacobian: rank %d/%lld (residual %lld x vars %lld), "
        "sigma [%.3e, %.3e], cond %.3e\n",
        rank, static_cast<long long>(jacobian.cols()),
        static_cast<long long>(jacobian.rows()),
        static_cast<long long>(jacobian.cols()),
        sigmaMin, sigmaMax,
        sigmaMin > 0.0 ? sigmaMax / sigmaMin
                       : std::numeric_limits<double>::infinity());
    std::printf("  smallest singular values:");
    const Eigen::Index first = std::max<Eigen::Index>(
        0, singular.size() - 5);
    for (Eigen::Index i = first; i < singular.size(); ++i)
        std::printf(" %.3e", singular[i]);
    std::printf("\n");
    Eigen::JacobiSVD<Eigen::MatrixXd> primarySvd(primary,
        Eigen::ComputeThinU | Eigen::ComputeFullV);
    const Eigen::VectorXd primarySingular =
        primarySvd.singularValues();
    const double primaryMax = primarySingular.maxCoeff();
    const double primaryMin = primarySingular.minCoeff();
    const double primaryTolerance =
        static_cast<double>(
            std::max(primary.rows(), primary.cols())) *
        std::numeric_limits<double>::epsilon() *
        std::max(1.0, primaryMax) * 100.0;
    int primaryRank = 0;
    for (Eigen::Index i = 0;
        i < primarySingular.size(); ++i)
    {
        if (primarySingular[i] > primaryTolerance)
            ++primaryRank;
    }
    std::printf(
        "primary force/torque Jacobian: rank %d/%lld "
        "(residual %lld x vars %lld), sigma [%.3e, %.3e], "
        "cond %.3e\n",
        primaryRank, static_cast<long long>(primary.cols()),
        static_cast<long long>(primary.rows()),
        static_cast<long long>(primary.cols()),
        primaryMin, primaryMax,
        primaryMin > 0.0 ? primaryMax / primaryMin
                         : std::numeric_limits<double>::infinity());
}

void PrintStability(const char* label,
    const StabilityResult& stability)
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
}

void PrintEnergy(const EnergyBreakdown& energy)
{
    std::printf(
        "lowest-mode energy: total %.6e = stretch %.6e + "
        "shear %.6e + bend %.6e + twist %.6e + gravity %.6e + "
        "clump %.6e + tether %.6e\n",
        energy.Total(), energy.stretch, energy.shear,
        energy.bend, energy.twist, energy.gravity, energy.clump,
        energy.tether);
    const double magnitude =
        std::max(std::abs(energy.Total()), 1.0e-30);
    std::printf(
        "  signed share: stretch %+.1f%%, shear %+.1f%%, "
        "bend %+.1f%%, twist %+.1f%%, gravity %+.1f%%, "
        "clump %+.1f%%, tether %+.1f%%\n",
        100.0 * energy.stretch / magnitude,
        100.0 * energy.shear / magnitude,
        100.0 * energy.bend / magnitude,
        100.0 * energy.twist / magnitude,
        100.0 * energy.gravity / magnitude,
        100.0 * energy.clump / magnitude,
        100.0 * energy.tether / magnitude);
}

void PrintSimMetrics(const char* label, const SimMetrics& metrics)
{
    std::printf(
        "%s: final max %.6e L, final tip %.6e L, "
        "trajectory max %.6e L, max stretch %.6e, "
        "max clump residual %.6e, max tether residual %.6e, %s\n",
        label, metrics.finalMaxDeviation,
        metrics.finalTipDeviation,
        metrics.trajectoryMaxDeviation,
        metrics.maxSegmentStretch,
        metrics.maxClumpResidual,
        metrics.maxTetherResidual,
        metrics.finite ? "finite" : "NON-FINITE");
    if (!metrics.finalDeviations.empty())
    {
        std::vector<std::pair<double, size_t>> deviations;
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

struct ShapeMetrics
{
    double tip = 0.0;
    double max = 0.0;
    double centroid = 0.0;
    double heightRatio = 1.0;
    double rms = 0.0;
};

ShapeMetrics MeasureShape(const Scene& scene, const State& state)
{
    ShapeMetrics metrics;
    const double totalLength = scene.totalLength;
    metrics.tip =
        (state.x.back() - scene.x.back()).norm() / totalLength;
    double maxDistance = 0.0;
    double rms = 0.0;
    Vec settledCentroid = Vec::Zero();
    Vec authoredCentroid = Vec::Zero();
    const size_t movable = scene.x.size() -
        static_cast<size_t>(2);
    for (size_t point = 0; point < scene.x.size(); ++point)
    {
        const double distance =
            (state.x[point] - scene.x[point]).norm();
        maxDistance = std::max(maxDistance, distance);
        if (point >= 2)
        {
            rms += distance * distance;
            settledCentroid += state.x[point];
            authoredCentroid += scene.x[point];
        }
    }
    metrics.max = maxDistance / totalLength;
    metrics.rms = std::sqrt(
        rms / static_cast<double>(std::max<size_t>(1, movable))) /
        totalLength;
    metrics.centroid =
        (settledCentroid - authoredCentroid).norm() /
        static_cast<double>(std::max<size_t>(1, movable)) /
        totalLength;
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    double minAuthoredY = std::numeric_limits<double>::infinity();
    double maxAuthoredY = -std::numeric_limits<double>::infinity();
    for (size_t point = 0; point < scene.x.size(); ++point)
    {
        minY = std::min(minY, state.x[point].y());
        maxY = std::max(maxY, state.x[point].y());
        minAuthoredY = std::min(minAuthoredY, scene.x[point].y());
        maxAuthoredY = std::max(maxAuthoredY, scene.x[point].y());
    }
    const double authoredHeight = maxAuthoredY - minAuthoredY;
    metrics.heightRatio = authoredHeight > 1.0e-12
        ? (maxY - minY) / authoredHeight : 1.0;
    return metrics;
}

double MaxTetherResidual(const Scene& scene, const Params& params,
    const State& state)
{
    double residual = 0.0;
    for (size_t point = static_cast<size_t>(params.rootFixed);
        point < scene.x.size(); ++point)
    {
        const bool tethered =
            params.globalTetherStiffness > 0.0 ||
            params.tetherProgressive > 0.0 ||
            (params.localTetherStride > 0 &&
                (point - static_cast<size_t>(params.rootFixed)) %
                    static_cast<size_t>(
                        params.localTetherStride) == 0);
        if (tethered)
        {
            residual = std::max(residual,
                std::max(0.0,
                    (state.x[point] - scene.x[point]).norm() -
                        params.tetherDeadZone));
        }
    }
    return residual;
}

State SettleState(const Scene& scene, const Params& params,
    const Rest& rest, const State& initial, int steps)
{
    State state = initial;
    state.velocity.assign(scene.x.size(), Vec::Zero());
    state.angularVelocity.assign(scene.q.size(), Vec::Zero());
    for (int step = 0; step < steps; ++step)
    {
        if (!AdvanceOneStep(
            scene, params, rest, state, Vec::Zero()))
        {
            break;
        }
    }
    return state;
}

struct WindMetrics
{
    ShapeMetrics staticShape;
    double staticVelocity = 0.0;
    double staticStretch = 0.0;
    double staticTetherResidual = 0.0;
    double staticClumpResidual = 0.0;
    double maxDynamicOffset = 0.0;
    double peakTime = -1.0;
    double peakCentroidDirectionZ = 0.0;
    double maxAngularSpeed = 0.0;
    double recoveryTime = -1.0;
    double residualOffset = 0.0;
    double maxStretch = 0.0;
    double maxTetherResidual = 0.0;
    double maxClumpResidual = 0.0;
    bool visible = false;
    bool finite = true;
};

WindMetrics SimulateWind(const Scene& scene, const Params& params,
    const Rest& rest, const State& settled, int windSteps,
    int recoverySteps, double windAcceleration)
{
    WindMetrics metrics;
    metrics.staticShape = MeasureShape(scene, settled);
    for (size_t point = static_cast<size_t>(params.rootFixed);
        point < settled.x.size(); ++point)
    {
        metrics.staticVelocity = std::max(
            metrics.staticVelocity,
            settled.velocity[point].norm());
    }
    for (size_t s = 0; s < scene.restLengths.size(); ++s)
    {
        const double length =
            (settled.x[s + 1] - settled.x[s]).norm();
        metrics.staticStretch = std::max(
            metrics.staticStretch,
            std::abs(length / rest.lengths[s] - 1.0));
    }
    metrics.staticTetherResidual =
        MaxTetherResidual(scene, params, settled);
    double staticClump = 0.0;
    ClumpEnergyAndResidual(
        scene, params, settled, &staticClump);
    metrics.staticClumpResidual = staticClump;

    State state = settled;
    state.velocity.assign(scene.x.size(), Vec::Zero());
    state.angularVelocity.assign(scene.q.size(), Vec::Zero());
    const std::vector<Vec> base = settled.x;
    const double totalLength = scene.totalLength;
    const auto centroidDirectionZ = [&]()
    {
        Vec centroid = Vec::Zero();
        for (size_t point = 0; point < base.size(); ++point)
            centroid += state.x[point] - base[point];
        centroid /= static_cast<double>(base.size());
        const double norm = centroid.norm();
        return norm > 1.0e-12 ? centroid.z() / norm : 0.0;
    };
    const auto offset = [&]()
    {
        double value = 0.0;
        for (size_t point = 0; point < base.size(); ++point)
        {
            value = std::max(value,
                (state.x[point] - base[point]).norm());
        }
        return value / totalLength;
    };
    const auto track = [&]()
    {
        for (const Vec& point : state.x)
        {
            if (!point.allFinite())
                metrics.finite = false;
        }
        for (size_t s = 0; s < scene.restLengths.size(); ++s)
        {
            const double length =
                (state.x[s + 1] - state.x[s]).norm();
            metrics.maxStretch = std::max(
                metrics.maxStretch,
                std::abs(length / rest.lengths[s] - 1.0));
        }
        metrics.maxTetherResidual = std::max(
            metrics.maxTetherResidual,
            MaxTetherResidual(scene, params, state));
        for (size_t segment = 1;
            segment < state.angularVelocity.size(); ++segment)
        {
            metrics.maxAngularSpeed = std::max(
                metrics.maxAngularSpeed,
                state.angularVelocity[segment].norm());
        }
        double clump = 0.0;
        ClumpEnergyAndResidual(
            scene, params, state, &clump);
        metrics.maxClumpResidual = std::max(
            metrics.maxClumpResidual, clump);
    };

    const Vec wind(0.0, 0.0, windAcceleration);
    double peak = 0.0;
    for (int step = 0; step < windSteps; ++step)
    {
        if (!AdvanceOneStep(
            scene, params, rest, state, wind))
        {
            metrics.finite = false;
            break;
        }
        const double value = offset();
        if (value > peak)
        {
            peak = value;
            metrics.peakTime =
                static_cast<double>(step + 1) * params.dt;
            metrics.peakCentroidDirectionZ =
                centroidDirectionZ();
        }
        metrics.maxDynamicOffset =
            std::max(metrics.maxDynamicOffset, value);
        track();
    }
    if (metrics.finite)
    {
        for (int step = 0; step < recoverySteps; ++step)
        {
            if (!AdvanceOneStep(
                scene, params, rest, state, Vec::Zero()))
            {
                metrics.finite = false;
                break;
            }
            const double value = offset();
            metrics.maxDynamicOffset =
                std::max(metrics.maxDynamicOffset, value);
            if (metrics.recoveryTime < 0.0 &&
                peak > 1.0e-12 &&
                value < 0.1 * peak)
            {
                metrics.recoveryTime =
                    static_cast<double>(step + 1) * params.dt;
            }
            track();
        }
    }
    metrics.residualOffset = offset();
    metrics.visible = metrics.maxDynamicOffset > 0.005;
    if (peak <= 1.0e-12)
        metrics.recoveryTime = 0.0;
    return metrics;
}

int RunM0_007(const std::string& sceneName, int points,
    int settleSteps, int attempts, double clumpStiffness,
    int clumpGroupSize, bool clumpWholeGuide)
{
    Params params;
    params.clumpStiffness = clumpStiffness;
    params.clumpGroupSize = clumpGroupSize;
    params.clumpWholeGuide = clumpWholeGuide;
    const Scene scene = BuildScene(sceneName, points);
    if (scene.x.empty())
    {
        std::fprintf(stderr, "unknown scene: %s\n",
            sceneName.c_str());
        return 2;
    }
    const Rest authored = AuthoredRest(scene);
    std::printf(
        "=== %s scene=%s points=%d rootFixed=%d "
        "segments=%zu settle=%d attempts=%d ===\n",
        params.clumpStiffness > 0.0 ? "M0-008" : "M0-007",
        sceneName.c_str(), points, params.rootFixed,
        scene.q.size(), settleSteps, attempts);
    std::printf(
        "parameters: EA=%.6e EI=%.6e GJ=%.6e GA=%.6e "
        "g=%.6f dt=%.6e substeps=%d damping=%.6e\n",
        params.EA, params.EI, params.GJ, params.GA,
        params.gravity, params.dt, params.substeps,
        params.damping);
    std::printf(
        "model: segment-center q_s; c_s=(x_s+x_{s+1})/2; "
        "C_ss uses diag(1/(GA L),1/(GA L),1/(EA L)); "
        "joint s uses rest segments s and s+1, "
        "omega=Log(q_s^-1 q_{s+1})/Lbar_rest, "
        "Lbar_rest=(L_s^0+L_{s+1}^0)/2\n");
    std::printf(
        "log convention: r=q_s^-1 q_{s+1}; if r.w<0 r=-r; "
        "theta=2 atan2(|r.v|,r.w); velocity recovery uses the same log\n");
    std::printf(
        "rest variables: %zu lengths + %zu joints * 3 rest "
        "rotation-vector components; rest quaternions are unit; "
        "|v|<pi-%.3f\n",
        scene.restLengths.size(), scene.q.size() - 1,
        params.flipMargin);
    if (params.clumpStiffness > 0.0)
    {
        std::printf(
            "clump: stiffness=%.6e, groupSize=%d, wholeGuide=%s, "
            "uniform centroid weights, rest d_i=|x_i^auth-c^auth|, "
            "no new rest variables\n",
            params.clumpStiffness, params.clumpGroupSize,
            params.clumpWholeGuide ? "yes" : "no");
    }
    PrintRestSummary("authored rest", scene, authored);

    Rest bestRest = authored;
    const State authoredState = AuthoredState(scene);
    State bestTarget = authoredState;
    StaticMetrics bestMetrics;
    SimMetrics bestSim;
    bool bestHasMetrics = false;
    bool bestHasSim = false;
    double bestScore = std::numeric_limits<double>::infinity();
    bool found = false;

    std::mt19937 generator(
        20261114u + static_cast<unsigned>(points) +
        static_cast<unsigned>(sceneName.size()));
    std::uniform_real_distribution<double> uniform(-1.0, 1.0);
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        Rest initialRest = authored;
        State initialTarget = authoredState;
        if (attempt > 0)
        {
            for (size_t s = 0; s < initialRest.lengths.size(); ++s)
            {
                initialRest.lengths[s] *=
                    1.0 + 0.02 * uniform(generator);
            }
            for (size_t s = 0;
                s < initialRest.relativeRotation.size(); ++s)
            {
                Vec& rotation = initialRest.relativeRotation[s];
                rotation += Vec(
                    0.2 * uniform(generator),
                    0.2 * uniform(generator),
                    0.2 * uniform(generator));
                const double angle = rotation.norm();
                if (angle > Pi - params.flipMargin)
                {
                    rotation *= (Pi - params.flipMargin) / angle;
                }
            }
            for (size_t s = 1; s < initialTarget.q.size(); ++s)
            {
                Vec delta(
                    0.2 * uniform(generator),
                    0.2 * uniform(generator),
                    0.2 * uniform(generator));
                const double angle = delta.norm();
                if (angle > TargetDeviationBound)
                    delta *= TargetDeviationBound / angle;
                initialTarget.q[s] = Normalize(
                    scene.q[s] * Exp(0.5 * delta));
            }
        }
        const SolveReport solve = SolveBlockInverse(
            scene, params, initialRest, initialTarget, 40, 1.0e-6);
        std::printf(
            "attempt %2d: force %.3e torque %.3e "
            "residual %.3e iter %d %s\n",
            attempt, solve.forceNorm, solve.torqueNorm,
            solve.residualNorm, solve.iterations,
            solve.converged ? "converged" : "not-converged");
        if (!solve.finite)
            continue;
        if (solve.forceNorm > 1.0e-3)
        {
            continue;
        }
        const StaticMetrics metrics =
            EvaluateStatic(scene, params, solve.rest, solve.target);
        std::printf(
            "  static: force %.3e torque %.3e lambda_min %.6e "
            "max|dL| %.3e max angle %.3e flip %.3e "
            "target dev %.3e shear %.3e clump %.3e\n",
            metrics.forceNorm, metrics.torqueNorm,
            metrics.stability.minEigenvalue,
            metrics.maxRestLengthChange,
            metrics.maxRestAngle, metrics.minFlipMargin,
            metrics.maxTargetDeviation, metrics.maxTargetShear,
            metrics.clumpResidual);
        const bool staticPass =
            metrics.forceNorm < 1.0e-5 &&
            metrics.torqueNorm < 1.0e-5 &&
            metrics.stability.finite &&
            metrics.stability.minEigenvalue > 0.0 &&
            metrics.maxRestLengthChange < 0.05 &&
            metrics.minFlipMargin > 0.10 &&
            metrics.maxTargetShear < 0.20;
        SimMetrics simulation;
        bool simulationPass = false;
        if (staticPass)
        {
            simulation = Simulate(
                scene, params, solve.rest, solve.target, settleSteps);
            simulationPass =
                simulation.finite &&
                simulation.finalMaxDeviation < 0.05 &&
                simulation.finalTipDeviation < 0.01 &&
                simulation.maxSegmentStretch < 0.05;
            PrintSimMetrics("  dynamics", simulation);
        }
        const double score =
            std::max(metrics.forceNorm, metrics.torqueNorm) +
            1.0e-8 * std::max(
                0.0, -metrics.stability.minEigenvalue);
        if (score < bestScore)
        {
            bestScore = score;
            bestRest = solve.rest;
            bestTarget = solve.target;
            bestMetrics = metrics;
            bestHasMetrics = true;
            if (staticPass)
            {
                bestSim = simulation;
                bestHasSim = true;
            }
        }
        if (staticPass && simulationPass)
        {
            bestRest = solve.rest;
            bestTarget = solve.target;
            bestMetrics = metrics;
            bestSim = simulation;
            bestHasMetrics = true;
            bestHasSim = true;
            found = true;
            std::printf("  physical stable candidate found\n");
            break;
        }
    }

    if (!bestHasMetrics)
    {
        std::printf(
            "no candidate reached the static evaluation gate\n");
        std::printf("verdict: FAIL (no candidate)\n");
        return 1;
    }

    std::printf("best candidate:\n");
    PrintRestSummary("  rest", scene, bestRest);
    PrintTargetSummary("  target", scene, bestTarget);
    std::printf(
        "  force residual %.6e, torque residual %.6e\n",
        bestMetrics.forceNorm, bestMetrics.torqueNorm);
    PrintRank(scene, params, bestRest, bestTarget);
    PrintStability("  reduced Hessian", bestMetrics.stability);
    if (bestMetrics.stability.finite)
    {
        const EnergyBreakdown quadratic = StateQuadraticForm(
            scene, params, bestRest, bestTarget,
            bestMetrics.stability.minEigenvector);
        PrintEnergy(quadratic);
    }
    if (!bestHasSim)
    {
        bestSim = Simulate(
            scene, params, bestRest, bestTarget, settleSteps);
    }
    PrintSimMetrics("  200-step", bestSim);

    const bool forcePass = bestMetrics.forceNorm < 1.0e-5;
    const bool torquePass = bestMetrics.torqueNorm < 1.0e-5;
    const bool hessianPass =
        bestMetrics.stability.finite &&
        bestMetrics.stability.minEigenvalue > 0.0;
    const bool deviationPass =
        bestSim.finalMaxDeviation < 0.05;
    const bool tipPass = bestSim.finalTipDeviation < 0.01;
    const bool stretchPass = bestSim.maxSegmentStretch < 0.05;
    const bool restLengthPass =
        bestMetrics.maxRestLengthChange < 0.05;
    const bool flipPass = bestMetrics.minFlipMargin > 0.10;
    const bool targetShearPass =
        bestMetrics.maxTargetShear < 0.20;
    const bool clumpPass =
        bestSim.maxClumpResidual / scene.totalLength < 0.01;
    const bool finitePass =
        bestSim.finite && bestMetrics.stability.finite;
    const bool pass = forcePass && torquePass && hessianPass &&
        deviationPass && tipPass && stretchPass &&
        restLengthPass && flipPass && targetShearPass &&
        clumpPass && finitePass;
    std::printf(
        "thresholds: force %s, torque %s, Hessian %s, "
        "200-step max %s, tip %s, stretch %s, rest length %s, "
        "flip margin %s, target shear %s, clump %s, finite %s\n",
        forcePass ? "PASS" : "FAIL",
        torquePass ? "PASS" : "FAIL",
        hessianPass ? "PASS" : "FAIL",
        deviationPass ? "PASS" : "FAIL",
        tipPass ? "PASS" : "FAIL",
        stretchPass ? "PASS" : "FAIL",
        restLengthPass ? "PASS" : "FAIL",
        flipPass ? "PASS" : "FAIL",
        targetShearPass ? "PASS" : "FAIL",
        clumpPass ? "PASS" : "FAIL",
        finitePass ? "PASS" : "FAIL");
    std::printf("verdict: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int RunCrossGuideClump(const std::string& sceneName, int points,
    int attempts, int guideCount, double spacing, double maxStiffness)
{
    Params params;
    const Scene scene = BuildScene(sceneName, points);
    if (scene.x.empty())
    {
        std::fprintf(stderr, "unknown scene: %s\n",
            sceneName.c_str());
        return 2;
    }
    const Rest authored = AuthoredRest(scene);
    const State authoredState = AuthoredState(scene);
    const SolveReport solve = SolveBlockInverse(
        scene, params, authored, authoredState, 40, 1.0e-6);
    const Rest rest = solve.rest;
    const State target = solve.target;
    const StabilityResult singleH =
        ComputeStateHessian(scene, params, rest, target);
    const Eigen::MatrixXd h0 =
        ComputeStateHessianMatrix(scene, params, rest, target);

    const size_t nodeCount = scene.x.size();
    const StateLayout layout = BuildLayout(scene, params);
    const size_t positionCount = layout.positionCount;
    const size_t quaternionCount = layout.quaternionCount;
    const size_t stride = positionCount + quaternionCount;
    const size_t totalDof =
        static_cast<size_t>(guideCount) * stride;
    std::printf(
        "=== M0-008 cross-guide clump scene=%s points=%d "
        "guides=%d spacing=%.3f ===\n",
        sceneName.c_str(), points, guideCount, spacing);
    std::printf(
        "single-guide solve: force %.3e torque %.3e "
        "lambda_min %.6e (torque block unchanged by clump)\n",
        solve.forceNorm, solve.torqueNorm, singleH.minEigenvalue);

    std::vector<std::vector<Vec>> authoredPositions(
        static_cast<size_t>(guideCount),
        std::vector<Vec>(nodeCount));
    std::vector<std::vector<Vec>> currentPositions(
        static_cast<size_t>(guideCount),
        std::vector<Vec>(nodeCount));
    for (int guide = 0; guide < guideCount; ++guide)
    {
        const double offset = spacing *
            (static_cast<double>(guide) -
                0.5 * static_cast<double>(guideCount - 1));
        for (size_t point = 0; point < nodeCount; ++point)
        {
            const Vec shift(0.0, 0.0, offset);
            authoredPositions[static_cast<size_t>(guide)][point] =
                scene.x[point] + shift;
            currentPositions[static_cast<size_t>(guide)][point] =
                target.x[point] + shift;
        }
    }

    const auto crossClumpEnergy =
        [&](const std::vector<std::vector<Vec>>& positions,
            double stiffness)
    {
        double energy = 0.0;
        for (size_t point = 0; point < nodeCount; ++point)
        {
            Vec authoredCentroid = Vec::Zero();
            Vec currentCentroid = Vec::Zero();
            for (int guide = 0; guide < guideCount; ++guide)
            {
                authoredCentroid +=
                    authoredPositions[static_cast<size_t>(guide)][point];
                currentCentroid +=
                    positions[static_cast<size_t>(guide)][point];
            }
            authoredCentroid /= static_cast<double>(guideCount);
            currentCentroid /= static_cast<double>(guideCount);
            for (int guide = 0; guide < guideCount; ++guide)
            {
                const double restDistance =
                    (authoredPositions[static_cast<size_t>(guide)][point] -
                        authoredCentroid).norm();
                const double C =
                    (positions[static_cast<size_t>(guide)][point] -
                        currentCentroid).norm() -
                    restDistance;
                energy += 0.5 * stiffness * C * C;
            }
        }
        return energy;
    };

    const double step = 1.0e-5 * scene.totalLength /
        static_cast<double>(scene.restLengths.size());
    const auto clumpGradient =
        [&](const std::vector<std::vector<Vec>>& positions,
            Eigen::VectorXd& gradient)
    {
        gradient = Eigen::VectorXd::Zero(
            static_cast<Eigen::Index>(guideCount) *
            static_cast<Eigen::Index>(positionCount));
        for (int guide = 0; guide < guideCount; ++guide)
        {
            for (size_t point = layout.rootFixed;
                point < nodeCount; ++point)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    const size_t local =
                        static_cast<size_t>(guide) * positionCount +
                        layout.PositionIndex(point, axis);
                    auto plus = positions;
                    auto minus = positions;
                    plus[static_cast<size_t>(guide)][point][axis] += step;
                    minus[static_cast<size_t>(guide)][point][axis] -= step;
                    gradient[static_cast<Eigen::Index>(local)] =
                        (crossClumpEnergy(plus, 1.0) -
                            crossClumpEnergy(minus, 1.0)) /
                        (2.0 * step);
                }
            }
        }
    };

    Eigen::MatrixXd crossHessian =
        Eigen::MatrixXd::Zero(
            static_cast<Eigen::Index>(totalDof),
            static_cast<Eigen::Index>(totalDof));
    for (int guide = 0; guide < guideCount; ++guide)
    {
        for (size_t point = layout.rootFixed;
            point < nodeCount; ++point)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                const size_t local = layout.PositionIndex(point, axis);
                auto plus = currentPositions;
                auto minus = currentPositions;
                plus[static_cast<size_t>(guide)][point][axis] += step;
                minus[static_cast<size_t>(guide)][point][axis] -= step;
                Eigen::VectorXd plusGradient;
                Eigen::VectorXd minusGradient;
                clumpGradient(plus, plusGradient);
                clumpGradient(minus, minusGradient);
                const Eigen::VectorXd column =
                    (plusGradient - minusGradient) / (2.0 * step);
                const size_t columnIndex =
                    static_cast<size_t>(guide) * stride + local;
                for (int rowGuide = 0;
                    rowGuide < guideCount; ++rowGuide)
                {
                    for (size_t rowLocal = 0;
                        rowLocal < positionCount; ++rowLocal)
                    {
                        const size_t rowIndex =
                            static_cast<size_t>(rowGuide) * stride +
                            rowLocal;
                        crossHessian(
                            static_cast<Eigen::Index>(rowIndex),
                            static_cast<Eigen::Index>(columnIndex)) =
                            column[static_cast<Eigen::Index>(
                                static_cast<size_t>(rowGuide) *
                                    positionCount +
                                rowLocal)];
                    }
                }
            }
        }
    }
    crossHessian =
        0.5 * (crossHessian + crossHessian.transpose());

    Eigen::MatrixXd baseHessian =
        Eigen::MatrixXd::Zero(
            static_cast<Eigen::Index>(totalDof),
            static_cast<Eigen::Index>(totalDof));
    for (int guide = 0; guide < guideCount; ++guide)
    {
        baseHessian.block(
            static_cast<Eigen::Index>(guide * stride),
            static_cast<Eigen::Index>(guide * stride),
            h0.rows(), h0.cols()) = h0;
    }

    const double stiffnessValues[] = {
        1.0e1, 1.0e2, 1.0e3, 1.0e4,
        1.0e5, 1.0e6, 1.0e7, 1.0e8
    };
    double stableStiffness = -1.0;
    double stableLambda = 0.0;
    for (double stiffness : stiffnessValues)
    {
        if (stiffness > maxStiffness)
            break;
        const Eigen::MatrixXd hessian =
            baseHessian + stiffness * crossHessian;
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(hessian);
        if (solver.info() != Eigen::Success)
            continue;
        const double lambdaMin = solver.eigenvalues().minCoeff();
        const double lambdaMax = solver.eigenvalues().maxCoeff();
        std::printf(
            "  k=%.3e lambda_min=%.6e lambda_max=%.6e\n",
            stiffness, lambdaMin, lambdaMax);
        if (stableStiffness < 0.0 && lambdaMin > 0.0)
        {
            stableStiffness = stiffness;
            stableLambda = lambdaMin;
        }
    }
    if (stableStiffness < 0.0)
    {
        std::printf(
            "  no stable k up to %.3e; cross-guide clump insufficient\n",
            maxStiffness);
    }
    else
    {
        const Eigen::MatrixXd hessian =
            baseHessian + stableStiffness * crossHessian;
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(hessian);
        const Eigen::VectorXd direction = solver.eigenvectors().col(0);
        const double energyStep = 1.0e-4;
        auto perturb = [&](double scale,
            std::vector<std::vector<Vec>>& result)
        {
            result = currentPositions;
            for (int guide = 0; guide < guideCount; ++guide)
            {
                for (size_t point = layout.rootFixed;
                    point < nodeCount; ++point)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        result[static_cast<size_t>(guide)][point][axis] +=
                            scale * direction[
                                static_cast<Eigen::Index>(
                                    static_cast<size_t>(guide) * stride +
                                    layout.PositionIndex(point, axis))];
                    }
                }
            }
        };
        std::vector<std::vector<Vec>> plus;
        std::vector<std::vector<Vec>> minus;
        perturb(energyStep, plus);
        perturb(-energyStep, minus);
        const double base = crossClumpEnergy(currentPositions, 1.0);
        const double positive = crossClumpEnergy(plus, 1.0);
        const double negative = crossClumpEnergy(minus, 1.0);
        const double quadratic =
            (positive - 2.0 * base + negative) /
            (energyStep * energyStep);
        const double clumpShare = stableStiffness * quadratic;
        std::printf(
            "  minimum stable cross-clump k=%.3e lambda_min=%.6e; "
            "clump share %.3e (%.1f%% of lambda_min)\n",
            stableStiffness, stableLambda, clumpShare,
            100.0 * clumpShare / std::max(stableLambda, 1.0e-30));
        std::printf(
            "  cost: %d scalar constraints (M*N), %zu Hessian DOF, "
            "2*M*P gradient evaluations for Hc\n",
            guideCount * static_cast<int>(nodeCount), totalDof,
            2 * guideCount * static_cast<int>(positionCount));
    }
    std::printf(
        "  note: cross-guide clump couples node positions only; "
        "single-guide torque residual %.3e remains, so a positive "
        "Hessian alone is not a full M0-008 PASS\n",
        solve.torqueNorm);
    return stableStiffness > 0.0 ? 0 : 1;
}

int RunClumpProjection(const std::string& sceneName, int points,
    int attempts)
{
    Params params;
    const Scene scene = BuildScene(sceneName, points);
    if (scene.x.empty())
    {
        std::fprintf(stderr, "unknown scene: %s\n",
            sceneName.c_str());
        return 2;
    }
    const Rest authored = AuthoredRest(scene);
    const State authoredState = AuthoredState(scene);
    const SolveReport solve = SolveBlockInverse(
        scene, params, authored, authoredState, 40, 1.0e-6);
    const Rest rest = solve.rest;
    const State target = solve.target;
    const StabilityResult stability =
        ComputeStateHessian(scene, params, rest, target);
    const StateLayout layout = BuildLayout(scene, params);
    const Eigen::VectorXd direction = stability.minEigenvector;
    const double lambda0 = stability.minEigenvalue;
    double positionEnergy = 0.0;
    double quaternionEnergy = 0.0;
    for (size_t point = layout.rootFixed;
        point < scene.x.size(); ++point)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const double value = direction[
                static_cast<Eigen::Index>(
                    layout.PositionIndex(point, axis))];
            positionEnergy += value * value;
        }
    }
    for (size_t segment = 1; segment < scene.q.size(); ++segment)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const double value = direction[
                static_cast<Eigen::Index>(
                    layout.QuaternionIndex(segment, axis))];
            quaternionEnergy += value * value;
        }
    }
    std::printf(
        "=== M0-008 clump projection scene=%s points=%d ===\n",
        sceneName.c_str(), points);
    std::printf(
        "lambda0=%.6e, negative mode position %.6e + quaternion %.6e; "
        "force %.3e torque %.3e\n",
        lambda0, positionEnergy, quaternionEnergy,
        solve.forceNorm, solve.torqueNorm);
    const double step = 1.0e-5 * scene.totalLength /
        static_cast<double>(scene.restLengths.size());
    const double practicalStiffness = 1.0e6;
    const auto reportProjection = [&](const char* label,
        int rank, size_t nullity, double projection)
    {
        const double required = (lambda0 < 0.0 &&
            projection > 1.0e-16)
            ? -lambda0 / projection
            : std::numeric_limits<double>::infinity();
        std::printf(
            "  %-18s rank %d, nullity %zu, |Jc v|^2 %.6e, "
            "k_req %.6e (%s), k=1e6 share %.6e\n",
            label, rank, nullity, projection, required,
            !std::isfinite(required)
                ? "NULLSPACE"
                : (required <= practicalStiffness
                    ? "finite/practical"
                    : "exceeds practical"),
            std::isfinite(required)
                ? 1.0e6 * projection
                : 0.0);
        return required;
    };

    const auto singleResiduals = [&](const State& state,
        const std::vector<std::vector<size_t>>& groups)
    {
        std::vector<double> residuals;
        for (const std::vector<size_t>& group : groups)
        {
            Vec authoredCentroid = Vec::Zero();
            Vec currentCentroid = Vec::Zero();
            for (size_t point : group)
            {
                authoredCentroid += scene.x[point];
                currentCentroid += state.x[point];
            }
            authoredCentroid /= static_cast<double>(group.size());
            currentCentroid /= static_cast<double>(group.size());
            for (size_t point : group)
            {
                const double restDistance =
                    (scene.x[point] - authoredCentroid).norm();
                residuals.push_back(
                    (state.x[point] - currentCentroid).norm() -
                    restDistance);
            }
        }
        return residuals;
    };

    struct GroupConfig
    {
        int groupSize;
        bool whole;
        const char* name;
    };
    const GroupConfig configs[] = {
        {4, false, "single-group4"},
        {8, false, "single-group8"},
        {0, true, "single-whole"}
    };
    for (const GroupConfig& config : configs)
    {
        Params clumpParams = params;
        clumpParams.clumpStiffness = 1.0;
        clumpParams.clumpGroupSize = config.groupSize;
        clumpParams.clumpWholeGuide = config.whole;
        const auto groups = BuildClumpGroups(scene, clumpParams);
        const std::vector<double> base =
            singleResiduals(target, groups);
        State plus;
        State minus;
        ApplyDelta(scene, params, target, direction, step, plus);
        ApplyDelta(scene, params, target, direction, -step, minus);
        const std::vector<double> positive =
            singleResiduals(plus, groups);
        const std::vector<double> negative =
            singleResiduals(minus, groups);
        double projection = 0.0;
        for (size_t i = 0; i < base.size(); ++i)
        {
            const double value =
                (positive[i] - negative[i]) / (2.0 * step);
            projection += value * value;
        }
        Eigen::MatrixXd jacobian(
            static_cast<Eigen::Index>(base.size()),
            static_cast<Eigen::Index>(layout.positionCount));
        for (size_t point = layout.rootFixed;
            point < scene.x.size(); ++point)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                State pointPlus = target;
                State pointMinus = target;
                pointPlus.x[point][axis] += step;
                pointMinus.x[point][axis] -= step;
                const std::vector<double> plusResidual =
                    singleResiduals(pointPlus, groups);
                const std::vector<double> minusResidual =
                    singleResiduals(pointMinus, groups);
                const Eigen::Index column =
                    static_cast<Eigen::Index>(
                        layout.PositionIndex(point, axis));
                for (size_t row = 0; row < base.size(); ++row)
                {
                    jacobian(
                        static_cast<Eigen::Index>(row), column) =
                        (plusResidual[row] - minusResidual[row]) /
                        (2.0 * step);
                }
            }
        }
        const Eigen::JacobiSVD<Eigen::MatrixXd> svd(jacobian);
        const Eigen::VectorXd singular = svd.singularValues();
        const double tolerance =
            static_cast<double>(
                std::max(jacobian.rows(), jacobian.cols())) *
            std::numeric_limits<double>::epsilon() *
            std::max(1.0, singular.maxCoeff()) * 100.0;
        int rank = 0;
        for (Eigen::Index i = 0; i < singular.size(); ++i)
        {
            if (singular[i] > tolerance)
                ++rank;
        }
        const size_t nullity =
            layout.positionCount - static_cast<size_t>(rank);
        reportProjection(config.name, rank, nullity, projection);
    }

    for (int guideCount : {2, 3})
    {
        const size_t nodeCount = scene.x.size();
        std::vector<std::vector<Vec>> authoredPositions(
            static_cast<size_t>(guideCount),
            std::vector<Vec>(nodeCount));
        std::vector<std::vector<Vec>> currentPositions(
            static_cast<size_t>(guideCount),
            std::vector<Vec>(nodeCount));
        const double spacing = 0.3;
        for (int guide = 0; guide < guideCount; ++guide)
        {
            const double offset = spacing *
                (static_cast<double>(guide) -
                    0.5 * static_cast<double>(guideCount - 1));
            for (size_t point = 0; point < nodeCount; ++point)
            {
                const Vec shift(0.0, 0.0, offset);
                authoredPositions[static_cast<size_t>(guide)][point] =
                    scene.x[point] + shift;
                currentPositions[static_cast<size_t>(guide)][point] =
                    target.x[point] + shift;
            }
        }
        const auto crossResiduals =
            [&](const std::vector<std::vector<Vec>>& positions)
        {
            std::vector<double> residuals;
            for (size_t point = 0; point < nodeCount; ++point)
            {
                Vec authoredCentroid = Vec::Zero();
                Vec currentCentroid = Vec::Zero();
                for (int guide = 0; guide < guideCount; ++guide)
                {
                    authoredCentroid += authoredPositions[
                        static_cast<size_t>(guide)][point];
                    currentCentroid += positions[
                        static_cast<size_t>(guide)][point];
                }
                authoredCentroid /= static_cast<double>(guideCount);
                currentCentroid /= static_cast<double>(guideCount);
                for (int guide = 0; guide < guideCount; ++guide)
                {
                    const double restDistance =
                        (authoredPositions[
                            static_cast<size_t>(guide)][point] -
                            authoredCentroid).norm();
                    residuals.push_back(
                        (positions[static_cast<size_t>(guide)][point] -
                            currentCentroid).norm() -
                        restDistance);
                }
            }
            return residuals;
        };
        const std::vector<double> base =
            crossResiduals(currentPositions);
        const size_t columns =
            static_cast<size_t>(guideCount) * layout.positionCount;
        Eigen::MatrixXd jacobian(
            static_cast<Eigen::Index>(base.size()),
            static_cast<Eigen::Index>(columns));
        for (int guide = 0; guide < guideCount; ++guide)
        {
            for (size_t point = layout.rootFixed;
                point < nodeCount; ++point)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    auto plus = currentPositions;
                    auto minus = currentPositions;
                    plus[static_cast<size_t>(guide)][point][axis] += step;
                    minus[static_cast<size_t>(guide)][point][axis] -= step;
                    const std::vector<double> plusResidual =
                        crossResiduals(plus);
                    const std::vector<double> minusResidual =
                        crossResiduals(minus);
                    const Eigen::Index column =
                        static_cast<Eigen::Index>(
                            static_cast<size_t>(guide) *
                                layout.positionCount +
                            layout.PositionIndex(point, axis));
                    for (size_t row = 0; row < base.size(); ++row)
                    {
                        jacobian(
                            static_cast<Eigen::Index>(row),
                            column) =
                            (plusResidual[row] - minusResidual[row]) /
                            (2.0 * step);
                    }
                }
            }
        }
        const Eigen::JacobiSVD<Eigen::MatrixXd> svd(jacobian);
        const Eigen::VectorXd singular = svd.singularValues();
        const double tolerance =
            static_cast<double>(
                std::max(jacobian.rows(), jacobian.cols())) *
            std::numeric_limits<double>::epsilon() *
            std::max(1.0, singular.maxCoeff()) * 100.0;
        int rank = 0;
        for (Eigen::Index i = 0; i < singular.size(); ++i)
        {
            if (singular[i] > tolerance)
                ++rank;
        }
        const size_t nullity =
            columns - static_cast<size_t>(rank);
        const auto modeProjection = [&](bool antisymmetric)
        {
            auto plus = currentPositions;
            auto minus = currentPositions;
            for (int guide = 0; guide < guideCount; ++guide)
            {
                const double sign = antisymmetric
                    ? (guide % 2 == 0 ? 1.0 : -1.0) : 1.0;
                for (size_t point = layout.rootFixed;
                    point < nodeCount; ++point)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const double value = direction[
                            static_cast<Eigen::Index>(
                                layout.PositionIndex(point, axis))];
                        plus[static_cast<size_t>(guide)][point][axis] +=
                            sign * step * value;
                        minus[static_cast<size_t>(guide)][point][axis] -=
                            sign * step * value;
                    }
                }
            }
            const std::vector<double> plusResidual =
                crossResiduals(plus);
            const std::vector<double> minusResidual =
                crossResiduals(minus);
            double projection = 0.0;
            for (size_t i = 0; i < base.size(); ++i)
            {
                const double value =
                    (plusResidual[i] - minusResidual[i]) /
                    (2.0 * step);
                projection += value * value;
            }
            return projection;
        };
        const double syncProjection = modeProjection(false);
        const double antiProjection = modeProjection(true);
        const std::string prefix =
            "cross-" + std::to_string(guideCount);
        {
            const std::string label = prefix + "-sync";
            reportProjection(label.c_str(), rank, nullity,
                syncProjection);
        }
        {
            const std::string label = prefix + "-anti";
            reportProjection(label.c_str(), rank, nullity,
                antiProjection);
        }
    }
    std::printf(
        "conclusion: if single-guide projection is ~0, clump is "
        "NULLSPACE; cross-guide sync projection ~0 means the "
        "synchronous negative mode cannot be stabilized by clump, "
        "so contact/external support or a model change is required.\n");
    return 0;
}

int RunContactProjection(const std::string& sceneName, int points,
    int attempts, int guideCount, double spacing,
    double guideRadius, double normalStiffness,
    double frictionStiffness, double frictionCoefficient)
{
    Params params;
    const Scene scene = BuildScene(sceneName, points);
    if (scene.x.empty())
    {
        std::fprintf(stderr, "unknown scene: %s\n",
            sceneName.c_str());
        return 2;
    }
    const Rest authored = AuthoredRest(scene);
    const State authoredState = AuthoredState(scene);
    const SolveReport solve = SolveBlockInverse(
        scene, params, authored, authoredState, 40, 1.0e-6);
    const Rest rest = solve.rest;
    const State target = solve.target;
    const StabilityResult stability =
        ComputeStateHessian(scene, params, rest, target);
    const StateLayout layout = BuildLayout(scene, params);
    const Eigen::VectorXd direction = stability.minEigenvector;
    const double lambda0 = stability.minEigenvalue;
    const size_t nodeCount = scene.x.size();
    const size_t positionCount = layout.positionCount;
    const size_t quaternionCount = layout.quaternionCount;
    const size_t stride = positionCount + quaternionCount;
    const size_t totalDof =
        static_cast<size_t>(guideCount) * stride;
    const double radiusSum = 2.0 * guideRadius;
    const double normalResidual = std::max(0.0, radiusSum - spacing);
    const double normalForce = normalStiffness * normalResidual;
    const double frictionCap =
        frictionCoefficient * normalForce;
    std::printf(
        "=== M0-009 contact projection scene=%s points=%d "
        "guides=%d spacing=%.3f ===\n",
        sceneName.c_str(), points, guideCount, spacing);
    std::printf(
        "single-guide: force %.3e torque %.3e lambda0 %.6e\n",
        solve.forceNorm, solve.torqueNorm, lambda0);
    std::printf(
        "contact: radiusSum=%.3f normal stiffness=%.3e "
        "(compliance %.3e), stick stiffness=%.3e "
        "(compliance %.3e), mu=%.3f; rest C_n=%.3e, "
        "rest friction=0, normal force=%.3e, friction cap=%.3e\n",
        radiusSum, normalStiffness, 1.0 / normalStiffness,
        frictionStiffness, 1.0 / frictionStiffness,
        frictionCoefficient, normalResidual, normalForce,
        frictionCap);
    if (lambda0 >= 0.0)
    {
        std::printf(
            "lambda0>=0; the single-guide state is already stable, "
            "contact is not required for this mode\n");
        return 0;
    }
    if (!(spacing < radiusSum))
    {
        std::printf(
            "  contacts inactive at rest (spacing >= radiusSum); "
            "no contact projection\n");
        return 1;
    }

    std::vector<std::vector<Vec>> authoredPositions(
        static_cast<size_t>(guideCount),
        std::vector<Vec>(nodeCount));
    std::vector<std::vector<Vec>> currentPositions(
        static_cast<size_t>(guideCount),
        std::vector<Vec>(nodeCount));
    for (int guide = 0; guide < guideCount; ++guide)
    {
        const double offset = spacing *
            (static_cast<double>(guide) -
                0.5 * static_cast<double>(guideCount - 1));
        for (size_t point = 0; point < nodeCount; ++point)
        {
            const Vec shift(0.0, 0.0, offset);
            authoredPositions[static_cast<size_t>(guide)][point] =
                scene.x[point] + shift;
            currentPositions[static_cast<size_t>(guide)][point] =
                target.x[point] + shift;
        }
    }

    const int contactPairs = (guideCount - 1) *
        static_cast<int>(nodeCount);
    const int normalRows = contactPairs;
    const int frictionRows = 2 * contactPairs;
    const size_t columns =
        static_cast<size_t>(guideCount) * positionCount;
    Eigen::MatrixXd normalJacobian(
        normalRows, static_cast<Eigen::Index>(columns));
    Eigen::MatrixXd frictionJacobian(
        frictionRows, static_cast<Eigen::Index>(columns));
    const auto normalAt = [&](int guide, size_t point)
    {
        return (authoredPositions[static_cast<size_t>(guide + 1)][point] -
            authoredPositions[static_cast<size_t>(guide)][point])
            .normalized();
    };
    const auto addPointGradient = [&](Eigen::MatrixXd& matrix,
        int row, int guide, size_t point, const Vec& value)
    {
        if (point < layout.rootFixed)
            return;
        const Eigen::Index column =
            static_cast<Eigen::Index>(
                static_cast<size_t>(guide) * positionCount +
                layout.PositionIndex(point, 0));
        for (int axis = 0; axis < 3; ++axis)
        {
            matrix(row, column + axis) = value[axis];
        }
    };
    int row = 0;
    for (int guide = 0; guide + 1 < guideCount; ++guide)
    {
        for (size_t point = 0; point < nodeCount; ++point)
        {
            const Vec normal = normalAt(guide, point);
            addPointGradient(normalJacobian, row, guide,
                point, normal);
            addPointGradient(normalJacobian, row, guide + 1,
                point, -normal);
            const Vec tangent1 =
                std::abs(normal.x()) < 0.9
                ? Vec(1.0, 0.0, 0.0)
                : Vec(0.0, 1.0, 0.0);
            const Vec e1 =
                (tangent1 - normal * tangent1.dot(normal)).normalized();
            const Vec e2 = normal.cross(e1).normalized();
            addPointGradient(frictionJacobian, 2 * row,
                guide, point, -e1);
            addPointGradient(frictionJacobian, 2 * row,
                guide + 1, point, e1);
            addPointGradient(frictionJacobian, 2 * row + 1,
                guide, point, -e2);
            addPointGradient(frictionJacobian, 2 * row + 1,
                guide + 1, point, e2);
            ++row;
        }
    }
    Eigen::MatrixXd combined(
        normalRows + frictionRows,
        static_cast<Eigen::Index>(columns));
    combined.topRows(normalRows) = normalJacobian;
    combined.bottomRows(frictionRows) = frictionJacobian;

    const auto printRank = [&](const char* label,
        const Eigen::MatrixXd& matrix)
    {
        const Eigen::JacobiSVD<Eigen::MatrixXd> svd(matrix);
        const Eigen::VectorXd singular = svd.singularValues();
        const double tolerance =
            static_cast<double>(
                std::max(matrix.rows(), matrix.cols())) *
            std::numeric_limits<double>::epsilon() *
            std::max(1.0, singular.maxCoeff()) * 100.0;
        int rank = 0;
        for (Eigen::Index i = 0; i < singular.size(); ++i)
        {
            if (singular[i] > tolerance)
                ++rank;
        }
        const size_t nullity =
            columns - static_cast<size_t>(rank);
        std::printf(
            "  %-16s rank %d/%lld nullity %zu sigma [%.3e, %.3e]\n",
            label, rank, static_cast<long long>(matrix.cols()),
            nullity, singular.minCoeff(), singular.maxCoeff());
    };
    printRank("normal graph", normalJacobian);
    printRank("friction graph", frictionJacobian);
    printRank("combined graph", combined);

    const auto modeProjection = [&](bool antisymmetric)
    {
        Eigen::VectorXd normalProjection =
            Eigen::VectorXd::Zero(normalRows);
        Eigen::VectorXd frictionProjection =
            Eigen::VectorXd::Zero(frictionRows);
        int modeRow = 0;
        for (int guide = 0; guide + 1 < guideCount; ++guide)
        {
            const double sign0 = antisymmetric
                ? (guide % 2 == 0 ? 1.0 : -1.0) : 1.0;
            const double sign1 = antisymmetric
                ? ((guide + 1) % 2 == 0 ? 1.0 : -1.0) : 1.0;
            for (size_t point = 0; point < nodeCount; ++point)
            {
                const Vec normal = normalAt(guide, point);
                Vec difference;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const double value = direction[
                        static_cast<Eigen::Index>(
                            layout.PositionIndex(point, axis))];
                    difference[axis] =
                        sign1 * value - sign0 * value;
                }
                normalProjection[modeRow] =
                    normal.dot(difference);
                const Vec tangent1 =
                    std::abs(normal.x()) < 0.9
                    ? Vec(1.0, 0.0, 0.0)
                    : Vec(0.0, 1.0, 0.0);
                const Vec e1 = (tangent1 -
                    normal * tangent1.dot(normal)).normalized();
                const Vec e2 = normal.cross(e1).normalized();
                const Vec tangential =
                    difference - normal * normal.dot(difference);
                frictionProjection[2 * modeRow] =
                    e1.dot(tangential);
                frictionProjection[2 * modeRow + 1] =
                    e2.dot(tangential);
                ++modeRow;
            }
        }
        const double normalSquared =
            normalProjection.squaredNorm();
        const double frictionSquared =
            frictionProjection.squaredNorm();
        return std::make_tuple(
            normalSquared, frictionSquared,
            normalSquared + frictionSquared);
    };

    const auto [syncNormal, syncFriction, syncCombined] =
        modeProjection(false);
    const auto [antiNormal, antiFriction, antiCombined] =
        modeProjection(true);
    const auto required = [&](double projection)
    {
        return (lambda0 < 0.0 && projection > 1.0e-16)
            ? -lambda0 / projection
            : std::numeric_limits<double>::infinity();
    };
    const double syncNormalK = required(syncNormal);
    const double syncFrictionK = required(syncFriction);
    const double antiNormalK = required(antiNormal);
    const double antiFrictionK = required(antiFriction);
    const double practical = 1.0e6;
    const auto classify = [&](double value)
    {
        if (!std::isfinite(value))
            return "NULLSPACE";
        return value <= practical ? "finite/practical" : "exceeds practical";
    };
    std::printf(
        "  sync mode: |Jn v|^2 %.6e k_n %.6e (%s); "
        "|Jt v|^2 %.6e k_t %.6e (%s); combined %.6e\n",
        syncNormal, syncNormalK, classify(syncNormalK),
        syncFriction, syncFrictionK, classify(syncFrictionK),
        syncCombined);
    std::printf(
        "  anti mode: |Jn v|^2 %.6e k_n %.6e (%s); "
        "|Jt v|^2 %.6e k_t %.6e (%s); combined %.6e\n",
        antiNormal, antiNormalK, classify(antiNormalK),
        antiFriction, antiFrictionK, classify(antiFrictionK),
        antiCombined);
    std::printf(
        "  contact pairs %d, normal constraints %d, friction "
        "constraints %d; extra variables 0; Hessian size %zu\n",
        contactPairs, normalRows, frictionRows, totalDof);
    if (!std::isfinite(syncNormalK) &&
        !std::isfinite(syncFrictionK))
    {
        std::printf(
            "conclusion: cross-guide contact normal/friction are in the "
            "synchronous-mode nullspace; contact stiffness cannot "
            "stabilize the M0-007 negative mode. NO_EXTERNAL_SUPPORT "
            "remains; need external support or continuum/MPM.\n");
    }
    else
    {
        std::printf(
            "conclusion: synchronous contact projection is nonzero; "
            "a full contact XPBD acceptance run is required before "
            "claiming stability.\n");
    }
    return 0;
}

int RunM0_010(const std::string& sceneName, int points,
    int settleSteps, int windSteps, int recoverySteps,
    double windAcceleration)
{
    Params params;
    const Scene scene = BuildScene(sceneName, points);
    if (scene.x.empty())
    {
        std::fprintf(stderr, "unknown scene: %s\n",
            sceneName.c_str());
        return 2;
    }
    const Rest authored = AuthoredRest(scene);
    const State authoredState = AuthoredState(scene);
    const SolveReport solve = SolveBlockInverse(
        scene, params, authored, authoredState, 40, 1.0e-6);
    const Rest rest = solve.rest;
    const State target = solve.target;
    const double windSeconds =
        static_cast<double>(windSteps) * params.dt;
    const double recoverySeconds =
        static_cast<double>(recoverySteps) * params.dt;
    std::printf(
        "=== M0-010 strategy benchmark scene=%s points=%d ===\n",
        sceneName.c_str(), points);
    std::printf(
        "single-guide solve: force %.3e torque %.3e; "
        "settle=%d steps (%.2fs), wind=%.1f m/s^2 for %.2fs, "
        "recovery %.2fs; no lambda_min gate\n",
        solve.forceNorm, solve.torqueNorm, settleSteps,
        static_cast<double>(settleSteps) * params.dt,
        windAcceleration, windSeconds, recoverySeconds);
    std::printf(
        "criteria: Dtip/L<0.10, Dmax/L<0.15, centroid/L<0.05, "
        "heightRatio>0.8, visible maxDyn>0.005L, stretch<10%%\n");

    struct Strategy
    {
        const char* name;
        double globalK;
        double localK;
        int stride;
        double clumpK;
        bool clumpWhole;
    };
    const Strategy strategies[] = {
        {"baseline", 0.0, 0.0, 0, 0.0, false},
        {"global-1", 1.0, 0.0, 0, 0.0, false},
        {"global-10", 10.0, 0.0, 0, 0.0, false},
        {"global-100", 100.0, 0.0, 0, 0.0, false},
        {"global-1000", 1000.0, 0.0, 0, 0.0, false},
        {"local3-100", 0.0, 100.0, 3, 0.0, false},
        {"local3-1000", 0.0, 1000.0, 3, 0.0, false},
        {"local4-1", 0.0, 1.0, 4, 0.0, false},
        {"local4-10", 0.0, 10.0, 4, 0.0, false},
        {"local4-100", 0.0, 100.0, 4, 0.0, false},
        {"local4-1000", 0.0, 1000.0, 4, 0.0, false},
        {"local8-100", 0.0, 100.0, 8, 0.0, false},
        {"local8-1000", 0.0, 1000.0, 8, 0.0, false},
        {"mixed-10", 10.0, 10.0, 4, 0.0, false},
        {"mixed-100", 100.0, 100.0, 4, 0.0, false},
        {"clump-1e3", 0.0, 0.0, 0, 1.0e3, true},
        {"clump-1e6", 0.0, 0.0, 0, 1.0e6, true}
    };

    struct BenchRow
    {
        std::string name;
        ShapeMetrics shape;
        WindMetrics wind;
        double msPerStep = 0.0;
        int constraints = 0;
        bool candidate = false;
    };
    std::vector<BenchRow> rows;
    const size_t movable = scene.x.size() -
        static_cast<size_t>(params.rootFixed);
    for (const Strategy& strategy : strategies)
    {
        Params strategyParams = params;
        strategyParams.globalTetherStiffness = strategy.globalK;
        strategyParams.localTetherStiffness = strategy.localK;
        strategyParams.localTetherStride = strategy.stride;
        strategyParams.clumpStiffness = strategy.clumpK;
        strategyParams.clumpWholeGuide = strategy.clumpWhole;
        int constraints = 0;
        if (strategy.globalK > 0.0)
            constraints += static_cast<int>(movable);
        if (strategy.localK > 0.0 && strategy.stride > 0)
        {
            for (size_t point = static_cast<size_t>(params.rootFixed);
                point < scene.x.size(); ++point)
            {
                if ((point - static_cast<size_t>(params.rootFixed)) %
                    static_cast<size_t>(strategy.stride) == 0)
                {
                    ++constraints;
                }
            }
        }
        if (strategy.clumpK > 0.0)
            constraints += static_cast<int>(scene.x.size());

        const auto start = std::chrono::steady_clock::now();
        const State settled = SettleState(
            scene, strategyParams, rest, target, settleSteps);
        const WindMetrics wind = SimulateWind(
            scene, strategyParams, rest, settled,
            windSteps, recoverySteps, windAcceleration);
        const auto end = std::chrono::steady_clock::now();
        const double elapsedMilliseconds =
            std::chrono::duration<double, std::milli>(
                end - start).count();
        const int totalSteps =
            settleSteps + windSteps + recoverySteps;
        const double msPerStep =
            elapsedMilliseconds /
            static_cast<double>(std::max(1, totalSteps));
        const double stretch =
            std::max(wind.staticStretch, wind.maxStretch);
        const double tetherResidual =
            std::max(wind.staticTetherResidual,
                wind.maxTetherResidual) / scene.totalLength;
        const double clumpResidual =
            std::max(wind.staticClumpResidual,
                wind.maxClumpResidual) / scene.totalLength;
        const bool shapePass =
            wind.staticShape.tip < 0.10 &&
            wind.staticShape.max < 0.15 &&
            wind.staticShape.centroid < 0.05 &&
            wind.staticShape.heightRatio > 0.8;
        const bool numericalPass =
            wind.finite && stretch < 0.10;
        const bool candidate =
            shapePass && wind.visible && numericalPass;
        std::printf(
            "%-14s c=%3d | Dtip=%7.4f Dmax=%7.4f C=%7.4f "
            "H=%6.3f RMS=%7.4f v0=%7.3f\n",
            strategy.name, constraints,
            wind.staticShape.tip, wind.staticShape.max,
            wind.staticShape.centroid, wind.staticShape.heightRatio,
            wind.staticShape.rms, wind.staticVelocity);
        std::printf(
            "               | dyn=%7.4f rec=%6.3f res=%7.4f "
            "stretch=%7.4f teth=%7.4f clump=%7.4f "
            "vis=%d fin=%d ms/step=%6.3f %s\n",
            wind.maxDynamicOffset, wind.recoveryTime,
            wind.residualOffset, stretch, tetherResidual,
            clumpResidual, wind.visible ? 1 : 0,
            wind.finite ? 1 : 0, msPerStep,
            candidate ? "CANDIDATE" : "");
        rows.push_back({strategy.name, wind.staticShape, wind,
            msPerStep, constraints, candidate});
    }

    std::vector<BenchRow> candidates;
    for (const BenchRow& row : rows)
    {
        if (row.candidate)
            candidates.push_back(row);
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const BenchRow& lhs, const BenchRow& rhs) {
            return std::tie(lhs.shape.max, lhs.shape.tip,
                lhs.constraints) <
                std::tie(rhs.shape.max, rhs.shape.tip,
                    rhs.constraints);
        });
    std::printf(
        "Pareto candidates (shape pass + visible dynamics + "
        "numerical):\n");
    if (candidates.empty())
    {
        std::printf(
            "  none; no strategy met the relaxed static shape "
            "thresholds while keeping visible dynamics\n");
    }
    else
    {
        const size_t count =
            std::min<size_t>(3, candidates.size());
        for (size_t i = 0; i < count; ++i)
        {
            const BenchRow& row = candidates[i];
            std::printf(
                "  %-14s Dtip=%.4f Dmax=%.4f C=%.4f H=%.3f "
                "dyn=%.4f rec=%.3f res=%.4f constraints=%d "
                "ms/step=%.3f\n",
                row.name.c_str(), row.shape.tip, row.shape.max,
                row.shape.centroid, row.shape.heightRatio,
                row.wind.maxDynamicOffset, row.wind.recoveryTime,
                row.wind.residualOffset, row.constraints,
                row.msPerStep);
        }
    }
    std::printf(
        "contact: M0-009 projection = synchronous nullspace; "
        "NO_EXTERNAL_SUPPORT; no full contact dynamic run.\n");
    std::printf(
        "note: tether/clump are external shape-maintenance terms, "
        "not a physical sag-free rest; recovery=-1 means not "
        "recovered within %.2fs\n", recoverySeconds);
    return 0;
}

int RunM0_011(const std::string& sceneName, int points,
    int settleSteps, int windSteps, int recoverySteps,
    double windAcceleration)
{
    Params params;
    const Scene scene = BuildScene(sceneName, points);
    if (scene.x.empty())
    {
        std::fprintf(stderr, "unknown scene: %s\n",
            sceneName.c_str());
        return 2;
    }
    // This benchmark deliberately uses the authored curve as the rod rest.
    // The sag-free inverse is not part of the tether route: for strongly
    // bent N=32 scenes it drives the rest rotation to the pi-flip boundary
    // and contaminates every downstream metric.
    const Rest rest = AuthoredRest(scene);
    const State target = AuthoredState(scene);
    const double totalLength = scene.totalLength;
    const double substep = params.dt / params.substeps;
    const double gravity = std::abs(params.gravity);

    std::printf(
        "=== M0-011 progressive shape benchmark scene=%s points=%d ===\n",
        sceneName.c_str(), points);
    std::printf(
        "derivation: m=1 per point; k=g/(deltaG/L); "
        "c=2*zeta*sqrt(k); damping=c*h; "
        "k2=(F_w-k*deltaMax)/deltaMax^3; "
        "h=%.6e\n", substep);
    std::printf(
        "rest: authored curve (no sag-free inverse); "
        "tether carries the gravity offset\n");
    std::printf(
        "criteria: Dtip/L<0.10, Dmax/L<0.15, centroid/L<0.05, "
        "heightRatio>0.8, 0.02<=dyn<=0.08, recovery<=0.50s, "
        "residual<0.02, windDirZ>0.5, stretch<10%%\n");

    struct Strategy
    {
        std::string name;
        double globalK = 0.0;
        double localK = 0.0;
        double cubicK = 0.0;
        int stride = 0;
        double damping = 0.0;
    };
    std::vector<Strategy> strategies;
    strategies.push_back(
        {"baseline", 0.0, 0.0, 0.0, 0, params.damping});
    strategies.push_back(
        {"linear-g100", 100.0, 0.0, 0.0, 0, params.damping});
    strategies.push_back(
        {"linear-g1000", 1000.0, 0.0, 0.0, 0, params.damping});

    // Soft core + progressive cubic:
    //   F = k1*d + k2*d^3,  E = 0.5*k1*d^2 + 0.25*k2*d^4.
    // k1 is derived from a target gravity offset delta1=g/k1.  k2 is
    // derived so the static wind force is capped at deltaMax:
    //   k1*deltaMax + k2*deltaMax^3 = F_wind.
    // A pure dead zone is deliberately not used: inside the band it has
    // no unique equilibrium, so wind history leaves a residual offset.
    const double coreFractions[] = {0.010, 0.020, 0.030, 0.050};
    const double capFractions[] = {0.0, 0.08};
    const double zeta = 0.5;
    for (double core : coreFractions)
    {
        const double coreStiffness =
            gravity / std::max(1.0e-9, core * totalLength);
        const double damping = std::clamp(
            2.0 * zeta * std::sqrt(coreStiffness) * substep,
            1.0e-5, 0.20);
        for (double cap : capFractions)
        {
            double cubicStiffness = 0.0;
            if (cap > 0.0)
            {
                const double deltaMax = cap * totalLength;
                cubicStiffness = std::max(0.0,
                    (windAcceleration -
                        coreStiffness * deltaMax) /
                    (deltaMax * deltaMax * deltaMax));
                if (cubicStiffness <= 1.0e-9)
                    continue;
            }
            char name[64];
            std::snprintf(name, sizeof(name),
                "prog-g-c%03.0f-m%03.0f", core * 100.0,
                cap * 100.0);
            strategies.push_back({name, coreStiffness, 0.0,
                cubicStiffness, 0, damping});
            std::snprintf(name, sizeof(name),
                "prog-l4-c%03.0f-m%03.0f", core * 100.0,
                cap * 100.0);
            strategies.push_back({name, 0.0, coreStiffness,
                cubicStiffness, 4, damping});
        }
    }

    struct BenchRow
    {
        std::string name;
        ShapeMetrics shape;
        WindMetrics wind;
        double msPerStep = 0.0;
        int constraints = 0;
        double stiffness = 0.0;
        double cubicStiffness = 0.0;
        bool candidate = false;
    };
    std::vector<BenchRow> rows;
    const size_t movable = scene.x.size() -
        static_cast<size_t>(params.rootFixed);
    for (const Strategy& strategy : strategies)
    {
        Params strategyParams = params;
        strategyParams.globalTetherStiffness = strategy.globalK;
        strategyParams.localTetherStiffness = strategy.localK;
        strategyParams.localTetherStride = strategy.stride;
        strategyParams.tetherDeadZone = 0.0;
        strategyParams.tetherProgressive = strategy.cubicK;
        strategyParams.damping = strategy.damping;
        int constraints = 0;
        if (strategy.globalK > 0.0)
            constraints += static_cast<int>(movable);
        if (strategy.localK > 0.0 && strategy.stride > 0)
        {
            for (size_t point = static_cast<size_t>(params.rootFixed);
                point < scene.x.size(); ++point)
            {
                if ((point - static_cast<size_t>(params.rootFixed)) %
                    static_cast<size_t>(strategy.stride) == 0)
                {
                    ++constraints;
                }
            }
        }

        const auto start = std::chrono::steady_clock::now();
        const State settled = SettleState(
            scene, strategyParams, rest, target, settleSteps);
        const WindMetrics wind = SimulateWind(
            scene, strategyParams, rest, settled,
            windSteps, recoverySteps, windAcceleration);
        const auto end = std::chrono::steady_clock::now();
        const double elapsedMilliseconds =
            std::chrono::duration<double, std::milli>(
                end - start).count();
        const int totalSteps =
            settleSteps + windSteps + recoverySteps;
        const double msPerStep =
            elapsedMilliseconds /
            static_cast<double>(std::max(1, totalSteps));
        const double stretch =
            std::max(wind.staticStretch, wind.maxStretch);

        const bool shapePass =
            wind.staticShape.tip < 0.10 &&
            wind.staticShape.max < 0.15 &&
            wind.staticShape.centroid < 0.05 &&
            wind.staticShape.heightRatio > 0.8;
        const bool windPass =
            wind.maxDynamicOffset >= 0.02 &&
            wind.maxDynamicOffset <= 0.08 &&
            wind.recoveryTime >= 0.0 &&
            wind.recoveryTime <= 0.50 &&
            wind.residualOffset < 0.02 &&
            wind.peakCentroidDirectionZ > 0.5;
        const bool numericalPass =
            wind.finite && stretch < 0.10;
        const bool candidate =
            shapePass && windPass && numericalPass;
        std::printf(
            "%-16s c=%3d k1=%8.1f k2=%8.3f | "
            "Dtip=%7.4f Dmax=%7.4f C=%7.4f H=%6.3f "
            "dyn=%7.4f rec=%6.3f res=%7.4f dirZ=%6.3f "
            "peakT=%6.3f w=%8.3f stretch=%7.4f "
            "ms/step=%6.3f %s\n",
            strategy.name.c_str(), constraints,
            strategy.globalK > 0.0 ? strategy.globalK
                : strategy.localK,
            strategy.cubicK,
            wind.staticShape.tip, wind.staticShape.max,
            wind.staticShape.centroid,
            wind.staticShape.heightRatio,
            wind.maxDynamicOffset, wind.recoveryTime,
            wind.residualOffset, wind.peakCentroidDirectionZ,
            wind.peakTime, wind.maxAngularSpeed, stretch,
            msPerStep, candidate ? "CANDIDATE" : "");
        rows.push_back({strategy.name, wind.staticShape, wind,
            msPerStep, constraints,
            strategy.globalK > 0.0 ? strategy.globalK
                : strategy.localK,
            strategy.cubicK, candidate});
    }

    std::vector<BenchRow> candidates;
    for (const BenchRow& row : rows)
    {
        if (row.candidate)
            candidates.push_back(row);
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const BenchRow& lhs, const BenchRow& rhs) {
            const double lhsScore =
                std::abs(lhs.wind.maxDynamicOffset - 0.04);
            const double rhsScore =
                std::abs(rhs.wind.maxDynamicOffset - 0.04);
            return std::make_tuple(lhsScore, lhs.shape.max,
                lhs.wind.recoveryTime) <
                std::make_tuple(rhsScore, rhs.shape.max,
                    rhs.wind.recoveryTime);
        });
    std::printf(
        "Pareto candidates (static + wind envelope + recovery):\n");
    if (candidates.empty())
    {
        std::printf(
            "  none; no dead-zone strategy met static shape, "
            "normal wind response and recovery together\n");
    }
    else
    {
        const size_t count =
            std::min<size_t>(5, candidates.size());
        for (size_t i = 0; i < count; ++i)
        {
            const BenchRow& row = candidates[i];
            std::printf(
                "  %-16s Dtip=%.4f Dmax=%.4f C=%.4f H=%.3f "
                "dyn=%.4f rec=%.3f res=%.4f dirZ=%.3f "
                "ang=%.1f constraints=%d ms/step=%.3f\n",
                row.name.c_str(), row.shape.tip, row.shape.max,
                row.shape.centroid, row.shape.heightRatio,
                row.wind.maxDynamicOffset,
                row.wind.recoveryTime, row.wind.residualOffset,
                row.wind.peakCentroidDirectionZ,
                row.wind.maxAngularSpeed, row.constraints,
                row.msPerStep);
        }
    }
    std::printf(
        "note: soft core + cubic is still a numeric shape term, "
        "not a physical anchor; contact M0-009 remains "
        "NO_EXTERNAL_SUPPORT\n");
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    std::string scene = "straight";
    int points = 8;
    int settleSteps = 200;
    int attempts = 1;
    double clumpStiffness = 0.0;
    int clumpGroupSize = 4;
    bool clumpWhole = false;
    int crossGuides = 0;
    double guideSpacing = 0.3;
    double crossMaxStiffness = 1.0e8;
    bool clumpProjection = false;
    bool contactProjection = false;
    int contactGuides = 2;
    double contactSpacing = 0.3;
    double contactRadius = 0.2;
    double contactStiffness = 1.0e4;
    double frictionStiffness = 1.0e4;
    double frictionCoefficient = 0.3;
    bool benchmark = false;
    bool benchmarkDeadZone = false;
    int windSteps = 120;
    int recoverySteps = 120;
    double windAcceleration = 20.0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--scene" && i + 1 < argc)
            scene = argv[++i];
        else if (argument == "--points" && i + 1 < argc)
            points = std::atoi(argv[++i]);
        else if (argument == "--settle" && i + 1 < argc)
            settleSteps = std::atoi(argv[++i]);
        else if (argument == "--attempts" && i + 1 < argc)
            attempts = std::atoi(argv[++i]);
        else if (argument == "--clump-stiffness" && i + 1 < argc)
            clumpStiffness = std::atof(argv[++i]);
        else if (argument == "--clump-group" && i + 1 < argc)
            clumpGroupSize = std::atoi(argv[++i]);
        else if (argument == "--clump-whole")
            clumpWhole = true;
        else if (argument == "--cross-guides" && i + 1 < argc)
            crossGuides = std::atoi(argv[++i]);
        else if (argument == "--guide-spacing" && i + 1 < argc)
            guideSpacing = std::atof(argv[++i]);
        else if (argument == "--cross-max-stiffness" && i + 1 < argc)
            crossMaxStiffness = std::atof(argv[++i]);
        else if (argument == "--clump-projection")
            clumpProjection = true;
        else if (argument == "--contact-projection")
            contactProjection = true;
        else if (argument == "--contact-guides" && i + 1 < argc)
            contactGuides = std::atoi(argv[++i]);
        else if (argument == "--contact-spacing" && i + 1 < argc)
            contactSpacing = std::atof(argv[++i]);
        else if (argument == "--contact-radius" && i + 1 < argc)
            contactRadius = std::atof(argv[++i]);
        else if (argument == "--contact-stiffness" && i + 1 < argc)
            contactStiffness = std::atof(argv[++i]);
        else if (argument == "--friction-stiffness" && i + 1 < argc)
            frictionStiffness = std::atof(argv[++i]);
        else if (argument == "--friction-coefficient" && i + 1 < argc)
            frictionCoefficient = std::atof(argv[++i]);
        else if (argument == "--benchmark")
            benchmark = true;
        else if (argument == "--benchmark-deadzone")
            benchmarkDeadZone = true;
        else if (argument == "--wind-steps" && i + 1 < argc)
            windSteps = std::atoi(argv[++i]);
        else if (argument == "--recovery-steps" && i + 1 < argc)
            recoverySteps = std::atoi(argv[++i]);
        else if (argument == "--wind-accel" && i + 1 < argc)
            windAcceleration = std::atof(argv[++i]);
    }
    if (benchmark)
    {
        return RunM0_010(scene, points, settleSteps,
            windSteps, recoverySteps, windAcceleration);
    }
    if (benchmarkDeadZone)
    {
        return RunM0_011(scene, points, settleSteps,
            windSteps, recoverySteps, windAcceleration);
    }
    if (contactProjection)
    {
        return RunContactProjection(scene, points, attempts,
            contactGuides, contactSpacing, contactRadius,
            contactStiffness, frictionStiffness,
            frictionCoefficient);
    }
    if (clumpProjection)
        return RunClumpProjection(scene, points, attempts);
    if (crossGuides >= 2)
    {
        return RunCrossGuideClump(scene, points, attempts,
            crossGuides, guideSpacing, crossMaxStiffness);
    }
    return RunM0_007(scene, points, settleSteps, attempts,
        clumpStiffness, clumpGroupSize, clumpWhole);
}
