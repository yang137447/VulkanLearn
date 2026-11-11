#include "editor/service/material/materialInstanceDocumentValidation.h"

#include "editor/service/materialInstanceDocumentService.h"
#include "material/validation/materialAssetValidator.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace VL::Editor::MaterialDocumentDetail
{

void ValidateEditorParameterRange(
    const nlohmann::json& descriptor,
    const EditorMaterialParameterValue& value,
    std::string_view parameterName)
{
    const nlohmann::json serialized =
        Persistence::SerializeMaterialInstanceNumericValue(
            std::visit(
                [](const auto& typedValue)
                    -> Persistence::MaterialInstanceNumericValue
                {
                    using ValueType = std::decay_t<decltype(typedValue)>;
                    if constexpr (std::is_same_v<ValueType, EditorColor>)
                    {
                        return Persistence::MaterialInstanceColor{
                            typedValue.components};
                    }
                    else
                    {
                        return typedValue;
                    }
                },
                value));

    const auto readComponent = [&serialized](std::size_t index)
        -> std::optional<float>
    {
        if (serialized.is_number())
            return index == 0 ? std::optional<float>(serialized.get<float>())
                              : std::nullopt;
        if (!serialized.is_array() || index >= serialized.size())
            return std::nullopt;
        return serialized.at(index).get<float>();
    };

    if (descriptor.contains("range") && descriptor.at("range").is_object() &&
        descriptor.at("range").contains("min") &&
        descriptor.at("range").contains("max"))
    {
        const nlohmann::json& range = descriptor.at("range");
        const float minValue = range.at("min").get<float>();
        const float maxValue = range.at("max").get<float>();
        if (!std::isfinite(minValue) || !std::isfinite(maxValue) ||
            minValue > maxValue)
        {
            ThrowServiceError(
                EditorErrorCode::ValidationFailed,
                "parameter range is invalid: " + std::string(parameterName));
        }
        const std::size_t componentCount = serialized.is_array() ? serialized.size() : 1;
        for (std::size_t index = 0; index < componentCount; ++index)
        {
            const std::optional<float> component = readComponent(index);
            const float componentValue = component.value_or(0.0f);
            if (!component.has_value() || !std::isfinite(componentValue) || componentValue < minValue ||
                componentValue > maxValue)
            {
                ThrowServiceError(
                    EditorErrorCode::ValidationFailed,
                    "parameter is outside its declared range: " +
                        std::string(parameterName) + "[" +
                        std::to_string(index) + "]");
            }
        }
    }

    if (!descriptor.contains("channels") ||
        !descriptor.at("channels").is_object())
    {
        return;
    }
    static constexpr std::array<std::string_view, 4> componentNames = {
        "x", "y", "z", "w"};
    const nlohmann::json& channels = descriptor.at("channels");
    for (std::size_t index = 0; index < componentNames.size(); ++index)
    {
        const std::string component(componentNames[index]);
        if (!channels.contains(component) ||
            !channels.at(component).is_object() ||
            !channels.at(component).contains("range"))
        {
            continue;
        }
        const nlohmann::json& range = channels.at(component).at("range");
        if (!range.is_object() || !range.contains("min") ||
            !range.contains("max") || !serialized.is_array() ||
            index >= serialized.size())
        {
            continue;
        }
        const std::optional<float> componentValue = readComponent(index);
        if (!componentValue.has_value())
            continue;
        const float minValue = range.at("min").get<float>();
        const float maxValue = range.at("max").get<float>();
        if (!std::isfinite(componentValue.value()) ||
            componentValue.value() < minValue || componentValue.value() > maxValue)
        {
            ThrowServiceError(
                EditorErrorCode::ValidationFailed,
                "parameter is outside its declared channel range: " +
                    std::string(parameterName) + "[" +
                    std::to_string(index) + "]");
        }
    }
}

} // namespace VL::Editor::MaterialDocumentDetail

namespace VL::Editor
{

using MaterialDocumentDetail::MakeManagedMaterialInstanceJson;
using MaterialDocumentDetail::ReadStringField;
using MaterialDocumentDetail::ServiceError;
using MaterialDocumentDetail::ThrowServiceError;
using MaterialDocumentDetail::ValidateEditorParameterRange;

void MaterialInstanceDocumentService::ValidateManagedMaterialInstance(
    const Json& materialJson,
    const Json& materialInstanceJson,
    std::string_view materialInstancePath) const
{
    // 现有 validator 会拒绝未知顶层字段；先只取它负责的字段，保存时仍
    // 使用原始 sourceJson，因此 editor 不会因扩展字段而丢数据。
    const Json managed =
        MakeManagedMaterialInstanceJson(materialInstanceJson);
    try
    {
        MaterialAssetValidator::ValidateInstanceHeader(
            managed, materialInstancePath);
        MaterialAssetValidator::ValidateInstanceOverrides(
            materialJson, managed, materialInstancePath);
    }
    catch (const ServiceError&)
    {
        throw;
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::ValidationFailed,
            exception.what());
    }
}
void MaterialInstanceDocumentService::ValidateTextureReference(
    std::string_view textureAssetPath,
    std::string_view materialInstancePath) const
{
    std::string normalized;
    try
    {
        normalized = NormalizeTextureAssetPath(textureAssetPath);
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::InvalidTextureAssetReference,
            "invalid texture reference in " +
                std::string(materialInstancePath) + ": " +
                exception.what());
    }

    std::filesystem::path texturePath;
    try
    {
        texturePath = ResolveResourceFile(normalized);
    }
    catch (const ServiceError& exception)
    {
        ThrowServiceError(
            EditorErrorCode::InvalidTextureAssetReference,
            exception.what());
    }

    const Json textureJson = ReadJsonFile(texturePath);
    if (!textureJson.is_object() ||
        textureJson.value("type", std::string()) != "texture")
    {
        ThrowServiceError(
            EditorErrorCode::InvalidTextureAssetReference,
            "texture asset has invalid type: " + normalized);
    }
    const std::string source =
        ReadStringField(textureJson, "source", normalized);
    try
    {
        const std::string normalizedSource =
            NormalizeRelativePath(source);
        if (config.validateTextureSources)
        {
            static_cast<void>(ResolveResourceFile(normalizedSource));
        }
    }
    catch (const ServiceError& exception)
    {
        ThrowServiceError(
            EditorErrorCode::InvalidTextureAssetReference,
            "texture source is not available for " + normalized + ": " +
                std::string(exception.what()));
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::InvalidTextureAssetReference,
            "texture source is invalid for " + normalized + ": " +
                exception.what());
    }
}
void MaterialInstanceDocumentService::ValidateWorkingState(
    const Document& document,
    const PersistenceOverrides& workingOverrides) const
{
    const Json candidate = BuildCandidateJson(document, workingOverrides);
    ValidateManagedMaterialInstance(
        document.materialJson,
        candidate,
        document.assetPath);

    try
    {
        // candidate parser 复用持久化层的有限数值、类型和 override key 门禁。
        static_cast<void>(Persistence::ParseMaterialInstanceSparseOverrides(
            candidate,
            document.defaults));
        MaterialAssetValidator::ValidateInstanceOverrides(
            document.materialJson,
            MakeManagedMaterialInstanceJson(candidate),
            document.assetPath);
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::ValidationFailed,
            exception.what());
    }

    const Json effectiveParameters =
        BuildEffectiveParameters(document, workingOverrides);
    for (const auto& [name, descriptor] :
         document.materialJson.at("parameters").items())
    {
        if (!descriptor.contains("channels"))
        {
            continue;
        }
        const Json& channels = descriptor.at("channels");
        if (!channels.is_object())
        {
            ThrowServiceError(
                EditorErrorCode::ValidationFailed,
                "parameter channels must be an object: " + name);
        }
        const Json& value = effectiveParameters.at(name);
        static constexpr std::array<std::string_view, 4> componentNames = {
            "x", "y", "z", "w"};
        for (std::size_t index = 0; index < componentNames.size(); ++index)
        {
            const std::string component(componentNames[index]);
            if (!channels.contains(component))
            {
                continue;
            }
            const Json& channel = channels.at(component);
            if (!channel.is_object() || !channel.contains("range"))
            {
                continue;
            }
            const Json& range = channel.at("range");
            if (!range.is_object() || !range.contains("min") ||
                !range.contains("max") || !value.is_array() ||
                index >= value.size())
            {
                continue;
            }
            const float componentValue = value.at(index).get<float>();
            const float minValue = range.at("min").get<float>();
            const float maxValue = range.at("max").get<float>();
            if (!std::isfinite(componentValue) ||
                componentValue < minValue || componentValue > maxValue)
            {
                ThrowServiceError(
                    EditorErrorCode::ValidationFailed,
                    "parameter is outside its declared channel range: " +
                        name + "[" + std::to_string(index) + "]");
            }
        }
    }

    const Json effectiveTextures =
        BuildEffectiveTextures(document, workingOverrides);
    for (const auto& [slot, value] : effectiveTextures.items())
    {
        if (!value.is_string())
        {
            ThrowServiceError(
                EditorErrorCode::InvalidTextureAssetReference,
                "effective texture binding is not a path: " + slot);
        }
        ValidateTextureReference(
            value.get<std::string>(),
            document.assetPath);
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ValidateMaterialInstanceDocument(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        try
        {
            ValidateWorkingState(document, document.workingOverrides);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "material instance draft is valid";
        return MakeSnapshotResult(
            MakeSuccess("material instance document validated", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

} // namespace VL::Editor
