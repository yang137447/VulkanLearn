#include "engine/testing/runtimeValidationServices.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "commonFunction.h"
#include "controller.h"
#include "engine/engineLoop.h"
#include "engine/runtimeCommand.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/runtimeConfig.h"
#include "engine/subsystemCollection.h"
#include "material.h"
#include "materialInstance.h"
#include "pipeline/pipelineFactory.h"
#include "pipeline/pipelineBase.h"
#include "platform/platformWindow.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "render/hair/hairResourceSet.h"
#include "render/eye/eyeResourceSet.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"
#include "renderSystem.h"
#include "world/world.h"
#include "shader/build/contentHash.h"
#include "shaderCompiler.h"

namespace VL
{
namespace
{

constexpr const char* ShaderReloadTestShaderName =
    "runtimeTest/shaderReloadTest";

std::string HashFileIfPresent(
    const std::filesystem::path& path)
{
    return std::filesystem::is_regular_file(path)
        ? ContentHasher::HashFile(path).ToHex()
        : std::string();
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

    for (const auto& instanceEntry : resources->materialInstances)
    {
        const std::shared_ptr<MaterialInstance>& instance =
            instanceEntry.second;
        const std::shared_ptr<Material> material =
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

    for (const auto& instanceEntry : resources->materialInstances)
    {
        const std::shared_ptr<MaterialInstance>& instance =
            instanceEntry.second;
        if (instance &&
            instance->HasParameter("u_reloadBatchColor"))
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
        resources->objectResources.find("ShaderReloadAxis_001");
    return resourceIt != resources->objectResources.end()
        ? resourceIt->second
        : nullptr;
}

std::shared_ptr<Texture> FindShaderReloadTestPrimaryTexture()
{
    const std::shared_ptr<MaterialInstance> instance =
        FindShaderReloadTestMaterialInstance();
    return instance
        ? instance->GetTexture("u_reloadTexture")
        : nullptr;
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
    for (const auto& [name, value] : snapshot.parameters)
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

    for (const auto& [name, binding] : snapshot.textures)
    {
        stream << "mi.texture." << name << ".pointer=" <<
            reinterpret_cast<std::uintptr_t>(
                binding.texture.get()) << ";";
        stream << "mi.texture." << name << ".asset=" <<
            binding.textureAssetIdentity.value_or("<none>") << ";";
        stream << "mi.texture." << name << ".cache=" <<
            binding.textureCacheIdentity.value_or("<none>") << ";";
    }
}

std::string CaptureRenderGraphGpuFingerprint(
    const RenderGraph& renderGraph)
{
    std::ostringstream stream;
    stream << "owner=" <<
        renderGraph.GetOwnerGeneration() << ";";

    std::vector<std::string> resourceNames;
    for (const auto& entry : renderGraph.GetResourcesMsaa())
    {
        resourceNames.push_back("msaa:" + entry.first);
    }
    for (const auto& entry : renderGraph.GetResourcesResolve())
    {
        resourceNames.push_back("resolve:" + entry.first);
    }
    std::sort(resourceNames.begin(), resourceNames.end());
    for (const std::string& name : resourceNames)
    {
        const bool isMsaa = name.rfind("msaa:", 0) == 0;
        const std::string resourceName =
            name.substr(isMsaa ? 5 : 8);
        const auto& resourceMap =
            isMsaa
                ? renderGraph.GetResourcesMsaa()
                : renderGraph.GetResourcesResolve();
        const std::vector<RenderResource>& resources =
            resourceMap.at(resourceName);
        for (size_t index = 0; index < resources.size(); ++index)
        {
            const RenderResource& resource = resources[index];
            stream << name << "." << index <<
                ".image=" << resource.imageHandle.id <<
                ".view=" << resource.imageViewHandle.id <<
                ".sampler=" << resource.samplerHandle.id << ";";
            for (const RHIImageViewHandle& layerView :
                 resource.imageViewHandles)
            {
                stream << "layer=" << layerView.id << ";";
            }
        }
    }

    for (const std::string& passName :
         renderGraph.GetRenderpassesOrdered())
    {
        const Renderpass& renderpass =
            renderGraph.GetRenderpasses().at(passName);
        stream << "pass=" << passName <<
            ".renderPass=" << renderpass.renderPassHandle.id <<
            ".descriptorPool=" <<
                renderpass.descriptorPoolHandle.id <<
            ".descriptorLayout=" <<
                renderpass.descriptorSetLayoutHandle.id <<
            ".emptyLayout=" <<
                renderpass.emptyDescriptorSetLayoutHandle.id << ";";
        for (const RHIFramebufferHandle& framebuffer :
             renderpass.framebufferHandles)
        {
            stream << "framebuffer=" << framebuffer.id << ";";
        }
    }

    return ContentHasher::HashString(stream.str()).ToHex();
}

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

bool ManifestArtifactDependsOnAllSources(
    const ShaderBuildManifestSnapshot& manifest,
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
        for (const ShaderDependencyRecord& dependency :
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

std::string FormatHairParameterValue(
    const MaterialInstanceParameterValue& value)
{
    return std::visit(
        [](const auto& currentValue)
        {
            using ValueType = std::decay_t<decltype(currentValue)>;
            std::ostringstream stream;
            if constexpr (std::is_same_v<ValueType, float>)
            {
                stream << currentValue;
            }
            else
            {
                stream << currentValue.x() << "," << currentValue.y();
                if constexpr (ValueType::RowsAtCompileTime >= 3)
                {
                    stream << "," << currentValue.z();
                }
                if constexpr (ValueType::RowsAtCompileTime >= 4)
                {
                    stream << "," << currentValue.w();
                }
            }
            return stream.str();
        },
        value);
}

bool HasHairLutBinding(
    const Renderpass& renderpass,
    uint32_t expectedBinding)
{
    for (const CompiledRenderGraphPassInputDescriptor& descriptor :
         renderpass.inputDescriptorPlan)
    {
        if (descriptor.resource == "hairAzimuthalLut" &&
            descriptor.source == "worldTexture" &&
            descriptor.binding == expectedBinding)
        {
            return true;
        }
    }
    return false;
}

bool HasEyeLutBinding(
    const Renderpass& renderpass,
    uint32_t expectedBinding)
{
    for (const CompiledRenderGraphPassInputDescriptor& descriptor :
         renderpass.inputDescriptorPlan)
    {
        if (descriptor.resource == "eyeCausticLut" &&
            descriptor.source == "worldTexture" &&
            descriptor.binding == expectedBinding)
        {
            return true;
        }
    }
    return false;
}

bool HasCompatiblePassDescriptorLayout(
    const PipelineBase& pipeline,
    const Renderpass& renderpass)
{
    const std::vector<ShaderBinding>& layoutBindings =
        pipeline.GetDescriptorLayoutBindings();
    size_t passBindingCount = 0;
    for (const ShaderBinding& binding : layoutBindings)
    {
        if (binding.set != PassSetIndex)
        {
            continue;
        }
        ++passBindingCount;
        if (binding.type != vk::DescriptorType::eCombinedImageSampler ||
            binding.descriptorCount != 1 ||
            binding.stageFlags != vk::ShaderStageFlagBits::eFragment)
        {
            return false;
        }

        bool foundInput = false;
        for (const CompiledRenderGraphPassInputDescriptor& inputDescriptor :
             renderpass.inputDescriptorPlan)
        {
            if (inputDescriptor.binding == binding.binding)
            {
                foundInput = true;
                break;
            }
        }
        if (!foundInput)
        {
            return false;
        }
    }

    return passBindingCount == renderpass.inputDescriptorPlan.size();
}

} // namespace

RuntimeValidationServices::RuntimeValidationServices(
    EngineLoop& engineLoop) noexcept
    : engineLoop(&engineLoop)
{
}

std::string RuntimeValidationServices::GetResourcePath() const
{
    return engineLoop->GetRuntimeConfig().GetResourcePath();
}

std::array<uint32_t, 2>
RuntimeValidationServices::GetConfiguredWindowSize() const
{
    const Eigen::Vector2f size =
        engineLoop->GetRuntimeConfig().GetWindowSize();
    return {
        static_cast<uint32_t>(size.x()),
        static_cast<uint32_t>(size.y())};
}

int RuntimeValidationServices::GetDebugViewMode() const noexcept
{
    return RenderSystem::GetInstance().GetDebugViewMode();
}

void RuntimeValidationServices::QueueRuntimeCommand(
    RuntimeCommand command)
{
    engineLoop->QueueRuntimeCommand(std::move(command));
}

WorldHandle RuntimeValidationServices::GetActiveWorldHandle() const
{
    return engineLoop->GetSubsystems()
        .GetWorldManager()
        .GetActiveWorldHandle();
}

RuntimeValidationOwnerSnapshot
RuntimeValidationServices::CaptureOwnerSnapshot() const
{
    RuntimeValidationOwnerSnapshot snapshot;
    const WorldManager& worldManager =
        engineLoop->GetSubsystems().GetWorldManager();
    snapshot.world = worldManager.GetActiveWorldHandle();
    snapshot.worldIdentity =
        reinterpret_cast<std::uintptr_t>(
            worldManager.GetActiveWorld().get());
    snapshot.nextWorldGeneration =
        worldManager.GetNextWorldGeneration();
    snapshot.rendererResources =
        CaptureRuntimeRendererResourceFingerprint();
    snapshot.worldResourceGeneration =
        snapshot.rendererResources.worldOwnerGeneration;

    const RenderGraph& renderGraph = RenderGraph::GetInstance();
    snapshot.renderGraphGeneration =
        renderGraph.GetOwnerGeneration();
    snapshot.renderGraphGpuFingerprint =
        CaptureRenderGraphGpuFingerprint(renderGraph);

    const RenderSystem& renderSystem =
        RenderSystem::GetInstance();
    snapshot.renderSystemGeneration =
        renderSystem.GetActiveWorldGeneration();
    snapshot.controllerGeneration =
        engineLoop->controller
            ? engineLoop->controller
                  ->GetBoundWorldGeneration()
            : 0;
    snapshot.lightCapacity =
        renderSystem.GetLightCapacityForTest();
    const auto& lightHandles =
        renderSystem.GetLightBufferHandlesForTest();
    snapshot.lightBufferIds.reserve(lightHandles.size());
    for (const RHIBufferHandle& handle : lightHandles)
    {
        snapshot.lightBufferIds.push_back(handle.id);
    }
    return snapshot;
}

RuntimeValidationBackendSnapshot
RuntimeValidationServices::CaptureBackendSnapshot() const
{
    RuntimeValidationBackendSnapshot snapshot;
    if (!engineLoop->rendererBackend)
    {
        return snapshot;
    }

    snapshot.identityCounts =
        CaptureBackendIdentityCounts(*engineLoop->rendererBackend);
    snapshot.imageResourceNames =
        CaptureImageResourceDebugNames(*engineLoop->rendererBackend);
    const vk::Extent2D extent =
        engineLoop->rendererBackend->GetSwapchainExtent();
    snapshot.swapchainWidth = extent.width;
    snapshot.swapchainHeight = extent.height;
    return snapshot;
}

RuntimeValidationWorldPackageIdentities
RuntimeValidationServices::CaptureWorldPackageIdentities() const
{
    RuntimeValidationWorldPackageIdentities snapshot;
    const WorldManager& worldManager =
        engineLoop->GetSubsystems().GetWorldManager();
    snapshot.generation =
        worldManager.GetActiveWorldHandle().generation;
    snapshot.world = worldManager.GetActiveWorld();

    const auto resources =
        RendererResourceCache::GetInstance()
            .CaptureActiveWorldLocalResources();
    snapshot.worldResources =
        std::const_pointer_cast<
            RendererResourceCache::WorldLocalResourcePackage>(
                resources);

    const std::shared_ptr<Material> material =
        FindShaderReloadTestMaterial();
    const std::shared_ptr<MaterialInstance> materialInstance =
        FindShaderReloadTestMaterialInstance();
    const std::shared_ptr<RendererObjectResourceEntry> objectResources =
        FindShaderReloadTestObjectResources();
    const std::shared_ptr<Texture> primaryTexture =
        FindShaderReloadTestPrimaryTexture();
    snapshot.material = material;
    snapshot.materialInstance = materialInstance;
    snapshot.objectResources = objectResources;
    snapshot.primaryTexture = primaryTexture;
    snapshot.primaryTextureIdentity =
        reinterpret_cast<std::uintptr_t>(primaryTexture.get());
    snapshot.materialInstanceHasCandidateParameter =
        materialInstance &&
        materialInstance->HasParameter(
            "u_worldGraphTransactionCandidate");
    return snapshot;
}

RuntimeHairValidationSnapshot
RuntimeValidationServices::CaptureHairValidationSnapshot() const
{
    RuntimeHairValidationSnapshot snapshot;
    const WorldHandle& worldHandle =
        engineLoop->GetSubsystems()
            .GetWorldManager()
            .GetActiveWorldHandle();
    snapshot.worldGeneration = worldHandle.generation;

    const RendererResourceCache::ImmutableWorldLocalResourceRefs resources =
        RendererResourceCache::GetInstance()
            .CaptureActiveWorldLocalResources();
    if (!resources)
    {
        return snapshot;
    }

    const std::shared_ptr<World> activeWorld =
        engineLoop->GetSubsystems().GetWorldManager().GetActiveWorld();
    if (!activeWorld)
    {
        return snapshot;
    }

    // World 是 mesh object 名称的权威 owner；按名称排序后快照保持稳定，避免
    // unordered_map 遍历顺序让 runtime validation 产生非确定性结果。
    snapshot.meshObjectNames.reserve(activeWorld->GetMeshObjects().size());
    for (const auto& [objectName, object] : activeWorld->GetMeshObjects())
    {
        (void)object;
        snapshot.meshObjectNames.push_back(objectName);
    }
    std::sort(
        snapshot.meshObjectNames.begin(),
        snapshot.meshObjectNames.end());

    snapshot.hasHairResources = resources->hairResources != nullptr;
    if (resources->hairResources)
    {
        snapshot.sourceIdentity =
            resources->hairResources->sourceIdentity;
        snapshot.hairLutTextureIdentity = reinterpret_cast<std::uintptr_t>(
            resources->hairResources->azimuthalLutTexture.get());
    }

    const std::shared_ptr<Texture>* boundHairTexture =
        RendererResourceCache::GetInstance().GetWorldTexture(
            "hairAzimuthalLut");
    snapshot.boundHairWorldTextureIdentity =
        boundHairTexture != nullptr && *boundHairTexture != nullptr
        ? reinterpret_cast<std::uintptr_t>(boundHairTexture->get())
        : 0;

    const RenderGraph& renderGraph = RenderGraph::GetInstance();
    const auto forwardPassIt =
        renderGraph.GetRenderpasses().find("forwardTransparent");
    if (forwardPassIt != renderGraph.GetRenderpasses().end())
    {
        snapshot.forwardHairLutBinding = HasHairLutBinding(
            forwardPassIt->second,
            1);
    }
    const auto deferredPassIt =
        renderGraph.GetRenderpasses().find("deferredLighting");
    if (deferredPassIt != renderGraph.GetRenderpasses().end())
    {
        snapshot.deferredHairLutBinding = HasHairLutBinding(
            deferredPassIt->second,
            10);
    }

    const auto appendMaterialSnapshot =
        [&snapshot, &renderGraph](
            const std::shared_ptr<MaterialInstance>& instance)
        {
            if (!instance)
            {
                return;
            }
            const std::shared_ptr<Material> material =
                instance->GetBaseMaterial().lock();
            if (!material ||
                material->GetShaderVariantKey().shadingModelMacro !=
                    "SHADING_MODEL_HAIR")
            {
                return;
            }

            RuntimeHairMaterialSnapshot materialSnapshot;
            materialSnapshot.name = instance->GetName();
            materialSnapshot.materialKey = material->GetMaterialKey();
            materialSnapshot.shaderName = material->GetShaderName();
            materialSnapshot.shadingModelMacro =
                material->GetShaderVariantKey().shadingModelMacro;
            materialSnapshot.renderMode = RenderModeToString(
                material->GetShaderVariantKey().renderMode);
            materialSnapshot.hasRenderPipeline =
                material->GetRenderPipeline() != nullptr;
            materialSnapshot.hasShadowPipeline =
                material->GetShadowPipeline() != nullptr;

            const MaterialInstanceStateSnapshot state =
                instance->CaptureStateSnapshot();
            for (const auto& [parameterName, parameterValue] : state.parameters)
            {
                materialSnapshot.parameterValues[parameterName] =
                    FormatHairParameterValue(parameterValue);
            }
            // hairTangent 来自几何/顶点 frame，不是 MI 参数；这里校验 M_* 的完整材质合同。
            static const std::array<const char*, 7> requiredHairParameters = {
                "u_alphaClipThreshold",
                "u_tintColor",
                "u_pbrFactors",
                "u_hairOptical",
                "u_hairScattering",
                "u_hairCoverage",
                "u_emissiveStrength"};
            materialSnapshot.hasHairParameters = true;
            for (const char* parameterName : requiredHairParameters)
            {
                if (materialSnapshot.parameterValues.find(parameterName) ==
                    materialSnapshot.parameterValues.end())
                {
                    materialSnapshot.hasHairParameters = false;
                    break;
                }
            }

            if (materialSnapshot.renderMode == "TransparentAlphaBlend")
            {
                const auto forwardIt =
                    renderGraph.GetRenderpasses().find("forwardTransparent");
                if (forwardIt != renderGraph.GetRenderpasses().end() &&
                    material->GetRenderPipeline() != nullptr)
                {

                    // 两个独立创建的 VkDescriptorSetLayout 句柄不会因定义相同而相等，
                    // 必须比较完整 binding 合同，而不是比较句柄身份。
                    materialSnapshot.forwardDescriptorLayoutCompatible =
                        material->GetPassPipelineContractKey() ==
                            forwardIt->second.pipelineContractKey &&
                        HasCompatiblePassDescriptorLayout(
                            *material->GetRenderPipeline(),
                            forwardIt->second);
                }
            }
            snapshot.materials.push_back(std::move(materialSnapshot));
        };

    for (const auto& [materialInstanceKey, instance] : resources->materialInstances)
    {
        (void)materialInstanceKey;
        appendMaterialSnapshot(instance);
    }

    snapshot.captured = true;
    return snapshot;
}

RuntimeEyeValidationSnapshot
RuntimeValidationServices::CaptureEyeValidationSnapshot() const
{
    RuntimeEyeValidationSnapshot snapshot;
    const WorldHandle& worldHandle =
        engineLoop->GetSubsystems()
            .GetWorldManager()
            .GetActiveWorldHandle();
    snapshot.worldGeneration = worldHandle.generation;

    const RendererResourceCache::ImmutableWorldLocalResourceRefs resources =
        RendererResourceCache::GetInstance()
            .CaptureActiveWorldLocalResources();
    if (!resources)
    {
        return snapshot;
    }

    const std::shared_ptr<World> activeWorld =
        engineLoop->GetSubsystems().GetWorldManager().GetActiveWorld();
    if (!activeWorld)
    {
        return snapshot;
    }

    snapshot.meshObjectNames.reserve(activeWorld->GetMeshObjects().size());
    for (const auto& [objectName, object] : activeWorld->GetMeshObjects())
    {
        (void)object;
        snapshot.meshObjectNames.push_back(objectName);
    }
    std::sort(
        snapshot.meshObjectNames.begin(),
        snapshot.meshObjectNames.end());

    snapshot.hasEyeResources = resources->eyeResources != nullptr;
    if (resources->eyeResources)
    {
        snapshot.sourceDigest = resources->eyeResources->sourceDigest;
        snapshot.artifactGenerationKey =
            resources->eyeResources->computeArtifactGenerationKey;
        snapshot.lutWidth = resources->eyeResources->lutMetadata.width;
        snapshot.lutHeight = resources->eyeResources->lutMetadata.height;
        snapshot.lutLayers = resources->eyeResources->lutMetadata.layers;
        snapshot.eyeLutTextureIdentity = reinterpret_cast<std::uintptr_t>(
            resources->eyeResources->causticLutTexture.get());
    }

    const std::shared_ptr<Texture>* boundEyeTexture =
        RendererResourceCache::GetInstance().GetWorldTexture(
            "eyeCausticLut");
    snapshot.boundEyeWorldTextureIdentity =
        boundEyeTexture != nullptr && *boundEyeTexture != nullptr
        ? reinterpret_cast<std::uintptr_t>(boundEyeTexture->get())
        : 0;

    const RenderGraph& renderGraph = RenderGraph::GetInstance();
    const auto findPass = [&renderGraph](const char* name)
        -> const Renderpass*
    {
        const auto it = renderGraph.GetRenderpasses().find(name);
        return it == renderGraph.GetRenderpasses().end()
            ? nullptr
            : &it->second;
    };
    const Renderpass* forwardPass = findPass("forwardOpaque");
    const Renderpass* innerPass = findPass("forwardEyeInner");
    const Renderpass* corneaPass = findPass("forwardEyeCornea");
    const Renderpass* deferredPass = findPass("deferredLighting");
    snapshot.forwardEyeLutBinding =
        forwardPass != nullptr && HasEyeLutBinding(*forwardPass, 2);
    snapshot.forwardEyeInnerPassPresent =
        innerPass != nullptr && HasEyeLutBinding(*innerPass, 2);
    snapshot.forwardEyeCorneaPassPresent =
        corneaPass != nullptr && HasEyeLutBinding(*corneaPass, 2);
    snapshot.deferredEyeLutBinding =
        deferredPass != nullptr && HasEyeLutBinding(*deferredPass, 11);
    snapshot.sssSourcePresent = false;
    if (deferredPass != nullptr)
    {
        for (const std::string& output : deferredPass->outputResources)
        {
            if (output == "sssSource")
            {
                snapshot.sssSourcePresent = true;
                break;
            }
        }
    }

    const auto& compiledGraph = renderGraph.GetCompiledRenderGraph();
    for (const VL::CompiledRenderGraphPass& pass : compiledGraph.passes)
    {
        if (pass.name != "geometry")
        {
            continue;
        }
        snapshot.deferredGBufferAttachmentCount =
            pass.outputResources.size();
        const std::array<const char*, 9> requiredOutputs = {
            "gbufferA", "gbufferB", "gbufferC", "gbufferD", "gbufferE",
            "gbufferVelocity", "gbufferF", "sceneColorBase", "sceneDepth"};
        snapshot.deferredGBufferContract =
            pass.outputResources.size() == requiredOutputs.size();
        for (const char* required : requiredOutputs)
        {
            bool found = false;
            for (const VL::CompiledRenderGraphPassOutput& output :
                 pass.outputResources)
            {
                if (output.resource == required)
                {
                    found = true;
                    break;
                }
            }
            snapshot.deferredGBufferContract =
                snapshot.deferredGBufferContract && found;
        }
        break;
    }

    snapshot.eyeLutMemoryBytes = CalculateEyeLutMemoryBytes(
        RenderSystem::GetInstance().GetEyePerformanceBudget());
    snapshot.performanceStats =
        RenderSystem::GetInstance().GetEyePerformanceFrameStats();
    snapshot.performanceWithinBudget =
        RenderSystem::GetInstance().IsEyePerformanceFrameWithinBudget();
    snapshot.lodContractValid = true;
    for (const EyeProfileAsset& profile : resources->eyeResources->profiles)
    {
        try
        {
            ValidateEyeLodContract(profile.lodContract, profile.assetPath);
            snapshot.lodContractValid = snapshot.lodContractValid &&
                profile.lodContract.profileVersion == profile.profileVersion &&
                profile.lodContract.lutVersion == profile.causticLutVersion;
        }
        catch (...)
        {
            snapshot.lodContractValid = false;
        }
    }

    const std::array<const char*, 14> requiredEyeParameters = {
        "u_eyeSurface",
        "u_eyeGeometry",
        "u_eyeIrisColor",
        "u_eyeScleraColor",
        "u_eyeProfileId",
        "u_eyeScleraProfileId",
        "u_eyeCorneaIor",
        "u_eyeCausticStrength",
        "u_eyeLayer",
        "u_eyeContactVisibility",
        "u_eyeCiliaVisibility",
        "u_eyeUvHandedness",
        "u_eyePupilDilation",
        "u_eyeGaze"};
    for (const auto& [materialInstanceKey, instance] :
         resources->materialInstances)
    {
        (void)materialInstanceKey;
        if (!instance)
        {
            continue;
        }
        const std::shared_ptr<Material> material =
            instance->GetBaseMaterial().lock();
        if (!material ||
            material->GetShaderVariantKey().shadingModelMacro !=
                "SHADING_MODEL_EYE")
        {
            continue;
        }

        RuntimeEyeMaterialSnapshot materialSnapshot;
        materialSnapshot.name = instance->GetName();
        materialSnapshot.materialKey = material->GetMaterialKey();
        materialSnapshot.shadingModelMacro =
            material->GetShaderVariantKey().shadingModelMacro;
        materialSnapshot.renderMode = RenderModeToString(
            material->GetShaderVariantKey().renderMode);
        materialSnapshot.hasRenderPipeline =
            material->GetRenderPipeline() != nullptr;
        materialSnapshot.hasShadowPipeline =
            material->GetShadowPipeline() != nullptr;
        // ForwardOpaque/Opaque 普通材质由公共 Shadow pipeline 投影；只有
        // OpaqueClip/WPO 等特殊材质才拥有 Material-local Shadow pipeline。
        materialSnapshot.hasShadowRoute =
            materialSnapshot.hasShadowPipeline ||
            materialSnapshot.renderMode == "Opaque" ||
            materialSnapshot.renderMode == "ForwardOpaque";

        const MaterialInstanceStateSnapshot state =
            instance->CaptureStateSnapshot();
        for (const auto& [parameterName, parameterValue] : state.parameters)
        {
            materialSnapshot.parameterValues[parameterName] =
                FormatHairParameterValue(parameterValue);
        }
        materialSnapshot.hasEyeParameters = true;
        for (const char* parameterName : requiredEyeParameters)
        {
            if (materialSnapshot.parameterValues.find(parameterName) ==
                materialSnapshot.parameterValues.end())
            {
                materialSnapshot.hasEyeParameters = false;
                break;
            }
        }
        if (material->GetRenderPipeline() != nullptr)
        {
            const Renderpass* expectedPass = nullptr;
            if (materialSnapshot.renderMode == "ForwardOpaque")
            {
                expectedPass = forwardPass;
            }
            else if (materialSnapshot.renderMode == "ForwardEyeInner")
            {
                expectedPass = innerPass;
            }
            else if (materialSnapshot.renderMode == "ForwardEyeCornea")
            {
                expectedPass = corneaPass;
            }
            else if (materialSnapshot.renderMode == "Opaque")
            {
                expectedPass = findPass("geometry");
            }
            if (expectedPass != nullptr)
            {
                const bool compatible =
                    material->GetPassPipelineContractKey() ==
                        expectedPass->pipelineContractKey &&
                    HasCompatiblePassDescriptorLayout(
                        *material->GetRenderPipeline(),
                        *expectedPass);
                materialSnapshot.forwardDescriptorLayoutCompatible =
                    (materialSnapshot.renderMode == "ForwardOpaque" ||
                     materialSnapshot.renderMode == "ForwardEyeInner" ||
                     materialSnapshot.renderMode == "ForwardEyeCornea") &&
                    compatible;
                materialSnapshot.deferredDescriptorLayoutCompatible =
                    materialSnapshot.renderMode == "Opaque" && compatible;
            }
        }
        materialSnapshot.dualShellLayerContract =
            materialSnapshot.parameterValues.find("u_eyeLayer") !=
            materialSnapshot.parameterValues.end();
        snapshot.materials.push_back(std::move(materialSnapshot));
    }

    snapshot.captured = true;
    return snapshot;
}

RuntimeRendererResourceFingerprint
RuntimeValidationServices::CaptureRendererResourceFingerprint() const
{
    return CaptureRuntimeRendererResourceFingerprint();
}

RuntimeValidationGraphicsShaderSnapshot
RuntimeValidationServices::CaptureGraphicsShaderSnapshot() const
{
    RuntimeValidationGraphicsShaderSnapshot snapshot;
    const std::shared_ptr<Material> material =
        FindShaderReloadTestMaterial();
    if (!material)
    {
        throw std::runtime_error(
            "Shader reload runtime test material is not live");
    }
    snapshot.material = material;

    const GraphicsShaderVariantArtifact& surface =
        material->GetSurfaceShaderArtifact();
    const auto& shadow = material->GetShadowShaderArtifact();
    if (!material->GetRenderPipeline() ||
        !material->GetShadowPipeline() ||
        !shadow)
    {
        throw std::runtime_error(
            "Shader reload runtime test requires live Surface and Shadow pipelines");
    }

    snapshot.surfacePipeline =
        reinterpret_cast<std::uintptr_t>(
            material->GetRenderPipeline().get());
    snapshot.shadowPipeline =
        reinterpret_cast<std::uintptr_t>(
            material->GetShadowPipeline().get());
    snapshot.surfaceLogicalBuildId = surface.logicalBuildId;
    snapshot.shadowLogicalBuildId = shadow->logicalBuildId;
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
        engineLoop->shaderCompiler->GetShaderRoot() /
        "spv" /
        "shader-build-cache.json");
    snapshot.resolvedGeneration =
        RenderSystem::GetInstance()
            .GetResolvedShaderGenerationFingerprint();
    return snapshot;
}

bool RuntimeValidationServices::
SurfaceArtifactDependsOnSourceWithDigest(
    const std::string& logicalBuildId,
    const std::string& dependencyIdentity,
    const std::string& expectedDigest) const
{
    if (logicalBuildId.empty() || !engineLoop->shaderCompiler)
    {
        return false;
    }

    const ShaderBuildManifestSnapshot manifest =
        engineLoop->shaderCompiler->CaptureManifestSnapshot();
    const auto artifactIt =
        manifest.artifacts.find(logicalBuildId);
    if (artifactIt == manifest.artifacts.end())
    {
        return false;
    }
    for (const ShaderDependencyRecord& dependency :
         artifactIt->second.dependencies)
    {
        if (dependency.path == dependencyIdentity)
        {
            return dependency.digest == expectedDigest;
        }
    }
    return false;
}

bool RuntimeValidationServices::
ManifestArtifactsDependOnAllSources(
    const std::vector<std::string>& logicalBuildIds,
    const std::vector<std::string>& sourceIdentities) const
{
    if (!engineLoop->shaderCompiler)
    {
        return false;
    }

    const ShaderBuildManifestSnapshot manifest =
        engineLoop->shaderCompiler->CaptureManifestSnapshot();
    for (const std::string& logicalBuildId : logicalBuildIds)
    {
        if (!ManifestArtifactDependsOnAllSources(
                manifest,
                logicalBuildId,
                sourceIdentities))
        {
            return false;
        }
    }
    return true;
}

bool RuntimeValidationServices::
UiArtifactFragmentMatchesCurrentSource() const
{
    if (!engineLoop->shaderCompiler)
    {
        return false;
    }

    ShaderVariantKey variant;
    variant.shaderName = "uiOverlay";
    const ShaderBuildRequest request =
        engineLoop->shaderCompiler
            ->CreateGraphicsVariantBuildRequest(variant);
    const ShaderBuildManifestSnapshot manifest =
        engineLoop->shaderCompiler->CaptureManifestSnapshot();
    const auto artifactIt =
        manifest.artifacts.find(request.logicalBuildId);
    if (artifactIt == manifest.artifacts.end())
    {
        return false;
    }

    const std::filesystem::path fragmentPath =
        engineLoop->shaderCompiler->GetShaderRoot() /
        "glsl" /
        "uiOverlay.frag";
    const std::string currentDigest =
        ContentHasher::HashFile(fragmentPath).ToHex();
    for (const ShaderBuildSourceRecord& source :
         artifactIt->second.primarySources)
    {
        if (source.identity == "uiOverlay.frag")
        {
            return source.digest == currentDigest;
        }
    }
    return false;
}

std::string RuntimeValidationServices::GetComputeShaderGeneration(
    const std::string& shaderName) const
{
    return RenderSystem::GetInstance()
        .GetActiveComputeShaderArtifact(shaderName)
        .artifactGenerationKey;
}

RuntimeValidationManualShaderReloadResult
RuntimeValidationServices::ExecuteManualGraphicsShaderReload(
    const std::vector<std::string>& sourceIdentities,
    bool injectPipelineFailure)
{
    RuntimeValidationManualShaderReloadResult result;
    try
    {
        const uint64_t generation =
            engineLoop->shaderReloadRuntime
                ->AllocateGenerationForValidation();
        const uint64_t worldGeneration =
            GetActiveWorldHandle().generation;
        ShaderReloadPlan plan =
            engineLoop->shaderReloadCoordinator
                ->CaptureGraphicsPlanForSources(
                    sourceIdentities,
                    generation,
                    worldGeneration);
        ShaderReloadCandidateBatch batch =
            engineLoop->shaderReloadCoordinator
                ->CompileGraphicsCandidates(std::move(plan));
        engineLoop->WaitForRenderThreadIdle();
        if (engineLoop->shouldClose)
        {
            throw std::runtime_error(
                "Render thread failed during shader reload test");
        }

        if (injectPipelineFailure)
        {
            PipelineFactory::TestFaultInjection injection;
            injection.failGraphicsPipelineCreationAt = 2;
            engineLoop->pipelineFactory
                ->SetTestFaultInjection(injection);
        }

        const ShaderReloadCommitStatistics statistics =
            engineLoop->shaderReloadCoordinator
                ->CommitGraphicsCandidates(
                    batch,
                    worldGeneration);
        engineLoop->pipelineFactory->SetTestFaultInjection({});
        RenderSystem::GetInstance()
            .RefreshResolvedSceneAfterShaderReload();
        result.succeeded = true;
        result.committed = statistics.committed;
        result.affectedBuildCount =
            statistics.affectedBuildCount;
        result.pipelinesCreated =
            statistics.pipelinesCreated;
        result.pipelinesRetired =
            statistics.pipelinesRetired;
    }
    catch (const std::exception& exception)
    {
        if (engineLoop->pipelineFactory)
        {
            engineLoop->pipelineFactory
                ->SetTestFaultInjection({});
        }
        result.failureMessage = exception.what();
    }
    return result;
}

ShaderReloadRuntimeStateSnapshot
RuntimeValidationServices::CaptureShaderReloadState() const
{
    return engineLoop->shaderReloadRuntime
        ? engineLoop->shaderReloadRuntime->CaptureSnapshot()
        : ShaderReloadRuntimeStateSnapshot{};
}

void RuntimeValidationServices::SetShaderMonitorPollInterval(
    std::chrono::milliseconds interval)
{
    engineLoop->shaderReloadRuntime
        ->SetTestPollInterval(interval);
}

void RuntimeValidationServices::SetShaderMonitorScanSuspended(
    bool suspended)
{
    engineLoop->shaderReloadRuntime
        ->SetTestScanSuspended(suspended);
}

void RuntimeValidationServices::RefreshShaderMonitorBaseline(
    const std::vector<std::string>& sourceIdentities)
{
    engineLoop->shaderReloadRuntime
        ->RefreshBaselineForSources(sourceIdentities);
}

void RuntimeValidationServices::ArmShaderCompileGate()
{
    engineLoop->shaderReloadRuntime
        ->ArmTestCompileGateForNextSubmit();
}

bool RuntimeValidationServices::IsShaderCompileGateWaiting() const
{
    return engineLoop->shaderReloadRuntime &&
        engineLoop->shaderReloadRuntime
            ->IsWaitingAtTestCompileGate();
}

void RuntimeValidationServices::ReleaseShaderCompileGate()
{
    engineLoop->shaderReloadRuntime
        ->ReleaseTestCompileGate();
}

void RuntimeValidationServices::DisableShaderCompileGate()
{
    if (engineLoop->shaderReloadRuntime)
    {
        engineLoop->shaderReloadRuntime
            ->DisableTestCompileGate();
    }
}

void RuntimeValidationServices::ArmShaderPostCompileGate()
{
    engineLoop->shaderReloadRuntime
        ->ArmTestPostCompileGateForNextSubmit();
}

bool RuntimeValidationServices::IsShaderPostCompileGateWaiting() const
{
    return engineLoop->shaderReloadRuntime &&
        engineLoop->shaderReloadRuntime
            ->IsWaitingAtTestPostCompileGate();
}

RuntimeResult<WorldHandle>
RuntimeValidationServices::ExecuteWorldGraphTransaction(
    const std::string& scenePath)
{
    return engineLoop->ExecuteWorldGraphTransaction(scenePath);
}

RuntimeResult<WorldHandle>
RuntimeValidationServices::
ExecuteMaterialDefinitionWorldGraphTransaction(
    const std::set<std::string>& sourceIdentities,
    uint64_t batchId)
{
    return engineLoop
        ->ExecuteMaterialDefinitionWorldGraphTransactionForTest(
            sourceIdentities,
            batchId);
}

RuntimeResult<void>
RuntimeValidationServices::ReloadRenderGraphResources()
{
    return engineLoop->ReloadRenderGraphResources();
}

RuntimeResult<void>
RuntimeValidationServices::RecreateRendererForWindowResize(
    uint32_t width,
    uint32_t height)
{
    return engineLoop->RecreateRendererForWindowResize(
        width,
        height);
}

RuntimeResult<void>
RuntimeValidationServices::ResizeWindowAndRenderer(
    uint32_t width,
    uint32_t height)
{
    if (!engineLoop->window)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "RuntimeValidation.NullWindow",
            "Runtime validation cannot resize before window initialization."));
    }
    engineLoop->window->SetSize(width, height);
    return engineLoop->RecreateRendererForWindowResize(
        width,
        height);
}

void RuntimeValidationServices::
SetWorldGraphTransactionFaultInjection(
    WorldGraphTransactionTestFaultInjection injection) noexcept
{
    engineLoop->SetWorldGraphTransactionTestFaultInjection(
        injection);
}

void RuntimeValidationServices::ProcessShaderRuntimeRequest(
    const RuntimeCommandExecutionResult& commandResult)
{
    engineLoop->ProcessShaderRuntimeRequests(commandResult);
}

void RuntimeValidationServices::WaitForRenderThreadIdle()
{
    engineLoop->WaitForRenderThreadIdle();
}

size_t RuntimeValidationServices::
GetPendingRetiredResourceCount() const noexcept
{
    return ResourceRetireQueue::GetInstance().GetPendingCount();
}

std::weak_ptr<void>
RuntimeValidationServices::FindPendingRetiredResource(
    const std::string& label,
    uint64_t generation) const
{
    return ResourceRetireQueue::GetInstance()
        .FindPendingResourceForTest(label, generation);
}

std::uintptr_t RuntimeValidationServices::GetWorldTextureIdentity(
    const std::string& key) const
{
    const std::shared_ptr<Texture>* texture =
        RendererResourceCache::GetInstance()
            .GetWorldTexture(key);
    return texture != nullptr && *texture
        ? reinterpret_cast<std::uintptr_t>(texture->get())
        : 0;
}

bool RuntimeValidationServices::IsClosing() const noexcept
{
    return engineLoop->shouldClose;
}

int RuntimeValidationServices::GetExitCode() const noexcept
{
    return engineLoop->exitCode;
}

void RuntimeValidationServices::MarkClosingTestFailure() noexcept
{
    engineLoop->exitCode = 2;
    engineLoop->shouldClose = true;
}

void RuntimeValidationServices::MarkShutdownTestSucceeded() noexcept
{
    engineLoop->exitCode = 0;
}

std::string
RuntimeValidationServices::CaptureWorldGraphRuntimeFingerprint(
    const std::string& primaryDefinitionPath,
    const std::string& batchDefinitionPath,
    bool includeFrameLifecycleDiagnostics,
    std::string* details) const
{
    std::ostringstream stream;
    const WorldManager& worldManager =
        engineLoop->GetSubsystems().GetWorldManager();
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
    for (const std::string& name : graphResourceNames)
    {
        const bool isMsaa =
            name.rfind("msaa:", 0) == 0;
        const std::string resourceName =
            name.substr(isMsaa ? 5 : 8);
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
                 layer < resource.imageViewHandles.size();
                 ++layer)
            {
                stream << "graph.resource." << name <<
                    "." << index << ".layer." <<
                    layer << "=" <<
                    resource.imageViewHandles[layer].id << ";";
            }
        }
    }

    std::vector<std::string> passNames;
    passNames.reserve(renderGraph.GetRenderpasses().size());
    for (const auto& [passName, renderpass] :
         renderGraph.GetRenderpasses())
    {
        (void)renderpass;
        passNames.push_back(passName);
    }
    std::sort(passNames.begin(), passNames.end());
    for (const std::string& passName : passNames)
    {
        const Renderpass& renderpass =
            renderGraph.GetRenderpasses().at(passName);
        stream << "graph.pass." << passName <<
            ".renderPass=" <<
            renderpass.renderPassHandle.id << ";";
        stream << "graph.pass." << passName <<
            ".descriptorPool=" <<
            renderpass.descriptorPoolHandle.id << ";";
        stream << "graph.pass." << passName <<
            ".descriptorLayout=" <<
            renderpass.descriptorSetLayoutHandle.id << ";";
        stream << "graph.pass." << passName <<
            ".emptyLayout=" <<
            renderpass.emptyDescriptorSetLayoutHandle.id << ";";
        for (size_t index = 0;
             index < renderpass.framebufferHandles.size();
             ++index)
        {
            stream << "graph.pass." << passName <<
                ".framebuffer." << index << "=" <<
                renderpass.framebufferHandles[index].id << ";";
        }
    }

    const RenderSystem& renderSystem =
        RenderSystem::GetInstance();
    stream << "renderSystem.generation=" <<
        renderSystem.GetActiveWorldGeneration() << ";";
    stream << "renderSystem.resolved=" <<
        renderSystem.GetResolvedShaderGenerationFingerprint() << ";";
    stream << "controller.generation=" <<
        (engineLoop->controller
            ? engineLoop->controller
                  ->GetBoundWorldGeneration()
            : 0) << ";";
    if (includeFrameLifecycleDiagnostics)
    {
        stream << "renderSystem.pendingSnapshot=" <<
            renderSystem.HasPendingWorldSnapshotForTest() << ";";
        const WorldSnapshotPtr pendingSnapshot =
            renderSystem.PeekPendingWorldSnapshotForTest();
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
        renderSystem.GetLightCapacityForTest() << ";";
    const auto& lightBufferHandles =
        renderSystem.GetLightBufferHandlesForTest();
    for (size_t index = 0;
         index < lightBufferHandles.size();
         ++index)
    {
        stream << "renderSystem.lightBuffer." <<
            index << "=" <<
            lightBufferHandles[index].id << ";";
    }
    stream << "pipelineFactory=" <<
        engineLoop->pipelineFactory
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
        engineLoop->rendererBackend)
    {
        const std::array<size_t, 9> backendCounts =
            CaptureBackendIdentityCounts(
                *engineLoop->rendererBackend);
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
    const std::shared_ptr<MaterialInstance> materialInstance =
        FindShaderReloadTestMaterialInstance();
    const std::shared_ptr<RendererObjectResourceEntry>
        objectResources =
            FindShaderReloadTestObjectResources();
    if (!material || !materialInstance || !objectResources)
    {
        throw std::runtime_error(
            "Cannot fingerprint the shader definition runtime package");
    }
    stream << "material.pointer=" <<
        reinterpret_cast<std::uintptr_t>(material.get()) << ";";
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
            ? shadowArtifact->artifactGenerationKey
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
        objectPackage.shadowDescriptorPoolHandle.id << ";";
    for (size_t imageIndex = 0;
         imageIndex <
             objectPackage.descriptorSetHandles.size();
         ++imageIndex)
    {
        for (size_t setIndex = 0;
             setIndex <
                 objectPackage
                     .descriptorSetHandles[imageIndex].size();
             ++setIndex)
        {
            stream << "object.descriptor." <<
                imageIndex << "." << setIndex << "=" <<
                objectPackage
                    .descriptorSetHandles[imageIndex][setIndex]
                    .id << ";";
        }
    }

    const std::shared_ptr<MaterialInstance> batchInstance =
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
        engineLoop->shaderCompiler->GetShaderRoot();
    stream << "artifact.manifest=" <<
        HashFileIfPresent(
            shaderRoot /
            "spv" /
            "shader-build-cache.json") << ";";
    const std::filesystem::path primaryInclude =
        std::filesystem::path(primaryDefinitionPath)
            .parent_path() /
        "generate" /
        "M_shaderReloadTestParamter.glsl";
    const std::filesystem::path batchInclude =
        std::filesystem::path(batchDefinitionPath)
            .parent_path() /
        "generate" /
        "M_shaderReloadBatchTestParamter.glsl";
    stream << "artifact.primaryInclude=" <<
        HashFileIfPresent(primaryInclude) << ";";
    stream << "artifact.batchInclude=" <<
        HashFileIfPresent(batchInclude) << ";";

    const std::string capturedDetails = stream.str();
    if (details)
    {
        *details = capturedDetails;
    }
    return ContentHasher::HashString(capturedDetails).ToHex();
}

std::string
RuntimeValidationServices::
CaptureShaderShutdownInflightFingerprint() const
{
    if (!engineLoop->shaderCompiler ||
        !engineLoop->pipelineFactory ||
        !engineLoop->rendererBackend)
    {
        throw std::runtime_error(
            "Cannot capture shutdown-in-flight fingerprint before renderer initialization");
    }

    std::ostringstream stream;
    const WorldHandle& world =
        engineLoop->GetSubsystems()
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
        engineLoop->pipelineFactory
            ->CaptureIdentityFingerprintForTest() << ";";

    const std::filesystem::path shaderRoot =
        engineLoop->shaderCompiler->GetShaderRoot();
    stream << "artifact.manifest=" <<
        HashFileIfPresent(
            shaderRoot /
            "spv" /
            "shader-build-cache.json") << ";";
    ShaderVariantKey uiVariant;
    uiVariant.shaderName = "uiOverlay";
    const ShaderBuildRequest uiRequest =
        engineLoop->shaderCompiler
            ->CreateGraphicsVariantBuildRequest(uiVariant);
    for (const auto& [role, outputPath] :
         uiRequest.outputPaths)
    {
        stream << "artifact." << role << "=" <<
            HashFileIfPresent(outputPath) << ";";
    }

    stream << "reload.committed=" <<
        CaptureShaderReloadState()
            .latestAutoReloadCommittedGeneration << ";";
    return ContentHasher::HashString(stream.str()).ToHex();
}

} // namespace VL
