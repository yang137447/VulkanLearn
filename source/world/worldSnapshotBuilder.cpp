#include "world/worldSnapshotBuilder.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <unordered_map>

#include "commonFunction.h"
#include "sceneNode.h"

namespace VL
{
namespace
{

RuntimeId StableRuntimeId(const std::string& key)
{
    constexpr RuntimeId fnvOffset = 14695981039346656037ull;
    constexpr RuntimeId fnvPrime = 1099511628211ull;

    RuntimeId hash = fnvOffset;
    for (unsigned char value : key)
    {
        hash ^= value;
        hash *= fnvPrime;
    }
    return hash;
}

ResourceHandle MakeResourceHandle(const std::string& key, uint64_t generation = 0)
{
    ResourceHandle handle;
    handle.key = key;
    handle.generation = generation;
    return handle;
}

std::array<Eigen::Vector3f, 8> BuildWorldCorners(
    const Eigen::Vector3f& localMin,
    const Eigen::Vector3f& localMax,
    const Eigen::Matrix4f& model)
{
    std::array<Eigen::Vector3f, 8> corners;
    int index = 0;

    for (float x : {localMin.x(), localMax.x()})
    {
        for (float y : {localMin.y(), localMax.y()})
        {
            for (float z : {localMin.z(), localMax.z()})
            {
                Eigen::Vector4f localCorner(x, y, z, 1.0f);
                corners[index++] = (model * localCorner).head<3>();
            }
        }
    }

    return corners;
}

void ComputeWorldBounds(
    const std::array<Eigen::Vector3f, 8>& corners,
    Eigen::Vector3f& outMin,
    Eigen::Vector3f& outMax)
{
    outMin = Eigen::Vector3f(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    outMax = Eigen::Vector3f(
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());

    for (const Eigen::Vector3f& corner : corners)
    {
        outMin = outMin.cwiseMin(corner);
        outMax = outMax.cwiseMax(corner);
    }
}

std::string BuildObjectRuntimeKey(const World& world, const std::string& objectName)
{
    return world.GetScenePath() + "|object|" + objectName;
}

std::string BuildLightRuntimeKey(const World& world, const std::string& lightName)
{
    return world.GetScenePath() + "|light|" + lightName;
}

MeshDrawSnapshot BuildMeshDrawSnapshot(
    const std::string& objectName,
    RuntimeId objectId,
    const WorldMeshObject& meshObject,
    const Eigen::Matrix4f& previousModel)
{
    MeshDrawSnapshot snapshot;
    snapshot.objectId = objectId;
    snapshot.debugName = meshObject.debugName.empty() ? objectName : meshObject.debugName;
    snapshot.model = meshObject.model;
    snapshot.previousModel = previousModel;
    snapshot.mesh = MakeResourceHandle(meshObject.meshKey);
    snapshot.material = MakeResourceHandle(meshObject.materialKey);
    snapshot.materialInstance = MakeResourceHandle(meshObject.materialInstanceKey);

    const std::array<Eigen::Vector3f, 8> worldCorners = BuildWorldCorners(
        meshObject.localBoundsMin,
        meshObject.localBoundsMax,
        snapshot.model);
    ComputeWorldBounds(worldCorners, snapshot.worldBoundsMin, snapshot.worldBoundsMax);

    return snapshot;
}

LightSnapshot BuildDirectionalLightSnapshot(
    const World& world,
    const std::string& lightName,
    const DirectionalLight& light)
{
    LightSnapshot snapshot;
    snapshot.lightId = StableRuntimeId(BuildLightRuntimeKey(world, lightName));
    snapshot.type = LightSnapshotType::Directional;
    snapshot.color = light.GetColor();
    snapshot.intensity = light.GetIntensity();
    snapshot.position = light.GetPosition();
    snapshot.direction = light.GetForwardVector();
    snapshot.worldToLight = CommonFunction::RotationToMatrix(light.GetRotation()).block<3, 3>(0, 0).transpose();
    return snapshot;
}

LightSnapshot BuildPointLightSnapshot(
    const World& world,
    const std::string& lightName,
    const PointLight& light)
{
    LightSnapshot snapshot;
    snapshot.lightId = StableRuntimeId(BuildLightRuntimeKey(world, lightName));
    snapshot.type = LightSnapshotType::Point;
    snapshot.color = light.GetColor();
    snapshot.intensity = light.GetIntensity();
    snapshot.position = light.GetPosition();
    snapshot.radius = light.GetRadius();
    return snapshot;
}

LightSnapshot BuildSpotLightSnapshot(
    const World& world,
    const std::string& lightName,
    const SpotLight& light)
{
    LightSnapshot snapshot;
    snapshot.lightId = StableRuntimeId(BuildLightRuntimeKey(world, lightName));
    snapshot.type = LightSnapshotType::Spot;
    snapshot.color = light.GetColor();
    snapshot.intensity = light.GetIntensity();
    snapshot.position = light.GetPosition();
    snapshot.radius = light.GetRadius();
    snapshot.direction = light.GetForwardVector();
    snapshot.worldToLight = CommonFunction::RotationToMatrix(light.GetRotation()).block<3, 3>(0, 0).transpose();
    snapshot.coneAngleOuter = light.GetConeAngleOuter();
    snapshot.coneAngleInner = light.GetConeAngleInner();
    return snapshot;
}

EnvironmentSnapshot BuildEnvironmentSnapshot(
    const World& world,
    const WorldSnapshotBuildDesc& desc)
{
    EnvironmentSnapshot snapshot;
    const WorldEnvironment& environment = world.GetEnvironment();
    const std::string& hdrPath = environment.hdrPath;

    if (!hdrPath.empty())
    {
        const std::string cubeSize = std::to_string(environment.cubeSize);
        snapshot.cube = MakeResourceHandle("environment/cube|" + hdrPath + "|" + cubeSize);
        snapshot.prefilteredCube = MakeResourceHandle("environment/prefilter|" + hdrPath + "|" + cubeSize);
    }

    if (environment.hasBrdfLut)
    {
        snapshot.brdfLut = MakeResourceHandle("global/brdfLut");
    }

    snapshot.skyParameters = environment.skyParameters;
    snapshot.sphericalHarmonics = environment.sphericalHarmonics;
    snapshot.hasSphericalHarmonics = environment.hasSphericalHarmonics;
    snapshot.intensity = desc.environmentIntensity;
    return snapshot;
}

} // namespace

RuntimeResult<WorldSnapshot> WorldSnapshotBuilder::Build(
    const World& world,
    const WorldSnapshotBuildDesc& desc)
{
    if (desc.worldGeneration != 0 && desc.worldGeneration != world.GetGeneration())
    {
        return RuntimeResult<WorldSnapshot>::Failure(MakeRuntimeError(
            "WorldSnapshot.GenerationMismatch",
            "Cannot build a world snapshot because the requested generation does not match the active World.",
            world.GetScenePath()));
    }

    if (!world.GetCamera())
    {
        return RuntimeResult<WorldSnapshot>::Failure(MakeRuntimeError(
            "WorldSnapshot.NoCamera",
            "Cannot build a world snapshot because the active scene has no camera.",
            world.GetScenePath()));
    }

    if (!previousWorldGeneration.has_value() || *previousWorldGeneration != world.GetGeneration())
    {
        previousViewProjection.reset();
        previousObjectModels.clear();
        previousWorldGeneration = world.GetGeneration();
    }

    WorldSnapshot snapshot;
    snapshot.worldGeneration = world.GetGeneration();
    snapshot.frameIndex = desc.frameIndex;
    snapshot.debugViewMode = desc.debugViewMode;

    Camera& camera = *world.GetCamera();
    snapshot.camera.view = camera.GetViewMatrix();
    snapshot.camera.projection = camera.GetProjectionMatrix();
    snapshot.camera.viewProjection = snapshot.camera.projection * snapshot.camera.view;
    snapshot.camera.previousViewProjection =
        previousViewProjection.value_or(snapshot.camera.viewProjection);
    snapshot.camera.position = camera.GetPosition();
    snapshot.camera.forward = camera.GetForwardVector();
    snapshot.camera.right = camera.GetRightVector();
    snapshot.camera.up = camera.GetUpVector();
    snapshot.camera.horizontalFovDegrees = camera.GetHFOV();
    snapshot.camera.clipNear = camera.GetClipNear();
    snapshot.camera.clipFar = camera.GetClipFar();

    snapshot.meshDraws.reserve(world.GetMeshObjects().size());
    std::unordered_map<RuntimeId, Eigen::Matrix4f> currentObjectModels;
    currentObjectModels.reserve(world.GetMeshObjects().size());
    for (const auto& [objectName, meshObject] : world.GetMeshObjects())
    {
        const RuntimeId objectId = StableRuntimeId(BuildObjectRuntimeKey(world, objectName));

        const auto previousModelIt = previousObjectModels.find(objectId);
        const Eigen::Matrix4f previousModel =
            previousModelIt != previousObjectModels.end()
                ? previousModelIt->second
                : meshObject.model;

        snapshot.meshDraws.push_back(BuildMeshDrawSnapshot(
            objectName,
            objectId,
            meshObject,
            previousModel));
        currentObjectModels[objectId] = meshObject.model;
    }

    const size_t lightCount =
        world.GetDirectionalLights().size() +
        world.GetPointLights().size() +
        world.GetSpotLights().size();
    snapshot.lights.reserve(lightCount);

    for (const auto& [lightName, light] : world.GetDirectionalLights())
    {
        snapshot.lights.push_back(BuildDirectionalLightSnapshot(world, lightName, *light));
    }
    for (const auto& [lightName, light] : world.GetPointLights())
    {
        snapshot.lights.push_back(BuildPointLightSnapshot(world, lightName, *light));
    }
    for (const auto& [lightName, light] : world.GetSpotLights())
    {
        snapshot.lights.push_back(BuildSpotLightSnapshot(world, lightName, *light));
    }

    snapshot.environment = BuildEnvironmentSnapshot(world, desc);
    previousViewProjection = snapshot.camera.viewProjection;
    previousObjectModels = std::move(currentObjectModels);

    return RuntimeResult<WorldSnapshot>::Success(std::move(snapshot));
}

void WorldSnapshotBuilder::Reset()
{
    previousWorldGeneration.reset();
    previousViewProjection.reset();
    previousObjectModels.clear();
}

} // namespace VL
