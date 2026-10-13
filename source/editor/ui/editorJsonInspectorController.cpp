#include "editor/ui/editorJsonInspectorController.h"

#include <algorithm>
#include <utility>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>

#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{
namespace
{
constexpr size_t FieldsPerPage = 64;

std::string FoldText(std::string value)
{
    for (char& ch : value)
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return value;
}

bool IsContainer(EditorJsonValueKind kind)
{
    return kind == EditorJsonValueKind::Object || kind == EditorJsonValueKind::Array;
}

const char* ValueClass(EditorJsonValueKind kind)
{
    switch (kind)
    {
    case EditorJsonValueKind::Object: return "object";
    case EditorJsonValueKind::Array: return "array";
    case EditorJsonValueKind::String: return "string";
    case EditorJsonValueKind::Number: return "number";
    case EditorJsonValueKind::Boolean: return "boolean";
    case EditorJsonValueKind::Null: return "null";
    }
    return "null";
}
} // namespace

bool EditorJsonInspectorController::Attach(Rml::ElementDocument& candidate, std::string& error,
    std::string prefix)
{
    idPrefix = std::move(prefix);
    for (const char* suffix : {"title", "kind", "path", "description", "status",
        "fields", "search", "page", "prev", "next"})
    {
        const std::string id = ElementId(suffix);
        if (candidate.GetElementById(id.c_str()) == nullptr)
        {
            error = std::string("JSON inspector is missing element: ") + id;
            return false;
        }
    }
    document = &candidate;
    widgets.Attach(candidate);
    dirty = true;
    return true;
}

std::string EditorJsonInspectorController::ElementId(const char* suffix) const
{
    return idPrefix + "-" + suffix;
}

std::string EditorJsonInspectorController::ActionName(const char* suffix) const
{
    return idPrefix + "-" + suffix;
}

void EditorJsonInspectorController::Detach()
{
    widgets.Detach();
    document = nullptr;
    dirty = true;
}

void EditorJsonInspectorController::Build(
    std::shared_ptr<const EditorJsonDocumentSnapshot> value,
    const std::string& path, bool isLoading)
{
    if (snapshot != value || requestedPath != path || loading != isLoading)
    {
        const bool newAsset = requestedPath != path;
        snapshot = std::move(value);
        requestedPath = path;
        loading = isLoading;
        dirty = true;
        if (newAsset)
        {
            expanded.clear();
            query.clear();
            page = 0;
        }
        if (snapshot != nullptr && snapshot->assetPath == requestedPath)
        {
            for (const auto& field : snapshot->fields)
                if (field.parent == -1 && IsContainer(field.kind)) expanded.insert(field.path);
        }
    }
    if (document != nullptr && dirty) Refresh();
}

void EditorJsonInspectorController::CollectVisibleFields()
{
    visible.clear();
    if (snapshot == nullptr || snapshot->assetPath != requestedPath || loading) return;
    const auto folded = FoldText(query);
    std::vector<bool> included(snapshot->fields.size(), false);
    for (size_t index = 0; index < snapshot->fields.size(); ++index)
    {
        const auto& field = snapshot->fields[index];
        if (!folded.empty())
        {
            included[index] = FoldText(field.path + " " + field.value).find(folded) != std::string::npos;
        }
        else if (field.parent < 0)
        {
            included[index] = true;
        }
        else
        {
            const auto parent = static_cast<size_t>(field.parent);
            // worker 保证前序遍历的 parent 在当前行之前；UI 不重新解释 JSON 层级。
            included[index] = included[parent] && expanded.count(snapshot->fields[parent].path) != 0;
        }
        if (included[index]) visible.push_back(index);
    }
    const size_t pages = std::max(size_t(1), (visible.size() + FieldsPerPage - 1) / FieldsPerPage);
    page = std::min(page, pages - 1);
}

std::string EditorJsonInspectorController::FieldsMarkup() const
{
    using UiWidgets::Text;
    if (snapshot == nullptr) return {};
    std::string markup;
    const size_t end = std::min(visible.size(), (page + 1) * FieldsPerPage);
    for (size_t offset = page * FieldsPerPage; offset < end; ++offset)
    {
        const size_t index = visible[offset];
        const auto& field = snapshot->fields[index];
        markup += "<div class=\"json-field\" title=\"" + Text(field.path) + "\">";
        markup += "<div class=\"json-field-key\" style=\"padding-left: " +
            std::to_string(std::min(field.depth, uint32_t(8)) * 10) + "px;\">";
        if (IsContainer(field.kind))
        {
            markup += "<button class=\"json-expand";
            if (expanded.count(field.path) != 0) markup += " expanded";
            markup += "\" data-action=\"" + ActionName("expand") + "\" data-index=\"" + std::to_string(index) +
                "\"><img src=\"icons/chevron-right.png\"/></button>";
        }
        else markup += "<span class=\"json-indent\"></span>";
        markup += "<span>" + Text(field.label) + "</span></div>";
        markup += "<div class=\"json-field-value json-" + std::string(ValueClass(field.kind)) +
            "\">" + Text(field.value) + "</div></div>";
    }
    return markup;
}

void EditorJsonInspectorController::Refresh()
{
    using UiWidgets::Text;
    const bool matches = snapshot != nullptr && snapshot->assetPath == requestedPath;
    widgets.SetMarkup(ElementId("title"), Text(matches && !snapshot->name.empty() ? snapshot->name : requestedPath));
    widgets.SetMarkup(ElementId("kind"), matches ? EditorAssetKindName(snapshot->kind) : "JSON");
    widgets.SetMarkup(ElementId("path"), Text(requestedPath));
    widgets.SetMarkup(ElementId("description"), matches ? Text(snapshot->description) : "");
    widgets.SetMarkup(ElementId("status"), loading ? "Loading..." : (matches ? Text(snapshot->diagnostic) : ""));
    CollectVisibleFields();
    widgets.SetMarkup(ElementId("fields"), FieldsMarkup());
    const size_t pages = std::max(size_t(1), (visible.size() + FieldsPerPage - 1) / FieldsPerPage);
    widgets.SetMarkup(ElementId("page"), std::to_string(page + 1) + " / " + std::to_string(pages));
    widgets.SetDisabled(ElementId("prev"), page == 0);
    widgets.SetDisabled(ElementId("next"), page + 1 >= pages);
    widgets.SetControlValue(ElementId("search"), query);
    dirty = false;
}

bool EditorJsonInspectorController::ProcessEvent(Rml::Event& event)
{
    if (document == nullptr) return false;
    auto* element = event.GetTargetElement();
    while (element != nullptr && element != document && !element->HasAttribute("data-action"))
        element = element->GetParentNode();
    if (element == nullptr || element == document) return false;
    const auto action = element->GetAttribute<std::string>("data-action", "");
    const std::string expandAction = ActionName("expand");
    const std::string searchAction = ActionName("search");
    const std::string prevAction = ActionName("prev");
    const std::string nextAction = ActionName("next");
    if (action != expandAction && action != searchAction &&
        action != prevAction && action != nextAction) return false;
    if (element->HasAttribute("disabled")) return true;
    if (event.GetType() == "change" && action == searchAction)
    {
        const auto* control = dynamic_cast<Rml::ElementFormControl*>(element);
        if (control != nullptr) query = control->GetValue();
        page = 0;
        dirty = true;
    }
    else if (event.GetType() == "click")
    {
        if (action == expandAction && snapshot != nullptr && !loading &&
            snapshot->assetPath == requestedPath)
        {
            const int index = element->GetAttribute<int>("data-index", -1);
            // DOM 中的地址仍需属于当前已展示集合，不能把旧页的索引移到新快照上。
            if (index >= 0 && std::find(visible.begin(), visible.end(), static_cast<size_t>(index)) != visible.end())
            {
                const auto& field = snapshot->fields[static_cast<size_t>(index)];
                if (expanded.count(field.path) != 0) expanded.erase(field.path);
                else expanded.insert(field.path);
                dirty = true;
            }
        }
        else if (action == prevAction && page > 0) { --page; dirty = true; }
        else if (action == nextAction && (page + 1) * FieldsPerPage < visible.size()) { ++page; dirty = true; }
    }
    return true;
}

} // namespace VL::EditorUi
