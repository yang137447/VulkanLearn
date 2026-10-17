#pragma once

#include <nlohmann/json.hpp>
#include "ui/widgets/input/inputWidgets.h"

namespace Rml
{
class Element;
class ElementDocument;
}
namespace VL::UiWidgets
{
class WidgetDocument;
}

namespace VL::EditorUi
{
nlohmann::json ScriptPropertyField(const nlohmann::json& classes,
    const std::string& className, const std::string& name, const nlohmann::json& value);
std::string ScriptPropertyText(const nlohmann::json& value);
bool ScriptPropertyUsesComponents(const nlohmann::json& field, const nlohmann::json& value);
bool ScriptPropertyIsComposite(const nlohmann::json& field);
std::string ScriptPropertyPathId(const std::string& root, const nlohmann::json& path);
bool ParseScriptQuaternion(const std::string& text, nlohmann::json& value);
std::string ScriptPropertyControl(const std::string& id, UiWidgets::WidgetAction action,
    const nlohmann::json& field, const nlohmann::json& value);
std::string ScriptPropertyRow(const std::string& label, const std::string& id,
    UiWidgets::WidgetAction action, const nlohmann::json& field, const nlohmann::json& value,
    const std::string& trailing = {});
void RefreshScriptPropertyControl(Rml::ElementDocument& document, UiWidgets::WidgetDocument& widgets,
    const std::string& root, UiWidgets::WidgetAction action, const nlohmann::json& field,
    const nlohmann::json& value, bool disabled, bool overridden = false,
    const nlohmann::json& path = nlohmann::json::array(),
    const nlohmann::json& authored = nlohmann::json(nullptr), bool allowNestedReset = true);

enum class ScriptPropertyEvent { Ignored, Local, Edit };
ScriptPropertyEvent ReadScriptCompositeEvent(Rml::Element& element,
    const nlohmann::json& schema, const nlohmann::json& effective, bool click,
    nlohmann::json& request);
} // namespace VL::EditorUi
