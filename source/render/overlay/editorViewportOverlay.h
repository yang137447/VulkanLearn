#pragma once

#include <vulkan/vulkan.hpp>

namespace VL
{

class RendererBackendVulkan;

// Runtime-owned seam for editor-only viewport geometry. RenderSystem knows
// only this interface; the editor owns the concrete grid and its lifetime.
class IEditorViewportOverlay
{
public:
    virtual ~IEditorViewportOverlay() = default;

    virtual void EnsureCreated(RendererBackendVulkan& rendererBackend) = 0;
    virtual bool IsCreated() const noexcept = 0;
    virtual void Bind(vk::CommandBuffer& commandBuffer) = 0;
    virtual void Draw(vk::CommandBuffer& commandBuffer) = 0;
    virtual void Shutdown(RendererBackendVulkan& rendererBackend) = 0;
};

} // namespace VL
