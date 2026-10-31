#include "retargetAssetEditorPanel.h"

#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace VL::EditorUi
{
namespace
{
constexpr size_t PageSize = 64;
constexpr float Pi = 3.14159265358979323846f;
using UiWidgets::Text;

std::string Number(float value, int precision = 2)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

Eigen::Vector3f Position(const EditorRetargetJoint& joint, bool calibrated)
{
    const auto& value = calibrated ? joint.referencePosition : joint.bindPosition;
    return {value[0], value[1], value[2]};
}

Eigen::Vector2f Project(const Eigen::Vector3f& point, const std::string& view)
{
    if (view == "side") return {point.z(), -point.y()};
    if (view == "top") return {point.x(), point.z()};
    return {point.x(), -point.y()};
}

std::string MappingName(const EditorRetargetPreview& preview, size_t index)
{
    const auto& mapping = preview.mappings[index];
    return preview.source[mapping.source].name + " / " + preview.target[mapping.target].name;
}

float Distance(const EditorRetargetPreview& preview, size_t mapping, size_t anchor, bool calibrated)
{
    const auto& value = preview.mappings[mapping];
    const auto& origin = preview.mappings[anchor];
    return (Position(preview.source[value.source], calibrated) -
        Position(preview.source[origin.source], calibrated) -
        Position(preview.target[value.target], calibrated) +
        Position(preview.target[origin.target], calibrated)).norm();
}

std::string Angle(const EditorRetargetPreview& preview, size_t mapping, bool calibrated)
{
    const auto& value = preview.mappings[mapping];
    if (value.ancestor < 0) return "--";
    const auto& parent = preview.mappings[static_cast<size_t>(value.ancestor)];
    const Eigen::Vector3d source = Position(preview.source[value.source], calibrated).cast<double>() -
        Position(preview.source[parent.source], calibrated).cast<double>();
    const Eigen::Vector3d target = Position(preview.target[value.target], calibrated).cast<double>() -
        Position(preview.target[parent.target], calibrated).cast<double>();
    if (source.norm() < 0.00001f || target.norm() < 0.00001f) return "--";
    // atan2 在接近平行时仍保留小角精度；float 的 acos(dot) 会把舍入噪声放大到约 0.02 度。
    return Number(static_cast<float>(std::atan2(source.cross(target).norm(), source.dot(target)) * 180 / Pi));
}

std::vector<bool> VisibleBones(const EditorRetargetPreview& preview, bool source, bool all)
{
    const auto& joints = source ? preview.source : preview.target;
    std::vector<bool> visible(joints.size(), all);
    for (const auto& mapping : preview.mappings)
        for (int index = source ? mapping.source : mapping.target; index >= 0 && !visible[index];
            index = joints[index].parent)
            visible[index] = true;
    return visible;
}

std::string Line(const Eigen::Vector2f& first, const Eigen::Vector2f& second,
    const std::string& kind)
{
    const Eigen::Vector2f delta = second - first;
    return "<div class=\"retarget-line " + kind + "\" style=\"left:" + Number(first.x()) +
        "px;top:" + Number(first.y()) + "px;width:" + Number(delta.norm()) +
        "px;transform:rotate(" + Number(std::atan2(delta.y(), delta.x())) + "rad);\"></div>";
}

std::string Point(const Eigen::Vector2f& point, const std::string& kind)
{
    return "<div class=\"retarget-point " + kind + "\" style=\"left:" +
        Number(point.x() - 3) + "px;top:" + Number(point.y() - 3) + "px;\"></div>";
}

std::string DetailRow(const std::string& label, const std::string& value)
{
    return "<div class=\"retarget-detail-row\"><label>" + Text(label) +
        "</label><span title=\"" + Text(value) + "\">" + Text(value) + "</span></div>";
}
}

void RetargetAssetEditorPanel::Attach(Rml::ElementDocument& value)
{
    Detach();
    document = &value;
    widgets.Attach(value);
}

void RetargetAssetEditorPanel::Detach()
{
    widgets.Detach();
    document = nullptr;
    preview.reset();
    path.clear();
    optionsReady = false;
    tableSignature.clear();
    drawingSignature.clear();
    hierarchySignature.clear();
    collapsedBones.clear();
}

void RetargetAssetEditorPanel::Build(const EditorResourceEditorTab& tab)
{
    if (!document) return;
    const bool differentPath = path != tab.path;
    path = tab.path;
    auto next = tab.loading || !tab.snapshot ? nullptr : tab.snapshot->retargetPreview;
    const bool firstValues = !preview;
    if (next != preview || differentPath)
    {
        preview = std::move(next);
        if (differentPath)
        {
            selection = page = 0;
            calibrated = true;
            view = "front";
            zoom = 1;
            focusSelected = false;
            showAllBones = false;
            hierarchyTarget = false;
        }
        if (preview)
        {
            if (differentPath || firstValues) animationVisible = !preview->targetModelPath.empty();
            anchor = preview->defaultAnchor;
            selection = std::min(selection, preview->mappings.size() - 1);
        }
        tableSignature.clear();
        drawingSignature.clear();
        optionsReady = false;
        hierarchySignature.clear();
        collapsedBones.clear();
    }
    std::string status = tab.loading ? "Loading reference poses..." : "Reference preview unavailable";
    if (!tab.loading && tab.snapshot)
    {
        if (!tab.snapshot->diagnostic.empty()) status = tab.snapshot->diagnostic;
        else if (!tab.snapshot->previewDiagnostic.empty()) status = tab.snapshot->previewDiagnostic;
    }
    if (preview)
        status = std::to_string(preview->mappings.size()) + " mapped | " +
            std::to_string(preview->source.size()) + " / " + std::to_string(preview->target.size()) +
            " joints | " + preview->sourcePose + " -> " + preview->targetPose + " | offsets " +
            std::to_string(preview->sourceOffsets) + " / " + std::to_string(preview->targetOffsets);
    // 两种视图各自承载只读叠层，切换模式不重挂 DOM，也不占用预览高度。
    for (const std::string prefix : {"retarget-", "retarget-reference-"})
    {
        widgets.SetMarkup(prefix + "title", Text(tab.snapshot ? tab.snapshot->name : "Retarget"));
        widgets.SetMarkup(prefix + "identity", Text(preview ?
            preview->sourceSkeleton + " -> " + preview->targetSkeleton : path));
        widgets.SetMarkup(prefix + "status", Text(status));
    }
    widgets.SetClass("retarget-reference", "active", !AnimationVisible());
    widgets.SetClass("retarget-animation", "active", AnimationVisible());
    widgets.SetDisabled("retarget-animation", !preview || preview->targetModelPath.empty());
    widgets.SetProperty("retarget-reference-toolbar", "display", AnimationVisible() ? "none" : "flex");
    widgets.SetProperty("retarget-reference-workspace", "display", AnimationVisible() ? "none" : "flex");
    widgets.SetProperty("retarget-animation-workspace", "display", AnimationVisible() ? "flex" : "none");
    Refresh();
}

void RetargetAssetEditorPanel::Refresh()
{
    UiWidgets::WidgetRefreshScope refresh(refreshing);
    widgets.SetClass("retarget-hierarchy-source", "active", !hierarchyTarget);
    widgets.SetClass("retarget-hierarchy-target", "active", hierarchyTarget);
    for (const char* id : {"retarget-bind", "retarget-calibrated", "retarget-front",
        "retarget-side", "retarget-top", "retarget-anchor", "retarget-focus", "retarget-fit", "retarget-zoom",
        "retarget-all-bones", "retarget-hierarchy-source", "retarget-hierarchy-target"})
        widgets.SetDisabled(id, !preview);
    widgets.SetClass("retarget-bind", "active", !calibrated);
    widgets.SetClass("retarget-calibrated", "active", calibrated);
    widgets.SetClass("retarget-focus", "active", focusSelected);
    widgets.SetControlValue("retarget-zoom", Number(zoom));
    auto* allBones = document->GetElementById("retarget-all-bones");
    if (allBones->HasAttribute("checked") != showAllBones)
    {
        if (showAllBones) allBones->SetAttribute("checked", "checked");
        else allBones->RemoveAttribute("checked");
    }
    for (const char* axis : {"front", "side", "top"})
        widgets.SetClass(std::string("retarget-") + axis, "active", view == axis);
    if (!preview)
    {
        widgets.SetMarkup("retarget-lines", "");
        widgets.SetMarkup("retarget-mappings", "");
        widgets.SetMarkup("retarget-anchor", "");
        widgets.SetMarkup("retarget-selection", "");
        widgets.SetMarkup("retarget-measurement", "");
        widgets.SetMarkup("retarget-page", "");
        widgets.SetMarkup("retarget-hierarchy", "");
        widgets.SetMarkup("retarget-details", "");
        widgets.SetDisabled("retarget-prev", true);
        widgets.SetDisabled("retarget-next", true);
        return;
    }
    if (!optionsReady)
    {
        std::string options;
        for (size_t index = 0; index < preview->mappings.size(); ++index)
            options += "<option value=\"" + std::to_string(index) + "\">Anchor: " +
                Text(MappingName(*preview, index)) + "</option>";
        widgets.SetMarkup("retarget-anchor", std::move(options));
        optionsReady = true;
    }
    widgets.SetControlValue("retarget-anchor", std::to_string(anchor));
    const std::string signature = std::to_string(calibrated) + "|" + std::to_string(anchor) +
        "|" + std::to_string(page);
    if (tableSignature != signature)
    {
        std::string rows;
        const size_t end = std::min(preview->mappings.size(), (page + 1) * PageSize);
        for (size_t index = page * PageSize; index < end; ++index)
        {
            const auto& mapping = preview->mappings[index];
            const std::string names = MappingName(*preview, index);
            rows += "<button id=\"retarget-row-" + std::to_string(index) +
                "\" class=\"retarget-row\" data-action=\"retarget-select\" data-index=\"" +
                std::to_string(index) + "\" title=\"" + Text(names) + "\"><span class=\"retarget-names\">" +
                "<span class=\"retarget-source\">" + Text(preview->source[mapping.source].name) +
                "</span><span class=\"retarget-target\">" + Text(preview->target[mapping.target].name) +
                "</span></span><span class=\"retarget-metric\">" + Number(Distance(*preview, index, anchor, calibrated)) +
                "</span><span class=\"retarget-metric\">" + Angle(*preview, index, calibrated) + "</span></button>";
        }
        widgets.SetMarkup("retarget-mappings", std::move(rows));
        tableSignature = signature;
    }
    const size_t pages = (preview->mappings.size() + PageSize - 1) / PageSize;
    widgets.SetMarkup("retarget-page", std::to_string(page + 1) + " / " + std::to_string(pages));
    widgets.SetDisabled("retarget-prev", page == 0);
    widgets.SetDisabled("retarget-next", page + 1 >= pages);
    for (size_t index = page * PageSize; index < std::min(preview->mappings.size(), (page + 1) * PageSize); ++index)
        widgets.SetClass("retarget-row-" + std::to_string(index), "selected", index == selection);
    widgets.SetMarkup("retarget-selection", Text(MappingName(*preview, selection)));
    const auto& mapping = preview->mappings[selection];
    widgets.SetMarkup("retarget-details",
        DetailRow("Source", preview->sourceSkeleton) +
        DetailRow("Target", preview->targetSkeleton) +
        DetailRow("Pose", calibrated ? "Calibrated" : "Bind") +
        DetailRow("Source", preview->source[mapping.source].name) +
        DetailRow("Target", preview->target[mapping.target].name));
    widgets.SetMarkup("retarget-measurement", "Delta " +
        Number(Distance(*preview, selection, anchor, calibrated)) + " cm | Chain " +
        Angle(*preview, selection, calibrated) + " deg | " + (mapping.ancestor < 0 ? "No common mapped ancestor" :
            "From " + Text(MappingName(*preview, static_cast<size_t>(mapping.ancestor)))) +
        " | Axes: <span class=\"retarget-axis-x\">X</span> <span class=\"retarget-axis-y\">Y</span> "
        "<span class=\"retarget-axis-z\">Z</span>");
    RefreshHierarchy();
    RefreshDrawing();
}

void RetargetAssetEditorPanel::RefreshHierarchy()
{
    const std::string signature = std::to_string(hierarchyTarget) + "|" + std::to_string(selection);
    if (signature == hierarchySignature) return;
    hierarchySignature = signature;
    const auto& joints = hierarchyTarget ? preview->target : preview->source;
    std::vector<int> mappings(joints.size(), -1);
    std::vector<bool> hasChildren(joints.size(), false);
    for (size_t index = 0; index < preview->mappings.size(); ++index)
    {
        const auto& mapping = preview->mappings[index];
        mappings[hierarchyTarget ? mapping.target : mapping.source] = static_cast<int>(index);
    }
    for (const auto& joint : joints)
        if (joint.parent >= 0) hasChildren[joint.parent] = true;
    std::string rows;
    for (size_t index = 0; index < joints.size(); ++index)
    {
        UiWidgets::TreeRowData row;
        bool hidden = false;
        for (int ancestor = joints[index].parent; ancestor >= 0; ancestor = joints[ancestor].parent)
        {
            ++row.depth;
            if (collapsedBones.count(static_cast<size_t>(ancestor))) hidden = true;
        }
        if (hidden) continue;
        row.label = joints[index].name;
        row.hasChildren = hasChildren[index];
        row.expanded = !collapsedBones.count(index);
        row.toggle = {"retarget-hierarchy-toggle", index};
        if (mappings[index] >= 0)
        {
            row.select = {"retarget-select", static_cast<size_t>(mappings[index])};
            row.selected = static_cast<size_t>(mappings[index]) == selection;
        }
        else row.rowClass = "retarget-unmapped";
        rows += UiWidgets::TreeRow(row);
    }
    widgets.SetMarkup("retarget-hierarchy", std::move(rows));
}

void RetargetAssetEditorPanel::RefreshDrawing()
{
    const auto size = document->GetElementById("retarget-surface")->GetBox().GetSize(Rml::BoxArea::Content);
    const std::string signature = tableSignature + "|" + view + "|" + std::to_string(selection) + "|" +
        Number(size.x) + "|" + Number(size.y) + "|" + Number(zoom) + "|" +
        std::to_string(focusSelected) + "|" + std::to_string(showAllBones);
    if (signature == drawingSignature || size.x < 64 || size.y < 64) return;
    drawingSignature = signature;
    const auto& origin = preview->mappings[anchor];
    // 两种姿态共用取景范围，Bind/Calibrated 切换不会用重新缩放掩盖真实位置变化。
    Eigen::Vector2f minimum = Eigen::Vector2f::Constant(std::numeric_limits<float>::max());
    Eigen::Vector2f maximum = -minimum;
    for (const auto* joints : {&preview->source, &preview->target})
    {
        const int anchorIndex = joints == &preview->source ? origin.source : origin.target;
        const auto visible = VisibleBones(*preview, joints == &preview->source, showAllBones);
        for (bool pose : {false, true})
            for (size_t index = 0; index < joints->size(); ++index)
            {
                if (!visible[index]) continue;
                const auto value = Project(Position((*joints)[index], pose) - Position((*joints)[anchorIndex], pose), view);
                minimum = minimum.cwiseMin(value);
                maximum = maximum.cwiseMax(value);
            }
    }
    const Eigen::Vector2f extent = (maximum - minimum).cwiseMax(1);
    const float scale = std::min((size.x - 48) / extent.x(), (size.y - 48) / extent.y()) * zoom;
    Eigen::Vector2f center = (minimum + maximum) * 0.5f;
    if (focusSelected)
    {
        const auto& selected = preview->mappings[selection];
        center = (Project(Position(preview->source[selected.source], calibrated) -
            Position(preview->source[origin.source], calibrated), view) +
            Project(Position(preview->target[selected.target], calibrated) -
            Position(preview->target[origin.target], calibrated), view)) * 0.5f;
    }
    const Eigen::Vector2f offset = Eigen::Vector2f(size.x, size.y) * 0.5f - center * scale;
    std::string markup;
    for (const auto* joints : {&preview->source, &preview->target})
    {
        const bool source = joints == &preview->source;
        const auto visible = VisibleBones(*preview, source, showAllBones);
        const std::string kind = source ? "retarget-source" : "retarget-target";
        const int anchorIndex = source ? origin.source : origin.target;
        const auto& selected = preview->mappings[selection];
        const int selectedIndex = source ? selected.source : selected.target;
        const Eigen::Vector3f anchorPosition = Position((*joints)[anchorIndex], calibrated);
        std::vector<Eigen::Vector2f> projected;
        for (const auto& joint : *joints)
            projected.push_back(Project(Position(joint, calibrated) - anchorPosition, view) * scale + offset);
        for (size_t index = 0; index < joints->size(); ++index)
        {
            const int parent = (*joints)[index].parent;
            if (parent >= 0 && visible[index]) markup += Line(projected[parent], projected[index], kind);
        }
        for (const auto& mapping : preview->mappings)
        {
            const int index = source ? mapping.source : mapping.target;
            markup += Point(projected[index], kind + (index == selectedIndex ? " selected" : ""));
        }
        const auto& joint = (*joints)[selectedIndex];
        const auto& axes = calibrated ? joint.referenceAxes : joint.bindAxes;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const auto& v = axes[axis];
            const Eigen::Vector2f end = projected[selectedIndex] +
                Project(Eigen::Vector3f(v[0], v[1], v[2]), view) * 26;
            markup += Line(projected[selectedIndex], end, "retarget-axis-" + std::string(1, "xyz"[axis]));
        }
    }
    const auto& selected = preview->mappings[selection];
    const Eigen::Vector2f first = Project(Position(preview->source[selected.source], calibrated) -
        Position(preview->source[origin.source], calibrated), view) * scale + offset;
    const Eigen::Vector2f second = Project(Position(preview->target[selected.target], calibrated) -
        Position(preview->target[origin.target], calibrated), view) * scale + offset;
    markup += Line(first, second, "retarget-delta");
    widgets.SetMarkup("retarget-lines", std::move(markup));
}

bool RetargetAssetEditorPanel::ProcessEvent(Rml::Event& event)
{
    if (refreshing || !preview || !document ||
        (event.GetType() != "click" && event.GetType() != "change")) return false;
    auto* element = event.GetTargetElement();
    while (element && element != document && !element->HasAttribute("data-action"))
        element = element->GetParentNode();
    if (!element || element == document || element->HasAttribute("disabled")) return false;
    const auto action = element->GetAttribute<std::string>("data-action", "");
    if (action.compare(0, 9, "retarget-") != 0) return false;
    if (action != "retarget-reference" && action != "retarget-animation" &&
        action != "retarget-bind" && action != "retarget-calibrated" &&
        action != "retarget-front" && action != "retarget-side" && action != "retarget-top" &&
        action != "retarget-fit" && action != "retarget-focus" && action != "retarget-prev" &&
        action != "retarget-next" && action != "retarget-select" && action != "retarget-anchor" &&
        action != "retarget-all-bones" && action != "retarget-zoom" &&
        action != "retarget-hierarchy-source" && action != "retarget-hierarchy-target" &&
        action != "retarget-hierarchy-toggle") return false;
    if (event.GetType() == "change" && action == "retarget-anchor")
    {
        const auto* control = dynamic_cast<Rml::ElementFormControl*>(element);
        if (!control) return true;
        const auto value = control->GetValue();
        for (size_t index = 0; index < preview->mappings.size(); ++index)
            if (value == std::to_string(index)) { anchor = index; break; }
    }
    else if (event.GetType() == "change" && action == "retarget-all-bones")
        showAllBones = element->HasAttribute("checked");
    else if (event.GetType() == "change" && action == "retarget-zoom")
    {
        // range 的 value 属性保留精度；边界仅检查 UI 输入，不回写资产校准数据。
        const float value = element->GetAttribute<float>("value", 1);
        if (std::isfinite(value) && value >= 1 && value <= 6) zoom = value;
    }
    else if (event.GetType() == "click")
    {
        if (action == "retarget-reference") animationVisible = false;
        else if (action == "retarget-animation") animationVisible = true;
        else if (action == "retarget-hierarchy-source" || action == "retarget-hierarchy-target")
        {
            hierarchyTarget = action == "retarget-hierarchy-target";
            collapsedBones.clear();
            hierarchySignature.clear();
        }
        else if (action == "retarget-hierarchy-toggle")
        {
            const size_t index = element->GetAttribute<size_t>("data-index", preview->source.size() + preview->target.size());
            if (index < (hierarchyTarget ? preview->target.size() : preview->source.size()))
            {
                if (!collapsedBones.erase(index)) collapsedBones.insert(index);
                hierarchySignature.clear();
            }
        }
        else if (action == "retarget-bind") calibrated = false;
        else if (action == "retarget-calibrated") calibrated = true;
        else if (action == "retarget-front") view = "front";
        else if (action == "retarget-side") view = "side";
        else if (action == "retarget-top") view = "top";
        else if (action == "retarget-fit") { focusSelected = false; zoom = 1; }
        else if (action == "retarget-focus")
        {
            focusSelected = !focusSelected;
            if (focusSelected && zoom == 1) zoom = 3;
        }
        else if (action == "retarget-prev" && page > 0) --page;
        else if (action == "retarget-next" && (page + 1) * PageSize < preview->mappings.size()) ++page;
        else if (action == "retarget-select")
        {
            const size_t index = element->GetAttribute<size_t>("data-index", preview->mappings.size());
            if (index < preview->mappings.size())
            {
                selection = index;
                page = selection / PageSize;
                // 从映射表选骨时展开左侧祖先，避免当前选择藏在折叠分支中。
                const auto& mapping = preview->mappings[selection];
                const auto& joints = hierarchyTarget ? preview->target : preview->source;
                for (int ancestor = joints[hierarchyTarget ? mapping.target : mapping.source].parent;
                    ancestor >= 0; ancestor = joints[ancestor].parent)
                    collapsedBones.erase(static_cast<size_t>(ancestor));
                hierarchySignature.clear();
            }
        }
    }
    Refresh();
    return true;
}
}
