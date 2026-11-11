#include "renderSystem.h"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "materialInstance.h"
#include "texture.h"
#include "textureAssetLoader.h"
#include "commonFunction.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererDescriptorContext.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "world/world.h"

void RenderSystem::ApplyScriptMaterialCommands(
    const std::vector<VL::ScriptMaterialCommand>& commands)
{
    if (commands.empty()) return;
    std::lock_guard<std::mutex> lock(scriptMaterialMutex);
    pendingScriptMaterialCommands.insert(
        pendingScriptMaterialCommands.end(),
        commands.begin(), commands.end());
}

void RenderSystem::ApplyPendingScriptMaterialCommands()
{
    std::vector<VL::ScriptMaterialCommand> commands;
    {
        std::lock_guard<std::mutex> lock(scriptMaterialMutex);
        commands.swap(pendingScriptMaterialCommands);
    }
    if (commands.empty()) return;
    std::vector<std::string> errors;
    VL::RendererResourceCache& activeCache =
        VL::RendererResourceCache::GetInstance();
    for (const VL::ScriptMaterialCommand& command : commands)
    {
        const std::shared_ptr<MaterialInstance> found =
            activeCache.GetMaterialInstance(command.materialInstanceKey);
        if (found == nullptr)
        {
            errors.push_back(
                "Material instance is not bound in the active world: " +
                command.materialInstanceKey);
            continue;
        }
        MaterialInstance& instance = *found;
        if (!command.textureAssetPath.empty())
        {
            if (!instance.HasTexture(command.parameterName))
            {
                errors.push_back(
                    "Material texture parameter not found: " + command.parameterName +
                    " (" + command.materialInstanceKey + ")");
                continue;
            }
            const TextureBindingLoadDesc textureLoadDesc =
                LoadTextureAssetDesc(command.textureAssetPath);
            const std::string textureCacheKey =
                BuildTextureCacheKey(textureLoadDesc);
            std::shared_ptr<Texture> texture;
            std::shared_ptr<Texture> cachedTexture =
                activeCache.GetTexture(textureCacheKey);
            if (cachedTexture != nullptr)
                texture = std::move(cachedTexture);
            else
            {
                const Texture::CreateDesc createDesc =
                    ToTextureCreateDesc(textureLoadDesc);
                if (textureLoadDesc.textureArray)
                {
                    // 数组走 CPU 逐层解码；层序必须与纹理资产 sources 顺序一致。
                    std::vector<std::filesystem::path> sources;
                    sources.reserve(textureLoadDesc.sources.size());
                    for (const std::string& source : textureLoadDesc.sources)
                        sources.push_back(CommonFunction::Path(source));
                    const HostImage image = TextureIO::LoadArray(
                        sources,
                        Texture::MaterialLoadOptionsForPath(
                            createDesc, sources.front()));
                    texture = std::make_shared<Texture>(
                        *rendererBackend, image, createDesc);
                }
                else
                {
                    texture = std::make_shared<Texture>(
                        *rendererBackend,
                        textureLoadDesc.sources.front(),
                        createDesc);
                }
                activeCache.BindTexture(textureCacheKey, texture);
            }
            instance.SetTexture(
                command.parameterName,
                texture,
                command.textureAssetPath,
                textureCacheKey);
            // 贴图换了，对象描述符集必须重建；延后到下一次 frame begin 的 acquired image，
            // 避免改写仍在飞行中的描述符集。
            const uint32_t imageCount = rendererBackend->GetSwapchainImageCount();
            const uint32_t imageMask = imageCount >= 32
                ? 0xFFFFFFFFu : ((1u << imageCount) - 1u);
            bool pending = false;
            for (PendingMaterialTextureRefresh& refresh : pendingMaterialTextureRefresh)
                if (refresh.materialInstanceKey == command.materialInstanceKey)
                {
                    refresh.remainingImagesMask = imageMask;
                    pending = true;
                    break;
                }
            if (!pending)
                pendingMaterialTextureRefresh.push_back({
                    command.materialInstanceKey,
                    imageMask});
            continue;
        }
        const std::unordered_map<std::string, ParamMap> parameters =
            instance.GetParameters();
        const auto parameterIt = parameters.find(command.parameterName);
        if (parameterIt == parameters.end())
        {
            errors.push_back(
                "Material parameter not found: " + command.parameterName +
                " (" + command.materialInstanceKey + ")");
            continue;
        }
        const bool typeMatches =
            (command.valueCount == 1 && parameterIt->second.type == ParamType::Float) ||
            (command.valueCount == 3 && parameterIt->second.type == ParamType::Vec3) ||
            (command.valueCount == 4 && parameterIt->second.type == ParamType::Vec4);
        if (!typeMatches)
        {
            errors.push_back(
                "Material parameter type mismatch: " + command.parameterName +
                " (" + command.materialInstanceKey + ")");
            continue;
        }
        if (command.valueCount == 1)
            instance.SetParameter(command.parameterName, command.values[0]);
        else if (command.valueCount == 3)
            instance.SetParameter(command.parameterName, Eigen::Vector3f(
                command.values[0], command.values[1], command.values[2]));
        else
            instance.SetParameter(command.parameterName, Eigen::Vector4f(
                command.values[0], command.values[1],
                command.values[2], command.values[3]));
    }
    if (!errors.empty())
    {
        std::lock_guard<std::mutex> lock(scriptMaterialMutex);
        pendingScriptMaterialErrors.insert(
            pendingScriptMaterialErrors.end(),
            std::make_move_iterator(errors.begin()),
            std::make_move_iterator(errors.end()));
    }
}

void RenderSystem::RefreshPendingMaterialTextureDescriptors()
{
    if (pendingMaterialTextureRefresh.empty()) return;
    VL::RendererResourceCache& activeCache =
        VL::RendererResourceCache::GetInstance();
    const VL::RendererDescriptorContext descriptorContext =
        BuildRendererDescriptorContext();
    for (auto it = pendingMaterialTextureRefresh.begin();
         it != pendingMaterialTextureRefresh.end();)
    {
        const std::shared_ptr<MaterialInstance> material =
            activeCache.GetMaterialInstance(it->materialInstanceKey);
        if (material == nullptr)
        {
            it = pendingMaterialTextureRefresh.erase(it);
            continue;
        }
        const uint32_t imageBit = 1u << swapChainImageIndex;
        if ((it->remainingImagesMask & imageBit) != 0)
        {
            for (VL::RendererObjectResourceEntry* entry :
                 activeCache.FindObjectResourcesByMaterial(it->materialInstanceKey))
            {
                if (entry == nullptr) continue;
                entry->RefreshMaterialDescriptors(
                    *rendererBackend,
                    descriptorContext,
                    *material,
                    swapChainImageIndex);
            }
            it->remainingImagesMask &= ~imageBit;
        }
        if (it->remainingImagesMask == 0)
            it = pendingMaterialTextureRefresh.erase(it);
        else
            ++it;
    }
}

void RenderSystem::PublishScriptMaterialState()
{
    std::vector<std::string> materialInstanceKeys;
    for (const VL::RenderDrawPacket& draw : currentRenderScene.drawPackets)
    {
        const std::string& key = draw.materialInstance.key;
        if (key.find("#bp/") == std::string::npos) continue;
        if (std::find(materialInstanceKeys.begin(), materialInstanceKeys.end(), key) ==
            materialInstanceKeys.end())
            materialInstanceKeys.push_back(key);
    }
    nlohmann::json result = nlohmann::json::object();
    VL::RendererResourceCache& activeCache =
        VL::RendererResourceCache::GetInstance();
    for (const std::string& key : materialInstanceKeys)
    {
        const std::shared_ptr<MaterialInstance> found =
            activeCache.GetMaterialInstance(key);
        if (found == nullptr) continue;
        MaterialInstance& instance = *found;
        nlohmann::json values = nlohmann::json::object();
        for (const auto& [name, parameter] : instance.GetParameters())
        {
            switch (parameter.type)
            {
            case ParamType::Float:
                values[name] = nlohmann::json::array({instance.GetParameter<float>(name)});
                break;
            case ParamType::Vec2:
            {
                const Eigen::Vector2f value = instance.GetParameter<Eigen::Vector2f>(name);
                values[name] = nlohmann::json::array({value.x(), value.y()});
                break;
            }
            case ParamType::Vec3:
            {
                const Eigen::Vector3f value = instance.GetParameter<Eigen::Vector3f>(name);
                values[name] = nlohmann::json::array({value.x(), value.y(), value.z()});
                break;
            }
            case ParamType::Vec4:
            {
                const Eigen::Vector4f value = instance.GetParameter<Eigen::Vector4f>(name);
                values[name] = nlohmann::json::array(
                    {value.x(), value.y(), value.z(), value.w()});
                break;
            }
            }
        }
        if (!values.empty()) result[key] = std::move(values);
    }
    std::lock_guard<std::mutex> lock(scriptMaterialMutex);
    if (result != scriptMaterialState)
    {
        scriptMaterialState = std::move(result);
        scriptMaterialStateDirty = true;
    }
}

bool RenderSystem::TakeScriptMaterialState(nlohmann::json& state)
{
    std::lock_guard<std::mutex> lock(scriptMaterialMutex);
    if (!scriptMaterialStateDirty) return false;
    scriptMaterialStateDirty = false;
    state = scriptMaterialState;
    return true;
}

void RenderSystem::TakeScriptMaterialErrors(std::vector<std::string>& errors)
{
    std::lock_guard<std::mutex> lock(scriptMaterialMutex);
    if (pendingScriptMaterialErrors.empty()) return;
    errors.insert(
        errors.end(),
        std::make_move_iterator(pendingScriptMaterialErrors.begin()),
        std::make_move_iterator(pendingScriptMaterialErrors.end()));
    pendingScriptMaterialErrors.clear();
}

void RenderSystem::EnsureBlueprintMaterialInstances(VL::RenderScene& renderScene)
{
    VL::RendererResourceCache& activeCache =
        VL::RendererResourceCache::GetInstance();
    for (VL::RenderDrawPacket& draw : renderScene.drawPackets)
    {
        if (draw.materialInstanceParentKey.empty()) continue;
        const std::string& key = draw.materialInstance.key;
        if (key.empty()) continue;
        const std::shared_ptr<MaterialInstance> existing =
            activeCache.GetMaterialInstance(key);
        if (existing != nullptr) continue;
        const std::shared_ptr<MaterialInstance> parent =
            activeCache.GetMaterialInstance(draw.materialInstanceParentKey);
        if (parent == nullptr) continue;
        // 只复制 CPU 参数；GPU UBO/描述符由对象资源注册时 RenderInitialize 建立。
        activeCache.BindMaterialInstance(key, parent->Clone(key));
    }
}
