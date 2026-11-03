#pragma once

// File responsibility: Builds the CPU-side particle dump document and resolves
// a safe output path. Vulkan readback stays in ParticleRenderer so tests can
// validate statistics and truncation without a device.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "particles/particleFrameStats.h"

namespace VL
{

constexpr uint32_t kParticleDumpDefaultDetails = 128;
constexpr uint32_t kParticleDumpMaxDetails = 4096;

struct ParticleDumpSample
{
    uint32_t index = 0;
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    float age = 0.0f;
    Eigen::Vector3f velocity = Eigen::Vector3f::Zero();
    float lifetime = 0.0f;

    bool IsAlive() const noexcept
    {
        return lifetime > 0.0f && age < lifetime;
    }
};

// One emitter's readback result. `particles` may contain dead entries; the
// serializer filters them and keeps the first N alive samples in pool order.
// `aliveCount` is counted over the whole pool, not only the sampled subset.
struct ParticleDumpEmitter
{
    std::string key;
    uint32_t maxParticles = 0;
    uint32_t aliveCount = 0;
    // 本帧通过 discard 后的片元数（GPU overdraw 样本）。
    uint64_t overdrawFragments = 0;
    // aliveCount * GpuParticle 的字节数；这是着色器读取规模的透明估算，
    // 不是硬件显存带宽计数器。
    uint64_t estimatedParticleBufferReadBytes = 0;
    bool culled = false;
    bool sortParticles = false;
    bool localSpace = false;
    std::string renderMode = "billboard";
    // billboard 为 6；ribbon 为 ribbonSegments * 6；beam 为 segments * 6；
    // mesh 为合并后的索引数。
    uint32_t verticesPerParticle = 6;
    // beam 用作者 count 作为实例数；0 表示沿用 maxParticles。
    uint32_t drawInstances = 0;
    Eigen::Vector3f boundsCenter = Eigen::Vector3f::Zero();
    float boundsRadius = 0.0f;
    float elapsedSeconds = 0.0f;
    std::vector<ParticleDumpSample> particles;
};

// 全局 alpha billboard 池不是独立 emitter；其回读结果单独发布，避免把
// 容量重复计入逐 emitter 总和。
struct ParticleDumpGlobalStats
{
    uint32_t aliveParticles = 0;
    uint64_t overdrawFragments = 0;
    uint64_t estimatedParticleBufferReadBytes = 0;
};

inline nlohmann::ordered_json ParticleDumpVectorJson(
    const Eigen::Vector3f& value)
{
    return nlohmann::ordered_json::array(
        {static_cast<double>(value.x()),
         static_cast<double>(value.y()),
         static_cast<double>(value.z())});
}

inline nlohmann::ordered_json BuildParticleDumpJson(
    std::vector<ParticleDumpEmitter> emitters,
    uint32_t maxPerEmitter,
    const ParticleFrameStats& frameStats = ParticleFrameStats{},
    const ParticleDumpGlobalStats& globalStats = ParticleDumpGlobalStats{})
{
    // unordered_map iteration is not stable; dumps are regression artifacts,
    // so sort by the same stable emitter key used by the runtime identity.
    std::stable_sort(
        emitters.begin(),
        emitters.end(),
        [](const ParticleDumpEmitter& left, const ParticleDumpEmitter& right)
        {
            return left.key < right.key;
        });

    uint64_t totalParticles = 0;
    uint64_t aliveParticles = 0;
    uint64_t estimatedDrawnVertices = 0;
    uint64_t estimatedParticleBufferReadBytes = 0;
    uint32_t visibleEmitters = 0;

    nlohmann::ordered_json emitterArray =
        nlohmann::ordered_json::array();
    for (const ParticleDumpEmitter& emitter : emitters)
    {
        totalParticles += emitter.maxParticles;
        aliveParticles += emitter.aliveCount;
        estimatedParticleBufferReadBytes +=
            emitter.estimatedParticleBufferReadBytes;
        const uint64_t instanceCount =
            emitter.drawInstances > 0
                ? emitter.drawInstances
                : emitter.maxParticles;
        const uint64_t drawnVertices =
            instanceCount *
            std::max(emitter.verticesPerParticle, 1u);
        if (!emitter.culled)
        {
            ++visibleEmitters;
            estimatedDrawnVertices += drawnVertices;
        }

        nlohmann::ordered_json emitterJson =
            nlohmann::ordered_json::object();
        emitterJson["key"] = emitter.key;
        emitterJson["maxParticles"] = emitter.maxParticles;
        emitterJson["aliveCount"] = emitter.aliveCount;
        emitterJson["overdrawFragments"] = emitter.overdrawFragments;
        emitterJson["estimatedParticleBufferReadBytes"] =
            emitter.estimatedParticleBufferReadBytes;
        emitterJson["totalParticles"] = emitter.maxParticles;
        emitterJson["culled"] = emitter.culled;
        emitterJson["sortParticles"] = emitter.sortParticles;
        emitterJson["drawInstances"] =
            emitter.drawInstances > 0
                ? emitter.drawInstances
                : emitter.maxParticles;
        emitterJson["localSpace"] = emitter.localSpace;
        emitterJson["renderMode"] = emitter.renderMode;
        emitterJson["bounds"] = nlohmann::ordered_json{
            {"center", ParticleDumpVectorJson(emitter.boundsCenter)},
            {"radius", static_cast<double>(emitter.boundsRadius)}};
        emitterJson["elapsedSeconds"] =
            static_cast<double>(emitter.elapsedSeconds);
        emitterJson["drawnVertices"] = drawnVertices;

        nlohmann::ordered_json particleArray =
            nlohmann::ordered_json::array();
        for (const ParticleDumpSample& sample : emitter.particles)
        {
            if (particleArray.size() >= maxPerEmitter)
            {
                break;
            }
            if (!sample.IsAlive())
            {
                continue;
            }

            particleArray.push_back(nlohmann::ordered_json{
                {"index", sample.index},
                {"position", ParticleDumpVectorJson(sample.position)},
                {"age", static_cast<double>(sample.age)},
                {"velocity", ParticleDumpVectorJson(sample.velocity)},
                {"lifetime", static_cast<double>(sample.lifetime)}});
        }
        emitterJson["particles"] = std::move(particleArray);
        emitterArray.push_back(std::move(emitterJson));
    }

    nlohmann::ordered_json root =
        nlohmann::ordered_json::object();
    root["kind"] = "particleDump";
    root["version"] = 1;
    root["maxPerEmitter"] = maxPerEmitter;
    root["totalEmitters"] = emitters.size();
    root["visibleEmitters"] = visibleEmitters;
    root["totalParticles"] = totalParticles;
    root["aliveParticles"] = aliveParticles;
    root["estimatedDrawnVertices"] = estimatedDrawnVertices;
    root["estimatedParticleBufferReadBytes"] =
        estimatedParticleBufferReadBytes;
    root["globalAliveParticles"] = globalStats.aliveParticles;
    root["globalOverdrawFragments"] = globalStats.overdrawFragments;
    root["globalEstimatedParticleBufferReadBytes"] =
        globalStats.estimatedParticleBufferReadBytes;
    // GPU pass 计时来自 RenderSystem 的 particle timestamp timer；
    // 不支持时为 false 且数值保持 0。
    root["gpuTimingSupported"] = frameStats.gpuTimingSupported;
    root["gpuMilliseconds"] = frameStats.gpuMilliseconds;
    root["gpuAverageMilliseconds"] = frameStats.gpuAverageMilliseconds;
    root["gpuMaxMilliseconds"] = frameStats.gpuMaxMilliseconds;
    root["gpuSampleCount"] = frameStats.gpuSampleCount;
    root["emitters"] = std::move(emitterArray);
    return root;
}

inline bool ResolveParticleDumpPath(
    const std::filesystem::path& resourceRoot,
    const std::string& requestedName,
    std::filesystem::path& outputPath,
    std::string& error)
{
    if (requestedName.empty())
    {
        error = "Particle dump file name must not be empty.";
        return false;
    }

    std::filesystem::path relativePath(requestedName);
    if (relativePath.is_absolute() ||
        relativePath.has_root_name() ||
        relativePath.has_root_directory())
    {
        error = "Particle dump file name must be relative to the resource root.";
        return false;
    }
    for (const std::filesystem::path& component : relativePath)
    {
        if (component == "..")
        {
            error = "Particle dump file name must not contain '..'.";
            return false;
        }
        if (component.native().find(':') !=
            std::filesystem::path::string_type::npos)
        {
            error = "Particle dump file name must not contain ':'.";
            return false;
        }
    }
    const std::string fileName = relativePath.filename().string();
    if (fileName.empty() || fileName == "." || fileName == "..")
    {
        error = "Particle dump file name must name a file.";
        return false;
    }

    // 输出契约固定为小写 .json；无扩展名时 replace_extension 会直接追加。
    relativePath.replace_extension(".json");

    outputPath = (resourceRoot / "Generated" / "ParticleDumps" / relativePath)
        .lexically_normal();
    return true;
}

} // namespace VL
