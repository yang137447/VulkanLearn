#pragma once

#include "render/diagnostics/terrainCaptureHooks.h"
#include "render/backend/frameAbandon.h"
#include <array>
#include <memory>
#include <stdexcept>
#include <utility>

namespace VL
{
// 只连接实际录制/提交/弃录制/fence；强 owner 在提交前准备，异常不推断提交成功。
class TerrainCaptureFrameLifecycle
{
public:
    static constexpr size_t Capacity = 8;
    struct Flight
    {
        std::shared_ptr<ITerrainCaptureHooks> hooks;
        std::shared_ptr<const void> frameOwner;
        std::shared_ptr<const void> graphOwner;
        std::shared_ptr<const void> submittedOwner;
        uint64_t epoch = 0;
        bool geometryOpen = false;
        bool afterGeometry = false;
        bool submitAttempted = false;
        bool accepted = false;
        bool submittedNotified = false;
        bool aborted = false;
    };

    std::shared_ptr<Flight> Prepare(size_t slot, uint64_t epoch,
        std::shared_ptr<ITerrainCaptureHooks> hooks,
        std::shared_ptr<const void> frameOwner, std::shared_ptr<const void> graphOwner)
    {
        if (slot >= Capacity || slots[slot] || !epoch || !hooks || !frameOwner || !graphOwner)
            throw std::logic_error("TES frame slot/epoch/immutable owners are incomplete or still in flight");
        auto flight = std::make_shared<Flight>();
        flight->hooks = std::move(hooks);
        flight->frameOwner = std::move(frameOwner);
        flight->graphOwner = std::move(graphOwner);
        flight->epoch = epoch;
        slots[slot] = flight;
        return flight;
    }

    void BeforeGeometry(const std::shared_ptr<Flight>& flight, PassRuntimeContext& context,
        const TerrainCaptureFrameInfo& info)
    {
        BeginGeometry(flight, info, [&] { flight->hooks->BeforeGeometry(context, info); });
    }

    template<class Record>
    void BeginGeometry(const std::shared_ptr<Flight>& flight, const TerrainCaptureFrameInfo& info, Record&& record)
    {
        if (!flight || flight->geometryOpen || flight->afterGeometry || flight->submitAttempted ||
            info.submissionEpoch != flight->epoch || info.immutableFrameOwner != flight->frameOwner ||
            info.immutableGraphOwner != flight->graphOwner)
            throw std::logic_error("TES geometry must use the prepared actual frame identity");
        flight->geometryOpen = true;
        std::forward<Record>(record)();
    }

    void AfterGeometry(const std::shared_ptr<Flight>& flight, vk::CommandBuffer command)
    {
        if (!flight || !flight->geometryOpen || flight->afterGeometry || flight->submitAttempted)
            throw std::logic_error("TES geometry copy requires its actual recorded geometry");
        flight->hooks->AfterGeometry(command);
        flight->geometryOpen = false;
        flight->afterGeometry = true;
    }

    void SubmitAttempted(const std::shared_ptr<Flight>& flight)
    {
        if (!flight || !flight->afterGeometry || flight->geometryOpen || flight->submitAttempted)
            throw std::logic_error("TES submit requires completed geometry copy");
        flight->submitAttempted = true;
    }

    void Submitted(const std::shared_ptr<Flight>& flight, AcquiredFramePhase phase, uint64_t epoch)
    {
        if (!flight || !flight->submitAttempted || phase != AcquiredFramePhase::Submitted ||
            epoch != flight->epoch || flight->aborted)
            throw std::logic_error("TES submission requires backend acceptance of the exact epoch");
        flight->accepted = true;
        if (!flight->submittedNotified)
        {
            flight->submittedNotified = true;
            flight->submittedOwner = flight->hooks->Submitted(epoch);
        }
    }

    bool AbortProvenUnsubmitted(const std::shared_ptr<Flight>& flight, AcquiredFramePhase phase)
    {
        if (!flight || flight->aborted)
            return false;
        if (flight->submitAttempted || flight->accepted ||
            (phase != AcquiredFramePhase::Discarded && phase != AcquiredFramePhase::Recovered &&
             phase != AcquiredFramePhase::RecoverySubmitted))
            return false;
        flight->hooks->AbortUnsubmitted();
        flight->aborted = true;
        Release(flight);
        return true;
    }

    void Complete(uint64_t completedEpoch)
    {
        for (auto& flight : slots)
            if (flight && flight->accepted && flight->epoch <= completedEpoch)
            {
                flight->hooks->ConsumeCompleted(completedEpoch);
                flight.reset();
            }
    }

    bool Pending(size_t slot) const { return slot < Capacity && bool(slots[slot]); }

    void CloseAfterDeviceIdle()
    {
        std::array<ITerrainCaptureHooks*, Capacity> closed{};
        size_t count = 0;
        for (auto& flight : slots)
            if (flight)
            {
                const auto pointer = flight->hooks.get();
                bool seen = false;
                for (size_t index = 0; index < count; ++index)
                    seen = seen || closed[index] == pointer;
                if (!seen)
                {
                    flight->hooks->CloseAfterDeviceIdle();
                    closed[count++] = pointer;
                }
                flight->submittedOwner.reset();
                flight->frameOwner.reset();
                flight->graphOwner.reset();
                flight.reset();
            }
    }

    void ReleaseAfterDeviceIdle()
    {
        for (auto& flight : slots)
            if (flight)
            {
                flight->submittedOwner.reset();
                flight->frameOwner.reset();
                flight->graphOwner.reset();
                flight.reset();
            }
    }

private:
    void Release(const std::shared_ptr<Flight>& completed)
    {
        for (auto& flight : slots)
            if (flight == completed)
                flight.reset();
    }
    std::array<std::shared_ptr<Flight>, Capacity> slots;
};
}
