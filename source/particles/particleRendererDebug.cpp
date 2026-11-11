#include "particles/particleRendererInternal.h"

namespace VL
{

bool ParticleRenderer::DumpParticles(
    RendererBackendVulkan& readbackBackend,
    const std::filesystem::path& outputPath,
    uint32_t maxPerEmitter,
    const ParticleFrameStats& frameStats,
    std::string& outError)
{
    static_assert(
        sizeof(GpuParticle) == sizeof(Eigen::Vector4f) * 21,
        "Particle dump assumes the std430 Particle layout is 21 vec4s.");

    if (!initialized || backend != &readbackBackend)
    {
        outError =
            "Particle renderer is not initialized for this backend.";
        return false;
    }
    if (emitters.empty())
    {
        outError = "No particle emitters are available for dump.";
        return false;
    }

    maxPerEmitter = std::min(maxPerEmitter, kParticleDumpMaxDetails);
    std::vector<ParticleDumpEmitter> dumpEmitters;
    dumpEmitters.reserve(emitters.size());

    for (const auto& [key, emitterPtr] : emitters)
    {
        const EmitterResources& emitter = *emitterPtr;
        if (!emitter.particles.buffer ||
            !emitter.particles.memory ||
            emitter.asset.maxParticles == 0)
        {
            outError = "Particle emitter has no readable particle pool: " + key;
            return false;
        }

        const vk::DeviceSize particleBytes =
            sizeof(GpuParticle) *
            static_cast<vk::DeviceSize>(emitter.asset.maxParticles);
        const vk::DeviceSize readbackBytes =
            particleBytes + sizeof(ParticleOverdrawCounters);
        vk::Buffer readbackBuffer;
        vk::DeviceMemory readbackMemory;
        void* mappedMemory = nullptr;
        bool mapped = false;
        try
        {
            std::tie(readbackBuffer, readbackMemory) =
                readbackBackend.CreateBuffer(
                    readbackBytes,
                    vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleDumpReadback:" + key);

            vk::CommandBuffer commandBuffer =
                readbackBackend.BeginSingleTimeCommands();
            vk::BufferMemoryBarrier toTransfer;
            toTransfer
                .setSrcAccessMask(
                    vk::AccessFlagBits::eMemoryRead |
                    vk::AccessFlagBits::eMemoryWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setBuffer(emitter.particles.buffer)
                .setOffset(0)
                .setSize(VK_WHOLE_SIZE);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                emitter.particles.buffer,
                readbackBuffer,
                // vkCmdCopyBuffer 不接受 VK_WHOLE_SIZE；必须给出实际字节数。
                vk::BufferCopy(0, 0, particleBytes));

            toTransfer.setBuffer(emitter.debugCounters.buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                emitter.debugCounters.buffer,
                readbackBuffer,
                vk::BufferCopy(
                    0,
                    particleBytes,
                    sizeof(ParticleOverdrawCounters)));

            vk::BufferMemoryBarrier toHost;
            toHost
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eHostRead)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setBuffer(readbackBuffer)
                .setOffset(0)
                .setSize(VK_WHOLE_SIZE);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eHost,
                {},
                nullptr,
                toHost,
                nullptr);
            // End 内部提交并等待 fence；返回后 host-coherent staging 可安全映射。
            readbackBackend.EndSingleTimeCommands(commandBuffer);

            mappedMemory = readbackBackend.MapMemory(
                readbackMemory,
                readbackBytes);
            mapped = mappedMemory != nullptr;
            if (!mappedMemory)
            {
                throw std::runtime_error("map returned null");
            }

            const GpuParticle* particles =
                static_cast<const GpuParticle*>(mappedMemory);
            const ParticleOverdrawCounters* overdraw =
                reinterpret_cast<const ParticleOverdrawCounters*>(
                    static_cast<const std::uint8_t*>(mappedMemory) +
                    particleBytes);
            ParticleDumpEmitter dumpEmitter;
            dumpEmitter.key = emitter.key;
            // beam 不占用模拟粒子池；dump 用 drawInstances 表达弧数量。
            dumpEmitter.maxParticles =
                emitter.asset.renderMode == "beam"
                    ? 0u
                    : emitter.asset.maxParticles;
            dumpEmitter.drawInstances =
                emitter.asset.renderMode == "beam"
                    ? static_cast<uint32_t>(
                        ParticleBeamInstanceCount(
                            emitter.asset.beam.count,
                            emitter.asset.beam.branchCount))
                    : 0u;
            dumpEmitter.culled = emitter.culled;
            dumpEmitter.sortParticles = emitter.asset.sortParticles;
            dumpEmitter.localSpace = emitter.asset.localSpace;
            dumpEmitter.renderMode = emitter.asset.renderMode;
            if (emitter.asset.renderMode == "light")
            {
                // light 只写 GPU light SSBO，不产生可见 draw；dump 不能把
                // 存活粒子误报成 billboard 顶点。
                dumpEmitter.verticesPerParticle = 0u;
            }
            else if (emitter.asset.renderMode == "mesh" &&
                !emitter.meshFallbackBillboard &&
                emitter.meshIndexCount > 0)
            {
                dumpEmitter.verticesPerParticle =
                    emitter.meshIndexCount;
            }
            else
            {
                if (emitter.asset.renderMode == "beam")
                {
                    dumpEmitter.verticesPerParticle =
                        ParticleBeamVerticesPerParticle(
                            emitter.asset.beam.segments);
                }
                else
                {
                    dumpEmitter.verticesPerParticle =
                        emitter.asset.renderMode == "ribbon"
                            ? ParticleRibbonVerticesPerParticle(
                                emitter.asset.ribbonSegments)
                            : 6u;
                }
            }
            dumpEmitter.boundsCenter = emitter.worldCenter;
            dumpEmitter.boundsRadius = emitter.boundsRadius;
            dumpEmitter.elapsedSeconds = emitter.elapsedSeconds;
            // 被剔除的 emitter 不录制图形 pass，计数器会保留上一帧值，
            // dump 必须显式归零而不是发布陈旧 overdraw。
            dumpEmitter.overdrawFragments = emitter.culled
                ? 0u
                : overdraw->fragments;
            dumpEmitter.particles.reserve(
                std::min(
                    emitter.asset.maxParticles,
                    maxPerEmitter));

            for (uint32_t index = 0;
                 index < emitter.asset.maxParticles;
                 ++index)
            {
                const GpuParticle& particle = particles[index];
                const float age = particle.positionAge.w();
                const float lifetime = particle.velocityLife.w();
                if (lifetime > 0.0f && age < lifetime)
                {
                    ++dumpEmitter.aliveCount;
                    if (dumpEmitter.particles.size() < maxPerEmitter)
                    {
                        ParticleDumpSample sample;
                        sample.index = index;
                        sample.position =
                            particle.positionAge.head<3>();
                        sample.age = age;
                        sample.velocity =
                            particle.velocityLife.head<3>();
                        sample.lifetime = lifetime;
                        dumpEmitter.particles.push_back(sample);
                    }
                }
            }
            dumpEmitters.push_back(std::move(dumpEmitter));
            dumpEmitters.back().estimatedParticleBufferReadBytes =
                static_cast<uint64_t>(
                    dumpEmitters.back().aliveCount) *
                sizeof(GpuParticle);

            readbackBackend.UnmapMemory(readbackMemory);
            mapped = false;
            readbackBackend.DestroyBuffer(
                readbackBuffer,
                readbackMemory);
            readbackBuffer = vk::Buffer();
            readbackMemory = vk::DeviceMemory();
        }
        catch (const std::exception& exception)
        {
            if (mapped)
            {
                readbackBackend.UnmapMemory(readbackMemory);
            }
            if (readbackBuffer || readbackMemory)
            {
                readbackBackend.DestroyBuffer(
                    readbackBuffer,
                    readbackMemory);
            }
            outError =
                "Particle dump readback failed for " + key + ": " +
                exception.what();
            return false;
        }
    }

    ParticleDumpGlobalStats globalStats;
    const uint32_t globalImageIndex =
        std::min(
            globalSortPool.lastImageIndex,
            static_cast<uint32_t>(
                std::max<size_t>(
                    globalSortPool.counters.size(),
                    1u) - 1u));
    if (globalSortPool.active &&
        globalSortPool.counters.size() > globalImageIndex &&
        globalSortPool.debugCounters.size() > globalImageIndex)
    {
        const vk::DeviceSize counterBytes =
            sizeof(ParticleEventCounters);
        const vk::DeviceSize overdrawBytes =
            sizeof(ParticleOverdrawCounters);
        const vk::DeviceSize readbackBytes =
            counterBytes + overdrawBytes;
        vk::Buffer readbackBuffer;
        vk::DeviceMemory readbackMemory;
        try
        {
            std::tie(readbackBuffer, readbackMemory) =
                readbackBackend.CreateBuffer(
                    readbackBytes,
                    vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleGlobalDumpReadback");
            vk::CommandBuffer commandBuffer =
                readbackBackend.BeginSingleTimeCommands();
            vk::BufferMemoryBarrier toTransfer;
            toTransfer
                .setSrcAccessMask(
                    vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            toTransfer.setBuffer(
                globalSortPool.counters[globalImageIndex].buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                globalSortPool.counters[globalImageIndex].buffer,
                readbackBuffer,
                vk::BufferCopy(0, 0, counterBytes));
            toTransfer.setBuffer(
                globalSortPool.debugCounters[globalImageIndex].buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                globalSortPool.debugCounters[globalImageIndex].buffer,
                readbackBuffer,
                vk::BufferCopy(0, counterBytes, overdrawBytes));
            readbackBackend.EndSingleTimeCommands(commandBuffer);

            const std::uint8_t* mapped =
                static_cast<const std::uint8_t*>(
                    readbackBackend.MapMemory(
                        readbackMemory,
                        readbackBytes));
            if (mapped == nullptr)
            {
                throw std::runtime_error("map returned null");
            }
            const ParticleEventCounters* counters =
                reinterpret_cast<const ParticleEventCounters*>(mapped);
            const ParticleOverdrawCounters* overdraw =
                reinterpret_cast<const ParticleOverdrawCounters*>(
                    mapped + counterBytes);
            globalStats.aliveParticles = counters->alive;
            globalStats.overdrawFragments = overdraw->fragments;
            globalStats.estimatedParticleBufferReadBytes =
                static_cast<uint64_t>(counters->alive) *
                sizeof(GpuParticle);
            readbackBackend.UnmapMemory(readbackMemory);
            readbackBackend.DestroyBuffer(
                readbackBuffer,
                readbackMemory);
        }
        catch (const std::exception& exception)
        {
            if (readbackBuffer || readbackMemory)
            {
                readbackBackend.DestroyBuffer(
                    readbackBuffer,
                    readbackMemory);
            }
            outError =
                "Particle global dump readback failed: " +
                std::string(exception.what());
            return false;
        }
    }

    try
    {
        std::filesystem::create_directories(outputPath.parent_path());
        const std::string text =
            BuildParticleDumpJson(
                std::move(dumpEmitters),
                maxPerEmitter,
                frameStats,
                globalStats).dump(2) + "\n";
        WriteTextFileAtomically(outputPath, text);
    }
    catch (const std::exception& exception)
    {
        outError =
            "Particle dump write failed: " +
            std::string(exception.what());
        return false;
    }
    return true;
}

} // namespace VL
