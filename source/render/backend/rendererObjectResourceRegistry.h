#pragma once

#include <string>

#include "render/backend/rendererObjectGpuResources.h"

class MaterialInstance;

namespace VL
{

struct RendererDescriptorContext;
class RendererBackendVulkan;
class RendererResourceCache;
struct RenderScene;
struct ResolvedRenderScene;

// Backend-owned GPU resource entry for one render object. The resource cache
// keeps these entries as world-local resources so they can retire after the GPU
// finishes the epoch that may still reference their descriptor sets and buffers.
class RendererObjectResourceEntry
{
public:
    explicit RendererObjectResourceEntry(std::string objectName);
    ~RendererObjectResourceEntry();

    RendererObjectResourceEntry(const RendererObjectResourceEntry&) = delete;
    RendererObjectResourceEntry& operator=(const RendererObjectResourceEntry&) = delete;

    RendererObjectGpuResources& GetResources() { return resources; }
    const RendererObjectGpuResources& GetResources() const { return resources; }
    const std::string& GetObjectName() const { return objectName; }

    void Initialize(
        RendererBackendVulkan& rendererBackend,
        const RendererDescriptorContext& descriptorContext,
        MaterialInstance& materialInstance);
    void Shutdown();

private:
    std::string objectName;
    RendererBackendVulkan* rendererBackend = nullptr;
    RendererObjectGpuResources resources;
};

// Resolves and initializes backend object GPU resources for a resolved scene.
// RenderSystem owns frame orchestration; this registry owns the cache lookup and
// creation policy for object resource entries.
class RendererObjectResourceRegistry
{
public:
    void InitializeResolvedSceneResources(
        RendererBackendVulkan& rendererBackend,
        const RendererDescriptorContext& descriptorContext,
        const RenderScene& renderScene,
        ResolvedRenderScene& resolvedRenderScene,
        RendererResourceCache& resourceCache) const;
};

} // namespace VL
