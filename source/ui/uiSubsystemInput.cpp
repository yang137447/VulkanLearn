#include "ui/uiSubsystem.h"
#include "ui/debugDrawOverlay.h"
#include "ui/gameplayHudView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <nlohmann/json.hpp>


#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeCommand.h"
#include "core/renderSession.h"
#include "platform/platformEvent.h"
#include "platform/platformWindow.h"
#include "ui/sceneObjectTypeSupport.h"
#include "ui/uiBindingUpdateScope.h"
#include "Profiler.h"

namespace VL
{

namespace
{

constexpr float GamepadNavigationThreshold = 0.55f;

Rml::Input::KeyIdentifier ToRmlKey(PlatformKey key)
{
    switch (key)
    {
    case PlatformKey::Tab: return Rml::Input::KI_TAB;
    case PlatformKey::Left: return Rml::Input::KI_LEFT;
    case PlatformKey::Right: return Rml::Input::KI_RIGHT;
    case PlatformKey::Up: return Rml::Input::KI_UP;
    case PlatformKey::Down: return Rml::Input::KI_DOWN;
    case PlatformKey::PageUp: return Rml::Input::KI_PRIOR;
    case PlatformKey::PageDown: return Rml::Input::KI_NEXT;
    case PlatformKey::Home: return Rml::Input::KI_HOME;
    case PlatformKey::End: return Rml::Input::KI_END;
    case PlatformKey::Insert: return Rml::Input::KI_INSERT;
    case PlatformKey::Delete: return Rml::Input::KI_DELETE;
    case PlatformKey::Backspace: return Rml::Input::KI_BACK;
    case PlatformKey::Space: return Rml::Input::KI_SPACE;
    case PlatformKey::Enter: return Rml::Input::KI_RETURN;
    case PlatformKey::Escape: return Rml::Input::KI_ESCAPE;
    case PlatformKey::A: return Rml::Input::KI_A;
    case PlatformKey::S: return Rml::Input::KI_S;
    case PlatformKey::C: return Rml::Input::KI_C;
    case PlatformKey::V: return Rml::Input::KI_V;
    case PlatformKey::X: return Rml::Input::KI_X;
    case PlatformKey::Y: return Rml::Input::KI_Y;
    case PlatformKey::Z: return Rml::Input::KI_Z;
    case PlatformKey::F1: return Rml::Input::KI_F1;
    case PlatformKey::F10: return Rml::Input::KI_F10;
    case PlatformKey::F11: return Rml::Input::KI_F11;
    case PlatformKey::Num0: return Rml::Input::KI_0;
    case PlatformKey::Num1: return Rml::Input::KI_1;
    case PlatformKey::Num2: return Rml::Input::KI_2;
    case PlatformKey::Num3: return Rml::Input::KI_3;
    case PlatformKey::Num4: return Rml::Input::KI_4;
    case PlatformKey::Num5: return Rml::Input::KI_5;
    case PlatformKey::Num6: return Rml::Input::KI_6;
    case PlatformKey::Num7: return Rml::Input::KI_7;
    case PlatformKey::Num8: return Rml::Input::KI_8;
    case PlatformKey::Num9: return Rml::Input::KI_9;
    default: return Rml::Input::KI_UNKNOWN;
    }
}

int ToRmlModifiers(const PlatformEvent& event)
{
    int modifiers = 0;
    if (event.control) modifiers |= Rml::Input::KM_CTRL;
    if (event.shift) modifiers |= Rml::Input::KM_SHIFT;
    if (event.alt) modifiers |= Rml::Input::KM_ALT;
    if (event.super) modifiers |= Rml::Input::KM_META;
    return modifiers;
}

int ToRmlMouseButton(PlatformMouseButton button)
{
    switch (button)
    {
    case PlatformMouseButton::Left: return 0;
    case PlatformMouseButton::Right: return 1;
    case PlatformMouseButton::Middle: return 2;
    case PlatformMouseButton::X1: return 3;
    case PlatformMouseButton::X2: return 4;
    default: return -1;
    }
}

// 这些事件交给 RmlUi 后可能改变 hover/pressed/focus/滚动/文本或拖动状态；
// 第 3 层调度必须在同一帧强制一次上下文更新，不能只等 10 Hz 兜底。
bool IsRmlInputEvent(PlatformEventType type)
{
    switch (type)
    {
    case PlatformEventType::KeyDown:
    case PlatformEventType::KeyUp:
    case PlatformEventType::TextInput:
    case PlatformEventType::TextEditing:
    case PlatformEventType::MouseMove:
    case PlatformEventType::MouseButtonDown:
    case PlatformEventType::MouseButtonUp:
    case PlatformEventType::MouseWheel:
    case PlatformEventType::MouseLeave:
    case PlatformEventType::GamepadButtonDown:
    case PlatformEventType::GamepadButtonUp:
    case PlatformEventType::GamepadAxisMotion:
        return true;
    default:
        return false;
    }
}

Rml::Input::KeyIdentifier ToRmlGamepadKey(PlatformGamepadButton button)
{
    switch (button)
    {
    case PlatformGamepadButton::South: return Rml::Input::KI_RETURN;
    case PlatformGamepadButton::East: return Rml::Input::KI_ESCAPE;
    case PlatformGamepadButton::DpadUp: return Rml::Input::KI_UP;
    case PlatformGamepadButton::DpadDown: return Rml::Input::KI_DOWN;
    case PlatformGamepadButton::DpadLeft: return Rml::Input::KI_LEFT;
    case PlatformGamepadButton::DpadRight: return Rml::Input::KI_RIGHT;
    default: return Rml::Input::KI_UNKNOWN;
    }
}

} // namespace

bool UiSubsystem::IsDebugDrawOverlayVisible() const
{
    if (!lastViewModel.debugDraw || lastViewModel.offlineMode)
    {
        return false;
    }
    // 与 BuildAndPublishRenderSnapshot 的追加条件保持同一份判定。
    if (!developerUiVisible)
    {
        return true;
    }
    return editorUiShell != nullptr && editorUiShell->IsMotionDebugViewport();
}

bool UiSubsystem::IsDebugDrawOverlayAnimating() const
{
    if (!IsDebugDrawOverlayVisible())
    {
        return false;
    }
    const Debug::Snapshot& view = *lastViewModel.debugDraw;
    if (!view.lines.empty() || !view.points.empty() || !view.texts.empty())
    {
        return true;
    }
    // 上一份快照含调试几何、这一帧已过期清空：必须重发一次空叠层。
    return lastPublishedHadDebugOverlay;
}

void UiSubsystem::UpdateInputOwnership()
{
    inputOwnership.mode = developerUiVisible ? UiInputMode::GameAndUi : UiInputMode::GameOnly;
    inputOwnership.keyboardOwner = UiInputOwner::Game;
    inputOwnership.pointerOwner = UiInputOwner::Game;
    inputOwnership.controllerOwner = UiInputOwner::Game;
    inputOwnership.textInputOwner = UiInputOwner::None;

    if (developerUiVisible && editorUiShell != nullptr)
    {
        if (editorUiShell->IsAssetEditorVisible())
            editorUiShell->SetSceneViewportInputActive(false);
        const bool sceneViewportInputActive =
            editorUiShell->IsSceneViewportInputActive();
        const auto* hit = rmlContext->GetElementAtPoint(Rml::Vector2f(pointerX, pointerY));
        const auto* focus = rmlContext->GetFocusElement();
        const bool modal = editorUiShell->HasModal();
        const bool editorFocus = editorUiShell->IsEditorDocumentElement(focus);
        const bool sceneHit = editorUiShell->IsSceneViewportElement(hit);
        const bool dragging = editorUiShell->IsWorkspaceDragging();
        if (!sceneViewportInputActive &&
            (modal || dragging || editorUiShell->IsPointerCaptured() ||
             (hit != nullptr && editorUiShell->IsEditorDocumentElement(hit) &&
              !sceneHit))
            )
            inputOwnership.pointerOwner = UiInputOwner::DeveloperUi;
        if (modal || (!sceneViewportInputActive && editorFocus))
        {
            inputOwnership.keyboardOwner = UiInputOwner::DeveloperUi;
            if (gamepadConnected) inputOwnership.controllerOwner = UiInputOwner::DeveloperUi;
        }
        if (editorFocus && dynamic_cast<const Rml::ElementFormControl*>(focus) != nullptr)
            inputOwnership.textInputOwner = UiInputOwner::DeveloperUi;
    }
}

bool UiSubsystem::PressRmlKey(Rml::Input::KeyIdentifier key, bool repeat, int modifiers)
{
    if (rmlContext == nullptr || key == Rml::Input::KI_UNKNOWN)
    {
        return true;
    }

    if (repeat)
    {
        return rmlContext->ProcessKeyDown(key, modifiers);
    }

    auto keyIt = rmlKeyPressCounts.find(key);
    if (keyIt != rmlKeyPressCounts.end())
    {
        ++keyIt->second;
        return true;
    }

    rmlKeyPressCounts.emplace(key, 1u);
    return rmlContext->ProcessKeyDown(key, modifiers);
}

bool UiSubsystem::ReleaseRmlKey(Rml::Input::KeyIdentifier key, int modifiers)
{
    if (rmlContext == nullptr || key == Rml::Input::KI_UNKNOWN)
    {
        return true;
    }

    auto keyIt = rmlKeyPressCounts.find(key);
    if (keyIt == rmlKeyPressCounts.end())
    {
        return true;
    }
    if (keyIt->second > 1u)
    {
        --keyIt->second;
        return true;
    }

    rmlKeyPressCounts.erase(keyIt);
    return rmlContext->ProcessKeyUp(key, modifiers);
}

bool UiSubsystem::SetRmlAxisDirection(
    bool pressed,
    bool& currentState,
    Rml::Input::KeyIdentifier key)
{
    if (currentState == pressed)
    {
        return true;
    }

    currentState = pressed;
    return pressed ? PressRmlKey(key, false) : ReleaseRmlKey(key);
}

bool UiSubsystem::ProcessRmlGamepadAxisEvent(const PlatformEvent& event)
{
    bool propagates = true;
    if (event.gamepadAxis == PlatformGamepadAxis::LeftX)
    {
        propagates = SetRmlAxisDirection(
            event.gamepadAxisValue <= -GamepadNavigationThreshold,
            rmlAxisLeftDown,
            Rml::Input::KI_LEFT) && propagates;
        propagates = SetRmlAxisDirection(
            event.gamepadAxisValue >= GamepadNavigationThreshold,
            rmlAxisRightDown,
            Rml::Input::KI_RIGHT) && propagates;
    }
    else if (event.gamepadAxis == PlatformGamepadAxis::LeftY)
    {
        propagates = SetRmlAxisDirection(
            event.gamepadAxisValue <= -GamepadNavigationThreshold,
            rmlAxisUpDown,
            Rml::Input::KI_UP) && propagates;
        propagates = SetRmlAxisDirection(
            event.gamepadAxisValue >= GamepadNavigationThreshold,
            rmlAxisDownDown,
            Rml::Input::KI_DOWN) && propagates;
    }
    return !propagates;
}

void UiSubsystem::ResetRuntimeInputState()
{
    if (editorUiShell != nullptr)
        editorUiShell->CancelTransientInteraction();
    ResetRmlInputState();
}

void UiSubsystem::ResetRmlInputState()
{
    if (rmlContext != nullptr)
    {
        // 释放按键/鼠标并离开：hover/pressed 状态已变化，下一帧必须更新。
        uiUpdateScheduler.MarkInputEvent();
        for (const auto& keyEntry : rmlKeyPressCounts)
        {
            rmlContext->ProcessKeyUp(keyEntry.first, 0);
        }
        for (PlatformMouseButton mouseButton : rmlPressedMouseButtons)
        {
            const int button = ToRmlMouseButton(mouseButton);
            if (button >= 0)
            {
                rmlContext->ProcessMouseButtonUp(button, 0);
            }
        }
        rmlContext->ProcessMouseLeave();
    }

    rmlTextInputHandler.CancelComposition();
    rmlKeyPressCounts.clear();
    rmlPressedKeyboardKeys.clear();
    rmlPressedMouseButtons.clear();
    rmlPressedGamepadButtons.clear();
    rmlAxisLeftDown = false;
    rmlAxisRightDown = false;
    rmlAxisUpDown = false;
    rmlAxisDownDown = false;
}

void UiSubsystem::ResetRmlGamepadInputState()
{
    for (PlatformGamepadButton gamepadButton : rmlPressedGamepadButtons)
    {
        ReleaseRmlKey(ToRmlGamepadKey(gamepadButton));
    }
    rmlPressedGamepadButtons.clear();
    SetRmlAxisDirection(false, rmlAxisLeftDown, Rml::Input::KI_LEFT);
    SetRmlAxisDirection(false, rmlAxisRightDown, Rml::Input::KI_RIGHT);
    SetRmlAxisDirection(false, rmlAxisUpDown, Rml::Input::KI_UP);
    SetRmlAxisDirection(false, rmlAxisDownDown, Rml::Input::KI_DOWN);
}

bool UiSubsystem::ProcessRuntimeUiEvent(const PlatformEvent& event)
{
    if (rmlContext == nullptr)
    {
        return false;
    }

    if (IsRmlInputEvent(event.type))
    {
        uiUpdateScheduler.MarkInputEvent();
    }

    const int modifiers = ToRmlModifiers(event);
    bool propagates = true;
    switch (event.type)
    {
    case PlatformEventType::KeyDown:
    {
        const Rml::Input::KeyIdentifier key = ToRmlKey(event.key);
        bool processKey = event.repeat;
        if (!event.repeat)
        {
            processKey = rmlPressedKeyboardKeys.insert(event.key).second;
        }
        if (processKey)
        {
            propagates = PressRmlKey(key, event.repeat, modifiers);
            if (event.key == PlatformKey::Enter && !rmlTextInputHandler.IsComposing())
            {
                propagates = rmlContext->ProcessTextInput('\n') && propagates;
            }
        }
        break;
    }
    case PlatformEventType::KeyUp:
        if (rmlPressedKeyboardKeys.erase(event.key) > 0)
        {
            propagates = ReleaseRmlKey(ToRmlKey(event.key), modifiers);
        }
        break;
    case PlatformEventType::TextInput:
        if (rmlTextInputHandler.CommitComposition(event.text))
        {
            propagates = false;
        }
        else
        {
            propagates = rmlContext->ProcessTextInput(event.text);
        }
        break;
    case PlatformEventType::TextEditing:
        propagates = !rmlTextInputHandler.UpdateComposition(
            event.text,
            event.textEditingStart,
            event.textEditingLength);
        break;
    case PlatformEventType::MouseMove:
        propagates = rmlContext->ProcessMouseMove(
            static_cast<int>(std::lround(event.mouseX)),
            static_cast<int>(std::lround(event.mouseY)),
            modifiers);
        break;
    case PlatformEventType::MouseButtonDown:
    case PlatformEventType::MouseButtonUp:
    {
        const int mouseX = static_cast<int>(std::lround(event.mouseX));
        const int mouseY = static_cast<int>(std::lround(event.mouseY));
        const bool mouseMovePropagates = rmlContext->ProcessMouseMove(mouseX, mouseY, modifiers);
        const int button = ToRmlMouseButton(event.mouseButton);
        bool buttonPropagates = true;
        if (button >= 0 && event.type == PlatformEventType::MouseButtonDown)
        {
            if (rmlPressedMouseButtons.insert(event.mouseButton).second)
            {
                buttonPropagates = rmlContext->ProcessMouseButtonDown(button, modifiers);
            }
        }
        else if (button >= 0 && rmlPressedMouseButtons.erase(event.mouseButton) > 0)
        {
            buttonPropagates = rmlContext->ProcessMouseButtonUp(button, modifiers);
        }
        propagates = mouseMovePropagates && buttonPropagates;
        break;
    }
    case PlatformEventType::MouseWheel:
    {
        const bool mouseMovePropagates = rmlContext->ProcessMouseMove(
            static_cast<int>(std::lround(event.mouseX)),
            static_cast<int>(std::lround(event.mouseY)),
            modifiers);
        const bool wheelPropagates = rmlContext->ProcessMouseWheel(
            Rml::Vector2f(-event.wheelX, -event.wheelY),
            modifiers);
        propagates = mouseMovePropagates && wheelPropagates;
        break;
    }
    case PlatformEventType::MouseLeave:
        propagates = rmlContext->ProcessMouseLeave();
        break;
    case PlatformEventType::GamepadButtonDown:
    case PlatformEventType::GamepadButtonUp:
    {
        const Rml::Input::KeyIdentifier key = ToRmlGamepadKey(event.gamepadButton);
        if (key != Rml::Input::KI_UNKNOWN && event.type == PlatformEventType::GamepadButtonDown)
        {
            if (rmlPressedGamepadButtons.insert(event.gamepadButton).second)
            {
                propagates = PressRmlKey(key, false);
            }
        }
        else if (key != Rml::Input::KI_UNKNOWN &&
            rmlPressedGamepadButtons.erase(event.gamepadButton) > 0)
        {
            propagates = ReleaseRmlKey(key);
        }
        break;
    }
    case PlatformEventType::GamepadAxisMotion:
        return ProcessRmlGamepadAxisEvent(event);
    case PlatformEventType::WindowFocusLost:
        ResetRuntimeInputState();
        break;
    case PlatformEventType::WindowFocusGained:
        break;
    default:
        break;
    }
    return !propagates;
}

} // namespace VL
