#include "ui/rmlUiInterfaces.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

#include <RmlUi/Core/Matrix4.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/TextInputContext.h>
#include <RmlUi/Core/Vector4.h>
#include <RmlUi/Core/Vertex.h>
#include <SDL3/SDL.h>
#include <stb_image.h>

#include "engine/diagnosticsSubsystem.h"
#include "core/logStore.h"
#include "platform/platformWindow.h"
#include "ui/uiColorTransform.h"

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
            diagnostics->ReportError(formattedMessage, "RmlUi");
        }
        else if (type == Rml::Log::LT_WARNING)
        {
            diagnostics->ReportWarning(formattedMessage, "RmlUi");
        }
        else if (type != Rml::Log::LT_DEBUG)
        {
            diagnostics->ReportInfo(formattedMessage, "RmlUi");
        }
    }
    // Debug 原先不输出到 console；仍进入集中日志，未绑定 diagnostics 的早期消息也保留。
    if (type == Rml::Log::LT_DEBUG || diagnostics == nullptr)
        ProcessLog().Append(type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ?
            LogSeverity::Error : type == Rml::Log::LT_WARNING ? LogSeverity::Warning :
            type == Rml::Log::LT_DEBUG ? LogSeverity::Debug : LogSeverity::Info,
            "RmlUi", formattedMessage);
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
    std::vector<UiVertex> vertices;
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
    geometry->vertices.reserve(vertices.size());
    for (const Rml::Vertex& source : vertices)
    {
        UiVertex vertex;
        vertex.position[0] = source.position.x;
        vertex.position[1] = source.position.y;
        vertex.texCoord[0] = source.tex_coord.x;
        vertex.texCoord[1] = source.tex_coord.y;
        // 编译静态几何时转换一次，不在每帧字形/资产网格快照中重复 gamma 计算。
        const auto& color = source.colour;
        vertex.color = linearColorOutput
            ? UiPremultipliedSrgbToLinear(color.red, color.green, color.blue, color.alpha)
            : std::array<float, 4>{color.red / 255.0f, color.green / 255.0f,
                color.blue / 255.0f, color.alpha / 255.0f};
        geometry->vertices.push_back(vertex);
    }
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

    // vector 按几何级数扩容；按每个 draw 精确 reserve 会让整帧顶点被反复复制。
    for (const UiVertex& sourceVertex : geometry->vertices)
    {
        Rml::Vector4f position(
            sourceVertex.position[0] + translation.x,
            sourceVertex.position[1] + translation.y,
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

        UiVertex vertex = sourceVertex;
        vertex.position[0] = position.x;
        vertex.position[1] = position.y;
        currentSnapshot->vertices.push_back(vertex);
    }

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

void RmlUiRenderInterface::PublishTexturePreview(
    const std::shared_ptr<const EditorUi::EditorJsonDocumentSnapshot>& document)
{
    for (auto it = previewSources.begin(); it != previewSources.end();)
        if (it->second.expired())
        {
            Rml::ReleaseTexture(it->first, this);
            it = previewSources.erase(it);
        }
        else ++it;
    if (lastPreviewDocument.lock() == document) return;
    lastPreviewDocument = document;
    if (document && document->texturePreview)
        previewSources[EditorUi::EditorTexturePreviewSource(document->revision)] = document->texturePreview;
}

Rml::TextureHandle RmlUiRenderInterface::LoadTexture(
    Rml::Vector2i& textureDimensions,
    const Rml::String& source)
{
    if (source == SceneViewportTextureSource)
    {
        // 固定占位尺寸只供 RmlUi 布局；图像实际分辨率与所有权都留在 renderer。
        textureDimensions = {1, 1};
        return static_cast<Rml::TextureHandle>(SceneViewportTextureId);
    }
    if (source.compare(0, 21, "vl://texture-preview/") == 0 || source.compare(0, 15, "vl://thumbnail/") == 0)
    {
        const auto gpu = gpuThumbnailSources.find(source);
        if (gpu != gpuThumbnailSources.end())
        {
            const auto image = gpu->second.lock();
            if (!image) return 0;
            textureDimensions = {int(image->width), int(image->height)};
            return RegisterTexture(image->width, image->height, source, {}, image);
        }
        const auto found = previewSources.find(source);
        const auto preview = found == previewSources.end() ? nullptr : found->second.lock();
        if (!preview) return 0;
        auto pixels = preview->pixels;
        if (linearColorOutput) ConvertUiPixelsToLinear(pixels, false);
        textureDimensions = {int(preview->width), int(preview->height)};
        return RegisterTexture(preview->width, preview->height, source,
            std::make_shared<const std::vector<uint8_t>>(std::move(pixels)));
    }
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
    if (linearColorOutput) ConvertUiPixelsToLinear(pixels, false);
    else
    {
        for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
            for (size_t channel = 0; channel < 3; ++channel)
                pixels[offset + channel] = static_cast<uint8_t>(
                    (static_cast<uint32_t>(pixels[offset + channel]) * pixels[offset + 3] + 127u) / 255u);
    }

    textureDimensions = Rml::Vector2i(width, height);
    return RegisterTexture(
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        source,
        std::make_shared<const std::vector<uint8_t>>(std::move(pixels)));
}

void RmlUiRenderInterface::PublishThumbnails(
    const std::shared_ptr<const EditorUi::EditorThumbnailSnapshot>& snapshot)
{
    // 旧发布者可能已释放；weak_ptr 过期和显式空快照都等于 nullptr，
    // 此时仍须撤销旧 source，不能把清理误判成同一份快照。
    if (snapshot && lastThumbnails.lock() == snapshot) return;
    lastThumbnails = snapshot;
    std::unordered_set<std::string> next;
    if (snapshot)
        for (const auto& entry : snapshot->entries)
            if (entry.second.image || entry.second.gpuImage)
            {
                next.insert(entry.second.source);
                if (entry.second.gpuImage)
                    gpuThumbnailSources[entry.second.source] = entry.second.gpuImage;
                else
                    previewSources[entry.second.source] = entry.second.image;
            }
    // LRU 可保留像素或成图 token；离开窗口只释放 Rml 纹理/descriptor，
    // GPU 成图由 renderer 根据剩余 token 与帧 epoch 决定回收。
    for (const auto& source : thumbnailSources)
        if (!next.count(source))
        {
            Rml::ReleaseTexture(source, this);
            previewSources.erase(source);
            gpuThumbnailSources.erase(source);
        }
    thumbnailSources = std::move(next);
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
    if (linearColorOutput) ConvertUiPixelsToLinear(pixels, true);
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
        if (texture.pixels == nullptr && !texture.gpuImage)
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
        snapshot.gpuImage = texture.gpuImage;
        snapshot.linearSampling = linearColorOutput;
        outTextures.push_back(std::move(snapshot));
    }
}

Rml::TextureHandle RmlUiRenderInterface::RegisterTexture(
    uint32_t width,
    uint32_t height,
    std::string source,
    std::shared_ptr<const std::vector<uint8_t>> pixels,
    std::shared_ptr<const RendererGpuImageToken> gpuImage)
{
    TextureRecord record;
    record.id = nextTextureId++;
    record.generation = nextTextureGeneration++;
    record.width = width;
    record.height = height;
    record.source = std::move(source);
    record.pixels = std::move(pixels);
    record.gpuImage = std::move(gpuImage);

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

} // namespace VL
