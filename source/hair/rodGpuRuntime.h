#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <iostream>
#include <tuple>
#include <vector>

#include <Eigen/Geometry>

#include "hair/tressfxSimulationParams.h"
#include "render/backend/rendererBackendVulkan.h"

namespace VL::Hair
{
inline constexpr uint32_t RodGpuMaxSteps = 8;
inline constexpr uint32_t RodGpuSweeps = 16;
inline constexpr double RodGpuStepSeconds = 1.0 / 240.0;
inline constexpr uint32_t RodGpuGuideVec4Count = 32 * 5 + 31 * 8 + 4;
inline constexpr uint32_t RodGpuReferenceVec4Count = 96;

struct alignas(16) RodGpuParams
{
    Eigen::Matrix4f hairToWorld = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f worldToHair = Eigen::Matrix4f::Identity();
    std::array<Eigen::Matrix4f, RodGpuMaxSteps + 1> roots{};
    Eigen::Vector4f gravityStep = Eigen::Vector4f::Zero();
    Eigen::Vector4f material = Eigen::Vector4f(0.0005f, 1300.0f, 1.0e6f, 0.4e6f);
    Eigen::Vector4f support = Eigen::Vector4f(0.01f, 0.5f, 9.8f, 0.0f);
    Eigen::Vector4f timing = Eigen::Vector4f::Zero();
    std::array<uint32_t, 4> counts{};
    std::array<uint32_t, 4> flags{};
    std::array<std::array<GuideColliderGpu, 8>, RodGpuMaxSteps + 1> capsules{};
    Eigen::Vector4f windDrag = Eigen::Vector4f::Zero();
    Eigen::Vector4f probeAcceleration = Eigen::Vector4f::Zero();
};
static_assert(sizeof(RodGpuParams) == 3136);
static_assert(offsetof(RodGpuParams, gravityStep) == 704);
static_assert(offsetof(RodGpuParams, counts) == 768);
static_assert(offsetof(RodGpuParams, capsules) == 800);

struct RodGpuBuffer
{
    RendererBackendVulkan* backend = nullptr;
    vk::Buffer buffer = nullptr;
    vk::DeviceMemory memory = nullptr;
    vk::DeviceSize size = 0;
    void* mapped = nullptr;

    ~RodGpuBuffer()
    {
        if (backend && buffer)
        {
            if (mapped)
                backend->UnmapMemory(memory);
            backend->DestroyBuffer(buffer, memory);
        }
    }

    void Create(RendererBackendVulkan& renderer, vk::DeviceSize bytes,
        bool host, const std::string& name)
    {
        backend = &renderer;
        size = bytes;
        std::tie(buffer, memory) = renderer.CreateBuffer(bytes,
            vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eUniformBuffer |
                vk::BufferUsageFlagBits::eTransferSrc |
                vk::BufferUsageFlagBits::eTransferDst,
            host ? vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent
                 : vk::MemoryPropertyFlagBits::eDeviceLocal, name);
        if (host)
            mapped = renderer.MapMemory(memory, bytes);
    }

    vk::DescriptorBufferInfo Info() const
    {
        return vk::DescriptorBufferInfo(buffer, 0, size);
    }
};

struct RodGpuPose
{
    Eigen::Matrix4f root = Eigen::Matrix4f::Identity();
    std::vector<Eigen::Matrix4f> palette;
};

struct RodGpuPoseInterval
{
    double begin = 0.0;
    double end = 0.0;
    RodGpuPose first;
    RodGpuPose last;
};

inline Eigen::Matrix4f RodGpuInterpolateRigid(
    const Eigen::Matrix4f& first, const Eigen::Matrix4f& last, float fraction)
{
    Eigen::Quaternionf initial(first.block<3, 3>(0, 0));
    Eigen::Quaternionf final(last.block<3, 3>(0, 0));
    Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
    result.block<3, 3>(0, 0) =
        initial.normalized().slerp(fraction, final.normalized()).toRotationMatrix();
    result.block<3, 1>(0, 3) = first.block<3, 1>(0, 3) * (1.0f - fraction) +
        last.block<3, 1>(0, 3) * fraction;
    return result;
}

inline bool RodGpuIsRigid(const Eigen::Matrix4f& matrix)
{
    const Eigen::Matrix3f rotation = matrix.block<3, 3>(0, 0);
    return matrix.allFinite() &&
        (rotation.transpose() * rotation - Eigen::Matrix3f::Identity()).norm() < 1.0e-3f &&
        std::abs(rotation.determinant() - 1.0f) < 1.0e-3f;
}

struct RodGpuObject
{
    std::weak_ptr<RenderableObject> owner;
    RodGpuBuffer state;
    RodGpuBuffer sdfHistory;
    RodGpuBuffer references;
    RodGpuBuffer frameDeltas;
    RodGpuBuffer lowParts;
    RodGpuBuffer failureMatrix;
    RodGpuBuffer failureCandidate;
    uint32_t captureFailureSlot = UINT32_MAX;
    bool failureEvidenceWritten = false;
    std::array<RodGpuBuffer, GuideSimulationParamBufferCount> params;
    std::array<RodGpuBuffer, GuideSimulationParamBufferCount> samples;
    std::array<RodGpuBuffer, GuideSimulationParamBufferCount> implicitParams;
    std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> solveSets{};
    std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> followSets{};
    std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> sdfHistorySets{};
    std::array<RodGpuBuffer, GuideSimulationParamBufferCount> readback;
    std::array<bool, GuideSimulationParamBufferCount> readbackPending{};
    std::array<uint64_t, GuideSimulationParamBufferCount> readbackFrame{};
    std::array<uint64_t, GuideSimulationParamBufferCount> readbackGeneration{};
    vk::DescriptorPool pool = nullptr;
    RendererBackendVulkan* backend = nullptr;
    vk::Buffer current = nullptr;
    uint32_t paletteCapacity = 0;
    uint32_t guides = 0;
    uint32_t algorithmVersion = 1;
    uint32_t profileMathVersion = 1;
    uint32_t kineticMathVersion = 1;
    uint32_t numericEncodingVersion = 1;
    Eigen::Vector3f fixedWorldOrigin = Eigen::Vector3f::Zero();
    uint64_t world = 0;
    uint64_t lastFrame = UINT64_MAX;
    uint64_t simulationGeneration = 0;
    uint32_t lastResetReason = 0;
    double receivedSeconds = 0.0;
    double dispatchedSeconds = 0.0;
    double debtSeconds = 0.0;
    double acceptedSeconds = 0.0;
    uint32_t gpuStatus = 0;
    RodGpuPose previousPose;
    RodGpuPose referencePose;
    bool poseInitialized = false;
    bool initialized = false;
    bool reset = true;
    bool reported = false;
    std::deque<RodGpuPoseInterval> intervals;

    ~RodGpuObject()
    {
        if (backend && pool)
        {
            std::cout << "[RodGPU] destroy descriptor pool=0x" << std::hex <<
                reinterpret_cast<uintptr_t>(static_cast<VkDescriptorPool>(pool)) <<
                std::dec << " world=" << world << std::endl;
            backend->DestroyDescriptorPool(pool);
        }
    }

    void ClearTimeline(const RodGpuPose& pose)
    {
        intervals.clear();
        previousPose = pose;
        referencePose = pose;
        poseInitialized = true;
        receivedSeconds = 0.0;
        dispatchedSeconds = 0.0;
        debtSeconds = 0.0;
        acceptedSeconds = 0.0;
        gpuStatus = 0;
        ++simulationGeneration;
        reset = true;
    }

    RodGpuPose Sample(double time) const
    {
        if (intervals.empty())
            return previousPose;
        const RodGpuPoseInterval* interval = &intervals.back();
        for (const RodGpuPoseInterval& candidate : intervals)
            if (time <= candidate.end + 1.0e-10)
            {
                interval = &candidate;
                break;
            }
        const float fraction = static_cast<float>(std::clamp(
            (time - interval->begin) / (interval->end - interval->begin), 0.0, 1.0));
        RodGpuPose pose;
        pose.root = RodGpuInterpolateRigid(interval->first.root, interval->last.root, fraction);
        pose.palette.resize(interval->last.palette.size());
        for (size_t joint = 0; joint < pose.palette.size(); ++joint)
            pose.palette[joint] = RodGpuInterpolateRigid(
                interval->first.palette[joint], interval->last.palette[joint], fraction);
        return pose;
    }
};
}
