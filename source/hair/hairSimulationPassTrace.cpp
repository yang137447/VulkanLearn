#include "hair/hairSimulationPass.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "animation/skinPose.h"
#include "hair/tressfxSourceSchedule.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "renderableObject.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/resolvedRenderScene.h"

namespace VL::Hair
{
void HairSimulationPass::StartTraceObject(RenderableObject& object)
{
    const uint32_t guideCount = object.GetTressfxGuideCount();
    const uint32_t pointsPerGuide = object.GetTressfxPointsPerGuide();
    if (guideCount == 0 || pointsPerGuide < 2)
    {
        std::cerr << "[TressFX] Hair trace rejected: object \"" << object.GetName()
                  << "\" has no GPU guide geometry" << std::endl;
        trace = TraceState{};
        return;
    }

    const uint32_t stride = trace.request.guideStride;
    const uint64_t selected64 =
        (static_cast<uint64_t>(guideCount) + stride - 1) / stride;
    const uint32_t selected = static_cast<uint32_t>(
        std::min<uint64_t>(selected64, HairTraceMaxSelectedGuides));
    if (selected == 0)
    {
        trace = TraceState{};
        return;
    }

    try
    {
        trace.object = &object;
        trace.objectName = object.GetName();
        trace.totalGuideCount = guideCount;
        trace.pointsPerGuide = pointsPerGuide;
        trace.selectedCount = selected;
        trace.selectedGuides.resize(selected);
        for (uint32_t index = 0; index < selected; ++index)
            trace.selectedGuides[index] = index * stride;

        // 选中 guide 的 rest length 只拷一次：源 buffer 是 device-local，无法直接
        // 主机读取。它放在独立 host-visible buffer，第一次 flush 时再读出来。
        trace.restBytes = static_cast<vk::DeviceSize>(selected) * pointsPerGuide *
            sizeof(float);
        std::tie(trace.restBuffer, trace.restMemory) = backend->CreateBuffer(
            trace.restBytes,
            vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "HairTraceRestLengths");
        trace.restMapped = backend->MapMemory(trace.restMemory, trace.restBytes);

        const vk::DeviceSize blockBytes = static_cast<vk::DeviceSize>(selected) *
            pointsPerGuide * sizeof(Eigen::Vector4f);
        for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        {
            TraceSlot& target = trace.slots[slot];
            target.size = blockBytes * 3u + 2u * sizeof(float);
            std::tie(target.buffer, target.memory) = backend->CreateBuffer(
                target.size,
                vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "HairTraceReadback:" + std::to_string(slot));
            target.mapped = backend->MapMemory(target.memory, target.size);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "[TressFX] Hair trace rejected: " << error.what() << std::endl;
        DestroyTraceResources();
        trace = TraceState{};
        return;
    }

    std::cout << "[TressFX] Hair trace start: file=\"" << trace.request.path
              << "\" object=\"" << trace.objectName << "\" guideCount=" << guideCount
              << " selectedGuides=" << selected << " pointsPerGuide=" << pointsPerGuide
              << " stride=" << stride << " requestedFrames=" << trace.request.frames
              << " bytesPerFrame=" << (trace.slots[0].size + trace.restBytes)
              << std::endl;
}

bool HairSimulationPass::WriteTraceHeaderFromRestLengths()
{
    if (!trace.restMapped)
        return false;
    const size_t count = static_cast<size_t>(trace.selectedCount) * trace.pointsPerGuide;
    trace.restLengths.resize(count);
    const float* source = static_cast<const float*>(trace.restMapped);
    for (size_t index = 0; index < count; ++index)
    {
        const float value = source[index];
        if (!std::isfinite(value) || value < 0.0f)
        {
            std::cerr << "[TressFX] Hair trace aborted: rest length " << index
                      << " is not finite/non-negative" << std::endl;
            return false;
        }
        trace.restLengths[index] = value;
    }

    HairTraceHeader header;
    header.version = HairTraceVersion;
    header.totalGuideCount = trace.totalGuideCount;
    header.pointsPerGuide = trace.pointsPerGuide;
    header.stride = trace.request.guideStride;
    header.selectedCount = trace.selectedCount;
    header.startFrame = trace.hasStartFrame ? trace.startFrame : 0;
    header.selectedGuides = trace.selectedGuides;
    header.restLengths = trace.restLengths;

    std::string error;
    trace.writer = std::make_unique<HairTraceWriter>();
    if (!trace.writer->Open(trace.request.path, header, error))
    {
        std::cerr << "[TressFX] Hair trace aborted: " << error << std::endl;
        trace.writer.reset();
        return false;
    }
    trace.writerOpen = true;
    return true;
}

void HairSimulationPass::FlushTraceSlot(uint32_t frameSlot)
{
    TraceSlot& slot = trace.slots[frameSlot];
    if (!slot.pending)
        return;
    if (trace.writeFailed)
    {
        // 文件已经失败后不再尝试续写；剩余 slot 只清 pending，等 FinishTrace 收尾。
        slot.pending = false;
        trace.request.frames = static_cast<uint32_t>(trace.capturedFrames);
        trace.finishing = true;
        return;
    }
    if (!trace.writerOpen && !WriteTraceHeaderFromRestLengths())
    {
        // header 写不出去时立刻停止后续拷贝；已录制的 slot 不再尝试。
        trace.writeFailed = true;
        trace.finishing = true;
        trace.request.frames = static_cast<uint32_t>(trace.capturedFrames);
        slot.pending = false;
        return;
    }

    const size_t blockFloats =
        static_cast<size_t>(trace.selectedCount) * trace.pointsPerGuide * 4;
    const float* base = static_cast<const float*>(slot.mapped);
    const float* current = base;
    const float* previous = base + blockFloats;
    const float* previousPrevious = base + blockFloats * 2;
    // 末尾 2 个 float 是碰撞前 SDF/胶囊最大内部穿透深度。
    slot.info.sdfPenetrationMaxCm = base[blockFloats * 3 + 0];
    slot.info.capsulePenetrationMaxCm = base[blockFloats * 3 + 1];
    std::string error;
    if (!trace.writer->WriteFrame(
            slot.info, current, previous, previousPrevious, error))
    {
        std::cerr << "[TressFX] Hair trace aborted: " << error << std::endl;
        std::string closeError;
        trace.writer->Close(closeError);
        trace.writerOpen = false;
        trace.writeFailed = true;
        trace.finishing = true;
        trace.request.frames = static_cast<uint32_t>(trace.capturedFrames);
        slot.pending = false;
        return;
    }
    slot.pending = false;
    ++trace.flushedFrames;
    trace.bytesWritten = trace.writer->BytesWritten();
}

void HairSimulationPass::RecordTraceCopies(vk::CommandBuffer commandBuffer,
    uint32_t frameSlot,
    const HairTraceFrameInfo& info)
{
    if (!trace.active || trace.finishing || trace.object == nullptr)
        return;
    TraceSlot& slot = trace.slots[frameSlot];
    if (!slot.buffer || !trace.restBuffer)
        return;

    const vk::DeviceSize guideBytes = static_cast<vk::DeviceSize>(trace.pointsPerGuide) *
        sizeof(Eigen::Vector4f);
    const vk::DeviceSize blockBytes =
        static_cast<vk::DeviceSize>(trace.selectedCount) * guideBytes;
    const vk::Buffer sources[3] = {
        trace.object->GetTressfxPositionInfo().buffer,
        trace.object->GetTressfxPreviousPositionInfo().buffer,
        trace.object->GetTressfxPreviousPreviousPositionInfo().buffer,
    };

    // 所有模拟 dispatch（含 SDF collide）写完后，compute 的写入必须对 transfer
    // 读可见；trace 只读，不改任何求解状态。
    vk::MemoryBarrier toTransfer;
    toTransfer.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer, {}, toTransfer, nullptr, nullptr);

    std::vector<vk::BufferCopy> regions;
    regions.reserve(static_cast<size_t>(trace.selectedCount) * 3);
    for (uint32_t state = 0; state < 3; ++state)
    {
        regions.clear();
        for (uint32_t index = 0; index < trace.selectedCount; ++index)
        {
            const vk::DeviceSize sourceOffset =
                static_cast<vk::DeviceSize>(trace.selectedGuides[index]) * guideBytes;
            const vk::DeviceSize destinationOffset =
                static_cast<vk::DeviceSize>(state) * blockBytes +
                static_cast<vk::DeviceSize>(index) * guideBytes;
            regions.push_back(
                {sourceOffset, destinationOffset, guideBytes});
        }
        commandBuffer.copyBuffer(sources[state], slot.buffer, regions);
    }

    // 末尾追加 8 B：本帧 collision/SDF collide 记录的穿透统计。统计 buffer
    // 是 host-coherent，但 trace 读的是这里的 GPU→host 拷贝，避免槽复用覆盖。
    const vk::BufferCopy statsCopy{0, blockBytes * 3u, 2u * sizeof(float)};
    commandBuffer.copyBuffer(
        trace.object->GetTressfxPenetrationStatsInfo(frameSlot).buffer,
        slot.buffer, statsCopy);

    // rest length 只拷一次，与首帧的 current/previous/prevPrev 共用同一组 barrier。
    if (!trace.restRecorded)
    {
        const vk::DeviceSize restGuideBytes =
            static_cast<vk::DeviceSize>(trace.pointsPerGuide) * sizeof(float);
        regions.clear();
        for (uint32_t index = 0; index < trace.selectedCount; ++index)
        {
            const vk::DeviceSize sourceOffset =
                static_cast<vk::DeviceSize>(trace.selectedGuides[index]) * restGuideBytes;
            const vk::DeviceSize destinationOffset =
                static_cast<vk::DeviceSize>(index) * restGuideBytes;
            regions.push_back({sourceOffset, destinationOffset, restGuideBytes});
        }
        commandBuffer.copyBuffer(trace.object->GetTressfxRestLengthInfo().buffer,
            trace.restBuffer, regions);
        trace.restRecorded = true;
    }

    // transfer 的读（源位置）和写（readback buffer）必须在下一次 compute 写入
    // 位置 buffer 之前完成；dst 同时覆盖下一帧的 compute 读写和本帧稍后的 VS 读。
    vk::MemoryBarrier toCompute;
    toCompute.setSrcAccessMask(
            vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader, {}, toCompute, nullptr, nullptr);

    slot.pending = true;
    slot.info = info;
    if (!trace.hasStartFrame)
    {
        trace.startFrame = info.frameIndex;
        trace.hasStartFrame = true;
    }
    ++trace.capturedFrames;
    if (trace.capturedFrames >= trace.request.frames)
        trace.finishing = true;
}

void HairSimulationPass::TryFinishTrace()
{
    if (!trace.active || !trace.finishing)
        return;
    if (trace.capturedFrames < trace.request.frames)
        return;
    for (const TraceSlot& slot : trace.slots)
    {
        if (slot.pending)
            return;
    }
    FinishTraceInternal(false);
}

void HairSimulationPass::DestroyTraceResources()
{
    if (backend)
    {
        for (TraceSlot& slot : trace.slots)
        {
            if (slot.mapped && slot.memory)
                backend->UnmapMemory(slot.memory);
            if (slot.buffer || slot.memory)
                backend->DestroyBuffer(slot.buffer, slot.memory);
            slot = TraceSlot{};
        }
        if (trace.restMapped && trace.restMemory)
            backend->UnmapMemory(trace.restMemory);
        if (trace.restBuffer || trace.restMemory)
            backend->DestroyBuffer(trace.restBuffer, trace.restMemory);
    }
    trace.restBuffer = nullptr;
    trace.restMemory = nullptr;
    trace.restMapped = nullptr;
    trace.restBytes = 0;
}

void HairSimulationPass::FinishTraceInternal(bool waitForGpu)
{
    if (!trace.active)
        return;
    if (waitForGpu && backend)
    {
        // 仅正常退出/显式结束时使用：BeginFrame 只等当前 slot 的 fence，最后
        // 1..2 个 slot 可能仍 pending；一次 queue idle 后 map 才是安全的。
        backend->WaitIdle();
    }
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        FlushTraceSlot(slot);
    if (trace.writerOpen && trace.writer)
    {
        std::string error;
        if (!trace.writer->Close(error))
            std::cerr << "[TressFX] " << error << std::endl;
        trace.writerOpen = false;
    }
    if (trace.flushedFrames > 0 && trace.writer && !trace.writeFailed)
    {
        std::cout << "[TressFX] Hair trace complete: file=\"" << trace.request.path
                  << "\" frames=" << trace.flushedFrames
                  << " requestedFrames=" << trace.requestedFrames
                  << " selectedGuides=" << trace.selectedCount
                  << " pointsPerGuide=" << trace.pointsPerGuide
                  << " stride=" << trace.request.guideStride
                  << " bytes=" << trace.writer->BytesWritten() << std::endl;
    }
    else if (trace.writeFailed)
    {
        std::cerr << "[TressFX] Hair trace failed after " << trace.flushedFrames
                  << " frames: " << trace.request.path << std::endl;
    }
    else if (trace.object != nullptr)
    {
        std::cerr << "[TressFX] Hair trace finished with no captured frames: object=\""
                  << trace.objectName << "\"" << std::endl;
    }
    trace.writer.reset();
    DestroyTraceResources();
    trace = TraceState{};
}

} // namespace VL::Hair
