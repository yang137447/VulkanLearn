#include "world/loading/worldBuilder.h"

#include <exception>

#include "commonFunction.h"
#include "render/resource/rendererResourceCache.h"
#include "sceneNode.h"
#include "world/loading/worldLoader.h"

namespace VL
{

namespace
{

SkyParametersGPU BuildSkyParameters(const nlohmann::json& environmentNode)
{
    SkyParametersGPU skyParameters;
    if (!environmentNode.contains("skyParameters"))
    {
        return skyParameters;
    }

    const nlohmann::json& node = environmentNode["skyParameters"];
    skyParameters.sunDirectionIntensity.w() = JsonParser::ParseValue<float>(node["sunIntensity"]);
    skyParameters.sunColorAngularRadius.head<3>() = JsonParser::ParseValue<Eigen::Vector3f>(node["sunColor"]);
    skyParameters.sunColorAngularRadius.w() = JsonParser::ParseValue<float>(node["sunAngularRadius"]);
    skyParameters.zenithColor.head<3>() = JsonParser::ParseValue<Eigen::Vector3f>(node["zenithColor"]);
    skyParameters.horizonColor.head<3>() = JsonParser::ParseValue<Eigen::Vector3f>(node["horizonColor"]);
    skyParameters.groundColor.head<3>() = JsonParser::ParseValue<Eigen::Vector3f>(node["groundColor"]);
    skyParameters.scatteringControls.x() = JsonParser::ParseValue<float>(node["skyGradientExponent"]);
    skyParameters.scatteringControls.y() = JsonParser::ParseValue<float>(node["groundGradientExponent"]);
    skyParameters.scatteringControls.z() = JsonParser::ParseValue<float>(node["sunHaloExponent"]);
    skyParameters.scatteringControls.w() = JsonParser::ParseValue<float>(node["sunHaloStrength"]);

    return skyParameters;
}

std::shared_ptr<Camera> BuildCamera(const nlohmann::json& node)
{
    const std::string name = node["name"];
    const float fov = node["fov"].get<float>();
    const float nearClip = node["near_clip"].get<float>();
    const float farClip = node["far_clip"].get<float>();
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);

    std::shared_ptr<Camera> camera = std::make_shared<Camera>();
    camera->SetName(name);
    camera->SetHFOV(fov);
    camera->SetClip(nearClip, farClip);
    if (node.contains("look_at"))
    {
        const Eigen::Vector3f lookAt =
            JsonParser::ParseValue<Eigen::Vector3f>(node["look_at"]);
        if ((lookAt - position).squaredNorm() <= 1.0e-8f)
        {
            throw std::runtime_error(
                "Camera look_at must differ from position: " + name);
        }
        camera->SetInitialLookAt(position, lookAt);
    }
    else
    {
        camera->SetPosition(position);
        camera->SetRotation(rotation);
    }
    return camera;
}

std::shared_ptr<DirectionalLight> BuildDirectionalLight(const nlohmann::json& node)
{
    const std::string name = node["name"];
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    const float intensity = JsonParser::ParseValue<float>(node["intensity"]);

    std::shared_ptr<DirectionalLight> light = std::make_shared<DirectionalLight>();
    light->SetName(name);
    light->SetColor(color);
    light->SetIntensity(intensity);
    light->SetPosition(position);
    light->SetRotation(rotation);
    return light;
}

std::shared_ptr<PointLight> BuildPointLight(const nlohmann::json& node)
{
    const std::string name = node["name"];
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    const float intensity = JsonParser::ParseValue<float>(node["intensity"]);
    const float radius = node.value("radius", 0.0f);

    std::shared_ptr<PointLight> light = std::make_shared<PointLight>();
    light->SetName(name);
    light->SetColor(color);
    light->SetIntensity(intensity);
    light->SetRadius(radius);
    light->SetPosition(position);
    light->SetRotation(rotation);
    return light;
}

std::shared_ptr<SpotLight> BuildSpotLight(const nlohmann::json& node)
{
    const std::string name = node["name"];
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    const float intensity = JsonParser::ParseValue<float>(node["intensity"]);
    const float radius = node.value("radius", 0.0f);
    const float coneAngleOuter = JsonParser::ParseValue<float>(node["cone_angle_outer"]);
    const float coneAngleInner = JsonParser::ParseValue<float>(node["cone_angle_inner"]);

    std::shared_ptr<SpotLight> light = std::make_shared<SpotLight>();
    light->SetName(name);
    light->SetColor(color);
    light->SetIntensity(intensity);
    light->SetRadius(radius);
    light->SetPosition(position);
    light->SetRotation(rotation);
    light->SetConeAngleOuter(coneAngleOuter);
    light->SetConeAngleInner(coneAngleInner);
    return light;
}

WorldEnvironment BuildEnvironment(const nlohmann::json& node)
{
    const nlohmann::json& config = node["environment"];

    WorldEnvironment environment;
    environment.type = ParseEnvironmentType(config["type"].get<std::string>());
    environment.cubeSize = config.value("cubeSize", 128u);
    environment.intensity = config.value("intensity", 1.0f);
    environment.hdrPath = config.value("hdrPath", std::string());
    environment.skyParameters = BuildSkyParameters(config);

    return environment;
}

void SetSkySunDirection(WorldEnvironment& environment, const Eigen::Vector3f& sceneToSunDirection)
{
    const Eigen::Vector3f normalizedDirection = sceneToSunDirection.normalized();
    environment.skyParameters.sunDirectionIntensity.x() = normalizedDirection.x();
    environment.skyParameters.sunDirectionIntensity.y() = normalizedDirection.y();
    environment.skyParameters.sunDirectionIntensity.z() = normalizedDirection.z();
}

void SyncEnvironmentWithPrimaryDirectionalLight(
    WorldEnvironment& environment,
    const DirectionalLight& light)
{
    // Convert incoming light-ray direction back to the scene-to-sun direction
    // used by sky evaluation.
    SetSkySunDirection(environment, -light.GetForwardVector());
}

void AddDirectionalLightIfFirst(World& world, std::shared_ptr<DirectionalLight> light)
{
    const std::string name = light->GetName();
    if (world.GetDirectionalLights().find(name) == world.GetDirectionalLights().end())
    {
        world.AddDirectionalLight(name, std::move(light));
    }
}

void AddPointLightIfFirst(World& world, std::shared_ptr<PointLight> light)
{
    const std::string name = light->GetName();
    if (world.GetPointLights().find(name) == world.GetPointLights().end())
    {
        world.AddPointLight(name, std::move(light));
    }
}

void AddSpotLightIfFirst(World& world, std::shared_ptr<SpotLight> light)
{
    const std::string name = light->GetName();
    if (world.GetSpotLights().find(name) == world.GetSpotLights().end())
    {
        world.AddSpotLight(name, std::move(light));
    }
}

} // namespace

RuntimeResult<std::shared_ptr<World>> WorldBuilder::BuildFromLoadedScene(
    uint64_t generation,
    const WorldBuildPlan& worldBuildPlan,
    const RendererResourceCache& resourceCache,
    uint32_t shadowCascadeCount) const
{
    try
    {
        std::shared_ptr<World> world = std::make_shared<World>(
            generation,
            worldBuildPlan.scenePath);

        WorldEnvironment environment;
        std::shared_ptr<DirectionalLight> primaryDirectionalLight;
        CsmSettings csmSettings;

        const nlohmann::json& objectsJson = worldBuildPlan.sceneJson["objects"];
        for (const SceneAssetObjectPlan& objectPlan : worldBuildPlan.sceneAssetPlan.objectPlans)
        {
            const nlohmann::json& objectJson = objectsJson[objectPlan.objectIndex];
            const std::string& type = objectPlan.objectType;

            if (type == "camera")
            {
                std::shared_ptr<Camera> camera = BuildCamera(objectJson);
                world->SetViewTarget(camera);
                world->SetCamera(std::move(camera));
            }
            else if (type == "directionalLight")
            {
                std::shared_ptr<DirectionalLight> light = BuildDirectionalLight(objectJson);
                if (!primaryDirectionalLight)
                {
                    primaryDirectionalLight = light;
                    auto csmResult =
                        BuildDirectionalLightCsmSettings(
                            objectJson,
                            shadowCascadeCount,
                            worldBuildPlan.scenePath);
                    if (csmResult.IsFailure())
                    {
                        return RuntimeResult<std::shared_ptr<World>>::Failure(
                            csmResult.Error());
                    }
                    csmSettings =
                        std::move(csmResult.Value());
                }
                AddDirectionalLightIfFirst(*world, std::move(light));
            }
            else if (type == "pointLight")
            {
                AddPointLightIfFirst(*world, BuildPointLight(objectJson));
            }
            else if (type == "spotLight")
            {
                AddSpotLightIfFirst(*world, BuildSpotLight(objectJson));
            }
            else if (type == "environment")
            {
                environment = BuildEnvironment(objectJson);
            }
        }

        if (!world->GetCamera())
        {
            return RuntimeResult<std::shared_ptr<World>>::Failure(MakeRuntimeError(
                "WorldBuilder.NoCamera",
                "Cannot build a World because the build plan has no camera.",
                worldBuildPlan.scenePath));
        }

        SyncEnvironmentWithPrimaryDirectionalLight(environment, *primaryDirectionalLight);

        world->SetEnvironment(environment);
        world->SetCsmSettings(std::move(csmSettings));

        for (const SpeedTreeWindProfile& profile : worldBuildPlan.speedTreeWindProfiles)
        {
            world->AddSpeedTreeWindProfile(profile);
        }

        for (const MeshObjectBuildPlan& meshObjectPlan : worldBuildPlan.meshObjectPlans)
        {
            WorldMeshObject object;
            object.debugName = meshObjectPlan.debugName;
            object.sceneObjectIdentity = meshObjectPlan.sceneObjectIdentity;
            object.materialSlotIndex = meshObjectPlan.materialSlotIndex;
            object.materialSlotName = meshObjectPlan.materialSlotName;
            object.model = meshObjectPlan.model;
            object.localBoundsMin = meshObjectPlan.localBoundsMin;
            object.localBoundsMax = meshObjectPlan.localBoundsMax;
            object.meshKey = meshObjectPlan.meshKey;
            object.materialKey = meshObjectPlan.materialKey;
            object.materialInstanceKey = meshObjectPlan.materialInstanceKey;
            object.speedTreeWindProfileKey = meshObjectPlan.speedTreeWindProfileKey;
            world->AddMeshObject(meshObjectPlan.objectName, std::move(object));
        }

        return RuntimeResult<std::shared_ptr<World>>::Success(std::move(world));
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<std::shared_ptr<World>>::Failure(MakeRuntimeError(
            "WorldBuilder.BuildFailed",
            exception.what(),
            worldBuildPlan.scenePath));
    }
}

} // namespace VL
