#include "editor/ui/terrainAssetEditorController.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{
namespace
{
using Json = nlohmann::json;
using namespace UiWidgets;

std::string ControlText(Rml::Element& element)
{
    auto* control = dynamic_cast<Rml::ElementFormControl*>(&element);
    return control ? control->GetValue() : std::string();
}

std::vector<std::string> SplitFields(const std::string& text)
{
    std::string normalized = text;
    for (char& character : normalized)
        if (character == ',' || character == ';') character = ' ';
    std::istringstream stream(normalized);
    std::vector<std::string> result;
    std::string item;
    while (stream >> item) result.push_back(item);
    return result;
}

double ParseNumber(const std::string& text, const char* field)
{
    size_t end = 0;
    const double value = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(value) ||
        !std::isfinite(static_cast<float>(value)))
        throw std::invalid_argument(std::string("A complete finite number is required for ") + field);
    return value;
}

int64_t ParseInteger(const std::string& text, const char* field)
{
    size_t end = 0;
    const long long value = std::stoll(text, &end);
    if (end != text.size() || value < 0)
        throw std::invalid_argument(std::string("A non-negative integer is required for ") + field);
    return static_cast<int64_t>(value);
}

std::string ValueText(const Json& value)
{
    if (value.is_null()) return {};
    if (value.is_string()) return value.get<std::string>();
    if (value.is_array())
    {
        std::string text;
        for (size_t index = 0; index < value.size(); ++index)
        {
            if (index != 0) text += ", ";
            text += value[index].is_string() ? value[index].get<std::string>() : value[index].dump();
        }
        return text;
    }
    return value.dump();
}

std::vector<Json> CapabilityFields(const Json& capabilities, bool layer)
{
    std::vector<Json> fields;
    if (!layer)
    {
        for (const auto& field : capabilities.value("fields", Json::array()))
            fields.push_back(field);
        return fields;
    }
    const auto layers = capabilities.value("layers", Json::array());
    if (!layers.empty())
        for (const auto& field : layers.front().value("fields", Json::array()))
            fields.push_back(field);
    return fields;
}

Json FindCapability(const Json& capabilities, bool layer, const std::string& name)
{
    for (const Json& field : CapabilityFields(capabilities, layer))
        if (field.value("field", std::string()) == name) return field;
    return Json::object();
}

std::string FieldRow(
    const std::string& id,
    const std::string& label,
    const std::string& value,
    bool layer,
    const std::string& field,
    int layerIndex,
    const std::string& expectedLayerName,
    const std::string& unit)
{
    std::string attributes = " id=\"" + id + "\" data-action=\"terrain-asset-field\" data-field=\"" +
        Text(field) + "\" data-layer=\"" + std::to_string(layerIndex) + "\" data-expected=\"" +
        Text(expectedLayerName) + "\"";
    if (layer) attributes += " data-layer-scope=\"1\"";
    std::string markup = "<div class=\"terrain-field-row\"><label>" + Text(label) + "</label>" +
        "<input type=\"text\"" + attributes + " value=\"" + Text(value) + "\"/>";
    if (!unit.empty()) markup += "<span class=\"terrain-field-unit\">" + Text(unit) + "</span>";
    return markup + "</div>";
}

} // namespace

void TerrainAssetEditorController::SetCommandSink(IEditorCommandSink* value)
{
    sink = value;
}

bool TerrainAssetEditorController::Attach(Rml::ElementDocument& candidate, std::string& error)
{
    if (candidate.GetElementById("terrain-asset-editor") == nullptr)
    {
        error = "Editor workspace is missing element: terrain-asset-editor";
        return false;
    }
    document = &candidate;
    renderedIdentity.clear();
    return true;
}

void TerrainAssetEditorController::Detach()
{
    document = nullptr;
    current = nullptr;
    currentDocument = Json::object();
    currentCapabilities = Json::object();
    currentPath.clear();
    renderedIdentity.clear();
    localError.clear();
}

std::string TerrainAssetEditorController::InputId(
    bool layer,
    const std::string& field,
    int layerIndex)
{
    return layer ? "terrain-layer-" + std::to_string(layerIndex) + "-" + field
        : "terrain-field-" + field;
}

std::vector<TerrainAssetEditorController::FieldSpec>
TerrainAssetEditorController::FieldSpecs(bool layer) const
{
    if (!layer)
    {
        return {
            {"name", "string", "", false},
            {"description", "string", "", true},
            {"heightmapPath", "string", "source_path", false},
            {"materialInstancePath", "string", "asset_path", false},
            {"size", "vec2", "cm", false},
            {"heightScale", "number", "cm", false},
            {"heightOffset", "number", "cm", false},
            {"gridResolution", "integer", "", false},
            {"chunkQuads", "integer", "", false},
            {"lodLevels", "integer", "", true},
            {"lodDistances", "number_array", "cm", true},
            {"chunkResidencyDistances", "number_array", "cm", true},
            {"pageOrigin", "vec2_int", "", true},
            {"pageTilesPerSide", "integer", "", true}};
    }
    return {
        {"name", "string", "", false},
        {"key", "string", "", true},
        {"heightRange", "vec2", "", false},
        {"slopeRange", "vec2", "degrees", false},
        {"noise", "number", "", true},
        {"uvScale", "number", "", true},
        {"uvRotation", "number", "degrees", true},
        {"sourceResolution", "integer", "", true},
        {"avgColor", "vec3", "linear_rgb", true},
        {"sourceSet", "integer", "", true},
        {"flags", "integer", "", true}};
}

std::string TerrainAssetEditorController::FieldsMarkup(
    const EditorAssetDocumentResultPayload& payload) const
{
    std::string markup;
    for (const FieldSpec& field : FieldSpecs(false))
        markup += FieldRow(InputId(false, field.name, 0), field.name,
            ValueText(payload.document.value(field.name, Json())), false,
            field.name, -1, "", field.unit);
    return markup;
}

std::string TerrainAssetEditorController::LayerMarkup(
    const EditorAssetDocumentResultPayload& payload) const
{
    std::string markup;
    const auto layers = payload.document.value("layers", Json::array());
    for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex)
    {
        const Json& layer = layers[layerIndex];
        const std::string name = layer.value("name", std::string());
        markup += "<div class=\"terrain-layer-card\"><div class=\"terrain-layer-heading\">" +
            Text(name) + "<button data-action=\"terrain-layer-remove\" data-layer=\"" +
            std::to_string(layerIndex) + "\" data-expected=\"" + Text(name) +
            "\">Remove</button></div>";
        for (const FieldSpec& field : FieldSpecs(true))
            markup += FieldRow(InputId(true, field.name, static_cast<int>(layerIndex)),
                field.name, ValueText(layer.value(field.name, Json())), true,
                field.name, static_cast<int>(layerIndex), name, field.unit);
        markup += "</div>";
    }
    if (layers.empty())
        markup = "<p class=\"terrain-empty\">No layers. Add one to author a terrain palette.</p>";
    return markup;
}

std::string TerrainAssetEditorController::StatusText(
    const EditorAssetDocumentResultPayload& payload) const
{
    if (!localError.empty()) return localError;
    std::string status = payload.assetPath + " | rev " +
        std::to_string(payload.documentRevision) +
        (payload.dirty ? " | dirty" : " | clean");
    if (payload.diagnostics.is_array())
        for (const auto& diagnostic : payload.diagnostics)
            status += " | " + diagnostic.value("message", std::string());
    return status;
}

void TerrainAssetEditorController::Build(
    const EditorAssetDocumentResultPayload* payload,
    bool loading)
{
    if (document == nullptr) return;
    current = payload;
    auto* root = document->GetElementById("terrain-asset-editor");
    if (root == nullptr) return;
    if (payload == nullptr)
    {
        localError.clear();
        currentDocument = Json::object();
        currentCapabilities = Json::object();
        currentPath.clear();
        if (renderedIdentity != "empty")
        {
            renderedIdentity = "empty";
            root->SetInnerRML(
                "<p class=\"terrain-empty\">Open a TR_*.json terrain asset from Content Browser.</p>"
                "<p id=\"terrain-asset-status\" class=\"notice\"></p>");
        }
        if (auto* status = document->GetElementById("terrain-asset-status"))
        {
            const std::string text = Text(loading ? "Loading terrain asset..." : localError);
            if (status->GetInnerRML() != text) status->SetInnerRML(text);
        }
        return;
    }
    if (!currentPath.empty() && currentPath != payload->assetPath)
        localError.clear();
    currentDocument = payload->document;
    currentCapabilities = payload->capabilities;
    currentPath = payload->assetPath;
    currentRevision = payload->documentRevision;
    const std::string identity = payload->assetPath + "|" +
        std::to_string(payload->documentRevision) + "|" +
        (payload->dirty ? "dirty" : "clean") + "|" + localError;
    if (identity != renderedIdentity)
    {
        const std::string markup =
            "<div class=\"terrain-asset-header\"><h3>" +
            Text(payload->document.value("name", std::string("Terrain"))) + "</h3>" +
            "<span>TR asset</span></div>" +
            (payload->heightmapPreviewPath.empty()
                ? "<p class=\"terrain-empty\">Heightmap preview unavailable (missing, EXR, or unsupported format).</p>"
                : "<div class=\"terrain-heightmap-preview\"><img src=\"" +
                    Text(payload->heightmapPreviewPath) + "\" alt=\"Heightmap preview\"/></div>") +
            "<div id=\"terrain-asset-fields\">" + FieldsMarkup(*payload) + "</div>" +
            "<div class=\"terrain-layer-title\">Layers</div>" +
            "<div id=\"terrain-layer-list\">" + LayerMarkup(*payload) + "</div>" +
            "<button id=\"terrain-layer-add\" data-action=\"terrain-layer-add\">Add Layer</button>" +
            "<p id=\"terrain-asset-status\" class=\"notice\"></p>";
        root->SetInnerRML(markup);
        renderedIdentity = identity;
    }
    if (auto* status = document->GetElementById("terrain-asset-status"))
    {
        const std::string text = Text(StatusText(*payload));
        if (status->GetInnerRML() != text) status->SetInnerRML(text);
    }
}

nlohmann::json TerrainAssetEditorController::CurrentLayers() const
{
    return currentDocument.value("layers", Json::array());
}

bool TerrainAssetEditorController::SubmitChange(Rml::Element& element, bool layer)
{
    if (current == nullptr || sink == nullptr) return true;
    const std::string field = element.GetAttribute<std::string>("data-field", "");
    const std::string text = ControlText(element);
    const int layerIndex = element.GetAttribute<int>("data-layer", -1);
    const std::string expected = element.GetAttribute<std::string>("data-expected", "");
    const Json capability = FindCapability(currentCapabilities, layer, field);
    const std::string type = capability.value("type", std::string("string"));
    const bool resettable = capability.value("resettable", false);
    if (field.empty() || (layer && (layerIndex < 0 || expected.empty())))
    {
        localError = "Terrain field identity is incomplete";
        renderedIdentity.clear();
        return true;
    }
    EditorAssetFieldChange change;
    change.field = field;
    if (layer)
    {
        change.layerIndex = static_cast<uint32_t>(layerIndex);
        change.expectedLayerName = expected;
    }
    try
    {
        if (text.empty())
        {
            if (!resettable)
                throw std::invalid_argument("Field requires a value: " + field);
            change.reset = true;
        }
        else if (type == "string")
        {
            change.value = text;
        }
        else if (type == "number")
        {
            change.value = ParseNumber(text, field.c_str());
        }
        else if (type == "integer")
        {
            change.value = ParseInteger(text, field.c_str());
        }
        else if (type == "vec2" || type == "vec2_int")
        {
            const auto parts = SplitFields(text);
            if (parts.size() != 2)
                throw std::invalid_argument("Two components are required for " + field);
            if (type == "vec2")
                change.value = std::array<double, 2>{
                    ParseNumber(parts[0], field.c_str()),
                    ParseNumber(parts[1], field.c_str())};
            else
                change.value = std::array<int64_t, 2>{
                    ParseInteger(parts[0], field.c_str()),
                    ParseInteger(parts[1], field.c_str())};
        }
        else if (type == "vec3")
        {
            const auto parts = SplitFields(text);
            if (parts.size() != 3)
                throw std::invalid_argument("Three components are required for " + field);
            change.value = std::array<double, 3>{
                ParseNumber(parts[0], field.c_str()),
                ParseNumber(parts[1], field.c_str()),
                ParseNumber(parts[2], field.c_str())};
        }
        else if (type == "number_array")
        {
            std::vector<double> numbers;
            for (const std::string& part : SplitFields(text))
                numbers.push_back(ParseNumber(part, field.c_str()));
            change.value = std::move(numbers);
        }
        else
        {
            throw std::invalid_argument("Unsupported terrain field type: " + type);
        }
    }
    catch (const std::exception& error)
    {
        localError = error.what();
        renderedIdentity.clear();
        return true;
    }

    localError.clear();
    EditorCommandEnvelope command;
    command.type = EditorCommandType::ModifyTerrainDocument;
    command.expectedDocumentRevision = currentRevision;
    command.payload = ModifyAssetDocumentPayload{currentPath, {std::move(change)}};
    command.commandId = 0;
    command.source = EditorCommandSource::RmlUi;
    sink->Submit(std::move(command));
    return true;
}

bool TerrainAssetEditorController::SubmitLayerTable(const nlohmann::json& layers)
{
    if (current == nullptr || sink == nullptr) return true;
    EditorAssetFieldChange change;
    change.field = "layers";
    // layers 字段本身可缺省；删除最后一层应 reset 而不是写空数组，
    // 正式校验器要求“存在即 1..256 层”。
    if (layers.empty()) change.reset = true;
    else change.value = layers;
    localError.clear();
    EditorCommandEnvelope command;
    command.type = EditorCommandType::ModifyTerrainDocument;
    command.expectedDocumentRevision = currentRevision;
    command.payload = ModifyAssetDocumentPayload{currentPath, {std::move(change)}};
    command.commandId = 0;
    command.source = EditorCommandSource::RmlUi;
    sink->Submit(std::move(command));
    return true;
}

bool TerrainAssetEditorController::HandleClick(
    Rml::Element& element,
    const std::string& action)
{
    if (current == nullptr || sink == nullptr) return false;
    if (action == "terrain-layer-add")
    {
        Json layers = CurrentLayers();
        if (!layers.is_array()) layers = Json::array();
        std::unordered_set<std::string> names;
        for (const auto& layer : layers)
            names.insert(layer.value("name", std::string()));
        size_t suffix = layers.size() + 1;
        std::string name = "Layer_" + std::to_string(suffix);
        while (names.count(name) != 0)
            name = "Layer_" + std::to_string(++suffix);
        layers.push_back(Json{
            {"name", name},
            {"heightRange", Json::array({0.0, 1.0})},
            {"slopeRange", Json::array({0.0, 90.0})}});
        return SubmitLayerTable(layers);
    }
    if (action == "terrain-layer-remove")
    {
        const int index = element.GetAttribute<int>("data-layer", -1);
        Json layers = CurrentLayers();
        const std::string expected = element.GetAttribute<std::string>("data-expected", "");
        if (!layers.is_array() || index < 0 || static_cast<size_t>(index) >= layers.size() ||
            layers[index].value("name", std::string()) != expected)
        {
            localError = "Terrain layer identity changed before removal";
            renderedIdentity.clear();
            return true;
        }
        layers.erase(layers.begin() + index);
        return SubmitLayerTable(layers);
    }
    return false;
}

bool TerrainAssetEditorController::HandleChange(
    Rml::Element& element,
    const std::string& action)
{
    if (action != "terrain-asset-field") return false;
    return SubmitChange(element, element.HasAttribute("data-layer-scope"));
}

} // namespace VL::EditorUi
