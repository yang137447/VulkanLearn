#pragma once
#include "uiRenderSnapshot.h"
#include "debug/debugProjection.h"
#include "debug/drawDebug.h"
#include <RmlUi/Core/Types.h>

namespace Rml { class Context; class ElementDocument; class RenderInterface; }
namespace VL
{
class RmlUiRenderInterface;
Debug::Camera DebugDrawCamera(const UiCameraProjection& projection);
void AppendDebugDrawOverlay(UiRenderSnapshot& snapshot, const Debug::Snapshot& view,
    const UiCameraProjection& camera, bool linearOutput = true);
void InsertDebugDrawCommands(UiRenderSnapshot& snapshot, size_t firstAppended);

// 文字继续使用 RmlUi 字形/字体回退；独立的无输入 context 只生成文字几何，
// 同一录制接口冻结到 UiRenderSnapshot 后按场景图像/菜单边界插入，不截获游戏输入。
class DebugDrawLabels
{
public:
    ~DebugDrawLabels();
    void Reset();
    bool Initialize(const std::string& contextName, RmlUiRenderInterface& renderer);
    void Render(const Debug::Snapshot& view, const UiCameraProjection& camera, const UiViewportRect& rect,
        uint32_t width, uint32_t height);
    Rml::ElementDocument* Document() const { return document; }
private:
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    RmlUiRenderInterface* renderer = nullptr;
    std::vector<std::string> labelTexts;
};
}
