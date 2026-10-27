#include "editor/scene/editorActorProperty.h"

#include <cmath>
#include <limits>

#include "editor/scene/editorSceneActorBuilder.h"
#include "editor/ui/editorSceneActorTree.h"
#include "sceneNode.h"
#include "world/meshSceneObjectIdentity.h"
#include "world/world.h"

namespace VL
{
namespace
{
constexpr float DegreesToRadians = 3.14159265358979323846f / 180.0f;

bool Reject(std::string& error, const char* message)
{
    error = message;
    return false;
}

template<class T>
T* FindNode(const std::unordered_map<std::string, std::shared_ptr<T>>& nodes,
    const std::string& name)
{
    const auto it = nodes.find(name);
    return it == nodes.end() ? nullptr : it->second.get();
}

bool SetNodeTransform(SceneNode& node, const EditorActorPropertyEdit& edit)
{
    Eigen::Vector3f value = edit.property == EditorActorProperty::Position
        ? node.GetPosition() : node.GetRotation();
    value[edit.component] = edit.value;
    if (edit.property == EditorActorProperty::Position) node.SetPosition(value);
    else node.SetRotation(value);
    return true;
}

template<class T>
bool SetLight(T& light, const EditorActorPropertyEdit& edit, std::string& error)
{
    if (edit.property == EditorActorProperty::Position ||
        edit.property == EditorActorProperty::Rotation)
        return SetNodeTransform(light, edit);
    if (edit.property == EditorActorProperty::Color)
    {
        if (edit.value < 0.0f || edit.value > 1.0f)
            return Reject(error, "Linear light colour must be in [0,1].");
        Eigen::Vector3f color = light.GetColor();
        color[edit.component] = edit.value;
        light.SetColor(color);
        return true;
    }
    if (edit.property == EditorActorProperty::Intensity)
    {
        if (edit.value < 0.0f) return Reject(error, "Intensity must be non-negative.");
        light.SetIntensity(edit.value);
        return true;
    }
    return Reject(error, "This property does not belong to this light.");
}
} // namespace

bool ApplyEditorActorProperty(World& world, const EditorActorPropertyEdit& edit,
    std::string& error)
{
    using EditorUi::EditorSceneActorType;
    using P = EditorActorProperty;
    error.clear();
    if (edit.worldGeneration != world.GetGeneration())
        return Reject(error, "The World changed since this edit was produced.");
    if (!std::isfinite(edit.value))
        return Reject(error, "A complete finite number is required.");
    const bool vector = edit.property == P::Position || edit.property == P::Rotation ||
        edit.property == P::Scale || edit.property == P::Color || edit.property == P::Normal;
    if (edit.component >= (vector ? 3u : 1u))
        return Reject(error, "Unknown property component.");
    EditorUi::EditorSceneActorIdentity identity;
    if (!EditorUi::ParseSceneActorIdentity(edit.identity, identity))
        return Reject(error, "Unknown actor identity.");

    if (identity.type == EditorSceneActorType::Script)
    {
        const char* field = edit.property == P::Position ? "position" :
            edit.property == P::Rotation ? "rotation" : edit.property == P::Scale ? "scale" : nullptr;
        if (!field || (edit.property == P::Scale && std::abs(edit.value) < 1e-6f))
            return Reject(error, "Actor editing requires Transform with non-zero scale.");
        if (edit.property == P::Scale)
            for (const auto& actor : world.GetScriptBlueprints())
                if (actor.at("id") == identity.name && actor.contains("physics") &&
                    edit.value != actor.at("scale")[edit.component].get<float>())
                    return Reject(error, "Rigid body scale must stay uniform; edit its authored vec3 together.");
        return world.SetScriptBlueprintTransform(identity.name, field, edit.component, edit.value);
    }
    if (identity.type == EditorSceneActorType::Mesh)
    {
        if (edit.property != P::Position && edit.property != P::Rotation && edit.property != P::Scale)
            return Reject(error, "Mesh editing only supports Transform.");
        for (const auto& entry : world.GetMeshObjects())
        {
            const WorldMeshObject& object = entry.second;
            if (MeshSceneObjectIdentity(object, entry.first) != identity.name) continue;
            Eigen::Vector3f position = object.scenePosition;
            Eigen::Vector3f rotation = object.sceneRotation;
            Eigen::Vector3f scale = object.sceneScale;
            if (edit.property == P::Position) position[edit.component] = edit.value;
            else if (edit.property == P::Rotation) rotation[edit.component] = edit.value;
            else scale[edit.component] = edit.value;
            if ((scale.array().abs() < 1.0e-6f).any())
                return Reject(error, "Scale must be non-zero (negative scale is supported).");
            return world.SetMeshSceneObjectTransform(identity.name, position, rotation, scale);
        }
    }
    else if (identity.type == EditorSceneActorType::DirectionalLight)
    {
        if (auto* light = FindNode(world.GetDirectionalLights(), identity.name))
        {
            if (edit.property == P::CastShadows)
            {
                if (light != world.GetPrimaryDirectionalLight().get() ||
                    (edit.value != 0 && edit.value != 1))
                    return Reject(error, "CSM requires the primary directional light and a boolean value.");
                auto settings = world.GetCsmSettings();
                settings.castShadows = edit.value != 0;
                world.SetCsmSettings(settings);
                return true;
            }
            if (!SetLight(*light, edit, error)) return false;
            if (light == world.GetPrimaryDirectionalLight().get() && edit.property == P::Rotation)
            {
                // 主光旋转同时更新天空的 scene-to-sun 方向；光线方向是其相反数。
                WorldEnvironment environment = world.GetEnvironment();
                environment.skyParameters.sunDirectionIntensity.head<3>() = -light->GetForwardVector();
                world.SetEnvironment(std::move(environment));
            }
            return true;
        }
    }
    else if (identity.type == EditorSceneActorType::PointLight)
    {
        if (auto* light = FindNode(world.GetPointLights(), identity.name))
            return SetLight(*light, edit, error);
    }
    else if (identity.type == EditorSceneActorType::SpotLight)
    {
        if (auto* light = FindNode(world.GetSpotLights(), identity.name))
        {
            if (edit.property != P::InnerCone && edit.property != P::OuterCone)
                return SetLight(*light, edit, error);
            const float angle = edit.value * DegreesToRadians;
            const float inner = edit.property == P::InnerCone ? angle : light->GetConeAngleInner();
            const float outer = edit.property == P::OuterCone ? angle : light->GetConeAngleOuter();
            if (inner < 0.0f || outer <= inner || outer >= 0.5f * 3.14159265358979323846f)
                return Reject(error, "Cone half-angles require 0 <= inner < outer < 90 degrees.");
            light->SetConeAngleInner(inner);
            light->SetConeAngleOuter(outer);
            return true;
        }
    }
    else if (identity.type == EditorSceneActorType::SquareAreaLight)
    {
        const auto it = world.GetSquareAreaLights().find(identity.name);
        if (it != world.GetSquareAreaLights().end())
        {
            WorldSquareAreaLight light = it->second;
            if (edit.property == P::Position) light.position[edit.component] = edit.value;
            else if (edit.property == P::Normal)
            {
                light.normal[edit.component] = edit.value;
                if (light.normal.squaredNorm() <= 1.0e-8f)
                    return Reject(error, "Area normal must be non-zero.");
                light.normal.normalize();
            }
            else if (edit.property == P::Color && edit.value >= 0.0f && edit.value <= 1.0f)
                light.color[edit.component] = edit.value;
            else if (edit.property == P::Intensity && edit.value >= 0.0f)
                light.intensity = edit.value;
            else if (edit.property == P::AreaSize && edit.value > 0.0f)
                light.size = edit.value;
            else return Reject(error, "Invalid area-light property or range.");
            return world.SetSquareAreaLight(identity.name, light);
        }
    }
    else if (identity.type == EditorSceneActorType::Camera)
    {
        const auto& camera = world.GetCamera();
        if (camera && (camera->GetName().empty() ? "Camera" : camera->GetName()) == identity.name)
        {
            if (edit.property == P::Position || edit.property == P::Rotation)
                return SetNodeTransform(*camera, edit);
            if (const char* field = CameraExposureField(edit.property))
            {
                try
                {
                    // 从作者 Camera 的当前状态改一个字段；完整校验成功才写回，
                    // 连续命令互不覆盖，也不把编辑器观察相机当作曝光作者。
                    const CameraExposure candidate = EditCameraExposure(
                        camera->GetExposure(), field, edit.value);
                    camera->SetExposure(candidate);
                    return true;
                }
                catch (const std::invalid_argument& exception)
                {
                    error = exception.what();
                    return false;
                }
            }
            const float fov = edit.property == P::CameraFov ? edit.value : camera->GetHFOV();
            const float nearClip = edit.property == P::CameraNear ? edit.value : camera->GetClipNear();
            const float farClip = edit.property == P::CameraFar ? edit.value : camera->GetClipFar();
            if (edit.property != P::CameraFov && edit.property != P::CameraNear && edit.property != P::CameraFar)
                return Reject(error, "This property does not belong to Camera.");
            if (fov <= 0.0f || fov >= 179.0f || nearClip <= 0.0f || farClip <= nearClip)
                return Reject(error, "Camera requires 0 < FOV < 179 and 0 < near < far.");
            // 由现有投影的比例恢复视口 aspect，改 FOV/clip 不等下一次 resize 才生效。
            const Eigen::Matrix4f projection = camera->GetProjectionMatrix();
            const float aspect = std::abs(projection(1, 1) / projection(0, 0));
            camera->SetProjection(fov, aspect, nearClip, farClip);
            return true;
        }
    }
    return Reject(error, "Actor is not in the active World or is read-only.");
}

bool FrameEditorActor(World& world, uint64_t generation, const std::string& identity)
{
    if (!world.IsEditorViewEnabled() || world.GetGeneration() != generation ||
        !world.GetRenderCamera()) return false;
    for (const auto& actor : BuildEditorSceneActors(world))
    {
        if (actor.identity != identity) continue;
        // 场景风与风剖面是全局/派生状态，没有可框选的空间位置。
        if (actor.type == EditorUi::EditorSceneActorType::Environment ||
            actor.type == EditorUi::EditorSceneActorType::WindProfile ||
            actor.type == EditorUi::EditorSceneActorType::SceneWind) return false;
        Eigen::Vector3f target = actor.position;
        float radius = 150.0f;
        if (actor.type == EditorUi::EditorSceneActorType::Mesh)
        {
            Eigen::Vector3f lower = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
            Eigen::Vector3f upper = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
            // 用所有 section 的变换后角点，而不是作者根位置：源模型可偏心，section 可相距很远。
            for (const auto& section : actor.sections)
            {
                const auto& object = world.GetMeshObjects().at(section.objectName);
                for (float x : {object.localBoundsMin.x(), object.localBoundsMax.x()})
                    for (float y : {object.localBoundsMin.y(), object.localBoundsMax.y()})
                        for (float z : {object.localBoundsMin.z(), object.localBoundsMax.z()})
                        {
                            const Eigen::Vector3f point =
                                (object.model * Eigen::Vector4f(x, y, z, 1)).head<3>();
                            lower = lower.cwiseMin(point);
                            upper = upper.cwiseMax(point);
                        }
            }
            target = (lower + upper) * 0.5f;
            radius = std::max(1.0f, (upper - lower).norm() * 0.5f);
        }
        const auto& view = world.GetRenderCamera();
        const Eigen::Matrix4f projection = view->GetProjectionMatrix();
        const float aspect = std::abs(projection(1, 1) / projection(0, 0));
        const float halfFov = view->GetHFOV() * DegreesToRadians * 0.5f;
        const float halfVerticalFov = std::atan(std::tan(halfFov) / aspect);
        const float distance = std::max(radius + view->GetClipNear() * 2.0f,
            1.2f * radius / std::sin(std::min(halfFov, halfVerticalFov)));
        // 侧移让被取景的 Camera 有可见视锥；保留观察者 up，避免垂直视角与世界 Y 共线。
        const Eigen::Vector3f offset =
            (-view->GetForwardVector() + view->GetRightVector() * 0.3f).normalized();
        view->SetCamera(target + offset * distance, target, view->GetUpVector());
        view->SetProjection(view->GetHFOV(), aspect, view->GetClipNear(),
            std::max(view->GetClipFar(), distance + radius * 2.0f));
        return true;
    }
    return false;
}
} // namespace VL
