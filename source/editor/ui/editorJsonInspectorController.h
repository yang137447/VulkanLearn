#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "editor/assets/editorJsonAssetTypes.h"
#include "ui/widgets/widgetDocument.h"

namespace Rml { class ElementDocument; class Event; }

namespace VL::EditorUi
{

// JSON 查看器只消费 worker 扁平化的字段快照；展开、搜索和分页不访问文件或运行时对象。
class EditorJsonInspectorController
{
public:
    bool Attach(Rml::ElementDocument& document, std::string& error,
        std::string idPrefix = "json");
    void Detach();
    void Build(std::shared_ptr<const EditorJsonDocumentSnapshot> snapshot,
        const std::string& requestedPath, bool loading);
    bool ProcessEvent(Rml::Event& event);

private:
    void Refresh();
    void CollectVisibleFields();
    std::string FieldsMarkup() const;
    std::string ElementId(const char* suffix) const;
    std::string ActionName(const char* suffix) const;
    Rml::ElementDocument* document = nullptr;
    UiWidgets::WidgetDocument widgets;
    std::shared_ptr<const EditorJsonDocumentSnapshot> snapshot;
    std::set<std::string> expanded;
    std::vector<size_t> visible;
    std::string requestedPath;
    std::string query;
    size_t page = 0;
    bool loading = false;
    bool dirty = true;
    std::string idPrefix = "json";
};

} // namespace VL::EditorUi
