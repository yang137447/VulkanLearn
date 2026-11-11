#pragma once

#include "particles/particleRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "commonFunction.h"
#include "animation/animationAssets.h"
#include "animation/skeletalPlayback.h"
#include "core/logStore.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "pipeline/computePipeline.h"
#include "pipeline/graphicsPipelineLayoutDesc.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "particles/particleCollision.h"
#include "particles/particleBudget.h"
#include "particles/particleBeam.h"
#include "particles/particleAttributeBindings.h"
#include "particles/particleBurst.h"
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
#include "particles/particleSkeletalPreflight.h"
#include "particles/particleTextureReference.h"
#include "particles/particleUserParameterOverride.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/frontend/renderScene.h"
#include "render/lightCulling/lightCulling.h"
#include "render/rendergraph/renderGraphPassType.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"
#include "resource/image/textureIO.h"
#include "shader/build/atomicFile.h"
#include "shaderReflect.h"
#include "texture.h"
#include "textureAssetLoader.h"

namespace VL
{

namespace
{

// 骨骼网格粒子资源构建期间使用的 CPU 蒙皮参考。palette/matrix 语义与
// particleSkeletalSkin.glsl 完全一致：p = Σ w · (model_j · inverseBind_j) · p_bind。
template <typename Section>
void SkinParticleSkeletalSection(
    Section& section,
    const std::vector<Eigen::Matrix4f>& models)
{
    section.skinnedVertices = section.sourceVertices;
    for (size_t vertexIndex = 0;
         vertexIndex < section.sourceVertices.size();
         ++vertexIndex)
    {
        Eigen::Matrix4f skin = Eigen::Matrix4f::Zero();
        const uint32_t begin = section.influenceOffsets.at(vertexIndex);
        const uint32_t end = section.influenceOffsets.at(vertexIndex + 1);
        for (uint32_t index = begin; index < end; ++index)
        {
            const auto& influence = section.influences.at(index);
            if (influence.joint >= models.size() ||
                influence.joint >= section.inverseBinds.size())
            {
                continue;
            }
            skin += influence.weight *
                models[influence.joint] *
                section.inverseBinds[influence.joint];
        }
        Vertex& target = section.skinnedVertices[vertexIndex];
        const Vertex& source = section.sourceVertices[vertexIndex];
        target.position =
            (skin * Eigen::Vector4f(
                source.position.x(),
                source.position.y(),
                source.position.z(),
                1.0f)).head<3>();
        const Eigen::Matrix3f linear = skin.block<3, 3>(0, 0);
        target.normal = (linear.inverse().transpose() * source.normal).normalized();
        Eigen::Vector3f tangent = linear * source.tangent.head<3>();
        tangent -= target.normal * target.normal.dot(tangent);
        if (tangent.squaredNorm() > 1.0e-10f)
        {
            target.tangent.head<3>() = tangent.normalized();
        }
    }
}

} // namespace

namespace
{

// maxParticles 是作者池大小；排序网络要求 2 的幂，最小 256 以摊薄 dispatch。
constexpr uint32_t kParticleSortMinimumCapacity = 256;
constexpr uint32_t kParticleSortMaximumCapacity = 4096;

uint64_t BuildParticleDataChannelSignature(
    const ParticleDataChannelMap& channels)
{
    std::vector<std::pair<std::string, std::string>> ordered;
    ordered.reserve(channels.size());
    for (const auto& [key, value] : channels)
    {
        ordered.emplace_back(key, value.ToTypedJson().dump());
    }
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const auto& left, const auto& right)
        {
            return left.first < right.first;
        });
    uint64_t hash = 1469598103934665603ull;
    for (const auto& [key, value] : ordered)
    {
        for (char character : key)
        {
            hash ^= static_cast<uint8_t>(character);
            hash *= 1099511628211ull;
        }
        for (char character : value)
        {
            hash ^= static_cast<uint8_t>(character);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

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
    globalBindings.push_back(MakeParticleLayoutBinding(
        10,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "LightGridBuffer"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        11,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "LightIndexBuffer"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        12,
        vk::DescriptorType::eUniformBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "LightGridParamsBlock"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        13,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "translucencyVolume"));
    globalBindings.push_back(MakeParticleLayoutBinding(
        14,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "iesLut"));
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
    ShaderBinding softDepth;
    softDepth.set = PassSetIndex;
    softDepth.binding = 3;
    softDepth.type = vk::DescriptorType::eCombinedImageSampler;
    softDepth.descriptorCount = 1;
    softDepth.stageFlags = vk::ShaderStageFlagBits::eFragment;
    softDepth.name = "softDepth";
    layout.setBindings[PassSetIndex] = {
        std::move(sceneDepth),
        std::move(shadowMap),
        std::move(localShadowMap),
        std::move(softDepth)};
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
    bindings.push_back(MakeParticleLayoutBinding(
        10,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "LightGridBuffer"));
    bindings.push_back(MakeParticleLayoutBinding(
        11,
        vk::DescriptorType::eStorageBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "LightIndexBuffer"));
    bindings.push_back(MakeParticleLayoutBinding(
        12,
        vk::DescriptorType::eUniformBuffer,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "LightGridParamsBlock"));
    bindings.push_back(MakeParticleLayoutBinding(
        13,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "translucencyVolume"));
    bindings.push_back(MakeParticleLayoutBinding(
        14,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment,
        "iesLut"));
    // Set 3 必须与 RenderGraph 的输入全集完全一致。反射会裁掉未静态使用的
    // binding（beam 不采样 CSM/local shadow），若各管线各走反射，layout 会
    // 少于 pass descriptor set 的 0/1/2 三个 binding，导致绑定时不兼容。
    layout.overrideSets[PassSetIndex] = true;
    layout.setBindings[PassSetIndex] = {
        MakeParticlePassSetBinding(0, "sceneDepth"),
        MakeParticlePassSetBinding(1, "shadowMap"),
        MakeParticlePassSetBinding(2, "localShadowMap"),
        MakeParticlePassSetBinding(3, "softDepth")};
    return layout;
}

// mesh 可见管线独占 set 1：材质 UBO + albedo/normal/ORM/emissive。
// 其它粒子渲染模式继续使用不含 set 1 的共享 layout，避免无材质管线
// 被要求绑定空材质描述符。
GraphicsPipelineLayoutDesc BuildParticleMeshLayout()
{
    GraphicsPipelineLayoutDesc layout = BuildParticleSharedLayout();
    layout.overrideSets[kParticleMaterialSetIndex] = true;
    std::vector<ShaderBinding>& bindings =
        layout.setBindings[kParticleMaterialSetIndex];
    ShaderBinding materialUniform;
    materialUniform.set = kParticleMaterialSetIndex;
    materialUniform.binding = kParticleMaterialUniformBinding;
    materialUniform.type = vk::DescriptorType::eUniformBuffer;
    materialUniform.descriptorCount = 1;
    materialUniform.stageFlags =
        vk::ShaderStageFlagBits::eVertex |
        vk::ShaderStageFlagBits::eFragment;
    materialUniform.name = "ParticleMaterialUniform";
    bindings.push_back(std::move(materialUniform));
    const char* textureNames[] = {
        "albedoMap",
        "normalMap",
        "ormMap",
        "emissiveMap"};
    for (uint32_t index = 0;
         index < kParticleMaterialTextureCount;
         ++index)
    {
        ShaderBinding texture;
        texture.set = kParticleMaterialSetIndex;
        texture.binding = kParticleMaterialAlbedoBinding + index;
        texture.type = vk::DescriptorType::eCombinedImageSampler;
        texture.descriptorCount = 1;
        texture.stageFlags = vk::ShaderStageFlagBits::eFragment;
        texture.name = textureNames[index];
        bindings.push_back(std::move(texture));
    }
    return layout;
}

// 骨骼完整实例（mesh 模式 + 骨骼资产）额外独占 set 2：一个只读关节
// palette SSBO。palette 按 swapchain image 写，顶点着色器读取当前姿态。
GraphicsPipelineLayoutDesc BuildParticleSkeletalLayout()
{
    GraphicsPipelineLayoutDesc layout = BuildParticleMeshLayout();
    layout.overrideSets[kParticleSkinSetIndex] = true;
    std::vector<ShaderBinding>& bindings =
        layout.setBindings[kParticleSkinSetIndex];
    ShaderBinding palette;
    palette.set = kParticleSkinSetIndex;
    palette.binding = kParticleSkinPaletteBinding;
    palette.type = vk::DescriptorType::eStorageBuffer;
    palette.descriptorCount = 1;
    palette.stageFlags = vk::ShaderStageFlagBits::eVertex;
    palette.name = "ParticleSkinPalette";
    bindings.push_back(std::move(palette));
    ShaderBinding influences;
    influences.set = kParticleSkinSetIndex;
    influences.binding = kParticleSkinInfluenceBinding;
    influences.type = vk::DescriptorType::eStorageBuffer;
    influences.descriptorCount = 1;
    influences.stageFlags = vk::ShaderStageFlagBits::eVertex;
    influences.name = "ParticleSkinInfluences";
    bindings.push_back(std::move(influences));
    return layout;
}

std::shared_ptr<Texture> CreateParticleSolidTexture(
    RendererBackendVulkan& backend,
    const std::array<uint8_t, 4>& rgba,
    HostImage::PixelFormat pixelFormat,
    HostImage::TextureSemantic semantic,
    const char* debugName)
{
    HostImage image;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.format = pixelFormat;
    image.semantic = semantic;
    image.rowStrideBytes = 4;
    image.data = {rgba[0], rgba[1], rgba[2], rgba[3]};
    Texture::CreateDesc desc;
    desc.semantic = semantic;
    desc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    desc.generateMipmaps = false;
    desc.filter = vk::Filter::eLinear;
    desc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    desc.debugName = debugName;
    return std::make_shared<Texture>(backend, image, desc);
}

ParticleRenderIdentity BuildParticleRenderIdentity(
    const RenderGraph& graph,
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

} // namespace VL
