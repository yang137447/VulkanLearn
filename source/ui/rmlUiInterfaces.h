#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/TextInputHandler.h>

#include "ui/uiRenderSnapshot.h"

namespace VL
{

class DiagnosticsSubsystem;
class PlatformWindow;

// Adapts RmlUi system callbacks to SDL3, diagnostics, clipboard, and text input.
// It does not own engine commands or Vulkan resources.
class RmlUiSystemInterface final : public Rml::SystemInterface
{
public:
    void Initialize(PlatformWindow& platformWindow, const DiagnosticsSubsystem& diagnosticsSubsystem);

    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
    void SetClipboardText(const Rml::String& text) override;
    void GetClipboardText(Rml::String& text) override;
    void ActivateKeyboard(Rml::Vector2f caretPosition, float lineHeight) override;
    void DeactivateKeyboard() override;

    void BeginCandidateValidation();
    bool EndCandidateValidation(std::string& outError);

private:
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    PlatformWindow* window = nullptr;
    const DiagnosticsSubsystem* diagnostics = nullptr;
    bool validatingCandidate = false;
    std::vector<std::string> candidateErrors;
};

// Tracks the active RmlUi editable control and applies SDL composition updates.
// The handler owns no element; RmlUi notifies it before a text context is destroyed.
class RmlUiTextInputHandler final : public Rml::TextInputHandler
{
public:
    void OnActivate(Rml::TextInputContext* inputContext) override;
    void OnDeactivate(Rml::TextInputContext* inputContext) override;
    void OnDestroy(Rml::TextInputContext* inputContext) override;

    bool UpdateComposition(const std::string& text, int selectionStart, int selectionLength);
    bool CommitComposition(const std::string& text);
    void CancelComposition();
    bool IsComposing() const { return composing; }

private:
    void ClearCompositionState();

    Rml::TextInputContext* inputContext = nullptr;
    int compositionStart = 0;
    int compositionEnd = 0;
    bool composing = false;
};

// Converts RmlUi geometry and textures into an immutable UiRenderSnapshot.
// Vulkan allocation and command recording remain in UiOverlayRendererVulkan.
class RmlUiRenderInterface final : public Rml::RenderInterface
{
public:
    RmlUiRenderInterface() = default;
    ~RmlUiRenderInterface() override;

    Rml::CompiledGeometryHandle CompileGeometry(
        Rml::Span<const Rml::Vertex> vertices,
        Rml::Span<const int> indices) override;
    void RenderGeometry(
        Rml::CompiledGeometryHandle geometry,
        Rml::Vector2f translation,
        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(
        Rml::Vector2i& textureDimensions,
        const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(
        Rml::Span<const Rml::byte> source,
        Rml::Vector2i sourceDimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

    void BeginFrame(UiRenderSnapshot& renderSnapshot);
    void EndFrame();
    void AppendTextureSnapshots(std::vector<UiTextureSnapshot>& outTextures) const;

private:
    struct CompiledGeometry;

    struct TextureRecord
    {
        UiTextureId id = 0;
        uint64_t generation = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string source;
        std::shared_ptr<const std::vector<uint8_t>> pixels;
    };

    Rml::TextureHandle RegisterTexture(
        uint32_t width,
        uint32_t height,
        std::string source,
        std::shared_ptr<const std::vector<uint8_t>> pixels);
    UiClipRect BuildClipRect() const;
    static uint32_t PackColor(const Rml::ColourbPremultiplied& color);
    static void PremultiplyAlpha(std::vector<uint8_t>& pixels);

    UiRenderSnapshot* currentSnapshot = nullptr;
    bool scissorEnabled = false;
    Rml::Rectanglei scissorRegion;
    bool transformEnabled = false;
    Rml::Matrix4f currentTransform = Rml::Matrix4f::Identity();
    UiTextureId nextTextureId = 1;
    uint64_t nextTextureGeneration = 1;
    std::unordered_map<UiTextureId, TextureRecord> textures;
    std::unordered_set<UiTextureId> pendingTextureReleases;
};

} // namespace VL
