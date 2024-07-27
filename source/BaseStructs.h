#pragma once

#include <optional>

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsQueue;
    std::optional<uint32_t> presentQueue;
};