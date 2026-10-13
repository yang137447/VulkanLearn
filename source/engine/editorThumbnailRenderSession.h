#pragma once

#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "editor/assets/editorJsonAssetTypes.h"

class PipelineFactory;
namespace VL
{
class RendererBackendVulkan;

// GT 稳定帧边界独占推进；不向 WorldManager 发布，不借用活动图的 UBO/descriptor。
class EditorThumbnailRenderSession
{
public:
    EditorThumbnailRenderSession(RendererBackendVulkan& backend, PipelineFactory& factory);
    ~EditorThumbnailRenderSession();
    void Begin(const std::string& path, EditorUi::EditorAssetKind kind,
        const nlohmann::json& graph);
    // 每次只推进一个准备阶段或一个 IBL mip；返回 true 表示完成（包括失败）。
    bool Step();
    std::shared_ptr<const EditorUi::EditorTexturePreview> Image() const;
    const std::string& Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
