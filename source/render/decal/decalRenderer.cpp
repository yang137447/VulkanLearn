#include "render/decal/decalRenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <Eigen/Geometry>

#include "commonFunction.h"
#include "core/logStore.h"
#include "pipeline/pipelineFactory.h"
#include "pipeline/pipelineBase.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/frontend/renderScene.h"
#include "render/rendergraph/renderGraphPassType.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"
#include "texture.h"
#include "textureAssetLoader.h"
#include "VertexDataStruct.h"

namespace VL
{
namespace
{

constexpr uint32_t kDecalCubeVertexCount = 8;
constexpr uint32_t kDecalCubeIndexCount = 36;
const Renderpass* FindDecalPass(const RenderGraph& graph)
{
    for (const auto& [name, pass] : graph.GetRenderpasses())
    {
        (void)name;
        if (pass.type == RenderGraphPassType::Decal)
            return &pass;
    }
    return nullptr;
}

std::array<Vertex, kDecalCubeVertexCount> UnitCubeVertices()
{
    std::array<Vertex, kDecalCubeVertexCount> vertices{};
    const std::array<Eigen::Vector3f, kDecalCubeVertexCount> positions = {
        Eigen::Vector3f(-1.0f, -1.0f, -1.0f),
        Eigen::Vector3f( 1.0f, -1.0f, -1.0f),
        Eigen::Vector3f( 1.0f,  1.0f, -1.0f),
        Eigen::Vector3f(-1.0f,  1.0f, -1.0f),
        Eigen::Vector3f(-1.0f, -1.0f,  1.0f),
        Eigen::Vector3f( 1.0f, -1.0f,  1.0f),
        Eigen::Vector3f( 1.0f,  1.0f,  1.0f),
        Eigen::Vector3f(-1.0f,  1.0f,  1.0f)};
    for (uint32_t index = 0; index < kDecalCubeVertexCount; ++index)
    {
        vertices[index].position = positions[index];
        vertices[index].normal = Eigen::Vector3f::Zero();
        vertices[index].color = Eigen::Vector4f::Ones();
        vertices[index].texCoord = Eigen::Vector2f::Zero();
        vertices[index].tangent = Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f);
    }
    return vertices;
}

std::array<uint32_t, kDecalCubeIndexCount> UnitCubeIndices()
{
    return {
        0, 2, 1, 0, 3, 2,
        4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4,
        2, 3, 7, 2, 7, 6,
        1, 2, 6, 1, 6, 5,
        3, 0, 4, 3, 4, 7};
}

// 1x1 常量纹理：decal 没有对应贴图槽位时仍需要合法的 combined image
// sampler，shader 用实例标志位区分“默认值”和“真实贴图”。
std::shared_ptr<Texture> CreateSolidTexture(
    RendererBackendVulkan& renderer,
    const std::array<uint8_t, 4>& rgba,
    HostImage::PixelFormat format,
    HostImage::TextureSemantic semantic,
    const char* debugName)
{
    HostImage image;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.format = format;
    image.semantic = semantic;
    image.rowStrideBytes = 4;
    image.data.assign(rgba.begin(), rgba.end());

    Texture::CreateDesc desc;
    desc.semantic = semantic;
    desc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    desc.generateMipmaps = false;
    desc.filter = vk::Filter::eLinear;
    desc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    desc.debugName = debugName;
    return std::make_shared<Texture>(renderer, image, desc);
}

} // namespace

void DecalRenderer::EnsureInitialized(
    PipelineFactory& factory,
    RendererBackendVulkan& renderer,
    RenderGraph& graph)
{
    if (initialized)
        return;
    Initialize(factory, renderer, graph);
}

void DecalRenderer::Initialize(
    PipelineFactory& factory,
    RendererBackendVulkan& renderer,
    RenderGraph& graph)
{
    backend = &renderer;
    const Renderpass* decalPass = FindDecalPass(graph);
    if (decalPass == nullptr)
    {
        initialized = false;
        return;
    }

    ShaderVariantKey variant;
    variant.shaderName = "decal/decal";
    variant.renderMode = RenderMode::Opaque;
    variant.shadingModelMacro = "SHADING_MODEL_UNLIT";
    const GraphicsShaderVariantArtifact& artifact =
        factory.PrepareGraphicsShaderVariant(variant);

    PassPipelineContractKey contract =
        decalPass->pipelineContractKey;
    contract.useVertexInput = true;
    contract.depthTestEnable = false;
    contract.depthWriteEnable = false;
    contract.depthCompareOp = vk::CompareOp::eAlways;
    Renderpass* mutableDecalPass = const_cast<Renderpass*>(decalPass);
    pipeline = factory.CreateGraphicsPipeline(
        &mutableDecalPass->renderPass,
        contract,
        artifact,
        // 闭合 cube 只保留正面；否则同一 sceneDepth 表面会被前后两面
        // 各混合一次。索引绕序与 pipeline 的 clockwise front face 对齐。
        vk::CullModeFlagBits::eBack,
        // DBuffer A/B/C/D 的 alpha 是逐通道覆盖率，mask 附件则用
        // One/One 累积响应位；两种状态由 DecalDBuffer 模式分开设置。
        GraphicsPipelineBlendMode::DecalDBuffer);
    if (!pipeline)
        throw std::runtime_error("DecalRenderer failed to create its graphics pipeline");

    CreateStaticGeometry(renderer);
    CreateMaterialTextures(renderer);
    CreateFrameResources(renderer);
    initialized = true;
}

void DecalRenderer::CreateMaterialTextures(
    RendererBackendVulkan& renderer)
{
    // 默认 albedo 用 sRGB 白图：采样后仍是 1.0，不改变作者 color。
    defaultAlbedoTexture = CreateSolidTexture(
        renderer,
        {255, 255, 255, 255},
        HostImage::PixelFormat::RGBA8_SRGB,
        HostImage::TextureSemantic::Color,
        "DecalDefaultAlbedo");
    // 默认切线空间法线 (0,0,1)；normalMap 缺失时 shader 走程序化 FBM，
    // 这张 flat normal 只用于保证 set 1 绑定完整。
    defaultNormalTexture = CreateSolidTexture(
        renderer,
        {128, 128, 255, 255},
        HostImage::PixelFormat::RGBA8_UNORM,
        HostImage::TextureSemantic::Normal,
        "DecalDefaultNormal");
    // ORM 白图：R=roughness/G=metallic/B=AO 都为 1，使
    // instance.surface 的作者值原样通过。
    defaultOrmTexture = CreateSolidTexture(
        renderer,
        {255, 255, 255, 255},
        HostImage::PixelFormat::RGBA8_UNORM,
        HostImage::TextureSemantic::Data,
        "DecalDefaultOrm");
}

void DecalRenderer::CreateStaticGeometry(
    RendererBackendVulkan& renderer)
{
    const auto vertices = UnitCubeVertices();
    const auto indices = UnitCubeIndices();
    const vk::DeviceSize vertexBytes = sizeof(Vertex) * vertices.size();
    const vk::DeviceSize indexBytes = sizeof(uint32_t) * indices.size();

    auto createHostBuffer = [&renderer](
        vk::DeviceSize bytes,
        vk::BufferUsageFlags usage,
        const char* name)
    {
        GpuBuffer buffer{};
        std::tie(buffer.buffer, buffer.memory) = renderer.CreateBuffer(
            bytes,
            usage,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            name);
        buffer.mapped = renderer.MapMemory(buffer.memory, bytes);
        if (buffer.mapped == nullptr)
            throw std::runtime_error(std::string("DecalRenderer failed to map ") + name);
        return buffer;
    };

    cubeVertices = createHostBuffer(
        vertexBytes,
        vk::BufferUsageFlagBits::eVertexBuffer,
        "DecalCubeVertices");
    cubeIndices = createHostBuffer(
        indexBytes,
        vk::BufferUsageFlagBits::eIndexBuffer,
        "DecalCubeIndices");
    std::memcpy(cubeVertices.mapped, vertices.data(), vertexBytes);
    std::memcpy(cubeIndices.mapped, indices.data(), indexBytes);
    renderer.UnmapMemory(cubeVertices.memory);
    renderer.UnmapMemory(cubeIndices.memory);
    cubeVertices.mapped = nullptr;
    cubeIndices.mapped = nullptr;
}

void DecalRenderer::CreateFrameResources(
    RendererBackendVulkan& renderer)
{
    if (!pipeline)
        throw std::runtime_error("DecalRenderer has no pipeline");

    const uint32_t imageCount = renderer.GetSwapchainImageCount();
    frameUniforms.resize(imageCount);
    instanceBuffers.resize(imageCount);
    descriptorSets.resize(imageCount);

    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer, imageCount},
        {vk::DescriptorType::eStorageBuffer, imageCount}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount);
    descriptorPool = renderer.CreateDescriptorPool(
        poolInfo,
        "DecalRendererDescriptors");

    const auto& layouts = pipeline->GetDescriptorSetLayouts();
    if (layouts.empty())
        throw std::runtime_error("DecalRenderer shader has no set 0 layout");
    if (layouts.size() < MaterialSetIndex + 1)
    {
        throw std::runtime_error(
            "DecalRenderer shader must declare set 1 material textures");
    }

    vk::DescriptorSetAllocateInfo allocateInfo;
    const std::vector<vk::DescriptorSetLayout> setLayouts(
        imageCount,
        layouts.at(0));
    allocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayouts)
        .setDescriptorSetCount(imageCount);
    renderer.AllocateDescriptorSets(allocateInfo, descriptorSets);

    // 逐 image × 逐实例槽位的 material descriptor：set 1 只有 3 个
    // combined image sampler，不与其他 decal 共享可变绑定。
    const uint32_t materialSetCount =
        imageCount * kMaxDecalInstances;
    std::vector<vk::DescriptorPoolSize> materialPoolSizes{
        {vk::DescriptorType::eCombinedImageSampler,
         materialSetCount * 3}};
    vk::DescriptorPoolCreateInfo materialPoolInfo;
    materialPoolInfo
        .setPoolSizes(materialPoolSizes)
        .setMaxSets(materialSetCount);
    materialDescriptorPool = renderer.CreateDescriptorPool(
        materialPoolInfo,
        "DecalMaterialDescriptors");

    vk::DescriptorSetAllocateInfo materialAllocateInfo;
    const std::vector<vk::DescriptorSetLayout> materialLayouts(
        materialSetCount,
        layouts.at(MaterialSetIndex));
    materialAllocateInfo
        .setDescriptorPool(materialDescriptorPool)
        .setSetLayouts(materialLayouts)
        .setDescriptorSetCount(materialSetCount);
    std::vector<vk::DescriptorSet> allocatedMaterialSets;
    renderer.AllocateDescriptorSets(
        materialAllocateInfo,
        allocatedMaterialSets);
    materialDescriptorSets.assign(imageCount, {});
    descriptorTextureStates.assign(imageCount, {});
    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        const size_t offset =
            static_cast<size_t>(imageIndex) * kMaxDecalInstances;
        materialDescriptorSets[imageIndex].assign(
            allocatedMaterialSets.begin() + offset,
            allocatedMaterialSets.begin() + offset + kMaxDecalInstances);
        descriptorTextureStates[imageIndex].resize(kMaxDecalInstances);
    }

    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        const auto createMapped = [&renderer, imageIndex](
            vk::DeviceSize bytes,
            vk::BufferUsageFlags usage,
            const char* name)
        {
            GpuBuffer buffer{};
            std::tie(buffer.buffer, buffer.memory) = renderer.CreateBuffer(
                bytes,
                usage,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                std::string(name) + ":" + std::to_string(imageIndex));
            buffer.mapped = renderer.MapMemory(buffer.memory, bytes);
            if (buffer.mapped == nullptr)
                throw std::runtime_error(
                    std::string("DecalRenderer failed to map ") + name);
            return buffer;
        };

        frameUniforms[imageIndex] = createMapped(
            sizeof(FrameUniforms),
            vk::BufferUsageFlagBits::eUniformBuffer,
            "DecalFrameUniforms");
        instanceBuffers[imageIndex] = createMapped(
            sizeof(DecalInstance) * kMaxDecalInstances,
            vk::BufferUsageFlagBits::eStorageBuffer,
            "DecalInstances");

        const vk::DescriptorBufferInfo uniformInfo(
            frameUniforms[imageIndex].buffer,
            0,
            sizeof(FrameUniforms));
        const vk::DescriptorBufferInfo instanceInfo(
            instanceBuffers[imageIndex].buffer,
            0,
            sizeof(DecalInstance) * kMaxDecalInstances);
        vk::WriteDescriptorSet uniformWrite;
        uniformWrite
            .setDstSet(descriptorSets[imageIndex])
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eUniformBuffer)
            .setBufferInfo(uniformInfo);
        vk::WriteDescriptorSet instanceWrite = uniformWrite;
        instanceWrite
            .setDstBinding(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setBufferInfo(instanceInfo);
        renderer.UpdateDescriptorSets({uniformWrite, instanceWrite});
    }
}

void DecalRenderer::Shutdown(RendererBackendVulkan& renderer)
{
    if (!initialized &&
        static_cast<VkDescriptorPool>(descriptorPool) == VK_NULL_HANDLE &&
        static_cast<VkDescriptorPool>(materialDescriptorPool) ==
            VK_NULL_HANDLE)
        return;
    DestroyResources(renderer);
    initialized = false;
    backend = nullptr;
}

void DecalRenderer::InvalidatePipelines(RendererBackendVulkan& renderer)
{
    if (!initialized)
        return;
    renderer.WaitIdle();
    Shutdown(renderer);
}

void DecalRenderer::DestroyResources(RendererBackendVulkan& renderer)
{
    for (GpuBuffer& buffer : frameUniforms)
        DestroyBuffer(renderer, buffer);
    for (GpuBuffer& buffer : instanceBuffers)
        DestroyBuffer(renderer, buffer);
    frameUniforms.clear();
    instanceBuffers.clear();
    DestroyBuffer(renderer, cubeVertices);
    DestroyBuffer(renderer, cubeIndices);
    // 描述符集随 pool 一起释放；先清 CPU 句柄，避免 Shutdown 后误用。
    descriptorSets.clear();
    materialDescriptorSets.clear();
    descriptorTextureStates.clear();
    if (static_cast<VkDescriptorPool>(materialDescriptorPool) !=
        VK_NULL_HANDLE)
    {
        renderer.DestroyDescriptorPool(materialDescriptorPool);
        materialDescriptorPool = nullptr;
    }
    if (static_cast<VkDescriptorPool>(descriptorPool) != VK_NULL_HANDLE)
    {
        renderer.DestroyDescriptorPool(descriptorPool);
        descriptorPool = nullptr;
    }
    // 调用方必须已到达设备空闲边界（WaitIdle 或引擎关闭路径）；
    // 已退休纹理集仍由 ResourceRetireQueue 持有。
    textureSets.clear();
    persistentTextureSetIds.clear();
    persistentTextureMissingFrames.clear();
    hasTextureSetWorldGeneration = false;
    textureCache.clear();
    warnedTextureFailures.clear();
    defaultAlbedoTexture.reset();
    defaultNormalTexture.reset();
    defaultOrmTexture.reset();
    pipeline.reset();
}

void DecalRenderer::DestroyBuffer(
    RendererBackendVulkan& renderer,
    GpuBuffer& buffer)
{
    if (buffer.mapped != nullptr)
    {
        renderer.UnmapMemory(buffer.memory);
        buffer.mapped = nullptr;
    }
    if (static_cast<VkBuffer>(buffer.buffer) != VK_NULL_HANDLE)
    {
        renderer.DestroyBuffer(buffer.buffer, buffer.memory);
        buffer.buffer = nullptr;
        buffer.memory = nullptr;
    }
}

void DecalRenderer::Synchronize(const RenderScene& renderScene)
{
    if (!initialized || backend == nullptr)
        return;

    // 粒子 decal 的稳定纹理身份只在同一 World 内保留；World 换代时
    // 清掉持久集合，让旧的动态纹理集按正常 retire 路径释放。
    if (!hasTextureSetWorldGeneration)
    {
        textureSetWorldGeneration = renderScene.worldGeneration;
        hasTextureSetWorldGeneration = true;
    }
    else if (textureSetWorldGeneration != renderScene.worldGeneration)
    {
        persistentTextureSetIds.clear();
        persistentTextureMissingFrames.clear();
        textureSetWorldGeneration = renderScene.worldGeneration;
    }

    bool textureSetChanged = false;
    std::unordered_set<uint64_t> desiredDecalIds;
    desiredDecalIds.reserve(renderScene.decals.size());
    for (const DecalSnapshot& decal : renderScene.decals)
    {
        if (!decal.visible)
            continue;
        desiredDecalIds.insert(decal.decalId);
        if (decal.persistentTextureSet)
        {
            persistentTextureSetIds.insert(decal.decalId);
        }

        std::shared_ptr<DecalTextureSet>& textureSet =
            textureSets[decal.decalId];
        if (!textureSet)
            textureSet = std::make_shared<DecalTextureSet>();

        // 空路径与失败都解析成默认纹理，但路径本身仍是变更检测的一部分：
        // 作者从 "" 改成真实贴图（或反向）必须重新解析。
        if (textureSet->albedoPath == decal.albedoMap &&
            textureSet->normalPath == decal.normalMap &&
            textureSet->ormPath == decal.ormMap &&
            textureSet->albedo &&
            textureSet->normal &&
            textureSet->orm)
        {
            continue;
        }

        // 路径变化时不能原地替换纹理句柄：旧纹理可能仍被前几帧采样。
        // 先把旧集合交给 retire queue，再创建新的集合。
        if (textureSet->albedo || textureSet->normal || textureSet->orm)
            RetireTextureSet(textureSet);
        textureSet = std::make_shared<DecalTextureSet>();
        textureSet->ownerGeneration = renderScene.worldGeneration;

        textureSet->albedoPath = decal.albedoMap;
        textureSet->normalPath = decal.normalMap;
        textureSet->ormPath = decal.ormMap;
        textureSet->albedo = ResolveTexture(
            decal.albedoMap,
            HostImage::TextureSemantic::Color,
            defaultAlbedoTexture,
            decal.name);
        textureSet->normal = ResolveTexture(
            decal.normalMap,
            HostImage::TextureSemantic::Normal,
            defaultNormalTexture,
            decal.name);
        textureSet->orm = ResolveTexture(
            decal.ormMap,
            HostImage::TextureSemantic::Data,
            defaultOrmTexture,
            decal.name);
        // 只有真正加载成功才允许 shader 采法线贴图；默认 flat normal
        // 不能触发贴图分支，否则 normalStrength 会覆盖程序化 FBM。
        textureSet->hasNormalMap =
            !decal.normalMap.empty() &&
            textureSet->normal != defaultNormalTexture;
        textureSetChanged = true;
    }

    for (auto it = textureSets.begin(); it != textureSets.end();)
    {
        if (desiredDecalIds.find(it->first) != desiredDecalIds.end())
        {
            persistentTextureMissingFrames.erase(it->first);
            ++it;
            continue;
        }
        if (persistentTextureSetIds.find(it->first) !=
            persistentTextureSetIds.end())
        {
            // 粒子 decal 寿命之间有短暂空档；保留几帧纹理集，但 emitter
            // 删除/重命名超过 120 帧后必须真正退休。
            constexpr uint32_t kPersistentTextureGraceFrames = 120;
            uint32_t& missingFrames =
                persistentTextureMissingFrames[it->first];
            ++missingFrames;
            if (missingFrames <= kPersistentTextureGraceFrames)
            {
                ++it;
                continue;
            }
            persistentTextureSetIds.erase(it->first);
            persistentTextureMissingFrames.erase(it->first);
        }
        RetireTextureSet(std::move(it->second));
        it = textureSets.erase(it);
        textureSetChanged = true;
    }

    // 只保留仍然存活的弱索引；强引用由活跃或已退休纹理集持有。
    for (auto it = textureCache.begin(); it != textureCache.end();)
    {
        if (it->second.expired())
            it = textureCache.erase(it);
        else
            ++it;
    }

    // 贴图身份变化后，逐槽 descriptor 可能仍指向旧纹理；整体失效一次
    // 比逐槽反查简单，而且只在路径变化/删除时发生。
    if (textureSetChanged)
    {
        for (auto& imageStates : descriptorTextureStates)
            std::fill(imageStates.begin(), imageStates.end(),
                DescriptorTextureState{});
    }
}

std::shared_ptr<Texture> DecalRenderer::ResolveTexture(
    const std::string& assetPath,
    HostImage::TextureSemantic semantic,
    const std::shared_ptr<Texture>& fallback,
    const std::string& decalName)
{
    if (assetPath.empty())
        return fallback;
    if (backend == nullptr)
        return fallback;

    try
    {
        TextureBindingLoadDesc loadDesc =
            LoadTextureAssetDesc(assetPath);
        // 法线/ORM 的 sRGB 语义由资产 JSON 的 colorSpace 决定，这里只
        // 补充宿主纹理语义，供设备层与调试工具区分用途。
        loadDesc.semantic = semantic;
        const std::string cacheKey =
            BuildTextureCacheKey(loadDesc);
        auto cached = textureCache.find(cacheKey);
        if (cached != textureCache.end())
        {
            std::shared_ptr<Texture> texture =
                cached->second.lock();
            if (texture)
                return texture;
            textureCache.erase(cached);
        }

        std::shared_ptr<Texture> texture =
            std::make_shared<Texture>(
                *backend,
                loadDesc.source,
                ToTextureCreateDesc(loadDesc));
        textureCache[cacheKey] = texture;
        return texture;
    }
    catch (const std::exception& exception)
    {
        WarnTextureFallbackOnce(
            assetPath,
            decalName,
            exception.what());
    }
    catch (...)
    {
        WarnTextureFallbackOnce(
            assetPath,
            decalName,
            "unknown texture load failure");
    }
    return fallback;
}

void DecalRenderer::RetireTextureSet(
    std::shared_ptr<DecalTextureSet> textureSet)
{
    if (!textureSet)
        return;
    // decal 消失时，最近若干已提交帧仍可能采样这些纹理；交给 retire
    // queue，等对应 fence 完成后再析构 Texture。
    ResourceRetireQueue& retireQueue =
        ResourceRetireQueue::GetInstance();
    const uint64_t lastUsedEpoch =
        retireQueue.GetLastSubmittedEpoch();
    retireQueue.RetireShared(
        "DecalTextureSet",
        textureSet->ownerGeneration,
        lastUsedEpoch,
        std::move(textureSet));
    retireQueue.CollectCompletedEpoch(
        retireQueue.GetLastCompletedEpoch());
}

void DecalRenderer::WarnTextureFallbackOnce(
    const std::string& assetPath,
    const std::string& decalName,
    const std::string& error)
{
    if (!warnedTextureFailures.insert(assetPath).second)
        return;
    const std::string message =
        "Decal texture fallback for '" + decalName +
        "' (" + assetPath + "): " + error +
        "; using the default decal texture";
    ProcessLog().Append(
        LogSeverity::Warning,
        "Decals",
        message);
    std::cout << "[Decals][Warning] " << message << std::endl;
}

void DecalRenderer::WriteMaterialDescriptors(
    uint32_t imageIndex,
    uint32_t slot,
    const DecalTextureSet& textures)
{
    if (backend == nullptr ||
        imageIndex >= materialDescriptorSets.size() ||
        slot >= materialDescriptorSets[imageIndex].size() ||
        imageIndex >= descriptorTextureStates.size() ||
        slot >= descriptorTextureStates[imageIndex].size())
    {
        return;
    }

    const DescriptorTextureState desired{
        textures.albedo.get(),
        textures.normal.get(),
        textures.orm.get()};
    if (descriptorTextureStates[imageIndex][slot] == desired)
        return;

    std::array<vk::DescriptorImageInfo, 3> imageInfos{
        textures.albedo->GetDescriptorInfo(),
        textures.normal->GetDescriptorInfo(),
        textures.orm->GetDescriptorInfo()};
    const vk::DescriptorSet descriptorSet =
        materialDescriptorSets[imageIndex][slot];

    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding)
    {
        writes[binding]
            .setDstSet(descriptorSet)
            .setDstBinding(binding)
            .setDescriptorCount(1)
            .setDescriptorType(
                vk::DescriptorType::eCombinedImageSampler)
            .setImageInfo(imageInfos[binding]);
    }
    backend->UpdateDescriptorSets(
        {writes[0], writes[1], writes[2]});
    descriptorTextureStates[imageIndex][slot] = desired;
}

void DecalRenderer::RecordGraphics(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass,
    const RenderScene& renderScene)
{
    if (!initialized || !pipeline ||
        imageIndex >= descriptorSets.size() ||
        imageIndex >= frameUniforms.size() ||
        imageIndex >= instanceBuffers.size() ||
        imageIndex >= materialDescriptorSets.size() ||
        materialDescriptorSets[imageIndex].size() < kMaxDecalInstances)
        return;

    std::vector<const DecalSnapshot*> visibleDecals;
    visibleDecals.reserve(renderScene.decals.size());
    for (const DecalSnapshot& decal : renderScene.decals)
    {
        if (decal.visible)
            visibleDecals.push_back(&decal);
    }
    if (visibleDecals.empty())
        return;
    std::sort(
        visibleDecals.begin(),
        visibleDecals.end(),
        [](const DecalSnapshot* left, const DecalSnapshot* right)
        {
            if (left->sortOrder != right->sortOrder)
                return left->sortOrder < right->sortOrder;
            return left->decalId < right->decalId;
        });
    if (visibleDecals.size() > kMaxDecalInstances)
        visibleDecals.resize(kMaxDecalInstances);

    FrameUniforms frame{};
    frame.viewProjection = renderScene.camera.viewProjection;
    frame.invViewProjection =
        renderScene.camera.viewProjection.inverse();
    std::memcpy(
        frameUniforms[imageIndex].mapped,
        &frame,
        sizeof(frame));

    auto* instances = static_cast<DecalInstance*>(
        instanceBuffers[imageIndex].mapped);
    for (size_t index = 0; index < visibleDecals.size(); ++index)
    {
        const DecalSnapshot& source = *visibleDecals[index];
        DecalInstance& target = instances[index];
        const auto textureSetIt =
            textureSets.find(source.decalId);
        const DecalTextureSet* textureSet =
            textureSetIt != textureSets.end()
                ? textureSetIt->second.get()
                : nullptr;
        target.decalToWorld = source.decalToWorld;
        target.worldToDecal = source.decalToWorld.inverse();
        target.color = source.color;
        // color.w 已改为 normalStrength；opacity 只走 surface.w。
        target.color.w() = source.normalStrength;
        target.surface = Eigen::Vector4f(
            source.roughness,
            source.metallic,
            source.ambientOcclusion,
            source.opacity);
        // params.w 传入 DBufferMask 的响应位掩码，shader 解码为
        // color/normal/roughness/metallic 四个 0/1 通道。
        target.params = Eigen::Vector4f(
            source.edgeSoftness,
            source.noiseStrength,
            source.randomSeed,
            static_cast<float>(source.responseMask));
        // w = 1 时 shader 采 normalMap；Synchronize 尚未跑过或加载失败
        // 时保持 0，继续使用程序化 FBM。
        target.normal = Eigen::Vector4f(
            0.0f,
            0.0f,
            1.0f,
            textureSet != nullptr && textureSet->hasNormalMap
                ? 1.0f
                : 0.0f);
    }

    commandBuffer.bindPipeline(
        pipeline->GetBindPoint(),
        pipeline->GetPipeline());
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        pipeline->GetPipelineLayout(),
        0,
        descriptorSets[imageIndex],
        nullptr);
    const auto& passSets = renderPass.GetDescriptorSets();
    if (imageIndex < passSets.size() &&
        PassSetIndex < passSets[imageIndex].size())
    {
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            pipeline->GetPipelineLayout(),
            PassSetIndex,
            passSets[imageIndex][PassSetIndex],
            nullptr);
    }
    commandBuffer.bindVertexBuffers(
        0,
        cubeVertices.buffer,
        vk::DeviceSize{0});
    commandBuffer.bindIndexBuffer(
        cubeIndices.buffer,
        0,
        vk::IndexType::eUint32);
    // 每个 decal 独立绑定 set 1 并只画 1 个实例：gl_InstanceIndex 就是
    // 预填充的 SSBO 槽位，避免把材质贴图打包进全局（不存在的）数组。
    // 未同步过的 decal 也必须有可绑定材质；默认纹理保证 shader 不会
    // 采样未定义图像。
    const DecalTextureSet fallbackMaterial{
        {},
        {},
        {},
        defaultAlbedoTexture,
        defaultNormalTexture,
        defaultOrmTexture,
        false};
    for (size_t index = 0; index < visibleDecals.size(); ++index)
    {
        const DecalSnapshot& source = *visibleDecals[index];
        const auto textureSetIt =
            textureSets.find(source.decalId);
        const DecalTextureSet* textureSet =
            textureSetIt != textureSets.end() &&
            textureSetIt->second != nullptr
                ? textureSetIt->second.get()
                : nullptr;
        const DecalTextureSet& material =
            textureSet != nullptr ? *textureSet : fallbackMaterial;
        if (!material.albedo || !material.normal || !material.orm)
            continue;
        WriteMaterialDescriptors(
            imageIndex,
            static_cast<uint32_t>(index),
            material);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            pipeline->GetPipelineLayout(),
            MaterialSetIndex,
            materialDescriptorSets[imageIndex][index],
            nullptr);
        commandBuffer.drawIndexed(
            kDecalCubeIndexCount,
            1,
            0,
            0,
            static_cast<uint32_t>(index));
    }
}

} // namespace VL
