#include "script/scriptPhysics.h"
#include "script/scriptComponents.h"
#include "physics/runtime/joltRuntime.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#endif

namespace VL
{
#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
namespace
{
// 1/60 只是刚体积分子步，不再是 gameplay tick；角色时间轴完全由帧 dt 决定。
constexpr double RigidSubstep = 1.0 / 60.0;
constexpr double MaxRigidSubsteps = 16.0;
// 角色 CMC 式内部迭代：对应 UE 的 MaxSimulationTimeStep=0.05 / MaxSimulationIterations=8。
constexpr float MaxCharacterIteration = 0.05f;
constexpr int MaxCharacterIterations = 8;
constexpr JPH::ObjectLayer StaticLayer = 0, MovingLayer = 1;

JPH::Vec3 PhysicsVector(const nlohmann::json& node, const char* field, float factor = 0.01f)
{
    const Eigen::Vector3f value = ScriptVector(node, field) * factor;
    return {value.x(), value.y(), value.z()};
}
JPH::Quat PhysicsRotation(const nlohmann::json& state)
{
    // 与 Blueprint/组件矩阵一致：先局部 Z，再 X，最后世界 Y；不直接套用其它后端的 Euler 约定。
    const Eigen::Vector3f angles = ScriptVector(state, "rotation") * (3.14159265358979323846f / 180);
    return JPH::Quat::sRotation(JPH::Vec3::sAxisY(), angles.y()) *
        JPH::Quat::sRotation(JPH::Vec3::sAxisX(), angles.x()) *
        JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), angles.z());
}
nlohmann::json VectorJson(JPH::Vec3Arg value, float factor = 100)
{
    return {value.GetX() * factor, value.GetY() * factor, value.GetZ() * factor};
}
JPH::ShapeRefC CreateShape(const nlohmann::json& physics, float scale)
{
    const auto kind = physics.value("shape", "box");
    JPH::ShapeSettings::ShapeResult result;
    if (kind == "box")
    {
        // BoxShape 的默认圆角有最小尺寸假设；按已验证尺寸缩小圆角，不改变作者 halfExtents。
        const auto half = PhysicsVector(physics, "halfExtents", 0.01f * scale);
        result = JPH::BoxShapeSettings(half, std::min(0.05f, half.ReduceMin() * 0.1f)).Create();
    }
    else if (kind == "capsule")
        result = JPH::CapsuleShapeSettings(physics.at("halfHeight").get<float>() * 0.01f * scale,
            physics.at("radius").get<float>() * 0.01f * scale).Create();
    else result = JPH::SphereShapeSettings(physics.at("radius").get<float>() * 0.01f * scale).Create();
    if (result.HasError()) throw std::runtime_error("Script.PhysicsShape: " + std::string(result.GetError().c_str()));
    return result.Get();
}

class QueryBodyFilter final : public JPH::BodyFilter
{
public:
    JPH::BodyID ignore;
    bool includeTriggers = false;
    uint32_t mask = 65535;
    int layer = -1;
    bool ShouldCollide(const JPH::BodyID& id) const override { return id != ignore; }
    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
        return (includeTriggers || !body.IsSensor()) &&
            (mask & (1u << body.GetCollisionGroup().GetGroupID())) != 0 &&
            (layer < 0 || (body.GetCollisionGroup().GetSubGroupID() & (1u << layer)) != 0);
    }
};

class ScriptCollisionGroupFilter final : public JPH::GroupFilter
{
public:
    bool CanCollide(const JPH::CollisionGroup& first, const JPH::CollisionGroup& second) const override
    {
        return (first.GetSubGroupID() & (1u << second.GetGroupID())) != 0 &&
            (second.GetSubGroupID() & (1u << first.GetGroupID())) != 0;
    }
};
}

struct ScriptPhysicsWorld::Impl
{
    struct Body
    {
        JPH::ShapeRefC shape;
        JPH::BodyID id;
        JPH::Ref<JPH::CharacterVirtual> character;
        nlohmann::json last;
        std::string motion;
        JPH::Vec3 walk = JPH::Vec3::sZero();
        JPH::Vec3 rootMotion = JPH::Vec3::sZero();
        JPH::Vec3 achievedVelocity = JPH::Vec3::sZero();
        float jumpSpeed = 5, stepHeight = 0.3f;
        bool trigger = false, jump = false, refreshContacts = true;
        bool rootMotionActive = false;
        uint32_t layer = 0, mask = 65535;
        std::string compoundSignature;
    };
    Physics::JoltRegistrationToken registration;
    JPH::BroadPhaseLayerInterfaceTable layers{2, 2};
    JPH::ObjectLayerPairFilterTable pairs{2};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broadphaseFilter;
    JPH::TempAllocatorImpl allocator{16 * 1024 * 1024};
    // 物理任务同步执行在 GT；不把托管回调、JSON 或活动 World 暴露给 Jolt worker。
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem world;
    std::map<std::string, Body> bodies;
    std::map<JPH::BodyID, std::string> identities;
    JPH::Ref<ScriptCollisionGroupFilter> collisionGroups = new ScriptCollisionGroupFilter();
    std::map<std::string, std::vector<Eigen::Vector3f>> meshGeometry;
    struct Joint
    {
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        std::string owner, target;
    };
    std::map<std::string, Joint> joints;
    nlohmann::json contacts = nlohmann::json::array(), triggers = nlohmann::json::array();
    double accumulated = 0;

    Impl()
    {
        layers.MapObjectToBroadPhaseLayer(StaticLayer, JPH::BroadPhaseLayer(0));
        layers.MapObjectToBroadPhaseLayer(MovingLayer, JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(StaticLayer, MovingLayer);
        pairs.EnableCollision(MovingLayer, MovingLayer);
        broadphaseFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(layers, 2, pairs, 2);
        world.Init(4096, 0, 16384, 16384, layers, *broadphaseFilter, pairs);
        world.SetGravity({0, -9.81f, 0});
        auto settings = world.GetPhysicsSettings();
        settings.mPenetrationSlop = 0.001f;
        world.SetPhysicsSettings(settings);
    }
    ~Impl()
    {
        for (auto& joint : joints) world.RemoveConstraint(joint.second.constraint);
        joints.clear();
        for (auto& body : bodies) Remove(body.second);
    }
    void Remove(Body& body)
    {
        for (auto it = joints.begin(); it != joints.end();)
            if (it->second.constraint->GetBody1()->GetID() == body.id ||
                it->second.constraint->GetBody2()->GetID() == body.id)
            {
                const auto first = it->second.constraint->GetBody1()->GetID();
                const auto second = it->second.constraint->GetBody2()->GetID();
                world.RemoveConstraint(it->second.constraint);
                // 另一端可能远离被删体的 AABB 且已经休眠；拆关节必须显式唤醒两个连接体。
                world.GetBodyInterface().ActivateBody(first);
                world.GetBodyInterface().ActivateBody(second);
                it = joints.erase(it);
            }
            else ++it;
        WakeAround(body);
        identities.erase(body.id);
        if (body.character) body.character = nullptr; // CharacterVirtual 拥有并移除它的查询代理体。
        else
        {
            world.GetBodyInterface().RemoveBody(body.id);
            world.GetBodyInterface().DestroyBody(body.id);
        }
    }
    void WakeAround(const Body& body)
    {
        JPH::AABox bounds;
        {
            JPH::BodyLockRead lock(world.GetBodyLockInterface(), body.id);
            bounds = lock.GetBody().GetWorldSpaceBounds();
        }
        // 静态支撑体/查询代理没有自身激活流程；移走或删除时显式唤醒旧位置附近的动态体。
        bounds.ExpandBy(JPH::Vec3::sReplicate(0.002f));
        world.GetBodyInterface().ActivateBodiesInAABox(bounds, {}, {});
    }
    JPH::ShapeRefC CreateBodyShape(const nlohmann::json& state, const nlohmann::json& declaration)
    {
        const auto& physics = declaration.at("physics");
        const auto kind = physics.value("shape", "box");
        const float rootScale = declaration.at("scale")[0].get<float>();
        if (kind == "mesh")
        {
            const auto id = declaration.value("__physicsTemplateId", state.at("id").get<std::string>());
            const auto found = meshGeometry.find(id + "/" + physics.at("component").get<std::string>());
            if (found == meshGeometry.end() || found->second.empty())
                throw std::invalid_argument("Script.MeshCollisionMissingFormalGeometry");
            const auto componentId = physics.at("component").get<std::string>();
            const auto& authored = declaration.at("components");
            const auto& current = state.value("components", authored);
            size_t authoredIndex = 0, currentIndex = 0;
            while (authoredIndex < authored.size() && ScriptComponentId(authored[authoredIndex]) != componentId) ++authoredIndex;
            while (currentIndex < current.size() && ScriptComponentId(current[currentIndex]) != componentId) ++currentIndex;
            if (authoredIndex == authored.size() || currentIndex == current.size())
                throw std::invalid_argument("Script.MeshCollisionComponentMissing");
            auto authoredRoot = declaration, currentRoot = state;
            authoredRoot["position"] = currentRoot["position"] = {0, 0, 0};
            authoredRoot["rotation"] = currentRoot["rotation"] = {0, 0, 0};
            authoredRoot["scale"] = currentRoot["scale"] = {1, 1, 1};
            // 正式导入三角形已带作者组件矩阵；只乘本次相对变化，避免组件变换重复应用。
            const Eigen::Matrix4f adjustment = ScriptComponentMatrix(currentRoot, currentIndex) *
                ScriptComponentMatrix(authoredRoot, authoredIndex).inverse();
            JPH::TriangleList triangles;
            for (size_t index = 0; index + 2 < found->second.size(); index += 3)
            {
                const Eigen::Vector3f first = (adjustment * found->second[index].homogeneous()).head<3>() * 0.01f * rootScale;
                const Eigen::Vector3f second = (adjustment * found->second[index + 1].homogeneous()).head<3>() * 0.01f * rootScale;
                const Eigen::Vector3f third = (adjustment * found->second[index + 2].homogeneous()).head<3>() * 0.01f * rootScale;
                triangles.emplace_back(JPH::Float3(first.x(), first.y(), first.z()),
                    JPH::Float3(second.x(), second.y(), second.z()),
                    JPH::Float3(third.x(), third.y(), third.z()));
            }
            const auto result = JPH::MeshShapeSettings(triangles).Create();
            if (result.HasError()) throw std::invalid_argument("Script.InvalidMeshCollision: " + std::string(result.GetError().c_str()));
            return result.Get();
        }
        if (kind != "compound") return CreateShape(physics, rootScale);
        JPH::StaticCompoundShapeSettings compound;
        auto shapes = physics.value("shapes", nlohmann::json::array());
        const auto components = state.value("components", declaration.value("components", nlohmann::json::array()));
        if (shapes.empty())
            for (const auto& component : components)
                if (component.contains("collision"))
                {
                    auto shape = component.at("collision");
                    shape["component"] = ScriptComponentId(component);
                    shapes.push_back(std::move(shape));
                }
        if (shapes.empty()) throw std::invalid_argument("Script.CompoundRequiresComponentCollisions");
        auto localBlueprint = state;
        localBlueprint["position"] = {0, 0, 0};
        localBlueprint["rotation"] = {0, 0, 0};
        localBlueprint["scale"] = {rootScale, rootScale, rootScale};
        localBlueprint["components"] = components;
        for (const auto& shape : shapes)
        {
            const auto id = shape.at("component").get<std::string>();
            size_t index = 0;
            while (index < components.size() && ScriptComponentId(components[index]) != id) ++index;
            if (index == components.size()) throw std::invalid_argument("Script.CompoundComponentMissing");
            const Eigen::Matrix4f matrix = ScriptComponentMatrix(localBlueprint, index);
            const Eigen::Matrix3f linear = matrix.block<3, 3>(0, 0);
            const float scale = linear.col(0).norm();
            const Eigen::Matrix3f rotation = linear / scale;
            if (scale <= 0 || rotation.determinant() <= 0 ||
                !(rotation.transpose() * rotation).isApprox(Eigen::Matrix3f::Identity(), 1e-4f))
                throw std::invalid_argument("Script.CollisionComponentRequiresPositiveUniformScaleWithoutShear");
            const Eigen::Quaternionf q(rotation);
            const Eigen::Vector3f point = matrix.block<3, 1>(0, 3) * 0.01f;
            const auto child = CreateShape(shape, scale);
            compound.AddShape({point.x(), point.y(), point.z()}, {q.x(), q.y(), q.z(), q.w()}, child);
        }
        const auto result = compound.Create();
        if (result.HasError()) throw std::invalid_argument("Script.InvalidCompound: " + std::string(result.GetError().c_str()));
        return result.Get();
    }

    void SynchronizeConstraints(const nlohmann::json& declarations)
    {
        for (const auto& item : declarations.items())
        {
            const auto owner = bodies.find(item.key());
            if (owner == bodies.end() || owner->second.character) continue;
            for (const auto& declaration : item.value().value("physics", nlohmann::json::object())
                .value("constraints", nlohmann::json::array()))
            {
                const auto targetId = declaration.at("target").get<std::string>();
                const auto target = bodies.find(targetId);
                const auto key = item.key() + "/" + declaration.at("id").get<std::string>();
                if (joints.count(key) || target == bodies.end() || target->second.character) continue;
                if (target == owner || (owner->second.motion == "static" && target->second.motion == "static"))
                    throw std::invalid_argument("Script.InvalidConstraintBodies");
                // GT 同步阶段没有Jolt任务；NoLock避免两个body共享锁槽时递归锁死。
                JPH::BodyLockWrite first(world.GetBodyLockInterfaceNoLock(), owner->second.id);
                JPH::BodyLockWrite second(world.GetBodyLockInterfaceNoLock(), target->second.id);
                const auto anchor = PhysicsVector(declaration, "anchor");
                JPH::Ref<JPH::TwoBodyConstraint> constraint;
                if (declaration.at("type") == "fixed")
                {
                    JPH::FixedConstraintSettings settings;
                    settings.mPoint1 = settings.mPoint2 = anchor;
                    constraint = settings.Create(first.GetBody(), second.GetBody());
                }
                else
                {
                    JPH::HingeConstraintSettings settings;
                    settings.mPoint1 = settings.mPoint2 = anchor;
                    settings.mHingeAxis1 = settings.mHingeAxis2 = PhysicsVector(declaration, "axis", 1);
                    const Eigen::Vector3f axis = ScriptVector(declaration, "axis");
                    const Eigen::Vector3f normal = axis.unitOrthogonal();
                    settings.mNormalAxis1 = settings.mNormalAxis2 = {normal.x(), normal.y(), normal.z()};
                    settings.mLimitsMin = declaration.value("minAngle", -180.0f) * (JPH::JPH_PI / 180);
                    settings.mLimitsMax = declaration.value("maxAngle", 180.0f) * (JPH::JPH_PI / 180);
                    constraint = settings.Create(first.GetBody(), second.GetBody());
                }
                world.AddConstraint(constraint);
                joints.emplace(key, Joint{constraint, item.key(), targetId});
            }
        }
    }

    Body Create(const nlohmann::json& state, const nlohmann::json& declaration)
    {
        const auto& physics = declaration.at("physics");
        Body body;
        body.motion = physics.value("motion", "static");
        body.trigger = physics.value("trigger", false);
        body.layer = physics.value("collisionLayer", 0u);
        body.mask = physics.value("collisionMask", 65535u);
        body.shape = CreateBodyShape(state, declaration);
        if (physics.value("shape", "box") == "compound" || physics.value("shape", "box") == "mesh")
            body.compoundSignature = state.value("components", nlohmann::json::array()).dump();
        const auto position = PhysicsVector(state, "position");
        const auto rotation = PhysicsRotation(state);
        if (body.motion == "character")
        {
            JPH::CharacterVirtualSettings settings;
            settings.mShape = body.shape;
            settings.mInnerBodyShape = body.shape;
            settings.mInnerBodyLayer = MovingLayer;
            settings.mMass = physics.value("mass", 70.0f);
            settings.mMaxSlopeAngle = physics.value("maxSlopeAngle", 45.0f) * (JPH::JPH_PI / 180);
            settings.mCharacterPadding = 0.002f;
            body.jumpSpeed = physics.value("jumpSpeed", 500.0f) * 0.01f;
            body.stepHeight = physics.value("stepHeight", 30.0f) * 0.01f;
            // 根位置始终是竖直胶囊中心，不从视觉组件推导脚底或尺寸。
            body.character = new JPH::CharacterVirtual(&settings, position, rotation, &world);
            body.id = body.character->GetInnerBodyID();
            if (body.id.IsInvalid()) throw std::runtime_error("Script.PhysicsBodyBudget");
            world.GetBodyInterface().SetCollisionGroup(body.id, JPH::CollisionGroup(collisionGroups, body.layer, body.mask));
        }
        else
        {
            const auto motion = body.motion == "dynamic" ? JPH::EMotionType::Dynamic :
                body.motion == "kinematic" ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static;
            JPH::BodyCreationSettings settings(body.shape, position, rotation, motion,
                motion == JPH::EMotionType::Static ? StaticLayer : MovingLayer);
            settings.mFriction = physics.value("friction", 0.5f);
            settings.mRestitution = physics.value("restitution", 0.0f);
            settings.mIsSensor = body.trigger;
            settings.mCollisionGroup = JPH::CollisionGroup(collisionGroups, body.layer, body.mask);
            settings.mCollideKinematicVsNonDynamic = true;
            // 保留原 API 无阻尼速度语义；质量影响冲量与惯量，不采用形状默认密度。
            settings.mLinearDamping = settings.mAngularDamping = 0;
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = physics.value("mass", 1.0f);
            body.id = world.GetBodyInterface().CreateAndAddBody(settings,
                motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            if (body.id.IsInvalid()) throw std::runtime_error("Script.PhysicsBodyBudget");
        }
        body.last = state;
        return body;
    }
    // CMC 式角色推进：一帧一次，内部把 dt 拆成 <=0.05 的迭代（最多 8 次）；
    // 每次迭代重新求重力/root/walk 速度后调用 Jolt ExtendedUpdate，jump 只在第一次迭代消费。
    void MoveCharacter(Body& body, float dt)
    {
        if (dt <= 0) return; // 零时步只同步指令/接触，不移动角色，jump 保留到下一有效帧。
        auto& character = *body.character;
        const auto previousPosition = character.GetPosition();
        QueryBodyFilter filter;
        filter.mask = body.mask;
        filter.layer = int(body.layer);
        JPH::CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mWalkStairsStepUp = {0, body.stepHeight, 0};
        settings.mStickToFloorStepDown = {0, -body.stepHeight, 0};
        float remaining = dt;
        for (int iteration = 0; iteration < MaxCharacterIterations && remaining > 0; ++iteration)
        {
            const float step = std::min(remaining, MaxCharacterIteration);
            character.UpdateGroundVelocity();
            const auto ground = character.GetGroundVelocity();
            const bool supported = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
                character.GetLinearVelocity().GetY() - ground.GetY() < 0.1f;
            auto velocity = body.rootMotionActive
                ? body.rootMotion
                : (supported ? ground : JPH::Vec3(0, character.GetLinearVelocity().GetY(), 0));
            if (!body.rootMotionActive && body.jump && supported) velocity += JPH::Vec3(0, body.jumpSpeed, 0);
            body.jump = false;
            // Jolt 的 gravity 参数只给支撑体施力；角色自己的重力必须在速度中按本次迭代时长积分。
            if (!body.rootMotionActive) velocity += body.walk;
            velocity += world.GetGravity() * step;
            character.SetLinearVelocity(character.CancelVelocityTowardsSteepSlopes(velocity));
            character.ExtendedUpdate(step, world.GetGravity(), settings,
                world.GetDefaultBroadPhaseLayerFilter(MovingLayer), world.GetDefaultLayerFilter(MovingLayer),
                filter, {}, allocator);
            remaining -= step;
        }
        // CharacterVirtual 的线速度仍可能包含被墙体阻止的意图；动画反馈取整帧实际位移。
        body.achievedVelocity = JPH::Vec3(character.GetPosition() - previousPosition) / dt;
    }
    void CollectContacts()
    {
        std::set<std::pair<std::string, std::string>> collisions, overlaps;
        // 以窄相位形状查询生成最终帧快照，休眠不产生假 Exit；代理胶囊也可参与 trigger。
        // 运动体/compound 查询复用 Jolt broadphase；静态三角网格只作为被查询对象。
        for (const auto& entry : bodies)
        {
            const auto& body = entry.second;
            if (body.motion == "static" && !body.trigger) continue;
            QueryBodyFilter filter;
            filter.ignore = body.id; filter.includeTriggers = true;
            filter.mask = body.mask;
            JPH::CollideShapeSettings settings;
            settings.mMaxSeparationDistance = 0.002f;
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            const auto transform = world.GetBodyInterface().GetCenterOfMassTransform(body.id);
            world.GetNarrowPhaseQuery().CollideShape(body.shape, JPH::Vec3::sOne(), transform,
                settings, transform.GetTranslation(), collector, {}, {}, filter);
            for (const auto& hit : collector.mHits)
            {
                const auto& otherId = identities.at(hit.mBodyID2);
                const auto& other = bodies.at(otherId);
                if (!(other.mask & (1u << body.layer))) continue;
                const bool trigger = body.trigger || other.trigger;
                if (trigger && hit.mPenetrationDepth < 0) continue;
                const auto pair = std::minmax(entry.first, otherId);
                (trigger ? overlaps : collisions).emplace(pair.first, pair.second);
            }
        }
        contacts = nlohmann::json::array(); triggers = nlohmann::json::array();
        for (const auto& pair : collisions) contacts.push_back({pair.first, pair.second});
        for (const auto& pair : overlaps) triggers.push_back({pair.first, pair.second});
    }
};

ScriptPhysicsWorld::ScriptPhysicsWorld() : impl(std::make_unique<Impl>()) {}
ScriptPhysicsWorld::~ScriptPhysicsWorld() = default;

void ScriptPhysicsWorld::Step(nlohmann::json& states, const nlohmann::json& declarations, float dt)
{
    // 一帧物理：刚体时间按 1/60 子步累计，最多 16 步；未命中上限时保留不足一步的分数余量，
    // 命中上限时丢弃整步多余量。角色不使用这个 accumulator，直接消费帧 dt。
    // dt=0 的指令同步帧不消费余量，保证这一帧只同步状态/指令并刷新接触。
    int substeps = 0;
    if (dt > 0)
    {
        impl->accumulated += double(dt);
        const double requested = std::floor((impl->accumulated + 1e-9) / RigidSubstep);
        substeps = int(std::min(requested, MaxRigidSubsteps));
        impl->accumulated -= substeps * RigidSubstep;
        if (requested > MaxRigidSubsteps) impl->accumulated = std::fmod(impl->accumulated, RigidSubstep);
        // 1e-9 偏置可能留下极小负余量；夹回 0，避免下一帧 floor 得到负步数。
        if (impl->accumulated < 0) impl->accumulated = 0;
    }
    StepInternal(states, declarations, dt, substeps);
}

void ScriptPhysicsWorld::StepInternal(nlohmann::json& states, const nlohmann::json& declarations, float dt, int substeps)
{
    auto& world = impl->world;
    auto& interface = world.GetBodyInterface();
    for (auto& state : states)
    {
        const auto id = state.at("id").get<std::string>();
        const auto& declaration = declarations.at(id);
        if (!declaration.contains("physics")) continue;
        auto found = impl->bodies.find(id);
        if (state.value("destroyed", false) || !state.value("enabled", declaration.value("enabled", true)))
        {
            if (found != impl->bodies.end())
            {
                impl->Remove(found->second);
                impl->bodies.erase(found);
            }
            continue;
        }
        if (found == impl->bodies.end())
        {
            found = impl->bodies.emplace(id, impl->Create(state, declaration)).first;
            impl->identities.emplace(found->second.id, id);
            impl->WakeAround(found->second);
        }
        auto& body = found->second;
        if (declaration.at("physics").value("shape", "box") == "compound" ||
            declaration.at("physics").value("shape", "box") == "mesh")
        {
            const auto signature = state.value("components", nlohmann::json::array()).dump();
            if (signature != body.compoundSignature)
            {
                const auto shape = impl->CreateBodyShape(state, declaration);
                impl->WakeAround(body);
                interface.SetShape(body.id, shape, true, JPH::EActivation::Activate);
                body.shape = shape;
                body.compoundSignature = signature;
                impl->WakeAround(body);
            }
        }
        const bool positionChanged = state.at("position") != body.last.at("position");
        const bool rotationChanged = state.at("rotation") != body.last.at("rotation");
        if (positionChanged || rotationChanged)
        {
            impl->WakeAround(body);
            const auto position = PhysicsVector(state, "position");
            const auto rotation = PhysicsRotation(state);
            if (body.character)
            {
                if (positionChanged)
                {
                    body.character->SetPosition(position);
                    body.achievedVelocity = JPH::Vec3::sZero();
                    body.refreshContacts = true;
                }
                // 胶囊只允许世界 Y 轴转向，形状/支撑不变；dt=0 的指令同步帧保留
                // 最近一帧物理的实际速度，不能把纯转向误报为传送或碰墙停止。
                if (rotationChanged) body.character->SetRotation(rotation);
            }
            else interface.SetPositionAndRotation(body.id, position, rotation,
                body.motion == "static" ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            impl->WakeAround(body);
        }
        if (body.character)
        {
            body.walk = PhysicsVector(state, "walkVelocity");
            if (state.contains("rootMotion")) body.rootMotion = PhysicsVector(state, "rootMotion");
            body.rootMotionActive = state.value("rootMotionActive", false);
            body.jump = body.jump || state.value("jump", false);
        }
        else if (body.motion != "static")
        {
            if (state.contains("velocity")) interface.SetLinearVelocity(body.id, PhysicsVector(state, "velocity"));
            if (state.contains("impulse") && body.motion == "dynamic")
                interface.AddImpulse(body.id, PhysicsVector(state, "impulse"));
        }
        // cm/kg/s 在唯一后端边界转成 m/kg/s；零时步同步也只领取一次指令。
        state.erase("velocity"); state.erase("impulse"); state.erase("jump");
    }
    // 完整同步 body 集合后才刷新，避免角色出生/传送时仍引用旧地面，或受声明顺序影响。
    for (auto& entry : impl->bodies)
    {
        auto& body = entry.second;
        if (body.character && body.refreshContacts)
        {
            QueryBodyFilter filter;
            filter.mask = body.mask;
            filter.layer = int(body.layer);
            body.character->RefreshContacts(world.GetDefaultBroadPhaseLayerFilter(MovingLayer),
                world.GetDefaultLayerFilter(MovingLayer), filter, {}, impl->allocator);
            body.refreshContacts = false;
        }
    }
    impl->SynchronizeConstraints(declarations);
    // 角色每帧只推进一次（内部再按 0.05 拆分迭代）；刚体按固定 1/60 子步追赶，
    // 子步数已由 Step 决定，这里不再重复累计时间。
    if (dt > 0)
        for (auto& body : impl->bodies)
            if (body.second.character) impl->MoveCharacter(body.second, dt);
    for (int step = 0; step < substeps; ++step)
    {
        if (world.Update(float(RigidSubstep), 1, &impl->allocator, &impl->jobs) != JPH::EPhysicsUpdateError::None)
            throw std::runtime_error("Script.PhysicsCapacityExceeded");
    }
    impl->CollectContacts();
    for (auto& state : states)
    {
        const auto found = impl->bodies.find(state.at("id").get<std::string>());
        if (found == impl->bodies.end()) continue;
        auto& body = found->second;
        if (body.character)
        {
            state["position"] = VectorJson(body.character->GetPosition());
            state["bodyVelocity"] = VectorJson(body.achievedVelocity);
            state["grounded"] = body.character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        }
        else
        {
            if (body.motion != "static")
            {
                state["position"] = VectorJson(interface.GetPosition(body.id));
                const auto q = interface.GetRotation(body.id);
                const Eigen::Quaternionf rotation(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
                const Eigen::Vector3f angles = rotation.toRotationMatrix().eulerAngles(1, 0, 2) *
                    (180.0f / 3.14159265358979323846f);
                state["rotation"] = {angles.y(), angles.x(), angles.z()};
            }
            state["bodyVelocity"] = VectorJson(interface.GetLinearVelocity(body.id));
        }
        body.last = state;
    }
}
nlohmann::json ScriptPhysicsWorld::Raycast(const nlohmann::json& requests) const
{
    auto results = nlohmann::json::array();
    for (const auto& request : requests)
    {
        QueryBodyFilter filter;
        filter.includeTriggers = request.value("includeTriggers", false);
        filter.mask = request.value("collisionMask", 65535u);
        const auto owner = impl->bodies.find(request.at("owner").get<std::string>());
        if (owner != impl->bodies.end()) filter.ignore = owner->second.id;
        const auto from = PhysicsVector(request, "from");
        const auto direction = PhysicsVector(request, "to") - from;
        nlohmann::json result{{"id", request.at("id")}};
        if (request.value("radius", 0.0f) > 0)
        {
            const auto sphere = JPH::SphereShapeSettings(request.at("radius").get<float>() * 0.01f).Create().Get();
            const JPH::RShapeCast cast(sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(from), direction);
            JPH::ShapeCastSettings settings;
            settings.mReturnDeepestPoint = true;
            settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
            JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
            impl->world.GetNarrowPhaseQuery().CastShape(cast, settings, from, collector, {}, {}, filter);
            if (collector.HadHit())
            {
                const auto& hit = collector.mHit;
                result["blueprint"] = impl->identities.at(hit.mBodyID2);
                result["point"] = VectorJson(from + direction * hit.mFraction);
                const auto normal = hit.mPenetrationAxis.IsNearZero() ? -direction.Normalized() : -hit.mPenetrationAxis.Normalized();
                result["normal"] = VectorJson(normal, 1);
                result["distance"] = direction.Length() * hit.mFraction * 100;
            }
            results.push_back(std::move(result));
            continue;
        }
        const JPH::RRayCast ray(from, direction);
        JPH::RayCastResult hit;
        if (impl->world.GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter))
        {
            JPH::BodyLockRead lock(impl->world.GetBodyLockInterface(), hit.mBodyID);
            const auto point = ray.GetPointOnRay(hit.mFraction);
            result["blueprint"] = impl->identities.at(hit.mBodyID);
            result["point"] = VectorJson(point);
            // 从凸体内部出发时命中距离为零，并无唯一表面法线；统一返回逆射线方向，避免球心归一化零向量。
            const auto normal = hit.mFraction == 0 ? -direction.Normalized() :
                lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
            result["normal"] = VectorJson(normal, 1);
            result["distance"] = direction.Length() * hit.mFraction * 100;
        }
        results.push_back(std::move(result));
    }
    return results;
}
nlohmann::json ScriptPhysicsWorld::Contacts() const { return impl->contacts; }
nlohmann::json ScriptPhysicsWorld::Triggers() const { return impl->triggers; }
Animation::FootGroundHit ScriptPhysicsWorld::ProbeGround(const std::string& owner,
    const Eigen::Vector3f& origin, const Eigen::Vector3f& displacement) const
{
    Animation::FootGroundHit result;
    const auto source = impl->bodies.find(owner);
    if (source == impl->bodies.end()) return result;
    QueryBodyFilter filter;
    filter.ignore = source->second.id;
    filter.mask = source->second.mask;
    filter.layer = static_cast<int>(source->second.layer);
    // 动画在最终物理同步后直接借用查询；厘米只在 Jolt 边界换算，不经 CLR/JSON 往返。
    const Eigen::Vector3f from = origin * 0.01f, delta = displacement * 0.01f;
    const JPH::RRayCast ray({from.x(), from.y(), from.z()}, {delta.x(), delta.y(), delta.z()});
    JPH::RayCastResult hit;
    if (!impl->world.GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) return result;
    const auto identity = impl->identities.find(hit.mBodyID);
    if (identity == impl->identities.end()) return result;
    const auto& support = impl->bodies.at(identity->second);
    // 首次遮挡是移动支撑时明确拒绝，不能穿过它继续锁到下方静态地板。
    // 内部起点没有唯一接触法线，同样不伪造可站立面；坡度由动画 solver 决定。
    if (support.motion != "static" || hit.mFraction <= 0) return result;
    JPH::BodyLockRead lock(impl->world.GetBodyLockInterface(), hit.mBodyID);
    const auto point = ray.GetPointOnRay(hit.mFraction);
    const auto normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
    result.hit = true;
    result.point = Eigen::Vector3f(point.GetX(), point.GetY(), point.GetZ()) * 100;
    result.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
    result.supportIdentity = identity->second;
    return result;
}
void ScriptPhysicsWorld::SetMeshGeometry(const std::map<std::string, std::vector<Eigen::Vector3f>>& triangles)
{
    impl->meshGeometry = triangles;
}
#else
struct ScriptPhysicsWorld::Impl {};
ScriptPhysicsWorld::ScriptPhysicsWorld() = default;
ScriptPhysicsWorld::~ScriptPhysicsWorld() = default;
void ScriptPhysicsWorld::Step(nlohmann::json&, const nlohmann::json&, float) {}
void ScriptPhysicsWorld::StepInternal(nlohmann::json&, const nlohmann::json&, float, int) {}
nlohmann::json ScriptPhysicsWorld::Contacts() const { return nlohmann::json::array(); }
nlohmann::json ScriptPhysicsWorld::Triggers() const { return nlohmann::json::array(); }
nlohmann::json ScriptPhysicsWorld::Raycast(const nlohmann::json&) const { return nlohmann::json::array(); }
Animation::FootGroundHit ScriptPhysicsWorld::ProbeGround(const std::string&, const Eigen::Vector3f&,
    const Eigen::Vector3f&) const { return {}; }
void ScriptPhysicsWorld::SetMeshGeometry(const std::map<std::string, std::vector<Eigen::Vector3f>>&) {}
#endif
} // namespace VL
