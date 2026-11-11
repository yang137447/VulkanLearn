#pragma once

#include "vulkan/vulkan.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <Eigen/Dense>
#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <math.h>
#include <optional>
#include <stdexcept>
#include <string>
#include "vulkanDebug.h"
#include "vulkanManager.h"

constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
constexpr uint32_t MAX_DESCRIPTOR_SETS = 4;
constexpr uint32_t GlobalSetIndex = 0;
constexpr uint32_t MaterialSetIndex = 1;
constexpr uint32_t ObjectSetIndex = 2;
constexpr uint32_t PassSetIndex = 3;

namespace JsonParser
{
    inline Eigen::Vector2f ParseVector2(const nlohmann::basic_json<>& Value)
    {
        if(Value.is_array() && Value.size() == 2)
        {
            return Eigen::Vector2f(Value[0].get<float>(), Value[1].get<float>());
        }
        else {
            throw std::runtime_error("Invalid vector2 format");
        }
    }

    inline Eigen::Vector3f ParseVector3(const nlohmann::basic_json<>& Value)
    {
        if(Value.is_array() && Value.size() == 3)
        {
            return Eigen::Vector3f(Value[0].get<float>(), Value[1].get<float>(), Value[2].get<float>());
        }
        else {
            throw std::runtime_error("Invalid vector3 format");
        }
    }

    inline Eigen::Vector4f ParseVector4(const nlohmann::basic_json<>& Value)
    {
        if(Value.is_array() && Value.size() == 4)
        {
            return Eigen::Vector4f(Value[0].get<float>(), Value[1].get<float>(), Value[2].get<float>(), Value[3].get<float>());
        }
        else {
            throw std::runtime_error("Invalid vector4 format");
        }
    }

    inline uint32_t ParseValueSize(const nlohmann::basic_json<>& Value)
    {
        if(Value.is_number())
        {
            return sizeof(float);
        }
        else if(Value.is_array())
        {
            if(Value.size() == 2)
            {
                return sizeof(Eigen::Vector2f);
            }
            else if(Value.size() == 3)
            {
                return sizeof(Eigen::Vector3f);
            }
            else if(Value.size() == 4)
            {
                return sizeof(Eigen::Vector4f);
            }
        }
        throw std::runtime_error("Unsupported parameter type or size");
    }

    template<typename T>
    inline T ParseValue(const nlohmann::basic_json<>& Value)
    {
        if constexpr(std::is_same_v<T, float>)
        {
            return Value.get<float>();
        }
        else if constexpr(std::is_same_v<T, Eigen::Vector2f>)
        {
            return ParseVector2(Value);
        }
        else if constexpr(std::is_same_v<T, Eigen::Vector3f>)
        {
            return ParseVector3(Value);
        }
        else if constexpr(std::is_same_v<T, Eigen::Vector4f>)
        {
            return ParseVector4(Value);    }
        throw std::runtime_error("Unsupported parameter type or size");
    }
}

namespace Algorithm
{
    /**
     * @brief 计算点集合的凸包点
     * 
     * 使用 Andrew 算法计算凸包。
     * 注意：点只使用 xy 分量，忽略 z 分量，退化成二维处理。
     * 
     * @param points 输入的点集合
     * @return std::vector<uint32_t> 凸包点的索引集合
     */
    inline std::vector<uint32_t> ConvexHull(const std::vector<Eigen::Vector3f>& points)
    {
        // 辅助结构体：存储点及其原始索引
        struct PointWithIndex {
            Eigen::Vector3f point;
            uint32_t index;
        };

        size_t n = points.size();
        if (n <= 2) {
            std::vector<uint32_t> result;
            for(size_t i = 0; i < n; ++i) result.push_back(static_cast<uint32_t>(i));
            return result;
        }

        std::vector<PointWithIndex> P(n);
        for (size_t i = 0; i < n; ++i) {
            P[i] = {points[i], static_cast<uint32_t>(i)};
        }

        // 按 x 坐标排序，x 相同按 y 坐标排序
        std::sort(P.begin(), P.end(), [](const PointWithIndex& a, const PointWithIndex& b) {
            return a.point.x() < b.point.x() || (a.point.x() == b.point.x() && a.point.y() < b.point.y());
        });

        // 叉积计算：判断三个点 (O, A, B) 的方向
        // 返回值：> 0: 逆时针 (左转), < 0: 顺时针 (右转), = 0: 共线
        auto CrossProduct = [](const Eigen::Vector3f& O, const Eigen::Vector3f& A, const Eigen::Vector3f& B) {
            return (A.x() - O.x()) * (B.y() - O.y()) - (A.y() - O.y()) * (B.x() - O.x());
        };

        std::vector<PointWithIndex> hull;

        // 构建下凸包
        for (size_t i = 0; i < n; ++i) {
            while (hull.size() >= 2 && CrossProduct(hull[hull.size() - 2].point, hull.back().point, P[i].point) <= 0) {
                hull.pop_back();
            }
            hull.push_back(P[i]);
        }

        // 构建上凸包
        size_t lowerHullSize = hull.size();
        for (int i = static_cast<int>(n) - 2; i >= 0; --i) {
            while (hull.size() > lowerHullSize && CrossProduct(hull[hull.size() - 2].point, hull.back().point, P[i].point) <= 0) {
                hull.pop_back();
            }
            hull.push_back(P[i]);
        }

        // 移除重复的起点（Andrew 算法最后会回到起点）
        if (hull.size() > 1) {
            hull.pop_back();
        }

        // 提取索引
        std::vector<uint32_t> result(hull.size());
        for (size_t i = 0; i < hull.size(); ++i) {
            result[i] = hull[i].index;
        }

        return result;
    }
}

namespace CommonFunction
{
    inline std::filesystem::path FindProjectRoot()
    {
        std::filesystem::path current = std::filesystem::current_path();
        if (std::filesystem::is_regular_file(current))
        {
            current = current.parent_path();
        }

        while (!current.empty())
        {
            const std::filesystem::path configPath = current / "config" / "config.json";
            if (std::filesystem::exists(configPath))
            {
                return current;
            }

            const std::filesystem::path parent = current.parent_path();
            if (parent == current)
            {
                break;
            }
            current = parent;
        }

        throw std::runtime_error(
            "Failed to find config/config.json from current path: " +
            std::filesystem::current_path().string());
    }

    inline std::string GetConfigFolderPath()
    {
        static std::string configFolderPath;
        if( !configFolderPath.empty() )
        {
            return configFolderPath;
        }

        configFolderPath = (FindProjectRoot() / "config").string();

        return configFolderPath;
    }

    inline nlohmann::basic_json<>& InitConfigJson()
    {
        static std::optional<nlohmann::json> configJson;
        if( configJson.has_value() )
        {
            return configJson.value();
        }

        std::string configFolderPath = GetConfigFolderPath();
        std::ifstream configFile(configFolderPath + "/config.json");
        configJson = nlohmann::json();
        configFile >> configJson.value();
        return configJson.value();
    }

    inline nlohmann::basic_json<>& InitRenderGraphJson()
    {
        static std::optional<nlohmann::json> renderGraphJson;
        if( renderGraphJson.has_value() )
        {
            return renderGraphJson.value();
        }

        std::string configFolderPath = GetConfigFolderPath();
        std::ifstream renderGraphFile(configFolderPath + "/renderGraphConfig.json");
        renderGraphJson = nlohmann::json();
        renderGraphFile >> renderGraphJson.value();
        return renderGraphJson.value();
    }

    inline Eigen::Vector2f GetWindowSize()
    {
        static std::optional<Eigen::Vector2f> windowSize;
        if( windowSize.has_value() )
        {
            return windowSize.value();
        }
        const nlohmann::json& configJson = InitConfigJson();
        windowSize = JsonParser::ParseVector2(configJson["windowSize"]);
        return windowSize.value();
    }

    inline vk::SampleCountFlagBits GetMsaaSampleCount()
    {
        static std::optional<vk::SampleCountFlagBits> msaa;
        if( msaa.has_value() )
        {
            return msaa.value();
        }
        const nlohmann::json& configJson = InitConfigJson();
        uint32_t msaaSampleCount = configJson["msaa"];
        switch(msaaSampleCount)
        {
            case 1:
                msaa = vk::SampleCountFlagBits::e1;
                break;
            case 2:
                msaa = vk::SampleCountFlagBits::e2;
                break;
            case 4:
                msaa = vk::SampleCountFlagBits::e4;
                break;
            case 8:
                msaa = vk::SampleCountFlagBits::e8;
                break;
            case 16:
                msaa = vk::SampleCountFlagBits::e16;
                break;
            default:
                msaa = vk::SampleCountFlagBits::e1;
                break;
        }
        return msaa.value();
    }

    inline std::string GetProjectPath()
    {
        static std::optional<std::string> projectPath;
        if( projectPath.has_value() )
        {
            return projectPath.value();
        }
        // Derive the root from the discovered config directory so config.json
        // does not become a second owner of physical project-root lookup.
        projectPath = std::filesystem::path(GetConfigFolderPath()).parent_path().string();
        return projectPath.value();
    }

    inline std::string GetResourcePath()
    {
        static std::optional<std::string> resourcePath;
        if (resourcePath.has_value())
        {
            return resourcePath.value();
        }

        const nlohmann::json& configJson = InitConfigJson();
        if (!configJson.contains("resourcePath") || !configJson["resourcePath"].is_string())
        {
            throw std::runtime_error("config/config.json must define string field \"resourcePath\"");
        }

        resourcePath = configJson["resourcePath"].get<std::string>();
        return resourcePath.value();
    }

    inline std::string Path(const std::string& path)
    {
        std::string fullPath = (std::filesystem::path(GetResourcePath()) / path).string();
        if( !std::filesystem::exists(fullPath) )
        {
            fullPath = GetProjectPath() + "/shader/spv/" + path;
        }
        if( !std::filesystem::exists(fullPath) )
        {
            fullPath = GetProjectPath() + "/" + path;
        }
        if( !std::filesystem::exists(fullPath) )
        {
            throw std::runtime_error("Failed to find file: " + path);
        }
        return fullPath;
    }

    inline std::string ReadFile(const std::string& absFilePath)
    {
        std::ifstream file(absFilePath.data(), std::ios::ate | std::ios::binary);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open file: " + std::string(absFilePath));
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::string buffer;
        buffer.resize(size);
        if (!file.read(buffer.data(), size))
        {
            throw std::runtime_error("Failed to read file: " + std::string(absFilePath));
        }
        file.close();
        if (size <= 0) {
            throw std::runtime_error("File is empty or unreadable: " + absFilePath);
        }  

        return buffer;
    }

    //Rotation(degrees): x, y, z
    inline Eigen::Quaternionf RotationToQuat(Eigen::Vector3f rotation)
    {
        rotation *= M_PI / 180.0f;

        Eigen::AngleAxisf yawAngle(rotation.y(), Eigen::Vector3f::UnitY());
        Eigen::AngleAxisf pitchAngle(rotation.x(), Eigen::Vector3f::UnitX());
        Eigen::AngleAxisf rollAngle(rotation.z(), Eigen::Vector3f::UnitZ());
        Eigen::Quaternionf quaternion = yawAngle * pitchAngle * rollAngle;
        quaternion.normalize();

        return quaternion;
    }

    inline Eigen::Vector3f QuatToRotation(Eigen::Quaternionf quaternion)
    {
        Eigen::Vector3f rotation = quaternion.matrix().eulerAngles(1, 0, 2) * 180.0f / M_PI;
        rotation = Eigen::Vector3f(rotation.y(), rotation.x(), rotation.z());
        return rotation;
    }

    inline Eigen::Matrix4f QuatToMatrix(Eigen::Quaternionf quaternion)
    {
        Eigen::Matrix3f rotationMatrix = quaternion.toRotationMatrix();
        Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
        matrix.block<3, 3>(0, 0) = rotationMatrix;
        return matrix;
    }

    inline Eigen::Matrix4f RotationToMatrix(Eigen::Vector3f rotation)
    {
        Eigen::Quaternionf quaternion = RotationToQuat(rotation);
        return QuatToMatrix(quaternion);
    }

    inline Eigen::Quaternionf RemoveRoll(const Eigen::Quaternionf& q)
    {
        Eigen::Vector3f f = q * Eigen::Vector3f(0, 0, 1); // forward
        Eigen::Vector3f u = Eigen::Vector3f(0, 1, 0); // up
        Eigen::Vector3f r = -1.0 * f.cross(u).normalized(); // right
        u = f.cross(r).normalized();

        Eigen::Matrix3f R;
        R.col(0) = r;
        R.col(1) = u;
        R.col(2) = f;

        return Eigen::Quaternionf(R);
    }

    inline uint32_t FindMemoryType(vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, uint32_t typeFilter, vk::MemoryPropertyFlags& memoryPropertyFlags)
    {
        const auto find = [&](vk::MemoryPropertyFlags flags)
            -> std::optional<uint32_t>
        {
            for (uint32_t i = 0;
                 i < physicalDeviceMemoryProperties.memoryTypeCount;
                 ++i)
            {
                if ((typeFilter & (1 << i)) &&
                    (physicalDeviceMemoryProperties.memoryTypes[i].propertyFlags &
                        flags) == flags)
                {
                    return i;
                }
            }
            return std::nullopt;
        };

        if (const std::optional<uint32_t> exact = find(memoryPropertyFlags))
            return *exact;

        // HOST_CACHED 是读回性能提示，不是功能要求。没有 cached 型时退回
        // 同一组 HOST_VISIBLE|HOST_COHERENT 内存，避免为了性能标志直接失败。
        if ((memoryPropertyFlags & vk::MemoryPropertyFlagBits::eHostCached) !=
            vk::MemoryPropertyFlags{})
        {
            const vk::MemoryPropertyFlags fallback =
                memoryPropertyFlags & ~vk::MemoryPropertyFlagBits::eHostCached;
            if (const std::optional<uint32_t> relaxed = find(fallback))
                return *relaxed;
        }
        throw std::runtime_error("Failed to find suitable memory type!");
    }

    inline bool IsDepthFormat(vk::Format format) {
        return format == vk::Format::eD32Sfloat || format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint;
    }

    inline bool HasStencilComponent(vk::Format format) {
        return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint;
    }

    inline vk::CommandBuffer BeginSingleTimeCommands(vk::Device& device, vk::CommandPool& commandPool)
    {
        // A single-time pool is dedicated to uploads, but command buffers still
        // borrow the parent pool for their lifetime. Serialize the whole
        // begin..end sequence so concurrent uploads cannot trip validation.
        GlobalVulkanSubmissionGate().LockSingleTimeQueue();
        try
        {
        vk::CommandBufferAllocateInfo allocInfo;
        allocInfo
            .setCommandPool(commandPool)
            .setLevel(vk::CommandBufferLevel::ePrimary)
            .setCommandBufferCount(1);

        vk::CommandBuffer commandBuffer =
            GlobalVulkanSubmissionGate().AllocateCommandBuffers(
                device, allocInfo).front();

        vk::CommandBufferBeginInfo beginInfo;
        beginInfo
            .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        commandBuffer.begin(beginInfo);
        return commandBuffer;
        }
        catch (...)
        {
            GlobalVulkanSubmissionGate().UnlockSingleTimeQueue();
            throw;
        }
    }

    inline void EndSingleTimeCommands(vk::Device& device, vk::CommandBuffer& commandBuffer, vk::Queue& graphicsQueue, vk::CommandPool& commandPool)
    {
        vk::Fence fence;
        bool submitted = false;
        try
        {
            commandBuffer.end();

            vk::FenceCreateInfo fenceCreateInfo;
            fence = device.createFence(fenceCreateInfo);

            vk::SubmitInfo submitInfo;
            submitInfo
                .setCommandBuffers(commandBuffer);

            GlobalVulkanSubmissionGate().Submit(
                graphicsQueue, submitInfo, fence);
            submitted = true;
            const vk::Result waitResult =
                device.waitForFences(fence, true, UINT64_MAX);
            if (waitResult != vk::Result::eSuccess)
            {
                throw std::runtime_error(
                    "Failed to wait for single-time command fence");
            }

            device.destroyFence(fence);
            fence = nullptr;
            GlobalVulkanSubmissionGate().FreeCommandBuffer(
                device, commandPool, commandBuffer);
            commandBuffer = nullptr;
            GlobalVulkanSubmissionGate().UnlockSingleTimeQueue();
        }
        catch (...)
        {
            // 提交后的异常路径先尽力等待队列，再释放 command buffer，避免
            // candidate 失败时把仍在 GPU 使用的命令缓冲提前回收。
            if (submitted)
            {
                try
                {
                    GlobalVulkanSubmissionGate().WaitIdle(device);
                }
                catch (...)
                {
                }
            }
            if (commandBuffer)
            {
                try
                {
                    GlobalVulkanSubmissionGate().FreeCommandBuffer(
                        device, commandPool, commandBuffer);
                }
                catch (...)
                {
                }
                commandBuffer = nullptr;
            }
            if (fence)
            {
                try
                {
                    device.destroyFence(fence);
                }
                catch (...)
                {
                }
            }
            GlobalVulkanSubmissionGate().UnlockSingleTimeQueue();
            throw;
        }
    }

    inline void AbortSingleTimeCommands(
        vk::Device& device,
        vk::CommandBuffer& commandBuffer,
        vk::CommandPool& commandPool)
    {
        if (!commandBuffer)
        {
            return;
        }

        // 录制阶段发生异常时，command buffer 尚未提交，可以直接释放，
        // 避免 World candidate 失败后留下不可见的 command pool 资源。
        GlobalVulkanSubmissionGate().FreeCommandBuffer(
            device, commandPool, commandBuffer);
        commandBuffer = nullptr;
        GlobalVulkanSubmissionGate().UnlockSingleTimeQueue();
    }

    inline std::pair<vk::Buffer, vk::DeviceMemory> CreateBuffer(vk::Device& device, vk::DeviceSize& size, vk::BufferUsageFlags& usage, vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, vk::MemoryPropertyFlags& memoryPropertyFlags, const std::string& name = "")
    {
        vk::BufferCreateInfo bufferInfo;
        bufferInfo
            .setSize(size)
            .setUsage(usage)
            .setSharingMode(vk::SharingMode::eExclusive);

        vk::Buffer buffer = device.createBuffer(bufferInfo);

        vk::MemoryRequirements memRequirements = device.getBufferMemoryRequirements(buffer);

        vk::MemoryAllocateInfo allocInfo;
        allocInfo
            .setAllocationSize(memRequirements.size)
            .setMemoryTypeIndex(FindMemoryType(physicalDeviceMemoryProperties, memRequirements.memoryTypeBits, memoryPropertyFlags));

        vk::DeviceMemory bufferMemory = device.allocateMemory(allocInfo);
        device.bindBufferMemory(buffer, bufferMemory, 0);

        if (!name.empty())
        {
            VulkanDebug::SetObjectName(device, buffer, vk::ObjectType::eBuffer, name);
            VulkanDebug::SetObjectName(device, bufferMemory, vk::ObjectType::eDeviceMemory, "DeviceMemory: " + name);
        }

        return { buffer, bufferMemory };
    }

    inline std::pair<vk::Image, vk::DeviceMemory> CreateImage(vk::Device& device, uint32_t width, uint32_t height, uint32_t mipLevels, vk::SampleCountFlagBits samples, vk::Format& format, vk::ImageTiling& tiling, vk::ImageUsageFlags& usage, vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, vk::MemoryPropertyFlags& memoryPropertyFlags, const std::string& name = "")
    {
        vk::ImageCreateInfo imageInfo;
        imageInfo
            .setImageType(vk::ImageType::e2D)
            .setExtent(vk::Extent3D{ width, height, 1 })
            .setMipLevels(mipLevels)
            .setArrayLayers(1)
            .setFormat(format)
            .setTiling(tiling)
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setUsage(usage)
            .setSharingMode(vk::SharingMode::eExclusive)
            .setSamples(samples);

        vk::Image image = device.createImage(imageInfo);

        vk::MemoryRequirements memRequirements = device.getImageMemoryRequirements(image);
        uint32_t memoryTypeIndex = FindMemoryType(physicalDeviceMemoryProperties,memRequirements.memoryTypeBits,memoryPropertyFlags);
        vk::MemoryAllocateInfo allocInfo;
        allocInfo
            .setAllocationSize(memRequirements.size)
            .setMemoryTypeIndex(memoryTypeIndex);
        
        vk::DeviceMemory imageMemory = device.allocateMemory(allocInfo);
        device.bindImageMemory(image, imageMemory, 0);

        if (!name.empty())
        {
            VulkanDebug::SetObjectName(device, image, vk::ObjectType::eImage, name);
            VulkanDebug::SetObjectName(device, imageMemory, vk::ObjectType::eDeviceMemory, "DeviceMemory: " + name);
        }

        return { image, imageMemory };
    }

    inline vk::ImageView CreateImageViewBase(
        vk::Device& device,
        vk::Image& image,
        vk::ImageViewType viewType,
        vk::Format& format,
        vk::ImageAspectFlags aspectMask,
        uint32_t baseMipLevel,
        uint32_t mipLevelCount,
        uint32_t baseArrayLayer,
        uint32_t layerCount,
        const std::string& name = "")
    {
        vk::ImageViewCreateInfo viewInfo;
        viewInfo
            .setImage(image)
            .setViewType(viewType)
            .setFormat(format)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(aspectMask)
                .setBaseMipLevel(baseMipLevel)
                .setLevelCount(mipLevelCount)
                .setBaseArrayLayer(baseArrayLayer)
                .setLayerCount(layerCount));

        vk::ImageView imageView = device.createImageView(viewInfo);
        if (!name.empty())
        {
            VulkanDebug::SetObjectName(device, imageView, vk::ObjectType::eImageView, name);
        }
        return imageView;
    }

    inline vk::ImageView Create2DImageView(vk::Device& device, vk::Image& image, uint32_t mipLevels, vk::Format& format, vk::ImageAspectFlags aspectMask = vk::ImageAspectFlagBits::eColor, const std::string& name = "")
    {
        return CreateImageViewBase(
            device,
            image,
            vk::ImageViewType::e2D,
            format,
            aspectMask,
            0,
            mipLevels,
            0,
            1,
            name);
    }

    inline vk::ImageView CreateCubeImageView(vk::Device& device, vk::Image& image, uint32_t mipLevels, vk::Format& format, const std::string& name = "")
    {
        return CreateImageViewBase(
            device,
            image,
            vk::ImageViewType::eCube,
            format,
            vk::ImageAspectFlagBits::eColor,
            0,
            mipLevels,
            0,
            6,
            name);
    }

    inline vk::ImageView CreateCubeStorageImageView(vk::Device& device, vk::Image& image, vk::Format& format, const std::string& name = "")
    {
        return CreateImageViewBase(
            device,
            image,
            vk::ImageViewType::e2DArray,
            format,
            vk::ImageAspectFlagBits::eColor,
            0,
            1,
            0,
            6,
            name);
    }

    inline vk::Sampler CreateSamplerBase(vk::Device& device, const vk::SamplerCreateInfo& samplerInfo, const std::string& name = "")
    {
        vk::Sampler sampler = device.createSampler(samplerInfo);
        if (!name.empty())
        {
            VulkanDebug::SetObjectName(device, sampler, vk::ObjectType::eSampler, name);
        }
        return sampler;
    }

    inline vk::Sampler Create2DSampler(
        vk::Device& device,
        vk::PhysicalDevice& physicalDevice,
        vk::Filter filter,
        vk::SamplerAddressMode addressMode,
        bool enableMipmaps,
        const std::string& name = "")
    {

        vk::PhysicalDeviceProperties physicalDeviceProperties = physicalDevice.getProperties();
        vk::PhysicalDeviceFeatures physicalDeviceFeatures = physicalDevice.getFeatures();
        const bool enableAnisotropy = filter == vk::Filter::eLinear;
        const vk::SamplerMipmapMode mipmapMode = filter == vk::Filter::eNearest
            ? vk::SamplerMipmapMode::eNearest
            : vk::SamplerMipmapMode::eLinear;
        
        vk::SamplerCreateInfo samplerInfo;
        samplerInfo
            .setMagFilter(filter)
            .setMinFilter(filter)
            .setAddressModeU(addressMode)
            .setAddressModeV(addressMode)
            .setAddressModeW(addressMode)
            .setAnisotropyEnable(enableAnisotropy ? VK_TRUE : VK_FALSE)
            .setMaxAnisotropy(enableAnisotropy ? physicalDeviceProperties.limits.maxSamplerAnisotropy : 1.0f)
            .setBorderColor(vk::BorderColor::eIntOpaqueBlack)
            .setUnnormalizedCoordinates(VK_FALSE)
            .setCompareEnable(VK_FALSE)
            .setCompareOp(vk::CompareOp::eAlways)
            .setMipmapMode(mipmapMode)
            .setMipLodBias(0.0f)
            .setMinLod(0.0f)
            .setMaxLod(enableMipmaps ? VK_LOD_CLAMP_NONE : 0.0f);

        if(physicalDeviceFeatures.samplerAnisotropy == VK_FALSE || !enableAnisotropy)
        {
            samplerInfo
                .setAnisotropyEnable(VK_FALSE)
                .setMaxAnisotropy(1.0f);
        }

        return CreateSamplerBase(device, samplerInfo, name);
    }

    inline vk::Sampler Create2DSampler(vk::Device& device, vk::PhysicalDevice& physicalDevice, const std::string& name = "")
    {
        return Create2DSampler(
            device,
            physicalDevice,
            vk::Filter::eLinear,
            vk::SamplerAddressMode::eRepeat,
            true,
            name);
    }

    inline vk::Sampler CreateDepthSampler(vk::Device& device, vk::PhysicalDevice& physicalDevice, const std::string& name = "")
    {
        vk::PhysicalDeviceProperties physicalDeviceProperties = physicalDevice.getProperties();
        vk::PhysicalDeviceFeatures physicalDeviceFeatures = physicalDevice.getFeatures();

        vk::SamplerCreateInfo samplerInfo;
        samplerInfo
            .setMagFilter(vk::Filter::eLinear)
            .setMinFilter(vk::Filter::eLinear)
            .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
            .setAnisotropyEnable(VK_TRUE)
            .setMaxAnisotropy(physicalDeviceProperties.limits.maxSamplerAnisotropy)
            .setBorderColor(vk::BorderColor::eIntOpaqueBlack)
            .setUnnormalizedCoordinates(VK_FALSE)
            .setCompareEnable(VK_FALSE)
            .setCompareOp(vk::CompareOp::eAlways)
            .setMipmapMode(vk::SamplerMipmapMode::eLinear)
            .setMipLodBias(0.0f)
            .setMinLod(0.0f)
            .setMaxLod(VK_LOD_CLAMP_NONE);

        if(physicalDeviceFeatures.samplerAnisotropy == VK_FALSE)
        {
            samplerInfo
                .setAnisotropyEnable(VK_FALSE)
                .setMaxAnisotropy(1.0f);
        }

        return CreateSamplerBase(device, samplerInfo, name);
    }

    inline vk::Sampler CreateDepthCompareSampler(vk::Device& device, vk::PhysicalDevice& physicalDevice, const std::string& name = "")
    {
        vk::PhysicalDeviceProperties physicalDeviceProperties = physicalDevice.getProperties();
        vk::PhysicalDeviceFeatures physicalDeviceFeatures = physicalDevice.getFeatures();

        vk::SamplerCreateInfo samplerInfo;
        samplerInfo
            .setMagFilter(vk::Filter::eLinear)
            .setMinFilter(vk::Filter::eLinear)
            .setAddressModeU(vk::SamplerAddressMode::eClampToBorder)
            .setAddressModeV(vk::SamplerAddressMode::eClampToBorder)
            .setAddressModeW(vk::SamplerAddressMode::eClampToBorder)
            .setAnisotropyEnable(VK_TRUE)
            .setMaxAnisotropy(physicalDeviceProperties.limits.maxSamplerAnisotropy)
            .setBorderColor(vk::BorderColor::eFloatOpaqueWhite)
            .setUnnormalizedCoordinates(VK_FALSE)
            .setCompareEnable(VK_TRUE)
            .setCompareOp(vk::CompareOp::eLessOrEqual)
            .setMipmapMode(vk::SamplerMipmapMode::eLinear)
            .setMipLodBias(0.0f)
            .setMinLod(0.0f)
            .setMaxLod(VK_LOD_CLAMP_NONE);

        if(physicalDeviceFeatures.samplerAnisotropy == VK_FALSE)
        {
            samplerInfo
                .setAnisotropyEnable(VK_FALSE)
                .setMaxAnisotropy(1.0f);
        }

        return CreateSamplerBase(device, samplerInfo, name);
    }

    inline vk::Sampler CreateCubeSampler(vk::Device& device, float maxLod = 0.0f, const std::string& name = "")
    {
        vk::SamplerCreateInfo samplerInfo;
        samplerInfo
            .setMagFilter(vk::Filter::eLinear)
            .setMinFilter(vk::Filter::eLinear)
            .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
            .setAnisotropyEnable(VK_FALSE)
            .setMaxAnisotropy(1.0f)
            .setBorderColor(vk::BorderColor::eFloatOpaqueBlack)
            .setUnnormalizedCoordinates(VK_FALSE)
            .setCompareEnable(VK_FALSE)
            .setCompareOp(vk::CompareOp::eAlways)
            .setMipmapMode(vk::SamplerMipmapMode::eLinear)
            .setMipLodBias(0.0f)
            .setMinLod(0.0f)
            .setMaxLod(maxLod);

        return CreateSamplerBase(device, samplerInfo, name);
    }

    // 只把 layout barrier 录制到调用方给的主帧 command buffer，不 submit。
    // 流式上传需要在主帧里把“页表/atlas 写入”排在 scene pass 之前，不能用
    // BeginSingleTimeCommands 另起一次提交；加载期同步路径仍复用同一个 barrier 逻辑。
    inline void RecordTransitionImageLayout(
        vk::CommandBuffer commandBuffer,
        vk::Image image,
        uint32_t mipLevels,
        vk::Format format,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout,
        uint32_t baseArrayLayer = 0,
        uint32_t layerCount = 1)
    {
        if (layerCount == 0)
        {
            throw std::invalid_argument("TransitionImageLayout requires at least one array layer");
        }

        vk::ImageMemoryBarrier barrier;
        barrier
            .setOldLayout(oldLayout)
            .setNewLayout(newLayout)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(image)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(mipLevels)
                .setBaseArrayLayer(baseArrayLayer)
                .setLayerCount(layerCount));

        vk::PipelineStageFlags sourceStage;
        vk::PipelineStageFlags destinationStage;

        if(oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eTransferDstOptimal)
        {
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eNone)
                .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
            sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
            destinationStage = vk::PipelineStageFlagBits::eTransfer;
        }
        else if(oldLayout == vk::ImageLayout::eTransferDstOptimal && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
            sourceStage = vk::PipelineStageFlagBits::eTransfer;
            destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
        }
        else if(oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eNone)
                .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
            sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
            destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
        }
        else if(oldLayout == vk::ImageLayout::eShaderReadOnlyOptimal && newLayout == vk::ImageLayout::eTransferDstOptimal)
        {
            // 增量页上传：先把已发布的只读纹理重新置为 transfer 目标。
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
                .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
            sourceStage = vk::PipelineStageFlagBits::eFragmentShader;
            destinationStage = vk::PipelineStageFlagBits::eTransfer;
        }
        else if(oldLayout == vk::ImageLayout::eShaderReadOnlyOptimal && newLayout == vk::ImageLayout::eTransferSrcOptimal)
        {
            // 层 atlas 上传：源层纹理已作为材质采样源发布为只读，GPU 切页时
            // 需要把它临时转成 transfer 源；完成后必须转回只读。
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
            sourceStage = vk::PipelineStageFlagBits::eFragmentShader;
            destinationStage = vk::PipelineStageFlagBits::eTransfer;
        }
        else if(oldLayout == vk::ImageLayout::eTransferSrcOptimal && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
                .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
            sourceStage = vk::PipelineStageFlagBits::eTransfer;
            destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
        }
        else if(oldLayout == vk::ImageLayout::eTransferDstOptimal && newLayout == vk::ImageLayout::eTransferSrcOptimal)
        {
            // 写后回读：transfer 写与 transfer 读之间只需要 transfer 阶段的屏障。
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
            sourceStage = vk::PipelineStageFlagBits::eTransfer;
            destinationStage = vk::PipelineStageFlagBits::eTransfer;
        }
        else if(oldLayout == vk::ImageLayout::eColorAttachmentOptimal && newLayout == vk::ImageLayout::eTransferSrcOptimal)
        {
            // feedback 回读：render pass 的 color attachment 写入必须先对后续
            // transfer read 可见，再允许 CopyImageToBuffer 读取该小 RT。
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
            sourceStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
            destinationStage = vk::PipelineStageFlagBits::eTransfer;
        }
        else if(oldLayout == vk::ImageLayout::eTransferSrcOptimal && newLayout == vk::ImageLayout::eColorAttachmentOptimal)
        {
            // 回读完成后恢复 attachment layout；下一帧 render pass 仍按
            // Undefined/clear 取得内容，旧像素不会成为新帧输入。
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
                .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite);
            sourceStage = vk::PipelineStageFlagBits::eTransfer;
            destinationStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        }
        else if(oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal)
        {
            barrier
                .subresourceRange.setAspectMask(vk::ImageAspectFlagBits::eDepth);
            if (HasStencilComponent(format))
            {
                barrier.subresourceRange.aspectMask |= vk::ImageAspectFlagBits::eStencil;
            }

            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eNone)
                .setDstAccessMask(vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite);
            sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
            destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests;
        }
        else
        {
            throw std::invalid_argument(
                "Unsupported Vulkan image layout transition: " +
                vk::to_string(oldLayout) +
                " -> " +
                vk::to_string(newLayout) +
                ", format=" +
                vk::to_string(format));
        }   
    
        commandBuffer.pipelineBarrier(
            sourceStage, 
            destinationStage,
            vk::DependencyFlags(),
            nullptr, nullptr, barrier);
    }

    inline void  TransitionImageLayout(vk::Image& image, uint32_t mipLevels, vk::Format& format, vk::Device& device, vk::CommandPool& commandPool, vk::Queue& GraphicsQueue, vk::ImageLayout oldLayout, vk::ImageLayout newLayout, uint32_t baseArrayLayer = 0, uint32_t layerCount = 1)
    {
        // 校验放在 Begin 之前，保持旧入口“非法参数不留下未提交 command buffer”的行为。
        if (layerCount == 0)
        {
            throw std::invalid_argument("TransitionImageLayout requires at least one array layer");
        }
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);
        RecordTransitionImageLayout(
            commandBuffer,
            image,
            mipLevels,
            format,
            oldLayout,
            newLayout,
            baseArrayLayer,
            layerCount);

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    inline void CopyBufferToBuffer(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Buffer& srcBuffer, vk::Buffer& dstBuffer, vk::DeviceSize& size)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        vk::BufferCopy copyRegion;
        copyRegion
            .setSrcOffset(0)
            .setDstOffset(0)
            .setSize(size);
        commandBuffer.copyBuffer(srcBuffer, dstBuffer, 1, &copyRegion);

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    inline std::vector<vk::BufferImageCopy> MakeFlipYBufferImageCopyRegions(uint32_t width, uint32_t height, vk::DeviceSize rowBytes, bool bufferYReversed)
    {
        if (rowBytes == 0)
        {
            throw std::invalid_argument("rowBytes must be greater than 0 when flipY is enabled");
        }
        std::vector<vk::BufferImageCopy> regions;
        regions.reserve(height);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t bufferRow = bufferYReversed ? (height - 1 - y) : y;
            vk::BufferImageCopy region;
            region
                .setBufferOffset(static_cast<vk::DeviceSize>(bufferRow) * rowBytes)
                .setBufferRowLength(0)
                .setBufferImageHeight(0)
                .setImageSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(0)
                    .setBaseArrayLayer(0)
                    .setLayerCount(1))
                .setImageOffset(vk::Offset3D{ 0, static_cast<int32_t>(y), 0 })
                .setImageExtent(vk::Extent3D{ width, 1, 1 });
            regions.push_back(region);
        }
        return regions;
    }

    // layerBytes 是缓冲区中相邻数组层之间的字节跨度；单层调用保持默认 0，行为与旧路径一致。
    inline void CopyBufferToImage(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Buffer& buffer, vk::Image& image, uint32_t width, uint32_t height, bool flipY = false, vk::DeviceSize rowBytes = 0, uint32_t baseArrayLayer = 0, uint32_t layerCount = 1, vk::DeviceSize layerBytes = 0, uint32_t bufferRowLength = 0)
    {
        if (layerCount == 0)
        {
            throw std::invalid_argument("CopyBufferToImage requires at least one array layer");
        }
        if (layerCount > 1 && layerBytes == 0)
        {
            throw std::invalid_argument("Multi-layer CopyBufferToImage requires layerBytes");
        }
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        if (!flipY)
        {
            std::vector<vk::BufferImageCopy> regions;
            regions.reserve(layerCount);
            for (uint32_t layer = 0; layer < layerCount; ++layer)
            {
                vk::BufferImageCopy region;
                region
                    .setBufferOffset(static_cast<vk::DeviceSize>(layer) * layerBytes)
                    .setBufferRowLength(bufferRowLength)
                    .setBufferImageHeight(0)
                    .setImageSubresource(vk::ImageSubresourceLayers()
                        .setAspectMask(vk::ImageAspectFlagBits::eColor)
                        .setMipLevel(0)
                        .setBaseArrayLayer(baseArrayLayer + layer)
                        .setLayerCount(1))
                    .setImageOffset(vk::Offset3D{ 0, 0, 0 })
                    .setImageExtent(vk::Extent3D{ width, height, 1 });
                regions.push_back(region);
            }
            commandBuffer.copyBufferToImage(
                buffer,
                image,
                vk::ImageLayout::eTransferDstOptimal,
                static_cast<uint32_t>(regions.size()),
                regions.data());
        }
        else
        {
            std::vector<vk::BufferImageCopy> regions;
            regions.reserve(static_cast<size_t>(height) * layerCount);
            for (uint32_t layer = 0; layer < layerCount; ++layer)
            {
                std::vector<vk::BufferImageCopy> layerRegions =
                    MakeFlipYBufferImageCopyRegions(width, height, rowBytes, true);
                for (vk::BufferImageCopy& region : layerRegions)
                {
                    region.bufferOffset += static_cast<vk::DeviceSize>(layer) * layerBytes;
                    region.imageSubresource.baseArrayLayer = baseArrayLayer + layer;
                }
                regions.insert(regions.end(), layerRegions.begin(), layerRegions.end());
            }
            commandBuffer.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, static_cast<uint32_t>(regions.size()), regions.data());
        }

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    // 子区域上传：只覆盖目标 image 中 (imageOffsetX, imageOffsetY) 起、指定 mip/array layer 的 width×height。
    // 调用方必须自行把 image 转为 eTransferDstOptimal；本函数只做 copy，不改变 layout。
    inline void CopyBufferToImageRegion(
        vk::Device& device,
        vk::Queue& GraphicsQueue,
        vk::CommandPool& commandPool,
        vk::Buffer& buffer,
        vk::Image& image,
        vk::DeviceSize bufferOffset,
        uint32_t bufferRowLength,
        uint32_t imageOffsetX,
        uint32_t imageOffsetY,
        uint32_t width,
        uint32_t height,
        uint32_t mipLevel,
        uint32_t baseArrayLayer = 0,
        uint32_t layerCount = 1,
        vk::DeviceSize layerBytes = 0)
    {
        if (width == 0 || height == 0)
        {
            throw std::invalid_argument("CopyBufferToImageRegion requires non-zero width and height");
        }
        if (layerCount == 0)
        {
            throw std::invalid_argument("CopyBufferToImageRegion requires at least one array layer");
        }
        if (layerCount > 1 && layerBytes == 0)
        {
            throw std::invalid_argument("Multi-layer CopyBufferToImageRegion requires layerBytes");
        }
        if (bufferRowLength != 0 && bufferRowLength < width)
        {
            throw std::invalid_argument(
                "CopyBufferToImageRegion bufferRowLength must be zero or at least width");
        }
        // vk::Offset3D 使用 int32；偏移加尺寸超出范围时，转换结果不可信，必须在源头拒绝。
        if (static_cast<uint64_t>(imageOffsetX) + width > static_cast<uint64_t>(INT32_MAX) ||
            static_cast<uint64_t>(imageOffsetY) + height > static_cast<uint64_t>(INT32_MAX))
        {
            throw std::invalid_argument(
                "CopyBufferToImageRegion image offset plus extent exceeds int32 range");
        }

        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        std::vector<vk::BufferImageCopy> regions;
        regions.reserve(layerCount);
        for (uint32_t layer = 0; layer < layerCount; ++layer)
        {
            vk::BufferImageCopy region;
            region
                .setBufferOffset(bufferOffset + static_cast<vk::DeviceSize>(layer) * layerBytes)
                .setBufferRowLength(bufferRowLength)
                .setBufferImageHeight(0)
                .setImageSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(mipLevel)
                    .setBaseArrayLayer(baseArrayLayer + layer)
                    .setLayerCount(1))
                .setImageOffset(vk::Offset3D{
                    static_cast<int32_t>(imageOffsetX),
                    static_cast<int32_t>(imageOffsetY),
                    0 })
                .setImageExtent(vk::Extent3D{ width, height, 1 });
            regions.push_back(region);
        }

        commandBuffer.copyBufferToImage(
            buffer,
            image,
            vk::ImageLayout::eTransferDstOptimal,
            static_cast<uint32_t>(regions.size()),
            regions.data());

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    // 同步/录制两条路径共用的 region 几何校验，保证非法输入在任何 Vulkan 调用
    // 之前就被拒绝（空列表由调用方单独检查，错误文案保持各自入口的名字）。
    inline void ValidateBufferImageCopyRegions(
        const std::vector<vk::BufferImageCopy>& regions,
        const char* callerName)
    {
        for (const vk::BufferImageCopy& region : regions)
        {
            if (region.imageExtent.width == 0 || region.imageExtent.height == 0 ||
                region.imageExtent.depth == 0)
            {
                throw std::invalid_argument(
                    std::string(callerName) + " requires non-zero region extent");
            }
            if (region.imageSubresource.layerCount == 0)
            {
                throw std::invalid_argument(
                    std::string(callerName) + " requires at least one array layer per region");
            }
            if (region.imageOffset.x < 0 || region.imageOffset.y < 0 ||
                region.imageOffset.z < 0)
            {
                throw std::invalid_argument(
                    std::string(callerName) + " requires non-negative image offsets");
            }
            // 与单 region 版本相同的 int32 约束：偏移加尺寸必须仍能放进 vk::Offset3D 的坐标空间。
            const int64_t maxX =
                static_cast<int64_t>(region.imageOffset.x) + region.imageExtent.width;
            const int64_t maxY =
                static_cast<int64_t>(region.imageOffset.y) + region.imageExtent.height;
            const int64_t maxZ =
                static_cast<int64_t>(region.imageOffset.z) + region.imageExtent.depth;
            if (maxX > INT32_MAX || maxY > INT32_MAX || maxZ > INT32_MAX)
            {
                throw std::invalid_argument(
                    std::string(callerName) + " image offset plus extent exceeds int32 range");
            }
        }
    }

    // 只录制批量 copy，不 submit；布局与 staging 生命周期都由调用方负责。
    // 所有 region 必须是同一 buffer/image 上的子区域，调用方需要先把 image
    // 转到 eTransferDstOptimal。
    inline void RecordCopyBufferToImageRegions(
        vk::CommandBuffer commandBuffer,
        vk::Buffer buffer,
        vk::Image image,
        const std::vector<vk::BufferImageCopy>& regions)
    {
        if (regions.empty())
        {
            throw std::invalid_argument("RecordCopyBufferToImageRegions requires at least one region");
        }
        // 所有几何约束在录制前校验：主帧 command buffer 一旦开始录制就很难回滚，
        // 非法 region 必须在写入任何命令之前被拒绝。
        ValidateBufferImageCopyRegions(regions, "RecordCopyBufferToImageRegions");
        commandBuffer.copyBufferToImage(
            buffer,
            image,
            vk::ImageLayout::eTransferDstOptimal,
            static_cast<uint32_t>(regions.size()),
            regions.data());
    }

    // 批量上传：所有 region 共用同一个 command buffer，一次 submit + wait。
    // 与单 region 版本一样，调用方负责把 image 预置为 eTransferDstOptimal；
    // 本函数只做 copy，不改变 layout。返回前 fence 已等到，因此调用方可以立即
    // 复用或释放这一批 staging buffer，不会留下仍在 GPU 上的引用。
    inline void CopyBufferToImageRegions(
        vk::Device& device,
        vk::Queue& GraphicsQueue,
        vk::CommandPool& commandPool,
        vk::Buffer& buffer,
        vk::Image& image,
        const std::vector<vk::BufferImageCopy>& regions)
    {
        // 先校验再 Begin：非法输入不会留下已开始录制、却永远不会被提交的 command buffer。
        if (regions.empty())
        {
            throw std::invalid_argument("CopyBufferToImageRegions requires at least one region");
        }
        ValidateBufferImageCopyRegions(regions, "CopyBufferToImageRegions");
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);
        RecordCopyBufferToImageRegions(commandBuffer, buffer, image, regions);
        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    // 只录制 vkCmdClearColorImage，不 submit。调用方负责先转到
    // eTransferDstOptimal/eGeneral，并在之后转回采样 layout。
    // 块压缩格式禁止使用 clearColor；BC atlas 必须走零 buffer 的 copy 路径。
    inline void RecordClearColorImage(
        vk::CommandBuffer commandBuffer,
        vk::Image image,
        vk::ImageLayout layout,
        vk::ClearColorValue clearValue,
        uint32_t baseArrayLayer = 0,
        uint32_t layerCount = 1)
    {
        if (layerCount == 0)
        {
            throw std::invalid_argument(
                "RecordClearColorImage requires at least one array layer");
        }
        if (layout != vk::ImageLayout::eTransferDstOptimal &&
            layout != vk::ImageLayout::eGeneral)
        {
            throw std::invalid_argument(
                "RecordClearColorImage requires TransferDst or General layout");
        }
        vk::ImageSubresourceRange range;
        range
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setBaseMipLevel(0)
            .setLevelCount(1)
            .setBaseArrayLayer(baseArrayLayer)
            .setLayerCount(layerCount);
        commandBuffer.clearColorImage(image, layout, clearValue, range);
    }

    // image -> image 子区域复制的几何校验。源/目标必须是同一 queue family
    // 且 image 已由调用方转到 TransferSrc/TransferDst。
    inline void ValidateImageCopyRegions(
        const std::vector<vk::ImageCopy>& regions)
    {
        for (const vk::ImageCopy& region : regions)
        {
            if (region.extent.width == 0 || region.extent.height == 0 ||
                region.extent.depth == 0)
            {
                throw std::invalid_argument(
                    "RecordCopyImageRegions requires non-zero extent");
            }
            if (region.srcSubresource.layerCount == 0 ||
                region.dstSubresource.layerCount == 0)
            {
                throw std::invalid_argument(
                    "RecordCopyImageRegions requires at least one array layer");
            }
            if (region.srcOffset.x < 0 || region.srcOffset.y < 0 ||
                region.srcOffset.z < 0 || region.dstOffset.x < 0 ||
                region.dstOffset.y < 0 || region.dstOffset.z < 0)
            {
                throw std::invalid_argument(
                    "RecordCopyImageRegions requires non-negative image offsets");
            }
            const int64_t srcMaxX =
                static_cast<int64_t>(region.srcOffset.x) + region.extent.width;
            const int64_t srcMaxY =
                static_cast<int64_t>(region.srcOffset.y) + region.extent.height;
            const int64_t dstMaxX =
                static_cast<int64_t>(region.dstOffset.x) + region.extent.width;
            const int64_t dstMaxY =
                static_cast<int64_t>(region.dstOffset.y) + region.extent.height;
            if (srcMaxX > INT32_MAX || srcMaxY > INT32_MAX ||
                dstMaxX > INT32_MAX || dstMaxY > INT32_MAX)
            {
                throw std::invalid_argument(
                    "RecordCopyImageRegions offset plus extent exceeds int32 range");
            }
        }
    }

    // GPU->GPU 批量切页：一张源数组（按 mip/layer 子区域）写入 atlas 的
    // 物理页。所有 region 共用一个 command buffer，页像素不经过 CPU。
    inline void RecordCopyImageRegions(
        vk::CommandBuffer commandBuffer,
        vk::Image source,
        vk::Image destination,
        const std::vector<vk::ImageCopy>& regions)
    {
        if (regions.empty())
        {
            throw std::invalid_argument(
                "RecordCopyImageRegions requires at least one region");
        }
        ValidateImageCopyRegions(regions);
        commandBuffer.copyImage(
            source,
            vk::ImageLayout::eTransferSrcOptimal,
            destination,
            vk::ImageLayout::eTransferDstOptimal,
            static_cast<uint32_t>(regions.size()),
            regions.data());
    }

    // 同步版本：一次 submit + fence wait，调用方负责布局与资源存活。
    inline void CopyImageRegions(
        vk::Device& device,
        vk::Queue& GraphicsQueue,
        vk::CommandPool& commandPool,
        vk::Image source,
        vk::Image destination,
        const std::vector<vk::ImageCopy>& regions)
    {
        if (regions.empty())
        {
            throw std::invalid_argument(
                "CopyImageRegions requires at least one region");
        }
        ValidateImageCopyRegions(regions);
        vk::CommandBuffer commandBuffer =
            BeginSingleTimeCommands(device, commandPool);
        RecordCopyImageRegions(commandBuffer, source, destination, regions);
        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    // 只录制 image -> buffer 的 copy；调用方负责把 image 转到 eTransferSrcOptimal
    // 并保证 buffer 足够大、提交完成后才回读。
    inline void RecordCopyImageToBuffer(
        vk::CommandBuffer commandBuffer,
        vk::Image image,
        vk::Buffer buffer,
        uint32_t width,
        uint32_t height,
        bool flipY = false,
        vk::DeviceSize rowBytes = 0,
        // 多张图像写进同一个回读 buffer 时用；4 字节对齐由调用方保证。
        vk::DeviceSize bufferOffset = 0)
    {
        if (!flipY)
        {
            vk::BufferImageCopy region;
            region
                .setBufferOffset(bufferOffset)
                .setBufferRowLength(0)
                .setBufferImageHeight(0)
                .setImageSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(0)
                    .setBaseArrayLayer(0)
                    .setLayerCount(1))
                .setImageOffset(vk::Offset3D{ 0, 0, 0 })
                .setImageExtent(vk::Extent3D{ width, height, 1 });
            commandBuffer.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer, 1, &region);
        }
        else
        {
            std::vector<vk::BufferImageCopy> regions = MakeFlipYBufferImageCopyRegions(width, height, rowBytes, true);
            if (bufferOffset != 0)
            {
                for (vk::BufferImageCopy& region : regions)
                    region.bufferOffset += bufferOffset;
            }
            commandBuffer.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer, static_cast<uint32_t>(regions.size()), regions.data());
        }
    }

    inline void CopyImageToBuffer(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Image& image, vk::Buffer& buffer, uint32_t width, uint32_t height, bool flipY = false, vk::DeviceSize rowBytes = 0)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);
        RecordCopyImageToBuffer(commandBuffer, image, buffer, width, height, flipY, rowBytes);

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    // 单层 mip 链；调用方负责逐层循环，避免把不同数组层当成同一个子资源。
    inline void GenerateMipmapsLayer(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Image& image, uint32_t width, uint32_t height, uint32_t mipLevels, uint32_t arrayLayer)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        vk::ImageMemoryBarrier barrier;
        barrier
            .setImage(image)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(arrayLayer)
                .setLayerCount(1));

        int32_t mipWidth = width;
        int32_t mipHeight = height;
        for (uint32_t i = 1; i < mipLevels; i++)
        {
            barrier
                .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
                .setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                .setSubresourceRange(vk::ImageSubresourceRange()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setBaseMipLevel(i - 1)
                    .setLevelCount(1)
                    .setBaseArrayLayer(arrayLayer)
                    .setLayerCount(1));

            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlags(),
                nullptr,nullptr,
                barrier);

            vk::ImageBlit blit;
            blit
                .setSrcOffsets
                ({
                    vk::Offset3D{ 0, 0, 0 },
                    vk::Offset3D{ mipWidth, mipHeight, 1 }
                })
                .setSrcSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(i - 1)
                    .setBaseArrayLayer(arrayLayer)
                    .setLayerCount(1))
                .setDstOffsets
                ({
                    vk::Offset3D{ 0, 0, 0 },
                    vk::Offset3D{ mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 }
                })
                .setDstSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(i)
                    .setBaseArrayLayer(arrayLayer)
                    .setLayerCount(1));

            commandBuffer.blitImage(
                image, vk::ImageLayout::eTransferSrcOptimal,
                image, vk::ImageLayout::eTransferDstOptimal,
                1, &blit,
                vk::Filter::eLinear);

            barrier
                .setOldLayout(vk::ImageLayout::eTransferSrcOptimal)
                .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
                .setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
                .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
            
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eFragmentShader,
                vk::DependencyFlags(),
                nullptr, nullptr,
                barrier);


            mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
            mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
        }

        barrier
            .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(mipLevels - 1)
                .setLevelCount(1)
                .setBaseArrayLayer(arrayLayer)
                .setLayerCount(1));

        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr, nullptr,
            barrier);

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    inline void GenerateMipmaps(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Image& image, uint32_t width, uint32_t height, uint32_t mipLevels, uint32_t baseArrayLayer = 0, uint32_t layerCount = 1)
    {
        if (mipLevels == 0 || layerCount == 0)
        {
            throw std::invalid_argument("GenerateMipmaps requires mip levels and array layers");
        }
        for (uint32_t layer = 0; layer < layerCount; ++layer)
        {
            GenerateMipmapsLayer(
                device,
                GraphicsQueue,
                commandPool,
                image,
                width,
                height,
                mipLevels,
                baseArrayLayer + layer);
        }
    }
}
