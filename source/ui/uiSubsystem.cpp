#include "ui/uiSubsystem.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <nlohmann/json.hpp>

#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
#include <imgui.h>
#endif

#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeCommand.h"
#include "platform/platformEvent.h"
#include "platform/platformWindow.h"

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
    case PlatformKey::C: return Rml::Input::KI_C;
    case PlatformKey::V: return Rml::Input::KI_V;
    case PlatformKey::X: return Rml::Input::KI_X;
    case PlatformKey::Y: return Rml::Input::KI_Y;
    case PlatformKey::Z: return Rml::Input::KI_Z;
    case PlatformKey::F1: return Rml::Input::KI_F1;
    case PlatformKey::F10: return Rml::Input::KI_F10;
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

#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
constexpr float ImGuiGamepadDeadZone = 0.20f;

float NormalizeImGuiGamepadAxis(float magnitude)
{
    if (magnitude <= ImGuiGamepadDeadZone)
    {
        return 0.0f;
    }
    return (magnitude - ImGuiGamepadDeadZone) / (1.0f - ImGuiGamepadDeadZone);
}

ImGuiKey ToImGuiKey(PlatformKey key)
{
    switch (key)
    {
    case PlatformKey::Tab: return ImGuiKey_Tab;
    case PlatformKey::Left: return ImGuiKey_LeftArrow;
    case PlatformKey::Right: return ImGuiKey_RightArrow;
    case PlatformKey::Up: return ImGuiKey_UpArrow;
    case PlatformKey::Down: return ImGuiKey_DownArrow;
    case PlatformKey::PageUp: return ImGuiKey_PageUp;
    case PlatformKey::PageDown: return ImGuiKey_PageDown;
    case PlatformKey::Home: return ImGuiKey_Home;
    case PlatformKey::End: return ImGuiKey_End;
    case PlatformKey::Insert: return ImGuiKey_Insert;
    case PlatformKey::Delete: return ImGuiKey_Delete;
    case PlatformKey::Backspace: return ImGuiKey_Backspace;
    case PlatformKey::Space: return ImGuiKey_Space;
    case PlatformKey::Enter: return ImGuiKey_Enter;
    case PlatformKey::Escape: return ImGuiKey_Escape;
    case PlatformKey::A: return ImGuiKey_A;
    case PlatformKey::C: return ImGuiKey_C;
    case PlatformKey::V: return ImGuiKey_V;
    case PlatformKey::X: return ImGuiKey_X;
    case PlatformKey::Y: return ImGuiKey_Y;
    case PlatformKey::Z: return ImGuiKey_Z;
    case PlatformKey::F1: return ImGuiKey_F1;
    case PlatformKey::F10: return ImGuiKey_F10;
    case PlatformKey::Num0: return ImGuiKey_0;
    case PlatformKey::Num1: return ImGuiKey_1;
    case PlatformKey::Num2: return ImGuiKey_2;
    case PlatformKey::Num3: return ImGuiKey_3;
    case PlatformKey::Num4: return ImGuiKey_4;
    case PlatformKey::Num5: return ImGuiKey_5;
    case PlatformKey::Num6: return ImGuiKey_6;
    case PlatformKey::Num7: return ImGuiKey_7;
    case PlatformKey::Num8: return ImGuiKey_8;
    case PlatformKey::Num9: return ImGuiKey_9;
    default: return ImGuiKey_None;
    }
}

ImGuiKey ToImGuiGamepadButton(PlatformGamepadButton button)
{
    switch (button)
    {
    case PlatformGamepadButton::South: return ImGuiKey_GamepadFaceDown;
    case PlatformGamepadButton::East: return ImGuiKey_GamepadFaceRight;
    case PlatformGamepadButton::West: return ImGuiKey_GamepadFaceLeft;
    case PlatformGamepadButton::North: return ImGuiKey_GamepadFaceUp;
    case PlatformGamepadButton::Back: return ImGuiKey_GamepadBack;
    case PlatformGamepadButton::Start: return ImGuiKey_GamepadStart;
    case PlatformGamepadButton::LeftShoulder: return ImGuiKey_GamepadL1;
    case PlatformGamepadButton::RightShoulder: return ImGuiKey_GamepadR1;
    case PlatformGamepadButton::DpadUp: return ImGuiKey_GamepadDpadUp;
    case PlatformGamepadButton::DpadDown: return ImGuiKey_GamepadDpadDown;
    case PlatformGamepadButton::DpadLeft: return ImGuiKey_GamepadDpadLeft;
    case PlatformGamepadButton::DpadRight: return ImGuiKey_GamepadDpadRight;
    default: return ImGuiKey_None;
    }
}
#endif

std::vector<std::filesystem::path> BuildPlatformFontCandidates()
{
    std::vector<std::filesystem::path> fonts;
#if defined(_WIN32)
    fonts.emplace_back("C:/Windows/Fonts/segoeui.ttf");
    fonts.emplace_back("C:/Windows/Fonts/NotoSansSC-VF.ttf");
    fonts.emplace_back("C:/Windows/Fonts/msyh.ttc");
    fonts.emplace_back("C:/Windows/Fonts/arial.ttf");
#elif defined(__APPLE__)
    fonts.emplace_back("/System/Library/Fonts/SFNS.ttf");
    fonts.emplace_back("/System/Library/Fonts/PingFang.ttc");
#else
    fonts.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    fonts.emplace_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
#endif
    return fonts;
}

uint64_t GetFileFingerprint(const std::filesystem::directory_entry& entry)
{
    const uint64_t writeTime = static_cast<uint64_t>(entry.last_write_time().time_since_epoch().count());
    const uint64_t size = entry.is_regular_file() ? static_cast<uint64_t>(entry.file_size()) : 0;
    return writeTime ^ (size + 0x9e3779b97f4a7c15ull + (writeTime << 6u) + (writeTime >> 2u));
}

} // namespace

UiSubsystem::~UiSubsystem()
{
    Shutdown();
}

RuntimeResult<void> UiSubsystem::Initialize(
    const UiSubsystemDesc& initializeDesc,
    PlatformWindow& platformWindow,
    CommandBus& initializeCommandBus,
    const DiagnosticsSubsystem& diagnosticsSubsystem)
{
    Shutdown();
    if (initializeDesc.viewportWidth == 0 || initializeDesc.viewportHeight == 0)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.InvalidViewport",
            "UI viewport dimensions must be positive."));
    }
    if (!std::filesystem::exists(initializeDesc.assetRoot))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.AssetRootMissing",
            "UI asset root does not exist.",
            initializeDesc.assetRoot.string()));
    }

    desc = initializeDesc;
    window = &platformWindow;
    commandBus = &initializeCommandBus;
    diagnostics = &diagnosticsSubsystem;
    currentLocale = desc.defaultLocale;
    developerUiEnabled = desc.developerUiEnabled;
    developerUiVisible = developerUiEnabled && desc.developerUiVisible;
    nextHotReloadCheck = std::chrono::steady_clock::now();

    RuntimeResult<void> rmlResult = InitializeRmlUi();
    if (rmlResult.IsFailure())
    {
        Shutdown();
        return rmlResult;
    }

    RuntimeResult<void> dataModelResult = InitializeDataModel();
    if (dataModelResult.IsFailure())
    {
        Shutdown();
        return dataModelResult;
    }

    RuntimeResult<void> fontResult = LoadFonts();
    if (fontResult.IsFailure())
    {
        Shutdown();
        return fontResult;
    }

    RuntimeResult<void> assetResult = CommitInitialAssets();
    if (assetResult.IsFailure())
    {
        Shutdown();
        return assetResult;
    }

    RuntimeResult<void> developerResult = InitializeDeveloperUi();
    if (developerResult.IsFailure())
    {
        Shutdown();
        return developerResult;
    }

    initialized = true;
    UpdateInputOwnership();
    diagnostics->ReportInfo(
        "UI subsystem initialized: RmlUi runtime UI enabled, developer UI " +
        std::string(developerUiEnabled ? "enabled" : "disabled"));
    return RuntimeResult<void>::Success();
}

void UiSubsystem::Shutdown()
{
    renderSnapshotQueue.Clear();
    if (rmlContext != nullptr)
    {
        ResetRuntimeInputState();
    }

#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    if (imguiInitialized)
    {
        if (imguiFontTextureId != 0)
        {
            rmlRenderInterface.ReleaseTexture(static_cast<Rml::TextureHandle>(imguiFontTextureId));
            imguiFontTextureId = 0;
        }
        ImGui::DestroyContext();
        imguiInitialized = false;
    }
#endif

    if (rmlContext != nullptr && runtimeDocument != nullptr)
    {
        rmlContext->UnloadDocument(runtimeDocument);
        runtimeDocument = nullptr;
        rmlContext->Update();
    }
    if (rmlContext != nullptr)
    {
        Rml::RemoveContext(rmlContext->GetName());
        rmlContext = nullptr;
    }
    if (rmlInitialized)
    {
        Rml::Shutdown();
        rmlInitialized = false;
    }

    if (window != nullptr)
    {
        window->SetTextInputEnabled(false);
    }
    window = nullptr;
    commandBus = nullptr;
    diagnostics = nullptr;
    localizations.clear();
    assetFingerprint.clear();
    dataModelHandle = Rml::DataModelHandle();
    initialized = false;
    runtimePageVisible = false;
    developerUiVisible = false;
    developerUiEnabled = false;
    hotReloadStatus.clear();
    rmlKeyPressCounts.clear();
    rmlPressedKeyboardKeys.clear();
    rmlPressedMouseButtons.clear();
    rmlPressedGamepadButtons.clear();
    gamepadConnected = false;
    rmlAxisLeftDown = false;
    rmlAxisRightDown = false;
    rmlAxisUpDown = false;
    rmlAxisDownDown = false;
}

bool UiSubsystem::HandlePlatformEvent(const PlatformEvent& event)
{
    if (!initialized)
    {
        return false;
    }

    if (event.type == PlatformEventType::WindowResized)
    {
        Resize(event.width, event.height);
        return false;
    }

    if (event.type == PlatformEventType::GamepadConnected ||
        event.type == PlatformEventType::GamepadDisconnected)
    {
        if (event.type == PlatformEventType::GamepadDisconnected)
        {
            ResetRmlGamepadInputState();
            ResetDeveloperGamepadInputState();
        }
        gamepadConnected = event.gamepadConnected;
        UpdateDeveloperGamepadCapability();
        UpdateInputOwnership();
        return false;
    }

    if (event.type == PlatformEventType::WindowFocusGained ||
        event.type == PlatformEventType::WindowFocusLost)
    {
        ProcessRuntimeUiEvent(event);
        ProcessDeveloperUiEvent(event);
        UpdateInputOwnership();
        return false;
    }

    if (HandleGlobalShortcut(event))
    {
        return true;
    }

    bool consumed = false;
    if (runtimePageVisible)
    {
        consumed = ProcessRuntimeUiEvent(event);
        if (!consumed)
        {
            switch (event.type)
            {
            case PlatformEventType::KeyDown:
            case PlatformEventType::KeyUp:
            case PlatformEventType::TextInput:
            case PlatformEventType::TextEditing:
            case PlatformEventType::MouseMove:
            case PlatformEventType::MouseButtonDown:
            case PlatformEventType::MouseButtonUp:
            case PlatformEventType::MouseWheel:
            case PlatformEventType::GamepadButtonDown:
            case PlatformEventType::GamepadButtonUp:
            case PlatformEventType::GamepadAxisMotion:
                consumed = true;
                break;
            default:
                break;
            }
        }
    }

    if (!consumed && developerUiVisible)
    {
        consumed = ProcessDeveloperUiEvent(event);
    }

    UpdateInputOwnership();
    return consumed;
}

void UiSubsystem::Update(const UiViewModelSnapshot& viewModelSnapshot)
{
    if (!initialized)
    {
        return;
    }

    lastViewModel = viewModelSnapshot;
    lastViewModel.runtimePageVisible = runtimePageVisible;
    lastViewModel.developerUiVisible = developerUiVisible;
    lastViewModel.locale = currentLocale;

    TryHotReload();
    SyncBindingData();
    dataModelHandle.DirtyAllVariables();
    rmlContext->Update();
    BuildAndPublishRenderSnapshot();
    UpdateInputOwnership();
}

void UiSubsystem::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    desc.viewportWidth = width;
    desc.viewportHeight = height;
    if (rmlContext != nullptr)
    {
        rmlContext->SetDimensions(Rml::Vector2i(static_cast<int>(width), static_cast<int>(height)));
    }
}

void UiSubsystem::ApplyAction(const UiAction& action)
{
    switch (action.type)
    {
    case UiActionType::ToggleRuntimePage:
        runtimePageVisible = !runtimePageVisible;
        if (!runtimePageVisible)
        {
            ResetRuntimeInputState();
        }
        ApplyDocumentState();
        break;
    case UiActionType::CloseRuntimePage:
        ResetRuntimeInputState();
        runtimePageVisible = false;
        ApplyDocumentState();
        break;
    case UiActionType::ToggleDeveloperUi:
        if (developerUiEnabled)
        {
            developerUiVisible = !developerUiVisible;
            if (!developerUiVisible)
            {
                ResetDeveloperInputState();
            }
        }
        break;
    case UiActionType::SetLocale:
        if (localizations.find(action.stringValue) != localizations.end())
        {
            currentLocale = action.stringValue;
            SyncBindingData();
            dataModelHandle.DirtyAllVariables();
        }
        break;
    default:
        break;
    }
    UpdateInputOwnership();
}

bool UiSubsystem::ShouldUseRelativeMouseModeForGame() const
{
    return initialized && !runtimePageVisible && !developerUiVisible;
}

bool UiSubsystem::ShouldGameReceiveKeyboard() const
{
    return !initialized || inputOwnership.keyboardOwner == UiInputOwner::Game ||
        inputOwnership.keyboardOwner == UiInputOwner::None;
}

bool UiSubsystem::ShouldGameReceivePointer() const
{
    return !initialized || inputOwnership.pointerOwner == UiInputOwner::Game ||
        inputOwnership.pointerOwner == UiInputOwner::None;
}

RuntimeResult<void> UiSubsystem::InitializeRmlUi()
{
    rmlSystemInterface.Initialize(*window, *diagnostics);
    Rml::SetSystemInterface(&rmlSystemInterface);
    Rml::SetRenderInterface(&rmlRenderInterface);
    if (!Rml::Initialise())
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.RmlInitializeFailed",
            "RmlUi failed to initialize."));
    }
    rmlInitialized = true;

    rmlContext = Rml::CreateContext(
        "vulkanlearn",
        Rml::Vector2i(
            static_cast<int>(desc.viewportWidth),
            static_cast<int>(desc.viewportHeight)),
        &rmlRenderInterface,
        &rmlTextInputHandler);
    if (rmlContext == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.RmlContextFailed",
            "RmlUi failed to create the runtime context."));
    }
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> UiSubsystem::InitializeDataModel()
{
    Rml::DataModelConstructor constructor = rmlContext->CreateDataModel("game_ui");
    if (!constructor)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.DataModelFailed",
            "RmlUi failed to create the game_ui data model."));
    }

    bool bound = true;
    bound = constructor.Bind("active_section", &bindingData.activeSection) && bound;
    bound = constructor.Bind("world_text", &bindingData.worldText) && bound;
    bound = constructor.Bind("panel_title", &bindingData.panelTitle) && bound;
    bound = constructor.Bind("panel_subtitle", &bindingData.panelSubtitle) && bound;
    bound = constructor.Bind("close_label", &bindingData.closeLabel) && bound;
    bound = constructor.Bind("quit_label", &bindingData.quitLabel) && bound;

    bound = constructor.Bind("tab_overview_label", &bindingData.tabOverviewLabel) && bound;
    bound = constructor.Bind("tab_visualization_label", &bindingData.tabVisualizationLabel) && bound;
    bound = constructor.Bind("tab_post_process_label", &bindingData.tabPostProcessLabel) && bound;
    bound = constructor.Bind("tab_environment_label", &bindingData.tabEnvironmentLabel) && bound;
    bound = constructor.Bind("tab_shadows_label", &bindingData.tabShadowsLabel) && bound;
    bound = constructor.Bind("tab_system_label", &bindingData.tabSystemLabel) && bound;

    bound = constructor.Bind("frame_label", &bindingData.frameLabel) && bound;
    bound = constructor.Bind("frame_value", &bindingData.frameValue) && bound;
    bound = constructor.Bind("fps_label", &bindingData.fpsLabel) && bound;
    bound = constructor.Bind("fps_value", &bindingData.fpsValue) && bound;
    bound = constructor.Bind("scene_label", &bindingData.sceneLabel) && bound;
    bound = constructor.Bind("input_mode_label", &bindingData.inputModeLabel) && bound;
    bound = constructor.Bind("input_mode_value", &bindingData.inputModeValue) && bound;
    bound = constructor.Bind("wind_profiles_label", &bindingData.windProfilesLabel) && bound;
    bound = constructor.Bind("wind_profiles_value", &bindingData.windProfilesValue) && bound;
    bound = constructor.Bind("hot_reload_label", &bindingData.hotReloadLabel) && bound;
    bound = constructor.Bind("hot_reload_status", &bindingData.hotReloadStatus) && bound;

    bound = constructor.Bind("debug_view_label", &bindingData.debugViewLabel) && bound;
    bound = constructor.Bind("debug_view_value", &bindingData.debugViewValue) && bound;
    bound = constructor.Bind("debug_view_mode", &bindingData.debugViewMode) && bound;
    bound = constructor.Bind("debug_full_label", &bindingData.debugFullLabel) && bound;
    bound = constructor.Bind("debug_base_color_label", &bindingData.debugBaseColorLabel) && bound;
    bound = constructor.Bind("debug_emissive_label", &bindingData.debugEmissiveLabel) && bound;
    bound = constructor.Bind("debug_normal_label", &bindingData.debugNormalLabel) && bound;
    bound = constructor.Bind("debug_roughness_label", &bindingData.debugRoughnessLabel) && bound;
    bound = constructor.Bind("debug_metallic_label", &bindingData.debugMetallicLabel) && bound;
    bound = constructor.Bind("debug_ao_label", &bindingData.debugAoLabel) && bound;
    bound = constructor.Bind("debug_shadow_label", &bindingData.debugShadowLabel) && bound;
    bound = constructor.Bind("debug_direct_lighting_label", &bindingData.debugDirectLightingLabel) && bound;
    bound = constructor.Bind("debug_indirect_diffuse_label", &bindingData.debugIndirectDiffuseLabel) && bound;
    bound = constructor.Bind("debug_indirect_specular_label", &bindingData.debugIndirectSpecularLabel) && bound;
    bound = constructor.Bind("debug_shadow_cascade_label", &bindingData.debugShadowCascadeLabel) && bound;
    bound = constructor.Bind("debug_shading_model_label", &bindingData.debugShadingModelLabel) && bound;
    bound = constructor.Bind("debug_subsurface_weight_label", &bindingData.debugSubsurfaceWeightLabel) && bound;
    bound = constructor.Bind("debug_transmission_weight_label", &bindingData.debugTransmissionWeightLabel) && bound;
    bound = constructor.Bind("debug_subsurface_asset_id_label", &bindingData.debugSubsurfaceAssetIdLabel) && bound;
    bound = constructor.Bind("debug_local_subsurface_label", &bindingData.debugLocalSubsurfaceLabel) && bound;
    bound = constructor.Bind("debug_diffuse_before_sss_label", &bindingData.debugDiffuseBeforeSssLabel) && bound;
    bound = constructor.Bind("debug_diffuse_after_sss_label", &bindingData.debugDiffuseAfterSssLabel) && bound;
    bound = constructor.Bind("debug_sss_pixel_radius_label", &bindingData.debugSssPixelRadiusLabel) && bound;
    bound = constructor.Bind("debug_sss_valid_weight_label", &bindingData.debugSssValidWeightLabel) && bound;
    bound = constructor.Bind("debug_hair_world_tangent_label", &bindingData.debugHairWorldTangentLabel) && bound;
    bound = constructor.Bind("debug_hair_tangent_rootward_label", &bindingData.debugHairTangentRootwardLabel) && bound;
    bound = constructor.Bind("debug_hair_theta_io_label", &bindingData.debugHairThetaIoLabel) && bound;
    bound = constructor.Bind("debug_hair_delta_phi_label", &bindingData.debugHairDeltaPhiLabel) && bound;
    bound = constructor.Bind("debug_hair_r_label", &bindingData.debugHairRLabel) && bound;
    bound = constructor.Bind("debug_hair_tt_label", &bindingData.debugHairTtLabel) && bound;
    bound = constructor.Bind("debug_hair_trt_label", &bindingData.debugHairTrtLabel) && bound;
    bound = constructor.Bind("debug_hair_path_length_label", &bindingData.debugHairPathLengthLabel) && bound;
    bound = constructor.Bind("debug_hair_absorption_label", &bindingData.debugHairAbsorptionLabel) && bound;
    bound = constructor.Bind("debug_hair_coverage_label", &bindingData.debugHairCoverageLabel) && bound;
    bound = constructor.Bind("debug_hair_shadow_transmittance_label", &bindingData.debugHairShadowTransmittanceLabel) && bound;
    bound = constructor.Bind("debug_hair_lut_coordinates_label", &bindingData.debugHairLutCoordinatesLabel) && bound;
    bound = constructor.Bind("debug_hair_primary_highlight_label", &bindingData.debugHairPrimaryHighlightLabel) && bound;
    bound = constructor.Bind("debug_hair_secondary_highlight_label", &bindingData.debugHairSecondaryHighlightLabel) && bound;
    bound = constructor.Bind("debug_hair_scatter_label", &bindingData.debugHairScatterLabel) && bound;
    bound = constructor.Bind("debug_hair_backlit_label", &bindingData.debugHairBacklitLabel) && bound;
    bound = constructor.Bind("debug_hair_r_path_color_label", &bindingData.debugHairRPathColorLabel) && bound;
    bound = constructor.Bind("debug_hair_tt_path_color_label", &bindingData.debugHairTtPathColorLabel) && bound;
    bound = constructor.Bind("debug_hair_trt_path_color_label", &bindingData.debugHairTrtPathColorLabel) && bound;
    bound = constructor.Bind("debug_hair_ibl_fallback_label", &bindingData.debugHairIblFallbackLabel) && bound;
    bound = constructor.Bind("debug_hair_ms_fallback_label", &bindingData.debugHairMultipleScatteringFallbackLabel) && bound;
    bound = constructor.Bind("debug_cloth_model_label", &bindingData.debugClothModelLabel) && bound;
    bound = constructor.Bind("debug_cloth_sheen_color_label", &bindingData.debugClothSheenColorLabel) && bound;
    bound = constructor.Bind("debug_cloth_sheen_roughness_label", &bindingData.debugClothSheenRoughnessLabel) && bound;
    bound = constructor.Bind("debug_cloth_charlie_d_label", &bindingData.debugClothCharlieDLabel) && bound;
    bound = constructor.Bind("debug_cloth_neubelt_visibility_label", &bindingData.debugClothNeubeltVisibilityLabel) && bound;
    bound = constructor.Bind("debug_cloth_directional_albedo_label", &bindingData.debugClothDirectionalAlbedoLabel) && bound;
    bound = constructor.Bind("debug_cloth_base_energy_scale_label", &bindingData.debugClothBaseEnergyScaleLabel) && bound;
    bound = constructor.Bind("debug_cloth_direct_sheen_label", &bindingData.debugClothDirectSheenLabel) && bound;
    bound = constructor.Bind("debug_cloth_indirect_sheen_label", &bindingData.debugClothIndirectSheenLabel) && bound;
    bound = constructor.Bind("debug_cloth_ibl_fallback_label", &bindingData.debugClothIblFallbackLabel) && bound;

    bound = constructor.Bind("tone_mapping_label", &bindingData.toneMappingLabel) && bound;
    bound = constructor.Bind("tone_mapping_value", &bindingData.toneMappingValue) && bound;
    bound = constructor.Bind("tone_mapping_mode", &bindingData.toneMappingMode) && bound;
    bound = constructor.Bind("tone_none_label", &bindingData.toneNoneLabel) && bound;
    bound = constructor.Bind("tone_reinhard_label", &bindingData.toneReinhardLabel) && bound;
    bound = constructor.Bind("tone_hable_label", &bindingData.toneHableLabel) && bound;
    bound = constructor.Bind("tone_aces_label", &bindingData.toneAcesLabel) && bound;
    bound = constructor.Bind("bloom_strength_label", &bindingData.bloomStrengthLabel) && bound;
    bound = constructor.Bind("bloom_strength_value", &bindingData.bloomStrengthValue) && bound;
    bound = constructor.Bind("bloom_strength", &bindingData.bloomStrength) && bound;
    bound = constructor.Bind("bloom_threshold_label", &bindingData.bloomThresholdLabel) && bound;
    bound = constructor.Bind("bloom_threshold_value", &bindingData.bloomThresholdValue) && bound;
    bound = constructor.Bind("bloom_threshold", &bindingData.bloomThreshold) && bound;
    bound = constructor.Bind("bloom_knee_label", &bindingData.bloomKneeLabel) && bound;
    bound = constructor.Bind("bloom_knee_value", &bindingData.bloomKneeValue) && bound;
    bound = constructor.Bind("bloom_knee", &bindingData.bloomKnee) && bound;
    bound = constructor.Bind("bloom_clamp_label", &bindingData.bloomClampLabel) && bound;
    bound = constructor.Bind("bloom_clamp_value", &bindingData.bloomClampValue) && bound;
    bound = constructor.Bind("bloom_clamp", &bindingData.bloomClamp) && bound;

    bound = constructor.Bind("shadow_cast_shadows_label", &bindingData.shadowCastShadowsLabel) && bound;
    bound = constructor.Bind("shadow_cast_shadows_value", &bindingData.shadowCastShadowsValue) && bound;
    bound = constructor.Bind("csm_cast_shadows", &bindingData.csmCastShadows) && bound;
    bound = constructor.Bind("shadow_enable_label", &bindingData.shadowEnableLabel) && bound;
    bound = constructor.Bind("shadow_disable_label", &bindingData.shadowDisableLabel) && bound;
    bound = constructor.Bind("shadow_dynamic_distance_label", &bindingData.shadowDynamicDistanceLabel) && bound;
    bound = constructor.Bind("shadow_dynamic_distance_value", &bindingData.shadowDynamicDistanceValue) && bound;
    bound = constructor.Bind("csm_dynamic_shadow_distance", &bindingData.csmDynamicShadowDistance) && bound;
    bound = constructor.Bind("shadow_dynamic_cascades_label", &bindingData.shadowDynamicCascadesLabel) && bound;
    bound = constructor.Bind("csm_dynamic_shadow_cascades", &bindingData.csmDynamicShadowCascades) && bound;
    bound = constructor.Bind("shadow_cascade_distribution_exponent_label", &bindingData.shadowCascadeDistributionExponentLabel) && bound;
    bound = constructor.Bind("shadow_cascade_distribution_exponent_value", &bindingData.shadowCascadeDistributionExponentValue) && bound;
    bound = constructor.Bind("csm_cascade_distribution_exponent", &bindingData.csmCascadeDistributionExponent) && bound;
    bound = constructor.Bind("shadow_cascade_transition_fraction_label", &bindingData.shadowCascadeTransitionFractionLabel) && bound;
    bound = constructor.Bind("shadow_cascade_transition_fraction_value", &bindingData.shadowCascadeTransitionFractionValue) && bound;
    bound = constructor.Bind("csm_cascade_transition_fraction", &bindingData.csmCascadeTransitionFraction) && bound;
    bound = constructor.Bind("shadow_distance_fadeout_fraction_label", &bindingData.shadowDistanceFadeoutFractionLabel) && bound;
    bound = constructor.Bind("shadow_distance_fadeout_fraction_value", &bindingData.shadowDistanceFadeoutFractionValue) && bound;
    bound = constructor.Bind("csm_shadow_distance_fadeout_fraction", &bindingData.csmShadowDistanceFadeoutFraction) && bound;
    bound = constructor.Bind("shadow_bias_label", &bindingData.shadowBiasLabel) && bound;
    bound = constructor.Bind("shadow_bias_value", &bindingData.shadowBiasValue) && bound;
    bound = constructor.Bind("csm_shadow_bias", &bindingData.csmShadowBias) && bound;
    bound = constructor.Bind("shadow_slope_bias_label", &bindingData.shadowSlopeBiasLabel) && bound;
    bound = constructor.Bind("shadow_slope_bias_value", &bindingData.shadowSlopeBiasValue) && bound;
    bound = constructor.Bind("csm_shadow_slope_bias", &bindingData.csmShadowSlopeBias) && bound;
    bound = constructor.Bind("shadow_cascade_bias_distribution_label", &bindingData.shadowCascadeBiasDistributionLabel) && bound;
    bound = constructor.Bind("shadow_cascade_bias_distribution_value", &bindingData.shadowCascadeBiasDistributionValue) && bound;
    bound = constructor.Bind("csm_shadow_cascade_bias_distribution", &bindingData.csmShadowCascadeBiasDistribution) && bound;
    bound = constructor.Bind("shadow_debug_label", &bindingData.shadowDebugLabel) && bound;
    bound = constructor.Bind("shadow_debug_full_label", &bindingData.shadowDebugFullLabel) && bound;
    bound = constructor.Bind("shadow_debug_cascades_label", &bindingData.shadowDebugCascadesLabel) && bound;
    bound = constructor.Bind("shadow_save_label", &bindingData.shadowSaveLabel) && bound;

    bound = constructor.Bind("environment_label", &bindingData.environmentLabel) && bound;
    bound = constructor.Bind("environment_value", &bindingData.environmentValue) && bound;
    bound = constructor.Bind("environment_intensity", &bindingData.environmentIntensity) && bound;
    bound = constructor.Bind("vegetation_heading", &bindingData.vegetationHeading) && bound;
    bound = constructor.Bind("wind_strength_label", &bindingData.windStrengthLabel) && bound;
    bound = constructor.Bind("wind_strength_value", &bindingData.windStrengthValue) && bound;
    bound = constructor.Bind("speedtree_strength", &bindingData.speedTreeStrength) && bound;
    bound = constructor.Bind("gusting_label", &bindingData.gustingLabel) && bound;
    bound = constructor.Bind("gusting_value", &bindingData.gustingValue) && bound;
    bound = constructor.Bind("speedtree_gusting_enabled", &bindingData.speedTreeGustingEnabled) && bound;
    bound = constructor.Bind("gust_on_label", &bindingData.gustOnLabel) && bound;
    bound = constructor.Bind("gust_off_label", &bindingData.gustOffLabel) && bound;
    bound = constructor.Bind("gust_once_label", &bindingData.gustOnceLabel) && bound;

    bound = constructor.Bind("locale_label", &bindingData.localeLabel) && bound;
    bound = constructor.Bind("locale_value", &bindingData.localeValue) && bound;
    bound = constructor.Bind("developer_ui_label", &bindingData.developerUiLabel) && bound;
    bound = constructor.Bind("developer_ui_value", &bindingData.developerUiValue) && bound;
    bound = constructor.Bind("developer_ui_toggle_label", &bindingData.developerUiToggleLabel) && bound;

    bound = constructor.BindEventCallback("close_page", &UiSubsystem::OnClosePage, this) && bound;
    bound = constructor.BindEventCallback("set_debug_view", &UiSubsystem::OnDebugViewSelected, this) && bound;
    bound = constructor.BindEventCallback("tone_none", &UiSubsystem::OnToneMappingNone, this) && bound;
    bound = constructor.BindEventCallback("tone_reinhard", &UiSubsystem::OnToneMappingReinhard, this) && bound;
    bound = constructor.BindEventCallback("tone_hable", &UiSubsystem::OnToneMappingHable, this) && bound;
    bound = constructor.BindEventCallback("tone_aces", &UiSubsystem::OnToneMappingAces, this) && bound;
    bound = constructor.BindEventCallback("bloom_strength_changed", &UiSubsystem::OnBloomStrengthChanged, this) && bound;
    bound = constructor.BindEventCallback("bloom_threshold_changed", &UiSubsystem::OnBloomThresholdChanged, this) && bound;
    bound = constructor.BindEventCallback("bloom_knee_changed", &UiSubsystem::OnBloomKneeChanged, this) && bound;
    bound = constructor.BindEventCallback("bloom_clamp_changed", &UiSubsystem::OnBloomClampChanged, this) && bound;
    bound = constructor.BindEventCallback("set_csm_cast_shadows", &UiSubsystem::OnCsmCastShadowsSelected, this) && bound;
    bound = constructor.BindEventCallback("csm_dynamic_shadow_distance_changed", &UiSubsystem::OnCsmDynamicShadowDistanceChanged, this) && bound;
    bound = constructor.BindEventCallback("set_csm_dynamic_shadow_cascades", &UiSubsystem::OnCsmDynamicShadowCascadesSelected, this) && bound;
    bound = constructor.BindEventCallback("csm_cascade_distribution_exponent_changed", &UiSubsystem::OnCsmCascadeDistributionExponentChanged, this) && bound;
    bound = constructor.BindEventCallback("csm_cascade_transition_fraction_changed", &UiSubsystem::OnCsmCascadeTransitionFractionChanged, this) && bound;
    bound = constructor.BindEventCallback("csm_shadow_distance_fadeout_fraction_changed", &UiSubsystem::OnCsmShadowDistanceFadeoutFractionChanged, this) && bound;
    bound = constructor.BindEventCallback("csm_shadow_bias_changed", &UiSubsystem::OnCsmShadowBiasChanged, this) && bound;
    bound = constructor.BindEventCallback("csm_shadow_slope_bias_changed", &UiSubsystem::OnCsmShadowSlopeBiasChanged, this) && bound;
    bound = constructor.BindEventCallback("csm_shadow_cascade_bias_distribution_changed", &UiSubsystem::OnCsmShadowCascadeBiasDistributionChanged, this) && bound;
    bound = constructor.BindEventCallback("save_csm_to_scene", &UiSubsystem::OnSaveCsmSettingsToScene, this) && bound;
    bound = constructor.BindEventCallback("environment_intensity_changed", &UiSubsystem::OnEnvironmentIntensityChanged, this) && bound;
    bound = constructor.BindEventCallback("speedtree_strength_changed", &UiSubsystem::OnSpeedTreeStrengthChanged, this) && bound;
    bound = constructor.BindEventCallback("set_speedtree_gusting", &UiSubsystem::OnSpeedTreeGustingSelected, this) && bound;
    bound = constructor.BindEventCallback("force_speedtree_gust", &UiSubsystem::OnForceSpeedTreeGust, this) && bound;
    bound = constructor.BindEventCallback("locale_en", &UiSubsystem::OnLocaleEnglish, this) && bound;
    bound = constructor.BindEventCallback("locale_zh", &UiSubsystem::OnLocaleChinese, this) && bound;
    bound = constructor.BindEventCallback("toggle_developer_ui", &UiSubsystem::OnToggleDeveloperUi, this) && bound;
    bound = constructor.BindEventCallback("quit", &UiSubsystem::OnQuit, this) && bound;

    if (!bound)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.DataModelBindingFailed",
            "One or more RmlUi game_ui data model bindings failed."));
    }

    dataModelHandle = constructor.GetModelHandle();
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> UiSubsystem::InitializeDeveloperUi()
{
#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    if (!developerUiEnabled)
    {
        return RuntimeResult<void>::Success();
    }

    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    if (context == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.ImGuiContextFailed",
            "Dear ImGui failed to create the developer UI context."));
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    unsigned char* fontPixels = nullptr;
    int fontWidth = 0;
    int fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    if (fontPixels == nullptr || fontWidth <= 0 || fontHeight <= 0)
    {
        ImGui::DestroyContext(context);
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.ImGuiFontAtlasFailed",
            "Dear ImGui failed to build its font atlas."));
    }

    const size_t fontByteCount =
        static_cast<size_t>(fontWidth) * static_cast<size_t>(fontHeight) * 4u;
    const Rml::TextureHandle fontTexture = rmlRenderInterface.GenerateTexture(
        {reinterpret_cast<const Rml::byte*>(fontPixels), fontByteCount},
        Rml::Vector2i(fontWidth, fontHeight));
    if (fontTexture == 0)
    {
        ImGui::DestroyContext(context);
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.ImGuiFontTextureFailed",
            "Dear ImGui failed to register its font texture."));
    }

    imguiFontTextureId = static_cast<UiTextureId>(fontTexture);
    io.Fonts->SetTexID(static_cast<ImTextureID>(imguiFontTextureId));
    imguiInitialized = true;
    UpdateDeveloperGamepadCapability();
#else
    developerUiEnabled = false;
    developerUiVisible = false;
#endif
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> UiSubsystem::LoadFonts()
{
    std::vector<std::filesystem::path> candidates = desc.fontFaces;
    std::vector<std::filesystem::path> platformCandidates = BuildPlatformFontCandidates();
    candidates.insert(candidates.end(), platformCandidates.begin(), platformCandidates.end());

    bool loadedPrimary = false;
    for (const std::filesystem::path& candidate : candidates)
    {
        std::filesystem::path fontPath = candidate;
        if (!fontPath.is_absolute())
        {
            fontPath = desc.assetRoot / fontPath;
        }
        if (!std::filesystem::exists(fontPath))
        {
            continue;
        }

        const bool fallback = loadedPrimary;
        if (Rml::LoadFontFace(fontPath.string(), fallback))
        {
            loadedPrimary = true;
        }
    }

    if (!loadedPrimary)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.FontMissing",
            "No configured or platform fallback UI font could be loaded.",
            desc.assetRoot.string()));
    }
    return RuntimeResult<void>::Success();
}

RuntimeResult<UiSubsystem::LocalizationDatabase> UiSubsystem::LoadLocalizationCandidate() const
{
    std::ifstream file(desc.localizationPath);
    if (!file.is_open())
    {
        return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
            "UiSubsystem.LocalizationOpenFailed",
            "Could not open UI localization file.",
            desc.localizationPath.string()));
    }

    try
    {
        nlohmann::json json;
        file >> json;
        if (!json.is_object())
        {
            return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                "UiSubsystem.LocalizationInvalidRoot",
                "UI localization root must be an object.",
                desc.localizationPath.string()));
        }

        LocalizationDatabase database;
        for (const auto& localeEntry : json.items())
        {
            if (!localeEntry.value().is_object())
            {
                return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                    "UiSubsystem.LocalizationInvalidLocale",
                    "Each UI locale must be an object.",
                    desc.localizationPath.string(),
                    localeEntry.key()));
            }

            LocalizationTable table;
            for (const auto& stringEntry : localeEntry.value().items())
            {
                if (!stringEntry.value().is_string())
                {
                    return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                        "UiSubsystem.LocalizationInvalidString",
                        "Each UI localization value must be a string.",
                        desc.localizationPath.string(),
                        localeEntry.key() + "." + stringEntry.key()));
                }
                table.emplace(stringEntry.key(), stringEntry.value().get<std::string>());
            }
            database.emplace(localeEntry.key(), std::move(table));
        }

        if (database.find(desc.defaultLocale) == database.end())
        {
            return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                "UiSubsystem.DefaultLocaleMissing",
                "The configured default UI locale is not present in the localization file.",
                desc.localizationPath.string(),
                desc.defaultLocale));
        }
        return RuntimeResult<LocalizationDatabase>::Success(std::move(database));
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
            "UiSubsystem.LocalizationParseFailed",
            exception.what(),
            desc.localizationPath.string()));
    }
}

RuntimeResult<Rml::ElementDocument*> UiSubsystem::LoadDocumentCandidate()
{
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();

    rmlSystemInterface.BeginCandidateValidation();
    Rml::ElementDocument* candidate = rmlContext->LoadDocument(desc.documentPath.string());
    std::string parseError;
    const bool parseValid = rmlSystemInterface.EndCandidateValidation(parseError);
    if (candidate == nullptr || !parseValid)
    {
        if (candidate != nullptr)
        {
            rmlContext->UnloadDocument(candidate);
            rmlContext->Update();
        }
        return RuntimeResult<Rml::ElementDocument*>::Failure(MakeRuntimeError(
            "UiSubsystem.DocumentParseFailed",
            parseError.empty() ? "RmlUi returned no document." : parseError,
            desc.documentPath.string()));
    }

    std::string validationError;
    if (!ValidateDocument(*candidate, validationError))
    {
        rmlContext->UnloadDocument(candidate);
        rmlContext->Update();
        return RuntimeResult<Rml::ElementDocument*>::Failure(MakeRuntimeError(
            "UiSubsystem.DocumentValidationFailed",
            validationError,
            desc.documentPath.string()));
    }

    candidate->Hide();
    return RuntimeResult<Rml::ElementDocument*>::Success(candidate);
}

RuntimeResult<void> UiSubsystem::CommitInitialAssets()
{
    auto localizationResult = LoadLocalizationCandidate();
    if (localizationResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(localizationResult.Error());
    }
    auto documentResult = LoadDocumentCandidate();
    if (documentResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(documentResult.Error());
    }

    CommitDocumentCandidate(*documentResult.Value(), std::move(localizationResult.Value()));
    assetFingerprint = BuildAssetFingerprint();
    hotReloadStatus = desc.hotReload ? "hot reload: watching" : "hot reload: disabled";
    return RuntimeResult<void>::Success();
}

void UiSubsystem::CommitDocumentCandidate(
    Rml::ElementDocument& candidate,
    LocalizationDatabase localizationCandidate)
{
    CapturePersistentDocumentState();
    ResetRuntimeInputState();
    Rml::ElementDocument* previousDocument = runtimeDocument;
    runtimeDocument = &candidate;
    localizations = std::move(localizationCandidate);
    if (localizations.find(currentLocale) == localizations.end())
    {
        currentLocale = desc.defaultLocale;
    }

    Rml::ReleaseTextures(&rmlRenderInterface);
    runtimeDocument->Show();
    ApplyDocumentState();
    RestorePersistentDocumentState();
    SyncBindingData();
    dataModelHandle.DirtyAllVariables();
    rmlContext->Update();

    if (previousDocument != nullptr)
    {
        rmlContext->UnloadDocument(previousDocument);
        rmlContext->Update();
    }
}

bool UiSubsystem::ValidateDocument(Rml::ElementDocument& document, std::string& outError) const
{
    static const char* requiredIds[] = {
        "ui-root",
        "hud",
        "control-backdrop",
        "control-panel",
        "control-scroll",
        "close-button"
    };
    for (const char* requiredId : requiredIds)
    {
        if (document.GetElementById(requiredId) == nullptr)
        {
            outError = std::string("Required UI element is missing: #") + requiredId;
            return false;
        }
    }
    outError.clear();
    return true;
}

void UiSubsystem::ApplyDocumentState()
{
    if (runtimeDocument == nullptr)
    {
        return;
    }

    Rml::Element* backdrop = runtimeDocument->GetElementById("control-backdrop");
    Rml::Element* panel = runtimeDocument->GetElementById("control-panel");
    if (backdrop != nullptr)
    {
        backdrop->SetProperty("display", runtimePageVisible ? "block" : "none");
    }
    if (panel != nullptr)
    {
        panel->SetProperty("display", runtimePageVisible ? "block" : "none");
    }

    if (runtimePageVisible)
    {
        Rml::Element* closeButton = runtimeDocument->GetElementById("close-button");
        if (closeButton != nullptr)
        {
            closeButton->Focus(true);
        }
    }
}

void UiSubsystem::CapturePersistentDocumentState()
{
    if (runtimeDocument == nullptr)
    {
        return;
    }
    Rml::Element* scrollElement = runtimeDocument->GetElementById("control-scroll");
    if (scrollElement != nullptr)
    {
        persistentScrollTop = scrollElement->GetScrollTop();
    }
}

void UiSubsystem::RestorePersistentDocumentState()
{
    if (runtimeDocument == nullptr)
    {
        return;
    }
    Rml::Element* scrollElement = runtimeDocument->GetElementById("control-scroll");
    if (scrollElement != nullptr)
    {
        scrollElement->SetScrollTop(persistentScrollTop);
    }
}

void UiSubsystem::TryHotReload()
{
    if (!desc.hotReload || std::chrono::steady_clock::now() < nextHotReloadCheck)
    {
        return;
    }
    nextHotReloadCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

    AssetFingerprint candidateFingerprint = BuildAssetFingerprint();
    if (!HasAssetFingerprintChanged(candidateFingerprint))
    {
        return;
    }

    auto localizationResult = LoadLocalizationCandidate();
    if (localizationResult.IsFailure())
    {
        hotReloadStatus = "hot reload rollback: localization invalid";
        diagnostics->ReportRuntimeError("UI hot reload rejected", localizationResult.Error());
        return;
    }
    auto documentResult = LoadDocumentCandidate();
    if (documentResult.IsFailure())
    {
        hotReloadStatus = "hot reload rollback: document invalid";
        diagnostics->ReportRuntimeError("UI hot reload rejected", documentResult.Error());
        return;
    }

    CommitDocumentCandidate(*documentResult.Value(), std::move(localizationResult.Value()));
    assetFingerprint = std::move(candidateFingerprint);
    hotReloadStatus = "hot reload: committed";
    diagnostics->ReportInfo("UI hot reload committed: " + desc.documentPath.string());
}

UiSubsystem::AssetFingerprint UiSubsystem::BuildAssetFingerprint() const
{
    AssetFingerprint fingerprint;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        desc.assetRoot,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_regular_file(error))
        {
            fingerprint.emplace(entry.path().string(), GetFileFingerprint(entry));
        }
        iterator.increment(error);
    }
    return fingerprint;
}

bool UiSubsystem::HasAssetFingerprintChanged(const AssetFingerprint& candidate) const
{
    if (candidate.size() != assetFingerprint.size())
    {
        return true;
    }
    for (const auto& entry : candidate)
    {
        auto current = assetFingerprint.find(entry.first);
        if (current == assetFingerprint.end() || current->second != entry.second)
        {
            return true;
        }
    }
    return false;
}

void UiSubsystem::SyncBindingData()
{
    bindingData.worldText = std::filesystem::path(lastViewModel.activeWorldPath).stem().string();
    bindingData.panelTitle = Localize("panel.title");
    bindingData.panelSubtitle = Localize("panel.subtitle");
    bindingData.closeLabel = Localize("action.close");
    bindingData.quitLabel = Localize("action.quit");

    bindingData.tabOverviewLabel = Localize("tab.overview");
    bindingData.tabVisualizationLabel = Localize("tab.visualization");
    bindingData.tabPostProcessLabel = Localize("tab.post_process");
    bindingData.tabEnvironmentLabel = Localize("tab.environment");
    bindingData.tabShadowsLabel = Localize("tab.shadows");
    bindingData.tabSystemLabel = Localize("tab.system");

    bindingData.frameLabel = Localize("status.frame");
    bindingData.frameValue = std::to_string(lastViewModel.frameIndex);
    bindingData.fpsLabel = Localize("status.fps");
    bindingData.fpsValue = FormatFloat(lastViewModel.framesPerSecond);
    bindingData.sceneLabel = Localize("status.scene");
    bindingData.inputModeLabel = Localize("status.input_mode");
    bindingData.inputModeValue = GetInputModeName(inputOwnership.mode);
    bindingData.windProfilesLabel = Localize("status.wind_profiles");
    bindingData.windProfilesValue = std::to_string(lastViewModel.speedTreeWindProfileCount);
    bindingData.hotReloadLabel = Localize("status.hot_reload");
    bindingData.hotReloadStatus = hotReloadStatus;

    bindingData.debugViewLabel = Localize("control.debug_view");
    bindingData.debugViewMode = lastViewModel.debugViewMode;
    bindingData.debugViewValue = GetDebugViewName(lastViewModel.debugViewMode);
    bindingData.debugFullLabel = Localize("debug.full");
    bindingData.debugBaseColorLabel = Localize("debug.base_color");
    bindingData.debugEmissiveLabel = Localize("debug.emissive");
    bindingData.debugNormalLabel = Localize("debug.normal");
    bindingData.debugRoughnessLabel = Localize("debug.roughness");
    bindingData.debugMetallicLabel = Localize("debug.metallic");
    bindingData.debugAoLabel = Localize("debug.ao");
    bindingData.debugShadowLabel = Localize("debug.shadow");
    bindingData.debugDirectLightingLabel = Localize("debug.direct_lighting");
    bindingData.debugIndirectDiffuseLabel = Localize("debug.indirect_diffuse");
    bindingData.debugIndirectSpecularLabel = Localize("debug.indirect_specular");
    bindingData.debugShadowCascadeLabel = Localize("debug.shadow_cascade");
    bindingData.debugShadingModelLabel = Localize("debug.shading_model");
    bindingData.debugSubsurfaceWeightLabel = Localize("debug.subsurface_weight");
    bindingData.debugTransmissionWeightLabel = Localize("debug.transmission_weight");
    bindingData.debugSubsurfaceAssetIdLabel = Localize("debug.subsurface_asset_id");
    bindingData.debugLocalSubsurfaceLabel = Localize("debug.local_subsurface");
    bindingData.debugDiffuseBeforeSssLabel = Localize("debug.diffuse_before_sss");
    bindingData.debugDiffuseAfterSssLabel = Localize("debug.diffuse_after_sss");
    bindingData.debugSssPixelRadiusLabel = Localize("debug.sss_pixel_radius");
    bindingData.debugSssValidWeightLabel = Localize("debug.sss_valid_weight");
    bindingData.debugHairWorldTangentLabel = Localize("debug.hair_world_tangent");
    bindingData.debugHairTangentRootwardLabel = Localize("debug.hair_tangent_rootward");
    bindingData.debugHairThetaIoLabel = Localize("debug.hair_theta_io");
    bindingData.debugHairDeltaPhiLabel = Localize("debug.hair_delta_phi");
    bindingData.debugHairRLabel = Localize("debug.hair_r");
    bindingData.debugHairTtLabel = Localize("debug.hair_tt");
    bindingData.debugHairTrtLabel = Localize("debug.hair_trt");
    bindingData.debugHairPathLengthLabel = Localize("debug.hair_path_length");
    bindingData.debugHairAbsorptionLabel = Localize("debug.hair_absorption");
    bindingData.debugHairCoverageLabel = Localize("debug.hair_coverage");
    bindingData.debugHairShadowTransmittanceLabel = Localize("debug.hair_shadow_transmittance");
    bindingData.debugHairLutCoordinatesLabel = Localize("debug.hair_lut_coordinates");
    bindingData.debugHairPrimaryHighlightLabel = Localize("debug.hair_primary_highlight");
    bindingData.debugHairSecondaryHighlightLabel = Localize("debug.hair_secondary_highlight");
    bindingData.debugHairScatterLabel = Localize("debug.hair_scatter");
    bindingData.debugHairBacklitLabel = Localize("debug.hair_backlit");
    bindingData.debugHairRPathColorLabel = Localize("debug.hair_r_path_color");
    bindingData.debugHairTtPathColorLabel = Localize("debug.hair_tt_path_color");
    bindingData.debugHairTrtPathColorLabel = Localize("debug.hair_trt_path_color");
    bindingData.debugHairIblFallbackLabel = Localize("debug.hair_ibl_fallback");
    bindingData.debugHairMultipleScatteringFallbackLabel = Localize("debug.hair_ms_fallback");
    bindingData.debugClothModelLabel = Localize("debug.cloth_model");
    bindingData.debugClothSheenColorLabel = Localize("debug.cloth_sheen_color");
    bindingData.debugClothSheenRoughnessLabel = Localize("debug.cloth_sheen_roughness");
    bindingData.debugClothCharlieDLabel = Localize("debug.cloth_charlie_d");
    bindingData.debugClothNeubeltVisibilityLabel = Localize("debug.cloth_neubelt_visibility");
    bindingData.debugClothDirectionalAlbedoLabel = Localize("debug.cloth_directional_albedo");
    bindingData.debugClothBaseEnergyScaleLabel = Localize("debug.cloth_base_energy_scale");
    bindingData.debugClothDirectSheenLabel = Localize("debug.cloth_direct_sheen");
    bindingData.debugClothIndirectSheenLabel = Localize("debug.cloth_indirect_sheen");
    bindingData.debugClothIblFallbackLabel = Localize("debug.cloth_ibl_fallback");

    bindingData.toneMappingLabel = Localize("control.tone_mapping");
    bindingData.toneMappingMode = lastViewModel.toneMappingMode;
    bindingData.toneMappingValue = GetToneMappingName(lastViewModel.toneMappingMode);
    bindingData.toneNoneLabel = Localize("tone.none");
    bindingData.toneReinhardLabel = Localize("tone.reinhard");
    bindingData.toneHableLabel = Localize("tone.hable");
    bindingData.toneAcesLabel = Localize("tone.aces");
    bindingData.bloomStrengthLabel = Localize("control.bloom_strength");
    bindingData.bloomStrength = lastViewModel.bloomStrength;
    bindingData.bloomStrengthValue = FormatFloat(lastViewModel.bloomStrength);
    bindingData.bloomThresholdLabel = Localize("control.bloom_threshold");
    bindingData.bloomThreshold = lastViewModel.bloomThreshold;
    bindingData.bloomThresholdValue = FormatFloat(lastViewModel.bloomThreshold);
    bindingData.bloomKneeLabel = Localize("control.bloom_knee");
    bindingData.bloomKnee = lastViewModel.bloomKnee;
    bindingData.bloomKneeValue = FormatFloat(lastViewModel.bloomKnee);
    bindingData.bloomClampLabel = Localize("control.bloom_clamp");
    bindingData.bloomClamp = lastViewModel.bloomClamp;
    bindingData.bloomClampValue = FormatFloat(lastViewModel.bloomClamp);

    bindingData.shadowCastShadowsLabel =
        Localize("control.shadow_cast_shadows");
    bindingData.csmCastShadows =
        lastViewModel.csmCastShadows;
    bindingData.shadowCastShadowsValue =
        lastViewModel.csmCastShadows ?
        Localize("status.enabled") : Localize("status.disabled");
    bindingData.shadowEnableLabel = Localize("action.enable");
    bindingData.shadowDisableLabel = Localize("action.disable");
    bindingData.shadowDynamicDistanceLabel =
        Localize("control.shadow_dynamic_distance");
    bindingData.csmDynamicShadowDistance =
        lastViewModel.csmDynamicShadowDistance;
    bindingData.shadowDynamicDistanceValue =
        FormatFloat(
            lastViewModel.csmDynamicShadowDistance);
    bindingData.shadowDynamicCascadesLabel =
        Localize("control.shadow_dynamic_cascades");
    bindingData.csmDynamicShadowCascades =
        lastViewModel.csmDynamicShadowCascades;
    bindingData.shadowCascadeDistributionExponentLabel =
        Localize(
            "control.shadow_cascade_distribution_exponent");
    bindingData.csmCascadeDistributionExponent =
        lastViewModel.csmCascadeDistributionExponent;
    bindingData.shadowCascadeDistributionExponentValue =
        FormatFloat(
            lastViewModel.csmCascadeDistributionExponent);
    bindingData.shadowCascadeTransitionFractionLabel =
        Localize(
            "control.shadow_cascade_transition_fraction");
    bindingData.csmCascadeTransitionFraction =
        lastViewModel.csmCascadeTransitionFraction;
    bindingData.shadowCascadeTransitionFractionValue =
        FormatFloat(
            lastViewModel.csmCascadeTransitionFraction);
    bindingData.shadowDistanceFadeoutFractionLabel =
        Localize(
            "control.shadow_distance_fadeout_fraction");
    bindingData.csmShadowDistanceFadeoutFraction =
        lastViewModel.csmShadowDistanceFadeoutFraction;
    bindingData.shadowDistanceFadeoutFractionValue =
        FormatFloat(
            lastViewModel.csmShadowDistanceFadeoutFraction);
    bindingData.shadowBiasLabel =
        Localize("control.shadow_bias");
    bindingData.csmShadowBias =
        lastViewModel.csmShadowBias;
    bindingData.shadowBiasValue =
        FormatFloat(lastViewModel.csmShadowBias);
    bindingData.shadowSlopeBiasLabel = Localize("control.shadow_slope_bias");
    bindingData.csmShadowSlopeBias =
        lastViewModel.csmShadowSlopeBias;
    bindingData.shadowSlopeBiasValue =
        FormatFloat(lastViewModel.csmShadowSlopeBias);
    bindingData.shadowCascadeBiasDistributionLabel =
        Localize(
            "control.shadow_cascade_bias_distribution");
    bindingData.csmShadowCascadeBiasDistribution =
        lastViewModel.csmShadowCascadeBiasDistribution;
    bindingData.shadowCascadeBiasDistributionValue =
        FormatFloat(
            lastViewModel.csmShadowCascadeBiasDistribution);
    bindingData.shadowDebugLabel = Localize("control.shadow_debug");
    bindingData.shadowDebugFullLabel = Localize("debug.full");
    bindingData.shadowDebugCascadesLabel = Localize("debug.shadow_cascade");
    bindingData.shadowSaveLabel = Localize("action.save_to_scene");
    bindingData.environmentLabel = Localize("control.environment");
    bindingData.environmentIntensity = lastViewModel.environmentIntensity;
    bindingData.environmentValue = FormatFloat(lastViewModel.environmentIntensity);
    bindingData.vegetationHeading = Localize("section.vegetation");
    bindingData.windStrengthLabel = Localize("control.wind_strength");
    bindingData.speedTreeStrength = lastViewModel.speedTreeStrength;
    bindingData.windStrengthValue = FormatFloat(lastViewModel.speedTreeStrength);
    bindingData.gustingLabel = Localize("control.gusting");
    bindingData.speedTreeGustingEnabled = lastViewModel.speedTreeGustingEnabled;
    bindingData.gustingValue = lastViewModel.speedTreeGustingEnabled ?
        Localize("status.enabled") : Localize("status.disabled");
    bindingData.gustOnLabel = Localize("action.gust_on");
    bindingData.gustOffLabel = Localize("action.gust_off");
    bindingData.gustOnceLabel = Localize("action.gust_once");

    bindingData.localeLabel = Localize("status.locale");
    bindingData.localeValue = currentLocale;
    bindingData.developerUiLabel = Localize("status.developer_ui");
    bindingData.developerUiValue = lastViewModel.developerUiVisible ?
        Localize("status.visible") : Localize("status.hidden");
    bindingData.developerUiToggleLabel = Localize("action.developer_ui");
}

std::string UiSubsystem::Localize(const std::string& key) const
{
    auto localeIt = localizations.find(currentLocale);
    if (localeIt != localizations.end())
    {
        auto valueIt = localeIt->second.find(key);
        if (valueIt != localeIt->second.end())
        {
            return valueIt->second;
        }
    }
    auto fallbackIt = localizations.find(desc.defaultLocale);
    if (fallbackIt != localizations.end())
    {
        auto valueIt = fallbackIt->second.find(key);
        if (valueIt != fallbackIt->second.end())
        {
            return valueIt->second;
        }
    }
    return key;
}

std::string UiSubsystem::FormatFloat(float value) const
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

std::string UiSubsystem::GetDebugViewName(int mode) const
{
    switch (mode)
    {
    case 0: return Localize("debug.full");
    case 1: return Localize("debug.base_color");
    case 2: return Localize("debug.emissive");
    case 3: return Localize("debug.normal");
    case 4: return Localize("debug.roughness");
    case 5: return Localize("debug.metallic");
    case 6: return Localize("debug.ao");
    case 7: return Localize("debug.shadow");
    case 8: return Localize("debug.direct_lighting");
    case 9: return Localize("debug.indirect_diffuse");
    case 10: return Localize("debug.indirect_specular");
    case 11: return Localize("debug.shadow_cascade");
    case 12: return Localize("debug.shading_model");
    case 13: return Localize("debug.subsurface_weight");
    case 14: return Localize("debug.transmission_weight");
    case 15: return Localize("debug.subsurface_asset_id");
    case 16: return Localize("debug.local_subsurface");
    case 17: return Localize("debug.diffuse_before_sss");
    case 18: return Localize("debug.diffuse_after_sss");
    case 19: return Localize("debug.sss_pixel_radius");
    case 20: return Localize("debug.sss_valid_weight");
    case 21: return Localize("debug.hair_world_tangent");
    case 22: return Localize("debug.hair_tangent_rootward");
    case 23: return Localize("debug.hair_theta_io");
    case 24: return Localize("debug.hair_delta_phi");
    case 25: return Localize("debug.hair_r");
    case 26: return Localize("debug.hair_tt");
    case 27: return Localize("debug.hair_trt");
    case 28: return Localize("debug.hair_path_length");
    case 29: return Localize("debug.hair_absorption");
    case 30: return Localize("debug.hair_coverage");
    case 31: return Localize("debug.hair_shadow_transmittance");
    case 32: return Localize("debug.hair_lut_coordinates");
    case 33: return Localize("debug.hair_primary_highlight");
    case 34: return Localize("debug.hair_secondary_highlight");
    case 35: return Localize("debug.hair_scatter");
    case 36: return Localize("debug.hair_backlit");
    case 37: return Localize("debug.hair_r_path_color");
    case 38: return Localize("debug.hair_tt_path_color");
    case 39: return Localize("debug.hair_trt_path_color");
    case 40: return Localize("debug.hair_ibl_fallback");
    case 41: return Localize("debug.hair_ms_fallback");
    case 42: return Localize("debug.eye_frame");
    case 43: return Localize("debug.eye_cornea_normal");
    case 44: return Localize("debug.eye_iris_normal");
    case 45: return Localize("debug.eye_iris_plane_normal");
    case 46: return Localize("debug.eye_cornea_fresnel");
    case 47: return Localize("debug.eye_cornea_specular");
    case 48: return Localize("debug.eye_refracted_view_direction");
    case 49: return Localize("debug.eye_iris_hit_distance");
    case 50: return Localize("debug.eye_iris_uv");
    case 51: return Localize("debug.eye_valid_iris_hit");
    case 52: return Localize("debug.eye_iris_mask");
    case 53: return Localize("debug.eye_pupil_mask");
    case 54: return Localize("debug.eye_limbus_mask");
    case 55: return Localize("debug.eye_light_transmission_in");
    case 56: return Localize("debug.eye_view_transmission_out");
    case 57: return Localize("debug.eye_iris_direct");
    case 58: return Localize("debug.eye_sclera_direct");
    case 59: return Localize("debug.eye_inner_ibl");
    case 60: return Localize("debug.eye_caustic_gain");
    case 61: return Localize("debug.eye_inner_shadow");
    case 62: return Localize("debug.eye_cornea_shadow");
    case 63: return Localize("debug.eye_profile");
    case 64: return Localize("debug.cloth_model");
    case 65: return Localize("debug.cloth_sheen_color");
    case 66: return Localize("debug.cloth_sheen_roughness");
    case 67: return Localize("debug.cloth_charlie_d");
    case 68: return Localize("debug.cloth_neubelt_visibility");
    case 69: return Localize("debug.cloth_directional_albedo");
    case 70: return Localize("debug.cloth_base_energy_scale");
    case 71: return Localize("debug.cloth_direct_sheen");
    case 72: return Localize("debug.cloth_indirect_sheen");
    case 73: return Localize("debug.cloth_ibl_fallback");
    default: return Localize("debug.unknown");
    }
}

std::string UiSubsystem::GetToneMappingName(int mode) const
{
    switch (mode)
    {
    case 0: return Localize("tone.none");
    case 1: return Localize("tone.reinhard");
    case 2: return Localize("tone.hable");
    case 3: return Localize("tone.aces");
    default: return Localize("tone.unknown");
    }
}

std::string UiSubsystem::GetInputModeName(UiInputMode mode) const
{
    switch (mode)
    {
    case UiInputMode::GameOnly: return Localize("input.game_only");
    case UiInputMode::UiOnly: return Localize("input.ui_only");
    case UiInputMode::GameAndUi: return Localize("input.game_and_ui");
    }
    return Localize("status.unknown");
}

void UiSubsystem::BuildAndPublishRenderSnapshot()
{
    std::shared_ptr<UiRenderSnapshot> snapshot = std::make_shared<UiRenderSnapshot>();
    snapshot->frameIndex = uiFrameIndex++;
    snapshot->viewportWidth = desc.viewportWidth;
    snapshot->viewportHeight = desc.viewportHeight;

    rmlRenderInterface.BeginFrame(*snapshot);
    rmlContext->Render();
    AppendDeveloperDrawData(*snapshot);
    rmlRenderInterface.AppendTextureSnapshots(snapshot->textures);
    rmlRenderInterface.EndFrame();
    renderSnapshotQueue.Publish(std::move(snapshot));
}

void UiSubsystem::AppendDeveloperDrawData(UiRenderSnapshot& snapshot)
{
#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    if (!imguiInitialized)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const bool developerUiWasVisible = developerUiVisible;
    io.DisplaySize = ImVec2(
        static_cast<float>(desc.viewportWidth),
        static_cast<float>(desc.viewportHeight));
    io.DeltaTime = lastViewModel.deltaTimeSeconds > 0.0f ? lastViewModel.deltaTimeSeconds : 1.0f / 60.0f;
    ImGui::NewFrame();
    if (developerUiVisible)
    {
        BuildDeveloperPanels();
    }
    ImGui::Render();
    if (developerUiWasVisible && !developerUiVisible)
    {
        ResetDeveloperInputState();
    }

    const ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr)
    {
        return;
    }

    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
    {
        const ImDrawList* drawList = drawData->CmdLists[listIndex];
        const uint32_t baseVertex = static_cast<uint32_t>(snapshot.vertices.size());
        const uint32_t baseIndex = static_cast<uint32_t>(snapshot.indices.size());

        snapshot.vertices.reserve(snapshot.vertices.size() + static_cast<size_t>(drawList->VtxBuffer.Size));
        for (const ImDrawVert& sourceVertex : drawList->VtxBuffer)
        {
            UiVertex vertex;
            vertex.position[0] = sourceVertex.pos.x;
            vertex.position[1] = sourceVertex.pos.y;
            vertex.texCoord[0] = sourceVertex.uv.x;
            vertex.texCoord[1] = sourceVertex.uv.y;
            vertex.color = sourceVertex.col;
            snapshot.vertices.push_back(vertex);
        }

        snapshot.indices.reserve(snapshot.indices.size() + static_cast<size_t>(drawList->IdxBuffer.Size));
        for (ImDrawIdx sourceIndex : drawList->IdxBuffer)
        {
            snapshot.indices.push_back(baseVertex + static_cast<uint32_t>(sourceIndex));
        }

        for (const ImDrawCmd& sourceCommand : drawList->CmdBuffer)
        {
            if (sourceCommand.UserCallback != nullptr)
            {
                continue;
            }

            const float clipLeft = (sourceCommand.ClipRect.x - drawData->DisplayPos.x) * drawData->FramebufferScale.x;
            const float clipTop = (sourceCommand.ClipRect.y - drawData->DisplayPos.y) * drawData->FramebufferScale.y;
            const float clipRight = (sourceCommand.ClipRect.z - drawData->DisplayPos.x) * drawData->FramebufferScale.x;
            const float clipBottom = (sourceCommand.ClipRect.w - drawData->DisplayPos.y) * drawData->FramebufferScale.y;
            const int left = std::max(0, static_cast<int>(std::floor(clipLeft)));
            const int top = std::max(0, static_cast<int>(std::floor(clipTop)));
            const int right = std::min(static_cast<int>(snapshot.viewportWidth), static_cast<int>(std::ceil(clipRight)));
            const int bottom = std::min(static_cast<int>(snapshot.viewportHeight), static_cast<int>(std::ceil(clipBottom)));
            if (right <= left || bottom <= top)
            {
                continue;
            }

            UiDrawCommand command;
            command.firstIndex = baseIndex + sourceCommand.IdxOffset;
            command.indexCount = sourceCommand.ElemCount;
            command.textureId = static_cast<UiTextureId>(sourceCommand.GetTexID());
            command.clipRect.x = left;
            command.clipRect.y = top;
            command.clipRect.width = static_cast<uint32_t>(right - left);
            command.clipRect.height = static_cast<uint32_t>(bottom - top);
            command.blendMode = UiBlendMode::StraightAlpha;
            snapshot.drawCommands.push_back(command);
        }
    }
#else
    static_cast<void>(snapshot);
#endif
}

void UiSubsystem::BuildDeveloperPanels()
{
#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    ImGui::DockSpaceOverViewport(
        0,
        ImGui::GetMainViewport(),
        ImGuiDockNodeFlags_PassthruCentralNode);
    if (!ImGui::Begin("VulkanLearn Developer Tools", &developerUiVisible))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(lastViewModel.frameIndex));
    ImGui::Text("FPS: %.1f", lastViewModel.framesPerSecond);
    ImGui::TextWrapped("World: %s", lastViewModel.activeWorldPath.c_str());

    ImGui::SeparatorText("Renderer State");
    ImGui::Text("Debug view: %d", lastViewModel.debugViewMode);
    ImGui::Text("Tone mapping: %d", lastViewModel.toneMappingMode);
    ImGui::Text("Bloom: %.2f / %.2f / %.2f / %.2f",
        lastViewModel.bloomStrength,
        lastViewModel.bloomThreshold,
        lastViewModel.bloomKnee,
        lastViewModel.bloomClamp);
    ImGui::Text("Environment intensity: %.2f", lastViewModel.environmentIntensity);
    ImGui::Text(
        "Environment update: %s  active=%llu pending=%llu  old=%s",
        lastViewModel.environmentUpdateStage.c_str(),
        static_cast<unsigned long long>(lastViewModel.environmentActiveGeneration),
        static_cast<unsigned long long>(lastViewModel.environmentPendingGeneration),
        lastViewModel.environmentUsesPreviousResources ? "yes" : "no");
    ImGui::Text(
        "IBL progress: cube %u/%u  SH %u/1  prefilter %u/%u",
        lastViewModel.environmentCubemapFacesCompleted,
        lastViewModel.environmentCubemapFaceCount,
        lastViewModel.environmentShUpdatesCompleted,
        lastViewModel.environmentPrefilterMipsCompleted,
        lastViewModel.environmentPrefilterMipCount);
    if (lastViewModel.environmentGpuTimingSupported)
    {
        ImGui::Text(
            "IBL GPU ms: cube %.3f  SH %.3f  prefilter %.3f  commit %.3f",
            lastViewModel.environmentCubemapGpuMs,
            lastViewModel.environmentShGpuMs,
            lastViewModel.environmentPrefilterGpuMs,
            lastViewModel.environmentCommitGpuMs);
    }
    else
    {
        ImGui::Text("IBL GPU timing: unsupported");
    }
    ImGui::Text("SpeedTree: profiles=%u strength=%.2f gusting=%s",
        lastViewModel.speedTreeWindProfileCount,
        lastViewModel.speedTreeStrength,
        lastViewModel.speedTreeGustingEnabled ? "on" : "off");

    ImGui::SeparatorText("UI Runtime");
    ImGui::Text("Input mode: %d", static_cast<int>(inputOwnership.mode));
    ImGui::Text("Keyboard owner: %d", static_cast<int>(inputOwnership.keyboardOwner));
    ImGui::Text("Pointer owner: %d", static_cast<int>(inputOwnership.pointerOwner));
    ImGui::TextWrapped("%s", hotReloadStatus.c_str());
    ImGui::TextDisabled("Runtime controls are authored in RmlUi.");
    ImGui::End();
#endif
}

void UiSubsystem::UpdateInputOwnership()
{
    if (runtimePageVisible)
    {
        inputOwnership.mode = UiInputMode::UiOnly;
        inputOwnership.keyboardOwner = UiInputOwner::RuntimeUi;
        inputOwnership.pointerOwner = UiInputOwner::RuntimeUi;
        inputOwnership.controllerOwner = UiInputOwner::RuntimeUi;
        inputOwnership.textInputOwner = UiInputOwner::RuntimeUi;
        return;
    }

    inputOwnership.mode = developerUiVisible ? UiInputMode::GameAndUi : UiInputMode::GameOnly;
    inputOwnership.keyboardOwner = UiInputOwner::Game;
    inputOwnership.pointerOwner = UiInputOwner::Game;
    inputOwnership.controllerOwner = UiInputOwner::Game;
    inputOwnership.textInputOwner = UiInputOwner::None;

#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    if (developerUiVisible && imguiInitialized)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureKeyboard)
        {
            inputOwnership.keyboardOwner = UiInputOwner::DeveloperUi;
        }
        if (io.WantCaptureMouse)
        {
            inputOwnership.pointerOwner = UiInputOwner::DeveloperUi;
        }
        if (io.WantTextInput)
        {
            inputOwnership.textInputOwner = UiInputOwner::DeveloperUi;
        }
        if (gamepadConnected && io.NavActive)
        {
            inputOwnership.controllerOwner = UiInputOwner::DeveloperUi;
        }
    }
#endif
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
    if (rmlContext != nullptr)
    {
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

void UiSubsystem::ResetDeveloperInputState()
{
#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    if (!imguiInitialized)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
#endif
}
void UiSubsystem::ResetDeveloperGamepadInputState()
{
#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    if (!imguiInitialized)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImGuiKey gamepadKeys[] = {
        ImGuiKey_GamepadFaceDown,
        ImGuiKey_GamepadFaceRight,
        ImGuiKey_GamepadFaceLeft,
        ImGuiKey_GamepadFaceUp,
        ImGuiKey_GamepadBack,
        ImGuiKey_GamepadStart,
        ImGuiKey_GamepadL1,
        ImGuiKey_GamepadR1,
        ImGuiKey_GamepadDpadUp,
        ImGuiKey_GamepadDpadDown,
        ImGuiKey_GamepadDpadLeft,
        ImGuiKey_GamepadDpadRight
    };
    for (ImGuiKey key : gamepadKeys)
    {
        io.AddKeyEvent(key, false);
    }
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, false, 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, false, 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, false, 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, false, 0.0f);
#endif
}

void UiSubsystem::UpdateDeveloperGamepadCapability()
{
#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    if (!imguiInitialized)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (gamepadConnected)
    {
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    }
    else
    {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    }
#endif
}

bool UiSubsystem::ProcessRuntimeUiEvent(const PlatformEvent& event)
{
    if (rmlContext == nullptr)
    {
        return false;
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

bool UiSubsystem::ProcessDeveloperUiEvent(const PlatformEvent& event)
{
#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    if (!imguiInitialized)
    {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    switch (event.type)
    {
    case PlatformEventType::KeyDown:
    case PlatformEventType::KeyUp:
    {
        io.AddKeyEvent(ImGuiMod_Ctrl, event.control);
        io.AddKeyEvent(ImGuiMod_Shift, event.shift);
        io.AddKeyEvent(ImGuiMod_Alt, event.alt);
        io.AddKeyEvent(ImGuiMod_Super, event.super);
        const ImGuiKey key = ToImGuiKey(event.key);
        if (key != ImGuiKey_None)
        {
            io.AddKeyEvent(key, event.type == PlatformEventType::KeyDown);
        }
        return io.WantCaptureKeyboard;
    }
    case PlatformEventType::TextInput:
        io.AddInputCharactersUTF8(event.text.c_str());
        return io.WantTextInput || io.WantCaptureKeyboard;
    case PlatformEventType::TextEditing:
        return io.WantTextInput;
    case PlatformEventType::MouseMove:
        io.AddMousePosEvent(event.mouseX, event.mouseY);
        return io.WantCaptureMouse;
    case PlatformEventType::MouseButtonDown:
    case PlatformEventType::MouseButtonUp:
    {
        const int button = ToRmlMouseButton(event.mouseButton);
        if (button >= 0 && button < 5)
        {
            io.AddMouseButtonEvent(button, event.type == PlatformEventType::MouseButtonDown);
        }
        return io.WantCaptureMouse;
    }
    case PlatformEventType::MouseWheel:
        io.AddMouseWheelEvent(event.wheelX, event.wheelY);
        return io.WantCaptureMouse;
    case PlatformEventType::MouseLeave:
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        return io.WantCaptureMouse;
    case PlatformEventType::GamepadButtonDown:
    case PlatformEventType::GamepadButtonUp:
    {
        const ImGuiKey key = ToImGuiGamepadButton(event.gamepadButton);
        if (key != ImGuiKey_None)
        {
            io.AddKeyEvent(key, event.type == PlatformEventType::GamepadButtonDown);
        }
        return io.NavActive;
    }
    case PlatformEventType::GamepadAxisMotion:
        if (event.gamepadAxis == PlatformGamepadAxis::LeftX)
        {
            const float left = NormalizeImGuiGamepadAxis(std::max(0.0f, -event.gamepadAxisValue));
            const float right = NormalizeImGuiGamepadAxis(std::max(0.0f, event.gamepadAxisValue));
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, left > 0.0f, left);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, right > 0.0f, right);
        }
        else if (event.gamepadAxis == PlatformGamepadAxis::LeftY)
        {
            const float up = NormalizeImGuiGamepadAxis(std::max(0.0f, -event.gamepadAxisValue));
            const float down = NormalizeImGuiGamepadAxis(std::max(0.0f, event.gamepadAxisValue));
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, up > 0.0f, up);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, down > 0.0f, down);
        }
        return io.NavActive;
    case PlatformEventType::WindowFocusGained:
    case PlatformEventType::WindowFocusLost:
        io.AddFocusEvent(event.type == PlatformEventType::WindowFocusGained);
        return false;
    default:
        break;
    }
#else
    static_cast<void>(event);
#endif
    return false;
}
bool UiSubsystem::HandleGlobalShortcut(const PlatformEvent& event)
{
    if (event.type == PlatformEventType::KeyDown && !event.repeat)
    {
        if (event.key == PlatformKey::F10)
        {
            UiAction action;
            action.type = runtimePageVisible ?
                UiActionType::CloseRuntimePage :
                UiActionType::ToggleRuntimePage;
            QueueAction(std::move(action));
            return true;
        }
        if (event.key == PlatformKey::F1 && developerUiEnabled)
        {
            UiAction action;
            action.type = UiActionType::ToggleDeveloperUi;
            QueueAction(std::move(action));
            return true;
        }
    }

    if (event.type == PlatformEventType::GamepadButtonDown)
    {
        if (event.gamepadButton == PlatformGamepadButton::Start)
        {
            UiAction action;
            action.type = runtimePageVisible ? UiActionType::CloseRuntimePage : UiActionType::ToggleRuntimePage;
            QueueAction(std::move(action));
            return true;
        }
        if (event.gamepadButton == PlatformGamepadButton::East && runtimePageVisible)
        {
            UiAction action;
            action.type = UiActionType::CloseRuntimePage;
            QueueAction(std::move(action));
            return true;
        }
    }
    return false;
}

void UiSubsystem::QueueAction(UiAction action)
{
    if (commandBus != nullptr)
    {
        commandBus->Queue(std::move(action));
    }
}

void UiSubsystem::QueueIntAction(UiActionType type, int value)
{
    UiAction action;
    action.type = type;
    action.intValue = value;
    QueueAction(std::move(action));
}

void UiSubsystem::QueueFloatAction(UiActionType type, float value)
{
    UiAction action;
    action.type = type;
    action.floatValue = value;
    QueueAction(std::move(action));
}

void UiSubsystem::QueueChangedFloatAction(UiActionType type, float value, float currentValue)
{
    if (std::abs(value - currentValue) <= 0.0001f)
    {
        return;
    }
    QueueFloatAction(type, value);
}

void UiSubsystem::OnClosePage(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::CloseRuntimePage;
    QueueAction(std::move(action));
}

void UiSubsystem::OnDebugViewSelected(
    Rml::DataModelHandle,
    Rml::Event&,
    const Rml::VariantList& arguments)
{
    const int mode = arguments[0].Get<int>();
    if (mode != lastViewModel.debugViewMode)
    {
        QueueIntAction(UiActionType::SetDebugViewMode, mode);
    }
}

void UiSubsystem::OnToneMappingNone(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::SetToneMappingMode;
    action.intValue = 0;
    QueueAction(std::move(action));
}

void UiSubsystem::OnToneMappingReinhard(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::SetToneMappingMode;
    action.intValue = 1;
    QueueAction(std::move(action));
}

void UiSubsystem::OnToneMappingHable(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::SetToneMappingMode;
    action.intValue = 2;
    QueueAction(std::move(action));
}

void UiSubsystem::OnToneMappingAces(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::SetToneMappingMode;
    action.intValue = 3;
    QueueAction(std::move(action));
}

void UiSubsystem::OnBloomStrengthChanged(Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
{
    QueueChangedFloatAction(UiActionType::SetBloomStrength, event.GetParameter<float>("value", lastViewModel.bloomStrength), lastViewModel.bloomStrength);
}

void UiSubsystem::OnBloomThresholdChanged(Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
{
    QueueChangedFloatAction(UiActionType::SetBloomThreshold, event.GetParameter<float>("value", lastViewModel.bloomThreshold), lastViewModel.bloomThreshold);
}

void UiSubsystem::OnBloomKneeChanged(Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
{
    QueueChangedFloatAction(UiActionType::SetBloomKnee, event.GetParameter<float>("value", lastViewModel.bloomKnee), lastViewModel.bloomKnee);
}

void UiSubsystem::OnBloomClampChanged(Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
{
    QueueChangedFloatAction(UiActionType::SetBloomClamp, event.GetParameter<float>("value", lastViewModel.bloomClamp), lastViewModel.bloomClamp);
}

void UiSubsystem::OnCsmCastShadowsSelected(
    Rml::DataModelHandle,
    Rml::Event&,
    const Rml::VariantList& arguments)
{
    const bool enabled = arguments[0].Get<int>() != 0;
    if (enabled != lastViewModel.csmCastShadows)
    {
        QueueIntAction(
            UiActionType::SetCsmCastShadows,
            enabled ? 1 : 0);
    }
}

void UiSubsystem::OnCsmDynamicShadowDistanceChanged(
    Rml::DataModelHandle,
    Rml::Event& event,
    const Rml::VariantList&)
{
    QueueChangedFloatAction(
        UiActionType::SetCsmDynamicShadowDistance,
        event.GetParameter<float>(
            "value",
            lastViewModel.csmDynamicShadowDistance),
        lastViewModel.csmDynamicShadowDistance);
}

void UiSubsystem::OnCsmDynamicShadowCascadesSelected(
    Rml::DataModelHandle,
    Rml::Event&,
    const Rml::VariantList& arguments)
{
    const int cascadeCount = arguments[0].Get<int>();
    if (cascadeCount !=
        static_cast<int>(
            lastViewModel.csmDynamicShadowCascades))
    {
        QueueIntAction(
            UiActionType::SetCsmDynamicShadowCascades,
            cascadeCount);
    }
}

void UiSubsystem::OnCsmCascadeDistributionExponentChanged(
    Rml::DataModelHandle,
    Rml::Event& event,
    const Rml::VariantList&)
{
    QueueChangedFloatAction(
        UiActionType::SetCsmCascadeDistributionExponent,
        event.GetParameter<float>(
            "value",
            lastViewModel.csmCascadeDistributionExponent),
        lastViewModel.csmCascadeDistributionExponent);
}

void UiSubsystem::OnCsmCascadeTransitionFractionChanged(
    Rml::DataModelHandle,
    Rml::Event& event,
    const Rml::VariantList&)
{
    QueueChangedFloatAction(
        UiActionType::SetCsmCascadeTransitionFraction,
        event.GetParameter<float>(
            "value",
            lastViewModel.csmCascadeTransitionFraction),
        lastViewModel.csmCascadeTransitionFraction);
}

void UiSubsystem::OnCsmShadowDistanceFadeoutFractionChanged(
    Rml::DataModelHandle,
    Rml::Event& event,
    const Rml::VariantList&)
{
    QueueChangedFloatAction(
        UiActionType::SetCsmShadowDistanceFadeoutFraction,
        event.GetParameter<float>(
            "value",
            lastViewModel.csmShadowDistanceFadeoutFraction),
        lastViewModel.csmShadowDistanceFadeoutFraction);
}

void UiSubsystem::OnCsmShadowBiasChanged(
    Rml::DataModelHandle,
    Rml::Event& event,
    const Rml::VariantList&)
{
    QueueChangedFloatAction(
        UiActionType::SetCsmShadowBias,
        event.GetParameter<float>(
            "value",
            lastViewModel.csmShadowBias),
        lastViewModel.csmShadowBias);
}

void UiSubsystem::OnCsmShadowSlopeBiasChanged(
    Rml::DataModelHandle,
    Rml::Event& event,
    const Rml::VariantList&)
{
    QueueChangedFloatAction(
        UiActionType::SetCsmShadowSlopeBias,
        event.GetParameter<float>(
            "value",
            lastViewModel.csmShadowSlopeBias),
        lastViewModel.csmShadowSlopeBias);
}

void UiSubsystem::OnCsmShadowCascadeBiasDistributionChanged(
    Rml::DataModelHandle,
    Rml::Event& event,
    const Rml::VariantList&)
{
    QueueChangedFloatAction(
        UiActionType::SetCsmShadowCascadeBiasDistribution,
        event.GetParameter<float>(
            "value",
            lastViewModel.csmShadowCascadeBiasDistribution),
        lastViewModel.csmShadowCascadeBiasDistribution);
}

void UiSubsystem::OnSaveCsmSettingsToScene(
    Rml::DataModelHandle,
    Rml::Event&,
    const Rml::VariantList&)
{
    QueueAction(UiAction{
        UiActionType::SaveCsmSettingsToScene});
}

void UiSubsystem::OnEnvironmentIntensityChanged(Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
{
    QueueChangedFloatAction(
        UiActionType::SetEnvironmentIntensity,
        event.GetParameter<float>("value", lastViewModel.environmentIntensity),
        lastViewModel.environmentIntensity);
}

void UiSubsystem::OnSpeedTreeStrengthChanged(
    Rml::DataModelHandle,
    Rml::Event& event,
    const Rml::VariantList&)
{
    QueueChangedFloatAction(
        UiActionType::SetSpeedTreeStrength,
        event.GetParameter<float>("value", lastViewModel.speedTreeStrength),
        lastViewModel.speedTreeStrength);
}

void UiSubsystem::OnSpeedTreeGustingSelected(
    Rml::DataModelHandle,
    Rml::Event&,
    const Rml::VariantList& arguments)
{
    const bool enabled = arguments[0].Get<int>() != 0;
    if (enabled != lastViewModel.speedTreeGustingEnabled)
    {
        QueueIntAction(UiActionType::SetSpeedTreeGustingEnabled, enabled ? 1 : 0);
    }
}

void UiSubsystem::OnForceSpeedTreeGust(
    Rml::DataModelHandle,
    Rml::Event&,
    const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::ForceSpeedTreeGust;
    QueueAction(std::move(action));
}

void UiSubsystem::OnLocaleEnglish(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::SetLocale;
    action.stringValue = "en-US";
    QueueAction(std::move(action));
}

void UiSubsystem::OnLocaleChinese(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::SetLocale;
    action.stringValue = "zh-CN";
    QueueAction(std::move(action));
}

void UiSubsystem::OnToggleDeveloperUi(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::ToggleDeveloperUi;
    QueueAction(std::move(action));
}

void UiSubsystem::OnQuit(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
{
    UiAction action;
    action.type = UiActionType::Quit;
    QueueAction(std::move(action));
}

} // namespace VL
