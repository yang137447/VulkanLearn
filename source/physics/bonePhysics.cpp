#include "bonePhysics.h"

#include "physics/runtime/joltRuntime.h"
#include "shader/build/contentHash.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace VL::Physics
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr float DegreesToRadians = Pi / 180.0f;
// UE 与 VL 的骨骼姿态单位是厘米；Jolt 统一用米。
constexpr float CentimetersToMeters = 0.01f;
constexpr float MetersToCentimeters = 100.0f;
constexpr float KindaSmallNumber = 1.0e-4f;

constexpr JPH::ObjectLayer StaticLayer = 0;
constexpr JPH::ObjectLayer KinematicLayer = 1;
constexpr JPH::ObjectLayer SimulatedLayer = 2;

JPH::Vec3 JoltVector(const Eigen::Vector3f& value, float scale = CentimetersToMeters)
{
    return JPH::Vec3(value.x() * scale, value.y() * scale, value.z() * scale);
}

JPH::Quat JoltRotation(const Eigen::Quaternionf& value)
{
    return JPH::Quat(value.x(), value.y(), value.z(), value.w());
}

Eigen::Quaternionf EigenRotation(JPH::QuatArg value)
{
    return Eigen::Quaternionf(value.GetW(), value.GetX(), value.GetY(), value.GetZ()).normalized();
}

Eigen::Vector3f EigenVector(JPH::Vec3Arg value)
{
    return {value.GetX() * MetersToCentimeters, value.GetY() * MetersToCentimeters,
        value.GetZ() * MetersToCentimeters};
}

// UE 的 capsule 轴是 X，Jolt 的是 Y；先绕 Z 转 -90° 再叠加元素旋转。
Eigen::Quaternionf CapsuleAxisRotation(const Eigen::Quaternionf& element)
{
    const Eigen::Quaternionf yToX(Eigen::AngleAxisf(-Pi * 0.5f, Eigen::Vector3f::UnitZ()));
    return (element * yToX).normalized();
}

const nlohmann::json& Required(const nlohmann::json& object, const char* key, const char* owner)
{
    if (!object.is_object() || !object.contains(key))
        throw std::invalid_argument(std::string("BonePhysics missing field ") + owner + "." + key);
    return object.at(key);
}

void RejectUnknown(const nlohmann::json& object, std::initializer_list<const char*> allowed,
    const char* owner)
{
    if (!object.is_object())
        throw std::invalid_argument(std::string("BonePhysics object expected at ") + owner);
    for (const auto& item : object.items())
    {
        bool known = false;
        for (const char* key : allowed)
            if (item.key() == key) { known = true; break; }
        if (!known)
            throw std::invalid_argument(std::string("BonePhysics unknown field ") + owner + "." +
                item.key());
    }
}

std::string ReadString(const nlohmann::json& value, const char* field)
{
    if (!value.is_string())
        throw std::invalid_argument(std::string("BonePhysics expects string: ") + field);
    return value.get<std::string>();
}

float ReadNumber(const nlohmann::json& value, const char* field)
{
    if (!value.is_number())
        throw std::invalid_argument(std::string("BonePhysics expects number: ") + field);
    const float result = value.get<float>();
    if (!std::isfinite(result))
        throw std::invalid_argument(std::string("BonePhysics number is not finite: ") + field);
    return result;
}

bool ReadBool(const nlohmann::json& value, const char* field)
{
    if (!value.is_boolean())
        throw std::invalid_argument(std::string("BonePhysics expects bool: ") + field);
    return value.get<bool>();
}

Eigen::Vector3f ReadVector3(const nlohmann::json& value, const char* field)
{
    if (!value.is_array() || value.size() != 3)
        throw std::invalid_argument(std::string("BonePhysics expects [x,y,z]: ") + field);
    return {ReadNumber(value.at(0), field), ReadNumber(value.at(1), field),
        ReadNumber(value.at(2), field)};
}

Eigen::Quaternionf ReadQuaternion(const nlohmann::json& value, const char* field)
{
    if (!value.is_array() || value.size() != 4)
        throw std::invalid_argument(std::string("BonePhysics expects [x,y,z,w]: ") + field);
    const Eigen::Quaternionf result(ReadNumber(value.at(3), field), ReadNumber(value.at(0), field),
        ReadNumber(value.at(1), field), ReadNumber(value.at(2), field));
    if (std::abs(result.norm() - 1.0f) > 1.0e-3f)
        throw std::invalid_argument(std::string("BonePhysics quaternion is not normalized: ") +
            field);
    return result.normalized();
}

BonePhysicsMotion ParseMotion(const std::string& value)
{
    if (value == "free") return BonePhysicsMotion::Free;
    if (value == "limited") return BonePhysicsMotion::Limited;
    if (value == "locked") return BonePhysicsMotion::Locked;
    throw std::invalid_argument("BonePhysics unknown motion: " + value);
}

BonePhysicsLimit ParseLimit(const nlohmann::json& value)
{
    BonePhysicsLimit limit;
    limit.motion = ParseMotion(ReadString(Required(value, "motion", "limit"), "limit.motion"));
    limit.degrees = ReadNumber(Required(value, "limit", "limit"), "limit.limit");
    if (limit.degrees < 0.0f || limit.degrees > 180.0f)
        throw std::invalid_argument("BonePhysics limit must be within [0,180]");
    return limit;
}
}

// Jolt 的分配器/Factory 必须先注册，再构造 JobSystem/TempAllocator 等成员；
// 用基类保证构造顺序（成员初始化早于构造函数体）。
struct JoltRegistrationBase
{
    JoltRegistrationBase() { EnsureJoltRegistered(); }
};

struct BonePhysicsSolver::Impl : JoltRegistrationBase
{
    struct BoundBody
    {
        int joint = -1;
        bool simulated = false;
        JPH::BodyID id;
        BonePhysicsShape shape;
        std::string bone;
        std::string constraintParent;
    };

    JPH::TempAllocatorImpl tempAllocator{8 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobSystem{1024};
    JPH::BroadPhaseLayerInterfaceTable broadPhaseLayers{3, 3};
    JPH::ObjectLayerPairFilterTable objectPairs{3};
    // PhysicsSystem::Init 只保存引用，过滤器必须是成员而不是临时对象。
    JPH::ObjectVsBroadPhaseLayerFilterTable objectVsBroadPhaseFilter{
        broadPhaseLayers, 3, objectPairs, 3};
    JPH::PhysicsSystem system;
    std::vector<BoundBody> bodies;
    // 约束必须先于刚体释放：Jolt 的约束持有两端 body 的引用。
    std::vector<JPH::Ref<JPH::Constraint>> constraints;
    bool initialized = false;
    bool firstFrame = true;

    void Initialize()
    {
        if (initialized) return;
        EnsureJoltRegistered();
        broadPhaseLayers.MapObjectToBroadPhaseLayer(StaticLayer, JPH::BroadPhaseLayer(0));
        broadPhaseLayers.MapObjectToBroadPhaseLayer(KinematicLayer, JPH::BroadPhaseLayer(1));
        broadPhaseLayers.MapObjectToBroadPhaseLayer(SimulatedLayer, JPH::BroadPhaseLayer(2));
        // 骨骼刚体之间默认不互撞：UE 的 CollisionDisableTable 没有随 T3D 导出，
        // 而裙摆面板的绑定平移都是零（刚体几乎重叠），保留自碰撞会互相顶飞。
        // 只保留与静态场景（未来接地面/墙体）的碰撞。
        objectPairs.EnableCollision(StaticLayer, SimulatedLayer);
        system.Init(1024, 0, 4096, 1024, broadPhaseLayers, objectVsBroadPhaseFilter, objectPairs);
        system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        initialized = true;
    }

    ~Impl()
    {
        if (!initialized) return;
        JPH::BodyInterface& interface = system.GetBodyInterface();
        for (auto& constraint : constraints)
            system.RemoveConstraint(constraint);
        constraints.clear();
        for (const auto& body : bodies)
            if (!body.id.IsInvalid())
            {
                interface.RemoveBody(body.id);
                interface.DestroyBody(body.id);
            }
    }
};

BonePhysicsAsset ParseBonePhysicsAsset(const nlohmann::json& json, std::string_view sourcePath)
{
    RejectUnknown(json, {"format", "id", "sources", "sets"}, "root");
    BonePhysicsAsset asset;
    asset.format = ReadString(Required(json, "format", "root"), "format");
    if (asset.format != "vl.bone-physics.v1")
        throw std::invalid_argument("BonePhysics unsupported format: " + asset.format);
    asset.id = ReadString(Required(json, "id", "root"), "id");
    if (asset.id.empty())
        throw std::invalid_argument("BonePhysics id is empty");
    asset.sourcePath = std::string(sourcePath);

    const auto& sets = Required(json, "sets", "root");
    if (!sets.is_array() || sets.empty() || sets.size() > 16)
        throw std::invalid_argument("BonePhysics sets must contain 1..16 entries");
    std::set<std::string> names;
    for (const auto& node : sets)
    {
        RejectUnknown(node, {"name", "asset", "baseBone", "gravity", "bodies", "constraints"},
            "set");
        BonePhysicsSet set;
        set.name = ReadString(Required(node, "name", "set"), "set.name");
        if (set.name.empty() || !names.insert(set.name).second)
            throw std::invalid_argument("BonePhysics set names must be non-empty and unique");
        set.asset = ReadString(Required(node, "asset", "set"), "set.asset");
        set.baseBone = ReadString(Required(node, "baseBone", "set"), "set.baseBone");
        set.gravity = ReadVector3(Required(node, "gravity", "set"), "set.gravity");

        const auto& bodies = Required(node, "bodies", "set");
        if (!bodies.is_array() || bodies.empty() || bodies.size() > 64)
            throw std::invalid_argument("BonePhysics bodies must contain 1..64 entries");
        std::set<std::string> bones;
        for (const auto& bodyNode : bodies)
        {
            RejectUnknown(bodyNode, {"bone", "type", "shapes", "mass", "linearDamping",
                "angularDamping"}, "body");
            BonePhysicsBody body;
            body.bone = ReadString(Required(bodyNode, "bone", "body"), "body.bone");
            if (body.bone.empty() || !bones.insert(body.bone).second)
                throw std::invalid_argument("BonePhysics body bones must be unique");
            const std::string type = ReadString(Required(bodyNode, "type", "body"), "body.type");
            if (type != "simulated" && type != "kinematic")
                throw std::invalid_argument("BonePhysics unknown body type: " + type);
            body.simulated = type == "simulated";
            const auto& shapes = Required(bodyNode, "shapes", "body");
            if (!shapes.is_array() || shapes.size() != 1)
                throw std::invalid_argument("BonePhysics requires exactly one shape per body");
            const auto& shape = shapes.at(0);
            RejectUnknown(shape, {"type", "radius", "halfHeight", "center", "rotation"}, "shape");
            if (ReadString(Required(shape, "type", "shape"), "shape.type") != "capsule")
                throw std::invalid_argument("BonePhysics only supports capsule shapes");
            body.shape.radius = ReadNumber(Required(shape, "radius", "shape"), "shape.radius");
            body.shape.halfHeight = ReadNumber(
                Required(shape, "halfHeight", "shape"), "shape.halfHeight");
            body.shape.center = ReadVector3(Required(shape, "center", "shape"), "shape.center");
            body.shape.rotation = ReadQuaternion(
                Required(shape, "rotation", "shape"), "shape.rotation");
            if (body.shape.radius <= 0.0f || body.shape.halfHeight < 0.0f)
                throw std::invalid_argument("BonePhysics capsule dimensions are invalid");
            if (bodyNode.contains("mass") && !bodyNode.at("mass").is_null())
                body.mass = ReadNumber(bodyNode.at("mass"), "body.mass");
            if (bodyNode.contains("linearDamping"))
                body.linearDamping = ReadNumber(bodyNode.at("linearDamping"), "body.linearDamping");
            if (bodyNode.contains("angularDamping"))
                body.angularDamping = ReadNumber(bodyNode.at("angularDamping"),
                    "body.angularDamping");
            if (body.mass < 0.0f || body.linearDamping < 0.0f || body.angularDamping < 0.0f)
                throw std::invalid_argument("BonePhysics body mass/damping must be nonnegative");
            set.bodies.push_back(std::move(body));
        }

        if (node.contains("constraints"))
        {
            const auto& constraints = node.at("constraints");
            if (!constraints.is_array() || constraints.size() > 64)
                throw std::invalid_argument(
                    "BonePhysics constraints must be an array of at most 64");
            for (const auto& constraintNode : constraints)
            {
                RejectUnknown(constraintNode, {"name", "child", "parent", "swing1", "swing2",
                    "twist", "stiffness", "damping", "childFrame", "parentFrame"}, "constraint");
                BonePhysicsConstraint constraint;
                constraint.name = ReadString(
                    Required(constraintNode, "name", "constraint"), "constraint.name");
                constraint.child = ReadString(
                    Required(constraintNode, "child", "constraint"), "constraint.child");
                constraint.parent = ReadString(
                    Required(constraintNode, "parent", "constraint"), "constraint.parent");
                constraint.swing1 = ParseLimit(
                    Required(constraintNode, "swing1", "constraint"));
                constraint.swing2 = ParseLimit(
                    Required(constraintNode, "swing2", "constraint"));
                constraint.twist = ParseLimit(
                    Required(constraintNode, "twist", "constraint"));
                if (constraintNode.contains("stiffness"))
                    constraint.stiffness = ReadNumber(constraintNode.at("stiffness"),
                        "constraint.stiffness");
                if (constraintNode.contains("damping"))
                    constraint.damping = ReadNumber(constraintNode.at("damping"),
                        "constraint.damping");
                const auto& childFrame = Required(constraintNode, "childFrame", "constraint");
                RejectUnknown(childFrame, {"position", "rotation"}, "childFrame");
                constraint.childPosition = ReadVector3(
                    Required(childFrame, "position", "childFrame"), "childFrame.position");
                constraint.childRotation = ReadQuaternion(
                    Required(childFrame, "rotation", "childFrame"), "childFrame.rotation");
                const auto& parentFrame = Required(constraintNode, "parentFrame", "constraint");
                RejectUnknown(parentFrame, {"position", "rotation"}, "parentFrame");
                constraint.parentPosition = ReadVector3(
                    Required(parentFrame, "position", "parentFrame"), "parentFrame.position");
                constraint.parentRotation = ReadQuaternion(
                    Required(parentFrame, "rotation", "parentFrame"), "parentFrame.rotation");
                set.constraints.push_back(std::move(constraint));
            }
        }
        asset.sets.push_back(std::move(set));
    }
    return asset;
}

std::shared_ptr<const BonePhysicsAsset> LoadBonePhysicsAsset(
    const std::filesystem::path& resourceRoot, const std::string& relativePath)
{
    const auto relative = std::filesystem::u8path(relativePath);
    if (relativePath.empty() || relativePath.size() > 1024 || relative.is_absolute() ||
        relative.has_root_name() || relativePath.find('\\') != std::string::npos ||
        relativePath.find(':') != std::string::npos ||
        relative.lexically_normal().generic_string() != relativePath ||
        relative.extension() != ".json" ||
        (relativePath.rfind("Maps/", 0) != 0 && relativePath.rfind("Common/", 0) != 0))
        throw std::invalid_argument("BonePhysics requires a formal resource path");
    const auto canonicalRoot = std::filesystem::canonical(resourceRoot);
    const auto resolved = std::filesystem::weakly_canonical(canonicalRoot / relative);
    const auto inside = resolved.lexically_relative(canonicalRoot);
    if (inside.empty() || *inside.begin() == "..")
        throw std::invalid_argument("BonePhysics escapes the resource root");
    if (!std::filesystem::exists(resolved))
        throw std::runtime_error("BonePhysics asset does not exist: " + relativePath);
    if (std::filesystem::file_size(resolved) > 4 * 1024 * 1024)
        throw std::invalid_argument("BonePhysics asset exceeds 4 MiB");
    std::ifstream input(resolved, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (!input || bytes.size() > 4 * 1024 * 1024)
        throw std::runtime_error("Cannot read BonePhysics asset: " + relativePath);
    auto asset = std::make_shared<BonePhysicsAsset>(
        ParseBonePhysicsAsset(nlohmann::json::parse(bytes), relativePath));
    asset->sourceDigest = ContentHasher::HashString(bytes).ToHex();
    return asset;
}

BonePhysicsSolver::BonePhysicsSolver(std::vector<std::string> jointNames,
    std::vector<int> jointParents, std::vector<Eigen::Matrix4f> bindLocals,
    std::shared_ptr<const BonePhysicsAsset> asset)
    : impl(std::make_unique<Impl>()), asset(std::move(asset)),
      jointNames(std::move(jointNames)), jointParents(std::move(jointParents)),
      bindLocals(std::move(bindLocals))
{
    if (!this->asset)
        throw std::invalid_argument("BonePhysics asset is null");
    if (this->jointNames.size() != this->jointParents.size() ||
        this->jointNames.size() != this->bindLocals.size() || this->jointNames.empty())
        throw std::invalid_argument("BonePhysics joint table is empty or inconsistent");
    state.enabled = true;
    state.reason = "enabled";

    auto findJoint = [&](const std::string& name)
    {
        for (size_t joint = 0; joint < this->jointNames.size(); ++joint)
            if (this->jointNames[joint] == name) return static_cast<int>(joint);
        return -1;
    };

    // 绑定参考姿态的全局变换：刚体与约束都按它创建。
    std::vector<Eigen::Matrix4f> bindGlobals(this->jointNames.size(),
        Eigen::Matrix4f::Identity());
    for (size_t joint = 0; joint < bindGlobals.size(); ++joint)
    {
        const int parent = this->jointParents[joint];
        bindGlobals[joint] = parent >= 0 ? bindGlobals[static_cast<size_t>(parent)] *
            this->bindLocals[joint] : this->bindLocals[joint];
    }

    impl->Initialize();
    JPH::BodyInterface& interface = impl->system.GetBodyInterface();

    for (const auto& set : this->asset->sets)
    {
        std::vector<int> bodyJoints;
        bool complete = true;
        for (const auto& body : set.bodies)
        {
            const int joint = findJoint(body.bone);
            if (joint < 0)
            {
                bindDiagnostics.push_back({set.name, "missing-body-bone", body.bone});
                complete = false;
                continue;
            }
            bodyJoints.push_back(joint);
        }
        if (!complete)
        {
            bodyJoints.clear();
            continue;
        }

        // 约束的 child/parent 必须都在本 set 的 body 里。
        std::vector<std::pair<std::string, std::string>> constraintBones;
        for (const auto& constraint : set.constraints)
        {
            const bool hasChild = std::any_of(set.bodies.begin(), set.bodies.end(),
                [&](const BonePhysicsBody& body) { return body.bone == constraint.child; });
            const bool hasParent = std::any_of(set.bodies.begin(), set.bodies.end(),
                [&](const BonePhysicsBody& body) { return body.bone == constraint.parent; });
            if (!hasChild || !hasParent)
            {
                bindDiagnostics.push_back({set.name, "missing-constraint-body",
                    constraint.child + " -> " + constraint.parent});
                complete = false;
            }
            constraintBones.emplace_back(constraint.child, constraint.parent);
        }
        if (!complete)
            continue;

        std::vector<JPH::BodyID> ids;
        std::vector<JPH::Body*> createdBodies;
        for (size_t index = 0; index < set.bodies.size(); ++index)
        {
            const auto& body = set.bodies[index];
            const int joint = bodyJoints[index];
            const Eigen::Matrix4f& world = bindGlobals[static_cast<size_t>(joint)];
            const Eigen::Vector3f center = world.block<3, 1>(0, 3) +
                world.block<3, 3>(0, 0) * body.shape.center;
            const Eigen::Quaternionf rotation = Eigen::Quaternionf(
                world.block<3, 3>(0, 0)).normalized() *
                CapsuleAxisRotation(body.shape.rotation);

            // Jolt 的 ShapeSettings 是引用计数对象，必须堆分配；栈对象会导致
            // RotatedTranslatedShapeSettings 析构时重复释放。
            JPH::Ref<JPH::ShapeSettings> capsule = new JPH::CapsuleShapeSettings(
                body.shape.halfHeight * CentimetersToMeters,
                body.shape.radius * CentimetersToMeters);
            JPH::RotatedTranslatedShapeSettings shape(JPH::Vec3::sZero(),
                JPH::Quat::sIdentity(), capsule);
            JPH::ShapeSettings::ShapeResult shapeResult = shape.Create();
            if (shapeResult.HasError())
            {
                bindDiagnostics.push_back({set.name, "shape-error",
                    std::string(shapeResult.GetError().c_str())});
                complete = false;
                break;
            }

            JPH::BodyCreationSettings creation(shapeResult.Get(),
                JoltVector(center), JoltRotation(rotation),
                body.simulated ? JPH::EMotionType::Dynamic : JPH::EMotionType::Kinematic,
                body.simulated ? SimulatedLayer : KinematicLayer);
            creation.mLinearDamping = body.linearDamping;
            creation.mAngularDamping = body.angularDamping;
            creation.mAllowSleeping = false;
            if (body.simulated && body.mass > 0.0f)
            {
                creation.mOverrideMassProperties =
                    JPH::EOverrideMassProperties::CalculateInertia;
                creation.mMassPropertiesOverride.mMass = body.mass;
            }
            const JPH::Body* created = interface.CreateBody(creation);
            if (!created)
            {
                bindDiagnostics.push_back({set.name, "body-create-failed", body.bone});
                complete = false;
                break;
            }
            ids.push_back(created->GetID());
            createdBodies.push_back(const_cast<JPH::Body*>(created));
        }
        if (!complete || ids.size() != set.bodies.size())
            continue;

        for (size_t index = 0; index < ids.size(); ++index)
        {
            interface.AddBody(ids[index], JPH::EActivation::Activate);
            if (set.bodies[index].simulated) ++state.simulatedBodies;
            impl->bodies.push_back(
                {bodyJoints[index], set.bodies[index].simulated, ids[index],
                 set.bodies[index].shape, set.bodies[index].bone, std::string()});
            for (const auto& constraint : set.constraints)
                if (constraint.child == set.bodies[index].bone)
                    impl->bodies.back().constraintParent = constraint.parent;
        }

        bool constraintsBound = true;
        for (const auto& constraint : set.constraints)
        {
            size_t childIndex = 0, parentIndex = 0;
            for (size_t index = 0; index < set.bodies.size(); ++index)
            {
                if (set.bodies[index].bone == constraint.child) childIndex = index;
                if (set.bodies[index].bone == constraint.parent) parentIndex = index;
            }
            // 两端都是 kinematic 的约束不会产生任何模拟，UE 数据里也确实存在这种
            // 残留（裙摆中背根）。直接跳过并记录，而不是让 Jolt 建一个空约束。
            if (!set.bodies[childIndex].simulated && !set.bodies[parentIndex].simulated)
            {
                bindDiagnostics.push_back({set.name, "kinematic-kinematic-constraint",
                    constraint.name});
                continue;
            }
            JPH::SwingTwistConstraintSettings settings;
            // 约束帧用 body 局部空间（COM 相对），与角色 component 无关；
            // WorldSpace 帧会在角色移动/旋转后失效。
            settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
            settings.mPosition1 = JoltVector(constraint.childPosition -
                set.bodies[childIndex].shape.center);
            settings.mPosition2 = JoltVector(constraint.parentPosition -
                set.bodies[parentIndex].shape.center);
            const Eigen::Quaternionf childRotation = constraint.childRotation;
            const Eigen::Quaternionf parentRotation = constraint.parentRotation;
            settings.mTwistAxis1 = JoltVector(childRotation * Eigen::Vector3f::UnitX(), 1.0f)
                .Normalized();
            settings.mPlaneAxis1 = JoltVector(childRotation * Eigen::Vector3f::UnitY(), 1.0f)
                .Normalized();
            settings.mTwistAxis2 = JoltVector(parentRotation * Eigen::Vector3f::UnitX(), 1.0f)
                .Normalized();
            settings.mPlaneAxis2 = JoltVector(parentRotation * Eigen::Vector3f::UnitY(), 1.0f)
                .Normalized();
            settings.mSwingType = JPH::ESwingType::Pyramid;
            auto angle = [](const BonePhysicsLimit& limit)
            {
                switch (limit.motion)
                {
                case BonePhysicsMotion::Locked: return 0.0f;
                case BonePhysicsMotion::Limited: return limit.degrees * DegreesToRadians;
                case BonePhysicsMotion::Free:
                default: return Pi;
                }
            };
            settings.mPlaneHalfConeAngle = angle(constraint.swing1);
            settings.mNormalHalfConeAngle = angle(constraint.swing2);
            switch (constraint.twist.motion)
            {
            case BonePhysicsMotion::Locked:
                settings.mTwistMinAngle = settings.mTwistMaxAngle = 0.0f;
                break;
            case BonePhysicsMotion::Limited:
                settings.mTwistMinAngle = -constraint.twist.degrees * DegreesToRadians;
                settings.mTwistMaxAngle = constraint.twist.degrees * DegreesToRadians;
                break;
            case BonePhysicsMotion::Free:
            default:
                settings.mTwistMinAngle = -Pi;
                settings.mTwistMaxAngle = Pi;
                break;
            }
            JPH::Body* childBody = childIndex < createdBodies.size() ?
                createdBodies[childIndex] : nullptr;
            JPH::Body* parentBody = parentIndex < createdBodies.size() ?
                createdBodies[parentIndex] : nullptr;
            if (!childBody || !parentBody)
            {
                bindDiagnostics.push_back({set.name, "constraint-body-missing",
                    constraint.name});
                constraintsBound = false;
                continue;
            }
            JPH::Ref<JPH::Constraint> created = settings.Create(*childBody, *parentBody);
            if (!created)
            {
                bindDiagnostics.push_back({set.name, "constraint-create-failed",
                    constraint.name});
                constraintsBound = false;
                continue;
            }
            impl->system.AddConstraint(created);
            impl->constraints.push_back(created);
            ++state.constraints;
        }
        if (!constraintsBound)
            bindDiagnostics.push_back({set.name, "constraints-incomplete",
                "one or more constraints could not be created"});
        ++validSets;
    }
    const JPH::Vec3 gravity = this->asset->sets.empty() ? JPH::Vec3(0, -9.81f, 0) :
        JoltVector(this->asset->sets.front().gravity);
    impl->system.SetGravity(gravity);
    state.active = validSets > 0;
    if (validSets == 0) state.reason = "no-valid-sets";
}

BonePhysicsSolver::~BonePhysicsSolver() = default;

const BonePhysicsAsset* BonePhysicsSolver::Asset() const
{
    return asset.get();
}

void BonePhysicsSolver::Reset(const char* reason)
{
    impl->firstFrame = true;
    state.active = false;
    state.reason = reason ? reason : "reset";
}

bool BonePhysicsSolver::Solve(std::vector<Eigen::Matrix4f>& globals, float deltaSeconds)
{
    if (!impl || validSets == 0 || globals.empty())
        return false;
    if (deltaSeconds <= 0.0f || !std::isfinite(deltaSeconds))
        return false;
    const float dt = std::min(deltaSeconds, 0.05f);
    const size_t jointCount = globals.size();

    JPH::BodyInterface& interface = impl->system.GetBodyInterface();
    float maxDisplacement = 0.0f;
    bool moved = false;
    for (const auto& body : impl->bodies)
    {
        if (body.joint < 0 || static_cast<size_t>(body.joint) >= jointCount ||
            static_cast<size_t>(body.joint) >= globals.size())
            continue;
        const Eigen::Matrix4f& world = globals[static_cast<size_t>(body.joint)];
        const Eigen::Vector3f position = world.block<3, 1>(0, 3);
        const Eigen::Quaternionf rotation = Eigen::Quaternionf(
            world.block<3, 3>(0, 0)).normalized();
        if (!body.simulated)
        {
            if (impl->firstFrame)
                interface.SetPositionAndRotation(body.id, JoltVector(position),
                    JoltRotation(rotation), JPH::EActivation::DontActivate);
            else
            {
                interface.MoveKinematic(body.id, JoltVector(position), JoltRotation(rotation), dt);
                const JPH::Vec3 current = interface.GetPosition(body.id);
                const float delta = (JoltVector(position) - current).Length() * MetersToCentimeters;
                maxDisplacement = std::max(maxDisplacement, delta);
                moved = true;
            }
        }
        else if (impl->firstFrame)
        {
            // 首帧把 simulated 也吸附到当前姿态，避免刚体从绑定姿态“跳”进模拟。
            interface.SetPositionAndRotation(body.id, JoltVector(position),
                JoltRotation(rotation), JPH::EActivation::Activate);
            interface.SetLinearAndAngularVelocity(body.id, JPH::Vec3::sZero(),
                JPH::Vec3::sZero());
        }
    }
    // 传送保护：根骨单帧跳变过大时把 simulated 刚体直接吸附回姿态，避免爆开。
    if (maxDisplacement > 50.0f)
    {
        for (const auto& body : impl->bodies)
        {
            if (!body.simulated) continue;
            const Eigen::Vector3f position = globals[static_cast<size_t>(body.joint)]
                .block<3, 1>(0, 3);
            const Eigen::Quaternionf rotation = Eigen::Quaternionf(
                globals[static_cast<size_t>(body.joint)].block<3, 3>(0, 0)).normalized();
            interface.SetPositionAndRotation(body.id, JoltVector(position),
                JoltRotation(rotation), JPH::EActivation::Activate);
            interface.SetLinearAndAngularVelocity(body.id, JPH::Vec3::sZero(),
                JPH::Vec3::sZero());
        }
    }
    impl->firstFrame = false;
    impl->system.Update(dt, 1, &impl->tempAllocator, &impl->jobSystem);

    bool changed = false;
    for (const auto& body : impl->bodies)
    {
        if (!body.simulated) continue;
        if (body.joint < 0 || static_cast<size_t>(body.joint) >= jointCount) continue;
        const size_t joint = static_cast<size_t>(body.joint);
        JPH::RVec3 position;
        JPH::Quat rotation;
        interface.GetPositionAndRotation(body.id, position, rotation);
        Eigen::Matrix4f updated = Eigen::Matrix4f::Identity();
        updated.block<3, 3>(0, 0) = EigenRotation(rotation).toRotationMatrix();
        updated.block<3, 1>(0, 3) = EigenVector(JPH::Vec3(position));
        globals[joint] = updated;
        changed = true;
    }
    state.maxDisplacement = maxDisplacement;
    state.active = validSets > 0 && dt > 0.0f;
    state.reason = moved ? "simulating" : "idle";
    return changed;
}

void BonePhysicsSolver::CollectDebugBodies(const std::vector<Eigen::Matrix4f>& globals,
    std::vector<BonePhysicsDebugBody>& out) const
{
    out.clear();
    if (!impl || globals.empty()) return;
    for (const auto& body : impl->bodies)
    {
        if (body.joint < 0 || static_cast<size_t>(body.joint) >= globals.size()) continue;
        const Eigen::Matrix4f& world = globals[static_cast<size_t>(body.joint)];
        BonePhysicsDebugBody debug;
        debug.bone = body.bone;
        debug.constraintParent = body.constraintParent;
        debug.simulated = body.simulated;
        debug.radius = body.shape.radius;
        debug.halfHeight = body.shape.halfHeight;
        debug.center = world.block<3, 1>(0, 3) +
            world.block<3, 3>(0, 0) * body.shape.center;
        debug.rotation = Eigen::Quaternionf(world.block<3, 3>(0, 0)).normalized() *
            CapsuleAxisRotation(body.shape.rotation);
        out.push_back(std::move(debug));
    }
}
}
