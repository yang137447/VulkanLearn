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

// maxParticles 是作者池大小；排序网络要求 2 的幂，最小 256 以摊薄 dispatch。
constexpr uint32_t kParticleSortMinimumCapacity = 256;

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

} // namespace
} // namespace VL
