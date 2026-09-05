#include "materialAssetValidator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include "../materialAssetUtils.h"

namespace
{
    bool IsFiniteNumber(const nlohmann::json& value);

    void RequireObjectField(const nlohmann::json& json, std::string_view field, std::string_view context)
    {
        const std::string fieldName(field);
        if (!json.contains(fieldName) || !json.at(fieldName).is_object())
        {
            throw std::runtime_error(std::string(context) + " is missing object field \"" + fieldName + "\"");
        }
    }

    bool IsShaderRootRelativePath(const nlohmann::json& value)
    {
        if (!value.is_string())
        {
            return false;
        }
        const std::filesystem::path path(value.get<std::string>());
        if (path.empty() || path.is_absolute())
        {
            return false;
        }
        const std::filesystem::path normalized = path.lexically_normal();
        return normalized.begin() == normalized.end() || *normalized.begin() != "..";
    }

    void ValidateMaterialEvaluationPaths(
        const nlohmann::json& evaluation,
        std::string_view materialPath)
    {
        const std::string materialGlslPath =
            MaterialAssetUtils::NormalizeShaderGlslRelativePath(materialPath);
        const std::filesystem::path materialFilePath(materialGlslPath);
        const std::string materialStem = materialFilePath.stem().string();
        const std::filesystem::path materialDirectory = materialFilePath.parent_path();
        const std::string expectedVertex =
            (materialDirectory / (materialStem + ".vertex.glsl")).generic_string();
        const std::string expectedSurface =
            (materialDirectory / (materialStem + ".surface.glsl")).generic_string();
        const std::string actualVertex =
            std::filesystem::path(evaluation.at("vertex").get<std::string>())
                .lexically_normal().generic_string();
        const std::string actualSurface =
            std::filesystem::path(evaluation.at("surface").get<std::string>())
                .lexically_normal().generic_string();
        if (actualVertex != expectedVertex || actualSurface != expectedSurface)
        {
            throw std::runtime_error(
                "shaderEvaluation must point to the material-local public entries " +
                expectedVertex + " and " + expectedSurface + ": " +
                std::string(materialPath));
        }
    }

    void ValidateSparseOverrideObject(
        const nlohmann::json& overrides,
        std::string_view field,
        std::string_view materialInstancePath)
    {
        if (overrides.is_object() && overrides.empty())
        {
            throw std::runtime_error(
                "Empty material instance " + std::string(field) +
                " object must be omitted: " + std::string(materialInstancePath));
        }
    }

    size_t GetMaterialParameterComponentCount(std::string_view type)
    {
        if (type == "vec2") return 2;
        if (type == "vec3") return 3;
        if (type == "vec4" || type == "color") return 4;
        return 1;
    }

    void ValidateParameterChannels(
        const nlohmann::json& parameterJson,
        std::string_view parameterName,
        std::string_view type,
        std::string_view materialPath)
    {
        if (!parameterJson.contains("channels"))
        {
            return;
        }
        if (type == "float")
        {
            throw std::runtime_error(
                "Scalar material parameter cannot declare channels: " +
                std::string(parameterName) + " in " + std::string(materialPath));
        }

        const nlohmann::json& channels = parameterJson.at("channels");
        if (!channels.is_object())
        {
            throw std::runtime_error(
                "Material parameter channels must be an object: " +
                std::string(parameterName) + " in " + std::string(materialPath));
        }

        static constexpr std::array<std::string_view, 4> componentNames = {
            "x", "y", "z", "w"};
        const size_t componentCount =
            GetMaterialParameterComponentCount(type);
        if (channels.size() != componentCount)
        {
            throw std::runtime_error(
                "Material parameter channels must describe every " +
                std::string(type) + " component exactly once: " +
                std::string(parameterName) + " in " + std::string(materialPath));
        }

        for (size_t componentIndex = 0;
             componentIndex < componentCount;
             ++componentIndex)
        {
            const std::string componentName(componentNames[componentIndex]);
            if (!channels.contains(componentName) ||
                !channels.at(componentName).is_object())
            {
                throw std::runtime_error(
                    "Material parameter channels is missing object component \"" +
                    componentName + "\": " + std::string(parameterName) +
                    " in " + std::string(materialPath));
            }

            const nlohmann::json& channel = channels.at(componentName);
            if (!channel.contains("name") || !channel.at("name").is_string() ||
                MaterialAssetUtils::Trim(channel.at("name").get<std::string>()).empty() ||
                !channel.contains("description") ||
                !channel.at("description").is_string() ||
                MaterialAssetUtils::Trim(
                    channel.at("description").get<std::string>()).empty())
            {
                throw std::runtime_error(
                    "Material parameter channel requires non-empty name and description: " +
                    std::string(parameterName) + "." + componentName +
                    " in " + std::string(materialPath));
            }

            if (!channel.contains("range") || !channel.at("range").is_object() ||
                !channel.at("range").contains("min") ||
                !channel.at("range").contains("max") ||
                !IsFiniteNumber(channel.at("range").at("min")) ||
                !IsFiniteNumber(channel.at("range").at("max")))
            {
                throw std::runtime_error(
                    "Material parameter channel requires finite range min/max: " +
                    std::string(parameterName) + "." + componentName +
                    " in " + std::string(materialPath));
            }

            const double minimum =
                channel.at("range").at("min").get<double>();
            const double maximum =
                channel.at("range").at("max").get<double>();
            if (minimum > maximum)
            {
                throw std::runtime_error(
                    "Material parameter channel range min must not exceed max: " +
                    std::string(parameterName) + "." + componentName +
                    " in " + std::string(materialPath));
            }

            const nlohmann::json& defaultValue =
                parameterJson.at("default").at(componentIndex);
            if (!IsFiniteNumber(defaultValue) ||
                defaultValue.get<double>() < minimum ||
                defaultValue.get<double>() > maximum)
            {
                throw std::runtime_error(
                    "Material parameter default is outside its declared channel range: " +
                    std::string(parameterName) + "." + componentName +
                    " in " + std::string(materialPath));
            }
        }

        for (const auto& [componentName, channel] : channels.items())
        {
            (void)channel;
            const auto componentEnd =
                componentNames.begin() + static_cast<std::ptrdiff_t>(componentCount);
            if (std::find(
                    componentNames.begin(),
                    componentEnd,
                    componentName) == componentEnd)
            {
                throw std::runtime_error(
                    "Material parameter channels contains unknown component \"" +
                    componentName + "\": " + std::string(parameterName) +
                    " in " + std::string(materialPath));
            }
        }
    }
}

namespace
{
    bool IsFiniteNumber(const nlohmann::json& value)
    {
        return value.is_number() && std::isfinite(value.get<double>());
    }

    void RequireClothParameter(
        const nlohmann::json& parameters,
        std::string_view name,
        std::string_view type,
        std::string_view materialPath)
    {
        if (!parameters.contains(std::string(name)) ||
            !parameters.at(std::string(name)).is_object() ||
            parameters.at(std::string(name)).value("type", std::string()) != type)
        {
            throw std::runtime_error(
                "Cloth material requires " + std::string(name) + " as " +
                std::string(type) + ": " + std::string(materialPath));
        }
    }

    void ValidateClothDefinition(
        const nlohmann::json& materialJson,
        std::string_view materialPath)
    {
        const auto& parameters = materialJson.at("parameters");
        RequireClothParameter(parameters, "u_clothSheenColor", "color", materialPath);
        RequireClothParameter(parameters, "u_clothSheenRoughness", "float", materialPath);
        RequireClothParameter(parameters, "u_clothAnisotropy", "float", materialPath);
        RequireClothParameter(parameters, "u_clothAnisotropyCross", "float", materialPath);
        RequireClothParameter(parameters, "u_pbrFactors", "vec4", materialPath);

        const auto& sheenColor = parameters.at("u_clothSheenColor").at("default");
        if (!sheenColor.is_array() || sheenColor.size() != 4)
        {
            throw std::runtime_error(
                "Cloth sheen color default must be a four-component linear RGBA value: " +
                std::string(materialPath));
        }
        for (const auto& component : sheenColor)
        {
            if (!IsFiniteNumber(component) || component.get<double>() < 0.0 ||
                component.get<double>() > 1.0)
            {
                throw std::runtime_error(
                    "Cloth sheen color default must be finite and within [0, 1]: " +
                    std::string(materialPath));
            }
        }
        if (sheenColor[3].get<double>() != 1.0)
        {
            throw std::runtime_error(
                "Cloth sheen color alpha is reserved and must remain 1: " +
                std::string(materialPath));
        }

        const auto& sheenRoughness =
            parameters.at("u_clothSheenRoughness").at("default");
        if (!IsFiniteNumber(sheenRoughness) ||
            sheenRoughness.get<double>() < 0.02 ||
            sheenRoughness.get<double>() > 1.0)
        {
            throw std::runtime_error(
                "Cloth sheen roughness default must be within [0.02, 1]: " +
                std::string(materialPath));
        }

        const auto& anisotropy =
            parameters.at("u_clothAnisotropy").at("default");
        if (!IsFiniteNumber(anisotropy) ||
            anisotropy.get<double>() < -1.0 ||
            anisotropy.get<double>() > 1.0)
        {
            throw std::runtime_error(
                "Cloth anisotropy default must be within [-1, 1]: " +
                std::string(materialPath));
        }

        const auto& anisotropyCross =
            parameters.at("u_clothAnisotropyCross").at("default");
        if (!IsFiniteNumber(anisotropyCross) ||
            anisotropyCross.get<double>() < 0.0 ||
            anisotropyCross.get<double>() > 1.0)
        {
            throw std::runtime_error(
                "Cloth anisotropy cross default must be within [0, 1]: " +
                std::string(materialPath));
        }

        const auto& pbrFactors = parameters.at("u_pbrFactors").at("default");
        if (!pbrFactors.is_array() || pbrFactors.size() != 4)
        {
            throw std::runtime_error(
                "Cloth u_pbrFactors default must be a four-component value: " +
                std::string(materialPath));
        }
        for (const auto& component : pbrFactors)
        {
            if (!IsFiniteNumber(component))
            {
                throw std::runtime_error(
                    "Cloth u_pbrFactors default must be finite: " +
                    std::string(materialPath));
            }
        }
        if (pbrFactors[0].get<double>() < 0.02 ||
            pbrFactors[0].get<double>() > 1.0 ||
            pbrFactors[1].get<double>() != 0.0 ||
            pbrFactors[2].get<double>() < 0.0 ||
            pbrFactors[2].get<double>() > 1.0)
        {
            throw std::runtime_error(
                "Cloth base PBR defaults require roughness [0.02, 1], metallic 0 and AO [0, 1]: " +
                std::string(materialPath));
        }
    }
}
void MaterialAssetValidator::ValidateDefinition(
    const nlohmann::json& materialJson,
    std::string_view materialPath)
{
    if (!materialJson.is_object())
    {
        throw std::runtime_error("Material definition must be a JSON object: " + std::string(materialPath));
    }
    if (materialJson.value("type", std::string()) != "material")
    {
        throw std::runtime_error("Material definition type must be \"material\": " + std::string(materialPath));
    }
    if (!materialJson.contains("name") || !materialJson["name"].is_string())
    {
        throw std::runtime_error("Material definition is missing string field \"name\": " + std::string(materialPath));
    }
    if (!materialJson.contains("shadingModel") || !materialJson["shadingModel"].is_string())
    {
        throw std::runtime_error("Material definition is missing string field \"shadingModel\": " + std::string(materialPath));
    }
    RequireObjectField(materialJson, "renderStates", materialPath);
    RequireObjectField(materialJson, "macros", materialPath);
    RequireObjectField(materialJson, "parameters", materialPath);
    RequireObjectField(materialJson, "textures", materialPath);
    MaterialAssetUtils::ShadingModelToId(materialJson["shadingModel"].get<std::string>());
    const auto& renderStates = materialJson["renderStates"];
    ValidateRenderStateCombination(
        materialJson["shadingModel"].get<std::string>(),
        renderStates.value("renderMode", std::string("Opaque")),
        renderStates.value("cullMode", std::string("Back")),
        materialPath);
    if (materialJson["shadingModel"].get<std::string>() == "Cloth")
    {
        ValidateClothDefinition(materialJson, materialPath);
    }

    if (materialJson.contains("features"))
    {
        RequireObjectField(materialJson, "features", materialPath);
        for (const auto& [featureName, featureValue] : materialJson["features"].items())
        {
            if (featureName != "modifiesMeshPosition" || !featureValue.is_boolean())
            {
                throw std::runtime_error(
                    "Material features only supports boolean modifiesMeshPosition: " +
                    std::string(materialPath));
            }
        }
    }

    if (materialJson.contains("shaderEvaluation"))
    {
        RequireObjectField(materialJson, "shaderEvaluation", materialPath);
        const auto& evaluation = materialJson["shaderEvaluation"];
        if (evaluation.size() != 2 ||
            !evaluation.contains("vertex") || !IsShaderRootRelativePath(evaluation["vertex"]) ||
            !evaluation.contains("surface") || !IsShaderRootRelativePath(evaluation["surface"]))
        {
            throw std::runtime_error(
                "shaderEvaluation must contain shader/glsl-relative vertex and surface paths: " +
                std::string(materialPath));
        }
        ValidateMaterialEvaluationPaths(evaluation, materialPath);
    }

    for (const auto& [macroName, macroValue] : materialJson["macros"].items())
    {
        if (MaterialAssetUtils::Trim(macroName).empty() || !MaterialAssetUtils::IsNumericMacroValue(macroValue))
        {
            throw std::runtime_error("Material macros must be non-empty numeric values: " + std::string(materialPath));
        }
    }

    for (const auto& [parameterName, parameterJson] : materialJson["parameters"].items())
    {
        if (!parameterJson.is_object() ||
            !parameterJson.contains("type") || !parameterJson["type"].is_string() ||
            !parameterJson.contains("default"))
        {
            throw std::runtime_error(
                "Material parameter must contain type and default: " + parameterName +
                " in " + std::string(materialPath));
        }
        const std::string type = parameterJson["type"].get<std::string>();
        if (!MaterialAssetUtils::IsMaterialParameterType(type) ||
            !MaterialAssetUtils::MaterialParameterValueMatchesType(parameterJson["default"], type))
        {
            throw std::runtime_error(
                "Material parameter type/default mismatch: " + parameterName +
                " in " + std::string(materialPath));
        }
        // 通道说明只服务作者与工具；完整性在资产入口校验，不能改变 UBO 或 shader ABI。
        ValidateParameterChannels(
            parameterJson,
            parameterName,
            type,
            materialPath);
    }

    for (const auto& [textureName, textureJson] : materialJson["textures"].items())
    {
        if (!textureJson.is_object() ||
            textureJson.value("type", std::string()) != "sampler2D" ||
            !textureJson.contains("default"))
        {
            throw std::runtime_error(
                "Material texture must be sampler2D and contain default: " + textureName +
                " in " + std::string(materialPath));
        }
    }
}

void MaterialAssetValidator::ValidateInstanceHeader(
    const nlohmann::json& materialInstanceJson,
    std::string_view materialInstancePath)
{
    if (!materialInstanceJson.is_object())
    {
        throw std::runtime_error("Material instance must be a JSON object: " + std::string(materialInstancePath));
    }

    static const std::vector<std::string> allowedFields = {
        "name",
        "type",
        "configHelp",
        "material",
        "renderStateOverrides",
        "macros",
        "parameters",
        "textures",
        "subsurfaceProfile",
        "skinLut",
        "eyeProfile"
    };
    for (const auto& [field, value] : materialInstanceJson.items())
    {
        if (std::find(allowedFields.begin(), allowedFields.end(), field) == allowedFields.end())
        {
            throw std::runtime_error("Unknown material instance field \"" + field + "\": " + std::string(materialInstancePath));
        }
    }

    if (materialInstanceJson.value("type", std::string()) != "materialInstance")
    {
        throw std::runtime_error("Material instance type must be \"materialInstance\": " + std::string(materialInstancePath));
    }
    if (!materialInstanceJson.contains("material") || !materialInstanceJson["material"].is_string())
    {
        throw std::runtime_error("Material instance is missing string field \"material\": " + std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("renderStateOverrides") && !materialInstanceJson["renderStateOverrides"].is_object())
    {
        throw std::runtime_error("renderStateOverrides must be an object: " + std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("macros") && !materialInstanceJson["macros"].is_object())
    {
        throw std::runtime_error("Material instance macros must be an object: " + std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("parameters") && !materialInstanceJson["parameters"].is_object())
    {
        throw std::runtime_error("Material instance parameters must be an object: " + std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("textures") && !materialInstanceJson["textures"].is_object())
    {
        throw std::runtime_error("Material instance textures must be an object: " + std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("subsurfaceProfile") &&
        (!materialInstanceJson["subsurfaceProfile"].is_string() ||
         materialInstanceJson["subsurfaceProfile"].get<std::string>().empty()))
    {
        throw std::runtime_error(
            "subsurfaceProfile must be a non-empty asset path string: " +
            std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("skinLut") &&
        (!materialInstanceJson["skinLut"].is_string() ||
         materialInstanceJson["skinLut"].get<std::string>().empty()))
    {
        throw std::runtime_error(
            "skinLut must be a non-empty asset path string: " +
            std::string(materialInstancePath));
    }
    if (materialInstanceJson.contains("eyeProfile") &&
        (!materialInstanceJson["eyeProfile"].is_string() ||
         materialInstanceJson["eyeProfile"].get<std::string>().empty()))
    {
        throw std::runtime_error(
            "eyeProfile must be a non-empty asset path string: " +
            std::string(materialInstancePath));
    }
}

void MaterialAssetValidator::ValidateInstanceOverrides(
    const nlohmann::json& materialJson,
    const nlohmann::json& materialInstanceJson,
    std::string_view materialInstancePath)
{
    const nlohmann::json emptyObject = nlohmann::json::object();
    const auto& macroOverrides = materialInstanceJson.value("macros", emptyObject);
    const auto& parameterOverrides = materialInstanceJson.value("parameters", emptyObject);
    const auto& textureOverrides = materialInstanceJson.value("textures", emptyObject);
    const auto& renderStateOverrides = materialInstanceJson.value("renderStateOverrides", emptyObject);

    if (materialInstanceJson.contains("macros"))
    {
        ValidateSparseOverrideObject(macroOverrides, "macros", materialInstancePath);
    }
    if (materialInstanceJson.contains("parameters"))
    {
        ValidateSparseOverrideObject(parameterOverrides, "parameters", materialInstancePath);
    }
    if (materialInstanceJson.contains("textures"))
    {
        ValidateSparseOverrideObject(textureOverrides, "textures", materialInstancePath);
    }
    if (materialInstanceJson.contains("renderStateOverrides"))
    {
        ValidateSparseOverrideObject(
            renderStateOverrides,
            "renderStateOverrides",
            materialInstancePath);
    }

    EnsureKnownOverrideKeys(
        macroOverrides,
        materialJson["macros"],
        "macro",
        materialInstancePath);
    EnsureKnownOverrideKeys(
        parameterOverrides,
        materialJson["parameters"],
        "parameter",
        materialInstancePath);
    EnsureKnownOverrideKeys(
        textureOverrides,
        materialJson["textures"],
        "texture",
        materialInstancePath);

    for (const auto& [name, value] : macroOverrides.items())
    {
        if (!MaterialAssetUtils::IsNumericMacroValue(value))
        {
            throw std::runtime_error(
                "Material instance macro overrides must be numeric: " +
                std::string(materialInstancePath));
        }
        if (value == materialJson["macros"].at(name))
        {
            throw std::runtime_error(
                "Material instance macro redundantly repeats M_ default \"" + name +
                "\": " + std::string(materialInstancePath));
        }
    }

    for (const auto& [name, value] : renderStateOverrides.items())
    {
        if (name == "shadingModel")
        {
            if (!value.is_string())
            {
                throw std::runtime_error(
                    "renderStateOverrides.shadingModel must be a string: " +
                    std::string(materialInstancePath));
            }
            MaterialAssetUtils::ShadingModelToId(value.get<std::string>());
            if (value == materialJson["shadingModel"])
            {
                throw std::runtime_error(
                    "Material instance redundantly repeats M_ shadingModel: " +
                    std::string(materialInstancePath));
            }
            continue;
        }

        if (name != "renderMode" &&
            !materialJson["renderStates"].contains(name))
        {
            throw std::runtime_error(
                "Material instance overrides unknown render state \"" + name + "\": " +
                std::string(materialInstancePath));
        }
        const std::string materialDefault =
            materialJson["renderStates"].value(
                name,
                name == "renderMode" ? std::string("Opaque") : std::string());
        if (value == materialDefault)
        {
            throw std::runtime_error(
                "Material instance redundantly repeats M_ render state \"" + name +
                "\": " + std::string(materialInstancePath));
        }
    }

    for (const auto& [name, value] : parameterOverrides.items())
    {
        const auto& descriptor = materialJson["parameters"].at(name);
        if (value == descriptor.at("default"))
        {
            throw std::runtime_error(
                "Material instance parameter redundantly repeats M_ default \"" + name +
                "\": " + std::string(materialInstancePath));
        }
    }

    for (const auto& [name, value] : textureOverrides.items())
    {
        const auto& descriptor = materialJson["textures"].at(name);
        if (value == descriptor.at("default"))
        {
            throw std::runtime_error(
                "Material instance texture redundantly repeats M_ default \"" + name +
                "\": " + std::string(materialInstancePath));
        }
    }

    const auto shadingModelOverride = renderStateOverrides.find("shadingModel");
    const std::string shadingModel = shadingModelOverride !=
            renderStateOverrides.end()
        ? shadingModelOverride->get<std::string>()
        : materialJson.at("shadingModel").get<std::string>();
    const auto renderModeOverride = renderStateOverrides.find("renderMode");
    const std::string renderMode = renderModeOverride !=
            renderStateOverrides.end()
        ? renderModeOverride->get<std::string>()
        : materialJson["renderStates"].value("renderMode", std::string("Opaque"));
    const auto cullModeOverride = renderStateOverrides.find("cullMode");
    const std::string cullMode = cullModeOverride !=
            renderStateOverrides.end()
        ? cullModeOverride->get<std::string>()
        : materialJson["renderStates"].value("cullMode", std::string("Back"));
    ValidateRenderStateCombination(
        shadingModel,
        renderMode,
        cullMode,
        materialInstancePath);
}

void MaterialAssetValidator::ValidateRenderStateCombination(
    std::string_view shadingModel,
    std::string_view renderMode,
    std::string_view materialInstancePath)
{
    ValidateRenderStateCombination(
        shadingModel,
        renderMode,
        std::string_view(),
        materialInstancePath);
}

void MaterialAssetValidator::ValidateRenderStateCombination(
    std::string_view shadingModel,
    std::string_view renderMode,
    std::string_view cullMode,
    std::string_view materialInstancePath)
{
    if (shadingModel.empty())
    {
        throw std::runtime_error(
            "Material shadingModel must not be empty: " +
            std::string(materialInstancePath));
    }
    MaterialAssetUtils::ShadingModelToId(shadingModel);

    static constexpr std::array<std::string_view, 9> renderModes = {
        "Opaque",
        "OpaqueClip",
        "ForwardOpaque",
        "ForwardEyeInner",
        "ForwardEyeCornea",
        "TransparentAlphaBlend",
        "TransparentAlphaBlendWriteDepth",
        "TransparentAdditive",
        "ThinTranslucent"};
    if (std::find(renderModes.begin(), renderModes.end(), renderMode) ==
        renderModes.end())
    {
        throw std::runtime_error(
            "Unsupported renderMode: " + std::string(renderMode) +
            ": " + std::string(materialInstancePath));
    }

    const bool usesThinTranslucentShadingModel =
        shadingModel == "ThinTranslucent";
    const bool usesEyeShadingModel = shadingModel == "Eye";
    const bool usesForwardOpaqueRenderMode = renderMode == "ForwardOpaque";
    const bool usesEyeLayerRenderMode =
        renderMode == "ForwardEyeInner" ||
        renderMode == "ForwardEyeCornea";
    const bool usesDeferredOpaqueRenderMode = renderMode == "Opaque";

    if (usesEyeShadingModel &&
        !usesForwardOpaqueRenderMode &&
        !usesEyeLayerRenderMode &&
        !usesDeferredOpaqueRenderMode)
    {
        throw std::runtime_error(
            "Eye shadingModel requires an explicit Eye render path: " +
            std::string(materialInstancePath));
    }
    if (!usesEyeShadingModel &&
        (usesForwardOpaqueRenderMode || usesEyeLayerRenderMode))
    {
        throw std::runtime_error(
            "Only Eye shadingModel may use an Eye forward render path: " +
            std::string(materialInstancePath));
    }
    if (usesThinTranslucentShadingModel !=
        (renderMode == "ThinTranslucent"))
    {
        throw std::runtime_error(
            "ThinTranslucent shadingModel and renderMode must be selected together: " +
            std::string(materialInstancePath));
    }

    if (shadingModel == "TwoSidedFoliage")
    {
        if (renderMode != "Opaque" && renderMode != "OpaqueClip")
        {
            throw std::runtime_error(
                "TwoSidedFoliage only supports Opaque or OpaqueClip renderMode: " +
                std::string(materialInstancePath));
        }
        if (!cullMode.empty() && cullMode != "None")
        {
            throw std::runtime_error(
                "TwoSidedFoliage requires cullMode None: " +
                std::string(materialInstancePath));
        }
    }
}

void MaterialAssetValidator::EnsureKnownOverrideKeys(
    const nlohmann::json& overrides,
    const nlohmann::json& source,
    std::string_view field,
    std::string_view materialInstancePath)
{
    for (const auto& [name, value] : overrides.items())
    {
        if (!source.contains(name))
        {
            throw std::runtime_error(
                "Material instance overrides unknown " + std::string(field) + " \"" + name + "\": " +
                std::string(materialInstancePath));
        }
    }
}
