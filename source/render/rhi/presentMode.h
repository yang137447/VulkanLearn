#pragma once

namespace VL
{

// Presentation policy is a renderer/RHI concern, not a launch-parser detail.
// Auto preserves the existing "mailbox, else immediate, else fifo" preference.
enum class PresentModePreference
{
    Auto,
    Fifo,
    Mailbox,
    Immediate
};

} // namespace VL
