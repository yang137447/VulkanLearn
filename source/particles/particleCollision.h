#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

#include <Eigen/Dense>

#include "particles/particleAsset.h"
#include "particles/particleSdf.h"

namespace VL
{

struct ParticleCollisionState
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f velocity = Eigen::Vector3f::Zero();
    bool alive = true;
};

// 与 particleSimulate.comp 的 ResolveCollision 保持同一套公式。
// type 的 GPU 编码：0 none、1 plane、2 box、3 capsule、4 sdf。
inline int ParticleCollisionTypeValue(std::string_view type) noexcept
{
    if (type == "plane") return 1;
    if (type == "box") return 2;
    if (type == "capsule") return 3;
    if (type == "sdf") return 4;
    return 0;
}

inline Eigen::Vector3f NormalizedParticleCollisionPlaneNormal(
    const ParticleEmitterAsset& emitter) noexcept
{
    const float length = emitter.collisionPlaneNormal.norm();
    if (!emitter.collisionPlaneNormal.allFinite() ||
        !std::isfinite(length) || length <= 1.0e-6f)
    {
        return Eigen::Vector3f::Zero();
    }
    return emitter.collisionPlaneNormal / length;
}

// 返回粒子碰撞后是否仍存活。killOnCollision 命中时 alive=false。
// 碰撞是粒子球（radius）与无限平面/世界 AABB/世界胶囊的解析修正。
inline bool ResolveParticleCollision(
    ParticleCollisionState& state,
    const ParticleEmitterAsset& emitter,
    Eigen::Vector3f* outNormal = nullptr)
{
    if (outNormal != nullptr)
    {
        *outNormal = Eigen::Vector3f::Zero();
    }
    if (!state.alive)
    {
        return false;
    }
    const int type = ParticleCollisionTypeValue(emitter.collisionType);
    if (type == 0)
    {
        return true;
    }
    if (!state.position.allFinite() || !state.velocity.allFinite())
    {
        return true;
    }

    const float bounce = std::clamp(emitter.collisionBounce, 0.0f, 1.0f);
    const float friction =
        std::clamp(emitter.collisionFriction, 0.0f, 1.0f);
    const float radius = std::max(emitter.collisionRadius, 0.0f);
    bool hit = false;
    Eigen::Vector3f hitNormal = Eigen::Vector3f::Zero();

    if (type == 1)
    {
        const Eigen::Vector3f normal =
            NormalizedParticleCollisionPlaneNormal(emitter);
        if (normal.squaredNorm() <= 1.0e-12f)
        {
            return true;
        }
        const float distance =
            normal.dot(state.position) -
            emitter.collisionPlaneOffset -
            radius;
        if (distance >= 0.0f)
        {
            return true;
        }

        state.position -= normal * distance;
        const float normalSpeed = normal.dot(state.velocity);
        if (normalSpeed < 0.0f)
        {
            const Eigen::Vector3f normalVelocity =
                normal * normalSpeed;
            const Eigen::Vector3f tangentVelocity =
                state.velocity - normalVelocity;
            state.velocity =
                tangentVelocity * (1.0f - friction) -
                normalVelocity * bounce;
        }
        hitNormal = normal;
        hit = true;
    }
    else if (type == 2)
    {
        const Eigen::Vector3f minimum =
            emitter.collisionBoxMin.array() - radius;
        const Eigen::Vector3f maximum =
            emitter.collisionBoxMax.array() + radius;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (state.position[axis] < minimum[axis] ||
                state.position[axis] > maximum[axis])
            {
                return true;
            }
        }

        int axis = 0;
        bool minimumSide = true;
        float best = std::numeric_limits<float>::infinity();
        for (int candidate = 0; candidate < 3; ++candidate)
        {
            const float toMinimum =
                state.position[candidate] - minimum[candidate];
            const float toMaximum =
                maximum[candidate] - state.position[candidate];
            if (toMinimum < best)
            {
                best = toMinimum;
                axis = candidate;
                minimumSide = true;
            }
            if (toMaximum < best)
            {
                best = toMaximum;
                axis = candidate;
                minimumSide = false;
            }
        }

        Eigen::Vector3f normal = Eigen::Vector3f::Zero();
        if (minimumSide)
        {
            state.position[axis] = minimum[axis];
            normal[axis] = -1.0f;
        }
        else
        {
            state.position[axis] = maximum[axis];
            normal[axis] = 1.0f;
        }

        const float normalSpeed = normal.dot(state.velocity);
        const Eigen::Vector3f normalVelocity =
            normal * normalSpeed;
        const Eigen::Vector3f tangentVelocity =
            state.velocity - normalVelocity;
        const float reflectedNormalSpeed = normalSpeed < 0.0f
            ? -bounce * normalSpeed
            : normalSpeed;
        state.velocity =
            tangentVelocity * (1.0f - friction) +
            normal * reflectedNormalSpeed;
        hitNormal = normal;
        hit = true;
    }
    else if (type == 3)
    {
        // 胶囊：把 p 投影到线段 AB 得到最近点 q，再按球-球处理。
        // A/B 重合时 t=0，退化为球。
        const Eigen::Vector3f segment =
            emitter.collisionCapsuleB - emitter.collisionCapsuleA;
        const float segmentLengthSquared = segment.squaredNorm();
        float t = 0.0f;
        if (segmentLengthSquared > 1.0e-12f)
        {
            t = std::clamp(
                (state.position - emitter.collisionCapsuleA)
                    .dot(segment) /
                    segmentLengthSquared,
                0.0f,
                1.0f);
        }
        const Eigen::Vector3f closest =
            emitter.collisionCapsuleA + segment * t;
        const float surfaceDistance =
            std::max(emitter.collisionCapsuleRadius, 0.0f) + radius;
        const Eigen::Vector3f offset = state.position - closest;
        const float distance = offset.norm();
        if (distance >= surfaceDistance)
        {
            return true;
        }

        // 圆心正落在轴线上时方向未定义；沿用 +Y 稳定轴避免 NaN。
        const Eigen::Vector3f normal = distance > 1.0e-6f
            ? offset / distance
            : Eigen::Vector3f(0.0f, 1.0f, 0.0f);
        state.position = closest + normal * surfaceDistance;

        const float normalSpeed = normal.dot(state.velocity);
        if (normalSpeed < 0.0f)
        {
            const Eigen::Vector3f normalVelocity =
                normal * normalSpeed;
            const Eigen::Vector3f tangentVelocity =
                state.velocity - normalVelocity;
            state.velocity =
                tangentVelocity * (1.0f - friction) -
                normalVelocity * bounce;
        }
        hitNormal = normal;
        hit = true;
    }
    else if (type == 4)
    {
        // SDF：d<radius 时沿梯度把粒子球推到等距面外。梯度为 0 时
        // 放弃修正，保持位置不变，避免产生 NaN。
        if (!emitter.collisionSdf || emitter.collisionSdf->Empty())
        {
            return true;
        }
        const float distance =
            emitter.collisionSdf->SampleDistance(state.position);
        if (!std::isfinite(distance) || distance >= radius)
        {
            return true;
        }
        const Eigen::Vector3f gradient =
            emitter.collisionSdf->SampleGradient(state.position);
        if (!gradient.allFinite() ||
            gradient.squaredNorm() <= 1.0e-12f)
        {
            return true;
        }
        const Eigen::Vector3f normal = gradient.normalized();
        state.position += normal * (radius - distance);

        const float normalSpeed = normal.dot(state.velocity);
        if (normalSpeed < 0.0f)
        {
            const Eigen::Vector3f normalVelocity =
                normal * normalSpeed;
            const Eigen::Vector3f tangentVelocity =
                state.velocity - normalVelocity;
            state.velocity =
                tangentVelocity * (1.0f - friction) -
                normalVelocity * bounce;
        }
        hitNormal = normal;
        hit = true;
    }

    if (outNormal != nullptr)
    {
        *outNormal = hit ? hitNormal : Eigen::Vector3f::Zero();
    }
    if (hit && emitter.killOnCollision)
    {
        state.alive = false;
    }
    return state.alive;
}

// CCD 子步数：按一帧扫过的距离与碰撞体尺度（粒子半径，最小 0.5cm）
// 计算，上限 8 步。未开启 CCD 或碰撞类型为 none 时恒为 1。
inline int ParticleCollisionStepCount(
    const Eigen::Vector3f& velocity,
    const ParticleEmitterAsset& emitter,
    float deltaSeconds) noexcept
{
    if (!emitter.collisionCcd || deltaSeconds <= 0.0f ||
        !std::isfinite(deltaSeconds) ||
        ParticleCollisionTypeValue(emitter.collisionType) == 0)
    {
        return 1;
    }
    const float speed = velocity.norm();
    if (!std::isfinite(speed))
    {
        return 1;
    }
    const float stepDistance =
        std::max(std::max(emitter.collisionRadius, 0.0f), 0.5f);
    const float swept = speed * deltaSeconds;
    if (!std::isfinite(swept) || swept <= stepDistance)
    {
        return 1;
    }
    return std::clamp(
        static_cast<int>(std::ceil(swept / stepDistance)),
        1,
        8);
}

// 运行时帧积分的 CPU 镜像：重力、指数 drag、位置积分与碰撞按子步重复。
// 与 particleSimulate.comp 的循环同序；prewarm 不走这里，只在末尾解析一次。
inline bool ResolveParticleCollisionSteps(
    ParticleCollisionState& state,
    const ParticleEmitterAsset& emitter,
    const Eigen::Vector3f& gravity,
    float drag,
    float deltaSeconds)
{
    if (!state.alive)
    {
        return false;
    }
    if (deltaSeconds <= 0.0f || !std::isfinite(deltaSeconds))
    {
        return state.alive;
    }

    const int steps =
        ParticleCollisionStepCount(state.velocity, emitter, deltaSeconds);
    const float stepSeconds = deltaSeconds / static_cast<float>(steps);
    const float damping = std::exp(-std::max(drag, 0.0f) * stepSeconds);
    for (int step = 0; step < steps; ++step)
    {
        state.velocity += gravity * stepSeconds;
        state.velocity *= damping;
        state.position += state.velocity * stepSeconds;
        if (!ResolveParticleCollision(state, emitter))
        {
            return false;
        }
    }
    return state.alive;
}

} // namespace VL
