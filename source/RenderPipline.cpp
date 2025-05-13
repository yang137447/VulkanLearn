#include "RenderPipline.h"

RenderPipline::RenderPipline(vk::Device &device, vk::RenderPass &renderPass, vk::PhysicalDeviceMemoryProperties &gpuMemoryProperties, )
{
    this->device = &device;
    this->renderPass = &renderPass;
    this->gpuMemoryProperties = &gpuMemoryProperties;

    CreateUniformBuffer();
    CreatePipelineLayout();
}

RenderPipline::~RenderPipline()
{
    DestroyGraphicsPipeline();
    DestroyUniformBuffer();
}

RenderPipline::RenderPipline()
{
}

void RenderPipline::CreateUniformBuffer()
{
    uniformBufferSize = sizeof(float) * 16;
    vk::BufferCreateInfo bufferCreateInfo;
    bufferCreateInfo
        .setSize(uniformBufferSize)
        .setUsage(vk::BufferUsageFlagBits::eUniformBuffer)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setFlags(vk::BufferCreateFlagBits::eSparseBinding);
    
    vk::Result result = device->createBuffer(&bufferCreateInfo, nullptr, &uniformBuffer);
    assert(result != vk::Result::eSuccess);

    vk::MemoryRequirements memoryRequirements;
    device->getBufferMemoryRequirements(uniformBuffer, &memoryRequirements);

    vk::MemoryAllocateInfo memoryAllocateInfo;
    memoryAllocateInfo
        .setAllocationSize(memoryRequirements.size)
        .setMemoryTypeIndex(0);

    vk::Flags requiredMask = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    for (uint32_t i = 0; i < gpuMemoryProperties->memoryTypeCount; i++)
    {
        if ((memoryRequirements.memoryTypeBits & (1 << i)) && (gpuMemoryProperties->memoryTypes[i].propertyFlags & requiredMask) == requiredMask)
        {
            memoryAllocateInfo.setMemoryTypeIndex(i);
            break;
        }
    }

    result = device->allocateMemory(&memoryAllocateInfo, nullptr, &uniformBufferMemory);
    assert(result != vk::Result::eSuccess);

    device->bindBufferMemory(uniformBuffer, uniformBufferMemory, 0);
    void* data = device->mapMemory(uniformBufferMemory, 0, uniformBufferSize);
    device->unmapMemory(uniformBufferMemory);
    device->bindBufferMemory(uniformBuffer, uniformBufferMemory, 0);

    uniformBufferInfo
        .setBuffer(uniformBuffer)
        .setOffset(0)
        .setRange(uniformBufferSize);
}

void RenderPipline::DestroyUniformBuffer()
{
    device->destroyBuffer(uniformBuffer, nullptr);
    device->freeMemory(uniformBufferMemory, nullptr);
}

void RenderPipline::CreatePipelineLayout()
{
    vk::DescriptorSetLayoutBinding descriptorSetLayoutBindings[1];
    descriptorSetLayoutBindings[0]
        .setBinding(0)
        .setDescriptorType(vk::DescriptorType::eUniformBuffer)
        .setDescriptorCount(1)
        .setStageFlags(vk::ShaderStageFlagBits::eVertex)
        .setPImmutableSamplers(nullptr);

    vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    descriptorSetLayoutCreateInfo
        .setBindingCount(1)
        .setPBindings(descriptorSetLayoutBindings);
    
    vk::Result result = device->createDescriptorSetLayout(&descriptorSetLayoutCreateInfo, nullptr, descriptorSetLayouts.data());
    assert(result != vk::Result::eSuccess);

    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
    pipelineLayoutCreateInfo
        .setSetLayoutCount(static_cast<uint32_t>(descriptorSetLayouts.size()))
        .setPSetLayouts(descriptorSetLayouts.data())
        .setPushConstantRangeCount(0)
        .setPPushConstantRanges(nullptr);
    
    result = device->createPipelineLayout(&pipelineLayoutCreateInfo, nullptr, &pipelineLayout);
    assert(result != vk::Result::eSuccess);
}

void RenderPipline::DestroyPipelineLayout()
{
    device->destroyPipelineLayout(pipelineLayout, nullptr);
    for (auto descriptorSetLayout : descriptorSetLayouts)
    {
        device->destroyDescriptorSetLayout(descriptorSetLayout, nullptr);
    }
    descriptorSetLayouts.clear();
}
