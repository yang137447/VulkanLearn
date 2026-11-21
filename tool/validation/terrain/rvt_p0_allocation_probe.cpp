#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <iostream>
#include <string>

int main()
{
    auto instance = vk::createInstance(vk::InstanceCreateInfo{});
    auto physical = instance.enumeratePhysicalDevices().at(0);
    const auto families = physical.getQueueFamilyProperties();
    uint32_t family = 0;
    while (!(families.at(family).queueFlags & vk::QueueFlagBits::eGraphics))
        ++family;
    float priority = 1;
    vk::DeviceQueueCreateInfo queue({}, family, 1, &priority);
    auto device = physical.createDevice(vk::DeviceCreateInfo({}, queue));
    std::cout << "{\"device\":" << '"' << physical.getProperties().deviceName
        << "\",\"scope\":\"vkGetImageMemoryRequirements without allocation or GPU dispatch\",\"images\":[";
    bool first = true;
    const auto image = [&](const std::string& name, uint32_t width, uint32_t height,
                           uint32_t layers, uint32_t mips, vk::Format format,
                           uint32_t texelBytes, uint32_t multiplicity)
    {
        vk::ImageCreateInfo create({}, vk::ImageType::e2D, format, {width, height, 1},
            mips, layers, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
            vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);
        auto handle = device.createImage(create);
        const auto requirements = device.getImageMemoryRequirements(handle);
        uint64_t payload = 0;
        for (uint32_t mip = 0; mip < mips; ++mip)
            payload += uint64_t(std::max(1u, width >> mip)) *
                std::max(1u, height >> mip) * layers * texelBytes;
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << "{\"name\":\"" << name << "\",\"width\":" << width
            << ",\"height\":" << height << ",\"layers\":" << layers
            << ",\"multiplicity\":" << multiplicity << ",\"payloadBytes\":" << payload
            << ",\"allocationBytes\":" << requirements.size
            << ",\"alignmentBytes\":" << requirements.alignment << '}';
        device.destroyImage(handle);
    };
    image("shared-material-pool-albedo", 136, 136, 512, 1, vk::Format::eR8G8B8A8Srgb, 4, 1);
    image("shared-material-pool-normal-orm", 136, 136, 512, 1, vk::Format::eR8G8B8A8Unorm, 4, 2);
    image("old-private-material-albedo", 136, 136, 192, 1, vk::Format::eR8G8B8A8Srgb, 4, 16);
    image("old-private-material-normal-orm", 136, 136, 192, 1, vk::Format::eR8G8B8A8Unorm, 4, 32);
    image("surface-page-table", 64, 64, 14, 1, vk::Format::eR32G32B32A32Sfloat, 16, 16);
    image("frame-input-normal-or-tangent", 513, 513, 1, 1, vk::Format::eR32G32B32A32Sfloat, 16, 32);
    image("shared-coverage-ids-or-weights", 4096, 4096, 1, 1, vk::Format::eR8G8B8A8Unorm, 4, 2);
    image("shared-coverage-table", 32, 32, 1, 1, vk::Format::eR8G8B8A8Unorm, 4, 1);
    image("local-weight-overlay", 515, 515, 7, 1, vk::Format::eR32Sfloat, 4, 16);
    image("source-layer-rgba-mips", 1024, 1024, 7, 11, vk::Format::eR8G8B8A8Unorm, 4, 3);
    image("source-layer-height-mips", 1024, 1024, 7, 11, vk::Format::eR16Unorm, 2, 1);
    image("author-local-weight-mips", 515, 515, 7, 10, vk::Format::eR16Unorm, 2, 16);
    image("global-colour-mips", 2048, 2048, 1, 12, vk::Format::eR8G8B8A8Srgb, 4, 1);
    image("global-masks-mips", 2048, 2048, 1, 12, vk::Format::eR8G8B8A8Unorm, 4, 1);
    image("tiling-height-atlas", 680, 544, 1, 1, vk::Format::eR16Unorm, 2, 16);
    image("tiling-height-layer-map", 7, 1, 1, 1, vk::Format::eR32G32B32A32Sfloat, 16, 16);
    image("source-map", 256, 2, 1, 1, vk::Format::eR32G32B32A32Sfloat, 16, 16);
    image("layer-catalog", 256, 4, 1, 1, vk::Format::eR32G32B32A32Sfloat, 16, 1);
    std::cout << "],\"buffers\":[";
    first = true;
    const auto buffer = [&](const std::string& name, uint64_t bytes,
                            vk::BufferUsageFlags usage, uint32_t multiplicity)
    {
        auto handle = device.createBuffer(vk::BufferCreateInfo({}, bytes, usage));
        const auto requirements = device.getBufferMemoryRequirements(handle);
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << "{\"name\":\"" << name << "\",\"payloadBytes\":" << bytes
            << ",\"allocationBytes\":" << requirements.size
            << ",\"alignmentBytes\":" << requirements.alignment
            << ",\"multiplicity\":" << multiplicity << '}';
        device.destroyBuffer(handle);
    };
    buffer("production-output-with-status", 221956,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
        vk::BufferUsageFlagBits::eTransferDst, 4);
    buffer("upload-ring", 4 * 1024 * 1024, vk::BufferUsageFlagBits::eTransferSrc, 2);
    buffer("feedback-readback", 256 * 1024, vk::BufferUsageFlagBits::eTransferDst, 2);
    buffer("task-parameter-reservation", 16 * 1024, vk::BufferUsageFlagBits::eUniformBuffer, 4);
    std::cout << "]}\n";
    device.destroy();
    instance.destroy();
}
