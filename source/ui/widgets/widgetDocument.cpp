#include "ui/widgets/widgetDocument.h"

#include <cstdlib>
#include <cstdio>
#include <utility>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

namespace VL::UiWidgets
{

namespace
{
// 范围控件由 RmlUi 的 WidgetSlider 驱动：它把 value 属性重写成量化后的浮点，
// GetValue() 又用 "%f" 序列化（6 位小数）。快照推送的是 9 位有效数字，
// 字符串比较因此永远不相等，静止帧会反复回写 value 属性。
// 只对范围控件退化成数值比较；文本输入保持原有字符串比较，未完成输入不受影响。
bool ReportsSameValue(Rml::Element* element, const std::string& reported, const std::string& value)
{
    if (reported == value) return true;
    auto* input = dynamic_cast<Rml::ElementFormControlInput*>(element);
    if (input == nullptr || input->GetAttribute<Rml::String>("type", "") != "range") return false;
    return std::strtof(reported.c_str(), nullptr) == std::strtof(value.c_str(), nullptr);
}
}

void WidgetDocument::Attach(Rml::ElementDocument& value)
{
    document = &value;
    markupCache.clear();
    propertyCache.clear();
    propertyWriteCount = 0;
}

void WidgetDocument::Detach()
{
    document = nullptr;
    markupCache.clear();
    propertyCache.clear();
}

void WidgetDocument::SetMarkup(const std::string& id, std::string markup)
{
    if (document == nullptr) return;
    const auto cached = markupCache.find(id);
    if (cached != markupCache.end() && cached->second == markup) return;
    if (auto* element = document->GetElementById(id))
    {
        // 诊断：markup 必须只承载结构。若调用方把实时值拼进 markup，替换会销毁
        // 正在拖动的滑块/正在输入的文本；焦点在这里给出明确信号。
        if (auto* focus = document->GetContext()->GetFocusElement())
        {
            bool focusedInside = false;
            for (Rml::Element* ancestor = focus; ancestor != nullptr;
                ancestor = ancestor->GetParentNode())
            {
                if (ancestor == element)
                {
                    focusedInside = true;
                    break;
                }
            }
            if (focusedInside)
                std::fprintf(stderr,
                    "[WidgetDocument] SetMarkup replaced focused subtree: %s\n", id.c_str());
        }
        InvalidateDescendants(*element);
        // RmlUi 将 option 移到内部 widget；替换 DOM 不会自动清理旧选项。
        if (auto* selector = dynamic_cast<Rml::ElementFormControlSelect*>(element))
            selector->RemoveAll();
        element->SetInnerRML(markup);
        markupCache[id] = std::move(markup);
    }
}

void WidgetDocument::InvalidateDescendants(Rml::Element& host)
{
    // 只遍历将被替换的子树，避免每个相机数值/FPS 文本更新都按缓存项反复搜索整份文档。
    // 包含 non-DOM 子节点：select 的 option 会被 RmlUi 移入内部 widget。
    for (int index = 0; index < host.GetNumChildren(true); ++index)
    {
        auto& child = *host.GetChild(index);
        if (!child.GetId().empty())
        {
            propertyCache.erase(child.GetId());
            markupCache.erase(child.GetId());
        }
        InvalidateDescendants(child);
    }
}

void WidgetDocument::SetControlValue(const std::string& id, const std::string& value)
{
    if (document == nullptr) return;
    Rml::Element* element = document->GetElementById(id);
    auto* control = dynamic_cast<Rml::ElementFormControl*>(element);
    // 文本可处于未完成状态；只有非焦点控件才接收快照，不抢夺用户输入。
    if (control == nullptr || document->GetContext()->GetFocusElement() == control) return;
    if (ReportsSameValue(element, control->GetValue(), value)) return;
    control->SetValue(value);
}

void WidgetDocument::SetDisabled(const std::string& id, bool disabled)
{
    if (document == nullptr) return;
    if (auto* element = document->GetElementById(id))
    {
        if (disabled != element->HasAttribute("disabled"))
        {
            if (disabled) element->SetAttribute("disabled", true);
            else element->RemoveAttribute("disabled");
        }
        // 普通 button 标签没有 FormControl 的 :disabled 状态，样式需显式同步。
        SetClass(id, "disabled", disabled);
    }
}

void WidgetDocument::SetClass(const std::string& id, const std::string& name, bool active)
{
    if (document == nullptr) return;
    const std::string key = "@" + name;
    const std::string value = active ? "1" : "0";
    auto& properties = propertyCache[id];
    const auto cached = properties.find(key);
    if (cached != properties.end() && cached->second == value) return;
    if (auto* element = document->GetElementById(id))
    {
        element->SetClass(name, active);
        properties[key] = value;
        ++propertyWriteCount;
    }
}

void WidgetDocument::SetProperty(const std::string& id, const std::string& property, const std::string& value)
{
    if (document == nullptr) return;
    auto& properties = propertyCache[id];
    const auto cached = properties.find(property);
    if (cached != properties.end() && cached->second == value) return;
    if (auto* element = document->GetElementById(id))
    {
        // RmlUi 即使收到相同值也会 DirtyProperty；静止帧不得反复触发整棵树布局。
        if (element->SetProperty(property, value))
        {
            properties[property] = value;
            ++propertyWriteCount;
        }
    }
}

void WidgetDocument::ForgetMarkup(const std::string& id)
{
    markupCache.erase(id);
}

WidgetRefreshScope::WidgetRefreshScope(bool& value) : flag(value), previous(value)
{
    flag = true;
}

WidgetRefreshScope::~WidgetRefreshScope()
{
    flag = previous;
}

} // namespace VL::UiWidgets
