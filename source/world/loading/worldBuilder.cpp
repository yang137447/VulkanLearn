#include "world/loading/worldBuilder.h"

#include <exception>

#include "commonFunction.h"
#include "render/resource/rendererResourceCache.h"
#include "sceneNode.h"
#include "world/loading/worldLoader.h"
#include "render/lighting/physicalLighting.h"

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
    camera->SetExposure(ReadCameraExposure(node));
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
    const float intensity = ReadCanonicalLightIntensity(node);

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
    const float intensity = ReadCanonicalLightIntensity(node);
    const float radius = node.value("radius", 0.0f);

    std::shared_ptr<PointLight> light = std::make_shared<PointLight>();
    light->SetName(name);
    light->SetColor(color);
    light->SetIntensity(intensity);
    light->SetRadius(radius);
    light->SetPosition(position);
    light->SetRotation(rotation);
    if (node.contains("pulse"))
    {
        const nlohmann::json& pulse = node.at("pulse");
        // 校验器要求 pulse 的四个子字段同时存在；这里直接取，避免绕过校验
        // 的调用路径把缺失字段静默降级成“无脉冲”。
        light->SetPulsePeriod(pulse.at("period").get<float>());
        light->SetPulseDuration(pulse.at("duration").get<float>());
        light->SetPulsePhase(pulse.at("phase").get<float>());
        light->SetPulsePeakIntensity(
            pulse.at("peakIntensity").get<float>());
    }
    return light;
}

std::shared_ptr<SpotLight> BuildSpotLight(const nlohmann::json& node)
{
    const std::string name = node["name"];
    Eigen::Vector3f position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    Eigen::Vector3f rotation = JsonParser::ParseValue<Eigen::Vector3f>(node["rotation"]);
    Eigen::Vector3f color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    const float intensity = ReadCanonicalLightIntensity(node);
    const float radius = node.value("radius", 0.0f);
    // 场景 JSON 使用角度制；GPU 的 acos 与锥角比较使用弧度制。
    const float coneAngleOuter =
        JsonParser::ParseValue<float>(node["cone_angle_outer"]) * M_PI / 180.0f;
    const float coneAngleInner =
        JsonParser::ParseValue<float>(node["cone_angle_inner"]) * M_PI / 180.0f;

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

WorldSquareAreaLight BuildSquareAreaLight(const nlohmann::json& node)
{
    WorldSquareAreaLight light;
    light.position = JsonParser::ParseValue<Eigen::Vector3f>(node["position"]);
    light.normal = JsonParser::ParseValue<Eigen::Vector3f>(node["normal"]).normalized();
    light.color = JsonParser::ParseValue<Eigen::Vector3f>(node["color"]);
    light.size = node["size"].get<float>();
    light.intensity = ReadCanonicalLightIntensity(node);
    return light;
}

WorldEnvironment BuildEnvironment(const nlohmann::json& node)
{
    const nlohmann::json& config = node["environment"];

    WorldEnvironment environment;
    environment.name = node.value("name", std::string());
    environment.type = ParseEnvironmentType(config["type"].get<std::string>());
    environment.cubeSize = config.value("cubeSize", 128u);
    environment.intensity = config.value("intensity", 1.0f);
    environment.hdrPath = config.value("hdrPath", std::string());
    environment.skyParameters = BuildSkyParameters(config);

    return environment;
}

Eigen::Matrix4f BuildParticleSystemModel(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& rotation,
    const Eigen::Vector3f& scale)
{
    Eigen::Matrix4f scaleMatrix = Eigen::Matrix4f::Identity();
    scaleMatrix(0, 0) = scale.x();
    scaleMatrix(1, 1) = scale.y();
    scaleMatrix(2, 2) = scale.z();
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation.block<3, 1>(0, 3) = position;
    return translation *
        CommonFunction::RotationToMatrix(rotation) *
        scaleMatrix;
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
            else if (type == "squareAreaLight")
            {
                world->AddSquareAreaLight(objectPlan.objectName,
                                          BuildSquareAreaLight(objectJson));
            }
            else if (type == "environment")
            {
                environment = BuildEnvironment(objectJson);
            }
            else if (type == "decal")
            {
                WorldDecal decal;
                decal.name = objectPlan.objectName;
                decal.scenePosition =
                    JsonParser::ParseValue<Eigen::Vector3f>(
                        objectJson["position"]);
                decal.sceneRotation =
                    JsonParser::ParseValue<Eigen::Vector3f>(
                        objectJson["rotation"]);
                decal.sceneScale = objectJson.contains("scale")
                    ? JsonParser::ParseValue<Eigen::Vector3f>(
                        objectJson["scale"])
                    : Eigen::Vector3f::Ones();
                decal.size =
                    JsonParser::ParseValue<Eigen::Vector3f>(
                        objectJson["size"]);
                const Eigen::Vector3f halfExtent =
                    decal.size.cwiseProduct(decal.sceneScale) * 0.5f;
                Eigen::Matrix4f scale = Eigen::Matrix4f::Identity();
                scale(0, 0) = halfExtent.x();
                scale(1, 1) = halfExtent.y();
                scale(2, 2) = halfExtent.z();
                decal.decalToWorld =
                    Eigen::Affine3f(Eigen::Translation3f(
                        decal.scenePosition)).matrix() *
                    CommonFunction::RotationToMatrix(
                        decal.sceneRotation) *
                    scale;
                if (objectJson.contains("color"))
                {
                    const Eigen::Vector3f color =
                        JsonParser::ParseValue<Eigen::Vector3f>(
                            objectJson["color"]);
                    decal.color =
                        Eigen::Vector4f(color.x(), color.y(), color.z(), 1.0f);
                }
                decal.opacity = objectJson.value(
                    "opacity", decal.opacity);
                decal.edgeSoftness = objectJson.value(
                    "edgeSoftness", decal.edgeSoftness);
                decal.noiseStrength = objectJson.value(
                    "noiseStrength", decal.noiseStrength);
                decal.roughness = objectJson.value(
                    "roughness", decal.roughness);
                decal.metallic = objectJson.value(
                    "metallic", decal.metallic);
                decal.ambientOcclusion = objectJson.value(
                    "ambientOcclusion", decal.ambientOcclusion);
                if (objectJson.contains("decalResponse"))
                {
                    const std::string response =
                        objectJson.at("decalResponse").get<std::string>();
                    if (!TryParseDecalResponse(
                            response, decal.responseMask))
                    {
                        throw std::runtime_error(
                            "Decal " + objectPlan.objectName +
                            " has unknown decalResponse: " + response);
                    }
                }
                decal.normalStrength = objectJson.value(
                    "normalStrength", decal.normalStrength);
                decal.albedoMap = objectJson.value(
                    "albedoMap", decal.albedoMap);
                decal.normalMap = objectJson.value(
                    "normalMap", decal.normalMap);
                decal.ormMap = objectJson.value(
                    "ormMap", decal.ormMap);
                decal.sortOrder = static_cast<uint32_t>(
                    objectJson.value("sortOrder", 0));
                world->AddDecal(
                    objectPlan.objectName,
                    std::move(decal));
            }
            else if (type == "particleSystem")
            {
                if (!objectPlan.particleAsset)
                {
                    throw std::runtime_error(
                        "Particle system plan is missing its prepared asset: " +
                        objectPlan.objectName);
                }
                WorldParticleSystem particleSystem;
                particleSystem.name = objectPlan.objectName;
                particleSystem.assetPath =
                    objectJson.at("particlePath").get<std::string>();
                particleSystem.asset = *objectPlan.particleAsset;
                particleSystem.scenePosition =
                    JsonParser::ParseValue<Eigen::Vector3f>(
                        objectJson["position"]);
                particleSystem.sceneRotation =
                    JsonParser::ParseValue<Eigen::Vector3f>(
                        objectJson["rotation"]);
                particleSystem.sceneScale = objectJson.contains("scale")
                    ? JsonParser::ParseValue<Eigen::Vector3f>(
                        objectJson["scale"])
                    : Eigen::Vector3f::Ones();
                particleSystem.model =
                    BuildParticleSystemModel(
                        particleSystem.scenePosition,
                        particleSystem.sceneRotation,
                        particleSystem.sceneScale);
                world->AddParticleSystem(
                    objectPlan.objectName,
                    std::move(particleSystem));
            }
        }

        if (!world->GetCamera())
        {
            return RuntimeResult<std::shared_ptr<World>>::Failure(MakeRuntimeError(
                "WorldBuilder.NoCamera",
                "Cannot build a World because the build plan has no camera.",
                worldBuildPlan.scenePath));
        }

        if (primaryDirectionalLight)
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
            object.animation = meshObjectPlan.animation;
            object.skinPose = meshObjectPlan.skinPose;
            object.skinSection = meshObjectPlan.skinSection;
            object.debugName = meshObjectPlan.debugName;
            object.sceneObjectIdentity = meshObjectPlan.sceneObjectIdentity;
            object.materialSlotIndex = meshObjectPlan.materialSlotIndex;
            object.materialSlotName = meshObjectPlan.materialSlotName;
            object.model = meshObjectPlan.model;
            object.scenePosition = meshObjectPlan.scenePosition;
            object.sceneRotation = meshObjectPlan.sceneRotation;
            object.sceneScale = meshObjectPlan.sceneScale;
            object.localBoundsMin = meshObjectPlan.localBoundsMin;
            object.localBoundsMax = meshObjectPlan.localBoundsMax;
            object.collisionTriangles = meshObjectPlan.collisionTriangles;
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
