#pragma once

#include <stdexcept>
#include <string>

#include <vulkan/vulkan.hpp>

namespace VL
{

inline void RequireVulkanPipelineSuccess(
    vk::Result result,
    const std::string& operation,
    const std::string& pipelineName,
    const std::string& pipelineKind)
{
    if (result == vk::Result::eSuccess)
    {
        return;
    }

    throw std::runtime_error(
        operation +
        " failed for " +
        pipelineKind +
        " '" +
        pipelineName +
        "': " +
        vk::to_string(result));
}

} // namespace VL
