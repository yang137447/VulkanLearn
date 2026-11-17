#include "terrain/terrainCollision.h"

#include <stdexcept>
#include <vector>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "physics/joltRegistration.h"

namespace VL
{
namespace
{

// 与 ScriptPhysicsWorld 一致：Jolt 世界用米，引擎/资产用厘米。
constexpr float CmToMeter = 0.01f;
constexpr JPH::ObjectLayer StaticLayer = 0;
constexpr JPH::ObjectLayer MovingLayer = 1;

void ValidateSettings(const TerrainCollisionSettings& settings)
{
    if (settings.bitsPerSample < 1 || settings.bitsPerSample > 16)
        throw std::invalid_argument("Terrain collision bitsPerSample must be in [1, 16]");
    if (settings.objectLayerCount == 0 || settings.objectLayer >= settings.objectLayerCount)
        throw std::invalid_argument(
            "Terrain collision objectLayer must be inside the registered layer count");
}

void ValidateProxy(const TerrainCollisionProxy& proxy)
{
    if (!proxy.IsValid())
        throw std::invalid_argument("Terrain collision proxy is invalid");
}

JPH::ShapeRefC BuildHeightFieldShape(
    const TerrainCollisionProxy& proxy,
    const TerrainCollisionSettings& settings)
{
    const uint32_t sampleCount = proxy.sampleCount;
    std::vector<float> samples(proxy.heightCm.size());
    for (size_t index = 0; index < proxy.heightCm.size(); ++index)
        samples[index] = proxy.heightCm[index] * CmToMeter;

    const float inverseCountMinusOne = 1.0f / static_cast<float>(sampleCount - 1);
    const float extentX = proxy.sizeXCm;
    const float extentZ = proxy.sizeZCm;
    // HeightFieldShape 的局部表面是 offset + scale * (x, sample, y)，x/y 是样本索引。
    // 用 offset/scale 把索引映射回地形局部空间 [-size/2, +size/2]，保持 Y 为米。
    const JPH::Vec3 offset(
        -0.5f * extentX * CmToMeter,
        0.0f,
        -0.5f * extentZ * CmToMeter);
    const JPH::Vec3 scale(
        extentX * inverseCountMinusOne * CmToMeter,
        1.0f,
        extentZ * inverseCountMinusOne * CmToMeter);

    JPH::HeightFieldShapeSettings shapeSettings(
        samples.data(), offset, scale, sampleCount);
    shapeSettings.mBitsPerSample = settings.bitsPerSample;
    shapeSettings.mBlockSize = TerrainCollisionHeightFieldBlockSize;
    const JPH::ShapeSettings::ShapeResult result = shapeSettings.Create();
    if (result.HasError())
        throw std::runtime_error(
            std::string("Terrain collision heightfield shape failed: ") + result.GetError().c_str());
    return result.Get();
}

// 与 CommonFunction::RotationToQuat 相同的 Y*X*Z 顺序；Jolt 用弧度。
JPH::Quat PlacementRotation(const Eigen::Vector3f& rotationDegrees)
{
    constexpr float DegreesToRadians = 3.14159265358979323846f / 180.0f;
    const Eigen::Vector3f radians = rotationDegrees * DegreesToRadians;
    return JPH::Quat::sRotation(JPH::Vec3::sAxisY(), radians.y()) *
        JPH::Quat::sRotation(JPH::Vec3::sAxisX(), radians.x()) *
        JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), radians.z());
}

JPH::AABox BodyWorldBounds(const JPH::PhysicsSystem& world, JPH::BodyID id)
{
    JPH::BodyLockRead lock(world.GetBodyLockInterface(), id);
    return lock.GetBody().GetWorldSpaceBounds();
}

} // namespace

struct TerrainCollisionBody::Impl
{
    JPH::PhysicsSystem* world = nullptr;
    JPH::BodyID id;
    uint32_t sampleCount = 0;
    Eigen::Vector3f boundsMinCm = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMaxCm = Eigen::Vector3f::Zero();
};

TerrainCollisionBody::TerrainCollisionBody() = default;
TerrainCollisionBody::~TerrainCollisionBody()
{
    Reset();
}
TerrainCollisionBody::TerrainCollisionBody(TerrainCollisionBody&& other) noexcept = default;
TerrainCollisionBody& TerrainCollisionBody::operator=(TerrainCollisionBody&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        impl = std::move(other.impl);
    }
    return *this;
}

void TerrainCollisionBody::Reset()
{
    if (!impl)
        return;
    if (impl->world != nullptr && !impl->id.IsInvalid())
    {
        JPH::BodyInterface& bodies = impl->world->GetBodyInterface();
        if (bodies.IsAdded(impl->id))
        {
            // 先移出 broadphase 再销毁，避免静态体残留查询代理。
            bodies.RemoveBody(impl->id);
            bodies.DestroyBody(impl->id);
        }
    }
    impl.reset();
}

bool TerrainCollisionBody::IsValid() const
{
    return impl != nullptr && impl->world != nullptr && !impl->id.IsInvalid();
}

uint32_t TerrainCollisionBody::GetBodyIdValue() const
{
    return impl == nullptr || impl->id.IsInvalid()
        ? 0xFFFFFFFFu
        : impl->id.GetIndexAndSequenceNumber();
}

uint32_t TerrainCollisionBody::GetSampleCount() const
{
    return impl == nullptr ? 0 : impl->sampleCount;
}

uint32_t TerrainCollisionBody::GetQuadCount() const
{
    if (impl == nullptr || impl->sampleCount < 2)
        return 0;
    const uint32_t side = impl->sampleCount - 1;
    return side * side;
}

Eigen::Vector3f TerrainCollisionBody::GetWorldBoundsMinCm() const
{
    return impl == nullptr ? Eigen::Vector3f::Zero() : impl->boundsMinCm;
}

Eigen::Vector3f TerrainCollisionBody::GetWorldBoundsMaxCm() const
{
    return impl == nullptr ? Eigen::Vector3f::Zero() : impl->boundsMaxCm;
}

TerrainCollisionBody CreateTerrainCollisionBody(
    JPH::PhysicsSystem& world,
    const TerrainBuildPlan& plan,
    const TerrainCollisionPlacement& placement,
    const TerrainCollisionSettings& settings)
{
    const std::shared_ptr<const TerrainCollisionProxy> proxy =
        BuildTerrainCollisionProxy(
            plan, placement, settings.sampleCount);
    return CreateTerrainCollisionBody(world, *proxy, settings);
}

TerrainCollisionBody CreateTerrainCollisionBody(
    JPH::PhysicsSystem& world,
    const TerrainCollisionProxy& proxy,
    const TerrainCollisionSettings& settings)
{
    ValidateProxy(proxy);
    ValidateSettings(settings);
    Physics::EnsureJoltRegistered();

    const JPH::ShapeRefC shape = BuildHeightFieldShape(proxy, settings);

    const JPH::RVec3 position(
        proxy.placement.position.x() * CmToMeter,
        proxy.placement.position.y() * CmToMeter,
        proxy.placement.position.z() * CmToMeter);
    JPH::BodyCreationSettings bodySettings(
        shape, position, PlacementRotation(proxy.placement.rotation),
        JPH::EMotionType::Static, static_cast<JPH::ObjectLayer>(settings.objectLayer));
    // 静态地形不参与睡眠/唤醒；摩擦取 1.0 让角色/刚体在坡面上不无限滑动。
    bodySettings.mAllowSleeping = false;
    bodySettings.mFriction = 1.0f;
    // 脚本 body 用 CollisionGroup(collisionLayer, collisionMask) 过滤；地形属于
    // 组 0 且对所有子组开放，才能被任意 collisionLayer/Mask 的脚本查询命中。
    // 不设置会得到 Jolt 的 cInvalidGroup，GroupFilter 的位移运算可能出现未定义行为。
    bodySettings.mCollisionGroup = JPH::CollisionGroup(
        nullptr, 0, JPH::CollisionGroup::cInvalidSubGroup);

    const JPH::BodyID id = world.GetBodyInterface().CreateAndAddBody(
        bodySettings, JPH::EActivation::DontActivate);
    if (id.IsInvalid())
        throw std::runtime_error("Terrain collision body creation failed");

    auto impl = std::make_unique<TerrainCollisionBody::Impl>();
    impl->world = &world;
    impl->id = id;
    impl->sampleCount = proxy.sampleCount;
    {
        const JPH::AABox bounds = BodyWorldBounds(world, id);
        impl->boundsMinCm = Eigen::Vector3f(
            bounds.mMin.GetX(), bounds.mMin.GetY(), bounds.mMin.GetZ()) / CmToMeter;
        impl->boundsMaxCm = Eigen::Vector3f(
            bounds.mMax.GetX(), bounds.mMax.GetY(), bounds.mMax.GetZ()) / CmToMeter;
    }

    TerrainCollisionBody body;
    body.impl = std::move(impl);
    return body;
}

struct TerrainCollisionWorld::Impl
{
    // Jolt 的 Allocate() 依赖 RegisterDefaultAllocator() 写入的函数指针；
    // TempAllocatorImpl 在成员初始化阶段就会分配，因此注册必须是第一个成员。
    struct RegistrationGuard
    {
        RegistrationGuard()
        {
            Physics::EnsureJoltRegistered();
        }
    };
    RegistrationGuard registration;
    JPH::BroadPhaseLayerInterfaceTable layers{2, 2};
    JPH::ObjectLayerPairFilterTable pairs{2};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broadphaseFilter;
    JPH::TempAllocatorImpl allocator{16 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem world;

    Impl()
    {
        layers.MapObjectToBroadPhaseLayer(StaticLayer, JPH::BroadPhaseLayer(0));
        layers.MapObjectToBroadPhaseLayer(MovingLayer, JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(StaticLayer, MovingLayer);
        pairs.EnableCollision(MovingLayer, MovingLayer);
        broadphaseFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
            layers, 2, pairs, 2);
        world.Init(4096, 0, 16384, 16384, layers, *broadphaseFilter, pairs);
        world.SetGravity({0, -9.81f, 0});
        auto physicsSettings = world.GetPhysicsSettings();
        physicsSettings.mPenetrationSlop = 0.001f;
        world.SetPhysicsSettings(physicsSettings);
    }
};

TerrainCollisionWorld::TerrainCollisionWorld()
    : impl(std::make_unique<Impl>())
{
}

TerrainCollisionWorld::~TerrainCollisionWorld() = default;

JPH::PhysicsSystem& TerrainCollisionWorld::GetPhysicsSystem()
{
    return impl->world;
}

const JPH::PhysicsSystem& TerrainCollisionWorld::GetPhysicsSystem() const
{
    return impl->world;
}

TerrainCollisionBody TerrainCollisionWorld::AddTerrain(
    const TerrainBuildPlan& plan,
    const TerrainCollisionPlacement& placement,
    const TerrainCollisionSettings& settings)
{
    return CreateTerrainCollisionBody(impl->world, plan, placement, settings);
}

bool TerrainCollisionWorld::RaycastDown(
    const Eigen::Vector3f& originCm,
    float distanceCm,
    Eigen::Vector3f& outPointCm,
    Eigen::Vector3f* outNormal) const
{
    if (!(distanceCm > 0.0f))
        return false;
    const JPH::RVec3 origin(
        originCm.x() * CmToMeter, originCm.y() * CmToMeter, originCm.z() * CmToMeter);
    const JPH::RRayCast ray(origin, JPH::Vec3(0.0f, -distanceCm * CmToMeter, 0.0f));
    JPH::RayCastResult hit;
    if (!impl->world.GetNarrowPhaseQuery().CastRay(ray, hit))
        return false;
    const JPH::Vec3 point = ray.GetPointOnRay(hit.mFraction);
    outPointCm = Eigen::Vector3f(point.GetX(), point.GetY(), point.GetZ()) / CmToMeter;
    if (outNormal != nullptr)
    {
        JPH::BodyLockRead lock(impl->world.GetBodyLockInterface(), hit.mBodyID);
        const JPH::Vec3 normal =
            lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
        *outNormal = Eigen::Vector3f(normal.GetX(), normal.GetY(), normal.GetZ());
    }
    return true;
}

void TerrainCollisionWorld::Step(float deltaSeconds)
{
    if (!(deltaSeconds > 0.0f))
        return;
    impl->world.Update(deltaSeconds, 1, &impl->allocator, &impl->jobs);
}

} // namespace VL
