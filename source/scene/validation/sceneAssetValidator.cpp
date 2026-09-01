#include "sceneAssetValidator.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    std::string BuildObjectContext(
        std::string_view scenePath,
        size_t objectIndex,
        std::string_view objectType)
    {
        return std::string(scenePath) +
            " objectIndex=" + std::to_string(objectIndex) +
            " type=" + std::string(objectType);
    }

    std::string ReadRequiredStringField(
        const nlohmann::json& objectJson,
        std::string_view field,
        std::string_view context)
    {
        const std::string fieldName(field);
        if (!objectJson.contains(fieldName) || !objectJson[fieldName].is_string())
        {
            throw std::runtime_error(
                "Scene object is missing string field \"" + fieldName + "\": " +
                std::string(context));
        }

        const std::string fieldValue = objectJson[fieldName].get<std::string>();
        if (fieldValue.empty())
        {
            throw std::runtime_error(
                "Scene object field \"" + fieldName + "\" must not be empty: " +
                std::string(context));
        }
        return fieldValue;
    }

    void RequireNumberField(
        const nlohmann::json& objectJson,
        std::string_view field,
        std::string_view context)
    {
        const std::string fieldName(field);
        if (!objectJson.contains(fieldName) || !objectJson[fieldName].is_number())
        {
            throw std::runtime_error(
                "Scene object is missing numeric field \"" + fieldName + "\": " +
                std::string(context));
        }
    }

    void RequireVector3Field(
        const nlohmann::json& objectJson,
        std::string_view field,
        std::string_view context)
    {
        const std::string fieldName(field);
        if (!objectJson.contains(fieldName) ||
            !objectJson[fieldName].is_array() ||
            objectJson[fieldName].size() != 3)
        {
            throw std::runtime_error(
                "Scene object field \"" + fieldName + "\" must be a numeric vec3 array: " +
                std::string(context));
        }

        for (const auto& value : objectJson[fieldName])
        {
            if (!value.is_number())
            {
                throw std::runtime_error(
                    "Scene object field \"" + fieldName + "\" must be a numeric vec3 array: " +
                    std::string(context));
            }
        }
    }

    void RequireTransformFields(const nlohmann::json& objectJson, std::string_view context)
    {
        RequireVector3Field(objectJson, "position", context);
        RequireVector3Field(objectJson, "rotation", context);
        RequireVector3Field(objectJson, "scale", context);
    }

    void ValidateDirectionalLightObject(const nlohmann::json& objectJson, std::string_view context)
    {
        RequireVector3Field(objectJson, "position", context);
        RequireVector3Field(objectJson, "rotation", context);
        RequireVector3Field(objectJson, "color", context);
        RequireNumberField(objectJson, "intensity", context);
    }

    void ValidatePointLightObject(const nlohmann::json& objectJson, std::string_view context)
    {
        RequireVector3Field(objectJson, "position", context);
        RequireVector3Field(objectJson, "rotation", context);
        RequireVector3Field(objectJson, "color", context);
        RequireNumberField(objectJson, "intensity", context);
    }

    void ValidateSpotLightObject(const nlohmann::json& objectJson, std::string_view context)
    {
        RequireVector3Field(objectJson, "position", context);
        RequireVector3Field(objectJson, "rotation", context);
        RequireVector3Field(objectJson, "color", context);
        RequireNumberField(objectJson, "intensity", context);
        RequireNumberField(objectJson, "cone_angle_outer", context);
        RequireNumberField(objectJson, "cone_angle_inner", context);
    }

    void ValidateCameraObject(const nlohmann::json& objectJson, std::string_view context)
    {
        RequireNumberField(objectJson, "fov", context);
        RequireNumberField(objectJson, "near_clip", context);
        RequireNumberField(objectJson, "far_clip", context);
        RequireTransformFields(objectJson, context);
        if (objectJson.contains("look_at"))
        {
            RequireVector3Field(objectJson, "look_at", context);
        }
    }

    void ValidateEnvironmentObject(const nlohmann::json& objectJson, std::string_view context)
    {
        if (objectJson.contains("hdrPath") && !objectJson["hdrPath"].is_string())
        {
            throw std::runtime_error("Scene environment field \"hdrPath\" must be a string: " + std::string(context));
        }

        if (objectJson.contains("cubeSize"))
        {
            if (!objectJson["cubeSize"].is_number_integer() && !objectJson["cubeSize"].is_number_unsigned())
            {
                throw std::runtime_error("Scene environment field \"cubeSize\" must be an integer: " + std::string(context));
            }

            if (objectJson["cubeSize"].get<int64_t>() <= 0)
            {
                throw std::runtime_error("Scene environment field \"cubeSize\" must be > 0: " + std::string(context));
            }
        }

        if (objectJson.contains("skyParameters"))
        {
            if (!objectJson["skyParameters"].is_object())
            {
                throw std::runtime_error("Scene environment field \"skyParameters\" must be an object: " + std::string(context));
            }

            const nlohmann::json& skyParameters = objectJson["skyParameters"];
            RequireVector3Field(skyParameters, "sunColor", context);
            RequireVector3Field(skyParameters, "zenithColor", context);
            RequireVector3Field(skyParameters, "horizonColor", context);
            RequireVector3Field(skyParameters, "groundColor", context);
            RequireNumberField(skyParameters, "sunIntensity", context);
            RequireNumberField(skyParameters, "sunAngularRadius", context);
            RequireNumberField(skyParameters, "skyGradientExponent", context);
            RequireNumberField(skyParameters, "groundGradientExponent", context);
            RequireNumberField(skyParameters, "sunHaloExponent", context);
            RequireNumberField(skyParameters, "sunHaloStrength", context);
        }
    }
}

SceneAssetBuildPlan SceneAssetValidator::BuildLoadPlan(
    std::string_view scenePath,
    const nlohmann::json& sceneJson)
{
    if (!sceneJson.is_object())
    {
        throw std::runtime_error("Scene must be a JSON object: " + std::string(scenePath));
    }
    if (!sceneJson.contains("objects") || !sceneJson["objects"].is_array())
    {
        throw std::runtime_error("Scene is missing objects array: " + std::string(scenePath));
    }

    SceneAssetBuildPlan sceneBuildPlan;
    sceneBuildPlan.scenePath = std::string(scenePath);

    bool hasCamera = false;
    const auto& objectsJson = sceneJson["objects"];
    sceneBuildPlan.objectPlans.reserve(objectsJson.size());
    for (size_t objectIndex = 0; objectIndex < objectsJson.size(); ++objectIndex)
    {
        const auto& objectJson = objectsJson[objectIndex];
        if (!objectJson.is_object())
        {
            throw std::runtime_error(
                "Scene objects entry must be an object: " +
                std::string(scenePath) + " objectIndex=" + std::to_string(objectIndex));
        }

        const std::string objectName = ReadRequiredStringField(
            objectJson,
            "name",
            std::string(scenePath) + " objectIndex=" + std::to_string(objectIndex));
        const std::string objectType = ReadRequiredStringField(
            objectJson,
            "type",
            std::string(scenePath) + " objectIndex=" + std::to_string(objectIndex) +
                " object=" + objectName);
        const std::string objectContext = BuildObjectContext(scenePath, objectIndex, objectType);

        SceneAssetObjectPlan objectPlan;
        objectPlan.objectIndex = objectIndex;
        objectPlan.objectName = objectName;
        objectPlan.objectType = objectType;

        if (objectType == "mesh")
        {
            const std::string modelPath = ReadRequiredStringField(objectJson, "modelPath", objectContext);
            RequireTransformFields(objectJson, objectContext);
            objectPlan.meshLoadRequest = MeshAssetLoader::Prepare(modelPath);
        }
        else if (objectType == "camera")
        {
            ValidateCameraObject(objectJson, objectContext);
            hasCamera = true;
        }
        else if (objectType == "directionalLight")
        {
            ValidateDirectionalLightObject(objectJson, objectContext);
        }
        else if (objectType == "pointLight")
        {
            ValidatePointLightObject(objectJson, objectContext);
        }
        else if (objectType == "spotLight")
        {
            ValidateSpotLightObject(objectJson, objectContext);
        }
        else if (objectType == "environment")
        {
            ValidateEnvironmentObject(objectJson, objectContext);
        }
        else
        {
            throw std::runtime_error("Unknown scene object type: " + objectContext);
        }

        sceneBuildPlan.objectPlans.push_back(std::move(objectPlan));
    }

    if (!hasCamera)
    {
        throw std::runtime_error("Scene has no camera: " + std::string(scenePath));
    }

    return sceneBuildPlan;
}

void SceneAssetValidator::Validate(const SceneAssetBuildPlan& sceneBuildPlan)
{
    for (const SceneAssetObjectPlan& objectPlan : sceneBuildPlan.objectPlans)
    {
        if (!objectPlan.meshLoadRequest.has_value())
        {
            continue;
        }

        const MeshAssetLoadRequest& meshLoadRequest = *objectPlan.meshLoadRequest;
        try
        {
            MeshAssetLoader::ValidateImportRequest(meshLoadRequest);
            if (meshLoadRequest.buildPlan.assetType == "speedtree")
            {
                MeshAssetLoader::ValidateSectionLoadable(meshLoadRequest);
            }
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                "Scene mesh object load validation failed: " + sceneBuildPlan.scenePath +
                " object=" + objectPlan.objectName +
                " modelPath=" + meshLoadRequest.buildPlan.meshAssetPath +
                " error=" + exception.what());
        }
    }
}
