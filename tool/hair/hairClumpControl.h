#pragma once

#include <filesystem>

namespace C0
{
int Startup(const std::filesystem::path& directory);
int Control(const std::filesystem::path& directory);
}
