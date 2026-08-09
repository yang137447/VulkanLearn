#include "ui/rmlUiInterfaces.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

#include <RmlUi/Core/Matrix4.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/TextInputContext.h>
#include <RmlUi/Core/Vector4.h>
#include <RmlUi/Core/Vertex.h>
#include <SDL3/SDL.h>
#include <stb_image.h>

#include "engine/diagnosticsSubsystem.h"
#include "platform/platformWindow.h"

namespace VL
{

namespace
{

const char* GetRmlLogPrefix(Rml::Log::Type type)
{
    switch (type)
    {
    case Rml::Log::LT_ERROR: return "RmlUi error: ";
    case Rml::Log::LT_ASSERT: return "RmlUi assertion: ";
    case Rml::Log::LT_WARNING: return "RmlUi warning: ";
    case Rml::Log::LT_INFO: return "RmlUi: ";
    case Rml::Log::LT_DEBUG: return "RmlUi debug: ";
    default: return "RmlUi: ";
    }
}

} // namespace

void RmlUiSystemInterface::Initialize(
    PlatformWindow& platformWindow,
    const DiagnosticsSubsystem& diagnosticsSubsystem)
{
    window = &platformWindow;
    diagnostics = &diagnosticsSubsystem;
    startTime = std::chrono::steady_clock::now();
}

double RmlUiSystemInterface::GetElapsedTime()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
}

bool RmlUiSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message)
{
    const std::string formattedMessage = std::string(GetRmlLogPrefix(type)) + message;
    if (validatingCandidate && (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT || type == Rml::Log::LT_WARNING))
    {
        candidateErrors.push_back(formattedMessage);
    }

    if (diagnostics != nullptr)
    {
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT)
        {
            diagnostics->ReportError(formattedMessage);
        }
        else if (type == Rml::Log::LT_WARNING)
        {
            diagnostics->ReportWarning(formattedMessage);
        }
        else if (type != Rml::Log::LT_DEBUG)
        {
            diagnostics->ReportInfo(formattedMessage);
        }
    }
    return true;
}

void RmlUiSystemInterface::SetClipboardText(const Rml::String& text)
{
    SDL_SetClipboardText(text.c_str());
}

void RmlUiSystemInterface::GetClipboardText(Rml::String& text)
{
    char* clipboardText = SDL_GetClipboardText();
    text = clipboardText != nullptr ? clipboardText : "";
    SDL_free(clipboardText);
}

void RmlUiSystemInterface::ActivateKeyboard(Rml::Vector2f caretPosition, float lineHeight)
{
    if (window == nullptr)
    {
        return;
    }

    window->SetTextInputArea(
        static_cast<int>(std::floor(caretPosition.x)),
        static_cast<int>(std::floor(caretPosition.y)),
        1,
        std::max(1, static_cast<int>(std::ceil(lineHeight))));
    window->SetTextInputEnabled(true);
}

void RmlUiSystemInterface::DeactivateKeyboard()
{
    if (window != nullptr)
    {
        window->SetTextInputEnabled(false);
    }
}

void RmlUiSystemInterface::BeginCandidateValidation()
{
    candidateErrors.clear();
    validatingCandidate = true;
}

bool RmlUiSystemInterface::EndCandidateValidation(std::string& outError)
{
    validatingCandidate = false;
    if (candidateErrors.empty())
    {
        outError.clear();
        return true;
    }

    std::ostringstream stream;
    for (size_t index = 0; index < candidateErrors.size(); ++index)
    {
        if (index > 0)
        {
            stream << '\n';
        }
        stream << candidateErrors[index];
    }
    outError = stream.str();
    candidateErrors.clear();
    return false;
}

void RmlUiTextInputHandler::OnActivate(Rml::TextInputContext* activatedInputContext)
{
    if (inputContext != activatedInputContext)
    {
        CancelComposition();
        inputContext = activatedInputContext;
        ClearCompositionState();
    }
}

void RmlUiTextInputHandler::OnDeactivate(Rml::TextInputContext* deactivatedInputContext)
{
    if (inputContext == deactivatedInputContext)
    {
        CancelComposition();
        inputContext = nullptr;
    }
}

void RmlUiTextInputHandler::OnDestroy(Rml::TextInputContext* destroyedInputContext)
{
    if (inputContext == destroyedInputContext)
    {
        inputContext = nullptr;
        ClearCompositionState();
    }
}

bool RmlUiTextInputHandler::UpdateComposition(
    const std::string& text,
    int selectionStart,
    int selectionLength)
{
    if (inputContext == nullptr)
    {
        return false;
    }

    if (!composing)
    {
        int selectionBegin = 0;
        int selectionEnd = 0;
        inputContext->GetSelectionRange(selectionBegin, selectionEnd);
        compositionStart = std::min(selectionBegin, selectionEnd);
        compositionEnd = std::max(selectionBegin, selectionEnd);
        composing = true;
    }

    inputContext->SetText(text, compositionStart, compositionEnd);
    const int compositionLength = static_cast<int>(Rml::StringUtilities::LengthUTF8(text));
    compositionEnd = compositionStart + compositionLength;

    if (text.empty())
    {
        inputContext->SetCompositionRange(0, 0);
        inputContext->SetCursorPosition(compositionStart);
        ClearCompositionState();
        return true;
    }

    inputContext->SetCompositionRange(compositionStart, compositionEnd);
    const int localSelectionStart = selectionStart >= 0 ?
        std::min(selectionStart, compositionLength) : compositionLength;
    const int localSelectionLength = selectionLength >= 0 ? selectionLength : 0;
    const int localSelectionEnd = std::min(
        localSelectionStart + localSelectionLength,
        compositionLength);
    if (localSelectionStart == localSelectionEnd)
    {
        inputContext->SetCursorPosition(compositionStart + localSelectionStart);
    }
    else
    {
        inputContext->SetSelectionRange(
            compositionStart + localSelectionStart,
            compositionStart + localSelectionEnd);
    }
    return true;
}

bool RmlUiTextInputHandler::CommitComposition(const std::string& text)
{
    if (inputContext == nullptr || !composing)
    {
        return false;
    }

    inputContext->SetCompositionRange(compositionStart, compositionEnd);
    inputContext->CommitComposition(text);
    const int committedLength = static_cast<int>(Rml::StringUtilities::LengthUTF8(text));
    inputContext->SetCompositionRange(0, 0);
    inputContext->SetCursorPosition(compositionStart + committedLength);
    ClearCompositionState();
    return true;
}

void RmlUiTextInputHandler::CancelComposition()
{
    if (inputContext != nullptr && composing)
    {
        inputContext->SetText(Rml::StringView(), compositionStart, compositionEnd);
        inputContext->SetCompositionRange(0, 0);
        inputContext->SetCursorPosition(compositionStart);
    }
    ClearCompositionState();
}

void RmlUiTextInputHandler::ClearCompositionState()
{
    compositionStart = 0;
    compositionEnd = 0;
    composing = false;
}
struct RmlUiRenderInterface::CompiledGeometry
{
    std::vector<Rml::Vertex> vertices;
    std::vector<int> indices;
};

RmlUiRenderInterface::~RmlUiRenderInterface()
{
    pendingTextureReleases.clear();
    textures.clear();
}

Rml::CompiledGeometryHandle RmlUiRenderInterface::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices,
    Rml::Span<const int> indices)
{
    std::unique_ptr<CompiledGeometry> geometry = std::make_unique<CompiledGeometry>();
    geometry->vertices.assign(vertices.begin(), vertices.end());
    geometry->indices.assign(indices.begin(), indices.end());
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry.release());
}

void RmlUiRenderInterface::RenderGeometry(
    Rml::CompiledGeometryHandle geometryHandle,
    Rml::Vector2f translation,
    Rml::TextureHandle texture)
{
    if (currentSnapshot == nullptr || geometryHandle == 0)
    {
        return;
    }

    const CompiledGeometry* geometry = reinterpret_cast<const CompiledGeometry*>(geometryHandle);
    const uint32_t baseVertex = static_cast<uint32_t>(currentSnapshot->vertices.size());
    const uint32_t firstIndex = static_cast<uint32_t>(currentSnapshot->indices.size());

    currentSnapshot->vertices.reserve(currentSnapshot->vertices.size() + geometry->vertices.size());
    for (const Rml::Vertex& sourceVertex : geometry->vertices)
    {
        Rml::Vector4f position(
            sourceVertex.position.x + translation.x,
            sourceVertex.position.y + translation.y,
            0.0f,
            1.0f);
        if (transformEnabled)
        {
            position = currentTransform * position;
            if (position.w != 0.0f)
            {
                position.x /= position.w;
                position.y /= position.w;
            }
        }

        UiVertex vertex;
        vertex.position[0] = position.x;
        vertex.position[1] = position.y;
        vertex.texCoord[0] = sourceVertex.tex_coord.x;
        vertex.texCoord[1] = sourceVertex.tex_coord.y;
        vertex.color = PackColor(sourceVertex.colour);
        currentSnapshot->vertices.push_back(vertex);
    }

    currentSnapshot->indices.reserve(currentSnapshot->indices.size() + geometry->indices.size());
    for (int sourceIndex : geometry->indices)
    {
        currentSnapshot->indices.push_back(baseVertex + static_cast<uint32_t>(sourceIndex));
    }

    UiDrawCommand command;
    command.firstIndex = firstIndex;
    command.indexCount = static_cast<uint32_t>(geometry->indices.size());
    command.textureId = static_cast<UiTextureId>(texture);
    command.clipRect = BuildClipRect();
    command.blendMode = UiBlendMode::PremultipliedAlpha;
    currentSnapshot->drawCommands.push_back(command);
}

void RmlUiRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    delete reinterpret_cast<CompiledGeometry*>(geometry);
}

Rml::TextureHandle RmlUiRenderInterface::LoadTexture(
    Rml::Vector2i& textureDimensions,
    const Rml::String& source)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* loadedPixels = stbi_load(source.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (loadedPixels == nullptr || width <= 0 || height <= 0)
    {
        if (loadedPixels != nullptr)
        {
            stbi_image_free(loadedPixels);
        }
        return 0;
    }

    const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    std::vector<uint8_t> pixels(loadedPixels, loadedPixels + byteCount);
    stbi_image_free(loadedPixels);
    PremultiplyAlpha(pixels);

    textureDimensions = Rml::Vector2i(width, height);
    return RegisterTexture(
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        source,
        std::make_shared<const std::vector<uint8_t>>(std::move(pixels)));
}

Rml::TextureHandle RmlUiRenderInterface::GenerateTexture(
    Rml::Span<const Rml::byte> source,
    Rml::Vector2i sourceDimensions)
{
    if (sourceDimensions.x <= 0 || sourceDimensions.y <= 0)
    {
        return 0;
    }

    std::vector<uint8_t> pixels(source.begin(), source.end());
    return RegisterTexture(
        static_cast<uint32_t>(sourceDimensions.x),
        static_cast<uint32_t>(sourceDimensions.y),
        "generated:rmlui",
        std::make_shared<const std::vector<uint8_t>>(std::move(pixels)));
}

void RmlUiRenderInterface::ReleaseTexture(Rml::TextureHandle texture)
{
    const UiTextureId textureId = static_cast<UiTextureId>(texture);
    auto textureIt = textures.find(textureId);
    if (textureIt == textures.end())
    {
        return;
    }

    if (currentSnapshot != nullptr)
    {
        pendingTextureReleases.insert(textureId);
        return;
    }
    textures.erase(textureIt);
}

void RmlUiRenderInterface::EnableScissorRegion(bool enable)
{
    scissorEnabled = enable;
}

void RmlUiRenderInterface::SetScissorRegion(Rml::Rectanglei region)
{
    scissorRegion = region;
}

void RmlUiRenderInterface::SetTransform(const Rml::Matrix4f* transform)
{
    transformEnabled = transform != nullptr;
    currentTransform = transform != nullptr ? *transform : Rml::Matrix4f::Identity();
}

void RmlUiRenderInterface::BeginFrame(UiRenderSnapshot& renderSnapshot)
{
    currentSnapshot = &renderSnapshot;
    scissorEnabled = false;
    transformEnabled = false;
    currentTransform = Rml::Matrix4f::Identity();
}

void RmlUiRenderInterface::EndFrame()
{
    currentSnapshot = nullptr;
    for (UiTextureId textureId : pendingTextureReleases)
    {
        textures.erase(textureId);
    }
    pendingTextureReleases.clear();
}

void RmlUiRenderInterface::AppendTextureSnapshots(std::vector<UiTextureSnapshot>& outTextures) const
{
    for (const auto& textureEntry : textures)
    {
        const TextureRecord& texture = textureEntry.second;
        if (texture.pixels == nullptr)
        {
            continue;
        }

        UiTextureSnapshot snapshot;
        snapshot.id = texture.id;
        snapshot.generation = texture.generation;
        snapshot.width = texture.width;
        snapshot.height = texture.height;
        snapshot.source = texture.source;
        snapshot.rgba8Pixels = texture.pixels;
        outTextures.push_back(std::move(snapshot));
    }
}

Rml::TextureHandle RmlUiRenderInterface::RegisterTexture(
    uint32_t width,
    uint32_t height,
    std::string source,
    std::shared_ptr<const std::vector<uint8_t>> pixels)
{
    TextureRecord record;
    record.id = nextTextureId++;
    record.generation = nextTextureGeneration++;
    record.width = width;
    record.height = height;
    record.source = std::move(source);
    record.pixels = std::move(pixels);

    const UiTextureId id = record.id;
    textures.emplace(id, std::move(record));
    return static_cast<Rml::TextureHandle>(id);
}

UiClipRect RmlUiRenderInterface::BuildClipRect() const
{
    UiClipRect clipRect;
    if (currentSnapshot == nullptr)
    {
        return clipRect;
    }

    if (!scissorEnabled)
    {
        clipRect.width = currentSnapshot->viewportWidth;
        clipRect.height = currentSnapshot->viewportHeight;
        return clipRect;
    }

    const int left = std::max(0, scissorRegion.Left());
    const int top = std::max(0, scissorRegion.Top());
    const int right = std::min(static_cast<int>(currentSnapshot->viewportWidth), scissorRegion.Right());
    const int bottom = std::min(static_cast<int>(currentSnapshot->viewportHeight), scissorRegion.Bottom());
    clipRect.x = left;
    clipRect.y = top;
    clipRect.width = right > left ? static_cast<uint32_t>(right - left) : 0;
    clipRect.height = bottom > top ? static_cast<uint32_t>(bottom - top) : 0;
    return clipRect;
}

uint32_t RmlUiRenderInterface::PackColor(const Rml::ColourbPremultiplied& color)
{
    return static_cast<uint32_t>(color.red) |
        (static_cast<uint32_t>(color.green) << 8u) |
        (static_cast<uint32_t>(color.blue) << 16u) |
        (static_cast<uint32_t>(color.alpha) << 24u);
}

void RmlUiRenderInterface::PremultiplyAlpha(std::vector<uint8_t>& pixels)
{
    for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
    {
        const uint32_t alpha = pixels[offset + 3];
        pixels[offset + 0] = static_cast<uint8_t>((static_cast<uint32_t>(pixels[offset + 0]) * alpha + 127u) / 255u);
        pixels[offset + 1] = static_cast<uint8_t>((static_cast<uint32_t>(pixels[offset + 1]) * alpha + 127u) / 255u);
        pixels[offset + 2] = static_cast<uint8_t>((static_cast<uint32_t>(pixels[offset + 2]) * alpha + 127u) / 255u);
    }
}

} // namespace VL
