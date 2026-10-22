#pragma once

#include "editorResourceEditorController.h"
#include "ui/widgets/widgetDocument.h"
#include <unordered_set>

namespace Rml { class ElementDocument; class Event; }

namespace VL::EditorUi
{
// 参考图与模式只消费冻结值；网格动画由外层通过正式预览 World 和 GT 值请求驱动。
class RetargetAssetEditorPanel
{
public:
    void Attach(Rml::ElementDocument& document);
    void Detach();
    void Build(const EditorResourceEditorTab& tab);
    bool ProcessEvent(Rml::Event& event);
    bool AnimationVisible() const { return animationVisible && preview && !preview->targetModelPath.empty(); }

private:
    void Refresh();
    void RefreshDrawing();
    void RefreshHierarchy();
    Rml::ElementDocument* document = nullptr;
    UiWidgets::WidgetDocument widgets;
    std::shared_ptr<const EditorRetargetPreview> preview;
    std::string path, tableSignature, drawingSignature, hierarchySignature;
    std::unordered_set<size_t> collapsedBones;
    bool hierarchyTarget = false;
    bool optionsReady = false;
    bool refreshing = false;
    std::string view = "front";
    bool calibrated = true;
    bool animationVisible = false;
    bool focusSelected = false;
    bool showAllBones = false;
    float zoom = 1;
    size_t selection = 0, anchor = 0, page = 0;
};
}
