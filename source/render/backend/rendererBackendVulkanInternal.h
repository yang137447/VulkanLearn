#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>

namespace VL
{
namespace
{
template <typename HandleType>
uint64_t ToRawHandleValue(HandleType handle)
{
    if constexpr (std::is_pointer_v<HandleType>)
    {
        return reinterpret_cast<uint64_t>(handle);
    }
    else
    {
        return static_cast<uint64_t>(handle);
    }
}

template <typename HandleType>
std::string FormatRawHandle(HandleType handle)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << ToRawHandleValue(handle);
    return stream.str();
}

template <typename HandleType>
std::string BuildUnknownRawHandleMessage(const char* resourceType, HandleType handle)
{
    return std::string("RendererBackendVulkan received an unknown ") +
        resourceType +
        " raw handle " +
        FormatRawHandle(handle) +
        ". The resource should be created or registered through RendererBackendVulkan before use.";
}
} // namespace
} // namespace VL