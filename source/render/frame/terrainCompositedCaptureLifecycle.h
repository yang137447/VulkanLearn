#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>

namespace VL
{
class TerrainCompositedCaptureLifecycle
{
public:
    struct Identity
    {
        uint64_t handle = 0;
        uint64_t world = 0;
        uint64_t pose = 0;
        uint64_t frame = 0;
        uint64_t drawEpoch = 0;
        uint64_t submissionEpoch = 0;
    };

    void Recorded(uint64_t handle, uint64_t world, uint64_t pose,
        uint64_t frame, uint64_t drawEpoch)
    {
        if (pending || !handle || !world || !pose || !drawEpoch)
            throw std::runtime_error("Quality capture requires one actual recorded identity");
        pending = Identity{handle, world, pose, frame, drawEpoch, 0};
    }

    template<class Emit>
    void Submitted(uint64_t handle, uint64_t epoch, Emit&& emit)
    {
        if (!pending || pending->handle != handle || pending->submissionEpoch ||
            epoch != pending->drawEpoch)
            throw std::runtime_error("Quality capture submission identity mismatch");
        pending->submissionEpoch = epoch;
        std::forward<Emit>(emit)(Event("capture_submitted", false));
    }

    template<class Emit>
    void Completed(uint64_t handle, uint64_t world, uint64_t pose, uint64_t frame,
        uint64_t epoch, uint64_t completedEpoch, Emit&& emit)
    {
        if (!pending || !pending->submissionEpoch || pending->handle != handle ||
            pending->world != world || pending->pose != pose || pending->frame != frame ||
            pending->submissionEpoch != epoch || completedEpoch < epoch)
            throw std::runtime_error("Quality capture completion requires its actual fenced readback");
        auto event = Event("capture_completed", true);
        event["completedEpoch"] = completedEpoch;
        std::forward<Emit>(emit)(std::move(event));
        pending.reset();
    }

    template<class Emit>
    bool Close(std::string_view reason, Emit&& emit)
    {
        if (!pending)
            return false;
        if (reason.empty())
            throw std::invalid_argument("Quality capture closure requires an actual reason");
        auto event = Event("capture_aborted", false);
        event["reason"] = std::string(reason);
        std::forward<Emit>(emit)(std::move(event));
        pending.reset();
        return true;
    }

    const std::optional<Identity>& Pending() const noexcept { return pending; }

private:
    nlohmann::json Event(const char* name, bool measurementProvided) const
    {
        return {{"event", name}, {"worldGeneration", pending->world}, {"poseId", pending->pose},
            {"frame", pending->frame}, {"drawSubmissionEpoch", pending->drawEpoch},
            {"submissionEpoch", pending->submissionEpoch}, {"captureHandle", pending->handle},
            {"submitted", pending->submissionEpoch != 0}, {"measurementProvided", measurementProvided}};
    }

    std::optional<Identity> pending;
};
}
