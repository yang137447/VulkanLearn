#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editorJsonAssetTypes.h"

namespace VL::Editor
{
struct ThumbnailRenderRequest
{
    uint64_t id = 0;
    uint64_t epoch = 0;
    std::string path;
    EditorUi::EditorAssetKind kind = EditorUi::EditorAssetKind::Mesh;
};
// 独立 CPU worker；可见集合替换队列，绝不加载 World 或调用 Vulkan。
// GT 只提交路径并 Poll，不在 RmlUi 的 LoadTexture 回调中做文件 I/O。
class EditorThumbnailLibrary
{
public:
    static constexpr size_t MaxEntries = 192;
    EditorThumbnailLibrary();
    ~EditorThumbnailLibrary();
    bool Initialize(const std::filesystem::path& resourceRoot,
        const std::filesystem::path& cacheRoot, std::string& error);
    void RequestVisible(const std::vector<std::string>& paths);
    void Refresh();
    void Poll();
    void SetPaused(bool paused);
    std::optional<ThumbnailRenderRequest> TakeRenderRequest();
    bool IsRenderRequestCurrent(const ThumbnailRenderRequest& request) const;
    void CompleteRenderRequest(const ThumbnailRenderRequest& request,
        std::shared_ptr<const EditorUi::EditorTexturePreview> image, const std::string& error);
    std::shared_ptr<const EditorUi::EditorThumbnailSnapshot> GetSnapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace VL::Editor
