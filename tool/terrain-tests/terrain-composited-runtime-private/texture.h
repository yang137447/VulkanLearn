#pragma once

#include "render/backend/rendererBackendVulkan.h"

#include <memory>

class Texture
{
public:
    struct EmptyArrayDesc
    {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t arrayLayers = 1;
        vk::Format format = vk::Format::eR8G8B8A8Unorm;
        vk::Filter filter = vk::Filter::eLinear;
        std::string debugName;
    };

    Texture(VL::RendererBackendVulkan& backend, vk::Image image, vk::DeviceMemory memory,
        vk::ImageView view, vk::Sampler sampler, uint32_t, vk::Format, uint32_t layers)
        : backend(backend), image(image), memory(memory), view(view), sampler(sampler), layers(layers) {}
    ~Texture() { backend.DestroyImageResource(image, memory, view, sampler); }
    vk::Image getImage() const { return image; }
    uint32_t GetArrayLayers() const { return layers; }

private:
    VL::RendererBackendVulkan& backend;
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
    vk::Sampler sampler;
    uint32_t layers;
};
