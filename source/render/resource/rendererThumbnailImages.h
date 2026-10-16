#pragma once

#include <memory>
#include <unordered_map>

#include <vulkan/vulkan.hpp>

#include "render/resource/rendererGpuImageToken.h"

namespace VL
{

class RendererBackendVulkan;

// Backend 独占缩略图成图；GT 只在稳定帧边界录制拷贝，RT 按逻辑 token 借用采样描述。
// 成图不依赖预览会话或 swapchain；最后一个 token 释放后按主帧 epoch 退休。
class RendererThumbnailImages
{
public:
    explicit RendererThumbnailImages(RendererBackendVulkan& backend);
    ~RendererThumbnailImages();
    // source 已处于 TransferSrc；调用者等提交完成后才可发布返回的 token。
    std::shared_ptr<const RendererGpuImageToken> RecordCopy(
        vk::CommandBuffer command, vk::Image source, uint32_t width, uint32_t height);
    vk::DescriptorImageInfo GetDescriptor(uint64_t id, bool linearSampling) const;
    void CollectUnused();

private:
    struct Image;
    struct Entry
    {
        std::weak_ptr<const RendererGpuImageToken> token;
        std::shared_ptr<Image> image;
    };
    RendererBackendVulkan& backend;
    uint64_t nextId = 1;
    std::unordered_map<uint64_t, Entry> images;
};

} // namespace VL
