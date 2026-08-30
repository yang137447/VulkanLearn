#include "materialInstanceEditorContract.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include "editor/persistence/materialInstancePersistence.h"
#include "editor/persistence/materialInstanceSparseCandidate.h"
#include "shader/build/atomicFile.h"

namespace material_instance_editor_test
{
namespace
{

using Json = nlohmann::json;

void Require(
    bool condition,
    std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

void RequireObject(
    const Json& value,
    std::string_view context)
{
    Require(value.is_object(), std::string(context) + " must be an object");
}

void RequireString(
    const Json& object,
    std::string_view field,
    std::string_view context)
{
    Require(
        object.contains(std::string(field)) &&
            object.at(std::string(field)).is_string(),
        std::string(context) + " requires string field " + std::string(field));
}

uint64_t ReadUInt64(
    const Json& object,
    std::string_view field,
    std::string_view context)
{
    Require(
        object.contains(std::string(field)) &&
            (object.at(std::string(field)).is_number_integer() ||
             object.at(std::string(field)).is_number_unsigned()),
        std::string(context) + " requires unsigned integer field " +
            std::string(field));

    const Json& valueJson = object.at(std::string(field));
    if (valueJson.is_number_unsigned())
    {
        return valueJson.get<uint64_t>();
    }

    const int64_t value = valueJson.get<int64_t>();
    Require(
        value >= 0,
        std::string(context) + " field " + std::string(field) +
            " must not be negative");
    return static_cast<uint64_t>(value);
}

bool ContainsString(
    const Json& array,
    std::string_view value)
{
    if (!array.is_array())
    {
        return false;
    }
    for (const Json& element : array)
    {
        if (element.is_string() && element.get<std::string>() == value)
        {
            return true;
        }
    }
    return false;
}

void RequireAllowedFields(
    const Json& object,
    const std::vector<std::string>& required,
    const std::vector<std::string>& optional,
    std::string_view context)
{
    for (const std::string& field : required)
    {
        Require(
            object.contains(field),
            std::string(context) + " is missing field " + field);
    }

    for (const auto& [field, value] : object.items())
    {
        (void)value;
        const bool known =
            std::find(required.begin(), required.end(), field) != required.end() ||
            std::find(optional.begin(), optional.end(), field) != optional.end();
        Require(
            known,
            std::string(context) + " contains unknown field " + field);
    }
}

std::vector<std::string> ReadStringArray(
    const Json& object,
    std::string_view field,
    std::string_view context)
{
    Require(
        object.contains(std::string(field)) &&
            object.at(std::string(field)).is_array(),
        std::string(context) + " requires string array " + std::string(field));

    std::vector<std::string> values;
    for (const Json& value : object.at(std::string(field)))
    {
        Require(
            value.is_string(),
            std::string(context) + " contains a non-string in " +
                std::string(field));
        values.push_back(value.get<std::string>());
    }
    return values;
}

std::vector<std::string> ReadFieldList(
    const Json& commandSchema,
    std::string_view field,
    std::string_view command)
{
    if (!commandSchema.contains(std::string(field)))
    {
        return {};
    }
    return ReadStringArray(commandSchema, field, command);
}

std::string ReadRequiredString(
    const Json& object,
    std::string_view field,
    std::string_view context)
{
    RequireString(object, field, context);
    const std::string value = object.at(std::string(field)).get<std::string>();
    Require(
        !value.empty(),
        std::string(context) + " field " + std::string(field) +
            " must not be empty");
    return value;
}

std::string NormalizePathWithoutAssetPolicy(std::string_view path)
{
    std::string portable(path);
    std::replace(portable.begin(), portable.end(), '\\', '/');
    const std::filesystem::path normalizedPath =
        std::filesystem::path(portable).lexically_normal();
    const std::string normalized = normalizedPath.generic_string();
    Require(
        !normalized.empty() && normalized != "." &&
            normalized != ".." && normalized.rfind("../", 0) != 0 &&
            !normalizedPath.is_absolute(),
        "asset path must be relative and remain inside the resource root");
    return normalized;
}

const Json& FindCommandSchema(
    const Json& schema,
    std::string_view command)
{
    Require(
        schema.contains("commands") && schema.at("commands").is_object(),
        "editor command schema requires commands object");
    const auto& commands = schema.at("commands");
    Require(
        commands.contains(std::string(command)),
        "editor command schema does not define command " + std::string(command));
    return commands.at(std::string(command));
}

void ValidateParameterValue(
    const Json& value,
    std::string_view type,
    std::string_view context)
{
    static const std::map<std::string, size_t> componentCounts = {
        {"float", 1},
        {"vec2", 2},
        {"vec3", 3},
        {"vec4", 4}};
    const auto countIt = componentCounts.find(std::string(type));
    Require(
        countIt != componentCounts.end(),
        std::string(context) + " has unsupported parameter type");
    Require(
        value.is_array() && value.size() == countIt->second,
        std::string(context) + " requires a complete typed value array");
    for (const Json& component : value)
    {
        Require(
            component.is_number() &&
                std::isfinite(component.get<double>()),
            std::string(context) + " contains a non-finite component");
    }
}

void ValidatePayloadPaths(
    Json& payload,
    std::string_view command)
{
    if (payload.contains("assetPath"))
    {
        RequireString(payload, "assetPath", command);
        payload["assetPath"] = NormalizeRelativeAssetPath(
            payload.at("assetPath").get<std::string>());
    }
    if (payload.contains("scenePath"))
    {
        RequireString(payload, "scenePath", command);
        payload["scenePath"] = NormalizeRelativeAssetPath(
            payload.at("scenePath").get<std::string>());
    }
    if (payload.contains("textureAssetPath"))
    {
        RequireString(payload, "textureAssetPath", command);
        const std::string normalizedTexturePath = NormalizeRelativeAssetPath(
            payload.at("textureAssetPath").get<std::string>());
        const std::filesystem::path texturePath(normalizedTexturePath);
        Require(
            texturePath.extension() == ".json" &&
                texturePath.filename().string().rfind("T_", 0) == 0,
            std::string(command) +
                " accepts only normalized T_*.json texture assets");
        payload["textureAssetPath"] = normalizedTexturePath;
    }
}

void ValidateCommandSpecificPayload(
    Json& payload,
    std::string_view command)
{
    ValidatePayloadPaths(payload, command);

    if (command == "material.set_parameter")
    {
        RequireString(payload, "type", command);
        RequireString(payload, "parameter", command);
        const std::string type = payload.at("type").get<std::string>();
        ValidateParameterValue(
            payload.at("value"),
            type,
            "material.set_parameter.value");
        Require(
            !payload.at("parameter").get<std::string>().empty(),
            "material.set_parameter.parameter must not be empty");
    }
    else if (command == "material.preview.apply")
    {
        ReadUInt64(payload, "documentRevision", command);
    }
    else if (command == "editor.get_command_result")
    {
        ReadUInt64(payload, "commandId", command);
    }

    if (payload.contains("navigationOrigin"))
    {
        Require(
            payload.at("navigationOrigin").is_object(),
            "navigationOrigin must be an object");
    }
}

void ValidateEnvelopeTopLevel(
    const Json& wire)
{
    static const std::vector<std::string> required = {
        "protocolVersion", "commandId", "source", "command", "payload"};
    static const std::vector<std::string> optional = {
        "correlationId", "expectedDocumentRevision"};
    RequireAllowedFields(wire, required, optional, "EditorCommandEnvelope");
}

void ValidateResultTopLevel(
    const Json& wire)
{
    static const std::vector<std::string> required = {
        "protocolVersion", "commandId", "status", "errorCode", "message", "payload"};
    static const std::vector<std::string> optional = {"documentRevision"};
    RequireAllowedFields(wire, required, optional, "EditorCommandResult");
}

} // namespace

std::string NormalizeRelativeAssetPath(std::string_view path)
{
    const std::string normalized = NormalizePathWithoutAssetPolicy(path);
    const std::filesystem::path normalizedPath(normalized);
    Require(
        normalizedPath.extension() == ".json",
        "editor asset path must use a JSON asset file");
    return normalized;
}

CommandEnvelope DecodeCommandEnvelope(
    const Json& wire,
    const Json& schema)
{
    RequireObject(wire, "EditorCommandEnvelope");
    ValidateEnvelopeTopLevel(wire);

    CommandEnvelope envelope;
    envelope.protocolVersion = static_cast<uint32_t>(
        ReadUInt64(wire, "protocolVersion", "EditorCommandEnvelope"));
    Require(
        envelope.protocolVersion == schema.at("protocolVersion").get<uint32_t>(),
        "unsupported editor command protocol version");
    envelope.commandId = ReadUInt64(wire, "commandId", "EditorCommandEnvelope");
    Require(envelope.commandId != 0, "commandId must be non-zero");
    envelope.source = ReadRequiredString(wire, "source", "EditorCommandEnvelope");
    Require(
        ContainsString(schema.at("sources"), envelope.source),
        "unknown editor command source " + envelope.source);
    envelope.command = ReadRequiredString(wire, "command", "EditorCommandEnvelope");
    const Json& commandSchema = FindCommandSchema(schema, envelope.command);

    if (wire.contains("correlationId"))
    {
        envelope.correlationId = ReadUInt64(
            wire,
            "correlationId",
            "EditorCommandEnvelope");
    }
    if (wire.contains("expectedDocumentRevision"))
    {
        envelope.expectedDocumentRevision = ReadUInt64(
            wire,
            "expectedDocumentRevision",
            "EditorCommandEnvelope");
    }

    const bool requiresExpectedRevision = commandSchema.value(
        "requiresExpectedDocumentRevision",
        false);
    Require(
        !requiresExpectedRevision || envelope.expectedDocumentRevision.has_value(),
        "write command requires expectedDocumentRevision");

    RequireObject(wire.at("payload"), "EditorCommandEnvelope.payload");
    envelope.payload = wire.at("payload");
    const std::vector<std::string> requiredFields = ReadFieldList(
        commandSchema,
        "requiredPayload",
        envelope.command);
    const std::vector<std::string> optionalFields = ReadFieldList(
        commandSchema,
        "optionalPayload",
        envelope.command);
    RequireAllowedFields(
        envelope.payload,
        requiredFields,
        optionalFields,
        envelope.command + ".payload");
    ValidateCommandSpecificPayload(envelope.payload, envelope.command);
    return envelope;
}

Json EncodeCommandEnvelope(const CommandEnvelope& envelope)
{
    Json wire = {
        {"protocolVersion", envelope.protocolVersion},
        {"commandId", envelope.commandId},
        {"source", envelope.source},
        {"command", envelope.command},
        {"payload", envelope.payload}};
    if (envelope.correlationId.has_value())
    {
        wire["correlationId"] = *envelope.correlationId;
    }
    if (envelope.expectedDocumentRevision.has_value())
    {
        wire["expectedDocumentRevision"] = *envelope.expectedDocumentRevision;
    }
    return wire;
}

CommandResult DecodeCommandResult(
    const Json& wire,
    const Json& schema)
{
    RequireObject(wire, "EditorCommandResult");
    ValidateResultTopLevel(wire);

    CommandResult result;
    result.protocolVersion = static_cast<uint32_t>(
        ReadUInt64(wire, "protocolVersion", "EditorCommandResult"));
    Require(
        result.protocolVersion == schema.at("protocolVersion").get<uint32_t>(),
        "unsupported editor result protocol version");
    result.commandId = ReadUInt64(wire, "commandId", "EditorCommandResult");
    Require(result.commandId != 0, "result commandId must be non-zero");
    result.status = ReadRequiredString(wire, "status", "EditorCommandResult");
    Require(
        ContainsString(schema.at("statuses"), result.status),
        "unknown editor command result status " + result.status);
    result.errorCode = ReadRequiredString(wire, "errorCode", "EditorCommandResult");
    Require(
        ContainsString(schema.at("errorCodes"), result.errorCode),
        "unknown editor command error code " + result.errorCode);
    RequireString(wire, "message", "EditorCommandResult");
    result.message = wire.at("message").get<std::string>();
    RequireObject(wire.at("payload"), "EditorCommandResult.payload");
    result.payload = wire.at("payload");
    if (wire.contains("documentRevision"))
    {
        result.documentRevision = ReadUInt64(
            wire,
            "documentRevision",
            "EditorCommandResult");
    }

    const bool finalStatus =
        result.status == "Succeeded" ||
        result.status == "Rejected" ||
        result.status == "Failed";
    if (finalStatus)
    {
        Require(
            result.errorCode == "None" || result.status != "Succeeded",
            "Succeeded result must use errorCode None");
    }
    return result;
}

NavigationReference ResolveNavigationReference(
    const Json& scene,
    std::string_view objectId,
    std::string_view selector)
{
    RequireObject(scene, "navigation scene");
    Require(
        scene.contains("scenePath") && scene.at("scenePath").is_string(),
        "navigation scene requires scenePath");
    Require(
        scene.contains("objects") && scene.at("objects").is_array(),
        "navigation scene requires objects array");

    for (const Json& object : scene.at("objects"))
    {
        if (!object.is_object() ||
            object.value("objectId", std::string()) != objectId)
        {
            continue;
        }

        NavigationReference reference;
        reference.scenePath = NormalizeRelativeAssetPath(
            scene.at("scenePath").get<std::string>());
        reference.objectId = std::string(objectId);
        reference.objectType = object.at("type").get<std::string>();
        reference.assetPath = NormalizeRelativeAssetPath(
            object.at("assetPath").get<std::string>());
        reference.selector = std::string(selector);

        if (reference.objectType == "mesh")
        {
            Require(
                object.contains("sections") && object.at("sections").is_array(),
                "mesh navigation object requires sections");
            for (const Json& section : object.at("sections"))
            {
                const std::string sectionSelector = std::to_string(
                    section.at("index").get<uint32_t>());
                if ((!selector.empty() && selector != sectionSelector) ||
                    (selector.empty() && object.at("sections").size() != 1))
                {
                    continue;
                }
                reference.selector = sectionSelector;
                reference.materialInstancePath = NormalizeRelativeAssetPath(
                    section.at("materialInstancePath").get<std::string>());
                return reference;
            }
        }
        else if (reference.objectType == "terrain")
        {
            Require(
                object.contains("materialSlots") &&
                    object.at("materialSlots").is_array(),
                "terrain navigation object requires materialSlots");
            for (const Json& slot : object.at("materialSlots"))
            {
                const std::string slotName = slot.at("slot").get<std::string>();
                if ((!selector.empty() && selector != slotName) ||
                    (selector.empty() && object.at("materialSlots").size() != 1))
                {
                    continue;
                }
                reference.selector = slotName;
                reference.materialInstancePath = NormalizeRelativeAssetPath(
                    slot.at("materialInstancePath").get<std::string>());
                return reference;
            }
        }
        throw std::runtime_error(
            "navigation selector did not resolve an MI reference for object " +
            std::string(objectId));
    }

    throw std::runtime_error(
        "navigation object was not found: " + std::string(objectId));
}

Json MergeEffectiveValues(
    const Json& defaults,
    const Json& overrides)
{
    RequireObject(defaults, "defaults");
    RequireObject(overrides, "overrides");
    Json effective = defaults;
    for (const auto& [name, value] : overrides.items())
    {
        effective[name] = value;
    }
    return effective;
}

Json BuildSparseCandidate(
    const Json& originalMaterialInstance,
    const Json& materialDefinition,
    const Json& workingParameters,
    const Json& workingTextures)
{
    const VL::Editor::Persistence::MaterialInstanceDefaults defaults =
        VL::Editor::Persistence::ParseMaterialInstanceDefaults(materialDefinition);
    Json workingDocument = {
        {"parameters", workingParameters},
        {"textures", workingTextures}};
    const VL::Editor::Persistence::MaterialInstanceSparseOverrides workingOverrides =
        VL::Editor::Persistence::ParseMaterialInstanceSparseOverrides(
            workingDocument,
            defaults);
    const VL::Editor::Persistence::MaterialInstanceSparseCandidate candidate =
        VL::Editor::Persistence::BuildMaterialInstanceSparseCandidate(
            originalMaterialInstance,
            defaults,
            workingOverrides);
    return VL::Editor::Persistence::SerializeMaterialInstanceSparseCandidate(candidate);
}

void ValidateTextureAssetDocument(
    const std::filesystem::path& fixtureRoot,
    std::string_view textureAssetPath)
{
    const std::string normalizedPath = NormalizeRelativeAssetPath(textureAssetPath);
    const std::filesystem::path logicalPath(normalizedPath);
    Require(
        logicalPath.filename().string().rfind("T_", 0) == 0,
        "MI texture binding must reference a T_*.json asset");
    const std::filesystem::path assetPath = fixtureRoot / normalizedPath;
    Require(
        std::filesystem::is_regular_file(assetPath),
        "texture asset fixture is missing: " + normalizedPath);

    std::ifstream input(assetPath);
    Require(input.is_open(), "texture asset fixture could not be opened");
    Json textureJson;
    input >> textureJson;
    Require(
        textureJson.value("type", std::string()) == "texture",
        "texture reference does not point to a texture asset: " + normalizedPath);
    Require(
        textureJson.contains("source") && textureJson.at("source").is_string() &&
            !textureJson.at("source").get<std::string>().empty(),
        "texture asset must declare a source: " + normalizedPath);

    const std::string sourcePath = NormalizePathWithoutAssetPolicy(
        textureJson.at("source").get<std::string>());
    Require(
        std::filesystem::is_regular_file(fixtureRoot / sourcePath),
        "texture source fixture is missing: " + sourcePath);
}

CommitResult TryCommitCandidate(
    const std::filesystem::path& materialInstancePath,
    const VL::ContentDigest& expectedSourceDigest,
    const Json& candidate)
{
    try
    {
        const VL::Editor::Persistence::MaterialInstanceSaveResult result =
            VL::Editor::Persistence::MaterialInstancePersistence::SaveTextIfUnchanged(
            materialInstancePath,
            expectedSourceDigest,
            candidate.dump(2) + "\n");
        if (result.Succeeded())
        {
            return {true, "none"};
        }
        if (result.status ==
                VL::Editor::Persistence::MaterialInstancePersistenceStatus::Missing ||
            result.status ==
                VL::Editor::Persistence::MaterialInstancePersistenceStatus::NotRegularFile ||
            result.status ==
                VL::Editor::Persistence::MaterialInstancePersistenceStatus::InvalidPath)
        {
            return {false, "asset_not_found"};
        }
        if (result.HasSourceConflict())
        {
            return {false, "source_changed"};
        }
    }
    catch (const std::exception&)
    {
        return {false, "atomic_write_failed"};
    }
    return {false, "atomic_write_failed"};
}

void DocumentContractRegistry::Open(
    std::string_view materialInstancePath,
    std::string_view navigationOrigin,
    std::string_view schemaDigest,
    const Json& baseline)
{
    const std::string normalizedPath = NormalizeRelativeAssetPath(materialInstancePath);
    auto documentIt = documents.find(normalizedPath);
    if (documentIt != documents.end())
    {
        if (!navigationOrigin.empty())
        {
            documentIt->second.origins.insert(std::string(navigationOrigin));
        }
        return;
    }

    Document document;
    document.normalizedPath = normalizedPath;
    document.baseline = baseline;
    document.working = baseline;
    document.schemaDigest = std::string(schemaDigest);
    if (!navigationOrigin.empty())
    {
        document.origins.insert(std::string(navigationOrigin));
    }
    documents.emplace(normalizedPath, std::move(document));
}

bool DocumentContractRegistry::Contains(
    std::string_view materialInstancePath) const
{
    return documents.find(NormalizeRelativeAssetPath(materialInstancePath)) !=
        documents.end();
}

bool DocumentContractRegistry::SetWorking(
    std::string_view materialInstancePath,
    uint64_t expectedRevision,
    const Json& working)
{
    auto documentIt = documents.find(NormalizeRelativeAssetPath(materialInstancePath));
    if (documentIt == documents.end() ||
        documentIt->second.revision != expectedRevision)
    {
        return false;
    }
    documentIt->second.working = working;
    documentIt->second.dirty =
        documentIt->second.working != documentIt->second.baseline;
    documentIt->second.state = documentIt->second.dirty ? "dirty" : "clean";
    ++documentIt->second.revision;
    return true;
}

bool DocumentContractRegistry::Revert(
    std::string_view materialInstancePath,
    uint64_t expectedRevision)
{
    auto documentIt = documents.find(NormalizeRelativeAssetPath(materialInstancePath));
    if (documentIt == documents.end() ||
        documentIt->second.revision != expectedRevision ||
        documentIt->second.state == "source_changed")
    {
        return false;
    }
    documentIt->second.working = documentIt->second.baseline;
    documentIt->second.dirty = false;
    documentIt->second.state = "clean";
    ++documentIt->second.revision;
    return true;
}

bool DocumentContractRegistry::Reload(
    std::string_view materialInstancePath,
    uint64_t expectedRevision,
    std::string_view schemaDigest,
    const Json& baseline)
{
    auto documentIt = documents.find(NormalizeRelativeAssetPath(materialInstancePath));
    if (documentIt == documents.end() ||
        documentIt->second.revision != expectedRevision)
    {
        return false;
    }

    // Source Changed 后只能用显式 Reload/Merge 重新建立 schema 对齐的 baseline。
    documentIt->second.baseline = baseline;
    documentIt->second.working = baseline;
    documentIt->second.schemaDigest = std::string(schemaDigest);
    documentIt->second.dirty = false;
    documentIt->second.state = "clean";
    ++documentIt->second.revision;
    return true;
}

bool DocumentContractRegistry::Save(
    std::string_view materialInstancePath,
    uint64_t expectedRevision,
    std::string_view newSchemaDigest)
{
    auto documentIt = documents.find(NormalizeRelativeAssetPath(materialInstancePath));
    if (documentIt == documents.end() ||
        documentIt->second.revision != expectedRevision ||
        documentIt->second.state == "source_changed")
    {
        return false;
    }
    documentIt->second.baseline = documentIt->second.working;
    documentIt->second.schemaDigest = std::string(newSchemaDigest);
    documentIt->second.dirty = false;
    documentIt->second.state = "clean";
    ++documentIt->second.revision;
    return true;
}

bool DocumentContractRegistry::Close(
    std::string_view materialInstancePath,
    ClosePolicy policy)
{
    const std::string normalizedPath = NormalizeRelativeAssetPath(materialInstancePath);
    auto documentIt = documents.find(normalizedPath);
    if (documentIt == documents.end())
    {
        return false;
    }
    if (documentIt->second.dirty && policy == ClosePolicy::Cancel)
    {
        return false;
    }
    documents.erase(documentIt);
    return true;
}

void DocumentContractRegistry::ConnectPreview(
    std::string_view materialInstancePath)
{
    auto documentIt = documents.find(NormalizeRelativeAssetPath(materialInstancePath));
    Require(documentIt != documents.end(), "preview requires an open document");
    documentIt->second.previewConnected = true;
}

void DocumentContractRegistry::WorldChanged(
    std::string_view worldIdentity)
{
    activeWorldIdentity = std::string(worldIdentity);
    for (auto& [path, document] : documents)
    {
        (void)path;
        document.previewConnected = false;
    }
}

void DocumentContractRegistry::MarkSchemaChanged(
    std::string_view materialInstancePath,
    std::string_view schemaDigest)
{
    auto documentIt = documents.find(NormalizeRelativeAssetPath(materialInstancePath));
    Require(documentIt != documents.end(), "schema change requires an open document");
    documentIt->second.schemaDigest = std::string(schemaDigest);
    documentIt->second.state = "source_changed";
}

DocumentContractRegistry::Snapshot DocumentContractRegistry::GetSnapshot(
    std::string_view materialInstancePath) const
{
    const auto documentIt = documents.find(NormalizeRelativeAssetPath(materialInstancePath));
    Require(documentIt != documents.end(), "document is not open");
    const Document& document = documentIt->second;
    return {
        document.normalizedPath,
        document.revision,
        document.dirty,
        document.previewConnected,
        document.state,
        document.schemaDigest,
        document.origins.size()};
}

} // namespace material_instance_editor_test
