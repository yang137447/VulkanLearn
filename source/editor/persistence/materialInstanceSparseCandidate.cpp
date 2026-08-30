#include "materialInstanceSparseCandidate.h"

#include <charconv>
#include <cmath>
#include <sstream>
#include <system_error>

namespace VL::Editor::Persistence
{
    namespace
    {
        constexpr std::size_t ComponentCount(MaterialInstanceNumericType type) noexcept
        {
            switch (type)
            {
            case MaterialInstanceNumericType::Float:
                return 1;
            case MaterialInstanceNumericType::Vec2:
                return 2;
            case MaterialInstanceNumericType::Vec3:
                return 3;
            case MaterialInstanceNumericType::Vec4:
                return 4;
            }
            return 0;
        }

        std::string FieldSuffix(std::string_view fieldName)
        {
            if (fieldName.empty())
            {
                return {};
            }
            return " for '" + std::string(fieldName) + "'";
        }

        float ParseFiniteFloat(const nlohmann::json& value, std::string_view fieldName)
        {
            if (!value.is_number())
            {
                throw MaterialInstancePersistenceError(
                    "Material instance numeric value must be a number" +
                    FieldSuffix(fieldName));
            }

            const float result = value.get<float>();
            if (!std::isfinite(result))
            {
                throw MaterialInstancePersistenceError(
                    "Material instance numeric value must be finite" +
                    FieldSuffix(fieldName));
            }
            return result;
        }

        std::array<float, 4> ParseVectorComponents(
            MaterialInstanceNumericType type,
            const nlohmann::json& value,
            std::string_view fieldName)
        {
            const std::size_t componentCount = ComponentCount(type);
            if (!value.is_array() || value.size() != componentCount)
            {
                throw MaterialInstancePersistenceError(
                    "Material instance " + std::string(ToString(type)) +
                    " value must contain exactly " +
                    std::to_string(componentCount) + " components" +
                    FieldSuffix(fieldName));
            }

            std::array<float, 4> result{};
            for (std::size_t index = 0; index < componentCount; ++index)
            {
                result[index] = ParseFiniteFloat(value.at(index), fieldName);
            }
            return result;
        }

        void RequireObject(const nlohmann::json& value, std::string_view fieldName)
        {
            if (!value.is_object())
            {
                throw MaterialInstancePersistenceError(
                    std::string(fieldName) + " must be an object");
            }
        }

        const nlohmann::json& OptionalObject(
            const nlohmann::json& root,
            std::string_view fieldName)
        {
            static const nlohmann::json emptyObject = nlohmann::json::object();
            if (!root.contains(fieldName))
            {
                return emptyObject;
            }
            RequireObject(root.at(fieldName), fieldName);
            return root.at(fieldName);
        }

        bool IsKnownMaterialRenderStateField(std::string_view fieldName) noexcept
        {
            return fieldName == "renderMode" ||
                fieldName == "cullMode" ||
                fieldName == "shadingModel";
        }

        bool IsKnownMaterialRenderStateValue(
            std::string_view fieldName,
            std::string_view value) noexcept
        {
            if (fieldName == "renderMode")
            {
                return value == "Opaque" ||
                    value == "OpaqueClip" ||
                    value == "ForwardOpaque" ||
                    value == "ForwardEyeInner" ||
                    value == "ForwardEyeCornea" ||
                    value == "TransparentAlphaBlend" ||
                    value == "TransparentAlphaBlendWriteDepth" ||
                    value == "TransparentAdditive" ||
                    value == "ThinTranslucent";
            }
            if (fieldName == "cullMode")
            {
                return value == "Back" || value == "Front" || value == "None";
            }
            if (fieldName == "shadingModel")
            {
                return value == "DefaultLit" ||
                    value == "Unlit" ||
                    value == "Subsurface" ||
                    value == "PreintegratedSkin" ||
                    value == "ClearCoat" ||
                    value == "SubsurfaceProfile" ||
                    value == "TwoSidedFoliage" ||
                    value == "Hair" ||
                    value == "Cloth" ||
                    value == "Eye" ||
                    value == "ThinTranslucent";
            }
            return false;
        }

        void ValidateMaterialRenderStateValue(
            std::string_view fieldName,
            const nlohmann::json& value,
            std::string_view context)
        {
            if (!IsKnownMaterialRenderStateField(fieldName))
            {
                throw MaterialInstancePersistenceError(
                    "Unknown material render state: " + std::string(fieldName));
            }
            if (!value.is_string() || value.get<std::string>().empty())
            {
                throw MaterialInstancePersistenceError(
                    "Material render state must be a non-empty string" +
                    FieldSuffix(context));
            }
            if (!IsKnownMaterialRenderStateValue(
                    fieldName,
                    value.get<std::string>()))
            {
                throw MaterialInstancePersistenceError(
                    "Unsupported material render state value '" +
                    value.get<std::string>() + "' for '" +
                    std::string(fieldName) + "'");
            }
        }

        void ValidateTexturePath(std::string_view path, std::string_view fieldName)
        {
            if (!IsMaterialInstanceTextureAssetPath(path))
            {
                throw MaterialInstancePersistenceError(
                    "Texture override must reference a T_*.json asset" +
                    FieldSuffix(fieldName));
            }
        }

        void ValidateMaterialInstanceRoot(const nlohmann::json& root)
        {
            RequireObject(root, "Material instance JSON");
            if (root.contains("parameters"))
            {
                RequireObject(root.at("parameters"), "parameters");
            }
            if (root.contains("textures"))
            {
                RequireObject(root.at("textures"), "textures");
            }
            if (root.contains("renderStateOverrides"))
            {
                RequireObject(
                    root.at("renderStateOverrides"),
                    "renderStateOverrides");
            }
        }

        void SetSparseParameters(
            nlohmann::json& root,
            const MaterialInstanceParameterMap& parameters)
        {
            if (parameters.empty())
            {
                root.erase("parameters");
                return;
            }

            nlohmann::json serialized = nlohmann::json::object();
            for (const auto& [name, value] : parameters)
            {
                serialized[name] = SerializeMaterialInstanceNumericValue(value);
            }
            root["parameters"] = std::move(serialized);
        }

        void SetSparseTextures(
            nlohmann::json& root,
            const MaterialInstanceTextureMap& textures)
        {
            if (textures.empty())
            {
                root.erase("textures");
                return;
            }

            nlohmann::json serialized = nlohmann::json::object();
            for (const auto& [slot, path] : textures)
            {
                ValidateTexturePath(path, "textures." + slot);
                serialized[slot] = path;
            }
            root["textures"] = std::move(serialized);
        }

        void SetSparseRenderStates(
            nlohmann::json& root,
            const MaterialInstanceRenderStateMap& renderStates)
        {
            if (renderStates.empty())
            {
                root.erase("renderStateOverrides");
                return;
            }

            nlohmann::json serialized = nlohmann::json::object();
            for (const auto& [name, value] : renderStates)
            {
                ValidateMaterialRenderStateValue(
                    name,
                    nlohmann::json(value),
                    "renderStateOverrides." + name);
                serialized[name] = value;
            }
            root["renderStateOverrides"] = std::move(serialized);
        }

        nlohmann::json SerializeFloat(float value)
        {
            std::array<char, 32> buffer{};
            const auto conversion = std::to_chars(
                buffer.data(),
                buffer.data() + buffer.size(),
                value);
            if (conversion.ec != std::errc{})
            {
                throw MaterialInstancePersistenceError(
                    "Failed to serialize material instance float value");
            }

            // 使用能往返到同一 float 的最短十进制文本，避免把 0.4f 展开成二进制尾数。
            return nlohmann::json::parse(
                std::string(buffer.data(), conversion.ptr));
        }
    }

    MaterialInstanceNumericType GetMaterialInstanceNumericType(
        const MaterialInstanceNumericValue& value) noexcept
    {
        switch (value.index())
        {
        case 0:
            return MaterialInstanceNumericType::Float;
        case 1:
            return MaterialInstanceNumericType::Vec2;
        case 2:
            return MaterialInstanceNumericType::Vec3;
        case 3:
            return MaterialInstanceNumericType::Vec4;
        default:
            return MaterialInstanceNumericType::Float;
        }
    }

    std::string_view ToString(MaterialInstanceNumericType type) noexcept
    {
        switch (type)
        {
        case MaterialInstanceNumericType::Float:
            return "float";
        case MaterialInstanceNumericType::Vec2:
            return "vec2";
        case MaterialInstanceNumericType::Vec3:
            return "vec3";
        case MaterialInstanceNumericType::Vec4:
            return "vec4";
        }
        return "unknown";
    }

    MaterialInstanceNumericType ParseMaterialInstanceNumericType(
        std::string_view type)
    {
        if (type == "float") return MaterialInstanceNumericType::Float;
        if (type == "vec2") return MaterialInstanceNumericType::Vec2;
        if (type == "vec3") return MaterialInstanceNumericType::Vec3;
        if (type == "vec4") return MaterialInstanceNumericType::Vec4;
        throw MaterialInstancePersistenceError(
            "Unsupported material instance parameter type: " + std::string(type));
    }

    MaterialInstanceNumericValue ParseMaterialInstanceNumericValue(
        MaterialInstanceNumericType type,
        const nlohmann::json& value,
        std::string_view fieldName)
    {
        if (type == MaterialInstanceNumericType::Float)
        {
            return ParseFiniteFloat(value, fieldName);
        }

        const std::array<float, 4> components =
            ParseVectorComponents(type, value, fieldName);
        switch (type)
        {
        case MaterialInstanceNumericType::Vec2:
            return std::array<float, 2>{components[0], components[1]};
        case MaterialInstanceNumericType::Vec3:
            return std::array<float, 3>{components[0], components[1], components[2]};
        case MaterialInstanceNumericType::Vec4:
            return components;
        case MaterialInstanceNumericType::Float:
            break;
        }
        throw MaterialInstancePersistenceError("Unsupported material instance numeric type");
    }

    nlohmann::json SerializeMaterialInstanceNumericValue(
        const MaterialInstanceNumericValue& value)
    {
        return std::visit(
            [](const auto& typedValue) -> nlohmann::json
            {
                using ValueType = std::decay_t<decltype(typedValue)>;
                if constexpr (std::is_same_v<ValueType, float>)
                {
                    return SerializeFloat(typedValue);
                }
                else
                {
                    nlohmann::json result = nlohmann::json::array();
                    for (const float component : typedValue)
                    {
                        result.push_back(SerializeFloat(component));
                    }
                    return result;
                }
            },
            value);
    }

    bool MaterialInstanceNumericValuesExactlyEqual(
        const MaterialInstanceNumericValue& left,
        const MaterialInstanceNumericValue& right) noexcept
    {
        if (left.index() != right.index())
        {
            return false;
        }
        return std::visit(
            [](const auto& leftValue, const auto& rightValue) noexcept
            {
                using LeftType = std::decay_t<decltype(leftValue)>;
                using RightType = std::decay_t<decltype(rightValue)>;
                if constexpr (std::is_same_v<LeftType, RightType>)
                {
                    return leftValue == rightValue;
                }
                else
                {
                    return false;
                }
            },
            left,
            right);
    }

    bool IsMaterialInstanceTextureAssetPath(std::string_view path) noexcept
    {
        const std::size_t separator = path.find_last_of("/\\");
        const std::string_view fileName =
            separator == std::string_view::npos ? path : path.substr(separator + 1);
        if (fileName.size() <= 7 || fileName.substr(0, 2) != "T_" ||
            fileName.substr(fileName.size() - 5) != ".json")
        {
            return false;
        }
        return fileName.size() > 7;
    }

    MaterialInstanceDefaults ParseMaterialInstanceDefaults(
        const nlohmann::json& materialDefinitionJson)
    {
        RequireObject(materialDefinitionJson, "Material definition JSON");
        const nlohmann::json& parameters =
            OptionalObject(materialDefinitionJson, "parameters");
        const nlohmann::json& textures =
            OptionalObject(materialDefinitionJson, "textures");

        MaterialInstanceDefaults defaults;
        for (const auto& [name, descriptor] : parameters.items())
        {
            RequireObject(descriptor, "parameters." + name);
            if (!descriptor.contains("type") || !descriptor.at("type").is_string())
            {
                throw MaterialInstancePersistenceError(
                    "Material parameter '" + name + "' must declare a string type");
            }
            if (!descriptor.contains("default"))
            {
                throw MaterialInstancePersistenceError(
                    "Material parameter '" + name + "' must declare a default");
            }

            const MaterialInstanceNumericType type =
                ParseMaterialInstanceNumericType(descriptor.at("type").get<std::string>());
            defaults.parameters.emplace(
                name,
                ParseMaterialInstanceNumericValue(
                    type,
                    descriptor.at("default"),
                    "parameters." + name + ".default"));
        }

        for (const auto& [name, descriptor] : textures.items())
        {
            RequireObject(descriptor, "textures." + name);
            if (!descriptor.contains("default") || descriptor.at("default").is_null())
            {
                defaults.textures.emplace(name, std::nullopt);
                continue;
            }
            if (!descriptor.at("default").is_string())
            {
                throw MaterialInstancePersistenceError(
                    "Texture default must be a T_*.json path: textures." + name);
            }
            const std::string path = descriptor.at("default").get<std::string>();
            ValidateTexturePath(path, "textures." + name + ".default");
            defaults.textures.emplace(name, path);
        }

        const nlohmann::json& renderStates =
            OptionalObject(materialDefinitionJson, "renderStates");
        for (const auto& [name, value] : renderStates.items())
        {
            if (name == "shadingModel")
            {
                throw MaterialInstancePersistenceError(
                    "Material shadingModel must be declared at the material root");
            }
            ValidateMaterialRenderStateValue(
                name,
                value,
                "renderStates." + name);
            defaults.renderStates.emplace(name, value.get<std::string>());
        }
        if (defaults.renderStates.find("renderMode") ==
            defaults.renderStates.end())
        {
            defaults.renderStates.emplace("renderMode", "Opaque");
        }
        if (!materialDefinitionJson.contains("shadingModel") ||
            !materialDefinitionJson.at("shadingModel").is_string())
        {
            throw MaterialInstancePersistenceError(
                "Material definition must declare a string shadingModel");
        }
        ValidateMaterialRenderStateValue(
            "shadingModel",
            materialDefinitionJson.at("shadingModel"),
            "shadingModel");
        defaults.renderStates.emplace(
            "shadingModel",
            materialDefinitionJson.at("shadingModel").get<std::string>());
        return defaults;
    }

    MaterialInstanceSparseOverrides ParseMaterialInstanceSparseOverrides(
        const nlohmann::json& materialInstanceJson,
        const MaterialInstanceDefaults& defaults)
    {
        ValidateMaterialInstanceRoot(materialInstanceJson);
        const nlohmann::json& parameters =
            OptionalObject(materialInstanceJson, "parameters");
        const nlohmann::json& textures =
            OptionalObject(materialInstanceJson, "textures");
        const nlohmann::json& renderStateOverrides =
            OptionalObject(materialInstanceJson, "renderStateOverrides");

        MaterialInstanceSparseOverrides overrides;
        overrides.renderStatesManaged =
            materialInstanceJson.contains("renderStateOverrides");
        for (const auto& [name, value] : parameters.items())
        {
            const auto defaultIt = defaults.parameters.find(name);
            if (defaultIt == defaults.parameters.end())
            {
                throw MaterialInstancePersistenceError(
                    "Unknown material parameter override: " + name);
            }
            overrides.parameters.emplace(
                name,
                ParseMaterialInstanceNumericValue(
                    GetMaterialInstanceNumericType(defaultIt->second),
                    value,
                    "parameters." + name));
        }

        for (const auto& [slot, value] : textures.items())
        {
            const auto defaultIt = defaults.textures.find(slot);
            if (defaultIt == defaults.textures.end())
            {
                throw MaterialInstancePersistenceError(
                    "Unknown material texture override: " + slot);
            }
            if (!value.is_string())
            {
                throw MaterialInstancePersistenceError(
                    "Texture override must be a T_*.json path: textures." + slot);
            }
            const std::string path = value.get<std::string>();
            ValidateTexturePath(path, "textures." + slot);
            overrides.textures.emplace(slot, path);
        }
        for (const auto& [name, value] : renderStateOverrides.items())
        {
            ValidateMaterialRenderStateValue(
                name,
                value,
                "renderStateOverrides." + name);
            const auto defaultIt = defaults.renderStates.find(name);
            if (defaultIt == defaults.renderStates.end())
            {
                throw MaterialInstancePersistenceError(
                    "Unknown material render state override: " + name);
            }
            overrides.renderStates.emplace(name, value.get<std::string>());
        }
        return overrides;
    }

    MaterialInstanceSparseCandidate BuildMaterialInstanceSparseCandidate(
        const nlohmann::json& originalMaterialInstanceJson,
        const MaterialInstanceDefaults& defaults,
        const MaterialInstanceSparseOverrides& workingOverrides)
    {
        ValidateMaterialInstanceRoot(originalMaterialInstanceJson);
        MaterialInstanceSparseCandidate candidate;
        candidate.sourceJson = originalMaterialInstanceJson;

        for (const auto& [name, value] : workingOverrides.parameters)
        {
            const auto defaultIt = defaults.parameters.find(name);
            if (defaultIt == defaults.parameters.end())
            {
                throw MaterialInstancePersistenceError(
                    "Unknown material parameter override: " + name);
            }
            if (GetMaterialInstanceNumericType(value) !=
                GetMaterialInstanceNumericType(defaultIt->second))
            {
                throw MaterialInstancePersistenceError(
                    "Material parameter type mismatch: " + name);
            }
            if (!MaterialInstanceNumericValuesExactlyEqual(value, defaultIt->second))
            {
                candidate.overrides.parameters.emplace(name, value);
            }
        }

        for (const auto& [slot, path] : workingOverrides.textures)
        {
            const auto defaultIt = defaults.textures.find(slot);
            if (defaultIt == defaults.textures.end())
            {
                throw MaterialInstancePersistenceError(
                    "Unknown material texture override: " + slot);
            }
            ValidateTexturePath(path, "textures." + slot);
            if (!defaultIt->second.has_value() || path != *defaultIt->second)
            {
                candidate.overrides.textures.emplace(slot, path);
            }
        }
        for (const auto& [name, value] : workingOverrides.renderStates)
        {
            ValidateMaterialRenderStateValue(
                name,
                nlohmann::json(value),
                "renderStateOverrides." + name);
            const auto defaultIt = defaults.renderStates.find(name);
            if (defaultIt == defaults.renderStates.end())
            {
                throw MaterialInstancePersistenceError(
                    "Unknown material render state override: " + name);
            }
            if (value.empty())
            {
                throw MaterialInstancePersistenceError(
                    "Material render state override must be non-empty: " + name);
            }
            if (value != defaultIt->second)
            {
                candidate.overrides.renderStates.emplace(name, value);
            }
        }
        candidate.overrides.renderStatesManaged =
            workingOverrides.renderStatesManaged ||
            !workingOverrides.renderStates.empty();
        return candidate;
    }

    nlohmann::json SerializeMaterialInstanceSparseCandidate(
        const MaterialInstanceSparseCandidate& candidate)
    {
        ValidateMaterialInstanceRoot(candidate.sourceJson);
        nlohmann::json result = candidate.sourceJson;
        SetSparseParameters(result, candidate.overrides.parameters);
        SetSparseTextures(result, candidate.overrides.textures);
        if (candidate.overrides.renderStatesManaged)
        {
            SetSparseRenderStates(result, candidate.overrides.renderStates);
        }
        return result;
    }

    std::string SerializeMaterialInstanceSparseCandidateText(
        const MaterialInstanceSparseCandidate& candidate,
        int indent,
        bool appendFinalNewline)
    {
        std::string result =
            SerializeMaterialInstanceSparseCandidate(candidate).dump(indent);
        if (appendFinalNewline)
        {
            result.push_back('\n');
        }
        return result;
    }
}
