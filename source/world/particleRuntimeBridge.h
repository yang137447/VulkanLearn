#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace VL
{

// EngineLoop 的 GPU 事件回读节流：只在 Play + 至少一个 C# 订阅存在时
// 以最多 10 Hz 进入设备安全点，避免普通帧每帧 WaitIdle。
struct ParticleEventPollClock
{
    static constexpr float kIntervalSeconds = 0.1f;
    float accumulator = 0.0f;

    void Reset() { accumulator = 0.0f; }

    bool Consume(float deltaSeconds, bool playing, bool interested)
    {
        if (!playing || !interested)
        {
            Reset();
            return false;
        }
        if (std::isfinite(deltaSeconds) && deltaSeconds > 0.0f)
            accumulator += deltaSeconds;
        if (accumulator < kIntervalSeconds) return false;
        // 保留余数而不是清零，低帧率下仍维持近似固定回读节拍。
        accumulator = std::fmod(accumulator, kIntervalSeconds);
        return true;
    }
};

struct ParticleEmitterKeyIdentity
{
    std::string sceneName;
    std::string emitterName;
};

// 与 BuildParticleEmitterKey 的转义规则配对：反斜杠转义 `\` 和 `|`，
// 未转义的 `|` 才是字段分隔符。只接受恰好三段（scene/asset/emitter）。
inline bool DecodeParticleEmitterKey(
    std::string_view key, ParticleEmitterKeyIdentity& outIdentity)
{
    std::vector<std::string> segments;
    std::string current;
    bool escaped = false;
    for (const char character : key)
    {
        if (escaped)
        {
            current.push_back(character);
            escaped = false;
        }
        else if (character == '\\')
        {
            escaped = true;
        }
        else if (character == '|')
        {
            segments.push_back(std::move(current));
            current.clear();
        }
        else
        {
            current.push_back(character);
        }
    }
    if (escaped) return false;
    segments.push_back(std::move(current));
    if (segments.size() != 3 || segments[0].empty() ||
        segments[2].empty())
        return false;
    outIdentity.sceneName = std::move(segments[0]);
    outIdentity.emitterName = std::move(segments[2]);
    return true;
}

// ParticleEventType: 0 spawn, 1 death, 2 collision。
inline const char* ParticleEventKindName(uint32_t eventType)
{
    if (eventType == 0) return "spawn";
    if (eventType == 1) return "death";
    if (eventType == 2) return "collision";
    return nullptr;
}

// 事件回读是诊断/玩法桥，不追求把整个 bounded GPU 队列全部搬空；
// 每 emitter 最多读取 256 条，其余由 counters.events 表示已截断。
constexpr uint32_t kParticleEventDrainPerEmitter = 256;

} // namespace VL
