#include "ueProjectInventory.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "shader/build/contentHash.h"

namespace VL::AnimationPreflight
{
namespace
{
bool IsWithin(const std::filesystem::path& path, const std::filesystem::path& root)
{
    // lexical 比较在 Windows 上区分大小写，不能用于保护真实目录；按祖先的文件身份判断。
    auto ancestor = std::filesystem::weakly_canonical(path);
    while (!ancestor.empty())
    {
        if (std::filesystem::exists(ancestor) && std::filesystem::equivalent(ancestor, root))
        {
            return true;
        }
        const auto parent = ancestor.parent_path();
        if (parent == ancestor)
        {
            break;
        }
        ancestor = parent;
    }
    return false;
}

void CollectInputs(
    const std::filesystem::path& root,
    const std::filesystem::path& directory,
    std::vector<std::filesystem::path>& files)
{
    if (!std::filesystem::exists(directory))
    {
        return;
    }
    // 禁止沿目录链接读取项目外的资源并把它们错误标为项目自有输入。
    if (!IsWithin(std::filesystem::canonical(directory), root))
    {
        throw std::runtime_error("Inventory directory resolves outside project");
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
    {
        const auto resolved = std::filesystem::canonical(entry.path());
        if (!IsWithin(resolved, root))
        {
            throw std::runtime_error("Inventory input resolves outside project: " + entry.path().u8string());
        }
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto extension = entry.path().extension().u8string();
        if (extension == ".uasset" || extension == ".umap" ||
            extension == ".uplugin" || extension == ".ini")
        {
            files.push_back(entry.path());
        }
    }
}

std::string NameHint(const std::filesystem::path& path)
{
    const auto stem = path.stem().u8string();
    if (stem.rfind("PSD_", 0) == 0) return "pose-search-database-name";
    if (stem.rfind("PSS_", 0) == 0) return "pose-search-schema-name";
    if (stem.rfind("CHT_", 0) == 0 || stem.rfind("CHO_", 0) == 0) return "chooser-name";
    if (stem.rfind("ABP_", 0) == 0) return "animation-blueprint-name";
    return "";
}
}

void ValidateUnrealInventoryOutput(
    const std::filesystem::path& project,
    const std::filesystem::path& output)
{
    const auto root = std::filesystem::canonical(project).parent_path();
    if (IsWithin(output, root))
    {
        throw std::invalid_argument("Inventory output must stay outside the source UE project");
    }
}

nlohmann::json InventoryUnrealProject(const std::filesystem::path& project)
{
    if (project.extension() != ".uproject")
    {
        throw std::invalid_argument("Expected a .uproject file");
    }
    const auto resolved = std::filesystem::canonical(project);
    const auto root = resolved.parent_path();
    std::ifstream input(resolved, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Cannot read UE project");
    }
    const auto descriptor = nlohmann::json::parse(input);
    if (!descriptor.is_object())
    {
        throw std::invalid_argument("UE project descriptor must be an object");
    }
    const auto descriptorHash = ContentHasher::HashFile(resolved);
    std::vector<std::filesystem::path> files;
    CollectInputs(root, root / "Content", files);
    CollectInputs(root, root / "Config", files);
    CollectInputs(root, root / "Plugins", files);
    std::sort(files.begin(), files.end());
    CanonicalFieldHasher identity("vl-ue-project-inventory-v1");
    identity.AddDigest("uproject", descriptorHash);
    nlohmann::json records = nlohmann::json::array();
    for (const auto& file : files)
    {
        const auto relative = file.lexically_relative(root).generic_u8string();
        const auto digest = ContentHasher::HashFile(file);
        identity.AddString("path", relative);
        identity.AddDigest("bytes", digest);
        records.push_back({{"path", relative}, {"size", std::filesystem::file_size(file)},
            {"blake3", digest.ToHex()}, {"nameHint", NameHint(file)}});
    }
    return {
        {"format", "vl-ue-project-inventory-v1"},
        {"project", resolved.u8string()},
        {"declaredEngineAssociation", descriptor.value("EngineAssociation", nlohmann::json())},
        {"descriptorBlake3", descriptorHash.ToHex()}, {"inputBlake3", identity.Finalize().ToHex()},
        {"descriptor", descriptor}, {"inputFileCount", records.size()}, {"files", std::move(records)},
        {"limitations", nlohmann::json::array({
            "Read-only filesystem inventory, not an Unreal Asset Registry audit",
            "Name hints are not confirmed asset classes or proof that this is GASP",
            "No source control revision, licensing, bone/curve/event/schema or behavior verification",
            "No export or VL coordinate conversion; no GASP acceptance implied",
            "Close the editor and other writers before capturing a reference baseline",
            "Scope: .uproject plus .uasset/.umap/.uplugin/.ini in Content, Config, Plugins"
        })}
    };
}
}
