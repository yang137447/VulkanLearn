#include "physics/runtime/fractureImpactWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include "physics/runtime/joltRuntime.h"

namespace VL::Physics
{
namespace
{

constexpr JPH::ObjectLayer StaticLayer = 0;
constexpr JPH::ObjectLayer MovingLayer = 1;
constexpr float FixedStep = 1.0f / 60.0f;
constexpr int MaxSubsteps = 16;

uint32_t BodyKey(const JPH::BodyID& id)
{
    return id.GetIndexAndSequenceNumber();
}

// Jolt 用米、项目用厘米；这两个转换只允许出现在物理世界边界。
Eigen::Vector3f ToCentimeters(JPH::Vec3Arg value)
{
    return {value.GetX() * 100.0f, value.GetY() * 100.0f, value.GetZ() * 100.0f};
}

JPH::Vec3 ToMeters(const Eigen::Vector3f& value)
{
    return {value.x() * 0.01f, value.y() * 0.01f, value.z() * 0.01f};
}

JPH::Quat ToJolt(const Eigen::Quaternionf& value)
{
    return JPH::Quat(value.x(), value.y(), value.z(), value.w());
}

} // namespace

const char* FractureMaterialProfileName(FractureMaterialProfile profile)
{
    switch (profile)
    {
    case FractureMaterialProfile::Glass: return "glass";
    case FractureMaterialProfile::Balloon: return "balloon";
    case FractureMaterialProfile::Generic:
    default: return "generic";
    }
}

struct FractureImpactWorld::Impl
{
    struct BodyIdentity
    {
        enum class Shape
        {
            Sphere,
            Pane,
            Shard,
            AttachedShard,
            Projectile,
            Ground
        };
        Shape shape = Shape::Projectile;
        FractureBodyRole role = FractureBodyRole::Projectile;
        FractureMaterialProfile profile = FractureMaterialProfile::Generic;
        std::string objectName;
        std::string shardName;
    };

    class Listener final : public JPH::ContactListener
    {
    public:
        explicit Listener(Impl& owner) : owner(owner) {}

        void OnContactAdded(const JPH::Body& first, const JPH::Body& second,
            const JPH::ContactManifold& manifold,
            JPH::ContactSettings&) override
        {
            owner.RecordContact(first, second, manifold);
        }

    private:
        Impl& owner;
    };

    // 进程级 Jolt 注册必须先于其余成员构造；容器成员可能在注册前分配内存。
    JoltRegistrationToken registration;
    JPH::BroadPhaseLayerInterfaceTable layers{2, 2};
    JPH::ObjectLayerPairFilterTable pairs{2};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broadphaseFilter;
    JPH::TempAllocatorImpl allocator{8 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem world;
    Listener listener{*this};
    std::map<uint32_t, BodyIdentity> identities;
    std::vector<JPH::BodyID> sphereBodies;
    std::vector<JPH::BodyID> paneBodies;
    std::vector<JPH::BodyID> shardBodies;
    std::vector<JPH::BodyID> projectiles;
    std::map<uint32_t, JPH::BodyID> projectileIds;
    std::map<std::string, JPH::BodyID> shardIds;
    std::set<uint32_t> impactedProjectiles;
    std::vector<FractureImpact> impacts;
    double accumulated = 0.0;
    JPH::BodyID groundBody;
    float groundHeight = -100.0f;

    Impl()
    {
        layers.MapObjectToBroadPhaseLayer(StaticLayer, JPH::BroadPhaseLayer(0));
        layers.MapObjectToBroadPhaseLayer(MovingLayer, JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(StaticLayer, MovingLayer);
        pairs.EnableCollision(MovingLayer, MovingLayer);
        broadphaseFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
            layers, 2, pairs, 2);
        world.Init(2048, 0, 8192, 8192, layers, *broadphaseFilter, pairs);
        world.SetGravity({0, -9.81f, 0});
        world.SetContactListener(&listener);
        CreateGround();
    }

    ~Impl()
    {
        Clear();
    }

    void Clear()
    {
        for (const JPH::BodyID& id : projectiles)
        {
            world.GetBodyInterface().RemoveBody(id);
            world.GetBodyInterface().DestroyBody(id);
        }
        projectiles.clear();
        projectileIds.clear();
        for (const JPH::BodyID& id : sphereBodies)
        {
            world.GetBodyInterface().RemoveBody(id);
            world.GetBodyInterface().DestroyBody(id);
        }
        sphereBodies.clear();
        for (const JPH::BodyID& id : paneBodies)
        {
            world.GetBodyInterface().RemoveBody(id);
            world.GetBodyInterface().DestroyBody(id);
        }
        paneBodies.clear();
        for (const JPH::BodyID& id : shardBodies)
        {
            world.GetBodyInterface().RemoveBody(id);
            world.GetBodyInterface().DestroyBody(id);
        }
        shardBodies.clear();
        shardIds.clear();
        impactedProjectiles.clear();
        identities.clear();
        if (groundBody.IsInvalid()) CreateGround();
        else identities[BodyKey(groundBody)] = {
            BodyIdentity::Shape::Ground, FractureBodyRole::Ground,
            FractureMaterialProfile::Generic, "", "" };
        impacts.clear();
    }

    void CreateGround()
    {
        const auto shapeResult = JPH::BoxShapeSettings(
            JPH::Vec3(20.0f, 0.05f, 20.0f), 0.01f).Create();
        if (shapeResult.HasError())
            throw std::runtime_error("Jolt ground shape failed: " +
                std::string(shapeResult.GetError().c_str()));
        const float y = groundHeight * 0.01f - 0.05f;
        JPH::BodyCreationSettings settings(
            shapeResult.Get(), JPH::Vec3(0, y, 0), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, StaticLayer);
        groundBody = world.GetBodyInterface().CreateAndAddBody(
            settings, JPH::EActivation::DontActivate);
        if (groundBody.IsInvalid())
            throw std::runtime_error("Jolt ground body budget exhausted");
        identities[BodyKey(groundBody)] = {
            BodyIdentity::Shape::Ground, FractureBodyRole::Ground,
            FractureMaterialProfile::Generic, "", "" };
    }

    void ReplaceGround()
    {
        if (!groundBody.IsInvalid())
        {
            identities.erase(BodyKey(groundBody));
            world.GetBodyInterface().RemoveBody(groundBody);
            world.GetBodyInterface().DestroyBody(groundBody);
            groundBody = {};
        }
        CreateGround();
    }

    void RecordContact(const JPH::Body& first, const JPH::Body& second,
        const JPH::ContactManifold& manifold)
    {
        const auto firstIdentity = identities.find(BodyKey(first.GetID()));
        const auto secondIdentity = identities.find(BodyKey(second.GetID()));
        if (firstIdentity == identities.end() || secondIdentity == identities.end())
            return;
        const BodyIdentity& firstBody = firstIdentity->second;
        const BodyIdentity& secondBody = secondIdentity->second;
        const bool firstProjectile =
            firstBody.role == FractureBodyRole::Projectile;
        const bool secondProjectile =
            secondBody.role == FractureBodyRole::Projectile;

        FractureImpact impact;
        const JPH::Vec3 normal = manifold.mWorldSpaceNormal;
        // 接触点相对 mBaseOffset，而不是刚体质心；用 manifold 的官方访问器避免
        // 把中心点加上相对偏移后得到错位的破口/气团位置。
        const JPH::RVec3 contactPoint = manifold.GetWorldSpaceContactPointOn1(0);
        impact.point = ToCentimeters(contactPoint);
        impact.normal = ToCentimeters(normal);
        impact.normal.normalize();

        // 投射物与任意目标接触：角色只影响 targetRole，消费方按 profile 处理。
        if (firstProjectile != secondProjectile)
        {
            const JPH::Body& projectile = firstProjectile ? first : second;
            const BodyIdentity& target = firstProjectile ? secondBody : firstBody;
            const uint32_t projectileKey = BodyKey(projectile.GetID());
            if (!impactedProjectiles.insert(projectileKey).second) return;
            const JPH::Body& targetBody = firstProjectile ? second : first;
            const JPH::Vec3 relativeVelocity =
                projectile.GetLinearVelocity() - targetBody.GetLinearVelocity();
            impact.targetRole = target.role;
            impact.contact = FractureContactKind::Impact;
            impact.materialProfile = target.profile;
            impact.targetBodyId = BodyKey(targetBody.GetID());
            impact.objectName = target.objectName;
            impact.shardName = target.shardName;
            impact.relativeSpeed = relativeVelocity.Length();
            impact.normalSpeed =
                std::abs(relativeVelocity.Dot(manifold.mWorldSpaceNormal));
            impact.projectileId = projectileKey;
            impacts.push_back(std::move(impact));
            return;
        }

        // 自由碎片与地面/另一自由碎片：每次新接触都上报，节流归消费方。
        const bool firstFree = firstBody.role == FractureBodyRole::FreeShard;
        const bool secondFree = secondBody.role == FractureBodyRole::FreeShard;
        if (firstFree && secondBody.role == FractureBodyRole::Ground)
        {
            impact.targetRole = FractureBodyRole::FreeShard;
            impact.contact = FractureContactKind::Resting;
            impact.materialProfile = firstBody.profile;
            impact.targetBodyId = BodyKey(first.GetID());
            impact.objectName = firstBody.objectName;
            impact.shardName = firstBody.shardName;
            impact.relativeSpeed = first.GetLinearVelocity().Length();
            impacts.push_back(std::move(impact));
            return;
        }
        if (secondFree && firstBody.role == FractureBodyRole::Ground)
        {
            impact.targetRole = FractureBodyRole::FreeShard;
            impact.contact = FractureContactKind::Resting;
            impact.materialProfile = secondBody.profile;
            impact.targetBodyId = BodyKey(second.GetID());
            impact.objectName = secondBody.objectName;
            impact.shardName = secondBody.shardName;
            impact.relativeSpeed = second.GetLinearVelocity().Length();
            impacts.push_back(std::move(impact));
            return;
        }
        if (firstFree && secondFree)
        {
            const bool firstIsTarget = BodyKey(first.GetID()) < BodyKey(second.GetID());
            const BodyIdentity& target = firstIsTarget ? firstBody : secondBody;
            const JPH::Body& targetBody = firstIsTarget ? first : second;
            const JPH::Body& otherBody = firstIsTarget ? second : first;
            impact.targetRole = FractureBodyRole::FreeShard;
            impact.contact = FractureContactKind::Impact;
            impact.materialProfile = target.profile;
            impact.targetBodyId = BodyKey(targetBody.GetID());
            impact.objectName = target.objectName;
            impact.shardName = target.shardName;
            const JPH::Vec3 relativeVelocity =
                targetBody.GetLinearVelocity() - otherBody.GetLinearVelocity();
            impact.relativeSpeed = relativeVelocity.Length();
            impact.normalSpeed =
                std::abs(relativeVelocity.Dot(manifold.mWorldSpaceNormal));
            impacts.push_back(std::move(impact));
        }
    }
};

FractureImpactWorld::FractureImpactWorld()
    : impl(std::make_unique<Impl>())
{
}

FractureImpactWorld::~FractureImpactWorld() = default;

void FractureImpactWorld::RegisterSphereProxy(const std::string& name,
    const Eigen::Vector3f& center, float radius,
    FractureMaterialProfile profile)
{
    if (name.empty() || radius <= 0.0f)
        throw std::invalid_argument("FractureImpactWorld requires a named positive sphere proxy");
    const auto shapeResult = JPH::SphereShapeSettings(radius * 0.01f).Create();
    if (shapeResult.HasError())
        throw std::runtime_error("Jolt sphere proxy shape failed: " +
            std::string(shapeResult.GetError().c_str()));
    JPH::BodyCreationSettings settings(
        shapeResult.Get(), ToMeters(center), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, StaticLayer);
    const JPH::BodyID id = impl->world.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::DontActivate);
    if (id.IsInvalid())
        throw std::runtime_error("Jolt sphere proxy body budget exhausted");
    impl->identities[BodyKey(id)] = {
        Impl::BodyIdentity::Shape::Sphere, FractureBodyRole::StaticTarget,
        profile, name, "" };
    impl->sphereBodies.push_back(id);
}

void FractureImpactWorld::UnregisterSphereProxy(const std::string& name)
{
    for (auto it = impl->sphereBodies.begin(); it != impl->sphereBodies.end();)
    {
        const auto identity = impl->identities.find(BodyKey(*it));
        if (identity != impl->identities.end() &&
            identity->second.objectName == name)
        {
            impl->world.GetBodyInterface().RemoveBody(*it);
            impl->world.GetBodyInterface().DestroyBody(*it);
            impl->identities.erase(identity);
            it = impl->sphereBodies.erase(it);
        }
        else ++it;
    }
}

void FractureImpactWorld::SetSphereProxyPose(
    const std::string& name, const Eigen::Vector3f& center)
{
    for (const JPH::BodyID& id : impl->sphereBodies)
    {
        const auto identity = impl->identities.find(BodyKey(id));
        if (identity == impl->identities.end() ||
            identity->second.objectName != name)
            continue;
        impl->world.GetBodyInterface().SetPositionAndRotation(
            id, ToMeters(center), JPH::Quat::sIdentity(),
            JPH::EActivation::Activate);
        return;
    }
}

void FractureImpactWorld::RegisterPane(const std::string& name,
    const Eigen::Vector3f& center, const Eigen::Quaternionf& rotation,
    const Eigen::Vector3f& halfExtents, FractureMaterialProfile profile)
{
    if (name.empty() || halfExtents.minCoeff() <= 0.0f ||
        !halfExtents.allFinite() || !rotation.coeffs().allFinite())
        throw std::invalid_argument(
            "FractureImpactWorld requires a named pane with positive half extents");
    const auto shapeResult = JPH::BoxShapeSettings(
        ToMeters(halfExtents), 0.01f).Create();
    if (shapeResult.HasError())
        throw std::runtime_error("Jolt pane proxy shape failed: " +
            std::string(shapeResult.GetError().c_str()));
    JPH::BodyCreationSettings settings(
        shapeResult.Get(), ToMeters(center), ToJolt(rotation.normalized()),
        JPH::EMotionType::Static, StaticLayer);
    const JPH::BodyID id = impl->world.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::DontActivate);
    if (id.IsInvalid())
        throw std::runtime_error("Jolt pane proxy body budget exhausted");
    impl->identities[BodyKey(id)] = {
        Impl::BodyIdentity::Shape::Pane, FractureBodyRole::StaticTarget,
        profile, name, "" };
    impl->paneBodies.push_back(id);
}

void FractureImpactWorld::UnregisterPane(const std::string& name)
{
    for (auto it = impl->paneBodies.begin(); it != impl->paneBodies.end();)
    {
        const auto identity = impl->identities.find(BodyKey(*it));
        if (identity != impl->identities.end() &&
            identity->second.objectName == name)
        {
            impl->world.GetBodyInterface().RemoveBody(*it);
            impl->world.GetBodyInterface().DestroyBody(*it);
            impl->identities.erase(identity);
            it = impl->paneBodies.erase(it);
        }
        else ++it;
    }
}

void FractureImpactWorld::RegisterShard(const std::string& objectName,
    const std::string& shardName, const Eigen::Vector3f& center,
    const Eigen::Quaternionf& rotation, const Eigen::Vector3f& velocity,
    float mass, const std::vector<Eigen::Vector3f>& localHullPoints,
    FractureMaterialProfile profile, float friction, float restitution,
    const Eigen::Vector3f& angularVelocity,
    bool dynamic)
{
    if (objectName.empty() || shardName.empty() || mass <= 0.0f ||
        !rotation.coeffs().allFinite())
        throw std::invalid_argument(
            "FractureImpact shard requires names, positive mass and finite rotation");
    JPH::ShapeRefC shape;
    if (localHullPoints.size() >= 4)
    {
        JPH::Array<JPH::Vec3> points;
        const size_t stride = std::max<size_t>(1, localHullPoints.size() / 240);
        for (size_t index = 0; index < localHullPoints.size(); index += stride)
            points.push_back(ToMeters(localHullPoints[index]));
        const auto hullResult = JPH::ConvexHullShapeSettings(points, 0.005f).Create();
        if (!hullResult.HasError()) shape = hullResult.Get();
    }
    if (!shape)
    {
        float radius = 0.0f;
        for (const Eigen::Vector3f& point : localHullPoints)
            radius = std::max(radius, point.norm());
        radius = std::max(radius, 1.0f);
        const auto fallback = JPH::SphereShapeSettings(radius * 0.01f).Create();
        if (fallback.HasError())
            throw std::runtime_error("Jolt shard shape failed: " +
                std::string(fallback.GetError().c_str()));
        shape = fallback.Get();
    }
    JPH::BodyCreationSettings settings(
        shape, ToMeters(center), ToJolt(rotation.normalized()),
        dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
        dynamic ? MovingLayer : StaticLayer);
    settings.mLinearVelocity = ToMeters(velocity);
    // 角速度单位是 rad/s，不参与厘米到米的换算。
    if (dynamic)
    {
        settings.mAngularVelocity =
            JPH::Vec3(angularVelocity.x(), angularVelocity.y(), angularVelocity.z());
        settings.mFriction = friction;
        settings.mRestitution = restitution;
        settings.mAllowSleeping = true;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = mass;
    }
    const JPH::BodyID id = impl->world.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::Activate);
    if (id.IsInvalid())
        throw std::runtime_error("Jolt shard body budget exhausted");
    impl->identities[BodyKey(id)] = {
        dynamic ? Impl::BodyIdentity::Shape::Shard
                : Impl::BodyIdentity::Shape::AttachedShard,
        dynamic ? FractureBodyRole::FreeShard
                : FractureBodyRole::AttachedPart,
        profile, objectName, shardName };
    impl->shardBodies.push_back(id);
    impl->shardIds[objectName + "/" + shardName] = id;
}

void FractureImpactWorld::RemoveShard(
    const std::string& objectName, const std::string& shardName)
{
    const std::string key = objectName + "/" + shardName;
    const auto found = impl->shardIds.find(key);
    if (found == impl->shardIds.end()) return;
    const JPH::BodyID id = found->second;
    impl->world.GetBodyInterface().RemoveBody(id);
    impl->world.GetBodyInterface().DestroyBody(id);
    impl->identities.erase(BodyKey(id));
    impl->shardIds.erase(found);
    impl->shardBodies.erase(
        std::remove(impl->shardBodies.begin(), impl->shardBodies.end(), id),
        impl->shardBodies.end());
}

void FractureImpactWorld::SetGroundPlane(float heightCentimeters)
{
    if (!std::isfinite(heightCentimeters))
        throw std::invalid_argument("FractureImpact ground height is not finite");
    impl->groundHeight = heightCentimeters;
    impl->ReplaceGround();
}

std::optional<FractureShardPose> FractureImpactWorld::GetShardPose(
    const std::string& objectName, const std::string& shardName) const
{
    const auto found = impl->shardIds.find(objectName + "/" + shardName);
    if (found == impl->shardIds.end()) return std::nullopt;
    JPH::BodyLockRead lock(impl->world.GetBodyLockInterface(), found->second);
    if (!lock.Succeeded()) return std::nullopt;
    const JPH::Body& body = lock.GetBody();
    const JPH::Vec3 position = body.GetCenterOfMassPosition();
    const JPH::Quat rotation = body.GetRotation();
    FractureShardPose pose;
    pose.position = ToCentimeters(position);
    pose.rotation = Eigen::Quaternionf(
        rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
    return pose;
}

std::optional<Eigen::Vector3f> FractureImpactWorld::GetShardVelocity(
    const std::string& objectName, const std::string& shardName) const
{
    const auto found = impl->shardIds.find(objectName + "/" + shardName);
    if (found == impl->shardIds.end()) return std::nullopt;
    JPH::BodyLockRead lock(impl->world.GetBodyLockInterface(), found->second);
    if (!lock.Succeeded()) return std::nullopt;
    return ToCentimeters(lock.GetBody().GetLinearVelocity());
}

void FractureImpactWorld::Clear()
{
    impl->Clear();
}

uint32_t FractureImpactWorld::LaunchProjectile(const Eigen::Vector3f& position,
    const Eigen::Vector3f& velocity, float radius, float mass)
{
    if (radius <= 0.0f || mass <= 0.0f)
        throw std::invalid_argument(
            "FractureImpact projectile requires positive radius and mass");
    const auto shapeResult = JPH::SphereShapeSettings(radius * 0.01f).Create();
    if (shapeResult.HasError())
        throw std::runtime_error("Jolt projectile shape failed: " +
            std::string(shapeResult.GetError().c_str()));
    JPH::BodyCreationSettings settings(
        shapeResult.Get(), ToMeters(position), JPH::Quat::sIdentity(),
        JPH::EMotionType::Dynamic, MovingLayer);
    settings.mLinearVelocity = ToMeters(velocity);
    // 玻璃板很薄，高速投射物必须线性连续检测，否则会直接穿过去。
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    settings.mFriction = 0.2f;
    settings.mRestitution = 0.1f;
    settings.mAllowSleeping = false;
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = mass;
    const JPH::BodyID id = impl->world.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::Activate);
    if (id.IsInvalid())
        throw std::runtime_error("Jolt projectile body budget exhausted");
    impl->projectiles.push_back(id);
    impl->identities[BodyKey(id)] = {
        Impl::BodyIdentity::Shape::Projectile, FractureBodyRole::Projectile,
        FractureMaterialProfile::Generic, "", "" };
    impl->projectileIds[BodyKey(id)] = id;
    return BodyKey(id);
}

bool FractureImpactWorld::HasProjectile(uint32_t projectileId) const
{
    const auto found = impl->identities.find(projectileId);
    return found != impl->identities.end() &&
        found->second.role == FractureBodyRole::Projectile;
}

void FractureImpactWorld::CancelProjectile(uint32_t projectileId)
{
    const auto found = impl->projectileIds.find(projectileId);
    if (found == impl->projectileIds.end()) return;
    const JPH::BodyID id = found->second;
    impl->world.GetBodyInterface().RemoveBody(id);
    impl->world.GetBodyInterface().DestroyBody(id);
    impl->identities.erase(projectileId);
    impl->projectileIds.erase(found);
    impl->impactedProjectiles.erase(projectileId);
    impl->projectiles.erase(
        std::remove(impl->projectiles.begin(), impl->projectiles.end(), id),
        impl->projectiles.end());
}

std::optional<FractureShardPose> FractureImpactWorld::GetProjectilePose(
    uint32_t projectileId) const
{
    const auto found = impl->projectileIds.find(projectileId);
    if (found == impl->projectileIds.end()) return std::nullopt;
    JPH::BodyLockRead lock(impl->world.GetBodyLockInterface(), found->second);
    if (!lock.Succeeded()) return std::nullopt;
    const JPH::Body& body = lock.GetBody();
    const JPH::Vec3 position = body.GetCenterOfMassPosition();
    const JPH::Quat rotation = body.GetRotation();
    FractureShardPose pose;
    pose.position = ToCentimeters(position);
    pose.rotation = Eigen::Quaternionf(
        rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
    return pose;
}

std::optional<Eigen::Vector3f> FractureImpactWorld::GetProjectileVelocity(
    uint32_t projectileId) const
{
    const auto found = impl->projectileIds.find(projectileId);
    if (found == impl->projectileIds.end()) return std::nullopt;
    JPH::BodyLockRead lock(impl->world.GetBodyLockInterface(), found->second);
    if (!lock.Succeeded()) return std::nullopt;
    return ToCentimeters(lock.GetBody().GetLinearVelocity());
}

std::vector<FractureImpact> FractureImpactWorld::Step(float deltaSeconds)
{
    if (deltaSeconds <= 0.0f) return {};
    impl->accumulated += deltaSeconds;
    const int requested = static_cast<int>(std::floor(impl->accumulated / FixedStep));
    const int substeps = std::min(requested, MaxSubsteps);
    impl->accumulated -= static_cast<double>(substeps) * FixedStep;
    for (int step = 0; step < substeps; ++step)
    {
        if (impl->world.Update(FixedStep, 1, &impl->allocator, &impl->jobs) !=
            JPH::EPhysicsUpdateError::None)
            throw std::runtime_error("FractureImpactWorld physics capacity exceeded");
    }

    std::vector<FractureImpact> result = std::move(impl->impacts);
    impl->impacts.clear();
    for (const JPH::BodyID& id : impl->projectiles)
    {
        if (!impl->impactedProjectiles.count(BodyKey(id))) continue;
        impl->world.GetBodyInterface().RemoveBody(id);
        impl->world.GetBodyInterface().DestroyBody(id);
        impl->identities.erase(BodyKey(id));
        impl->projectileIds.erase(BodyKey(id));
    }
    impl->projectiles.erase(
        std::remove_if(impl->projectiles.begin(), impl->projectiles.end(),
            [this](const JPH::BodyID& id)
            {
                return impl->impactedProjectiles.count(BodyKey(id)) != 0;
            }),
        impl->projectiles.end());
    impl->impactedProjectiles.clear();
    return result;
}

} // namespace VL::Physics
