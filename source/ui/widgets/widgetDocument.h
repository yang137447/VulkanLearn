#pragma once

#include <string>
#include <cstddef>
#include <unordered_map>

namespace Rml { class ElementDocument; }

namespace VL::UiWidgets
{

// GT 借用 RML 文档并刷新展示值；缓存不拥有 DOM，不提交业务命令。
class WidgetDocument
{
public:
    void Attach(Rml::ElementDocument& document);
    void Detach();
    void SetMarkup(const std::string& id, std::string markup);
    void SetControlValue(const std::string& id, const std::string& value);
    void SetDisabled(const std::string& id, bool disabled);
    void SetProperty(const std::string& id, const std::string& property, const std::string& value);
    // class 与属性同样需要幂等：宿主每帧刷新选中/激活/隐藏状态时不能反复触碰样式。
    void SetClass(const std::string& id, const std::string& name, bool active);
    void ForgetMarkup(const std::string& id);
    size_t PropertyWriteCount() const { return propertyWriteCount; }
private:
    Rml::ElementDocument* document = nullptr;
    std::unordered_map<std::string, std::string> markupCache;
    std::unordered_map<std::string, std::string> propertyCache;
    size_t propertyWriteCount = 0;
};

// SetValue 也可能产生 change；复合控件刷新期间不能反向提交业务修改。
class WidgetRefreshScope
{
public:
    explicit WidgetRefreshScope(bool& flag);
    ~WidgetRefreshScope();
    WidgetRefreshScope(const WidgetRefreshScope&) = delete;
    WidgetRefreshScope& operator=(const WidgetRefreshScope&) = delete;
private:
    bool& flag;
    bool previous;
};

} // namespace VL::UiWidgets
