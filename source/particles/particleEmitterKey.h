#pragma once

#include <string>
#include <string_view>

namespace VL
{

// 发射器 GPU 池以场景对象 + 资产路径 + 发射器名作为身份。直接拼接会
// 让名字里的分隔符与另一组字段产生同一 key；这里转义 `\` 与 `|`，
// 段之间仍用未转义的 `|` 分隔，编码因此可逆且无碰撞。
inline std::string EscapeParticleEmitterKeySegment(
    std::string_view segment)
{
    std::string escaped;
    escaped.reserve(segment.size());
    for (char character : segment)
    {
        if (character == '\\' || character == '|')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

inline std::string BuildParticleEmitterKey(
    std::string_view sceneName,
    std::string_view assetPath,
    std::string_view emitterName)
{
    return EscapeParticleEmitterKeySegment(sceneName) + "|" +
        EscapeParticleEmitterKeySegment(assetPath) + "|" +
        EscapeParticleEmitterKeySegment(emitterName);
}

// 场景对象名是 key 的第一段；先按同一套规则转义再比较前缀，避免
// `A|B` 与 `A` 这类名字在运行时 restart 时互相误配。
inline bool ParticleEmitterKeyMatchesScene(
    std::string_view key,
    std::string_view sceneName)
{
    const std::string prefix =
        EscapeParticleEmitterKeySegment(sceneName) + "|";
    return key.size() > prefix.size() &&
        key.compare(0, prefix.size(), prefix) == 0;
}

} // namespace VL
