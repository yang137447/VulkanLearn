#include <vulkan/vulkan.hpp>
#include <iostream>

int main()
{
    try
    {
        const vk::ApplicationInfo application("Terrain descriptor capability query",
            1, "VL read-only physical query", 1, VK_API_VERSION_1_2);
        const auto instance = vk::createInstance(vk::InstanceCreateInfo({}, &application));
        for (const auto physical : instance.enumeratePhysicalDevices())
        {
            vk::PhysicalDeviceDescriptorIndexingProperties indexing;
            vk::PhysicalDeviceProperties2 properties;
            properties.pNext = &indexing;
            physical.getProperties2(&properties);
            vk::PhysicalDeviceVulkan12Features features12;
            vk::PhysicalDeviceFeatures2 features;
            features.pNext = &features12;
            physical.getFeatures2(&features);
            const auto& limits = properties.properties.limits;
            std::cout << "{\"device\":\"" << properties.properties.deviceName
                << "\",\"driverVersion\":" << properties.properties.driverVersion
                << ",\"apiVersion\":" << properties.properties.apiVersion
                << ",\"maxPerStageDescriptorSamplers\":" << limits.maxPerStageDescriptorSamplers
                << ",\"maxPerStageDescriptorSampledImages\":" << limits.maxPerStageDescriptorSampledImages
                << ",\"maxDescriptorSetSamplers\":" << limits.maxDescriptorSetSamplers
                << ",\"maxDescriptorSetSampledImages\":" << limits.maxDescriptorSetSampledImages
                << ",\"maxPerStageResources\":" << limits.maxPerStageResources
                << ",\"maxPerStageDescriptorStorageBuffers\":" << limits.maxPerStageDescriptorStorageBuffers
                << ",\"maxStorageBufferRange\":" << limits.maxStorageBufferRange
                << ",\"maxSamplerAllocationCount\":" << limits.maxSamplerAllocationCount
                << ",\"minStorageBufferOffsetAlignment\":" << limits.minStorageBufferOffsetAlignment
                << ",\"shaderSampledImageArrayDynamicIndexing\":" << features.features.shaderSampledImageArrayDynamicIndexing
                << ",\"descriptorIndexing\":" << features12.descriptorIndexing
                << ",\"shaderSampledImageArrayNonUniformIndexing\":" << features12.shaderSampledImageArrayNonUniformIndexing
                << ",\"runtimeDescriptorArray\":" << features12.runtimeDescriptorArray
                << ",\"descriptorBindingPartiallyBound\":" << features12.descriptorBindingPartiallyBound
                << ",\"maxPerStageDescriptorUpdateAfterBindSamplers\":" << indexing.maxPerStageDescriptorUpdateAfterBindSamplers
                << ",\"scope\":\"physical-device query only; no logical device, queue or GPU submission\"}\n";
        }
        instance.destroy();
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << "\n";
        return 1;
    }
}
