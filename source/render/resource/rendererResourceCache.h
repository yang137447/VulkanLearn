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

// Renderer-side CPU resource cache. Global resources survive world reloads;
// world-local resources are captured/restored around a staged load and retired
// through ResourceRetireQueue when replaced.
class RendererResourceCache
{
public:
    struct WorldLocalResourceSnapshot
    {
        uint64_t ownerGeneration = 0;
        std::unordered_map<std::string, std::shared_ptr<Texture>> worldTextures;
        std::unordered_map<std::string, std::shared_ptr<RenderableObject>> renderableObjects;
        std::unordered_map<std::string, std::shared_ptr<Material>> materials;
        std::unordered_map<std::string, std::shared_ptr<MaterialInstance>> materialInstances;
        std::unordered_map<std::string, std::shared_ptr<RendererObjectResourceEntry>> objectResources;
        std::unordered_map<std::string, std::shared_ptr<Texture>> textures;
    };

    static RendererResourceCache& GetInstance()
    {
        static RendererResourceCache instance;
        return instance;
    }

    void Clear();
    void BeginWorldLocalResourceLoad(uint64_t ownerGeneration);
    // Used by WorldTransitionCoordinator rollback. The snapshot holds shared
    // references to the active world's renderer resources while a new world is
    // being loaded, so a failed load can restore the old binding table.
    WorldLocalResourceSnapshot CaptureWorldLocalResources() const;
    void RestoreWorldLocalResources(WorldLocalResourceSnapshot snapshot);
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

private:
    RendererResourceCache() = default;
    void ClearWorldLocalResources();

    uint64_t currentWorldGeneration = 0;
    std::unordered_map<std::string, std::shared_ptr<Texture>> globalTextures;
    std::unordered_map<std::string, std::shared_ptr<Texture>> worldTextures;
    std::unordered_map<std::string, std::shared_ptr<RenderableObject>> renderableObjects;
    std::unordered_map<std::string, std::shared_ptr<Material>> materials;
    std::unordered_map<std::string, std::shared_ptr<MaterialInstance>> materialInstances;
    std::unordered_map<std::string, std::shared_ptr<RendererObjectResourceEntry>> objectResources;
    std::unordered_map<std::string, std::shared_ptr<Texture>> textures;
};

} // namespace VL
