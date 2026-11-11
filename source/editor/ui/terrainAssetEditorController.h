#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "editor/ui/materialInstanceEditorView.h"

namespace Rml { class Element; class ElementDocument; }

namespace VL::EditorUi
{

// TR_*.json 编辑页只消费 AssetDocumentService 的冻结投影；字段解析和命令提交
// 都留在 UI 侧，文件、World 和 Vulkan 不进入这里。
class TerrainAssetEditorController
{
public:
    void SetCommandSink(IEditorCommandSink* sink);
    bool Attach(Rml::ElementDocument& document, std::string& error);
    void Detach();
    bool HandleClick(Rml::Element& element, const std::string& action);
    bool HandleChange(Rml::Element& element, const std::string& action);
    void Build(const EditorAssetDocumentResultPayload* document, bool loading);

private:
    struct FieldSpec
    {
        std::string name;
        std::string type;
        std::string unit;
        bool resettable = false;
    };

    void Rebuild(const EditorAssetDocumentResultPayload& document);
    std::string FieldsMarkup(const EditorAssetDocumentResultPayload& document) const;
    std::string LayerMarkup(const EditorAssetDocumentResultPayload& document) const;
    std::vector<FieldSpec> FieldSpecs(bool layer) const;
    bool SubmitChange(Rml::Element& element, bool layer);
    bool SubmitLayerTable(const nlohmann::json& layers);
    nlohmann::json CurrentLayers() const;
    std::string StatusText(const EditorAssetDocumentResultPayload& document) const;
    static std::string InputId(bool layer, const std::string& field, int layerIndex);

    Rml::ElementDocument* document = nullptr;
    IEditorCommandSink* sink = nullptr;
    const EditorAssetDocumentResultPayload* current = nullptr;
    nlohmann::json currentDocument = nlohmann::json::object();
    nlohmann::json currentCapabilities = nlohmann::json::object();
    std::string currentPath;
    std::string renderedIdentity;
    std::string localError;
    uint64_t currentRevision = 0;
};

} // namespace VL::EditorUi
