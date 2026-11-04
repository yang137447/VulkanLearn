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
#include "pipeline/graphicsPipelineLayoutDesc.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "particles/particleCollision.h"
#include "particles/particleBudget.h"
#include "particles/particleBeam.h"
#include "particles/particleDeterminism.h"
#include "particles/particleDump.h"
#include "particles/particleEmitterBounds.h"
#include "particles/particleEmitterKey.h"
#include "particles/particleEmitterCulling.h"
#include "particles/particleEmitterOrder.h"
#include "particles/particleHitScan.h"
#include "particles/particleLod.h"
#include "particles/particleMesh.h"
#include "particles/particleRibbon.h"
#include "particles/particleShadowCaster.h"
#include "particles/particleTextureReference.h"
#include "particles/particleUserParameterOverride.h"
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

Renderpass* FindDistortionPass(RenderGraph& graph)
{
    for (auto& [name, pass] : graph.GetRenderpasses())
    {
        (void)name;
        if (pass.type == RenderGraphPassType::Distortion)
        {
            return &pass;
        }
    }
    return nullptr;
}

ShaderBinding MakeParticleLayoutBinding(
    uint32_t binding,
    vk::DescriptorType type,
    uint32_t descriptorCount,
    vk::ShaderStageFlags stageFlags,
    const char* name)
{
    ShaderBinding result;
    result.set = 0;
    result.binding = binding;
    result.type = type;
    result.descriptorCount = descriptorCount;
    result.stageFlags = stageFlags;
    result.name = name;
    return result;
}

// MakeParticleLayoutBinding 固定写 set 0（共享合同）；pass set 3 必须显式
// 指定 set，否则 BuildPipelineLayoutBindings 会把这些 binding 归到 set 0。
ShaderBinding MakeParticlePassSetBinding(uint32_t binding, const char* name)
{
    ShaderBinding result = MakeParticleLayoutBinding(
        binding,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        name);
    result.set = PassSetIndex;
    return result;
}

GraphicsPipelineLayoutDesc BuildParticleGlobalLayout()
{
    // particleGlobal.frag 明确不使用 CSM，编译器会把 shadowMap 从反射
    // 结果中裁掉；但 particle pass 的 set 3 由图输入分配为
    // sceneDepth + shadowMap + localShadowMap 三个 binding。显式覆盖 set 3，
    // 保证 pipeline layout 与 render pass descriptor set 的布局兼容。
    GraphicsPipelineLayoutDesc layout;
    // 全局 alpha 池有自己的一套 set 0：0 UBO、1 gathered records、
    // 2 sorted pairs、3 纹理数组、5 debug、6/7 两盏灯缓冲、8/9 IBL。
    // 同样必须显式覆盖 6..9，否则未采样的 brdfLut 会被 glslang 裁掉，
    // 导致 draw set 的写入越界。
    layout.overrideSets[0] = true;
    std::vector<ShaderBinding>& globalBindings = layout.setBindings[0];
    globalBindings.push_back(MakeParticleLayoutBinding(
        0,
        vk::DescriptorType::eUniformBuffer,
        1,
        vk::ShaderStageFlagBits::eVertex |
            vk::ShaderStageFlagBits::eFragment,
        "ParticleFrame"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        1,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eVertex,
        "GatheredBuffer"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        2,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eVertex,
        "SortedPairs"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        3,
        vk::DescriptorType::eCombinedImageSampler,
        kParticleGlobalTextureSlots,
        vk::ShaderStageFlagBits::eVertex |
            vk::ShaderStageFlagBits::eFragment,
        "particleTextures"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        5,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "ParticleDebug"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        6,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "ParticleSceneLightSSBO"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        7,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "ParticleLightSSBO"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        8,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "prefilteredEnvironmentCube"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        9,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "brdfLut"));
    layout.overrideSets[PassSetIndex] = true;
    ShaderBinding sceneDepth;
    sceneDepth.set = PassSetIndex;
    sceneDepth.binding = 0;
    sceneDepth.type = vk::DescriptorType::eCombinedImageSampler;
    sceneDepth.descriptorCount = 1;
    sceneDepth.stageFlags = vk::ShaderStageFlagBits::eFragment;
    sceneDepth.name = "sceneDepth";
    ShaderBinding shadowMap;
    shadowMap.set = PassSetIndex;
    shadowMap.binding = 1;
    shadowMap.type = vk::DescriptorType::eCombinedImageSampler;
    shadowMap.descriptorCount = 1;
    shadowMap.stageFlags = vk::ShaderStageFlagBits::eFragment;
    shadowMap.name = "shadowMap";
    // 全局池现在会采样局部光阴影（点光/聚光），必须与 pass 的第三个输入对齐。
    ShaderBinding localShadowMap;
    localShadowMap.set = PassSetIndex;
    localShadowMap.binding = 2;
    localShadowMap.type = vk::DescriptorType::eCombinedImageSampler;
    localShadowMap.descriptorCount = 1;
    localShadowMap.stageFlags = vk::ShaderStageFlagBits::eFragment;
    localShadowMap.name = "localShadowMap";
    layout.setBindings[PassSetIndex] = {
        std::move(sceneDepth),
        std::move(shadowMap),
        std::move(localShadowMap)};
    return layout;
}

GraphicsPipelineLayoutDesc BuildParticleSharedLayout()
{
    // 所有逐发射器图形管线（billboard/ribbon/beam/mesh/distortion）共用同一批
    // descriptor set，因此 set 0 必须逐字段完全一致。这里显式列出 0..9：
    // glslang 会裁掉某个 shader 未静态使用的 binding（billboard 不采样
    // brdfLut，mesh 才采样），若各管线各自走反射，layout 会不兼容并在
    // vkCmdBindDescriptorSets 报 00358/08600，或写入时越界。
    GraphicsPipelineLayoutDesc layout;
    layout.overrideSets[0] = true;
    std::vector<ShaderBinding>& bindings = layout.setBindings[0];
    bindings.push_back(MakeParticleLayoutBinding(
        0,
        vk::DescriptorType::eUniformBuffer,
        1,
        vk::ShaderStageFlagBits::eVertex |
            vk::ShaderStageFlagBits::eFragment,
        "ParticleFrame"));
    bindings.push_back(MakeParticleLayoutBinding(
        1,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eVertex,
        "ParticleBuffer"));
    bindings.push_back(MakeParticleLayoutBinding(
        2,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eVertex |
            vk::ShaderStageFlagBits::eFragment,
        "particleTexture"));
    bindings.push_back(MakeParticleLayoutBinding(
        3,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eVertex,
        "SortedPairs"));
    bindings.push_back(MakeParticleLayoutBinding(
        4,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eVertex,
        "ParticleAliveIndices"));
    bindings.push_back(MakeParticleLayoutBinding(
        5,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "ParticleDebug"));
    bindings.push_back(MakeParticleLayoutBinding(
        6,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "ParticleSceneLightSSBO"));
    bindings.push_back(MakeParticleLayoutBinding(
        7,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "ParticleLightSSBO"));
    bindings.push_back(MakeParticleLayoutBinding(
        8,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "prefilteredEnvironmentCube"));
    bindings.push_back(MakeParticleLayoutBinding(
        9,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "brdfLut"));
    // Set 3 必须与 RenderGraph 的输入全集完全一致。反射会裁掉未静态使用的
    // binding（beam 不采样 CSM/local shadow），若各管线各走反射，layout 会
    // 少于 pass descriptor set 的 0/1/2 三个 binding，导致绑定时不兼容。
    layout.overrideSets[PassSetIndex] = true;
    layout.setBindings[PassSetIndex] = {
        MakeParticlePassSetBinding(0, "sceneDepth"),
        MakeParticlePassSetBinding(1, "shadowMap"),
        MakeParticlePassSetBinding(2, "localShadowMap")};
    return layout;
}

ParticleRenderIdentity BuildParticleRenderIdentity(
    RenderGraph& graph,
    const Renderpass& pass,
    const Renderpass* distortionPass,
    const RendererBackendVulkan& backend)
{
    ParticleRenderIdentity identity;
    identity.ownerGeneration = graph.GetOwnerGeneration();
    identity.particleRenderPass = reinterpret_cast<uintptr_t>(
        static_cast<VkRenderPass>(pass.renderPass));
    identity.distortionRenderPass = distortionPass != nullptr
        ? reinterpret_cast<uintptr_t>(
            static_cast<VkRenderPass>(distortionPass->renderPass))
        : 0;
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

uint32_t GlobalSortCapacity(uint64_t desired)
{
    uint32_t capacity = kParticleSortMinimumCapacity;
    while (capacity < desired &&
           capacity < kParticleGlobalSortCapacity)
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
            generateMipmaps) +
            (asset.renderMode == "distortion" ? "|data" : "|color");
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
        // mesh.scale 参与局部包围球计算；非均匀缩放不能用简单的半径乘
        // 最大轴近似，因此把缩放纳入源身份，变化时重建 mesh 缓冲与 bounds。
        key += '|';
        key += std::to_string(asset.meshScale.x()) + "," +
            std::to_string(asset.meshScale.y()) + "," +
            std::to_string(asset.meshScale.z());
        return key;
    }
    catch (const std::exception&)
    {
        // 文件删除/路径解析失败也必须是稳定 key；下一次同步会触发重建。
        return asset.meshPath + "|missing";
    }
}

ParticleMaterialParams LoadParticleMaterialInstance(
    const std::string& path,
    std::string& outError)
{
    ParticleMaterialParams params;
    try
    {
        RequireValidParticleMeshPath(path, "materialInstancePath");
        std::ifstream file(CommonFunction::Path(path));
        if (!file.is_open())
        {
            outError = "file not found";
            return params;
        }
        nlohmann::json json;
        file >> json;
        params = ParseParticleMaterialInstanceJson(json);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
    }
    return params;
}

} // namespace

ParticleRenderer::~ParticleRenderer()
{
    // 正常关闭走 Shutdown；若调用方绕过它，至少在同一 backend 仍有效时
    // 走到设备空闲并释放全局池/管线/查询池，避免析构泄漏。
    if (backend != nullptr)
    {
        try
        {
            backend->WaitIdle();
        }
        catch (...)
        {
            // 析构不能把关闭路径异常抛回引擎。
        }
        Shutdown(*backend);
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
            FindDistortionPass(graph),
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
    pipelineFactory = &factory;

    Renderpass* particlePass = FindParticlePass(graph);
    if (particlePass == nullptr)
    {
        // 图没有声明 particle pass 时保持未初始化；渲染路径只跳过粒子，
        // 不把缺少可选 pass 当成启动失败。
        renderIdentity = {};
        initialized = false;
        return;
    }
    Renderpass* distortionPass = FindDistortionPass(graph);

    renderIdentity = BuildParticleRenderIdentity(
        graph,
        *particlePass,
        distortionPass,
        renderer);
    simulatePipeline =
        factory.CreateComputePipeline("particle/particleSimulate");
    sortInitPipeline =
        factory.CreateComputePipeline("particle/particleSortInit");
    sortPipeline =
        factory.CreateComputePipeline("particle/particleSort");
    finalizeCountPipeline =
        factory.CreateComputePipeline("particle/particleFinalizeCount");
    globalGatherPipeline =
        factory.CreateComputePipeline("particle/particleGlobalGather");
    globalSortInitPipeline =
        factory.CreateComputePipeline("particle/particleGlobalSortInit");
    particleLightEmitPipeline =
        factory.CreateComputePipeline("particle/particleLightEmit");

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
    // 所有逐发射器图形管线共用这一份 set 0 显式合同；不能依赖各自的
    // shader 反射，否则未被静态使用的 binding 会被裁掉导致 layout 不一致。
    const GraphicsPipelineLayoutDesc sharedLayout =
        BuildParticleSharedLayout();
    additivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        artifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive,
        sharedLayout);
    alphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        artifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        sharedLayout);

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
        GraphicsPipelineBlendMode::Additive,
        sharedLayout);
    ribbonAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        ribbonArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        sharedLayout);

    // beam 同样不使用顶点输入；起止点、段数与噪声参数都来自 FrameUniforms。
    // 与 ribbon 使用同一 pass 合同和 descriptor set 0，避免额外描述符池。
    ShaderVariantKey beamVariant = variant;
    beamVariant.shaderName = "particle/particleBeam";
    const GraphicsShaderVariantArtifact& beamArtifact =
        factory.PrepareGraphicsShaderVariant(beamVariant);
    beamAdditivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        beamArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive,
        sharedLayout);
    beamAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        beamArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        sharedLayout);

    // distortion 自己写独立附件，因此必须使用 distortion pass 的 VkRenderPass；
    // set 0 复用共享合同；distortion pass 的图输入只有 sceneDepth，所以 set 3
    // 必须去掉共享合同里的 shadowMap/localShadowMap，否则 layout 与图不一致。
    if (distortionPass != nullptr)
    {
        GraphicsPipelineLayoutDesc distortionLayout = sharedLayout;
        distortionLayout.setBindings[PassSetIndex] = {
            MakeParticlePassSetBinding(0, "sceneDepth")};

        ShaderVariantKey distortionVariant = variant;
        distortionVariant.shaderName = "particle/particleDistortion";
        const GraphicsShaderVariantArtifact& distortionArtifact =
            factory.PrepareGraphicsShaderVariant(distortionVariant);
        PassPipelineContractKey distortionContract =
            distortionPass->pipelineContractKey;
        distortionContract.useVertexInput = false;
        distortionContract.depthTestEnable = false;
        distortionContract.depthWriteEnable = false;
        distortionContract.depthCompareOp = vk::CompareOp::eLess;
        distortionPipeline = factory.CreateGraphicsPipeline(
            &distortionPass->renderPass,
            distortionContract,
            distortionArtifact,
            vk::CullModeFlagBits::eNone,
            GraphicsPipelineBlendMode::Additive,
            distortionLayout);
    }

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
        GraphicsPipelineBlendMode::Additive,
        sharedLayout);
    meshAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        meshContract,
        meshArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        sharedLayout);

    // 跨发射器 alpha billboard 的统一绘制管线：无顶点输入，读取
    // gathered 记录 + 全局排序 pair 表 + 8 槽纹理数组。
    ShaderVariantKey globalVariant = variant;
    globalVariant.shaderName = "particle/particleGlobal";
    const GraphicsShaderVariantArtifact& globalArtifact =
        factory.PrepareGraphicsShaderVariant(globalVariant);
    globalDrawPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        globalArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        BuildParticleGlobalLayout());

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
    // 场景光照 descriptor 的兜底资源必须在第一个发射器创建前就绪。
    CreateFallbackLightingResources();
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
    particleLightBuffers.clear();
    particleLightBufferGeneration = 0;
    sceneLightInfos.clear();
    sceneParticleLightInfos.clear();
    environmentCubeDescriptor = vk::DescriptorImageInfo{};
    brdfLutDescriptor = vk::DescriptorImageInfo{};
    sceneLightingGeneration = 0;
    fallbackSceneLightInfo = vk::DescriptorBufferInfo{};
    fallbackParticleLightInfo = vk::DescriptorBufferInfo{};
    DestroyFallbackLightingResources();
    warnedMissingParticleLightBuffer = false;
    textureCache.clear();
    DestroyGlobalSortPool();
    simulatePipeline.reset();
    sortInitPipeline.reset();
    sortPipeline.reset();
    finalizeCountPipeline.reset();
    globalGatherPipeline.reset();
    globalSortInitPipeline.reset();
    particleLightEmitPipeline.reset();
    additivePipeline.reset();
    alphaPipeline.reset();
    ribbonAdditivePipeline.reset();
    ribbonAlphaPipeline.reset();
    beamAdditivePipeline.reset();
    beamAlphaPipeline.reset();
    distortionPipeline.reset();
    meshAdditivePipeline.reset();
    meshAlphaPipeline.reset();
    shadowPipelines.clear();
    pipelineFactory = nullptr;
    globalDrawPipeline.reset();
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

const ParticleSystemAsset&
ParticleRenderer::ResolveParticleSystemAsset(
    const ParticleSystemSnapshot& snapshot,
    const std::unordered_map<std::string, float>& runtimeParameters)
{
    const ParticleSystemAsset& base = snapshot.asset;
    // 只有“带参数的 v2 作者 JSON + 该资产实际引用的运行时值”才需要重解析。
    const std::unordered_map<std::string, float> effectiveParameters =
        FilterParticleRuntimeParameters(base, runtimeParameters);
    if (!base.authoringJson || effectiveParameters.empty())
    {
        return base;
    }

    const std::string cacheKey = BuildParticleSystemAssetKey(
        snapshot.name, base.sourcePath);
    const uint64_t signature =
        BuildParticleUserParameterSignature(
            effectiveParameters);
    const nlohmann::json* authoring = base.authoringJson.get();

    auto entryIt = userParameterCache.find(cacheKey);
    if (entryIt != userParameterCache.end() &&
        entryIt->second.attempted &&
        entryIt->second.signature == signature &&
        entryIt->second.authoringJson == authoring)
    {
        return entryIt->second.parsed
            ? entryIt->second.asset
            : (entryIt->second.asset.authoringJson !=
                    base.authoringJson ||
                entryIt->second.asset.emitters.empty()
                ? base
                : entryIt->second.asset);
    }

    // 缓存有界：每个系统一条，超出上限时淘汰最早插入项。缓存只服务
    // 渲染线程，不跨系统共享资产。
    if (entryIt == userParameterCache.end())
    {
        if (userParameterCache.size() >= 32)
        {
            userParameterCache.erase(userParameterCache.begin());
        }
        ParticleUserParameterCacheEntry entry;
        entry.asset = base; // 解析失败时的 last-good 起点
        entryIt = userParameterCache.emplace(
            cacheKey, std::move(entry)).first;
    }

    ParticleUserParameterCacheEntry& entry = entryIt->second;
    entry.signature = signature;
    entry.authoringJson = authoring;
    entry.attempted = true;
    try
    {
        // 合并 -> ParseJson -> 运行时-only emitter 对齐；缓存资产始终
        // 保留原始作者 JSON 指针，下一次覆盖仍在原始模块结构上合并。
        ParticleSystemAsset parsed =
            ParseParticleAssetWithRuntimeParameters(
                base,
                effectiveParameters,
                std::filesystem::path(
                    CommonFunction::GetResourcePath()));
        entry.asset = std::move(parsed);
        entry.parsed = true;
    }
    catch (const std::exception& exception)
    {
        // 渲染线程不能抛异常：保留 last-good，并只告警一次。
        WarnUserParameterFailureOnce(
            cacheKey, signature, exception.what());
        entry.parsed = false;
    }
    return entry.parsed
        ? entry.asset
        : (entry.asset.authoringJson != base.authoringJson ||
            entry.asset.emitters.empty()
            ? base
            : entry.asset);
}

void ParticleRenderer::WarnUserParameterFailureOnce(
    const std::string& key,
    uint64_t signature,
    const std::string& error)
{
    const std::string warningKey =
        key + "|" + std::to_string(signature);
    if (!warnedUserParameterFailures.insert(warningKey).second)
    {
        return;
    }
    const std::string message =
        "Particle runtime user parameters ignored for '" + key +
        "': " + error + "; keeping the last good asset";
    ProcessLog().Append(
        LogSeverity::Warning, "Particles", message);
    std::cout << "[Particles][Warning] " << message << std::endl;
}

void ParticleRenderer::SetParticleLightBuffers(
    std::vector<vk::Buffer> buffers)
{
    if (particleLightBuffers != buffers)
    {
        ++particleLightBufferGeneration;
    }
    particleLightBuffers = std::move(buffers);
}

namespace
{

bool SameBufferInfo(
    const vk::DescriptorBufferInfo& left,
    const vk::DescriptorBufferInfo& right)
{
    return left.buffer == right.buffer &&
        left.offset == right.offset &&
        left.range == right.range;
}

bool SameImageInfo(
    const vk::DescriptorImageInfo& left,
    const vk::DescriptorImageInfo& right)
{
    return left.sampler == right.sampler &&
        left.imageView == right.imageView &&
        left.imageLayout == right.imageLayout;
}

} // namespace

void ParticleRenderer::SetSceneLightingResources(
    std::vector<vk::DescriptorBufferInfo> lightInfos,
    std::vector<vk::DescriptorBufferInfo> particleLightInfos,
    vk::DescriptorImageInfo environmentCube,
    vk::DescriptorImageInfo brdfLut)
{
    bool changed = sceneLightInfos.size() != lightInfos.size() ||
        sceneParticleLightInfos.size() != particleLightInfos.size() ||
        !SameImageInfo(environmentCubeDescriptor, environmentCube) ||
        !SameImageInfo(brdfLutDescriptor, brdfLut);
    for (size_t index = 0; !changed && index < lightInfos.size(); ++index)
    {
        changed = !SameBufferInfo(sceneLightInfos[index], lightInfos[index]);
    }
    for (size_t index = 0;
         !changed && index < particleLightInfos.size();
         ++index)
    {
        changed = !SameBufferInfo(
            sceneParticleLightInfos[index],
            particleLightInfos[index]);
    }
    if (changed)
    {
        ++sceneLightingGeneration;
    }
    sceneLightInfos = std::move(lightInfos);
    sceneParticleLightInfos = std::move(particleLightInfos);
    environmentCubeDescriptor = environmentCube;
    brdfLutDescriptor = brdfLut;
}

const vk::DescriptorBufferInfo&
ParticleRenderer::SceneLightInfoForImage(uint32_t imageIndex) const
{
    return imageIndex < sceneLightInfos.size()
        ? sceneLightInfos[imageIndex]
        : fallbackSceneLightInfo;
}

const vk::DescriptorBufferInfo&
ParticleRenderer::ParticleLightInfoForImage(uint32_t imageIndex) const
{
    return imageIndex < sceneParticleLightInfos.size()
        ? sceneParticleLightInfos[imageIndex]
        : fallbackParticleLightInfo;
}

const vk::DescriptorImageInfo&
ParticleRenderer::EnvironmentCubeDescriptor() const
{
    return environmentCubeDescriptor.imageView != vk::ImageView{}
        ? environmentCubeDescriptor
        : fallbackEnvironmentCube->GetDescriptorInfo();
}

const vk::DescriptorImageInfo&
ParticleRenderer::BrdfLutDescriptor() const
{
    return brdfLutDescriptor.imageView != vk::ImageView{}
        ? brdfLutDescriptor
        : defaultBlackTexture->GetDescriptorInfo();
}

void ParticleRenderer::CreateFallbackLightingResources()
{
    if (backend == nullptr)
    {
        return;
    }

    // 32 字节 = LightSSBOHeader（6 个有效 int + 2 个 padding）。全零表示
    // 没有任何场景灯，材质仍可安全读取 binding 6。
    constexpr vk::DeviceSize kSceneLightHeaderBytes = 32;
    const auto sceneLightAllocation = backend->CreateBuffer(
        kSceneLightHeaderBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleFallbackSceneLights");
    fallbackSceneLightBuffer.buffer = sceneLightAllocation.first;
    fallbackSceneLightBuffer.memory = sceneLightAllocation.second;
    fallbackSceneLightBuffer.mapped = backend->MapMemory(
        fallbackSceneLightBuffer.memory,
        kSceneLightHeaderBytes);
    if (fallbackSceneLightBuffer.mapped != nullptr)
    {
        std::memset(
            fallbackSceneLightBuffer.mapped,
            0,
            static_cast<size_t>(kSceneLightHeaderBytes));
    }
    fallbackSceneLightInfo = vk::DescriptorBufferInfo(
        fallbackSceneLightBuffer.buffer,
        0,
        VK_WHOLE_SIZE);

    // GPU 粒子灯兜底：header 的 count/capacity 都是 0，任何读取循环立即结束。
    const auto particleLightAllocation = backend->CreateBuffer(
        ParticleLightSSBOSize(),
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleFallbackLights");
    fallbackParticleLightBuffer.buffer = particleLightAllocation.first;
    fallbackParticleLightBuffer.memory = particleLightAllocation.second;
    fallbackParticleLightBuffer.mapped = backend->MapMemory(
        fallbackParticleLightBuffer.memory,
        ParticleLightSSBOSize());
    if (fallbackParticleLightBuffer.mapped != nullptr)
    {
        std::memset(
            fallbackParticleLightBuffer.mapped,
            0,
            ParticleLightSSBOSize());
    }
    fallbackParticleLightInfo = vk::DescriptorBufferInfo(
        fallbackParticleLightBuffer.buffer,
        0,
        VK_WHOLE_SIZE);

    // BRDF LUT 兜底用全黑图：环境高光乘 0，不会伪造能量。漫反射 IBL 仍
    // 可独立工作，因为它的近似只采样 prefiltered cubemap。
    HostImage blackImage;
    blackImage.width = 1;
    blackImage.height = 1;
    blackImage.channels = 4;
    blackImage.format = HostImage::PixelFormat::RGBA8_UNORM;
    blackImage.semantic = HostImage::TextureSemantic::Data;
    blackImage.rowStrideBytes = 4;
    blackImage.data = {0, 0, 0, 0};
    Texture::CreateDesc blackDesc;
    blackDesc.semantic = HostImage::TextureSemantic::Data;
    blackDesc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    blackDesc.generateMipmaps = false;
    blackDesc.filter = vk::Filter::eLinear;
    blackDesc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    blackDesc.debugName = "ParticleDefaultBlack";
    defaultBlackTexture = std::make_shared<Texture>(
        *backend,
        blackImage,
        blackDesc);

    // 1x1x6 黑色 cubemap：cube 采样要求视图类型匹配，不能拿 2D 白图顶替。
    // 图像创建后必须把全部 6 个 layer 从 UNDEFINED 转到 ShaderReadOnly；
    // 通用 TransitionImageLayout 只处理 1 个 layer，这里显式写 barrier。
    const vk::Format cubeFormat = vk::Format::eR8G8B8A8Unorm;
    vk::ImageCreateInfo cubeInfo;
    cubeInfo
        .setFlags(vk::ImageCreateFlagBits::eCubeCompatible)
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{1, 1, 1})
        .setMipLevels(1)
        .setArrayLayers(6)
        .setFormat(cubeFormat)
        .setTiling(vk::ImageTiling::eOptimal)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setUsage(vk::ImageUsageFlagBits::eSampled)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setSamples(vk::SampleCountFlagBits::e1);
    vk::Image cubeImage;
    vk::DeviceMemory cubeMemory;
    std::tie(cubeImage, cubeMemory) = backend->CreateImage(
        cubeInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleFallbackEnvironmentCube");
    vk::CommandBuffer command = backend->BeginSingleTimeCommands();
    try
    {
        vk::ImageMemoryBarrier barrier;
        barrier
            .setOldLayout(vk::ImageLayout::eUndefined)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(cubeImage)
            .setSrcAccessMask(vk::AccessFlagBits::eNone)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(6));
        command.pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eFragmentShader,
            {},
            nullptr,
            nullptr,
            barrier);
        backend->EndSingleTimeCommands(command);
    }
    catch (...)
    {
        if (command)
        {
            backend->AbortSingleTimeCommands(command);
        }
        throw;
    }
    vk::ImageView cubeView = backend->CreateCubeImageView(
        cubeImage,
        1,
        cubeFormat,
        "ParticleFallbackEnvironmentCubeView");
    vk::Sampler cubeSampler = backend->CreateCubeSampler(
        0.0f,
        "ParticleFallbackEnvironmentCubeSampler");
    fallbackEnvironmentCube = std::make_shared<Texture>(
        *backend,
        cubeImage,
        cubeMemory,
        cubeView,
        cubeSampler,
        1,
        cubeFormat);
}

void ParticleRenderer::DestroyFallbackLightingResources()
{
    if (backend != nullptr)
    {
        DestroyBuffer(*backend, fallbackSceneLightBuffer);
        DestroyBuffer(*backend, fallbackParticleLightBuffer);
    }
    else
    {
        fallbackSceneLightBuffer = {};
        fallbackParticleLightBuffer = {};
    }
    fallbackEnvironmentCube.reset();
    defaultBlackTexture.reset();
}

void ParticleRenderer::Synchronize(const RenderScene& renderScene)
{
    if (!initialized)
    {
        return;
    }

    resolvedSystemAssets.clear();
    std::unordered_set<std::string> desired;
    size_t emitterCount = 0;
    for (const ParticleSystemSnapshot& snapshot :
         renderScene.particleSystems)
        emitterCount += snapshot.asset.emitters.size();
    desired.reserve(emitterCount);
    const uint32_t swapchainImageCount =
        backend->GetSwapchainImageCount();
    for (const ParticleSystemSnapshot& snapshot :
         renderScene.particleSystems)
    {
        // 运行时 userParameters 覆盖可能改变展平后的数值字段；只有签名/
        // 作者指针变化时才重新 ParseJson，失败保留 last-good。
        const ParticleSystemAsset* resolvedAsset = &snapshot.asset;
        const auto runtimeParameters =
            CombineParticleRuntimeParameters(
                renderScene.particleDataChannels,
                snapshot.userParameters);
        try
        {
            resolvedAsset = &ResolveParticleSystemAsset(
                snapshot, runtimeParameters);
        }
        catch (const std::exception& exception)
        {
            // 缓存插入/JSON 复制也可能分配失败；渲染线程必须保持无异常。
            WarnUserParameterFailureOnce(
                snapshot.name + "|" + snapshot.assetPath,
                0u,
                exception.what());
        }
        catch (...)
        {
            WarnUserParameterFailureOnce(
                snapshot.name + "|" + snapshot.assetPath,
                0u,
                "unknown runtime user parameter failure");
        }
        const ParticleSystemAsset& systemAsset = *resolvedAsset;
        resolvedSystemAssets[BuildParticleSystemAssetKey(
            snapshot.name, snapshot.assetPath)] = systemAsset;
        std::unordered_map<std::string,
            const ParticleEmitterTelemetry*> telemetryByName;
        telemetryByName.reserve(snapshot.telemetry.emitters.size());
        for (const ParticleEmitterTelemetry& telemetry :
             snapshot.telemetry.emitters)
        {
            if (!telemetry.name.empty())
            {
                telemetryByName.emplace(
                    telemetry.name, &telemetry);
            }
        }
        // 快照通常带 World 生成的逐发射器预算；若 telemetry 缺失/名称
        // 不匹配，则用本地镜像按 snapshot.budget 计算，避免总上限失效。
        bool telemetryBudgetsUsable =
            !snapshot.telemetry.emitters.empty();
        if (telemetryBudgetsUsable)
        {
            for (const ParticleEmitterAsset& emitter :
                 systemAsset.emitters)
            {
                if (telemetryByName.find(emitter.name) ==
                    telemetryByName.end())
                {
                    telemetryBudgetsUsable = false;
                    break;
                }
            }
        }
        std::vector<uint32_t> fallbackBudgets;
        if (!telemetryBudgetsUsable)
        {
            std::vector<uint32_t> authored;
            authored.reserve(systemAsset.emitters.size());
            for (const ParticleEmitterAsset& emitter :
                 systemAsset.emitters)
            {
                // beam/light/decal 不分配模拟粒子池，也不参与总预算消耗。
                authored.push_back(
                    (emitter.renderMode == "beam" ||
                     emitter.renderMode == "light" ||
                     emitter.renderMode == "decal")
                        ? 0u
                        : emitter.maxParticles);
            }
            fallbackBudgets =
                ComputeParticleEmitterBudgetCapacities(
                    authored,
                    snapshot.budget.maxTotalParticles,
                    snapshot.budget.emitterBudgetScale);
        }

        for (size_t emitterIndex = 0;
             emitterIndex < systemAsset.emitters.size();
             ++emitterIndex)
        {
            const ParticleEmitterAsset& authoredEmitter =
                systemAsset.emitters[emitterIndex];
            // decal 发射器由 CPU runtime 生成 DecalSnapshot；粒子渲染器
            // 不创建池、indirect 参数或排序资源。light 发射器则必须进入
            // GPU 模拟，由独立 light emit compute 输出到场景光照缓冲。
            if (authoredEmitter.renderMode == "decal")
            {
                continue;
            }
            const auto telemetryIt = telemetryByName.find(
                authoredEmitter.name);
            const ParticleEmitterTelemetry* telemetry =
                telemetryIt != telemetryByName.end()
                ? telemetryIt->second
                : nullptr;
            // 预算 0 表示本帧不分配池；不加入 desired，已有池走正常
            // retire 清理路径。预算变化会让下面的 maxParticles 比较
            // 触发池重建。
            const uint32_t budgetedMaxParticles =
                telemetryBudgetsUsable && telemetry != nullptr
                ? telemetry->budgetedMaxParticles
                : fallbackBudgets.at(emitterIndex);
            // beam 是纯渲染模式：即使 emitterBudgetScale=0 或前面的
            // emitter 耗尽总预算，也不能把电弧整个裁掉。
            const uint32_t effectiveMaxParticles =
                authoredEmitter.renderMode == "beam"
                    ? authoredEmitter.maxParticles
                    : (authoredEmitter.renderMode == "light"
                        ? std::min(
                            authoredEmitter.maxParticles,
                            kMaxParticleLights)
                        : ResolveParticleEmitterCapacity(
                            authoredEmitter.maxParticles,
                            budgetedMaxParticles,
                            telemetryBudgetsUsable));
            if (!ShouldAllocateParticlePool(
                    effectiveMaxParticles))
            {
                continue;
            }
            ParticleEmitterAsset emitter = authoredEmitter;
            emitter.maxParticles = effectiveMaxParticles;
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
            auto emitterIt = emitters.find(key);
            const ParticleIndirectDrawShape indirectShape =
                BuildParticleIndirectDrawShape(
                    emitter.renderMode,
                    emitter.maxParticles,
                    emitter.ribbonSegments,
                    emitter.renderMode == "mesh",
                    emitterIt == emitters.end()
                        ? 0u
                        : emitterIt->second->meshIndexCount,
                    emitter.beam.segments,
                    emitter.beam.count,
                    emitter.beam.branchCount);
            // 可见性/启用状态与实例覆盖写入所有分支：新建池直接继承，
            // 已有池每帧跟随快照；开关切换不走资源重建。
            if (emitterIt == emitters.end() ||
                emitterIt->second->asset.maxParticles !=
                    emitter.maxParticles ||
                emitterIt->second->asset.sortParticles !=
                    emitter.sortParticles ||
                emitterIt->second->asset.materialInstancePath !=
                    emitter.materialInstancePath ||
                ParticleCollisionResourceIdentityChanged(
                    emitterIt->second->asset,
                    emitter) ||
                // GPU light 输出 buffer 在本帧才可用时，需要重建描述符包；
                // 不能留下一个只有 simulate set、没有 emit set 的发射器。
                (emitter.renderMode == "light" &&
                 particleLightBuffers.size() >= swapchainImageCount &&
                 (emitterIt->second->lightEmitSets.size() <
                      swapchainImageCount ||
                  emitterIt->second->lightBufferGeneration !=
                      particleLightBufferGeneration)) ||
                // 场景灯/IBL descriptor 换代后所有图形发射器都要重建 set 0，
                // 否则会继续引用已退休的 buffer/cubemap。
                emitterIt->second->sceneLightingGeneration !=
                    sceneLightingGeneration ||
                emitterIt->second->textureSourceKey !=
                    textureSourceKey ||
                emitterIt->second->meshSourceKey !=
                    meshSourceKey ||
                emitterIt->second->indirectShape != indirectShape)
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
                const auto created = emitters.find(key);
                if (created != emitters.end() && telemetry != nullptr)
                {
                    created->second->restartEpoch =
                        telemetry->restartEpoch;
                }
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
            // World stable slot 的 restartEpoch 变化只重置该发射器的模拟
            // 状态；池/纹理/描述符保持原样，不重建 GPU 资源。
            if (telemetry != nullptr &&
                emitterIt->second->restartEpoch !=
                    telemetry->restartEpoch)
            {
                emitterIt->second->elapsedSeconds = 0.0f;
                emitterIt->second->fixedStepAccumulator = 0.0f;
                emitterIt->second->fixedStepIndex = 0;
                emitterIt->second->prewarm = true;
                emitterIt->second->needsClear = true;
                emitterIt->second->culled = false;
                emitterIt->second->restartEpoch =
                    telemetry->restartEpoch;
            }
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
    AssignParticleLightSlots();
    // 发射器集合稳定后再决定全局透明排序池的成员/纹理槽位；签名不变时
    // 直接复用，不在每帧创建 GPU 资源。
    SynchronizeGlobalSort();
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
        emitter.fixedStepAccumulator = 0.0f;
        emitter.fixedStepIndex = 0;
        emitter.prewarm = true;
        emitter.needsClear = true;
        emitter.culled = false;
    }
}

void ParticleRenderer::SetShadowCascadeData(
    const ParticleShadowCascadeData& data)
{
    shadowCascadeData = data;
}

bool ParticleRenderer::DrainParticleEvents(
    RendererBackendVulkan& readbackBackend,
    uint32_t maxPerEmitter,
    std::vector<ParticleEventBatch>& outBatches,
    std::string& outError)
{
    outBatches.clear();
    if (!initialized || backend != &readbackBackend)
    {
        outError =
            "Particle renderer is not initialized for this backend.";
        return false;
    }
    if (emitters.empty())
    {
        outError = "No particle emitters are available for event drain.";
        return false;
    }

    maxPerEmitter = std::min(
        maxPerEmitter, kParticleEventCapacity);
    const vk::DeviceSize counterBytes = sizeof(ParticleEventCounters);
    const vk::DeviceSize recordBytes =
        maxPerEmitter > 0
        ? sizeof(ParticleEventRecord) *
            static_cast<vk::DeviceSize>(maxPerEmitter)
        : sizeof(ParticleEventRecord);

    for (const auto& [key, emitterPtr] : emitters)
    {
        const EmitterResources& emitter = *emitterPtr;
        // 冻结 emitter 的计数在 RecordSimulation 中已清零；这里直接跳过，
        // 避免 cull 边界上的旧记录被重复派发。
        if (emitter.culled)
        {
            continue;
        }
        if (!emitter.counters.buffer || !emitter.events.buffer)
        {
            outError = "Particle emitter has no event buffers: " + key;
            return false;
        }

        vk::Buffer counterReadback;
        vk::DeviceMemory counterMemory;
        vk::Buffer eventReadback;
        vk::DeviceMemory eventMemory;
        try
        {
            std::tie(counterReadback, counterMemory) =
                readbackBackend.CreateBuffer(
                    counterBytes,
                    vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleEventCountersReadback:" + key);
            std::tie(eventReadback, eventMemory) =
                readbackBackend.CreateBuffer(
                    recordBytes,
                    vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleEventsReadback:" + key);

            vk::CommandBuffer commandBuffer =
                readbackBackend.BeginSingleTimeCommands();
            vk::BufferMemoryBarrier toTransfer;
            toTransfer
                .setSrcAccessMask(
                    vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            toTransfer.setBuffer(emitter.counters.buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                emitter.counters.buffer,
                counterReadback,
                vk::BufferCopy(0, 0, counterBytes));

            toTransfer.setBuffer(emitter.events.buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                emitter.events.buffer,
                eventReadback,
                vk::BufferCopy(0, 0, recordBytes));
            readbackBackend.EndSingleTimeCommands(commandBuffer);

            const ParticleEventCounters* counters =
                static_cast<const ParticleEventCounters*>(
                    readbackBackend.MapMemory(counterMemory, counterBytes));
            const ParticleEventRecord* records =
                static_cast<const ParticleEventRecord*>(
                    readbackBackend.MapMemory(eventMemory, recordBytes));
            if (counters == nullptr || records == nullptr)
            {
                readbackBackend.UnmapMemory(counterMemory);
                readbackBackend.UnmapMemory(eventMemory);
                readbackBackend.DestroyBuffer(
                    counterReadback, counterMemory);
                readbackBackend.DestroyBuffer(
                    eventReadback, eventMemory);
                outError = "Particle event readback map failed: " + key;
                return false;
            }

            ParticleEventBatch batch;
            batch.emitterKey = key;
            batch.counters = *counters;
            const uint32_t recordCount = std::min(
                std::min(maxPerEmitter, kParticleEventCapacity),
                counters->events);
            batch.records.assign(records, records + recordCount);
            outBatches.push_back(std::move(batch));

            readbackBackend.UnmapMemory(counterMemory);
            readbackBackend.UnmapMemory(eventMemory);
            readbackBackend.DestroyBuffer(
                counterReadback, counterMemory);
            readbackBackend.DestroyBuffer(
                eventReadback, eventMemory);
        }
        catch (const std::exception& exception)
        {
            outError =
                "Particle event drain failed for '" + key +
                "': " + exception.what();
            return false;
        }
    }
    return true;
}

void ParticleRenderer::DestroyGlobalSortPool()
{
    // 全局池可能在上一帧仍被 GPU 引用；重建/销毁只发生在发射器集合或
    // 纹理变化时，用一次 WaitIdle 换取安全，而不是每帧等待。
    if (backend != nullptr && globalSortPool.active)
    {
        backend->WaitIdle();
    }
    if (backend != nullptr)
    {
        if (globalSortPool.descriptorPool)
        {
            backend->DestroyDescriptorPool(
                globalSortPool.descriptorPool);
        }
        for (GpuBuffer& buffer : globalSortPool.records)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.pairsA)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.pairsB)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.counters)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.indirectArgs)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.debugCounters)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& uniform : globalSortPool.uniforms)
        {
            DestroyBuffer(*backend, uniform);
        }
        globalSortPool.gpuTimer.Shutdown(*backend);
    }
    globalSortPool = GlobalSortPool{};
}

bool ParticleRenderer::IsGlobalSortEmitter(
    const std::string& key) const
{
    if (!globalSortPool.active)
    {
        return false;
    }
    return std::find(
        globalSortPool.emitterKeys.begin(),
        globalSortPool.emitterKeys.end(),
        key) != globalSortPool.emitterKeys.end();
}

void ParticleRenderer::AssignParticleLightSlots()
{
    std::vector<std::pair<std::string, uint32_t>> requests;
    requests.reserve(emitters.size());
    for (const auto& [key, emitterPtr] : emitters)
    {
        if (emitterPtr->asset.renderMode == "light")
        {
            requests.emplace_back(
                key,
                std::min(
                    emitterPtr->asset.maxParticles,
                    kMaxParticleLights));
        }
    }
    const std::vector<ParticleLightSlot> slots =
        AllocateParticleLightSlots(std::move(requests));
    for (const ParticleLightSlot& slot : slots)
    {
        const auto emitter = emitters.find(slot.key);
        if (emitter == emitters.end())
        {
            continue;
        }
        emitter->second->lightSlotBase = slot.base;
        emitter->second->lightSlotCapacity = slot.capacity;
    }
}

void ParticleRenderer::SynchronizeGlobalSort()
{
    if (!initialized || backend == nullptr)
    {
        return;
    }

    struct Candidate
    {
        std::string key;
        std::string textureKey;
        uint32_t maxParticles = 0;
    };
    std::vector<Candidate> candidates;
    uint64_t totalCapacity = 0;
    for (const auto& [key, emitterPtr] : emitters)
    {
        const EmitterResources& emitter = *emitterPtr;
        if (!emitter.visible || !emitter.enabled)
        {
            continue;
        }
        // v1 全局排序只接纳 alpha billboard；ribbon/mesh 保留逐发射器
        // 排序（它们的顶点生成与材质路径不同）。
        if (emitter.asset.renderMode != "billboard" ||
            !emitter.asset.sortParticles ||
            emitter.asset.additive ||
            // 全局 gather 记录不携带逐 emitter 阴影开关；需要 CSM 的
            // billboard 留在逐 emitter 路径，避免静默丢失作者设置。
            emitter.asset.billboardShadows)
        {
            continue;
        }
        totalCapacity += emitter.asset.maxParticles;
        if (totalCapacity > kParticleGlobalSortCapacity)
        {
            DestroyGlobalSortPool();
            return;
        }
        candidates.push_back(Candidate{
            key, emitter.textureSourceKey, emitter.asset.maxParticles});
    }
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& left, const Candidate& right)
        {
            return left.key < right.key;
        });

    std::vector<std::string> emitterKeys;
    std::vector<uint32_t> textureSlots;
    std::vector<std::shared_ptr<Texture>> textures;
    std::unordered_map<std::string, uint32_t> slotByTexture;
    std::string signature;
    for (const Candidate& candidate : candidates)
    {
        const auto slot = slotByTexture.find(candidate.textureKey);
        uint32_t textureSlot = 0;
        if (slot == slotByTexture.end())
        {
            if (textures.size() >= kParticleGlobalTextureSlots)
            {
                DestroyGlobalSortPool();
                return;
            }
            textureSlot = static_cast<uint32_t>(textures.size());
            slotByTexture.emplace(candidate.textureKey, textureSlot);
            const auto emitter = emitters.find(candidate.key);
            textures.push_back(
                emitter != emitters.end() && emitter->second->texture
                ? emitter->second->texture
                : defaultTexture);
        }
        else
        {
            textureSlot = slot->second;
        }
        emitterKeys.push_back(candidate.key);
        textureSlots.push_back(textureSlot);
        signature += candidate.key + "#" + candidate.textureKey +
            "#" + std::to_string(candidate.maxParticles) + ";";
    }
    // 场景灯/IBL descriptor 换代也必须重建全局 draw set；否则全局池会
    // 继续引用旧 light SSBO 或旧 prefiltered cubemap。
    signature += "lighting#" +
        std::to_string(sceneLightingGeneration) + ";";

    if (emitterKeys.empty())
    {
        DestroyGlobalSortPool();
        return;
    }
    // 失败签名也缓存，避免每帧重复创建/告警；发射器集合或纹理变化后
    // 签名不同才会重试。
    if (globalSortPool.signature == signature)
    {
        return;
    }

    const uint32_t sortCapacity =
        GlobalSortCapacity(totalCapacity);
    if (!CreateGlobalSortPool(
            emitterKeys,
            textureSlots,
            textures,
            sortCapacity))
    {
        globalSortPool.signature = signature;
        const std::string message =
            "Global particle sort disabled: pool creation failed";
        ProcessLog().Append(
            LogSeverity::Warning, "Particles", message);
        std::cout << "[Particles][Warning] " << message << std::endl;
        DestroyGlobalSortPool();
        globalSortPool.signature = signature;
        return;
    }
    globalSortPool.signature = signature;
    const std::string message =
        "Global particle sort active: emitters=" +
        std::to_string(emitterKeys.size()) +
        " capacity=" + std::to_string(sortCapacity) +
        " textures=" + std::to_string(textures.size());
    ProcessLog().Append(LogSeverity::Info, "Particles", message);
    std::cout << "[Particles][Info] " << message << std::endl;
}

bool ParticleRenderer::CreateGlobalSortPool(
    const std::vector<std::string>& emitterKeys,
    const std::vector<uint32_t>& textureSlots,
    const std::vector<std::shared_ptr<Texture>>& textures,
    uint32_t capacity)
{
    DestroyGlobalSortPool();
    if (backend == nullptr || emitterKeys.empty() ||
        emitterKeys.size() != textureSlots.size() ||
        !globalGatherPipeline || !globalSortInitPipeline ||
        !globalDrawPipeline || !sortPipeline ||
        !finalizeCountPipeline)
    {
        std::cout << "[Particles][Warning] Global particle sort "
            "precondition failed: pipelines="
                  << (globalGatherPipeline ? 1 : 0)
                  << (globalSortInitPipeline ? 1 : 0)
                  << (globalDrawPipeline ? 1 : 0)
                  << (sortPipeline ? 1 : 0)
                  << (finalizeCountPipeline ? 1 : 0)
                  << std::endl;
        return false;
    }

    const uint32_t imageCount = backend->GetSwapchainImageCount();
    const uint32_t emitterCount =
        static_cast<uint32_t>(emitterKeys.size());
    const vk::DeviceSize recordBytes =
        static_cast<vk::DeviceSize>(capacity) *
        kParticleGlobalRecordVec4Count * sizeof(Eigen::Vector4f);
    const vk::DeviceSize pairBytes =
        static_cast<vk::DeviceSize>(capacity) * sizeof(Eigen::Vector4f);

    globalSortPool.capacity = capacity;
    globalSortPool.emitterCount = emitterCount;
    globalSortPool.emitterKeys = emitterKeys;
    globalSortPool.textures = textures;
    for (size_t index = 0; index < emitterKeys.size(); ++index)
    {
        globalSortPool.textureSlotByKey.emplace(
            emitterKeys[index], textureSlots[index]);
    }

    const auto createStorage = [this](
        vk::DeviceSize bytes,
        const std::string& name)
    {
        return backend->CreateBuffer(
            bytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            name);
    };
    globalSortPool.records.resize(imageCount);
    globalSortPool.pairsA.resize(imageCount);
    globalSortPool.pairsB.resize(imageCount);
    globalSortPool.counters.resize(imageCount);
    globalSortPool.indirectArgs.resize(imageCount);
    globalSortPool.debugCounters.resize(imageCount);
    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        const std::string suffix = ":" + std::to_string(imageIndex);
        std::tie(globalSortPool.records[imageIndex].buffer,
            globalSortPool.records[imageIndex].memory) =
            createStorage(recordBytes, "ParticleGlobalRecords" + suffix);
        std::tie(globalSortPool.pairsA[imageIndex].buffer,
            globalSortPool.pairsA[imageIndex].memory) =
            createStorage(pairBytes, "ParticleGlobalPairsA" + suffix);
        std::tie(globalSortPool.pairsB[imageIndex].buffer,
            globalSortPool.pairsB[imageIndex].memory) =
            createStorage(pairBytes, "ParticleGlobalPairsB" + suffix);

        const auto counterAllocation = backend->CreateBuffer(
            sizeof(ParticleEventCounters),
            vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferDst |
                vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleGlobalCounters" + suffix);
        globalSortPool.counters[imageIndex].buffer =
            counterAllocation.first;
        globalSortPool.counters[imageIndex].memory =
            counterAllocation.second;
        void* mappedCounters = backend->MapMemory(
            globalSortPool.counters[imageIndex].memory,
            sizeof(ParticleEventCounters));
        if (mappedCounters == nullptr)
        {
            return false;
        }
        ParticleEventCounters initialCounters;
        initialCounters.capacity = capacity;
        std::memcpy(
            mappedCounters, &initialCounters, sizeof(initialCounters));
        backend->UnmapMemory(globalSortPool.counters[imageIndex].memory);

        const auto indirectAllocation = backend->CreateBuffer(
            sizeof(vk::DrawIndirectCommand),
            vk::BufferUsageFlagBits::eIndirectBuffer |
                vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleGlobalIndirect" + suffix);
        globalSortPool.indirectArgs[imageIndex].buffer =
            indirectAllocation.first;
        globalSortPool.indirectArgs[imageIndex].memory =
            indirectAllocation.second;
        void* mappedIndirect = backend->MapMemory(
            globalSortPool.indirectArgs[imageIndex].memory,
            sizeof(vk::DrawIndirectCommand));
        if (mappedIndirect == nullptr)
        {
            return false;
        }
        const vk::DrawIndirectCommand command(6u, capacity, 0u, 0u);
        std::memcpy(mappedIndirect, &command, sizeof(command));
        backend->UnmapMemory(
            globalSortPool.indirectArgs[imageIndex].memory);

        const auto debugCounterAllocation = backend->CreateBuffer(
            sizeof(ParticleOverdrawCounters),
            vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferDst |
                vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            "ParticleGlobalOverdraw" + suffix);
        globalSortPool.debugCounters[imageIndex].buffer =
            debugCounterAllocation.first;
        globalSortPool.debugCounters[imageIndex].memory =
            debugCounterAllocation.second;
    }
    globalSortPool.gpuTimer.Initialize(*backend);

    globalSortPool.uniforms.resize(imageCount);
    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        const auto uniformAllocation = backend->CreateBuffer(
            sizeof(FrameUniforms),
            vk::BufferUsageFlagBits::eUniformBuffer |
                vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleGlobalUniforms:" + std::to_string(imageIndex));
        globalSortPool.uniforms[imageIndex].buffer =
            uniformAllocation.first;
        globalSortPool.uniforms[imageIndex].memory =
            uniformAllocation.second;
        globalSortPool.uniforms[imageIndex].mapped =
            backend->MapMemory(
                uniformAllocation.second, sizeof(FrameUniforms));
        if (globalSortPool.uniforms[imageIndex].mapped == nullptr)
        {
            return false;
        }
    }

    const uint32_t sortPassCount =
        ParticleSortDispatchCount(capacity);
    globalSortPool.finalUsesA = (sortPassCount % 2u) == 0u;

    const std::vector<vk::DescriptorSetLayout> gatherLayouts(
        imageCount * emitterCount,
        globalGatherPipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> sortInitLayouts(
        imageCount,
        globalSortInitPipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> sortLayouts(
        imageCount,
        sortPipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> finalizeLayouts(
        imageCount,
        finalizeCountPipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> drawLayouts(
        imageCount,
        globalDrawPipeline->GetDescriptorSetLayouts().at(0));

    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer,
            imageCount * (emitterCount + 5)},
        {vk::DescriptorType::eStorageBuffer,
            imageCount * (emitterCount * 3u + 14u)},
        {vk::DescriptorType::eCombinedImageSampler,
            imageCount * (kParticleGlobalTextureSlots + 2u)}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount * (emitterCount + 5));
    globalSortPool.descriptorPool = backend->CreateDescriptorPool(
        poolInfo, "ParticleGlobalDescriptors");

    try
    {
        vk::DescriptorSetAllocateInfo allocate;
        allocate.setDescriptorPool(globalSortPool.descriptorPool);
        allocate.setSetLayouts(gatherLayouts);
        globalSortPool.gatherSets.resize(imageCount * emitterCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.gatherSets);

        allocate.setSetLayouts(sortInitLayouts);
        globalSortPool.sortInitSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.sortInitSets);
        allocate.setSetLayouts(sortLayouts);
        globalSortPool.sortAtoBSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.sortAtoBSets);
        globalSortPool.sortBtoASets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.sortBtoASets);
        allocate.setSetLayouts(finalizeLayouts);
        globalSortPool.finalizeSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.finalizeSets);
        allocate.setSetLayouts(drawLayouts);
        globalSortPool.drawSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.drawSets);

        for (uint32_t imageIndex = 0;
             imageIndex < imageCount;
             ++imageIndex)
        {
            const vk::DescriptorBufferInfo recordInfo(
                globalSortPool.records[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo pairAInfo(
                globalSortPool.pairsA[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo pairBInfo(
                globalSortPool.pairsB[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo counterInfo(
                globalSortPool.counters[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo indirectInfo(
                globalSortPool.indirectArgs[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo debugCounterInfo(
                globalSortPool.debugCounters[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo uniformInfo(
                globalSortPool.uniforms[imageIndex].buffer,
                0,
                sizeof(FrameUniforms));
            for (uint32_t emitterIndex = 0;
                 emitterIndex < emitterCount;
                 ++emitterIndex)
            {
                const auto emitter = emitters.find(
                    emitterKeys[emitterIndex]);
                if (emitter == emitters.end() ||
                    emitter->second->frameUniforms.size() <= imageIndex)
                {
                    throw std::runtime_error(
                        "Global particle gather lost an emitter");
                }
                const vk::DescriptorBufferInfo emitterUniform(
                    emitter->second->frameUniforms
                        .at(imageIndex).buffer,
                    0,
                    sizeof(FrameUniforms));
                const vk::DescriptorBufferInfo particleInfo(
                    emitter->second->particles.buffer,
                    0,
                    VK_WHOLE_SIZE);
                vk::WriteDescriptorSet uniformWrite;
                uniformWrite
                    .setDstSet(globalSortPool.gatherSets.at(
                        imageIndex * emitterCount + emitterIndex))
                    .setDstBinding(0)
                    .setDescriptorCount(1)
                    .setDescriptorType(
                        vk::DescriptorType::eUniformBuffer)
                    .setBufferInfo(emitterUniform);
                vk::WriteDescriptorSet particleWrite = uniformWrite;
                particleWrite
                    .setDstBinding(1)
                    .setDescriptorType(
                        vk::DescriptorType::eStorageBuffer)
                    .setBufferInfo(particleInfo);
                vk::WriteDescriptorSet recordWrite = particleWrite;
                recordWrite
                    .setDstBinding(2)
                    .setBufferInfo(recordInfo);
                vk::WriteDescriptorSet counterWrite = particleWrite;
                counterWrite
                    .setDstBinding(3)
                    .setBufferInfo(counterInfo);
                backend->UpdateDescriptorSets(
                    {uniformWrite,
                     particleWrite,
                     recordWrite,
                     counterWrite});
            }

            vk::WriteDescriptorSet uniformWrite;
            uniformWrite
                .setDstSet(globalSortPool.sortInitSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            vk::WriteDescriptorSet recordWrite = uniformWrite;
            recordWrite
                .setDstBinding(1)
                .setDescriptorType(
                    vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(recordInfo);
            vk::WriteDescriptorSet pairAWrite = recordWrite;
            pairAWrite
                .setDstBinding(2)
                .setBufferInfo(pairAInfo);
            vk::WriteDescriptorSet counterWrite = recordWrite;
            counterWrite
                .setDstBinding(3)
                .setBufferInfo(counterInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite, recordWrite, pairAWrite, counterWrite});

            uniformWrite.setDstSet(
                globalSortPool.sortAtoBSets.at(imageIndex));
            vk::WriteDescriptorSet inA = pairAWrite;
            inA.setDstSet(globalSortPool.sortAtoBSets.at(imageIndex))
                .setDstBinding(1)
                .setBufferInfo(pairAInfo);
            vk::WriteDescriptorSet outB = pairAWrite;
            outB.setDstSet(globalSortPool.sortAtoBSets.at(imageIndex))
                .setDstBinding(2)
                .setBufferInfo(pairBInfo);
            backend->UpdateDescriptorSets({uniformWrite, inA, outB});

            uniformWrite.setDstSet(
                globalSortPool.sortBtoASets.at(imageIndex));
            vk::WriteDescriptorSet inB = inA;
            inB.setDstSet(globalSortPool.sortBtoASets.at(imageIndex))
                .setBufferInfo(pairBInfo);
            vk::WriteDescriptorSet outA = outB;
            outA.setDstSet(globalSortPool.sortBtoASets.at(imageIndex))
                .setBufferInfo(pairAInfo);
            backend->UpdateDescriptorSets({uniformWrite, inB, outA});

            uniformWrite.setDstSet(
                globalSortPool.finalizeSets.at(imageIndex));
            vk::WriteDescriptorSet finalizeCounter = counterWrite;
            finalizeCounter.setDstSet(
                    globalSortPool.finalizeSets.at(imageIndex))
                .setDstBinding(0)
                .setBufferInfo(counterInfo);
            vk::WriteDescriptorSet finalizeArgs = counterWrite;
            finalizeArgs.setDstSet(
                    globalSortPool.finalizeSets.at(imageIndex))
                .setDstBinding(1)
                .setBufferInfo(indirectInfo);
            backend->UpdateDescriptorSets(
                {finalizeCounter, finalizeArgs});

            // draw：UBO + records + 排序 pair + 8 槽纹理数组。
            vk::WriteDescriptorSet drawUniform;
            drawUniform
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            vk::WriteDescriptorSet drawRecords = recordWrite;
            drawRecords.setDstSet(
                    globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(1)
                .setBufferInfo(recordInfo);
            vk::WriteDescriptorSet drawPairs = pairAWrite;
            drawPairs.setDstSet(
                    globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(2)
                .setBufferInfo(globalSortPool.finalUsesA
                    ? pairAInfo
                    : pairBInfo);
            std::array<vk::DescriptorImageInfo, kParticleGlobalTextureSlots>
                textureInfos;
            for (uint32_t slotIndex = 0;
                 slotIndex < kParticleGlobalTextureSlots;
                 ++slotIndex)
            {
                const std::shared_ptr<Texture>& texture =
                    slotIndex < textures.size()
                    ? textures[slotIndex]
                    : defaultTexture;
                textureInfos[slotIndex] =
                    texture->GetDescriptorInfo();
            }
            vk::WriteDescriptorSet drawTextures;
            drawTextures
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(3)
                .setDescriptorCount(kParticleGlobalTextureSlots)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(textureInfos);
            vk::WriteDescriptorSet drawDebug;
            drawDebug
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(5)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(debugCounterInfo);
            // 全局 alpha 池与逐发射器 billboard 使用同一组场景光照绑定；
            // 全局记录只携带 billboardLightParams，IBL/灯光本身来自这里。
            vk::WriteDescriptorSet drawSceneLight;
            drawSceneLight
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(6)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(SceneLightInfoForImage(imageIndex));
            vk::WriteDescriptorSet drawParticleLight;
            drawParticleLight
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(7)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(ParticleLightInfoForImage(imageIndex));
            vk::WriteDescriptorSet drawEnvironment;
            drawEnvironment
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(8)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(EnvironmentCubeDescriptor());
            vk::WriteDescriptorSet drawBrdfLut;
            drawBrdfLut
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(9)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(BrdfLutDescriptor());
            backend->UpdateDescriptorSets(
                {drawUniform,
                 drawRecords,
                 drawPairs,
                 drawTextures,
                 drawDebug,
                 drawSceneLight,
                 drawParticleLight,
                 drawEnvironment,
                 drawBrdfLut});
        }
    }
    catch (const std::exception& exception)
    {
        std::cout << "[Particles][Warning] Global particle sort pool "
            "creation threw: " << exception.what() << std::endl;
        DestroyGlobalSortPool();
        return false;
    }
    catch (...)
    {
        std::cout << "[Particles][Warning] Global particle sort pool "
            "creation threw an unknown exception" << std::endl;
        DestroyGlobalSortPool();
        return false;
    }
    globalSortPool.active = true;
    return true;
}

void ParticleRenderer::RecordGlobalGather(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex)
{
    if (!globalSortPool.active || backend == nullptr)
    {
        return;
    }
    const uint32_t imageCount = backend->GetSwapchainImageCount();
    if (globalSortPool.gatherSets.size() <
        imageCount * globalSortPool.emitterCount)
    {
        return;
    }

    vk::MemoryBarrier previousToClear;
    previousToClear
        .setSrcAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        previousToClear,
        nullptr,
        nullptr);
    commandBuffer.fillBuffer(
        globalSortPool.counters[imageIndex].buffer,
        0,
        sizeof(uint32_t) * 5u,
        0);
    commandBuffer.fillBuffer(
        globalSortPool.debugCounters[imageIndex].buffer,
        0,
        sizeof(ParticleOverdrawCounters),
        0);
    vk::BufferMemoryBarrier clearToCompute;
    clearToCompute
        .setBuffer(globalSortPool.counters[imageIndex].buffer)
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
    vk::BufferMemoryBarrier debugClearToGraphics;
    debugClearToGraphics
        .setBuffer(globalSortPool.debugCounters[imageIndex].buffer)
        .setOffset(0)
        .setSize(VK_WHOLE_SIZE)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eFragmentShader,
        {},
        nullptr,
        debugClearToGraphics,
        nullptr);
    globalSortPool.lastImageIndex = imageIndex;
    // simulate 刚写过粒子池；gather 在 compute 阶段读取同一 buffer。
    vk::MemoryBarrier simulateToGather;
    simulateToGather
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        simulateToGather,
        nullptr,
        nullptr);

    globalGatherPipeline->Bind(commandBuffer);
    globalSortPool.gatheredEmitters = 0;
    for (uint32_t emitterIndex = 0;
         emitterIndex < globalSortPool.emitterCount;
         ++emitterIndex)
    {
        const auto emitter = emitters.find(
            globalSortPool.emitterKeys[emitterIndex]);
        if (emitter == emitters.end() ||
            emitter->second->culled ||
            emitter->second->frameUniforms.size() <= imageIndex)
        {
            continue;
        }
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            globalGatherPipeline->GetPipelineLayout(),
            0,
            globalSortPool.gatherSets.at(
                imageIndex * globalSortPool.emitterCount + emitterIndex),
            nullptr);
        const uint32_t groupCount =
            (emitter->second->asset.maxParticles + 255u) / 256u;
        globalGatherPipeline->Dispatch(
            commandBuffer, groupCount, 1, 1);
        ++globalSortPool.gatheredEmitters;
    }
}

void ParticleRenderer::RecordGlobalSort(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const RenderScene& renderScene)
{
    if (!globalSortPool.active ||
        globalSortPool.uniforms.size() <= imageIndex)
    {
        return;
    }
    if (globalSortPool.gatheredEmitters > 0)
    {
        frameStats.sortedEmitters +=
            globalSortPool.gatheredEmitters;
        frameStats.sortCapacityTotal += globalSortPool.capacity;
        frameStats.sortDispatchCount +=
            ParticleSortDispatchCount(globalSortPool.capacity);
    }

    // 全局 UBO：排序与绘制只消费相机矩阵；per-emitter 数据已经烘焙进
    // gathered 记录。
    FrameUniforms uniforms{};
    uniforms.viewProjection = renderScene.camera.viewProjection;
    uniforms.view = renderScene.camera.view;
    uniforms.invProjection = renderScene.camera.projection.inverse();
    uniforms.cameraPosition = Eigen::Vector4f(
        renderScene.camera.position.x(),
        renderScene.camera.position.y(),
        renderScene.camera.position.z(),
        0.0f);
    uniforms.sortParams = Eigen::Vector4f(
        1.0f,
        static_cast<float>(globalSortPool.capacity),
        static_cast<float>(globalSortPool.capacity),
        0.0f);
    std::memcpy(
        globalSortPool.uniforms.at(imageIndex).mapped,
        &uniforms,
        sizeof(uniforms));

    vk::MemoryBarrier recordWriteToRead;
    recordWriteToRead
        .setSrcAccessMask(
            vk::AccessFlagBits::eShaderWrite |
            vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        recordWriteToRead,
        nullptr,
        nullptr);

    const uint32_t groupCount =
        (globalSortPool.capacity + 255u) / 256u;
    globalSortInitPipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        globalSortInitPipeline->GetPipelineLayout(),
        0,
        globalSortPool.sortInitSets.at(imageIndex),
        nullptr);
    globalSortInitPipeline->Dispatch(
        commandBuffer, groupCount, 1, 1);

    bool sourceIsA = true;
    for (uint32_t k = 2;
         k <= globalSortPool.capacity;
         k <<= 1)
    {
        for (uint32_t j = k >> 1; j > 0; j >>= 1)
        {
            InsertBufferBarrier(
                commandBuffer,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderRead,
                vk::PipelineStageFlagBits::eTransfer,
                vk::AccessFlagBits::eTransferWrite);
            const Eigen::Vector4f sortPass(
                static_cast<float>(k),
                static_cast<float>(j),
                static_cast<float>(globalSortPool.capacity),
                0.0f);
            commandBuffer.updateBuffer(
                globalSortPool.uniforms.at(imageIndex).buffer,
                offsetof(FrameUniforms, sortPass),
                sizeof(sortPass),
                &sortPass);
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
                    ? globalSortPool.sortAtoBSets.at(imageIndex)
                    : globalSortPool.sortBtoASets.at(imageIndex),
                nullptr);
            sortPipeline->Dispatch(
                commandBuffer, groupCount, 1, 1);
            sourceIsA = !sourceIsA;
        }
    }

    InsertBufferBarrier(
        commandBuffer,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::AccessFlagBits::eShaderWrite,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    finalizeCountPipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        finalizeCountPipeline->GetPipelineLayout(),
        0,
        globalSortPool.finalizeSets.at(imageIndex),
        nullptr);
    finalizeCountPipeline->Dispatch(commandBuffer, 1, 1, 1);
}

void ParticleRenderer::RecordGlobalDraw(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    if (!globalSortPool.active || !globalDrawPipeline ||
        globalSortPool.drawSets.size() <= imageIndex ||
        globalSortPool.indirectArgs.size() <= imageIndex ||
        !globalSortPool.indirectArgs[imageIndex].buffer)
    {
        return;
    }
    globalSortPool.gpuTimer.BeginPass(commandBuffer, imageIndex);
    commandBuffer.bindPipeline(
        globalDrawPipeline->GetBindPoint(),
        globalDrawPipeline->GetPipeline());
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        globalDrawPipeline->GetPipelineLayout(),
        0,
        globalSortPool.drawSets.at(imageIndex),
        nullptr);
    bool usesPassSet = false;
    for (const ShaderBinding& binding :
         globalDrawPipeline->GetShaderBindings())
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
            globalDrawPipeline->GetPipelineLayout(),
            PassSetIndex,
            descriptorSets[imageIndex][PassSetIndex],
            nullptr);
    }
    commandBuffer.drawIndirect(
        globalSortPool.indirectArgs[imageIndex].buffer,
        0,
        1,
        sizeof(vk::DrawIndirectCommand));
    globalSortPool.gpuTimer.EndPass(commandBuffer, imageIndex);
    ++frameStats.estimatedDrawCalls;
    frameStats.estimatedDrawnVertices +=
        static_cast<uint64_t>(globalSortPool.capacity) * 6u;
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
    if (asset.renderMode == "mesh" &&
        !asset.materialInstancePath.empty())
    {
        std::string materialError;
        emitter->material = LoadParticleMaterialInstance(
            asset.materialInstancePath, materialError);
        if (!emitter->material.valid)
        {
            WarnMaterialFallbackOnce(
                key,
                asset.materialInstancePath,
                materialError.empty()
                    ? "material instance parse failed"
                    : materialError);
        }
    }
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
                // distortion 是方向/覆盖率数据纹理，不能走 sRGB 解码，
                // 否则作者化的 0.5 中点会变成约 0.214。
                const bool dataTexture =
                    asset.renderMode == "distortion";
                options.semantic = dataTexture
                    ? HostImage::TextureSemantic::Data
                    : HostImage::TextureSemantic::Color;
                options.transfer = dataTexture
                    ? TextureIO::LoadOptions::Transfer::Linear
                    : TextureIO::LoadOptions::Transfer::SRGB;
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
                desc.semantic = options.semantic;
                desc.transfer = options.transfer;
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
                emitter->textureFallback = true;
            }
            catch (...)
            {
                WarnTextureFallbackOnce(
                    key,
                    asset.texturePath,
                    "unknown texture load failure");
                emitter->texture = defaultTexture;
                emitter->textureFallback = true;
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
    emitter->gpuTimer.Initialize(*backend);
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

    // GPU 计数（alive/spawn/death/collision）每帧 fillBuffer 清零。
    const auto counterAllocation = backend->CreateBuffer(
        sizeof(ParticleEventCounters),
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst |
            vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleCounters:" + emitter.key);
    emitter.counters.buffer = counterAllocation.first;
    emitter.counters.memory = counterAllocation.second;
    void* mappedCounters = backend->MapMemory(
        emitter.counters.memory, sizeof(ParticleEventCounters));
    if (mappedCounters == nullptr)
    {
        throw std::runtime_error("particle counter map failed");
    }
    ParticleEventCounters initialCounters;
    initialCounters.capacity = emitter.asset.maxParticles;
    std::memcpy(
        mappedCounters, &initialCounters, sizeof(initialCounters));
    backend->UnmapMemory(emitter.counters.memory);

    // 紧凑存活索引：simulate 原子追加，未排序绘制与 CPU 统计共用。
    const vk::DeviceSize aliveBytes =
        sizeof(uint32_t) *
        static_cast<vk::DeviceSize>(
            std::max(emitter.asset.maxParticles, 1u));
    const auto aliveAllocation = backend->CreateBuffer(
        aliveBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleAliveIndices:" + emitter.key);
    emitter.aliveIndices.buffer = aliveAllocation.first;
    emitter.aliveIndices.memory = aliveAllocation.second;

    // 有界事件缓冲；每帧从 slot 0 覆盖写，计数器超容量表示截断。
    const vk::DeviceSize eventBytes =
        sizeof(ParticleEventRecord) * kParticleEventCapacity;
    const auto eventAllocation = backend->CreateBuffer(
        eventBytes,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleEvents:" + emitter.key);
    emitter.events.buffer = eventAllocation.first;
    emitter.events.memory = eventAllocation.second;

    // 图形 overdraw 计数：只写 uint，但需要 transfer 清除和回读。
    const auto debugCounterAllocation = backend->CreateBuffer(
        sizeof(ParticleOverdrawCounters),
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst |
            vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleOverdraw:" + emitter.key);
    emitter.debugCounters.buffer = debugCounterAllocation.first;
    emitter.debugCounters.memory = debugCounterAllocation.second;

    // SDF 体素上传；无 SDF 时绑定 1 个 float 的占位 buffer，保持
    // descriptor layout 与 shader binding 5 始终有效。
    emitter.sdfVoxelCount = 0;
    const vk::DeviceSize sdfBytes =
        emitter.asset.collisionSdf &&
            !emitter.asset.collisionSdf->Empty()
        ? static_cast<vk::DeviceSize>(
              emitter.asset.collisionSdf->distances.size()) *
              sizeof(float)
        : sizeof(float);
    const auto sdfAllocation = backend->CreateBuffer(
        sdfBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleSdf:" + emitter.key);
    emitter.sdfData.buffer = sdfAllocation.first;
    emitter.sdfData.memory = sdfAllocation.second;
    void* mappedSdf = backend->MapMemory(
        emitter.sdfData.memory, sdfBytes);
    if (mappedSdf == nullptr)
    {
        throw std::runtime_error("particle SDF map failed");
    }
    if (emitter.asset.collisionSdf &&
        !emitter.asset.collisionSdf->Empty())
    {
        emitter.sdf = emitter.asset.collisionSdf;
        emitter.sdfVoxelCount = static_cast<uint32_t>(
            emitter.sdf->distances.size());
        std::memcpy(
            mappedSdf,
            emitter.sdf->distances.data(),
            static_cast<size_t>(sdfBytes));
    }
    else
    {
        const float empty = 0.0f;
        std::memcpy(mappedSdf, &empty, sizeof(float));
    }
    backend->UnmapMemory(emitter.sdfData.memory);

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

    // 间接参数与 emitter 池同生命周期；内容目前是固定顶点/索引与实例数，
    // 但录制路径已经不再关心具体计数，后续 GPU culling 只需更新该缓冲。
    const bool meshUsable =
        !emitter.meshFallbackBillboard &&
        emitter.meshVertices.buffer && emitter.meshIndices.buffer;
    const ParticleIndirectDrawShape indirectShape =
        BuildParticleIndirectDrawShape(
            emitter.asset.renderMode,
            emitter.asset.maxParticles,
            emitter.asset.ribbonSegments,
            meshUsable,
            emitter.meshIndexCount,
            emitter.asset.beam.segments,
            emitter.asset.beam.count,
            emitter.asset.beam.branchCount);
    const vk::DeviceSize indirectBytes = indirectShape.indexed
        ? sizeof(vk::DrawIndexedIndirectCommand)
        : sizeof(vk::DrawIndirectCommand);
    const auto indirectAllocation = backend->CreateBuffer(
        indirectBytes,
        vk::BufferUsageFlagBits::eIndirectBuffer |
            vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleIndirect:" + emitter.key);
    emitter.indirectArgs.buffer = indirectAllocation.first;
    emitter.indirectArgs.memory = indirectAllocation.second;
    void* mappedIndirect = backend->MapMemory(
        emitter.indirectArgs.memory,
        indirectBytes);
    if (mappedIndirect == nullptr)
    {
        throw std::runtime_error("particle indirect parameter map failed");
    }
    if (indirectShape.indexed)
    {
        const vk::DrawIndexedIndirectCommand command(
            indirectShape.indexCount,
            indirectShape.instanceCount,
            0,
            0,
            0);
        std::memcpy(mappedIndirect, &command, sizeof(command));
    }
    else
    {
        const vk::DrawIndirectCommand command(
            indirectShape.vertexCount,
            indirectShape.instanceCount,
            0,
            0);
        std::memcpy(mappedIndirect, &command, sizeof(command));
    }
    backend->UnmapMemory(emitter.indirectArgs.memory);
    emitter.indirectShape = indirectShape;
    emitter.indirectIndexed = indirectShape.indexed;

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
    if (emitter.asset.renderMode == "light")
    {
        CreateLightEmitterDescriptors(emitter);
        return;
    }

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
    const std::vector<vk::DescriptorSetLayout> finalizeLayouts(
        imageCount,
        finalizeCountPipeline->GetDescriptorSetLayouts().at(0));

    // 每个 image：simulate、graphics、sortInit、AtoB、BtoA、finalize 共 6 个 set。
    // storage 计数：simulate 5 + graphics 6 + sort 三组各 2 + finalize 2 = 19。
    // graphics 新增的 2 个 storage 是场景灯/GPU 粒子灯 SSBO（binding 6/7）；
    // combined 新增 2 个是 IBL cubemap 与 BRDF LUT（binding 8/9）。
    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer, imageCount * 5},
        {vk::DescriptorType::eStorageBuffer, imageCount * 19},
        {vk::DescriptorType::eCombinedImageSampler, imageCount * 3}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount * 6);
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

        allocate.setSetLayouts(finalizeLayouts);
        emitter.finalizeSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.finalizeSets);

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
            const vk::DescriptorBufferInfo aliveInfo(
                emitter.aliveIndices.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo counterInfo(
                emitter.counters.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo eventInfo(
                emitter.events.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo sdfInfo(
                emitter.sdfData.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo debugCounterInfo(
                emitter.debugCounters.buffer,
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
            vk::WriteDescriptorSet aliveWrite;
            aliveWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(aliveInfo);
            vk::WriteDescriptorSet counterWrite;
            counterWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(3)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(counterInfo);
            vk::WriteDescriptorSet eventWrite;
            eventWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(4)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(eventInfo);
            vk::WriteDescriptorSet sdfWrite;
            sdfWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(5)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(sdfInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 particleWrite,
                 aliveWrite,
                 counterWrite,
                 eventWrite,
                 sdfWrite});

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
            vk::WriteDescriptorSet aliveGraphicsWrite;
            aliveGraphicsWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(4)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(aliveInfo);
            vk::WriteDescriptorSet debugGraphicsWrite;
            debugGraphicsWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(5)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(debugCounterInfo);
            // 场景灯与 GPU 粒子灯按 image 绑定各自的 SSBO；IBL 是全局资源，
            // 所有 image 共用同一 cubemap/LUT。
            vk::WriteDescriptorSet sceneLightWrite;
            sceneLightWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(6)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(SceneLightInfoForImage(imageIndex));
            vk::WriteDescriptorSet particleLightWrite;
            particleLightWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(7)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(ParticleLightInfoForImage(imageIndex));
            vk::WriteDescriptorSet environmentWrite;
            environmentWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(8)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(EnvironmentCubeDescriptor());
            vk::WriteDescriptorSet brdfLutWrite;
            brdfLutWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(9)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(BrdfLutDescriptor());
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 particleWrite,
                 textureWrite,
                 sortedWrite,
                 aliveGraphicsWrite,
                 debugGraphicsWrite,
                 sceneLightWrite,
                 particleLightWrite,
                 environmentWrite,
                 brdfLutWrite});

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

            // finalize：counters -> indirectArgs[1]（instanceCount）。
            vk::WriteDescriptorSet finalizeCounterWrite;
            finalizeCounterWrite
                .setDstSet(emitter.finalizeSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(counterInfo);
            vk::DescriptorBufferInfo indirectInfo(
                emitter.indirectArgs.buffer,
                0,
                VK_WHOLE_SIZE);
            vk::WriteDescriptorSet finalizeArgsWrite;
            finalizeArgsWrite
                .setDstSet(emitter.finalizeSets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(indirectInfo);
            backend->UpdateDescriptorSets(
                {finalizeCounterWrite, finalizeArgsWrite});
        }
        emitter.sceneLightingGeneration = sceneLightingGeneration;
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

void ParticleRenderer::CreateLightEmitterDescriptors(
    EmitterResources& emitter)
{
    const uint32_t imageCount =
        backend->GetSwapchainImageCount();
    const bool hasLightOutput =
        particleLightBuffers.size() >= imageCount;
    if (!hasLightOutput && !warnedMissingParticleLightBuffer)
    {
        warnedMissingParticleLightBuffer = true;
        const std::string message =
            "Particle light output buffer is unavailable; "
            "renderMode=\"light\" emitters will simulate but emit no lights";
        ProcessLog().Append(
            LogSeverity::Warning,
            "Particles",
            message);
        std::cout << "[Particles][Warning] " << message << std::endl;
    }

    const std::vector<vk::DescriptorSetLayout> computeLayouts(
        imageCount,
        simulatePipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> lightEmitLayouts(
        imageCount,
        particleLightEmitPipeline->GetDescriptorSetLayouts().at(0));

    // simulate 固定 1 UBO + 5 storage；light emit 固定 1 UBO + 2 storage。
    const uint32_t setCountPerImage = hasLightOutput ? 2u : 1u;
    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer,
         imageCount * setCountPerImage},
        {vk::DescriptorType::eStorageBuffer,
         imageCount * (5u + (hasLightOutput ? 2u : 0u))}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount * setCountPerImage);
    emitter.descriptorPool = backend->CreateDescriptorPool(
        poolInfo,
        "ParticleLightDescriptors:" + emitter.key);

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

        if (hasLightOutput)
        {
            allocate.setSetLayouts(lightEmitLayouts);
            emitter.lightEmitSets.resize(imageCount);
            backend->AllocateDescriptorSets(
                allocate,
                emitter.lightEmitSets);
        }

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
            const vk::DescriptorBufferInfo aliveInfo(
                emitter.aliveIndices.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo counterInfo(
                emitter.counters.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo eventInfo(
                emitter.events.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo sdfInfo(
                emitter.sdfData.buffer,
                0,
                VK_WHOLE_SIZE);

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
            vk::WriteDescriptorSet aliveWrite;
            aliveWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(aliveInfo);
            vk::WriteDescriptorSet counterWrite;
            counterWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(3)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(counterInfo);
            vk::WriteDescriptorSet eventWrite;
            eventWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(4)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(eventInfo);
            vk::WriteDescriptorSet sdfWrite;
            sdfWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(5)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(sdfInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 particleWrite,
                 aliveWrite,
                 counterWrite,
                 eventWrite,
                 sdfWrite});

            if (!hasLightOutput)
            {
                continue;
            }
            vk::DescriptorBufferInfo lightOutputInfo(
                particleLightBuffers.at(imageIndex),
                0,
                VK_WHOLE_SIZE);
            vk::WriteDescriptorSet lightUniformWrite;
            lightUniformWrite
                .setDstSet(emitter.lightEmitSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            vk::WriteDescriptorSet lightParticleWrite;
            lightParticleWrite
                .setDstSet(emitter.lightEmitSets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(particleInfo);
            vk::WriteDescriptorSet lightOutputWrite;
            lightOutputWrite
                .setDstSet(emitter.lightEmitSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(lightOutputInfo);
            backend->UpdateDescriptorSets(
                {lightUniformWrite,
                 lightParticleWrite,
                 lightOutputWrite});
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
    emitter.lightBufferGeneration =
        particleLightBufferGeneration;
    emitter.sceneLightingGeneration = sceneLightingGeneration;
}

void ParticleRenderer::DestroyEmitterResources(
    RendererBackendVulkan& rendererBackend,
    EmitterResources& emitter)
{
    emitter.gpuTimer.Shutdown(rendererBackend);
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
    emitter.finalizeSets.clear();
    emitter.lightEmitSets.clear();
    emitter.texture.reset();
    DestroyBuffer(rendererBackend, emitter.meshVertices);
    DestroyBuffer(rendererBackend, emitter.meshIndices);
    DestroyBuffer(rendererBackend, emitter.particles);
    DestroyBuffer(rendererBackend, emitter.counters);
    DestroyBuffer(rendererBackend, emitter.aliveIndices);
    DestroyBuffer(rendererBackend, emitter.events);
    DestroyBuffer(rendererBackend, emitter.debugCounters);
    DestroyBuffer(rendererBackend, emitter.sdfData);
    DestroyBuffer(rendererBackend, emitter.sortPairsA);
    DestroyBuffer(rendererBackend, emitter.sortPairsB);
    DestroyBuffer(rendererBackend, emitter.indirectArgs);
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

void ParticleRenderer::WarnMaterialFallbackOnce(
    const std::string& key,
    const std::string& materialPath,
    const std::string& error)
{
    if (!warnedMaterialFallbacks.insert(key + "|" + materialPath).second)
    {
        return;
    }
    const std::string message =
        "Particle material fallback for emitter '" + key +
        "' (" + materialPath + "): " + error +
        "; using built-in default PBR factors";
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

void ParticleRenderer::ClearParticleRuntimeCounters(
    vk::CommandBuffer commandBuffer,
    EmitterResources& emitter)
{
    if (!emitter.counters.buffer && !emitter.debugCounters.buffer)
    {
        return;
    }
    // 上一轮 compute 写 alive/事件，fragment 写 overdraw；都必须在 transfer
    // 清零之前完成（即使调用方已做提交级串行化，这里仍保留显式依赖）。
    vk::MemoryBarrier toTransfer;
    toTransfer
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        toTransfer,
        nullptr,
        nullptr);
    if (emitter.counters.buffer)
    {
        commandBuffer.fillBuffer(
            emitter.counters.buffer,
            0,
            sizeof(uint32_t) * 5u,
            0);
    }
    if (emitter.debugCounters.buffer)
    {
        commandBuffer.fillBuffer(
            emitter.debugCounters.buffer,
            0,
            sizeof(ParticleOverdrawCounters),
            0);
    }
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
            emitter.culled = true;
            ClearParticleRuntimeCounters(commandBuffer, emitter);
            ++frameStats.culledEmitters;
            continue;
        }
        // 每 image 每帧回收上一轮 query，并重置本轮时间戳。即使随后被
        // 剔除，也保持计时器状态与帧边界同步。
        emitter.gpuTimer.BeginFrame(commandBuffer, imageIndex);
        // 作者隐藏、发射器禁用与视锥剔除共用"冻结"路径：不推进 elapsed/prewarm，
        // 不清池、不排序、不 dispatch，也不进入 RecordGraphics 的绘制列表。
        if (!emitter.visible || !emitter.enabled)
        {
            emitter.culled = true;
            ClearParticleRuntimeCounters(commandBuffer, emitter);
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
            ClearParticleRuntimeCounters(commandBuffer, emitter);
            ++frameStats.culledEmitters;
            continue;
        }
        ++frameStats.visibleEmitters;
        emitter.culled = false;

        // 固定步长：帧间余量进入 accumulator，只推进完整步；最多 8 步/帧
        // 防止长时间停顿后一次补太多。余量保留到下一帧，避免时间漂移。
        ParticleFixedStepState fixedStepState{
            emitter.fixedStepAccumulator,
            emitter.fixedStepIndex};
        const ParticleFixedStepTick fixedStepTick =
            AdvanceParticleFixedStep(
                dt,
                emitter.asset.fixedTimeStep,
                8u,
                fixedStepState);
        emitter.fixedStepAccumulator = fixedStepState.accumulator;
        emitter.fixedStepIndex = fixedStepState.stepIndex;
        const float simulationStepSeconds =
            fixedStepTick.stepSeconds;
        const uint32_t simulationSteps = fixedStepTick.steps;
        if (emitter.asset.fixedTimeStep > 0.0f)
        {
            emitter.elapsedSeconds +=
                simulationStepSeconds *
                static_cast<float>(simulationSteps);
        }
        else
        {
            emitter.elapsedSeconds += dt;
        }

        // mesh/billboard 照明只取场景第一个方向光：方向光的 direction 是
        // 光线传播方向，表面指向光源需要取反。颜色使用线性 RGB，不乘 lux
        // 强度，避免粒子颜色与灯光量级错位；作者关闭时保持旧 unlit 输出。
        const bool wantsMeshLighting =
            emitter.asset.renderMode == "mesh" &&
            emitter.asset.meshLighting;
        const bool wantsBillboardLighting =
            emitter.asset.renderMode == "billboard" &&
            emitter.asset.billboardLighting;
        bool meshLightingEnabled = false;
        bool billboardLightingEnabled = false;
        Eigen::Vector3f lightDirection =
            Eigen::Vector3f(0.0f, -1.0f, 0.0f);
        Eigen::Vector3f lightColor = Eigen::Vector3f::Ones();
        if (wantsMeshLighting || wantsBillboardLighting)
        {
            for (const LightSnapshot& light : renderScene.lights)
            {
                if (light.type != LightSnapshotType::Directional)
                    continue;
                // 零方向/零光色会让 GGX 结果退化成黑块；文档承诺回退 unlit，
                // 因此在启用前同时检查方向与颜色，而不是只检查存在性。
                if (light.direction.squaredNorm() <= 1.0e-8f ||
                    light.color.maxCoeff() <= 0.0f)
                    continue;
                lightDirection = -light.direction;
                lightColor = light.color;
                meshLightingEnabled = wantsMeshLighting;
                billboardLightingEnabled = wantsBillboardLighting;
                break;
            }
        }
        emitter.lightingEnabled = meshLightingEnabled;
        emitter.lightDirection = lightDirection;
        emitter.lightColor = lightColor;
        emitter.ambient = emitter.asset.meshAmbient;

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
            simulationStepSeconds,
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
            globalSortPool.active
                ? static_cast<float>(
                    globalSortPool.textureSlotByKey.count(key) > 0
                    ? globalSortPool.textureSlotByKey.at(key)
                    : 0u)
                : 0.0f);
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
        const int billboardAlignmentCode =
            ParticleBillboardAlignmentCode(
                emitter.asset.billboardAlignment);
        uniforms.billboardParams = Eigen::Vector4f(
            static_cast<float>(billboardAlignmentCode),
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
        uniforms.curlParams = Eigen::Vector4f(
            emitter.asset.curlNoiseStrength,
            emitter.asset.curlNoiseFrequency,
            emitter.asset.curlNoiseSpeed,
            0.0f);
        uniforms.vortexParams = Eigen::Vector4f(
            emitter.asset.vortexStrength,
            emitter.asset.vortexRadius,
            0.0f,
            0.0f);
        uniforms.vortexAxis = Eigen::Vector4f(
            emitter.asset.vortexAxis.x(),
            emitter.asset.vortexAxis.y(),
            emitter.asset.vortexAxis.z(),
            0.0f);
        uniforms.attractorParams = Eigen::Vector4f(
            emitter.asset.attractorStrength,
            emitter.asset.attractorRadius,
            0.0f,
            0.0f);
        uniforms.attractorPosition = Eigen::Vector4f(
            emitter.asset.attractorPosition.x(),
            emitter.asset.attractorPosition.y(),
            emitter.asset.attractorPosition.z(),
            0.0f);
        uniforms.wind = Eigen::Vector4f(
            emitter.asset.wind.x(),
            emitter.asset.wind.y(),
            emitter.asset.wind.z(),
            0.0f);
        // 每帧整表重写：空表也必须清掉上一份 UBO 内容，count=0 才是
        // “恒为 1”的唯一信号。
        uniforms.lifetimeScaleKeys.fill(Eigen::Vector4f::Zero());
        const size_t lifetimeScaleCount = std::min<size_t>(
            emitter.asset.lifetimeScaleKeys.size(),
            kMaxParticleCurveKeys);
        for (size_t keyIndex = 0;
             keyIndex < lifetimeScaleCount;
             ++keyIndex)
        {
            const ParticleLifetimeScaleKey& key =
                emitter.asset.lifetimeScaleKeys[keyIndex];
            uniforms.lifetimeScaleKeys[keyIndex] =
                Eigen::Vector4f(key.time, key.scale, 0.0f, 0.0f);
        }
        uniforms.lifetimeScaleParams = Eigen::Vector4f(
            static_cast<float>(lifetimeScaleCount),
            0.0f,
            0.0f,
            0.0f);
        uniforms.meshLightParams = Eigen::Vector4f(
            meshLightingEnabled ? 1.0f : 0.0f,
            emitter.asset.meshAmbient,
            0.0f,
            0.0f);
        uniforms.meshLightDirection = Eigen::Vector4f(
            lightDirection.x(),
            lightDirection.y(),
            lightDirection.z(),
            0.0f);
        uniforms.meshLightColor = Eigen::Vector4f(
            lightColor.x(),
            lightColor.y(),
            lightColor.z(),
            0.0f);
        uniforms.materialBaseColor = emitter.material.valid
            ? emitter.material.baseColor
            : Eigen::Vector4f::Ones();
        uniforms.materialEmissiveStrength = Eigen::Vector4f(
            emitter.material.emissive.x(),
            emitter.material.emissive.y(),
            emitter.material.emissive.z(),
            emitter.material.emissiveStrength);
        uniforms.materialPbr = Eigen::Vector4f(
            emitter.material.roughness,
            emitter.material.metallic,
            emitter.material.valid ? 1.0f : 0.0f,
            0.0f);
        uniforms.billboardLightParams = Eigen::Vector4f(
            billboardLightingEnabled ? 1.0f : 0.0f,
            emitter.asset.billboardAmbient,
            // z 只缩放直接光：sprite 是体积感卡片，全量逐像素直接光会过曝；
            // 全局 alpha 池通过 gather record 原样携带该字段。
            std::clamp(
                emitter.asset.billboardLightingStrength,
                0.0f,
                1.0f),
            0.0f);
        // beam 端点是发射器局部空间作者数据，渲染前统一乘 emitter.model；
        // localSpace 对 beam 没有额外含义。
        const Eigen::Vector3f beamStartWorld =
            (emitter.model * Eigen::Vector4f(
                emitter.asset.beam.start.x(),
                emitter.asset.beam.start.y(),
                emitter.asset.beam.start.z(),
                1.0f)).head<3>();
        Eigen::Vector3f beamEndWorld =
            (emitter.model * Eigen::Vector4f(
                emitter.asset.beam.end.x(),
                emitter.asset.beam.end.y(),
                emitter.asset.beam.end.z(),
                1.0f)).head<3>();
        if (emitter.asset.beam.hitScan.enabled)
        {
            const float authoredRayLength =
                (beamEndWorld - beamStartWorld).norm();
            const auto hit = IntersectParticlePlane(
                beamStartWorld,
                beamEndWorld,
                emitter.asset.beam.hitScan.planeNormal,
                emitter.asset.beam.hitScan.planeOffset,
                std::min(
                    emitter.asset.beam.hitScan.maxDistance,
                    authoredRayLength));
            if (hit)
            {
                beamEndWorld = hit->position;
            }
        }
        // 端点已经被 model 的尺度放大；宽度/噪声/抖动是局部空间长度，
        // 必须使用同一最大列缩放，否则非 1 缩放下包围球与画面不一致。
        float beamScale = 0.0f;
        const Eigen::Matrix3f beamLinear =
            emitter.model.block<3, 3>(0, 0);
        for (int column = 0; column < 3; ++column)
        {
            beamScale = std::max(
                beamScale,
                beamLinear.col(column).norm());
        }
        uniforms.beamStart = Eigen::Vector4f(
            beamStartWorld.x(),
            beamStartWorld.y(),
            beamStartWorld.z(),
            static_cast<float>(emitter.asset.beam.segments));
        uniforms.beamEnd = Eigen::Vector4f(
            beamEndWorld.x(),
            beamEndWorld.y(),
            beamEndWorld.z(),
            emitter.asset.beam.width * beamScale);
        uniforms.beamNoise = Eigen::Vector4f(
            emitter.asset.beam.noiseAmplitude * beamScale,
            emitter.asset.beam.noiseFrequency,
            emitter.asset.beam.noiseSpeed,
            emitter.asset.beam.endpointJitter * beamScale);
        uniforms.beamParams = Eigen::Vector4f(
            static_cast<float>(emitter.asset.beam.count),
            static_cast<float>(emitter.asset.beam.branchCount),
            emitter.asset.beam.branchStart,
            emitter.asset.beam.branchLength);
        uniforms.beamBranch = Eigen::Vector4f(
            emitter.asset.beam.branchSpread * beamScale,
            0.0f,
            0.0f,
            0.0f);
        // 曲线与逐弧覆盖只对 beam 有意义；其它模式保持 count=0、
        // 恒等 tint/宽度，避免误读未初始化数组。
        uniforms.beamColorKeys.fill(Eigen::Vector4f::Zero());
        uniforms.beamColorKeyTimes.fill(Eigen::Vector4f::Zero());
        uniforms.beamWidthKeys.fill(Eigen::Vector4f::Zero());
        uniforms.beamCurveParams = Eigen::Vector4f::Zero();
        uniforms.beamArcColorStart.fill(
            Eigen::Vector4f(1.0f, 1.0f, 1.0f, 0.0f));
        uniforms.beamArcColorEnd.fill(
            Eigen::Vector4f(1.0f, 1.0f, 1.0f, 0.0f));
        uniforms.beamArcParams.fill(Eigen::Vector4f(1.0f, 1.0f, 0.0f, 0.0f));
        if (emitter.asset.renderMode == "beam")
        {
            const ParticleBeamAsset& beam = emitter.asset.beam;
            for (size_t index = 0;
                 index < beam.colorKeys.size();
                 ++index)
            {
                uniforms.beamColorKeys[index] =
                    beam.colorKeys[index].color;
                uniforms.beamColorKeyTimes[index / 4][index % 4] =
                    beam.colorKeys[index].time;
            }
            for (size_t index = 0;
                 index < beam.widthKeys.size();
                 ++index)
            {
                uniforms.beamWidthKeys[index] = Eigen::Vector4f(
                    beam.widthKeys[index].time,
                    beam.widthKeys[index].width,
                    0.0f,
                    0.0f);
            }
            for (size_t index = 0; index < beam.arcs.size(); ++index)
            {
                const ParticleBeamArc& arc = beam.arcs[index];
                uniforms.beamArcColorStart[index] = Eigen::Vector4f(
                    arc.colorStart.x(),
                    arc.colorStart.y(),
                    arc.colorStart.z(),
                    0.0f);
                uniforms.beamArcColorEnd[index] = Eigen::Vector4f(
                    arc.colorEnd.x(),
                    arc.colorEnd.y(),
                    arc.colorEnd.z(),
                    0.0f);
                uniforms.beamArcParams[index] = Eigen::Vector4f(
                    arc.widthScale,
                    arc.intensity,
                    arc.noisePhase,
                    0.0f);
            }
            uniforms.beamCurveParams = Eigen::Vector4f(
                static_cast<float>(beam.colorKeys.size()),
                static_cast<float>(beam.widthKeys.size()),
                0.0f,
                0.0f);
        }
        // fixed billboard 基在发射器局部空间作者化；上传前乘 emitter.model
        // 的线性部分，使其跟随粒子系统/武器的旋转与缩放。其它 alignment
        // 上传默认基；shader 即使误读也拿到有效向量。
        Eigen::Vector3f fixedNormal(0.0f, 0.0f, 1.0f);
        Eigen::Vector3f fixedUp(0.0f, 1.0f, 0.0f);
        if (billboardAlignmentCode == 2)
        {
            const Eigen::Matrix3f linear =
                emitter.model.block<3, 3>(0, 0);
            fixedNormal = linear * emitter.asset.billboardNormal;
            fixedUp = linear * emitter.asset.billboardUp;
            if (!(fixedNormal.squaredNorm() > 1.0e-6f))
            {
                fixedNormal = Eigen::Vector3f(0.0f, 0.0f, 1.0f);
            }
            else
            {
                fixedNormal.normalize();
            }
            if (!(fixedUp.squaredNorm() > 1.0e-6f))
            {
                fixedUp = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
            }
            else
            {
                fixedUp.normalize();
            }
            if (fixedNormal.cross(fixedUp).squaredNorm() <= 1.0e-6f)
            {
                fixedUp = std::abs(fixedNormal.y()) < 0.99f
                    ? Eigen::Vector3f(0.0f, 1.0f, 0.0f)
                    : Eigen::Vector3f(1.0f, 0.0f, 0.0f);
            }
        }
        uniforms.billboardFixedNormal = Eigen::Vector4f(
            fixedNormal.x(),
            fixedNormal.y(),
            fixedNormal.z(),
            0.0f);
        uniforms.billboardFixedUp = Eigen::Vector4f(
            fixedUp.x(),
            fixedUp.y(),
            fixedUp.z(),
            0.0f);
        // 只有 distortion emitter 消费该字段；其它模式保持 0，shader 即使
        // 误读也得到零偏移。
        uniforms.distortionParams = Eigen::Vector4f(
            emitter.asset.renderMode == "distortion"
                ? emitter.asset.distortionStrength
                : 0.0f,
            // y = 真实纹理可用；加载失败时强制走程序化方向场。
            emitter.textureFallback ? 0.0f : 1.0f,
            0.0f,
            0.0f);
        uniforms.particleLightParams = Eigen::Vector4f(
            static_cast<float>(emitter.lightSlotBase),
            static_cast<float>(emitter.lightSlotCapacity),
            emitter.asset.light.intensity,
            emitter.asset.light.radius);
        for (int cascade = 0; cascade < 4; ++cascade)
        {
            uniforms.shadowLightViewProj[cascade] =
                shadowCascadeData.valid
                ? shadowCascadeData.lightViewProj[cascade]
                : Eigen::Matrix4f::Identity();
            uniforms.shadowCascadeBias[cascade] =
                shadowCascadeData.valid
                ? shadowCascadeData.bias[cascade]
                : Eigen::Vector4f::Zero();
        }
        uniforms.shadowCascadeSplits =
            shadowCascadeData.valid
            ? shadowCascadeData.cascadeSplits
            : Eigen::Vector4f::Zero();
        uniforms.shadowParams = Eigen::Vector4f(
            shadowCascadeData.valid
                ? shadowCascadeData.csmParameters.z()
                : 0.0f,
            shadowCascadeData.valid
                ? shadowCascadeData.csmParameters.w()
                : 0.0f,
            shadowCascadeData.valid ? 1.0f : 0.0f,
            emitter.asset.billboardShadows ? 1.0f : 0.0f);
        // seed 只保留 24 位，保证 float 往返后仍精确；其余输入都由
        // fixedStepIndex 驱动，不依赖 wall clock。
        uniforms.determinismParams = Eigen::Vector4f(
            static_cast<float>(emitter.asset.randomSeed & 0x00FFFFFFu),
            static_cast<float>(simulationSteps > 0 ? simulationSteps : 1),
            static_cast<float>(emitter.fixedStepIndex),
            emitter.asset.deterministicRandom ? 1.0f : 0.0f);
        uniforms.eventParams = Eigen::Vector4f(
            emitter.asset.emitSpawnEvent ? 1.0f : 0.0f,
            emitter.asset.emitDeathEvent ? 1.0f : 0.0f,
            emitter.asset.emitCollisionEvent ? 1.0f : 0.0f,
            0.0f);
        if (emitter.sdf)
        {
            uniforms.collisionSdfBoundsMin = Eigen::Vector4f(
                emitter.sdf->boundsMin.x(),
                emitter.sdf->boundsMin.y(),
                emitter.sdf->boundsMin.z(),
                0.0f);
            uniforms.collisionSdfBoundsMax = Eigen::Vector4f(
                emitter.sdf->boundsMax.x(),
                emitter.sdf->boundsMax.y(),
                emitter.sdf->boundsMax.z(),
                0.0f);
            uniforms.collisionSdfResolution = Eigen::Vector4f(
                static_cast<float>(emitter.sdf->resolution.x()),
                static_cast<float>(emitter.sdf->resolution.y()),
                static_cast<float>(emitter.sdf->resolution.z()),
                0.0f);
        }
        else
        {
            uniforms.collisionSdfBoundsMin = Eigen::Vector4f::Zero();
            uniforms.collisionSdfBoundsMax = Eigen::Vector4f::Zero();
            uniforms.collisionSdfResolution = Eigen::Vector4f::Zero();
        }
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

        // 每帧清零 alive/spawn/death/collision 计数。上一帧 finalize 读过
        // counters，因此先做 shaderWrite -> transferWrite 依赖。
        {
            vk::MemoryBarrier computeToTransfer;
            computeToTransfer
                .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                computeToTransfer,
                nullptr,
                nullptr);
            commandBuffer.fillBuffer(
                emitter.counters.buffer,
                0,
                sizeof(uint32_t) * 5u,
                0);
            commandBuffer.fillBuffer(
                emitter.debugCounters.buffer,
                0,
                sizeof(ParticleOverdrawCounters),
                0);
            vk::BufferMemoryBarrier clearToCompute;
            clearToCompute
                .setBuffer(emitter.counters.buffer)
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
            vk::BufferMemoryBarrier debugClearToGraphics;
            debugClearToGraphics
                .setBuffer(emitter.debugCounters.buffer)
                .setOffset(0)
                .setSize(VK_WHOLE_SIZE)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eFragmentShader,
                {},
                nullptr,
                debugClearToGraphics,
                nullptr);
        }

        // beam 是纯渲染模式：弧的形状完全来自 UBO，不消费粒子池、
        // 不参与局部/全局排序。跳过 simulate/sort/finalize，避免为零
        // 渲染的粒子池做无意义计算；计数与 overdraw 缓冲仍已清零。
        if (emitter.asset.renderMode == "beam")
        {
            emitter.prewarm = false;
            continue;
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

        // light 发射器只产光，不进入排序/绘制。emit 读取 simulate 刚写入
        // 的粒子池，并写全局 light 输出 SSBO；两个 compute 之间必须显式
        // 建立 shaderWrite -> shaderRead 依赖。
        if (emitter.asset.renderMode == "light")
        {
            if (particleLightEmitPipeline &&
                emitter.lightEmitSets.size() > imageIndex)
            {
                InsertBufferBarrier(
                    commandBuffer,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderRead |
                        vk::AccessFlagBits::eShaderWrite);
                particleLightEmitPipeline->Bind(commandBuffer);
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eCompute,
                    particleLightEmitPipeline->GetPipelineLayout(),
                    0,
                    emitter.lightEmitSets.at(imageIndex),
                    nullptr);
                particleLightEmitPipeline->Dispatch(
                    commandBuffer,
                    groupCount,
                    1,
                    1);
            }
            emitter.prewarm = false;
            continue;
        }

        // 参与全局排序的发射器跳过自己的 bitonic 排序，统一在帧末做一次
        // 跨发射器排序。
        const bool globalSorted = IsGlobalSortEmitter(key);
        if (emitter.asset.sortParticles && !globalSorted)
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

        // GPU 收尾：把原子 alive 计数写回 indirect args.instanceCount。
        // 后续 graphics 直接 drawIndirect，不再使用 CPU 侧容量/存活猜测。
        InsertBufferBarrier(
            commandBuffer,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::AccessFlagBits::eShaderWrite,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::AccessFlagBits::eShaderRead |
                vk::AccessFlagBits::eShaderWrite);
        finalizeCountPipeline->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            finalizeCountPipeline->GetPipelineLayout(),
            0,
            emitter.finalizeSets.at(imageIndex),
            nullptr);
        finalizeCountPipeline->Dispatch(commandBuffer, 1, 1, 1);

        // prewarm 只允许第一帧；若录制后提交失败，下一帧会按正常发射补齐，
        // 不会重复把整池粒子重置成同一寿命分布。
        emitter.prewarm = false;
    }

    // 跨发射器 gather + 一次全局 bitonic + finalize。只有存在参与发射器
    // 时才执行；否则池保持 inactive。
    if (globalSortPool.active)
    {
        globalSortPool.gpuTimer.BeginFrame(commandBuffer, imageIndex);
    }
    RecordGlobalGather(commandBuffer, imageIndex);
    RecordGlobalSort(commandBuffer, imageIndex, renderScene);

    vk::MemoryBarrier writeToRead;
    writeToRead
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eIndirectCommandRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eFragmentShader |
            vk::PipelineStageFlagBits::eDrawIndirect,
        {},
        writeToRead,
        nullptr,
        nullptr);

    std::vector<ParticleEmitterDepthKey> depthKeys;
    depthKeys.reserve(emitters.size() + 1u);
    for (const auto& [key, emitterPtr] : emitters)
    {
        const EmitterResources& emitter = *emitterPtr;
        ParticleEmitterDepthKey depthKey;
        depthKey.key = key;
        depthKey.worldCenter = emitter.worldCenter;
        depthKey.worldRadius = emitter.boundsRadius;
        depthKey.sortOrder = emitter.asset.sortOrder;
        depthKeys.push_back(std::move(depthKey));
    }
    // 全局 alpha 池不是一个真实 emitter，但必须作为一个 draw item 参与
    // 同一份排序；否则它只能在所有逐发射器之后录制，重新出现“烟永远
    // 盖住碎块/弹壳”的问题。
    // 只统计本帧可真实 gather 的成员（缺失/被剔除/本 image UBO 未就绪
    // 的成员不会产生顶点，不能影响整池位置）。池整体取最小 sortOrder
    // 与池的背景边缘深度，近似边界见 TryMakeParticleGlobalSortDrawKey。
    if (globalSortPool.active)
    {
        std::vector<ParticleEmitterDepthKey> globalMembers;
        globalMembers.reserve(globalSortPool.emitterKeys.size());
        for (const std::string& memberKey : globalSortPool.emitterKeys)
        {
            const auto memberIt = emitters.find(memberKey);
            if (memberIt == emitters.end())
            {
                continue;
            }
            const EmitterResources& member = *memberIt->second;
            if (member.culled ||
                member.frameUniforms.size() <= imageIndex)
            {
                continue;
            }
            ParticleEmitterDepthKey memberKeyData;
            memberKeyData.key = memberKey;
            memberKeyData.worldCenter = member.worldCenter;
            memberKeyData.worldRadius = member.boundsRadius;
            memberKeyData.sortOrder = member.asset.sortOrder;
            globalMembers.push_back(std::move(memberKeyData));
        }
        ParticleEmitterDepthKey globalKey;
        if (TryMakeParticleGlobalSortDrawKey(
                globalMembers,
                renderScene.camera.position,
                renderScene.camera.forward,
                globalKey))
        {
            depthKeys.push_back(std::move(globalKey));
        }
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

    // 计时快照来自各 emitter 在上一轮同 image 的 query；按 key 排序发布，
    // 避免 unordered_map 迭代顺序让 stats JSON 抖动。
    frameStats.emitterGpu.clear();
    frameStats.emitterGpu.reserve(emitters.size() + 1u);
    for (const auto& [key, emitterPtr] : emitters)
    {
        const ParticleGpuTimingSnapshot& snapshot =
            emitterPtr->gpuTimer.GetSnapshot();
        frameStats.emitterGpu.push_back(ParticleFrameStats::EmitterGpu{
            key,
            snapshot.supported,
            snapshot.lastMilliseconds,
            snapshot.averageMilliseconds,
            snapshot.maxMilliseconds,
            snapshot.sampleCount});
    }
    if (globalSortPool.active)
    {
        const ParticleGpuTimingSnapshot& snapshot =
            globalSortPool.gpuTimer.GetSnapshot();
        frameStats.emitterGpu.push_back(ParticleFrameStats::EmitterGpu{
            "__global__",
            snapshot.supported,
            snapshot.lastMilliseconds,
            snapshot.averageMilliseconds,
            snapshot.maxMilliseconds,
            snapshot.sampleCount});
    }
    std::sort(
        frameStats.emitterGpu.begin(),
        frameStats.emitterGpu.end(),
        [](const ParticleFrameStats::EmitterGpu& left,
           const ParticleFrameStats::EmitterGpu& right)
        {
            return left.key < right.key;
        });
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
            emitter.asset.renderMode == "distortion" ||
            emitter.asset.renderMode == "light" ||
            IsGlobalSortEmitter(emitter.key) ||
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
        const bool beam =
            emitter.asset.renderMode == "beam";
        const std::shared_ptr<PipelineBase>& pipeline = mesh
            ? (emitter.asset.additive
                ? meshAdditivePipeline
                : meshAlphaPipeline)
            : (beam
                ? (emitter.asset.additive
                    ? beamAdditivePipeline
                    : beamAlphaPipeline)
                : (emitter.asset.renderMode == "ribbon"
                ? (emitter.asset.additive
                    ? ribbonAdditivePipeline
                    : ribbonAlphaPipeline)
                : (emitter.asset.additive
                    ? additivePipeline
                    : alphaPipeline)));
        if (!pipeline)
        {
            return;
        }
        emitter.gpuTimer.BeginPass(commandBuffer, imageIndex);
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
            if (emitter.indirectArgs.buffer)
            {
                commandBuffer.drawIndexedIndirect(
                    emitter.indirectArgs.buffer,
                    0,
                    1,
                    sizeof(vk::DrawIndexedIndirectCommand));
            }
            else
            {
                commandBuffer.drawIndexed(
                    emitter.meshIndexCount,
                    emitter.asset.maxParticles,
                    0,
                    0,
                    0);
            }
            drawnVertices = ParticleMeshDrawnVertexCount(
                emitter.meshIndexCount,
                emitter.asset.maxParticles);
        }
        else
        {
            const bool ribbon =
                emitter.asset.renderMode == "ribbon";
            const uint32_t verticesPerParticle = ribbon
                ? ParticleRibbonVerticesPerParticle(
                    emitter.asset.ribbonSegments)
                : 6u;
            if (beam)
            {
                // beam 的实例数来自 count * (1 + branchCount)，而不是存活粒子数；必须绕过
                // indirect/finalize，否则零粒子发射器会被画成 0 条弧。
                const uint32_t segments =
                    ClampParticleBeamSegments(
                        emitter.asset.beam.segments);
                const uint32_t count = emitter.asset.beam.count;
                const uint32_t branchCount =
                    emitter.asset.beam.branchCount;
                const uint32_t totalInstances =
                    static_cast<uint32_t>(
                        ParticleBeamInstanceCount(
                            count,
                            branchCount));
                commandBuffer.draw(
                    segments * kParticleBeamVerticesPerSegment,
                    totalInstances,
                    0,
                    0);
                drawnVertices = ParticleBeamVertexCount(
                    segments,
                    count,
                    branchCount);
            }
            else if (emitter.indirectArgs.buffer)
            {
                commandBuffer.drawIndirect(
                    emitter.indirectArgs.buffer,
                    0,
                    1,
                    sizeof(vk::DrawIndirectCommand));
                // indirect 的 instanceCount 由 GPU 改写；统计仍沿用本帧
                // 上界，与 direct fallback 保持一致，不能漏记成 0。
                drawnVertices =
                    static_cast<uint64_t>(verticesPerParticle) *
                    emitter.asset.maxParticles;
            }
            else
            {
                commandBuffer.draw(
                    verticesPerParticle,
                    emitter.asset.maxParticles,
                    0,
                    0);
                drawnVertices =
                    static_cast<uint64_t>(verticesPerParticle) *
                    emitter.asset.maxParticles;
            }
        }
        emitter.gpuTimer.EndPass(commandBuffer, imageIndex);
        ++frameStats.estimatedDrawCalls;
        frameStats.estimatedDrawnVertices += drawnVertices;
    };

    std::unordered_set<std::string> orderedKeys;
    orderedKeys.reserve(drawOrder.size());
    for (const std::string& key : drawOrder)
    {
        // 全局池在排序表里占一个特殊 item；轮到它时一次性画完池内
        // 所有已 gather 的粒子，而不是固定拖到所有 emitter 之后。
        if (key == kParticleGlobalDrawKey)
        {
            RecordGlobalDraw(commandBuffer, imageIndex, renderPass);
            continue;
        }
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

std::shared_ptr<PipelineBase>
ParticleRenderer::GetOrCreateShadowPipeline(
    const Renderpass& renderPass)
{
    if (!initialized || pipelineFactory == nullptr)
    {
        return nullptr;
    }

    // 同一图里的 CSM、localShadow 层各自拥有 VkRenderPass 句柄；管线状态
    // 完全相同也必须按句柄分开缓存，否则会把 CSM 管线拿去绑 localShadow。
    const uintptr_t renderPassKey =
        reinterpret_cast<uintptr_t>(
            static_cast<VkRenderPass>(renderPass.renderPass));
    const auto existing = shadowPipelines.find(renderPassKey);
    if (existing != shadowPipelines.end())
    {
        return existing->second;
    }

    ShaderVariantKey variant;
    variant.shaderName = "particle/particleShadow";
    variant.renderMode = RenderMode::TransparentAdditive;
    variant.shadingModelMacro = "SHADING_MODEL_UNLIT";
    const GraphicsShaderVariantArtifact& artifact =
        pipelineFactory->PrepareGraphicsShaderVariant(variant);

    PassPipelineContractKey contract =
        renderPass.pipelineContractKey;
    contract.useVertexInput = true;

    // caster shader 只声明 set 0；显式保留与可见粒子完全相同的 set 0
    // 合同（graphicsSets 就是用这份 layout 分配的），但不把 pass set 3
    // 带进阴影管线布局。
    GraphicsPipelineLayoutDesc layout = BuildParticleSharedLayout();
    layout.overrideSets[PassSetIndex] = false;
    layout.setBindings[PassSetIndex].clear();

    // PipelineFactory 的旧接口接受非 const 句柄指针；这里传一份副本，
    // 不把 render pass 的只读缓存暴露给可写接口。
    vk::RenderPass renderPassHandle = renderPass.renderPass;
    std::shared_ptr<PipelineBase> pipeline =
        pipelineFactory->CreateGraphicsPipeline(
            &renderPassHandle,
            contract,
            artifact,
            vk::CullModeFlagBits::eNone,
            GraphicsPipelineBlendMode::Opaque,
            layout);
    if (pipeline)
    {
        shadowPipelines.emplace(renderPassKey, pipeline);
    }
    return pipeline;
}

bool ParticleRenderer::IsMeshShadowCaster(
    const EmitterResources& emitter,
    uint32_t imageIndex,
    const Eigen::Vector3f& lightPosition,
    float farCm)
{
    return ParticleEmitterCastsMeshShadow(
               emitter.asset,
               emitter.meshFallbackBillboard) &&
        !emitter.culled &&
        emitter.visible &&
        emitter.enabled &&
        emitter.meshIndexCount > 0 &&
        emitter.meshVertices.buffer &&
        emitter.meshIndices.buffer &&
        emitter.frameUniforms.size() > imageIndex &&
        ParticleMeshCasterIntersectsShadowVolume(
            emitter.worldCenter,
            emitter.boundsRadius,
            lightPosition,
            farCm);
}

void ParticleRenderer::PrepareShadow(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Eigen::Matrix4f& viewProjection,
    int mode,
    int octant,
    const Eigen::Vector3f& lightPosition,
    float farCm,
    float biasCm)
{
    // RecordShadow 与 PrepareShadow 严格按 pass 交替调用；这里缓存本次
    // shadow 视图的剔除参数，避免绘制阶段再依赖已经更新的全局 UBO。
    shadowCullLightPosition = lightPosition;
    shadowCullFarCm = farCm;
    if (!initialized || emitters.empty())
    {
        return;
    }

    struct ShadowCasterUniforms
    {
        Eigen::Matrix4f viewProjection;
        Eigen::Vector4f parameters;
        Eigen::Vector4f lightPosition;
    };
    static_assert(
        sizeof(ShadowCasterUniforms) == 96,
        "Shadow caster UBO block must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shadowCasterParams) -
                offsetof(FrameUniforms, shadowViewProj) ==
            64,
        "shadowCasterParams must directly follow shadowViewProj");
    static_assert(
        offsetof(FrameUniforms, shadowLightPosition) -
                offsetof(FrameUniforms, shadowViewProj) ==
            80,
        "shadowLightPosition must directly follow shadowCasterParams");

    bool hasCaster = false;
    for (const auto& [key, emitter] : emitters)
    {
        (void)key;
        if (IsMeshShadowCaster(
                *emitter, imageIndex, lightPosition, farCm))
        {
            hasCaster = true;
            break;
        }
    }
    if (!hasCaster)
    {
        return;
    }

    // 上一段 shadow draw 的顶点着色器可能仍在读同一 per-image UBO；
    // 有序 updateBuffer 之前必须先把 uniform read 与 transfer write 隔开。
    vk::MemoryBarrier readToTransfer;
    readToTransfer
        .setSrcAccessMask(vk::AccessFlagBits::eUniformRead)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eVertexShader,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        readToTransfer,
        nullptr,
        nullptr);

    ShadowCasterUniforms uniforms{};
    uniforms.viewProjection = viewProjection;
    uniforms.parameters = Eigen::Vector4f(
        static_cast<float>(mode),
        static_cast<float>(octant),
        farCm,
        biasCm);
    uniforms.lightPosition = Eigen::Vector4f(
        lightPosition.x(),
        lightPosition.y(),
        lightPosition.z(),
        0.0f);
    for (auto& [key, emitter] : emitters)
    {
        (void)key;
        if (!IsMeshShadowCaster(
                *emitter, imageIndex, lightPosition, farCm))
        {
            continue;
        }
        commandBuffer.updateBuffer(
            emitter->frameUniforms.at(imageIndex).buffer,
            offsetof(FrameUniforms, shadowViewProj),
            sizeof(uniforms),
            &uniforms);
    }

    vk::MemoryBarrier transferToVertex;
    transferToVertex
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eUniformRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eVertexShader,
        {},
        transferToVertex,
        nullptr,
        nullptr);
}

void ParticleRenderer::RecordShadow(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    if (!initialized || emitters.empty())
    {
        return;
    }
    std::shared_ptr<PipelineBase> pipeline =
        GetOrCreateShadowPipeline(renderPass);
    if (!pipeline)
    {
        return;
    }

    bool pipelineBound = false;
    for (auto& [key, emitterPtr] : emitters)
    {
        (void)key;
        EmitterResources& emitter = *emitterPtr;
        if (!IsMeshShadowCaster(
                emitter,
                imageIndex,
                shadowCullLightPosition,
                shadowCullFarCm) ||
            emitter.graphicsSets.size() <= imageIndex)
        {
            continue;
        }

        if (!pipelineBound)
        {
            commandBuffer.bindPipeline(
                pipeline->GetBindPoint(),
                pipeline->GetPipeline());
            pipelineBound = true;
        }
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            pipeline->GetPipelineLayout(),
            0,
            emitter.graphicsSets.at(imageIndex),
            nullptr);

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
        if (emitter.indirectArgs.buffer)
        {
            commandBuffer.drawIndexedIndirect(
                emitter.indirectArgs.buffer,
                0,
                1,
                sizeof(vk::DrawIndexedIndirectCommand));
        }
        else
        {
            commandBuffer.drawIndexed(
                emitter.meshIndexCount,
                emitter.asset.maxParticles,
                0,
                0,
                0);
        }
    }
}

void ParticleRenderer::RecordDistortion(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    if (!initialized || !distortionPipeline)
    {
        return;
    }

    const auto& descriptorSets = renderPass.GetDescriptorSets();
    bool usesPassSet = false;
    for (const ShaderBinding& binding :
         distortionPipeline->GetShaderBindings())
    {
        if (binding.set == PassSetIndex)
        {
            usesPassSet = true;
            break;
        }
    }

    for (auto& [key, emitterPtr] : emitters)
    {
        (void)key;
        EmitterResources& emitter = *emitterPtr;
        if (emitter.culled ||
            emitter.asset.renderMode != "distortion" ||
            emitter.graphicsSets.size() <= imageIndex)
        {
            continue;
        }

        emitter.gpuTimer.BeginPass(commandBuffer, imageIndex);
        commandBuffer.bindPipeline(
            distortionPipeline->GetBindPoint(),
            distortionPipeline->GetPipeline());
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            distortionPipeline->GetPipelineLayout(),
            0,
            emitter.graphicsSets.at(imageIndex),
            nullptr);
        if (usesPassSet &&
            imageIndex < descriptorSets.size() &&
            PassSetIndex < descriptorSets[imageIndex].size())
        {
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics,
                distortionPipeline->GetPipelineLayout(),
                PassSetIndex,
                descriptorSets[imageIndex][PassSetIndex],
                nullptr);
        }

        // 与 billboard 相同：indirect 的 instanceCount 由 GPU finalize
        // 写入存活数；没有 indirect 缓冲时按池上限绘制，由 shader 丢弃死亡槽。
        if (emitter.indirectArgs.buffer)
        {
            commandBuffer.drawIndirect(
                emitter.indirectArgs.buffer,
                0,
                1,
                sizeof(vk::DrawIndirectCommand));
        }
        else
        {
            commandBuffer.draw(
                6u,
                emitter.asset.maxParticles,
                0,
                0);
        }
        // 与 particle pass 的统计口径一致：indirect 的实例数由 GPU 改写，
        // 这里按池上限记录本帧上界；dump 也按同一公式估算顶点数。
        ++frameStats.estimatedDrawCalls;
        frameStats.estimatedDrawnVertices +=
            static_cast<uint64_t>(6u) * emitter.asset.maxParticles;
        emitter.gpuTimer.EndPass(commandBuffer, imageIndex);
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

        const vk::DeviceSize particleBytes =
            sizeof(GpuParticle) *
            static_cast<vk::DeviceSize>(emitter.asset.maxParticles);
        const vk::DeviceSize readbackBytes =
            particleBytes + sizeof(ParticleOverdrawCounters);
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
                vk::BufferCopy(0, 0, particleBytes));

            toTransfer.setBuffer(emitter.debugCounters.buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                emitter.debugCounters.buffer,
                readbackBuffer,
                vk::BufferCopy(
                    0,
                    particleBytes,
                    sizeof(ParticleOverdrawCounters)));

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
            const ParticleOverdrawCounters* overdraw =
                reinterpret_cast<const ParticleOverdrawCounters*>(
                    static_cast<const std::uint8_t*>(mappedMemory) +
                    particleBytes);
            ParticleDumpEmitter dumpEmitter;
            dumpEmitter.key = emitter.key;
            // beam 不占用模拟粒子池；dump 用 drawInstances 表达弧数量。
            dumpEmitter.maxParticles =
                emitter.asset.renderMode == "beam"
                    ? 0u
                    : emitter.asset.maxParticles;
            dumpEmitter.drawInstances =
                emitter.asset.renderMode == "beam"
                    ? static_cast<uint32_t>(
                        ParticleBeamInstanceCount(
                            emitter.asset.beam.count,
                            emitter.asset.beam.branchCount))
                    : 0u;
            dumpEmitter.culled = emitter.culled;
            dumpEmitter.sortParticles = emitter.asset.sortParticles;
            dumpEmitter.localSpace = emitter.asset.localSpace;
            dumpEmitter.renderMode = emitter.asset.renderMode;
            if (emitter.asset.renderMode == "light")
            {
                // light 只写 GPU light SSBO，不产生可见 draw；dump 不能把
                // 存活粒子误报成 billboard 顶点。
                dumpEmitter.verticesPerParticle = 0u;
            }
            else if (emitter.asset.renderMode == "mesh" &&
                !emitter.meshFallbackBillboard &&
                emitter.meshIndexCount > 0)
            {
                dumpEmitter.verticesPerParticle =
                    emitter.meshIndexCount;
            }
            else
            {
                if (emitter.asset.renderMode == "beam")
                {
                    dumpEmitter.verticesPerParticle =
                        ParticleBeamVerticesPerParticle(
                            emitter.asset.beam.segments);
                }
                else
                {
                    dumpEmitter.verticesPerParticle =
                        emitter.asset.renderMode == "ribbon"
                            ? ParticleRibbonVerticesPerParticle(
                                emitter.asset.ribbonSegments)
                            : 6u;
                }
            }
            dumpEmitter.boundsCenter = emitter.worldCenter;
            dumpEmitter.boundsRadius = emitter.boundsRadius;
            dumpEmitter.elapsedSeconds = emitter.elapsedSeconds;
            // 被剔除的 emitter 不录制图形 pass，计数器会保留上一帧值，
            // dump 必须显式归零而不是发布陈旧 overdraw。
            dumpEmitter.overdrawFragments = emitter.culled
                ? 0u
                : overdraw->fragments;
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
            dumpEmitters.back().estimatedParticleBufferReadBytes =
                static_cast<uint64_t>(
                    dumpEmitters.back().aliveCount) *
                sizeof(GpuParticle);

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

    ParticleDumpGlobalStats globalStats;
    const uint32_t globalImageIndex =
        std::min(
            globalSortPool.lastImageIndex,
            static_cast<uint32_t>(
                std::max<size_t>(
                    globalSortPool.counters.size(),
                    1u) - 1u));
    if (globalSortPool.active &&
        globalSortPool.counters.size() > globalImageIndex &&
        globalSortPool.debugCounters.size() > globalImageIndex)
    {
        const vk::DeviceSize counterBytes =
            sizeof(ParticleEventCounters);
        const vk::DeviceSize overdrawBytes =
            sizeof(ParticleOverdrawCounters);
        const vk::DeviceSize readbackBytes =
            counterBytes + overdrawBytes;
        vk::Buffer readbackBuffer;
        vk::DeviceMemory readbackMemory;
        try
        {
            std::tie(readbackBuffer, readbackMemory) =
                readbackBackend.CreateBuffer(
                    readbackBytes,
                    vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleGlobalDumpReadback");
            vk::CommandBuffer commandBuffer =
                readbackBackend.BeginSingleTimeCommands();
            vk::BufferMemoryBarrier toTransfer;
            toTransfer
                .setSrcAccessMask(
                    vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            toTransfer.setBuffer(
                globalSortPool.counters[globalImageIndex].buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                globalSortPool.counters[globalImageIndex].buffer,
                readbackBuffer,
                vk::BufferCopy(0, 0, counterBytes));
            toTransfer.setBuffer(
                globalSortPool.debugCounters[globalImageIndex].buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                globalSortPool.debugCounters[globalImageIndex].buffer,
                readbackBuffer,
                vk::BufferCopy(0, counterBytes, overdrawBytes));
            readbackBackend.EndSingleTimeCommands(commandBuffer);

            const std::uint8_t* mapped =
                static_cast<const std::uint8_t*>(
                    readbackBackend.MapMemory(
                        readbackMemory,
                        readbackBytes));
            if (mapped == nullptr)
            {
                throw std::runtime_error("map returned null");
            }
            const ParticleEventCounters* counters =
                reinterpret_cast<const ParticleEventCounters*>(mapped);
            const ParticleOverdrawCounters* overdraw =
                reinterpret_cast<const ParticleOverdrawCounters*>(
                    mapped + counterBytes);
            globalStats.aliveParticles = counters->alive;
            globalStats.overdrawFragments = overdraw->fragments;
            globalStats.estimatedParticleBufferReadBytes =
                static_cast<uint64_t>(counters->alive) *
                sizeof(GpuParticle);
            readbackBackend.UnmapMemory(readbackMemory);
            readbackBackend.DestroyBuffer(
                readbackBuffer,
                readbackMemory);
        }
        catch (const std::exception& exception)
        {
            if (readbackBuffer || readbackMemory)
            {
                readbackBackend.DestroyBuffer(
                    readbackBuffer,
                    readbackMemory);
            }
            outError =
                "Particle global dump readback failed: " +
                std::string(exception.what());
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
                frameStats,
                globalStats).dump(2) + "\n";
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
