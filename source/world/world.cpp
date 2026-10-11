#include "world/world.h"

#include <utility>

#include "sceneNode.h"

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

bool World::SetMeshObjectVisible(const std::string& name, bool visible)
{
    const auto objectIt = meshObjects.find(name);
    if (objectIt == meshObjects.end()) return false;
    objectIt->second.visible = visible;
    return true;
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
