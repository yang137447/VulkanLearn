#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <utility>

namespace VL
{
// 从同一冻结 Frame context 取得 CPU/GPU 共用身份；不以读回序号或 slot 代替帧号。
struct RenderFrameIdentity
{
    uint64_t renderFrame = 0;
    uint64_t worldGeneration = 0;
    uint64_t submitEpoch = 0;

    static RenderFrameIdentity FromFrame(uint32_t frame, uint64_t world)
    {
        return {frame, world, static_cast<uint64_t>(frame) + 1};
    }

    template<class Context>
    static RenderFrameIdentity FromContext(const Context& context)
    {
        return FromFrame(context.currentFrame, context.renderScene.worldGeneration);
    }
};

inline void WriteRawFrameIdentity(std::ostream& output, const RenderFrameIdentity& identity)
{
    output << "\"identityVersion\":2,\"frame\":" << identity.renderFrame
        << ",\"renderFrame\":" << identity.renderFrame
        << ",\"worldGeneration\":" << identity.worldGeneration
        << ",\"submitEpoch\":" << identity.submitEpoch;
}

// slot 的旧批身份与新录制身份分离；只有真实 submit 后才进入可读回批次。
class GpuQueryFrameIdentity
{
public:
    void Begin(const RenderFrameIdentity& identity)
    {
        recording = identity;
        prepared.reset();
    }
    void End(bool hasQueries)
    {
        prepared = hasQueries ? recording : std::nullopt;
        recording.reset();
    }
    bool Submitted()
    {
        if (!prepared)
            return false;
        pending = std::exchange(prepared, std::nullopt);
        return true;
    }
    void Abort()
    {
        recording.reset();
        prepared.reset();
    }
    const std::optional<RenderFrameIdentity>& Pending() const { return pending; }
    const RenderFrameIdentity* Completed(uint64_t completedEpoch) const
    {
        return pending && completedEpoch >= pending->submitEpoch ? &*pending : nullptr;
    }
    void Consumed() { pending.reset(); }

private:
    std::optional<RenderFrameIdentity> recording;
    std::optional<RenderFrameIdentity> prepared;
    std::optional<RenderFrameIdentity> pending;
};
}
