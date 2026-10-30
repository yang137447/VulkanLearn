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
        enum class Kind
        {
            Sphere,
            Pane,
            Shard,
            AttachedShard,
            Projectile,
            Ground
        };
        Kind kind = Kind::Projectile;
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
    std::set<uint32_t> landingBodies;
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
        landingBodies.clear();
        identities.clear();
        if (groundBody.IsInvalid()) CreateGround();
        else identities[BodyKey(groundBody)] = {
            BodyIdentity::Kind::Ground, FractureMaterialProfile::Generic,
            "", "" };
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
            BodyIdentity::Kind::Ground, FractureMaterialProfile::Generic,
            "", "" };
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
        const bool firstSphere = firstBody.kind == BodyIdentity::Kind::Sphere;
        const bool secondSphere = secondBody.kind == BodyIdentity::Kind::Sphere;
        const bool firstPane = firstBody.kind == BodyIdentity::Kind::Pane;
        const bool secondPane = secondBody.kind == BodyIdentity::Kind::Pane;
        const bool firstShard = firstBody.kind == BodyIdentity::Kind::Shard;
        const bool secondShard = secondBody.kind == BodyIdentity::Kind::Shard;
        const bool firstAttached = firstBody.kind == BodyIdentity::Kind::AttachedShard;
        const bool secondAttached = secondBody.kind == BodyIdentity::Kind::AttachedShard;
        const bool firstGround = firstBody.kind == BodyIdentity::Kind::Ground;
        const bool secondGround = secondBody.kind == BodyIdentity::Kind::Ground;
        const bool firstProjectile = firstBody.kind == BodyIdentity::Kind::Projectile;
        const bool secondProjectile = secondBody.kind == BodyIdentity::Kind::Projectile;

        FractureImpact impact;
        const JPH::Vec3 normal = manifold.mWorldSpaceNormal;
        // 接触点相对 mBaseOffset，而不是刚体质心；用 manifold 的官方访问器避免
        // 把中心点加上相对偏移后得到错位的破口/气团位置。
        const JPH::RVec3 contactPoint = manifold.GetWorldSpaceContactPointOn1(0);
        impact.point = ToCentimeters(contactPoint);
        impact.normal = ToCentimeters(normal);
        impact.normal.normalize();

        if ((firstSphere && secondProjectile) || (secondSphere && firstProjectile))
        {
            const JPH::Body& projectile = firstSphere ? second : first;
            const uint32_t projectileKey = BodyKey(projectile.GetID());
            if (!impactedProjectiles.insert(projectileKey).second) return;
            impact.type = FractureImpactType::BalloonHit;
            impact.objectName = firstSphere ? firstBody.objectName : secondBody.objectName;
            impact.materialProfile = firstSphere
                ? firstBody.profile : secondBody.profile;
            impact.relativeSpeed = projectile.GetLinearVelocity().Length();
            impact.normalSpeed = std::abs(
                projectile.GetLinearVelocity().Dot(manifold.mWorldSpaceNormal));
            impact.projectileId = projectileKey;
            impacts.push_back(std::move(impact));
            return;
        }

        if ((firstPane && secondProjectile) || (secondPane && firstProjectile))
        {
            const JPH::Body& projectile = firstPane ? second : first;
            const uint32_t projectileKey = BodyKey(projectile.GetID());
            if (!impactedProjectiles.insert(projectileKey).second) return;
            impact.type = FractureImpactType::PaneHit;
            impact.objectName = firstPane ? firstBody.objectName : secondBody.objectName;
            impact.materialProfile = firstPane
                ? firstBody.profile : secondBody.profile;
            impact.relativeSpeed = projectile.GetLinearVelocity().Length();
            impact.normalSpeed = std::abs(
                projectile.GetLinearVelocity().Dot(manifold.mWorldSpaceNormal));
            impact.projectileId = projectileKey;
            impacts.push_back(std::move(impact));
            return;
        }

        if ((firstAttached && secondProjectile) || (secondAttached && firstProjectile))
        {
            const JPH::Body& projectile = firstAttached ? second : first;
            const uint32_t projectileKey = BodyKey(projectile.GetID());
            if (!impactedProjectiles.insert(projectileKey).second) return;
            const BodyIdentity& plate = firstAttached ? firstBody : secondBody;
            impact.type = FractureImpactType::AttachedHit;
            impact.materialProfile = plate.profile;
            impact.objectName = plate.objectName;
            impact.shardName = plate.shardName;
            impact.relativeSpeed = projectile.GetLinearVelocity().Length();
            impact.projectileId = projectileKey;
            impacts.push_back(std::move(impact));
            return;
        }

        if ((firstShard && secondProjectile) || (secondShard && firstProjectile))
        {
            // 箭命中已经掉到地上的动态碎片：这是“继续打碎”的入口。
            // 不能复用 landingBodies 去重，否则碎片落地后后续箭矢永远无效；
            // 每支投射物只允许产生一次命中，由 impactedProjectiles 负责。
            const JPH::Body& projectile = firstShard ? second : first;
            const JPH::Body& shardBody = firstShard ? first : second;
            const uint32_t projectileKey = BodyKey(projectile.GetID());
            if (!impactedProjectiles.insert(projectileKey).second) return;
            const BodyIdentity& shard = firstShard ? firstBody : secondBody;
            const JPH::Vec3 relativeVelocity =
                projectile.GetLinearVelocity() - shardBody.GetLinearVelocity();
            impact.type = FractureImpactType::ShardHit;
            impact.materialProfile = shard.profile;
            impact.objectName = shard.objectName;
            impact.shardName = shard.shardName;
            impact.relativeSpeed = relativeVelocity.Length();
            impact.normalSpeed = std::abs(
                relativeVelocity.Dot(manifold.mWorldSpaceNormal));
            impact.projectileId = projectileKey;
            impacts.push_back(std::move(impact));
            return;
        }

        if ((firstShard && secondGround) || (secondShard && firstGround))
        {
            const JPH::BodyID shardId = firstShard ? first.GetID() : second.GetID();
            if (!landingBodies.insert(BodyKey(shardId)).second) return;
            const BodyIdentity& shard = firstShard ? firstBody : secondBody;
            impact.type = FractureImpactType::ShardLanding;
            impact.materialProfile = shard.profile;
            impact.objectName = shard.objectName;
            impact.shardName = shard.shardName;
            impact.relativeSpeed = firstShard
                ? first.GetLinearVelocity().Length()
                : second.GetLinearVelocity().Length();
            impacts.push_back(std::move(impact));
            return;
        }

        if (firstShard && secondShard)
        {
            const JPH::BodyID firstShardId = first.GetID();
            const JPH::BodyID secondShardId = second.GetID();
            const JPH::BodyID shardId = BodyKey(firstShardId) < BodyKey(secondShardId)
                ? firstShardId : secondShardId;
            if (!landingBodies.insert(BodyKey(shardId)).second) return;
            const BodyIdentity& shard = BodyKey(firstShardId) < BodyKey(secondShardId)
                ? firstBody : secondBody;
            impact.type = FractureImpactType::ShardHit;
            impact.materialProfile = shard.profile;
            impact.objectName = shard.objectName;
            impact.shardName = shard.shardName;
            impact.relativeSpeed = std::max(
                first.GetLinearVelocity().Length(),
                second.GetLinearVelocity().Length());
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
        Impl::BodyIdentity::Kind::Sphere, profile, name, "" };
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
        Impl::BodyIdentity::Kind::Pane, profile, name, "" };
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
    FractureMaterialProfile profile,
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
        settings.mFriction = 0.35f;
        settings.mRestitution = 0.12f;
        settings.mAllowSleeping = true;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = mass;
    }
    const JPH::BodyID id = impl->world.GetBodyInterface().CreateAndAddBody(
        settings, JPH::EActivation::Activate);
    if (id.IsInvalid())
        throw std::runtime_error("Jolt shard body budget exhausted");
    impl->identities[BodyKey(id)] = {
        dynamic ? Impl::BodyIdentity::Kind::Shard : Impl::BodyIdentity::Kind::AttachedShard,
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
        Impl::BodyIdentity::Kind::Projectile,
        FractureMaterialProfile::Generic, "", "" };
    impl->projectileIds[BodyKey(id)] = id;
    return BodyKey(id);
}

bool FractureImpactWorld::HasProjectile(uint32_t projectileId) const
{
    const auto found = impl->identities.find(projectileId);
    return found != impl->identities.end() &&
        found->second.kind == Impl::BodyIdentity::Kind::Projectile;
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
    impl->landingBodies.clear();
    return result;
}

} // namespace VL::Physics
