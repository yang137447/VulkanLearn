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
#include "terrainPackageClosure.h"
#include "terrain/terrainPaintSha256.h"
#include "terrain/validation/terrainAssetValidator.h"
#include "material/loader/materialInstanceResolver.h"
#include "material/materialDescriptorSchema.h"
#include "material/generator/materialParameterIncludeGenerator.h"

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
constexpr const char* kTerrainFrozenPath = "config/terrainRvtP0Frozen.json";
constexpr const char* kTerrainFrozenShaPath = "config/terrainRvtP0Frozen.json.sha256";
constexpr const char* kTerrainFrozenSha256 =
    "43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd";

void ValidateTerrainRuntimeMaterial(
    const fs::path& resourceRoot, const fs::path& projectRoot, const std::string& material)
{
    std::ifstream input(TerrainPackageClosure::Resolve(resourceRoot, material));
    const auto instance = Json::parse(input);
    const auto definitionName = TerrainPackageClosure::Relative(
        TerrainPackageClosure::Text(instance.at("material"))).generic_string();
    std::ifstream definitionInput(TerrainPackageClosure::Resolve(projectRoot, definitionName));
    const auto definition = Json::parse(definitionInput);
    const auto resolved = MaterialInstanceResolver::ResolveWithDefinition(material, instance, definition);
    const auto schema = MaterialDescriptorSchema::Build(resolved.materialJson, resolved.materialPath);
    schema.ValidateInstanceValues(resolved.effectiveMaterialInstanceJson.at("parameters"),
        resolved.effectiveMaterialInstanceJson.at("textures"), {}, material);
}

void ValidateTerrainFrozenConfig(const fs::path& root)
{
    std::ifstream input(root / kTerrainFrozenPath, std::ios::binary);
    if (!input) throw std::runtime_error("Missing frozen terrain P0 configuration");
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (TerrainPaintSha256Hex(bytes) != kTerrainFrozenSha256)
        throw std::runtime_error("Frozen terrain P0 configuration differs from approved SHA256");
    std::ifstream sidecar(root / kTerrainFrozenShaPath);
    std::string recorded;
    sidecar >> recorded;
    if (!sidecar || recorded != kTerrainFrozenSha256)
        throw std::runtime_error("Frozen terrain P0 SHA256 sidecar differs from approved SHA256");
}

// 运行时 Play 会直接使用这份裂纹覆盖层默认材质；它没有出现在
// SC_fracture_playground 的 scene JSON 里，所以导出器必须显式补进闭包。
constexpr const char* kGameplayDefaultMaterials[] = {
    "Common/Materials/MI_glass_crack.json"};

// 这些 compute shader 属于运行时功能，不是编辑器内容。列表按 GLSL 相对路径
// 固定，导出时用 normalizedKey 的 path= 字段匹配，不猜测 artifact kind 的语义。
constexpr const char* kRuntimeComputeShaders[] = {
    "engine/skinnedDeform.comp",
    "terrain/terrainPageBake.comp",
    "terrain/terrainCompositedBakeWorld.comp",
    "terrain/terrainCompositedBakeWorldPadding.comp",
    "terrain/terrainCompositedQuality.comp",
    "pass/autoExposureHistogram.comp",
    "pass/autoExposureAdapt.comp",
    "generator/brfdLut.comp",
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
        if (!TerrainPackageClosure::IsRegularFile(source))
            throw std::runtime_error("Missing export file: " + source.string());
        const auto physical = TerrainPackageClosure::CanonicalOrAbsolute(source);
        const auto key = Lower(destination.generic_string());
        const auto existing = destinations.find(key);
        if (existing != destinations.end())
        {
            if (existing->second != physical) throw std::runtime_error("Export destination conflict: " + destination.string());
            return;
        }
        destinations.emplace(key, physical);
        const auto staged = staging / destination;
        fs::create_directories(staged.parent_path());
#ifdef _WIN32
        // 源可能是超过 MAX_PATH 的 VT 输入；CopyFileW 配合 \\?\ 前缀才能打开。
        if (!CopyFileW(TerrainPackageClosure::ExtendedPath(source).c_str(),
                TerrainPackageClosure::ExtendedPath(staged).c_str(), FALSE))
            throw std::runtime_error("Cannot copy export file: " + source.string());
#else
        fs::copy_file(source, staged);
#endif
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
        if (runtimeTerrainOutputs.count(name)) return;
        const auto source = ResolveContent(resourceRoot, name);
        const auto relative = source.lexically_relative(resourceRoot);
        Copy(source, fs::path("resources") / relative);
        if (modelSource)
        {
            // glTF/OBJ/SpeedTree 的外部 buffer/贴图可以藏在二进制或 MTL 中；保留源目录整包。
            Tree(source.parent_path(), fs::path("resources") / relative.parent_path());
        }
        if (source.extension() == ".json" && parsedAssets.insert(source).second)
        {
            auto value = ReadJson(staging / "resources" / relative);
            if (value.is_object() && value.value("type", std::string()) == "terrain")
            {
                throw std::runtime_error("Terrain export requires a scene placement identity: " + name);
            }
            if (value.is_object() && value.contains("kind") && value.at("kind") == "terrain-composited-vt")
            {
                Terrain(relative.generic_string());
            }
            else
            {
                RewriteTerrainPlacements(value);
                WriteJson(staging / "resources" / relative, value);
                Scan(value);
            }
        }
    }

    std::string RuntimeTerrain(const std::string& name, const std::string& surfaceId)
    {
        const auto source = ResolveContent(resourceRoot, name);
        const auto relative = source.lexically_relative(resourceRoot).generic_string();
        const auto key = relative + "\n" + surfaceId;
        const auto existing = runtimeTerrainPaths.find(key);
        if (existing != runtimeTerrainPaths.end()) return existing->second;
        auto value = ReadJson(source);
        TerrainAssetValidator::Validate(value, relative);
        const auto& settings = value.at("compositedVT");
        const auto manifestName = TerrainPackageClosure::Text(settings.at("packagePath"));
        const auto relocated = Terrain(manifestName);
        const auto store = TerrainCompositedVTStore::Open(staging / "resources" / relocated);
        const auto& surfaces = store.Production().surfaces;
        const auto surface = std::find_if(surfaces.begin(), surfaces.end(),
            [&](const auto& entry) { return entry.surfaceId == surfaceId; });
        if (surface == surfaces.end())
            throw std::runtime_error("Terrain placement has no published surface: " + surfaceId);
        const auto manifestParent = fs::path(relocated).parent_path();
        value["materialInstancePath"] =
            (manifestParent / TerrainPackageClosure::Relative(surface->runtimeMaterialInstancePath)).generic_string();
        if (surface->runtimeDetailMaterialInstancePath.empty())
            value.erase("detailMaterialInstancePath");
        else
            value["detailMaterialInstancePath"] =
                (manifestParent / TerrainPackageClosure::Relative(surface->runtimeDetailMaterialInstancePath)).generic_string();
        const auto height = ResolveContent(resourceRoot, value.at("heightmapPath").get<std::string>());
        const auto heightName = fs::path("Runtime/Terrain") /
            ("height_" + ContentHasher::HashFile(height).ToHex() + height.extension().generic_string());
        if (runtimeHeightPaths.insert(heightName.generic_string()).second)
            Copy(height, fs::path("resources") / heightName);
        value["heightmapPath"] = heightName.generic_string();
        value.erase("paintPath");
        value["compositedVT"]["packagePath"] = relocated;
        value["compositedVT"].erase("productionSourcePath");
        std::string output = relative;
        if (!runtimeTerrainSources.insert(relative).second)
        {
            output = "Runtime/Terrain/tr_" + ContentHasher::HashString(key).ToHex() + ".json";
            if (value.contains("layers"))
                for (auto& layer : value["layers"])
                    if (layer.value("key", std::string()).empty())
                        layer["key"] = relative + "#" + layer.at("name").get<std::string>();
        }
        TerrainAssetValidator::Validate(value, output);
        WriteJson(staging / "resources" / output, value);
        runtimeTerrainPaths.emplace(key, output);
        runtimeTerrainOutputs.insert(output);
        return output;
    }

    void RewriteTerrainPlacements(Json& value)
    {
        if (value.is_object())
        {
            if (value.value("type", std::string()) == "terrain" && value.contains("terrainPath"))
            {
                const auto name = value.at("terrainPath").get<std::string>();
                const auto identity = value.value("name", std::string());
                if (identity.empty())
                    throw std::runtime_error("Terrain scene placement requires its native object name: " + name);
                value["terrainPath"] = RuntimeTerrain(name, "terrain|" + identity);
            }
            for (auto& child : value.items()) RewriteTerrainPlacements(child.value());
        }
        else if (value.is_array())
            for (auto& child : value) RewriteTerrainPlacements(child);
    }

    std::string Terrain(const std::string& manifestName)
    {
        const auto sourceManifest = TerrainPackageClosure::Resolve(resourceRoot, manifestName);
        const auto manifestKey = Lower(fs::canonical(sourceManifest).generic_string());
        const auto existing = terrainPackages.find(manifestKey);
        if (existing != terrainPackages.end())
            return existing->second;
        const auto manifest = sourceManifest.lexically_relative(fs::canonical(resourceRoot));
        const auto closure = TerrainPackageClosure::Open(resourceRoot, manifest.generic_string());
        std::vector<std::string> materialErrors;
        for (const auto& material : closure.materialInstances)
        {
            try { ValidateTerrainRuntimeMaterial(resourceRoot, projectRoot, material); }
            catch (const std::exception& error)
            { materialErrors.push_back(material + ": " + error.what()); }
        }
        if (!materialErrors.empty())
        {
            std::ostringstream errors;
            errors << "Terrain runtime MI semantic validation failed (" << materialErrors.size()
                << "/" << closure.materialInstances.size() << "):";
            for (const auto& error : materialErrors) errors << "\n" << error;
            throw std::runtime_error(errors.str());
        }
        for (const auto& material : closure.materialInstances)
            Scan(ReadJson(TerrainPackageClosure::Resolve(resourceRoot, material)));
        const auto inputRoot = fs::canonical(resourceRoot / manifest.parent_path());
        const bool shorten = std::any_of(closure.files.begin(), closure.files.end(),
            [&](const fs::path& file)
            {
                return (staging / "resources" / file.lexically_relative(resourceRoot)).wstring().size() > 259;
            });
        const auto directory = shorten
            ? fs::path("VT") / ContentHasher::HashString(manifest.generic_string()).ToHex().substr(0, 16)
            : manifest.parent_path();
        for (const auto& file : closure.files)
        {
            const auto relative = file.lexically_relative(inputRoot);
            TerrainPackageClosure::Relative(relative.generic_string());
            Copy(file, fs::path("resources") / directory / relative);
        }
        const auto relocated = (directory / manifest.filename()).generic_string();
        TerrainPackageClosure::Open(staging / "resources", relocated);
        terrainPackages.emplace(manifestKey, relocated);
        return relocated;
    }

    void Scan(const Json& value, const std::string& field = {},
        const std::string& parentField = {})
    {
        if (field == "compositedVT")
        {
            if (!value.is_object() || !value.contains("enabled") ||
                !value.at("enabled").is_boolean() || !value.at("enabled").get<bool>())
                throw std::runtime_error("Terrain export requires explicit compositedVT.enabled=true");
            const auto manifest = TerrainPackageClosure::Relative(
                TerrainPackageClosure::Text(value.at("packagePath"))).generic_string();
            Asset(manifest);
            return;
        }
        if (value.is_object())
        {
            if (value.value("type", std::string()) == "glassPane")
                requiresGlassCrack = true;
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
        // renderGraph 的 pass input 用 "source" 表示图内资源名（例如 worldTexture），
        // 不是文件。只有带路径分隔符或扩展名的 source 才进入资源闭包。
        if (field == "source" && name.find('/') == std::string::npos &&
            name.find('.') == std::string::npos)
            return;
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
    bool RequiresGlassCrack() const { return requiresGlassCrack; }
    const fs::path& Staging() const { return staging; }
    const fs::path& ResourceRoot() const { return resourceRoot; }
    const fs::path& ProjectRoot() const { return projectRoot; }

private:
    fs::path projectRoot;
    fs::path resourceRoot;
    fs::path staging;
    std::map<std::string, fs::path> destinations;
    std::map<std::string, std::string> terrainPackages;
    std::set<fs::path> parsedAssets;
    std::map<std::string, std::string> runtimeTerrainPaths;
    std::set<std::string> runtimeTerrainSources;
    std::set<std::string> runtimeTerrainOutputs;
    std::set<std::string> runtimeHeightPaths;
    bool requiresScripts = false;
    bool requiresGlassCrack = false;
};

fs::path SelectVersion(const fs::path& directory, const std::string& prefix)
{
    std::vector<fs::path> candidates;
    if (fs::is_directory(directory))
        for (const auto& entry : fs::directory_iterator(directory))
            if (entry.is_directory() && entry.path().filename().string().rfind(prefix, 0) == 0)
                candidates.push_back(entry.path());
    if (candidates.empty()) throw std::runtime_error("Missing private .NET runtime under " + directory.string());
    // 版本的每一段按数字比较，避免 11.0.9 胜过 11.0.20。
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
    const auto runtime = SelectVersion(dotnet / "shared" / "Microsoft.NETCore.App", "11.");
    const auto fxr = SelectVersion(dotnet / "host" / "fxr", "11.");
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

void ValidateTerrainShaderIdentity(
    const TerrainCompositedProductionClosure& production, const Json& cache)
{
    production.Validate();
    const auto shaderDigest = [&](const std::string& id, TerrainCompositedProductionObjectKind kind)
    {
        const auto& object = production.Object(id);
        if (object.kind != kind)
            throw std::runtime_error("Terrain generator references a non-shader production object");
        return object.digest.ToHex();
    };
    const auto& generator = production.generator;
    if (generator.version != 3 ||
        generator.filterRule != "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3" ||
        generator.encoding != "gpu-rvt-adjacency-area4-rgba8-v3")
        throw std::runtime_error("Terrain publication requires gen3 adjacency-padding identity; recook old pages");
    if (generator.compilePolicy != cache.at("compilePolicy").get<std::string>())
        throw std::runtime_error("Terrain producer compile policy differs from game shader cache");
    const auto mainDigest = shaderDigest(generator.computeSource, TerrainCompositedProductionObjectKind::ShaderSource);
    std::set<std::string> includes;
    for (const auto& id : generator.includeSources)
        includes.insert(shaderDigest(id, TerrainCompositedProductionObjectKind::ShaderSource));
    std::set<std::string> binaries;
    for (const auto& id : generator.compiledArtifacts)
        binaries.insert(shaderDigest(id, TerrainCompositedProductionObjectKind::ShaderArtifact));
    const std::set<std::string> required = {
        "terrain/terrainCompositedBakeWorld.comp", "terrain/terrainCompositedBakeWorldPadding.comp"};
    const std::set<std::string> requiredIncludes = {
        "terrain/terrainCompositedBakeWorldEvaluator.glsl", "terrain/terrainCompositedBakeWorldTypes.glsl",
        "terrain/terrainCompositedBakeRoute.glsl"};
    std::set<std::string> matched, expectedSources, expectedBinaries;
    std::set<std::string> matchedIncludes;
    std::set<std::string> primaryIdentities, outputPaths;
    bool mainMatches = false;
    for (const auto& artifact : cache.at("artifacts"))
    {
        if (artifact.at("kind") != "StageEntry") continue;
        const auto path = ExtractNormalizedField(
            artifact.at("normalizedKey").get<std::string>(), "path");
        if (!path || !required.count(*path)) continue;
        if (!matched.insert(*path).second)
            throw std::runtime_error("Ambiguous terrain producer compute artifact");
        if (artifact.at("primarySources").size() != 1)
            throw std::runtime_error("Terrain producer requires one distinct primary source per pipeline");
        for (const auto& source : artifact.at("primarySources"))
        {
            if (source.at("identity") != *path ||
                !primaryIdentities.insert(source.at("identity").get<std::string>()).second)
                throw std::runtime_error("Terrain producer primary GLSL identity differs from pipeline path");
            if (source.at("identity") == "terrain/terrainCompositedBakeWorld.comp" && source.at("digest") == mainDigest)
                mainMatches = true;
            else
                expectedSources.insert(source.at("digest").get<std::string>());
        }
        for (const auto& dependency : artifact.at("dependencies"))
        {
            const auto dependencyPath = dependency.at("path").get<std::string>();
            if (requiredIncludes.count(dependencyPath))
                matchedIncludes.insert(dependencyPath);
            if (dependencyPath != "terrain/terrainCompositedBakeWorld.comp")
                expectedSources.insert(dependency.at("digest").get<std::string>());
        }
        const auto& outputs = artifact.at("outputs");
        if (!outputs.contains("runtimeCompute"))
            throw std::runtime_error("Terrain producer compute runtime output missing");
        for (const auto& output : outputs)
        {
            if (!outputPaths.insert(output.at("path").get<std::string>()).second)
                throw std::runtime_error("Terrain producer pipelines alias a compiled output path");
            expectedBinaries.insert(output.at("digest").get<std::string>());
        }
    }
    if (matched == required && mainMatches && matchedIncludes == requiredIncludes &&
        includes == expectedSources && binaries == expectedBinaries)
        return;
    throw std::runtime_error("Terrain producer main/include/SPV identity differs from game shader cache");
}

void ValidateRuntimeComputeClosure(const fs::path& root, const Json& cache, const Json& manifest);

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
            if (value.is_object() && value.value("kind", std::string()) == "terrain-composited-vt")
            {
                const auto terrain = TerrainPackageClosure::Open(staging / "resources",
                    entry.path().lexically_relative(staging / "resources").generic_string());
                ValidateTerrainShaderIdentity(terrain.store.Production(), cache);
            }
        }

    std::set<std::string> materialSources;
    std::set<std::string> graphicsShaders{"uiOverlay", "offlineDisplay"};
    std::set<std::string> computeShaders;
    for (const auto* name : kRuntimeComputeShaders) computeShaders.insert(name);
    for (const auto& pass : graph.value("passes", Json::array()))
    {
        const std::string type = pass.value("type", std::string());
        if (type == "shadow")
        {
            graphicsShaders.insert("shadow");
            graphicsShaders.insert("shadowLocal");
        }
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
            if (shaderName.has_value() &&
                (shaderName->rfind("particle/", 0) == 0 || shaderName->rfind("decal/", 0) == 0 ||
                    *shaderName == "pass/fogComposite"))
                graphicsShaders.insert(*shaderName);
            if (shaderName.has_value() && graphicsShaders.count(*shaderName) != 0)
            {
                retained.insert(logicalBuildId);
                matchedGraphics.insert(*shaderName);
            }
        }
        else if (kind == "StageEntry")
        {
            const auto shaderPath = ExtractNormalizedField(normalizedKey, "path");
            if (shaderPath.has_value() &&
                (shaderPath->rfind("particle/", 0) == 0 ||
                    *shaderPath == "hair/tressfxSimFollowInterpolation.comp" ||
                    *shaderPath == "pass/volumetricFog.comp" ||
                    *shaderPath == "pass/lightCulling.comp"))
                computeShaders.insert(*shaderPath);
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
                const auto generatedSource = project / "shader/glsl" / relative;
                if (Digest(generatedSource) != expected)
                    throw std::runtime_error("Generated shader source digest mismatch: " + relative);
                copy(generatedSource, fs::path("shader/glsl") / relative);
                copiedSources.emplace(relative, expected);
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
            if (identity.rfind("generate/", 0) == 0 || identity.find("/generate/") != std::string::npos)
                continue;
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

    std::vector<fs::path> shippedDefinitions;
    for (const auto& entry : fs::recursive_directory_iterator(staging / "shader/glsl"))
        if (entry.is_regular_file() && entry.path().extension() == ".json" &&
            entry.path().filename().string().rfind("M_", 0) == 0)
            shippedDefinitions.push_back(entry.path());
    for (const auto& definitionPath : shippedDefinitions)
    {
        const auto definition = ReadJson(definitionPath);
        const auto materialPath = definitionPath.lexically_relative(staging / "shader/glsl");
        const auto generated = (materialPath.parent_path() / "generate" /
            (definition.at("name").get<std::string>() + "Paramter.glsl")).generic_string();
        const auto source = project / "shader/glsl" / generated;
        const auto native = MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent(
            project / "shader/glsl" / materialPath);
        const auto digest = Digest(source);
        if (native.outputPath.lexically_normal() != source.lexically_normal() ||
            ContentHasher::HashString(native.generatedBytes).ToHex() != digest)
            throw std::runtime_error("Stage material include differs from native deterministic generator: " + generated);
        const auto existing = copiedSources.find(generated);
        if (existing != copiedSources.end() && existing->second != digest)
            throw std::runtime_error("Generated material include conflicts with cache identity: " + generated);
        copy(source, fs::path("shader/glsl") / generated);
        copiedSources.emplace(generated, digest);
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
    ValidateRuntimeComputeClosure(staging, cache, closure.manifest);
    return closure;
}

void ValidateRuntimeComputeClosure(const fs::path& root, const Json& cache, const Json& manifest)
{
    for (const auto* required : kRuntimeComputeShaders)
    {
        size_t matched = 0;
        for (const auto& item : cache.at("artifacts").items())
        {
            const auto& artifact = item.value();
            if (artifact.at("kind") != "StageEntry" ||
                ExtractNormalizedField(artifact.at("normalizedKey").get<std::string>(), "path") != required)
                continue;
            ++matched;
            if (!manifest.at("artifacts").contains(item.key()) ||
                manifest.at("artifacts").at(item.key()).at("outputs") != artifact.at("outputs"))
                throw std::runtime_error("Required runtime compute missing from retained closure: " + std::string(required));
            for (const auto* role : {"runtimeCompute", "debugCompute"})
                if (!artifact.at("outputs").contains(role))
                    throw std::runtime_error("Required runtime compute output role missing: " + std::string(required));
            bool primaryFound = false;
            for (const auto& primary : artifact.at("primarySources"))
            {
                const auto name = primary.at("identity").get<std::string>();
                if (name == required) primaryFound = true;
                if (Digest(ResolveContent(root, "shader/glsl/" + name)) != primary.at("digest"))
                    throw std::runtime_error("Runtime compute primary identity mismatch: " + name);
            }
            if (!primaryFound)
                throw std::runtime_error("Runtime compute primary source is not its required entry: " + std::string(required));
            for (const auto& dependency : artifact.at("dependencies"))
            {
                const auto name = dependency.at("path").get<std::string>();
                if (Digest(ResolveContent(root, "shader/glsl/" + name)) != dependency.at("digest"))
                    throw std::runtime_error("Runtime compute include identity mismatch: " + name);
            }
            for (const auto& output : artifact.at("outputs"))
                if (Digest(ResolveContent(root, "shader/" + output.at("path").get<std::string>())) != output.at("digest"))
                    throw std::runtime_error("Runtime compute output identity mismatch: " + std::string(required));
        }
        if (matched != 1)
            throw std::runtime_error("Required runtime compute must have one cached entry: " + std::string(required));
    }
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
    const std::vector<std::string> mingwRuntimeDlls = {
        "libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll"};
    auto isMingwRuntime = [&mingwRuntimeDlls](const std::string& name)
    {
        for (const auto& runtime : mingwRuntimeDlls)
            if (Lower(name) == Lower(runtime)) return true;
        return false;
    };
    for (const auto& dependency : CollectNativeGameDependencies(executable))
    {
        if (isMingwRuntime(dependency.filename().string())) continue;
        files.Copy(dependency, dependency.filename());
    }
    if (options.flavor == GameExportFlavor::Game)
    {
        if (options.mingwRuntimeDirectory.empty())
            throw std::runtime_error(
                "Game export requires --mingw-runtime-dir (or a CMake-built VL_MINGW_RUNTIME_DIR).");
        for (const auto& runtime : mingwRuntimeDlls)
        {
            const auto source = options.mingwRuntimeDirectory / runtime;
            if (!fs::is_regular_file(source))
                throw std::runtime_error("Missing MinGW runtime DLL: " + source.string());
            files.Copy(source, runtime);
        }
    }
        if (!gameFlavor)
        {
            files.Tree(project / "shader/glsl", "shader/glsl");
            if (fs::is_directory(project / "shader/spv")) files.Tree(project / "shader/spv", "shader/spv");
        }
        CopyUiClosure(files, project, config, gameFlavor);
        fs::create_directories(staging / "resources/Maps");
        files.Asset(options.initialScene);
        for (const auto& asset : options.dynamicAssets) files.Asset(asset);
        if (gameFlavor)
        {
            ValidateTerrainFrozenConfig(project);
            files.Copy(project / kTerrainFrozenPath, kTerrainFrozenPath);
            files.Copy(project / kTerrainFrozenShaPath, kTerrainFrozenShaPath);
            if (files.RequiresGlassCrack())
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
        if (gameFlavor) result.manifest["terrainRvtFrozenSha256"] = kTerrainFrozenSha256;
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
    ValidateTerrainFrozenConfig(package);
    if (exportManifest.value("terrainRvtFrozenSha256", std::string()) != kTerrainFrozenSha256)
        throw std::runtime_error("Package receipt lost approved terrain P0 SHA256");
    std::set<std::string> listedFiles;
    for (const auto& file : exportManifest.at("files"))
    {
        const std::string relative = file.at("path").get<std::string>();
        listedFiles.insert(relative);
        const auto path = ResolveContent(package, relative);
        if (fs::file_size(path) != file.at("size").get<uintmax_t>())
            throw std::runtime_error("Package file size mismatch: " + relative);
        if (Digest(path) != file.at("digest").get<std::string>())
            throw std::runtime_error("Package file digest mismatch: " + relative);
    }

    const auto marker = ReadJson(package / kBuildFlavorMarker);
    if (marker.value("editorEnabled", true) || marker.value("flavor", std::string()) != "game")
        throw std::runtime_error("Packaged vl_build_flavor.json is not a game build marker");
    for (const auto* runtime :
        {"libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll"})
        if (!fs::is_regular_file(package / runtime))
            throw std::runtime_error(std::string("Packaged game is missing MinGW runtime DLL: ") + runtime);

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

    const Json shaderCache = ReadJson(cachePath);
    ValidateRuntimeComputeClosure(package, shaderCache, shaderManifest);
    for (const auto& entry : fs::recursive_directory_iterator(package / "shader/glsl"))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
            entry.path().filename().string().rfind("M_", 0) != 0)
            continue;
        const auto definition = ReadJson(entry.path());
        const auto materialPath = entry.path().lexically_relative(package / "shader/glsl");
        const auto generated = (materialPath.parent_path() / "generate" /
            (definition.at("name").get<std::string>() + "Paramter.glsl")).generic_string();
        const auto include = ResolveContent(package, "shader/glsl/" + generated);
        const auto native = MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent(entry.path());
        if (ContentHasher::HashString(native.generatedBytes).ToHex() != Digest(include))
            throw std::runtime_error("Installed material include differs from native generator: " + generated);
        bool recorded = false;
        for (const auto& source : shaderManifest.at("sourceFiles"))
            if (source.at("path") == generated && source.at("digest") == Digest(include))
                recorded = true;
        if (!recorded)
            throw std::runtime_error("Startup material include missing from installed source identity: " + generated);
    }
    std::set<std::string> verifiedTerrainPackages;
    for (const auto& entry : fs::recursive_directory_iterator(package / "resources"))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const Json value = ReadJson(entry.path());
        auto verifyTerrain = [&package, &listedFiles, &shaderCache, &verifiedTerrainPackages](
            const std::string& manifest)
        {
            const auto manifestKey = Lower(fs::canonical(
                TerrainPackageClosure::Resolve(package / "resources", manifest)).generic_string());
            if (verifiedTerrainPackages.count(manifestKey)) return;
            const auto closure = TerrainPackageClosure::Open(package / "resources", manifest);
            ValidateTerrainShaderIdentity(closure.store.Production(), shaderCache);
            auto files = closure.files;
            for (const auto& material : closure.materialInstances)
            {
                ValidateTerrainRuntimeMaterial(package / "resources", package, material);
                files.push_back(TerrainPackageClosure::Resolve(package / "resources", material));
            }
            for (const auto& file : files)
            {
                const auto relative = file.lexically_relative(package).generic_string();
                if (!listedFiles.count(relative))
                    throw std::runtime_error("Terrain VT dependency missing from export manifest: " + relative);
            }
            verifiedTerrainPackages.insert(manifestKey);
        };
        if (value.is_object() && value.contains("kind") && value.at("kind") == "terrain-composited-vt")
            verifyTerrain(entry.path().lexically_relative(package / "resources").generic_string());
        if (value.is_object() && value.value("type", std::string()) == "terrain" &&
            !value.contains("compositedVT"))
            throw std::runtime_error("Packaged terrain requires compositedVT; migrate and Cook before export");
        if (value.is_object() && value.contains("compositedVT"))
        {
            TerrainAssetValidator::Validate(value, entry.path().generic_string());
            for (const auto* field : {"heightmapPath", "materialInstancePath", "detailMaterialInstancePath"})
            {
                if (!value.contains(field)) continue;
                const auto dependency = TerrainPackageClosure::Resolve(
                    package / "resources", value.at(field).get<std::string>());
                if (!listedFiles.count(dependency.lexically_relative(package).generic_string()))
                    throw std::runtime_error("Packaged terrain dependency missing from inventory: " + dependency.string());
            }
            const auto& terrain = value.at("compositedVT");
            if (!terrain.is_object() || !terrain.contains("enabled") ||
                !terrain.at("enabled").is_boolean() || !terrain.at("enabled").get<bool>())
                throw std::runtime_error("Packaged terrain requires explicit compositedVT.enabled=true");
            verifyTerrain(TerrainPackageClosure::Text(terrain.at("packagePath")));
        }
        if (!value.is_object() || !value.contains("material") || !value.at("material").is_string()) continue;
        const std::string material =
            NormalizeShaderGlslRelativePath(value.at("material").get<std::string>());
        if (!fs::is_regular_file(package / "shader/glsl" / material))
            throw std::runtime_error(
                "Shipped material instance references a missing material: " + material);
    }
}
} // namespace VL
