#pragma once

#include "vulkan/vulkan.hpp"
#include <cstdint>
#include <vector>
#include <Eigen/Dense>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <math.h>
#include <optional>
#include "vulkanDebug.h"

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
        if(Value.is_number_float())
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

    inline std::string GetConfigFolderPath()
    {
        static std::string configFolderPath;
        if( !configFolderPath.empty() )
        {
            return configFolderPath;
        }

        // 获取当前文件所在目录
        std::string projectPath = std::filesystem::current_path().string();
        // 进入config目录, 确定config.json文件是否存在
        // TODO: 这里现在兼容的debug和debug不调试，未来需要兼容release模式
        configFolderPath = projectPath + "/config";
        if( !std::filesystem::exists(configFolderPath) )
        {
            //向前找两级目录，适配debug不调试
            projectPath = std::filesystem::path(projectPath).parent_path().parent_path().string();
            configFolderPath = projectPath + "/config";
            if( !std::filesystem::exists(configFolderPath) )
            {
                throw std::runtime_error("Failed to find config.json file");
            }
        }

        return configFolderPath;
    }

    inline nlohmann::basic_json<>& InitConfigJson()
    {
        static std::optional<nlohmann::json> configJson;
        if( configJson.has_value() )
        {
            return configJson.value();
        }

        // 获取configFolderPath
        std::string configFolderPath = GetConfigFolderPath();
        // 读取config.json文件
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

        // 获取configFolderPath
        std::string configFolderPath = GetConfigFolderPath();
        // 读取renderGraph.json文件
        std::ifstream renderGraphFile(configFolderPath + "/renderGraphConfig.json");
        renderGraphJson = nlohmann::json();
        renderGraphFile >> renderGraphJson.value();
        return renderGraphJson.value();
    }

    inline std::string GetInitScene()
    {
        const nlohmann::json& configJson = InitConfigJson();
        std::string initScene = configJson["initScene"];
        return initScene;
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

    inline Eigen::Vector2f ParserRenderResourceSize(const nlohmann::json& resourceNode)
    {
        Eigen::Vector2f size;
        if (resourceNode.contains("widthSize"))
        {
            size.x() = resourceNode["widthSize"].get<float>();
        }
        else{
            size.x() = GetWindowSize().x() * resourceNode["widthScale"].get<float>();
        }
        if (resourceNode.contains("heightSize"))
        {
            size.y() = resourceNode["heightSize"].get<float>();
        }
        else{
            size.y() = GetWindowSize().y() * resourceNode["heightScale"].get<float>();
        }
        return size;
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
        const nlohmann::json& configJson = InitConfigJson();
        projectPath = configJson["projectPath"];
        return projectPath.value();
    }

    inline std::string Path(const std::string& path)
    {
        std::string fullPath = GetProjectPath() + "/resources" + "/" + path;
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

    inline float GetDeltaTime()
    {
        static auto startTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();
        startTime = currentTime;
        return deltaTime;
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
        for (uint32_t i = 0; i < physicalDeviceMemoryProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (physicalDeviceMemoryProperties.memoryTypes[i].propertyFlags & memoryPropertyFlags) == memoryPropertyFlags)
            {
                return i;
            }
        }
        throw std::runtime_error("Failed to find suitable memory type!");
    }

    inline vk::Format FindSupportedFormat(vk::PhysicalDevice& physicalDevice, const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features)
    {
        for (const auto& format : candidates) {
            vk::FormatProperties props = physicalDevice.getFormatProperties(format);
            if (tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & features) == features) 
            {
                return format;
            }
            else if (tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & features) == features) 
            {
                return format;
            }
        }

        return vk::Format::eUndefined;
    }

    inline vk::Format FindDepthFormat(vk::PhysicalDevice& physicalDevice)
    {
        return FindSupportedFormat(physicalDevice,
            { vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint },
            vk::ImageTiling::eOptimal,
            vk::FormatFeatureFlagBits::eDepthStencilAttachment);
    }

    inline bool IsDepthFormat(vk::Format format) {
        return format == vk::Format::eD32Sfloat || format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint;
    }

    inline bool HasStencilComponent(vk::Format format) {
        return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint;
    }

    inline vk::CommandBuffer BeginSingleTimeCommands(vk::Device& device, vk::CommandPool& commandPool)
    {
        vk::CommandBufferAllocateInfo allocInfo;
        allocInfo
            .setCommandPool(commandPool)
            .setLevel(vk::CommandBufferLevel::ePrimary)
            .setCommandBufferCount(1);

        vk::CommandBuffer commandBuffer = device.allocateCommandBuffers(allocInfo)[0];

        vk::CommandBufferBeginInfo beginInfo;
        beginInfo
            .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        commandBuffer.begin(beginInfo);
        return commandBuffer;
    }

    inline void EndSingleTimeCommands(vk::Device& device, vk::CommandBuffer& commandBuffer, vk::Queue& graphicsQueue, vk::CommandPool& commandPool)
    {
        commandBuffer.end();

        vk::SubmitInfo submitInfo;
        submitInfo
            .setCommandBuffers(commandBuffer);
        
        graphicsQueue.submit(submitInfo);
        graphicsQueue.waitIdle();

        device.freeCommandBuffers(commandPool, commandBuffer);
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

    inline std::pair<vk::Image, vk::DeviceMemory> CreateDepthImage(vk::Device& device, vk::PhysicalDevice& physicalDevice, uint32_t width, uint32_t height, vk::SampleCountFlagBits samples, vk::Format& format, vk::ImageTiling& tiling, vk::ImageUsageFlags& usage, vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, vk::MemoryPropertyFlags& memoryPropertyFlags, const std::string& name = "")
    {
        return CreateImage(device, width, height, 1, samples, format, tiling, usage, physicalDeviceMemoryProperties, memoryPropertyFlags, name);
    }

    inline vk::ImageView CreateImageView(vk::Device& device, vk::Image& image, uint32_t mipLevels, vk::Format& format, vk::ImageAspectFlagBits aspectMask = vk::ImageAspectFlagBits::eColor, const std::string& name = "")
    {
        vk::ImageViewCreateInfo viewInfo;
        viewInfo
            .setImage(image)
            .setViewType(vk::ImageViewType::e2D)
            .setFormat(format)
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(aspectMask)
                .setBaseMipLevel(0)
                .setLevelCount(mipLevels)
                .setBaseArrayLayer(0)
                .setLayerCount(1));

        vk::ImageView imageView = device.createImageView(viewInfo);
        if (!name.empty())
        {
            VulkanDebug::SetObjectName(device, imageView, vk::ObjectType::eImageView, name);
        }
        return imageView;
    }

    inline vk::ImageView CreateDepthImageView(vk::Device& device, vk::PhysicalDevice& physicalDevice, vk::Image& image, vk::Format& format, const std::string& name = "")
    {
        return CreateImageView(device, image, 1, format, vk::ImageAspectFlagBits::eDepth, name);
    }

    inline vk::Sampler CreateSampler(vk::Device& device, vk::PhysicalDevice& physicalDevice, bool bUseDepth = false, const std::string& name = "")
    {

        vk::PhysicalDeviceProperties physicalDeviceProperties = physicalDevice.getProperties();
        vk::PhysicalDeviceFeatures physicalDeviceFeatures = physicalDevice.getFeatures();
        
        vk::SamplerCreateInfo samplerInfo;
        samplerInfo
            .setMagFilter(vk::Filter::eLinear)
            .setMinFilter(vk::Filter::eLinear)
            .setAddressModeU(vk::SamplerAddressMode::eRepeat)
            .setAddressModeV(vk::SamplerAddressMode::eRepeat)
            .setAddressModeW(vk::SamplerAddressMode::eRepeat)
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

        if(bUseDepth)
        {
            samplerInfo
                .setCompareEnable(VK_TRUE)
                .setCompareOp(vk::CompareOp::eLessOrEqual);
        }

        vk::Sampler sampler = device.createSampler(samplerInfo);
        if (!name.empty())
        {
            VulkanDebug::SetObjectName(device, sampler, vk::ObjectType::eSampler, name);
        }
        return sampler;
    }

    inline void  TransitionImageLayout(vk::Image& image, uint32_t mipLevels, vk::Format& format, vk::Device& device, vk::CommandPool& commandPool, vk::Queue& GraphicsQueue, vk::ImageLayout oldLayout, vk::ImageLayout newLayout)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

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
                .setBaseArrayLayer(0)
                .setLayerCount(1));

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
            throw std::invalid_argument("unsupported layout transition!");
        }   
    
        commandBuffer.pipelineBarrier(
            sourceStage, 
            destinationStage,
            vk::DependencyFlags(),
            nullptr, nullptr, barrier);

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

    inline void CopyBufferToImage(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Buffer& buffer, vk::Image& image, uint32_t width, uint32_t height, bool flipY = false, vk::DeviceSize rowBytes = 0)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        if (!flipY)
        {
            vk::BufferImageCopy region;
            region
                .setBufferOffset(0)
                .setBufferRowLength(0)
                .setBufferImageHeight(0)
                .setImageSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(0)
                    .setBaseArrayLayer(0)
                    .setLayerCount(1))
                .setImageOffset(vk::Offset3D{ 0, 0, 0 })
                .setImageExtent(vk::Extent3D{ width, height, 1 });
            commandBuffer.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, 1, &region);
        }
        else
        {
            std::vector<vk::BufferImageCopy> regions = MakeFlipYBufferImageCopyRegions(width, height, rowBytes, true);
            commandBuffer.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, static_cast<uint32_t>(regions.size()), regions.data());
        }

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    inline void CopyImageToBuffer(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Image& image, vk::Buffer& buffer, uint32_t width, uint32_t height, bool flipY = false, vk::DeviceSize rowBytes = 0)
    {
        vk::CommandBuffer commandBuffer = BeginSingleTimeCommands(device, commandPool);

        if (!flipY)
        {
            vk::BufferImageCopy region;
            region
                .setBufferOffset(0)
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
            commandBuffer.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer, static_cast<uint32_t>(regions.size()), regions.data());
        }

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }

    inline void GenerateMipmaps(vk::Device& device, vk::Queue& GraphicsQueue, vk::CommandPool& commandPool, vk::Image& image, uint32_t width, uint32_t height, uint32_t mipLevels)
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
                .setBaseArrayLayer(0)
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
                    .setBaseArrayLayer(0)
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
                    .setBaseArrayLayer(0)
                    .setLayerCount(1))
                .setDstOffsets
                ({
                    vk::Offset3D{ 0, 0, 0 },
                    vk::Offset3D{ mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 }
                })
                .setDstSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(i)
                    .setBaseArrayLayer(0)
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


            if (mipWidth > 1 && mipHeight > 1)
            {
                mipWidth /= 2;
                mipHeight /= 2;
            }
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
                .setBaseArrayLayer(0)
                .setLayerCount(1));

        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr, nullptr,
            barrier);

        EndSingleTimeCommands(device, commandBuffer, GraphicsQueue, commandPool);
    }
    inline vk::SampleCountFlagBits GetMaxUsableSampleCount(vk::PhysicalDevice& physicalDevice)
    {
        vk::PhysicalDeviceProperties physicalDeviceProperties = physicalDevice.getProperties();

        vk::SampleCountFlags count = physicalDeviceProperties.limits.framebufferColorSampleCounts & 
                                        physicalDeviceProperties.limits.framebufferDepthSampleCounts;
        if (count & vk::SampleCountFlagBits::e64) { return vk::SampleCountFlagBits::e64; }
        else if( count & vk::SampleCountFlagBits::e32) { return vk::SampleCountFlagBits::e32; }
        else if (count & vk::SampleCountFlagBits::e16) { return vk::SampleCountFlagBits::e16; }
        else if (count & vk::SampleCountFlagBits::e8) { return vk::SampleCountFlagBits::e8; }
        else if (count & vk::SampleCountFlagBits::e4) { return vk::SampleCountFlagBits::e4; }
        else if (count & vk::SampleCountFlagBits::e2) { return vk::SampleCountFlagBits::e2; }
        else { return vk::SampleCountFlagBits::e1; }
    }
}
