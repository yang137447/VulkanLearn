#pragma once

#include "terrain/terrainRvtBudget.h"
#include "pipeline/renderPassCompatibilityKey.h"
#include <array>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace VL
{
class RendererBackendVulkan;

struct TerrainLitRect
{
    uint32_t x=0,y=0,width=128,height=128,fullWidth=1280,fullHeight=720;
};

struct TerrainLitTargetChannel
{
    vk::Image image;
    vk::ImageView view;
    vk::Format format=vk::Format::eUndefined;
    vk::ImageAspectFlags aspect=vk::ImageAspectFlagBits::eColor;
    vk::SampleCountFlagBits samples=vk::SampleCountFlagBits::e1;
    vk::ImageLayout finalLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
};

class TerrainLitTarget
{
public:
    TerrainLitTarget(RendererBackendVulkan& backend,std::shared_ptr<TerrainRvtBudgetOwner> actualWorkspace,
        const RenderPassCompatibilityKey& exactPassCompatibility,TerrainLitRect rect);
    ~TerrainLitTarget();
    void Begin(vk::CommandBuffer command,const std::vector<vk::ClearValue>& actualClearValues);
    void End(vk::CommandBuffer command);
    vk::RenderPass RenderPass()const;
    const TerrainLitRect& Rect()const;
    const TerrainLitTargetChannel& Channel(uint32_t index)const;
    uint64_t ActualGpuBytes()const;
    std::shared_ptr<const void> Owner()const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

struct TerrainLitReadbackView
{
    const void* data=nullptr;
    uint64_t payloadBytes=0,submissionEpoch=0,completedEpoch=0;
    uint32_t width=0,height=0,bytesPerTexel=0;
    vk::Format format=vk::Format::eUndefined;
    std::shared_ptr<const void> owner;
};

class TerrainLitReadback
{
public:
    TerrainLitReadback(RendererBackendVulkan& backend,std::shared_ptr<TerrainRvtBudgetOwner> actualWorkspace,
        TerrainLitRect rect,vk::Format actualFormat);
    ~TerrainLitReadback();
    void Record(vk::CommandBuffer command,const TerrainLitTargetChannel& source,
        std::shared_ptr<const void> actualSourceOwner,uint64_t actualSubmissionEpoch);
    std::shared_ptr<const TerrainLitReadbackView> TryConsume(uint64_t actualCompletedEpoch);
    std::shared_ptr<const void> Owner()const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
