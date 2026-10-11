#include "world/world.h"

#include <algorithm>
#include <utility>

#include "sceneNode.h"
#include "world/meshSceneObjectIdentity.h"

namespace VL
{

World::World(uint64_t generation, std::string scenePath)
    : generation(generation)
    , scenePath(std::move(scenePath))
{
}

void World::SetViewTarget(std::shared_ptr<SceneNode> viewTarget)
{
    this->viewTarget = std::move(viewTarget);
}

void World::SetCamera(std::shared_ptr<Camera> camera)
{
    this->camera = std::move(camera);
}

void World::SetEnvironment(WorldEnvironment environment)
{
    this->environment = std::move(environment);
}

void World::SetCsmSettings(CsmSettings settings)
{
    csmSettings = std::move(settings);
}

void World::AddMeshObject(std::string name, WorldMeshObject object)
{
    meshObjects[std::move(name)] = std::move(object);
}

void World::AddSpeedTreeWindProfile(SpeedTreeWindProfile profile)
{
    speedTreeWindProfiles[profile.key] = std::move(profile);
}

void World::AddDirectionalLight(std::string name, std::shared_ptr<DirectionalLight> light)
{
    if (directionalLights.empty())
    {
        primaryDirectionalLightName = name;
    }
    directionalLights[std::move(name)] = std::move(light);
}

const std::shared_ptr<DirectionalLight>& World::GetPrimaryDirectionalLight() const
{
    static const std::shared_ptr<DirectionalLight> empty;
    const auto lightIt = directionalLights.find(primaryDirectionalLightName);
    return lightIt != directionalLights.end() ? lightIt->second : empty;
}

void World::AddPointLight(std::string name, std::shared_ptr<PointLight> light)
{
    pointLights[std::move(name)] = std::move(light);
}

void World::AddSpotLight(std::string name, std::shared_ptr<SpotLight> light)
{
    spotLights[std::move(name)] = std::move(light);
}

void World::AddSquareAreaLight(std::string name, WorldSquareAreaLight light)
{
    squareAreaLights[std::move(name)] = std::move(light);
}

template <typename ApplyFn>
bool World::ForEachMeshObjectInSceneObject(const std::string& sceneObjectIdentity, ApplyFn&& apply)
{
    bool matched = false;
    for (auto& entry : meshObjects)
    {
        if (MeshSceneObjectIdentity(entry.second, entry.first) != sceneObjectIdentity) continue;
        apply(entry.second);
        matched = true;
    }
    return matched;
}

bool World::SetMeshSceneObjectVisible(const std::string& sceneObjectIdentity, bool visible)
{
    // 一次写全该场景对象的所有 section：大纲的一行代表整个 actor，
    // 只改命中 identity 的那一个 section 会留下"行说隐藏、画面还有一半"的不一致。
    return ForEachMeshObjectInSceneObject(
        sceneObjectIdentity, [visible](WorldMeshObject& object) { object.visible = visible; });
}

bool World::IsMeshSceneObjectVisible(const std::string& sceneObjectIdentity) const
{
    // 与 actor 行一致：部分 section 可见时整行按可见显示。
    return std::any_of(meshObjects.begin(), meshObjects.end(),
        [&sceneObjectIdentity](const auto& entry)
        {
            return MeshSceneObjectIdentity(entry.second, entry.first) == sceneObjectIdentity &&
                entry.second.visible;
        });
}

bool World::SetLightVisible(const std::string& name, bool visible)
{
    // 三类灯光各有独立表；同名对象只会出现在其中一张表里，因此顺序查找即可。
    const auto directionalIt = directionalLights.find(name);
    if (directionalIt != directionalLights.end())
    {
        directionalIt->second->SetVisible(visible);
        return true;
    }
    const auto pointIt = pointLights.find(name);
    if (pointIt != pointLights.end())
    {
        pointIt->second->SetVisible(visible);
        return true;
    }
    const auto spotIt = spotLights.find(name);
    if (spotIt != spotLights.end())
    {
        spotIt->second->SetVisible(visible);
        return true;
    }
    return false;
}

bool World::SetSquareAreaLightVisible(const std::string& name, bool visible)
{
    const auto lightIt = squareAreaLights.find(name);
    if (lightIt == squareAreaLights.end()) return false;
    lightIt->second.visible = visible;
    return true;
}

} // namespace VL
