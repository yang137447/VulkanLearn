#include "editor/command/editorCommandJsonCodecInternal.h"
#include "editor/command/assetDocumentFields.h"
#include "editor/command/strictJson.h"

#include <charconv>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <type_traits>
#include <utility>


namespace VL
{
namespace EditorCommandJsonDetail
{

[[noreturn]] void ThrowCodecError(
    EditorErrorCode errorCode,
    std::string_view context,
    std::string_view detail)
{
    std::string message;
    message.reserve(context.size() + detail.size() + 2);
    message.append(context);
    message.append(": ");
    message.append(detail);
    throw EditorCommandJsonCodecError(errorCode, message);
}

void Require(
    bool condition,
    EditorErrorCode errorCode,
    std::string_view context,
    std::string_view detail)
{
    if (!condition)
    {
        ThrowCodecError(errorCode, context, detail);
    }
}

void RequireObject(
    const Json& value,
    std::string_view context,
    EditorErrorCode errorCode)
{
    Require(value.is_object(), errorCode, context, "must be an object");
}

bool ContainsField(
    const Json& object,
    std::string_view field)
{
    return object.find(std::string(field)) != object.end();
}

void RequireAllowedFields(
    const Json& object,
    std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional,
    std::string_view context)
{
    RequireObject(object, context);

    for (const std::string_view field : required)
    {
        Require(
            ContainsField(object, field),
            EditorErrorCode::InvalidPayload,
            context,
            std::string("missing field ") + std::string(field));
    }

    for (const auto& entry : object.items())
    {
        bool known = false;
        for (const std::string_view field : required)
        {
            if (entry.key() == field)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            for (const std::string_view field : optional)
            {
                if (entry.key() == field)
                {
                    known = true;
                    break;
                }
            }
        }
        Require(
            known,
            EditorErrorCode::InvalidPayload,
            context,
            std::string("contains unknown field ") + entry.key());
    }
}

float ReadFiniteFloat(
    const Json& value,
    std::string_view context)
{
    Require(
        value.is_number(),
        EditorErrorCode::InvalidPayload,
        context,
        "must be a number");
    const double doubleValue = value.get<double>();
    const float floatValue = static_cast<float>(doubleValue);
    Require(
        std::isfinite(doubleValue) && std::isfinite(floatValue),
        EditorErrorCode::InvalidPayload,
        context,
        "must be finite and representable as float");
    return floatValue;
}

std::string ReadString(
    const Json& object,
    std::string_view field,
    std::string_view context,
    bool requireNonEmpty)
{
    RequireObject(object, context);
    const auto iterator = object.find(std::string(field));
    Require(
        iterator != object.end() && iterator->is_string(),
        EditorErrorCode::InvalidPayload,
        context,
        std::string(field) + " must be a string");
    const std::string value = iterator->get<std::string>();
    if (requireNonEmpty)
    {
        Require(
            !value.empty(),
            EditorErrorCode::InvalidPayload,
            context,
            std::string(field) + " must not be empty");
    }
    return value;
}

bool ReadBoolean(
    const Json& object,
    std::string_view field,
    std::string_view context)
{
    RequireObject(object, context);
    const auto iterator = object.find(std::string(field));
    Require(
        iterator != object.end() && iterator->is_boolean(),
        EditorErrorCode::InvalidPayload,
        context,
        std::string(field) + " must be a boolean");
    return iterator->get<bool>();
}

std::string NormalizeRelativeAssetPath(
    std::string_view rawPath,
    std::string_view context,
    bool requireTextureAsset)
{
    const EditorErrorCode pathErrorCode = requireTextureAsset
        ? EditorErrorCode::InvalidTextureAssetReference
        : EditorErrorCode::InvalidPayload;
    Require(
        !rawPath.empty(),
        pathErrorCode,
        context,
        "asset path must not be empty");

    std::string portablePath(rawPath);
    for (char& character : portablePath)
    {
        if (character == '\\')
        {
            character = '/';
        }
    }

    const std::filesystem::path path(portablePath);
    Require(
        !path.empty() && !path.is_absolute() && path.root_name().empty() &&
            path.root_directory().empty(),
        pathErrorCode,
        context,
        "asset path must be relative");

    const std::filesystem::path normalizedPath = path.lexically_normal();
    Require(
        !normalizedPath.empty() && normalizedPath != "." &&
            normalizedPath != "..",
        pathErrorCode,
        context,
        "asset path must identify a file");

    for (const auto& component : normalizedPath)
    {
        Require(
            component != "..",
            pathErrorCode,
            context,
            "asset path must not traverse outside the resource root");
    }

    const std::string normalized = normalizedPath.generic_string();
    Require(
        !normalized.empty() && normalized.front() != '/',
        pathErrorCode,
        context,
        "asset path must remain relative");
    std::string extension = normalizedPath.extension().string();
    for (char& character : extension)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    Require(
        extension == ".json",
        pathErrorCode,
        context,
        requireTextureAsset
            ? "texture binding must reference a T_*.json asset"
            : "asset path must use a JSON asset file");

    if (requireTextureAsset)
    {
        const std::string filename = normalizedPath.filename().string();
        Require(
            filename.rfind("T_", 0) == 0,
            EditorErrorCode::InvalidTextureAssetReference,
            context,
            "texture binding must reference a T_*.json asset");
    }
    return normalized;
}

std::string ReadAssetPath(
    const Json& object,
    std::string_view field,
    std::string_view context,
    bool requireTextureAsset)
{
    return NormalizeRelativeAssetPath(
        ReadString(object, field, context),
        context,
        requireTextureAsset);
}

std::string_view RequireWireName(
    std::string_view name,
    std::string_view context)
{
    Require(
        !name.empty(),
        EditorErrorCode::InvalidPayload,
        context,
        "enum has no stable wire name");
    return name;
}

EditorCommandSource DecodeSource(
    std::string_view name)
{
    if (name == "rmlui") return EditorCommandSource::RmlUi;
    if (name == "console") return EditorCommandSource::Console;
    if (name == "ai") return EditorCommandSource::AI;
    if (name == "runtime_test") return EditorCommandSource::RuntimeTest;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "EditorCommandEnvelope.source",
        std::string("unknown lower-case source name '") +
            std::string(name) + "'");
}

EditorCommandStatus DecodeStatus(
    std::string_view name)
{
    if (name == "accepted") return EditorCommandStatus::Accepted;
    if (name == "running") return EditorCommandStatus::Running;
    if (name == "succeeded") return EditorCommandStatus::Succeeded;
    if (name == "rejected") return EditorCommandStatus::Rejected;
    if (name == "failed") return EditorCommandStatus::Failed;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "EditorCommandResult.status",
        std::string("unknown lower-case status name '") +
            std::string(name) + "'");
}

EditorErrorCode DecodeErrorCode(
    std::string_view name)
{
    if (name == "none") return EditorErrorCode::None;
    if (name == "invalid_protocol_version") return EditorErrorCode::InvalidProtocolVersion;
    if (name == "invalid_command_id") return EditorErrorCode::InvalidCommandId;
    if (name == "invalid_command_type") return EditorErrorCode::InvalidCommandType;
    if (name == "invalid_payload") return EditorErrorCode::InvalidPayload;
    if (name == "missing_expected_document_revision") return EditorErrorCode::MissingExpectedDocumentRevision;
    if (name == "stale_document_revision") return EditorErrorCode::StaleDocumentRevision;
    if (name == "duplicate_command_id") return EditorErrorCode::DuplicateCommandId;
    if (name == "result_store_capacity_exceeded") return EditorErrorCode::ResultStoreCapacityExceeded;
    if (name == "asset_not_found") return EditorErrorCode::AssetNotFound;
    if (name == "invalid_asset_type") return EditorErrorCode::InvalidAssetType;
    if (name == "reference_resolution_failed") return EditorErrorCode::ReferenceResolutionFailed;
    if (name == "document_not_open") return EditorErrorCode::DocumentNotOpen;
    if (name == "document_dirty") return EditorErrorCode::DocumentDirty;
    if (name == "unknown_parameter") return EditorErrorCode::UnknownParameter;
    if (name == "parameter_type_mismatch") return EditorErrorCode::ParameterTypeMismatch;
    if (name == "unknown_texture_slot") return EditorErrorCode::UnknownTextureSlot;
    if (name == "invalid_texture_asset_reference") return EditorErrorCode::InvalidTextureAssetReference;
    if (name == "source_changed") return EditorErrorCode::SourceChanged;
    if (name == "validation_failed") return EditorErrorCode::ValidationFailed;
    if (name == "atomic_write_failed") return EditorErrorCode::AtomicWriteFailed;
    if (name == "preview_unavailable") return EditorErrorCode::PreviewUnavailable;
    if (name == "preview_generation_changed") return EditorErrorCode::PreviewGenerationChanged;
    if (name == "preview_prepare_failed") return EditorErrorCode::PreviewPrepareFailed;
    if (name == "preview_commit_failed") return EditorErrorCode::PreviewCommitFailed;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "EditorCommandResult.errorCode",
        std::string("unknown lower-case error name '") +
            std::string(name) + "'");
}

EditorMaterialParameterType DecodeParameterType(
    std::string_view name)
{
    if (name == "float") return EditorMaterialParameterType::Float;
    if (name == "vec2") return EditorMaterialParameterType::Vec2;
    if (name == "vec3") return EditorMaterialParameterType::Vec3;
    if (name == "vec4") return EditorMaterialParameterType::Vec4;
    if (name == "color") return EditorMaterialParameterType::Color;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "material.set_parameter.type",
        std::string("unknown lower-case parameter type '") +
            std::string(name) + "'");
}

EditorDirtyDocumentPolicy DecodeDirtyPolicy(
    std::string_view name)
{
    if (name == "require_clean") return EditorDirtyDocumentPolicy::RequireClean;
    if (name == "discard_changes") return EditorDirtyDocumentPolicy::DiscardChanges;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "dirtyPolicy",
        std::string("unknown lower-case dirty policy '") +
            std::string(name) + "'");
}

EditorResetScope DecodeResetScope(
    std::string_view name)
{
    if (name == "parameters") return EditorResetScope::Parameters;
    if (name == "textures") return EditorResetScope::Textures;
    if (name == "render_states") return EditorResetScope::RenderStates;
    if (name == "all") return EditorResetScope::All;
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        "material.reset_overrides.scope",
        std::string("unknown lower-case reset scope '") +
            std::string(name) + "'");
}

EditorMaterialRenderStateField DecodeRenderStateField(
    std::string_view name,
    std::string_view context)
{
    try
    {
        return ParseEditorMaterialRenderStateField(name);
    }
    catch (const std::exception&)
    {
        ThrowCodecError(
            EditorErrorCode::InvalidPayload,
            context,
            std::string("unknown render state field '") + std::string(name) + "'");
    }
}

EditorMaterialRenderStateValue DecodeRenderStateValue(
    EditorMaterialRenderStateField field,
    std::string_view name,
    std::string_view context)
{
    try
    {
        switch (field)
        {
        case EditorMaterialRenderStateField::RenderMode:
            return ParseEditorMaterialRenderMode(name);
        case EditorMaterialRenderStateField::CullMode:
            return ParseEditorMaterialCullMode(name);
        case EditorMaterialRenderStateField::ShadingModel:
            return ParseEditorMaterialShadingModel(name);
        }
    }
    catch (const std::exception&)
    {
    }
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        context,
        std::string("invalid value '") + std::string(name) + "' for render state field");
}

EditorCommandType DecodeCommandType(
    std::string_view name)
{
    const EditorCommandSpec* spec = FindEditorCommandSpec(name);
    Require(
        spec != nullptr,
        EditorErrorCode::InvalidCommandType,
        "EditorCommandEnvelope.command",
        std::string("unknown lower-case command name '") +
            std::string(name) + "'");
    return spec->type;
}

uint32_t DecodeSectionSelector(
    std::string_view selector,
    std::string_view context)
{
    Require(
        !selector.empty(),
        EditorErrorCode::InvalidPayload,
        context,
        "selector must not be empty");

    uint32_t section = 0;
    const auto conversion = std::from_chars(
        selector.data(),
        selector.data() + selector.size(),
        section,
        10);
    Require(
        conversion.ec == std::errc() && conversion.ptr == selector.data() + selector.size(),
        EditorErrorCode::InvalidPayload,
        context,
        "selector must be a decimal mesh section for this typed payload");
    return section;
}

Json EncodeParameterValue(
    const EditorMaterialParameterValue& value,
    std::string_view context)
{
    Json result = Json::array();
    std::visit(
        [&result, context](const auto& item) {
            using ValueType = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<ValueType, float>)
            {
                Require(
                    std::isfinite(item),
                    EditorErrorCode::InvalidPayload,
                    context,
                    "value must be finite");
                result.push_back(item);
            }
            else if constexpr (std::is_same_v<ValueType, EditorColor>)
            {
                for (const float component : item.components)
                {
                    Require(
                        std::isfinite(component),
                        EditorErrorCode::InvalidPayload,
                        context,
                        "value components must be finite");
                    result.push_back(component);
                }
            }
            else
            {
                for (const float component : item)
                {
                    Require(
                        std::isfinite(component),
                        EditorErrorCode::InvalidPayload,
                        context,
                        "value components must be finite");
                    result.push_back(component);
                }
            }
        },
        value);
    return result;
}

EditorMaterialParameterValue DecodeParameterValue(
    const Json& value,
    EditorMaterialParameterType type,
    std::string_view context)
{
    Require(
        value.is_array(),
        EditorErrorCode::InvalidPayload,
        context,
        "value must be an array");

    const std::size_t expectedCount =
        type == EditorMaterialParameterType::Float
            ? 1
            : type == EditorMaterialParameterType::Vec2
                ? 2
                : type == EditorMaterialParameterType::Vec3 ? 3 : 4;
    Require(
        value.size() == expectedCount,
        EditorErrorCode::InvalidPayload,
        context,
        "value array has the wrong vector arity");

    std::array<float, 4> components{};
    for (std::size_t index = 0; index < expectedCount; ++index)
    {
        components[index] = ReadFiniteFloat(
            value.at(index),
            std::string(context) + "[" + std::to_string(index) + "]");
    }

    switch (type)
    {
    case EditorMaterialParameterType::Float:
        return components[0];
    case EditorMaterialParameterType::Vec2:
        return EditorVec2{components[0], components[1]};
    case EditorMaterialParameterType::Vec3:
        return EditorVec3{components[0], components[1], components[2]};
    case EditorMaterialParameterType::Vec4:
        return EditorVec4{
            components[0],
            components[1],
            components[2],
            components[3]};
    case EditorMaterialParameterType::Color:
        return EditorColor{{
            components[0],
            components[1],
            components[2],
            components[3]}};
    }
    ThrowCodecError(
        EditorErrorCode::InvalidPayload,
        context,
        "unsupported parameter type");
}

Json EncodeNavigationOrigin(
    const EditorNavigationOrigin& origin)
{
    Require(
        !origin.sceneIdentity.empty() && !origin.objectIdentity.empty(),
        EditorErrorCode::InvalidPayload,
        "navigationOrigin",
        "scene and object identities must not be empty");

    Json result = {
        {"scenePath", NormalizeRelativeAssetPath(
                           origin.sceneIdentity,
                           "navigationOrigin.scenePath")},
        {"objectId", origin.objectIdentity}};
    if (origin.section.has_value())
    {
        result["section"] = *origin.section;
    }
    if (!origin.slot.empty())
    {
        result["slot"] = origin.slot;
    }
    return result;
}

EditorNavigationOrigin DecodeNavigationOrigin(
    const Json& wire)
{
    RequireAllowedFields(
        wire,
        {"scenePath", "objectId"},
        {"section", "slot"},
        "navigationOrigin");

    EditorNavigationOrigin origin;
    origin.sceneIdentity = ReadAssetPath(
        wire,
        "scenePath",
        "navigationOrigin");
    origin.objectIdentity = ReadString(
        wire,
        "objectId",
        "navigationOrigin");
    if (ContainsField(wire, "section"))
    {
        origin.section = ReadUnsigned<uint32_t>(
            wire,
            "section",
            "navigationOrigin");
    }
    if (ContainsField(wire, "slot"))
    {
        origin.slot = ReadString(wire, "slot", "navigationOrigin");
    }
    return origin;
}


} // namespace EditorCommandJsonDetail
} // namespace VL
