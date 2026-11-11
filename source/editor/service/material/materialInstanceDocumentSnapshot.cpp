#include "editor/service/material/materialInstanceDocumentSnapshot.h"

#include "editor/service/materialInstanceDocumentService.h"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace VL::Editor
{
namespace
{

using Json = nlohmann::json;

using MaterialDocumentDetail::ReadStringField;
using MaterialDocumentDetail::ServiceError;
using MaterialDocumentDetail::ThrowServiceError;

std::optional<float> ReadOptionalRangeValue(
    const Json& object,
    std::string_view field)
{
    if (!object.contains(std::string(field)))
    {
        return std::nullopt;
    }
    const Json& value = object.at(std::string(field));
    if (!value.is_number())
    {
        return std::nullopt;
    }
    const float result = value.get<float>();
    if (!std::isfinite(result))
    {
        return std::nullopt;
    }
    return result;
}

std::string AddNewline(std::string text)
{
    if (text.empty() || text.back() != '\n')
    {
        text.push_back('\n');
    }
    return text;
}

} // namespace

std::string MaterialInstanceDocumentService::JsonString(
    const Json& value)
{
    return AddNewline(value.dump(4));
}

MaterialInstanceDocumentService::Json
MaterialInstanceDocumentService::BuildEffectiveParameters(
    const Document& document,
    const PersistenceOverrides& workingOverrides) const
{
    Json result = Json::object();
    for (const auto& [name, defaultValue] : document.defaults.parameters)
    {
        const auto overrideIt = workingOverrides.parameters.find(name);
        result[name] = Persistence::SerializeMaterialInstanceNumericValue(
            overrideIt == workingOverrides.parameters.end()
                ? defaultValue
                : overrideIt->second);
    }
    return result;
}

MaterialInstanceDocumentService::Json
MaterialInstanceDocumentService::BuildEffectiveTextures(
    const Document& document,
    const PersistenceOverrides& workingOverrides) const
{
    Json result = Json::object();
    for (const auto& [slot, defaultValue] : document.defaults.textures)
    {
        const auto overrideIt = workingOverrides.textures.find(slot);
        if (overrideIt != workingOverrides.textures.end())
        {
            result[slot] = overrideIt->second;
        }
        else if (defaultValue.has_value())
        {
            result[slot] = *defaultValue;
        }
    }
    return result;
}

MaterialInstanceDocumentService::Json
MaterialInstanceDocumentService::BuildCandidateJson(
    const Document& document,
    const PersistenceOverrides& workingOverrides) const
{
    const Persistence::MaterialInstanceSparseCandidate candidate =
        Persistence::BuildMaterialInstanceSparseCandidate(
            document.sourceJson,
            document.defaults,
            workingOverrides);
    const Json serialized =
        Persistence::SerializeMaterialInstanceSparseCandidate(candidate);
    // service 持有原始 root；候选只允许重写这两个 editor-managed map，
    // 以保留宏、渲染状态和 opaque 扩展字段。
    Json result = document.sourceJson;
    static constexpr std::array<std::string_view, 3> managedFields = {
        "parameters", "textures", "renderStateOverrides"};
    for (const std::string_view field : managedFields)
    {
        const std::string key(field);
        if (serialized.contains(key))
        {
            result[key] = serialized.at(key);
        }
        else
        {
            result.erase(key);
        }
    }
    return result;
}

MaterialEditorDocumentSnapshot
MaterialInstanceDocumentService::MakeDocumentSnapshot(
    Document& document) const
{
    MaterialEditorDocumentSnapshot result;
    result.assetPath = document.assetPath;
    result.baseMaterialPath = document.baseMaterialPath;
    result.schemaDigest = document.schemaDigest.ToHex();
    result.sourceDigest = document.sourceDigest.ToHex();
    result.revision = document.revision;
    result.state = document.state;
    result.validation = document.validation;
    result.validationMessage = document.validationMessage;
    result.references = document.references;

    // 用稳定顺序展示影响 shader variant 与 pipeline 的三个 MI 状态，避免
    // map 的字典序把检查器布局变成 cullMode/renderMode/shadingModel。
    static constexpr std::array<std::string_view, 3> renderStateNames = {
        "renderMode", "shadingModel", "cullMode"};
    for (const std::string_view stateName : renderStateNames)
    {
        const std::string name(stateName);
        const auto defaultIt = document.defaults.renderStates.find(name);
        if (defaultIt == document.defaults.renderStates.end())
        {
            continue;
        }

        MaterialEditorRenderStateSnapshot state;
        state.name = name;
        state.defaultValue = defaultIt->second;
        const auto workingIt = document.workingOverrides.renderStates.find(name);
        state.effectiveValue = workingIt ==
                document.workingOverrides.renderStates.end()
            ? defaultIt->second
            : workingIt->second;
        if (workingIt != document.workingOverrides.renderStates.end())
        {
            state.overrideValue = workingIt->second;
        }
        result.renderStates.push_back(std::move(state));
    }

    for (const auto& [name, defaultValue] : document.defaults.parameters)
    {
        MaterialEditorParameterSnapshot parameter;
        parameter.name = name;
        const Json& descriptor = document.materialJson.at("parameters").at(name);
        parameter.description = descriptor.value(
            "description", std::string());
        parameter.type = ToEditorParameterType(
            Persistence::GetMaterialInstanceNumericType(defaultValue));
        parameter.defaultValue = ToEditorValue(defaultValue);
        const auto workingIt = document.workingOverrides.parameters.find(name);
        parameter.effectiveValue = ToEditorValue(
            workingIt == document.workingOverrides.parameters.end()
                ? defaultValue
                : workingIt->second);
        if (workingIt != document.workingOverrides.parameters.end())
        {
            parameter.overrideValue = ToEditorValue(workingIt->second);
        }
        if (descriptor.contains("range") && descriptor.at("range").is_object())
        {
            parameter.min = ReadOptionalRangeValue(
                descriptor.at("range"), "min");
            parameter.max = ReadOptionalRangeValue(
                descriptor.at("range"), "max");
        }
        if (descriptor.contains("channels") &&
            descriptor.at("channels").is_object())
        {
            static constexpr std::array<std::string_view, 4> componentNames = {
                "x", "y", "z", "w"};
            const Json& channels = descriptor.at("channels");
            for (const std::string_view componentName : componentNames)
            {
                const std::string key(componentName);
                if (!channels.contains(key) || !channels.at(key).is_object())
                {
                    continue;
                }
                const Json& channel = channels.at(key);
                MaterialEditorParameterChannelSnapshot channelSnapshot;
                channelSnapshot.name = channel.value(
                    "name", key);
                channelSnapshot.description = channel.value(
                    "description", std::string());
                if (channel.contains("range") &&
                    channel.at("range").is_object())
                {
                    channelSnapshot.min = ReadOptionalRangeValue(
                        channel.at("range"), "min");
                    channelSnapshot.max = ReadOptionalRangeValue(
                        channel.at("range"), "max");
                }
                parameter.channels.push_back(std::move(channelSnapshot));
            }
        }
        result.parameters.push_back(std::move(parameter));
    }

    for (const auto& [slot, defaultValue] : document.defaults.textures)
    {
        MaterialEditorTextureBindingSnapshot texture;
        texture.slotName = slot;
        const Json& descriptor = document.materialJson.at("textures").at(slot);
        texture.description = descriptor.value(
            "description", std::string());
        if (defaultValue.has_value())
        {
            texture.defaultAssetPath = *defaultValue;
        }
        const auto workingIt = document.workingOverrides.textures.find(slot);
        if (workingIt != document.workingOverrides.textures.end())
        {
            texture.effectiveAssetPath = workingIt->second;
            texture.overrideAssetPath = workingIt->second;
        }
        else if (defaultValue.has_value())
        {
            texture.effectiveAssetPath = *defaultValue;
        }
        result.textures.push_back(std::move(texture));
    }

    try
    {
        result.serializedWorkingDraft = JsonString(
            BuildCandidateJson(document, document.workingOverrides));
    }
    catch (const std::exception&)
    {
        // 候选无效时退回原始 root；只读 fallback 不触碰 dirty/baseline。
        result.serializedWorkingDraft = JsonString(document.sourceJson);
    }

    try
    {
        result.serializedBaselineDraft = JsonString(
            BuildCandidateJson(document, document.baselineOverrides));
    }
    catch (const std::exception&)
    {
        // baseline fallback 也只读原始 root，不能把 working 草稿冒充基线。
        result.serializedBaselineDraft = JsonString(document.sourceJson);
    }
    return result;
}

std::optional<MaterialEditorDocumentSnapshot>
MaterialInstanceDocumentService::BuildDocumentSnapshot(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        return MakeDocumentSnapshot(document);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

} // namespace VL::Editor
