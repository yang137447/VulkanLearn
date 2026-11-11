#pragma once

#include <string>

namespace Rml
{
class Element;
class Event;
}

namespace VL::EditorUi
{

class MaterialInstanceAssetEditorPanel;

namespace Panel
{

// 动作分发不需要面板的隐式 this；面板实例就是显式上下文，状态仍由面板独占。
void HandlePanelClick(MaterialInstanceAssetEditorPanel& panel, Rml::Element& element,
    const std::string& action);
void HandlePanelChange(MaterialInstanceAssetEditorPanel& panel, Rml::Element& element,
    const std::string& action, const Rml::Event* event);

} // namespace Panel
} // namespace VL::EditorUi
