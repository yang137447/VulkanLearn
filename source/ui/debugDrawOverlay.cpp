#include "debugDrawOverlay.h"
#include "ui/uiColorTransform.h"
#include "ui/rmlUiInterfaces.h"
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace VL
{
Debug::Camera DebugDrawCamera(const UiCameraProjection& p)
{
    Debug::Camera result;
    result.position = Eigen::Vector3f(p.position.data()); result.forward = Eigen::Vector3f(p.forward.data());
    result.right = Eigen::Vector3f(p.right.data()); result.up = Eigen::Vector3f(p.up.data());
    result.horizontalFovDegrees = p.horizontalFovDegrees; result.clipNear = p.clipNear; result.clipFar = p.clipFar;
    return result;
}
namespace
{
void AddQuad(UiRenderSnapshot& snapshot, const std::array<Eigen::Vector2f, 4>& points,
    const Debug::Color& color, bool linearOutput, const std::array<float, 4>& depths)
{
    const uint32_t first = static_cast<uint32_t>(snapshot.vertices.size());
    auto displayColor = color;
    if (!linearOutput)
        for (int axis = 0; axis < 3; ++axis) displayColor[axis] = UiLinearToSrgb(displayColor[axis]);
    for (size_t index = 0; index < points.size(); ++index)
    {
        const auto& point = points[index];
        UiVertex vertex; vertex.position[0] = point.x(); vertex.position[1] = point.y(); vertex.color = displayColor;
        vertex.debugDepth = depths[index];
        if (vertex.debugDepth >= 0) snapshot.debugDepthTest = true;
        snapshot.vertices.push_back(vertex);
    }
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) snapshot.indices.push_back(first + i);
}
// OnRender 紧接着录制子文字几何；每个标签设置自己的世界锚点深度。
// instancer 无 renderer 状态，静态寿命满足 Factory 在 Rml::Shutdown 前的非拥有引用。
class DebugLabelElement final : public Rml::Element
{
public:
    explicit DebugLabelElement(const Rml::String& tag) : Rml::Element(tag) {}
    RmlUiRenderInterface* renderer = nullptr;
    float depth = -1;
protected:
    void OnRender() override { renderer->SetDebugDepth(depth); }
};
std::string TextColor(const Debug::Color& color)
{
    std::ostringstream text;
    text << '#' << std::hex << std::setfill('0');
    for (int axis = 0; axis < 4; ++axis)
        text << std::setw(2) << UiSrgbHexByte(axis < 3 ? UiLinearToSrgb(color[axis]) : color[axis]);
    return text.str();
}
}
void AppendDebugDrawOverlay(UiRenderSnapshot& snapshot, const Debug::Snapshot& view,
    const UiCameraProjection& projection, bool linearOutput)
{
    const auto& rect = snapshot.sceneViewportRect;
    if (!view.enabled || !rect.width || !rect.height) return;
    const auto camera = DebugDrawCamera(projection);
    const Eigen::Vector2f origin(float(rect.x), float(rect.y));
    UiDrawCommand command;
    command.firstIndex = static_cast<uint32_t>(snapshot.indices.size());
    command.clipRect = {rect.x, rect.y, rect.width, rect.height};
    for (const auto& line : view.lines)
    {
        Debug::ScreenLine projected;
        if (!Debug::ProjectLine(camera, line.begin, line.end, float(rect.width), float(rect.height), projected)) continue;
        const Eigen::Vector2f delta = projected.end - projected.begin;
        if (delta.squaredNorm() < 1e-8f) continue;
        const Eigen::Vector2f normal = Eigen::Vector2f(-delta.y(), delta.x()).normalized() * (line.thickness * .5f);
        const bool depth = line.depth == Debug::DepthMode::DepthTest;
        AddQuad(snapshot, {origin + projected.begin - normal, origin + projected.end - normal,
            origin + projected.end + normal, origin + projected.begin + normal}, line.color, linearOutput,
            {depth ? projected.beginDepth : -1, depth ? projected.endDepth : -1,
             depth ? projected.endDepth : -1, depth ? projected.beginDepth : -1});
    }
    for (const auto& point : view.points)
    {
        Eigen::Vector2f pixel;
        float depth;
        if (!Debug::ProjectPoint(camera, point.position, float(rect.width), float(rect.height), pixel, &depth)) continue;
        if (point.depth == Debug::DepthMode::Overlay) depth = -1;
        pixel += origin;
        const float half = point.size * .5f;
        AddQuad(snapshot, {pixel + Eigen::Vector2f(-half, -half), pixel + Eigen::Vector2f(half, -half),
            pixel + Eigen::Vector2f(half, half), pixel + Eigen::Vector2f(-half, half)}, point.color, linearOutput,
            {depth, depth, depth, depth});
    }
    command.indexCount = static_cast<uint32_t>(snapshot.indices.size()) - command.firstIndex;
    if (command.indexCount) snapshot.drawCommands.push_back(command);
}
void InsertDebugDrawCommands(UiRenderSnapshot& snapshot, size_t firstAppended)
{
    size_t insertion = 0;
    for (size_t index = 0; index < firstAppended; ++index)
        if (snapshot.drawCommands[index].textureId == SceneViewportTextureId) insertion = index + 1;
    // 只移动绘制次序，不移动索引/顶点；没有场景图片的沉浸模式也位于 HUD 之前。
    std::rotate(snapshot.drawCommands.begin() + insertion, snapshot.drawCommands.begin() + firstAppended,
        snapshot.drawCommands.end());
}
DebugDrawLabels::~DebugDrawLabels() { Reset(); }
void DebugDrawLabels::Reset()
{
    if (context) Rml::RemoveContext(context->GetName());
    context = nullptr; document = nullptr;
    renderer = nullptr;
    labelTexts.clear();
}
bool DebugDrawLabels::Initialize(const std::string& contextName, RmlUiRenderInterface& renderInterface)
{
    Reset();
    renderer = &renderInterface;
    static Rml::ElementInstancerGeneric<DebugLabelElement> labelInstancer;
    Rml::Factory::RegisterElementInstancer("vl-debug-label", &labelInstancer);
    context = Rml::CreateContext(contextName, {1, 1}, renderer);
    if (!context) return false;
    document = context->LoadDocumentFromMemory(
        "<rml><head><style>body{font-family: 'Segoe UI';margin:0;width:100%;height:100%;pointer-events:none;}"
        "#labels{position:absolute;overflow:hidden;pointer-events:none;}"
        ".label{position:absolute;white-space:pre-wrap;word-break:break-all;pointer-events:none;line-height:1.2;}"
        "</style></head><body><div id=\"labels\"></div></body></rml>");
    if (!document) { Reset(); return false; }
    document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    return true;
}
void DebugDrawLabels::Render(const Debug::Snapshot& view, const UiCameraProjection& projection,
    const UiViewportRect& rect, uint32_t width, uint32_t height)
{
    if (!context || !view.enabled || view.texts.empty() || !rect.width || !rect.height) return;
    context->SetDimensions({int(width), int(height)});
    auto* root = document->GetElementById("labels");
    root->SetProperty("left", std::to_string(rect.x) + "px"); root->SetProperty("top", std::to_string(rect.y) + "px");
    root->SetProperty("width", std::to_string(rect.width) + "px"); root->SetProperty("height", std::to_string(rect.height) + "px");
    const auto camera = DebugDrawCamera(projection);
    size_t slot = 0;
    for (const auto& text : view.texts)
    {
        Eigen::Vector2f pixel;
        float depth;
        if (!Debug::ProjectPoint(camera, text.position, float(rect.width), float(rect.height), pixel, &depth)) continue;
        if (slot == size_t(root->GetNumChildren()))
        {
            root->AppendChild(document->CreateElement("vl-debug-label"));
            labelTexts.emplace_back();
        }
        auto* label = static_cast<DebugLabelElement*>(root->GetChild(int(slot++)));
        label->renderer = renderer;
        label->depth = text.depth == Debug::DepthMode::DepthTest ? depth : -1;
        label->SetClass("label", true);
        if (labelTexts[slot - 1] != text.value)
        {
            label->SetInnerRML(Rml::StringUtilities::EncodeRml(text.value));
            labelTexts[slot - 1] = text.value;
        }
        label->SetProperty("display", "block");
        label->SetProperty("font-size", std::to_string(text.size) + "px");
        label->SetProperty("color", TextColor(text.color));
        label->SetProperty("left", std::to_string(pixel.x()) + "px");
        label->SetProperty("top", std::to_string(pixel.y()) + "px");
        label->SetProperty("width", std::to_string(float(rect.width) - pixel.x()) + "px");
    }
    // 复用有界 DOM 槽；文字只作为转义文本，不允许脚本注入样式、图片或事件。
    for (int index = int(slot); index < root->GetNumChildren(); ++index) root->GetChild(index)->SetProperty("display", "none");
    context->Update();
    context->Render();
    renderer->SetDebugDepth(-1);
}
}
