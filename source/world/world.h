#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <Eigen/Dense>

#include "baseStructs.h"
#include "environmentType.h"
#include "mesh/meshAssetTypes.h"

class Camera;
class DirectionalLight;
class PointLight;
class SceneNode;
class SpotLight;

namespace VL
{

struct WorldEnvironment
{
    EnvironmentType type = EnvironmentType::ProceduralSky;
    uint32_t cubeSize = 128;
    float intensity = 1.0f;
    std::string hdrPath;
    SkyParametersGPU skyParameters;
};

struct WorldMeshObject
{
    std::string debugName;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    Eigen::Vector3f localBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f localBoundsMax = Eigen::Vector3f::Zero();
    std::string meshKey;
    std::string materialKey;
    std::string materialInstanceKey;
    std::string speedTreeWindProfileKey;
};

// Runtime view of the active game world. Camera, light, environment, and mesh
// object state is owned by World. Object GPU resources and descriptors live in
// renderer-owned backend entries and are referenced from snapshots by key.
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

    void AddMeshObject(std::string name, WorldMeshObject object);
    void AddSpeedTreeWindProfile(SpeedTreeWindProfile profile);
    void AddDirectionalLight(std::string name, std::shared_ptr<DirectionalLight> light);
    void AddPointLight(std::string name, std::shared_ptr<PointLight> light);
    void AddSpotLight(std::string name, std::shared_ptr<SpotLight> light);

    const std::unordered_map<std::string, WorldMeshObject>& GetMeshObjects() const { return meshObjects; }
    const std::unordered_map<std::string, SpeedTreeWindProfile>& GetSpeedTreeWindProfiles() const { return speedTreeWindProfiles; }
    const std::unordered_map<std::string, std::shared_ptr<DirectionalLight>>& GetDirectionalLights() const { return directionalLights; }
    const std::unordered_map<std::string, std::shared_ptr<PointLight>>& GetPointLights() const { return pointLights; }
    const std::unordered_map<std::string, std::shared_ptr<SpotLight>>& GetSpotLights() const { return spotLights; }

private:
    uint64_t generation = 0;
    std::string scenePath;
    std::weak_ptr<SceneNode> viewTarget;
    std::shared_ptr<Camera> camera;
    WorldEnvironment environment;
    std::unordered_map<std::string, WorldMeshObject> meshObjects;
    std::unordered_map<std::string, SpeedTreeWindProfile> speedTreeWindProfiles;
    std::unordered_map<std::string, std::shared_ptr<DirectionalLight>> directionalLights;
    std::unordered_map<std::string, std::shared_ptr<PointLight>> pointLights;
    std::unordered_map<std::string, std::shared_ptr<SpotLight>> spotLights;
};

} // namespace VL
