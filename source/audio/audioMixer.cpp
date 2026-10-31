#include "audio/audioMixer.h"

#include <algorithm>
#include <cmath>

namespace VL::Audio
{
namespace
{

constexpr float Pi = 3.14159265358979323846f;
constexpr float DistanceReferenceCm = 200.0f;
constexpr float MinDistanceGain = 0.04f;
constexpr float LowpassNearHz = 20000.0f;
constexpr float LowpassFarHz = 2000.0f;
// 3000 cm（30 m）之后不再继续压暗，避免远处声音完全变成低频嗡声。
constexpr float LowpassFarDistanceCm = 3000.0f;
constexpr float DirectionEpsilon = 1.0e-4f;
constexpr float DistanceTieEpsilonCm = 1.0f;

bool IsMoreImportant(
    VoicePriority aPriority,
    float aDistance,
    std::size_t aRemaining,
    VoicePriority bPriority,
    float bDistance,
    std::size_t bRemaining)
{
    if (aPriority != bPriority)
        return static_cast<int>(aPriority) > static_cast<int>(bPriority);
    if (std::abs(aDistance - bDistance) > DistanceTieEpsilonCm)
        return aDistance < bDistance;
    return aRemaining > bRemaining;
}

Eigen::Vector3f NormalizedOr(
    const Eigen::Vector3f& value, const Eigen::Vector3f& fallback)
{
    const float length = value.norm();
    if (length < DirectionEpsilon) return fallback;
    return value / length;
}

} // namespace

float LowpassAlphaForCutoff(float cutoffHz, int sampleRate)
{
    const float maxCutoff = static_cast<float>(sampleRate) * 0.45f;
    const float clamped = std::clamp(cutoffHz, 20.0f, maxCutoff);
    const float alpha = 1.0f -
        std::exp(-2.0f * Pi * clamped / static_cast<float>(sampleRate));
    return std::clamp(alpha, 0.0f, 1.0f);
}

SpatialParams ComputeSpatialParams(
    const ListenerState& listener, const Eigen::Vector3f& sourcePosition)
{
    SpatialParams params;
    const Eigen::Vector3f relative = sourcePosition - listener.position;
    params.distance = relative.norm();

    // 右向量取 up × forward：默认 forward=+Z、up=+Y 时得到 +X，
    // 与场景/相机使用的坐标一致。forward 与 up 共线时退回 +X。
    const Eigen::Vector3f forward = NormalizedOr(
        listener.forward, Eigen::Vector3f(0.0f, 0.0f, 1.0f));
    const Eigen::Vector3f up = NormalizedOr(
        listener.up, Eigen::Vector3f(0.0f, 1.0f, 0.0f));
    const Eigen::Vector3f right = NormalizedOr(
        up.cross(forward), Eigen::Vector3f(1.0f, 0.0f, 0.0f));

    params.pan = params.distance > DirectionEpsilon
        ? std::clamp(relative.dot(right) / params.distance, -1.0f, 1.0f)
        : 0.0f;

    // 等功率 pan：θ 从 -π/4 到 +π/4，左右增益平方和恒为 1。
    const float angle = (params.pan + 1.0f) * (Pi * 0.25f);
    params.leftGain = std::cos(angle);
    params.rightGain = std::sin(angle);

    params.gain = std::clamp(
        1.0f / (1.0f + params.distance / DistanceReferenceCm),
        MinDistanceGain, 1.0f);

    const float distanceT = std::clamp(
        params.distance / LowpassFarDistanceCm, 0.0f, 1.0f);
    params.lowpassCutoffHz = LowpassNearHz *
        std::pow(LowpassFarHz / LowpassNearHz, distanceT);
    params.lowpassAlpha = LowpassAlphaForCutoff(params.lowpassCutoffHz);
    return params;
}

VoiceMixer::VoiceMixer(int capacity)
    : voices(static_cast<std::size_t>(std::max(capacity, 0)))
{
}

void VoiceMixer::SetListener(const ListenerState& value)
{
    listener = value;
}

float VoiceMixer::DistanceToListener(const Eigen::Vector3f& position) const
{
    return (position - listener.position).norm();
}

VoiceSubmitResult VoiceMixer::Submit(
    VoicePriority priority,
    const Eigen::Vector3f& position,
    std::vector<float> samples)
{
    if (voices.empty() || samples.empty()) return VoiceSubmitResult{};

    const float distance = DistanceToListener(position);
    int slot = -1;
    int stolenSlot = -1;
    for (std::size_t index = 0; index < voices.size(); ++index)
    {
        if (!voices[index].active)
        {
            slot = static_cast<int>(index);
            break;
        }
    }

    if (slot < 0)
    {
        slot = SelectStealCandidate(priority, distance, samples.size());
        if (slot < 0) return VoiceSubmitResult{};
        stolenSlot = slot;
    }

    Voice& voice = voices[static_cast<std::size_t>(slot)];
    voice.active = true;
    voice.priority = priority;
    voice.position = position;
    voice.samples = std::move(samples);
    voice.cursor = 0;
    voice.filterState = 0.0f;
    return VoiceSubmitResult{true, slot, stolenSlot};
}

int VoiceMixer::SelectStealCandidate(
    VoicePriority incomingPriority,
    float incomingDistance,
    std::size_t incomingRemaining) const
{
    int leastImportant = -1;
    for (std::size_t index = 0; index < voices.size(); ++index)
    {
        const Voice& voice = voices[index];
        if (!voice.active) continue;
        if (leastImportant < 0)
        {
            leastImportant = static_cast<int>(index);
            continue;
        }
        const Voice& current = voices[static_cast<std::size_t>(leastImportant)];
        // 只有当前候选“更重要”时才换掉它；完全同级时保留更早创建的槽位，
        // 让抢占结果确定且优先淘汰先到的同权重声音。
        const bool candidateMoreImportant = IsMoreImportant(
            current.priority, DistanceToListener(current.position),
            current.samples.size() - current.cursor,
            voice.priority, DistanceToListener(voice.position),
            voice.samples.size() - voice.cursor);
        if (candidateMoreImportant) leastImportant = static_cast<int>(index);
    }
    if (leastImportant < 0) return -1;

    const Voice& candidate = voices[static_cast<std::size_t>(leastImportant)];
    const bool incomingMoreImportant = IsMoreImportant(
        incomingPriority, incomingDistance, incomingRemaining,
        candidate.priority, DistanceToListener(candidate.position),
        candidate.samples.size() - candidate.cursor);
    return incomingMoreImportant ? leastImportant : -1;
}

void VoiceMixer::MixBlock(float* interleavedStereo, int frames)
{
    if (frames <= 0) return;
    for (int frame = 0; frame < frames; ++frame)
    {
        interleavedStereo[frame * 2] = 0.0f;
        interleavedStereo[frame * 2 + 1] = 0.0f;
    }

    for (Voice& voice : voices)
    {
        if (!voice.active) continue;

        // 空间参数按 block 更新：listener 在一个 block 内视为静止，
        // 这样每 128 帧只做一次 sqrt/三角函数。
        const SpatialParams spatial = ComputeSpatialParams(listener, voice.position);
        const std::size_t count = std::min<std::size_t>(
            static_cast<std::size_t>(frames),
            voice.samples.size() - voice.cursor);
        for (std::size_t frame = 0; frame < count; ++frame)
        {
            float source = voice.samples[voice.cursor + frame] * spatial.gain;
            if (!std::isfinite(source)) source = 0.0f;
            voice.filterState += spatial.lowpassAlpha * (source - voice.filterState);
            interleavedStereo[frame * 2] += voice.filterState * spatial.leftGain;
            interleavedStereo[frame * 2 + 1] += voice.filterState * spatial.rightGain;
        }
        voice.cursor += count;

        if (voice.cursor >= voice.samples.size())
        {
            voice.active = false;
            voice.cursor = 0;
            voice.samples.clear();
            voice.filterState = 0.0f;
        }
    }

    // 32 个 voice 叠加可能超过 [-1,1]；这里做硬限幅，并拦掉任何
    // 非有限值，避免 NaN 进入低通状态后永久污染该槽位。
    for (int frame = 0; frame < frames; ++frame)
    {
        for (int channel = 0; channel < MixChannels; ++channel)
        {
            float& sample = interleavedStereo[frame * 2 + channel];
            if (!std::isfinite(sample)) sample = 0.0f;
            sample = std::clamp(sample, -1.0f, 1.0f);
        }
    }
}

void VoiceMixer::Reset()
{
    for (Voice& voice : voices)
    {
        voice = Voice{};
    }
}

int VoiceMixer::ActiveVoiceCount() const
{
    int count = 0;
    for (const Voice& voice : voices)
    {
        if (voice.active) ++count;
    }
    return count;
}

VoiceStatus VoiceMixer::GetVoiceStatus(int slot) const
{
    VoiceStatus status;
    if (slot < 0 || slot >= static_cast<int>(voices.size())) return status;
    const Voice& voice = voices[static_cast<std::size_t>(slot)];
    status.active = voice.active;
    status.priority = voice.priority;
    status.position = voice.position;
    status.remainingFrames = voice.active
        ? voice.samples.size() - voice.cursor
        : 0;
    return status;
}

} // namespace VL::Audio
