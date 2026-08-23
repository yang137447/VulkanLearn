#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

class Material;
class MaterialInstance;
class RenderableObject;
class Texture;

namespace VL
{

class RendererObjectResourceEntry;
class SubsurfaceResourceSet;
class HairResourceSet;

// Renderer-side CPU resource cache. Global resources survive world reloads;
// world-local resources are prepared in isolated candidate packages and retired
// through ResourceRetireQueue when replaced.
class RendererResourceCache
{
public:
    struct WorldLocalResourcePackage
    {
        uint64_t ownerGeneration = 0;
        std::unordered_map<std::string, std::shared_ptr<Texture>> worldTextures;
        std::unordered_map<std::string, std::shared_ptr<RenderableObject>> renderableObjects;
        std::unordered_map<std::string, std::shared_ptr<Material>> materials;
        std::unordered_map<std::string, std::shared_ptr<MaterialInstance>> materialInstances;
        std::unordered_map<std::string, std::shared_ptr<RendererObjectResourceEntry>> objectResources;
        std::unordered_map<std::string, std::shared_ptr<Texture>> textures;
        // lookup texture 与 stable ID 只属于当前 World；发布后通过 const 句柄消费。
        std::shared_ptr<const SubsurfaceResourceSet> subsurfaceResources;
        // Hair LUT 与 metadata 属于同一 World-local generation，不能跨 candidate 猜测。
        std::shared_ptr<const HairResourceSet> hairResources;

        bool Empty() const noexcept;
    };

    using WorldLocalResourcePackageHandle =
        std::shared_ptr<WorldLocalResourcePackage>;
    using ImmutableWorldLocalResourceRefs =
        std::shared_ptr<const WorldLocalResourcePackage>;

    static RendererResourceCache& GetInstance()
    {
        static RendererResourceCache instance;
        return instance;
    }

    RendererResourceCache(const RendererResourceCache&) = delete;
    RendererResourceCache& operator=(const RendererResourceCache&) = delete;
    RendererResourceCache(RendererResourceCache&&) noexcept = default;
    RendererResourceCache& operator=(RendererResourceCache&&) noexcept = default;

    void Clear();

    // Creates an isolated cache for candidate construction. Global texture
    // bindings are copied for lookup; all later Bind calls affect only the
    // returned cache instance.
    RendererResourceCache BeginCandidate(uint64_t ownerGeneration) const;

    // Keeps the currently active package alive without exposing mutable maps.
    ImmutableWorldLocalResourceRefs
    CaptureActiveWorldLocalResources() const noexcept;

    // The candidate must come from BeginCandidate(). Commit publishes any
    // missing process-global bindings prepared by the initial candidate and
    // swaps the world-local package pointer. Existing global bindings are
    // immutable, so inherited resources keep the same identity across World
    // generations. The caller owns retirement of the returned old package
    // after the appropriate GPU epoch.
    WorldLocalResourcePackageHandle CommitCandidate(
        RendererResourceCache&& candidate) noexcept;

    void BeginWorldLocalResourceLoad(uint64_t ownerGeneration);
    // Swapchain recreation keeps CPU-side resource bindings, but all
    // per-swapchain buffers and descriptor sets must be rebuilt.
    void ShutdownSwapchainDependentWorldResources();

    void BindGlobalTexture(std::string bindingName, std::shared_ptr<Texture> texture);
    void BindWorldTexture(std::string bindingName, std::shared_ptr<Texture> texture);
    // Descriptor code still asks for global-set texture bindings. The binding
    // may point to a process-global texture such as BRDF LUT or a world-local
    // texture such as the active environment cubemap.
    const std::shared_ptr<Texture>* GetGlobalTexture(std::string_view bindingName) const;
    bool HasGlobalTexture(std::string_view bindingName) const;
    const std::shared_ptr<Texture>* GetWorldTexture(std::string_view bindingName) const;

    void BindRenderableObject(std::string objectKey, std::shared_ptr<RenderableObject> object);
    const std::shared_ptr<RenderableObject>* GetRenderableObject(std::string_view objectKey) const;

    void BindMaterial(std::string materialKey, std::shared_ptr<Material> material);
    const std::shared_ptr<Material>* GetMaterial(std::string_view materialKey) const;

    void BindMaterialInstance(std::string materialInstanceKey, std::shared_ptr<MaterialInstance> materialInstance);
    const std::shared_ptr<MaterialInstance>* GetMaterialInstance(std::string_view materialInstanceKey) const;

    void BindObjectResource(std::string objectName, std::shared_ptr<RendererObjectResourceEntry> objectResource);
    const std::shared_ptr<RendererObjectResourceEntry>* GetObjectResource(std::string_view objectName) const;

    void BindTexture(std::string textureKey, std::shared_ptr<Texture> texture);
    const std::shared_ptr<Texture>* GetTexture(std::string_view textureKey) const;
    // 只绑定已完成的 candidate resource set，不允许 material loader 修改其内容。
    void BindSubsurfaceResources(
        std::shared_ptr<const SubsurfaceResourceSet> resources);
    const std::shared_ptr<const SubsurfaceResourceSet>&
    GetSubsurfaceResources() const noexcept;
    void BindHairResources(
        std::shared_ptr<const HairResourceSet> resources);
    const std::shared_ptr<const HairResourceSet>&
    GetHairResources() const noexcept;

private:
    RendererResourceCache();
    RendererResourceCache(
        std::unordered_map<std::string, std::shared_ptr<Texture>>
            inheritedGlobalTextures,
        uint64_t ownerGeneration);
    WorldLocalResourcePackage& GetMutableWorldLocalResources();
    void ClearWorldLocalResources();

    std::unordered_map<std::string, std::shared_ptr<Texture>> globalTextures;
    WorldLocalResourcePackageHandle worldLocalResources;
    bool retireWorldLocalResourcesOnClear = true;
};

} // namespace VL
