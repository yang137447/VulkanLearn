#pragma once

#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <nlohmann/json.hpp>
#include "hair/tressfxBehavior.h"

namespace VL
{
inline void ValidateScriptPhysics(const nlohmann::json& physics);
inline void ValidateScriptPrimitiveCollision(const nlohmann::json& collision);
// 纯 CPU 组件树：作者 JSON 经一次拓扑校验后，以完整矩阵供候选加载、脚本和渲染快照消费。
// 父级非均匀缩放产生的 shear 保留在矩阵中，不做有损 Euler/TRS 分解。
inline std::string ScriptComponentId(const nlohmann::json& component) { return component.value("id", "root"); }

inline bool IsScriptLight(const std::string& type)
{
    return type == "pointLight" || type == "spotLight" || type == "directionalLight";
}

// tressfx 组件在场景加载侧就是一个 mesh 对象（一个 guide section、一个材质槽），
// 组件类型只多出“父级绑定”语义；模板、同步与变换处理和 mesh 组件共用同一路径。
inline bool IsScriptMeshComponent(const std::string& type)
{
    return type == "mesh" || type == "tressfx";
}

// 最近的 mesh 祖先组件；没有则 nullptr。tressfx 组件的绑定来源（源角色与骨架）
// 就是它。组件树已由 ValidateScriptComponents 收口，这里不再做环检测。
inline const nlohmann::json* ScriptComponentMeshAncestor(
    const nlohmann::json& components, const nlohmann::json& component)
{
    auto ancestor = component.value("parent", "");
    while (!ancestor.empty())
    {
        const nlohmann::json* found = nullptr;
        for (const auto& node : components)
            if (ScriptComponentId(node) == ancestor) { found = &node; break; }
        if (found == nullptr) return nullptr;
        if (found->value("type", "") == "mesh") return found;
        ancestor = found->value("parent", "");
    }
    return nullptr;
}

inline bool IsScriptLightNumber(const std::string& field)
{
    return field == "intensity" || field == "coneAngleInner" || field == "coneAngleOuter";
}

inline float ScriptLightDefault(const std::string& field)
{
    return field == "intensity" ? 1000.0f : field == "coneAngleOuter" ? 45.0f : 30.0f;
}

inline void ValidateScriptVector(const nlohmann::json& value, bool scale = false)
{
    if (!value.is_array() || value.size() != 3) throw std::invalid_argument("Script transform requires vec3");
    for (const auto& number : value)
        if (!number.is_number() || !std::isfinite(number.get<float>()) ||
            (scale && std::abs(number.get<float>()) < 1e-6f))
            throw std::invalid_argument("Invalid Script transform vector");
}

inline void ValidateScriptComponents(const nlohmann::json& components)
{
    if (!components.is_array() || components.size() > 128)
        throw std::invalid_argument("Blueprint supports at most 128 components");
    std::map<std::string, std::string> parents;
    for (const auto& component : components)
    {
        if (!component.is_object()) throw std::invalid_argument("Component requires object");
        const auto type = component.value("type", "");
        if (!IsScriptMeshComponent(type) && type != "scene" && type != "camera" && type != "springArm" && !IsScriptLight(type))
            throw std::invalid_argument("Unsupported component type");
        const auto id = ScriptComponentId(component);
        if (id.empty() || id.find_first_of("/\\|\0", 0, 4) != std::string::npos ||
            (components.size() > 1 && !component.contains("id")) ||
            !parents.emplace(id, component.value("parent", "")).second)
            throw std::invalid_argument("Components require unique stable ids without separators");
        for (const auto& field : component.items())
            if (field.key() != "id" && field.key() != "parent" && field.key() != "type" &&
                field.key() != "position" && field.key() != "rotation" && field.key() != "scale" && field.key() != "collision" &&
                !(type == "mesh" && (field.key() == "modelPath" || field.key() == "materialOverride")) &&
                !(type == "tressfx" && (field.key() == "modelPath" || field.key() == "rootBone" ||
                    field.key() == "weights" || field.key() == "simulation" ||
                    field.key() == "hairCollision" || field.key() == "materialOverride")) &&
                !(type == "camera" && (field.key() == "fov" || field.key() == "near_clip" ||
                    field.key() == "far_clip" || field.key() == "autoActivate" || field.key() == "priority")) &&
                !(type == "springArm" && (field.key() == "targetArmLength" || field.key() == "socketOffset" ||
                    field.key() == "targetOffset" || field.key() == "useCameraLagSubstepping" ||
                    field.key() == "cameraLagMaxTimeStep" ||
                    field.key() == "probeRadius" || field.key() == "doCollisionTest" || field.key() == "collisionMask" ||
                    field.key() == "enableCameraLag" || field.key() == "cameraLagSpeed" ||
                    field.key() == "cameraLagMaxDistance" || field.key() == "enableCameraRotationLag" ||
                    field.key() == "cameraRotationLagSpeed" || field.key() == "usePawnControlRotation" ||
                    field.key() == "inheritPitch" || field.key() == "inheritYaw" || field.key() == "inheritRoll" ||
                    field.key() == "absoluteRotation" || field.key() == "rotationOffset" || field.key() == "drawDebug")) &&
                !(IsScriptLight(type) && (field.key() == "color" || field.key() == "intensity" ||
                    field.key() == "intensityUnit" || field.key() == "visible" ||
                    (type == "spotLight" && (field.key() == "coneAngleInner" || field.key() == "coneAngleOuter")))))
                throw std::invalid_argument("Unsupported component field: " + field.key());
        for (const char* field : {"position", "rotation", "scale"})
            if (component.contains(field)) ValidateScriptVector(component.at(field), std::string(field) == "scale");
        if (component.contains("collision"))
        {
            ValidateScriptPrimitiveCollision(component.at("collision"));
        }
        for (const char* field : {"modelPath", "materialOverride"})
            if ((type == "mesh" && std::string(field) == "modelPath") || component.contains(field))
                if (!component.contains(field) || !component.at(field).is_string() ||
                    component.at(field).get<std::string>().empty())
                    throw std::invalid_argument("Mesh component requires non-empty asset paths");
        if (type == "tressfx")
        {
            nlohmann::json behavior = nlohmann::json::object();
            if (component.contains("simulation")) behavior["simulation"] = component.at("simulation");
            if (component.contains("hairCollision")) behavior["hairCollision"] = component.at("hairCollision");
            if (component.contains("rootBone"))
                behavior["skinning"] = {{"rootBone", component.at("rootBone")}};
            try { Hair::ReadTressfxPlacementBehavior(behavior, id); }
            catch (const std::exception& error) { throw std::invalid_argument(error.what()); }
            // 毛发组件必须自带 tressfx 描述符；父级（源角色）由组件树决定，不再
            // 通过描述符里的场景对象名写死。
            if (!component.contains("modelPath") || !component.at("modelPath").is_string() ||
                component.at("modelPath").get<std::string>().empty())
                throw std::invalid_argument("TressFX component requires a non-empty modelPath");
            if (!component.contains("rootBone") || !component.at("rootBone").is_string() ||
                component.at("rootBone").get<std::string>().empty())
                throw std::invalid_argument("TressFX component rootBone requires a non-empty string");
            if (component.contains("weights"))
            {
                const auto& weights = component.at("weights");
                const std::string value = weights.is_string()
                    ? weights.get<std::string>() : std::string();
                if (value != "auto" && value != "rootBone" && value != "mesh")
                    throw std::invalid_argument(
                        "TressFX component weights must be auto/rootBone/mesh");
            }
        }
        if (type == "camera")
        {
            for (const char* field : {"fov", "near_clip", "far_clip"})
                if (component.contains(field) && (!component.at(field).is_number() ||
                    !std::isfinite(component.at(field).get<float>())))
                    throw std::invalid_argument("Camera projection requires finite numbers");
            const float fov = component.value("fov", 60.0f);
            const float nearClip = component.value("near_clip", 1.0f);
            const float farClip = component.value("far_clip", 10000.0f);
            if (fov <= 0 || fov >= 179 || nearClip <= 0 || farClip <= nearClip)
                throw std::invalid_argument("Invalid camera component projection");
            if (component.contains("autoActivate") && !component.at("autoActivate").is_boolean())
                throw std::invalid_argument("Camera autoActivate requires boolean");
            if (component.contains("priority") && (!component.at("priority").is_number() ||
                !std::isfinite(component.at("priority").get<double>()) ||
                std::floor(component.at("priority").get<double>()) != component.at("priority").get<double>() ||
                component.at("priority").get<double>() < -100000 || component.at("priority").get<double>() > 100000))
                throw std::invalid_argument("Camera priority requires integer");
        }
        if (type == "springArm")
        {
            for (const char* field : {"targetArmLength", "probeRadius"})
                if (component.contains(field) && (!component.at(field).is_number() ||
                    !std::isfinite(component.at(field).get<float>()) || component.at(field).get<float>() < 0 ||
                    component.at(field).get<float>() > 100000))
                    throw std::invalid_argument("SpringArm requires bounded finite numbers");
            if (component.contains("socketOffset")) ValidateScriptVector(component.at("socketOffset"));
            if (component.contains("targetOffset")) ValidateScriptVector(component.at("targetOffset"));
            if (component.contains("doCollisionTest") && !component.at("doCollisionTest").is_boolean())
                throw std::invalid_argument("SpringArm doCollisionTest requires boolean");
            if (component.contains("collisionMask") && (!component.at("collisionMask").is_number() ||
                !std::isfinite(component.at("collisionMask").get<double>()) ||
                component.at("collisionMask").get<double>() < 0 || component.at("collisionMask").get<double>() > 65535 ||
                std::floor(component.at("collisionMask").get<double>()) != component.at("collisionMask").get<double>()))
                throw std::invalid_argument("SpringArm collisionMask requires 16-bit integer");
            const std::pair<const char*, float> lagFields[] = {
                {"cameraLagSpeed", 1000.0f}, {"cameraLagMaxDistance", 100000.0f},
                {"cameraRotationLagSpeed", 1000.0f}};
            for (const auto& field : lagFields)
                if (component.contains(field.first) && (!component.at(field.first).is_number() ||
                    !std::isfinite(component.at(field.first).get<float>()) ||
                    component.at(field.first).get<float>() < 0 ||
                    component.at(field.first).get<float>() > field.second))
                    throw std::invalid_argument("SpringArm lag requires bounded finite numbers");
            if (component.contains("rotationOffset") && !component.at("rotationOffset").is_array())
                throw std::invalid_argument("SpringArm rotationOffset requires vector");
            if (component.contains("rotationOffset")) ValidateScriptVector(component.at("rotationOffset"));
            if (component.contains("cameraLagMaxTimeStep") && (!component.at("cameraLagMaxTimeStep").is_number() ||
                !std::isfinite(component.at("cameraLagMaxTimeStep").get<float>()) ||
                component.at("cameraLagMaxTimeStep").get<float>() < 0.005f ||
                component.at("cameraLagMaxTimeStep").get<float>() > 0.5f))
                throw std::invalid_argument("SpringArm lag step requires 0.005..0.5 seconds");
            for (const char* field : {"doCollisionTest", "enableCameraLag", "enableCameraRotationLag",
                "usePawnControlRotation", "inheritPitch", "inheritYaw", "inheritRoll", "absoluteRotation", "drawDebug",
                "useCameraLagSubstepping"})
                if (component.contains(field) && !component.at(field).is_boolean())
                    throw std::invalid_argument("SpringArm toggles require boolean");
        }
        if (IsScriptLight(type))
        {
            // 组件直接使用渲染快照的规范单位；不接受 lm，避免运行时修改锥角时重新解释强度。
            if (component.value("intensityUnit", "") != (type == "directionalLight" ? "lux" : "cd"))
                throw std::invalid_argument("Light component requires canonical intensityUnit");
            if (component.contains("color"))
            {
                ValidateScriptVector(component.at("color"));
                for (const auto& value : component.at("color"))
                    if (value.get<float>() < 0 || value.get<float>() > 1)
                        throw std::invalid_argument("Light color requires linear RGB in [0,1]");
            }
            if (component.contains("visible") && !component.at("visible").is_boolean())
                throw std::invalid_argument("Light visible requires boolean");
            for (const char* field : {"intensity", "coneAngleInner", "coneAngleOuter"})
                if (component.contains(field) && (!component.at(field).is_number() ||
                    !std::isfinite(component.at(field).get<float>()) || component.at(field).get<float>() < 0 ||
                    (std::string(field) == "intensity" && component.at(field).get<float>() > 1e12f)))
                    throw std::invalid_argument("Invalid light component number");
            if (type == "spotLight" && (component.value("coneAngleInner", 30.0f) >=
                component.value("coneAngleOuter", 45.0f) || component.value("coneAngleOuter", 45.0f) >= 90))
                throw std::invalid_argument("Spot component requires 0 <= inner < outer < 90 degrees");
        }
    }
    for (const auto& entry : parents)
    {
        std::set<std::string> visited;
        auto id = entry.first;
        while (!id.empty())
        {
            if (!visited.insert(id).second) throw std::invalid_argument("Component parent cycle");
            const auto parent = parents.find(id);
            if (parent == parents.end()) throw std::invalid_argument("Unknown component parent");
            id = parent->second;
        }
    }
    // tressfx 组件的绑定来源是最近的 mesh 祖先：没有它就没有源角色骨架与 SkinPose，
    // 这里在组件树拓扑收口时就拒绝，不等加载期才报“找不到源对象”。
    for (const auto& component : components)
        if (component.value("type", "") == "tressfx" &&
            ScriptComponentMeshAncestor(components, component) == nullptr)
            throw std::invalid_argument(
                "TressFX component \"" + ScriptComponentId(component) +
                "\" requires a mesh component ancestor");
}

inline void ValidateScriptPhysics(const nlohmann::json& physics)
{
    if (!physics.is_object()) throw std::invalid_argument("physics requires object");
    const auto motion = physics.value("motion", "static");
    const auto shape = physics.value("shape", "box");
    const bool character = motion == "character";
    if ((motion != "static" && motion != "dynamic" && motion != "kinematic" && !character) ||
        (shape != "box" && shape != "sphere" && shape != "capsule" && shape != "compound" && shape != "mesh") ||
        (shape == "mesh" && motion != "static") ||
        (character && shape != "capsule"))
        throw std::invalid_argument("Unsupported physics motion/shape; character requires capsule");
    for (const auto& field : physics.items())
        if (field.key() != "motion" && field.key() != "shape" && field.key() != "halfExtents" &&
            field.key() != "radius" && field.key() != "mass" && field.key() != "friction" &&
            field.key() != "restitution" && field.key() != "trigger" && field.key() != "halfHeight" &&
            field.key() != "stepHeight" && field.key() != "jumpSpeed" && field.key() != "maxSlopeAngle" &&
            field.key() != "collisionLayer" && field.key() != "collisionMask" && field.key() != "shapes" &&
            field.key() != "constraints" && field.key() != "component")
            throw std::invalid_argument("Unknown physics field");
    for (const char* field : {"collisionLayer", "collisionMask"})
        if (physics.contains(field) && (!physics.at(field).is_number_integer() ||
            physics.at(field).get<int64_t>() < 0 ||
            physics.at(field).get<int64_t>() > (std::string(field) == "collisionLayer" ? 15 : 65535)))
            throw std::invalid_argument("Invalid collision layer/mask");
    if (shape == "mesh" && (!physics.contains("component") || !physics.at("component").is_string() ||
        physics.at("component").get<std::string>().empty()))
        throw std::invalid_argument("Mesh physics requires a mesh component id");
    if (physics.contains("shapes"))
    {
        const auto& shapes = physics.at("shapes");
        if (shape != "compound" || !shapes.is_array() || shapes.empty() || shapes.size() > 128)
            throw std::invalid_argument("Compound requires 1..128 shapes");
        for (auto primitive : shapes)
        {
            if (!primitive.contains("component") || !primitive.at("component").is_string())
                throw std::invalid_argument("Compound shape requires component id");
            primitive.erase("component");
            ValidateScriptPrimitiveCollision(primitive);
        }
    }
    if (physics.contains("constraints"))
    {
        const auto& constraints = physics.at("constraints");
        if (!constraints.is_array() || constraints.size() > 32 || character)
            throw std::invalid_argument("Physics constraints require at most 32 non-character joints");
        std::set<std::string> ids;
        for (const auto& joint : constraints)
        {
            if (!joint.is_object()) throw std::invalid_argument("Constraint requires object");
            const auto type = joint.value("type", "");
            if ((type != "fixed" && type != "hinge") || !joint.contains("target") ||
                !joint.at("target").is_string() || joint.at("target").get<std::string>().empty() ||
                !joint.contains("id") || !joint.at("id").is_string() || joint.at("id").get<std::string>().empty() ||
                !ids.insert(joint.at("id").get<std::string>()).second)
                throw std::invalid_argument("Constraint requires unique id, fixed/hinge and target");
            for (const auto& field : joint.items())
                if (field.key() != "id" && field.key() != "type" && field.key() != "target" &&
                    field.key() != "anchor" &&
                    !(type == "hinge" && (field.key() == "axis" || field.key() == "minAngle" || field.key() == "maxAngle")))
                    throw std::invalid_argument("Unknown constraint field");
            ValidateScriptVector(joint.at("anchor"));
            if (type == "hinge")
            {
                ValidateScriptVector(joint.at("axis"));
                const auto& axis = joint.at("axis");
                const double length = std::sqrt(axis[0].get<double>() * axis[0].get<double>() +
                    axis[1].get<double>() * axis[1].get<double>() + axis[2].get<double>() * axis[2].get<double>());
                if (std::abs(length - 1) > 1e-4 || !std::isfinite(joint.value("minAngle", -180.0)) ||
                    !std::isfinite(joint.value("maxAngle", 180.0)) || joint.value("minAngle", -180.0) < -180 ||
                    joint.value("maxAngle", 180.0) > 180 ||
                    joint.value("minAngle", -180.0) > 0 || joint.value("maxAngle", 180.0) < 0 ||
                    joint.value("minAngle", -180.0) > joint.value("maxAngle", 180.0))
                    throw std::invalid_argument("Hinge requires unit axis and ordered degree limits");
            }
        }
    }
    if (physics.contains("trigger") && !physics.at("trigger").is_boolean())
        throw std::invalid_argument("trigger requires boolean");
    if (character && physics.value("trigger", false))
        throw std::invalid_argument("Character cannot be a trigger");
    if (shape != "capsule" && physics.contains("halfHeight"))
        throw std::invalid_argument("Only capsule can specify halfHeight");
    for (const char* field : {"stepHeight", "jumpSpeed", "maxSlopeAngle"})
        if (physics.contains(field) && (!character || !physics.at(field).is_number() ||
            !std::isfinite(physics.at(field).get<float>()) || physics.at(field).get<float>() < 0 ||
            (std::string(field) == "maxSlopeAngle" && physics.at(field).get<float>() >= 90)))
            throw std::invalid_argument("Invalid character setting");
    if (shape == "box")
    {
        ValidateScriptVector(physics.at("halfExtents"));
        for (const auto& value : physics.at("halfExtents"))
            if (value.get<float>() <= 0) throw std::invalid_argument("Box halfExtents must be positive cm");
        if (physics.contains("radius")) throw std::invalid_argument("Box cannot specify radius");
    }
    else if (shape != "compound" && shape != "mesh" && (!physics.contains("radius") || !physics.at("radius").is_number() ||
        !std::isfinite(physics.at("radius").get<float>()) || physics.at("radius").get<float>() <= 0 ||
        physics.contains("halfExtents"))) throw std::invalid_argument("Sphere/capsule requires positive radius cm");
    // halfHeight 只表示胶囊直段的一半；上下半球另计，防止作者把总高度当直段长度。
    if (shape == "capsule" && (!physics.contains("halfHeight") || !physics.at("halfHeight").is_number() ||
        !std::isfinite(physics.at("halfHeight").get<float>()) || physics.at("halfHeight").get<float>() <= 0))
        throw std::invalid_argument("Capsule requires positive halfHeight cm");
    for (const char* field : {"mass", "friction", "restitution"})
        if (physics.contains(field) && (!physics.at(field).is_number() ||
            !std::isfinite(physics.at(field).get<float>()) || physics.at(field).get<float>() < 0 ||
            (std::string(field) == "mass" && physics.at(field).get<float>() <= 0) ||
            (std::string(field) == "restitution" && physics.at(field).get<float>() > 1)))
            throw std::invalid_argument("Invalid rigid body coefficient");
}

inline void ValidateScriptPrimitiveCollision(const nlohmann::json& collision)
{
    if (!collision.is_object()) throw std::invalid_argument("Component collision requires object");
    for (const auto& field : collision.items())
        if (field.key() != "shape" && field.key() != "halfExtents" && field.key() != "radius" && field.key() != "halfHeight")
            throw std::invalid_argument("Primitive collision only accepts shape dimensions");
    const auto shape = collision.value("shape", "box");
    if (shape != "box" && shape != "sphere" && shape != "capsule")
        throw std::invalid_argument("Component collision requires primitive shape");
    ValidateScriptPhysics(collision);
}

inline Eigen::Vector3f ScriptVector(const nlohmann::json& node, const char* field, float fallback = 0)
{
    if (!node.contains(field)) return Eigen::Vector3f::Constant(fallback);
    const auto& value = node.at(field);
    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
}

inline Eigen::Matrix4f ScriptTransformMatrix(const nlohmann::json& node)
{
    const Eigen::Vector3f rotation = ScriptVector(node, "rotation") * (3.14159265358979323846f / 180);
    const Eigen::Quaternionf q = Eigen::AngleAxisf(rotation.y(), Eigen::Vector3f::UnitY()) *
        Eigen::AngleAxisf(rotation.x(), Eigen::Vector3f::UnitX()) *
        Eigen::AngleAxisf(rotation.z(), Eigen::Vector3f::UnitZ());
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix.block<3, 3>(0, 0) = q.toRotationMatrix() * ScriptVector(node, "scale", 1).asDiagonal();
    matrix.block<3, 1>(0, 3) = ScriptVector(node, "position");
    return matrix;
}

inline Eigen::Matrix4f ScriptComponentMatrix(const nlohmann::json& blueprint, size_t index);

inline void ValidateScriptPhysicsComponents(const nlohmann::json& blueprint)
{
    bool componentCollisions = false;
    for (const auto& component : blueprint.value("components", nlohmann::json::array()))
        componentCollisions = componentCollisions || component.contains("collision");
    if (!blueprint.contains("physics"))
    {
        if (componentCollisions) throw std::invalid_argument("Component collisions require a compound root body");
        return;
    }
    const auto& physics = blueprint.at("physics");
    const auto shape = physics.value("shape", "box");
    if (componentCollisions && shape != "compound")
        throw std::invalid_argument("Component collisions require a compound root body");
    if (shape != "compound" && shape != "mesh") return;
    const auto components = blueprint.value("components", nlohmann::json::array());
    auto shapes = physics.value("shapes", nlohmann::json::array());
    if (shape == "mesh") shapes.push_back({{"component", physics.at("component")}});
    else if (shapes.empty())
        for (const auto& node : components)
            if (node.contains("collision")) shapes.push_back({{"component", ScriptComponentId(node)}});
    if (shapes.empty()) throw std::invalid_argument("Compound requires component collision geometry");
    for (const auto& child : shapes)
    {
        const auto id = child.at("component").get<std::string>();
        size_t index = 0;
        while (index < components.size() && ScriptComponentId(components[index]) != id) ++index;
        if (index == components.size() || (shape == "mesh" && components[index].at("type") != "mesh"))
            throw std::invalid_argument("Physics references missing or incompatible component");
        if (shape == "mesh") continue;
        const Eigen::Matrix4f matrix = ScriptComponentMatrix(blueprint, index);
        if (!matrix.allFinite()) throw std::invalid_argument("Collision component matrix must be finite");
        const Eigen::Matrix3f linear = matrix.block<3, 3>(0, 0);
        const float scale = linear.col(0).norm();
        const Eigen::Matrix3f rotation = linear / scale;
        if (scale <= 0 || rotation.determinant() <= 0 ||
            !(rotation.transpose() * rotation).isApprox(Eigen::Matrix3f::Identity(), 1e-4f))
            throw std::invalid_argument("Collision components require positive uniform scale without shear");
    }
}

inline Eigen::Quaternionf ScriptRotationDegrees(const Eigen::Vector3f& degrees)
{
    const Eigen::Vector3f rotation = degrees * (3.14159265358979323846f / 180);
    return Eigen::AngleAxisf(rotation.y(), Eigen::Vector3f::UnitY()) *
        Eigen::AngleAxisf(rotation.x(), Eigen::Vector3f::UnitX()) *
        Eigen::AngleAxisf(rotation.z(), Eigen::Vector3f::UnitZ());
}

inline Eigen::Quaternionf ScriptRotation(const nlohmann::json& node)
{
    return ScriptRotationDegrees(ScriptVector(node, "rotation"));
}

inline void ValidateScriptPhysicsReferences(const nlohmann::json& blueprints, const nlohmann::json& templates)
{
    std::map<std::string, const nlohmann::json*> byId;
    for (const auto& blueprint : blueprints) byId.emplace(blueprint.at("id").get<std::string>(), &blueprint);
    // 模板会产生多个运行时实例；外部稳定 ID 无法表达每次 Spawn 的关节归属，先在作者入口拒绝。
    for (const auto& item : templates.items())
        if (!item.value().value("physics", nlohmann::json::object())
            .value("constraints", nlohmann::json::array()).empty())
            throw std::invalid_argument("Spawn templates cannot declare external constraints");
    for (const auto& blueprint : blueprints)
        for (const auto& joint : blueprint.value("physics", nlohmann::json::object())
            .value("constraints", nlohmann::json::array()))
        {
            const auto target = byId.find(joint.at("target").get<std::string>());
            if (target == byId.end() || target->second == &blueprint || !target->second->contains("physics"))
                throw std::invalid_argument("Constraint requires another Blueprint with a body in the same scene");
            const auto ownerMotion = blueprint.at("physics").value("motion", "static");
            const auto targetMotion = target->second->at("physics").value("motion", "static");
            if (ownerMotion == "character" || targetMotion == "character" ||
                (ownerMotion == "static" && targetMotion == "static"))
                throw std::invalid_argument("Constraint requires non-character bodies and at least one moving body");
        }
}

inline Eigen::Vector3f ScriptEulerDegrees(const Eigen::Quaternionf& rotation)
{
    // 与托管 YXZ 逆变换一致；不要用 Eigen 的不同 Euler 取值区间解释逐轴继承。
    const float sinPitch = std::clamp(2 * (rotation.w() * rotation.x() -
        rotation.y() * rotation.z()), -1.0f, 1.0f);
    const float pitch = std::asin(sinPitch);
    float yaw = std::atan2(2 * (rotation.w() * rotation.y() + rotation.z() * rotation.x()),
        1 - 2 * (rotation.x() * rotation.x() + rotation.y() * rotation.y()));
    float roll = std::atan2(2 * (rotation.w() * rotation.z() + rotation.x() * rotation.y()),
        1 - 2 * (rotation.x() * rotation.x() + rotation.z() * rotation.z()));
    if (std::abs(sinPitch) > 0.999999f)
    {
        yaw = 2 * std::atan2(rotation.y(), rotation.w());
        roll = 0;
    }
    return Eigen::Vector3f(pitch, yaw, roll) * 57.29577951308232f;
}

// 已校验组件树的空间求值：Mesh 本身也是普通组件空间，导入 section/bone 空间由各自 owner 继续合成。
// 组件原点与对子级提供的挂接空间分开；普通组件二者相同，SpringArm 的挂接空间在臂端。
struct ScriptComponentSpace
{
    Eigen::Matrix4f worldFromParent = Eigen::Matrix4f::Identity();
    Eigen::Quaternionf parentRotation = Eigen::Quaternionf::Identity();
    Eigen::Matrix4f worldFromComponent = Eigen::Matrix4f::Identity();
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
    Eigen::Matrix4f worldFromAttachment = Eigen::Matrix4f::Identity();
    Eigen::Quaternionf attachmentRotation = Eigen::Quaternionf::Identity();
};

inline ScriptComponentSpace EvaluateScriptComponentSpace(const nlohmann::json& blueprint, size_t index)
{
    const auto& nodes = blueprint.at("components");
    const auto& node = nodes.at(index);
    ScriptComponentSpace result;
    result.worldFromParent = ScriptTransformMatrix(blueprint);
    result.parentRotation = ScriptRotation(blueprint);
    const auto parent = node.value("parent", "");
    for (size_t ancestor = 0; !parent.empty() && ancestor < nodes.size(); ++ancestor)
        if (ScriptComponentId(nodes[ancestor]) == parent)
        {
            const auto space = EvaluateScriptComponentSpace(blueprint, ancestor);
            result.worldFromParent = space.worldFromAttachment;
            result.parentRotation = space.attachmentRotation;
            break;
        }
    result.worldFromComponent = result.worldFromParent * ScriptTransformMatrix(node);
    result.rotation = (result.parentRotation * ScriptRotation(node)).normalized();
    result.worldFromAttachment = result.worldFromComponent;
    result.attachmentRotation = result.rotation;
    if (node.at("type") != "springArm") return result;

    const auto local = ScriptVector(node, "rotation");
    Eigen::Vector3f euler = node.value("absoluteRotation", false) ? local : ScriptEulerDegrees(result.rotation);
    if (!node.value("absoluteRotation", false))
    {
        if (!node.value("inheritPitch", true)) euler.x() = local.x();
        if (!node.value("inheritYaw", true)) euler.y() = local.y();
        if (!node.value("inheritRoll", true)) euler.z() = local.z();
    }
    result.attachmentRotation = (ScriptRotationDegrees(euler) *
        ScriptRotationDegrees(ScriptVector(node, "rotationOffset"))).normalized();
    Eigen::Vector3f endpoint = result.worldFromComponent.block<3, 1>(0, 3) + ScriptVector(node, "targetOffset") +
        result.attachmentRotation * (Eigen::Vector3f(0, 0, node.value("targetArmLength", 300.0f)) +
            ScriptVector(node, "socketOffset"));
    // 运行时 socket 是 CLR 求值后的冻结结果；作者 schema 不接受这个内部字段。
    // 静态预览没有 ControlRotation/碰撞/lag，使用上述作者静止姿态。
    if (node.contains("__springArmSocket"))
    {
        const auto& socket = node.at("__springArmSocket");
        endpoint = ScriptVector(socket, "position");
        result.attachmentRotation = ScriptRotation(socket);
    }
    // 长度不乘父缩放；子组件的局部偏移仍继承挂接空间的完整缩放/shear。
    result.worldFromAttachment.block<3, 3>(0, 0) =
        (result.attachmentRotation * result.rotation.conjugate()).toRotationMatrix() *
        result.worldFromComponent.block<3, 3>(0, 0);
    result.worldFromAttachment.block<3, 1>(0, 3) = endpoint;
    return result;
}

inline Eigen::Matrix4f ScriptComponentMatrix(const nlohmann::json& blueprint, size_t index)
{
    return EvaluateScriptComponentSpace(blueprint, index).worldFromComponent;
}

inline Eigen::Quaternionf ScriptComponentRotation(const nlohmann::json& blueprint, size_t index)
{
    // 正交朝向不从含 shear 的矩阵分解，位置/几何仍保留完整父链矩阵。
    return EvaluateScriptComponentSpace(blueprint, index).rotation;
}

inline std::string ScriptMeshIdentity(const std::string& blueprintId, const nlohmann::json& component)
{
    return "__blueprint/" + blueprintId + (component.contains("id") ? "/" + ScriptComponentId(component) : "");
}

// 组件网格模板键。tressfx 的绑定来源是父组件身份，跨 actor 不能共享模板，
// 否则两个角色的毛发会错误复用同一份绑定；mesh 组件保持原有键不变。
inline std::string ScriptComponentMeshTemplateKey(
    const nlohmann::json& blueprint, const nlohmann::json& component)
{
    std::string key = component.at("modelPath").get<std::string>() + "\n" +
        component.value("materialOverride", "");
    if (component.value("type", "") == "tressfx")
    {
        const nlohmann::json* ancestor =
            ScriptComponentMeshAncestor(blueprint.at("components"), component);
        key += "\n@" + (ancestor == nullptr ? std::string() :
            ScriptMeshIdentity(blueprint.at("id").get<std::string>(), *ancestor));
        for (const char* field : {"simulation", "hairCollision", "rootBone", "weights"})
            if (component.contains(field)) key += "\n" + std::string(field) + "=" + component.at(field).dump();
    }
    return key;
}

// 组件结构签名项。tressfx 追加绑定来源，改父组件会触发重建而不是静默复用源角色。
inline nlohmann::json ScriptComponentMeshStructureEntry(
    const nlohmann::json& blueprint, const nlohmann::json& component)
{
    nlohmann::json entry = {ScriptComponentId(component), component.at("modelPath"),
        component.value("materialOverride", "")};
    if (component.value("type", "") == "tressfx")
    {
        const nlohmann::json* ancestor =
            ScriptComponentMeshAncestor(blueprint.at("components"), component);
        entry.push_back(ancestor == nullptr ? std::string() :
            ScriptMeshIdentity(blueprint.at("id").get<std::string>(), *ancestor));
        for (const char* field : {"simulation", "hairCollision", "rootBone", "weights"})
            entry.push_back(component.value(field, nlohmann::json()));
    }
    return entry;
}
} // namespace VL
