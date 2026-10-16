#include "render/resource/rendererThumbnailImages.h"

#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/resourceRetireQueue.h"

namespace VL
{

struct RendererThumbnailImages::Image
{
    explicit Image(RendererBackendVulkan& backend) : backend(backend) {}
    ~Image()
    {
        backend.DestroyImageView(encodedView);
        backend.DestroyImageResource(image, memory, linearView, sampler);
    }
    RendererBackendVulkan& backend;
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView linearView;
    vk::ImageView encodedView;
    vk::Sampler sampler;
};

RendererThumbnailImages::RendererThumbnailImages(RendererBackendVulkan& backend)
    : backend(backend) {}
RendererThumbnailImages::~RendererThumbnailImages() = default;

std::shared_ptr<const RendererGpuImageToken> RendererThumbnailImages::RecordCopy(
    vk::CommandBuffer command, vk::Image source, uint32_t width, uint32_t height)
{
    CollectUnused();
    const auto token = std::make_shared<const RendererGpuImageToken>(nextId++, width, height);
    auto image = std::make_shared<Image>(backend);
    const std::string name = "ThumbnailImage: " + std::to_string(token->id);
    // 拷贝保留 toneMapping 输出的 sRGB 字节；两种兼容 view 分别服务
    // sRGB framebuffer 的线性混合和 UNORM framebuffer 的编码域 UI。
    vk::ImageCreateInfo create;
    create.setFlags(vk::ImageCreateFlagBits::eMutableFormat)
        .setImageType(vk::ImageType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Srgb)
        .setExtent({width, height, 1}).setMipLevels(1).setArrayLayers(1)
        .setSamples(vk::SampleCountFlagBits::e1).setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled)
        .setSharingMode(vk::SharingMode::eExclusive).setInitialLayout(vk::ImageLayout::eUndefined);
    const auto resource = backend.CreateImage(create, vk::MemoryPropertyFlagBits::eDeviceLocal, name);
    image->image = resource.first;
    image->memory = resource.second;
    image->linearView = backend.Create2DImageView(image->image, 1, vk::Format::eR8G8B8A8Srgb,
        vk::ImageAspectFlagBits::eColor, name + ":sRGB");
    image->encodedView = backend.Create2DImageView(image->image, 1, vk::Format::eR8G8B8A8Unorm,
        vk::ImageAspectFlagBits::eColor, name + ":UNORM");
    image->sampler = backend.Create2DSampler(vk::Filter::eLinear,
        vk::SamplerAddressMode::eClampToEdge, false, name);
    images.emplace(token->id, Entry{token, image});

    vk::ImageMemoryBarrier barrier;
    barrier.setImage(image->image).setOldLayout(vk::ImageLayout::eUndefined)
        .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);
    vk::ImageCopy copy;
    copy.setSrcSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1})
        .setDstSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1})
        .setExtent({width, height, 1});
    command.copyImage(source, vk::ImageLayout::eTransferSrcOptimal,
        image->image, vk::ImageLayout::eTransferDstOptimal, copy);
    barrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, barrier);
    return token;
}

vk::DescriptorImageInfo RendererThumbnailImages::GetDescriptor(uint64_t id, bool linearSampling) const
{
    const auto& image = *images.at(id).image;
    return {image.sampler, linearSampling ? image.linearView : image.encodedView,
        vk::ImageLayout::eShaderReadOnlyOptimal};
}

void RendererThumbnailImages::CollectUnused()
{
    for (auto it = images.begin(); it != images.end();)
    {
        if (!it->second.token.expired()) { ++it; continue; }
        // UI 快照和待退休 descriptor 都固定 token；最后一个引用消失后，
        // 仍经过主帧 epoch，不能因 Refresh/LRU 淘汰直接销毁在途图像。
        ResourceRetireQueue::GetInstance().RetireShared(
            "ThumbnailImage: " + std::to_string(it->first), 0,
            ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch(), std::move(it->second.image));
        it = images.erase(it);
    }
}

} // namespace VL
