#pragma once

#include <cstdint>

namespace VL
{

enum class PlatformEventType
{
    Unknown,
    KeyDown,
    WindowResized,
    Quit
};

enum class PlatformKey
{
    Unknown,
    Escape
};

struct PlatformEvent
{
    PlatformEventType type = PlatformEventType::Unknown;
    PlatformKey key = PlatformKey::Unknown;
    bool repeat = false;
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace VL
