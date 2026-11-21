#include "terrain/terrainCompositedDependencies.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include "core/tracingIfstream.h"
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "resource/image/textureAssetValidation.h"

namespace VL
{
namespace
{
using Json = nlohmann::json;
namespace fs = std::filesystem;

[[noreturn]] void Fail(const std::string& path, const std::string& reason)
{
    throw std::runtime_error("Terrain composited dependency [" + path + "]: " + reason);
}

std::string RelativePath(std::string name)
{
    const auto original = name;
    std::replace(name.begin(), name.end(), '\\', '/');
    const fs::path path(name);
    if (name.empty() || name.find('\0') != std::string::npos ||
        name.find(':') != std::string::npos || path.is_absolute() ||
        path.has_root_name() || path.has_root_directory())
        Fail(original, "expected root-relative path");
    const auto normalized = path.lexically_normal();
    for (const auto& part : normalized)
        if (part == "..")
            Fail(original, "path escapes root");
    if (normalized == "." || normalized.filename().empty())
        Fail(original, "expected file path");
    return normalized.generic_string();
}

bool ShaderPath(const std::string& path)
{
    return path.rfind("shader/glsl/", 0) == 0;
}

struct ShaderInclude
{
    std::string path;
    bool standard = false;
};

std::vector<ShaderInclude> ShaderIncludes(const std::string& bytes, const std::string& path)
{
    std::string joined;
    for (size_t index = 0; index < bytes.size(); ++index)
    {
        if (bytes[index] == '\\' && index + 1 < bytes.size())
        {
            if (bytes[index + 1] == '\n')
            {
                ++index;
                continue;
            }
            if (bytes[index + 1] == '\r' && index + 2 < bytes.size() && bytes[index + 2] == '\n')
            {
                index += 2;
                continue;
            }
        }
        joined += bytes[index];
    }
    std::string clean;
    bool block = false;
    bool line = false;
    bool quoted = false;
    for (size_t index = 0; index < joined.size(); ++index)
    {
        const char character = joined[index];
        const char next = index + 1 < joined.size() ? joined[index + 1] : '\0';
        if (line || block)
        {
            if (character == '\n')
            {
                clean += '\n';
                line = false;
            }
            else if (block && character == '*' && next == '/')
            {
                block = false;
                ++index;
            }
            else
                clean += ' ';
            continue;
        }
        if (!quoted && character == '/' && (next == '/' || next == '*'))
        {
            line = next == '/';
            block = next == '*';
            clean += ' ';
            ++index;
            continue;
        }
        clean += character;
        if (quoted && character == '\\' && next != '\0')
        {
            clean += next;
            ++index;
        }
        else if (character == '"')
            quoted = !quoted;
    }
    std::vector<ShaderInclude> includes;
    std::istringstream lines(clean);
    std::string text;
    while (std::getline(lines, text))
    {
        size_t offset = 0;
        const auto whitespace = [&] {
            while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])))
                ++offset;
        };
        whitespace();
        if (offset == text.size() || text[offset++] != '#')
            continue;
        whitespace();
        const size_t begin = offset;
        while (offset < text.size() &&
            (std::isalnum(static_cast<unsigned char>(text[offset])) || text[offset] == '_'))
            ++offset;
        if (text.substr(begin, offset - begin) != "include")
            continue;
        whitespace();
        if (offset == text.size() || (text[offset] != '"' && text[offset] != '<'))
            Fail(path, "dependency collection requires a literal GLSL include operand");
        const bool standard = text[offset++] == '<';
        const char closing = standard ? '>' : '"';
        const size_t end = text.find(closing, offset);
        if (end == std::string::npos || end == offset)
            Fail(path, "malformed GLSL include");
        includes.push_back({text.substr(offset, end - offset), standard});
        offset = end + 1;
        whitespace();
        if (offset != text.size())
            Fail(path, "unexpected tokens after GLSL include");
    }
    return includes;
}

fs::path Resolve(const fs::path& root, const std::string& name)
{
    try
    {
        if (root.empty())
            Fail(name, "required dependency root was not provided");
        const auto canonicalRoot = fs::canonical(root);
        const auto target = fs::weakly_canonical(canonicalRoot / RelativePath(name));
        const auto relative = target.lexically_relative(canonicalRoot);
        if (relative.empty() || relative.is_absolute())
            Fail(name, "canonical path escapes root");
        for (const auto& part : relative)
            if (part == "..")
                Fail(name, "canonical path escapes root");
        return target;
    }
    catch (const fs::filesystem_error& error)
    {
        Fail(name, error.what());
    }
}

std::string ReadBytes(const fs::path& path)
{
    VL::RuntimeFileTrace::TracingIfstream file(
        path, std::ios::binary, "TerrainCompositedDependencies.ReadBytes");
    if (!file)
        Fail(path.generic_string(), "cannot open required file");
    std::ostringstream bytes;
    bytes << file.rdbuf();
    if (file.bad())
        Fail(path.generic_string(), "failed to read required file");
    return bytes.str();
}

Json Parse(const std::string& bytes, const std::string& path)
{
    try
    {
        auto document = Json::parse(bytes);
        if (!document.is_object())
            Fail(path, "expected JSON object");
        return document;
    }
    catch (const Json::exception& error)
    {
        Fail(path, error.what());
    }
}

Json WithoutPackagePath(Json document)
{
    if (document.contains("compositedVT") && document.at("compositedVT").is_object())
        document["compositedVT"].erase("packagePath");
    return document;
}

void FiniteJson(const Json& value)
{
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        Fail("configuration", "non-finite number");
    if (value.is_discarded() || value.is_binary())
        Fail("configuration", "unsupported JSON value");
    if (value.is_structured())
        for (const auto& child : value)
            FiniteJson(child);
}

bool RetainedTerrainParameter(const std::string& name)
{
    static const std::set<std::string> names{
        "u_compositedEnabled", "u_compositedResolution", "u_compositedAtlasTiles",
        "u_compositedMaxMip", "u_compositedRepeat", "u_compositedNormalRG",
        "u_terrainHeightAtlasEnabled", "u_pageTableSize", "u_pageResolution",
        "u_debugMode", "u_layerDisplacementScale",
        "u_detailUvScale", "u_detailFadeStartM", "u_detailFadeEndM", "u_detailStrength",
        "u_terrainTessRadiusM", "u_terrainTessMax", "u_terrainTessTargetPixels",
        "u_macroUvScale", "u_macroStrength", "u_globalOriginCm", "u_globalSizeCm",
        "u_globalColourWeight", "u_globalColourWeightNear", "u_globalFadeStartM",
        "u_globalFadeEndM", "u_globalAoStrength", "u_globalWetnessStrength"};
    return names.count(name) != 0;
}

bool RetainedTerrainTexture(const std::string& name)
{
    return name == "globalColour" || name == "globalMasks";
}

void RequireNumericParameter(const Json& value, const std::string& path)
{
    FiniteJson(value);
    if (value.is_number())
        return;
    if (value.is_array() && !value.empty() && value.size() <= 4 &&
        std::all_of(value.begin(), value.end(), [](const Json& component) { return component.is_number(); }))
        return;
    Fail(path, "retained terrain parameter must be numeric");
}

Json TerrainStaticMaterial(Json document, bool instance, const std::string& path)
{
    if (document.value("type", std::string()) != (instance ? "materialInstance" : "material"))
        Fail(path, "static terrain digest mode disagrees with asset type");
    if (document.contains("parameters"))
    {
        auto& parameters = document["parameters"];
        if (!parameters.is_object())
            Fail(path, "material parameters must be an object");
        for (auto parameter = parameters.begin(); parameter != parameters.end();)
        {
            if (!RetainedTerrainParameter(parameter.key()))
            {
                ++parameter;
                continue;
            }
            if (instance)
            {
                RequireNumericParameter(parameter.value(), path + ":" + parameter.key());
                parameter = parameters.erase(parameter);
            }
            else
            {
                RequireNumericParameter(parameter.value().at("default"), path + ":" + parameter.key());
                parameter.value().erase("default");
                ++parameter;
            }
        }
        if (instance && parameters.empty())
            document.erase("parameters");
    }
    if (document.contains("textures"))
    {
        auto& textures = document["textures"];
        if (!textures.is_object())
            Fail(path, "material textures must be an object");
        for (const char* name : {"globalColour", "globalMasks"})
            if (textures.contains(name))
            {
                if (instance)
                    textures.erase(name);
                else
                    textures[name].erase("default");
            }
        if (instance && textures.empty())
            document.erase("textures");
    }
    return document;
}

Json Configuration(
    const TerrainBuildPlan& plan, const std::array<float, 16>& placement,
    const std::string& materialInstancePath, const Json& buildConfig)
{
    const auto& desc = plan.desc;
    Json layers = Json::array();
    for (const auto& layer : desc.layers)
        layers.push_back({
            {"name", layer.name}, {"key", layer.key},
            {"heightRange", {layer.heightMin, layer.heightMax}},
            {"slopeRange", {layer.slopeMin, layer.slopeMax}},
            {"noise", layer.noise}, {"uvScale", layer.uvScale},
            {"uvRotation", layer.uvRotation}, {"sourceResolution", layer.sourceResolution},
            {"avgColor", {layer.avgColor.x(), layer.avgColor.y(), layer.avgColor.z()}},
            {"sourceSet", layer.sourceSet}, {"flags", layer.flags},
            {"displacementM", layer.displacementM}, {"hasHeight", layer.hasHeight}});
    for (const float component : placement)
        if (!std::isfinite(component))
            Fail("placement", "non-finite matrix component");
    const auto optionalPath = [](const std::string& path)
    {
        return path.empty() ? std::string() : RelativePath(path);
    };
    if (!buildConfig.is_object())
        Fail("buildConfig", "expected JSON object");
    Json config = {
        {"terrain", RelativePath(desc.assetPath)},
        {"placementId", TerrainPlacementIdentity(plan)},
        {"placement", placement},
        {"heightmapPath", RelativePath(desc.heightmapPath)},
        {"paintPath", optionalPath(desc.paintPath)},
        {"materialInstancePath", RelativePath(materialInstancePath)},
        {"baseMaterialInstancePath", optionalPath(desc.materialInstancePath)},
        {"detailMaterialInstancePath", optionalPath(desc.detailMaterialInstancePath)},
        {"size", {desc.size.x(), desc.size.y()}},
        {"heightScale", desc.heightScale}, {"heightOffset", desc.heightOffset},
        {"gridResolution", desc.gridResolution}, {"chunkQuads", desc.chunkQuads},
        {"heightmapMargin", desc.heightmapMargin},
        {"pageOrigin", {desc.pageOrigin.x(), desc.pageOrigin.y()}},
        {"pageTilesPerSide", desc.pageTilesPerSide},
        {"lodLevels", desc.lodLevels}, {"lodDistances", desc.lodDistancesCm},
        {"chunkResidencyDistances", desc.chunkResidencyDistancesCm},
        {"layers", std::move(layers)},
        {"compositedVT", {
            {"enabled", desc.compositedVT.enabled},
            {"virtualResolution", {desc.compositedVT.virtualResolution.x(),
                desc.compositedVT.virtualResolution.y()}}}},
        {"buildConfig", WithoutPackagePath(buildConfig)}};
    FiniteJson(config);
    return config;
}

ContentDigest EntryDigest(const std::string& bytes, const std::string& mode,
    const std::string& path)
{
    if (mode == "bytes")
        return ContentHasher::HashString(bytes);
    if (mode == "terrain-json-without-package-path-v1")
        return ContentHasher::HashString(WithoutPackagePath(Parse(bytes, path)).dump());
    if (mode == "terrain-material-instance-static-json-v2" || mode == "terrain-material-static-json-v2")
        return ContentHasher::HashString(TerrainStaticMaterial(Parse(bytes, path),
            mode == "terrain-material-instance-static-json-v2", path).dump());
    Fail(path, "unsupported digest mode: " + mode);
}

ContentDigest ManifestHash(const Json& manifest)
{
    CanonicalFieldHasher hash("terrain-composited-dependencies-v1");
    hash.AddString("config", manifest.at("config").dump());
    hash.AddUInt64("dependencyCount", manifest.at("dependencies").size());
    for (const auto& entry : manifest.at("dependencies"))
    {
        hash.AddString("root", entry.at("root").get<std::string>());
        hash.AddString("path", entry.at("path").get<std::string>());
        hash.AddString("mode", entry.at("mode").get<std::string>());
        hash.AddDigest("digest", ContentDigest::FromHex(entry.at("digest").get<std::string>()));
    }
    return hash.Finalize();
}

void ValidateManifest(const Json& manifest)
{
    try
    {
        if (!manifest.is_object() || manifest.size() != 5 ||
            manifest.at("kind") != "terrain-composited-dependencies" ||
            manifest.at("version") != 1 || !manifest.at("config").is_object() ||
            !manifest.at("dependencies").is_array() ||
            manifest.at("dependencies").empty())
            Fail("manifest", "unsupported dependency manifest");
        FiniteJson(manifest.at("config"));
        std::string previous;
        for (const auto& entry : manifest.at("dependencies"))
        {
            if (!entry.is_object() || entry.size() != 4)
                Fail("manifest", "invalid dependency record");
            const auto scope = entry.at("root").get<std::string>();
            const auto path = entry.at("path").get<std::string>();
            const auto mode = entry.at("mode").get<std::string>();
            if ((scope != "resource" && scope != "project") ||
                RelativePath(path) != path || (scope == "project" && !ShaderPath(path)))
                Fail(path, "invalid dependency root or noncanonical path");
            const auto key = scope + ":" + path;
            if (!previous.empty() && key <= previous)
                Fail(path, "dependencies must be sorted and unique");
            previous = key;
            const auto digest = entry.at("digest").get<std::string>();
            if (ContentDigest::FromHex(digest).ToHex() != digest)
                Fail(path, "noncanonical digest");
            if (mode != "bytes" && mode != "absent" &&
                mode != "terrain-json-without-package-path-v1" &&
                mode != "terrain-material-instance-static-json-v2" &&
                mode != "terrain-material-static-json-v2")
                Fail(path, "unsupported dependency digest mode");
            if (mode == "absent" &&
                digest != ContentHasher::HashString("terrain-optional-absent-v1").ToHex())
                Fail(path, "invalid absent dependency digest");
        }
        const auto expected = manifest.at("hash").get<std::string>();
        if (ManifestHash(manifest).ToHex() != expected)
            Fail("manifest", "dependency manifest hash mismatch");
    }
    catch (const Json::exception& error)
    {
        Fail("manifest", error.what());
    }
}

class Collector
{
public:
    explicit Collector(const TerrainCompositedDependencyRoots& roots) : roots(roots) {}

    Json Terrain(const std::string& name)
    {
        auto document = Document(name, "resource", "terrain-json-without-package-path-v1");
        if (document.contains("compositedVT") &&
            document.at("compositedVT").contains("packagePath"))
        {
            const auto path = document.at("compositedVT").at("packagePath").get<std::string>();
            if (!path.empty())
                excluded.insert(RelativePath(path));
        }
        return document;
    }

    void File(const std::string& name, bool optional = false)
    {
        const auto relative = RelativePath(name);
        if (excluded.count(relative))
            Fail(relative, "Cook output must not be an author input");
        const auto path = Resolve(roots.resourceRoot, relative);
        if (optional && !fs::exists(path))
        {
            Add(relative, "resource", "absent",
                ContentHasher::HashString("terrain-optional-absent-v1"));
            return;
        }
        RequireFile(path);
        Add(relative, "resource", "bytes", ContentHasher::HashFile(path));
    }

    bool Material(const std::string& name, uint32_t depth = 0)
    {
        const auto relative = RelativePath(name);
        if (depth > 64 || !active.insert(relative).second)
            Fail(relative, "material reference cycle or depth exceeds 64");
        std::string scope = "resource";
        const auto resource = Resolve(roots.resourceRoot, relative);
        if (!fs::exists(resource) && ShaderPath(relative))
        {
            Add(relative, "resource", "absent",
                ContentHasher::HashString("terrain-optional-absent-v1"));
            scope = "project";
        }
        const auto path = Resolve(scope == "resource" ? roots.resourceRoot : roots.projectRoot, relative);
        RequireFile(path);
        const auto bytes = ReadBytes(path);
        auto document = Parse(bytes, relative);
        const auto type = document.value("type", std::string());
        bool terrainMaterial = type == "material" && ShaderPath(relative) &&
            fs::path(relative).filename() == "M_terrain.json";
        if (type == "materialInstance")
            terrainMaterial = Material(document.at("material").get<std::string>(), depth + 1);
        else if (type != "material")
            Fail(relative, "expected materialInstance or material");
        const std::string mode = terrainMaterial ?
            (type == "materialInstance" ? "terrain-material-instance-static-json-v2" :
                "terrain-material-static-json-v2") : "bytes";
        Add(relative, scope, mode, EntryDigest(bytes, mode, relative));
        if (document.contains("textures"))
            for (const auto& texture : document.at("textures").items())
            {
                if (terrainMaterial && RetainedTerrainTexture(texture.key()))
                    continue;
                const auto& reference = type == "material" ?
                    texture.value().at("default") : texture.value();
                if (!reference.is_null())
                    Texture(reference.get<std::string>());
            }
        if (type == "material" && document.contains("shaderEvaluation"))
            for (const char* field : {"vertex", "surface"})
            {
                const auto shader = RelativePath("shader/glsl/" +
                    document.at("shaderEvaluation").at(field).get<std::string>());
                if (!ShaderPath(shader))
                    Fail(shader, "shaderEvaluation escapes shader/glsl");
                Shader(shader, scope, 0);
            }
        active.erase(relative);
        return terrainMaterial;
    }

    Json Entries() const
    {
        Json entries = Json::array();
        for (const auto& entry : collected)
            entries.push_back(entry.second);
        return entries;
    }

private:
    void Shader(const std::string& name, const std::string& scope, uint32_t depth)
    {
        const auto relative = RelativePath(name);
        if (!ShaderPath(relative) || depth > 128)
            Fail(relative, "GLSL include escapes shader/glsl or exceeds depth 128");
        if (!shaders.insert(scope + ":" + relative).second)
            return;
        const auto& root = scope == "resource" ? roots.resourceRoot : roots.projectRoot;
        const auto path = Resolve(root, relative);
        const auto shaderRoot = fs::canonical(root / "shader/glsl");
        const auto physicalRelative = path.lexically_relative(shaderRoot);
        if (physicalRelative.empty() || physicalRelative.is_absolute())
            Fail(relative, "canonical GLSL include escapes shader/glsl");
        for (const auto& component : physicalRelative)
            if (component == "..")
                Fail(relative, "canonical GLSL include escapes shader/glsl");
        RequireFile(path);
        const auto bytes = ReadBytes(path);
        Add(relative, scope, "bytes", ContentHasher::HashString(bytes));
        for (const auto& include : ShaderIncludes(bytes, relative))
        {
            const fs::path operand(include.path);
            RelativePath(include.standard ? include.path :
                (fs::path(relative).parent_path() / operand).generic_string());
            if (operand.is_absolute() || operand.has_root_name() || operand.has_root_directory() ||
                include.path.find(':') != std::string::npos)
                Fail(relative, "GLSL include must be relative: " + include.path);
            const auto dependency = include.standard ? fs::path("shader/glsl") / operand :
                fs::path(relative).parent_path() / operand;
            Shader(dependency.generic_string(), scope, depth + 1);
        }
    }

    static void RequireFile(const fs::path& path)
    {
        if (!fs::is_regular_file(path))
            Fail(path.generic_string(), "missing required regular file");
    }

    void Add(const std::string& path, const std::string& scope,
        const std::string& mode, const ContentDigest& digest)
    {
        Json entry = {{"root", scope}, {"path", path}, {"mode", mode},
            {"digest", digest.ToHex()}};
        const auto key = scope + ":" + path;
        const auto found = collected.find(key);
        if (found != collected.end() && found->second != entry)
            Fail(path, "dependency changed while capturing manifest");
        collected[key] = std::move(entry);
    }

    Json Document(const std::string& name, const std::string& scope, const std::string& mode)
    {
        const auto relative = RelativePath(name);
        if (scope == "resource" && excluded.count(relative))
            Fail(relative, "Cook output must not be an author input");
        const auto path = Resolve(scope == "resource" ? roots.resourceRoot : roots.projectRoot, relative);
        RequireFile(path);
        const auto bytes = ReadBytes(path);
        auto document = Parse(bytes, relative);
        Add(relative, scope, mode, EntryDigest(bytes, mode, relative));
        return document;
    }

    void Texture(const std::string& name)
    {
        const auto relative = RelativePath(name);
        if (!textures.insert(relative).second)
            return;
        auto document = Document(relative, "resource", "bytes");
        try
        {
            ValidateTextureAssetJson(document);
            if (document.at("type") == "texture")
                File(document.at("source").get<std::string>());
            else
            {
                for (const auto& source : document.at("sources"))
                    File(source.get<std::string>());
                if (document.contains("displacementSources"))
                    for (const auto& source : document.at("displacementSources"))
                        File(source.get<std::string>());
            }
        }
        catch (const std::exception& error)
        {
            Fail(relative, error.what());
        }
    }

    const TerrainCompositedDependencyRoots& roots;
    std::map<std::string, Json> collected;
    std::set<std::string> excluded;
    std::set<std::string> active;
    std::set<std::string> textures;
    std::set<std::string> shaders;
};
}

Json BuildTerrainCompositedDependencies(
    const TerrainCompositedDependencyRoots& roots, const TerrainBuildPlan& plan,
    const std::array<float, 16>& placement, const std::string& materialInstancePath,
    const Json& buildConfig)
{
    try
    {
        auto config = Configuration(plan, placement, materialInstancePath, buildConfig);
        Collector collector(roots);
        const auto terrain = collector.Terrain(plan.desc.assetPath);
        collector.File(terrain.at("heightmapPath").get<std::string>());
        if (terrain.contains("paintPath") && !terrain.at("paintPath").get<std::string>().empty())
        {
            const auto paint = terrain.at("paintPath").get<std::string>();
            collector.File(paint);
            collector.File(paint + ".meta.json", true);
        }
        collector.File(plan.desc.heightmapPath);
        if (!plan.desc.paintPath.empty())
        {
            collector.File(plan.desc.paintPath);
            collector.File(plan.desc.paintPath + ".meta.json", true);
        }
        std::set<std::string> materials{materialInstancePath};
        for (const char* field : {"materialInstancePath", "detailMaterialInstancePath"})
            if (terrain.contains(field) && !terrain.at(field).get<std::string>().empty())
                materials.insert(terrain.at(field).get<std::string>());
        for (const auto& path : {plan.desc.materialInstancePath, plan.desc.detailMaterialInstancePath})
            if (!path.empty())
                materials.insert(path);
        for (const auto& path : materials)
            collector.Material(path);
        Json manifest = {{"kind", "terrain-composited-dependencies"}, {"version", 1},
            {"config", std::move(config)}, {"dependencies", collector.Entries()}};
        manifest["hash"] = ManifestHash(manifest).ToHex();
        return manifest;
    }
    catch (const Json::exception& error)
    {
        Fail(plan.desc.assetPath, error.what());
    }
}

void ValidateTerrainCompositedAuthors(
    const TerrainCompositedDependencyRoots& roots, const Json& manifest)
{
    ValidateManifest(manifest);
    for (const auto& entry : manifest.at("dependencies"))
    {
        const auto scope = entry.at("root").get<std::string>();
        const auto name = entry.at("path").get<std::string>();
        const auto path = Resolve(scope == "resource" ? roots.resourceRoot : roots.projectRoot, name);
        const auto mode = entry.at("mode").get<std::string>();
        if (mode == "absent")
        {
            if (fs::exists(path))
                Fail(name, "previously absent optional dependency now exists; Cook is stale");
            continue;
        }
        if (!fs::is_regular_file(path))
            Fail(name, "required author dependency is missing");
        const auto digest = mode == "bytes" ? ContentHasher::HashFile(path) :
            EntryDigest(ReadBytes(path), mode, name);
        if (digest.ToHex() != entry.at("digest").get<std::string>())
            Fail(name, "author digest changed; Cook is stale");
    }
}

void ValidateTerrainCompositedManifestConfig(
    const Json& manifest, const TerrainBuildPlan& plan,
    const std::array<float, 16>& placement, const std::string& materialInstancePath,
    const Json& buildConfig)
{
    ValidateManifest(manifest);
    if (manifest.at("config") != Configuration(plan, placement, materialInstancePath, buildConfig))
        Fail(plan.desc.assetPath, "manifest build configuration differs; Cook is stale");
}

ContentDigest TerrainCompositedDependencyHash(const Json& manifest)
{
    ValidateManifest(manifest);
    return ManifestHash(manifest);
}

ContentDigest TerrainCompositedDependencyEntryHash(
    std::string_view bytes, const std::string& mode, const std::string& logicalPath)
{
    return EntryDigest(std::string(bytes), mode, logicalPath);
}
}
