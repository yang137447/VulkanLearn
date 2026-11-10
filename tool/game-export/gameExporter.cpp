#include "gameExporter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include "shader/build/contentHash.h"

namespace VL
{
namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr const char* kRuntimeUiManifest = "runtime-ui.manifest.json";
constexpr const char* kEditorUiManifest = "editor-ui.manifest.json";
constexpr const char* kBuildFlavorMarker = "vl_build_flavor.json";
constexpr const char* kGameBuildMarker = "main_game.exe";
constexpr const char* kShaderCachePath = "shader/spv/shader-build-cache.json";
constexpr const char* kRuntimeShaderManifest = "shader/runtime-shader.manifest.json";

// 运行时 Play 会直接使用这份裂纹覆盖层默认材质；它没有出现在
// SC_fracture_playground 的 scene JSON 里，所以导出器必须显式补进闭包。
constexpr const char* kGameplayDefaultMaterials[] = {
    "Common/Materials/MI_glass_crack.json"};

// 这些 compute shader 属于运行时功能，不是编辑器内容。列表按 GLSL 相对路径
// 固定，导出时用 normalizedKey 的 path= 字段匹配，不猜测 artifact kind 的语义。
constexpr const char* kRuntimeComputeShaders[] = {
    "engine/skinnedDeform.comp",
    "terrain/terrainPageBake.comp",
    "generator/skyToCubemap.comp",
    "generator/skySHGenerate.comp",
    "generator/prefilterEnvMap.comp",
    "generator/equirectToCubemap.comp",
    "hair/tressfxSkinning.comp",
    "hair/tressfxSimCollision.comp",
    "hair/tressfxSimIntegration.comp",
    "hair/tressfxSimLengthWind.comp",
    "hair/tressfxSimLocalShape.comp",
    "hair/tressfxSimSdfClear.comp",
    "hair/tressfxSimSdfCollide.comp",
    "hair/tressfxSimSdfSplat.comp",
    "hair/tressfxSimStrandLevel.comp",
    "hair/tressfxSimVsp.comp"};

std::string Lower(std::string value)
{
    for (auto& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

std::string NormalizeSlashes(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
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
    name = NormalizeSlashes(std::move(name));
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

std::string ReadText(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read export text: " + path.string());
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

void WriteJson(const fs::path& path, const Json& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value.dump(2) << '\n';
    output.close();
    if (!output) throw std::runtime_error("Cannot write export JSON: " + path.string());
}

std::string Digest(const fs::path& path)
{
    if (!fs::is_regular_file(path)) throw std::runtime_error("Missing export digest target: " + path.string());
    return ContentHasher::HashFile(path).ToHex();
}

std::optional<std::string> ExtractNormalizedField(
    const std::string& normalizedKey,
    const std::string& field)
{
    const std::string token = field + "=";
    size_t position = 0;
    while ((position = normalizedKey.find(token, position)) != std::string::npos)
    {
        if (position == 0 || normalizedKey[position - 1] == '|')
        {
            const size_t valueStart = position + token.size();
            const size_t valueEnd = normalizedKey.find('|', valueStart);
            return normalizedKey.substr(valueStart,
                valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);
        }
        position += token.size();
    }
    return std::nullopt;
}

std::string NormalizeShaderGlslRelativePath(std::string path)
{
    path = NormalizeSlashes(std::move(path));
    const fs::path normalized(path);
    if (normalized.is_absolute() || normalized.has_root_name())
        throw std::runtime_error("Shader source path must be relative: " + path);
    fs::path relative;
    bool foundGlsl = false;
    for (const auto& part : normalized)
    {
        if (foundGlsl)
            relative /= part;
        else if (part == "glsl")
            foundGlsl = true;
    }
    if (!foundGlsl || relative.empty())
        throw std::runtime_error("Shader source path must be under shader/glsl: " + path);
    return relative.lexically_normal().generic_string();
}

std::string InferShaderNameFromMaterialPath(const std::string& materialRelativePath)
{
    const fs::path materialPath(materialRelativePath);
    const std::string filename = materialPath.filename().string();
    if (filename.rfind("M_", 0) != 0)
        throw std::runtime_error("Material definition filename must start with M_: " + materialRelativePath);
    const std::string stem = fs::path(filename).stem().string().substr(2);
    return (materialPath.parent_path() / stem).generic_string();
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

    void Scan(const Json& value, const std::string& field = {},
        const std::string& parentField = {})
    {
        if (value.is_object())
        {
            if (value.contains("type") && value.at("type").is_string() &&
                (value.at("type") == "blueprint" || value.at("type") == "blueprintInstance"))
                requiresScripts = true;
            for (const auto& item : value.items()) Scan(item.value(), item.key(), field);
            return;
        }
        if (value.is_array())
        {
            for (const auto& item : value) Scan(item, field, parentField);
            return;
        }
        if (!value.is_string()) return;
        // 蒙皮 source 是场景身份，不是资源路径；代理与权重才属于导出依赖闭包。
        if (field == "source" && parentField == "skinning") return;
        auto name = value.get<std::string>();
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty()) return;
        const bool assetField = field == "modelPath" || field == "modelDataPath" || field == "materialOverride" ||
            field == "materialInstancePath" || field == "blueprintPath" || field == "spawnAssets" ||
            field == "hdrPath" || field == "source" || field == "boneWeights" ||
            (field == "path" && parentField == "mesh");
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
    const fs::path& Staging() const { return staging; }
    const fs::path& ResourceRoot() const { return resourceRoot; }
    const fs::path& ProjectRoot() const { return projectRoot; }

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

struct UiManifest
{
    fs::path path;
    std::vector<std::string> files;
};

UiManifest ReadUiManifest(const fs::path& path)
{
    const Json value = ReadJson(path);
    if (value.value("format", 0) != 1 || !value.contains("files") || !value.at("files").is_array())
        throw std::runtime_error("UI manifest must be format 1 with a files array: " + path.string());
    UiManifest manifest;
    manifest.path = path;
    for (const auto& item : value.at("files"))
    {
        if (!item.is_string()) throw std::runtime_error("UI manifest entries must be strings: " + path.string());
        const std::string relative = NormalizeSlashes(item.get<std::string>());
        const fs::path candidate(relative);
        if (relative.empty() || candidate.is_absolute() || candidate.has_root_name() ||
            candidate.has_root_directory() || relative.find("..") != std::string::npos)
            throw std::runtime_error("UI manifest contains an unsafe path: " + relative);
        manifest.files.push_back(candidate.lexically_normal().generic_string());
    }
    return manifest;
}

bool IsExternalUiReference(const std::string& reference)
{
    return reference.empty() || reference[0] == '#' || reference.rfind("data:", 0) == 0 ||
        reference.rfind("http:", 0) == 0 || reference.rfind("https:", 0) == 0 ||
        reference.rfind("vl://", 0) == 0;
}

std::vector<std::string> CollectUiReferences(const fs::path& path)
{
    const std::string text = ReadText(path);
    std::vector<std::string> references;
    auto collect = [&references](const std::string& bytes, const std::regex& expression)
    {
        for (std::sregex_iterator it(bytes.begin(), bytes.end(), expression), end; it != end; ++it)
            references.push_back((*it)[1].str());
    };
    const std::string extension = Lower(path.extension().string());
    if (extension == ".rml")
    {
        collect(text, std::regex(R"rml((?:href|src)\s*=\s*"([^"]+)")rml"));
    }
    else if (extension == ".rcss")
    {
        collect(text, std::regex(R"(url\(\s*['"]?([^'")\s]+)['"]?\s*\))"));
        collect(text, std::regex(R"(@import\s+(?:url\(\s*)?['"]([^'"]+)['"])"));
    }
    return references;
}

void ValidateUiManifest(
    const fs::path& uiRoot,
    const UiManifest& manifest,
    bool runtimeManifest)
{
    std::set<std::string> listed;
    for (const auto& file : manifest.files) listed.insert(Lower(file));
    for (const auto& file : manifest.files)
    {
        ResolveContent(uiRoot, file);
        const std::string lowerFile = Lower(file);
        if (runtimeManifest &&
            (lowerFile.rfind("editor-", 0) == 0 || lowerFile.rfind("widgets/", 0) == 0))
            throw std::runtime_error("Runtime UI manifest contains editor-only content: " + file);
        for (const std::string& reference : CollectUiReferences(uiRoot / file))
        {
            if (IsExternalUiReference(reference)) continue;
            const fs::path relativeReference(reference);
            if (relativeReference.is_absolute() || relativeReference.has_root_name() ||
                relativeReference.has_root_directory())
                throw std::runtime_error("UI reference must be relative: " + file + " -> " + reference);
            const fs::path target =
                (fs::path(file).parent_path() / relativeReference).lexically_normal();
            for (const auto& part : target)
                if (part == "..")
                    throw std::runtime_error("UI reference escapes the asset root: " + file + " -> " + reference);
            const std::string generic = target.generic_string();
            if (listed.find(Lower(generic)) == listed.end())
                throw std::runtime_error(
                    "UI manifest closure is incomplete: " + file + " references " + reference +
                    " (" + generic + ") but it is not listed in " + manifest.path.filename().string());
            ResolveContent(uiRoot, generic);
        }
    }
    if (runtimeManifest)
    {
        bool hasLocalization = false;
        for (const auto& file : manifest.files)
            if (Lower(file) == "localization.json") hasLocalization = true;
        if (!hasLocalization)
            throw std::runtime_error("Runtime UI manifest must list localization.json");
    }
}

void CopyUiClosure(
    ExportFiles& files,
    const fs::path& project,
    const Json& config,
    bool gameFlavor)
{
    const auto uiRoot = fs::canonical(project / config.value("ui", Json::object()).value("assetRoot", std::string("ui")));
    const auto editorManifestPath = uiRoot / kEditorUiManifest;
    if (fs::is_regular_file(editorManifestPath))
        ValidateUiManifest(uiRoot, ReadUiManifest(editorManifestPath), false);

    if (!gameFlavor)
    {
        files.Tree(uiRoot, "ui");
        return;
    }

    const auto runtimeManifestPath = uiRoot / kRuntimeUiManifest;
    if (!fs::is_regular_file(runtimeManifestPath))
        throw std::runtime_error("Game flavor requires ui/" + std::string(kRuntimeUiManifest));
    const UiManifest runtimeManifest = ReadUiManifest(runtimeManifestPath);
    ValidateUiManifest(uiRoot, runtimeManifest, true);
    for (const auto& file : runtimeManifest.files)
        files.Copy(uiRoot / file, fs::path("ui") / file);
    files.Copy(runtimeManifestPath, fs::path("ui") / kRuntimeUiManifest);
}

Json StripEditorGraphForGame(const Json& graph)
{
    Json stripped = graph;
    if (stripped.contains("resources") && stripped.at("resources").is_array())
    {
        Json resources = Json::array();
        for (const auto& resource : stripped.at("resources"))
        {
            const std::string text = Lower(resource.dump());
            if (text.find("editorgrid") != std::string::npos) continue;
            resources.push_back(resource);
        }
        stripped["resources"] = std::move(resources);
    }
    if (stripped.contains("passes") && stripped.at("passes").is_array())
    {
        Json passes = Json::array();
        for (const auto& pass : stripped.at("passes"))
        {
            const std::string name = pass.value("name", std::string());
            const std::string type = pass.value("type", std::string());
            if (Lower(name) == "editorgrid" || Lower(type) == "editorgrid") continue;
            passes.push_back(pass);
        }
        stripped["passes"] = std::move(passes);
    }
    return stripped;
}

struct ShaderClosure
{
    Json manifest;
};

ShaderClosure BuildGameShaderClosure(
    const fs::path& project,
    const fs::path& staging,
    const Json& graph,
    const std::function<void(const fs::path&, const fs::path&)>& copy)
{
    const auto cachePath = project / kShaderCachePath;
    if (!fs::is_regular_file(cachePath))
        throw std::runtime_error(
            "Game flavor requires shader/spv/shader-build-cache.json; build the Release game target first");
    const Json cache = ReadJson(cachePath);
    if (cache.value("schemaVersion", 0) != 1 ||
        cache.value("hashAlgorithm", std::string()) != "BLAKE3-256" ||
        cache.value("targetEnvironment", std::string()) != "Vulkan1.4")
        throw std::runtime_error("Unsupported shader build cache header: " + cachePath.string());
    const std::string compilePolicy = cache.at("compilePolicy").get<std::string>();
    if (compilePolicy.find("configuration=Release;") == std::string::npos)
        throw std::runtime_error(
            "Game shader cache compilePolicy is not a Release game build: " + compilePolicy);

    std::set<std::string> materialInstances;
    for (const auto& entry : fs::recursive_directory_iterator(staging / "resources"))
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            const Json value = ReadJson(entry.path());
            if (value.is_object() && value.contains("material") && value.at("material").is_string())
                materialInstances.insert(entry.path().lexically_relative(staging / "resources").generic_string());
        }

    std::set<std::string> materialSources;
    std::set<std::string> graphicsShaders{"uiOverlay"};
    std::set<std::string> computeShaders;
    for (const auto* name : kRuntimeComputeShaders) computeShaders.insert(name);
    for (const auto& pass : graph.value("passes", Json::array()))
    {
        const std::string type = pass.value("type", std::string());
        if (type == "shadow") graphicsShaders.insert("shadow");
        if (type == "autoExposure")
        {
            computeShaders.insert("pass/autoExposureHistogram.comp");
            computeShaders.insert("pass/autoExposureAdapt.comp");
        }
    }

    std::map<std::string, Json> materialDefinitions;
    for (const auto& instance : materialInstances)
    {
        const Json instanceJson = ReadJson(staging / "resources" / instance);
        if (!instanceJson.contains("material") || !instanceJson.at("material").is_string())
            throw std::runtime_error("Material instance is missing its material field: " + instance);
        const std::string materialPath =
            NormalizeShaderGlslRelativePath(instanceJson.at("material").get<std::string>());
        const auto sourcePath = project / "shader/glsl" / materialPath;
        if (!fs::is_regular_file(sourcePath))
            throw std::runtime_error("Material definition is missing: " + materialPath);
        if (materialDefinitions.find(materialPath) == materialDefinitions.end())
            materialDefinitions.emplace(materialPath, ReadJson(sourcePath));
        const Json& definition = materialDefinitions.at(materialPath);
        if (definition.contains("shaderEvaluation"))
            materialSources.insert(materialPath);
        else
            graphicsShaders.insert(InferShaderNameFromMaterialPath(materialPath));
    }

    const Json& artifacts = cache.at("artifacts");
    std::set<std::string> retained;
    std::set<std::string> matchedMaterials;
    std::set<std::string> matchedGraphics;
    std::set<std::string> matchedCompute;
    for (const auto& [logicalBuildId, artifact] : artifacts.items())
    {
        const std::string kind = artifact.at("kind").get<std::string>();
        const std::string normalizedKey = artifact.at("normalizedKey").get<std::string>();
        if (kind == "MaterialGraphicsPair")
        {
            const auto source = ExtractNormalizedField(normalizedKey, "materialSource");
            if (source.has_value() && materialSources.count(*source) != 0)
            {
                retained.insert(logicalBuildId);
                matchedMaterials.insert(*source);
            }
        }
        else if (kind == "GraphicsPair")
        {
            const auto shaderName = ExtractNormalizedField(normalizedKey, "shaderName");
            if (shaderName.has_value() && graphicsShaders.count(*shaderName) != 0)
            {
                retained.insert(logicalBuildId);
                matchedGraphics.insert(*shaderName);
            }
        }
        else if (kind == "StageEntry")
        {
            const auto shaderPath = ExtractNormalizedField(normalizedKey, "path");
            if (shaderPath.has_value() && computeShaders.count(*shaderPath) != 0)
            {
                retained.insert(logicalBuildId);
                matchedCompute.insert(*shaderPath);
            }
        }
    }
    for (const auto& source : materialSources)
        if (matchedMaterials.count(source) == 0)
            throw std::runtime_error(
                "Shader cache has no MaterialGraphicsPair for shipped material " + source +
                "; rebuild the Release game cache with --shader-force-rebuild");
    for (const auto& shaderName : graphicsShaders)
        if (matchedGraphics.count(shaderName) == 0)
            throw std::runtime_error(
                "Shader cache has no GraphicsPair for required shader " + shaderName +
                "; rebuild the Release game cache with --shader-force-rebuild");
    for (const auto& shaderPath : computeShaders)
        if (matchedCompute.count(shaderPath) == 0)
            throw std::runtime_error(
                "Shader cache has no StageEntry for required compute shader " + shaderPath +
                "; rebuild the Release game cache with --shader-force-rebuild");

    std::map<std::string, std::string> generatedIncludes;
    for (const auto& [materialPath, definition] : materialDefinitions)
    {
        const std::string name = definition.at("name").get<std::string>();
        const std::string generated =
            (fs::path(materialPath).parent_path() / "generate" / (name + "Paramter.glsl")).generic_string();
        generatedIncludes.emplace(generated, materialPath);
    }

    Json manifestArtifacts = Json::object();
    Json manifestGenerated = Json::array();
    std::map<std::string, std::string> copiedSources;
    for (const auto& logicalBuildId : retained)
    {
        const Json& artifact = artifacts.at(logicalBuildId);
        Json outputs = Json::object();
        for (const auto& [role, output] : artifact.at("outputs").items())
        {
            const std::string relative = NormalizeSlashes(output.at("path").get<std::string>());
            const auto source = project / "shader" / relative;
            const std::string expected = output.at("digest").get<std::string>();
            if (Digest(source) != expected)
                throw std::runtime_error("Shader artifact digest mismatch: " + relative);
            copy(source, fs::path("shader") / relative);
            outputs[role] = {{"path", relative}, {"digest", expected}};
        }
        for (const auto& dependency : artifact.at("dependencies"))
        {
            const std::string relative = NormalizeSlashes(dependency.at("path").get<std::string>());
            const std::string expected = dependency.at("digest").get<std::string>();
            if (relative.rfind("generate/", 0) == 0 || relative.find("/generate/") != std::string::npos)
            {
                const auto materialIt = generatedIncludes.find(relative);
                if (materialIt == generatedIncludes.end())
                    throw std::runtime_error(
                        "Generated shader dependency is not reproducible from a shipped M_ asset: " + relative);
                bool recorded = false;
                for (const auto& existing : manifestGenerated)
                    if (existing.at("path") == relative) recorded = true;
                if (!recorded)
                    manifestGenerated.push_back({{"path", relative}, {"derivedFrom", materialIt->second}});
                continue;
            }
            const auto source = project / "shader/glsl" / relative;
            if (Digest(source) != expected)
                throw std::runtime_error("Shader source digest mismatch: " + relative);
            copy(source, fs::path("shader/glsl") / relative);
            copiedSources.emplace(relative, expected);
        }
        for (const auto& primary : artifact.at("primarySources"))
        {
            const std::string identity = NormalizeSlashes(primary.at("identity").get<std::string>());
            if (identity.rfind("__composed__/", 0) == 0) continue;
            if (identity.find(':') != std::string::npos) continue;
            const auto source = project / "shader/glsl" / identity;
            if (!fs::is_regular_file(source)) continue;
            const std::string expected = primary.at("digest").get<std::string>();
            if (Digest(source) != expected)
                throw std::runtime_error("Shader primary source digest mismatch: " + identity);
            copy(source, fs::path("shader/glsl") / identity);
            copiedSources.emplace(identity, expected);
        }
        manifestArtifacts[logicalBuildId] = {
            {"kind", artifact.at("kind")},
            {"normalizedKey", artifact.at("normalizedKey")},
            {"outputs", std::move(outputs)}};
    }

    Json materials = Json::array();
    for (const auto& [materialPath, definition] : materialDefinitions)
    {
        const auto source = project / "shader/glsl" / materialPath;
        copy(source, fs::path("shader/glsl") / materialPath);
        materials.push_back({{"path", materialPath}, {"digest", Digest(source)}});
        if (definition.contains("shaderEvaluation"))
        {
            for (const auto* stage : {"vertex", "surface"})
            {
                const std::string relative = NormalizeSlashes(
                    definition.at("shaderEvaluation").at(stage).get<std::string>());
                const auto evaluationSource = project / "shader/glsl" / relative;
                if (!fs::is_regular_file(evaluationSource))
                    throw std::runtime_error("Material evaluation source is missing: " + relative);
                copy(evaluationSource, fs::path("shader/glsl") / relative);
                copiedSources.emplace(relative, Digest(evaluationSource));
            }
        }
    }

    copy(cachePath, fs::path("shader/spv/shader-build-cache.json"));
    const std::string cacheDigest = Digest(cachePath);

    Json sourceFiles = Json::array();
    for (const auto& [path, digest] : copiedSources)
        sourceFiles.push_back({{"path", path}, {"digest", digest}});
    Json retainedIds = Json::array();
    for (const auto& id : retained) retainedIds.push_back(id);
    Json materialSourceList = Json::array();
    for (const auto& source : materialSources) materialSourceList.push_back(source);
    Json graphicsShaderList = Json::array();
    for (const auto& shader : graphicsShaders) graphicsShaderList.push_back(shader);
    Json computeShaderList = Json::array();
    for (const auto& shader : computeShaders) computeShaderList.push_back(shader);

    ShaderClosure closure;
    closure.manifest = {
        {"format", 1},
        {"hash", "BLAKE3-256"},
        {"compilePolicy", compilePolicy},
        {"cache", {{"path", "spv/shader-build-cache.json"}, {"digest", cacheDigest}}},
        {"artifacts", std::move(manifestArtifacts)},
        {"artifactIds", std::move(retainedIds)},
        {"materialSources", std::move(materialSourceList)},
        {"graphicsShaders", std::move(graphicsShaderList)},
        {"computeShaders", std::move(computeShaderList)},
        {"materials", std::move(materials)},
        {"sourceFiles", std::move(sourceFiles)},
        {"generatedAtStartup", std::move(manifestGenerated)}};
    return closure;
}

void ValidateGameBuildMarker(const fs::path& binaries)
{
    const auto markerPath = binaries / kBuildFlavorMarker;
    if (!fs::is_regular_file(markerPath))
        throw std::runtime_error(
            "Game flavor requires " + std::string(kGameBuildMarker) + " and " +
            std::string(kBuildFlavorMarker) + " next to it; main.exe is not a game binary");
    const Json marker = ReadJson(markerPath);
    if (!marker.contains("editorEnabled") || !marker.at("editorEnabled").is_boolean() ||
        marker.at("editorEnabled").get<bool>())
        throw std::runtime_error("Game flavor requires vl_build_flavor.json editorEnabled=false");
    if (marker.value("flavor", std::string()) != "game")
        throw std::runtime_error("Game flavor requires vl_build_flavor.json flavor=game");
}

fs::path SelectDevExecutable(const fs::path& binaries)
{
    for (const auto* name : {"main.exe", "main_editor.exe", "main_game.exe"})
        if (fs::is_regular_file(binaries / name)) return binaries / name;
    throw std::runtime_error("Export binary directory has no main.exe/main_editor.exe/main_game.exe");
}

std::string FlavorName(GameExportFlavor flavor)
{
    return flavor == GameExportFlavor::Game ? "game" : "dev";
}
} // namespace

GameExportResult ExportGame(const GameExportOptions& options)
{
    if (options.projectRoot.empty() || options.binaryDirectory.empty() || options.outputDirectory.empty() ||
        options.initialScene.empty()) throw std::invalid_argument("Export requires project, binaries, output and initial scene");
    const bool gameFlavor = options.flavor == GameExportFlavor::Game;
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
        fs::path executable;
        if (gameFlavor)
        {
            executable = binaries / kGameBuildMarker;
            if (!fs::is_regular_file(executable))
                throw std::runtime_error(
                    "Game flavor requires main_game.exe and rejects main.exe; build the game target first");
            ValidateGameBuildMarker(binaries);
            files.Copy(binaries / kBuildFlavorMarker, kBuildFlavorMarker);
        }
        else
        {
            executable = SelectDevExecutable(binaries);
        }
        files.Copy(executable, executable.filename());
        for (const auto& dependency : CollectNativeGameDependencies(executable))
            files.Copy(dependency, dependency.filename());
        if (!gameFlavor)
        {
            files.Tree(project / "shader/glsl", "shader/glsl");
            if (fs::is_directory(project / "shader/spv")) files.Tree(project / "shader/spv", "shader/spv");
        }
        CopyUiClosure(files, project, config, gameFlavor);
        files.Asset(options.initialScene);
        for (const auto& asset : options.dynamicAssets) files.Asset(asset);
        if (gameFlavor)
        {
            for (const auto* material : kGameplayDefaultMaterials)
                files.Asset(material);
        }

        auto graph = ReadJson(project / "config/renderGraphConfig.json");
        if (gameFlavor) graph = StripEditorGraphForGame(graph);
        WriteJson(staging / "config/renderGraphConfig.json", graph);
        files.Scan(ReadJson(staging / "config/renderGraphConfig.json"));

        if (gameFlavor)
        {
            const ShaderClosure closure =
                BuildGameShaderClosure(project, staging, graph,
                    [&files](const fs::path& source, const fs::path& destination)
                    {
                        files.Copy(source, destination);
                    });
            WriteJson(staging / kRuntimeShaderManifest, closure.manifest);
        }
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
            {"flavor", FlavorName(options.flavor)}, {"uiFallback", !gameFlavor},
            {"initialScene", config.at("initScene")}, {"dynamicAssets", options.dynamicAssets},
            {"files", Json::array()}};
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

void VerifyGamePackage(const std::filesystem::path& packageDirectory)
{
    const auto package = fs::canonical(packageDirectory);
    const Json exportManifest = ReadJson(package / "game-export-manifest.json");
    for (const auto& file : exportManifest.at("files"))
    {
        const std::string relative = file.at("path").get<std::string>();
        const auto path = ResolveContent(package, relative);
        if (fs::file_size(path) != file.at("size").get<uintmax_t>())
            throw std::runtime_error("Package file size mismatch: " + relative);
        if (Digest(path) != file.at("digest").get<std::string>())
            throw std::runtime_error("Package file digest mismatch: " + relative);
    }

    const auto marker = ReadJson(package / kBuildFlavorMarker);
    if (marker.value("editorEnabled", true) || marker.value("flavor", std::string()) != "game")
        throw std::runtime_error("Packaged vl_build_flavor.json is not a game build marker");

    const Json graph = ReadJson(package / "config/renderGraphConfig.json");
    bool selectionMask = false;
    for (const auto& resource : graph.value("resources", Json::array()))
        if (resource.value("name", std::string()) == "selectionMask") selectionMask = true;
    bool needsSelectionMask = false;
    for (const auto& pass : graph.value("passes", Json::array()))
        if (Lower(pass.value("name", std::string())) == "tonemapping" ||
            Lower(pass.value("type", std::string())) == "tonemapping")
            needsSelectionMask = true;
    if (needsSelectionMask && !selectionMask)
        throw std::runtime_error("Packaged renderGraphConfig.json lost selectionMask");
    for (const auto& pass : graph.value("passes", Json::array()))
        if (Lower(pass.value("name", std::string())) == "editorgrid" ||
            Lower(pass.value("type", std::string())) == "editorgrid")
            throw std::runtime_error("Packaged renderGraphConfig.json still contains editorGrid");

    for (const auto& entry : fs::recursive_directory_iterator(package / "ui"))
    {
        const std::string relative = Lower(entry.path().lexically_relative(package).generic_string());
        if (relative.rfind("ui/editor-", 0) == 0 || relative.rfind("ui/widgets/", 0) == 0 ||
            relative.rfind("ui/icons/", 0) == 0)
            throw std::runtime_error("Editor UI content leaked into package: " + relative);
    }
    for (const auto* forbidden : {
             "shader/glsl/pass/editorGrid.vert",
             "shader/glsl/pass/editorGrid.frag",
             "shader/glsl/pass/M_editorGrid.json",
             "resources/Common/Materials/Pass/MI_editorGrid.json"})
        if (fs::exists(package / forbidden))
            throw std::runtime_error("Editor shader content leaked into package: " + std::string(forbidden));

    const Json shaderManifest = ReadJson(package / kRuntimeShaderManifest);
    std::set<std::string> allowedSpv;
    for (const auto& [id, artifact] : shaderManifest.at("artifacts").items())
    {
        static_cast<void>(id);
        for (const auto& [role, output] : artifact.at("outputs").items())
        {
            static_cast<void>(role);
            const std::string relative = output.at("path").get<std::string>();
            const auto path = ResolveContent(package, "shader/" + relative);
            if (Digest(path) != output.at("digest").get<std::string>())
                throw std::runtime_error("Retained shader output digest mismatch: " + relative);
            allowedSpv.insert(Lower("shader/" + relative));
        }
    }
    for (const auto& source : shaderManifest.at("sourceFiles"))
    {
        const std::string relative = source.at("path").get<std::string>();
        const auto path = ResolveContent(package, "shader/glsl/" + relative);
        if (Digest(path) != source.at("digest").get<std::string>())
            throw std::runtime_error("Shader source digest mismatch: " + relative);
    }
    for (const auto& material : shaderManifest.at("materials"))
    {
        const std::string relative = material.at("path").get<std::string>();
        const auto path = ResolveContent(package, "shader/glsl/" + relative);
        if (Digest(path) != material.at("digest").get<std::string>())
            throw std::runtime_error("Material digest mismatch: " + relative);
    }
    for (const auto& generated : shaderManifest.at("generatedAtStartup"))
    {
        const std::string relative = generated.at("path").get<std::string>();
        if (fs::exists(package / "shader/glsl" / relative))
            throw std::runtime_error("Generated shader include was packaged: " + relative);
    }
    const auto cacheRecord = shaderManifest.at("cache");
    const auto cachePath = package / "shader" / cacheRecord.at("path").get<std::string>();
    if (Digest(cachePath) != cacheRecord.at("digest").get<std::string>())
        throw std::runtime_error("Packaged shader cache digest mismatch");
    allowedSpv.insert(Lower(cachePath.lexically_relative(package).generic_string()));
    for (const auto& entry : fs::recursive_directory_iterator(package / "shader/spv"))
    {
        if (!entry.is_regular_file()) continue;
        const std::string relative = Lower(entry.path().lexically_relative(package).generic_string());
        if (allowedSpv.find(relative) == allowedSpv.end())
            throw std::runtime_error("shader/spv contains a file outside runtime-shader.manifest.json: " + relative);
    }

    for (const auto& entry : fs::recursive_directory_iterator(package / "resources"))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const Json value = ReadJson(entry.path());
        if (!value.is_object() || !value.contains("material") || !value.at("material").is_string()) continue;
        const std::string material =
            NormalizeShaderGlslRelativePath(value.at("material").get<std::string>());
        if (!fs::is_regular_file(package / "shader/glsl" / material))
            throw std::runtime_error(
                "Shipped material instance references a missing material: " + material);
    }
}
} // namespace VL
