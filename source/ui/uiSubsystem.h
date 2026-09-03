#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Input.h>

#include "core/runtimeResult.h"
#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "platform/platformEvent.h"
#include "ui/rmlUiInterfaces.h"
#include "ui/uiAction.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/uiRenderSnapshotQueue.h"

namespace Rml
{
class Context;
class ElementDocument;
class Event;
}

namespace VL
{

class CommandBus;
class DiagnosticsSubsystem;
class PlatformWindow;

namespace Editor
{
    class MaterialInstanceEditorRuntime;
namespace Selection
{
struct MaterialInstanceSelection;
}
namespace Preview
{
class IMaterialInstancePreviewAdapter;
}
}

namespace EditorUi
{
}

// Describes UI assets, viewport, localization, fonts, and runtime feature switches.
// RuntimeConfig supplies it; UiSubsystem owns the resulting widget contexts.
struct UiSubsystemDesc
{
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    std::filesystem::path assetRoot;
    std::filesystem::path documentPath;
    std::filesystem::path localizationPath;
    std::filesystem::path sceneRoot;
    std::string defaultLocale;
    std::vector<std::filesystem::path> fontFaces;
    bool hotReload = true;
    bool developerUiEnabled = true;
    bool developerUiVisible = false;
    // EngineLoop 注入 renderer-owned adapter；UiSubsystem 不拥有也不直接调用它。
    Editor::Preview::IMaterialInstancePreviewAdapter* previewAdapter = nullptr;
};

struct UiSceneOption
{
    Rml::String label;
    Rml::String path;
    bool active = false;
    std::string normalizedPhysicalPath;
};

// Owns game-thread RmlUi and optional Dear ImGui contexts, input arbitration, and hot reload.
// It publishes typed actions and immutable snapshots, but does not mutate renderer internals.
class UiSubsystem : public EditorUi::IMaterialInstanceSceneSelectionSink
{
public:
    UiSubsystem();
    ~UiSubsystem();

    UiSubsystem(const UiSubsystem&) = delete;
    UiSubsystem& operator=(const UiSubsystem&) = delete;

    RuntimeResult<void> Initialize(
        const UiSubsystemDesc& desc,
        PlatformWindow& platformWindow,
        CommandBus& commandBus,
        const DiagnosticsSubsystem& diagnosticsSubsystem);
    void Shutdown();

    bool HandlePlatformEvent(const PlatformEvent& event);
    void Update(const UiViewModelSnapshot& viewModelSnapshot);
    void Resize(uint32_t width, uint32_t height);
    void ApplyAction(const UiAction& action);
    void NotifyWorldChanged(const std::string& worldPath, uint64_t generation);
    void NotifyWorldLoadCompleted();
    // 只由 EngineLoop 在稳定帧边界调用，避免 UI 回调触碰 live MaterialInstance。
    void TickMaterialInstanceEditor();
    // 视口拾取只负责打开文档并记录导航来源，实际编辑仍经由 EditorCommand。
    bool OpenMaterialInstanceFromSelection(
        const Editor::Selection::MaterialInstanceSelection& selection);
    void SetMaterialInstanceSceneModels(
        const std::vector<Editor::Selection::MaterialInstanceModelContext>& models);
    std::optional<Editor::Selection::MaterialInstanceModelContext>
        ConsumeMaterialInstanceModelSelectionRequest();
    std::optional<Editor::Selection::MaterialInstanceSelection>
        ConsumeMaterialInstanceSelectionRequest();
    void ClearMaterialInstanceSceneSelection() noexcept;

    bool RequestModelSelection(
        const Editor::Selection::MaterialInstanceModelContext& model) override;
    bool RequestMaterialSelection(
        const Editor::Selection::MaterialInstanceSelection& selection) override;

    bool IsInitialized() const { return initialized; }
    bool IsRuntimePageVisible() const { return runtimePageVisible; }
    bool IsDeveloperUiVisible() const { return developerUiVisible; }
    bool IsDeveloperUiEnabled() const { return developerUiEnabled; }
    uint32_t GetViewportWidth() const { return desc.viewportWidth; }
    uint32_t GetViewportHeight() const { return desc.viewportHeight; }
    const UiViewportRect& GetSceneViewportRect() const { return sceneViewportRect; }
    bool IsScenePickTarget(float mouseX, float mouseY) const;
    bool ShouldUseRelativeMouseModeForGame() const;
    bool ShouldGameReceiveKeyboard() const;
    bool ShouldGameReceivePointer() const;
    const UiInputOwnershipSnapshot& GetInputOwnership() const { return inputOwnership; }
    UiRenderSnapshotQueue& GetRenderSnapshotQueue() { return renderSnapshotQueue; }

private:
    using LocalizationTable = std::unordered_map<std::string, std::string>;
    using LocalizationDatabase = std::unordered_map<std::string, LocalizationTable>;
    using AssetFingerprint = std::unordered_map<std::string, uint64_t>;

    struct BindingData
    {
        int activeSection = 0;
        Rml::String worldText;
        Rml::String panelTitle;
        Rml::String panelSubtitle;
        Rml::String closeLabel;
        Rml::String quitLabel;

        Rml::String tabOverviewLabel;
        Rml::String tabVisualizationLabel;
        Rml::String tabPostProcessLabel;
        Rml::String tabEnvironmentLabel;
        Rml::String tabShadowsLabel;
        Rml::String tabScenesLabel;
        Rml::String tabSystemLabel;

        Rml::String sceneListTitle;
        Rml::String sceneListHint;
        Rml::String sceneEmptyLabel;
        Rml::String sceneLoadingLabel;
        bool sceneLoading = false;
        std::vector<UiSceneOption> sceneOptions;

        Rml::String cameraMoveSpeedLabel;
        Rml::String cameraMoveSpeedValue;
        float cameraMoveSpeed = 10.0f;

        Rml::String frameLabel;
        Rml::String frameValue;
        Rml::String fpsLabel;
        Rml::String fpsValue;
        Rml::String sceneLabel;
        Rml::String inputModeLabel;
        Rml::String inputModeValue;
        Rml::String windProfilesLabel;
        Rml::String windProfilesValue;
        Rml::String hotReloadLabel;
        Rml::String hotReloadStatus;

        Rml::String debugViewLabel;
        Rml::String debugViewValue;
        int debugViewMode = 0;
        Rml::String debugMaterialGroupLabel;
        Rml::String debugSssGroupLabel;
        Rml::String debugHairGroupLabel;
        Rml::String debugEyeGroupLabel;
        Rml::String debugClothGroupLabel;
        Rml::String debugFullLabel;
        Rml::String debugBaseColorLabel;
        Rml::String debugEmissiveLabel;
        Rml::String debugNormalLabel;
        Rml::String debugRoughnessLabel;
        Rml::String debugMetallicLabel;
        Rml::String debugAoLabel;
        Rml::String debugShadowLabel;
        Rml::String debugDirectLightingLabel;
        Rml::String debugIndirectDiffuseLabel;
        Rml::String debugIndirectSpecularLabel;
        Rml::String debugShadowCascadeLabel;
        Rml::String debugShadingModelLabel;
        Rml::String debugSubsurfaceWeightLabel;
        Rml::String debugTransmissionWeightLabel;
        Rml::String debugSubsurfaceAssetIdLabel;
        Rml::String debugLocalSubsurfaceLabel;
        Rml::String debugDiffuseBeforeSssLabel;
        Rml::String debugDiffuseAfterSssLabel;
        Rml::String debugSssPixelRadiusLabel;
        Rml::String debugSssValidWeightLabel;
        Rml::String debugHairWorldTangentLabel;
        Rml::String debugHairTangentRootwardLabel;
        Rml::String debugHairThetaIoLabel;
        Rml::String debugHairDeltaPhiLabel;
        Rml::String debugHairRLabel;
        Rml::String debugHairTtLabel;
        Rml::String debugHairTrtLabel;
        Rml::String debugHairPathLengthLabel;
        Rml::String debugHairAbsorptionLabel;
        Rml::String debugHairCoverageLabel;
        Rml::String debugHairShadowTransmittanceLabel;
        Rml::String debugHairLutCoordinatesLabel;
        Rml::String debugHairPrimaryHighlightLabel;
        Rml::String debugHairSecondaryHighlightLabel;
        Rml::String debugHairScatterLabel;
        Rml::String debugHairBacklitLabel;
        Rml::String debugHairRPathColorLabel;
        Rml::String debugHairTtPathColorLabel;
        Rml::String debugHairTrtPathColorLabel;
        Rml::String debugHairIblFallbackLabel;
        Rml::String debugHairMultipleScatteringFallbackLabel;
        Rml::String debugEyeFrameLabel;
        Rml::String debugEyeCorneaNormalLabel;
        Rml::String debugEyeIrisNormalLabel;
        Rml::String debugEyeIrisPlaneNormalLabel;
        Rml::String debugEyeCorneaFresnelLabel;
        Rml::String debugEyeCorneaSpecularLabel;
        Rml::String debugEyeRefractedViewDirectionLabel;
        Rml::String debugEyeIrisHitDistanceLabel;
        Rml::String debugEyeIrisUvLabel;
        Rml::String debugEyeValidIrisHitLabel;
        Rml::String debugEyeIrisMaskLabel;
        Rml::String debugEyePupilMaskLabel;
        Rml::String debugEyeLimbusMaskLabel;
        Rml::String debugEyeLightTransmissionInLabel;
        Rml::String debugEyeViewTransmissionOutLabel;
        Rml::String debugEyeIrisDirectLabel;
        Rml::String debugEyeScleraDirectLabel;
        Rml::String debugEyeInnerIblLabel;
        Rml::String debugEyeCausticGainLabel;
        Rml::String debugEyeInnerShadowLabel;
        Rml::String debugEyeCorneaShadowLabel;
        Rml::String debugEyeProfileLabel;
        Rml::String debugClothModelLabel;
        Rml::String debugClothSheenColorLabel;
        Rml::String debugClothSheenRoughnessLabel;
        Rml::String debugClothCharlieDLabel;
        Rml::String debugClothNeubeltVisibilityLabel;
        Rml::String debugClothDirectionalAlbedoLabel;
        Rml::String debugClothBaseEnergyScaleLabel;
        Rml::String debugClothDirectSheenLabel;
        Rml::String debugClothIndirectSheenLabel;
        Rml::String debugClothIblFallbackLabel;
        Rml::String debugClothV2ModelLabel;
        Rml::String debugClothWorldTangentLabel;
        Rml::String debugClothAnisotropyLabel;
        Rml::String debugClothAnisotropyCrossLabel;
        Rml::String debugClothRoughnessAxesLabel;
        Rml::String debugClothAnisotropicCharlieDLabel;
        Rml::String debugClothVisibilityLabel;
        Rml::String debugClothV2DirectionalAlbedoLabel;
        Rml::String debugClothV2BaseEnergyScaleLabel;
        Rml::String debugClothV2IblFallbackLabel;

        Rml::String toneMappingLabel;
        Rml::String toneMappingValue;
        int toneMappingMode = 0;
        Rml::String toneNoneLabel;
        Rml::String toneReinhardLabel;
        Rml::String toneHableLabel;
        Rml::String toneAcesLabel;
        Rml::String bloomStrengthLabel;
        Rml::String bloomStrengthValue;
        float bloomStrength = 0.0f;
        Rml::String bloomThresholdLabel;
        Rml::String bloomThresholdValue;
        float bloomThreshold = 0.0f;
        Rml::String bloomKneeLabel;
        Rml::String bloomKneeValue;
        float bloomKnee = 0.0f;
        Rml::String bloomClampLabel;
        Rml::String bloomClampValue;
        float bloomClamp = 0.0f;

        Rml::String shadowCastShadowsLabel;
        Rml::String shadowCastShadowsValue;
        bool csmCastShadows = false;
        Rml::String shadowEnableLabel;
        Rml::String shadowDisableLabel;
        Rml::String shadowDynamicDistanceLabel;
        Rml::String shadowDynamicDistanceValue;
        float csmDynamicShadowDistance = 10.0f;
        Rml::String shadowDynamicCascadesLabel;
        uint32_t csmDynamicShadowCascades = 4;
        Rml::String shadowCascadeDistributionExponentLabel;
        Rml::String shadowCascadeDistributionExponentValue;
        float csmCascadeDistributionExponent = 3.0f;
        Rml::String shadowCascadeTransitionFractionLabel;
        Rml::String shadowCascadeTransitionFractionValue;
        float csmCascadeTransitionFraction = 0.1f;
        Rml::String shadowDistanceFadeoutFractionLabel;
        Rml::String shadowDistanceFadeoutFractionValue;
        float csmShadowDistanceFadeoutFraction = 0.1f;
        Rml::String shadowBiasLabel;
        Rml::String shadowBiasValue;
        float csmShadowBias = 0.5f;
        Rml::String shadowSlopeBiasLabel;
        Rml::String shadowSlopeBiasValue;
        float csmShadowSlopeBias = 0.5f;
        Rml::String shadowCascadeBiasDistributionLabel;
        Rml::String shadowCascadeBiasDistributionValue;
        float csmShadowCascadeBiasDistribution = 1.0f;
        Rml::String shadowDebugLabel;
        Rml::String shadowDebugFullLabel;
        Rml::String shadowDebugCascadesLabel;
        Rml::String shadowSaveLabel;

        Rml::String environmentLabel;
        Rml::String environmentValue;
        float environmentIntensity = 1.0f;
        Rml::String vegetationHeading;
        Rml::String windStrengthLabel;
        Rml::String windStrengthValue;
        float speedTreeStrength = 0.35f;
        Rml::String gustingLabel;
        Rml::String gustingValue;
        bool speedTreeGustingEnabled = true;
        Rml::String gustOnLabel;
        Rml::String gustOffLabel;
        Rml::String gustOnceLabel;

        Rml::String localeLabel;
        Rml::String localeValue;
        Rml::String developerUiLabel;
        Rml::String developerUiValue;
        Rml::String developerUiToggleLabel;
    };

    RuntimeResult<void> InitializeRmlUi();
    RuntimeResult<void> InitializeDataModel();
    RuntimeResult<void> InitializeDeveloperUi();
    RuntimeResult<void> InitializeMaterialInstanceEditor();
    RuntimeResult<void> LoadFonts();
    void LoadSceneOptions();
    RuntimeResult<LocalizationDatabase> LoadLocalizationCandidate() const;
    RuntimeResult<Rml::ElementDocument*> LoadDocumentCandidate();
    RuntimeResult<void> CommitInitialAssets();
    void CommitDocumentCandidate(
        Rml::ElementDocument& candidate,
        LocalizationDatabase localizationCandidate);
    bool ValidateDocument(Rml::ElementDocument& document, std::string& outError) const;
    void ApplyDocumentState();
    void CapturePersistentDocumentState();
    void RestorePersistentDocumentState();
    void TryHotReload();
    AssetFingerprint BuildAssetFingerprint() const;
    bool HasAssetFingerprintChanged(const AssetFingerprint& candidate) const;

    void SyncBindingData();
    std::string Localize(const std::string& key) const;
    std::string FormatFloat(float value) const;
    std::string GetDebugViewName(int mode) const;
    std::string GetToneMappingName(int mode) const;
    std::string GetInputModeName(UiInputMode mode) const;
    void BuildAndPublishRenderSnapshot();
    void AppendDeveloperDrawData(UiRenderSnapshot& snapshot);
    void BuildDeveloperPanels();
    void UpdateInputOwnership();

    bool ProcessRuntimeUiEvent(const PlatformEvent& event);
    bool ProcessDeveloperUiEvent(const PlatformEvent& event);
    bool PressRmlKey(Rml::Input::KeyIdentifier key, bool repeat, int modifiers = 0);
    bool ReleaseRmlKey(Rml::Input::KeyIdentifier key, int modifiers = 0);
    bool SetRmlAxisDirection(
        bool pressed,
        bool& currentState,
        Rml::Input::KeyIdentifier key);
    bool ProcessRmlGamepadAxisEvent(const PlatformEvent& event);
    void ResetRuntimeInputState();
    void ResetRmlGamepadInputState();
    void ResetDeveloperInputState();
    void ResetDeveloperGamepadInputState();
    void UpdateDeveloperGamepadCapability();
    bool HandleGlobalShortcut(const PlatformEvent& event);
    void QueueAction(UiAction action);
    void QueueIntAction(UiActionType type, int value);
    void QueueFloatAction(UiActionType type, float value);
    void QueueChangedFloatAction(UiActionType type, float value, float currentValue);

    void OnClosePage(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnLoadScene(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCameraMoveSpeedChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnDebugViewSelected(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnToneMappingNone(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnToneMappingReinhard(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnToneMappingHable(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnToneMappingAces(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnBloomStrengthChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnBloomThresholdChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnBloomKneeChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnBloomClampChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCsmCastShadowsSelected(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCsmDynamicShadowDistanceChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCsmDynamicShadowCascadesSelected(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCsmCascadeDistributionExponentChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCsmCascadeTransitionFractionChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCsmShadowDistanceFadeoutFractionChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCsmShadowBiasChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCsmShadowSlopeBiasChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnCsmShadowCascadeBiasDistributionChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnSaveCsmSettingsToScene(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnEnvironmentIntensityChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnSpeedTreeStrengthChanged(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnSpeedTreeGustingSelected(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnForceSpeedTreeGust(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnLocaleEnglish(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnLocaleChinese(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnToggleDeveloperUi(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
    void OnQuit(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);

    UiSubsystemDesc desc;
    PlatformWindow* window = nullptr;
    CommandBus* commandBus = nullptr;
    const DiagnosticsSubsystem* diagnostics = nullptr;
    std::unique_ptr<Editor::MaterialInstanceEditorRuntime>
        materialInstanceEditorRuntime;
    std::unique_ptr<EditorUi::MaterialInstanceAssetEditorPanel>
        materialInstanceEditorPanel;
    std::optional<EditorUi::MaterialInstanceModelSnapshot>
        selectedModelMaterials;
    std::optional<uint32_t> selectedMaterialSlotIndex;
    std::vector<EditorUi::MaterialInstanceModelSnapshot> sceneModels;
    std::optional<Editor::Selection::MaterialInstanceModelContext>
        pendingMaterialInstanceModelSelection;
    std::optional<Editor::Selection::MaterialInstanceSelection>
        pendingMaterialInstanceSelection;
    Rml::Context* rmlContext = nullptr;
    Rml::ElementDocument* runtimeDocument = nullptr;
    RmlUiSystemInterface rmlSystemInterface;
    RmlUiTextInputHandler rmlTextInputHandler;
    RmlUiRenderInterface rmlRenderInterface;
    Rml::DataModelHandle dataModelHandle;
    BindingData bindingData;
    LocalizationDatabase localizations;
    AssetFingerprint assetFingerprint;
    UiRenderSnapshotQueue renderSnapshotQueue;
    UiViewModelSnapshot lastViewModel;
    UiInputOwnershipSnapshot inputOwnership;
    std::unordered_map<Rml::Input::KeyIdentifier, uint32_t> rmlKeyPressCounts;
    std::unordered_set<PlatformKey> rmlPressedKeyboardKeys;
    std::unordered_set<PlatformMouseButton> rmlPressedMouseButtons;
    std::unordered_set<PlatformGamepadButton> rmlPressedGamepadButtons;
    std::chrono::steady_clock::time_point nextHotReloadCheck;
    uint64_t uiFrameIndex = 0;
    float persistentScrollTop = 0.0f;
    std::string currentLocale;
    std::string hotReloadStatus;
    UiTextureId imguiFontTextureId = 0;
    UiViewportRect sceneViewportRect;
    bool initialized = false;
    bool rmlInitialized = false;
    bool runtimePageVisible = false;
    bool developerUiEnabled = false;
    bool developerUiVisible = false;
    bool imguiInitialized = false;
    bool developerDockLayoutBuilt = false;
    bool gamepadConnected = false;
    bool rmlAxisLeftDown = false;
    bool rmlAxisRightDown = false;
    bool rmlAxisUpDown = false;
    bool rmlAxisDownDown = false;
};

} // namespace VL
