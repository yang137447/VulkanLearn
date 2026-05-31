#include "world/loading/worldBuilder.h"

#include <exception>

#include "commonFunction.h"
#include "pipeline/environmentSHGenerator.h"
#include "render/resource/rendererResourceCache.h"
#include "sceneObject.h"
#include "world/loading/worldLoader.h"

namespace VL
{

namespace
{

Eigen::Vector3f ReadVector3(const nlohmann::json& node, const char* fieldName)
{
    return JsonParser::ParseValue<Eigen::Vector3f>(node[fieldName]);
}

std::shared_ptr<Camera> BuildCamera(const nlohmann::json& node)
{
    const std::string name = node["name"];
    const float fov = node["fov"].get<float>();
    const float nearClip = node["near_clip"].get<float>();
    const float farClip = node["far_clip"].get<float>();
    Eigen::Vector3f position = ReadVector3(node, "position");
    Eigen::Vector3f rotation = ReadVector3(node, "rotation");

    std::shared_ptr<Camera> camera = std::make_shared<Camera>();
    camera->SetName(name);
    camera->SetHFOV(fov);
    camera->SetClip(nearClip, farClip);
    camera->SetPosition(position);
    camera->SetRotation(rotation);
    return camera;
}

std::shared_ptr<DirectionalLight> BuildDirectionalLight(const nlohmann::json& node)
{
    const std::string name = node["name"];
    Eigen::Vector3f position = ReadVector3(node, "position");
    Eigen::Vector3f rotation = ReadVector3(node, "rotation");
    Eigen::Vector3f color = ReadVector3(node, "color");
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
    Eigen::Vector3f position = ReadVector3(node, "position");
    Eigen::Vector3f rotation = ReadVector3(node, "rotation");
    Eigen::Vector3f color = ReadVector3(node, "color");
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
    Eigen::Vector3f position = ReadVector3(node, "position");
    Eigen::Vector3f rotation = ReadVector3(node, "rotation");
    Eigen::Vector3f color = ReadVector3(node, "color");
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

WorldEnvironment BuildEnvironment(const nlohmann::json& node, bool hasBrdfLut)
{
    WorldEnvironment environment;
    environment.hdrPath = node.value("hdrPath", std::string());
    environment.cubeSize = node.value("cubeSize", 512u);
    environment.hasBrdfLut = hasBrdfLut;

    if (!environment.hdrPath.empty())
    {
        environment.sphericalHarmonics = EnvironmentSHGenerator::Generate(environment.hdrPath);
        environment.hasSphericalHarmonics = true;
    }

    return environment;
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
    const RendererResourceCache& resourceCache) const
{
    try
    {
        std::shared_ptr<World> world = std::make_shared<World>(
            generation,
            worldBuildPlan.scenePath);

        WorldEnvironment environment;
        environment.hasBrdfLut = resourceCache.HasGlobalTexture("brdfLut");

        const nlohmann::json& objectsJson = worldBuildPlan.sceneJson["objects"];
        for (const SceneObjectBuildPlan& objectPlan : worldBuildPlan.sceneAssetPlan.objectPlans)
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
                AddDirectionalLightIfFirst(*world, BuildDirectionalLight(objectJson));
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
                environment = BuildEnvironment(objectJson, resourceCache.HasGlobalTexture("brdfLut"));
            }
        }

        if (!world->GetCamera())
        {
            return RuntimeResult<std::shared_ptr<World>>::Failure(MakeRuntimeError(
                "WorldBuilder.NoCamera",
                "Cannot build a World because the build plan has no camera.",
                worldBuildPlan.scenePath));
        }

        world->SetEnvironment(environment);

        // Mesh SceneObject instances still come from the renderer resource
        // cache because they own descriptors and UBOs. Gameplay-owned
        // camera/light data comes directly from the validated WorldBuildPlan.
        for (const auto& [objectName, object] : resourceCache.GetSceneObjects())
        {
            world->AddSceneObject(objectName, object);
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
