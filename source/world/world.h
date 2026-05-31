#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <Eigen/Dense>

class Camera;
class DirectionalLight;
class PointLight;
class SceneNode;
class SceneObject;
class SpotLight;

namespace VL
{

struct WorldEnvironment
{
    std::string hdrPath;
    uint32_t cubeSize = 512;
    std::array<Eigen::Vector4f, 9> sphericalHarmonics{};
    bool hasSphericalHarmonics = false;
    bool hasBrdfLut = false;
};

// Runtime view of the active game world. During the transition phase mesh
// SceneObject wrappers still come from RendererMeshLoader because mesh scene
// objects have not moved to a pure world actor/component model yet. Object GPU
// resources and descriptors live in renderer-owned backend entries.
// Camera/light/environment state is built from the WorldBuildPlan and exposed
// through World.
class World
{
public:
    World(uint64_t generation, std::string scenePath);

    uint64_t GetGeneration() const { return generation; }
    const std::string& GetScenePath() const { return scenePath; }

    void SetViewTarget(std::shared_ptr<SceneNode> viewTarget);
    std::weak_ptr<SceneNode> GetViewTarget() const { return viewTarget; }

    void SetCamera(std::shared_ptr<Camera> camera);
    const std::shared_ptr<Camera>& GetCamera() const { return camera; }

    void SetEnvironment(WorldEnvironment environment);
    const WorldEnvironment& GetEnvironment() const { return environment; }

    void AddSceneObject(std::string name, std::shared_ptr<SceneObject> object);
    void AddDirectionalLight(std::string name, std::shared_ptr<DirectionalLight> light);
    void AddPointLight(std::string name, std::shared_ptr<PointLight> light);
    void AddSpotLight(std::string name, std::shared_ptr<SpotLight> light);

    const std::unordered_map<std::string, std::shared_ptr<SceneObject>>& GetSceneObjects() const { return sceneObjects; }
    const std::unordered_map<std::string, std::shared_ptr<DirectionalLight>>& GetDirectionalLights() const { return directionalLights; }
    const std::unordered_map<std::string, std::shared_ptr<PointLight>>& GetPointLights() const { return pointLights; }
    const std::unordered_map<std::string, std::shared_ptr<SpotLight>>& GetSpotLights() const { return spotLights; }

private:
    uint64_t generation = 0;
    std::string scenePath;
    std::weak_ptr<SceneNode> viewTarget;
    std::shared_ptr<Camera> camera;
    WorldEnvironment environment;
    std::unordered_map<std::string, std::shared_ptr<SceneObject>> sceneObjects;
    std::unordered_map<std::string, std::shared_ptr<DirectionalLight>> directionalLights;
    std::unordered_map<std::string, std::shared_ptr<PointLight>> pointLights;
    std::unordered_map<std::string, std::shared_ptr<SpotLight>> spotLights;
};

} // namespace VL
