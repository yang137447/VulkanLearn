#include "engine/testing/runtimeTestFixtures.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/diagnosticsSubsystem.h"
#include "engine/testing/runtimeValidationServices.h"
#include "material.h"
#include "materialInstance.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "shader/build/atomicFile.h"
#include "world/worldManager.h"

namespace VL
{
namespace RuntimeTestFixtures
{

constexpr const char* ShaderReloadTestShaderName =
    "runtimeTest/shaderReloadTest";
std::string FormatBackendIdentityCounts(
    const std::array<size_t, 9>& counts)
{
    return "buffers=" + std::to_string(counts[0]) +
        ", images=" + std::to_string(counts[1]) +
        ", imageViews=" + std::to_string(counts[2]) +
        ", samplers=" + std::to_string(counts[3]) +
        ", descriptorLayouts=" + std::to_string(counts[4]) +
        ", descriptorPools=" + std::to_string(counts[5]) +
        ", descriptorSets=" + std::to_string(counts[6]) +
        ", renderPasses=" + std::to_string(counts[7]) +
        ", framebuffers=" + std::to_string(counts[8]);
}


std::string FormatImageResourceDebugNameDifference(
    const std::vector<std::string>& baseline,
    const std::vector<std::string>& current)
{
    std::vector<std::string> removed;
    std::vector<std::string> added;
    std::set_difference(
        baseline.begin(),
        baseline.end(),
        current.begin(),
        current.end(),
        std::back_inserter(removed));
    std::set_difference(
        current.begin(),
        current.end(),
        baseline.begin(),
        baseline.end(),
        std::back_inserter(added));

    std::ostringstream stream;
    stream << "removed=[";
    for (size_t index = 0; index < removed.size(); ++index)
    {
        if (index > 0)
        {
            stream << ", ";
        }
        stream << removed[index];
    }
    stream << "], added=[";
    for (size_t index = 0; index < added.size(); ++index)
    {
        if (index > 0)
        {
            stream << ", ";
        }
        stream << added[index];
    }
    stream << "]";
    return stream.str();
}


bool SameWorldHandle(const WorldHandle& lhs, const WorldHandle& rhs)
{
    return lhs.IsValid() &&
        rhs.IsValid() &&
        lhs.generation == rhs.generation &&
        lhs.scenePath == rhs.scenePath;
}

bool SameRendererResourceFingerprint(
    const RuntimeRendererResourceFingerprint& lhs,
    const RuntimeRendererResourceFingerprint& rhs)
{
    return lhs.captured &&
        rhs.captured &&
        lhs.worldOwnerGeneration == rhs.worldOwnerGeneration &&
        lhs.worldTextures == rhs.worldTextures &&
        lhs.renderableObjects == rhs.renderableObjects &&
        lhs.materials == rhs.materials &&
        lhs.materialInstances == rhs.materialInstances &&
        lhs.objectResources == rhs.objectResources &&
        lhs.textures == rhs.textures &&
        lhs.passMaterialInstances == rhs.passMaterialInstances;
}

bool SameRendererResourceFingerprintExceptWorldTexture(
    const RuntimeRendererResourceFingerprint& lhs,
    const RuntimeRendererResourceFingerprint& rhs,
    const std::string& replaceableTextureName)
{
    RuntimeRendererResourceFingerprint lhsStable = lhs;
    RuntimeRendererResourceFingerprint rhsStable = rhs;
    lhsStable.worldTextures.erase(replaceableTextureName);
    rhsStable.worldTextures.erase(replaceableTextureName);
    return SameRendererResourceFingerprint(
        lhsStable,
        rhsStable);
}

std::string DescribeRendererResourceFingerprintDifference(
    const RuntimeRendererResourceFingerprint& lhs,
    const RuntimeRendererResourceFingerprint& rhs)
{
    std::ostringstream stream;
    if (lhs.worldOwnerGeneration != rhs.worldOwnerGeneration)
    {
        stream << " ownerGeneration=" << lhs.worldOwnerGeneration <<
            "->" << rhs.worldOwnerGeneration;
    }
    if (lhs.worldTextures != rhs.worldTextures)
    {
        stream << " worldTextures{";
        std::set<std::string> textureNames;
        for (const auto& [name, identity] : lhs.worldTextures)
        {
            (void)identity;
            textureNames.insert(name);
        }
        for (const auto& [name, identity] : rhs.worldTextures)
        {
            (void)identity;
            textureNames.insert(name);
        }
        bool firstDifference = true;
        for (const std::string& name : textureNames)
        {
            const auto lhsIt = lhs.worldTextures.find(name);
            const auto rhsIt = rhs.worldTextures.find(name);
            const std::uintptr_t lhsIdentity =
                lhsIt != lhs.worldTextures.end()
                ? lhsIt->second
                : 0;
            const std::uintptr_t rhsIdentity =
                rhsIt != rhs.worldTextures.end()
                ? rhsIt->second
                : 0;
            if (lhsIdentity == rhsIdentity)
            {
                continue;
            }
            if (!firstDifference)
            {
                stream << ",";
            }
            stream << name << "=" << lhsIdentity <<
                "->" << rhsIdentity;
            firstDifference = false;
        }
        stream << "}";
    }
    if (lhs.renderableObjects != rhs.renderableObjects)
    {
        stream << " renderableObjects";
    }
    if (lhs.materials != rhs.materials)
    {
        stream << " materials";
    }
    if (lhs.materialInstances != rhs.materialInstances)
    {
        stream << " materialInstances";
    }
    if (lhs.objectResources != rhs.objectResources)
    {
        stream << " objectResources";
    }
    if (lhs.textures != rhs.textures)
    {
        stream << " textures";
    }
    if (lhs.passMaterialInstances != rhs.passMaterialInstances)
    {
        stream << " passMaterialInstances";
    }
    return stream.str();
}

std::string FormatRendererResourceFingerprint(
    const RuntimeRendererResourceFingerprint& fingerprint)
{
    if (!fingerprint.captured)
    {
        return "not captured";
    }

    return "ownerGeneration=" +
        std::to_string(fingerprint.worldOwnerGeneration) +
        ", worldTextures=" +
        std::to_string(fingerprint.worldTextures.size()) +
        ", renderableObjects=" +
        std::to_string(fingerprint.renderableObjects.size()) +
        ", materials=" +
        std::to_string(fingerprint.materials.size()) +
        ", materialInstances=" +
        std::to_string(fingerprint.materialInstances.size()) +
        ", objectResources=" +
        std::to_string(fingerprint.objectResources.size()) +
        ", textures=" +
        std::to_string(fingerprint.textures.size()) +
        ", passMaterialInstances=" +
        std::to_string(fingerprint.passMaterialInstances.size());
}


std::filesystem::path BuildGeneratedFixtureDirectory(
    const std::string& resourcePath,
    const std::string& namePrefix)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::path(resourcePath) /
        "generated" /
        "runtime-validation" /
        (namePrefix + std::to_string(now));
}

void WriteTextFile(
    const std::filesystem::path& path,
    const std::string& contents)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to write generated runtime test fixture: " + path.string());
    }

    file << contents;
}

std::string ReadTextFileBytes(const std::filesystem::path& path)
{
    const std::vector<uint8_t> bytes = ReadBinaryFile(path);
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
}


ShaderReloadRuntimeSnapshot CaptureShaderReloadRuntimeSnapshot(
    RuntimeValidationServices& validationServices)
{
    const RuntimeValidationGraphicsShaderSnapshot captured =
        validationServices.CaptureGraphicsShaderSnapshot();
    ShaderReloadRuntimeSnapshot snapshot;
    snapshot.material =
        reinterpret_cast<std::uintptr_t>(
            captured.material.lock().get());
    snapshot.surfacePipeline = captured.surfacePipeline;
    snapshot.shadowPipeline = captured.shadowPipeline;
    snapshot.surfaceGeneration = captured.surfaceGeneration;
    snapshot.shadowGeneration = captured.shadowGeneration;
    snapshot.surfaceVertexDigest =
        captured.surfaceVertexDigest;
    snapshot.surfaceFragmentDigest =
        captured.surfaceFragmentDigest;
    snapshot.shadowVertexDigest =
        captured.shadowVertexDigest;
    snapshot.shadowFragmentDigest =
        captured.shadowFragmentDigest;
    snapshot.manifestDigest = captured.manifestDigest;
    return snapshot;
}

bool SameShaderReloadRuntimeSnapshot(
    const ShaderReloadRuntimeSnapshot& lhs,
    const ShaderReloadRuntimeSnapshot& rhs)
{
    return lhs.material == rhs.material &&
        lhs.surfacePipeline == rhs.surfacePipeline &&
        lhs.shadowPipeline == rhs.shadowPipeline &&
        lhs.surfaceGeneration == rhs.surfaceGeneration &&
        lhs.shadowGeneration == rhs.shadowGeneration &&
        lhs.manifestDigest == rhs.manifestDigest &&
        lhs.surfaceVertexDigest == rhs.surfaceVertexDigest &&
        lhs.surfaceFragmentDigest == rhs.surfaceFragmentDigest &&
        lhs.shadowVertexDigest == rhs.shadowVertexDigest &&
        lhs.shadowFragmentDigest == rhs.shadowFragmentDigest;
}


ShaderAutoReloadRuntimeSnapshot
CaptureShaderAutoReloadRuntimeSnapshot(
    RuntimeValidationServices& validationServices)
{
    const RuntimeValidationGraphicsShaderSnapshot captured =
        validationServices.CaptureGraphicsShaderSnapshot();
    ShaderAutoReloadRuntimeSnapshot snapshot;
    snapshot.surfaceLogicalBuildId =
        captured.surfaceLogicalBuildId;
    snapshot.shadowLogicalBuildId =
        captured.shadowLogicalBuildId;
    snapshot.surfaceGeneration = captured.surfaceGeneration;
    snapshot.shadowGeneration = captured.shadowGeneration;
    snapshot.manifestDigest = captured.manifestDigest;
    snapshot.resolvedGeneration = captured.resolvedGeneration;
    return snapshot;
}

bool SameShaderAutoReloadRuntimeSnapshot(
    const ShaderAutoReloadRuntimeSnapshot& lhs,
    const ShaderAutoReloadRuntimeSnapshot& rhs)
{
    return lhs.surfaceGeneration == rhs.surfaceGeneration &&
        lhs.shadowGeneration == rhs.shadowGeneration &&
        lhs.manifestDigest == rhs.manifestDigest &&
        lhs.resolvedGeneration == rhs.resolvedGeneration;
}

bool ContainsSourceIdentity(
    const std::vector<std::string>& sources,
    const std::string& identity)
{
    return std::find(sources.begin(), sources.end(), identity) !=
        sources.end();
}

bool ContainsAllSourceIdentities(
    const std::vector<std::string>& sources,
    const std::vector<std::string>& expected)
{
    for (const std::string& identity : expected)
    {
        if (!ContainsSourceIdentity(sources, identity))
        {
            return false;
        }
    }
    return true;
}


std::filesystem::path CreateShaderReloadTestScene(
    const std::string& resourcePath)
{
    const std::filesystem::path fixtureDirectory =
        BuildGeneratedFixtureDirectory(
            resourcePath,
            "shader-reload-");
    std::filesystem::create_directories(
        fixtureDirectory);

    const std::filesystem::path materialInstancePath =
        fixtureDirectory /
        "MI_shader_reload_runtime_test.json";
    const std::filesystem::path batchMaterialInstancePath =
        fixtureDirectory /
        "MI_shader_reload_batch_runtime_test.json";
    const std::filesystem::path meshAssetPath =
        fixtureDirectory /
        "SM_shader_reload_runtime_test.json";
    const std::filesystem::path batchMeshAssetPath =
        fixtureDirectory /
        "SM_shader_reload_batch_runtime_test.json";
    const std::filesystem::path scenePath =
        fixtureDirectory /
        "SC_shader_reload_runtime_test.json";

    WriteTextFile(
        materialInstancePath,
        R"({
  "name": "Shader Reload Runtime Test Material Instance",
  "type": "materialInstance",
  "material": "shader/glsl/runtimeTest/M_shaderReloadTest.json",
  "parameters": {
    "u_reloadTestColor": [0.3, 0.6, 0.95, 0.9],
    "u_alphaClipThreshold": 0.3,
    "u_reloadRuntimeScalar": 0.25
  }
})");

    WriteTextFile(
        batchMaterialInstancePath,
        R"({
  "name": "Shader Reload Batch Runtime Test Material Instance",
  "type": "materialInstance",
  "material": "shader/glsl/runtimeTest/M_shaderReloadBatchTest.json",
  "parameters": {
    "u_reloadBatchColor": [0.25, 0.8, 0.4, 1.0]
  }
})");

    WriteTextFile(
        meshAssetPath,
        "{\n"
        "  \"name\": \"Shader Reload Runtime Test Mesh\",\n"
        "  \"type\": \"mesh\",\n"
        "  \"modelDataPath\": \"models/datas/axis.obj\",\n"
        "  \"materialSlots\": [\n"
        "    {\n"
        "      \"name\": \"Default\",\n"
        "      \"materialInstancePath\": \"" +
            materialInstancePath.generic_string() +
            "\"\n"
        "    }\n"
        "  ]\n"
        "}\n");

    WriteTextFile(
        batchMeshAssetPath,
        "{\n"
        "  \"name\": \"Shader Reload Batch Runtime Test Mesh\",\n"
        "  \"type\": \"mesh\",\n"
        "  \"modelDataPath\": \"models/datas/axis.obj\",\n"
        "  \"materialSlots\": [\n"
        "    {\n"
        "      \"name\": \"Default\",\n"
        "      \"materialInstancePath\": \"" +
            batchMaterialInstancePath.generic_string() +
            "\"\n"
        "    }\n"
        "  ]\n"
        "}\n");

    WriteTextFile(
        scenePath,
        "{\n"
        "  \"name\": \"Shader Reload Runtime Test Scene\",\n"
        "  \"type\": \"scene\",\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"ShaderReloadAxis_001\",\n"
        "      \"type\": \"mesh\",\n"
        "      \"modelPath\": \"" +
            meshAssetPath.generic_string() +
            "\",\n"
        "      \"position\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1],\n"
        "      \"rotation\": [0, 0, 0]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"ShaderReloadBatchAxis_001\",\n"
        "      \"type\": \"mesh\",\n"
        "      \"modelPath\": \"" +
            batchMeshAssetPath.generic_string() +
            "\",\n"
        "      \"position\": [2, 0, 0],\n"
        "      \"scale\": [1, 1, 1],\n"
        "      \"rotation\": [0, 0, 0]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Sun_Light\",\n"
        "      \"type\": \"directionalLight\",\n"
        "      \"position\": [0, 1, 0],\n"
        "      \"rotation\": [0, 45, 0],\n"
        "      \"color\": [1, 1, 1],\n"
        "      \"intensity\": 1.0\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Camera_01\",\n"
        "      \"type\": \"camera\",\n"
        "      \"fov\": 90,\n"
        "      \"near_clip\": 0.1,\n"
        "      \"far_clip\": 2000,\n"
        "      \"position\": [0, 2, 6],\n"
        "      \"rotation\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Environment_01\",\n"
        "      \"type\": \"environment\",\n"
        "      \"environment\": {\n"
        "        \"type\": \"hdri\",\n"
        "        \"hdrPath\": \"hdri/sunset.exr\",\n"
        "        \"cubeSize\": 64\n"
        "      }\n"
        "    }\n"
        "  ]\n"
        "}\n");

    return scenePath;
}

std::filesystem::path
CreateWorldGraphTransactionHighLightScene(
    const std::filesystem::path& baseScenePath,
    size_t pointLightCount)
{
    nlohmann::json scene =
        nlohmann::json::parse(
            ReadTextFileBytes(baseScenePath));
    scene["name"] =
        "World Graph Transaction High Light Scene";
    nlohmann::json& objects = scene["objects"];
    for (size_t lightIndex = 0;
         lightIndex < pointLightCount;
         ++lightIndex)
    {
        const double angle =
            6.28318530717958647692 *
            static_cast<double>(lightIndex) /
            static_cast<double>(
                std::max<size_t>(
                    pointLightCount,
                    1));
        objects.push_back({
            {"name",
             "WorldGraph_PointLight_" +
                 std::to_string(lightIndex)},
            {"type", "pointLight"},
            {"position",
             {std::cos(angle) * 6.0,
              2.5,
              std::sin(angle) * 6.0}},
            {"rotation", {0, 0, 0}},
            {"color", {1.0, 0.85, 0.65}},
            {"intensity", 0.25},
            {"radius", 5.0}});
    }

    const std::filesystem::path scenePath =
        baseScenePath.parent_path() /
        "SC_world_graph_high_light_runtime_test.json";
    WriteTextFile(
        scenePath,
        scene.dump(2) + "\n");
    return scenePath;
}

std::string BuildShaderReloadCompatibleSource(
    const std::string& expression)
{
    return
        "#ifndef VL_SHADER_RELOAD_TEST_SHARED_GLSL\n"
        "#define VL_SHADER_RELOAD_TEST_SHARED_GLSL\n\n"
        "vec3 ShaderReloadTestColor(in MaterialFunctionContext pixel)\n"
        "{\n"
        "    return " + expression + ";\n"
        "}\n\n"
        "#endif\n";
}

std::string BuildShaderReloadSyntaxErrorSource()
{
    return
        "#ifndef VL_SHADER_RELOAD_TEST_SHARED_GLSL\n"
        "#define VL_SHADER_RELOAD_TEST_SHARED_GLSL\n\n"
        "vec3 ShaderReloadTestColor(in MaterialFunctionContext pixel)\n"
        "{\n"
        "    return this is invalid GLSL;\n"
        "}\n\n"
        "#endif\n";
}

std::string BuildShaderReloadAbiIncompatibleSource()
{
    return
        "#ifndef VL_SHADER_RELOAD_TEST_SHARED_GLSL\n"
        "#define VL_SHADER_RELOAD_TEST_SHARED_GLSL\n\n"
        "layout(set = 1, binding = 4) uniform sampler2D "
        "u_reloadAbiTexture;\n\n"
        "vec3 ShaderReloadTestColor(in MaterialFunctionContext pixel)\n"
        "{\n"
        "    return u_reloadTestColor.rgb + "
        "texture(u_reloadAbiTexture, vec2(0.0)).rgb;\n"
        "}\n\n"
        "#endif\n";
}

std::string ReplaceFirstOccurrence(
    const std::string& source,
    const std::string& from,
    const std::string& to)
{
    const size_t position = source.find(from);
    if (position == std::string::npos)
    {
        throw std::runtime_error(
            "Runtime test could not find the expected source fragment");
    }
    std::string result = source;
    result.replace(position, from.size(), to);
    return result;
}

std::shared_ptr<Material> FindShaderReloadTestMaterial()
{
    const RendererResourceCache::ImmutableWorldLocalResourceRefs resources =
        RendererResourceCache::GetInstance()
            .CaptureActiveWorldLocalResources();
    if (!resources)
    {
        return nullptr;
    }
    for (const auto& materialEntry : resources->materials)
    {
        const std::shared_ptr<Material>& material =
            materialEntry.second;
        if (material &&
            material->GetShaderName() ==
                ShaderReloadTestShaderName)
        {
            return material;
        }
    }
    return nullptr;
}

std::shared_ptr<MaterialInstance>
FindShaderReloadTestMaterialInstance()
{
    const RendererResourceCache::ImmutableWorldLocalResourceRefs resources =
        RendererResourceCache::GetInstance()
            .CaptureActiveWorldLocalResources();
    if (!resources)
    {
        return nullptr;
    }
    for (const auto& instanceEntry :
         resources->materialInstances)
    {
        const std::shared_ptr<MaterialInstance>& instance =
            instanceEntry.second;
        std::shared_ptr<Material> material =
            instance
            ? instance->GetBaseMaterial().lock()
            : nullptr;
        if (material &&
            material->GetShaderName() ==
                ShaderReloadTestShaderName)
        {
            return instance;
        }
    }
    return nullptr;
}

std::shared_ptr<Texture>
FindShaderReloadTestPrimaryTexture()
{
    const std::shared_ptr<MaterialInstance> instance =
        FindShaderReloadTestMaterialInstance();
    return instance
        ? instance->GetTexture("u_reloadTexture")
        : nullptr;
}

std::shared_ptr<MaterialInstance>
FindShaderReloadBatchTestMaterialInstance()
{
    const RendererResourceCache::ImmutableWorldLocalResourceRefs resources =
        RendererResourceCache::GetInstance()
            .CaptureActiveWorldLocalResources();
    if (!resources)
    {
        return nullptr;
    }
    for (const auto& instanceEntry :
         resources->materialInstances)
    {
        const std::shared_ptr<MaterialInstance>& instance =
            instanceEntry.second;
        if (instance &&
            instance->HasParameter(
                "u_reloadBatchColor"))
        {
            return instance;
        }
    }
    return nullptr;
}

std::shared_ptr<RendererObjectResourceEntry>
FindShaderReloadTestObjectResources()
{
    const RendererResourceCache::ImmutableWorldLocalResourceRefs resources =
        RendererResourceCache::GetInstance()
            .CaptureActiveWorldLocalResources();
    if (!resources)
    {
        return nullptr;
    }
    const auto resourceIt =
        resources->objectResources.find(
            "ShaderReloadAxis_001");
    return resourceIt != resources->objectResources.end()
        ? resourceIt->second
        : nullptr;
}

uint64_t GetObjectDescriptorPoolIdentity(
    const std::shared_ptr<RendererObjectResourceEntry>& entry)
{
    return entry
        ? entry->GetResources()
              .descriptorPoolHandle.id
        : 0;
}

void ValidateShaderDefinitionMigratedState(
    const std::shared_ptr<MaterialInstance>& instance,
    std::uintptr_t retainedTextureIdentity,
    bool expectExtra,
    bool expectRemoved,
    bool expectMultiMain)
{
    if (!instance)
    {
        throw std::runtime_error(
            "shader definition test MaterialInstance is not live");
    }

    const Eigen::Vector4f expectedColor(
        0.85f,
        0.15f,
        0.45f,
        0.65f);
    if (!instance->HasParameter(
            "u_reloadRuntimeScalar") ||
        instance->GetParameter<float>(
            "u_reloadRuntimeScalar") !=
            0.875f ||
        !instance->HasParameter(
            "u_reloadTestColor") ||
        !instance->GetParameter<Eigen::Vector4f>(
            "u_reloadTestColor").isApprox(
                expectedColor))
    {
        throw std::runtime_error(
            "compatible runtime parameter values were not migrated");
    }

    if (expectExtra !=
        instance->HasParameter("u_reloadExtra"))
    {
        throw std::runtime_error(
            "u_reloadExtra presence does not match the candidate schema");
    }
    if (expectExtra &&
        instance->GetParameter<float>(
            "u_reloadExtra") != 0.5f)
    {
        throw std::runtime_error(
            "u_reloadExtra did not receive its candidate default");
    }
    if (expectRemoved !=
        instance->HasParameter("u_reloadRemoved"))
    {
        throw std::runtime_error(
            "u_reloadRemoved presence does not match the candidate schema");
    }
    if (expectRemoved &&
        instance->GetParameter<float>(
            "u_reloadRemoved") != 0.375f)
    {
        throw std::runtime_error(
            "u_reloadRemoved did not receive its candidate default");
    }
    if (expectMultiMain !=
        instance->HasParameter("u_reloadMultiMain"))
    {
        throw std::runtime_error(
            "u_reloadMultiMain presence does not match the candidate schema");
    }
    if (expectMultiMain &&
        instance->GetParameter<float>(
            "u_reloadMultiMain") != 0.625f)
    {
        throw std::runtime_error(
            "u_reloadMultiMain did not receive its candidate default");
    }

    const std::shared_ptr<Texture> retainedTexture =
        instance->GetTexture(
            "u_reloadTexture");
    if (!retainedTexture ||
        reinterpret_cast<std::uintptr_t>(
            retainedTexture.get()) !=
            retainedTextureIdentity)
    {
        throw std::runtime_error(
            "compatible live Texture identity was not migrated");
    }
}

std::filesystem::path CreateGeneratedMaterialFailureScene(const std::string& resourcePath)
{
    const std::filesystem::path fixtureDirectory =
        BuildGeneratedFixtureDirectory(resourcePath, "bad-material-");
    std::filesystem::create_directories(fixtureDirectory);

    const std::filesystem::path materialInstancePath = fixtureDirectory / "MI_bad_runtime_test.json";
    const std::filesystem::path meshAssetPath = fixtureDirectory / "SM_bad_runtime_test.json";
    const std::filesystem::path scenePath = fixtureDirectory / "SC_bad_material_runtime_test.json";

    // The generated scene and mesh are valid enough to reach renderer resource
    // loading. The material instance then fails on an unknown parameter, which
    // verifies rollback after pass materials/resources have already been staged.
    WriteTextFile(
        materialInstancePath,
        R"({
  "name": "Bad Runtime Test Material Instance",
  "type": "materialInstance",
  "material": "shader/glsl/M_vertexColor.json",
  "parameters": {
    "u_missingGeneratedRuntimeTestParam": [1, 1, 1, 1]
  }
})");

    WriteTextFile(
        meshAssetPath,
        "{\n"
        "  \"name\": \"Bad Runtime Test Mesh\",\n"
        "  \"type\": \"mesh\",\n"
        "  \"modelDataPath\": \"models/datas/axis.obj\",\n"
        "  \"materialSlots\": [\n"
        "    {\n"
        "      \"name\": \"Default\",\n"
        "      \"materialInstancePath\": \"" + materialInstancePath.generic_string() + "\"\n"
        "    }\n"
        "  ]\n"
        "}\n");

    WriteTextFile(
        scenePath,
        "{\n"
        "  \"name\": \"Bad Material Runtime Test Scene\",\n"
        "  \"type\": \"scene\",\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"BadMaterialMesh_001\",\n"
        "      \"type\": \"mesh\",\n"
        "      \"modelPath\": \"" + meshAssetPath.generic_string() + "\",\n"
        "      \"position\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1],\n"
        "      \"rotation\": [0, 0, 0]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Camera_01\",\n"
        "      \"type\": \"camera\",\n"
        "      \"fov\": 90,\n"
        "      \"near_clip\": 0.1,\n"
        "      \"far_clip\": 2000,\n"
        "      \"position\": [0, 2, 6],\n"
        "      \"rotation\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1]\n"
        "    }\n"
        "  ]\n"
        "}\n");

    return scenePath;
}

std::filesystem::path CreateGeneratedMeshFailureScene(const std::string& resourcePath)
{
    const std::filesystem::path fixtureDirectory =
        BuildGeneratedFixtureDirectory(resourcePath, "bad-mesh-");
    std::filesystem::create_directories(fixtureDirectory);

    const std::filesystem::path meshAssetPath = fixtureDirectory / "SM_bad_mesh_runtime_test.json";
    const std::filesystem::path scenePath = fixtureDirectory / "SC_bad_mesh_runtime_test.json";

    WriteTextFile(
        meshAssetPath,
        "{\n"
        "  \"name\": \"Bad Mesh Runtime Test Mesh\",\n"
        "  \"type\": \"mesh\",\n"
        "  \"modelDataPath\": \"models/datas/RUNTIME_VALIDATION_MISSING_ASSET.obj\",\n"
        "  \"materialSlots\": [\n"
        "    {\n"
        "      \"name\": \"Default\",\n"
        "      \"materialInstancePath\": \"materials/MI_axis.json\"\n"
        "    }\n"
        "  ]\n"
        "}\n");

    WriteTextFile(
        scenePath,
        "{\n"
        "  \"name\": \"Bad Mesh Runtime Test Scene\",\n"
        "  \"type\": \"scene\",\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"BadMesh_001\",\n"
        "      \"type\": \"mesh\",\n"
        "      \"modelPath\": \"" + meshAssetPath.generic_string() + "\",\n"
        "      \"position\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1],\n"
        "      \"rotation\": [0, 0, 0]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Camera_01\",\n"
        "      \"type\": \"camera\",\n"
        "      \"fov\": 90,\n"
        "      \"near_clip\": 0.1,\n"
        "      \"far_clip\": 2000,\n"
        "      \"position\": [0, 2, 6],\n"
        "      \"rotation\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1]\n"
        "    }\n"
        "  ]\n"
        "}\n");

    return scenePath;
}

std::filesystem::path CreateGeneratedTextureFailureScene(const std::string& resourcePath)
{
    const std::filesystem::path fixtureDirectory =
        BuildGeneratedFixtureDirectory(resourcePath, "bad-texture-");
    std::filesystem::create_directories(fixtureDirectory);

    const std::filesystem::path textureAssetPath = fixtureDirectory / "T_bad_runtime_test.json";
    const std::filesystem::path materialInstancePath = fixtureDirectory / "MI_bad_texture_runtime_test.json";
    const std::filesystem::path meshAssetPath = fixtureDirectory / "SM_bad_texture_runtime_test.json";
    const std::filesystem::path scenePath = fixtureDirectory / "SC_bad_texture_runtime_test.json";

    WriteTextFile(
        textureAssetPath,
        R"({
  "name": "Bad Runtime Test Texture",
  "type": "texture",
  "source": "textures/datas/RUNTIME_VALIDATION_MISSING_ASSET.png",
  "colorSpace": "srgb",
  "mipmaps": true,
  "filter": "linear",
  "wrapMode": "repeat"
})");

    WriteTextFile(
        materialInstancePath,
        "{\n"
        "  \"name\": \"Bad Runtime Test Texture Material Instance\",\n"
        "  \"type\": \"materialInstance\",\n"
        "  \"material\": \"shader/glsl/M_unlit.json\",\n"
        "  \"parameters\": {\n"
        "    \"u_tintColor\": [1, 1, 1, 1]\n"
        "  },\n"
        "  \"textures\": {\n"
        "    \"albedoMap\": \"" + textureAssetPath.generic_string() + "\"\n"
        "  }\n"
        "}\n");

    WriteTextFile(
        meshAssetPath,
        "{\n"
        "  \"name\": \"Bad Texture Runtime Test Mesh\",\n"
        "  \"type\": \"mesh\",\n"
        "  \"modelDataPath\": \"models/datas/axis.obj\",\n"
        "  \"materialSlots\": [\n"
        "    {\n"
        "      \"name\": \"Default\",\n"
        "      \"materialInstancePath\": \"" + materialInstancePath.generic_string() + "\"\n"
        "    }\n"
        "  ]\n"
        "}\n");

    WriteTextFile(
        scenePath,
        "{\n"
        "  \"name\": \"Bad Texture Runtime Test Scene\",\n"
        "  \"type\": \"scene\",\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"BadTextureMesh_001\",\n"
        "      \"type\": \"mesh\",\n"
        "      \"modelPath\": \"" + meshAssetPath.generic_string() + "\",\n"
        "      \"position\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1],\n"
        "      \"rotation\": [0, 0, 0]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Camera_01\",\n"
        "      \"type\": \"camera\",\n"
        "      \"fov\": 90,\n"
        "      \"near_clip\": 0.1,\n"
        "      \"far_clip\": 2000,\n"
        "      \"position\": [0, 2, 6],\n"
        "      \"rotation\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1]\n"
        "    }\n"
        "  ]\n"
        "}\n");

    return scenePath;
}

std::filesystem::path CreateGeneratedHighLightStressScene(const std::string& resourcePath)
{
    constexpr int GeneratedPointLightCount = 96;
    constexpr double TwoPi = 6.28318530717958647692;

    const std::filesystem::path fixtureDirectory =
        BuildGeneratedFixtureDirectory(resourcePath, "high-lights-");
    std::filesystem::create_directories(fixtureDirectory);

    const std::filesystem::path scenePath = fixtureDirectory / "SC_high_light_runtime_test.json";
    std::ostringstream scene;
    scene
        << "{\n"
        << "  \"name\": \"High Light Runtime Test Scene\",\n"
        << "  \"type\": \"scene\",\n"
        << "  \"objects\": [\n"
        << "    {\n"
        << "      \"name\": \"Oak_Complex_Rules_001\",\n"
        << "      \"type\": \"mesh\",\n"
        << "      \"modelPath\": \"models/SM_speedtree_oak_complex_rules.json\",\n"
        << "      \"position\": [0, 0, 0],\n"
        << "      \"scale\": [1, 1, 1],\n"
        << "      \"rotation\": [0, 0, 0]\n"
        << "    },\n"
        << "    {\n"
        << "      \"name\": \"Axis_001\",\n"
        << "      \"type\": \"mesh\",\n"
        << "      \"modelPath\": \"models/SM_axis.json\",\n"
        << "      \"position\": [0, 0, 0],\n"
        << "      \"scale\": [1, 1, 1],\n"
        << "      \"rotation\": [0, 0, 0]\n"
        << "    },\n"
        << "    {\n"
        << "      \"name\": \"Sun_Light\",\n"
        << "      \"type\": \"directionalLight\",\n"
        << "      \"position\": [0, 1, 0],\n"
        << "      \"rotation\": [0, 45, 0],\n"
        << "      \"color\": [1, 1, 1],\n"
        << "      \"intensity\": 1.0\n"
        << "    },\n"
        << "    {\n"
        << "      \"name\": \"Camera_01\",\n"
        << "      \"type\": \"camera\",\n"
        << "      \"fov\": 90,\n"
        << "      \"near_clip\": 0.1,\n"
        << "      \"far_clip\": 2000,\n"
        << "      \"position\": [0, 2, 6],\n"
        << "      \"rotation\": [0, 0, 0],\n"
        << "      \"scale\": [1, 1, 1]\n"
        << "    },\n"
        << "    {\n"
        << "      \"name\": \"Environment_01\",\n"
        << "      \"type\": \"environment\",\n"
        << "      \"environment\": {\n"
        << "        \"type\": \"hdri\",\n"
        << "        \"hdrPath\": \"hdri/sunset.exr\",\n"
        << "        \"cubeSize\": 512,\n"
        << "        \"intensity\": 1.0\n"
        << "      }\n"
        << "    }";

    for (int lightIndex = 0; lightIndex < GeneratedPointLightCount; ++lightIndex)
    {
        const double angle = TwoPi * static_cast<double>(lightIndex) /
            static_cast<double>(GeneratedPointLightCount);
        const double x = std::cos(angle) * 6.0;
        const double z = std::sin(angle) * 6.0;

        scene
            << ",\n"
            << "    {\n"
            << "      \"name\": \"Generated_PointLight_" << lightIndex << "\",\n"
            << "      \"type\": \"pointLight\",\n"
            << "      \"position\": [" << x << ", 2.5, " << z << "],\n"
            << "      \"rotation\": [0, 0, 0],\n"
            << "      \"color\": [1.0, 0.85, 0.65],\n"
            << "      \"intensity\": 0.25,\n"
            << "      \"radius\": 5.0\n"
            << "    }";
    }

    scene
        << "\n"
        << "  ]\n"
        << "}\n";

    WriteTextFile(scenePath, scene.str());
    return scenePath;
}

void UpdateMaxPendingRetiredResources(size_t pendingCount, size_t& maxPendingRetiredResources)
{
    maxPendingRetiredResources = std::max(maxPendingRetiredResources, pendingCount);
}

void CleanupGeneratedRuntimeFixture(
    const std::string& fixtureDirectory,
    const DiagnosticsSubsystem& diagnostics)
{
    if (fixtureDirectory.empty())
    {
        return;
    }

    std::error_code errorCode;
    std::filesystem::remove_all(fixtureDirectory, errorCode);
    if (errorCode)
    {
        diagnostics.ReportWarning(
            "Failed to remove generated runtime test fixture: " +
            fixtureDirectory +
            " error=" +
            errorCode.message());
    }
}

void CleanupGeneratedRuntimeFixtureIfNeeded(
    bool& cleanupFixture,
    std::string& fixtureDirectory,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!cleanupFixture)
    {
        return;
    }

    cleanupFixture = false;
    CleanupGeneratedRuntimeFixture(fixtureDirectory, diagnostics);
    fixtureDirectory.clear();
}

} // namespace RuntimeTestFixtures
} // namespace VL

