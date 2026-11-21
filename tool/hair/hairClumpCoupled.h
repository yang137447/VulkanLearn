#pragma once

#include <filesystem>

namespace C2
{
int Verify(const std::filesystem::path& directory);
int Startup(const std::filesystem::path& directory);
}
