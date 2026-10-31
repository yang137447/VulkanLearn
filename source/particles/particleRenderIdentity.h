#pragma once

#include <cstdint>
#include <string>

namespace VL
{

// 粒子管线依赖 RenderGraph 的 particle pass 和它绑定的 swapchain image 数。
// ownerGeneration 在 resize 后会被刻意写回旧值，因此不能单独作为图身份；
// pass handle、附件尺寸和管线契约必须一起参与比较。
struct ParticleRenderIdentity
{
    uint64_t ownerGeneration = 0;
    uintptr_t particleRenderPass = 0;
    uint32_t swapchainImageCount = 0;
    uint32_t passWidth = 0;
    uint32_t passHeight = 0;
    std::string pipelineContractKey;

    bool operator==(const ParticleRenderIdentity& other) const noexcept
    {
        return ownerGeneration == other.ownerGeneration &&
            particleRenderPass == other.particleRenderPass &&
            swapchainImageCount == other.swapchainImageCount &&
            passWidth == other.passWidth &&
            passHeight == other.passHeight &&
            pipelineContractKey == other.pipelineContractKey;
    }

    bool operator!=(const ParticleRenderIdentity& other) const noexcept
    {
        return !(*this == other);
    }
};

} // namespace VL
