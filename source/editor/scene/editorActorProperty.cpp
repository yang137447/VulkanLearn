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

bool Finite(const std::array<float, 3>& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

Eigen::Vector3f Vector(const std::array<float, 3>& value)
{
    return {value[0], value[1], value[2]};
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
    if (edit.property == EditorActorProperty::Transform)
    {
        // Camera 的快照不发布 authored scale；整组 Transform 仍只写位置/朝向，
        // 避免一次平移把未展示的缩放覆盖成默认 1。
        node.SetPosition(Vector(edit.transformPosition));
        node.SetRotation(Vector(edit.transformRotation));
        return true;
    }
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
    const bool transform = edit.property == P::Transform;
    if (transform)
    {
        // 完整 Transform 必须整体有效：任一分量非法或 scale 过零时，
        // 不允许只写入一半再失败。这里只是入口校验，真正写入在类型分支中。
        if (!Finite(edit.transformPosition) || !Finite(edit.transformRotation) ||
            !Finite(edit.transformScale))
            return Reject(error, "A complete finite Transform is required.");
        if ((Vector(edit.transformScale).array().abs() < 1.0e-6f).any())
            return Reject(error, "Scale must be non-zero (negative scale is supported).");
    }
    else
    {
        if (!std::isfinite(edit.value))
            return Reject(error, "A complete finite number is required.");
        const bool vector = edit.property == P::Position || edit.property == P::Rotation ||
            edit.property == P::Scale || edit.property == P::Color || edit.property == P::Normal ||
            edit.property == P::WindDirection;
        if (edit.component >= (vector ? 3u : 1u))
            return Reject(error, "Unknown property component.");
    }
    EditorUi::EditorSceneActorIdentity identity;
    if (!EditorUi::ParseSceneActorIdentity(edit.identity, identity))
        return Reject(error, "Unknown actor identity.");

    if (identity.type == EditorSceneActorType::Script)
    {
        if (transform)
        {
            const Eigen::Vector3f scale = Vector(edit.transformScale);
            if ((scale.array().abs() < 1.0e-6f).any())
                return Reject(error, "Actor editing requires Transform with non-zero scale.");
            const nlohmann::json* found = nullptr;
            for (const auto& actor : world.GetScriptBlueprints())
                if (actor.at("id") == identity.name) { found = &actor; break; }
            if (found == nullptr)
                return Reject(error, "Actor is not in the active World or is read-only.");
            if (found->contains("physics") &&
                !(edit.transformScale[0] == edit.transformScale[1] &&
                  edit.transformScale[1] == edit.transformScale[2]))
                return Reject(error, "Rigid body scale must stay uniform; edit its authored vec3 together.");
            // 只写真正变化的组；一次平移不应触发 3 次无关的 rotation/scale 重建。
            bool positionChanged = false, rotationChanged = false, scaleChanged = false;
            for (uint32_t component = 0; component < 3; ++component)
            {
                positionChanged = positionChanged ||
                    found->at("position")[component].get<float>() != edit.transformPosition[component];
                rotationChanged = rotationChanged ||
                    found->at("rotation")[component].get<float>() != edit.transformRotation[component];
                scaleChanged = scaleChanged ||
                    found->at("scale")[component].get<float>() != edit.transformScale[component];
            }
            if (positionChanged)
                for (uint32_t component = 0; component < 3; ++component)
                    if (!world.SetScriptBlueprintTransform(identity.name, "position",
                        component, edit.transformPosition[component])) return false;
            if (rotationChanged)
                for (uint32_t component = 0; component < 3; ++component)
                    if (!world.SetScriptBlueprintTransform(identity.name, "rotation",
                        component, edit.transformRotation[component])) return false;
            if (scaleChanged)
                for (uint32_t component = 0; component < 3; ++component)
                    if (!world.SetScriptBlueprintTransform(identity.name, "scale",
                        component, edit.transformScale[component])) return false;
            return true;
        }
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
    // 地形在 World 中仍是共享 sceneObjectIdentity 的 chunk mesh；编辑器 Terrain
    // 身份只改变大纲行类型。World 内部按 isTerrain 选择写入入口，避免同名
    // Mesh/Terrain 场景对象互相串写。
    if (identity.type == EditorSceneActorType::Mesh ||
        identity.type == EditorSceneActorType::Terrain)
    {
        const bool terrain = identity.type == EditorSceneActorType::Terrain;
        if (transform)
        {
            const bool applied = terrain
                ? world.SetTerrainSceneObjectTransform(identity.name,
                    Vector(edit.transformPosition), Vector(edit.transformRotation),
                    Vector(edit.transformScale))
                : world.SetMeshSceneObjectTransform(identity.name,
                    Vector(edit.transformPosition), Vector(edit.transformRotation),
                    Vector(edit.transformScale));
            return applied ? true :
                Reject(error, "Actor is not in the active World or is read-only.");
        }
        if (edit.property != P::Position && edit.property != P::Rotation && edit.property != P::Scale)
            return Reject(error, "Mesh/Terrain editing only supports Transform.");
        for (const auto& entry : world.GetMeshObjects())
        {
            const WorldMeshObject& object = entry.second;
            if (object.isTerrain != terrain) continue;
            if (MeshSceneObjectIdentity(object, entry.first) != identity.name) continue;
            Eigen::Vector3f position = object.scenePosition;
            Eigen::Vector3f rotation = object.sceneRotation;
            Eigen::Vector3f scale = object.sceneScale;
            if (edit.property == P::Position) position[edit.component] = edit.value;
            else if (edit.property == P::Rotation) rotation[edit.component] = edit.value;
            else scale[edit.component] = edit.value;
            if ((scale.array().abs() < 1.0e-6f).any())
                return Reject(error, "Scale must be non-zero (negative scale is supported).");
            return terrain
                ? world.SetTerrainSceneObjectTransform(
                    identity.name, position, rotation, scale)
                : world.SetMeshSceneObjectTransform(
                    identity.name, position, rotation, scale);
        }
    }
    else if (identity.type == EditorSceneActorType::DirectionalLight)
    {
        if (auto* light = FindNode(world.GetDirectionalLights(), identity.name))
        {
            if (transform)
            {
                light->SetPosition(Vector(edit.transformPosition));
                light->SetRotation(Vector(edit.transformRotation));
                if (light == world.GetPrimaryDirectionalLight().get())
                {
                    WorldEnvironment environment = world.GetEnvironment();
                    environment.skyParameters.sunDirectionIntensity.head<3>() = -light->GetForwardVector();
                    world.SetEnvironment(std::move(environment));
                }
                return true;
            }
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
        {
            if (transform)
            {
                light->SetPosition(Vector(edit.transformPosition));
                light->SetRotation(Vector(edit.transformRotation));
                return true;
            }
            return SetLight(*light, edit, error);
        }
    }
    else if (identity.type == EditorSceneActorType::SpotLight)
    {
        if (auto* light = FindNode(world.GetSpotLights(), identity.name))
        {
            if (transform)
            {
                light->SetPosition(Vector(edit.transformPosition));
                light->SetRotation(Vector(edit.transformRotation));
                return true;
            }
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
            if (transform) light.position = Vector(edit.transformPosition);
            else if (edit.property == P::Position) light.position[edit.component] = edit.value;
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
            if (transform) return SetNodeTransform(*camera, edit);
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
    else if (identity.type == EditorSceneActorType::SceneWind)
    {
        WorldWind wind = world.GetWind();
        // 身份解析用的是作者 name；名字不一致说明命令指向了别的场景风（或旧代 World）。
        if (identity.name != wind.name)
            return Reject(error, "Wind identity does not match the active scene wind.");
        switch (edit.property)
        {
        case P::WindDirection:
            wind.direction[edit.component] = edit.value;
            if (wind.direction.squaredNorm() <= 1.0e-12f)
                return Reject(error, "Wind direction must be non-zero.");
            wind.direction.normalize();
            break;
        case P::WindStrength:
            if (edit.value < 0.0f)
                return Reject(error, "Wind strength must be non-negative.");
            wind.strength = edit.value;
            break;
        case P::WindGustAmplitude:
            if (edit.value < 0.0f || edit.value > 1.0f)
                return Reject(error, "Wind gust amplitude must be in [0,1].");
            if (edit.value > 0.0f)
            {
                // UI 只暴露一个 Gust 滑条；首次开启时补齐场景风层的固定频率/湍流，
                // 让 JSON 仍满足 ValidateSceneWindJson 的完整 gust 对象。
                if (!wind.gust.enabled)
                {
                    wind.gust.frequency = 0.5f;
                    wind.gust.turbulence = 0.2f;
                }
                wind.gust.enabled = true;
            }
            else
            {
                wind.gust.enabled = false;
            }
            wind.gust.amplitude = edit.value;
            break;
        case P::WindGustFrequency:
            if (edit.value < 0.0f)
                return Reject(error, "Wind gust frequency must be non-negative.");
            wind.gust.frequency = edit.value;
            wind.gust.enabled = true;
            break;
        case P::WindGustTurbulence:
            if (edit.value < 0.0f || edit.value > 1.0f)
                return Reject(error, "Wind gust turbulence must be in [0,1].");
            wind.gust.turbulence = edit.value;
            wind.gust.enabled = true;
            break;
        default:
            return Reject(error, "This property does not belong to Scene Wind.");
        }
        world.SetWind(std::move(wind));
        return true;
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
            actor.type == EditorUi::EditorSceneActorType::SceneWind) return false;
        Eigen::Vector3f target = actor.position;
        float radius = 150.0f;
        if (actor.type == EditorUi::EditorSceneActorType::Mesh ||
            actor.type == EditorUi::EditorSceneActorType::Terrain)
        {
            Eigen::Vector3f lower = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
            Eigen::Vector3f upper = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
            // 用所有 section 的变换后角点，而不是作者根位置：源模型可偏心，section 可相距很远；
            // 地形的 section 就是同一 placement 下的各个 chunk，取景必须包住整块地形。
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
