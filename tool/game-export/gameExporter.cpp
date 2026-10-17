#include "gameExporter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include "shader/build/contentHash.h"

namespace VL
{
namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

std::string Lower(std::string value)
{
    for (auto& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

bool IsInside(const fs::path& root, const fs::path& path)
{
    auto candidate = path.begin();
    for (const auto& part : root)
    {
        if (candidate == path.end()) return false;
#ifdef _WIN32
        if (Lower(candidate->string()) != Lower(part.string())) return false;
#else
        if (*candidate != part) return false;
#endif
        ++candidate;
    }
    return true;
}

fs::path ResolveContent(const fs::path& root, std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    const fs::path relative(name);
    if (name.empty() || name.find('\0') != std::string::npos || relative.is_absolute() ||
        relative.has_root_name() || relative.has_root_directory())
        throw std::invalid_argument("Export asset path must be relative: " + name);
    for (const auto& part : relative)
        if (part == "..") throw std::invalid_argument("Export asset path contains traversal: " + name);
    const auto absolute = fs::weakly_canonical(root / relative);
    if (!IsInside(root, absolute) || !fs::is_regular_file(absolute))
        throw std::invalid_argument("Missing or escaped export dependency: " + name);
    // 安全检查用物理路径，复制位置仍保留作者逻辑路径，内部符号链接别名也能搬移。
    return (root / relative).lexically_normal();
}

Json ReadJson(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read export JSON: " + path.string());
    auto value = Json::parse(input);
    if (input.bad()) throw std::runtime_error("Cannot finish reading export JSON: " + path.string());
    return value;
}

void WriteJson(const fs::path& path, const Json& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value.dump(2) << '\n';
    output.close();
    if (!output) throw std::runtime_error("Cannot write export JSON: " + path.string());
}

// 所有写入先落到独占 staging 目录，依赖校验和清单完成后一次 rename 发布。
class ExportFiles
{
public:
    ExportFiles(fs::path project, fs::path resource, fs::path stage)
        : projectRoot(std::move(project)), resourceRoot(std::move(resource)), staging(std::move(stage)) {}

    void Copy(const fs::path& source, const fs::path& destination)
    {
        if (!fs::is_regular_file(source)) throw std::runtime_error("Missing export file: " + source.string());
        const auto physical = fs::canonical(source);
        const auto key = Lower(destination.generic_string());
        const auto existing = destinations.find(key);
        if (existing != destinations.end())
        {
            if (existing->second != physical) throw std::runtime_error("Export destination conflict: " + destination.string());
            return;
        }
        destinations.emplace(key, physical);
        fs::create_directories((staging / destination).parent_path());
        fs::copy_file(physical, staging / destination);
    }

    void Tree(const fs::path& source, const fs::path& destination)
    {
        if (!fs::is_directory(source)) throw std::runtime_error("Missing export directory: " + source.string());
        const auto root = fs::canonical(source);
        if (IsInside(root, staging))
            throw std::invalid_argument("Export output is nested inside packaged directory: " + root.string());
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            // 目录链接不能绕过内容域边界，尤其不能把输出 staging 反向纳入自己。
            if (fs::is_symlink(entry.symlink_status()))
                throw std::runtime_error("Export directory contains a symbolic link: " + entry.path().string());
            if (entry.is_regular_file()) Copy(entry.path(), destination / entry.path().lexically_relative(root));
        }
    }

    void Asset(const std::string& name, bool modelSource = false)
    {
        const auto source = ResolveContent(resourceRoot, name);
        const auto relative = source.lexically_relative(resourceRoot);
        Copy(source, fs::path("resources") / relative);
        if (modelSource)
        {
            // glTF/OBJ/SpeedTree 的外部 buffer/贴图可以藏在二进制或 MTL 中；保留源目录整包。
            Tree(source.parent_path(), fs::path("resources") / relative.parent_path());
        }
        if (source.extension() == ".json" && parsedAssets.insert(source).second)
            Scan(ReadJson(staging / "resources" / relative));
    }

    void Scan(const Json& value, const std::string& field = {})
    {
        if (value.is_object())
        {
            if (value.contains("type") && value.at("type").is_string() &&
                (value.at("type") == "blueprint" || value.at("type") == "blueprintInstance"))
                requiresScripts = true;
            for (const auto& item : value.items()) Scan(item.value(), item.key());
            return;
        }
        if (value.is_array())
        {
            for (const auto& item : value) Scan(item, field);
            return;
        }
        if (!value.is_string()) return;
        auto name = value.get<std::string>();
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty()) return;
        const bool assetField = field == "modelPath" || field == "modelDataPath" || field == "materialOverride" ||
            field == "materialInstancePath" || field == "blueprintPath" || field == "spawnAssets" ||
            field == "hdrPath" || field == "source";
        if (name.rfind("shader/", 0) == 0)
        {
            ResolveContent(projectRoot, name);
            return;
        }
        const bool resourcePrefix = name.rfind("Common/", 0) == 0 || name.rfind("Maps/", 0) == 0 ||
            name.rfind("Generated/", 0) == 0;
        if (assetField || resourcePrefix)
            Asset(name, field == "modelDataPath");
    }

    bool RequiresScripts() const { return requiresScripts; }

private:
    fs::path projectRoot;
    fs::path resourceRoot;
    fs::path staging;
    std::map<std::string, fs::path> destinations;
    std::set<fs::path> parsedAssets;
    bool requiresScripts = false;
};

fs::path SelectVersion(const fs::path& directory, const std::string& prefix)
{
    std::vector<fs::path> candidates;
    if (fs::is_directory(directory))
        for (const auto& entry : fs::directory_iterator(directory))
            if (entry.is_directory() && entry.path().filename().string().rfind(prefix, 0) == 0)
                candidates.push_back(entry.path());
    if (candidates.empty()) throw std::runtime_error("Missing private .NET runtime under " + directory.string());
    // 版本的每一段按数字比较，避免 8.0.9 胜过 8.0.20。
    std::map<std::vector<unsigned long>, fs::path> versions;
    for (const auto& path : candidates)
    {
        std::vector<unsigned long> version;
        auto text = path.filename().string();
        size_t start = 0;
        for (;;)
        {
            size_t end = text.find('.', start);
            auto number = text.substr(start, end - start);
            if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
            {
                version.clear();
                break;
            }
            version.push_back(std::stoul(number));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (!version.empty()) versions.emplace(std::move(version), path);
    }
    if (versions.empty()) throw std::runtime_error("No stable .NET version under " + directory.string());
    return versions.rbegin()->second;
}

void ManagedRuntime(ExportFiles& files, Json& config, const GameExportOptions& options,
    const fs::path& project, const fs::path& binaries)
{
    auto settings = config.value("scripts", Json::object());
    const auto managed = settings.contains("runtimeDirectory") ?
        fs::weakly_canonical(project / settings.at("runtimeDirectory").get<std::string>()) : binaries / "managed";
    const bool enabled = settings.value("enabled", options.csharpEnabled);
    if (!enabled)
    {
        if (files.RequiresScripts()) throw std::invalid_argument("Exported Blueprint assets require enabled C# scripts");
        config["scripts"] = {{"enabled", false}};
        return;
    }
    if (!options.csharpEnabled) throw std::invalid_argument("Cannot enable scripts in a native build without C# support");
    const auto assembly = settings.contains("assembly") ?
        fs::weakly_canonical(project / settings.at("assembly").get<std::string>()) : managed / "Gameplay.dll";
    if (assembly.parent_path() != fs::weakly_canonical(managed))
        throw std::runtime_error("Gameplay assembly must be inside scripts.runtimeDirectory for export");
    for (const auto& entry : fs::directory_iterator(managed))
        if (entry.is_regular_file() && (entry.path().extension() == ".dll" || entry.path().extension() == ".json"))
            files.Copy(entry.path(), fs::path("managed") / entry.path().filename());
    for (const auto& required : {"hostfxr.dll", "VulkanLearn.Runtime.dll", "VulkanLearn.Runtime.runtimeconfig.json"})
        files.Copy(managed / required, fs::path("managed") / required);
    files.Copy(assembly, fs::path("managed") / assembly.filename());
    auto dotnet = fs::is_directory(managed / "dotnet") ? managed / "dotnet" : options.dotnetRoot;
    const auto runtime = SelectVersion(dotnet / "shared" / "Microsoft.NETCore.App", "8.");
    const auto fxr = SelectVersion(dotnet / "host" / "fxr", "8.");
    files.Tree(runtime, fs::path("managed/dotnet/shared/Microsoft.NETCore.App") / runtime.filename());
    files.Tree(fxr, fs::path("managed/dotnet/host/fxr") / fxr.filename());
    config["scripts"] = {{"enabled", true}, {"autoPlay", true}, {"runtimeDirectory", "managed"},
        {"assembly", (fs::path("managed") / assembly.filename()).generic_string()}};
}
} // namespace

GameExportResult ExportGame(const GameExportOptions& options)
{
    if (options.projectRoot.empty() || options.binaryDirectory.empty() || options.outputDirectory.empty() ||
        options.initialScene.empty()) throw std::invalid_argument("Export requires project, binaries, output and initial scene");
    const auto project = fs::canonical(options.projectRoot);
    const auto binaries = fs::canonical(options.binaryDirectory);
    const auto output = fs::weakly_canonical(fs::absolute(options.outputDirectory));
    if (fs::exists(output)) throw std::invalid_argument("Export output already exists: " + output.string());
    auto config = ReadJson(project / "config/config.json");
    const auto resources = fs::canonical(project / config.at("resourcePath").get<std::string>());
    if (IsInside(resources, output) || IsInside(project / "shader", output) || IsInside(project / "ui", output) ||
        IsInside(binaries / "managed", output))
        throw std::invalid_argument("Export output cannot be nested inside packaged input");
    const auto scene = ResolveContent(resources, options.initialScene);
    if (ReadJson(scene).value("type", "") != "scene") throw std::invalid_argument("Export initial asset must be a scene");
    static std::atomic<uint64_t> sequence{0};
    auto staging = output;
    staging += ".staging." + std::to_string(++sequence);
    fs::create_directories(output.parent_path());
    if (!fs::create_directory(staging)) throw std::runtime_error("Export staging directory already exists");
    try
    {
        ExportFiles files(project, resources, staging);
        files.Copy(binaries / "main.exe", "main.exe");
        for (const auto& dependency : CollectNativeGameDependencies(binaries / "main.exe"))
            files.Copy(dependency, dependency.filename());
        files.Tree(project / "shader/glsl", "shader/glsl");
        if (fs::is_directory(project / "shader/spv")) files.Tree(project / "shader/spv", "shader/spv");
        files.Tree(project / config.value("ui", Json::object()).value("assetRoot", std::string("ui")), "ui");
        files.Asset(options.initialScene);
        for (const auto& asset : options.dynamicAssets) files.Asset(asset);
        files.Copy(project / "config/renderGraphConfig.json", "config/renderGraphConfig.json");
        files.Scan(ReadJson(staging / "config/renderGraphConfig.json"));
        // Shader 材质默认贴图也属于依赖，不能只收集 MI 中显式覆盖的绑定。
        for (const auto& entry : fs::recursive_directory_iterator(staging / "shader/glsl"))
            if (entry.is_regular_file() && entry.path().extension() == ".json") files.Scan(ReadJson(entry.path()));
        ManagedRuntime(files, config, options, project, binaries);
        config.erase("projectPath");
        config["resourcePath"] = "resources";
        config["initScene"] = scene.lexically_relative(resources).generic_string();
        config["ui"]["assetRoot"] = "ui";
        config["ui"]["hotReload"] = false;
        config["ui"]["developerUiEnabled"] = false;
        config["ui"]["developerUiVisible"] = false;
        WriteJson(staging / "config/config.json", config);
        GameExportResult result;
        result.manifest = {{"format", 1}, {"hash", "BLAKE3-256"},
            {"initialScene", config.at("initScene")}, {"dynamicAssets", options.dynamicAssets}, {"files", Json::array()}};
        std::map<std::string, fs::path> ordered;
        for (const auto& entry : fs::recursive_directory_iterator(staging))
            if (entry.is_regular_file()) ordered.emplace(entry.path().lexically_relative(staging).generic_string(), entry.path());
        for (const auto& entry : ordered)
        {
            const auto size = fs::file_size(entry.second);
            result.manifest["files"].push_back({{"path", entry.first}, {"size", size},
                {"digest", ContentHasher::HashFile(entry.second).ToHex()}});
            result.totalBytes += size;
        }
        result.fileCount = ordered.size();
        WriteJson(staging / "game-export-manifest.json", result.manifest);
        fs::rename(staging, output);
        return result;
    }
    catch (...)
    {
        std::error_code error;
        fs::remove_all(staging, error);
        throw;
    }
}
} // namespace VL
