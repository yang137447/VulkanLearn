#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace VL::Hair
{
// Console `hair reset [objectName]` 的参数语义。空参数表示复位所有 GPU guide 对象；
// 名字是渲染对象的 GetName()（场景对象名，不含 "mesh|" 身份前缀），大小写敏感。
// 只接受一个 token：多余参数按错误拒绝，避免拼错的第二项被静默忽略。
// 这里只做值校验，不接触 World、渲染线程或 GPU；命令由 CommandBus 投递。
struct HairResetRequest
{
    std::string objectName;
};

inline bool IsHairCommandSpace(char character)
{
    return character == ' ' || character == '\t' || character == '\r' ||
        character == '\n' || character == '\v' || character == '\f';
}

// 返回错误消息；成功返回 nullptr。arguments 是 `hair reset` 之后的原始文本。
inline const char* ParseHairResetArguments(std::string_view arguments,
    HairResetRequest& request)
{
    request.objectName.clear();
    size_t begin = 0;
    size_t end = arguments.size();
    while (begin < end && IsHairCommandSpace(arguments[begin])) ++begin;
    while (end > begin && IsHairCommandSpace(arguments[end - 1])) --end;
    if (begin == end) return nullptr;
    size_t tokenEnd = begin;
    while (tokenEnd < end && !IsHairCommandSpace(arguments[tokenEnd])) ++tokenEnd;
    size_t next = tokenEnd;
    while (next < end && IsHairCommandSpace(arguments[next])) ++next;
    if (next != end) return "hair reset accepts at most one object name";
    // 与编辑器/控制台的短身份一致：不做通配符或路径解析，过长的名字直接拒绝。
    // 先校验长度再发布结果，失败不留下半解析的 objectName。
    const size_t length = tokenEnd - begin;
    if (length > 128) return "hair reset object name is too long (max 128 bytes)";
    request.objectName.assign(arguments.substr(begin, length));
    return nullptr;
}
} // namespace VL::Hair
