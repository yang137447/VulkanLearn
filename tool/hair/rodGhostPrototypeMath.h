#pragma once

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace R1
{
using Vec = Eigen::Vector3d;
using Mat = Eigen::Matrix3d;
using Quat = Eigen::Quaterniond;
constexpr double Pi = 3.14159265358979323846;

struct State
{
    std::vector<Vec> positions;
    std::vector<Quat> frames;
};

struct Model
{
    State authored;
    std::vector<double> lengths;
    std::vector<Vec> restStrains;
    std::vector<Vec> restRotations;
    std::vector<double> masses;
    std::vector<double> ghostStiffness;
    Vec shearStretch = Vec::Ones();
    Vec bendTwist = Vec::Ones();
    double margin = 0.1;
};

struct Evaluation
{
    double shearStretch = 0.0;
    double bendTwist = 0.0;
    double ghost = 0.0;
    double gravity = 0.0;
    std::vector<Vec> force;
    std::vector<Vec> bodyTorque;
    std::vector<Vec> ghostReaction;

    double Energy() const { return shearStretch + bendTwist + ghost + gravity; }
};

inline void Require(bool valid)
{
    if (!valid)
        throw std::runtime_error("DOMAIN_FAILURE");
}

inline Quat Unit(const Quat& input)
{
    Require(input.coeffs().allFinite() && std::isfinite(input.norm()) &&
            input.norm() > 0.0 && std::abs(input.norm() - 1.0) <= 1e-12);
    return input.normalized();
}

inline Mat CrossMatrix(const Vec& value)
{
    Mat result;
    result << 0.0, -value.z(), value.y(), value.z(), 0.0, -value.x(),
              -value.y(), value.x(), 0.0;
    return result;
}

inline Quat ExpR(const Vec& fullAngle, double margin)
{
    const double angle = fullAngle.norm();
    Require(fullAngle.allFinite() && std::isfinite(angle) &&
            margin > 0.0 && margin < Pi && angle < Pi - margin);
    if (angle == 0.0)
        return Quat::Identity();
    return Quat(Eigen::AngleAxisd(angle, fullAngle / angle));
}

inline Vec LogR(const Quat& input, double margin)
{
    Quat value = Unit(input);
    Require(margin > 0.0 && margin < Pi);
    if (value.w() < 0.0)
        value.coeffs() *= -1.0;
    const double sine = value.vec().norm();
    const double angle = 2.0 * std::atan2(sine, value.w());
    Require(std::isfinite(angle) && angle < Pi - margin);
    if (sine == 0.0)
        return Vec::Zero();
    return angle * value.vec() / sine;
}

inline Mat RightJacobian(const Vec& rotation)
{
    const double squared = rotation.squaredNorm();
    Require(rotation.allFinite() && std::isfinite(squared));
    const Mat skew = CrossMatrix(rotation);
    if (squared < 1e-8)
        return Mat::Identity() - (0.5 - squared / 24.0 + squared * squared / 720.0) * skew +
               (1.0 / 6.0 - squared / 120.0 + squared * squared / 5040.0) * skew * skew;
    const double angle = std::sqrt(squared);
    return Mat::Identity() - ((1.0 - std::cos(angle)) / squared) * skew +
           ((angle - std::sin(angle)) / (squared * angle)) * skew * skew;
}

inline Model AuthoredModel(const State& authored, const Vec& shearStretch, const Vec& bendTwist,
                           double margin)
{
    Require(authored.frames.size() >= 2 && authored.positions.size() == authored.frames.size() + 1);
    Require(shearStretch.allFinite() && bendTwist.allFinite() &&
            shearStretch.minCoeff() > 0.0 && bendTwist.minCoeff() > 0.0);
    Model model;
    model.authored = authored;
    model.shearStretch = shearStretch;
    model.bendTwist = bendTwist;
    model.margin = margin;
    model.masses.assign(authored.positions.size(), 0.01);
    model.ghostStiffness.assign(authored.positions.size(), 0.0);
    for (size_t segment = 0; segment < authored.frames.size(); ++segment)
    {
        Require(authored.positions[segment].allFinite() && authored.positions[segment + 1].allFinite());
        const Vec edge = authored.positions[segment + 1] - authored.positions[segment];
        const double length = edge.norm();
        Require(std::isfinite(length) && length > 0.0);
        const Quat frame = Unit(authored.frames[segment]);
        model.lengths.push_back(length);
        model.restStrains.push_back(frame.conjugate() * edge / length);
        if (segment + 1 < authored.frames.size())
            model.restRotations.push_back(LogR(frame.conjugate() * Unit(authored.frames[segment + 1]), margin));
    }
    return model;
}

inline Evaluation Evaluate(const Model& model, const State& state,
                           const std::vector<Vec>& ghostPositions, const Vec& gravity)
{
    const size_t segments = model.lengths.size();
    Require(state.positions.size() == segments + 1 && state.frames.size() == segments &&
            ghostPositions.size() == segments + 1 && gravity.allFinite() &&
            model.masses.size() == segments + 1 && model.ghostStiffness.size() == segments + 1);
    Evaluation result;
    result.force.assign(segments + 1, Vec::Zero());
    result.bodyTorque.assign(segments, Vec::Zero());
    result.ghostReaction.assign(segments + 1, Vec::Zero());
    for (size_t node = 0; node <= segments; ++node)
    {
        Require(state.positions[node].allFinite() && ghostPositions[node].allFinite() &&
                std::isfinite(model.masses[node]) && model.masses[node] > 0.0 &&
                std::isfinite(model.ghostStiffness[node]) && model.ghostStiffness[node] >= 0.0);
        result.gravity -= model.masses[node] * gravity.dot(state.positions[node]);
        result.force[node] += model.masses[node] * gravity;
        if (node != 0)
        {
            const Vec offset = ghostPositions[node] - state.positions[node];
            const Vec ghostForce = model.ghostStiffness[node] * offset;
            result.ghost += 0.5 * model.ghostStiffness[node] * offset.squaredNorm();
            result.force[node] += ghostForce;
            result.ghostReaction[node] = -ghostForce;
        }
    }
    for (size_t segment = 0; segment < segments; ++segment)
    {
        const Quat frame = Unit(state.frames[segment]);
        const Vec edge = state.positions[segment + 1] - state.positions[segment];
        Require(edge.norm() > 0.0);
        const Vec strain = frame.conjugate() * edge / model.lengths[segment];
        const Vec error = strain - model.restStrains[segment];
        const Vec stress = model.shearStretch.cwiseProduct(error);
        result.shearStretch += 0.5 * model.lengths[segment] * error.dot(stress);
        const Vec force = frame * stress;
        result.force[segment] += force;
        result.force[segment + 1] -= force;
        result.bodyTorque[segment] += model.lengths[segment] * strain.cross(stress);
        if (segment + 1 < segments)
        {
            const Quat relative = frame.conjugate() * Unit(state.frames[segment + 1]);
            const Vec rotation = LogR(relative, model.margin);
            const Vec errorRotation = rotation - model.restRotations[segment];
            const double dual = 0.5 * (model.lengths[segment] + model.lengths[segment + 1]);
            const Vec moment = model.bendTwist.cwiseProduct(errorRotation) / dual;
            const Vec pullback = RightJacobian(rotation).transpose().fullPivLu().solve(moment);
            result.bendTwist += 0.5 * errorRotation.dot(moment);
            result.bodyTorque[segment] += relative * pullback;
            result.bodyTorque[segment + 1] -= pullback;
        }
    }
    Require(std::isfinite(result.Energy()));
    for (const auto& force : result.force)
        Require(force.allFinite());
    for (const auto& torque : result.bodyTorque)
        Require(torque.allFinite());
    return result;
}
}
