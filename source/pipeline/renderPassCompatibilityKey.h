#pragma once

// 文件职责：定义 Vulkan RenderPass 的图形管线兼容性身份，由 RenderGraph 构建并交给管线缓存；
// 不记录不参与 Vulkan render-pass compatibility 判定的 load/store 和 image layout。
// File responsibility: Defines Vulkan render-pass compatibility identity built by RenderGraph for pipeline
// caching; load/store operations and image layouts are excluded because they do not affect compatibility.

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

// 一项参与 RenderPass 兼容性判定的 attachment 格式与采样数。
// Format and sample count of one attachment participating in render-pass compatibility.
struct RenderPassAttachmentCompatibilityDesc
{
    vk::Format format = vk::Format::eUndefined;
    vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1;

    bool operator==(const RenderPassAttachmentCompatibilityDesc& other) const
    {
        return format == other.format && sampleCount == other.sampleCount;
    }
};

// 可共享图形管线的 Vulkan RenderPass 缓存身份。
// attachment 下标关系与格式参与比较，load/store 和 image layout 按 Vulkan 规则有意排除。
// Cache identity for Vulkan render passes that may share graphics pipelines.
// Attachment references and formats are compared; load/store operations and image layouts are excluded.
struct RenderPassCompatibilityKey
{
    std::vector<RenderPassAttachmentCompatibilityDesc> attachments;
    std::vector<uint32_t> colorAttachments;
    std::vector<uint32_t> resolveAttachments;
    bool hasDepthAttachment = false;
    uint32_t depthAttachment = VK_ATTACHMENT_UNUSED;
    bool hasDepthResolveAttachment = false;
    uint32_t depthResolveAttachment = VK_ATTACHMENT_UNUSED;

    bool operator==(const RenderPassCompatibilityKey& other) const
    {
        return attachments == other.attachments &&
            colorAttachments == other.colorAttachments &&
            resolveAttachments == other.resolveAttachments &&
            hasDepthAttachment == other.hasDepthAttachment &&
            depthAttachment == other.depthAttachment &&
            hasDepthResolveAttachment == other.hasDepthResolveAttachment &&
            depthResolveAttachment == other.depthResolveAttachment;
    }

    uint32_t GetColorAttachmentCount() const
    {
        return static_cast<uint32_t>(colorAttachments.size());
    }

    vk::SampleCountFlagBits GetRasterizationSampleCount() const
    {
        if (!colorAttachments.empty())
        {
            return attachments.at(colorAttachments.front()).sampleCount;
        }
        if (hasDepthAttachment)
        {
            return attachments.at(depthAttachment).sampleCount;
        }
        return vk::SampleCountFlagBits::e1;
    }

    std::string GetNormalizedKey() const
    {
        std::ostringstream stream;
        stream << "attachments=";
        for (const RenderPassAttachmentCompatibilityDesc& attachment : attachments)
        {
            stream << static_cast<uint32_t>(attachment.format) << ","
                   << static_cast<uint32_t>(attachment.sampleCount) << ";";
        }
        stream << "|color=";
        for (uint32_t attachment : colorAttachments)
        {
            stream << attachment << ",";
        }
        stream << "|resolve=";
        for (uint32_t attachment : resolveAttachments)
        {
            stream << attachment << ",";
        }
        stream << "|depth=" << hasDepthAttachment << "," << depthAttachment
               << "|depthResolve=" << hasDepthResolveAttachment << "," << depthResolveAttachment;
        return stream.str();
    }
};
