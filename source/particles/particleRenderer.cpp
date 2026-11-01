#include "particles/particleRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "commonFunction.h"
#include "core/logStore.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "particles/particleCollision.h"
#include "particles/particleDump.h"
#include "particles/particleEmitterBounds.h"
#include "particles/particleEmitterKey.h"
#include "particles/particleEmitterCulling.h"
#include "particles/particleEmitterOrder.h"
#include "particles/particleLod.h"
#include "particles/particleMesh.h"
#include "particles/particleRibbon.h"
#include "particles/particleTextureReference.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/frontend/renderScene.h"
#include "render/rendergraph/renderGraphPassType.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"
#include "resource/image/textureIO.h"
#include "shader/build/atomicFile.h"
#include "shaderReflect.h"
#include "texture.h"

namespace VL
{
namespace
{

// maxParticles 是作者池大小；排序网络要求 2 的幂，最小 256 以摊薄 dispatch。
constexpr uint32_t kParticleSortMinimumCapacity = 256;
constexpr uint32_t kParticleSortMaximumCapacity = 4096;

// shape.type 字符串到 UBO code 的映射。非法值已在资产校验阶段拒绝，
// 这里回退 sphere 只用于防御未走校验的构造路径。
int ParticleShapeCode(const std::string& shapeType)
{
    if (shapeType == "box")
    {
        return 1;
    }
    if (shapeType == "cone")
    {
        return 2;
    }
    if (shapeType == "cylinder")
    {
        return 3;
    }
    if (shapeType == "disc")
    {
        return 4;
    }
    if (shapeType == "ring")
    {
        return 5;
    }
    return 0;
}

Renderpass* FindParticlePass(RenderGraph& graph)
{
    for (auto& [name, pass] : graph.GetRenderpasses())
    {
        (void)name;
        if (pass.type == RenderGraphPassType::Particle)
        {
            return &pass;
        }
    }
    return nullptr;
}

ParticleRenderIdentity BuildParticleRenderIdentity(
    RenderGraph& graph,
    const Renderpass& pass,
    const RendererBackendVulkan& backend)
{
    ParticleRenderIdentity identity;
    identity.ownerGeneration = graph.GetOwnerGeneration();
    identity.particleRenderPass = reinterpret_cast<uintptr_t>(
        static_cast<VkRenderPass>(pass.renderPass));
    identity.swapchainImageCount = backend.GetSwapchainImageCount();
    identity.passWidth = pass.width;
    identity.passHeight = pass.height;
    identity.pipelineContractKey =
        pass.pipelineContractKey.GetNormalizedKey();
    return identity;
}

uint32_t SortedCapacity(uint32_t maxParticles)
{
    uint32_t capacity = kParticleSortMinimumCapacity;
    const uint32_t desired =
        std::max(maxParticles, kParticleSortMinimumCapacity);
    while (capacity < desired &&
           capacity < kParticleSortMaximumCapacity)
    {
        capacity <<= 1;
    }
    return capacity;
}

std::string BuildEmitterKey(
    const ParticleSystemSnapshot& snapshot,
    const ParticleEmitterAsset& emitter)
{
    // 场景对象名 + 资产路径 + 子发射器名：同一 PS 被多个场景对象引用时
    // 各自拥有独立 GPU 池，资产内改名会自然换成新池而不是复用旧状态。
    return BuildParticleEmitterKey(
        snapshot.name, snapshot.assetPath, emitter.name);
}

Eigen::Matrix4f BuildEmitterModel(
    const ParticleSystemSnapshot& snapshot,
    const ParticleEmitterAsset& emitter)
{
    // localPosition 是场景对象局部空间偏移，必须先乘场景矩阵再进入 GPU，
    // 这样粒子池、速度方向和软粒子深度都在同一个世界空间里。
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation.block<3, 1>(0, 3) = emitter.localPosition;
    return snapshot.model * translation;
}

void ComputeEmitterBounds(
    const ParticleEmitterAsset& asset,
    const Eigen::Matrix4f& model,
    Eigen::Vector3f& worldCenter,
    float& boundsRadius,
    float meshLocalBoundsRadius = 0.0f)
{
    ComputeParticleEmitterBounds(
        asset,
        model,
        worldCenter,
        boundsRadius,
        meshLocalBoundsRadius);
}

std::string BuildEmitterTextureSourceKey(
    const ParticleEmitterAsset& asset)
{
    if (asset.texturePath.empty())
    {
        return {};
    }
    try
    {
        const bool generateMipmaps =
            asset.flipbookColumns * asset.flipbookRows <= 1;
        return BuildParticleTextureSourceKey(
            asset.texturePath,
            CommonFunction::Path(asset.texturePath),
            generateMipmaps);
    }
    catch (const std::exception&)
    {
        // 文件在同步与解码之间被删除时不能让渲染线程抛出；"missing" 键
        // 仍随路径变化，文件重新出现后会得到真实大小/时间并触发重建。
        return asset.texturePath + "|missing|" +
            (asset.flipbookColumns * asset.flipbookRows <= 1
                ? "mipmap" : "no-mipmap");
    }
}

std::string BuildEmitterMeshSourceKey(
    const ParticleEmitterAsset& asset)
{
    if (asset.renderMode != "mesh" || asset.meshPath.empty())
    {
        return {};
    }
    try
    {
        const std::filesystem::path resolved =
            CommonFunction::Path(asset.meshPath);
        std::error_code sizeError;
        const std::uintmax_t size =
            std::filesystem::file_size(resolved, sizeError);
        std::error_code timeError;
        const std::filesystem::file_time_type writeTime =
            std::filesystem::last_write_time(resolved, timeError);
        std::string key = asset.meshPath;
        key += '|';
        key += sizeError ? "missing" : std::to_string(size);
        key += '|';
        key += timeError
            ? "missing"
            : std::to_string(static_cast<long long>(
                writeTime.time_since_epoch().count()));
        return key;
    }
    catch (const std::exception&)
    {
        // 文件删除/路径解析失败也必须是稳定 key；下一次同步会触发重建。
        return asset.meshPath + "|missing";
    }
}

} // namespace

ParticleRenderer::~ParticleRenderer()
{
    // backend 生命周期由 RenderSystem 保证；正常关闭走 Shutdown，
    // 这里只兜底释放 Vulkan 包装对象，避免析构顺序变化时泄漏。
    if (backend != nullptr)
    {
        emitters.clear();
        textureCache.clear();
    }
}

void ParticleRenderer::EnsureInitialized(
    PipelineFactory& factory,
    RendererBackendVulkan& renderer,
    RenderGraph& graph)
{
    Renderpass* particlePass = FindParticlePass(graph);
    ParticleRenderIdentity nextIdentity;
    if (particlePass != nullptr)
    {
        nextIdentity = BuildParticleRenderIdentity(
            graph,
            *particlePass,
            renderer);
    }
    if (initialized && backend == &renderer &&
        renderIdentity == nextIdentity)
    {
        return;
    }

    if (initialized && backend != nullptr)
    {
        // 图身份变化只发生在 resize、图重载或启动阶段，不在普通帧循环里。
        // particle pipeline/renderPass 可能仍被已提交帧引用，因此这里等待
        // 设备空闲后立即释放；发射器增删走 retire queue，不会每帧等待。
        backend->WaitIdle();
        Shutdown(*backend);
    }
    Initialize(factory, renderer, graph);
}

void ParticleRenderer::Initialize(
    PipelineFactory& factory,
    RendererBackendVulkan& renderer,
    RenderGraph& graph)
{
    backend = &renderer;

    Renderpass* particlePass = FindParticlePass(graph);
    if (particlePass == nullptr)
    {
        // 图没有声明 particle pass 时保持未初始化；渲染路径只跳过粒子，
        // 不把缺少可选 pass 当成启动失败。
        renderIdentity = {};
        initialized = false;
        return;
    }

    renderIdentity = BuildParticleRenderIdentity(
        graph,
        *particlePass,
        renderer);
    simulatePipeline =
        factory.CreateComputePipeline("particle/particleSimulate");
    sortInitPipeline =
        factory.CreateComputePipeline("particle/particleSortInit");
    sortPipeline =
        factory.CreateComputePipeline("particle/particleSort");

    ShaderVariantKey variant;
    variant.shaderName = "particle/particle";
    variant.renderMode = RenderMode::TransparentAdditive;
    variant.shadingModelMacro = "SHADING_MODEL_UNLIT";
    const GraphicsShaderVariantArtifact& artifact =
        factory.PrepareGraphicsShaderVariant(variant);

    PassPipelineContractKey contract =
        particlePass->pipelineContractKey;
    contract.useVertexInput = false;
    // 深度测试改在片元里采样 sceneDepth；pass 不再写深度附件。
    contract.depthTestEnable = false;
    contract.depthWriteEnable = false;
    contract.depthCompareOp = vk::CompareOp::eLess;
    additivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        artifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive);
    alphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        artifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend);

    // ribbon 复用同一 pass、descriptor layout 与 particle SSBO，只替换
    // 顶点/片元 shader；顶点数按段数在 RecordGraphics 中决定。
    ShaderVariantKey ribbonVariant = variant;
    ribbonVariant.shaderName = "particle/particleRibbon";
    const GraphicsShaderVariantArtifact& ribbonArtifact =
        factory.PrepareGraphicsShaderVariant(ribbonVariant);
    ribbonAdditivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        ribbonArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive);
    ribbonAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        ribbonArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend);

    // mesh 粒子读取静态 SM 的 Vertex stream，因此同一条 particle pass
    // 需要使用带 vertex input 的合同；descriptor set 仍与 billboard 相同。
    ShaderVariantKey meshVariant = variant;
    meshVariant.shaderName = "particle/particleMesh";
    const GraphicsShaderVariantArtifact& meshArtifact =
        factory.PrepareGraphicsShaderVariant(meshVariant);
    PassPipelineContractKey meshContract =
        particlePass->pipelineContractKey;
    meshContract.useVertexInput = true;
    meshContract.depthTestEnable = false;
    meshContract.depthWriteEnable = false;
    meshContract.depthCompareOp = vk::CompareOp::eLess;
    meshAdditivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        meshContract,
        meshArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive);
    meshAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        meshContract,
        meshArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend);

    // 无纹理发射器仍需一个合法 sampler；1x1 白图让 shader 走程序化形状分支。
    HostImage whiteImage;
    whiteImage.width = 1;
    whiteImage.height = 1;
    whiteImage.channels = 4;
    whiteImage.format = HostImage::PixelFormat::RGBA8_UNORM;
    whiteImage.semantic = HostImage::TextureSemantic::Color;
    whiteImage.rowStrideBytes = 4;
    whiteImage.data = {255, 255, 255, 255};
    Texture::CreateDesc whiteDesc;
    whiteDesc.semantic = HostImage::TextureSemantic::Color;
    whiteDesc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    whiteDesc.generateMipmaps = false;
    whiteDesc.filter = vk::Filter::eLinear;
    whiteDesc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    whiteDesc.debugName = "ParticleDefaultWhite";
    defaultTexture = std::make_shared<Texture>(
        renderer,
        whiteImage,
        whiteDesc);
    initialized = true;
}

void ParticleRenderer::Shutdown(RendererBackendVulkan& renderer)
{
    frameStats = ParticleFrameStats{};
    if (backend == nullptr)
    {
        return;
    }
    // 调用方必须已等待设备空闲（renderer.WaitIdle 或引擎关闭路径）；
    // 活跃发射器可以立即析构，已退休的包仍由 ResourceRetireQueue 持有。
    emitters.clear();
    textureCache.clear();
    simulatePipeline.reset();
    sortInitPipeline.reset();
    sortPipeline.reset();
    additivePipeline.reset();
    alphaPipeline.reset();
    ribbonAdditivePipeline.reset();
    ribbonAlphaPipeline.reset();
    meshAdditivePipeline.reset();
    meshAlphaPipeline.reset();
    defaultTexture.reset();
    drawOrder.clear();
    renderIdentity = {};
    initialized = false;
    backend = nullptr;
    (void)renderer;
}

void ParticleRenderer::InvalidatePipelines(
    RendererBackendVulkan& renderer)
{
    frameStats = ParticleFrameStats{};
    if (backend == nullptr || backend != &renderer)
    {
        return;
    }
    // reload 提交后没有尚未完成的粒子工作；等待设备空闲再走 Shutdown，
    // 避免释放仍被已提交帧引用的 pipeline/renderPass。延迟退休队列里的
    // 旧发射器包仍由队列按 epoch 持有，不会在这里提前析构。
    backend->WaitIdle();
    Shutdown(renderer);
}

void ParticleRenderer::Synchronize(const RenderScene& renderScene)
{
    if (!initialized)
    {
        return;
    }

    std::unordered_set<std::string> desired;
    size_t emitterCount = 0;
    for (const ParticleSystemSnapshot& snapshot :
         renderScene.particleSystems)
        emitterCount += snapshot.asset.emitters.size();
    desired.reserve(emitterCount);
    for (const ParticleSystemSnapshot& snapshot :
         renderScene.particleSystems)
    {
        for (size_t emitterIndex = 0;
             emitterIndex < snapshot.asset.emitters.size();
             ++emitterIndex)
        {
            const ParticleEmitterAsset& emitter =
                snapshot.asset.emitters[emitterIndex];
            const bool enabled =
                emitterIndex < kMaxParticleEmitters &&
                ((snapshot.runtime.enabledEmitterMask >> emitterIndex) & 1u) != 0;
            const std::string key =
                BuildEmitterKey(snapshot, emitter);
            desired.insert(key);
            const Eigen::Matrix4f model =
                BuildEmitterModel(snapshot, emitter);
            const std::string textureSourceKey =
                BuildEmitterTextureSourceKey(emitter);
            const std::string meshSourceKey =
                BuildEmitterMeshSourceKey(emitter);
            // 可见性/启用状态与实例覆盖写入所有分支：新建池直接继承，
            // 已有池每帧跟随快照；开关切换不走资源重建。
            auto emitterIt = emitters.find(key);
            if (emitterIt == emitters.end() ||
                emitterIt->second->asset.maxParticles !=
                    emitter.maxParticles ||
                emitterIt->second->asset.sortParticles !=
                    emitter.sortParticles ||
                emitterIt->second->textureSourceKey !=
                    textureSourceKey ||
                emitterIt->second->meshSourceKey !=
                    meshSourceKey)
            {
                if (emitterIt != emitters.end())
                {
                    RetireEmitterResources(
                        std::move(emitterIt->second));
                    emitters.erase(emitterIt);
                }
                CreateEmitter(
                    emitter, model, key, snapshot.visible, enabled,
                    snapshot.runtime.spawnRateScale,
                    snapshot.runtime.colorTint);
                continue;
            }

            // 资产参数和摆放矩阵每帧都可变化；GPU 缓冲只随 maxParticles 重建。
            emitterIt->second->asset = emitter;
            emitterIt->second->model = model;
            emitterIt->second->visible = snapshot.visible;
            emitterIt->second->enabled = enabled;
            emitterIt->second->spawnRateScale =
                snapshot.runtime.spawnRateScale;
            emitterIt->second->colorTint = snapshot.runtime.colorTint;
            emitterIt->second->meshScale = emitter.meshScale;
            emitterIt->second->meshAlignment =
                emitter.meshAlignment;
            // 逐帧时长/轴心是 pack 进 UBO 的派生表，必须在原地更新，
            // 否则编辑器改动或热重载后会继续用创建时的旧表。
            emitterIt->second->flipbookTable =
                BuildParticleFlipbookGpuTable(emitter);
            ComputeEmitterBounds(
                emitter,
                model,
                emitterIt->second->worldCenter,
                emitterIt->second->boundsRadius,
                emitterIt->second->meshLocalBoundsRadius);
        }
    }

    for (auto emitterIt = emitters.begin();
         emitterIt != emitters.end();)
    {
        if (desired.find(emitterIt->first) == desired.end())
        {
            RetireEmitterResources(
                std::move(emitterIt->second));
            emitterIt = emitters.erase(emitterIt);
        }
        else
        {
            ++emitterIt;
        }
    }
}

void ParticleRenderer::RestartParticleSystems(
    const std::string& sceneObjectName)
{
    for (auto& entry : emitters)
    {
        if (!ParticleEmitterKeyMatchesScene(
                entry.first,
                sceneObjectName))
        {
            continue;
        }
        EmitterResources& emitter = *entry.second;
        // elapsedSeconds 决定 burst 周期与 flipbook 系统时间；归零后从
        // 第一帧重新开始。prewarm/needsClear 让下一帧重建整池初始状态。
        emitter.elapsedSeconds = 0.0f;
        emitter.prewarm = true;
        emitter.needsClear = true;
        emitter.culled = false;
    }
}

void ParticleRenderer::CreateEmitter(
    const ParticleEmitterAsset& asset,
    const Eigen::Matrix4f& model,
    const std::string& key,
    bool visible,
    bool enabled,
    float spawnRateScale,
    const Eigen::Vector4f& colorTint)
{
    std::shared_ptr<EmitterResources> emitter(
        new EmitterResources(),
        [rendererBackend = backend](EmitterResources* resources)
        {
            DestroyEmitterResources(*rendererBackend, *resources);
            delete resources;
        });
    emitter->key = key;
    emitter->asset = asset;
    emitter->model = model;
    emitter->visible = visible;
    emitter->enabled = enabled;
    emitter->spawnRateScale = spawnRateScale;
    emitter->colorTint = colorTint;
    emitter->textureSourceKey =
        BuildEmitterTextureSourceKey(asset);
    emitter->meshSourceKey =
        BuildEmitterMeshSourceKey(asset);
    emitter->meshScale = asset.meshScale;
    emitter->meshAlignment = asset.meshAlignment;
    emitter->flipbookTable = BuildParticleFlipbookGpuTable(asset);
    emitter->elapsedSeconds = 0.0f;
    emitter->needsClear = true;
    emitter->prewarm = true;
    if (asset.texturePath.empty())
    {
        emitter->texture = defaultTexture;
    }
    else
    {
        const bool generateMipmaps =
            asset.flipbookColumns * asset.flipbookRows <= 1;
        auto cached = textureCache.find(emitter->textureSourceKey);
        if (cached != textureCache.end())
        {
            emitter->texture = cached->second.lock();
            if (!emitter->texture)
                textureCache.erase(cached);
        }
        if (!emitter->texture)
        {
            try
            {
                TextureIO::LoadOptions options;
                options.semantic = HostImage::TextureSemantic::Color;
                options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
                options.transfer = TextureIO::LoadOptions::Transfer::SRGB;
                options.forceChannels = 4;
                const std::optional<HostImage> cpuImage =
                    TextureIO::Load(
                        CommonFunction::Path(asset.texturePath),
                        options);
                if (!cpuImage.has_value())
                {
                    throw std::runtime_error(
                        "texel decoder returned no image");
                }
                Texture::CreateDesc desc;
                desc.semantic = HostImage::TextureSemantic::Color;
                desc.transfer = TextureIO::LoadOptions::Transfer::SRGB;
                // 多帧图集不能生成 mipmap：低 mip 会把相邻 cell 混在一起，
                // 线过滤再叠加半 texel 内缩即可避免帧间渗色。单帧纹理保留原行为。
                desc.generateMipmaps = generateMipmaps;
                desc.filter = vk::Filter::eLinear;
                desc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
                desc.debugName = "ParticleTexture:" + asset.texturePath;
                emitter->texture = std::make_shared<Texture>(
                    *backend,
                    *cpuImage,
                    desc);
                // 同路径+文件版本+mipmap 配置的活跃纹理可被多个发射器共享。
                textureCache[emitter->textureSourceKey] =
                    emitter->texture;
            }
            catch (const std::exception& exception)
            {
                WarnTextureFallbackOnce(
                    key,
                    asset.texturePath,
                    exception.what());
                emitter->texture = defaultTexture;
            }
            catch (...)
            {
                WarnTextureFallbackOnce(
                    key,
                    asset.texturePath,
                    "unknown texture load failure");
                emitter->texture = defaultTexture;
            }
        }
    }
    LoadEmitterMesh(*emitter, key);
    ComputeEmitterBounds(
        asset,
        model,
        emitter->worldCenter,
        emitter->boundsRadius,
        emitter->meshLocalBoundsRadius);
    CreateBuffers(*emitter);
    CreateDescriptors(*emitter);
    emitters.emplace(key, std::move(emitter));
}

void ParticleRenderer::LoadEmitterMesh(
    EmitterResources& emitter,
    const std::string& emitterKey)
{
    if (emitter.asset.renderMode != "mesh")
    {
        return;
    }
    try
    {
        // v1 只接受静态网格：骨骼动画需要 skinning vertex factory，
        // 与粒子实例化不兼容。
        MeshAssetLoadRequest request =
            MeshAssetLoader::Prepare(emitter.asset.meshPath);
        if (request.buildPlan.IsSkeletal())
        {
            throw std::runtime_error(
                "skeletal meshes are not supported");
        }
        const MeshAssetImportResult imported =
            MeshAssetLoader::ImportSections(request);

        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const MeshSection& section : imported.modelResource.sections)
        {
            if (section.vertices.empty() || section.indices.empty())
            {
                continue;
            }
            if (vertices.size() + section.vertices.size() >
                static_cast<size_t>(
                    std::numeric_limits<uint32_t>::max()))
            {
                throw std::runtime_error(
                    "mesh has too many vertices");
            }
            const uint32_t baseVertex =
                static_cast<uint32_t>(vertices.size());
            vertices.insert(
                vertices.end(),
                section.vertices.begin(),
                section.vertices.end());
            indices.reserve(indices.size() + section.indices.size());
            for (const uint32_t index : section.indices)
            {
                if (index >= section.vertices.size())
                {
                    throw std::runtime_error(
                        "mesh section index is out of range");
                }
                indices.push_back(baseVertex + index);
            }
        }
        if (vertices.empty() || indices.empty())
        {
            throw std::runtime_error(
                "mesh has no non-empty sections");
        }

        const vk::DeviceSize vertexBytes =
            static_cast<vk::DeviceSize>(
                vertices.size() * sizeof(Vertex));
        const auto vertexAllocation = backend->CreateBuffer(
            vertexBytes,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleMeshVertices:" + emitterKey);
        emitter.meshVertices.buffer = vertexAllocation.first;
        emitter.meshVertices.memory = vertexAllocation.second;
        void* mappedVertices = backend->MapMemory(
            emitter.meshVertices.memory,
            vertexBytes);
        if (mappedVertices == nullptr)
        {
            throw std::runtime_error("mesh vertex map failed");
        }
        std::memcpy(
            mappedVertices,
            vertices.data(),
            static_cast<size_t>(vertexBytes));
        backend->UnmapMemory(emitter.meshVertices.memory);

        const vk::DeviceSize indexBytes =
            static_cast<vk::DeviceSize>(
                indices.size() * sizeof(uint32_t));
        const auto indexAllocation = backend->CreateBuffer(
            indexBytes,
            vk::BufferUsageFlagBits::eIndexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleMeshIndices:" + emitterKey);
        emitter.meshIndices.buffer = indexAllocation.first;
        emitter.meshIndices.memory = indexAllocation.second;
        void* mappedIndices = backend->MapMemory(
            emitter.meshIndices.memory,
            indexBytes);
        if (mappedIndices == nullptr)
        {
            throw std::runtime_error("mesh index map failed");
        }
        std::memcpy(
            mappedIndices,
            indices.data(),
            static_cast<size_t>(indexBytes));
        backend->UnmapMemory(emitter.meshIndices.memory);

        float localRadius = 0.0f;
        for (const Vertex& vertex : vertices)
        {
            const Eigen::Vector3f scaled(
                vertex.position.x() * emitter.asset.meshScale.x(),
                vertex.position.y() * emitter.asset.meshScale.y(),
                vertex.position.z() * emitter.asset.meshScale.z());
            localRadius = std::max(localRadius, scaled.norm());
        }
        emitter.meshIndexCount =
            static_cast<uint32_t>(indices.size());
        emitter.meshLocalBoundsRadius = localRadius;
        emitter.meshFallbackBillboard = false;
    }
    catch (const std::exception& exception)
    {
        DestroyBuffer(*backend, emitter.meshVertices);
        DestroyBuffer(*backend, emitter.meshIndices);
        emitter.meshIndexCount = 0;
        emitter.meshLocalBoundsRadius = 0.0f;
        emitter.meshFallbackBillboard = true;
        WarnMeshFallbackOnce(
            emitterKey,
            emitter.asset.meshPath,
            exception.what());
    }
    catch (...)
    {
        DestroyBuffer(*backend, emitter.meshVertices);
        DestroyBuffer(*backend, emitter.meshIndices);
        emitter.meshIndexCount = 0;
        emitter.meshLocalBoundsRadius = 0.0f;
        emitter.meshFallbackBillboard = true;
        WarnMeshFallbackOnce(
            emitterKey,
            emitter.asset.meshPath,
            "unknown mesh load failure");
    }
}

void ParticleRenderer::CreateBuffers(EmitterResources& emitter)
{
    emitter.sortCapacity = SortedCapacity(emitter.asset.maxParticles);
    const uint32_t sortPassCount =
        ParticleSortDispatchCount(emitter.sortCapacity);
    emitter.sortFinalUsesA = (sortPassCount % 2u) == 0u;

    const vk::DeviceSize particleBytes =
        sizeof(GpuParticle) *
        static_cast<vk::DeviceSize>(emitter.asset.maxParticles);
    const auto particleAllocation = backend->CreateBuffer(
        particleBytes,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst |
            vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticlePool:" + emitter.key);
    emitter.particles.buffer = particleAllocation.first;
    emitter.particles.memory = particleAllocation.second;

    // pair 表始终存在：graphics shader 无条件声明 binding 3，排序关闭时
    // 绑定 A 但不读取，排序开启时按 pass 奇偶决定最终绑定 A 还是 B。
    const vk::DeviceSize pairBytes =
        sizeof(Eigen::Vector4f) *
        static_cast<vk::DeviceSize>(emitter.sortCapacity);
    const auto pairAAllocation = backend->CreateBuffer(
        pairBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleSortPairsA:" + emitter.key);
    emitter.sortPairsA.buffer = pairAAllocation.first;
    emitter.sortPairsA.memory = pairAAllocation.second;
    const auto pairBAllocation = backend->CreateBuffer(
        pairBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleSortPairsB:" + emitter.key);
    emitter.sortPairsB.buffer = pairBAllocation.first;
    emitter.sortPairsB.memory = pairBAllocation.second;

    const uint32_t imageCount =
        backend->GetSwapchainImageCount();
    emitter.frameUniforms.reserve(imageCount);
    for (uint32_t imageIndex = 0;
         imageIndex < imageCount; ++imageIndex)
    {
        const auto uniformAllocation = backend->CreateBuffer(
            sizeof(FrameUniforms),
            // sortPass 每个 bitonic pass 都用 vkCmdUpdateBuffer 顺序写入；
            // 普通 memcpy 会在提交时只留下最后一次值。
            vk::BufferUsageFlagBits::eUniformBuffer |
                vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleUniforms:" + emitter.key + ":" +
                std::to_string(imageIndex));
        GpuBuffer uniformBuffer;
        uniformBuffer.buffer = uniformAllocation.first;
        uniformBuffer.memory = uniformAllocation.second;
        uniformBuffer.mapped = backend->MapMemory(
            uniformBuffer.memory,
            sizeof(FrameUniforms));
        emitter.frameUniforms.push_back(uniformBuffer);
    }
}

void ParticleRenderer::CreateDescriptors(
    EmitterResources& emitter)
{
    const uint32_t imageCount =
        backend->GetSwapchainImageCount();
    const std::vector<vk::DescriptorSetLayout> computeLayouts(
        imageCount,
        simulatePipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> graphicsLayouts(
        imageCount,
        additivePipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> sortLayouts(
        imageCount,
        sortInitPipeline->GetDescriptorSetLayouts().at(0));

    // 每个 image：simulate、graphics、sortInit、AtoB、BtoA 共 5 个 set。
    // storage 计数：simulate 1 + graphics 2 + sort 三组各 2 = 9。
    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer, imageCount * 5},
        {vk::DescriptorType::eStorageBuffer, imageCount * 9},
        {vk::DescriptorType::eCombinedImageSampler, imageCount}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount * 5);
    emitter.descriptorPool = backend->CreateDescriptorPool(
        poolInfo,
        "ParticleDescriptors:" + emitter.key);

    try
    {
        vk::DescriptorSetAllocateInfo allocate;
        allocate
            .setDescriptorPool(emitter.descriptorPool)
            .setSetLayouts(computeLayouts);
        emitter.computeSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.computeSets);

        allocate.setSetLayouts(graphicsLayouts);
        emitter.graphicsSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.graphicsSets);

        allocate.setSetLayouts(sortLayouts);
        emitter.sortInitSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.sortInitSets);
        emitter.sortAtoBSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.sortAtoBSets);
        emitter.sortBtoASets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.sortBtoASets);

        for (uint32_t imageIndex = 0;
             imageIndex < imageCount; ++imageIndex)
        {
            const vk::DescriptorBufferInfo uniformInfo(
                emitter.frameUniforms.at(imageIndex).buffer,
                0,
                sizeof(FrameUniforms));
            const vk::DescriptorBufferInfo particleInfo(
                emitter.particles.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo pairAInfo(
                emitter.sortPairsA.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo pairBInfo(
                emitter.sortPairsB.buffer,
                0,
                VK_WHOLE_SIZE);
            // 排序关闭时 graphics 仍声明 binding 3，绑定 A 但不读取；
            // 排序开启时按 bitonic pass 奇偶选择最终 pair 表。
            const vk::DescriptorBufferInfo& finalPairInfo =
                (!emitter.asset.sortParticles ||
                 emitter.sortFinalUsesA)
                    ? pairAInfo
                    : pairBInfo;

            vk::WriteDescriptorSet uniformWrite;
            uniformWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            vk::WriteDescriptorSet particleWrite;
            particleWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(particleInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite, particleWrite});

            vk::WriteDescriptorSet sortedWrite;
            sortedWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(3)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(finalPairInfo);
            uniformWrite.setDstSet(
                emitter.graphicsSets.at(imageIndex));
            particleWrite.setDstSet(
                emitter.graphicsSets.at(imageIndex));
            vk::WriteDescriptorSet textureWrite;
            const vk::DescriptorImageInfo textureInfo =
                emitter.texture->GetDescriptorInfo();
            textureWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(textureInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 particleWrite,
                 textureWrite,
                 sortedWrite});

            uniformWrite.setDstSet(
                emitter.sortInitSets.at(imageIndex));
            particleWrite
                .setDstSet(emitter.sortInitSets.at(imageIndex))
                .setDstBinding(1);
            vk::WriteDescriptorSet pairAWrite;
            pairAWrite
                .setDstSet(emitter.sortInitSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairAInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite, particleWrite, pairAWrite});

            uniformWrite.setDstSet(
                emitter.sortAtoBSets.at(imageIndex));
            vk::WriteDescriptorSet pairAInputWrite;
            pairAInputWrite
                .setDstSet(emitter.sortAtoBSets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairAInfo);
            vk::WriteDescriptorSet pairBOutputWrite;
            pairBOutputWrite
                .setDstSet(emitter.sortAtoBSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairBInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 pairAInputWrite,
                 pairBOutputWrite});

            uniformWrite.setDstSet(
                emitter.sortBtoASets.at(imageIndex));
            vk::WriteDescriptorSet pairBInputWrite;
            pairBInputWrite
                .setDstSet(emitter.sortBtoASets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairBInfo);
            vk::WriteDescriptorSet pairAOutputWrite;
            pairAOutputWrite
                .setDstSet(emitter.sortBtoASets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairAInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 pairBInputWrite,
                 pairAOutputWrite});
        }
    }
    catch (...)
    {
        if (emitter.descriptorPool)
        {
            backend->DestroyDescriptorPool(
                emitter.descriptorPool);
            emitter.descriptorPool = nullptr;
        }
        throw;
    }
}

void ParticleRenderer::DestroyEmitterResources(
    RendererBackendVulkan& rendererBackend,
    EmitterResources& emitter)
{
    if (emitter.descriptorPool)
    {
        rendererBackend.DestroyDescriptorPool(emitter.descriptorPool);
        emitter.descriptorPool = nullptr;
    }
    emitter.computeSets.clear();
    emitter.graphicsSets.clear();
    emitter.sortInitSets.clear();
    emitter.sortAtoBSets.clear();
    emitter.sortBtoASets.clear();
    emitter.texture.reset();
    DestroyBuffer(rendererBackend, emitter.meshVertices);
    DestroyBuffer(rendererBackend, emitter.meshIndices);
    DestroyBuffer(rendererBackend, emitter.particles);
    DestroyBuffer(rendererBackend, emitter.sortPairsA);
    DestroyBuffer(rendererBackend, emitter.sortPairsB);
    for (GpuBuffer& buffer : emitter.frameUniforms)
    {
        DestroyBuffer(rendererBackend, buffer);
    }
    emitter.frameUniforms.clear();
}

void ParticleRenderer::DestroyBuffer(
    RendererBackendVulkan& rendererBackend,
    GpuBuffer& buffer)
{
    if (buffer.mapped != nullptr)
    {
        rendererBackend.UnmapMemory(buffer.memory);
        buffer.mapped = nullptr;
    }
    if (buffer.buffer)
    {
        rendererBackend.DestroyBuffer(buffer.buffer, buffer.memory);
    }
    buffer = {};
}

void ParticleRenderer::RetireEmitterResources(
    std::shared_ptr<EmitterResources> emitter)
{
    if (!emitter)
    {
        return;
    }
    // 先固定 debugName 再 move 载荷：C++ 未规定实参求值顺序，若编译器先
    // 构造最后一个 shared_ptr 参数，再读 emitter->key 就会解引用空指针。
    const std::string debugName =
        "ParticleEmitter:" + emitter->key;
    ResourceRetireQueue& retireQueue =
        ResourceRetireQueue::GetInstance();
    const uint64_t lastUsedEpoch =
        retireQueue.GetLastSubmittedEpoch();
    // 资源可能在最近若干帧都被 compute/graphics 引用。记录最后一次已提交
    // epoch，等对应 fence 完成后由队列析构整个 EmitterResources 包。
    retireQueue.RetireShared(
        debugName,
        renderIdentity.ownerGeneration,
        lastUsedEpoch,
        std::move(emitter));
}

void ParticleRenderer::WarnTextureFallbackOnce(
    const std::string& key,
    const std::string& texturePath,
    const std::string& error)
{
    if (!warnedTextureFallbacks.insert(key + "|" + texturePath).second)
    {
        return;
    }
    const std::string message =
        "Particle texture fallback for emitter '" + key +
        "' (" + texturePath + "): " + error +
        "; using the default white texture";
    ProcessLog().Append(
        LogSeverity::Warning,
        "Particles",
        message);
    std::cout << "[Particles][Warning] " << message << std::endl;
}

void ParticleRenderer::WarnMeshFallbackOnce(
    const std::string& key,
    const std::string& meshPath,
    const std::string& error)
{
    if (!warnedMeshFallbacks.insert(key + "|" + meshPath).second)
    {
        return;
    }
    const std::string message =
        "Particle mesh fallback for emitter '" + key +
        "' (" + meshPath + "): " + error +
        "; using billboard rendering";
    ProcessLog().Append(
        LogSeverity::Warning,
        "Particles",
        message);
    std::cout << "[Particles][Warning] " << message << std::endl;
}

void ParticleRenderer::InsertBufferBarrier(
    vk::CommandBuffer commandBuffer,
    vk::PipelineStageFlags sourceStage,
    vk::AccessFlags sourceAccess,
    vk::PipelineStageFlags destinationStage,
    vk::AccessFlags destinationAccess)
{
    vk::MemoryBarrier barrier;
    barrier
        .setSrcAccessMask(sourceAccess)
        .setDstAccessMask(destinationAccess);
    commandBuffer.pipelineBarrier(
        sourceStage,
        destinationStage,
        {},
        barrier,
        nullptr,
        nullptr);
}

void ParticleRenderer::RecordSimulation(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const RenderScene& renderScene,
    vk::Extent2D renderExtent)
{
    // frameIndex 与 elapsed 都保留跨帧累计值；elapsed 只累加非负的
    // 模拟 dt。统计重置发生在任何早退之前，便于诊断空场景。
    const uint64_t previousFrameIndex = frameStats.frameIndex;
    const double previousElapsedSeconds = frameStats.elapsedSeconds;
    frameStats = ParticleFrameStats{};
    frameStats.frameIndex = previousFrameIndex + 1;
    const float dt = std::clamp(
        renderScene.simulationDeltaSeconds, 0.0f, 0.1f);
    if (std::isfinite(dt) && dt > 0.0f)
    {
        frameStats.elapsedSeconds =
            previousElapsedSeconds + static_cast<double>(dt);
    }
    else
    {
        frameStats.elapsedSeconds = previousElapsedSeconds;
    }

    if (!initialized || emitters.empty())
    {
        return;
    }

    // 上一帧顶点着色器读取过粒子池，compute 也读取粒子池与 pair 表；跨提交的
    // semaphore 不表达 storage buffer 的 memory dependency，因此用一条全局
    // MemoryBarrier 把 vertex read -> compute read/write 一起隔开，覆盖两个 SSBO。
    vk::MemoryBarrier readToWrite;
    readToWrite
        .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eVertexShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        readToWrite,
        nullptr,
        nullptr);

    // 粒子时间来自 WorldSnapshot，由脚本子系统按 Pause/timeScale 计算；
    // 不再使用眼适应的 wall-clock delta，否则编辑器暂停时粒子仍会推进。
    const std::array<Eigen::Vector4f, 6> frustumPlanes =
        BuildParticleFrustumPlanes(
            renderScene.camera.viewProjection);
    const float projectionScaleY =
        std::abs(renderScene.camera.projection(1, 1));
    const float viewportHeightPixels =
        static_cast<float>(renderExtent.height);
    for (auto& [key, emitterPtr] : emitters)
    {
        (void)key;
        EmitterResources& emitter = *emitterPtr;
        ++frameStats.totalEmitters;
        frameStats.totalParticles += emitter.asset.maxParticles;
        if (emitter.frameUniforms.size() <= imageIndex)
        {
            ++frameStats.culledEmitters;
            continue;
        }
        // 作者隐藏、发射器禁用与视锥剔除共用"冻结"路径：不推进 elapsed/prewarm，
        // 不清池、不排序、不 dispatch，也不进入 RecordGraphics 的绘制列表。
        if (!emitter.visible || !emitter.enabled)
        {
            emitter.culled = true;
            ++frameStats.culledEmitters;
            continue;
        }
        const ParticleEmitterCullingResult culling =
            CullParticleEmitter(
                ParticleEmitterCullingInput{
                    emitter.worldCenter,
                    emitter.boundsRadius,
                    emitter.asset.maxDrawDistance,
                    emitter.asset.fadeDistance,
                    emitter.asset.minScreenSize},
                frustumPlanes,
                renderScene.camera.position,
                renderScene.camera.forward,
                projectionScaleY,
                viewportHeightPixels);
        emitter.culled = !culling.visible;
        if (emitter.culled)
        {
            // 冻结整个发射器状态：不推进时间、不写 UBO、不清池、不 dispatch。
            ++frameStats.culledEmitters;
            continue;
        }
        ++frameStats.visibleEmitters;
        emitter.culled = false;
        emitter.elapsedSeconds += dt;

        FrameUniforms uniforms{};
        uniforms.viewProjection =
            renderScene.camera.viewProjection;
        uniforms.view = renderScene.camera.view;
        uniforms.invProjection =
            renderScene.camera.projection.inverse();
        uniforms.cameraPosition = Eigen::Vector4f(
            renderScene.camera.position.x(),
            renderScene.camera.position.y(),
            renderScene.camera.position.z(),
            1.0f);
        uniforms.cameraRight = Eigen::Vector4f(
            renderScene.camera.right.x(),
            renderScene.camera.right.y(),
            renderScene.camera.right.z(),
            0.0f);
        uniforms.cameraUp = Eigen::Vector4f(
            renderScene.camera.up.x(),
            renderScene.camera.up.y(),
            renderScene.camera.up.z(),
            0.0f);
        uniforms.localToWorld = emitter.model;
        uniforms.gravityDrag = Eigen::Vector4f(
            emitter.asset.gravity.x(),
            emitter.asset.gravity.y(),
            emitter.asset.gravity.z(),
            emitter.asset.drag);
        uniforms.velocityMinLifetimeMin = Eigen::Vector4f(
            emitter.asset.velocityMin.x(),
            emitter.asset.velocityMin.y(),
            emitter.asset.velocityMin.z(),
            emitter.asset.lifetimeMin);
        uniforms.velocityMaxLifetimeMax = Eigen::Vector4f(
            emitter.asset.velocityMax.x(),
            emitter.asset.velocityMax.y(),
            emitter.asset.velocityMax.z(),
            emitter.asset.lifetimeMax);
        // LOD 只降连续发射预算：按发射器中心到相机的距离缩放 spawnRate。
        // burst/prewarm 的整池填充在 shader 里不读这个预算，因此不受影响。
        const float lodDistance =
            (emitter.worldCenter - renderScene.camera.position).norm();
        uniforms.simulation = Eigen::Vector4f(
            EffectiveParticleSpawnRate(
                emitter.asset, lodDistance) * emitter.spawnRateScale,
            dt,
            emitter.elapsedSeconds,
            emitter.prewarm ? 1.0f : 0.0f);
        uniforms.render = Eigen::Vector4f(
            emitter.asset.sizeStart,
            emitter.asset.sizeEnd,
            emitter.asset.softness,
            emitter.asset.opacity * culling.opacityScale);
        uniforms.shape = Eigen::Vector4f(
            emitter.asset.shapeRadius,
            emitter.asset.burst ? 1.0f : 0.0f,
            emitter.asset.localSpace ? 1.0f : 0.0f,
            0.0f);
        uniforms.softParams = Eigen::Vector4f(
            emitter.asset.softParticleDistance,
            emitter.asset.softParticles ? 1.0f : 0.0f,
            0.0f,
            0.0f);
        uniforms.colorStart = emitter.asset.colorStart;
        uniforms.colorEnd = emitter.asset.colorEnd;
        const bool hasColorKeys =
            !emitter.asset.colorKeys.empty();
        const size_t colorKeyCount = hasColorKeys
            ? std::min<size_t>(
                emitter.asset.colorKeys.size(),
                kMaxParticleCurveKeys)
            : 2;
        for (size_t index = 0; index < colorKeyCount; ++index)
        {
            if (hasColorKeys)
            {
                uniforms.colorKeys[index] =
                    emitter.asset.colorKeys[index].color;
                uniforms.colorKeyTimes[index / 4][index % 4] =
                    emitter.asset.colorKeys[index].time;
            }
            else
            {
                uniforms.colorKeys[index] = index == 0
                    ? emitter.asset.colorStart
                    : emitter.asset.colorEnd;
                uniforms.colorKeyTimes[index / 4][index % 4] =
                    index == 0 ? 0.0f : 1.0f;
            }
        }

        const bool hasSizeKeys =
            !emitter.asset.sizeKeys.empty();
        const size_t sizeKeyCount = hasSizeKeys
            ? std::min<size_t>(
                emitter.asset.sizeKeys.size(),
                kMaxParticleCurveKeys)
            : 2;
        for (size_t index = 0; index < sizeKeyCount; ++index)
        {
            if (hasSizeKeys)
            {
                uniforms.sizeKeys[index] = Eigen::Vector4f(
                    emitter.asset.sizeKeys[index].time,
                    emitter.asset.sizeKeys[index].size,
                    0.0f,
                    0.0f);
            }
            else
            {
                uniforms.sizeKeys[index] = Eigen::Vector4f(
                    index == 0 ? 0.0f : 1.0f,
                    index == 0
                        ? emitter.asset.sizeStart
                        : emitter.asset.sizeEnd,
                    0.0f,
                    0.0f);
            }
        }
        uniforms.curveParams = Eigen::Vector4f(
            static_cast<float>(colorKeyCount),
            static_cast<float>(sizeKeyCount),
            static_cast<float>(emitter.asset.flipbookColumns),
            static_cast<float>(emitter.asset.flipbookRows));
        uniforms.flipbookParams = Eigen::Vector4f(
            emitter.asset.flipbookFramesPerSecond,
            emitter.asset.texturePath.empty() ? 0.0f : 1.0f,
            static_cast<float>(emitter.asset.flipbookStartFrame),
            static_cast<float>(emitter.asset.flipbookEndFrame));
        uniforms.flipbookParams2 = Eigen::Vector4f(
            emitter.asset.flipbookLoop ? 1.0f : 0.0f,
            emitter.asset.flipbookRandomStartFrame ? 1.0f : 0.0f,
            0.0f,
            0.0f);
        uniforms.sortParams = Eigen::Vector4f(
            emitter.asset.sortParticles ? 1.0f : 0.0f,
            static_cast<float>(emitter.asset.maxParticles),
            static_cast<float>(emitter.sortCapacity),
            0.0f);
        uniforms.sortPass = Eigen::Vector4f(
            0.0f,
            0.0f,
            static_cast<float>(emitter.sortCapacity),
            0.0f);
        uniforms.screenParams = Eigen::Vector4f(
            viewportHeightPixels,
            emitter.asset.maxScreenSize,
            // prewarm 的稳态池填充使用作者 spawnRate 乘实例倍率；LOD 只
            // 影响 simulation.x 的连续发射预算。
            emitter.asset.spawnRate * emitter.spawnRateScale,
            0.0f);
        uniforms.flipbookFrameDurations = emitter.flipbookTable.durations;
        uniforms.flipbookFramePivots = emitter.flipbookTable.pivots;
        uniforms.flipbookParams3 = emitter.flipbookTable.flags;
        uniforms.flipbookFrameUVs = emitter.flipbookTable.uvs;
        const Eigen::Vector3f collisionPlaneNormal =
            NormalizedParticleCollisionPlaneNormal(emitter.asset);
        uniforms.collisionParams = Eigen::Vector4f(
            static_cast<float>(ParticleCollisionTypeValue(
                emitter.asset.collisionType)),
            emitter.asset.collisionBounce,
            emitter.asset.collisionFriction,
            emitter.asset.collisionRadius);
        uniforms.collisionPlane = Eigen::Vector4f(
            collisionPlaneNormal.x(),
            collisionPlaneNormal.y(),
            collisionPlaneNormal.z(),
            emitter.asset.collisionPlaneOffset);
        uniforms.collisionBoxMin = Eigen::Vector4f(
            emitter.asset.collisionBoxMin.x(),
            emitter.asset.collisionBoxMin.y(),
            emitter.asset.collisionBoxMin.z(),
            emitter.asset.killOnCollision ? 1.0f : 0.0f);
        uniforms.collisionBoxMax = Eigen::Vector4f(
            emitter.asset.collisionBoxMax.x(),
            emitter.asset.collisionBoxMax.y(),
            emitter.asset.collisionBoxMax.z(),
            0.0f);
        uniforms.collisionParams2 = Eigen::Vector4f(
            emitter.asset.collisionCcd ? 1.0f : 0.0f,
            emitter.asset.collisionCapsuleRadius,
            0.0f,
            0.0f);
        uniforms.collisionCapsuleA = Eigen::Vector4f(
            emitter.asset.collisionCapsuleA.x(),
            emitter.asset.collisionCapsuleA.y(),
            emitter.asset.collisionCapsuleA.z(),
            0.0f);
        uniforms.collisionCapsuleB = Eigen::Vector4f(
            emitter.asset.collisionCapsuleB.x(),
            emitter.asset.collisionCapsuleB.y(),
            emitter.asset.collisionCapsuleB.z(),
            0.0f);
        uniforms.burstParams = Eigen::Vector4f(
            emitter.asset.burstInterval,
            0.0f,
            0.0f,
            0.0f);
        uniforms.rotationParams = Eigen::Vector4f(
            emitter.asset.rotation,
            emitter.asset.rotationSpeed,
            emitter.asset.rotationRandom ? 1.0f : 0.0f,
            0.0f);
        uniforms.flipbookFrameRotations =
            emitter.flipbookTable.rotations;
        uniforms.flipbookParams4 =
            emitter.flipbookTable.rotationFlags;
        uniforms.ribbonParams = Eigen::Vector4f(
            static_cast<float>(
                ClampParticleRibbonSegments(
                    emitter.asset.ribbonSegments)),
            emitter.asset.ribbonFade,
            emitter.asset.ribbonWidth,
            0.0f);
        uniforms.meshParams = Eigen::Vector4f(
            static_cast<float>(ParticleMeshAlignmentCode(
                emitter.asset.meshAlignment)),
            0.0f,
            0.0f,
            0.0f);
        uniforms.meshScale = Eigen::Vector4f(
            emitter.asset.meshScale.x(),
            emitter.asset.meshScale.y(),
            emitter.asset.meshScale.z(),
            0.0f);
        uniforms.colorTint = emitter.colorTint;
        uniforms.billboardParams = Eigen::Vector4f(
            emitter.asset.billboardAlignment == "velocity" ? 1.0f : 0.0f,
            0.0f,
            0.0f,
            0.0f);
        uniforms.shapeParams = Eigen::Vector4f(
            static_cast<float>(
                ParticleShapeCode(emitter.asset.shapeType)),
            emitter.asset.shapeHeight,
            emitter.asset.shapeInnerRadius,
            0.0f);
        uniforms.shapeExtent = Eigen::Vector4f(
            emitter.asset.shapeExtent.x(),
            emitter.asset.shapeExtent.y(),
            emitter.asset.shapeExtent.z(),
            0.0f);
        std::memcpy(
            emitter.frameUniforms.at(imageIndex).mapped,
            &uniforms,
            sizeof(uniforms));

        if (emitter.needsClear)
        {
            commandBuffer.fillBuffer(
                emitter.particles.buffer,
                0,
                VK_WHOLE_SIZE,
                0);
            vk::BufferMemoryBarrier clearToCompute;
            clearToCompute
                .setBuffer(emitter.particles.buffer)
                .setOffset(0)
                .setSize(VK_WHOLE_SIZE)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(
                    vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eComputeShader,
                {},
                nullptr,
                clearToCompute,
                nullptr);
            emitter.needsClear = false;
        }

        simulatePipeline->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            simulatePipeline->GetPipelineLayout(),
            0,
            emitter.computeSets.at(imageIndex),
            nullptr);
        const uint32_t groupCount =
            (emitter.asset.maxParticles + 255u) / 256u;
        simulatePipeline->Dispatch(
            commandBuffer,
            groupCount,
            1,
            1);

        if (emitter.asset.sortParticles)
        {
            ++frameStats.sortedEmitters;
            frameStats.sortCapacityTotal += emitter.sortCapacity;
            frameStats.sortDispatchCount +=
                ParticleSortDispatchCount(emitter.sortCapacity);
            // 模拟写入 particle buffer，sort init 读取同一段数据并写入 pairA。
            InsertBufferBarrier(
                commandBuffer,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderWrite,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite);
            sortInitPipeline->Bind(commandBuffer);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute,
                sortInitPipeline->GetPipelineLayout(),
                0,
                emitter.sortInitSets.at(imageIndex),
                nullptr);
            const uint32_t sortGroupCount =
                (emitter.sortCapacity + 255u) / 256u;
            sortInitPipeline->Dispatch(
                commandBuffer,
                sortGroupCount,
                1,
                1);

            bool sourceIsA = true;
            for (uint32_t k = 2;
                 k <= emitter.sortCapacity;
                 k <<= 1)
            {
                for (uint32_t j = k >> 1; j > 0; j >>= 1)
                {
                    // 上一 pass 仍在读取 sortPass；先结束读再覆盖该 UBO 区域。
                    InsertBufferBarrier(
                        commandBuffer,
                        vk::PipelineStageFlagBits::eComputeShader,
                        vk::AccessFlagBits::eShaderRead,
                        vk::PipelineStageFlagBits::eTransfer,
                        vk::AccessFlagBits::eTransferWrite);
                    const Eigen::Vector4f sortPass(
                        static_cast<float>(k),
                        static_cast<float>(j),
                        static_cast<float>(emitter.sortCapacity),
                        0.0f);
                    commandBuffer.updateBuffer(
                        emitter.frameUniforms.at(imageIndex).buffer,
                        offsetof(FrameUniforms, sortPass),
                        sizeof(sortPass),
                        &sortPass);
                    // 同时发布上一 pass 的 pair 写入和本次 sortPass 传输写入。
                    InsertBufferBarrier(
                        commandBuffer,
                        vk::PipelineStageFlagBits::eComputeShader |
                            vk::PipelineStageFlagBits::eTransfer,
                        vk::AccessFlagBits::eShaderWrite |
                            vk::AccessFlagBits::eTransferWrite,
                        vk::PipelineStageFlagBits::eComputeShader,
                        vk::AccessFlagBits::eShaderRead |
                            vk::AccessFlagBits::eShaderWrite);

                    sortPipeline->Bind(commandBuffer);
                    commandBuffer.bindDescriptorSets(
                        vk::PipelineBindPoint::eCompute,
                        sortPipeline->GetPipelineLayout(),
                        0,
                        sourceIsA
                            ? emitter.sortAtoBSets.at(imageIndex)
                            : emitter.sortBtoASets.at(imageIndex),
                        nullptr);
                    sortPipeline->Dispatch(
                        commandBuffer,
                        sortGroupCount,
                        1,
                        1);
                    sourceIsA = !sourceIsA;
                }
            }
        }

        // prewarm 只允许第一帧；若录制后提交失败，下一帧会按正常发射补齐，
        // 不会重复把整池粒子重置成同一寿命分布。
        emitter.prewarm = false;
    }

    vk::MemoryBarrier writeToRead;
    writeToRead
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        {},
        writeToRead,
        nullptr,
        nullptr);

    std::vector<ParticleEmitterDepthKey> depthKeys;
    depthKeys.reserve(emitters.size());
    for (const auto& [key, emitterPtr] : emitters)
    {
        const EmitterResources& emitter = *emitterPtr;
        depthKeys.push_back(ParticleEmitterDepthKey{
            key,
            emitter.worldCenter,
            emitter.boundsRadius});
    }
    const std::vector<size_t> sortedIndices =
        ComputeParticleEmitterDrawOrder(
            depthKeys,
            renderScene.camera.position,
            renderScene.camera.forward);
    drawOrder.clear();
    drawOrder.reserve(sortedIndices.size());
    for (const size_t index : sortedIndices)
    {
        drawOrder.push_back(std::move(depthKeys[index].key));
    }
}

void ParticleRenderer::RecordGraphics(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    frameStats.estimatedDrawCalls = 0;
    frameStats.estimatedDrawnVertices = 0;
    if (!initialized)
    {
        return;
    }

    const auto recordEmitter = [&](EmitterResources& emitter)
    {
        if (emitter.culled ||
            emitter.graphicsSets.size() <= imageIndex)
        {
            return;
        }
        const bool mesh =
            emitter.asset.renderMode == "mesh" &&
            !emitter.meshFallbackBillboard &&
            emitter.meshIndexCount > 0 &&
            emitter.meshVertices.buffer &&
            emitter.meshIndices.buffer;
        const std::shared_ptr<PipelineBase>& pipeline = mesh
            ? (emitter.asset.additive
                ? meshAdditivePipeline
                : meshAlphaPipeline)
            : (emitter.asset.renderMode == "ribbon"
                ? (emitter.asset.additive
                    ? ribbonAdditivePipeline
                    : ribbonAlphaPipeline)
                : (emitter.asset.additive
                    ? additivePipeline
                    : alphaPipeline));
        if (!pipeline)
        {
            return;
        }
        commandBuffer.bindPipeline(
            pipeline->GetBindPoint(),
            pipeline->GetPipeline());
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            pipeline->GetPipelineLayout(),
            0,
            emitter.graphicsSets.at(imageIndex),
            nullptr);

        // sceneDepth 由 RenderGraph 的 pass 输入描述符集提供；只有 shader 反射
        // 真正声明了 set 3 时才绑定，避免改变无深度采样的旧管线契约。
        bool usesPassSet = false;
        for (const ShaderBinding& binding : pipeline->GetShaderBindings())
        {
            if (binding.set == PassSetIndex)
            {
                usesPassSet = true;
                break;
            }
        }
        const auto& descriptorSets = renderPass.GetDescriptorSets();
        if (usesPassSet &&
            imageIndex < descriptorSets.size() &&
            PassSetIndex < descriptorSets[imageIndex].size())
        {
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics,
                pipeline->GetPipelineLayout(),
                PassSetIndex,
                descriptorSets[imageIndex][PassSetIndex],
                nullptr);
        }
        uint64_t drawnVertices = 0;
        if (mesh)
        {
            const vk::DeviceSize vertexOffset = 0;
            commandBuffer.bindVertexBuffers(
                0,
                1,
                &emitter.meshVertices.buffer,
                &vertexOffset);
            commandBuffer.bindIndexBuffer(
                emitter.meshIndices.buffer,
                0,
                vk::IndexType::eUint32);
            commandBuffer.drawIndexed(
                emitter.meshIndexCount,
                emitter.asset.maxParticles,
                0,
                0,
                0);
            drawnVertices = ParticleMeshDrawnVertexCount(
                emitter.meshIndexCount,
                emitter.asset.maxParticles);
        }
        else
        {
            const bool ribbon =
                emitter.asset.renderMode == "ribbon";
            const uint32_t vertexCount = ribbon
                ? static_cast<uint32_t>(
                    ParticleRibbonVertexCount(
                        emitter.asset.maxParticles,
                        emitter.asset.ribbonSegments))
                : emitter.asset.maxParticles * 6u;
            commandBuffer.draw(
                vertexCount,
                1,
                0,
                0);
            drawnVertices = vertexCount;
        }
        ++frameStats.estimatedDrawCalls;
        frameStats.estimatedDrawnVertices += drawnVertices;
    };

    std::unordered_set<std::string> orderedKeys;
    orderedKeys.reserve(drawOrder.size());
    for (const std::string& key : drawOrder)
    {
        orderedKeys.insert(key);
        const auto emitterIt = emitters.find(key);
        if (emitterIt != emitters.end())
        {
            recordEmitter(*emitterIt->second);
        }
    }

    // Synchronize 可能在本帧排序后增删发射器；未进入排序表的池按原 map
    // 顺序补画，保证它们不会因为 drawOrder 短暂失配而消失。
    for (auto& [key, emitterPtr] : emitters)
    {
        if (orderedKeys.find(key) == orderedKeys.end())
        {
            recordEmitter(*emitterPtr);
        }
    }
}

bool ParticleRenderer::DumpParticles(
    RendererBackendVulkan& readbackBackend,
    const std::filesystem::path& outputPath,
    uint32_t maxPerEmitter,
    const ParticleFrameStats& frameStats,
    std::string& outError)
{
    static_assert(
        sizeof(GpuParticle) == sizeof(Eigen::Vector4f) * 5,
        "Particle dump assumes the std430 Particle layout is five vec4s.");

    if (!initialized || backend != &readbackBackend)
    {
        outError =
            "Particle renderer is not initialized for this backend.";
        return false;
    }
    if (emitters.empty())
    {
        outError = "No particle emitters are available for dump.";
        return false;
    }

    maxPerEmitter = std::min(maxPerEmitter, kParticleDumpMaxDetails);
    std::vector<ParticleDumpEmitter> dumpEmitters;
    dumpEmitters.reserve(emitters.size());

    for (const auto& [key, emitterPtr] : emitters)
    {
        const EmitterResources& emitter = *emitterPtr;
        if (!emitter.particles.buffer ||
            !emitter.particles.memory ||
            emitter.asset.maxParticles == 0)
        {
            outError = "Particle emitter has no readable particle pool: " + key;
            return false;
        }

        const vk::DeviceSize readbackBytes =
            sizeof(GpuParticle) *
            static_cast<vk::DeviceSize>(emitter.asset.maxParticles);
        vk::Buffer readbackBuffer;
        vk::DeviceMemory readbackMemory;
        void* mappedMemory = nullptr;
        bool mapped = false;
        try
        {
            std::tie(readbackBuffer, readbackMemory) =
                readbackBackend.CreateBuffer(
                    readbackBytes,
                    vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleDumpReadback:" + key);

            vk::CommandBuffer commandBuffer =
                readbackBackend.BeginSingleTimeCommands();
            vk::BufferMemoryBarrier toTransfer;
            toTransfer
                .setSrcAccessMask(
                    vk::AccessFlagBits::eMemoryRead |
                    vk::AccessFlagBits::eMemoryWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setBuffer(emitter.particles.buffer)
                .setOffset(0)
                .setSize(VK_WHOLE_SIZE);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                emitter.particles.buffer,
                readbackBuffer,
                // vkCmdCopyBuffer 不接受 VK_WHOLE_SIZE；必须给出实际字节数。
                vk::BufferCopy(0, 0, readbackBytes));

            vk::BufferMemoryBarrier toHost;
            toHost
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eHostRead)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setBuffer(readbackBuffer)
                .setOffset(0)
                .setSize(VK_WHOLE_SIZE);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eHost,
                {},
                nullptr,
                toHost,
                nullptr);
            // End 内部提交并等待 fence；返回后 host-coherent staging 可安全映射。
            readbackBackend.EndSingleTimeCommands(commandBuffer);

            mappedMemory = readbackBackend.MapMemory(
                readbackMemory,
                readbackBytes);
            mapped = mappedMemory != nullptr;
            if (!mappedMemory)
            {
                throw std::runtime_error("map returned null");
            }

            const GpuParticle* particles =
                static_cast<const GpuParticle*>(mappedMemory);
            ParticleDumpEmitter dumpEmitter;
            dumpEmitter.key = emitter.key;
            dumpEmitter.maxParticles = emitter.asset.maxParticles;
            dumpEmitter.culled = emitter.culled;
            dumpEmitter.sortParticles = emitter.asset.sortParticles;
            dumpEmitter.localSpace = emitter.asset.localSpace;
            dumpEmitter.renderMode = emitter.asset.renderMode;
            if (emitter.asset.renderMode == "mesh" &&
                !emitter.meshFallbackBillboard &&
                emitter.meshIndexCount > 0)
            {
                dumpEmitter.verticesPerParticle =
                    emitter.meshIndexCount;
            }
            else
            {
                dumpEmitter.verticesPerParticle =
                    emitter.asset.renderMode == "ribbon"
                        ? ParticleRibbonVerticesPerParticle(
                            emitter.asset.ribbonSegments)
                        : 6u;
            }
            dumpEmitter.boundsCenter = emitter.worldCenter;
            dumpEmitter.boundsRadius = emitter.boundsRadius;
            dumpEmitter.elapsedSeconds = emitter.elapsedSeconds;
            dumpEmitter.particles.reserve(
                std::min(
                    emitter.asset.maxParticles,
                    maxPerEmitter));

            for (uint32_t index = 0;
                 index < emitter.asset.maxParticles;
                 ++index)
            {
                const GpuParticle& particle = particles[index];
                const float age = particle.positionAge.w();
                const float lifetime = particle.velocityLife.w();
                if (lifetime > 0.0f && age < lifetime)
                {
                    ++dumpEmitter.aliveCount;
                    if (dumpEmitter.particles.size() < maxPerEmitter)
                    {
                        ParticleDumpSample sample;
                        sample.index = index;
                        sample.position =
                            particle.positionAge.head<3>();
                        sample.age = age;
                        sample.velocity =
                            particle.velocityLife.head<3>();
                        sample.lifetime = lifetime;
                        dumpEmitter.particles.push_back(sample);
                    }
                }
            }
            dumpEmitters.push_back(std::move(dumpEmitter));

            readbackBackend.UnmapMemory(readbackMemory);
            mapped = false;
            readbackBackend.DestroyBuffer(
                readbackBuffer,
                readbackMemory);
            readbackBuffer = vk::Buffer();
            readbackMemory = vk::DeviceMemory();
        }
        catch (const std::exception& exception)
        {
            if (mapped)
            {
                readbackBackend.UnmapMemory(readbackMemory);
            }
            if (readbackBuffer || readbackMemory)
            {
                readbackBackend.DestroyBuffer(
                    readbackBuffer,
                    readbackMemory);
            }
            outError =
                "Particle dump readback failed for " + key + ": " +
                exception.what();
            return false;
        }
    }

    try
    {
        std::filesystem::create_directories(outputPath.parent_path());
        const std::string text =
            BuildParticleDumpJson(
                std::move(dumpEmitters),
                maxPerEmitter,
                frameStats).dump(2) + "\n";
        WriteTextFileAtomically(outputPath, text);
    }
    catch (const std::exception& exception)
    {
        outError =
            "Particle dump write failed: " +
            std::string(exception.what());
        return false;
    }
    return true;
}

} // namespace VL
