#include "gameExporter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace VL
{
namespace
{
std::string Lower(std::string value)
{
    for (auto& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

class PeImage
{
public:
    explicit PeImage(const std::filesystem::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read native executable: " + file.string());
        const auto size = std::filesystem::file_size(file);
        if (size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            throw std::runtime_error("Native runtime exceeds addressable file size");
        // Debug EXE 的符号可达数百 MiB；批量读取避免 istreambuf_iterator 的逐字节开销。
        bytes.resize(static_cast<size_t>(size));
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!input) throw std::runtime_error("Cannot finish reading native executable: " + file.string());
        if (Read(0, 2) != 0x5a4d) throw std::runtime_error("Native runtime is not a PE image: " + file.string());
        header = Read(0x3c, 4);
        if (Read(header, 4) != 0x4550 || Read(header + 4, 2) != 0x8664 || Read(header + 24, 2) != 0x20b)
            throw std::runtime_error("Game export requires a Windows x64 PE image: " + file.string());
        sections = Read(header + 6, 2);
        sectionTable = header + 24 + Read(header + 20, 2);
    }

    std::vector<std::string> Imports() const
    {
        std::vector<std::string> result;
        const auto directoryCount = Read(header + 24 + 108, 4);
        if (directoryCount > 1) ReadImports(header + 24 + 112 + 8, false, result);
        if (directoryCount > 13) ReadImports(header + 24 + 112 + 13 * 8, true, result);
        return result;
    }

private:
    uint32_t Read(size_t offset, size_t count) const
    {
        if (offset > bytes.size() || count > bytes.size() - offset) throw std::runtime_error("Truncated PE image");
        uint32_t value = 0;
        for (size_t index = 0; index < count; ++index)
            value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + index])) << (index * 8);
        return value;
    }

    size_t Offset(uint32_t rva) const
    {
        for (size_t index = 0; index < sections; ++index)
        {
            const auto entry = sectionTable + index * 40;
            const auto address = Read(entry + 12, 4);
            const auto size = Read(entry + 16, 4);
            if (rva >= address && rva - address < size)
                return Read(entry + 20, 4) + (rva - address);
        }
        throw std::runtime_error("PE import address does not map to file data");
    }

    void ReadImports(size_t directory, bool delayed, std::vector<std::string>& result) const
    {
        const auto address = Read(directory, 4);
        const auto length = Read(directory + 4, 4);
        if (!address || !length) return;
        const size_t stride = delayed ? 32 : 20;
        if (length < stride) throw std::runtime_error("Truncated PE import directory");
        const auto start = Offset(address);
        for (size_t index = 0; index < length / stride; ++index)
        {
            const auto entry = start + index * stride;
            const auto name = Read(entry + (delayed ? 4 : 12), 4);
            if (!name) return;
            if (delayed && !(Read(entry, 4) & 1))
                throw std::runtime_error("Unsupported VA-based PE delay import");
            std::string text;
            size_t position = Offset(name);
            while (Read(position, 1))
            {
                text += static_cast<char>(Read(position++, 1));
                if (text.size() > 512) throw std::runtime_error("Invalid PE import name");
            }
            if (text.find_first_of("/\\:") != std::string::npos) throw std::runtime_error("Invalid PE import path");
            result.push_back(std::move(text));
        }
    }

    std::vector<char> bytes;
    size_t header = 0;
    size_t sections = 0;
    size_t sectionTable = 0;
};

std::vector<std::filesystem::path> SearchDirectories(const std::filesystem::path& binaryDirectory)
{
    std::vector<std::filesystem::path> result{binaryDirectory};
    const char* environment = std::getenv("PATH");
    const std::string paths = environment ? environment : "";
    size_t start = 0;
    do
    {
        auto end = paths.find(';', start);
        auto part = paths.substr(start, end - start);
        if (!part.empty()) result.emplace_back(part);
        if (end == std::string::npos) break;
        start = end + 1;
    } while (start < paths.size());
    return result;
}

bool SystemImport(const std::string& name)
{
    if (name.rfind("api-ms-", 0) == 0 || name.rfind("ext-ms-", 0) == 0) return true;
    // 开发机可能把 VC 运行库装进 System32；这些不是 Windows 自带契约，仍需随包携带。
    if (name.rfind("msvcp", 0) == 0 || name.rfind("vcruntime", 0) == 0 ||
        name.rfind("concrt", 0) == 0 || name == "ucrtbased.dll") return false;
#ifdef _WIN32
    wchar_t system[32768];
    const auto size = GetSystemDirectoryW(system, 32768);
    if (size && size < 32768)
        return std::filesystem::is_regular_file(std::filesystem::path(std::wstring(system, size)) / name);
#endif
    return false;
}
} // namespace

std::vector<std::filesystem::path> CollectNativeGameDependencies(const std::filesystem::path& executable)
{
    const auto directories = SearchDirectories(executable.parent_path());
    std::vector<std::filesystem::path> result;
    std::vector<std::filesystem::path> pending{executable};
    std::set<std::string> visited{Lower(executable.filename().string())};
    for (size_t index = 0; index < pending.size(); ++index)
        for (const auto& import : PeImage(pending[index]).Imports())
        {
            const auto name = Lower(import);
            if (!visited.insert(name).second) continue;
            std::filesystem::path dependency;
            const auto local = executable.parent_path() / import;
            if (std::filesystem::is_regular_file(local)) dependency = std::filesystem::canonical(local);
            else if (SystemImport(name)) continue;
            for (const auto& directory : directories)
                if (dependency.empty() && std::filesystem::is_regular_file(directory / import))
                {
                    dependency = std::filesystem::canonical(directory / import);
                    break;
                }
            if (dependency.empty()) throw std::runtime_error("Missing native game dependency: " + import);
            result.push_back(dependency);
            pending.push_back(std::move(dependency));
        }
    return result;
}
} // namespace VL
