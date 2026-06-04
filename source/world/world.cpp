#include "world/world.h"

#include <utility>

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

void World::AddMeshObject(std::string name, WorldMeshObject object)
{
    meshObjects[std::move(name)] = std::move(object);
}

void World::AddDirectionalLight(std::string name, std::shared_ptr<DirectionalLight> light)
{
    directionalLights[std::move(name)] = std::move(light);
}

void World::AddPointLight(std::string name, std::shared_ptr<PointLight> light)
{
    pointLights[std::move(name)] = std::move(light);
}

void World::AddSpotLight(std::string name, std::shared_ptr<SpotLight> light)
{
    spotLights[std::move(name)] = std::move(light);
}

} // namespace VL
