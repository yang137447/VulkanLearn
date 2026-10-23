#include "render/skinning/skinDeformPass.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererObjectGpuResources.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/skinning/skinDeformLayout.h"

namespace
{
constexpr uint32_t ParamsBinding = 0;
constexpr uint32_t PaletteBinding = 1;
constexpr uint32_t InfluencesBinding = 2;
constexpr uint32_t SourceVerticesBinding = 3;
constexpr uint32_t DeformedVerticesBinding = 4;
constexpr uint32_t BindingCount = 5;

// 与 shader/glsl/engine/skinnedDeform.comp 的 descriptor 声明一一对应。
constexpr std::array<vk::DescriptorType, BindingCount> BindingTypes{
    vk::DescriptorType::eUniformBuffer,
    vk::DescriptorType::eStorageBuffer,
    vk::DescriptorType::eStorageBuffer,
    vk::DescriptorType::eStorageBuffer,
    vk::DescriptorType::eStorageBuffer};

// 每池按 32 个 set 预留（一个对象每个 image 一个 set）；对象数量在图构建阶段
// 未知，池链只在溢出时增长。
constexpr uint32_t DescriptorSetsPerPool = 32;

// dispatch 前：palette 是 host 刚写的数据；输出 buffer 上一帧被 vertex shader
// 读过；源顶点流在建对象时由 transfer 写入。三类依赖都显式声明，不依赖提交
// 顺序或 image fence 的隐含可见性。
void RecordPreDispatchBarrier(vk::CommandBuffer commandBuffer)
{
    vk::MemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eHostWrite |
            vk::AccessFlagBits::eTransferWrite |
            vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eHost |
            vk::PipelineStageFlagBits::eTransfer |
            vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {}, barrier, nullptr, nullptr);
}

// dispatch 后：本帧 Shadow/Geometry/Forward 的 vertex shader 读取输出 buffer。
// 输出是单份 buffer，跨帧复用的 WAR 由下一帧 RecordPreDispatchBarrier 覆盖。
void RecordPostDispatchBarrier(vk::CommandBuffer commandBuffer)
{
    vk::MemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eVertexShader,
        {}, barrier, nullptr, nullptr);
}
} // namespace

namespace VL::Skinning
{

SkinDeformPass::~SkinDeformPass()
{
    ResetDescriptorCache();
    pipeline.reset();
    backend = nullptr;
}

void SkinDeformPass::Initialize(PipelineFactory& factory, RendererBackendVulkan& renderer)
{
    backend = &renderer;
    // Candidate 入口不写 active compute cache；本 pipeline 也不注册 hot reload participant。
    pipeline = factory.CreateComputePipelineCandidate("engine/skinnedDeform");
    initialized = true;
}

std::vector<vk::DescriptorSet> SkinDeformPass::AcquireDescriptorSets(uint32_t count)
{
    if (count > DescriptorSetsPerPool)
        throw std::runtime_error("SkinDeform image slot count exceeds the descriptor pool budget");
    for (DescriptorPoolBudget& budget : descriptorPools)
    {
        if (budget.remainingSets >= count)
        {
            budget.remainingSets -= count;
            std::vector<vk::DescriptorSetLayout> layouts(
                count, pipeline->GetDescriptorSetLayouts().at(0));
            vk::DescriptorSetAllocateInfo allocate;
            allocate.setDescriptorPool(budget.pool).setSetLayouts(layouts);
            std::vector<vk::DescriptorSet> allocated(count);
            backend->AllocateDescriptorSets(allocate, allocated);
            return allocated;
        }
    }

    const std::vector<vk::DescriptorPoolSize> sizes{
        {vk::DescriptorType::eUniformBuffer, DescriptorSetsPerPool},
        {vk::DescriptorType::eStorageBuffer, DescriptorSetsPerPool * 4}};
    vk::DescriptorPoolCreateInfo info;
    info.setPoolSizes(sizes).setMaxSets(DescriptorSetsPerPool);
    const vk::DescriptorPool pool =
        backend->CreateDescriptorPool(info, "SkinDeformDescriptors");
    descriptorPools.push_back({pool, DescriptorSetsPerPool - count});

    std::vector<vk::DescriptorSetLayout> layouts(
        count, pipeline->GetDescriptorSetLayouts().at(0));
    vk::DescriptorSetAllocateInfo allocate;
    allocate.setDescriptorPool(pool).setSetLayouts(layouts);
    std::vector<vk::DescriptorSet> allocated(count);
    backend->AllocateDescriptorSets(allocate, allocated);
    return allocated;
}

void SkinDeformPass::CreateObjectDescriptors(
    const RendererObjectGpuResources& resources,
    ObjectDescriptors& descriptors)
{
    const uint32_t imageCount =
        static_cast<uint32_t>(resources.skinPalette.buffers.size());
    descriptors.sets = AcquireDescriptorSets(imageCount);

    // vulkan-hpp 的 setBufferInfo 只保存指针，DescriptorBufferInfo 必须活到
    // UpdateDescriptorSets 返回；infos 在写入循环期间不再扩容。
    std::vector<std::array<vk::DescriptorBufferInfo, BindingCount>> infos(imageCount);
    for (uint32_t image = 0; image < imageCount; ++image)
    {
        infos[image][ParamsBinding] = resources.skinDeformParams.bufferInfos.at(0);
        infos[image][PaletteBinding] = resources.skinPalette.bufferInfos.at(image);
        infos[image][InfluencesBinding] = resources.skinInfluences;
        infos[image][SourceVerticesBinding] = resources.skinSourceVertices;
        infos[image][DeformedVerticesBinding] = resources.skinDeform.bufferInfos.at(0);
    }

    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(static_cast<size_t>(imageCount) * BindingCount);
    for (uint32_t image = 0; image < imageCount; ++image)
    {
        for (uint32_t binding = 0; binding < BindingCount; ++binding)
        {
            vk::WriteDescriptorSet write;
            write.setDstSet(descriptors.sets[image])
                .setDstBinding(binding)
                .setDescriptorType(BindingTypes[binding])
                .setBufferInfo(infos[image][binding]);
            writes.push_back(write);
        }
    }
    backend->UpdateDescriptorSets(writes);

    descriptors.paletteBuffers.clear();
    descriptors.paletteBuffers.reserve(imageCount);
    for (uint32_t image = 0; image < imageCount; ++image)
        descriptors.paletteBuffers.push_back(resources.skinPalette.buffers.at(image));
    descriptors.deformedBuffer = resources.skinDeform.buffers.at(0);
    descriptors.influences = resources.skinInfluences.buffer;
    descriptors.sourceVertices = resources.skinSourceVertices.buffer;
    descriptors.params = resources.skinDeformParams.buffers.at(0);
}

SkinDeformPass::ObjectDescriptors& SkinDeformPass::EnsureObjectDescriptors(
    const VL::RendererObjectGpuResources& resources)
{
    for (size_t index = 0; index < objectDescriptors.size(); ++index)
    {
        auto& entry = objectDescriptors[index];
        if (entry.first != &resources)
            continue;
        ObjectDescriptors& descriptors = entry.second;
        bool matches = descriptors.sets.size() == resources.skinPalette.buffers.size() &&
            descriptors.paletteBuffers.size() == resources.skinPalette.buffers.size() &&
            descriptors.deformedBuffer == resources.skinDeform.buffers.at(0) &&
            descriptors.influences == resources.skinInfluences.buffer &&
            descriptors.sourceVertices == resources.skinSourceVertices.buffer &&
            !resources.skinDeformParams.buffers.empty() &&
            descriptors.params == resources.skinDeformParams.buffers.at(0);
        for (size_t image = 0; image < descriptors.paletteBuffers.size() && matches; ++image)
        {
            matches = descriptors.paletteBuffers[image] ==
                resources.skinPalette.buffers.at(image);
        }
        if (matches)
            return descriptors;

        // 对象资源包已经换代；旧 set 留在池里统一 reset 时释放，不单独 free。
        objectDescriptors.erase(objectDescriptors.begin() +
            static_cast<std::ptrdiff_t>(index));
        break;
    }

    ObjectDescriptors descriptors;
    CreateObjectDescriptors(resources, descriptors);
    objectDescriptors.emplace_back(&resources, std::move(descriptors));
    return objectDescriptors.back().second;
}

bool SkinDeformPass::HasWork(const VL::ResolvedRenderScene& scene) const
{
    if (!initialized || !pipeline)
        return false;

    for (const ResolvedMaterialGroup& materialGroup : scene.materialGroups)
    {
        for (const ResolvedMaterialInstanceGroup& instanceGroup : materialGroup.materialInstances)
        {
            for (const ResolvedDrawPacket& draw : instanceGroup.draws)
            {
                if (draw.objectResourceEntry &&
                    draw.objectResourceEntry->GetResources().skinDeform.HasResources())
                    return true;
            }
        }
    }
    return false;
}

void SkinDeformPass::Record(vk::CommandBuffer commandBuffer,
    const VL::ResolvedRenderScene& scene,
    uint32_t imageIndex)
{
    if (!initialized || !pipeline)
        return;

    std::vector<const RendererObjectGpuResources*> objects;
    std::unordered_set<const RendererObjectGpuResources*> seen;
    for (const ResolvedMaterialGroup& materialGroup : scene.materialGroups)
    {
        for (const ResolvedMaterialInstanceGroup& instanceGroup : materialGroup.materialInstances)
        {
            for (const ResolvedDrawPacket& draw : instanceGroup.draws)
            {
                if (!draw.objectResourceEntry)
                    continue;
                const RendererObjectGpuResources& resources =
                    draw.objectResourceEntry->GetResources();
                if (!resources.skinDeform.HasResources())
                    continue;
                if (imageIndex >= resources.skinPalette.buffers.size())
                    throw std::out_of_range("SkinDeform swapchain image slot is out of range");
                // 同一对象可能出现在多个材质分组/draw 中；重复 dispatch 会写同一份输出，
                // 但浪费带宽，也破坏“每对象一次”的诊断计数。
                if (seen.insert(&resources).second)
                    objects.push_back(&resources);
            }
        }
    }
    if (objects.empty())
        return;

    if (!reportedFirstDispatch)
    {
        uint32_t totalVertices = 0;
        uint64_t totalGroups = 0;
        uint64_t totalBytes = 0;
        for (const RendererObjectGpuResources* resources : objects)
        {
            totalVertices += resources->skinDeformVertexCount;
            totalGroups += SkinDeformGroupCount(resources->skinDeformVertexCount);
            totalBytes += SkinDeformBufferSizeBytes(resources->skinDeformVertexCount);
        }
        std::cout << "[SkinDeform] GPU pre-skin dispatch: objects=" << objects.size()
                  << " vertices=" << totalVertices
                  << " groups=" << totalGroups
                  << " bytesPerFrame=" << totalBytes
                  << std::endl;
        reportedFirstDispatch = true;
    }

    RecordPreDispatchBarrier(commandBuffer);
    pipeline->Bind(commandBuffer);
    for (const RendererObjectGpuResources* resources : objects)
    {
        ObjectDescriptors& descriptors = EnsureObjectDescriptors(*resources);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            pipeline->GetPipelineLayout(), 0, descriptors.sets[imageIndex], nullptr);
        pipeline->Dispatch(commandBuffer,
            SkinDeformGroupCount(resources->skinDeformVertexCount), 1, 1);
    }
    RecordPostDispatchBarrier(commandBuffer);
}

void SkinDeformPass::ResetDescriptorCache() noexcept
{
    objectDescriptors.clear();
    if (backend)
    {
        for (DescriptorPoolBudget& budget : descriptorPools)
            backend->DestroyDescriptorPool(budget.pool);
        descriptorPools.clear();
    }
    initialized = false;
}

} // namespace VL::Skinning
