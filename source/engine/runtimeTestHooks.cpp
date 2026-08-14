#include "engine/runtimeTestHooks.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "controller.h"
#include "engine/diagnosticsSubsystem.h"
#include "engine/engineLoop.h"
#include "engine/runtimeConfig.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/subsystemCollection.h"
#include "material.h"
#include "materialInstance.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "pipeline/pipelineFactory.h"
#include "platform/platformWindow.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"
#include "renderSystem.h"
#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"
#include "shader/reload/shaderCompileWorker.h"
#include "shader/reload/shaderFileMonitor.h"
#include "shader/reload/shaderReloadCoordinator.h"
#include "shaderCompiler.h"
#include "world/world.h"
#include "world/worldManager.h"

namespace VL
{
namespace
{

constexpr int RetireDrainFrameBudget = 180;
constexpr std::chrono::seconds ShaderAsyncWaitTimeout{8};
constexpr std::chrono::seconds ShaderDefinitionWaitTimeout{12};
constexpr const char* ShaderReloadTestShaderName =
    "runtimeTest/shaderReloadTest";

std::array<size_t, 9> CaptureBackendIdentityCounts(
    const RendererBackendVulkan& backend)
{
    const RendererBackendResourceIdentityCounts counts =
        backend.CaptureResourceIdentityCounts();
    return {
        counts.buffers,
        counts.images,
        counts.imageViews,
        counts.samplers,
        counts.descriptorSetLayouts,
        counts.descriptorPools,
        counts.descriptorSets,
        counts.renderPasses,
        counts.framebuffers};
}

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

std::vector<std::string> CaptureImageResourceDebugNames(
    const RendererBackendVulkan& backend)
{
    const RendererBackendImageResourceDebugNames names =
        backend.CaptureImageResourceDebugNames();
    std::vector<std::string> result;
    result.reserve(
        names.images.size() +
        names.imageViews.size() +
        names.samplers.size());
    for (const std::string& name : names.images)
    {
        result.push_back("image:" + name);
    }
    for (const std::string& name : names.imageViews)
    {
        result.push_back("imageView:" + name);
    }
    for (const std::string& name : names.samplers)
    {
        result.push_back("sampler:" + name);
    }
    std::sort(result.begin(), result.end());
    return result;
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

std::vector<uint64_t> CaptureBufferHandleIds(
    const std::vector<RHIBufferHandle>& handles)
{
    std::vector<uint64_t> identities;
    identities.reserve(handles.size());
    for (const RHIBufferHandle& handle : handles)
    {
        identities.push_back(handle.id);
    }
    return identities;
}

std::string CaptureRenderGraphGpuFingerprint(
    const RenderGraph& renderGraph)
{
    std::ostringstream stream;
    stream << "owner=" <<
        renderGraph.GetOwnerGeneration() << ";";

    std::vector<std::string> resourceNames;
    for (const auto& entry :
         renderGraph.GetResourcesMsaa())
    {
        resourceNames.push_back(
            "msaa:" + entry.first);
    }
    for (const auto& entry :
         renderGraph.GetResourcesResolve())
    {
        resourceNames.push_back(
            "resolve:" + entry.first);
    }
    std::sort(
        resourceNames.begin(),
        resourceNames.end());
    for (const std::string& name : resourceNames)
    {
        const bool isMsaa =
            name.rfind("msaa:", 0) == 0;
        const std::string resourceName =
            name.substr(isMsaa ? 5 : 8);
        const auto& resourceMap =
            isMsaa
            ? renderGraph.GetResourcesMsaa()
            : renderGraph.GetResourcesResolve();
        const std::vector<RenderResource>& resources =
            resourceMap.at(resourceName);
        for (size_t index = 0;
             index < resources.size();
             ++index)
        {
            const RenderResource& resource =
                resources[index];
            stream << name << "." << index <<
                ".image=" << resource.imageHandle.id <<
                ".view=" << resource.imageViewHandle.id <<
                ".sampler=" << resource.samplerHandle.id <<
                ";";
            for (const RHIImageViewHandle& layerView :
                 resource.imageViewHandles)
            {
                stream << "layer=" <<
                    layerView.id << ";";
            }
        }
    }

    for (const std::string& passName :
         renderGraph.GetRenderpassesOrdered())
    {
        const Renderpass& renderpass =
            renderGraph.GetRenderpasses().at(passName);
        stream << "pass=" << passName <<
            ".renderPass=" <<
            renderpass.renderPassHandle.id <<
            ".descriptorPool=" <<
            renderpass.descriptorPoolHandle.id <<
            ".descriptorLayout=" <<
            renderpass.descriptorSetLayoutHandle.id <<
            ".emptyLayout=" <<
            renderpass.emptyDescriptorSetLayoutHandle.id <<
            ";";
        for (const RHIFramebufferHandle& framebuffer :
             renderpass.framebufferHandles)
        {
            stream << "framebuffer=" <<
                framebuffer.id << ";";
        }
    }
    return ContentHasher::HashString(
        stream.str()).ToHex();
}

struct ShaderReloadRuntimeSnapshot
{
    std::shared_ptr<Material> material;
    std::uintptr_t surfacePipeline = 0;
    std::uintptr_t shadowPipeline = 0;
    std::string surfaceGeneration;
    std::string shadowGeneration;
    std::string manifestDigest;
    std::string surfaceVertexDigest;
    std::string surfaceFragmentDigest;
    std::string shadowVertexDigest;
    std::string shadowFragmentDigest;
};

std::uintptr_t GetWorldTextureIdentity(const std::string& key)
{
    const std::shared_ptr<Texture>* texture =
        RendererResourceCache::GetInstance().GetWorldTexture(key);
    return texture != nullptr && *texture
        ? reinterpret_cast<std::uintptr_t>(texture->get())
        : 0;
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

void AppendPointerMapFingerprint(
    std::ostringstream& stream,
    std::string_view label,
    const std::unordered_map<std::string, std::uintptr_t>& values)
{
    std::vector<std::pair<std::string, std::uintptr_t>> ordered(
        values.begin(),
        values.end());
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.first < rhs.first;
        });
    stream << label << ".count=" << ordered.size() << ";";
    for (const auto& [name, value] : ordered)
    {
        stream << label << "." << name << "=" << value << ";";
    }
}

void AppendMaterialInstanceStateFingerprint(
    std::ostringstream& stream,
    const MaterialInstanceStateSnapshot& snapshot)
{
    stream << "mi.identity=" <<
        snapshot.normalizedMaterialInstanceIdentity << ";";
    for (const auto& [name, value] :
         snapshot.parameters)
    {
        stream << "mi.parameter." << name << ".type=" <<
            value.index() << ";";
        switch (value.index())
        {
        case 0:
            stream << std::get<float>(value);
            break;
        case 1:
        {
            const Eigen::Vector2f& vector =
                std::get<Eigen::Vector2f>(value);
            stream << vector.x() << "," << vector.y();
            break;
        }
        case 2:
        {
            const Eigen::Vector3f& vector =
                std::get<Eigen::Vector3f>(value);
            stream << vector.x() << "," << vector.y() <<
                "," << vector.z();
            break;
        }
        case 3:
        {
            const Eigen::Vector4f& vector =
                std::get<Eigen::Vector4f>(value);
            stream << vector.x() << "," << vector.y() <<
                "," << vector.z() << "," << vector.w();
            break;
        }
        default:
            throw std::runtime_error(
                "Unknown MaterialInstance snapshot parameter type");
        }
        stream << ";";
    }
    for (const auto& [name, binding] :
         snapshot.textures)
    {
        stream << "mi.texture." << name << ".pointer=" <<
            reinterpret_cast<std::uintptr_t>(
                binding.texture.get()) << ";";
        stream << "mi.texture." << name << ".asset=" <<
            binding.textureAssetIdentity.value_or(
                "<none>") << ";";
        stream << "mi.texture." << name << ".cache=" <<
            binding.textureCacheIdentity.value_or(
                "<none>") << ";";
    }
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

std::string HashFileIfPresent(const std::filesystem::path& path)
{
    return std::filesystem::is_regular_file(path)
        ? ContentHasher::HashFile(path).ToHex()
        : std::string();
}

ShaderReloadRuntimeSnapshot CaptureShaderReloadRuntimeSnapshot()
{
    ShaderReloadRuntimeSnapshot snapshot;
    RendererResourceCache::WorldLocalResourceSnapshot resources =
        RendererResourceCache::GetInstance().CaptureWorldLocalResources();
    for (const auto& materialEntry : resources.materials)
    {
        const std::shared_ptr<Material>& material =
            materialEntry.second;
        if (material &&
            material->GetShaderName() ==
                ShaderReloadTestShaderName)
        {
            snapshot.material = material;
            break;
        }
    }

    if (!snapshot.material)
    {
        throw std::runtime_error(
            "Shader reload runtime test material is not live");
    }

    const GraphicsShaderVariantArtifact& surface =
        snapshot.material->GetSurfaceShaderArtifact();
    const auto& shadow =
        snapshot.material->GetShadowShaderArtifact();
    if (!snapshot.material->GetRenderPipeline() ||
        !snapshot.material->GetShadowPipeline() ||
        !shadow)
    {
        throw std::runtime_error(
            "Shader reload runtime test requires live Surface and Shadow pipelines");
    }

    snapshot.surfacePipeline =
        reinterpret_cast<std::uintptr_t>(
            snapshot.material->GetRenderPipeline().get());
    snapshot.shadowPipeline =
        reinterpret_cast<std::uintptr_t>(
            snapshot.material->GetShadowPipeline().get());
    snapshot.surfaceGeneration =
        surface.artifactGenerationKey;
    snapshot.shadowGeneration =
        shadow->artifactGenerationKey;
    snapshot.surfaceVertexDigest =
        HashFileIfPresent(surface.vertexSpvPath);
    snapshot.surfaceFragmentDigest =
        HashFileIfPresent(surface.fragmentSpvPath);
    snapshot.shadowVertexDigest =
        HashFileIfPresent(shadow->vertexSpvPath);
    snapshot.shadowFragmentDigest =
        HashFileIfPresent(shadow->fragmentSpvPath);
    snapshot.manifestDigest = HashFileIfPresent(
        std::filesystem::path(CommonFunction::GetProjectPath()) /
        "shader" /
        "spv" /
        "shader-build-cache.json");
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

struct ShaderAutoReloadRuntimeSnapshot
{
    std::string surfaceLogicalBuildId;
    std::string shadowLogicalBuildId;
    std::string surfaceGeneration;
    std::string shadowGeneration;
    std::string manifestDigest;
    std::string resolvedGeneration;
};

ShaderAutoReloadRuntimeSnapshot CaptureShaderAutoReloadRuntimeSnapshot()
{
    ShaderAutoReloadRuntimeSnapshot snapshot;
    RendererResourceCache::WorldLocalResourceSnapshot resources =
        RendererResourceCache::GetInstance().CaptureWorldLocalResources();
    for (const auto& materialEntry : resources.materials)
    {
        const std::shared_ptr<Material>& material =
            materialEntry.second;
        if (material &&
            material->GetShaderName() ==
                ShaderReloadTestShaderName)
        {
            const GraphicsShaderVariantArtifact& surface =
                material->GetSurfaceShaderArtifact();
            const auto& shadow =
                material->GetShadowShaderArtifact();
            snapshot.surfaceLogicalBuildId =
                surface.logicalBuildId;
            snapshot.surfaceGeneration =
                surface.artifactGenerationKey;
            snapshot.shadowLogicalBuildId =
                shadow
                ? shadow->logicalBuildId
                : std::string();
            snapshot.shadowGeneration =
                shadow
                ? shadow->artifactGenerationKey
                : std::string();
            break;
        }
    }
    if (snapshot.surfaceGeneration.empty())
    {
        throw std::runtime_error(
            "Shader auto reload runtime test material is not live");
    }
    snapshot.manifestDigest = HashFileIfPresent(
        std::filesystem::path(CommonFunction::GetProjectPath()) /
        "shader" /
        "spv" /
        "shader-build-cache.json");
    snapshot.resolvedGeneration =
        RenderSystem::GetInstance()
            .GetResolvedShaderGenerationFingerprint();
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

bool ManifestArtifactDependsOnAllSources(
    const VL::ShaderBuildManifestSnapshot& manifest,
    const std::string& logicalBuildId,
    const std::vector<std::string>& sourceIdentities)
{
    const auto artifactIt = manifest.artifacts.find(logicalBuildId);
    if (artifactIt == manifest.artifacts.end())
    {
        return false;
    }

    for (const std::string& identity : sourceIdentities)
    {
        bool found = false;
        for (const VL::ShaderDependencyRecord& dependency :
             artifactIt->second.dependencies)
        {
            if (dependency.path == identity)
            {
                found = true;
                break;
            }
        }
        if (!found)
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
    "u_reloadTestColor": [0.2, 0.5, 0.9, 1.0],
    "u_alphaClipThreshold": 0.25,
    "u_reloadRuntimeScalar": 0.125
  },
  "textures": {
    "u_reloadTexture": "textures/T_Texture.json",
    "u_reloadAlternateTexture": "textures/T_UV_Checker.json"
  }
})");

    WriteTextFile(
        batchMaterialInstancePath,
        R"({
  "name": "Shader Reload Batch Runtime Test Material Instance",
  "type": "materialInstance",
  "material": "shader/glsl/runtimeTest/M_shaderReloadBatchTest.json",
  "parameters": {
    "u_reloadBatchColor": [0.15, 0.7, 0.35, 1.0]
  },
  "textures": {}
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
        "vec3 ShaderReloadTestColor(in MaterialPixelContext pixel)\n"
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
        "vec3 ShaderReloadTestColor(in MaterialPixelContext pixel)\n"
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
        "vec3 ShaderReloadTestColor(in MaterialPixelContext pixel)\n"
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
    RendererResourceCache::WorldLocalResourceSnapshot resources =
        RendererResourceCache::GetInstance()
            .CaptureWorldLocalResources();
    for (const auto& materialEntry : resources.materials)
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
    RendererResourceCache::WorldLocalResourceSnapshot resources =
        RendererResourceCache::GetInstance()
            .CaptureWorldLocalResources();
    for (const auto& instanceEntry :
         resources.materialInstances)
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
    RendererResourceCache::WorldLocalResourceSnapshot resources =
        RendererResourceCache::GetInstance()
            .CaptureWorldLocalResources();
    for (const auto& instanceEntry :
         resources.materialInstances)
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
    const RendererResourceCache::WorldLocalResourceSnapshot resources =
        RendererResourceCache::GetInstance()
            .CaptureWorldLocalResources();
    const auto resourceIt =
        resources.objectResources.find(
            "ShaderReloadAxis_001");
    return resourceIt != resources.objectResources.end()
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
  },
  "textures": {}
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
        << "      \"hdrPath\": \"hdri/sunset.exr\",\n"
        << "      \"cubeSize\": 512\n"
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

} // namespace

bool RuntimeTestHooks::SurfaceArtifactDependsOnSourceWithDigest(
    EngineLoop& engineLoop,
    const std::string& surfaceLogicalBuildId,
    const std::string& dependencyIdentity,
    const std::string& expectedDigest)
{
    if (surfaceLogicalBuildId.empty() ||
        engineLoop.shaderCompiler == nullptr)
    {
        return false;
    }

    const VL::ShaderBuildManifestSnapshot manifest =
        engineLoop.shaderCompiler->CaptureManifestSnapshot();
    const auto artifactIt =
        manifest.artifacts.find(surfaceLogicalBuildId);
    if (artifactIt == manifest.artifacts.end())
    {
        return false;
    }
    for (const VL::ShaderDependencyRecord& dependency :
         artifactIt->second.dependencies)
    {
        if (dependency.path == dependencyIdentity)
        {
            return dependency.digest == expectedDigest;
        }
    }
    return false;
}

bool RuntimeTestHooks::UiArtifactFragmentMatchesCurrentSource(
    EngineLoop& engineLoop) const
{
    if (engineLoop.shaderCompiler == nullptr)
    {
        return false;
    }

    ShaderVariantKey variant;
    variant.shaderName = "uiOverlay";
    const VL::ShaderBuildRequest request =
        engineLoop.shaderCompiler
            ->CreateGraphicsVariantBuildRequest(variant);
    const VL::ShaderBuildManifestSnapshot manifest =
        engineLoop.shaderCompiler->CaptureManifestSnapshot();
    const auto artifactIt =
        manifest.artifacts.find(request.logicalBuildId);
    if (artifactIt == manifest.artifacts.end())
    {
        return false;
    }

    const std::filesystem::path fragmentPath =
        engineLoop.shaderCompiler->GetShaderRoot() /
        "glsl" /
        "uiOverlay.frag";
    const std::string currentDigest =
        ContentHasher::HashFile(fragmentPath).ToHex();
    for (const VL::ShaderBuildSourceRecord& source :
         artifactIt->second.primarySources)
    {
        if (source.identity == "uiOverlay.frag")
        {
            return source.digest == currentDigest;
        }
    }
    return false;
}

std::string
RuntimeTestHooks::CaptureWorldGraphRuntimeFingerprint(
    EngineLoop& engineLoop,
    const std::string& primaryDefinitionPath,
    const std::string& batchDefinitionPath,
    bool includeFrameLifecycleDiagnostics,
    std::string* details) const
{
    std::ostringstream stream;
    const WorldManager& worldManager =
        engineLoop.GetSubsystems()
            .GetWorldManager();
    const WorldHandle& worldHandle =
        worldManager.GetActiveWorldHandle();
    stream << "world.generation=" <<
        worldHandle.generation << ";";
    stream << "world.path=" <<
        worldHandle.scenePath << ";";
    stream << "world.pointer=" <<
        reinterpret_cast<std::uintptr_t>(
            worldManager.GetActiveWorld().get()) << ";";
    stream << "world.nextGeneration=" <<
        worldManager.GetNextWorldGeneration() << ";";

    const RuntimeRendererResourceFingerprint resources =
        CaptureRuntimeRendererResourceFingerprint();
    stream << "cache.ownerGeneration=" <<
        resources.worldOwnerGeneration << ";";
    AppendPointerMapFingerprint(
        stream,
        "cache.worldTextures",
        resources.worldTextures);
    AppendPointerMapFingerprint(
        stream,
        "cache.renderables",
        resources.renderableObjects);
    AppendPointerMapFingerprint(
        stream,
        "cache.materials",
        resources.materials);
    AppendPointerMapFingerprint(
        stream,
        "cache.instances",
        resources.materialInstances);
    AppendPointerMapFingerprint(
        stream,
        "cache.objectResources",
        resources.objectResources);
    AppendPointerMapFingerprint(
        stream,
        "cache.textures",
        resources.textures);
    AppendPointerMapFingerprint(
        stream,
        "graph.passMaterials",
        resources.passMaterialInstances);

    const RenderGraph& renderGraph =
        RenderGraph::GetInstance();
    stream << "graph.generation=" <<
        renderGraph.GetOwnerGeneration() << ";";
    std::vector<std::string> graphResourceNames;
    for (const auto& entry :
         renderGraph.GetResourcesMsaa())
    {
        graphResourceNames.push_back(
            "msaa:" + entry.first);
    }
    for (const auto& entry :
         renderGraph.GetResourcesResolve())
    {
        graphResourceNames.push_back(
            "resolve:" + entry.first);
    }
    std::sort(
        graphResourceNames.begin(),
        graphResourceNames.end());
    for (const std::string& name :
         graphResourceNames)
    {
        const bool isMsaa =
            name.rfind("msaa:", 0) == 0;
        const std::string resourceName =
            name.substr(
                isMsaa ? 5 : 8);
        const auto& resourceMap =
            isMsaa
            ? renderGraph.GetResourcesMsaa()
            : renderGraph.GetResourcesResolve();
        const std::vector<RenderResource>& entries =
            resourceMap.at(resourceName);
        for (size_t index = 0;
             index < entries.size();
             ++index)
        {
            const RenderResource& resource =
                entries[index];
            stream << "graph.resource." << name <<
                "." << index << ".image=" <<
                resource.imageHandle.id << ";";
            stream << "graph.resource." << name <<
                "." << index << ".view=" <<
                resource.imageViewHandle.id << ";";
            stream << "graph.resource." << name <<
                "." << index << ".sampler=" <<
                resource.samplerHandle.id << ";";
            for (size_t layer = 0;
                 layer <
                     resource.imageViewHandles.size();
                 ++layer)
            {
                stream << "graph.resource." << name <<
                    "." << index << ".layer." <<
                    layer << "=" <<
                    resource.imageViewHandles[
                        layer].id << ";";
            }
        }
    }

    std::vector<std::string> passNames;
    passNames.reserve(
        renderGraph.GetRenderpasses().size());
    for (const auto& [passName, renderpass] :
         renderGraph.GetRenderpasses())
    {
        (void)renderpass;
        passNames.push_back(passName);
    }
    std::sort(
        passNames.begin(),
        passNames.end());
    for (const std::string& passName :
         passNames)
    {
        const Renderpass& renderpass =
            renderGraph.GetRenderpasses().at(
                passName);
        stream << "graph.pass." << passName <<
            ".renderPass=" <<
            renderpass.renderPassHandle.id << ";";
        stream << "graph.pass." << passName <<
            ".descriptorPool=" <<
            renderpass.descriptorPoolHandle.id << ";";
        stream << "graph.pass." << passName <<
            ".descriptorLayout=" <<
            renderpass.descriptorSetLayoutHandle.id <<
            ";";
        stream << "graph.pass." << passName <<
            ".emptyLayout=" <<
            renderpass.emptyDescriptorSetLayoutHandle.id <<
            ";";
        for (size_t index = 0;
             index <
                 renderpass.framebufferHandles.size();
             ++index)
        {
            stream << "graph.pass." << passName <<
                ".framebuffer." << index << "=" <<
                renderpass.framebufferHandles[
                    index].id << ";";
        }
    }

    stream << "renderSystem.generation=" <<
        RenderSystem::GetInstance()
            .GetActiveWorldGeneration() << ";";
    stream << "renderSystem.resolved=" <<
        RenderSystem::GetInstance()
            .GetResolvedShaderGenerationFingerprint() <<
        ";";
    stream << "controller.generation=" <<
        (engineLoop.controller
            ? engineLoop.controller
                  ->GetBoundWorldGeneration()
            : 0) << ";";
    if (includeFrameLifecycleDiagnostics)
    {
        stream << "renderSystem.pendingSnapshot=" <<
            RenderSystem::GetInstance()
                .HasPendingWorldSnapshotForTest() << ";";
        const WorldSnapshotPtr pendingSnapshot =
            RenderSystem::GetInstance()
                .PeekPendingWorldSnapshotForTest();
        stream << "renderSystem.pendingSnapshotGeneration=" <<
            (pendingSnapshot
                 ? pendingSnapshot->worldGeneration
                 : 0) << ";";
        stream << "renderSystem.pendingSnapshotFrame=" <<
            (pendingSnapshot
                 ? pendingSnapshot->frameIndex
                 : 0) << ";";
    }
    stream << "renderSystem.lightCapacity=" <<
        RenderSystem::GetInstance()
            .GetLightCapacityForTest() << ";";
    const auto& lightBufferHandles =
        RenderSystem::GetInstance()
            .GetLightBufferHandlesForTest();
    for (size_t index = 0;
         index < lightBufferHandles.size();
         ++index)
    {
        stream << "renderSystem.lightBuffer." <<
            index << "=" <<
            lightBufferHandles[index].id << ";";
    }
    stream << "pipelineFactory=" <<
        engineLoop.pipelineFactory
            ->CaptureIdentityFingerprintForTest(
                includeFrameLifecycleDiagnostics) << ";";
    const std::shared_ptr<Texture>* brdfLut =
        RendererResourceCache::GetInstance()
            .GetGlobalTexture("brdfLut");
    stream << "cache.global.brdfLut=" <<
        reinterpret_cast<std::uintptr_t>(
            brdfLut != nullptr && *brdfLut
                ? brdfLut->get()
                : nullptr) << ";";
    if (includeFrameLifecycleDiagnostics &&
        engineLoop.rendererBackend != nullptr)
    {
        const std::array<size_t, 9> backendCounts =
            CaptureBackendIdentityCounts(
                *engineLoop.rendererBackend);
        for (size_t index = 0;
             index < backendCounts.size();
             ++index)
        {
            stream << "backend.count." << index <<
                "=" << backendCounts[index] << ";";
        }
    }
    if (includeFrameLifecycleDiagnostics)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        stream << "retire.pending=" <<
            retireQueue.GetPendingCount() << ";";
        stream << "retire.submitted=" <<
            retireQueue.GetLastSubmittedEpoch() << ";";
        stream << "retire.completed=" <<
            retireQueue.GetLastCompletedEpoch() << ";";
    }

    const std::shared_ptr<Material> material =
        FindShaderReloadTestMaterial();
    const std::shared_ptr<MaterialInstance>
        materialInstance =
            FindShaderReloadTestMaterialInstance();
    const std::shared_ptr<
        RendererObjectResourceEntry>
            objectResources =
                FindShaderReloadTestObjectResources();
    if (!material || !materialInstance ||
        !objectResources)
    {
        throw std::runtime_error(
            "Cannot fingerprint the shader definition runtime package");
    }
    stream << "material.pointer=" <<
        reinterpret_cast<std::uintptr_t>(
            material.get()) << ";";
    stream << "material.surfacePipeline=" <<
        reinterpret_cast<std::uintptr_t>(
            material->GetRenderPipeline().get()) << ";";
    stream << "material.shadowPipeline=" <<
        reinterpret_cast<std::uintptr_t>(
            material->GetShadowPipeline().get()) << ";";
    stream << "material.surfaceGeneration=" <<
        material->GetSurfaceShaderArtifact()
            .artifactGenerationKey << ";";
    const auto& shadowArtifact =
        material->GetShadowShaderArtifact();
    stream << "material.shadowGeneration=" <<
        (shadowArtifact
            ? shadowArtifact
                  ->artifactGenerationKey
            : std::string()) << ";";
    AppendMaterialInstanceStateFingerprint(
        stream,
        materialInstance->CaptureStateSnapshot());
    const RendererObjectGpuResources& objectPackage =
        objectResources->GetResources();
    stream << "object.pointer=" <<
        reinterpret_cast<std::uintptr_t>(
            objectResources.get()) << ";";
    stream << "object.descriptorPool=" <<
        objectPackage.descriptorPoolHandle.id << ";";
    stream << "object.shadowDescriptorPool=" <<
        objectPackage.shadowDescriptorPoolHandle.id <<
        ";";
    for (size_t imageIndex = 0;
         imageIndex <
             objectPackage.descriptorSetHandles.size();
         ++imageIndex)
    {
        for (size_t setIndex = 0;
             setIndex <
                 objectPackage
                     .descriptorSetHandles[
                         imageIndex].size();
             ++setIndex)
        {
            stream << "object.descriptor." <<
                imageIndex << "." << setIndex <<
                "=" <<
                objectPackage
                    .descriptorSetHandles[
                        imageIndex][setIndex].id <<
                ";";
        }
    }

    const std::shared_ptr<MaterialInstance>
        batchInstance =
            FindShaderReloadBatchTestMaterialInstance();
    if (!batchInstance)
    {
        throw std::runtime_error(
            "Cannot fingerprint the secondary material definition package");
    }
    stream << "batchInstance.pointer=" <<
        reinterpret_cast<std::uintptr_t>(
            batchInstance.get()) << ";";
    AppendMaterialInstanceStateFingerprint(
        stream,
        batchInstance->CaptureStateSnapshot());

    const std::filesystem::path shaderRoot =
        engineLoop.shaderCompiler
            ->GetShaderRoot();
    stream << "artifact.manifest=" <<
        HashFileIfPresent(
            shaderRoot / "spv" /
            "shader-build-cache.json") << ";";
    const std::filesystem::path primaryInclude =
        std::filesystem::path(
            primaryDefinitionPath)
            .parent_path() /
        "generate" /
        "M_shaderReloadTestParamter.glsl";
    const std::filesystem::path batchInclude =
        std::filesystem::path(
            batchDefinitionPath)
            .parent_path() /
        "generate" /
        "M_shaderReloadBatchTestParamter.glsl";
    stream << "artifact.primaryInclude=" <<
        HashFileIfPresent(primaryInclude) << ";";
    stream << "artifact.batchInclude=" <<
        HashFileIfPresent(batchInclude) << ";";
    const std::string capturedDetails = stream.str();
    if (details != nullptr)
    {
        *details = capturedDetails;
    }
    return ContentHasher::HashString(
        capturedDetails).ToHex();
}

std::string
RuntimeTestHooks::CaptureShaderDefinitionRuntimeFingerprint(
    EngineLoop& engineLoop) const
{
    return CaptureWorldGraphRuntimeFingerprint(
        engineLoop,
        shaderDefinitionReloadTestSourcePath,
        shaderDefinitionReloadTestBatchSourcePath,
        false);
}

std::string
RuntimeTestHooks::CaptureShaderShutdownInflightFingerprint(
    EngineLoop& engineLoop) const
{
    if (engineLoop.shaderCompiler == nullptr ||
        engineLoop.pipelineFactory == nullptr ||
        engineLoop.rendererBackend == nullptr)
    {
        throw std::runtime_error(
            "Cannot capture shutdown-in-flight fingerprint before renderer initialization");
    }

    std::ostringstream stream;
    const WorldHandle& world =
        engineLoop.GetSubsystems()
            .GetWorldManager()
            .GetActiveWorldHandle();
    stream << "world.generation=" << world.generation << ";";
    stream << "world.path=" << world.scenePath << ";";
    stream << "renderSystem.generation=" <<
        RenderSystem::GetInstance()
            .GetActiveWorldGeneration() << ";";
    stream << "renderSystem.resolved=" <<
        RenderSystem::GetInstance()
            .GetResolvedShaderGenerationFingerprint() << ";";
    stream << "pipelineFactory=" <<
        engineLoop.pipelineFactory
            ->CaptureIdentityFingerprintForTest() << ";";

    const std::filesystem::path shaderRoot =
        engineLoop.shaderCompiler->GetShaderRoot();
    stream << "artifact.manifest=" <<
        HashFileIfPresent(
            shaderRoot / "spv" /
            "shader-build-cache.json") << ";";
    ShaderVariantKey uiVariant;
    uiVariant.shaderName = "uiOverlay";
    const ShaderBuildRequest uiRequest =
        engineLoop.shaderCompiler
            ->CreateGraphicsVariantBuildRequest(
                uiVariant);
    for (const auto& [role, outputPath] :
         uiRequest.outputPaths)
    {
        stream << "artifact." << role << "=" <<
            HashFileIfPresent(outputPath) << ";";
    }

    stream << "reload.committed=" <<
        engineLoop.latestAutoReloadCommittedGeneration << ";";
    return ContentHasher::HashString(
        stream.str()).ToHex();
}

RuntimeTestHooks::~RuntimeTestHooks()
{
    if (!shaderReloadTestSourcePath.empty() &&
        !shaderReloadTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderReloadTestSourcePath,
                shaderReloadTestOriginalSource);
        }
        catch (...)
        {
        }
    }
    if (!shaderReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderReloadTestFixtureDirectory,
            removeError);
    }
    if (!shaderAutoReloadTestSourcePath.empty() &&
        !shaderAutoReloadTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestSourcePath,
                shaderAutoReloadTestOriginalSource);
        }
        catch (...)
        {
        }
    }
    if (!shaderAutoReloadTestVertexSourcePath.empty() &&
        !shaderAutoReloadTestOriginalVertexSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestVertexSourcePath,
                shaderAutoReloadTestOriginalVertexSource);
        }
        catch (...)
        {
        }
    }
    if (!shaderAutoReloadTestSurfaceSourcePath.empty() &&
        !shaderAutoReloadTestOriginalSurfaceSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestSurfaceSourcePath,
                shaderAutoReloadTestOriginalSurfaceSource);
        }
        catch (...)
        {
        }
    }
    if (!shaderAutoReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderAutoReloadTestFixtureDirectory,
            removeError);
    }
    if (!shaderComputeSkyShSourcePath.empty() &&
        !shaderComputeSkyShOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderComputeSkyShSourcePath,
                shaderComputeSkyShOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderComputePrefilterSourcePath.empty() &&
        !shaderComputePrefilterOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderComputePrefilterSourcePath,
                shaderComputePrefilterOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderComputeReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderComputeReloadTestFixtureDirectory,
            removeError);
    }
    if (!shaderDefinitionReloadTestSourcePath.empty() &&
        !shaderDefinitionReloadTestOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderDefinitionReloadTestSourcePath,
                shaderDefinitionReloadTestOriginal);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    shaderDefinitionReloadTestSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (!shaderDefinitionReloadTestBatchSourcePath.empty() &&
        !shaderDefinitionReloadTestBatchOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderDefinitionReloadTestBatchSourcePath,
                shaderDefinitionReloadTestBatchOriginal);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    shaderDefinitionReloadTestBatchSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (!shaderDefinitionReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderDefinitionReloadTestFixtureDirectory,
            removeError);
    }
    if (!worldGraphTransactionTestSourcePath.empty() &&
        !worldGraphTransactionTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                worldGraphTransactionTestSourcePath,
                worldGraphTransactionTestOriginalSource);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    worldGraphTransactionTestSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (!worldGraphTransactionTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            worldGraphTransactionTestFixtureDirectory,
            removeError);
    }
    if (!shaderUiReloadTestVertexPath.empty() &&
        !shaderUiReloadTestVertexOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderUiReloadTestVertexPath,
                shaderUiReloadTestVertexOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderUiReloadTestFragmentPath.empty() &&
        !shaderUiReloadTestFragmentOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderUiReloadTestFragmentPath,
                shaderUiReloadTestFragmentOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderUiReloadTestFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            shaderUiReloadTestFixtureDirectory,
            removeError);
    }
    if (!shaderShutdownInflightSourcePath.empty() &&
        !shaderShutdownInflightOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderShutdownInflightSourcePath,
                shaderShutdownInflightOriginalSource);
        }
        catch (...)
        {
        }
    }
}

bool RuntimeTestHooks::BeginWorldReloadStress(
    std::string scenePath,
    int reloadCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (scenePath.empty())
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("World reload stress ignored because scene path is empty.");
        return false;
    }

    if (reloadCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("World reload stress ignored because reload count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    worldReloadStressScenePath = std::move(scenePath);
    totalWorldReloads = reloadCount;
    remainingWorldReloads = reloadCount;
    completedWorldReloads = 0;
    retireDrainFramesRemaining = 0;
    maxPendingRetiredResources = 0;
    waitingForWorldReloadResult = false;
    waitingForRetireDrain = false;
    worldReloadStressActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "World reload stress started: scene=" +
        worldReloadStressScenePath +
        ", count=" +
        std::to_string(totalWorldReloads));
    return true;
}

bool RuntimeTestHooks::BeginWorldReloadFailureRollbackTest(
    std::string scenePath,
    const DiagnosticsSubsystem& diagnostics,
    std::string expectedErrorCode)
{
    if (scenePath.empty())
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("World reload failure rollback test ignored because scene path is empty.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    failureRollbackScenePath = std::move(scenePath);
    failureRollbackExpectedErrorCode = std::move(expectedErrorCode);
    waitingForFailureRollbackResult = false;
    failureRollbackTestActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "World reload failure rollback test started: scene=" +
        failureRollbackScenePath);
    return true;
}

bool RuntimeTestHooks::BeginGeneratedMaterialFailureRollbackTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    try
    {
        generatedFailureFixtureDirectory.clear();
        const std::filesystem::path scenePath = CreateGeneratedMaterialFailureScene(resourcePath);
        generatedFailureFixtureDirectory = scenePath.parent_path().string();
        cleanupGeneratedFailureFixture = true;
        const bool started = BeginWorldReloadFailureRollbackTest(
            scenePath.string(),
            diagnostics,
            "Material.LoadFailed");
        if (!started)
        {
            CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
            cleanupGeneratedFailureFixture = false;
            generatedFailureFixtureDirectory.clear();
        }
        return started;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create generated material failure rollback fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
        cleanupGeneratedFailureFixture = false;
        generatedFailureFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginGeneratedMeshFailureRollbackTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    try
    {
        generatedFailureFixtureDirectory.clear();
        const std::filesystem::path scenePath = CreateGeneratedMeshFailureScene(resourcePath);
        generatedFailureFixtureDirectory = scenePath.parent_path().string();
        cleanupGeneratedFailureFixture = true;
        const bool started = BeginWorldReloadFailureRollbackTest(
            scenePath.string(),
            diagnostics,
            "Mesh.LoadFailed");
        if (!started)
        {
            CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
            cleanupGeneratedFailureFixture = false;
            generatedFailureFixtureDirectory.clear();
        }
        return started;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create generated mesh failure rollback fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
        cleanupGeneratedFailureFixture = false;
        generatedFailureFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginGeneratedTextureFailureRollbackTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    try
    {
        generatedFailureFixtureDirectory.clear();
        const std::filesystem::path scenePath = CreateGeneratedTextureFailureScene(resourcePath);
        generatedFailureFixtureDirectory = scenePath.parent_path().string();
        cleanupGeneratedFailureFixture = true;
        const bool started = BeginWorldReloadFailureRollbackTest(
            scenePath.string(),
            diagnostics,
            "Texture.LoadFailed");
        if (!started)
        {
            CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
            cleanupGeneratedFailureFixture = false;
            generatedFailureFixtureDirectory.clear();
        }
        return started;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create generated texture failure rollback fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
        cleanupGeneratedFailureFixture = false;
        generatedFailureFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginGeneratedHighLightReloadStress(
    const std::string& resourcePath,
    int reloadCount,
    const DiagnosticsSubsystem& diagnostics)
{
    try
    {
        generatedReloadStressFixtureDirectory.clear();
        const std::filesystem::path scenePath = CreateGeneratedHighLightStressScene(resourcePath);
        generatedReloadStressFixtureDirectory = scenePath.parent_path().string();
        cleanupGeneratedReloadStressFixture = true;

        const bool started = BeginWorldReloadStress(scenePath.string(), reloadCount, diagnostics);
        if (!started)
        {
            CleanupGeneratedRuntimeFixtureIfNeeded(
                cleanupGeneratedReloadStressFixture,
                generatedReloadStressFixtureDirectory,
                diagnostics);
        }
        return started;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create generated high-light reload stress fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixtureIfNeeded(
            cleanupGeneratedReloadStressFixture,
            generatedReloadStressFixtureDirectory,
            diagnostics);
        return false;
    }
}

bool RuntimeTestHooks::BeginResizeStress(
    int resizeCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (resizeCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("Resize stress ignored because resize count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    resizeStressTotal = resizeCount;
    resizeStressRemaining = resizeCount;
    resizeStressCompletedCount = 0;
    resizeStressActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "Resize stress started: count=" + std::to_string(resizeStressTotal));
    return true;
}

bool RuntimeTestHooks::BeginRenderGraphReloadStress(
    int reloadCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (reloadCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning(
            "Render graph reload stress ignored because reload count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    graphReloadStressTotal = reloadCount;
    graphReloadStressRemaining = reloadCount;
    graphReloadStressCompletedCount = 0;
    graphReloadStressWaitingForDrain = false;
    graphReloadRetireDrainFramesRemaining = 0;
    graphReloadMaxPendingRetiredResources = 0;
    graphReloadStressActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "Render graph reload stress started: count=" +
        std::to_string(graphReloadStressTotal));
    return true;
}

bool RuntimeTestHooks::BeginFrameSmokeTest(
    int frameCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (frameCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("Frame smoke test ignored because frame count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    frameSmokeTotal = frameCount;
    frameSmokeCompletedCount = 0;
    frameSmokeTotalMs = 0.0;
    frameSmokeMaxMs = 0.0;
    frameSmokeMinMs = std::numeric_limits<double>::max();
    frameSmokeIntervalFrameCount = 0;
    frameSmokeIntervalTotalMs = 0.0;
    frameSmokeIntervalMaxMs = 0.0;
    frameSmokeIntervalMinMs = std::numeric_limits<double>::max();
    frameSmokeIntervalRenderLoopTotalMs = 0.0;
    frameSmokeIntervalRenderLoopMaxMs = 0.0;
    frameSmokeActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "Frame smoke test started: frames=" + std::to_string(frameSmokeTotal));
    return true;
}

bool RuntimeTestHooks::BeginEnvironmentUpdateStress(
    int updateCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (updateCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning(
            "Environment update stress ignored because update count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    environmentUpdateStressTotal = updateCount;
    environmentUpdateStressCompletedCount = 0;
    environmentUpdateStressFrameBudget = std::max(300, (updateCount + 2) * 120);
    environmentUpdateStressPreviousActiveGeneration = 0;
    environmentUpdateStressObservedPreviousResources = false;
    waitingForProceduralSkyParametersResult = false;
    environmentUpdateStressBaselineTimingSamples.fill(0);
    environmentUpdateStressPrefilterMipCount = 0;
    environmentUpdateStressEnvironmentCubeIdentity = 0;
    environmentUpdateStressPrefilterCubeIdentity = 0;
    environmentUpdateStressPhase = EnvironmentUpdateStressPhase::WaitInitialGeneration;
    environmentUpdateStressActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "Environment update stress started: changes=" +
        std::to_string(environmentUpdateStressTotal));
    return true;
}

bool RuntimeTestHooks::BeginShaderReloadTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning(
            "A runtime validation test is already running.");
        return false;
    }

    try
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(
                CommonFunction::GetProjectPath()) /
            "shader" /
            "glsl" /
            "runtimeTest" /
            "shaderReloadTestShared.glsl";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        shaderReloadTestSourcePath =
            sourcePath.string();
        shaderReloadTestOriginalSource =
            ReadTextFileBytes(sourcePath);
        shaderReloadTestCompatibleSourceA =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.bgr");
        shaderReloadTestSyntaxErrorSource =
            BuildShaderReloadSyntaxErrorSource();
        shaderReloadTestCompatibleSourceB =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.5");
        shaderReloadTestAbiIncompatibleSource =
            BuildShaderReloadAbiIncompatibleSource();
        shaderReloadTestScenePath =
            scenePath.string();
        shaderReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderReloadTestRetireDrainFramesRemaining = 0;
        shaderReloadTestMaxPendingRetiredResources = 0;
        waitingForShaderReloadTestWorld = false;
        shaderReloadTestPhase =
            ShaderReloadTestPhase::WaitWorldLoad;
        shaderReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderReloadTestFixtureDirectory,
            diagnostics);
        shaderReloadTestFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginShaderAutoReloadTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning(
            "A runtime validation test is already running.");
        return false;
    }

    try
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(
                CommonFunction::GetProjectPath()) /
            "shader" /
            "glsl" /
            "runtimeTest" /
            "shaderReloadTestShared.glsl";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        shaderAutoReloadTestSourcePath =
            sourcePath.string();
        shaderAutoReloadTestVertexSourcePath =
            (std::filesystem::path(
                CommonFunction::GetProjectPath()) /
                "shader" / "glsl" / "runtimeTest" /
                "mf_shaderReloadTestVertex.glsl").string();
        shaderAutoReloadTestSurfaceSourcePath =
            (std::filesystem::path(
                CommonFunction::GetProjectPath()) /
                "shader" / "glsl" / "runtimeTest" /
                "mf_shaderReloadTestSurface.glsl").string();
        shaderAutoReloadTestOriginalSource =
            ReadTextFileBytes(sourcePath);
        shaderAutoReloadTestOriginalVertexSource =
            ReadTextFileBytes(shaderAutoReloadTestVertexSourcePath);
        shaderAutoReloadTestOriginalSurfaceSource =
            ReadTextFileBytes(shaderAutoReloadTestSurfaceSourcePath);
        shaderAutoReloadTestCompatibleSourceA =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.bgr * 0.9");
        shaderAutoReloadTestSyntaxErrorSource =
            BuildShaderReloadSyntaxErrorSource();
        shaderAutoReloadTestCompatibleSourceB =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.5");
        shaderAutoReloadTestRapidSourceC1 =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.75");
        shaderAutoReloadTestRapidSourceC2 =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.45");
        shaderAutoReloadTestRapidSourceC3 =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.25");
        shaderAutoReloadTestLeafSourceA =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalVertexSource,
                "return CreateDefaultMaterialVertex(vertexInput);",
                "MaterialVertex result = CreateDefaultMaterialVertex(vertexInput);\n"
                "    result.localPosition += vec3(0.0);\n"
                "    return result;");
        shaderAutoReloadTestLeafSourceB =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalVertexSource,
                "return CreateDefaultMaterialVertex(vertexInput);",
                "MaterialVertex result = CreateDefaultMaterialVertex(vertexInput);\n"
                "    result.localPosition += vec3(0.001);\n"
                "    return result;");
        shaderAutoReloadTestLeafSourceC =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalVertexSource,
                "return CreateDefaultMaterialVertex(vertexInput);",
                "MaterialVertex result = CreateDefaultMaterialVertex(vertexInput);\n"
                "    result.localPosition += vec3(0.002);\n"
                "    return result;");
        shaderAutoReloadTestSurfaceSourceA =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalSurfaceSource,
                "surface.opacity = u_reloadTestColor.a;",
                "surface.opacity = u_reloadTestColor.a * 0.99;");
        shaderAutoReloadTestSurfaceSourceB =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalSurfaceSource,
                "surface.opacity = u_reloadTestColor.a;",
                "surface.opacity = u_reloadTestColor.a * 0.98;");
        shaderAutoReloadTestSurfaceSourceC =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalSurfaceSource,
                "surface.opacity = u_reloadTestColor.a;",
                "surface.opacity = u_reloadTestColor.a * 0.97;");
        shaderAutoReloadTestMtimeOnlySource =
            shaderAutoReloadTestOriginalSource;
        shaderAutoReloadTestScenePath =
            scenePath.string();
        shaderAutoReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderAutoReloadTestDeadline = {};
        shaderAutoReloadTestRetireDrainFramesRemaining = 0;
        shaderAutoReloadTestMaxPendingRetiredResources = 0;
        shaderAutoReloadTestBaselineLatestGeneration = 0;
        shaderAutoReloadTestBaselineSurfaceGeneration.clear();
        shaderAutoReloadTestBaselineShadowGeneration.clear();
        shaderAutoReloadTestBaselineManifestDigest.clear();
        shaderAutoReloadTestBaselineResolvedGeneration.clear();
        shaderAutoReloadTestLastObservedSurfaceGeneration.clear();
        shaderAutoReloadTestCommitTransitions = 0;
        shaderAutoReloadTestPhaseEntryPending = true;
        shaderAutoReloadTestQueueManualReload = false;
        shaderAutoReloadTestManualReloadQueued = false;
        shaderAutoReloadTestWorker = nullptr;
        shaderAutoReloadTestUnionSources.clear();
        shaderAutoReloadTestLastSubmittedSources.clear();
        shaderAutoReloadTestBaselineObservedEpoch = 0;
        shaderAutoReloadTestBaselineSubmittedGeneration = 0;
        shaderAutoReloadTestBaselineCommittedGeneration = 0;
        shaderAutoReloadTestBaselineFailedGeneration = 0;
        shaderAutoReloadTestBaselineShadercInvocations = 0;
        shaderAutoReloadTestBaselineMonitorScanCount = 0;
        shaderAutoReloadTestGateGeneration = 0;
        shaderAutoReloadTestDeleteRejectedGeneration = 0;
        waitingForShaderAutoReloadTestWorld = false;
        shaderAutoReloadTestPhase =
            ShaderAutoReloadTestPhase::WaitWorldLoad;
        shaderAutoReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader auto reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader auto reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderAutoReloadTestFixtureDirectory,
            diagnostics);
        shaderAutoReloadTestFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginShaderComputeReloadTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning(
            "A runtime validation test is already running.");
        return false;
    }

    try
    {
        const std::filesystem::path shaderRoot =
            std::filesystem::path(CommonFunction::GetProjectPath()) /
            "shader" /
            "glsl";
        const std::filesystem::path skyShPath =
            shaderRoot / "generator" / "skySHGenerate.comp";
        const std::filesystem::path prefilterPath =
            shaderRoot / "generator" / "prefilterEnvMap.comp";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        shaderComputeSkyShSourcePath = skyShPath.string();
        shaderComputePrefilterSourcePath = prefilterPath.string();
        shaderComputeSkyShOriginal =
            ReadTextFileBytes(skyShPath);
        shaderComputePrefilterOriginal =
            ReadTextFileBytes(prefilterPath);
        shaderComputeSkyShCompatible =
            ReplaceFirstOccurrence(
                shaderComputeSkyShOriginal,
                "vec3 radiance = textureLod(inSkyCube, dir, 0.0).rgb;",
                "vec3 radiance = textureLod(inSkyCube, dir, 0.0).rgb * 0.9995;");
        shaderComputePrefilterCompatible =
            ReplaceFirstOccurrence(
                shaderComputePrefilterOriginal,
                "imageStore(outPrefilteredEnvironmentCube, ivec3(pixelCoord, faceIndex), vec4(color, 1.0));",
                "imageStore(outPrefilteredEnvironmentCube, ivec3(pixelCoord, faceIndex), vec4(color * 0.9995, 1.0));");
        shaderComputeSkyShAbiIncompatible =
            ReplaceFirstOccurrence(
                shaderComputeSkyShOriginal,
                "layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;",
                "layout(local_size_x = 8, local_size_y = 16, local_size_z = 1) in;");

        shaderComputeReloadTestScenePath = scenePath.string();
        shaderComputeReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderComputeReloadTestDeadline = {};
        shaderComputeReloadTestRetireDrainFramesRemaining = 0;
        shaderComputeReloadTestMaxPendingRetiredResources = 0;
        shaderComputeReloadTestBaselineLatestGeneration = 0;
        shaderComputeSkyShBaselineGeneration.clear();
        shaderComputePrefilterBaselineGeneration.clear();
        waitingForShaderComputeReloadTestWorld = false;
        shaderComputeReloadTestPhase =
            ShaderComputeReloadTestPhase::WaitWorldLoad;
        shaderComputeReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader compute reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader compute reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderComputeReloadTestFixtureDirectory,
            diagnostics);
        shaderComputeReloadTestFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginShaderDefinitionReloadTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning(
            "A runtime validation test is already running.");
        return false;
    }

    try
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(CommonFunction::GetProjectPath()) /
            "shader" /
            "glsl" /
            "runtimeTest" /
            "M_shaderReloadTest.json";
        const std::filesystem::path batchSourcePath =
            sourcePath.parent_path() /
            "M_shaderReloadBatchTest.json";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        shaderDefinitionReloadTestSourcePath =
            sourcePath.string();
        shaderDefinitionReloadTestBatchSourcePath =
            batchSourcePath.string();
        shaderDefinitionReloadTestOriginal =
            ReadTextFileBytes(sourcePath);
        shaderDefinitionReloadTestBatchOriginal =
            ReadTextFileBytes(batchSourcePath);
        nlohmann::json extendedJson =
            nlohmann::json::parse(
                shaderDefinitionReloadTestOriginal);
        extendedJson["parameters"]["u_reloadExtra"] = {
            {"type", "float"},
            {"default", 0.5}};
        extendedJson["parameters"]["u_reloadRemoved"] = {
            {"type", "float"},
            {"default", 0.375}};
        shaderDefinitionReloadTestExtended =
            extendedJson.dump(2) + "\n";

        nlohmann::json deletedJson =
            extendedJson;
        deletedJson["parameters"].erase(
            "u_reloadRemoved");
        deletedJson["textures"].erase(
            "u_zReloadRetiredTexture");
        shaderDefinitionReloadTestDeleted =
            deletedJson.dump(2) + "\n";

        nlohmann::json mismatchJson =
            deletedJson;
        mismatchJson["parameters"]["u_reloadRuntimeScalar"] = {
            {"type", "vec2"},
            {"default", nlohmann::json::array({0.125, 0.25})}};
        shaderDefinitionReloadTestTypeMismatch =
            mismatchJson.dump(2) + "\n";

        nlohmann::json requiredTextureJson =
            deletedJson;
        requiredTextureJson["macros"][
            "USE_RELOAD_REQUIRED_TEXTURE"] = 1;
        requiredTextureJson["textures"][
            "u_zReloadRequiredTexture"] = {
                {"type", "sampler2D"},
                {"default", nullptr}};
        shaderDefinitionReloadTestRequiredTextureMissing =
            requiredTextureJson.dump(2) + "\n";

        shaderDefinitionReloadTestMalformed =
            "{\n  \"name\": \"M_shaderReloadTest\",\n";

        nlohmann::json includeFailureJson =
            deletedJson;
        includeFailureJson["parameters"][
            "u_reloadIncludeFailure"] = {
                {"type", "float"},
                {"default", "not-a-float"}};
        shaderDefinitionReloadTestIncludeGenerationFailure =
            includeFailureJson.dump(2) + "\n";

        nlohmann::json multiMainJson =
            deletedJson;
        multiMainJson["parameters"][
            "u_reloadMultiMain"] = {
                {"type", "float"},
                {"default", 0.625}};
        shaderDefinitionReloadTestMultiMain =
            multiMainJson.dump(2) + "\n";

        nlohmann::json multiBatchJson =
            nlohmann::json::parse(
                shaderDefinitionReloadTestBatchOriginal);
        multiBatchJson["parameters"][
            "u_reloadBatchExtra"] = {
                {"type", "float"},
                {"default", 0.75}};
        shaderDefinitionReloadTestMultiBatchValid =
            multiBatchJson.dump(2) + "\n";

        nlohmann::json multiInvalidBatch =
            multiBatchJson;
        multiInvalidBatch["parameters"][
            "u_reloadBatchInvalid"] = {
                {"type", "vec3"},
                {"default", 1.0}};
        shaderDefinitionReloadTestMultiBatchInvalid =
            multiInvalidBatch.dump(2) + "\n";

        shaderDefinitionReloadTestScenePath =
            scenePath.string();
        shaderDefinitionReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderDefinitionReloadTestDeadline = {};
        shaderDefinitionReloadTestRetireDrainFramesRemaining = 0;
        shaderDefinitionReloadTestMaxPendingRetiredResources = 0;
        shaderDefinitionReloadTestBaselineWorldGeneration = 0;
        shaderDefinitionReloadTestBaselineFailedGeneration = 0;
        shaderDefinitionReloadTestBaselineCommittedGeneration = 0;
        shaderDefinitionReloadTestBaselineParameterCount = 0;
        shaderDefinitionReloadTestBaselineFingerprint.clear();
        shaderDefinitionReloadTestOldMaterial.reset();
        shaderDefinitionReloadTestOldMaterialInstance.reset();
        shaderDefinitionReloadTestOldObjectResources.reset();
        shaderDefinitionReloadTestOldPrimaryTexture.reset();
        shaderDefinitionReloadTestOldRetiredTexture.reset();
        shaderDefinitionReloadTestRetainedTexture.reset();
        shaderDefinitionReloadTestRetainedTextureIdentity = 0;
        shaderDefinitionReloadTestRetainedTextureAssetIdentity.clear();
        shaderDefinitionReloadTestInitialDescriptorPoolIdentity = 0;
        waitingForShaderDefinitionReloadTestWorld = false;
        shaderDefinitionReloadTestPhase =
            ShaderDefinitionReloadTestPhase::WaitWorldLoad;
        shaderDefinitionReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader material definition reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader definition reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderDefinitionReloadTestFixtureDirectory,
            diagnostics);
        shaderDefinitionReloadTestFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginWorldGraphTransactionTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning(
            "A runtime validation test is already running.");
        return false;
    }

    try
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(
                CommonFunction::GetProjectPath()) /
            "shader" / "glsl" / "runtimeTest" /
            "M_shaderReloadTest.json";
        const std::filesystem::path batchSourcePath =
            sourcePath.parent_path() /
            "M_shaderReloadBatchTest.json";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        worldGraphTransactionTestSourcePath =
            sourcePath.string();
        worldGraphTransactionTestBatchSourcePath =
            batchSourcePath.string();
        worldGraphTransactionTestOriginalSource =
            ReadTextFileBytes(sourcePath);
        nlohmann::json candidateJson =
            nlohmann::json::parse(
                worldGraphTransactionTestOriginalSource);
        candidateJson["parameters"][
            "u_worldGraphTransactionCandidate"] = {
                {"type", "float"},
                {"default", 0.8125}};
        worldGraphTransactionTestCandidateSource =
            candidateJson.dump(2) + "\n";
        worldGraphTransactionTestScenePath =
            scenePath.string();
        worldGraphTransactionTestHighLightScenePath.clear();
        worldGraphTransactionTestFixtureDirectory =
            scenePath.parent_path().string();
        worldGraphTransactionTestMonitorSuspended = false;
        worldGraphTransactionTestFramesUntilNextPhase = 0;
        worldGraphTransactionTestRetireDrainFramesRemaining = 0;
        worldGraphTransactionTestMaxPendingRetiredResources = 0;
        worldGraphTransactionTestNextBatchId = 1;
        worldGraphTransactionTestGenerationBeforeSuccess = 0;
        worldGraphTransactionTestBackendCountsBeforeSuccess = {};
        worldGraphTransactionTestImageResourceNamesBeforeSuccess.clear();
        worldGraphTransactionTestOldWorld.reset();
        worldGraphTransactionTestOldWorldPackage.reset();
        worldGraphTransactionTestOldGraphPackage.reset();
        worldGraphTransactionTestGraphReloadPackage.reset();
        worldGraphTransactionTestOldLightBuffer.reset();
        worldGraphTransactionTestOldMaterial.reset();
        worldGraphTransactionTestOldMaterialInstance.reset();
        worldGraphTransactionTestOldObjectResources.reset();
        worldGraphTransactionTestOldTexture.reset();
        worldGraphTransactionTestResourcesExpectedToExpire.clear();
        waitingForWorldGraphTransactionTestWorld = false;
        worldGraphTransactionTestPhase =
            WorldGraphTransactionTestPhase::WaitWorldLoad;
        worldGraphTransactionTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "World/graph transaction runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create World/graph transaction runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            worldGraphTransactionTestFixtureDirectory,
            diagnostics);
        worldGraphTransactionTestFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginShaderUiReloadTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning(
            "A runtime validation test is already running.");
        return false;
    }

    try
    {
        const std::filesystem::path shaderRoot =
            std::filesystem::path(CommonFunction::GetProjectPath()) /
            "shader" /
            "glsl";
        const std::filesystem::path vertexPath =
            shaderRoot / "uiOverlay.vert";
        const std::filesystem::path fragmentPath =
            shaderRoot / "uiOverlay.frag";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        shaderUiReloadTestVertexPath = vertexPath.string();
        shaderUiReloadTestFragmentPath = fragmentPath.string();
        shaderUiReloadTestVertexOriginal =
            ReadTextFileBytes(vertexPath);
        shaderUiReloadTestFragmentOriginal =
            ReadTextFileBytes(fragmentPath);
        shaderUiReloadTestFragmentCompatible =
            ReplaceFirstOccurrence(
                shaderUiReloadTestFragmentOriginal,
                "outColor = texture(uiTexture, inTexCoord) * inColor;",
                "outColor = texture(uiTexture, inTexCoord) * inColor * 0.9995;");
        shaderUiReloadTestFragmentAbiIncompatible =
            ReplaceFirstOccurrence(
                ReplaceFirstOccurrence(
                    shaderUiReloadTestFragmentOriginal,
                    "layout(location = 0) out vec4 outColor;",
                    "layout(location = 0) out vec3 outColor;"),
                "outColor = texture(uiTexture, inTexCoord) * inColor;",
                "outColor = (texture(uiTexture, inTexCoord) * inColor).rgb;");

        shaderUiReloadTestScenePath = scenePath.string();
        shaderUiReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderUiReloadTestDeadline = {};
        shaderUiReloadTestRetireDrainFramesRemaining = 0;
        shaderUiReloadTestMaxPendingRetiredResources = 0;
        shaderUiReloadTestBaselineLatestGeneration = 0;
        shaderUiReloadTestPhaseEntryPending = false;
        waitingForShaderUiReloadTestWorld = false;
        shaderUiReloadTestPhase =
            ShaderUiReloadTestPhase::WaitWorldLoad;
        shaderUiReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader UI overlay reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader UI reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderUiReloadTestFixtureDirectory,
            diagnostics);
        shaderUiReloadTestFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginShaderShutdownInflightTest(
    const DiagnosticsSubsystem& diagnostics)
{
    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning(
            "A runtime validation test is already running.");
        return false;
    }

    try
    {
        const std::filesystem::path sourcePath =
            std::filesystem::path(
                CommonFunction::GetProjectPath()) /
            "shader" / "glsl" / "uiOverlay.frag";
        shaderShutdownInflightSourcePath =
            sourcePath.string();
        shaderShutdownInflightOriginalSource =
            ReadTextFileBytes(sourcePath);
        shaderShutdownInflightCandidateSource =
            ReplaceFirstOccurrence(
                shaderShutdownInflightOriginalSource,
                "outColor = texture(uiTexture, inTexCoord) * inColor;",
                "outColor = texture(uiTexture, inTexCoord) * inColor * 0.99925;");
        shaderShutdownInflightBaselineFingerprint.clear();
        shaderShutdownInflightBaselineCommittedGeneration = 0;
        shaderShutdownInflightCandidateGeneration = 0;
        shaderShutdownInflightDeadline =
            std::chrono::steady_clock::now() +
            ShaderAsyncWaitTimeout;
        shaderShutdownInflightTestPhase =
            ShaderShutdownInflightTestPhase::WaitMonitorBaseline;
        shaderShutdownInflightTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader shutdown-in-flight runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to initialize shader shutdown-in-flight test: ") +
            exception.what());
        return false;
    }
}

void RuntimeTestHooks::Update(
    CommandBus& commandBus,
    const WorldManager& worldManager,
    const EnvironmentUpdateDiagnostics& environmentDiagnostics,
    const DiagnosticsSubsystem& diagnostics)
{
    if (shaderReloadTestActive &&
        shaderReloadTestPhase ==
            ShaderReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader reload runtime test queued its fixture World.");
        return;
    }

    if (shaderAutoReloadTestActive &&
        shaderAutoReloadTestPhase ==
            ShaderAutoReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderAutoReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderAutoReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-auto-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderAutoReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader auto reload runtime test queued its fixture World.");
        return;
    }

    if (shaderComputeReloadTestActive &&
        shaderComputeReloadTestPhase ==
            ShaderComputeReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderComputeReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderComputeReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-compute-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderComputeReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader compute reload runtime test queued its fixture World.");
        return;
    }

    if (shaderDefinitionReloadTestActive &&
        shaderDefinitionReloadTestPhase ==
            ShaderDefinitionReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderDefinitionReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderDefinitionReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-definition-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderDefinitionReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader definition reload runtime test queued its fixture World.");
        return;
    }

    if (worldGraphTransactionTestActive &&
        worldGraphTransactionTestPhase ==
            WorldGraphTransactionTestPhase::WaitWorldLoad &&
        !waitingForWorldGraphTransactionTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue =
            worldGraphTransactionTestScenePath;
        command.sourceText =
            "runtime-test: world-graph-transaction";
        commandBus.Queue(std::move(command));
        waitingForWorldGraphTransactionTestWorld = true;
        diagnostics.ReportInfo(
            "World/graph transaction runtime test queued its fixture World.");
        return;
    }

    if (shaderUiReloadTestActive &&
        shaderUiReloadTestPhase ==
            ShaderUiReloadTestPhase::WaitWorldLoad &&
        !waitingForShaderUiReloadTestWorld)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = shaderUiReloadTestScenePath;
        command.sourceText =
            "runtime-test: shader-ui-reload";
        commandBus.Queue(std::move(command));
        waitingForShaderUiReloadTestWorld = true;
        diagnostics.ReportInfo(
            "Shader UI overlay reload runtime test queued its fixture World.");
        return;
    }

    if (environmentUpdateStressActive)
    {
        UpdateEnvironmentUpdateStress(
            commandBus,
            worldManager,
            environmentDiagnostics,
            diagnostics);
        return;
    }

    if (worldReloadStressActive &&
        waitingForRetireDrain)
    {
        ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
        const size_t pendingRetiredResources = retireQueue.GetPendingCount();
        UpdateMaxPendingRetiredResources(pendingRetiredResources, maxPendingRetiredResources);

        if (pendingRetiredResources == 0)
        {
            if (totalWorldReloads > 1 && maxPendingRetiredResources == 0)
            {
                worldReloadStressActive = false;
                waitingForRetireDrain = false;
                runtimeTestStatus = RuntimeTestStatus::Failed;
                diagnostics.ReportError(
                    "World reload stress failed because no retired world-local resources were observed after repeated reloads.");
                CleanupGeneratedRuntimeFixtureIfNeeded(
                    cleanupGeneratedReloadStressFixture,
                    generatedReloadStressFixtureDirectory,
                    diagnostics);
                return;
            }

            worldReloadStressActive = false;
            waitingForRetireDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            diagnostics.ReportInfo(
                "World reload stress completed: " +
                std::to_string(completedWorldReloads) +
                "/" +
                std::to_string(totalWorldReloads) +
                " reloads succeeded, retire queue max pending=" +
                std::to_string(maxPendingRetiredResources) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
            CleanupGeneratedRuntimeFixtureIfNeeded(
                cleanupGeneratedReloadStressFixture,
                generatedReloadStressFixtureDirectory,
                diagnostics);
            return;
        }

        --retireDrainFramesRemaining;
        if (retireDrainFramesRemaining <= 0)
        {
            worldReloadStressActive = false;
            waitingForRetireDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload stress failed because retired world-local resources did not drain before the frame budget expired. pending=" +
                std::to_string(pendingRetiredResources) +
                ", submittedEpoch=" +
                std::to_string(retireQueue.GetLastSubmittedEpoch()) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
            CleanupGeneratedRuntimeFixtureIfNeeded(
                cleanupGeneratedReloadStressFixture,
                generatedReloadStressFixtureDirectory,
                diagnostics);
        }
        return;
    }

    if (worldReloadStressActive &&
        !waitingForWorldReloadResult &&
        remainingWorldReloads > 0)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = worldReloadStressScenePath;
        command.sourceText = "runtime-test: reloadstress";
        commandBus.Queue(std::move(command));

        waitingForWorldReloadResult = true;
        --remainingWorldReloads;

        diagnostics.ReportInfo(
            "World reload stress queued load " +
            std::to_string(completedWorldReloads + 1) +
            "/" +
            std::to_string(totalWorldReloads));
        return;
    }

    if (!failureRollbackTestActive || waitingForFailureRollbackResult)
    {
        return;
    }

    RuntimeCommand command;
    command.type = RuntimeCommandType::LoadWorld;
    command.stringValue = failureRollbackScenePath;
    command.sourceText = "runtime-test: reloadfail";
    commandBus.Queue(std::move(command));

    waitingForFailureRollbackResult = true;
    diagnostics.ReportInfo(
        "World reload failure rollback test queued expected-failure load: " +
        failureRollbackScenePath);
}

void RuntimeTestHooks::UpdateEngineLoopTests(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (shaderShutdownInflightTestActive)
    {
        UpdateShaderShutdownInflightTest(
            engineLoop,
            diagnostics);
        return;
    }

    if (shaderReloadTestActive &&
        shaderReloadTestPhase !=
            ShaderReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderReloadTest(
            engineLoop,
            diagnostics);
        return;
    }

    if (shaderAutoReloadTestActive &&
        shaderAutoReloadTestPhase !=
            ShaderAutoReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderAutoReloadTest(
            engineLoop,
            diagnostics);
        return;
    }

    if (shaderComputeReloadTestActive &&
        shaderComputeReloadTestPhase !=
            ShaderComputeReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderComputeReloadTest(
            engineLoop,
            diagnostics);
        return;
    }

    if (shaderDefinitionReloadTestActive &&
        shaderDefinitionReloadTestPhase !=
            ShaderDefinitionReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderDefinitionReloadTest(
            engineLoop,
            diagnostics);
        return;
    }

    if (worldGraphTransactionTestActive &&
        worldGraphTransactionTestPhase !=
            WorldGraphTransactionTestPhase::WaitWorldLoad)
    {
        UpdateWorldGraphTransactionTest(
            engineLoop,
            diagnostics);
        return;
    }

    if (shaderUiReloadTestActive &&
        shaderUiReloadTestPhase !=
            ShaderUiReloadTestPhase::WaitWorldLoad)
    {
        UpdateShaderUiReloadTest(
            engineLoop,
            diagnostics);
        return;
    }

    if (resizeStressActive)
    {
        UpdateResizeStress(engineLoop, diagnostics);
        return;
    }

    if (graphReloadStressActive)
    {
        UpdateRenderGraphReloadStress(engineLoop, diagnostics);
    }
}

void RuntimeTestHooks::UpdateShaderReloadTest(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderReloadTestActive)
    {
        return;
    }

    if (shaderReloadTestPhase ==
        ShaderReloadTestPhase::WaitRetireDrain)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const size_t pending = retireQueue.GetPendingCount();
        shaderReloadTestMaxPendingRetiredResources =
            std::max(
                shaderReloadTestMaxPendingRetiredResources,
                pending);
        if (pending == 0)
        {
            if (shaderReloadTestMaxPendingRetiredResources == 0)
            {
                FailShaderReloadTest(
                    "Shader reload runtime test did not observe any "
                    "epoch-retired pipelines.",
                    diagnostics);
                return;
            }

            shaderReloadTestActive = false;
            shaderReloadTestPhase =
                ShaderReloadTestPhase::Idle;
            runtimeTestStatus =
                RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                shaderReloadTestFixtureDirectory,
                diagnostics);
            shaderReloadTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "Shader reload runtime test completed: "
                "compatible Surface/Shadow commits, compile rollback, "
                "ABI rejection, pipeline creation rollback, recovery, "
                "formal artifact restoration, and epoch retirement passed.");
            return;
        }

        --shaderReloadTestRetireDrainFramesRemaining;
        if (shaderReloadTestRetireDrainFramesRemaining <= 0)
        {
            FailShaderReloadTest(
                "Shader reload runtime test timed out waiting for "
                "retired pipelines to drain. pending=" +
                    std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        const ShaderReloadRuntimeSnapshot before =
            CaptureShaderReloadRuntimeSnapshot();
        std::string nextSource;
        bool expectSuccess = false;
        bool injectPipelineFailure = false;
        const char* expectedFailureText = nullptr;
        const char* phaseName = nullptr;

        switch (shaderReloadTestPhase)
        {
        case ShaderReloadTestPhase::CompatibleCommit:
            nextSource = shaderReloadTestCompatibleSourceA;
            expectSuccess = true;
            phaseName = "compatible commit";
            break;
        case ShaderReloadTestPhase::SyntaxFailure:
            nextSource = shaderReloadTestSyntaxErrorSource;
            expectedFailureText = "Shader compile failed";
            phaseName = "syntax failure rollback";
            break;
        case ShaderReloadTestPhase::SyntaxRecovery:
            nextSource = shaderReloadTestCompatibleSourceB;
            expectSuccess = true;
            phaseName = "syntax recovery";
            break;
        case ShaderReloadTestPhase::AbiRejection:
            nextSource =
                shaderReloadTestAbiIncompatibleSource;
            expectedFailureText =
                "ABI changed";
            phaseName = "ABI rejection";
            break;
        case ShaderReloadTestPhase::PipelineFailure:
            nextSource =
                shaderReloadTestCompatibleSourceA;
            injectPipelineFailure = true;
            expectedFailureText =
                "Injected graphics pipeline creation failure";
            phaseName = "pipeline creation rollback";
            break;
        case ShaderReloadTestPhase::PipelineRecovery:
            nextSource =
                shaderReloadTestCompatibleSourceA;
            expectSuccess = true;
            phaseName = "pipeline creation recovery";
            break;
        case ShaderReloadTestPhase::RestoreOriginal:
            nextSource = shaderReloadTestOriginalSource;
            expectSuccess = true;
            phaseName = "original source restore";
            break;
        case ShaderReloadTestPhase::Idle:
        case ShaderReloadTestPhase::WaitWorldLoad:
        case ShaderReloadTestPhase::WaitRetireDrain:
            return;
        }

        WriteTextFileAtomically(
            shaderReloadTestSourcePath,
            nextSource);

        const uint64_t generation =
            engineLoop.nextShaderReloadGeneration++;
        const uint64_t worldGeneration =
            engineLoop.GetSubsystems()
                .GetWorldManager()
                .GetActiveWorldHandle()
                .generation;
        ShaderReloadPlan plan =
            engineLoop.shaderReloadCoordinator
                ->CaptureGraphicsPlanForSources(
                    {"runtimeTest/shaderReloadTestShared.glsl"},
                    generation,
                    worldGeneration);
        if (plan.builds.size() != 2 ||
            plan.materials.size() != 1)
        {
            throw std::runtime_error(
                "Shader reload runtime test expected one live Material "
                "and two affected Surface/Shadow builds");
        }

        ShaderReloadCandidateBatch batch;
        bool operationSucceeded = false;
        std::string failureMessage;
        ShaderReloadCommitStatistics statistics;
        try
        {
            batch = engineLoop.shaderReloadCoordinator
                ->CompileGraphicsCandidates(
                    std::move(plan));
            engineLoop.WaitForRenderThreadIdle();
            if (engineLoop.shouldClose)
            {
                throw std::runtime_error(
                    "Render thread failed during shader reload test");
            }
            if (injectPipelineFailure)
            {
                PipelineFactory::TestFaultInjection injection;
                injection.failGraphicsPipelineCreationAt = 2;
                engineLoop.pipelineFactory
                    ->SetTestFaultInjection(injection);
            }
            statistics =
                engineLoop.shaderReloadCoordinator
                    ->CommitGraphicsCandidates(
                        batch,
                        worldGeneration);
            engineLoop.pipelineFactory->SetTestFaultInjection({});
            RenderSystem::GetInstance()
                .RefreshResolvedSceneAfterShaderReload();
            operationSucceeded = true;
        }
        catch (const std::exception& exception)
        {
            engineLoop.pipelineFactory->SetTestFaultInjection({});
            failureMessage = exception.what();
        }

        const ShaderReloadRuntimeSnapshot after =
            CaptureShaderReloadRuntimeSnapshot();
        if (expectSuccess)
        {
            if (!operationSucceeded)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " unexpectedly failed: " +
                    failureMessage);
            }
            if (!statistics.committed ||
                statistics.affectedBuildCount != 2 ||
                statistics.pipelinesCreated != 2 ||
                statistics.pipelinesRetired != 2)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " did not commit the complete Surface/Shadow batch");
            }
            if (before.surfacePipeline ==
                    after.surfacePipeline ||
                before.shadowPipeline ==
                    after.shadowPipeline ||
                before.surfaceGeneration ==
                    after.surfaceGeneration ||
                before.shadowGeneration ==
                    after.shadowGeneration)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " did not replace both live Pipeline generations");
            }
            diagnostics.ReportInfo(
                std::string("Shader reload runtime test passed ") +
                phaseName +
                ": builds=2, pipelinesCreated=2, retired=2.");
        }
        else
        {
            if (operationSucceeded)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " unexpectedly committed");
            }
            if (expectedFailureText == nullptr ||
                failureMessage.find(expectedFailureText) ==
                    std::string::npos)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " reported an unexpected error: " +
                    failureMessage);
            }
            if (!SameShaderReloadRuntimeSnapshot(
                    before,
                    after))
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " changed a live Pipeline or formal artifact");
            }
            diagnostics.ReportInfo(
                std::string("Shader reload runtime test passed ") +
                phaseName +
                ": old Pipeline and formal artifact remained active.");
        }

        switch (shaderReloadTestPhase)
        {
        case ShaderReloadTestPhase::CompatibleCommit:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::SyntaxFailure;
            break;
        case ShaderReloadTestPhase::SyntaxFailure:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::SyntaxRecovery;
            break;
        case ShaderReloadTestPhase::SyntaxRecovery:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::AbiRejection;
            break;
        case ShaderReloadTestPhase::AbiRejection:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::PipelineFailure;
            break;
        case ShaderReloadTestPhase::PipelineFailure:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::PipelineRecovery;
            break;
        case ShaderReloadTestPhase::PipelineRecovery:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::RestoreOriginal;
            break;
        case ShaderReloadTestPhase::RestoreOriginal:
            shaderReloadTestSourcePath.clear();
            shaderReloadTestOriginalSource.clear();
            shaderReloadTestPhase =
                ShaderReloadTestPhase::WaitRetireDrain;
            shaderReloadTestRetireDrainFramesRemaining =
                RetireDrainFrameBudget;
            shaderReloadTestMaxPendingRetiredResources =
                ResourceRetireQueue::GetInstance()
                    .GetPendingCount();
            break;
        case ShaderReloadTestPhase::Idle:
        case ShaderReloadTestPhase::WaitWorldLoad:
        case ShaderReloadTestPhase::WaitRetireDrain:
            break;
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderReloadTest(
            std::string(
                "Shader reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

void RuntimeTestHooks::FailShaderReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderReloadTestSourcePath.empty() &&
        !shaderReloadTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderReloadTestSourcePath,
                shaderReloadTestOriginalSource);
        }
        catch (const std::exception& exception)
        {
            diagnostics.ReportError(
                std::string(
                    "Shader reload runtime test could not restore "
                    "the source fixture: ") +
                exception.what());
        }
    }
    shaderReloadTestSourcePath.clear();
    shaderReloadTestOriginalSource.clear();
    shaderReloadTestActive = false;
    waitingForShaderReloadTestWorld = false;
    shaderReloadTestPhase =
        ShaderReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderReloadTestFixtureDirectory,
        diagnostics);
    shaderReloadTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::FailShaderAutoReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (shaderAutoReloadTestWorker != nullptr)
    {
        shaderAutoReloadTestWorker->DisableTestCompileGate();
        shaderAutoReloadTestWorker = nullptr;
    }
    if (!shaderAutoReloadTestSourcePath.empty() &&
        !shaderAutoReloadTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestSourcePath,
                shaderAutoReloadTestOriginalSource);
        }
        catch (const std::exception& exception)
        {
            diagnostics.ReportError(
                std::string(
                    "Shader auto reload runtime test could not restore "
                    "the source fixture: ") +
                exception.what());
        }
    }
    if (!shaderAutoReloadTestVertexSourcePath.empty() &&
        !shaderAutoReloadTestOriginalVertexSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestVertexSourcePath,
                shaderAutoReloadTestOriginalVertexSource);
        }
        catch (const std::exception& exception)
        {
            diagnostics.ReportError(
                std::string(
                    "Shader auto reload runtime test could not restore "
                    "the vertex fixture: ") +
                exception.what());
        }
    }
    if (!shaderAutoReloadTestSurfaceSourcePath.empty() &&
        !shaderAutoReloadTestOriginalSurfaceSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestSurfaceSourcePath,
                shaderAutoReloadTestOriginalSurfaceSource);
        }
        catch (const std::exception& exception)
        {
            diagnostics.ReportError(
                std::string(
                    "Shader auto reload runtime test could not restore "
                    "the surface fixture: ") +
                exception.what());
        }
    }
    shaderAutoReloadTestSourcePath.clear();
    shaderAutoReloadTestVertexSourcePath.clear();
    shaderAutoReloadTestSurfaceSourcePath.clear();
    shaderAutoReloadTestOriginalSource.clear();
    shaderAutoReloadTestOriginalVertexSource.clear();
    shaderAutoReloadTestOriginalSurfaceSource.clear();
    shaderAutoReloadTestActive = false;
    waitingForShaderAutoReloadTestWorld = false;
    shaderAutoReloadTestPhaseEntryPending = false;
    shaderAutoReloadTestPhase =
        ShaderAutoReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderAutoReloadTestFixtureDirectory,
        diagnostics);
    shaderAutoReloadTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::UpdateShaderAutoReloadTest(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderAutoReloadTestActive)
    {
        return;
    }

    const std::string sharedIdentity =
        "runtimeTest/shaderReloadTestShared.glsl";
    const std::string vertexIdentity =
        "runtimeTest/mf_shaderReloadTestVertex.glsl";
    const std::string surfaceIdentity =
        "runtimeTest/mf_shaderReloadTestSurface.glsl";
    const std::vector<std::string> unionSources = {
        sharedIdentity,
        surfaceIdentity,
        vertexIdentity};

    try
    {
        if (shaderAutoReloadTestPhase ==
            ShaderAutoReloadTestPhase::WaitRetireDrain)
        {
            ResourceRetireQueue& retireQueue =
                ResourceRetireQueue::GetInstance();
            const size_t pending = retireQueue.GetPendingCount();
            shaderAutoReloadTestMaxPendingRetiredResources =
                std::max(
                    shaderAutoReloadTestMaxPendingRetiredResources,
                    pending);
            if (pending == 0)
            {
                if (shaderAutoReloadTestMaxPendingRetiredResources == 0)
                {
                    throw std::runtime_error(
                        "automatic reload matrix did not observe an "
                        "epoch-retired pipeline");
                }

                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestOriginalSource);
                WriteTextFileAtomically(
                    shaderAutoReloadTestVertexSourcePath,
                    shaderAutoReloadTestOriginalVertexSource);
                WriteTextFileAtomically(
                    shaderAutoReloadTestSurfaceSourcePath,
                    shaderAutoReloadTestOriginalSurfaceSource);
                CleanupGeneratedRuntimeFixture(
                    shaderAutoReloadTestFixtureDirectory,
                    diagnostics);
                shaderAutoReloadTestFixtureDirectory.clear();
                shaderAutoReloadTestActive = false;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::Idle;
                runtimeTestStatus = RuntimeTestStatus::Succeeded;
                diagnostics.ReportInfo(
                    "Shader auto reload runtime test completed: "
                    "in-flight stale generations, A1/A2/A3 coalescing, "
                    "independent source union, include/leaf dependency "
                    "updates, frozen-include deletion rollback, "
                    "mtime-only scan suppression, syntax failure/recovery, "
                    "manual/auto supersession, and epoch retirement passed.");
                return;
            }

            --shaderAutoReloadTestRetireDrainFramesRemaining;
            if (shaderAutoReloadTestRetireDrainFramesRemaining <= 0)
            {
                throw std::runtime_error(
                    "timed out waiting for retired pipelines to drain; "
                    "pending=" + std::to_string(pending));
            }
            return;
        }

        if (shaderAutoReloadTestPhaseEntryPending)
        {
            shaderAutoReloadTestPhaseEntryPending = false;
            shaderAutoReloadTestDeadline =
                std::chrono::steady_clock::now() +
                ShaderAsyncWaitTimeout;

            switch (shaderAutoReloadTestPhase)
            {
            case ShaderAutoReloadTestPhase::WaitA1Gate:
            {
                if (engineLoop.shaderFileMonitor == nullptr ||
                    engineLoop.shaderCompileWorker == nullptr)
                {
                    throw std::runtime_error(
                        "automatic reload matrix lost monitor or worker");
                }
                engineLoop.shaderFileMonitor
                    ->SetTestPollInterval(
                        std::chrono::milliseconds(0));
                const ShaderAutoReloadRuntimeSnapshot baseline =
                    CaptureShaderAutoReloadRuntimeSnapshot();
                shaderAutoReloadTestInitialSurfaceGeneration =
                    baseline.surfaceGeneration;
                shaderAutoReloadTestInitialShadowGeneration =
                    baseline.shadowGeneration;
                shaderAutoReloadTestInitialManifestDigest =
                    baseline.manifestDigest;
                shaderAutoReloadTestInitialResolvedGeneration =
                    baseline.resolvedGeneration;
                shaderAutoReloadTestBaselineObservedEpoch =
                    engineLoop.latestObservedSourceEpoch;
                engineLoop.shaderCompileWorker
                    ->ArmTestCompileGateForNextSubmit();
                shaderAutoReloadTestWorker =
                    engineLoop.shaderCompileWorker.get();
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestCompatibleSourceA);
                diagnostics.ReportInfo(
                    "Shader auto reload matrix phase A1: "
                    "wrote A1 and armed the worker gate.");
                break;
            }
            case ShaderAutoReloadTestPhase::WaitA2Stable:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC1);
                shaderAutoReloadTestBaselineObservedEpoch =
                    engineLoop.latestObservedSourceEpoch;
                break;
            case ShaderAutoReloadTestPhase::WaitA3Stable:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC2);
                shaderAutoReloadTestBaselineObservedEpoch =
                    engineLoop.latestObservedSourceEpoch;
                break;
            case ShaderAutoReloadTestPhase::WaitSyntaxFailure:
                shaderAutoReloadTestBaselineManifestDigest =
                    CaptureShaderAutoReloadRuntimeSnapshot()
                        .manifestDigest;
                shaderAutoReloadTestBaselineResolvedGeneration =
                    CaptureShaderAutoReloadRuntimeSnapshot()
                        .resolvedGeneration;
                shaderAutoReloadTestBaselineFailedGeneration =
                    engineLoop.latestAutoReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestSyntaxErrorSource);
                break;
            case ShaderAutoReloadTestPhase::WaitSyntaxRecovery:
                shaderAutoReloadTestBaselineCommittedGeneration =
                    engineLoop.latestAutoReloadCommittedGeneration;
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestCompatibleSourceB);
                break;
            case ShaderAutoReloadTestPhase::WaitUnionGate:
                engineLoop.shaderCompileWorker
                    ->ArmTestCompileGateForNextSubmit();
                shaderAutoReloadTestWorker =
                    engineLoop.shaderCompileWorker.get();
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC3);
                break;
            case ShaderAutoReloadTestPhase::WaitUnionYStable:
                WriteTextFileAtomically(
                    shaderAutoReloadTestVertexSourcePath,
                    shaderAutoReloadTestLeafSourceA);
                shaderAutoReloadTestBaselineObservedEpoch =
                    engineLoop.latestObservedSourceEpoch;
                break;
            case ShaderAutoReloadTestPhase::WaitUnionZStable:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSurfaceSourcePath,
                    shaderAutoReloadTestSurfaceSourceA);
                shaderAutoReloadTestBaselineObservedEpoch =
                    engineLoop.latestObservedSourceEpoch;
                break;
            case ShaderAutoReloadTestPhase::WaitDeleteGate:
            {
                const ShaderAutoReloadRuntimeSnapshot baseline =
                    CaptureShaderAutoReloadRuntimeSnapshot();
                shaderAutoReloadTestBaselineManifestDigest =
                    baseline.manifestDigest;
                shaderAutoReloadTestBaselineResolvedGeneration =
                    baseline.resolvedGeneration;
                shaderAutoReloadTestBaselineFailedGeneration =
                    engineLoop.latestAutoReloadFailedGeneration;
                engineLoop.shaderCompileWorker
                    ->ArmTestCompileGateForNextSubmit();
                shaderAutoReloadTestWorker =
                    engineLoop.shaderCompileWorker.get();
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC1);
                break;
            }
            case ShaderAutoReloadTestPhase::WaitDeleteCandidateRejected:
                if (std::filesystem::exists(
                        shaderAutoReloadTestSourcePath))
                {
                    throw std::runtime_error(
                        "delete phase expected the frozen include source "
                        "to be absent");
                }
                break;
            case ShaderAutoReloadTestPhase::WaitDeleteFailure:
                break;
            case ShaderAutoReloadTestPhase::WaitDeleteRecovery:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestOriginalSource);
                shaderAutoReloadTestBaselineCommittedGeneration =
                    engineLoop.latestAutoReloadCommittedGeneration;
                break;
            case ShaderAutoReloadTestPhase::WaitMtimeOnlyScan:
            {
                const std::filesystem::path path =
                    shaderAutoReloadTestSourcePath;
                shaderAutoReloadTestOriginalWriteTime =
                    std::filesystem::last_write_time(path);
                shaderAutoReloadTestOriginalWriteTimeCaptured = true;
                shaderAutoReloadTestBaselineObservedEpoch =
                    engineLoop.latestObservedSourceEpoch;
                shaderAutoReloadTestBaselineSubmittedGeneration =
                    engineLoop.latestSubmittedAutoReloadGeneration;
                shaderAutoReloadTestBaselineCommittedGeneration =
                    engineLoop.latestAutoReloadCommittedGeneration;
                shaderAutoReloadTestBaselineShadercInvocations =
                    engineLoop.totalAutoReloadShadercInvocations;
                shaderAutoReloadTestBaselineMonitorScanCount =
                    engineLoop.shaderFileMonitor->GetScanCount();
                std::filesystem::last_write_time(
                    path,
                    shaderAutoReloadTestOriginalWriteTime +
                        std::chrono::seconds(1));
                break;
            }
            case ShaderAutoReloadTestPhase::WaitManualGate:
                engineLoop.shaderCompileWorker
                    ->ArmTestCompileGateForNextSubmit();
                shaderAutoReloadTestWorker =
                    engineLoop.shaderCompileWorker.get();
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC2);
                break;
            case ShaderAutoReloadTestPhase::WaitManualCommit:
            {
                const ShaderAutoReloadRuntimeSnapshot baseline =
                    CaptureShaderAutoReloadRuntimeSnapshot();
                shaderAutoReloadTestBaselineSurfaceGeneration =
                    baseline.surfaceGeneration;
                shaderAutoReloadTestBaselineShadowGeneration =
                    baseline.shadowGeneration;
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC3);
                shaderAutoReloadTestQueueManualReload = true;
                shaderAutoReloadTestManualReloadQueued = false;
                shaderAutoReloadTestBaselineCommittedGeneration =
                    engineLoop.latestManualShaderReloadCommittedGeneration;
                RuntimeCommand command;
                command.type = RuntimeCommandType::ReloadShaders;
                command.shaderReloadScope =
                    RuntimeShaderReloadScope::Changed;
                command.sourceText =
                    "runtime-test: manual shader reload supersession";
                engineLoop.GetSubsystems().GetCommandBus().Queue(
                    std::move(command));
                shaderAutoReloadTestManualReloadQueued = true;
                break;
            }
            case ShaderAutoReloadTestPhase::RestoreOriginal:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestOriginalSource);
                WriteTextFileAtomically(
                    shaderAutoReloadTestVertexSourcePath,
                    shaderAutoReloadTestOriginalVertexSource);
                WriteTextFileAtomically(
                    shaderAutoReloadTestSurfaceSourcePath,
                    shaderAutoReloadTestOriginalSurfaceSource);
                shaderAutoReloadTestBaselineCommittedGeneration =
                    engineLoop.latestAutoReloadCommittedGeneration;
                break;
            default:
                break;
            }
        }

        const ShaderAutoReloadRuntimeSnapshot snapshot =
            CaptureShaderAutoReloadRuntimeSnapshot();
        const bool workerAtGate =
            engineLoop.shaderCompileWorker != nullptr &&
            engineLoop.shaderCompileWorker
                ->IsWaitingAtTestCompileGate();
        const bool pendingShared =
            engineLoop.pendingAutoReloadSources.count(
                sharedIdentity) != 0;
        const bool pendingVertex =
            engineLoop.pendingAutoReloadSources.count(
                vertexIdentity) != 0;
        const bool pendingSurface =
            engineLoop.pendingAutoReloadSources.count(
                surfaceIdentity) != 0;

        switch (shaderAutoReloadTestPhase)
        {
        case ShaderAutoReloadTestPhase::WaitA1Gate:
            if (workerAtGate &&
                engineLoop.inFlightAutoReloadGeneration != 0)
            {
                shaderAutoReloadTestGateGeneration =
                    engineLoop.inFlightAutoReloadGeneration;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitA2Stable;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitA2Stable:
            if (pendingShared &&
                engineLoop.latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitA3Stable;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitA3Stable:
            if (pendingShared &&
                engineLoop.latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                engineLoop.shaderCompileWorker
                    ->ReleaseTestCompileGate();
                shaderAutoReloadTestWorker = nullptr;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitA1Stale;
                shaderAutoReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitA1Stale:
            if (engineLoop.latestAutoReloadStaleDiscardGeneration ==
                shaderAutoReloadTestGateGeneration)
            {
                if (!SameShaderAutoReloadRuntimeSnapshot(
                        snapshot,
                        ShaderAutoReloadRuntimeSnapshot{
                            {},
                            {},
                            shaderAutoReloadTestInitialSurfaceGeneration,
                            shaderAutoReloadTestInitialShadowGeneration,
                            shaderAutoReloadTestInitialManifestDigest,
                            shaderAutoReloadTestInitialResolvedGeneration}))
                {
                    throw std::runtime_error(
                        "A1 stale result changed live pipeline, manifest, "
                        "or resolved draw generation");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed A1 in-flight stale "
                    "discard after A2/A3.");
                shaderAutoReloadTestBaselineCommittedGeneration =
                    engineLoop.latestAutoReloadCommittedGeneration;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitA3Commit;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitA3Commit:
            if (engineLoop.latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration &&
                snapshot.surfaceGeneration !=
                    shaderAutoReloadTestInitialSurfaceGeneration)
            {
                const std::string expectedDigest =
                    ContentHasher::HashFile(
                        shaderAutoReloadTestSourcePath).ToHex();
                if (!SurfaceArtifactDependsOnSourceWithDigest(
                        engineLoop,
                        snapshot.surfaceLogicalBuildId,
                        sharedIdentity,
                        expectedDigest))
                {
                    throw std::runtime_error(
                        "A3 commit does not match the final source digest");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed A3 final-source "
                    "commit.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitSyntaxFailure;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitSyntaxFailure:
            if (engineLoop.latestAutoReloadFailedGeneration >
                    shaderAutoReloadTestBaselineFailedGeneration)
            {
                if (snapshot.manifestDigest !=
                        shaderAutoReloadTestBaselineManifestDigest ||
                    snapshot.resolvedGeneration !=
                        shaderAutoReloadTestBaselineResolvedGeneration)
                {
                    throw std::runtime_error(
                        "syntax failure changed live artifacts");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed syntax failure "
                    "retention with pending source preserved.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitSyntaxRecovery;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitSyntaxRecovery:
            if (engineLoop.latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration &&
                snapshot.surfaceGeneration !=
                    shaderAutoReloadTestInitialSurfaceGeneration)
            {
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed syntax recovery.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionGate;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionGate:
            if (workerAtGate &&
                engineLoop.inFlightAutoReloadGeneration != 0)
            {
                shaderAutoReloadTestGateGeneration =
                    engineLoop.inFlightAutoReloadGeneration;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionYStable;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionYStable:
            if (pendingVertex &&
                engineLoop.latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionZStable;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionZStable:
            if (pendingSurface &&
                engineLoop.latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                engineLoop.shaderCompileWorker
                    ->ReleaseTestCompileGate();
                shaderAutoReloadTestWorker = nullptr;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionStale;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionStale:
            if (engineLoop.latestAutoReloadStaleDiscardGeneration ==
                shaderAutoReloadTestGateGeneration)
            {
                shaderAutoReloadTestLastSubmittedSources =
                    engineLoop.lastSubmittedAutoReloadSources;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionCommit;
                shaderAutoReloadTestBaselineCommittedGeneration =
                    engineLoop.latestAutoReloadCommittedGeneration;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionCommit:
            if (engineLoop.latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration)
            {
                if (!ContainsAllSourceIdentities(
                        engineLoop.lastSubmittedAutoReloadSources,
                        unionSources))
                {
                    throw std::runtime_error(
                        "independent source changes were not submitted as "
                        "one pending union");
                }
                const VL::ShaderBuildManifestSnapshot manifest =
                    engineLoop.shaderCompiler
                        ->CaptureManifestSnapshot();
                if (!ManifestArtifactDependsOnAllSources(
                        manifest,
                        snapshot.surfaceLogicalBuildId,
                        unionSources) ||
                    !ManifestArtifactDependsOnAllSources(
                        manifest,
                        snapshot.shadowLogicalBuildId,
                        unionSources))
                {
                    throw std::runtime_error(
                        "surface/shadow dependency artifacts did not both "
                        "update for include and leaf changes");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed independent-source "
                    "pending union and Surface/Shadow dependency update.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitDeleteGate;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitDeleteGate:
            if (workerAtGate &&
                engineLoop.inFlightAutoReloadGeneration != 0)
            {
                shaderAutoReloadTestGateGeneration =
                    engineLoop.inFlightAutoReloadGeneration;
                shaderAutoReloadTestBaselineObservedEpoch =
                    engineLoop.latestObservedSourceEpoch;
                std::filesystem::remove(
                    shaderAutoReloadTestSourcePath);
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitDeleteCandidateRejected;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitDeleteCandidateRejected:
            if (pendingShared &&
                engineLoop.latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                engineLoop.shaderCompileWorker
                    ->ReleaseTestCompileGate();
                shaderAutoReloadTestWorker = nullptr;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitDeleteFailure;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitDeleteFailure:
            if (engineLoop.latestAutoReloadStaleDiscardGeneration ==
                    shaderAutoReloadTestGateGeneration &&
                engineLoop.latestAutoReloadFailedGeneration >
                    shaderAutoReloadTestBaselineFailedGeneration)
            {
                if (snapshot.manifestDigest !=
                        shaderAutoReloadTestBaselineManifestDigest ||
                    snapshot.resolvedGeneration !=
                        shaderAutoReloadTestBaselineResolvedGeneration)
                {
                    throw std::runtime_error(
                        "deleted include changed the live state");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed frozen-include "
                    "stale discard and stable deletion failure.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitDeleteRecovery;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitDeleteRecovery:
            if (engineLoop.latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration)
            {
                if (!std::filesystem::exists(
                        shaderAutoReloadTestSourcePath))
                {
                    throw std::runtime_error(
                        "delete recovery did not restore the include file");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed include deletion "
                    "recovery.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitMtimeOnlyScan;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitMtimeOnlyScan:
            if (engineLoop.shaderFileMonitor->GetScanCount() >
                shaderAutoReloadTestBaselineMonitorScanCount)
            {
                if (engineLoop.latestObservedSourceEpoch !=
                        shaderAutoReloadTestBaselineObservedEpoch ||
                    engineLoop.latestSubmittedAutoReloadGeneration !=
                        shaderAutoReloadTestBaselineSubmittedGeneration ||
                    engineLoop.latestAutoReloadCommittedGeneration !=
                        shaderAutoReloadTestBaselineCommittedGeneration ||
                    engineLoop.totalAutoReloadShadercInvocations !=
                        shaderAutoReloadTestBaselineShadercInvocations ||
                    !SameShaderAutoReloadRuntimeSnapshot(
                        snapshot,
                        CaptureShaderAutoReloadRuntimeSnapshot()))
                {
                    throw std::runtime_error(
                        "mtime-only change caused source generation, "
                        "shaderc, or live artifact changes");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed mtime-only scan "
                    "suppression.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitManualGate;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitManualGate:
            if (workerAtGate &&
                engineLoop.inFlightAutoReloadGeneration != 0)
            {
                shaderAutoReloadTestGateGeneration =
                    engineLoop.inFlightAutoReloadGeneration;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitManualCommit;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitManualCommit:
            if (engineLoop.latestManualShaderReloadCommittedGeneration >
                shaderAutoReloadTestBaselineCommittedGeneration)
            {
                if (snapshot.surfaceGeneration ==
                        shaderAutoReloadTestInitialSurfaceGeneration)
                {
                    throw std::runtime_error(
                        "manual reload did not replace the live artifact");
                }
                shaderAutoReloadTestBaselineResolvedGeneration =
                    snapshot.resolvedGeneration;
                engineLoop.shaderCompileWorker
                    ->ReleaseTestCompileGate();
                shaderAutoReloadTestWorker = nullptr;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitManualStale;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitManualStale:
            if (engineLoop.latestAutoReloadStaleDiscardGeneration ==
                shaderAutoReloadTestGateGeneration)
            {
                if (snapshot.resolvedGeneration !=
                        shaderAutoReloadTestBaselineResolvedGeneration)
                {
                    throw std::runtime_error(
                        "stale auto result overwrote manual shader reload");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed manual/auto "
                    "supersession.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::RestoreOriginal;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::RestoreOriginal:
            if (engineLoop.latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration &&
                snapshot.surfaceGeneration ==
                    shaderAutoReloadTestInitialSurfaceGeneration)
            {
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitRetireDrain;
                shaderAutoReloadTestRetireDrainFramesRemaining =
                    RetireDrainFrameBudget;
                shaderAutoReloadTestMaxPendingRetiredResources =
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount();
            }
            break;
        default:
            break;
        }

        if (std::chrono::steady_clock::now() >=
            shaderAutoReloadTestDeadline)
        {
            throw std::runtime_error(
                "automatic reload matrix timed out in phase " +
                std::to_string(
                    static_cast<int>(shaderAutoReloadTestPhase)) +
                ", pending=" +
                std::to_string(
                    engineLoop.pendingAutoReloadSources.size()) +
                ", inFlight=" +
                std::to_string(
                    engineLoop.inFlightAutoReloadGeneration));
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderAutoReloadTest(
            std::string(
                "Shader auto reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

#if 0
void RuntimeTestHooks::UpdateShaderAutoReloadTestLegacy(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderAutoReloadTestActive)
    {
        return;
    }

    if (shaderAutoReloadTestPhase ==
        ShaderAutoReloadTestPhase::WaitRetireDrain)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const size_t pending = retireQueue.GetPendingCount();
        shaderAutoReloadTestMaxPendingRetiredResources =
            std::max(
                shaderAutoReloadTestMaxPendingRetiredResources,
                pending);
        if (pending == 0)
        {
            if (shaderAutoReloadTestMaxPendingRetiredResources == 0)
            {
                FailShaderAutoReloadTest(
                    "Shader auto reload runtime test did not observe any "
                    "epoch-retired pipelines.",
                    diagnostics);
                return;
            }

            shaderAutoReloadTestActive = false;
            shaderAutoReloadTestPhase =
                ShaderAutoReloadTestPhase::Idle;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            WriteTextFileAtomically(
                shaderAutoReloadTestSourcePath,
                shaderAutoReloadTestOriginalSource);
            CleanupGeneratedRuntimeFixture(
                shaderAutoReloadTestFixtureDirectory,
                diagnostics);
            shaderAutoReloadTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "Shader auto reload runtime test completed: "
                "debounced detection, worker-side CPU compilation, "
                "syntax failure retention, recovery, rapid-save "
                "single-commit semantics, and epoch retirement passed.");
            return;
        }

        --shaderAutoReloadTestRetireDrainFramesRemaining;
        if (shaderAutoReloadTestRetireDrainFramesRemaining <= 0)
        {
            FailShaderAutoReloadTest(
                "Shader auto reload runtime test timed out waiting for "
                "retired pipelines to drain. pending=" +
                    std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        if (shaderAutoReloadTestPhaseEntryPending)
        {
            shaderAutoReloadTestPhaseEntryPending = false;
            WriteTextFileAtomically(
                shaderAutoReloadTestSourcePath,
                shaderAutoReloadTestCompatibleSourceA);
            const ShaderAutoReloadRuntimeSnapshot baseline =
                CaptureShaderAutoReloadRuntimeSnapshot();
            shaderAutoReloadTestBaselineLatestGeneration =
                engineLoop.latestSubmittedAutoReloadGeneration;
            shaderAutoReloadTestBaselineSurfaceGeneration =
                baseline.surfaceGeneration;
            shaderAutoReloadTestBaselineShadowGeneration =
                baseline.shadowGeneration;
            shaderAutoReloadTestLastObservedSurfaceGeneration =
                baseline.surfaceGeneration;
            shaderAutoReloadTestCommitTransitions = 0;
            shaderAutoReloadTestDeadline =
                std::chrono::steady_clock::now() +
                ShaderAsyncWaitTimeout;
        }

        const bool pendingSourcesSettled =
            engineLoop.pendingAutoReloadSources.empty() ||
            (engineLoop.failedPendingAutoReloadSourceEpoch != 0 &&
             engineLoop.failedPendingAutoReloadSourceEpoch ==
                 engineLoop.pendingAutoReloadSourceEpoch);
        const bool workerSettled =
            engineLoop.shaderCompileWorker != nullptr &&
            engineLoop.shaderCompileWorker->IsIdle() &&
            pendingSourcesSettled &&
            engineLoop.inFlightAutoReloadGeneration == 0;
        const ShaderAutoReloadRuntimeSnapshot snapshot =
            CaptureShaderAutoReloadRuntimeSnapshot();

        if (snapshot.surfaceGeneration !=
            shaderAutoReloadTestLastObservedSurfaceGeneration)
        {
            shaderAutoReloadTestLastObservedSurfaceGeneration =
                snapshot.surfaceGeneration;
            ++shaderAutoReloadTestCommitTransitions;
        }

        const bool generationChanged =
            snapshot.surfaceGeneration !=
                shaderAutoReloadTestBaselineSurfaceGeneration ||
            snapshot.shadowGeneration !=
                shaderAutoReloadTestBaselineShadowGeneration;
        const bool autoPathAdvanced =
            engineLoop.latestSubmittedAutoReloadGeneration >
            shaderAutoReloadTestBaselineLatestGeneration;

        switch (shaderAutoReloadTestPhase)
        {
        case ShaderAutoReloadTestPhase::WaitCompatibleCommit:
            if (workerSettled && generationChanged)
            {
                if (!autoPathAdvanced)
                {
                    throw std::runtime_error(
                        "compatible commit did not flow through the "
                        "automatic monitor/worker path");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload runtime test passed debounced "
                    "compatible commit.");
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestSyntaxErrorSource);
                shaderAutoReloadTestBaselineLatestGeneration =
                    engineLoop.latestSubmittedAutoReloadGeneration;
                shaderAutoReloadTestBaselineSurfaceGeneration =
                    snapshot.surfaceGeneration;
                shaderAutoReloadTestBaselineShadowGeneration =
                    snapshot.shadowGeneration;
                shaderAutoReloadTestBaselineManifestDigest =
                    snapshot.manifestDigest;
                shaderAutoReloadTestLastObservedSurfaceGeneration =
                    snapshot.surfaceGeneration;
                shaderAutoReloadTestCommitTransitions = 0;
                shaderAutoReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitSyntaxFailure;
            }
            break;

        case ShaderAutoReloadTestPhase::WaitSyntaxFailure:
            if (workerSettled && autoPathAdvanced)
            {
                if (snapshot.surfaceGeneration !=
                        shaderAutoReloadTestBaselineSurfaceGeneration ||
                    snapshot.shadowGeneration !=
                        shaderAutoReloadTestBaselineShadowGeneration ||
                    snapshot.manifestDigest !=
                        shaderAutoReloadTestBaselineManifestDigest)
                {
                    throw std::runtime_error(
                        "syntax error auto reload changed a live Pipeline "
                        "or the formal artifact manifest");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload runtime test passed syntax failure "
                    "retention of the old Pipeline.");
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestCompatibleSourceB);
                shaderAutoReloadTestBaselineLatestGeneration =
                    engineLoop.latestSubmittedAutoReloadGeneration;
                shaderAutoReloadTestLastObservedSurfaceGeneration =
                    snapshot.surfaceGeneration;
                shaderAutoReloadTestCommitTransitions = 0;
                shaderAutoReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitSyntaxRecovery;
            }
            break;

        case ShaderAutoReloadTestPhase::WaitSyntaxRecovery:
            if (workerSettled && generationChanged)
            {
                if (!autoPathAdvanced)
                {
                    throw std::runtime_error(
                        "syntax recovery did not flow through the "
                        "automatic monitor/worker path");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload runtime test passed syntax "
                    "recovery.");
                // Rapid consecutive saves all land between two monitor polls;
                // only the newest stable source generation may commit.
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC1);
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC2);
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC3);
                shaderAutoReloadTestBaselineLatestGeneration =
                    engineLoop.latestSubmittedAutoReloadGeneration;
                shaderAutoReloadTestBaselineSurfaceGeneration =
                    snapshot.surfaceGeneration;
                shaderAutoReloadTestBaselineShadowGeneration =
                    snapshot.shadowGeneration;
                shaderAutoReloadTestLastObservedSurfaceGeneration =
                    snapshot.surfaceGeneration;
                shaderAutoReloadTestCommitTransitions = 0;
                shaderAutoReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitRapidSaveCommit;
            }
            break;

        case ShaderAutoReloadTestPhase::WaitRapidSaveCommit:
            if (workerSettled && generationChanged)
            {
                if (!autoPathAdvanced)
                {
                    throw std::runtime_error(
                        "rapid save commit did not flow through the "
                        "automatic monitor/worker path");
                }
                if (shaderAutoReloadTestCommitTransitions != 1)
                {
                    throw std::runtime_error(
                        "rapid saves committed intermediate source "
                        "generations: transitions=" +
                        std::to_string(
                            shaderAutoReloadTestCommitTransitions));
                }
                const std::string expectedDigest =
                    ContentHasher::HashFile(
                        shaderAutoReloadTestSourcePath).ToHex();
                if (!SurfaceArtifactDependsOnSourceWithDigest(
                        engineLoop,
                        snapshot.surfaceLogicalBuildId,
                        "runtimeTest/shaderReloadTestShared.glsl",
                        expectedDigest))
                {
                    throw std::runtime_error(
                        "rapid save committed an artifact that does not "
                        "match the final source content");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload runtime test passed rapid-save "
                    "stale-generation discard.");
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestOriginalSource);
                shaderAutoReloadTestBaselineLatestGeneration =
                    engineLoop.latestSubmittedAutoReloadGeneration;
                shaderAutoReloadTestBaselineSurfaceGeneration =
                    snapshot.surfaceGeneration;
                shaderAutoReloadTestBaselineShadowGeneration =
                    snapshot.shadowGeneration;
                shaderAutoReloadTestLastObservedSurfaceGeneration =
                    snapshot.surfaceGeneration;
                shaderAutoReloadTestCommitTransitions = 0;
                shaderAutoReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::RestoreOriginal;
            }
            break;

        case ShaderAutoReloadTestPhase::RestoreOriginal:
            if (workerSettled && generationChanged)
            {
                diagnostics.ReportInfo(
                    "Shader auto reload runtime test passed original "
                    "source restore.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitRetireDrain;
                shaderAutoReloadTestRetireDrainFramesRemaining =
                    RetireDrainFrameBudget;
                shaderAutoReloadTestMaxPendingRetiredResources =
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount();
                return;
            }
            break;

        case ShaderAutoReloadTestPhase::Idle:
        case ShaderAutoReloadTestPhase::WaitWorldLoad:
        case ShaderAutoReloadTestPhase::WaitRetireDrain:
            break;
        }

        if (std::chrono::steady_clock::now() >=
            shaderAutoReloadTestDeadline)
        {
            throw std::runtime_error(
                "timed out waiting for the automatic monitor/worker "
                "pipeline to settle in phase " +
                std::to_string(
                    static_cast<int>(shaderAutoReloadTestPhase)));
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderAutoReloadTest(
            std::string(
                "Shader auto reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}
#endif

void RuntimeTestHooks::FailShaderComputeReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderComputeSkyShSourcePath.empty() &&
        !shaderComputeSkyShOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderComputeSkyShSourcePath,
                shaderComputeSkyShOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderComputePrefilterSourcePath.empty() &&
        !shaderComputePrefilterOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderComputePrefilterSourcePath,
                shaderComputePrefilterOriginal);
        }
        catch (...)
        {
        }
    }
    shaderComputeReloadTestActive = false;
    waitingForShaderComputeReloadTestWorld = false;
    shaderComputeReloadTestPhaseEntryPending = false;
    shaderComputeReloadTestPhase =
        ShaderComputeReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderComputeReloadTestFixtureDirectory,
        diagnostics);
    shaderComputeReloadTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::FailShaderDefinitionReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderDefinitionReloadTestSourcePath.empty() &&
        !shaderDefinitionReloadTestOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderDefinitionReloadTestSourcePath,
                shaderDefinitionReloadTestOriginal);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    shaderDefinitionReloadTestSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (!shaderDefinitionReloadTestBatchSourcePath.empty() &&
        !shaderDefinitionReloadTestBatchOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderDefinitionReloadTestBatchSourcePath,
                shaderDefinitionReloadTestBatchOriginal);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    shaderDefinitionReloadTestBatchSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    shaderDefinitionReloadTestActive = false;
    waitingForShaderDefinitionReloadTestWorld = false;
    shaderDefinitionReloadTestPhaseEntryPending = false;
    shaderDefinitionReloadTestPhase =
        ShaderDefinitionReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderDefinitionReloadTestFixtureDirectory,
        diagnostics);
    shaderDefinitionReloadTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::FailWorldGraphTransactionTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!worldGraphTransactionTestSourcePath.empty() &&
        !worldGraphTransactionTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
        worldGraphTransactionTestSourcePath,
        worldGraphTransactionTestOriginalSource);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    worldGraphTransactionTestSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (worldGraphTransactionTestMonitorSuspended &&
        worldGraphTransactionTestMonitor != nullptr)
    {
        worldGraphTransactionTestMonitor
            ->RefreshBaselineForSources({
                "runtimeTest/M_shaderReloadTest.json"});
        worldGraphTransactionTestMonitor
            ->SetTestScanSuspended(false);
        worldGraphTransactionTestMonitorSuspended = false;
    }
    worldGraphTransactionTestMonitor = nullptr;
    worldGraphTransactionTestSourcePath.clear();
    worldGraphTransactionTestOriginalSource.clear();
    worldGraphTransactionTestCandidateSource.clear();
    worldGraphTransactionTestHighLightScenePath.clear();
    worldGraphTransactionTestResourcesExpectedToExpire.clear();
    worldGraphTransactionTestFramesUntilNextPhase = 0;
    worldGraphTransactionTestActive = false;
    waitingForWorldGraphTransactionTestWorld = false;
    worldGraphTransactionTestPhase =
        WorldGraphTransactionTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        worldGraphTransactionTestFixtureDirectory,
        diagnostics);
    worldGraphTransactionTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::ValidateWorldGraphTransactionFailure(
    EngineLoop& engineLoop,
    const WorldGraphTransactionTestFaultInjection& injection,
    bool graphOnly,
    bool materialDefinitionTransaction,
    const std::string& phaseName,
    const std::string& expectedFailureText,
    const DiagnosticsSubsystem& diagnostics)
{
    std::string beforeDetails;
    const std::string beforeFingerprint =
        CaptureWorldGraphRuntimeFingerprint(
            engineLoop,
            worldGraphTransactionTestSourcePath,
            worldGraphTransactionTestBatchSourcePath,
            true,
            &beforeDetails);
    const std::array<size_t, 9> beforeCounts =
        CaptureBackendIdentityCounts(
            *engineLoop.rendererBackend);

    engineLoop.SetWorldGraphTransactionTestFaultInjection(
        injection);
    bool succeeded = false;
    std::string failureMessage;
    if (materialDefinitionTransaction)
    {
        const RuntimeResult<WorldHandle> result =
            engineLoop
                .ExecuteMaterialDefinitionWorldGraphTransactionForTest(
                    {"runtimeTest/M_shaderReloadTest.json"},
                    worldGraphTransactionTestNextBatchId++);
        succeeded = result.IsSuccess();
        if (result.IsFailure())
        {
            failureMessage =
                FormatRuntimeError(result.Error());
        }
    }
    else if (graphOnly)
    {
        const RuntimeResult<void> result =
            engineLoop.ReloadRenderGraphResources();
        succeeded = result.IsSuccess();
        if (result.IsFailure())
        {
            failureMessage =
                FormatRuntimeError(result.Error());
        }
    }
    else
    {
        const RuntimeResult<WorldHandle> result =
            engineLoop.ExecuteWorldGraphTransaction(
                engineLoop.GetSubsystems()
                    .GetWorldManager()
                    .GetActiveWorldHandle()
                    .scenePath);
        succeeded = result.IsSuccess();
        if (result.IsFailure())
        {
            failureMessage =
                FormatRuntimeError(result.Error());
        }
    }
    engineLoop.SetWorldGraphTransactionTestFaultInjection({});

    if (succeeded)
    {
        throw std::runtime_error(
            phaseName +
            " unexpectedly committed");
    }
    if (failureMessage.find(expectedFailureText) ==
        std::string::npos)
    {
        throw std::runtime_error(
            phaseName +
            " reported an unexpected failure: " +
            failureMessage);
    }

    std::string afterDetails;
    const std::string afterFingerprint =
        CaptureWorldGraphRuntimeFingerprint(
            engineLoop,
            worldGraphTransactionTestSourcePath,
            worldGraphTransactionTestBatchSourcePath,
            true,
            &afterDetails);
    if (afterFingerprint != beforeFingerprint)
    {
        size_t beforeOffset = 0;
        size_t afterOffset = 0;
        std::string firstDifference =
            "no differing field could be isolated";
        while (beforeOffset < beforeDetails.size() ||
               afterOffset < afterDetails.size())
        {
            const size_t beforeEnd =
                beforeDetails.find(';', beforeOffset);
            const size_t afterEnd =
                afterDetails.find(';', afterOffset);
            const std::string beforeField =
                beforeDetails.substr(
                    beforeOffset,
                    beforeEnd == std::string::npos
                        ? std::string::npos
                        : beforeEnd - beforeOffset);
            const std::string afterField =
                afterDetails.substr(
                    afterOffset,
                    afterEnd == std::string::npos
                        ? std::string::npos
                        : afterEnd - afterOffset);
            if (beforeField != afterField)
            {
                firstDifference =
                    "beforeField={" + beforeField +
                    "}, afterField={" + afterField + "}";
                break;
            }
            beforeOffset =
                beforeEnd == std::string::npos
                    ? beforeDetails.size()
                    : beforeEnd + 1;
            afterOffset =
                afterEnd == std::string::npos
                    ? afterDetails.size()
                    : afterEnd + 1;
        }
        throw std::runtime_error(
            phaseName +
            " changed the live World/graph/runtime fingerprint: before=" +
            beforeFingerprint +
            ", after=" +
            afterFingerprint +
            ", firstDifference=" +
            firstDifference);
    }

    const std::array<size_t, 9> afterCounts =
        CaptureBackendIdentityCounts(
            *engineLoop.rendererBackend);
    if (afterCounts != beforeCounts)
    {
        throw std::runtime_error(
            phaseName +
            " leaked or destroyed renderer identities: before={" +
            FormatBackendIdentityCounts(beforeCounts) +
            "}, after={" +
            FormatBackendIdentityCounts(afterCounts) +
            "}");
    }

    diagnostics.ReportInfo(
        "World/graph transaction rollback passed " +
        phaseName +
        ": fingerprint=" +
        afterFingerprint +
        ", backend={" +
        FormatBackendIdentityCounts(afterCounts) +
        "}, failure=" +
        failureMessage +
        ".");
}

void RuntimeTestHooks::
AdvanceWorldGraphTransactionTestAfterRenderedFrames(
    WorldGraphTransactionTestPhase nextPhase) noexcept
{
    worldGraphTransactionTestPhase = nextPhase;
    worldGraphTransactionTestFramesUntilNextPhase = 3;
}

void RuntimeTestHooks::UpdateWorldGraphTransactionTest(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!worldGraphTransactionTestActive)
    {
        return;
    }

    try
    {
        if (worldGraphTransactionTestFramesUntilNextPhase > 0)
        {
            --worldGraphTransactionTestFramesUntilNextPhase;
            return;
        }

        if (worldGraphTransactionTestPhase ==
            WorldGraphTransactionTestPhase::WaitRetireDrain)
        {
            ResourceRetireQueue& retireQueue =
                ResourceRetireQueue::GetInstance();
            const size_t pending =
                retireQueue.GetPendingCount();
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    pending);
            if (pending == 0)
            {
                if (worldGraphTransactionTestMaxPendingRetiredResources == 0)
                {
                    throw std::runtime_error(
                        "successful transactions did not enqueue epoch-retired packages");
                }
                if (!worldGraphTransactionTestOldWorld.expired() ||
                    !worldGraphTransactionTestOldWorldPackage.expired() ||
                    !worldGraphTransactionTestOldGraphPackage.expired() ||
                    !worldGraphTransactionTestGraphReloadPackage.expired() ||
                    !worldGraphTransactionTestOldLightBuffer.expired() ||
                    !worldGraphTransactionTestOldMaterial.expired() ||
                    !worldGraphTransactionTestOldMaterialInstance.expired() ||
                    !worldGraphTransactionTestOldObjectResources.expired())
                {
                    throw std::runtime_error(
                        "retire queue drained while an old World/resource/graph owner remained live");
                }
                for (const std::weak_ptr<void>& retired :
                     worldGraphTransactionTestResourcesExpectedToExpire)
                {
                    if (!retired.expired())
                    {
                        throw std::runtime_error(
                            "retire queue drained while a tracked transaction resource remained live");
                    }
                }

                const std::shared_ptr<void> retainedTexture =
                    worldGraphTransactionTestOldTexture.lock();
                const std::shared_ptr<Texture> activeTexture =
                    FindShaderReloadTestPrimaryTexture();
                if (!retainedTexture || !activeTexture ||
                    retainedTexture.get() != activeTexture.get())
                {
                    throw std::runtime_error(
                        "compatible MaterialInstance texture identity was not retained across World/M_ transactions");
                }

                const std::array<size_t, 9> finalCounts =
                    CaptureBackendIdentityCounts(
                        *engineLoop.rendererBackend);
                if (finalCounts !=
                    worldGraphTransactionTestBackendCountsBeforeSuccess)
                {
                    const std::vector<std::string> finalNames =
                        CaptureImageResourceDebugNames(
                            *engineLoop.rendererBackend);
                    throw std::runtime_error(
                        "renderer identity counts did not return to the pre-commit baseline: baseline={" +
                        FormatBackendIdentityCounts(
                            worldGraphTransactionTestBackendCountsBeforeSuccess) +
                        "}, final={" +
                            FormatBackendIdentityCounts(finalCounts) +
                            "}, imageResourceDiff={" +
                            FormatImageResourceDebugNameDifference(
                                worldGraphTransactionTestImageResourceNamesBeforeSuccess,
                                finalNames) +
                            "}");
                }
                diagnostics.ReportInfo(
                    "World/graph transaction epoch retirement drain passed: "
                    "maxRetirePending=" +
                    std::to_string(
                        worldGraphTransactionTestMaxPendingRetiredResources) +
                    ", finalBackend={" +
                    FormatBackendIdentityCounts(finalCounts) +
                    "}.");
                worldGraphTransactionTestPhase =
                    WorldGraphTransactionTestPhase::ResizeFatalFailure;
                return;
            }

            --worldGraphTransactionTestRetireDrainFramesRemaining;
            if (worldGraphTransactionTestRetireDrainFramesRemaining <= 0)
            {
                throw std::runtime_error(
                    "timed out waiting for retired World/graph packages to drain; pending=" +
                    std::to_string(pending));
            }
            return;
        }

        WorldGraphTransactionTestFaultInjection injection;
        switch (worldGraphTransactionTestPhase)
        {
        case WorldGraphTransactionTestPhase::GraphResourceFailure:
            injection.failGraphResourceCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                false,
                "World transaction graph resource creation item 2",
                "Injected render graph resource creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::RenderPassFailure);
            return;

        case WorldGraphTransactionTestPhase::RenderPassFailure:
            injection.failRenderPassCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                false,
                "World transaction render pass creation item 2",
                "Injected render pass creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::FramebufferFailure);
            return;

        case WorldGraphTransactionTestPhase::FramebufferFailure:
            injection.failFramebufferCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                false,
                "World transaction framebuffer creation item 2",
                "Injected framebuffer creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::PassMaterialContractFailure);
            return;

        case WorldGraphTransactionTestPhase::PassMaterialContractFailure:
            injection.failPassMaterialContract = true;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                false,
                "World transaction pass material contract precheck",
                "Injected pass material contract failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::DescriptorFailure);
            return;

        case WorldGraphTransactionTestPhase::DescriptorFailure:
            injection.failDescriptorCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                false,
                "World transaction descriptor creation item 2",
                "Injected render graph descriptor creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::CandidateWorldFailure);
            return;

        case WorldGraphTransactionTestPhase::CandidateWorldFailure:
            injection.failAfterCandidateWorldBuilt = true;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                false,
                "candidate World built before commit",
                "Injected failure after candidate World build",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::ViewTargetFailure);
            return;

        case WorldGraphTransactionTestPhase::ViewTargetFailure:
            injection.failViewTargetPrecheck = true;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                false,
                "view target precheck",
                "Candidate World has no controller view target",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::RuntimeBindingFailure);
            return;

        case WorldGraphTransactionTestPhase::RuntimeBindingFailure:
            injection.failAfterRuntimeBindingPrepared = true;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                false,
                "World runtime binding prepared before commit",
                "Injected failure after runtime binding prepare",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::BeforeCommitFailure);
            return;

        case WorldGraphTransactionTestPhase::BeforeCommitFailure:
            injection.failBeforeCommit = true;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                false,
                "all prepare complete before commit",
                "Injected failure after all World/graph/runtime prepare steps",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::GraphOnlyResourceFailure);
            return;

        case WorldGraphTransactionTestPhase::GraphOnlyResourceFailure:
            injection.failGraphResourceCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                true,
                false,
                "graph-only resource creation item 2",
                "Injected render graph resource creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::GraphOnlyDescriptorFailure);
            return;

        case WorldGraphTransactionTestPhase::GraphOnlyDescriptorFailure:
            injection.failDescriptorCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                true,
                false,
                "graph-only descriptor creation item 2",
                "Injected render graph descriptor creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::GraphOnlyBeforeCommitFailure);
            return;

        case WorldGraphTransactionTestPhase::GraphOnlyBeforeCommitFailure:
            injection.failBeforeCommit = true;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                true,
                false,
                "graph-only all prepare complete before commit",
                "Injected failure after RenderGraph reload prepare",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::SuccessCommit);
            return;

        case WorldGraphTransactionTestPhase::SuccessCommit:
        {
            WorldManager& worldManager =
                engineLoop.GetSubsystems()
                    .GetWorldManager();
            ResourceRetireQueue& retireQueue =
                ResourceRetireQueue::GetInstance();
            const uint64_t oldGeneration =
                worldManager
                    .GetActiveWorldHandle()
                    .generation;
            const size_t oldLightCapacity =
                RenderSystem::GetInstance()
                    .GetLightCapacityForTest();
            const std::vector<uint64_t> oldLightHandles =
                CaptureBufferHandleIds(
                    RenderSystem::GetInstance()
                        .GetLightBufferHandlesForTest());
            worldGraphTransactionTestGenerationBeforeSuccess =
                oldGeneration;
            worldGraphTransactionTestBackendCountsBeforeSuccess =
                CaptureBackendIdentityCounts(
                    *engineLoop.rendererBackend);
            worldGraphTransactionTestImageResourceNamesBeforeSuccess =
                CaptureImageResourceDebugNames(
                    *engineLoop.rendererBackend);

            const std::shared_ptr<World> oldWorld =
                worldManager.GetActiveWorld();
            const RendererResourceCache::
                ImmutableWorldLocalResourceRefs
                    oldWorldPackage =
                        RendererResourceCache::GetInstance()
                            .CaptureActiveWorldLocalResources();
            const RendererResourceCache::
                WorldLocalResourcePackageHandle
                    oldWorldPackageMutable =
                        std::const_pointer_cast<
                            RendererResourceCache::
                                WorldLocalResourcePackage>(
                                    oldWorldPackage);
            worldGraphTransactionTestOldWorld =
                oldWorld;
            worldGraphTransactionTestOldWorldPackage =
                oldWorldPackageMutable;
            worldGraphTransactionTestOldMaterial =
                FindShaderReloadTestMaterial();
            worldGraphTransactionTestOldMaterialInstance =
                FindShaderReloadTestMaterialInstance();
            worldGraphTransactionTestOldObjectResources =
                FindShaderReloadTestObjectResources();
            const std::shared_ptr<Texture> oldTexture =
                FindShaderReloadTestPrimaryTexture();
            if (!oldWorld || !oldWorldPackageMutable ||
                worldGraphTransactionTestOldMaterial.expired() ||
                worldGraphTransactionTestOldMaterialInstance.expired() ||
                worldGraphTransactionTestOldObjectResources.expired() ||
                !oldTexture)
            {
                throw std::runtime_error(
                    "high-light success phase could not capture the old runtime package");
            }
            worldGraphTransactionTestOldTexture =
                std::static_pointer_cast<void>(
                    oldTexture);

            const std::filesystem::path highLightScenePath =
                CreateWorldGraphTransactionHighLightScene(
                    worldGraphTransactionTestScenePath,
                    oldLightCapacity + 1);
            worldGraphTransactionTestHighLightScenePath =
                highLightScenePath.string();

            const RuntimeResult<WorldHandle> result =
                engineLoop.ExecuteWorldGraphTransaction(
                    worldGraphTransactionTestHighLightScenePath);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "high-light World success transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }

            const WorldHandle& committed =
                worldManager.GetActiveWorldHandle();
            const RendererResourceCache::
                ImmutableWorldLocalResourceRefs
                    activePackage =
                        RendererResourceCache::GetInstance()
                            .CaptureActiveWorldLocalResources();
            const uint64_t committedGeneration =
                committed.generation;
            if (committedGeneration <= oldGeneration ||
                !activePackage ||
                activePackage->ownerGeneration != committedGeneration ||
                RenderGraph::GetInstance()
                        .GetOwnerGeneration() != committedGeneration ||
                RenderSystem::GetInstance()
                        .GetActiveWorldGeneration() != committedGeneration ||
                engineLoop.controller
                        ->GetBoundWorldGeneration() != committedGeneration)
            {
                throw std::runtime_error(
                    "successful transaction did not advance World/cache/graph/RenderSystem/Controller generations together");
            }

            const size_t newLightCapacity =
                RenderSystem::GetInstance()
                    .GetLightCapacityForTest();
            const std::vector<uint64_t> newLightHandles =
                CaptureBufferHandleIds(
                    RenderSystem::GetInstance()
                        .GetLightBufferHandlesForTest());
            if (newLightCapacity <= oldLightCapacity ||
                newLightHandles.empty() ||
                newLightHandles == oldLightHandles)
            {
                throw std::runtime_error(
                    "high-light World transaction did not replace the frame light buffer");
            }
            if (FindShaderReloadTestPrimaryTexture().get() !=
                oldTexture.get())
            {
                throw std::runtime_error(
                    "compatible texture was not retained by the high-light World transaction");
            }

            const std::weak_ptr<void> retiredWorld =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:World",
                    oldGeneration);
            const std::weak_ptr<void> retiredPackage =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:WorldLocalResources",
                    oldGeneration);
            const std::weak_ptr<void> retiredGraph =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:RenderGraph",
                    oldGeneration);
            const std::weak_ptr<void> retiredLightBuffer =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:FrameLightBuffer",
                    oldGeneration);
            if (retiredWorld.expired() ||
                retiredPackage.expired() ||
                retiredGraph.expired() ||
                retiredLightBuffer.expired() ||
                retiredWorld.lock().get() != oldWorld.get() ||
                retiredPackage.lock().get() !=
                    oldWorldPackageMutable.get())
            {
                throw std::runtime_error(
                    "successful transaction did not enqueue the exact old World/resource/graph packages");
            }
            worldGraphTransactionTestOldGraphPackage =
                retiredGraph;
            worldGraphTransactionTestOldLightBuffer =
                retiredLightBuffer;
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredWorld);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredPackage);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredGraph);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredLightBuffer);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                worldGraphTransactionTestOldMaterial);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                worldGraphTransactionTestOldMaterialInstance);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                worldGraphTransactionTestOldObjectResources);
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    retireQueue.GetPendingCount());
            diagnostics.ReportInfo(
                "World/graph transaction synchronized commit passed: oldGeneration=" +
                std::to_string(oldGeneration) +
                ", newGeneration=" +
                std::to_string(committedGeneration) +
                ", oldLightCapacity=" +
                std::to_string(oldLightCapacity) +
                ", newLightCapacity=" +
                std::to_string(newLightCapacity) +
                ", retirePending=" +
                std::to_string(retireQueue.GetPendingCount()) +
                ", backendBefore={" +
                FormatBackendIdentityCounts(
                    worldGraphTransactionTestBackendCountsBeforeSuccess) +
                "}, backendWithRetired={" +
                FormatBackendIdentityCounts(
                    CaptureBackendIdentityCounts(
                        *engineLoop.rendererBackend)) +
                "}.");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::GraphOnlySuccess);
            return;
        }

        case WorldGraphTransactionTestPhase::GraphOnlySuccess:
        {
            WorldManager& worldManager =
                engineLoop.GetSubsystems()
                    .GetWorldManager();
            const WorldHandle beforeHandle =
                worldManager.GetActiveWorldHandle();
            const std::shared_ptr<World> beforeWorld =
                worldManager.GetActiveWorld();
            const RuntimeRendererResourceFingerprint
                beforeResources =
                    CaptureRuntimeRendererResourceFingerprint();
            const uint64_t beforeRenderSystemGeneration =
                RenderSystem::GetInstance()
                    .GetActiveWorldGeneration();
            const uint64_t beforeControllerGeneration =
                engineLoop.controller
                    ->GetBoundWorldGeneration();
            const uint64_t beforeGraphGeneration =
                RenderGraph::GetInstance()
                    .GetOwnerGeneration();
            const std::string beforeGraphGpu =
                CaptureRenderGraphGpuFingerprint(
                    RenderGraph::GetInstance());

            const RuntimeResult<void> result =
                engineLoop.ReloadRenderGraphResources();
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "graph-only success transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }

            if (!SameWorldHandle(
                    beforeHandle,
                    worldManager.GetActiveWorldHandle()) ||
                beforeWorld.get() !=
                    worldManager.GetActiveWorld().get() ||
                !SameRendererResourceFingerprint(
                    beforeResources,
                    CaptureRuntimeRendererResourceFingerprint()) ||
                beforeGraphGeneration !=
                    RenderGraph::GetInstance()
                        .GetOwnerGeneration() ||
                beforeRenderSystemGeneration !=
                    RenderSystem::GetInstance()
                        .GetActiveWorldGeneration() ||
                beforeControllerGeneration !=
                    engineLoop.controller
                        ->GetBoundWorldGeneration())
            {
                throw std::runtime_error(
                    "graph-only success changed a World/cache/RenderSystem/Controller owner");
            }
            const std::string afterGraphGpu =
                CaptureRenderGraphGpuFingerprint(
                    RenderGraph::GetInstance());
            if (afterGraphGpu == beforeGraphGpu)
            {
                throw std::runtime_error(
                    "graph-only success did not replace graph GPU identities");
            }
            const std::weak_ptr<void> retiredGraph =
                ResourceRetireQueue::GetInstance()
                    .FindPendingResourceForTest(
                        "RenderGraphReload:State",
                        beforeGraphGeneration);
            if (retiredGraph.expired())
            {
                throw std::runtime_error(
                    "graph-only success did not enqueue the old graph state");
            }
            worldGraphTransactionTestGraphReloadPackage =
                retiredGraph;
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredGraph);
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount());
            diagnostics.ReportInfo(
                "World/graph transaction graph-only commit passed: ownerGeneration=" +
                std::to_string(beforeGraphGeneration) +
                ", beforeGraph=" + beforeGraphGpu +
                ", afterGraph=" + afterGraphGpu +
                ".");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::MinimizeNoop);
            return;
        }

        case WorldGraphTransactionTestPhase::MinimizeNoop:
        {
            const std::string beforeFingerprint =
                CaptureWorldGraphRuntimeFingerprint(
                    engineLoop,
                    worldGraphTransactionTestSourcePath,
                    worldGraphTransactionTestBatchSourcePath);
            const std::array<size_t, 9> beforeCounts =
                CaptureBackendIdentityCounts(
                    *engineLoop.rendererBackend);
            const RuntimeResult<void> result =
                engineLoop.RecreateRendererForWindowResize(
                    0,
                    0);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "zero-size resize did not return a successful no-op: " +
                    FormatRuntimeError(result.Error()));
            }
            const std::string afterFingerprint =
                CaptureWorldGraphRuntimeFingerprint(
                    engineLoop,
                    worldGraphTransactionTestSourcePath,
                    worldGraphTransactionTestBatchSourcePath);
            const std::array<size_t, 9> afterCounts =
                CaptureBackendIdentityCounts(
                    *engineLoop.rendererBackend);
            if (afterFingerprint != beforeFingerprint ||
                afterCounts != beforeCounts)
            {
                throw std::runtime_error(
                    "zero-size resize modified live runtime or backend identities");
            }
            diagnostics.ReportInfo(
                "World/graph transaction zero-size resize no-op passed.");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::ResizeSuccess);
            return;
        }

        case WorldGraphTransactionTestPhase::ResizeSuccess:
        {
            WorldManager& worldManager =
                engineLoop.GetSubsystems()
                    .GetWorldManager();
            const WorldHandle beforeHandle =
                worldManager.GetActiveWorldHandle();
            const std::shared_ptr<World> beforeWorld =
                worldManager.GetActiveWorld();
            const RuntimeRendererResourceFingerprint
                beforeResources =
                    CaptureRuntimeRendererResourceFingerprint();
            const auto beforePrefilteredTexture =
                beforeResources.worldTextures.find(
                    "prefilteredEnvironmentCube");
            if (beforePrefilteredTexture ==
                    beforeResources.worldTextures.end() ||
                beforePrefilteredTexture->second == 0)
            {
                throw std::runtime_error(
                    "resize test could not capture the active prefiltered environment cube");
            }
            const uint64_t beforeObjectDescriptorPool =
                GetObjectDescriptorPoolIdentity(
                    FindShaderReloadTestObjectResources());
            if (beforeObjectDescriptorPool == 0)
            {
                throw std::runtime_error(
                    "resize test could not capture the active object descriptor package");
            }
            const uint64_t beforeGraphGeneration =
                RenderGraph::GetInstance()
                    .GetOwnerGeneration();
            const uint64_t beforeRenderSystemGeneration =
                RenderSystem::GetInstance()
                    .GetActiveWorldGeneration();
            const uint64_t beforeControllerGeneration =
                engineLoop.controller
                    ->GetBoundWorldGeneration();
            const std::string beforeGraphGpu =
                CaptureRenderGraphGpuFingerprint(
                    RenderGraph::GetInstance());
            const std::array<size_t, 9> beforeCounts =
                CaptureBackendIdentityCounts(
                    *engineLoop.rendererBackend);
            const vk::Extent2D extent =
                engineLoop.rendererBackend
                    ->GetSwapchainExtent();

            const RuntimeResult<void> result =
                engineLoop.RecreateRendererForWindowResize(
                    extent.width,
                    extent.height);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "same-size resize transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }
            const RuntimeRendererResourceFingerprint afterResources =
                CaptureRuntimeRendererResourceFingerprint();
            const auto afterPrefilteredTexture =
                afterResources.worldTextures.find(
                    "prefilteredEnvironmentCube");
            const uint64_t afterObjectDescriptorPool =
                GetObjectDescriptorPoolIdentity(
                    FindShaderReloadTestObjectResources());
            if (!SameWorldHandle(
                    beforeHandle,
                    worldManager.GetActiveWorldHandle()) ||
                beforeWorld.get() !=
                    worldManager.GetActiveWorld().get() ||
                !SameRendererResourceFingerprintExceptWorldTexture(
                    beforeResources,
                    afterResources,
                    "prefilteredEnvironmentCube") ||
                RenderGraph::GetInstance()
                        .GetOwnerGeneration() !=
                    beforeGraphGeneration ||
                RenderSystem::GetInstance()
                        .GetActiveWorldGeneration() !=
                    beforeRenderSystemGeneration ||
                engineLoop.controller
                        ->GetBoundWorldGeneration() !=
                    beforeControllerGeneration)
            {
                throw std::runtime_error(
                    "successful resize changed a stable runtime owner or generation:" +
                    DescribeRendererResourceFingerprintDifference(
                        beforeResources,
                        afterResources));
            }
            if (afterPrefilteredTexture ==
                    afterResources.worldTextures.end() ||
                afterPrefilteredTexture->second == 0 ||
                afterPrefilteredTexture->second ==
                    beforePrefilteredTexture->second ||
                afterObjectDescriptorPool == 0 ||
                afterObjectDescriptorPool ==
                    beforeObjectDescriptorPool)
            {
                throw std::runtime_error(
                    "successful resize did not replace its environment or object descriptor packages");
            }
            const std::string afterGraphGpu =
                CaptureRenderGraphGpuFingerprint(
                    RenderGraph::GetInstance());
            const std::array<size_t, 9> afterCounts =
                CaptureBackendIdentityCounts(
                    *engineLoop.rendererBackend);
            if (afterGraphGpu == beforeGraphGpu ||
                afterCounts != beforeCounts)
            {
                throw std::runtime_error(
                    "successful resize did not replace graph GPU state with stable backend counts");
            }
            diagnostics.ReportInfo(
                "World/graph transaction same-size resize passed: generation=" +
                std::to_string(beforeGraphGeneration) +
                ", beforeGraph=" + beforeGraphGpu +
                ", afterGraph=" + afterGraphGpu +
                ", prefilteredEnvironment=" +
                std::to_string(
                    beforePrefilteredTexture->second) +
                "->" +
                std::to_string(
                    afterPrefilteredTexture->second) +
                ", objectDescriptorPool=" +
                std::to_string(
                    beforeObjectDescriptorPool) +
                "->" +
                std::to_string(
                    afterObjectDescriptorPool) +
                ".");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::ShaderReloadSuccess);
            return;
        }

        case WorldGraphTransactionTestPhase::ShaderReloadSuccess:
        {
            WorldManager& worldManager =
                engineLoop.GetSubsystems()
                    .GetWorldManager();
            const WorldHandle beforeHandle =
                worldManager.GetActiveWorldHandle();
            const std::shared_ptr<World> beforeWorld =
                worldManager.GetActiveWorld();
            const RuntimeRendererResourceFingerprint
                beforeResources =
                    CaptureRuntimeRendererResourceFingerprint();
            const uint64_t beforeGraphGeneration =
                RenderGraph::GetInstance()
                    .GetOwnerGeneration();
            const uint64_t beforeRenderSystemGeneration =
                RenderSystem::GetInstance()
                    .GetActiveWorldGeneration();
            const uint64_t beforeControllerGeneration =
                engineLoop.controller
                    ->GetBoundWorldGeneration();
            const uint64_t beforeReloadGeneration =
                engineLoop.latestManualShaderReloadCommittedGeneration;
            RuntimeCommandExecutionResult commandResult;
            commandResult.shaderReloadRequested = true;
            commandResult.shaderReloadScope =
                RuntimeShaderReloadScope::All;
            engineLoop.ProcessShaderRuntimeRequests(
                commandResult);
            if (engineLoop.shouldClose ||
                engineLoop
                        .latestManualShaderReloadCommittedGeneration <=
                    beforeReloadGeneration ||
                !SameWorldHandle(
                    beforeHandle,
                    worldManager.GetActiveWorldHandle()) ||
                beforeWorld.get() !=
                    worldManager.GetActiveWorld().get() ||
                !SameRendererResourceFingerprint(
                    beforeResources,
                    CaptureRuntimeRendererResourceFingerprint()) ||
                RenderGraph::GetInstance()
                        .GetOwnerGeneration() !=
                    beforeGraphGeneration ||
                RenderSystem::GetInstance()
                        .GetActiveWorldGeneration() !=
                    beforeRenderSystemGeneration ||
                engineLoop.controller
                        ->GetBoundWorldGeneration() !=
                    beforeControllerGeneration)
            {
                throw std::runtime_error(
                    "shader reload interleave changed World/graph ownership or did not commit");
            }

            if (engineLoop.shaderFileMonitor == nullptr)
            {
                throw std::runtime_error(
                    "material definition transaction test lost the shader file monitor");
            }
            worldGraphTransactionTestMonitor =
                engineLoop.shaderFileMonitor.get();
            worldGraphTransactionTestMonitor
                ->SetTestScanSuspended(true);
            worldGraphTransactionTestMonitorSuspended = true;
            WriteTextFileAtomically(
                worldGraphTransactionTestSourcePath,
                worldGraphTransactionTestCandidateSource);
            diagnostics.ReportInfo(
                "World/graph transaction shader reload interleave passed: generation=" +
                std::to_string(
                    engineLoop
                        .latestManualShaderReloadCommittedGeneration) +
                ".");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::MaterialDefinitionRuntimeFailure);
            return;
        }

        case WorldGraphTransactionTestPhase::MaterialDefinitionRuntimeFailure:
            injection.failDescriptorCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                engineLoop,
                injection,
                false,
                true,
                "M_ schema rebuild with runtime descriptor failure",
                "Injected render graph descriptor creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::MaterialDefinitionSuccess);
            return;

        case WorldGraphTransactionTestPhase::MaterialDefinitionSuccess:
        {
            WorldManager& worldManager =
                engineLoop.GetSubsystems()
                    .GetWorldManager();
            ResourceRetireQueue& retireQueue =
                ResourceRetireQueue::GetInstance();
            const uint64_t oldGeneration =
                worldManager.GetActiveWorldHandle()
                    .generation;
            const std::shared_ptr<World> oldWorld =
                worldManager.GetActiveWorld();
            const auto oldPackage =
                std::const_pointer_cast<
                    RendererResourceCache::
                        WorldLocalResourcePackage>(
                    RendererResourceCache::GetInstance()
                        .CaptureActiveWorldLocalResources());
            const std::shared_ptr<Material> oldMaterial =
                FindShaderReloadTestMaterial();
            const std::shared_ptr<MaterialInstance> oldInstance =
                FindShaderReloadTestMaterialInstance();
            const std::shared_ptr<RendererObjectResourceEntry>
                oldObject =
                    FindShaderReloadTestObjectResources();
            const std::shared_ptr<Texture> retainedTexture =
                FindShaderReloadTestPrimaryTexture();
            if (!oldWorld || !oldPackage || !oldMaterial ||
                !oldInstance || !oldObject || !retainedTexture)
            {
                throw std::runtime_error(
                    "candidate M_ success could not capture the old runtime package");
            }

            const RuntimeResult<WorldHandle> result =
                engineLoop
                    .ExecuteMaterialDefinitionWorldGraphTransactionForTest(
                        {"runtimeTest/M_shaderReloadTest.json"},
                        worldGraphTransactionTestNextBatchId++);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "candidate M_ success transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }
            const uint64_t committedGeneration =
                worldManager.GetActiveWorldHandle()
                    .generation;
            const auto activePackage =
                RendererResourceCache::GetInstance()
                    .CaptureActiveWorldLocalResources();
            const std::shared_ptr<MaterialInstance>
                committedInstance =
                    FindShaderReloadTestMaterialInstance();
            if (committedGeneration <= oldGeneration ||
                !activePackage ||
                activePackage->ownerGeneration !=
                    committedGeneration ||
                RenderGraph::GetInstance()
                        .GetOwnerGeneration() !=
                    committedGeneration ||
                RenderSystem::GetInstance()
                        .GetActiveWorldGeneration() !=
                    committedGeneration ||
                engineLoop.controller
                        ->GetBoundWorldGeneration() !=
                    committedGeneration ||
                !committedInstance ||
                !committedInstance->HasParameter(
                    "u_worldGraphTransactionCandidate") ||
                FindShaderReloadTestPrimaryTexture().get() !=
                    retainedTexture.get())
            {
                throw std::runtime_error(
                    "candidate M_ success did not atomically publish generations, schema, and compatible texture");
            }

            const std::weak_ptr<void> retiredWorld =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:World",
                    oldGeneration);
            const std::weak_ptr<void> retiredPackage =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:WorldLocalResources",
                    oldGeneration);
            const std::weak_ptr<void> retiredGraph =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:RenderGraph",
                    oldGeneration);
            if (retiredWorld.expired() ||
                retiredPackage.expired() ||
                retiredGraph.expired() ||
                retiredWorld.lock().get() != oldWorld.get() ||
                retiredPackage.lock().get() != oldPackage.get())
            {
                throw std::runtime_error(
                    "candidate M_ success did not enqueue the exact old runtime packages");
            }
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredWorld);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredPackage);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredGraph);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                std::static_pointer_cast<void>(oldMaterial));
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                std::static_pointer_cast<void>(oldInstance));
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                std::static_pointer_cast<void>(oldObject));
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    retireQueue.GetPendingCount());
            diagnostics.ReportInfo(
                "World/graph transaction candidate M_ commit passed: oldGeneration=" +
                std::to_string(oldGeneration) +
                ", newGeneration=" +
                std::to_string(committedGeneration) +
                ", retirePending=" +
                std::to_string(retireQueue.GetPendingCount()) +
                ".");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::RestoreOriginalCommit);
            return;
        }

        case WorldGraphTransactionTestPhase::RestoreOriginalCommit:
        {
            WriteTextFileAtomically(
                worldGraphTransactionTestSourcePath,
                worldGraphTransactionTestOriginalSource);
            const uint64_t candidateGeneration =
                engineLoop.GetSubsystems()
                    .GetWorldManager()
                    .GetActiveWorldHandle()
                    .generation;
            const std::shared_ptr<World> candidateWorld =
                engineLoop.GetSubsystems()
                    .GetWorldManager()
                    .GetActiveWorld();
            const auto candidatePackage =
                std::const_pointer_cast<
                    RendererResourceCache::
                        WorldLocalResourcePackage>(
                    RendererResourceCache::GetInstance()
                        .CaptureActiveWorldLocalResources());
            const std::shared_ptr<Material> candidateMaterial =
                FindShaderReloadTestMaterial();
            const std::shared_ptr<MaterialInstance>
                candidateInstance =
                    FindShaderReloadTestMaterialInstance();
            const std::shared_ptr<RendererObjectResourceEntry>
                candidateObject =
                    FindShaderReloadTestObjectResources();
            const std::shared_ptr<Texture> retainedTexture =
                FindShaderReloadTestPrimaryTexture();
            const RuntimeResult<WorldHandle> result =
                engineLoop
                    .ExecuteMaterialDefinitionWorldGraphTransactionForTest(
                        {"runtimeTest/M_shaderReloadTest.json"},
                        worldGraphTransactionTestNextBatchId++);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "original M_ restore transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }
            const uint64_t restoredGeneration =
                engineLoop.GetSubsystems()
                    .GetWorldManager()
                    .GetActiveWorldHandle()
                    .generation;
            if (restoredGeneration <= candidateGeneration ||
                RenderGraph::GetInstance()
                        .GetOwnerGeneration() !=
                    restoredGeneration ||
                RenderSystem::GetInstance()
                        .GetActiveWorldGeneration() !=
                    restoredGeneration ||
                engineLoop.controller
                        ->GetBoundWorldGeneration() !=
                    restoredGeneration)
            {
                throw std::runtime_error(
                    "original schema restore did not commit all runtime owners together");
            }
            const std::shared_ptr<MaterialInstance>
                restoredInstance =
                    FindShaderReloadTestMaterialInstance();
            if (!restoredInstance ||
                restoredInstance->HasParameter(
                    "u_worldGraphTransactionCandidate") ||
                FindShaderReloadTestPrimaryTexture().get() !=
                    retainedTexture.get())
            {
                throw std::runtime_error(
                    "original M_ restore left candidate state active or replaced a compatible texture");
            }
            ResourceRetireQueue& retireQueue =
                ResourceRetireQueue::GetInstance();
            const std::weak_ptr<void> retiredWorld =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:World",
                    candidateGeneration);
            const std::weak_ptr<void> retiredPackage =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:WorldLocalResources",
                    candidateGeneration);
            const std::weak_ptr<void> retiredGraph =
                retireQueue.FindPendingResourceForTest(
                    "WorldTransaction:RenderGraph",
                    candidateGeneration);
            if (retiredWorld.expired() ||
                retiredPackage.expired() ||
                retiredGraph.expired() ||
                retiredWorld.lock().get() !=
                    candidateWorld.get() ||
                retiredPackage.lock().get() !=
                    candidatePackage.get())
            {
                throw std::runtime_error(
                    "schema restore did not enqueue the exact candidate packages for retirement");
            }
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredWorld);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredPackage);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredGraph);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                std::static_pointer_cast<void>(
                    candidateMaterial));
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                std::static_pointer_cast<void>(
                    candidateInstance));
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                std::static_pointer_cast<void>(
                    candidateObject));
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    retireQueue.GetPendingCount());
            diagnostics.ReportInfo(
                "World/graph transaction original schema restore passed: "
                "candidateGeneration=" +
                std::to_string(candidateGeneration) +
                ", restoredGeneration=" +
                std::to_string(restoredGeneration) +
                ", retirePending=" +
                std::to_string(retireQueue.GetPendingCount()) +
                ".");
            worldGraphTransactionTestPhase =
                WorldGraphTransactionTestPhase::WaitRetireDrain;
            worldGraphTransactionTestRetireDrainFramesRemaining =
                RetireDrainFrameBudget;
            return;
        }

        case WorldGraphTransactionTestPhase::ResizeFatalFailure:
        {
            const vk::Extent2D extent =
                engineLoop.rendererBackend
                    ->GetSwapchainExtent();
            injection.failResizeAfterSwapchainRecreate =
                true;
            engineLoop.SetWorldGraphTransactionTestFaultInjection(
                injection);
            const RuntimeResult<void> result =
                engineLoop.RecreateRendererForWindowResize(
                    extent.width,
                    extent.height);
            engineLoop.SetWorldGraphTransactionTestFaultInjection(
                {});
            if (result.IsSuccess() ||
                FormatRuntimeError(result.Error()).find(
                    "Injected resize failure after swapchain recreation") ==
                    std::string::npos ||
                !engineLoop.shouldClose ||
                engineLoop.exitCode != 0)
            {
                throw std::runtime_error(
                    "post-swapchain resize failure did not stop the runtime with the expected fatal contract");
            }

            if (worldGraphTransactionTestMonitorSuspended &&
                worldGraphTransactionTestMonitor != nullptr)
            {
                worldGraphTransactionTestMonitor
                    ->RefreshBaselineForSources({
                        "runtimeTest/M_shaderReloadTest.json"});
                worldGraphTransactionTestMonitor
                    ->SetTestScanSuspended(false);
            }
            worldGraphTransactionTestMonitorSuspended = false;
            worldGraphTransactionTestMonitor = nullptr;
            worldGraphTransactionTestSourcePath.clear();
            worldGraphTransactionTestOriginalSource.clear();
            worldGraphTransactionTestCandidateSource.clear();
            worldGraphTransactionTestHighLightScenePath.clear();
            worldGraphTransactionTestResourcesExpectedToExpire.clear();
            worldGraphTransactionTestActive = false;
            worldGraphTransactionTestPhase =
                WorldGraphTransactionTestPhase::Idle;
            runtimeTestStatus =
                RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                worldGraphTransactionTestFixtureDirectory,
                diagnostics);
            worldGraphTransactionTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "World/graph transaction runtime test completed: "
                "World and graph rollback faults rendered through, "
                "synchronized World/light commit, graph-only commit, "
                "minimize/resize, shader/M_ interleave, retirement drain, "
                "and fatal post-swapchain failure checks passed.");
            return;
        }

        case WorldGraphTransactionTestPhase::Idle:
        case WorldGraphTransactionTestPhase::WaitWorldLoad:
        case WorldGraphTransactionTestPhase::WaitRetireDrain:
            return;
        }
    }
    catch (const std::exception& exception)
    {
        if (engineLoop.shouldClose)
        {
            engineLoop.exitCode = 2;
        }
        engineLoop.SetWorldGraphTransactionTestFaultInjection({});
        FailWorldGraphTransactionTest(
            std::string(
                "World/graph transaction runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

void RuntimeTestHooks::UpdateShaderComputeReloadTest(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderComputeReloadTestActive)
    {
        return;
    }

    if (shaderComputeReloadTestPhase ==
        ShaderComputeReloadTestPhase::WaitRetireDrain)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const size_t pending = retireQueue.GetPendingCount();
        shaderComputeReloadTestMaxPendingRetiredResources =
            std::max(
                shaderComputeReloadTestMaxPendingRetiredResources,
                pending);
        if (pending == 0)
        {
            if (shaderComputeReloadTestMaxPendingRetiredResources == 0)
            {
                FailShaderComputeReloadTest(
                    "Shader compute reload runtime test did not observe any "
                    "epoch-retired compute resources.",
                    diagnostics);
                return;
            }
            shaderComputeReloadTestActive = false;
            shaderComputeReloadTestPhase =
                ShaderComputeReloadTestPhase::Idle;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                shaderComputeReloadTestFixtureDirectory,
                diagnostics);
            shaderComputeReloadTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "Shader compute reload runtime test completed: "
                "ABI-compatible compute batch commit, ABI rejection with "
                "descriptor retention, recovery, and epoch retirement passed.");
            return;
        }

        --shaderComputeReloadTestRetireDrainFramesRemaining;
        if (shaderComputeReloadTestRetireDrainFramesRemaining <= 0)
        {
            FailShaderComputeReloadTest(
                "Shader compute reload runtime test timed out waiting for "
                "retired resources to drain. pending=" +
                    std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        if (shaderComputeReloadTestPhaseEntryPending)
        {
            shaderComputeReloadTestPhaseEntryPending = false;
            WriteTextFileAtomically(
                shaderComputeSkyShSourcePath,
                shaderComputeSkyShCompatible);
            WriteTextFileAtomically(
                shaderComputePrefilterSourcePath,
                shaderComputePrefilterCompatible);
            shaderComputeSkyShBaselineGeneration =
                RenderSystem::GetInstance()
                    .GetActiveComputeShaderArtifact(
                        "generator/skySHGenerate")
                    .artifactGenerationKey;
            shaderComputePrefilterBaselineGeneration =
                RenderSystem::GetInstance()
                    .GetActiveComputeShaderArtifact(
                        "generator/prefilterEnvMap")
                    .artifactGenerationKey;
            shaderComputeReloadTestBaselineLatestGeneration =
                engineLoop.latestSubmittedAutoReloadGeneration;
            shaderComputeReloadTestDeadline =
                std::chrono::steady_clock::now() +
                ShaderAsyncWaitTimeout;
        }

        const bool pendingSourcesSettled =
            engineLoop.pendingAutoReloadSources.empty() ||
            (engineLoop.failedPendingAutoReloadSourceEpoch != 0 &&
             engineLoop.failedPendingAutoReloadSourceEpoch ==
                 engineLoop.pendingAutoReloadSourceEpoch);
        const bool workerSettled =
            engineLoop.shaderCompileWorker != nullptr &&
            engineLoop.shaderCompileWorker->IsIdle() &&
            pendingSourcesSettled &&
            engineLoop.inFlightAutoReloadGeneration == 0;
        const std::string skyGeneration =
            RenderSystem::GetInstance()
                .GetActiveComputeShaderArtifact(
                    "generator/skySHGenerate")
                .artifactGenerationKey;
        const std::string prefilterGeneration =
            RenderSystem::GetInstance()
                .GetActiveComputeShaderArtifact(
                    "generator/prefilterEnvMap")
                .artifactGenerationKey;

        switch (shaderComputeReloadTestPhase)
        {
        case ShaderComputeReloadTestPhase::WaitCompatibleCommit:
            if (workerSettled &&
                engineLoop.latestSubmittedAutoReloadGeneration >
                    shaderComputeReloadTestBaselineLatestGeneration)
            {
                if (skyGeneration ==
                        shaderComputeSkyShBaselineGeneration ||
                    prefilterGeneration ==
                        shaderComputePrefilterBaselineGeneration)
                {
                    throw std::runtime_error(
                        "compatible compute batch did not commit both "
                        "pipelines");
                }
                diagnostics.ReportInfo(
                    "Shader compute reload runtime test passed the "
                    "ABI-compatible compute batch commit.");
                WriteTextFileAtomically(
                    shaderComputeSkyShSourcePath,
                    shaderComputeSkyShAbiIncompatible);
                shaderComputeSkyShBaselineGeneration =
                    skyGeneration;
                shaderComputePrefilterBaselineGeneration =
                    prefilterGeneration;
                shaderComputeReloadTestBaselineLatestGeneration =
                    engineLoop.latestSubmittedAutoReloadGeneration;
                shaderComputeReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderComputeReloadTestPhase =
                    ShaderComputeReloadTestPhase::WaitAbiRejection;
            }
            break;

        case ShaderComputeReloadTestPhase::WaitAbiRejection:
            if (workerSettled &&
                engineLoop.latestSubmittedAutoReloadGeneration >
                    shaderComputeReloadTestBaselineLatestGeneration)
            {
                if (skyGeneration !=
                        shaderComputeSkyShBaselineGeneration ||
                    prefilterGeneration !=
                        shaderComputePrefilterBaselineGeneration)
                {
                    throw std::runtime_error(
                        "ABI-incompatible compute edit changed a live "
                        "pipeline or descriptor package");
                }
                diagnostics.ReportInfo(
                    "Shader compute reload runtime test passed ABI "
                    "rejection with old resource retention.");
                WriteTextFileAtomically(
                    shaderComputeSkyShSourcePath,
                    shaderComputeSkyShOriginal);
                WriteTextFileAtomically(
                    shaderComputePrefilterSourcePath,
                    shaderComputePrefilterOriginal);
                shaderComputeSkyShBaselineGeneration =
                    skyGeneration;
                shaderComputePrefilterBaselineGeneration =
                    prefilterGeneration;
                shaderComputeReloadTestBaselineLatestGeneration =
                    engineLoop.latestSubmittedAutoReloadGeneration;
                shaderComputeReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderComputeReloadTestPhase =
                    ShaderComputeReloadTestPhase::RestoreOriginal;
            }
            break;

        case ShaderComputeReloadTestPhase::RestoreOriginal:
            if (workerSettled &&
                engineLoop.latestSubmittedAutoReloadGeneration >
                    shaderComputeReloadTestBaselineLatestGeneration)
            {
                if (skyGeneration ==
                        shaderComputeSkyShBaselineGeneration ||
                    prefilterGeneration ==
                        shaderComputePrefilterBaselineGeneration)
                {
                    throw std::runtime_error(
                        "compute original restore did not commit both "
                        "pipelines");
                }
                diagnostics.ReportInfo(
                    "Shader compute reload runtime test passed original "
                    "source restore.");
                shaderComputeReloadTestPhase =
                    ShaderComputeReloadTestPhase::WaitRetireDrain;
                shaderComputeReloadTestRetireDrainFramesRemaining =
                    RetireDrainFrameBudget;
                shaderComputeReloadTestMaxPendingRetiredResources =
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount();
                return;
            }
            break;

        case ShaderComputeReloadTestPhase::Idle:
        case ShaderComputeReloadTestPhase::WaitWorldLoad:
        case ShaderComputeReloadTestPhase::WaitRetireDrain:
            break;
        }

        if (std::chrono::steady_clock::now() >=
            shaderComputeReloadTestDeadline)
        {
            throw std::runtime_error(
                "timed out waiting for the compute reload pipeline to "
                "settle in phase " +
                std::to_string(
                    static_cast<int>(shaderComputeReloadTestPhase)) +
                "; skyGen=" + skyGeneration +
                ", baselineSky=" +
                    shaderComputeSkyShBaselineGeneration +
                ", prefilterGen=" + prefilterGeneration +
                ", latest=" +
                    std::to_string(
                        engineLoop.latestSubmittedAutoReloadGeneration) +
                ", baselineLatest=" +
                    std::to_string(
                        shaderComputeReloadTestBaselineLatestGeneration) +
                ", workerIdle=" +
                    std::string(
                        engineLoop.shaderCompileWorker != nullptr &&
                                engineLoop.shaderCompileWorker->IsIdle()
                            ? "true"
                            : "false") +
                ", hasResult=" +
                    std::string(
                        engineLoop.shaderCompileWorker != nullptr &&
                                engineLoop.shaderCompileWorker
                                    ->HasCompletedResult()
                            ? "true"
                            : "false") +
                ", inFlight=" +
                    std::to_string(
                        engineLoop.shaderCompileWorker != nullptr
                            ? engineLoop.shaderCompileWorker
                                  ->GetInFlightGeneration()
                            : 0) +
                ", pendingPlan=" +
                    std::string(
                        !engineLoop.pendingAutoReloadSources.empty()
                            ? "true"
                            : "false"));
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderComputeReloadTest(
            std::string(
                "Shader compute reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

void RuntimeTestHooks::UpdateShaderDefinitionReloadTest(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderDefinitionReloadTestActive)
    {
        return;
    }

    if (shaderDefinitionReloadTestPhase ==
        ShaderDefinitionReloadTestPhase::WaitRetireDrain)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const size_t pending = retireQueue.GetPendingCount();
        shaderDefinitionReloadTestMaxPendingRetiredResources =
            std::max(
                shaderDefinitionReloadTestMaxPendingRetiredResources,
                pending);
        if (pending == 0)
        {
            if (shaderDefinitionReloadTestMaxPendingRetiredResources == 0)
            {
                FailShaderDefinitionReloadTest(
                    "Shader definition reload runtime test did not observe "
                    "any epoch-retired material resources.",
                    diagnostics);
                return;
            }
            if (!shaderDefinitionReloadTestOldMaterial.expired() ||
                !shaderDefinitionReloadTestOldMaterialInstance.expired() ||
                !shaderDefinitionReloadTestOldObjectResources.expired() ||
                !shaderDefinitionReloadTestOldPrimaryTexture.expired() ||
                !shaderDefinitionReloadTestOldRetiredTexture.expired())
            {
                FailShaderDefinitionReloadTest(
                    "Shader definition reload runtime test drained the "
                    "retire queue while old Material/MI/Texture/descriptor "
                    "owners were still alive.",
                    diagnostics);
                return;
            }
            if (shaderDefinitionReloadTestRetainedTexture.expired())
            {
                FailShaderDefinitionReloadTest(
                    "Shader definition reload runtime test lost the "
                    "compatible migrated Texture after retirement drain.",
                    diagnostics);
                return;
            }
            ValidateShaderDefinitionMigratedState(
                FindShaderReloadTestMaterialInstance(),
                shaderDefinitionReloadTestRetainedTextureIdentity,
                false,
                false,
                false);
            if (!engineLoop.pendingMaterialDefinitionSources.empty())
            {
                FailShaderDefinitionReloadTest(
                    "Shader definition reload runtime test completed with "
                    "pending material definition sources.",
                    diagnostics);
                return;
            }
            shaderDefinitionReloadTestActive = false;
            shaderDefinitionReloadTestPhase =
                ShaderDefinitionReloadTestPhase::Idle;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                shaderDefinitionReloadTestFixtureDirectory,
                diagnostics);
            shaderDefinitionReloadTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "Shader material definition reload runtime test completed: "
                "runtime parameter and Texture migration, field deletion, "
                "type/required-texture/malformed/include failures, multi-M_ "
                "all-or-nothing recovery, restore, and epoch retirement "
                "passed. retainedTexture=" +
                std::to_string(
                    shaderDefinitionReloadTestRetainedTextureIdentity) +
                ", maxRetirePending=" +
                std::to_string(
                    shaderDefinitionReloadTestMaxPendingRetiredResources) +
                ".");
            return;
        }

        --shaderDefinitionReloadTestRetireDrainFramesRemaining;
        if (shaderDefinitionReloadTestRetireDrainFramesRemaining <= 0)
        {
            FailShaderDefinitionReloadTest(
                "Shader definition reload runtime test timed out waiting "
                "for retired resources to drain. pending=" +
                    std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        if (shaderDefinitionReloadTestPhaseEntryPending)
        {
            shaderDefinitionReloadTestPhaseEntryPending = false;
            const std::shared_ptr<Material> entryMaterial =
                FindShaderReloadTestMaterial();
            const std::shared_ptr<MaterialInstance>
                entryInstance =
                    FindShaderReloadTestMaterialInstance();
            const std::shared_ptr<
                RendererObjectResourceEntry>
                    entryObjectResources =
                        FindShaderReloadTestObjectResources();
            if (!entryMaterial || !entryInstance ||
                !entryObjectResources)
            {
                throw std::runtime_error(
                    "definition fixture runtime owners are not live");
            }

            const MaterialInstanceStateSnapshot entrySnapshot =
                entryInstance->CaptureStateSnapshot();
            const auto primaryTextureIt =
                entrySnapshot.textures.find(
                    "u_reloadTexture");
            const auto alternateTextureIt =
                entrySnapshot.textures.find(
                    "u_reloadAlternateTexture");
            const auto retiredTextureIt =
                entrySnapshot.textures.find(
                    "u_zReloadRetiredTexture");
            if (primaryTextureIt ==
                    entrySnapshot.textures.end() ||
                alternateTextureIt ==
                    entrySnapshot.textures.end() ||
                retiredTextureIt ==
                    entrySnapshot.textures.end() ||
                !primaryTextureIt->second.texture ||
                !alternateTextureIt->second.texture ||
                !retiredTextureIt->second.texture)
            {
                throw std::runtime_error(
                    "definition fixture did not load all Texture bindings");
            }

            entryInstance->SetParameter(
                "u_reloadRuntimeScalar",
                0.875f);
            entryInstance->SetParameter(
                "u_reloadTestColor",
                Eigen::Vector4f(
                    0.85f,
                    0.15f,
                    0.45f,
                    0.65f));
            entryInstance->SetTexture(
                "u_reloadTexture",
                alternateTextureIt->second.texture,
                alternateTextureIt->second
                    .textureAssetIdentity,
                alternateTextureIt->second
                    .textureCacheIdentity);

            shaderDefinitionReloadTestOldMaterial =
                entryMaterial;
            shaderDefinitionReloadTestOldMaterialInstance =
                entryInstance;
            shaderDefinitionReloadTestOldObjectResources =
                entryObjectResources;
            shaderDefinitionReloadTestOldPrimaryTexture =
                primaryTextureIt->second.texture;
            shaderDefinitionReloadTestOldRetiredTexture =
                retiredTextureIt->second.texture;
            shaderDefinitionReloadTestRetainedTexture =
                alternateTextureIt->second.texture;
            shaderDefinitionReloadTestRetainedTextureIdentity =
                reinterpret_cast<std::uintptr_t>(
                    alternateTextureIt->second.texture.get());
            shaderDefinitionReloadTestRetainedTextureAssetIdentity =
                alternateTextureIt->second
                    .textureAssetIdentity.value_or(
                        std::string());
            shaderDefinitionReloadTestInitialDescriptorPoolIdentity =
                GetObjectDescriptorPoolIdentity(
                    entryObjectResources);
            shaderDefinitionReloadTestBaselineWorldGeneration =
                engineLoop.GetSubsystems()
                    .GetWorldManager()
                    .GetActiveWorldHandle()
                    .generation;
            shaderDefinitionReloadTestBaselineParameterCount =
                entryMaterial
                ? static_cast<int>(
                      entryMaterial->GetMaterialDescriptorSchema()
                          .GetParameters()
                          .size())
                : 0;
            shaderDefinitionReloadTestBaselineCommittedGeneration =
                engineLoop
                    .latestMaterialDefinitionReloadCommittedGeneration;
            shaderDefinitionReloadTestBaselineFingerprint =
                CaptureShaderDefinitionRuntimeFingerprint(
                    engineLoop);
            WriteTextFileAtomically(
                shaderDefinitionReloadTestSourcePath,
                shaderDefinitionReloadTestExtended);
            shaderDefinitionReloadTestDeadline =
                std::chrono::steady_clock::now() +
                ShaderDefinitionWaitTimeout;
            diagnostics.ReportInfo(
                "Shader definition reload runtime test mutated live state: "
                "u_reloadRuntimeScalar=0.875, "
                "u_reloadTestColor=[0.85,0.15,0.45,0.65], "
                "u_reloadTexturePointer=" +
                std::to_string(
                    shaderDefinitionReloadTestRetainedTextureIdentity) +
                ", u_reloadTextureAsset=" +
                shaderDefinitionReloadTestRetainedTextureAssetIdentity +
                ", oldDescriptorPool=" +
                std::to_string(
                    shaderDefinitionReloadTestInitialDescriptorPoolIdentity) +
                ", baselineFingerprint=" +
                shaderDefinitionReloadTestBaselineFingerprint +
                ".");
        }

        const std::shared_ptr<Material> material =
            FindShaderReloadTestMaterial();
        const int parameterCount =
            material
            ? static_cast<int>(
                  material->GetMaterialDescriptorSchema()
                      .GetParameters()
                      .size())
            : 0;

        switch (shaderDefinitionReloadTestPhase)
        {
        case ShaderDefinitionReloadTestPhase::WaitAddedParameter:
            if (engineLoop
                    .latestMaterialDefinitionReloadCommittedGeneration >
                shaderDefinitionReloadTestBaselineCommittedGeneration)
            {
                std::shared_ptr<MaterialInstance> instance =
                    FindShaderReloadTestMaterialInstance();
                ValidateShaderDefinitionMigratedState(
                    instance,
                    shaderDefinitionReloadTestRetainedTextureIdentity,
                    true,
                    true,
                    false);
                if (parameterCount !=
                    shaderDefinitionReloadTestBaselineParameterCount + 2)
                {
                    throw std::runtime_error(
                        "extended schema parameter count is incorrect");
                }
                const uint64_t descriptorPoolIdentity =
                    GetObjectDescriptorPoolIdentity(
                        FindShaderReloadTestObjectResources());
                if (descriptorPoolIdentity == 0 ||
                    descriptorPoolIdentity ==
                        shaderDefinitionReloadTestInitialDescriptorPoolIdentity)
                {
                    throw std::runtime_error(
                        "extended schema did not create a replacement "
                        "descriptor package");
                }
                diagnostics.ReportInfo(
                    "Shader definition reload runtime test passed added-field "
                    "migration: scalar=0.875, extraDefault=0.5, "
                    "removedDefault=0.375, retainedTexture=" +
                    std::to_string(
                        shaderDefinitionReloadTestRetainedTextureIdentity) +
                    ", descriptorPool=" +
                    std::to_string(
                        descriptorPoolIdentity) +
                    ", worldGeneration=" +
                    std::to_string(
                        engineLoop.GetSubsystems()
                            .GetWorldManager()
                            .GetActiveWorldHandle()
                            .generation) +
                    ".");
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestDeleted);
                shaderDefinitionReloadTestBaselineCommittedGeneration =
                    engineLoop
                        .latestMaterialDefinitionReloadCommittedGeneration;
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitDeletedParameter;
            }
            break;

        case ShaderDefinitionReloadTestPhase::WaitDeletedParameter:
            if (engineLoop
                    .latestMaterialDefinitionReloadCommittedGeneration >
                shaderDefinitionReloadTestBaselineCommittedGeneration)
            {
                const std::shared_ptr<MaterialInstance> instance =
                    FindShaderReloadTestMaterialInstance();
                ValidateShaderDefinitionMigratedState(
                    instance,
                    shaderDefinitionReloadTestRetainedTextureIdentity,
                    true,
                    false,
                    false);
                if (instance->HasTexture(
                        "u_zReloadRetiredTexture"))
                {
                    throw std::runtime_error(
                        "deleted Texture schema field survived in the "
                        "candidate MaterialInstance");
                }
                diagnostics.ReportInfo(
                    "Shader definition reload runtime test passed deleted "
                    "field migration: u_reloadRemoved absent, retiredTexture "
                    "absent, compatible runtime values retained.");
                shaderDefinitionReloadTestBaselineFingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        engineLoop);
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    engineLoop
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestTypeMismatch);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitTypeMismatchRetention;
            }
            break;

        case ShaderDefinitionReloadTestPhase::WaitTypeMismatchRetention:
            if (engineLoop
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        engineLoop);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "type-mismatched schema changed the live package: "
                        "before=" +
                        shaderDefinitionReloadTestBaselineFingerprint +
                        ", after=" + fingerprint);
                }
                diagnostics.ReportInfo(
                    "Shader definition rollback fingerprint preserved after "
                    "same-name type mismatch: " + fingerprint + ".");
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    engineLoop
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestRequiredTextureMissing);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitRequiredTextureFailure;
            }
            break;

        case ShaderDefinitionReloadTestPhase::WaitRequiredTextureFailure:
            if (engineLoop
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        engineLoop);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "missing required Texture changed the live package");
                }
                diagnostics.ReportInfo(
                    "Shader definition rollback fingerprint preserved after "
                    "missing required Texture: " + fingerprint + ".");
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    engineLoop
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestMalformed);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitMalformedDefinitionFailure;
            }
            break;

        case ShaderDefinitionReloadTestPhase::
            WaitMalformedDefinitionFailure:
            if (engineLoop
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        engineLoop);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "malformed M_ JSON changed the live package");
                }
                diagnostics.ReportInfo(
                    "Shader definition rollback fingerprint preserved after "
                    "malformed M_ JSON: " + fingerprint + ".");
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    engineLoop
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestIncludeGenerationFailure);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitIncludeGenerationFailure;
            }
            break;

        case ShaderDefinitionReloadTestPhase::
            WaitIncludeGenerationFailure:
            if (engineLoop
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        engineLoop);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "generated include validation failure changed the "
                        "live package");
                }
                diagnostics.ReportInfo(
                    "Shader definition rollback fingerprint preserved after "
                    "generated include validation failure: " +
                    fingerprint + ".");
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    engineLoop
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestMultiMain);
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestBatchSourcePath,
                    shaderDefinitionReloadTestMultiBatchInvalid);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitMultiDefinitionFailure;
            }
            break;

        case ShaderDefinitionReloadTestPhase::
            WaitMultiDefinitionFailure:
            if (engineLoop
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        engineLoop);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "multi-M_ failed batch changed the live package");
                }
                if (engineLoop.pendingMaterialDefinitionSources.count(
                        "runtimeTest/M_shaderReloadTest.json") == 0 ||
                    engineLoop.pendingMaterialDefinitionSources.count(
                        "runtimeTest/M_shaderReloadBatchTest.json") == 0)
                {
                    throw std::runtime_error(
                        "multi-M_ failed batch did not retain the complete "
                        "pending source union");
                }
                diagnostics.ReportInfo(
                    "Shader definition multi-M_ rollback preserved "
                    "fingerprint=" + fingerprint +
                    ", pendingUnion=" +
                    std::to_string(
                        engineLoop.pendingMaterialDefinitionSources.size()) +
                    ".");
                shaderDefinitionReloadTestBaselineCommittedGeneration =
                    engineLoop
                        .latestMaterialDefinitionReloadCommittedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestBatchSourcePath,
                    shaderDefinitionReloadTestMultiBatchValid);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitMultiDefinitionRecovery;
            }
            break;

        case ShaderDefinitionReloadTestPhase::
            WaitMultiDefinitionRecovery:
            if (engineLoop
                    .latestMaterialDefinitionReloadCommittedGeneration >
                shaderDefinitionReloadTestBaselineCommittedGeneration)
            {
                ValidateShaderDefinitionMigratedState(
                    FindShaderReloadTestMaterialInstance(),
                    shaderDefinitionReloadTestRetainedTextureIdentity,
                    true,
                    false,
                    true);
                const std::shared_ptr<MaterialInstance> batchInstance =
                    FindShaderReloadBatchTestMaterialInstance();
                if (!batchInstance ||
                    !batchInstance->HasParameter(
                        "u_reloadBatchExtra") ||
                    batchInstance->GetParameter<float>(
                        "u_reloadBatchExtra") != 0.75f)
                {
                    throw std::runtime_error(
                        "multi-M_ recovery did not publish both candidate "
                        "schemas");
                }
                diagnostics.ReportInfo(
                    "Shader definition multi-M_ recovery committed both "
                    "schemas atomically: generation=" +
                    std::to_string(
                        engineLoop
                            .latestMaterialDefinitionReloadCommittedGeneration) +
                    ", retainedTexture=" +
                    std::to_string(
                        shaderDefinitionReloadTestRetainedTextureIdentity) +
                    ".");
                shaderDefinitionReloadTestBaselineCommittedGeneration =
                    engineLoop
                        .latestMaterialDefinitionReloadCommittedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestOriginal);
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestBatchSourcePath,
                    shaderDefinitionReloadTestBatchOriginal);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::RestoreOriginal;
            }
            break;

        case ShaderDefinitionReloadTestPhase::RestoreOriginal:
            if (engineLoop
                    .latestMaterialDefinitionReloadCommittedGeneration >
                shaderDefinitionReloadTestBaselineCommittedGeneration)
            {
                ValidateShaderDefinitionMigratedState(
                    FindShaderReloadTestMaterialInstance(),
                    shaderDefinitionReloadTestRetainedTextureIdentity,
                    false,
                    false,
                    false);
                const std::shared_ptr<MaterialInstance> batchInstance =
                    FindShaderReloadBatchTestMaterialInstance();
                if (!batchInstance ||
                    batchInstance->HasParameter(
                        "u_reloadBatchExtra"))
                {
                    throw std::runtime_error(
                        "original secondary M_ schema was not restored");
                }
                diagnostics.ReportInfo(
                    "Shader definition reload runtime test passed original "
                    "schema restore while retaining compatible live values.");
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::WaitRetireDrain;
                shaderDefinitionReloadTestRetireDrainFramesRemaining =
                    RetireDrainFrameBudget;
                shaderDefinitionReloadTestMaxPendingRetiredResources =
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount();
                return;
            }
            if (std::chrono::steady_clock::now() >=
                shaderDefinitionReloadTestDeadline)
            {
                throw std::runtime_error(
                    "timed out waiting for the original schema restore");
            }
            break;

        case ShaderDefinitionReloadTestPhase::Idle:
        case ShaderDefinitionReloadTestPhase::WaitWorldLoad:
        case ShaderDefinitionReloadTestPhase::WaitRetireDrain:
            break;
        }

        if (shaderDefinitionReloadTestPhase !=
                ShaderDefinitionReloadTestPhase::WaitRetireDrain &&
            shaderDefinitionReloadTestPhase !=
                ShaderDefinitionReloadTestPhase::Idle &&
            shaderDefinitionReloadTestPhase !=
                ShaderDefinitionReloadTestPhase::WaitWorldLoad)
        {
            if (std::chrono::steady_clock::now() >=
                shaderDefinitionReloadTestDeadline)
            {
                throw std::runtime_error(
                    "timed out waiting for the current definition reload "
                    "matrix phase");
            }
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderDefinitionReloadTest(
            std::string(
                "Shader definition reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

void RuntimeTestHooks::FailShaderUiReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderUiReloadTestVertexPath.empty() &&
        !shaderUiReloadTestVertexOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderUiReloadTestVertexPath,
                shaderUiReloadTestVertexOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderUiReloadTestFragmentPath.empty() &&
        !shaderUiReloadTestFragmentOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderUiReloadTestFragmentPath,
                shaderUiReloadTestFragmentOriginal);
        }
        catch (...)
        {
        }
    }
    shaderUiReloadTestActive = false;
    waitingForShaderUiReloadTestWorld = false;
    shaderUiReloadTestPhaseEntryPending = false;
    shaderUiReloadTestPhase =
        ShaderUiReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderUiReloadTestFixtureDirectory,
        diagnostics);
    shaderUiReloadTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::UpdateShaderUiReloadTest(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderUiReloadTestActive)
    {
        return;
    }

    if (shaderUiReloadTestPhase ==
        ShaderUiReloadTestPhase::WaitRetireDrain)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const size_t pending = retireQueue.GetPendingCount();
        shaderUiReloadTestMaxPendingRetiredResources =
            std::max(
                shaderUiReloadTestMaxPendingRetiredResources,
                pending);
        if (pending == 0)
        {
            if (shaderUiReloadTestMaxPendingRetiredResources == 0)
            {
                FailShaderUiReloadTest(
                    "Shader UI overlay reload runtime test did not observe "
                    "any epoch-retired UI pipeline resources.",
                    diagnostics);
                return;
            }
            shaderUiReloadTestActive = false;
            shaderUiReloadTestPhase =
                ShaderUiReloadTestPhase::Idle;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                shaderUiReloadTestFixtureDirectory,
                diagnostics);
            shaderUiReloadTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "Shader UI overlay reload runtime test completed: "
                "ABI-compatible pair swap, ABI rejection, restore, and "
                "epoch retirement passed.");
            return;
        }

        --shaderUiReloadTestRetireDrainFramesRemaining;
        if (shaderUiReloadTestRetireDrainFramesRemaining <= 0)
        {
            FailShaderUiReloadTest(
                "Shader UI overlay reload runtime test timed out waiting "
                "for retired resources to drain. pending=" +
                    std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        if (shaderUiReloadTestPhaseEntryPending)
        {
            shaderUiReloadTestPhaseEntryPending = false;
            WriteTextFileAtomically(
                shaderUiReloadTestFragmentPath,
                shaderUiReloadTestFragmentCompatible);
            shaderUiReloadTestBaselineLatestGeneration =
                engineLoop.latestSubmittedAutoReloadGeneration;
            shaderUiReloadTestDeadline =
                std::chrono::steady_clock::now() +
                ShaderAsyncWaitTimeout;
        }

        const bool pendingSourcesSettled =
            engineLoop.pendingAutoReloadSources.empty() ||
            (engineLoop.failedPendingAutoReloadSourceEpoch != 0 &&
             engineLoop.failedPendingAutoReloadSourceEpoch ==
                 engineLoop.pendingAutoReloadSourceEpoch);
        const bool workerSettled =
            engineLoop.shaderCompileWorker != nullptr &&
            engineLoop.shaderCompileWorker->IsIdle() &&
            pendingSourcesSettled &&
            engineLoop.inFlightAutoReloadGeneration == 0;
        const bool fragmentMatchesCurrent =
            UiArtifactFragmentMatchesCurrentSource(engineLoop);

        switch (shaderUiReloadTestPhase)
        {
        case ShaderUiReloadTestPhase::WaitCompatibleCommit:
            if (workerSettled &&
                engineLoop.latestSubmittedAutoReloadGeneration >
                    shaderUiReloadTestBaselineLatestGeneration)
            {
                if (!fragmentMatchesCurrent)
                {
                    throw std::runtime_error(
                        "compatible UI edit did not commit the candidate "
                        "pipeline pair");
                }
                diagnostics.ReportInfo(
                    "Shader UI overlay reload runtime test passed the "
                    "ABI-compatible pipeline pair swap.");
                WriteTextFileAtomically(
                    shaderUiReloadTestFragmentPath,
                    shaderUiReloadTestFragmentAbiIncompatible);
                shaderUiReloadTestBaselineLatestGeneration =
                    engineLoop.latestSubmittedAutoReloadGeneration;
                shaderUiReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderUiReloadTestPhase =
                    ShaderUiReloadTestPhase::WaitAbiRejection;
            }
            break;

        case ShaderUiReloadTestPhase::WaitAbiRejection:
            if (workerSettled &&
                engineLoop.latestSubmittedAutoReloadGeneration >
                    shaderUiReloadTestBaselineLatestGeneration)
            {
                if (fragmentMatchesCurrent)
                {
                    throw std::runtime_error(
                        "ABI-incompatible UI edit replaced the live "
                        "pipeline pair");
                }
                diagnostics.ReportInfo(
                    "Shader UI overlay reload runtime test passed ABI "
                    "rejection with old pipeline retention.");
                WriteTextFileAtomically(
                    shaderUiReloadTestFragmentPath,
                    shaderUiReloadTestFragmentOriginal);
                shaderUiReloadTestBaselineLatestGeneration =
                    engineLoop.latestSubmittedAutoReloadGeneration;
                shaderUiReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderUiReloadTestPhase =
                    ShaderUiReloadTestPhase::RestoreOriginal;
            }
            break;

        case ShaderUiReloadTestPhase::RestoreOriginal:
            if (workerSettled &&
                engineLoop.latestSubmittedAutoReloadGeneration >
                    shaderUiReloadTestBaselineLatestGeneration)
            {
                if (!fragmentMatchesCurrent)
                {
                    throw std::runtime_error(
                        "UI original restore did not commit the candidate "
                        "pipeline pair");
                }
                diagnostics.ReportInfo(
                    "Shader UI overlay reload runtime test passed original "
                    "source restore.");
                shaderUiReloadTestPhase =
                    ShaderUiReloadTestPhase::WaitRetireDrain;
                shaderUiReloadTestRetireDrainFramesRemaining =
                    RetireDrainFrameBudget;
                shaderUiReloadTestMaxPendingRetiredResources =
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount();
                return;
            }
            break;

        case ShaderUiReloadTestPhase::Idle:
        case ShaderUiReloadTestPhase::WaitWorldLoad:
        case ShaderUiReloadTestPhase::WaitRetireDrain:
            break;
        }

        if (std::chrono::steady_clock::now() >=
            shaderUiReloadTestDeadline)
        {
            throw std::runtime_error(
                "timed out waiting for the UI overlay reload pipeline to "
                "settle in phase " +
                std::to_string(
                    static_cast<int>(shaderUiReloadTestPhase)));
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderUiReloadTest(
            std::string(
                "Shader UI overlay reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

void RuntimeTestHooks::FailShaderShutdownInflightTest(
    EngineLoop& engineLoop,
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderShutdownInflightSourcePath.empty() &&
        !shaderShutdownInflightOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderShutdownInflightSourcePath,
                shaderShutdownInflightOriginalSource);
        }
        catch (...)
        {
        }
    }
    if (engineLoop.shaderFileMonitor != nullptr)
    {
        engineLoop.shaderFileMonitor
            ->SetTestScanSuspended(true);
    }
    shaderShutdownInflightTestActive = false;
    shaderShutdownInflightTestPhase =
        ShaderShutdownInflightTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    engineLoop.exitCode = 2;
    engineLoop.shouldClose = true;
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::UpdateShaderShutdownInflightTest(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderShutdownInflightTestActive)
    {
        return;
    }

    try
    {
        if (engineLoop.shaderFileMonitor == nullptr ||
            engineLoop.shaderCompileWorker == nullptr ||
            engineLoop.shaderCompiler == nullptr)
        {
            throw std::runtime_error(
                "shutdown-in-flight test lost its monitor, worker, or compiler");
        }

        switch (shaderShutdownInflightTestPhase)
        {
        case ShaderShutdownInflightTestPhase::WaitMonitorBaseline:
            if (engineLoop.shaderFileMonitor->GetScanCount() > 0 &&
                !engineLoop.shaderFileMonitor
                    ->HasUnstableSourceChanges() &&
                engineLoop.shaderCompileWorker->IsIdle() &&
                engineLoop.pendingAutoReloadSources.empty() &&
                engineLoop.inFlightAutoReloadGeneration == 0)
            {
                shaderShutdownInflightBaselineFingerprint =
                    CaptureShaderShutdownInflightFingerprint(
                        engineLoop);
                shaderShutdownInflightBaselineCommittedGeneration =
                    engineLoop.latestAutoReloadCommittedGeneration;
                engineLoop.shaderCompileWorker
                    ->ArmTestPostCompileGateForNextSubmit();
                engineLoop.shaderFileMonitor
                    ->SetTestPollInterval(
                        std::chrono::milliseconds(0));
                WriteTextFileAtomically(
                    shaderShutdownInflightSourcePath,
                    shaderShutdownInflightCandidateSource);
                shaderShutdownInflightTestPhase =
                    ShaderShutdownInflightTestPhase::WaitPostCompileGate;
                shaderShutdownInflightDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                diagnostics.ReportInfo(
                    "Shader shutdown-in-flight test armed the post-compile gate and changed uiOverlay.frag.");
            }
            break;

        case ShaderShutdownInflightTestPhase::WaitPostCompileGate:
            if (engineLoop.shaderCompileWorker
                    ->IsWaitingAtTestPostCompileGate())
            {
                shaderShutdownInflightCandidateGeneration =
                    engineLoop.shaderCompileWorker
                        ->GetInFlightGeneration();
                if (shaderShutdownInflightCandidateGeneration == 0 ||
                    engineLoop.latestSubmittedAutoReloadGeneration !=
                        shaderShutdownInflightCandidateGeneration)
                {
                    throw std::runtime_error(
                        "post-compile gate did not retain the submitted generation");
                }
                if (engineLoop.shaderCompileWorker
                        ->HasCompletedResult() ||
                    engineLoop.latestAutoReloadCommittedGeneration !=
                        shaderShutdownInflightBaselineCommittedGeneration)
                {
                    throw std::runtime_error(
                        "complete shutdown candidate was published or committed before shutdown");
                }
                const std::string currentFingerprint =
                    CaptureShaderShutdownInflightFingerprint(
                        engineLoop);
                if (currentFingerprint !=
                    shaderShutdownInflightBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "live or formal artifact fingerprint changed before shutdown");
                }

                engineLoop.shaderFileMonitor
                    ->SetTestScanSuspended(true);
                WriteTextFileAtomically(
                    shaderShutdownInflightSourcePath,
                    shaderShutdownInflightOriginalSource);
                shaderShutdownInflightTestPhase =
                    ShaderShutdownInflightTestPhase::AwaitShutdown;
                engineLoop.exitCode = 2;
                engineLoop.shouldClose = true;
                diagnostics.ReportInfo(
                    "Shader candidate ready at post-compile gate: generation=" +
                    std::to_string(
                        shaderShutdownInflightCandidateGeneration) +
                    "; live/formal fingerprint unchanged; shutdown requested.");
                return;
            }
            break;

        case ShaderShutdownInflightTestPhase::AwaitShutdown:
        case ShaderShutdownInflightTestPhase::Idle:
            break;
        }

        if (std::chrono::steady_clock::now() >=
            shaderShutdownInflightDeadline)
        {
            throw std::runtime_error(
                "shutdown-in-flight test timed out in phase " +
                std::to_string(
                    static_cast<int>(
                        shaderShutdownInflightTestPhase)));
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderShutdownInflightTest(
            engineLoop,
            std::string(
                "Shader shutdown-in-flight runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

bool RuntimeTestHooks::
FinalizeShaderShutdownInflightTestAfterWorkerShutdown(
    EngineLoop& engineLoop,
    const ShaderCompileWorkerShutdownDiagnostics& workerDiagnostics,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderShutdownInflightTestActive)
    {
        return runtimeTestStatus != RuntimeTestStatus::Failed;
    }

    try
    {
        if (shaderShutdownInflightTestPhase !=
            ShaderShutdownInflightTestPhase::AwaitShutdown)
        {
            throw std::runtime_error(
                "shutdown began before the complete candidate reached its gate");
        }
        if (!workerDiagnostics.joined ||
            !workerDiagnostics.discardedCompletedCandidate ||
            workerDiagnostics.postCompileGateGeneration !=
                shaderShutdownInflightCandidateGeneration ||
            workerDiagnostics.discardedGeneration !=
                shaderShutdownInflightCandidateGeneration ||
            workerDiagnostics.completedResultPublishedGeneration != 0)
        {
            throw std::runtime_error(
                "worker shutdown diagnostics did not prove complete-candidate discard");
        }
        if (engineLoop.latestAutoReloadCommittedGeneration !=
            shaderShutdownInflightBaselineCommittedGeneration)
        {
            throw std::runtime_error(
                "shutdown candidate advanced the committed reload generation");
        }
        const std::string currentFingerprint =
            CaptureShaderShutdownInflightFingerprint(
                engineLoop);
        if (currentFingerprint !=
            shaderShutdownInflightBaselineFingerprint)
        {
            throw std::runtime_error(
                "pre-teardown live/formal artifact fingerprint changed");
        }
        if (ReadTextFileBytes(
                shaderShutdownInflightSourcePath) !=
            shaderShutdownInflightOriginalSource)
        {
            throw std::runtime_error(
                "shutdown-in-flight source fixture was not restored");
        }

        shaderShutdownInflightTestActive = false;
        shaderShutdownInflightTestPhase =
            ShaderShutdownInflightTestPhase::Idle;
        runtimeTestStatus = RuntimeTestStatus::Succeeded;
        engineLoop.exitCode = 0;
        diagnostics.ReportInfo(
            "Shader compile worker joined: generation=" +
            std::to_string(
                shaderShutdownInflightCandidateGeneration) +
            ", discardedCompletedCandidate=true, completedResultPublished=false.");
        diagnostics.ReportInfo(
            "Shader shutdown-in-flight pre-teardown fingerprint unchanged; runtime test passed.");
        return true;
    }
    catch (const std::exception& exception)
    {
        shaderShutdownInflightTestActive = false;
        shaderShutdownInflightTestPhase =
            ShaderShutdownInflightTestPhase::Idle;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        engineLoop.exitCode = 2;
        diagnostics.ReportError(
            std::string(
                "Shader shutdown-in-flight finalization failed: ") +
            exception.what());
        return false;
    }
}

void RuntimeTestHooks::UpdateResizeStress(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!resizeStressActive || resizeStressRemaining <= 0)
    {
        return;
    }

    if (engineLoop.window == nullptr)
    {
        resizeStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError("Resize stress failed because EngineLoop has no active window.");
        return;
    }

    const Eigen::Vector2f configuredWindowSize = engineLoop.GetRuntimeConfig().GetWindowSize();
    const int baseWidth = static_cast<int>(configuredWindowSize.x());
    const int baseHeight = static_cast<int>(configuredWindowSize.y());
    const bool useSmallerSize = (resizeStressCompletedCount % 2) == 0;
    const int targetWidth = useSmallerSize ? std::max(320, baseWidth - 160) : baseWidth;
    const int targetHeight = useSmallerSize ? std::max(240, baseHeight - 90) : baseHeight;

    engineLoop.window->SetSize(targetWidth, targetHeight);
    suppressNextResizeEvent = true;
    suppressedResizeWidth = static_cast<uint32_t>(targetWidth);
    suppressedResizeHeight = static_cast<uint32_t>(targetHeight);

    auto resizeResult = engineLoop.RecreateRendererForWindowResize(
        static_cast<uint32_t>(targetWidth),
        static_cast<uint32_t>(targetHeight));
    if (resizeResult.IsFailure())
    {
        resizeStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportRuntimeError("Resize stress failed", resizeResult.Error());
        return;
    }

    ++resizeStressCompletedCount;
    --resizeStressRemaining;
    diagnostics.ReportInfo(
        "Resize stress step completed: " +
        std::to_string(resizeStressCompletedCount) +
        "/" +
        std::to_string(resizeStressTotal) +
        " size=" +
        std::to_string(targetWidth) +
        "x" +
        std::to_string(targetHeight));

    if (resizeStressRemaining <= 0)
    {
        resizeStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Succeeded;
        diagnostics.ReportInfo(
            "Resize stress completed: " +
            std::to_string(resizeStressCompletedCount) +
            "/" +
            std::to_string(resizeStressTotal) +
            " resize transactions succeeded.");
    }
}

void RuntimeTestHooks::UpdateRenderGraphReloadStress(
    EngineLoop& engineLoop,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!graphReloadStressActive)
    {
        return;
    }

    ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
    if (graphReloadStressWaitingForDrain)
    {
        const size_t pendingRetiredResources = retireQueue.GetPendingCount();
        UpdateMaxPendingRetiredResources(
            pendingRetiredResources,
            graphReloadMaxPendingRetiredResources);

        if (pendingRetiredResources == 0)
        {
            graphReloadStressActive = false;
            graphReloadStressWaitingForDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            diagnostics.ReportInfo(
                "Render graph reload stress completed: " +
                std::to_string(graphReloadStressCompletedCount) +
                "/" +
                std::to_string(graphReloadStressTotal) +
                " reloads succeeded, retire queue max pending=" +
                std::to_string(graphReloadMaxPendingRetiredResources) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
            return;
        }

        --graphReloadRetireDrainFramesRemaining;
        if (graphReloadRetireDrainFramesRemaining <= 0)
        {
            graphReloadStressActive = false;
            graphReloadStressWaitingForDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "Render graph reload stress failed because retired graph resources did not drain before the frame budget expired. pending=" +
                std::to_string(pendingRetiredResources) +
                ", submittedEpoch=" +
                std::to_string(retireQueue.GetLastSubmittedEpoch()) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
        }
        return;
    }

    if (graphReloadStressRemaining <= 0)
    {
        const size_t pendingRetiredResources = retireQueue.GetPendingCount();
        UpdateMaxPendingRetiredResources(
            pendingRetiredResources,
            graphReloadMaxPendingRetiredResources);

        if (graphReloadStressTotal > 1 && graphReloadMaxPendingRetiredResources == 0)
        {
            graphReloadStressActive = false;
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "Render graph reload stress failed because no retired graph resources were observed after repeated reloads.");
            return;
        }

        graphReloadStressWaitingForDrain = true;
        graphReloadRetireDrainFramesRemaining = RetireDrainFrameBudget;
        diagnostics.ReportInfo(
            "Render graph reload stress waiting for retire queue drain: pending=" +
            std::to_string(pendingRetiredResources) +
            ", maxPending=" +
            std::to_string(graphReloadMaxPendingRetiredResources) +
            ".");
        return;
    }

    auto reloadResult = engineLoop.ReloadRenderGraphResources();
    if (reloadResult.IsFailure())
    {
        graphReloadStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportRuntimeError(
            "Render graph reload stress failed",
            reloadResult.Error());
        return;
    }

    --graphReloadStressRemaining;
    ++graphReloadStressCompletedCount;

    const size_t pendingRetiredResources = retireQueue.GetPendingCount();
    UpdateMaxPendingRetiredResources(
        pendingRetiredResources,
        graphReloadMaxPendingRetiredResources);
    diagnostics.ReportInfo(
        "Render graph reload stress reloaded graph " +
        std::to_string(graphReloadStressCompletedCount) +
        "/" +
        std::to_string(graphReloadStressTotal) +
        ", pending retired resources=" +
        std::to_string(pendingRetiredResources) +
        ".");
}

void RuntimeTestHooks::RecordFrameRenderLoopTime(double renderLoopTimeMs)
{
    if (!frameSmokeActive)
    {
        return;
    }

    frameSmokeIntervalRenderLoopTotalMs += renderLoopTimeMs;
    frameSmokeIntervalRenderLoopMaxMs =
        std::max(frameSmokeIntervalRenderLoopMaxMs, renderLoopTimeMs);
}

void RuntimeTestHooks::RecordFrameTime(
    double frameTimeMs,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!frameSmokeActive)
    {
        return;
    }

    if (frameTimeMs <= 0.0 || !std::isfinite(frameTimeMs))
    {
        frameSmokeActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            "Frame smoke test failed because a measured frame time was invalid.");
        return;
    }

    ++frameSmokeCompletedCount;
    frameSmokeTotalMs += frameTimeMs;
    frameSmokeMaxMs = std::max(frameSmokeMaxMs, frameTimeMs);
    frameSmokeMinMs = std::min(frameSmokeMinMs, frameTimeMs);
    ++frameSmokeIntervalFrameCount;
    frameSmokeIntervalTotalMs += frameTimeMs;
    frameSmokeIntervalMaxMs = std::max(frameSmokeIntervalMaxMs, frameTimeMs);
    frameSmokeIntervalMinMs = std::min(frameSmokeIntervalMinMs, frameTimeMs);

    if (frameSmokeIntervalFrameCount >= frameSmokeIntervalSize)
    {
        ReportFrameSmokeInterval(diagnostics);
    }

    if (frameSmokeCompletedCount < frameSmokeTotal)
    {
        return;
    }

    if (frameSmokeIntervalFrameCount > 0)
    {
        ReportFrameSmokeInterval(diagnostics);
    }

    frameSmokeActive = false;
    runtimeTestStatus = RuntimeTestStatus::Succeeded;
    const double averageFrameMs = frameSmokeTotalMs /
        static_cast<double>(std::max(1, frameSmokeCompletedCount));
    const double averageFps = 1000.0 / averageFrameMs;

    diagnostics.ReportInfo(
        "Frame smoke test completed: " +
        std::to_string(frameSmokeCompletedCount) +
        "/" +
        std::to_string(frameSmokeTotal) +
        " frames, avgFrameMs=" +
        std::to_string(averageFrameMs) +
        ", minFrameMs=" +
        std::to_string(frameSmokeMinMs) +
        ", maxFrameMs=" +
        std::to_string(frameSmokeMaxMs) +
        ", avgFps=" +
        std::to_string(averageFps));
}

void RuntimeTestHooks::ReportFrameSmokeInterval(
    const DiagnosticsSubsystem& diagnostics)
{
    const double averageFrameMs = frameSmokeIntervalTotalMs /
        static_cast<double>(std::max(1, frameSmokeIntervalFrameCount));
    const double averageFps = 1000.0 / averageFrameMs;
    const double averageRenderLoopMs = frameSmokeIntervalRenderLoopTotalMs /
        static_cast<double>(std::max(1, frameSmokeIntervalFrameCount));
    const ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();

    diagnostics.ReportInfo(
        "Frame smoke interval: frame=" +
        std::to_string(frameSmokeCompletedCount) +
        "/" +
        std::to_string(frameSmokeTotal) +
        ", intervalFrames=" +
        std::to_string(frameSmokeIntervalFrameCount) +
        ", avgFrameMs=" +
        std::to_string(averageFrameMs) +
        ", minFrameMs=" +
        std::to_string(frameSmokeIntervalMinMs) +
        ", maxFrameMs=" +
        std::to_string(frameSmokeIntervalMaxMs) +
        ", avgFps=" +
        std::to_string(averageFps) +
        ", avgRenderLoopMs=" +
        std::to_string(averageRenderLoopMs) +
        ", maxRenderLoopMs=" +
        std::to_string(frameSmokeIntervalRenderLoopMaxMs) +
        ", retiredPending=" +
        std::to_string(retireQueue.GetPendingCount()) +
        ", submittedEpoch=" +
        std::to_string(retireQueue.GetLastSubmittedEpoch()) +
        ", completedEpoch=" +
        std::to_string(retireQueue.GetLastCompletedEpoch()));

    frameSmokeIntervalFrameCount = 0;
    frameSmokeIntervalTotalMs = 0.0;
    frameSmokeIntervalMaxMs = 0.0;
    frameSmokeIntervalMinMs = std::numeric_limits<double>::max();
    frameSmokeIntervalRenderLoopTotalMs = 0.0;
    frameSmokeIntervalRenderLoopMaxMs = 0.0;
}

bool RuntimeTestHooks::ShouldSuppressResizeEvent(uint32_t width, uint32_t height)
{
    if (!suppressNextResizeEvent)
    {
        return false;
    }

    if (width == suppressedResizeWidth && height == suppressedResizeHeight)
    {
        suppressNextResizeEvent = false;
        return true;
    }

    return false;
}

void RuntimeTestHooks::UpdateEnvironmentUpdateStress(
    CommandBus& commandBus,
    const WorldManager& worldManager,
    const EnvironmentUpdateDiagnostics& environmentDiagnostics,
    const DiagnosticsSubsystem& diagnostics)
{
    --environmentUpdateStressFrameBudget;
    if (environmentUpdateStressFrameBudget <= 0)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress exceeded its frame budget.",
            diagnostics);
        return;
    }

    const std::shared_ptr<World>& activeWorld = worldManager.GetActiveWorld();
    if (!activeWorld)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress requires an active World.",
            diagnostics);
        return;
    }

    const WorldEnvironment& worldEnvironment = activeWorld->GetEnvironment();
    if (worldEnvironment.type != EnvironmentType::ProceduralSky)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress requires a procedural-sky World.",
            diagnostics);
        return;
    }

    const EnvironmentUpdateProgress& progress = environmentDiagnostics.progress;
    const EnvironmentGpuTimingSnapshot& timing = environmentDiagnostics.gpuTiming;

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::WaitInitialGeneration)
    {
        const bool initialTimingsReady =
            timing.supported &&
            timing.Get(EnvironmentGpuProduct::Cubemap).sampleCount > 0 &&
            timing.Get(EnvironmentGpuProduct::SphericalHarmonics).sampleCount > 0 &&
            timing.Get(EnvironmentGpuProduct::Prefilter).sampleCount > 0 &&
            timing.Get(EnvironmentGpuProduct::Commit).sampleCount > 0;
        if (progress.stage != EnvironmentUpdateStage::Idle ||
            progress.activeGeneration == 0 ||
            !initialTimingsReady)
        {
            return;
        }

        environmentUpdateStressOriginalSkyParameters = worldEnvironment.skyParameters;
        environmentUpdateStressPreviousActiveGeneration = progress.activeGeneration;
        environmentUpdateStressPrefilterMipCount = progress.prefilterMipCount;
        environmentUpdateStressEnvironmentCubeIdentity =
            GetWorldTextureIdentity("environmentCube");
        environmentUpdateStressPrefilterCubeIdentity =
            GetWorldTextureIdentity("prefilteredEnvironmentCube");
        if (environmentUpdateStressEnvironmentCubeIdentity == 0 ||
            environmentUpdateStressPrefilterCubeIdentity == 0 ||
            environmentUpdateStressPrefilterMipCount == 0)
        {
            FailEnvironmentUpdateStress(
                "Environment update stress could not capture active environment resources.",
                diagnostics);
            return;
        }

        environmentUpdateStressBaselineTimingSamples[0] =
            timing.Get(EnvironmentGpuProduct::Cubemap).sampleCount;
        environmentUpdateStressBaselineTimingSamples[1] =
            timing.Get(EnvironmentGpuProduct::SphericalHarmonics).sampleCount;
        environmentUpdateStressBaselineTimingSamples[2] =
            timing.Get(EnvironmentGpuProduct::Prefilter).sampleCount;
        environmentUpdateStressBaselineTimingSamples[3] =
            timing.Get(EnvironmentGpuProduct::Commit).sampleCount;
        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::RequestMutation;
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::RequestMutation)
    {
        SkyParametersGPU changedSkyParameters = environmentUpdateStressOriginalSkyParameters;
        const int mutationIndex = environmentUpdateStressCompletedCount + 1;
        const float direction = (mutationIndex % 2) == 1 ? 1.0f : -1.0f;
        Eigen::Vector3f changedSunDirection =
            changedSkyParameters.sunDirectionIntensity.head<3>();
        changedSunDirection.x() += direction * 0.02f;
        changedSunDirection.normalize();
        changedSkyParameters.sunDirectionIntensity.head<3>() = changedSunDirection;

        // 测试子系统只投递意图；active World 的可变状态仍由命令执行器在 owner 侧修改。
        RuntimeCommand command;
        command.type = RuntimeCommandType::SetProceduralSkyParameters;
        command.skyParametersValue = changedSkyParameters;
        command.sourceText = "runtime-test: environmentstress mutation";
        commandBus.Queue(std::move(command));

        environmentUpdateStressPreviousActiveGeneration = progress.activeGeneration;
        environmentUpdateStressObservedPreviousResources = false;
        waitingForProceduralSkyParametersResult = true;
        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::WaitMutation;
        diagnostics.ReportInfo(
            "Environment update stress requested dirty generation " +
            std::to_string(mutationIndex) +
            "/" +
            std::to_string(environmentUpdateStressTotal) +
            ".");
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::WaitMutation)
    {
        if (waitingForProceduralSkyParametersResult)
        {
            return;
        }
        if (progress.usingPreviousResources &&
            progress.activeGeneration == environmentUpdateStressPreviousActiveGeneration)
        {
            environmentUpdateStressObservedPreviousResources = true;
        }

        if (progress.stage != EnvironmentUpdateStage::Idle ||
            progress.activeGeneration <= environmentUpdateStressPreviousActiveGeneration)
        {
            return;
        }
        if (!environmentUpdateStressObservedPreviousResources)
        {
            FailEnvironmentUpdateStress(
                "Environment update committed without exposing the previous-resource interval.",
                diagnostics);
            return;
        }
        if (GetWorldTextureIdentity("environmentCube") !=
                environmentUpdateStressEnvironmentCubeIdentity ||
            GetWorldTextureIdentity("prefilteredEnvironmentCube") !=
                environmentUpdateStressPrefilterCubeIdentity)
        {
            FailEnvironmentUpdateStress(
                "Environment active texture identity changed during an incremental generation.",
                diagnostics);
            return;
        }

        ++environmentUpdateStressCompletedCount;
        environmentUpdateStressPreviousActiveGeneration = progress.activeGeneration;
        diagnostics.ReportInfo(
            "Environment update stress committed generation " +
            std::to_string(environmentUpdateStressCompletedCount) +
            "/" +
            std::to_string(environmentUpdateStressTotal) +
            ".");
        environmentUpdateStressPhase =
            environmentUpdateStressCompletedCount < environmentUpdateStressTotal
            ? EnvironmentUpdateStressPhase::RequestMutation
            : EnvironmentUpdateStressPhase::RequestRestore;
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::RequestRestore)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::SetProceduralSkyParameters;
        command.skyParametersValue = environmentUpdateStressOriginalSkyParameters;
        command.sourceText = "runtime-test: environmentstress restore";
        commandBus.Queue(std::move(command));

        environmentUpdateStressPreviousActiveGeneration = progress.activeGeneration;
        environmentUpdateStressObservedPreviousResources = false;
        waitingForProceduralSkyParametersResult = true;
        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::WaitRestore;
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::WaitRestore)
    {
        if (waitingForProceduralSkyParametersResult)
        {
            return;
        }
        if (progress.usingPreviousResources &&
            progress.activeGeneration == environmentUpdateStressPreviousActiveGeneration)
        {
            environmentUpdateStressObservedPreviousResources = true;
        }
        if (progress.stage != EnvironmentUpdateStage::Idle ||
            progress.activeGeneration <= environmentUpdateStressPreviousActiveGeneration)
        {
            return;
        }
        if (!environmentUpdateStressObservedPreviousResources)
        {
            FailEnvironmentUpdateStress(
                "Environment restore generation did not preserve the previous active resources.",
                diagnostics);
            return;
        }

        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::WaitTimingDrain;
        return;
    }

    if (environmentUpdateStressPhase == EnvironmentUpdateStressPhase::WaitTimingDrain)
    {
        const uint64_t generationCount =
            static_cast<uint64_t>(environmentUpdateStressTotal + 1);
        const uint64_t expectedCubemapSamples =
            environmentUpdateStressBaselineTimingSamples[0] + generationCount * 6;
        const uint64_t expectedShSamples =
            environmentUpdateStressBaselineTimingSamples[1] + generationCount;
        const uint64_t expectedPrefilterSamples =
            environmentUpdateStressBaselineTimingSamples[2] +
            generationCount * environmentUpdateStressPrefilterMipCount;
        const uint64_t expectedCommitSamples =
            environmentUpdateStressBaselineTimingSamples[3] + generationCount;

        if (timing.Get(EnvironmentGpuProduct::Cubemap).sampleCount < expectedCubemapSamples ||
            timing.Get(EnvironmentGpuProduct::SphericalHarmonics).sampleCount < expectedShSamples ||
            timing.Get(EnvironmentGpuProduct::Prefilter).sampleCount < expectedPrefilterSamples ||
            timing.Get(EnvironmentGpuProduct::Commit).sampleCount < expectedCommitSamples)
        {
            return;
        }

        environmentUpdateStressActive = false;
        environmentUpdateStressPhase = EnvironmentUpdateStressPhase::Idle;
        runtimeTestStatus = RuntimeTestStatus::Succeeded;
        diagnostics.ReportInfo(
            "Environment update stress completed: changes=" +
            std::to_string(environmentUpdateStressCompletedCount) +
            ", cubeLastMs=" +
            std::to_string(timing.Get(EnvironmentGpuProduct::Cubemap).lastMilliseconds) +
            ", shLastMs=" +
            std::to_string(timing.Get(EnvironmentGpuProduct::SphericalHarmonics).lastMilliseconds) +
            ", prefilterLastMs=" +
            std::to_string(timing.Get(EnvironmentGpuProduct::Prefilter).lastMilliseconds) +
            ", commitLastMs=" +
            std::to_string(timing.Get(EnvironmentGpuProduct::Commit).lastMilliseconds) +
            ".");
    }
}

void RuntimeTestHooks::FailEnvironmentUpdateStress(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    environmentUpdateStressActive = false;
    waitingForProceduralSkyParametersResult = false;
    environmentUpdateStressPhase = EnvironmentUpdateStressPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::NotifyProceduralSkyParametersResult(
    bool succeeded,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!environmentUpdateStressActive)
    {
        return;
    }

    if (!waitingForProceduralSkyParametersResult)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress received an unexpected sky-parameter command result.",
            diagnostics);
        return;
    }

    waitingForProceduralSkyParametersResult = false;
    if (!succeeded)
    {
        FailEnvironmentUpdateStress(
            "Environment update stress could not apply procedural sky parameters through CommandBus.",
            diagnostics);
    }
}

void RuntimeTestHooks::NotifyCommandResult(
    const RuntimeCommandExecutionResult& commandResult,
    const DiagnosticsSubsystem& diagnostics)
{
    if (shaderReloadTestActive &&
        waitingForShaderReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }

        waitingForShaderReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderReloadTest(
                "Shader reload runtime test failed to load or bind "
                "its fixture World.",
                diagnostics);
            return;
        }

        shaderReloadTestPhase =
            ShaderReloadTestPhase::CompatibleCommit;
        diagnostics.ReportInfo(
            "Shader reload runtime test fixture World is active.");
        return;
    }

    if (shaderAutoReloadTestActive &&
        waitingForShaderAutoReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }

        waitingForShaderAutoReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderAutoReloadTest(
                "Shader auto reload runtime test failed to load or bind "
                "its fixture World.",
                diagnostics);
            return;
        }

        shaderAutoReloadTestPhase =
            ShaderAutoReloadTestPhase::WaitA1Gate;
        shaderAutoReloadTestPhaseEntryPending = true;
        diagnostics.ReportInfo(
            "Shader auto reload runtime test fixture World is active.");
        return;
    }

    if (shaderComputeReloadTestActive &&
        waitingForShaderComputeReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }
        waitingForShaderComputeReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderComputeReloadTest(
                "Shader compute reload runtime test failed to load or bind "
                "its fixture World.",
                diagnostics);
            return;
        }
        shaderComputeReloadTestPhase =
            ShaderComputeReloadTestPhase::WaitCompatibleCommit;
        shaderComputeReloadTestPhaseEntryPending = true;
        diagnostics.ReportInfo(
            "Shader compute reload runtime test fixture World is active.");
        return;
    }

    if (shaderDefinitionReloadTestActive &&
        waitingForShaderDefinitionReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }
        waitingForShaderDefinitionReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderDefinitionReloadTest(
                "Shader definition reload runtime test failed to load or bind "
                "its fixture World.",
                diagnostics);
            return;
        }
        shaderDefinitionReloadTestPhase =
            ShaderDefinitionReloadTestPhase::WaitAddedParameter;
        shaderDefinitionReloadTestPhaseEntryPending = true;
        diagnostics.ReportInfo(
            "Shader definition reload runtime test fixture World is active.");
        return;
    }

    if (worldGraphTransactionTestActive &&
        waitingForWorldGraphTransactionTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }
        waitingForWorldGraphTransactionTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailWorldGraphTransactionTest(
                "World/graph transaction runtime test failed to load or "
                "bind its fixture World.",
                diagnostics);
            return;
        }
        worldGraphTransactionTestPhase =
            WorldGraphTransactionTestPhase::GraphResourceFailure;
        diagnostics.ReportInfo(
            "World/graph transaction runtime test fixture World is active.");
        return;
    }

    if (shaderUiReloadTestActive &&
        waitingForShaderUiReloadTestWorld)
    {
        if (!commandResult.loadWorldAttempted)
        {
            return;
        }
        waitingForShaderUiReloadTestWorld = false;
        if (!commandResult.loadWorldSucceeded ||
            !commandResult.worldRuntimeBindingSucceeded)
        {
            FailShaderUiReloadTest(
                "Shader UI overlay reload runtime test failed to load or "
                "bind its fixture World.",
                diagnostics);
            return;
        }
        shaderUiReloadTestPhase =
            ShaderUiReloadTestPhase::WaitCompatibleCommit;
        shaderUiReloadTestPhaseEntryPending = true;
        diagnostics.ReportInfo(
            "Shader UI overlay reload runtime test fixture World is active.");
        return;
    }

    if (!worldReloadStressActive || !waitingForWorldReloadResult)
    {
        if (!failureRollbackTestActive || !waitingForFailureRollbackResult)
        {
            return;
        }

        if (!commandResult.loadWorldAttempted)
        {
            return;
        }

        waitingForFailureRollbackResult = false;
        failureRollbackTestActive = false;
        const bool shouldCleanupGeneratedFixture = cleanupGeneratedFailureFixture;
        cleanupGeneratedFailureFixture = false;

        if (commandResult.loadWorldSucceeded)
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test expected load to fail, but it succeeded.");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        if (!failureRollbackExpectedErrorCode.empty())
        {
            if (!commandResult.loadWorldError.has_value())
            {
                runtimeTestStatus = RuntimeTestStatus::Failed;
                diagnostics.ReportError(
                    "World reload failure rollback test failed because the expected load failure did not expose a RuntimeError.");
                failureRollbackExpectedErrorCode.clear();
                if (shouldCleanupGeneratedFixture)
                {
                    CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                    generatedFailureFixtureDirectory.clear();
                }
                return;
            }

            if (commandResult.loadWorldError->code != failureRollbackExpectedErrorCode)
            {
                runtimeTestStatus = RuntimeTestStatus::Failed;
                diagnostics.ReportError(
                    "World reload failure rollback test failed because the load error code was " +
                    commandResult.loadWorldError->code +
                    ", expected " +
                    failureRollbackExpectedErrorCode +
                    ".");
                failureRollbackExpectedErrorCode.clear();
                if (shouldCleanupGeneratedFixture)
                {
                    CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                    generatedFailureFixtureDirectory.clear();
                }
                return;
            }
        }

        if (commandResult.worldChanged || commandResult.worldRuntimeBindingAttempted)
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test failed because a failed load attempted to rebind runtime world state.");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        if (!SameWorldHandle(
                commandResult.activeWorldBeforeCommand,
                commandResult.activeWorldAfterCommand))
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test failed because active world handle changed after the expected load failure.");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        if (!SameRendererResourceFingerprint(
                commandResult.rendererResourcesBeforeLoad,
                commandResult.rendererResourcesAfterLoad))
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test failed because renderer resource cache or pass material bindings changed after the expected load failure. before={" +
                FormatRendererResourceFingerprint(commandResult.rendererResourcesBeforeLoad) +
                "} after={" +
                FormatRendererResourceFingerprint(commandResult.rendererResourcesAfterLoad) +
                "}");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        runtimeTestStatus = RuntimeTestStatus::Succeeded;
        diagnostics.ReportInfo(
            "World reload failure rollback test completed: active world, renderer resource cache, and pass material bindings preserved after expected load failure.");
        failureRollbackExpectedErrorCode.clear();
        if (shouldCleanupGeneratedFixture)
        {
            CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
            generatedFailureFixtureDirectory.clear();
        }
        return;
    }

    if (!commandResult.loadWorldAttempted)
    {
        return;
    }

    waitingForWorldReloadResult = false;

    if (!commandResult.loadWorldSucceeded)
    {
        worldReloadStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            "World reload stress aborted after " +
            std::to_string(completedWorldReloads) +
            "/" +
            std::to_string(totalWorldReloads) +
            " successful reloads.");
        CleanupGeneratedRuntimeFixtureIfNeeded(
            cleanupGeneratedReloadStressFixture,
            generatedReloadStressFixtureDirectory,
            diagnostics);
        return;
    }

    if (!commandResult.worldRuntimeBindingSucceeded)
    {
        worldReloadStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            "World reload stress aborted because runtime rebinding failed after load " +
            std::to_string(completedWorldReloads + 1) +
            "/" +
            std::to_string(totalWorldReloads) +
            ".");
        CleanupGeneratedRuntimeFixtureIfNeeded(
            cleanupGeneratedReloadStressFixture,
            generatedReloadStressFixtureDirectory,
            diagnostics);
        return;
    }

    const size_t pendingRetiredResources = ResourceRetireQueue::GetInstance().GetPendingCount();
    UpdateMaxPendingRetiredResources(pendingRetiredResources, maxPendingRetiredResources);

    ++completedWorldReloads;
    if (remainingWorldReloads <= 0)
    {
        waitingForRetireDrain = true;
        retireDrainFramesRemaining = RetireDrainFrameBudget;
        diagnostics.ReportInfo(
            "World reload stress waiting for retire queue drain: pending=" +
            std::to_string(pendingRetiredResources) +
            ", maxPending=" +
            std::to_string(maxPendingRetiredResources) +
            ".");
    }
}

} // namespace VL
