#include "render/camera/autoExposureCompute.h"

#include <cstring>
#include <stdexcept>
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "renderGraph.h"

namespace VL
{
std::string AutoExposureCompute::ShaderName(size_t stage)
{
    return stage == 0 ? "pass/autoExposureHistogram" : "pass/autoExposureAdapt";
}

AutoExposureCompute::~AutoExposureCompute()
{
    if (!backend) return;
    for (auto& pool : pools) backend->DestroyDescriptorPool(pool);
    for (auto& image : images)
    {
        DestroyBuffer(image.params);
        DestroyBuffer(image.histogram);
    }
    DestroyBuffer(history);
}

void AutoExposureCompute::DestroyBuffer(Buffer& buffer)
{
    if (buffer.mapped) backend->UnmapMemory(buffer.memory);
    if (buffer.buffer) backend->DestroyBuffer(buffer.buffer, buffer.memory);
    buffer = {};
}

void AutoExposureCompute::Initialize(PipelineFactory& factory,
    RendererBackendVulkan& renderer, const RenderGraph& graph)
{
    backend = &renderer;
    const auto& pass = graph.RequireUniquePass(RenderGraphPassType::AutoExposure);
    const auto& scene = graph.GetResourcesResolve().at(pass.inputResources.at(0));
    const auto& output = graph.GetResourcesResolve().at(pass.outputResources.at(0));
    width = scene.at(0).width;
    height = scene.at(0).height;
    // 临时候选 Compute 不污染 active factory cache；失败由本 owner 清理全部部分资源。
    for (size_t stage = 0; stage < pipelines.size(); ++stage)
        pipelines[stage] = factory.CreateComputePipelineCandidate(ShaderName(stage), &artifacts[stage]);

    auto historyAllocation = renderer.CreateBuffer(sizeof(float) * 2,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal, "AutoExposureHistory");
    history.buffer = historyAllocation.first;
    history.memory = historyAllocation.second;
    images.resize(renderer.GetSwapchainImageCount());
    for (size_t index = 0; index < images.size(); ++index)
    {
        auto& image = images[index];
        const auto paramsAllocation = renderer.CreateBuffer(sizeof(AutoExposureDispatchParams),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "AutoExposureParams:" + std::to_string(index));
        image.params.buffer = paramsAllocation.first;
        image.params.memory = paramsAllocation.second;
        image.params.mapped = renderer.MapMemory(image.params.memory, sizeof(AutoExposureDispatchParams));
        const auto histogramAllocation = renderer.CreateBuffer(sizeof(uint32_t) * AutoExposureBinCount,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, "AutoExposureHistogram:" + std::to_string(index));
        image.histogram.buffer = histogramAllocation.first;
        image.histogram.memory = histogramAllocation.second;
        image.sceneColor = vk::DescriptorImageInfo(scene.at(index).sampler,
            scene.at(index).GetShaderDescriptorView(), vk::ImageLayout::eShaderReadOnlyOptimal);
        image.output = vk::DescriptorImageInfo({}, output.at(index).GetShaderDescriptorView(),
            vk::ImageLayout::eGeneral);
    }
    for (size_t stage = 0; stage < pipelines.size(); ++stage)
    {
        auto package = BuildDescriptors(stage, pipelines[stage]);
        pools[stage] = package.descriptorPool;
        sets[stage] = std::move(package.descriptorSets);
    }
}

ComputeDescriptorReplacement AutoExposureCompute::BuildDescriptors(size_t stage,
    const std::shared_ptr<ComputePipeline>& pipeline) const
{
    const auto count = static_cast<uint32_t>(images.size());
    std::vector<vk::DescriptorPoolSize> sizes{
        {vk::DescriptorType::eUniformBuffer, count},
        {vk::DescriptorType::eStorageBuffer, stage == 0 ? count : count * 2}};
    sizes.emplace_back(stage == 0 ? vk::DescriptorType::eCombinedImageSampler :
        vk::DescriptorType::eStorageImage, count);
    ComputeDescriptorReplacement package;
    vk::DescriptorPoolCreateInfo info;
    info.setPoolSizes(sizes).setMaxSets(count);
    package.descriptorPool = backend->CreateDescriptorPool(info, "AutoExposureDescriptors");
    try
    {
        std::vector<vk::DescriptorSetLayout> layouts(count, pipeline->GetDescriptorSetLayouts().at(0));
        vk::DescriptorSetAllocateInfo allocate;
        allocate.setDescriptorPool(package.descriptorPool).setSetLayouts(layouts);
        package.descriptorSets.resize(count);
        backend->AllocateDescriptorSets(allocate, package.descriptorSets);
        for (size_t index = 0; index < images.size(); ++index)
        {
            const auto& image = images[index];
            vk::DescriptorBufferInfo paramsInfo(image.params.buffer, 0, sizeof(AutoExposureDispatchParams));
            vk::DescriptorBufferInfo histogramInfo(image.histogram.buffer, 0, sizeof(uint32_t) * AutoExposureBinCount);
            vk::DescriptorBufferInfo historyInfo(history.buffer, 0, sizeof(float) * 2);
            const vk::DescriptorSet set = package.descriptorSets[index];
            std::vector<vk::WriteDescriptorSet> writes;
            vk::WriteDescriptorSet paramsWrite;
            paramsWrite.setDstSet(set).setDstBinding(0).setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer).setBufferInfo(paramsInfo);
            // Histogram shader 没有相机 UBO；仅 adaptation 声明并消费冻结参数。
            if (stage == 1) writes.push_back(paramsWrite);
            vk::WriteDescriptorSet histogramWrite;
            histogramWrite.setDstSet(set).setDstBinding(2).setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(histogramInfo);
            writes.push_back(histogramWrite);
            if (stage == 0)
            {
                vk::WriteDescriptorSet sceneWrite;
                sceneWrite.setDstSet(set).setDstBinding(1).setDescriptorCount(1)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler).setImageInfo(image.sceneColor);
                writes.push_back(sceneWrite);
            }
            else
            {
                vk::WriteDescriptorSet historyWrite;
                historyWrite.setDstSet(set).setDstBinding(3).setDescriptorCount(1)
                    .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(historyInfo);
                writes.push_back(historyWrite);
                vk::WriteDescriptorSet outputWrite;
                outputWrite.setDstSet(set).setDstBinding(4).setDescriptorCount(1)
                    .setDescriptorType(vk::DescriptorType::eStorageImage).setImageInfo(image.output);
                writes.push_back(outputWrite);
            }
            backend->UpdateDescriptorSets(writes);
        }
    }
    catch (...)
    {
        backend->DestroyDescriptorPool(package.descriptorPool);
        throw;
    }
    return package;
}

void AutoExposureCompute::Record(vk::CommandBuffer commandBuffer, uint32_t imageIndex,
    const CameraExposure& camera, float deltaSeconds)
{
    auto& image = images.at(imageIndex);
    const auto params = sequence.Prepare(camera, deltaSeconds);
    std::memcpy(image.params.mapped, &params, sizeof(params));

    // 同一 graphics queue 的跨提交依赖：先前 exposure history 写入/读取必须完成，
    // 新帧才可复用 ping-pong 槽；不依赖 image acquisition 顺序或 CPU waitIdle。
    vk::MemoryBarrier historyBarrier;
    historyBarrier.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader, {}, historyBarrier, nullptr, nullptr);

    if (camera.mode == ExposureMode::Auto)
    {
        vk::BufferMemoryBarrier clearBarrier;
        clearBarrier.setBuffer(image.histogram.buffer).setOffset(0).setSize(VK_WHOLE_SIZE)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setSrcAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite)
            .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eTransfer, {}, nullptr, clearBarrier, nullptr);
        commandBuffer.fillBuffer(image.histogram.buffer, 0, VK_WHOLE_SIZE, 0);
        clearBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eComputeShader, {}, nullptr, clearBarrier, nullptr);
        pipelines[0]->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            pipelines[0]->GetPipelineLayout(), 0, sets[0].at(imageIndex), nullptr);
        pipelines[0]->Dispatch(commandBuffer, AutoExposureDispatchGroups(width),
            AutoExposureDispatchGroups(height), 1);
        clearBarrier.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eComputeShader, {}, nullptr, clearBarrier, nullptr);
    }
    pipelines[1]->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
        pipelines[1]->GetPipelineLayout(), 0, sets[1].at(imageIndex), nullptr);
    pipelines[1]->Dispatch(commandBuffer, 1, 1, 1);
    recordedMode = camera.mode;
}

void AutoExposureCompute::OnFrameSubmitted() noexcept
{
    sequence.Advance(recordedMode);
}

std::shared_ptr<ComputePipeline> AutoExposureCompute::GetPipeline(size_t stage) const
{
    return pipelines.at(stage);
}
const ComputeShaderArtifact& AutoExposureCompute::GetArtifact(size_t stage) const
{
    return artifacts.at(stage);
}
ComputeDescriptorReplacement AutoExposureCompute::PrepareDescriptors(size_t stage,
    const std::shared_ptr<ComputePipeline>& pipeline) const
{
    auto package = BuildDescriptors(stage, pipeline);
    // 与既有 Compute participant 相同：候选失败仅清理新 pool，旧 pool 直到整批 commit 后按 epoch 退休。
    try
    {
        package.release = [renderer = backend, pool = package.descriptorPool]() mutable
        { renderer->DestroyDescriptorPool(pool); };
        package.retirement = MakePreparedRetiredResourcePackage(
            [renderer = backend, pool = pools.at(stage)]() mutable
            { renderer->DestroyDescriptorPool(pool); });
    }
    catch (...)
    {
        backend->DestroyDescriptorPool(package.descriptorPool);
        throw;
    }
    return package;
}
void AutoExposureCompute::CommitReplacement(size_t stage, ComputeShaderArtifact artifact,
    std::shared_ptr<ComputePipeline> pipeline, ComputeDescriptorReplacement&& descriptors) noexcept
{
    pools[stage] = descriptors.descriptorPool;
    sets[stage] = std::move(descriptors.descriptorSets);
    pipelines[stage] = std::move(pipeline);
    artifacts[stage] = std::move(artifact);
}

std::string AutoExposureReloadParticipant::GetShaderName() const
{
    return AutoExposureCompute::ShaderName(stage);
}
std::shared_ptr<ComputePipeline> AutoExposureReloadParticipant::GetActivePipeline() const
{
    const auto& owner = RenderGraph::GetInstance().GetAutoExposure();
    return owner ? owner->GetPipeline(stage) : nullptr;
}
const ComputeShaderArtifact& AutoExposureReloadParticipant::GetActiveArtifact() const
{
    const auto& owner = RenderGraph::GetInstance().GetAutoExposure();
    static const ComputeShaderArtifact empty;
    return owner ? owner->GetArtifact(stage) : empty;
}
ComputeDescriptorReplacement AutoExposureReloadParticipant::PrepareReplacementDescriptors(
    const ComputeShaderArtifact&, const std::shared_ptr<ComputePipeline>& pipeline) const
{
    return RenderGraph::GetInstance().GetAutoExposure()->PrepareDescriptors(stage, pipeline);
}
void AutoExposureReloadParticipant::CommitReplacement(ComputeShaderArtifact artifact,
    std::shared_ptr<ComputePipeline> pipeline, ComputeDescriptorReplacement&& descriptors) noexcept
{
    RenderGraph::GetInstance().GetAutoExposure()->CommitReplacement(stage,
        std::move(artifact), std::move(pipeline), std::move(descriptors));
}
}
