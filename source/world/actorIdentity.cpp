#include "world/actorIdentity.h"

#include <cstddef>

namespace VL
{
namespace
{

// 类型表只供身份解析、过滤器与图标使用；大纲不再按类型自动生成文件夹。
const std::vector<EditorSceneActorGroupSpec> GroupSpecs = {
    {EditorSceneActorType::DirectionalLight, "directionalLight", "Directional Lights",
        "icons/light-directional.png"},
    {EditorSceneActorType::PointLight, "pointLight", "Point Lights", "icons/light-point.png"},
    {EditorSceneActorType::SpotLight, "spotLight", "Spot Lights", "icons/light-spot.png"},
    {EditorSceneActorType::SquareAreaLight, "squareAreaLight", "Square Area Lights",
        "icons/light-area.png"},
    {EditorSceneActorType::Camera, "camera", "Cameras", "icons/camera.png"},
    {EditorSceneActorType::Environment, "environment", "Environments", "icons/environment.png"},
    {EditorSceneActorType::SceneWind, "wind", "Wind", "icons/wind.png"},
    {EditorSceneActorType::Terrain, "terrain", "Terrains", "icons/terrain.png"},
    {EditorSceneActorType::Mesh, "mesh", "Meshes", "icons/mesh.png"},
    {EditorSceneActorType::Script, "blueprintInstance", "Blueprint Instances", "icons/file-box.png"},
};

} // namespace

const std::vector<EditorSceneActorGroupSpec>& SceneActorGroupSpecs()
{
    return GroupSpecs;
}

const EditorSceneActorGroupSpec* FindSceneActorGroupSpec(EditorSceneActorType type)
{
    for (const EditorSceneActorGroupSpec& spec : GroupSpecs)
        if (spec.type == type) return &spec;
    return nullptr;
}

const EditorSceneActorGroupSpec* FindSceneActorGroupSpecByKey(const std::string& key)
{
    for (const EditorSceneActorGroupSpec& spec : GroupSpecs)
        if (key == spec.key) return &spec;
    return nullptr;
}

const char* SceneActorGroupTitle(EditorSceneActorType type)
{
    const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(type);
    return spec == nullptr ? "" : spec->title;
}

const char* SceneActorGroupIcon(EditorSceneActorType type)
{
    const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(type);
    return spec == nullptr ? "" : spec->icon;
}

std::string SceneActorIdentity(EditorSceneActorType type, const std::string& name)
{
    const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(type);
    return std::string(spec == nullptr ? "actor" : spec->key) + "|" + name;
}

bool ParseSceneActorIdentity(const std::string& identity, EditorSceneActorIdentity& parsed)
{
    const size_t separator = identity.find('|');
    if (separator == std::string::npos) return false;
    const EditorSceneActorGroupSpec* spec =
        FindSceneActorGroupSpecByKey(identity.substr(0, separator));
    if (spec == nullptr) return false;
    parsed.type = spec->type;
    parsed.name = identity.substr(separator + 1);
    // 与 SceneActorIdentity() 对称：空名字不可能由 World 构建出来，解析端不接受它，
    // 否则会拿空名字去查表并报一条误导性的"对象不存在"。
    return !parsed.name.empty();
}

bool SceneActorHasVisibilityToggle(EditorSceneActorType type)
{
    switch (type)
    {
    case EditorSceneActorType::Mesh:
    case EditorSceneActorType::Terrain:
    case EditorSceneActorType::DirectionalLight:
    case EditorSceneActorType::PointLight:
    case EditorSceneActorType::SpotLight:
    case EditorSceneActorType::SquareAreaLight:
        return true;
    // 相机 / 环境 / 风剖面 / 场景风没有渲染侧可见性；蓝图实例的可见性属于脚本状态
    //（destroyed），也没有逐 actor 的渲染开关命令。
    case EditorSceneActorType::Camera:
    case EditorSceneActorType::Environment:
    case EditorSceneActorType::SceneWind:
    case EditorSceneActorType::Script:
        return false;
    }
    return false;
}

} // namespace VL
