#include "vulkanDebug.h"
#include "commonFunction.h"
#include <iostream>

PFN_vkCmdBeginDebugUtilsLabelEXT VulkanDebug::pfnCmdBeginDebugUtilsLabelEXT = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT VulkanDebug::pfnCmdEndDebugUtilsLabelEXT = nullptr;
PFN_vkSetDebugUtilsObjectNameEXT VulkanDebug::pfnSetDebugUtilsObjectNameEXT = nullptr;

void VulkanDebug::Init(vk::Instance instance)
{
#if !defined(NDEBUG)
    pfnCmdBeginDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(instance.getProcAddr("vkCmdBeginDebugUtilsLabelEXT"));
    pfnCmdEndDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(instance.getProcAddr("vkCmdEndDebugUtilsLabelEXT"));
    pfnSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(instance.getProcAddr("vkSetDebugUtilsObjectNameEXT"));
#endif
}

void VulkanDebug::BeginRegion(vk::CommandBuffer cmd, const std::string& labelName, DebugCategory category)
{
#if !defined(NDEBUG)
    if (pfnCmdBeginDebugUtilsLabelEXT)
    {
        VkDebugUtilsLabelEXT labelInfo = {};
        labelInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        labelInfo.pLabelName = labelName.c_str();
        std::array<float, 4> colorArray = GetColor(category);
        memcpy(labelInfo.color, colorArray.data(), sizeof(float) * 4);
        pfnCmdBeginDebugUtilsLabelEXT(cmd, &labelInfo);
    }
#endif
}

std::array<float, 4> VulkanDebug::GetColor(DebugCategory category)
{
    static std::array<std::array<float, 4>, 6> cachedColors = []() {
        std::array<std::array<float, 4>, 6> colors;
        // Default colors
        colors[0] = { 0.0f, 1.0f, 0.0f, 1.0f }; // ePass (Green)
        colors[1] = { 0.0f, 0.0f, 1.0f, 1.0f }; // eObject (Blue)
        colors[2] = { 1.0f, 1.0f, 0.0f, 1.0f }; // ePipeline (Yellow)
        colors[3] = { 0.0f, 1.0f, 1.0f, 1.0f }; // eResource (Cyan)
        colors[4] = { 1.0f, 0.0f, 0.0f, 1.0f }; // eWait (Red)
        colors[5] = { 1.0f, 1.0f, 1.0f, 1.0f }; // eDefault (White)

        try
        {
            auto& config = CommonFunction::InitConfigJson();
            if (config.contains("renderStateDebugColors"))
            {
                auto& debugColors = config["renderStateDebugColors"];
                auto load = [&](const char* key, int index) {
                    if (debugColors.contains(key))
                    {
                        auto& c = debugColors[key];
                        if (c.is_array() && c.size() == 4)
                        {
                            colors[index] = { c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>() };
                        }
                    }
                };
                load("ePass", 0);
                load("eObject", 1);
                load("ePipeline", 2);
                load("eResource", 3);
                load("eWait", 4);
                load("eDefault", 5);
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "Warning: Failed to load debug colors: " << e.what() << std::endl;
        }

        return colors;
    }();

    int index = static_cast<int>(category);
    if (index >= 0 && index < 6)
    {
        return cachedColors[index];
    }
    return cachedColors[5]; // eDefault
}

void VulkanDebug::EndRegion(vk::CommandBuffer cmd)
{
#if !defined(NDEBUG)
    if (pfnCmdEndDebugUtilsLabelEXT)
    {
        pfnCmdEndDebugUtilsLabelEXT(cmd);
    }
#endif
}

void VulkanDebug::SetObjectName(vk::Device device, uint64_t object, vk::ObjectType objectType, const std::string& name)
{
#if !defined(NDEBUG)
    if (pfnSetDebugUtilsObjectNameEXT)
    {
        VkDebugUtilsObjectNameInfoEXT nameInfo = {};
        nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        nameInfo.objectType = static_cast<VkObjectType>(objectType);
        nameInfo.objectHandle = object;
        nameInfo.pObjectName = name.c_str();
        pfnSetDebugUtilsObjectNameEXT(device, &nameInfo);
    }
#endif
}
