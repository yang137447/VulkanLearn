#include "renderSystem.h"
#include "passBarrier.h"
#include "render/offline/offlineWorldAssets.h"
#include "render/offline/offlineGroomAsset.h"
#include "shader/build/contentHash.h"
#include "shader/reload/shaderReloadCoordinator.h"
#include <cmath>
#include <limits>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include "core/runtimeResult.h"
#include "core/logStore.h"
#include "sceneNode.h"
#include "materialInstance.h"
#include "material.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "commonFunction.h"
#include "renderGraph.h"
#include "hair/hairSimulationPass.h"
#include "render/skinning/skinDeformPass.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/placementBindingResolver.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/rendergraph/preparedRenderGraphState.h"
#include "renderableObject.h"
#include "material/materialAssetUtils.h"
#include "material/loader/materialInstanceResolver.h"
#include "render/backend/rendererBackendVulkan.h"
#include "shaderReflect.h"
#include "profiler.h"
#include "ui/uiRenderSnapshot.h"
#include "vulkanDebug.h"
#include "world/world.h"
#include "scene/sceneAssetTypes.h"

namespace
{

std::shared_ptr<MaterialInstance> GetPassMaterialInstance(const char* passName)
{
    const auto& renderpasses = RenderGraph::GetInstance().GetRenderpasses();
    auto passIt = renderpasses.find(passName);
    if (passIt == renderpasses.end())
    {
        return nullptr;
    }

    return passIt->second.materialInstance.lock();
}

bool SetPassVectorComponent(
    const char* passName,
    const char* parameterName,
    int componentIndex,
    float value,
    const char* missingPassMessage,
    std::string& outMessage)
{
    std::shared_ptr<MaterialInstance> materialInstance = GetPassMaterialInstance(passName);
    if (!materialInstance)
    {
        outMessage = missingPassMessage;
        return false;
    }

    if (!materialInstance->HasParameter(parameterName))
    {
        outMessage = std::string("Parameter '") + parameterName + "' not found in pass material.";
        return false;
    }

    Eigen::Vector4f params = materialInstance->GetParameter<Eigen::Vector4f>(parameterName);
    params[componentIndex] = value;
    materialInstance->SetParameter(parameterName, params);
    return true;
}

bool SetPassVector(
    const char* passName,
    const char* parameterName,
    const Eigen::Vector4f& value,
    std::string& outMessage)
{
    std::shared_ptr<MaterialInstance> materialInstance = GetPassMaterialInstance(passName);
    if (!materialInstance)
    {
        outMessage = std::string("Pass material instance is expired: ") + passName;
        return false;
    }
    if (!materialInstance->HasParameter(parameterName))
    {
        outMessage = std::string("Parameter '") + parameterName +
            "' not found in pass material.";
        return false;
    }
    materialInstance->SetParameter(parameterName, value);
    return true;
}

float ResolveOfflineDisplayExposure(
    const VL::OfflinePublishedResult* result,
    float manualExposure,
    const VL::CameraSnapshot& fallbackCamera)
{
    if (result &&
        result->manifest.contains("camera") &&
        result->manifest["camera"].is_object() &&
        result->manifest["camera"].contains("exposureScale") &&
        result->manifest["camera"]["exposureScale"].is_number())
    {
        return manualExposure *
            result->manifest["camera"]["exposureScale"].get<float>();
    }
    return manualExposure * VL::ExposureScale(fallbackCamera.exposure);
}

using PreviewRenderStateValues = std::map<std::string, std::string>;

struct NumericPreviewDraftParseResult
{
    std::optional<MaterialInstanceNumericParameterValues> values;
    std::optional<nlohmann::json> serializedJson;
    PreviewRenderStateValues renderStates;
    std::string errorMessage;
};

PreviewRenderStateValues BuildPreviewRenderStateValues(
    const nlohmann::json& effectiveMaterialInstanceJson)
{
    PreviewRenderStateValues result;
    result.emplace(
        "shadingModel",
        effectiveMaterialInstanceJson.at("shadingModel").get<std::string>());
    const nlohmann::json& renderStates =
        effectiveMaterialInstanceJson.at("renderStates");
    if (!renderStates.is_object())
    {
        throw std::runtime_error(
            "Material instance preview render states must be a JSON object.");
    }
    for (const auto& [name, value] : renderStates.items())
    {
        if (!value.is_string())
        {
            throw std::runtime_error(
                "Material instance preview render state must be a string: " +
                name);
        }
        result.emplace(name, value.get<std::string>());
    }
    return result;
}

bool ReadFinitePreviewNumber(
    const nlohmann::json& value,
    float& outValue)
{
    if (!value.is_number())
    {
        return false;
    }

    const double doubleValue = value.get<double>();
    const float floatValue = static_cast<float>(doubleValue);
    if (!std::isfinite(doubleValue) || !std::isfinite(floatValue))
    {
        return false;
    }
    outValue = floatValue;
    return true;
}

void ApplyMaterialOverrideToRenderScene(
    VL::RenderScene& renderScene,
    const std::string& materialInstancePath,
    const std::string& materialKey)
{
    for (VL::RenderDrawPacket& drawPacket : renderScene.drawPackets)
    {
        if (drawPacket.materialInstance.key == materialInstancePath)
        {
            drawPacket.material = VL::ResourceHandle{materialKey, 0};
        }
    }
}

void RebuildRenderSceneMaterialGroupsImpl(VL::RenderScene& renderScene)
{
    renderScene.materialGroups.clear();
    for (size_t drawPacketIndex = 0;
         drawPacketIndex < renderScene.drawPackets.size();
         ++drawPacketIndex)
    {
        const VL::RenderDrawPacket& drawPacket =
            renderScene.drawPackets[drawPacketIndex];
        VL::MaterialDrawGroup* materialGroup = nullptr;
        for (VL::MaterialDrawGroup& candidateGroup :
             renderScene.materialGroups)
        {
            if (candidateGroup.material.key == drawPacket.material.key &&
                candidateGroup.material.generation ==
                    drawPacket.material.generation)
            {
                materialGroup = &candidateGroup;
                break;
            }
        }
        if (materialGroup == nullptr)
        {
            VL::MaterialDrawGroup newGroup;
            newGroup.material = drawPacket.material;
            renderScene.materialGroups.push_back(std::move(newGroup));
            materialGroup = &renderScene.materialGroups.back();
        }

        VL::MaterialInstanceDrawGroup* materialInstanceGroup = nullptr;
        for (VL::MaterialInstanceDrawGroup& candidateGroup :
             materialGroup->materialInstances)
        {
            if (candidateGroup.materialInstance.key ==
                    drawPacket.materialInstance.key &&
                candidateGroup.materialInstance.generation ==
                    drawPacket.materialInstance.generation)
            {
                materialInstanceGroup = &candidateGroup;
                break;
            }
        }
        if (materialInstanceGroup == nullptr)
        {
            VL::MaterialInstanceDrawGroup newGroup;
            newGroup.materialInstance = drawPacket.materialInstance;
            materialGroup->materialInstances.push_back(std::move(newGroup));
            materialInstanceGroup =
                &materialGroup->materialInstances.back();
        }
        materialInstanceGroup->drawPacketIndices.push_back(drawPacketIndex);
    }
}

NumericPreviewDraftParseResult ParseNumericPreviewDraft(
    const VL::Editor::Preview::MaterialInstancePreviewDraft& draft,
    const VL::Editor::Preview::NormalizedMaterialInstancePath& expectedPath,
    const MaterialInstanceStateSnapshot::TextureMap& activeTextures,
    const nlohmann::json& frozenDefinition)
{
    NumericPreviewDraftParseResult result;
    const VL::Editor::Preview::MaterialInstancePreviewPathNormalizationResult
        draftPath =
            VL::Editor::Preview::NormalizeMaterialInstancePath(
                draft.materialInstancePath);
    if (!draftPath.Succeeded())
    {
        result.errorMessage = draftPath.errorMessage;
        return result;
    }
    if (draftPath.path->value != expectedPath.value)
    {
        result.errorMessage =
            "Material instance preview draft does not match the connected MI path.";
        return result;
    }
    if (draft.serializedWorkingDraft.empty())
    {
        result.errorMessage =
            "Material instance preview draft must contain serialized content.";
        return result;
    }

    try
    {
        const nlohmann::json materialInstanceJson =
            nlohmann::json::parse(draft.serializedWorkingDraft);
        if (!materialInstanceJson.is_object())
        {
            result.errorMessage =
                "Material instance preview draft must be a JSON object.";
            return result;
        }
        if (materialInstanceJson.contains("textures"))
        {
            const nlohmann::json& textures = materialInstanceJson.at("textures");
            if (!textures.is_object())
            {
                result.errorMessage =
                    "Material instance preview textures must be a JSON object.";
                return result;
            }

            // 数值 preview 不创建 descriptor；只有能用 active MI 的稳定 asset
            // identity 证明纹理未变化时，才允许携带原有 texture override。
            for (const auto& [textureName, textureValue] : textures.items())
            {
                if (!textureValue.is_string())
                {
                    result.errorMessage =
                        "Material instance texture preview changes are unavailable: " +
                        textureName;
                    return result;
                }

                const auto activeTextureIt = activeTextures.find(textureName);
                if (activeTextureIt == activeTextures.end() ||
                    !activeTextureIt->second.textureAssetIdentity.has_value() ||
                    MaterialAssetUtils::NormalizeAssetPath(
                        textureValue.get<std::string>()) !=
                        MaterialAssetUtils::NormalizeAssetPath(
                            *activeTextureIt->second.textureAssetIdentity))
                {
                    result.errorMessage =
                        "Material instance texture preview changes are unavailable: " +
                        textureName;
                    return result;
                }
            }
        }

        const MaterialInstanceResolveResult resolved =
            MaterialInstanceResolver::ResolveWithDefinition(
                expectedPath.value,
                materialInstanceJson, frozenDefinition);
        PreviewRenderStateValues renderStates =
            BuildPreviewRenderStateValues(
                resolved.effectiveMaterialInstanceJson);
        result.renderStates = renderStates;
        result.serializedJson = materialInstanceJson;
        const nlohmann::json& parameters =
            resolved.effectiveMaterialInstanceJson.at("parameters");
        if (!parameters.is_object())
        {
            result.errorMessage =
                "Material instance preview parameters must be a JSON object.";
            return result;
        }

        MaterialInstanceNumericParameterValues values;
        for (const auto& [parameterName, parameterValue] : parameters.items())
        {
            if (parameterValue.is_number())
            {
                float scalarValue = 0.0f;
                if (!ReadFinitePreviewNumber(parameterValue, scalarValue))
                {
                    result.errorMessage =
                        "Material instance preview numeric value is invalid: " +
                        parameterName;
                    return result;
                }
                values.emplace(parameterName, scalarValue);
                continue;
            }

            if (!parameterValue.is_array() ||
                (parameterValue.size() != 2 &&
                 parameterValue.size() != 3 &&
                 parameterValue.size() != 4))
            {
                result.errorMessage =
                    "Material instance preview parameter must be float, vec2, vec3, or vec4: " +
                    parameterName;
                return result;
            }

            float components[4] = {};
            for (size_t componentIndex = 0;
                 componentIndex < parameterValue.size();
                 ++componentIndex)
            {
                if (!ReadFinitePreviewNumber(
                        parameterValue.at(componentIndex),
                        components[componentIndex]))
                {
                    result.errorMessage =
                        "Material instance preview numeric value is invalid: " +
                        parameterName;
                    return result;
                }
            }

            if (parameterValue.size() == 2)
            {
                values.emplace(
                    parameterName,
                    Eigen::Vector2f(components[0], components[1]));
            }
            else if (parameterValue.size() == 3)
            {
                values.emplace(
                    parameterName,
                    Eigen::Vector3f(
                        components[0],
                        components[1],
                        components[2]));
            }
            else
            {
                values.emplace(
                    parameterName,
                    Eigen::Vector4f(
                        components[0],
                        components[1],
                        components[2],
                        components[3]));
            }
        }
        result.values = std::move(values);
    }
    catch (const std::exception& exception)
    {
        result.errorMessage =
            std::string("Material instance preview draft validation failed: ") +
            exception.what();
    }
    return result;
}

} // namespace

class RenderSystem::MaterialInstancePreviewSession final
    : public VL::Editor::Preview::IRendererMaterialInstancePreviewSession
{
public:
    MaterialInstancePreviewSession(
        RenderSystem& ownerValue,
        VL::Editor::Preview::MaterialInstancePreviewWorldIdentity worldValue,
        VL::Editor::Preview::NormalizedMaterialInstancePath pathValue,
        uint64_t ownerGenerationValue,
        std::shared_ptr<MaterialInstance> materialInstanceValue,
        MaterialInstanceNumericParameterValues baselineParametersValue,
        MaterialInstanceStateSnapshot::TextureMap baselineTexturesValue,
        PreviewRenderStateValues baselineRenderStatesValue,
        VL::Editor::Preview::MaterialInstancePreviewDraft baselineDraftValue)
        : owner(ownerValue)
        , world(std::move(worldValue))
        , materialInstancePath(std::move(pathValue))
        , ownerGeneration(ownerGenerationValue)
        , materialInstance(std::move(materialInstanceValue))
        , baselineParameters(std::move(baselineParametersValue))
        , baselineTextures(std::move(baselineTexturesValue))
        , baselineRenderStates(std::move(baselineRenderStatesValue))
        , activeRenderStates(baselineRenderStates)
        , baselineDraft(std::move(baselineDraftValue))
    {
    }

    VL::Editor::Preview::MaterialInstancePreviewAdapterResult Apply(
        const VL::Editor::Preview::MaterialInstancePreviewDraft& draft) override
    {
        return Commit(draft, false);
    }

    VL::Editor::Preview::MaterialInstancePreviewAdapterResult RestoreBaseline(
        const VL::Editor::Preview::MaterialInstancePreviewDraft& baselineDraft) override
    {
        // 仍使用调用方 revision 参与门禁，但内容来自 connect 时捕获的源快照，
        // 避免外部 document 在连接期间改写 baseline 后影响运行时 Restore。
        VL::Editor::Preview::MaterialInstancePreviewDraft restoreDraft =
            this->baselineDraft;
        restoreDraft.documentRevision = baselineDraft.documentRevision;
        return Commit(restoreDraft, true);
    }

private:
    VL::Editor::Preview::MaterialInstancePreviewAdapterResult Commit(
        const VL::Editor::Preview::MaterialInstancePreviewDraft& draft,
        bool restoreBaseline)
    {
        if (owner.offlineMode)
            return owner.CommitMaterialInstancePreviewDraft(world, materialInstancePath, draft);
        const auto activeWorld = owner.GetActiveWorldIdentity();
        if (!activeWorld || *activeWorld != world ||
            !owner.activeOfflineWorldAssets ||
            !owner.activeOfflineWorldAssets->materialDefinitions.count(
                materialInstancePath.value))
        {
            VL::Editor::Preview::MaterialInstancePreviewAdapterResult result;
            result.status = VL::Editor::Preview::PreviewAdapterOperationStatus::Failed;
            result.failureStage = VL::Editor::Preview::PreviewAdapterFailureStage::Prepare;
            result.message = "Material instance preview lost its frozen World definition.";
            return result;
        }
        NumericPreviewDraftParseResult parsed =
        ParseNumericPreviewDraft(
                draft,
                materialInstancePath,
                baselineTextures,
                owner.activeOfflineWorldAssets->materialDefinitions.at(
                    materialInstancePath.value));
        if (!parsed.values.has_value())
        {
            VL::Editor::Preview::MaterialInstancePreviewAdapterResult result;
            result.status =
                VL::Editor::Preview::PreviewAdapterOperationStatus::Failed;
            result.failureStage =
                VL::Editor::Preview::PreviewAdapterFailureStage::Prepare;
            result.message = std::move(parsed.errorMessage);
            return result;
        }

        const bool renderStateChanged =
            parsed.renderStates != activeRenderStates;

        const std::optional<
            VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
            currentCapture =
                VL::RendererResourceCache::GetInstance()
                    .CaptureMaterialInstanceForGeneration(
                        ownerGeneration,
                        materialInstancePath.value);
        if (!currentCapture.has_value() ||
            currentCapture->ownerGeneration != ownerGeneration ||
            currentCapture->materialInstance.get() != materialInstance.get())
        {
            VL::Editor::Preview::MaterialInstancePreviewAdapterResult result;
            result.status =
                VL::Editor::Preview::PreviewAdapterOperationStatus::Failed;
            result.failureStage =
                VL::Editor::Preview::PreviewAdapterFailureStage::Prepare;
            result.message =
                "Material instance preview target is no longer the active World-local MI.";
            return result;
        }

        if (renderStateChanged)
        {
            VL::Editor::Preview::MaterialInstancePreviewAdapterResult result =
                owner.CommitMaterialInstancePreviewDraft(
                    world,
                    materialInstancePath,
                    draft);
            if (result.status ==
                VL::Editor::Preview::PreviewAdapterOperationStatus::Completed)
            {
                const std::optional<
                    VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
                    replacementCapture =
                        VL::RendererResourceCache::GetInstance()
                            .CaptureMaterialInstanceForGeneration(
                                ownerGeneration,
                                materialInstancePath.value);
                if (!replacementCapture.has_value() ||
                    !replacementCapture->materialInstance)
                {
                    result.status =
                        VL::Editor::Preview::PreviewAdapterOperationStatus::Failed;
                    result.failureStage =
                        VL::Editor::Preview::PreviewAdapterFailureStage::Commit;
                    result.liveSwapCommitted = false;
                    result.replacesLiveResource = false;
                    result.message =
                        "Material instance preview committed without an active replacement MI.";
                    return result;
                }
                materialInstance = replacementCapture->materialInstance;
                ownerGeneration = replacementCapture->ownerGeneration;
                activeRenderStates = parsed.renderStates;
            }
            return result;
        }

        std::shared_ptr<const VL::OfflineWorldAssets> preparedOfflineAssets;
        try
        {
            preparedOfflineAssets = owner.PrepareOfflineMaterialPreview(
                materialInstancePath.value, *parsed.serializedJson);
        }
        catch (const std::exception& exception)
        {
            VL::Editor::Preview::MaterialInstancePreviewAdapterResult result;
            result.status =
                VL::Editor::Preview::PreviewAdapterOperationStatus::Failed;
            result.failureStage =
                VL::Editor::Preview::PreviewAdapterFailureStage::Prepare;
            result.message =
                std::string("Offline material preview prepare failed: ") +
                exception.what();
            return result;
        }
        try
        {
            // 外部 profile 可能在准备与数值提交之间变化；先复核字节身份，
            // 否则 GPU 草稿与冻结 CPU 参数会指向不同版本。
            for (const auto& [reference, captured] :
                 preparedOfflineAssets->modelProfileDigests)
            {
                const auto& path =
                    preparedOfflineAssets->modelProfilePaths.at(reference);
                if (!std::filesystem::is_regular_file(path) ||
                    VL::ContentHasher::HashFile(path).ToHex() != captured)
                    throw std::runtime_error(
                        "modelProfile changed before preview commit: " + reference);
            }
            VL::ValidateOfflineTexturesStillCurrent(*preparedOfflineAssets);
            materialInstance->CommitNumericParameterValues(
                restoreBaseline ? baselineParameters : *parsed.values);
        }
        catch (const std::exception& exception)
        {
            VL::Editor::Preview::MaterialInstancePreviewAdapterResult result;
            result.status =
                VL::Editor::Preview::PreviewAdapterOperationStatus::Failed;
            result.failureStage =
                VL::Editor::Preview::PreviewAdapterFailureStage::Commit;
            result.message =
                std::string("Material instance numeric preview commit failed: ") +
                exception.what();
            return result;
        }
        owner.activeOfflineWorldAssets.swap(preparedOfflineAssets);

        VL::Editor::Preview::MaterialInstancePreviewAdapterResult result;
        result.status =
            VL::Editor::Preview::PreviewAdapterOperationStatus::Completed;
        result.runtimeResourceGeneration = ownerGeneration;
        result.liveSwapCommitted = true;
        result.replacesLiveResource = false;
        result.message = restoreBaseline
            ? "Material instance numeric preview baseline restored."
            : "Material instance numeric preview applied.";
        return result;
    }

    RenderSystem& owner;
    VL::Editor::Preview::MaterialInstancePreviewWorldIdentity world;
    VL::Editor::Preview::NormalizedMaterialInstancePath materialInstancePath;
    uint64_t ownerGeneration = 0;
    std::shared_ptr<MaterialInstance> materialInstance;
    MaterialInstanceNumericParameterValues baselineParameters;
    MaterialInstanceStateSnapshot::TextureMap baselineTextures;
    PreviewRenderStateValues baselineRenderStates;
    PreviewRenderStateValues activeRenderStates;
    VL::Editor::Preview::MaterialInstancePreviewDraft baselineDraft;
};

class RenderSystem::OfflineMaterialInstancePreviewSession final
    : public VL::Editor::Preview::IRendererMaterialInstancePreviewSession
{
public:
    OfflineMaterialInstancePreviewSession(RenderSystem& owner,
        VL::Editor::Preview::MaterialInstancePreviewWorldIdentity world,
        VL::Editor::Preview::NormalizedMaterialInstancePath path,
        VL::Editor::Preview::MaterialInstancePreviewDraft baseline)
        : owner(owner), world(std::move(world)), path(std::move(path)),
          baseline(std::move(baseline)) {}
    VL::Editor::Preview::MaterialInstancePreviewAdapterResult Apply(
        const VL::Editor::Preview::MaterialInstancePreviewDraft& draft) override
    {
        return owner.CommitMaterialInstancePreviewDraft(world, path, draft);
    }
    VL::Editor::Preview::MaterialInstancePreviewAdapterResult RestoreBaseline(
        const VL::Editor::Preview::MaterialInstancePreviewDraft& draft) override
    {
        auto restored = baseline;
        restored.documentRevision = draft.documentRevision;
        return owner.CommitMaterialInstancePreviewDraft(world, path, restored);
    }
private:
    RenderSystem& owner;
    VL::Editor::Preview::MaterialInstancePreviewWorldIdentity world;
    VL::Editor::Preview::NormalizedMaterialInstancePath path;
    VL::Editor::Preview::MaterialInstancePreviewDraft baseline;
};

RenderSystem::~RenderSystem()
{
    ShutdownRenderObject();
}

void RenderSystem::SetUiOverlayShaderPaths(std::string vertexPath, std::string fragmentPath)
{
    uiVertexShaderPath = std::move(vertexPath);
    uiFragmentShaderPath = std::move(fragmentPath);
}

void RenderSystem::SetCsmSettings(const VL::CsmSettings& settings)
{
    csmSettings = settings;
    shadowCascadeFrameData.valid = false;
}

bool RenderSystem::SetCsmCastShadows(bool enabled, std::string& outMessage)
{
    csmSettings.castShadows = enabled;
    shadowCascadeFrameData.valid = false;
    outMessage = std::string("CSM ") + (enabled ? "enabled." : "disabled.");
    return true;
}

bool RenderSystem::SetCsmDynamicShadowDistance(
    float distance,
    std::string& outMessage)
{
    if (distance <= 0.0f)
    {
        outMessage = "CSM shadow distance must be positive.";
        return false;
    }

    csmSettings.dynamicShadowDistance = distance;
    shadowCascadeFrameData.valid = false;
    outMessage = "CSM shadow distance set to " + std::to_string(distance);
    return true;
}

bool RenderSystem::SetCsmCascadeCount(
    uint32_t cascadeCount,
    std::string& outMessage)
{
    if (cascadeCount < 1 ||
        cascadeCount > VL::CsmSettings::MaxCascadeCount)
    {
        outMessage = "CSM cascade count must be in the range [1, 4].";
        return false;
    }

    csmSettings.cascadeCount = cascadeCount;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM cascade count set to " +
        std::to_string(cascadeCount);
    return true;
}

bool RenderSystem::SetCsmCascadeDistributionExponent(
    float exponent,
    std::string& outMessage)
{
    if (exponent < 1.0f || exponent > 10.0f)
    {
        outMessage =
            "CSM cascade distribution exponent must be in the range [1, 10].";
        return false;
    }

    csmSettings.cascadeDistributionExponent = exponent;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM cascade distribution exponent set to " +
        std::to_string(exponent);
    return true;
}

bool RenderSystem::SetCsmCascadeTransitionFraction(
    float fraction,
    std::string& outMessage)
{
    if (fraction < 0.0f || fraction > 1.0f)
    {
        outMessage =
            "CSM cascade transition fraction must be in the range [0, 1].";
        return false;
    }

    csmSettings.cascadeTransitionFraction = fraction;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM cascade transition fraction set to " +
        std::to_string(fraction);
    return true;
}

bool RenderSystem::SetCsmShadowDistanceFadeoutFraction(
    float fraction,
    std::string& outMessage)
{
    if (fraction < 0.0f || fraction > 1.0f)
    {
        outMessage =
            "CSM shadow distance fadeout fraction must be in the range [0, 1].";
        return false;
    }

    csmSettings.shadowDistanceFadeoutFraction = fraction;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM shadow distance fadeout fraction set to " +
        std::to_string(fraction);
    return true;
}

bool RenderSystem::SetCsmShadowBias(
    float value,
    std::string& outMessage)
{
    if (value < 0.0f || value > 1.0f)
    {
        outMessage = "CSM shadow bias must be in the range [0, 1].";
        return false;
    }

    csmSettings.shadowBias = value;
    shadowCascadeFrameData.valid = false;
    outMessage = "CSM shadow bias set to " + std::to_string(value);
    return true;
}

bool RenderSystem::SetCsmShadowSlopeBias(
    float value,
    std::string& outMessage)
{
    if (value < 0.0f || value > 1.0f)
    {
        outMessage = "CSM shadow slope bias must be in the range [0, 1].";
        return false;
    }

    csmSettings.shadowSlopeBias = value;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM shadow slope bias set to " +
        std::to_string(value);
    return true;
}

bool RenderSystem::SetCsmShadowCascadeBiasDistribution(
    float value,
    std::string& outMessage)
{
    if (value < 0.0f || value > 1.0f)
    {
        outMessage =
            "CSM shadow cascade bias distribution must be in the range [0, 1].";
        return false;
    }

    csmSettings.shadowCascadeBiasDistribution = value;
    shadowCascadeFrameData.valid = false;
    outMessage =
        "CSM shadow cascade bias distribution set to " +
        std::to_string(value);
    return true;
}

VL::EnvironmentUpdateDiagnostics RenderSystem::GetEnvironmentUpdateDiagnostics() const
{
    // 这里用了raii锁，确保在访问环境诊断信息时不会发生数据竞争。
    std::lock_guard<std::mutex> lock(environmentDiagnosticsMutex);
    return environmentDiagnostics;
}

ComputeShaderArtifact RenderSystem::GetActiveComputeShaderArtifact(
    const std::string& shaderName) const
{
    if (shaderName == exposureHistogramReloadParticipant.GetShaderName())
        return exposureHistogramReloadParticipant.GetActiveArtifact();
    if (shaderName == exposureAdaptReloadParticipant.GetShaderName())
        return exposureAdaptReloadParticipant.GetActiveArtifact();
    if (shaderName == "generator/skyToCubemap")
    {
        return proceduralSkyCubeGenerator.GetActiveArtifact();
    }
    if (shaderName == "generator/skySHGenerate")
    {
        return environmentIblBaker
            .GetActiveSkySHGenerateArtifact();
    }
    if (shaderName == "generator/prefilterEnvMap")
    {
        return environmentIblBaker
            .GetActivePrefilterEnvMapArtifact();
    }
    throw std::runtime_error(
        "No live compute reload participant for shader: " +
        shaderName);
}

void RenderSystem::SetActiveWorld(std::shared_ptr<const VL::World> world)
{
    // World 换代先释放旧 preview session，避免旧 MI shared_ptr 延长已退休
    // package 的生命周期；新的连接必须重新按 generation/path 捕获目标。
    materialInstancePreviewAdapter.Reset();
    activeWorld = std::move(world);
    // 显式清空/关闭 World 时，CPU 包也不再由 RenderSystem 留存。
    activeOfflineWorldAssets.reset();
    offlineCameraMotionPolicy.Reset();
    worldSnapshotQueue.Clear();
    worldSnapshotBuilder.Reset();
    currentRenderScene = VL::RenderScene();
    currentResolvedRenderScene = VL::ResolvedRenderScene();
    pickingRenderScene = VL::RenderScene();
    hasPickingRenderScene = false;
    hasMaterialInstancePreviewMaterialOverride = false;
    materialInstancePreviewOverridePath.clear();
    materialInstancePreviewOverrideMaterialKey.clear();
    hasRenderScene = false;
    ClearSelectedMaterialInstance();
    currentWindTimeSeconds = 0.0;
    windClockInitialized = false;
    windPendingDeltaSeconds.store(0.0f, std::memory_order_relaxed);
    windTickAccumulator = 0.0;
    sceneWindRuntime.Reset();
    initializedRenderWorldGeneration = 0;
    nextSnapshotFrameIndex = 0;
    simulationDeltaSeconds = 0.0f;
    gameSimulationDeltaSeconds = 0.0f;
    hairSimulationDeltaSeconds = 0.0f;
    shadowCascadeFrameData.valid = false;
    if (activeWorld)
    {
        windResponseProfiles.Configure(activeWorld->GetWindResponseProfiles());
    }
    else
    {
        windResponseProfiles.Reset();
    }
}

bool RenderSystem::SetToneMappingMode(int mode, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "toneMapping",
            "u_toneMappingParams",
            3,
            static_cast<float>(mode),
            "Tone mapping pass material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Tone mapping mode set to " + std::to_string(mode);
    toneMappingMode = mode;
    return true;
}

bool RenderSystem::SetBloomStrength(float value, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "toneMapping",
            "u_toneMappingParams",
            1,
            value,
            "Tone mapping pass material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Bloom strength set to " + std::to_string(value);
    bloomStrength = value;
    return true;
}

bool RenderSystem::SetBloomThreshold(float value, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "bloomPrefilter",
            "u_bloomPrefilterParams",
            0,
            value,
            "Bloom prefilter material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Bloom threshold set to " + std::to_string(value);
    bloomThreshold = value;
    return true;
}

bool RenderSystem::SetBloomKnee(float value, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "bloomPrefilter",
            "u_bloomPrefilterParams",
            1,
            value,
            "Bloom prefilter material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Bloom knee set to " + std::to_string(value);
    bloomKnee = value;
    return true;
}

bool RenderSystem::SetBloomClamp(float value, std::string& outMessage)
{
    if (!SetPassVectorComponent(
            "bloomPrefilter",
            "u_bloomPrefilterParams",
            2,
            value,
            "Bloom prefilter material instance is expired.",
            outMessage))
    {
        return false;
    }

    outMessage = "Bloom clamp set to " + std::to_string(value);
    bloomClamp = value;
    return true;
}

void RenderSystem::InitializeWorldTransactionResources()
{
    PROFILE_FUNCTION();
    RenderInitialize();
}

bool RenderSystem::SetOfflineMode(bool enabled, std::string& message)
{
    if (enabled == offlineMode) return true;
    try
    {
        if (!activeWorld || !activeOfflineWorldAssets)
            throw std::runtime_error("No active frozen World");
        if (enabled)
        {
            VL::WorldSnapshotBuilder builder;
            VL::WorldSnapshotBuildDesc desc;
            desc.worldGeneration = activeWorld->GetGeneration();
            const auto snapshot = builder.Build(*activeWorld, desc);
            if (snapshot.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(snapshot.Error()));
            auto settings = offlineSettings;
            settings.width = GetSceneRenderExtent().width;
            settings.height = GetSceneRenderExtent().height;
            const auto candidate = VL::PreflightOfflineRenderInput(
                snapshot.Value(), *activeOfflineWorldAssets, settings);
            if (candidate.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(candidate.Error()));
            offlineMode = true;
            offlineCoordinator->Restart();
            offlineCameraMotionPolicy.Reset();
            message = "Offline mode";
            return true;
        }
        if (!activeWorld->GetSquareAreaLights().empty())
            throw std::runtime_error("Realtime does not support squareAreaLight");
        for (const auto& [path, definition] : activeOfflineWorldAssets->materialDefinitions)
            if (!definition.value("realtimeSupported", true))
                throw std::runtime_error("Realtime implementation unavailable: " + path);
        if (!activeOfflineWorldAssets->realtimePrepared)
        {
            // 在新图/缓存候选中准备全部 GPU 资源；失败不写 active cache/World。
            auto assets = std::make_shared<VL::OfflineWorldAssets>(*activeOfflineWorldAssets);
            auto graph = std::make_shared<VL::PreparedRenderGraphState>(*rendererBackend);
            graph->Load(RenderGraph::GetInstance().GetSourceJson());
            auto cache = VL::RendererResourceCache::GetInstance().BeginCandidate(
                activeWorld->GetGeneration());
            PipelineFactory::GraphicsCandidateState graphics;
            VL::RendererResourceLoadContext context{cache, graph->GetGraph()};
            context.graphicsCandidateState = &graphics;
            context.offlineWorldAssets = assets.get();
            VL::RendererMaterialLoader materialLoader(*pipelineFactory, *rendererBackend, context);
            // pass MI 按图合同复用；冻结的仅是场景 MI。相同 shadow MI 被四个 pass
            // 共用，逐 pass 重建会使前几个 Renderpass 的 weak_ptr 立即过期。
            materialLoader.LoadPassMaterials();
            context.useFrozenMaterials = true;
            VL::RendererMeshLoader meshLoader(*pipelineFactory, *rendererBackend, context);
            for (const auto& object : assets->sceneJson.at("objects"))
            {
                if (object.at("type") != "mesh") continue;
                const auto imported = assets->meshImports.at(object.at("modelPath").get<std::string>());
                MeshAssetLoadRequest request;
                request.buildPlan = imported->buildPlan;
                meshLoader.LoadMeshObject(object, request);
            }
            auto runtime = PrepareRuntimeBinding(activeWorld, cache, graph->GetGraph(), assets, false);
            auto pipeline = pipelineFactory->PrepareCandidateCommit(graphics);
            // runtime 持有同一个候选包；下面的 no-throw swap 会一并发布此能力位。
            // 准备失败时 activeOfflineWorldAssets 仍保留旧值和离线模式。
            assets->realtimePrepared = true;
            auto& retireQueue = VL::ResourceRetireQueue::GetInstance();
            auto retirements = retireQueue.PrepareRetirements({
                {"OfflineMode:Resources", activeWorld->GetGeneration(), retireQueue.GetLastSubmittedEpoch(), {}},
                {"OfflineMode:Graph", activeWorld->GetGeneration(), retireQueue.GetLastSubmittedEpoch(), {}},
                {"OfflineMode:LightBuffer", activeWorld->GetGeneration(), retireQueue.GetLastSubmittedEpoch(), {}}});
            pipelineFactory->CommitPreparedCandidate(std::move(pipeline));
            auto oldCache = VL::RendererResourceCache::GetInstance().CommitCandidate(std::move(cache));
            RenderGraph::GetInstance().SwapState(graph->GetGraph());
            auto oldLight = CommitPreparedRuntimeBinding(std::move(runtime));
            retirements.GetAdditionalResource(0).resource = std::static_pointer_cast<void>(oldCache);
            retirements.GetAdditionalResource(1).resource = std::static_pointer_cast<void>(graph);
            retirements.GetAdditionalResource(2).resource = std::move(oldLight);
            retireQueue.CommitPreparedRetirements(std::move(retirements));
        }
        offlineMode = false;
        offlineCoordinator->Stop();
        offlineCameraMotionPolicy.Reset();
        message = "Realtime mode";
        return true;
    }
    catch (const std::exception& exception)
    {
        message = exception.what();
        return false;
    }
}

bool RenderSystem::SetOfflineSetting(const std::string& name, int value, std::string& message)
{
    if ((name == "samples" && value > 0 && value <= 1000000) ||
        (name == "bounces" && value >= 0 && value <= 64) ||
        (name == "seed" && value >= 0) ||
        (name == "lightSamples" && value > 0 && value <= 64))
    {
        if (name == "samples")
        {
            if (value < offlineSettings.samples && offlineCoordinator)
                offlineCoordinator->Restart();
            offlineSettings.samples = value;
        }
        else if (name == "bounces") offlineSettings.bounces = value;
        else if (name == "seed") offlineSettings.seed = static_cast<uint32_t>(value);
        else offlineSettings.lightSamples = value;
        return true;
    }
    message = "Invalid offline setting: " + name;
    return false;
}

bool RenderSystem::SetOfflineExposure(float exposure, std::string& message)
{
    if (!std::isfinite(exposure) || exposure <= 0)
    {
        message = "Offline exposure must be finite and positive";
        return false;
    }
    offlineExposure = exposure;
    return true;
}

void RenderSystem::ControlOfflineSession(int command)
{
    if (!offlineCoordinator) return;
    if (command == 0) offlineCoordinator->Pause();
    else if (command == 1) offlineCoordinator->Resume();
    else if (command == 2) offlineCoordinator->Stop();
    else if (command == 3)
    {
        offlineCoordinator->Restart();
        offlineCameraMotionPolicy.Reset();
    }
}

VL::Offline::SessionState RenderSystem::GetOfflineState() const
{
    return offlineCoordinator ? offlineCoordinator->State() : VL::Offline::SessionState::Idle;
}

std::string RenderSystem::GetOfflineError() const
{
    return offlineCoordinator ? offlineCoordinator->Error() : std::string();
}

std::shared_ptr<const VL::OfflinePublishedResult> RenderSystem::CaptureOfflineResult() const
{
    return offlineCoordinator ? offlineCoordinator->Capture() : nullptr;
}

bool RenderSystem::ExportOffline(const VL::OfflinePublishedResult& result,
    const std::filesystem::path& directory, std::string& message) const
{
    try
    {
        VL::ExportOfflineResult(
            result,
            directory,
            ResolveOfflineDisplayExposure(
                &result,
                offlineExposure,
                currentRenderScene.camera));
        message = "Offline output: " + directory.string();
        return true;
    }
    catch (const std::exception& exception)
    {
        message = exception.what();
        return false;
    }
}

void RenderSystem::FinalizeInitialRenderObjectInitialization()
{
    PROFILE_FUNCTION();
    offlineDisplay.Initialize(*rendererBackend);
    offlineCoordinator = std::make_unique<VL::OfflineRenderCoordinator>();
    if (uiRenderSnapshotQueue != nullptr && !uiVertexShaderPath.empty() && !uiFragmentShaderPath.empty())
    {
        uiOverlayRenderer.Initialize(
            *rendererBackend,
            uiVertexShaderPath,
            uiFragmentShaderPath);
        ShaderVariantKey uiVariant;
        uiVariant.shaderName = "uiOverlay";
        uiOverlayRenderer.SetActiveShaderArtifact(
            pipelineFactory->PrepareGraphicsShaderVariant(
                uiVariant));
        if (shaderReloadCoordinator != nullptr)
        {
            shaderReloadCoordinator->SetUiOverlayParticipant(
                &uiOverlayRenderer);
        }
    }
}

void RenderSystem::Render()
{
    PROFILE_SCOPE("RenderSystem::Render");
    // Single-thread compatibility path: GT publishes the same immutable
    // snapshot that the optional RT consumes, then renders it immediately.
    // EngineLoop 的 Tick 为了给 CPU 阶段分别计时，直接调用下面两步而不是本函数；
    // 这里保留组合入口给需要一次性完成 publish+render 的调用方。
    PublishSnapshotFromActiveWorld();
    RenderLatestSnapshotOrLastGood();
}

void RenderSystem::RequestScreenshot(std::string path)
{
    pendingScreenshotPath = std::move(path);
}

void RenderSystem::RequestHairTrace(std::string path, uint32_t frames,
    uint32_t guideStride)
{
    VL::Hair::HairTraceRequest request;
    request.path = std::move(path);
    request.frames = frames;
    request.guideStride = guideStride;
    std::lock_guard<std::mutex> lock(hairTraceRequestMutex);
    pendingHairTraceRequest = std::move(request);
}

void RenderSystem::RequestHairSimulationReset(std::string objectName)
{
    std::lock_guard<std::mutex> lock(hairResetRequestMutex);
    pendingHairResetRequest = std::move(objectName);
}

void RenderSystem::RequestHairSimulationParameter(
    const VL::Hair::HairLiveParameterRequest& request)
{
    std::lock_guard<std::mutex> lock(hairLiveParameterMutex);
    // 队列有界：滑杆拖动时一帧可能累积多条；超过上限先丢最旧的意图，
    // 保留最新值，避免没有渲染帧时无限增长。
    if (pendingHairLiveParameters.size() >= 64)
        pendingHairLiveParameters.erase(pendingHairLiveParameters.begin());
    pendingHairLiveParameters.push_back(request);
}

void RenderSystem::PrintGpuPassStats()
{
    // 控制台按需启用采集；Snapshot 只读最近一次读回的窗口，不等待渲染线程。
    gpuPassTimer.RequestCollection();
    const VL::GpuPassStatsSnapshot snapshot = gpuPassTimer.Snapshot();
    if (!snapshot.gpuTimingSupported)
    {
        std::cout << "[GPU] pass timing is unavailable on this device "
                     "(timestamp queries unsupported or timestampPeriod == 0)."
                  << std::endl;
        return;
    }
    if (!snapshot.collecting || snapshot.passes.empty())
    {
        // 首次启用采集时还没有可打印的表；让渲染线程在第一次读回后补打一张，
        // 这样单次 'gpustats' 命令也能在接下来一两帧后给出数据。
        gpuPassTimer.RequestPrint();
        std::cout << "[GPU] pass timing collection enabled; no completed samples "
                     "have been read back yet. A table will be logged after the next readback."
                  << std::endl;
        return;
    }

    const std::string table = VL::FormatGpuPassTimingTable(
        VL::ToGpuPassTimingTable(snapshot));
    VL::ProcessLog().Append(VL::LogSeverity::Info, "GPU", table);
    std::cout << "[Diagnostics][Info] " << table << std::endl;
}

void RenderSystem::PrintPerformanceJson()
{
    // 与 Performance 页消费同一个 GpuPassTimer 快照：面板、perf json 和
    // --perf-jsonl 只是同一份值的三个消费者，没有第二套计时或序列化规则。
    gpuPassTimer.RequestCollection();
    std::cout << VL::FormatPerformanceJson(gpuPassTimer.Snapshot()) << std::endl;
}

void RenderSystem::SetSceneViewport(const VL::UiViewportRect& rect, bool enabled)
{
    // GT 已等上一帧 RT 归还所有权；隐藏视口时保留最后有效尺寸，不触发离线重启。
    sceneViewportRequested = enabled;
    if (rect.width != 0 && rect.height != 0) requestedSceneViewport = rect;
}

vk::Extent2D RenderSystem::GetSceneRenderExtent() const
{
    if (sceneViewportRequested && requestedSceneViewport.width != 0 && requestedSceneViewport.height != 0)
        return {requestedSceneViewport.width, requestedSceneViewport.height};
    return rendererBackend->GetSwapchainExtent();
}

void RenderSystem::ReleaseSwapchainDependentResources()
{
    if (offlineCoordinator) offlineCoordinator->Stop();
    offlineDisplay.Shutdown();
    uiOverlayRenderer.ReleaseSwapchainDependentResources();
    VL::RendererResourceCache::GetInstance().ShutdownSwapchainDependentWorldResources();
    ShutdownFrameResources();
    initializedRenderWorldGeneration = 0;
}

void RenderSystem::RebuildSwapchainDependentResources()
{
    RenderGraph& renderGraph = RenderGraph::GetInstance();

    RefreshRenderSceneFromActiveWorld();
    if (!offlineMode) BuildResolvedRenderScene();
    RenderInitialize();

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    if (!offlineMode)
    {
        renderGraph.InitializeAutoExposure(*pipelineFactory, *rendererBackend);
        renderGraph.InitializeHairSimulation(*pipelineFactory, *rendererBackend);
        if (prerenderSkinEnabled)
            renderGraph.InitializeSkinDeform(*pipelineFactory, *rendererBackend);
        renderGraph.RenderInitialize(*rendererBackend, descriptorContext);
        InitializeCurrentRenderSceneResources();
    }
    offlineDisplay.Initialize(*rendererBackend);
    if (offlineCoordinator) offlineCoordinator->Restart();
    offlineCameraMotionPolicy.Reset();
    if (uiOverlayRenderer.IsInitialized())
    {
        uiOverlayRenderer.RebuildSwapchainDependentResources();
    }
}

void RenderSystem::RebuildRenderGraphDependentResources()
{
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem renderer backend is not set");
    }

    RenderGraph& renderGraph = RenderGraph::GetInstance();

    RefreshRenderSceneFromActiveWorld();
    BuildResolvedRenderScene();
    RenderInitialize();

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    renderGraph.InitializeAutoExposure(*pipelineFactory, *rendererBackend);
    renderGraph.InitializeHairSimulation(*pipelineFactory, *rendererBackend);
    if (prerenderSkinEnabled)
        renderGraph.InitializeSkinDeform(*pipelineFactory, *rendererBackend);
    renderGraph.RenderInitialize(*rendererBackend, descriptorContext);
    InitializeCurrentRenderSceneResources();
}

void RenderSystem::RefreshResolvedSceneAfterShaderReload()
{
    if (hasRenderScene && !offlineMode)
    {
        BuildResolvedRenderScene();
    }
}

VL::PreparedRuntimeBinding RenderSystem::PrepareRuntimeBinding(
    std::shared_ptr<const VL::World> world,
    VL::RendererResourceCache& candidateCache,
    RenderGraph& candidateGraph,
    std::shared_ptr<const VL::OfflineWorldAssets> offlineWorldAssets,
    bool prepareOffline)
{
    if (!world)
    {
        throw std::runtime_error(
            "Cannot prepare runtime binding without a candidate World");
    }
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error(
            "Cannot prepare runtime binding without a renderer backend");
    }

    VL::PreparedRuntimeBinding prepared;
    prepared.world = std::move(world);
    if (!offlineWorldAssets ||
        offlineWorldAssets->worldGeneration != prepared.world->GetGeneration())
        throw std::runtime_error("Offline CPU package generation mismatch");
    prepared.offlineWorldAssets = std::move(offlineWorldAssets);
    prepared.csmSettings =
        prepared.world->GetCsmSettings();

    VL::WorldSnapshotBuildDesc snapshotDesc;
    snapshotDesc.worldGeneration =
        prepared.world->GetGeneration();
    snapshotDesc.frameIndex = nextSnapshotFrameIndex;
    snapshotDesc.debugViewMode = debugViewMode;
    snapshotDesc.environmentIntensity =
        environmentIntensity;
    snapshotDesc.hasSelectedDraw =
        hasSelectedDraw && selectedWorldGeneration == prepared.world->GetGeneration();
    snapshotDesc.selectedAllMaterialSlots =
        selectedAllMaterialSlots &&
        selectedWorldGeneration == prepared.world->GetGeneration();
    snapshotDesc.selectedObjectId = selectedObjectId;
    snapshotDesc.selectedMaterialSlotIndex = selectedMaterialSlotIndex;
    auto snapshotResult =
        prepared.snapshotBuilder.Build(
            *prepared.world,
            snapshotDesc);
    if (snapshotResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(snapshotResult.Error()));
    }

    auto renderSceneResult =
        rendererFrontend.BuildRenderScene(
            snapshotResult.Value());
    if (renderSceneResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(
                renderSceneResult.Error()));
    }
    prepared.renderScene =
        std::move(renderSceneResult.Value());
    if (prepareOffline)
    {
        VL::OfflineRenderSettings settings = offlineSettings;
        settings.width = GetSceneRenderExtent().width;
        settings.height = GetSceneRenderExtent().height;
        auto offlineInput = VL::PreflightOfflineRenderInput(
            snapshotResult.Value(), *prepared.offlineWorldAssets, settings);
        if (offlineInput.IsFailure())
            throw std::runtime_error(VL::FormatRuntimeError(offlineInput.Error()));
        prepared.lightCapacity = frameResources.PrepareLightCapacity(0, *rendererBackend);
        prepared.nextSnapshotFrameIndex = nextSnapshotFrameIndex + 1;
        return prepared;
    }
    // 离线加载的 World 持有逻辑 M_ key；实时准备仅在渲染视图中绑定真正 GPU key，
    // 不重建 World 或改相机/对象作者状态。
    for (auto& draw : prepared.renderScene.drawPackets)
    {
        const auto* instance = candidateCache.GetMaterialInstance(draw.materialInstance.key);
        if (instance && *instance)
            draw.material.key = (*instance)->GetBaseMaterial().lock()->GetMaterialKey();
    }
    RebuildRenderSceneMaterialGroups(prepared.renderScene);

    auto resolvedSceneResult =
        resolvedRenderSceneBuilder.Build(
            prepared.renderScene,
            candidateCache);
    if (resolvedSceneResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(
                resolvedSceneResult.Error()));
    }
    prepared.resolvedRenderScene =
        std::move(resolvedSceneResult.Value());

    prepared.lightCapacity =
        frameResources.PrepareLightCapacity(
            prepared.renderScene.lights.size(),
            *rendererBackend);
    PrepareEnvironmentResources(
        prepared.renderScene,
        candidateCache);

    const std::vector<vk::DescriptorBufferInfo>&
        candidateLightBufferInfos =
            prepared.lightCapacity.GetBufferInfos(
                frameResources);
    const VL::RendererDescriptorContext
        descriptorContext =
            BuildRendererDescriptorContext(
                candidateCache,
                candidateGraph,
                candidateLightBufferInfos);
    candidateGraph.InitializeAutoExposure(*pipelineFactory, *rendererBackend);
    candidateGraph.InitializeHairSimulation(*pipelineFactory, *rendererBackend);
    if (prerenderSkinEnabled)
        candidateGraph.InitializeSkinDeform(*pipelineFactory, *rendererBackend);
    candidateGraph.RenderInitialize(
        *rendererBackend,
        descriptorContext);

    VL::RendererObjectResourceRegistry
        objectResourceRegistry;
    objectResourceRegistry.InitializeResolvedSceneResources(
        *rendererBackend,
        descriptorContext,
        prepared.renderScene,
        prepared.resolvedRenderScene,
        candidateCache,
        prerenderSkinEnabled);

    prepared.windResponseProfiles.Configure(
        prepared.world->GetWindResponseProfiles());
    prepared.nextSnapshotFrameIndex =
        nextSnapshotFrameIndex + 1;
    return prepared;
}

std::shared_ptr<void>
RenderSystem::CommitPreparedRuntimeBinding(
    VL::PreparedRuntimeBinding prepared) noexcept
{
    activeWorld.swap(prepared.world);
    activeOfflineWorldAssets.swap(prepared.offlineWorldAssets);
    worldSnapshotBuilder.Swap(
        prepared.snapshotBuilder);

    using std::swap;
    swap(
        currentRenderScene.worldGeneration,
        prepared.renderScene.worldGeneration);
    swap(
        currentRenderScene.frameIndex,
        prepared.renderScene.frameIndex);
    swap(
        currentRenderScene.hairSimulationDeltaSeconds,
        prepared.renderScene.hairSimulationDeltaSeconds);
    swap(
        currentRenderScene.camera,
        prepared.renderScene.camera);
    currentRenderScene.drawPackets.swap(
        prepared.renderScene.drawPackets);
    currentRenderScene.materialGroups.swap(
        prepared.renderScene.materialGroups);
    currentRenderScene.lights.swap(
        prepared.renderScene.lights);
    swap(
        currentRenderScene.environment,
        prepared.renderScene.environment);
    // 场景作者目标与场级解析结果随同一代 RenderScene 提交；头发消费 resolvedWind，
    // 植被消费 wind 目标并在每 profile 内维护自己的响应/gust 状态。
    swap(currentRenderScene.wind, prepared.renderScene.wind);
    swap(currentRenderScene.resolvedWind, prepared.renderScene.resolvedWind);
    swap(currentRenderScene.windTimeSeconds, prepared.renderScene.windTimeSeconds);
    swap(
        currentRenderScene.debugViewMode,
        prepared.renderScene.debugViewMode);
    currentResolvedRenderScene.materialGroups.swap(
        prepared.resolvedRenderScene.materialGroups);
    windResponseProfiles.Swap(
        prepared.windResponseProfiles);
    swap(csmSettings, prepared.csmSettings);

    std::shared_ptr<void> retiredLightBuffer =
        frameResources.CommitPreparedLightCapacity(
            std::move(prepared.lightCapacity));
    hasRenderScene = true;
    currentWindTimeSeconds = 0.0;
    windClockInitialized = false;
    windPendingDeltaSeconds.store(0.0f, std::memory_order_relaxed);
    windTickAccumulator = 0.0;
    sceneWindRuntime.Reset();
    initializedRenderWorldGeneration =
        currentRenderScene.worldGeneration;
    nextSnapshotFrameIndex =
        prepared.nextSnapshotFrameIndex;
    ClearSelectedMaterialInstance();
    shadowCascadeFrameData.valid = false;
    environmentUpdateState.Reset();
    environmentUpdateScheduler.Reset();
    environmentUpdateSourceCube.reset();
    // The old World package keeps its HDRI alive until the prepared retire
    // epoch. Drop descriptor-source mirrors now so they cannot outlive that
    // package; the next IBL use rewrites the acquired image's descriptor.
    environmentIblBaker.InvalidateEnvironmentCubeBindings();
    return retiredLightBuffer;
}

void RenderSystem::ClearPendingWorldSnapshots() noexcept
{
    worldSnapshotQueue.Clear();
}

void RenderSystem::PrepareRenderGraphReload(
    RenderGraph& candidateGraph)
{
    if (!activeWorld || !hasRenderScene)
    {
        throw std::runtime_error(
            "Cannot prepare a RenderGraph reload without an active runtime binding");
    }
    const VL::RendererDescriptorContext
        descriptorContext =
            BuildRendererDescriptorContext(
                VL::RendererResourceCache::GetInstance(),
                candidateGraph,
                GetLightBufferInfo());
    candidateGraph.InitializeAutoExposure(*pipelineFactory, *rendererBackend);
    candidateGraph.InitializeHairSimulation(*pipelineFactory, *rendererBackend);
    if (prerenderSkinEnabled)
        candidateGraph.InitializeSkinDeform(*pipelineFactory, *rendererBackend);
    candidateGraph.RenderInitialize(
        *rendererBackend,
        descriptorContext);
}

std::string RenderSystem::GetResolvedShaderGenerationFingerprint() const
{
    std::string fingerprint;
    for (const VL::ResolvedMaterialGroup& materialGroup :
         currentResolvedRenderScene.materialGroups)
    {
        if (!materialGroup.material)
        {
            continue;
        }
        fingerprint += materialGroup.material->GetMaterialKey();
        fingerprint += ":";
        fingerprint +=
            materialGroup.material
                ->GetSurfaceShaderArtifact()
                .artifactGenerationKey;
        if (materialGroup.material->GetShadowShaderArtifact())
        {
            fingerprint += ":";
            fingerprint +=
                materialGroup.material
                    ->GetShadowShaderArtifact()
                    ->artifactGenerationKey;
        }
        fingerprint += ";";
    }
    return fingerprint;
}

uint64_t RenderSystem::GetActiveWorldGeneration() const noexcept
{
    return activeWorld
        ? activeWorld->GetGeneration()
        : 0;
}

std::optional<VL::Editor::Selection::MaterialInstanceSelection>
RenderSystem::PickMaterialInstanceAt(
    float mouseX,
    float mouseY,
    uint32_t viewportWidth,
    uint32_t viewportHeight) const
{
    if (!activeWorld || !hasPickingRenderScene ||
        pickingRenderScene.worldGeneration != activeWorld->GetGeneration())
    {
        return std::nullopt;
    }

    VL::Editor::Selection::ScenePickRequest request;
    request.mouseX = mouseX;
    request.mouseY = mouseY;
    request.viewportWidth = viewportWidth;
    request.viewportHeight = viewportHeight;
    return VL::Editor::Selection::SceneObjectPicker().Pick(
        pickingRenderScene,
        activeWorld->GetScenePath(),
        request);
}

std::vector<VL::Editor::Selection::MaterialInstanceModelContext>
RenderSystem::GetMaterialInstanceModelContexts() const
{
    if (!activeWorld || !hasPickingRenderScene ||
        pickingRenderScene.worldGeneration != activeWorld->GetGeneration())
    {
        return {};
    }

    std::vector<VL::Editor::Selection::MaterialInstanceModelContext> result;
    std::unordered_map<std::string, std::size_t> modelIndices;
    modelIndices.reserve(pickingRenderScene.drawPackets.size());
    for (const VL::RenderDrawPacket& drawPacket : pickingRenderScene.drawPackets)
    {
        if (drawPacket.objectId == 0 || !drawPacket.materialInstance.IsValid())
        {
            continue;
        }
        const std::string objectIdentity = drawPacket.sceneObjectIdentity.empty()
            ? drawPacket.debugName
            : drawPacket.sceneObjectIdentity;
        if (objectIdentity.empty())
        {
            continue;
        }

        const std::string key = std::to_string(drawPacket.objectId) + "|" +
            objectIdentity;
        auto [iterator, inserted] = modelIndices.emplace(key, result.size());
        if (inserted)
        {
            result.push_back(
                VL::Editor::Selection::AggregateSceneModel(
                    pickingRenderScene,
                    activeWorld->GetScenePath(),
                    drawPacket.objectId,
                    objectIdentity));
        }
        static_cast<void>(iterator);
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const auto& left, const auto& right)
        {
            const std::string& leftName = left.displayName.empty()
                ? left.objectIdentity
                : left.displayName;
            const std::string& rightName = right.displayName.empty()
                ? right.objectIdentity
                : right.displayName;
            if (leftName != rightName)
            {
                return leftName < rightName;
            }
            return left.objectIdentity < right.objectIdentity;
        });
    return result;
}

void RenderSystem::SetSelectedMaterialInstanceModel(
    const VL::Editor::Selection::MaterialInstanceModelContext& model)
{
    if (!activeWorld || model.worldGeneration != activeWorld->GetGeneration() ||
        model.objectId == 0)
    {
        ClearSelectedMaterialInstance();
        return;
    }

    hasSelectedDraw = true;
    selectedAllMaterialSlots = true;
    selectedObjectId = model.objectId;
    selectedMaterialSlotIndex = 0;
    selectedWorldGeneration = model.worldGeneration;
}

void RenderSystem::SetSelectedMaterialInstance(
    const VL::Editor::Selection::MaterialInstanceSelection& selection)
{
    if (!activeWorld ||
        selection.worldGeneration != activeWorld->GetGeneration() ||
        selection.objectId == 0)
    {
        ClearSelectedMaterialInstance();
        return;
    }

    hasSelectedDraw = true;
    selectedAllMaterialSlots = false;
    selectedObjectId = selection.objectId;
    selectedMaterialSlotIndex = selection.materialSlotIndex;
    selectedWorldGeneration = selection.worldGeneration;
}

void RenderSystem::ClearSelectedMaterialInstance() noexcept
{
    hasSelectedDraw = false;
    selectedAllMaterialSlots = false;
    selectedObjectId = 0;
    selectedMaterialSlotIndex = 0;
    selectedWorldGeneration = 0;
}

std::unique_ptr<VL::Editor::Preview::IMaterialInstancePreviewAdapter>
RenderSystem::CreateMaterialInstancePreviewAdapter()
{
    VL::Editor::Preview::IRendererMaterialInstancePreviewOwner& owner = *this;
    return std::make_unique<
        VL::Editor::Preview::RendererOwnedMaterialInstancePreviewAdapter>(owner);
}

std::optional<VL::Editor::Preview::MaterialInstancePreviewWorldIdentity>
RenderSystem::GetActiveWorldIdentity() const
{
    if (!activeWorld)
    {
        return std::nullopt;
    }

    VL::Editor::Preview::MaterialInstancePreviewWorldIdentity identity;
    identity.generation = activeWorld->GetGeneration();
    identity.scenePath = activeWorld->GetScenePath();
    return VL::Editor::Preview::NormalizeWorldIdentity(std::move(identity));
}

VL::Editor::Preview::MaterialInstancePreviewAdapterResult
RenderSystem::CaptureMaterialInstancePreviewSession(
    const VL::Editor::Preview::MaterialInstancePreviewAdapterCommand& command,
    std::shared_ptr<
        VL::Editor::Preview::IRendererMaterialInstancePreviewSession>&
        outSession)
{
    using namespace VL::Editor::Preview;

    outSession.reset();
    MaterialInstancePreviewAdapterResult result;
    result.failureStage = PreviewAdapterFailureStage::Prepare;

    if (!command.world.IsValid() ||
        !command.materialInstancePath.IsValid())
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Renderer owner received an invalid World identity or MI path.";
        return result;
    }

    const std::optional<MaterialInstancePreviewWorldIdentity> activeWorldIdentity =
        GetActiveWorldIdentity();
    if (!activeWorldIdentity.has_value())
    {
        result.status = PreviewAdapterOperationStatus::Unavailable;
        result.message =
            "Renderer owner cannot capture a preview target without an active World.";
        return result;
    }
    if (*activeWorldIdentity != command.world)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Renderer owner rejected a preview target from an obsolete World generation.";
        return result;
    }
    if (offlineMode)
    {
        if (!activeOfflineWorldAssets)
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message = "Frozen offline World assets are missing";
            return result;
        }
        const auto found = activeOfflineWorldAssets->materialInstanceSources.find(
            command.materialInstancePath.value);
        if (found == activeOfflineWorldAssets->materialInstanceSources.end())
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message = "Frozen offline MI is missing";
            return result;
        }
        MaterialInstancePreviewDraft baseline;
        baseline.materialInstancePath = command.materialInstancePath.value;
        baseline.documentRevision = command.documentRevision.value_or(0);
        baseline.serializedWorkingDraft = found->second.dump();
        outSession = std::make_shared<OfflineMaterialInstancePreviewSession>(
            *this, command.world, command.materialInstancePath, std::move(baseline));
        result.status = PreviewAdapterOperationStatus::Completed;
        result.runtimeResourceGeneration = command.world.generation;
        result.message = "Frozen offline material preview connected";
        return result;
    }

    const std::optional<
        VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
        capture =
            VL::RendererResourceCache::GetInstance()
                .CaptureMaterialInstanceForGeneration(
                    command.world.generation,
                    command.materialInstancePath.value);
    if (!capture.has_value() || !capture->materialInstance)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Active World-local MaterialInstance was not found for the normalized MI path.";
        return result;
    }

    try
    {
        const MaterialInstanceStateSnapshot snapshot =
            capture->materialInstance->CaptureStateSnapshot();
        if (!activeOfflineWorldAssets)
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message =
                "Renderer owner has no frozen material assets.";
            return result;
        }
        // Connect 的 baseline 与 World 提交时冻结的源一致；磁盘上后来的
        // MI/M_ 改动不能在 Restore 时悄悄混入当前有效版本。
        const nlohmann::json& materialInstanceJson =
            activeOfflineWorldAssets->materialInstanceSources.at(
                command.materialInstancePath.value);
        const MaterialInstanceResolveResult resolved =
            MaterialInstanceResolver::ResolveWithDefinition(
                command.materialInstancePath.value,
                materialInstanceJson,
                activeOfflineWorldAssets->materialDefinitions.at(
                    command.materialInstancePath.value));
        const PreviewRenderStateValues baselineRenderStates =
            BuildPreviewRenderStateValues(
                resolved.effectiveMaterialInstanceJson);
        VL::Editor::Preview::MaterialInstancePreviewDraft baselineDraft;
        baselineDraft.materialInstancePath =
            command.materialInstancePath.value;
        baselineDraft.documentRevision =
            command.documentRevision.value_or(0);
        baselineDraft.serializedWorkingDraft =
            materialInstanceJson.dump();
        outSession = std::make_shared<MaterialInstancePreviewSession>(
            *this,
            command.world,
            command.materialInstancePath,
            capture->ownerGeneration,
            capture->materialInstance,
            snapshot.parameters,
            snapshot.textures,
            baselineRenderStates,
            std::move(baselineDraft));
    }
    catch (const std::exception& exception)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            std::string("Renderer owner failed to capture MI numeric baseline: ") +
            exception.what();
        return result;
    }

    result.status = PreviewAdapterOperationStatus::Completed;
    result.runtimeResourceGeneration = capture->ownerGeneration;
    result.liveSwapCommitted = false;
    result.replacesLiveResource = false;
    result.message =
        "Renderer-owned numeric material instance preview connected.";
    return result;
}

std::shared_ptr<const VL::OfflineWorldAssets>
RenderSystem::PrepareOfflineMaterialPreview(
    const std::string& materialInstancePath,
    const nlohmann::json& materialInstanceJson) const
{
    if (!activeOfflineWorldAssets)
        throw std::runtime_error("Active World has no frozen offline assets");

    const std::string key =
        MaterialAssetUtils::NormalizeAssetPath(materialInstancePath);
    if (activeOfflineWorldAssets->effectiveMaterialInstances.find(key) ==
        activeOfflineWorldAssets->effectiveMaterialInstances.end())
        throw std::runtime_error("Preview MI is absent from the frozen World: " + key);
    // MI 可以覆盖参数，却不能在旧 World 的几何/材质槽下改绑另一份 M_。
    // 这种改绑须经过 World 事务，避免 CPU 与 GPU 候选解释不同的定义。
    const std::string frozenMaterial =
        activeOfflineWorldAssets->materialInstanceSources.at(key)
            .at("material").get<std::string>();
    if (!materialInstanceJson.is_object() ||
        !materialInstanceJson.contains("material") ||
        !materialInstanceJson["material"].is_string() ||
        MaterialAssetUtils::NormalizeAssetPath(
            materialInstanceJson["material"].get<std::string>()) !=
            MaterialAssetUtils::NormalizeAssetPath(frozenMaterial))
        throw std::runtime_error("Preview MI cannot change its frozen material definition: " + key);

    const MaterialInstanceResolveResult resolved =
        MaterialInstanceResolver::ResolveWithDefinition(
            key, materialInstanceJson,
            activeOfflineWorldAssets->materialDefinitions.at(key));
    // 候选只替换一个 MI 的有效值；旧包和正在运行的 CPU 任务始终只读旧版本。
    auto candidate = std::make_shared<VL::OfflineWorldAssets>(
        *activeOfflineWorldAssets);
    candidate->effectiveMaterialInstances.insert_or_assign(
        key, resolved.effectiveMaterialInstanceJson);
    candidate->materialDefinitions.insert_or_assign(key, resolved.materialJson);
    candidate->materialInstanceSources.insert_or_assign(key, materialInstanceJson);
    VL::FreezeOfflineMaterialTextures(*candidate, resolved.effectiveMaterialInstanceJson);

    if (resolved.effectiveMaterialInstanceJson.contains("modelProfile"))
    {
        const std::string reference =
            resolved.effectiveMaterialInstanceJson["modelProfile"].get<std::string>();
        const std::filesystem::path physical = CommonFunction::Path(reference);
        std::ifstream file(physical, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open modelProfile: " + reference);
        const std::string bytes(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>{});
        if (file.bad())
            throw std::runtime_error("Cannot read modelProfile: " + reference);
        candidate->modelProfiles.insert_or_assign(
            reference, nlohmann::json::parse(bytes));
        candidate->modelProfilePaths.insert_or_assign(reference, physical);
        candidate->modelProfileDigests.insert_or_assign(
            reference, VL::ContentHasher::HashString(bytes).ToHex());
    }
    VL::PruneUnusedOfflineMaterialAssets(*candidate);
    return candidate;
}

VL::Editor::Preview::MaterialInstancePreviewAdapterResult
RenderSystem::CommitMaterialInstancePreviewDraft(
    const VL::Editor::Preview::MaterialInstancePreviewWorldIdentity& world,
    const VL::Editor::Preview::NormalizedMaterialInstancePath& materialInstancePath,
    const VL::Editor::Preview::MaterialInstancePreviewDraft& draft)
{
    using namespace VL::Editor::Preview;

    MaterialInstancePreviewAdapterResult result;
    result.failureStage = PreviewAdapterFailureStage::Prepare;
    if (offlineMode)
    {
        try
        {
            const auto draftPath = NormalizeMaterialInstancePath(draft.materialInstancePath);
            if (!world.IsValid() || !materialInstancePath.IsValid() ||
                GetActiveWorldIdentity() != world || !activeOfflineWorldAssets ||
                !draftPath.Succeeded() ||
                *draftPath.path != materialInstancePath)
                throw std::runtime_error("Offline preview targets an obsolete World or MI");
            const auto source = nlohmann::json::parse(draft.serializedWorkingDraft);
            auto candidate = PrepareOfflineMaterialPreview(materialInstancePath.value, source);
            VL::WorldSnapshotBuilder builder;
            VL::WorldSnapshotBuildDesc desc;
            desc.worldGeneration = world.generation;
            auto snapshot = builder.Build(*activeWorld, desc);
            if (snapshot.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(snapshot.Error()));
            auto settings = offlineSettings;
            settings.width = GetSceneRenderExtent().width;
            settings.height = GetSceneRenderExtent().height;
            const auto input = VL::PreflightOfflineRenderInput(snapshot.Value(), *candidate, settings);
            if (input.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(input.Error()));
            for (const auto& [reference, digest] : candidate->modelProfileDigests)
                if (VL::ContentHasher::HashFile(candidate->modelProfilePaths.at(reference)).ToHex() != digest)
                    throw std::runtime_error("modelProfile changed before preview commit: " + reference);
            VL::ValidateOfflineTexturesStillCurrent(*candidate);
            auto published = std::make_shared<VL::OfflineWorldAssets>(*candidate);
            // GPU 仍是旧版本；回切实时必须按此成功 Apply 的冻结值重建。
            published->realtimePrepared = false;
            activeOfflineWorldAssets = std::move(published);
            result.status = PreviewAdapterOperationStatus::Completed;
            result.failureStage = PreviewAdapterFailureStage::None;
            result.runtimeResourceGeneration = world.generation;
            result.liveSwapCommitted = true;
            result.message = "Offline effective preview committed; realtime pending synchronization";
        }
        catch (const std::exception& exception)
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message = exception.what();
        }
        return result;
    }
    if (!world.IsValid() || !materialInstancePath.IsValid())
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Material instance preview replacement received an invalid identity.";
        return result;
    }
    if (rendererBackend == nullptr || pipelineFactory == nullptr)
    {
        result.status = PreviewAdapterOperationStatus::Unavailable;
        result.message =
            "Material instance preview replacement requires initialized renderer services.";
        return result;
    }
    if (!activeWorld || !hasRenderScene ||
        activeWorld->GetGeneration() != world.generation ||
        currentRenderScene.worldGeneration != world.generation)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Material instance preview replacement targets an obsolete or uninitialized World.";
        return result;
    }

    VL::RendererResourceCache& activeCache =
        VL::RendererResourceCache::GetInstance();
    const std::optional<VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
        activeCapture = activeCache.CaptureMaterialInstanceForGeneration(
            world.generation,
            materialInstancePath.value);
    if (!activeCapture.has_value() || !activeCapture->materialInstance)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Active World-local MaterialInstance was not found for the preview replacement.";
        return result;
    }

    NumericPreviewDraftParseResult parsed =
        ParseNumericPreviewDraft(
            draft,
            materialInstancePath,
            activeCapture->materialInstance->CaptureStateSnapshot().textures,
            activeOfflineWorldAssets->materialDefinitions.at(
                materialInstancePath.value));
    if (!parsed.serializedJson.has_value())
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message = std::move(parsed.errorMessage);
        return result;
    }

    size_t targetDrawCount = 0;
    for (const VL::RenderDrawPacket& drawPacket : currentRenderScene.drawPackets)
    {
        if (drawPacket.materialInstance.key == materialInstancePath.value)
        {
            ++targetDrawCount;
        }
    }
    if (targetDrawCount == 0)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "The preview material instance is not referenced by the active RenderScene.";
        return result;
    }

    try
    {
        std::shared_ptr<const VL::OfflineWorldAssets> preparedOfflineAssets =
            PrepareOfflineMaterialPreview(
                materialInstancePath.value, *parsed.serializedJson);
        VL::RendererResourceCache candidateCache =
            activeCache.BeginActiveWorldCandidate(world.generation);
        RenderGraph& renderGraph = RenderGraph::GetInstance();
        PipelineFactory::GraphicsCandidateState graphicsCandidateState;
        VL::RendererResourceLoadContext loadContext{
            candidateCache,
            renderGraph};
        loadContext.graphicsCandidateState = &graphicsCandidateState;
        // GPU 候选与离线候选消费同一份冻结定义和有效覆写，不能再读磁盘 M_。
        auto frozenGpuAssets = std::make_shared<VL::OfflineWorldAssets>(
            *preparedOfflineAssets);
        loadContext.offlineWorldAssets = frozenGpuAssets.get();
        loadContext.useFrozenMaterials = true;
        // MI 预览候选复用主渲染的预蒙皮选择，避免同一材质在预览里切回混合路径。
        loadContext.prerenderSkin = prerenderSkinEnabled;

        VL::RendererMaterialLoader materialLoader(
            *pipelineFactory,
            *rendererBackend,
            loadContext);
        const std::shared_ptr<MaterialInstance> replacementMaterialInstance =
            materialLoader.LoadSceneMaterialInstance(materialInstancePath.value);
        if (!replacementMaterialInstance)
        {
            throw std::runtime_error(
                "Renderer material loader returned an empty preview MaterialInstance");
        }
        const std::shared_ptr<Material> replacementMaterial =
            replacementMaterialInstance->GetBaseMaterial().lock();
        if (!replacementMaterial)
        {
            throw std::runtime_error(
                "Preview MaterialInstance has no replacement base Material");
        }

        VL::RenderScene candidateRenderScene = currentRenderScene;
        ApplyMaterialOverrideToRenderScene(
            candidateRenderScene,
            materialInstancePath.value,
            replacementMaterial->GetMaterialKey());
        RebuildRenderSceneMaterialGroupsImpl(candidateRenderScene);

        auto resolvedSceneResult = resolvedRenderSceneBuilder.Build(
            candidateRenderScene,
            candidateCache);
        if (resolvedSceneResult.IsFailure())
        {
            throw std::runtime_error(
                VL::FormatRuntimeError(resolvedSceneResult.Error()));
        }
        VL::ResolvedRenderScene candidateResolvedRenderScene =
            std::move(resolvedSceneResult.Value());

        const VL::RendererDescriptorContext descriptorContext =
            BuildRendererDescriptorContext(
                candidateCache,
                renderGraph,
                GetLightBufferInfo());
        VL::RendererObjectResourceRegistry objectResourceRegistry;
        objectResourceRegistry.InitializeResolvedSceneResources(
            *rendererBackend,
            descriptorContext,
            candidateRenderScene,
            candidateResolvedRenderScene,
            candidateCache,
            prerenderSkinEnabled);

        PipelineFactory::PreparedGraphicsCandidateCommit preparedPipelineCommit =
            pipelineFactory->PrepareCandidateCommit(graphicsCandidateState);

        const std::string replacementMaterialKey =
            replacementMaterial->GetMaterialKey();
        std::string replacementPath = materialInstancePath.value;
        std::string replacementKey = replacementMaterialKey;
        VL::RenderScene candidatePickingRenderScene = pickingRenderScene;
        ApplyMaterialOverrideToRenderScene(
            candidatePickingRenderScene,
            materialInstancePath.value,
            replacementMaterialKey);
        RebuildRenderSceneMaterialGroupsImpl(candidatePickingRenderScene);

        VL::ResourceRetireQueue& retireQueue =
            VL::ResourceRetireQueue::GetInstance();
        std::vector<VL::RetiredResource> retirementResources;
        retirementResources.reserve(1);
        retirementResources.push_back({
            "MaterialInstancePreview:WorldLocalResources",
            world.generation,
            retireQueue.GetLastSubmittedEpoch(),
            {}});
        VL::PreparedResourceRetirements preparedRetirements =
            retireQueue.PrepareRetirements(std::move(retirementResources));

        for (const auto& [reference, captured] :
             preparedOfflineAssets->modelProfileDigests)
        {
            const auto& path =
                preparedOfflineAssets->modelProfilePaths.at(reference);
            if (!std::filesystem::is_regular_file(path) ||
                VL::ContentHasher::HashFile(path).ToHex() != captured)
                throw std::runtime_error(
                    "modelProfile changed before preview commit: " + reference);
        }
        VL::ValidateOfflineTexturesStillCurrent(*preparedOfflineAssets);
        pipelineFactory->CommitPreparedCandidate(
            std::move(preparedPipelineCommit));
        VL::RendererResourceCache::WorldLocalResourcePackageHandle retiredPackage =
        activeCache.CommitCandidate(std::move(candidateCache));
        std::swap(currentRenderScene, candidateRenderScene);
        std::swap(currentResolvedRenderScene, candidateResolvedRenderScene);
        std::swap(pickingRenderScene, candidatePickingRenderScene);
        activeOfflineWorldAssets.swap(preparedOfflineAssets);

        hasMaterialInstancePreviewMaterialOverride = true;
        materialInstancePreviewOverridePath.swap(replacementPath);
        materialInstancePreviewOverrideMaterialKey.swap(replacementKey);

        preparedRetirements.GetAdditionalResource(0).resource =
            std::static_pointer_cast<void>(std::move(retiredPackage));
        retireQueue.CommitPreparedRetirements(
            std::move(preparedRetirements));

        result.status = PreviewAdapterOperationStatus::Completed;
        result.failureStage = PreviewAdapterFailureStage::None;
        result.runtimeResourceGeneration = world.generation;
        result.liveSwapCommitted = true;
        result.replacesLiveResource = true;
        result.message =
            "Material instance render-state preview applied with a live resource replacement.";
        return result;
    }
    catch (const std::exception& exception)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.failureStage = PreviewAdapterFailureStage::Prepare;
        result.message =
            std::string("Material instance preview resource replacement failed: ") +
            exception.what();
        return result;
    }
}

void RenderSystem::ApplyMaterialInstancePreviewMaterialOverride(
    VL::RenderScene& renderScene) const
{
    if (!hasMaterialInstancePreviewMaterialOverride ||
        materialInstancePreviewOverridePath.empty() ||
        materialInstancePreviewOverrideMaterialKey.empty())
    {
        return;
    }
    ApplyMaterialOverrideToRenderScene(
        renderScene,
        materialInstancePreviewOverridePath,
        materialInstancePreviewOverrideMaterialKey);
}

void RenderSystem::RebuildRenderSceneMaterialGroups(
    VL::RenderScene& renderScene) const
{
    RebuildRenderSceneMaterialGroupsImpl(renderScene);
}

void RenderSystem::UpdateGlobalUBOForPass(vk::CommandBuffer& commandBuffer)
{
    UpdateUBOGlobal(commandBuffer);
}

void RenderSystem::UpdateShadowGlobalUBOForPass(
    vk::CommandBuffer& commandBuffer,
    uint32_t passWidth,
    uint32_t passHeight,
    uint32_t cascadeIndex)
{
    UpdateUBOGlobalForShadow(commandBuffer, passWidth, passHeight, cascadeIndex);
}

void RenderSystem::UpdateMaterialInstanceUBOForPass(
    const std::shared_ptr<MaterialInstance>& materialInstance)
{
    frameResources.UpdateMaterialInstanceUniformBuffer(swapChainImageIndex, *materialInstance);
}

void RenderSystem::UpdateObjectUBOForPass(
    VL::RendererObjectGpuResources& objectResources,
    const VL::RenderDrawPacket& drawPacket)
{
    // Object transforms come from the frozen RenderDrawPacket. The upload path
    // addresses backend-owned object GPU resources through the resolved draw
    // packet.
    const SpeedTreeWindStateGPU* windState =
        windResponseProfiles.FindGpuState(drawPacket.windResponseProfileKey);
    objectResources.submittedSkin.RetainIfSubmittedAt(submittedFrameSerial);
    frameResources.UpdateObjectUniformBuffer(
        swapChainImageIndex,
        objectResources,
        drawPacket,
        windState,
        // 有 pre-skin 输出 buffer 的对象已在帧前上传完整 palette；没有该 buffer
        // 的蒙皮对象仍按原路径逐 draw 上传，避免 flag 与对象资源不一致时漏写。
        !prerenderSkinEnabled || !objectResources.skinDeform.HasResources());
}

void RenderSystem::UploadLightsForPass(
    uint32_t swapChainImageIndex,
    const std::vector<VL::LightSnapshot>& lights)
{
    frameResources.UpdateLightBuffer(swapChainImageIndex, lights);
}

bool RenderSystem::IsCsmEnabled() const
{
    return csmSettings.castShadows;
}

bool RenderSystem::IsShadowCascadeActive(
    uint32_t cascadeIndex) const
{
    return csmSettings.castShadows &&
        cascadeIndex < csmSettings.cascadeCount;
}

bool RenderSystem::GetShadowCascadeLightViewProjection(
    uint32_t cascadeIndex,
    Eigen::Matrix4f& outLightViewProjection) const
{
    // shadowCascadeFrameData 在 UpdateShadowGlobalUBOForPass 里按本帧、本 pass
    // 分辨率构建；这里返回的矩阵与刚写入 shadow UBO 的级联完全同源。
    if (!shadowCascadeFrameData.valid ||
        cascadeIndex >= csmSettings.cascadeCount ||
        cascadeIndex >= VL::CsmSettings::MaxCascadeCount)
    {
        return false;
    }

    const Eigen::Matrix4f& lightViewProjection =
        shadowCascadeFrameData.lightViewProj[cascadeIndex];
    if (!lightViewProjection.allFinite())
    {
        return false;
    }

    outLightViewProjection = lightViewProjection;
    return true;
}

bool RenderSystem::GetShadowCascadeCasterReceiverBounds(
    uint32_t cascadeIndex,
    VL::ShadowCasterReceiverBounds& outBounds) const
{
    if (!shadowCascadeFrameData.valid ||
        cascadeIndex >= csmSettings.cascadeCount ||
        cascadeIndex >= VL::CsmSettings::MaxCascadeCount)
    {
        return false;
    }

    outBounds = shadowCascadeFrameData.casterReceiverBounds[cascadeIndex];
    return outBounds.valid &&
        outBounds.worldToLight.allFinite() &&
        outBounds.lightSpaceXyMinMax.allFinite();
}

void RenderSystem::LogShadowCasterCullStats() const
{
    // 统计的是最近一帧的 draw 数，不是窗口平均：级联覆盖是离散的，
    // 平均值会把“某级联恰好 0 draw”这个关键证据抹平。
    std::ostringstream message;
    message << "shadow caster culling (drawn/candidates):";
    for (uint32_t cascadeIndex = 0;
         cascadeIndex < csmSettings.cascadeCount;
         ++cascadeIndex)
    {
        const VL::ShadowCasterCullStats& stats =
            frameShadowCasterCullStats[cascadeIndex];
        message << " cascade" << cascadeIndex << ' '
                << stats.submittedDraws << '/' << stats.casterDraws;
    }
    const std::string line = message.str();
    VL::ProcessLog().Append(VL::LogSeverity::Info, "GPU", line);
    std::cout << "[Diagnostics][Info] " << line << std::endl;
}

void RenderSystem::DrawEditorGridGeometry(
    vk::CommandBuffer& commandBuffer)
{
    // 管线与描述符集由 pass 流程绑定，这里只提交几何；网格顶点缓冲是静态
    // 世界几何，首次绘制时创建，之后每帧复用同一份数据。
    editorViewportGrid.EnsureCreated(*rendererBackend);
    editorViewportGrid.Bind(commandBuffer);
    editorViewportGrid.Draw(commandBuffer);
}

void RenderSystem::UpdateUBOGlobal(vk::CommandBuffer& commandBuffer)
{
    UpdateUBOGlobalWithProjectionTransform(
        commandBuffer,
        1.0f,
        1.0f,
        0.0f,
        0.0f);
}

void RenderSystem::UpdateUBOGlobalWithProjectionTransform(
    vk::CommandBuffer& commandBuffer,
    float projectionScaleX,
    float projectionScaleY,
    float projectionOffsetX,
    float projectionOffsetY)
{
    PROFILE_FUNCTION();
    if (!hasRenderScene)
    {
        RefreshRenderSceneFromActiveWorld();
    }

    UBOGlobal ubo;
    const VL::CameraSnapshot& camera = currentRenderScene.camera;
    Eigen::Matrix4f projection = camera.projection;
    Eigen::Matrix4f previousViewProjection = camera.previousViewProjection;
    if (!std::isfinite(projectionScaleX) || projectionScaleX <= 0.0f ||
        !std::isfinite(projectionScaleY) || projectionScaleY <= 0.0f ||
        !std::isfinite(projectionOffsetX) ||
        !std::isfinite(projectionOffsetY))
    {
        projectionScaleX = 1.0f;
        projectionScaleY = 1.0f;
        projectionOffsetX = 0.0f;
        projectionOffsetY = 0.0f;
    }
    projection.row(0) =
        projectionScaleX * camera.projection.row(0) +
        projectionOffsetX * camera.projection.row(3);
    projection.row(1) =
        projectionScaleY * camera.projection.row(1) +
        projectionOffsetY * camera.projection.row(3);
    previousViewProjection.row(0) =
        projectionScaleX * camera.previousViewProjection.row(0) +
        projectionOffsetX * camera.previousViewProjection.row(3);
    previousViewProjection.row(1) =
        projectionScaleY * camera.previousViewProjection.row(1) +
        projectionOffsetY * camera.previousViewProjection.row(3);
    ubo.view = camera.view;
    ubo.projection = projection;
    ubo.invView = ubo.view.inverse();
    ubo.invProjection = ubo.projection.inverse();
    ubo.viewProjection = ubo.projection * ubo.view;
    ubo.invViewProjection = ubo.viewProjection.inverse();
    ubo.previousViewProjection = previousViewProjection;
    if (csmSettings.castShadows)
    {
        if (!shadowCascadeFrameData.valid)
        {
            BuildShadowCascadeFrameData(1, 1);
        }
        ubo.lightViewProj = shadowCascadeFrameData.lightViewProj;
        ubo.cascadeSplits = shadowCascadeFrameData.cascadeSplits;
        ubo.shadowBias = shadowCascadeFrameData.bias;
        ubo.csmParameters = shadowCascadeFrameData.csmParameters;
    }
    else
    {
        // Zero cascade splits is the GPU-side disabled flag consumed by
        // CalculateCsmShadow; keep every shadow field explicitly neutral.
        ubo.cascadeSplits = Eigen::Vector4f::Zero();
        ubo.shadowBias.fill(Eigen::Vector4f::Zero());
        ubo.csmParameters = Eigen::Vector4f::Zero();
        for (Eigen::Matrix4f& lightViewProj : ubo.lightViewProj)
        {
            lightViewProj = Eigen::Matrix4f::Identity();
        }
    }
    ubo.cameraPosition = camera.position;
    // environmentSH 由 GPU 在环境代际 commit 时广播，常规 CPU 上传会跳过该区间。
    ubo.debugViewMode = currentRenderScene.debugViewMode;
    ubo.environmentIntensity = currentRenderScene.environment.intensity;
    ubo.environmentType = currentRenderScene.environment.type == VL::EnvironmentType::ProceduralSky ? 1 : 0;
    ubo.skyParameters = currentRenderScene.environment.skyParameters;
    const float cameraExposureScale = VL::ExposureScale(camera.exposure);
    const bool autoExposureEnabled =
        camera.exposure.mode == VL::ExposureMode::Auto;
    std::string exposureMessage;
    (void)SetPassVectorComponent(
        "toneMapping",
        "u_toneMappingParams",
        0,
        cameraExposureScale,
        "Tone mapping pass material instance is expired.",
        exposureMessage);
    (void)SetPassVector(
        "toneMapping",
        "u_exposureSettings",
        Eigen::Vector4f(autoExposureEnabled ? 1.0f : 0.0f, 0, 0, 0),
        exposureMessage);
    // Bloom 阈值属于曝光后空间，与最终合成共用同一冻结相机，输出仍保留场景线性单位。
    (void)SetPassVectorComponent(
        "bloomPrefilter",
        "u_bloomPrefilterParams",
        3,
        cameraExposureScale,
        "Bloom prefilter material instance is expired.",
        exposureMessage);
    (void)SetPassVector(
        "bloomPrefilter",
        "u_exposureSettings",
        Eigen::Vector4f(autoExposureEnabled ? 1.0f : 0.0f, 0, 0, 0),
        exposureMessage);
    frameResources.UpdateGlobalUniformBuffer(commandBuffer, swapChainImageIndex, ubo);
}

void RenderSystem::UpdateExposureFrameTiming()
{
    const auto now = std::chrono::steady_clock::now();
    if (!exposureClockInitialized)
    {
        exposureLastFrameTime = now;
        exposureClockInitialized = true;
        exposureDeltaSeconds = 1.0f / 60.0f;
        // 首帧也发布 CPU 时长，避免 Performance 页在第一个读回窗口前空着 FPS。
        gpuPassTimer.SetCpuFrameTiming(exposureDeltaSeconds);
        return;
    }

    const double elapsed = std::chrono::duration<double>(
        now - exposureLastFrameTime).count();
    exposureLastFrameTime = now;
    // 眼适应不应因断点、窗口拖拽或首帧长暂停瞬间跳到目标值。
    exposureDeltaSeconds = static_cast<float>(
        std::clamp(elapsed, 1.0 / 240.0, 0.1));
    // Performance 通道复用这一个 frame delta，不另建帧率时钟；
    // 因此它和自动曝光看到的 CPU 帧间隔完全一致（含同样的 1/240..0.1 s 收敛区间）。
    gpuPassTimer.SetCpuFrameTiming(exposureDeltaSeconds);
}

void RenderSystem::UpdateUBOGlobalForShadow(
    vk::CommandBuffer& commandBuffer,
    uint32_t passSizeWidth,
    uint32_t passSizeHeight,
    uint32_t cascadeIndex)
{
    PROFILE_FUNCTION();
    if (!hasRenderScene)
    {
        RefreshRenderSceneFromActiveWorld();
    }

    BuildShadowCascadeFrameData(passSizeWidth, passSizeHeight);
    const uint32_t activeCascadeIndex =
        std::min(cascadeIndex, csmSettings.cascadeCount - 1);

    UBOGlobal ubo;
    ubo.view = shadowCascadeFrameData.view[activeCascadeIndex];
    ubo.projection = shadowCascadeFrameData.projection[activeCascadeIndex];
    ubo.invView = ubo.view.inverse();
    ubo.invProjection = ubo.projection.inverse();
    ubo.viewProjection = ubo.projection * ubo.view;
    ubo.invViewProjection = ubo.viewProjection.inverse();
    ubo.previousViewProjection = ubo.viewProjection;
    ubo.lightViewProj = shadowCascadeFrameData.lightViewProj;
    ubo.cascadeSplits = shadowCascadeFrameData.cascadeSplits;
    ubo.shadowBias = shadowCascadeFrameData.bias;
    ubo.csmParameters = shadowCascadeFrameData.csmParameters;
    // environmentSH 由 GPU 在环境代际 commit 时广播，常规 CPU 上传会跳过该区间。
    ubo.debugViewMode = currentRenderScene.debugViewMode;
    ubo.environmentIntensity = currentRenderScene.environment.intensity;
    ubo.environmentType = currentRenderScene.environment.type == VL::EnvironmentType::ProceduralSky ? 1 : 0;
    ubo.skyParameters = currentRenderScene.environment.skyParameters;
    {
        Eigen::Matrix3f rotT = ubo.view.block<3, 3>(0, 0);
        Eigen::Vector3f trans = ubo.view.block<3, 1>(0, 3);
        ubo.cameraPosition = -(rotT.transpose() * trans);
    }

    frameResources.UpdateGlobalUniformBuffer(commandBuffer, swapChainImageIndex, ubo);
}

void RenderSystem::BuildShadowCascadeFrameData(uint32_t passSizeWidth, uint32_t passSizeHeight)
{
    // 如果本帧已经计算过相同场景、相同分辨率的 cascade 数据，直接复用缓存
    if (shadowCascadeFrameData.valid &&
        shadowCascadeFrameData.frameIndex == currentFrame &&
        shadowCascadeFrameData.worldGeneration == currentRenderScene.worldGeneration &&
        shadowCascadeFrameData.width == passSizeWidth &&
        shadowCascadeFrameData.height == passSizeHeight)
    {
        return;
    }

    const VL::CameraSnapshot& camera = currentRenderScene.camera;
    const float cascadeNear = camera.clipNear;
    const float cascadeFar =
        std::max(
            cascadeNear + 0.1f,
            csmSettings.dynamicShadowDistance);

    shadowCascadeFrameData.view.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.projection.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.lightViewProj.fill(Eigen::Matrix4f::Identity());
    shadowCascadeFrameData.cascadeSplits = Eigen::Vector4f::Zero();
    shadowCascadeFrameData.bias.fill(Eigen::Vector4f::Zero());
    // 本帧 caster 的光源空间包围盒要重新投影：角色/头发/风每帧都可能动。
    casterLightSpaceBoundsValid = false;
    shadowCascadeFrameData.csmParameters = Eigen::Vector4f(
        csmSettings.cascadeTransitionFraction,
        csmSettings.shadowDistanceFadeoutFraction,
        static_cast<float>(csmSettings.cascadeCount),
        csmSettings.dynamicShadowDistance);

    float previousSplit = cascadeNear;
    for (uint32_t cascadeIndex = 0; cascadeIndex < csmSettings.cascadeCount; ++cascadeIndex)
    {
        const float p = static_cast<float>(cascadeIndex + 1) / static_cast<float>(csmSettings.cascadeCount);
        // UE 风格 Distribution Exponent：指数越大，越多分辨率集中到相机附近。
        // 最后一级始终精确落在 Dynamic Shadow Distance。
        const float splitFraction =
            std::pow(
                p,
                csmSettings.cascadeDistributionExponent);
        const float splitFar =
            cascadeNear +
            (cascadeFar - cascadeNear) * splitFraction;

        ShadowProjectionParams params = CalculateShadowMatrixForCameraRange(
            previousSplit,
            splitFar,
            passSizeWidth,
            passSizeHeight);
        shadowCascadeFrameData.view[cascadeIndex] = params.viewMatrix;
        shadowCascadeFrameData.projection[cascadeIndex] = params.projectionMatrix;
        shadowCascadeFrameData.lightViewProj[cascadeIndex] = params.projectionMatrix * params.viewMatrix;
        shadowCascadeFrameData.casterReceiverBounds[cascadeIndex] =
            params.casterReceiverBounds;
        shadowCascadeFrameData.cascadeSplits[cascadeIndex] = splitFar;

        // 用户只编辑 UE 风格的全局 Bias。每级实际接收面 Bias 在这里派生，
        // Distribution 为 0 时各级相同，为 1 时按级联序号逐级增大。
        const float cascadeBiasScale =
            1.0f +
            csmSettings.shadowCascadeBiasDistribution *
                static_cast<float>(cascadeIndex);
        shadowCascadeFrameData.bias[cascadeIndex] =
            Eigen::Vector4f(
                csmSettings.shadowBias *
                    VL::CsmSettings::MaxReceiverDepthBias *
                    cascadeBiasScale,
                csmSettings.shadowSlopeBias *
                    VL::CsmSettings::MaxSlopeBiasMultiplier,
                0.0f,
                0.0f);
        previousSplit = splitFar;
    }

    for (uint32_t cascadeIndex = csmSettings.cascadeCount;
         cascadeIndex < VL::CsmSettings::MaxCascadeCount;
         ++cascadeIndex)
    {
        shadowCascadeFrameData.cascadeSplits[cascadeIndex] =
            cascadeFar;
    }

    shadowCascadeFrameData.frameIndex = currentFrame;
    shadowCascadeFrameData.worldGeneration = currentRenderScene.worldGeneration;
    shadowCascadeFrameData.width = passSizeWidth;
    shadowCascadeFrameData.height = passSizeHeight;
    shadowCascadeFrameData.valid = true;
}

RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrixForCameraRange(
    float splitNear,
    float splitFar,
    uint32_t passSizeWidth,
    uint32_t passSizeHeight)
{
    PROFILE_FUNCTION();

    // 根据相机级联范围 (splitNear, splitFar) 计算当前级联的阴影投影矩阵。
    //
    // 流程：
    // 1. 从 camera 构建视锥体 8 个角点（世界空间）
    // 2. 将角点变换到光源空间 (worldToShadowMatrix)
    // 3. 计算光源空间 XY 范围 + Z 深度范围
    // 4. 可选：遍历场景 drawPacket 收紧 Z 范围 (ComputeCascadeLightSpaceZBounds)
    // 5. 根据当前 ShadowStrategy 委托具体算法：
    //    - DynamicTightBox  : 凸包 + 旋转边最小包围矩形
    //    - StableBoundingSphere : 视锥体外接球 + texel snapping
    //    - StableRectangular    : 光源空间 AABB + texel snapping
    const VL::CameraSnapshot& camera = currentRenderScene.camera;
    Eigen::Vector3f cameraPosition = camera.position;
    Eigen::Vector3f cameraDirection = camera.forward;
        // near, far
    float cameraNear = splitNear;
    float cameraFar = splitFar;

    Eigen::Vector3f cameraRight = camera.right;
    Eigen::Vector3f cameraUp = camera.up;
    float cameraHFov = camera.horizontalFovDegrees;
    // 阴影视锥必须与冻结相机投影一致；停靠视口不再采用整窗口宽高比。
    float aspect = std::abs(camera.projection(1, 1) / camera.projection(0, 0));
    float cameraHFovRad = cameraHFov * static_cast<float>(M_PI) / 180.0f;

    float tanHalfFov = std::tan(cameraHFovRad * 0.5f);
    float nearHalfWidth = tanHalfFov * cameraNear;
    float nearHalfHeight = nearHalfWidth / aspect;
    float farHalfWidth = tanHalfFov * cameraFar;
    float farHalfHeight = farHalfWidth / aspect;

    Eigen::Vector3f nearCenter = cameraPosition + cameraDirection * cameraNear;
    Eigen::Vector3f farCenter = cameraPosition + cameraDirection * cameraFar;

    std::vector<Eigen::Vector3f> frustumPoints;
    frustumPoints.reserve(8);
    frustumPoints.emplace_back(nearCenter + cameraUp * nearHalfHeight - cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(nearCenter + cameraUp * nearHalfHeight + cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(nearCenter - cameraUp * nearHalfHeight - cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(nearCenter - cameraUp * nearHalfHeight + cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(farCenter + cameraUp * farHalfHeight - cameraRight * farHalfWidth);
    frustumPoints.emplace_back(farCenter + cameraUp * farHalfHeight + cameraRight * farHalfWidth);
    frustumPoints.emplace_back(farCenter - cameraUp * farHalfHeight - cameraRight * farHalfWidth);
    frustumPoints.emplace_back(farCenter - cameraUp * farHalfHeight + cameraRight * farHalfWidth);
        
    // 2. 计算阴影映射相机的位置
        // 2.1以世界中心为原点，使用光源的旋转矩阵 构建shadowCoordinateSystem
    const VL::LightSnapshot* directionalLight = nullptr;
    for (const VL::LightSnapshot& light : currentRenderScene.lights)
    {
        if (light.type == VL::LightSnapshotType::Directional)
        {
            directionalLight = &light;
            break;
        }
    }
    if (!directionalLight)
    {
        ShadowProjectionParams params;
        params.viewMatrix = Eigen::Matrix4f::Identity();
        params.projectionMatrix = Eigen::Matrix4f::Identity();
        return params;
    }
        // 2.1构建worldCoordinateSystem -> shadowCoordinateSystem
    Eigen::Matrix3f worldToShadowMatrix = directionalLight->worldToLight;

    // 2.3将点转换到shadowCoordinateSystem
    std::vector<Eigen::Vector3f> pointsInShadowSys;
    pointsInShadowSys.reserve(frustumPoints.size());
    for (const auto& point : frustumPoints)
    {
        pointsInShadowSys.emplace_back(worldToShadowMatrix * point);
    }
    
    // ====================================================================================================
    // STABILIZATION LOGIC START
    // ====================================================================================================
    ShadowStrategy strategy = ShadowStrategy::StableBoundingSphere; // Default strategy: StableBoundingSphere (Safest)
    // Note: StableRectangular requires handling non-square shadow maps or viewport resizing to maintain isotropic texel density.
    // Since our physical shadow map texture is likely square, mapping a rectangular projection to it causes stretching/anisotropy.
    
    ShadowProjectionParams params;

    // Calculate Z bounds first as they are needed for all strategies
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float defaultMinZ = std::numeric_limits<float>::max();
    float defaultMaxZ = std::numeric_limits<float>::lowest();
    for(const auto& point : pointsInShadowSys)
    {
        maxX = std::max(maxX, point.x());
        minX = std::min(minX, point.x());
        maxY = std::max(maxY, point.y());
        minY = std::min(minY, point.y());
        defaultMaxZ = std::max(defaultMaxZ, point.z());
        defaultMinZ = std::min(defaultMinZ, point.z());
    }

    float minZ = defaultMinZ;
    float maxZ = defaultMaxZ;
    if (csmSettings.lightSpaceCasterBounds)
    {
        // Performance note: this scans all draw packets once per cascade. Current scenes are small
        // enough for this straightforward path; if profiling shows CPU pressure here, precompute
        // light-space draw bounds once per shadow frame and reuse them across cascades.
        const auto cascadeLightSpaceZBounds = ComputeCascadeLightSpaceZBounds(
            worldToShadowMatrix,
            minX,
            maxX,
            minY,
            maxY);
        if (cascadeLightSpaceZBounds.has_value())
        {
            minZ = cascadeLightSpaceZBounds->first;
            maxZ = cascadeLightSpaceZBounds->second;
        }
    }

    float zNear = 0.0f;
    float zFar = maxZ - minZ;
    if (zFar < 0.1f) zFar = 1.0f;

    switch (strategy)
    {
    case ShadowStrategy::DynamicTightBox:
        params = CalculateShadowMatrix_DynamicTight(pointsInShadowSys, worldToShadowMatrix.block<3, 3>(0, 0), maxZ, zFar);
        break;
    case ShadowStrategy::StableBoundingSphere:
        params = CalculateShadowMatrix_StableSphere(pointsInShadowSys, worldToShadowMatrix.block<3, 3>(0, 0), static_cast<float>(passSizeWidth), maxZ, zFar);
        break;
    case ShadowStrategy::StableRectangular:
        params = CalculateShadowMatrix_StableRectangular(pointsInShadowSys, worldToShadowMatrix.block<3, 3>(0, 0), static_cast<float>(passSizeWidth), maxZ, zFar);
        break;
    }

    // 接收面范围取自相机 split 的 8 个角点，和阴影拟合使用同一份
    // light-space 坐标；caster 预筛不依赖后续按 caster 扩张的 Z 范围。
    params.casterReceiverBounds.worldToLight = worldToShadowMatrix;
    params.casterReceiverBounds.lightSpaceXyMinMax =
        Eigen::Vector4f(minX, minY, maxX, maxY);
    params.casterReceiverBounds.valid = true;

    return params;
}

// ====================================================================================================
// Shadow Strategy Implementations
// ====================================================================================================

// DynamicTightBox：凸包 + 最小包围矩形。对光源空间视锥体的 XY 投影点计算凸包，
// 遍历每条凸包边作为候选方向，在该方向上求 AABB，选取边长最小的方向，得到
// 最小面积包围矩形。然后构建正交投影矩阵。
//
// 算法步骤：
//   ->shadowCoordinateSystem
//       获取凸包点序 (ConvexHull)
//       对每条边建立 EdgeCoordinateSystem (2D 坐标系)
//       ->EdgeCoordinateSystem
//           将凸包点转换到 EdgeCoordinateSystem
//           计算 AABB，获取 maxEdgeLength
//           在循环中找到最小的 maxEdgeLength → 记录 CenterInEdge
//       <-EdgeCoordinateSystem
//       将 CenterInEdge 转回 shadowCoordinateSystem
//       计算 ShadowCameraPosition、ZNear、ZFar
//   <-shadowCoordinateSystem
//   转回世界空间，构建 view/projection 矩阵
RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrix_DynamicTight(
    const std::vector<Eigen::Vector3f>& pointsInShadowSys,
    const Eigen::Matrix3f& worldToShadowRotation,
    float sceneMaxZ, 
    float sceneZRange)
{
    PROFILE_FUNCTION();
    // 1. 进行凸包点查找，获取index
    std::vector<uint32_t> cullPointIndex;
    cullPointIndex = Algorithm::ConvexHull(pointsInShadowSys);
    
    // 2. 根据这些凸包点来计算面积最小的方形区域（等价于边长最小的方形区域）
    float minLength = std::numeric_limits<float>::max();
    Eigen::Vector2f centerInEdgeCoord;
    Eigen::Matrix2f shadowToEdgeCoordMatrix = Eigen::Matrix2f::Identity();
    Eigen::Vector2f EdgeCoordOriginInShadowSys;
    
    // 遍历所有边，将平面坐标系旋转到该边的方向，计算方形的aabb, 求得面积
    // 这样就能找到最小的方形区域
    for (size_t i = 0; i < cullPointIndex.size(); i++)
    {
        // 2.4.1 以p1为中心，p2-p1为方向，构建平面坐标系
            uint32_t p1Idx = cullPointIndex[i];
            uint32_t p2Idx = cullPointIndex[(i + 1) % cullPointIndex.size()];
            Eigen::Vector3f p1 = pointsInShadowSys[p1Idx];
            Eigen::Vector3f p2 = pointsInShadowSys[p2Idx];

            Eigen::Vector2f edge(p2.x() - p1.x(), p2.y() - p1.y());
            if (edge.norm() < 1e-6) continue;
            edge.normalize();

            // R = [ c  -s ]  这里是左乘
            //     [ s c ]
            float c = edge.x();
            float s = edge.y();
            Eigen::Matrix2f _shadowToEdgeCoordMatrix;
            _shadowToEdgeCoordMatrix << c, -s,
                        s, c;
            // 2.4.2 求取该平面变换后的aabb
            float minX = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float minY = std::numeric_limits<float>::max();
            float maxY = std::numeric_limits<float>::lowest();

            for (auto idx : cullPointIndex)
            {
                Eigen::Vector2f rotatedP = _shadowToEdgeCoordMatrix * (Eigen::Vector2f(pointsInShadowSys[idx].x(), pointsInShadowSys[idx].y()) - Eigen::Vector2f(p1.x(), p1.y()));
                minX = std::min(minX, rotatedP.x());
                maxX = std::max(maxX, rotatedP.x());
                minY = std::min(minY, rotatedP.y());
                maxY = std::max(maxY, rotatedP.y());
            }
            float width = std::abs(maxX - minX);
            float height = std::abs(maxY - minY);
            float length = std::max(width, height);
            // 2.4.3 有最小的length, 则找到了最小的方形区域
            if(length < minLength)
            {
                minLength = length;
                centerInEdgeCoord = Eigen::Vector2f((minX + maxX) / 2.0f, (minY + maxY) / 2.0f);
                shadowToEdgeCoordMatrix = _shadowToEdgeCoordMatrix;
                EdgeCoordOriginInShadowSys = Eigen::Vector2f(p1.x(), p1.y());
            }
    }
    
    // 3. 计算中心点的位置
    // x, y 在shadowCoordinateSystem中的值
    Eigen::Vector2f center2DInShadowSys = shadowToEdgeCoordMatrix.transpose() * centerInEdgeCoord + EdgeCoordOriginInShadowSys;

    Eigen::Vector3f ShadowCameraPositionInShadowSys = Eigen::Vector3f(center2DInShadowSys.x(), center2DInShadowSys.y(), sceneMaxZ);
    
    // 4. 计算shadowCamera的位置 (World Space)。pointsInShadowSys 已经在 light-aligned shadow space，
    // worldToShadowRotation 用于把拟合出的 shadow-space camera 位置和方向转回世界空间。
    Eigen::Vector3f shadowCameraPosition = worldToShadowRotation.transpose() * ShadowCameraPositionInShadowSys;

    // 更新Camera
    Camera shadowCamera;
    shadowCamera.SetCamera(shadowCameraPosition, CommonFunction::QuatToRotation(Eigen::Quaternionf(worldToShadowRotation.transpose())));

    // 计算Near/Far
    float nearPlane = 0.0f;
    float farPlane = sceneZRange;

    shadowCamera.SetOrthographic(
        minLength, 
        1.0f, // Aspect ratio
        nearPlane, 
        farPlane);

    RenderSystem::ShadowProjectionParams params;
    
    // Apply the extra rotation (rollMatrix) to the View Matrix
    Eigen::Matrix4f rollMatrix = Eigen::Matrix4f::Identity();
    rollMatrix(0, 0) = shadowToEdgeCoordMatrix(0, 0);
    rollMatrix(0, 1) = shadowToEdgeCoordMatrix(0, 1);
    rollMatrix(1, 0) = shadowToEdgeCoordMatrix(1, 0);
    rollMatrix(1, 1) = shadowToEdgeCoordMatrix(1, 1);
    
    params.viewMatrix = rollMatrix * shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrix_StableSphere(
    const std::vector<Eigen::Vector3f>& pointsInShadowSys,
    const Eigen::Matrix3f& worldToShadowRotation, 
    float shadowMapResolution, 
    float sceneMaxZ, 
    float sceneZRange)
{
    PROFILE_FUNCTION();
    // 1. Calculate Frustum Center
    Eigen::Vector3f frustumCenter = Eigen::Vector3f::Zero();
    for(const auto& p : pointsInShadowSys) {
        frustumCenter += p;
    }
    frustumCenter /= static_cast<float>(pointsInShadowSys.size());

    // 2. Calculate Radius
    float radius = 0.0f;
    for(const auto& p : pointsInShadowSys) {
        radius = std::max(radius, (p - frustumCenter).norm());
    }
    radius = std::ceil(radius * 16.0f) / 16.0f;

    // 3. Snapping
    float worldUnitsPerTexel = (2.0f * radius) / shadowMapResolution;
    frustumCenter.x() = std::floor(frustumCenter.x() / worldUnitsPerTexel) * worldUnitsPerTexel;
    frustumCenter.y() = std::floor(frustumCenter.y() / worldUnitsPerTexel) * worldUnitsPerTexel;

    // 4. Construct Camera
    // Camera placed at the center of the sphere in X/Y, but at sceneMaxZ in Z
    Eigen::Vector3f shadowCameraPosition = frustumCenter;
    shadowCameraPosition.z() = sceneMaxZ;
    
    // Transform back to World Space for Camera.SetCamera
    Eigen::Vector3f shadowCameraPositionWorld = worldToShadowRotation.transpose() * shadowCameraPosition;
    
    Camera shadowCamera;
    shadowCamera.SetCamera(shadowCameraPositionWorld, CommonFunction::QuatToRotation(Eigen::Quaternionf(worldToShadowRotation.transpose()))); // Use Light Rotation

    // 5. Construct Projection
    shadowCamera.SetOrthographic(
        radius * 2.0f, 
        1.0f, // Aspect ratio is 1.0
        0.0f, // Near plane relative to camera
        sceneZRange // Far plane
    );

    RenderSystem::ShadowProjectionParams params;
    params.viewMatrix = shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrix_StableRectangular(
    const std::vector<Eigen::Vector3f>& pointsInShadowSys,
    const Eigen::Matrix3f& worldToShadowRotation, 
    float shadowMapResolution, 
    float sceneMaxZ, 
    float sceneZRange)
{
    PROFILE_FUNCTION();
    // 1. Calculate AABB of Frustum Slice in Shadow Space
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();

    for(const auto& p : pointsInShadowSys) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }

    // StableRectangular keeps a tight light-space AABB and snaps its edges to
    // texel units. It is less rotation-stable than the sphere path, but uses
    // more of the shadow map for the current frustum.
    Eigen::Vector3f diagonal = pointsInShadowSys[6] - pointsInShadowSys[0];
    float diagonalLength = diagonal.norm();
    float worldUnitsPerTexel = diagonalLength / shadowMapResolution;
    
    minX = std::floor(minX / worldUnitsPerTexel) * worldUnitsPerTexel;
    maxX = std::floor(maxX / worldUnitsPerTexel) * worldUnitsPerTexel;
    minY = std::floor(minY / worldUnitsPerTexel) * worldUnitsPerTexel;
    maxY = std::floor(maxY / worldUnitsPerTexel) * worldUnitsPerTexel;
    
    float width = maxX - minX;
    float height = maxY - minY;
    float centerX = (minX + maxX) * 0.5f;
    float centerY = (minY + maxY) * 0.5f;

    // 3. Construct Camera
    Eigen::Vector3f shadowCameraPosition = Eigen::Vector3f(centerX, centerY, sceneMaxZ);
    Eigen::Vector3f shadowCameraPositionWorld = worldToShadowRotation.transpose() * shadowCameraPosition;

    Camera shadowCamera;
    shadowCamera.SetCamera(shadowCameraPositionWorld, CommonFunction::QuatToRotation(Eigen::Quaternionf(worldToShadowRotation.transpose())));

    float aspect = width / height;

    // Camera::SetOrthographic uses horizontal coverage when paired with
    // aspect = width / height, matching the fitted light-space AABB.
    shadowCamera.SetOrthographic(
        width,
        aspect,
        0.0f,
        sceneZRange
    );

    RenderSystem::ShadowProjectionParams params;
    params.viewMatrix = shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

void RenderSystem::RefreshRenderSceneFromActiveWorld()
{
    PublishSnapshotFromActiveWorld(false);
    if (!ConsumeLatestSnapshotIntoRenderScene())
    {
        throw std::runtime_error("WorldSnapshotQueue did not return the snapshot just published");
    }
}

void RenderSystem::PublishSnapshotFromActiveWorld()
{
    PublishSnapshotFromActiveWorld(true);
}

void RenderSystem::PublishSnapshotFromActiveWorld(bool advanceSimulation)
{
    if (!activeWorld)
    {
        throw std::runtime_error("RenderSystem active World is not set");
    }

    VL::WorldSnapshotBuildDesc buildDesc;
    buildDesc.worldGeneration = activeWorld->GetGeneration();
    buildDesc.frameIndex = nextSnapshotFrameIndex++;
    // resize/图重建只刷新姿态，不拥有新的 GT 时间片；普通帧成功发布后
    // 领取一次 delta，避免组合 Render 或重复发布用新 frameIndex 重放旧时间。
    if (advanceSimulation)
    {
        buildDesc.simulationDeltaSeconds = simulationDeltaSeconds;
        buildDesc.gameSimulationDeltaSeconds = gameSimulationDeltaSeconds;
        buildDesc.hairSimulationDeltaSeconds = hairSimulationDeltaSeconds;
    }
    buildDesc.debugViewMode = debugViewMode;
    buildDesc.environmentIntensity = environmentIntensity;
    buildDesc.hasSelectedDraw =
        hasSelectedDraw && selectedWorldGeneration == activeWorld->GetGeneration();
    buildDesc.selectedAllMaterialSlots =
        selectedAllMaterialSlots &&
        selectedWorldGeneration == activeWorld->GetGeneration();
    buildDesc.selectedObjectId = selectedObjectId;
    buildDesc.selectedMaterialSlotIndex = selectedMaterialSlotIndex;

    auto snapshotResult = worldSnapshotBuilder.Build(*activeWorld, buildDesc);
    if (snapshotResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(snapshotResult.Error()));
    }

    VL::WorldSnapshot snapshot = std::move(snapshotResult.Value());
    currentOfflineSnapshot = std::make_shared<const VL::WorldSnapshot>(snapshot);
    if (offlineMode && offlineCoordinator)
    {
        offlineSettings.width = GetSceneRenderExtent().width;
        offlineSettings.height = GetSceneRenderExtent().height;
        const auto target = offlineCameraMotionPolicy.Decide(
            snapshot.camera, snapshot.worldGeneration,
            activeOfflineWorldAssets.get(), offlineSettings,
            offlineCoordinator->State(),
            VL::OfflineCameraMotionPolicy::Clock::now());
        if (target)
            offlineCoordinator->Observe(currentOfflineSnapshot,
                activeOfflineWorldAssets, *target);
    }
    auto pickingRenderSceneResult = rendererFrontend.BuildRenderScene(snapshot);
    if (pickingRenderSceneResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(pickingRenderSceneResult.Error()));
    }
    pickingRenderScene = std::move(pickingRenderSceneResult.Value());
    ApplyMaterialInstancePreviewMaterialOverride(pickingRenderScene);
    RebuildRenderSceneMaterialGroups(pickingRenderScene);
    hasPickingRenderScene = true;
    worldSnapshotQueue.Publish(std::move(snapshot));
    if (advanceSimulation)
    {
        simulationDeltaSeconds = 0.0f;
        gameSimulationDeltaSeconds = 0.0f;
        hairSimulationDeltaSeconds = 0.0f;
    }
}

bool RenderSystem::ConsumeLatestSnapshotIntoRenderScene()
{
    auto snapshot = worldSnapshotQueue.ConsumeLatest();
    if (!snapshot.has_value())
    {
        return false;
    }

    auto renderSceneResult = rendererFrontend.BuildRenderScene(**snapshot);
    if (renderSceneResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(renderSceneResult.Error()));
    }

    currentRenderScene = std::move(renderSceneResult.Value());
    ApplyMaterialInstancePreviewMaterialOverride(currentRenderScene);
    RebuildRenderSceneMaterialGroups(currentRenderScene);
    hasRenderScene = true;
    return true;
}

void RenderSystem::RenderLatestSnapshotOrLastGood()
{
    PROFILE_SCOPE("RenderSystem::RenderLatestSnapshotOrLastGood");
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem renderer backend is not set");
    }

    const bool consumedSnapshot = ConsumeLatestSnapshotIntoRenderScene();
    if (consumedSnapshot)
    {
        if (!offlineMode) BuildResolvedRenderScene();
        if (!offlineMode && currentRenderScene.worldGeneration != initializedRenderWorldGeneration)
        {
            InitializeCurrentRenderSceneResources();
        }
        else if (!offlineMode && currentResolvedRenderScene.NeedsObjectResources())
        {
            // 同一 World 的新对象（Spawn、重新显示）也有独立模型 UBO/descriptor。
            // 只补缺失的逐对象包；已有 entry 的 Initialize 是幂等的，不重建图或环境，
            // 不覆盖 GPU 正在读取的旧包，仍由 World-local cache 随 epoch 退休。
            VL::RendererObjectResourceRegistry registry;
            registry.InitializeResolvedSceneResources(*rendererBackend, BuildRendererDescriptorContext(),
                currentRenderScene, currentResolvedRenderScene, VL::RendererResourceCache::GetInstance(),
                prerenderSkinEnabled);
        }
    }
    else if (!hasRenderScene)
    {
        throw std::runtime_error("RenderSystem has no RenderScene to render");
    }

    RecordAndSubmitCurrentRenderScene();
}

void RenderSystem::BuildResolvedRenderScene()
{
    const auto& resourceCache = VL::RendererResourceCache::GetInstance();
    for (auto& draw : currentRenderScene.drawPackets)
    {
        const auto* instance = resourceCache.GetMaterialInstance(draw.materialInstance.key);
        if (instance && *instance)
            draw.material.key = (*instance)->GetBaseMaterial().lock()->GetMaterialKey();
    }
    RebuildRenderSceneMaterialGroups(currentRenderScene);
    // 放置级（per-placement）对象必须在 resolve 之前拥有自己的 RenderableObject，
    // 否则 ResolvedRenderSceneBuilder 会因为 cache 缺键整帧失败。
    EnsurePlacementRenderables(currentRenderScene, VL::RendererResourceCache::GetInstance());
    PrunePlacementRenderables(currentRenderScene);
    auto resolvedSceneResult = resolvedRenderSceneBuilder.Build(currentRenderScene, resourceCache);
    if (resolvedSceneResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(resolvedSceneResult.Error()));
    }

    currentResolvedRenderScene = std::move(resolvedSceneResult.Value());
}

void RenderSystem::EnsurePlacementRenderables(
    const VL::RenderScene& renderScene, VL::RendererResourceCache& resourceCache)
{
    // World 换代时旧键的帧计数没有意义；整包由 transaction 退休，这里只重置跟踪。
    if (renderScene.worldGeneration != placementWorldGeneration)
    {
        placementAbsentFrames.clear();
        placementObjectNames.clear();
        placementLiveCount = 0;
        placementCacheCount = 0;
        placementRuntimeCount = 0;
        placementBindingCache.clear();
        placementWorldGeneration = renderScene.worldGeneration;
    }
    for (const VL::RenderDrawPacket& draw : renderScene.drawPackets)
    {
        if (draw.placementTemplateMeshKey.empty())
            continue;
        if (draw.mesh.key.empty())
            throw std::runtime_error(
                "Placement-scoped draw packet has no instance mesh key: " + draw.debugName);
        const std::shared_ptr<RenderableObject>* existing =
            resourceCache.GetRenderableObject(draw.mesh.key);
        if (existing != nullptr && *existing != nullptr)
            continue;
        const std::shared_ptr<RenderableObject>* templateObject =
            resourceCache.GetRenderableObject(draw.placementTemplateMeshKey);
        if (templateObject == nullptr || *templateObject == nullptr)
            throw std::runtime_error(
                "Placement template renderable is missing: template=" +
                draw.placementTemplateMeshKey + " instance=" + draw.mesh.key);
        // 模板未解析绑定时（例如异步预加载的蓝图资产），解析需要来源 draw 的骨骼
        // 资产。按 sceneObjectIdentity 在本帧快照里找来源对象；缺失交给 handler 报错。
        const RenderableObject* sourceObject = nullptr;
        if (!draw.placementBinding.sourceSceneObject.empty())
            for (const VL::RenderDrawPacket& candidate : renderScene.drawPackets)
            {
                if (candidate.sceneObjectIdentity !=
                    draw.placementBinding.sourceSceneObject) continue;
                const std::shared_ptr<RenderableObject>* found =
                    resourceCache.GetRenderableObject(candidate.mesh.key);
                if (found != nullptr && *found != nullptr) sourceObject = found->get();
                break;
            }
        // 克隆本身不感知类型；绑定（毛发骨骼等）由通用 resolver 按 Kind 分派。
        std::shared_ptr<RenderableObject> clone =
            (*templateObject)->CloneForPlacement(draw.debugName,
                draw.placementBinding.tressfx ? &*draw.placementBinding.tressfx : nullptr);
        std::string bindingError;
        if (!VL::ApplyPlacementBinding(draw.placementBinding, **templateObject,
                sourceObject, *clone, placementBindingCache, bindingError))
            throw std::runtime_error(bindingError + " instance=" + draw.mesh.key);
        resourceCache.BindRenderableObject(draw.mesh.key, clone);
        placementAbsentFrames[draw.mesh.key] = 0;
        placementObjectNames[draw.mesh.key] = draw.debugName;
        std::cout << "[Placement] instance renderable: object=" << draw.debugName
                  << " template=" << draw.placementTemplateMeshKey
                  << " kind=" << static_cast<int>(draw.placementBinding.kind) << std::endl;
    }
}

void RenderSystem::PrunePlacementRenderables(const VL::RenderScene& renderScene)
{
    if (renderScene.worldGeneration != placementWorldGeneration)
    {
        // 换代由 World package 统一退休；旧键的帧计数没有意义。
        placementAbsentFrames.clear();
        placementObjectNames.clear();
        placementLiveCount = 0;
        placementCacheCount = 0;
        placementWorldGeneration = renderScene.worldGeneration;
        return;
    }
    std::set<std::string> live;
    for (const VL::RenderDrawPacket& draw : renderScene.drawPackets)
        if (!draw.placementTemplateMeshKey.empty())
            live.insert(draw.mesh.key);
    // 只在存活数变化时打一行：spawn 后 1、destroy 后 0，同时作为无泄漏计数。
    if (live.size() != placementLiveCount)
    {
        placementLiveCount = live.size();
        std::cout << "[Placement] live placement renderables: " << placementLiveCount
                  << std::endl;
    }
    // 活动包里实际持有的 placement-scoped renderable 数；包级释放后必须回到 0。
    const size_t cacheCount =
        VL::RendererResourceCache::GetInstance().CountPlacementScopedRenderableObjects();
    if (cacheCount != placementCacheCount)
    {
        placementCacheCount = cacheCount;
        std::cout << "[Placement] placement-scoped renderables in cache: " << cacheCount
                  << std::endl;
    }
    std::vector<std::string> retired;
    for (auto it = placementAbsentFrames.begin(); it != placementAbsentFrames.end();)
    {
        if (live.count(it->first) != 0)
        {
            // 本帧还在快照里：清零缺失计数。不能删除跟踪项，否则对象第一次存活
            // 就会失去跟踪，销毁时永远不会退休（泄漏）。
            it->second = 0;
            ++it;
            continue;
        }
        if (++it->second >= 2)
            retired.push_back(it->first);
        ++it;
    }
    if (!retired.empty())
    {
        const uint64_t lastSubmittedEpoch =
            VL::ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch();
        for (const std::string& key : retired)
        {
            placementAbsentFrames.erase(key);
            const auto nameIt = placementObjectNames.find(key);
            const std::string objectName =
                nameIt != placementObjectNames.end() ? nameIt->second : std::string("<unknown>");
            std::shared_ptr<RenderableObject> object =
                VL::RendererResourceCache::GetInstance().TakeRenderableObject(key);
            if (object == nullptr)
            {
                // 蓝图资产释放路径可能已经先摘走了这个 renderable；这里只记录事实，
                // 不能重复退休同一个键。
                std::cout << "[Placement] instance renderable released by package: object="
                          << objectName << " key=" << key << std::endl;
                placementObjectNames.erase(key);
                continue;
            }
            // 上一帧可能还在 GPU 上引用它；按已提交 epoch 交给统一退休队列。
            VL::ResourceRetireQueue::GetInstance().RetireShared(
                key, renderScene.worldGeneration, lastSubmittedEpoch, std::move(object));
            placementObjectNames.erase(key);
            std::cout << "[Placement] instance renderable retired: object=" << objectName
                      << " key=" << key << std::endl;
        }
    }
    // 由 Ensure 创建、尚未退休的运行时放置 renderable 数；destroy 后必须回到 0。
    if (placementObjectNames.size() != placementRuntimeCount)
    {
        placementRuntimeCount = placementObjectNames.size();
        std::cout << "[Placement] runtime placement renderables: "
                  << placementRuntimeCount << std::endl;
    }
}

void RenderSystem::InitializeCurrentRenderSceneResources()
{
    frameResources.EnsureLightCapacity(currentRenderScene.lights.size(), *rendererBackend);
    environmentUpdateState.Reset();
    environmentUpdateScheduler.Reset();
    environmentUpdateSourceCube.reset();
    RefreshEnvironmentUpdateDiagnostics();

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    VL::RendererResourceCache& resourceCache = VL::RendererResourceCache::GetInstance();
    
    PrepareEnvironmentResources(); // 环境光照的资源准备

    RenderGraph::GetInstance().RefreshRuntimeDescriptors(*rendererBackend, descriptorContext);

    VL::RendererObjectResourceRegistry objectResourceRegistry;
    objectResourceRegistry.InitializeResolvedSceneResources(
        *rendererBackend,
        descriptorContext,
        currentRenderScene,
        currentResolvedRenderScene,
        resourceCache,
        prerenderSkinEnabled);

    initializedRenderWorldGeneration = currentRenderScene.worldGeneration;
}

void RenderSystem::RecordAndSubmitCurrentRenderScene()
{
    const RenderGraph& renderGraph = RenderGraph::GetInstance();

    if (uiRenderSnapshotQueue != nullptr)
    {
        std::shared_ptr<const VL::UiRenderSnapshot> latestUiSnapshot =
            uiRenderSnapshotQueue->ConsumeLatest();
        if (latestUiSnapshot != nullptr)
        {
            currentUiRenderSnapshot = std::move(latestUiSnapshot);
            uiOverlayRenderer.SynchronizeTextures(*currentUiRenderSnapshot);
        }
    }

    // 统计 query pool 在 backend BeginFrame 之前创建：此时本帧命令缓冲尚未开始录制。
    // 未开启采集时这里只做两次原子读取，不创建任何 Vulkan 对象。
    if (!offlineMode)
    {
        gpuPassTimer.PrepareFrame(currentFrame % MAX_FRAMES_IN_FLIGHT);
    }
    // Backend owns swapchain frame synchronization. RenderSystem only records
    // pass work into the command buffer it receives for this image.
    VL::RendererFrameContext frameContext = rendererBackend->BeginFrame(currentFrame);
    if (!frameContext.ready)
    {
        // acquire 失败没有提交；保留帧号和截图请求，等待 GT 重建 swapchain。
        return;
    }
    // 相机历史同样绑定真实提交，不能使用可能未被渲染的 GT 快照历史。
    currentRenderScene.camera.previousViewProjection =
        submittedViewProjection && submittedCameraGeneration == currentRenderScene.worldGeneration
        ? *submittedViewProjection : currentRenderScene.camera.viewProjection;
    uiOverlayRenderer.CollectRetiredTextures();
    // 每帧重新累计：诊断输出只描述刚刚录制完的这一帧。
    frameShadowCasterCullStats = {};
    uint32_t frameIndex = frameContext.frameIndex;
    swapChainImageIndex = frameContext.swapchainImageIndex;
    vk::CommandBuffer commandBuffer = frameContext.commandBuffer;
    const bool renderToViewport = currentUiRenderSnapshot && currentUiRenderSnapshot->sceneViewportEnabled;
    const RenderResource* viewportOutput = renderToViewport
        ? renderGraph.GetSceneViewportOutput(swapChainImageIndex) : nullptr;
    if (renderToViewport && viewportOutput == nullptr)
        throw std::runtime_error("Editor scene viewport has no graph-owned output target.");
    // 自动曝光按帧推进：它读上一帧的直方图并产出本帧的 EV，因此必须在录制之前更新。
    UpdateExposureFrameTiming();

    {
        VulkanDebug::ScopedRegion frameRegion(
            commandBuffer,
            "Frame:" + std::to_string(frameIndex) + " Image:" + std::to_string(swapChainImageIndex),
            VulkanDebug::DebugCategory::eDefault);

        // Tick every tree species once so shadow and geometry consume the same
        // per-object wind state for the complete frame.
        if (offlineMode)
        {
            const auto result = CaptureOfflineResult();
            const auto preview = offlineCoordinator
                ? offlineCoordinator->CapturePreview() : nullptr;
            // RT 只消费冻结的完整 film 或视口专用部分预览；导出接口不见预览。
            offlineDisplay.Record(commandBuffer, swapChainImageIndex,
                result ? result->film : nullptr, preview, offlineAov,
                // 曝光取 master 的解析结果（手动/Auto 两模式），视口三个参数保留我方的：
                // 编辑器视口输出与 graph 代数是本分支引入的，master 侧没有。
                ResolveOfflineDisplayExposure(
                    result.get(),
                    offlineExposure,
                    currentRenderScene.camera),
                viewportOutput, renderGraph.GetOwnerGeneration(), GetSceneRenderExtent());
        }
        else
        {
        // 读回发生在这里：backend BeginFrame 已等待本 frame slot 的 fence，上一批查询必然就绪。
        // 随后在命令缓冲里重置该槽位的查询范围，早于本帧任何被计时命令。
        gpuPassTimer.BeginFrame(commandBuffer, frameIndex);
        AdvanceSceneWind();
        // Procedural sky IBL needs the latest camera and sky parameters in global UBO,
        // but environmentSH is GPU-owned and must not be overwritten by this upload.
        UpdateUBOGlobal(commandBuffer);
        // Generate the procedural sky cubemap and write environmentSH before graphics
        // passes read global lighting data.
        RecordEnvironmentIbl(commandBuffer, swapChainImageIndex);
        // guide 模拟必须在任何 pass 读取 current positions 之前完成本帧 dispatch。
        // 只有本帧确实在采集时才做 HasWork 扫描；关闭 --gpu-stats 且未执行 gpustats
        // 时，统计路径对每帧的正常渲染只增加一次原子读取和一次分支。
        const auto& hairSimulation = renderGraph.GetHairSimulation();
        const bool hairWillRecord =
            gpuPassTimer.IsFrameActive() &&
            hairSimulation &&
            hairSimulation->HasWork(currentResolvedRenderScene, currentRenderScene);
        const bool hairTimed = hairWillRecord &&
            gpuPassTimer.BeginPass(commandBuffer, "hairSimulation");
        RecordHairSimulation(commandBuffer, frameIndex);
        if (hairWillRecord)
        {
            gpuPassTimer.EndPass(commandBuffer, hairTimed);
        }

        // GPU 预蒙皮必须早于任何 Shadow/Geometry pass：本帧 palette 先在 CPU 侧
        // 写入 image 槽，compute 再一次性生成顶点结果，后续顶点着色器只读。
        const auto& skinDeform = renderGraph.GetSkinDeform();
        const bool skinDeformWillRecord =
            gpuPassTimer.IsFrameActive() &&
            skinDeform &&
            skinDeform->HasWork(currentResolvedRenderScene);
        const bool skinDeformTimed = skinDeformWillRecord &&
            gpuPassTimer.BeginPass(commandBuffer, "skinnedDeform");
        RecordSkinDeform(commandBuffer, swapChainImageIndex);
        if (skinDeformWillRecord)
        {
            gpuPassTimer.EndPass(commandBuffer, skinDeformTimed);
        }

        const auto& renderPassOrdered = renderGraph.GetRenderpassesOrdered();
        for (size_t passIndex = 0; passIndex < renderPassOrdered.size(); ++passIndex)
        {
            const auto& renderPassName = renderPassOrdered[passIndex];
            std::string renderPassScopeName = "RenderPass:" + renderPassName;
            PROFILE_SCOPE(renderPassScopeName.c_str());
            const auto& renderPass = renderGraph.GetRenderpasses().at(renderPassName);
            if (currentRenderScene.environment.type == VL::EnvironmentType::None)
            {
                const auto instance = renderPass.materialInstance.lock();
                const auto material = instance ? instance->GetBaseMaterial().lock() : nullptr;
                if (material && material->GetShaderVariantKey().shaderName == "pass/sky")
                    continue;
            }

            // 参考网格是编辑器视口的显示辅助：视口不可见（运行页面、无 UI、资源编辑器
            // 全屏）时整个 pass 不录制，运行时画面与截图保持与接入前一致。
            if (renderPass.type == VL::RenderGraphPassType::EditorGrid &&
                !renderToViewport)
            {
                continue;
            }

            // pass 本身可能开启 render pass 实例，因此 timestamp 必须在实例外 bracket：
            // BeginPass 在 ScopedRegion 之前写 begin timestamp，EndPass 在录制完成后写
            // end timestamp；两端都在 pass 实例外，因此不受 render pass 内部阶段限制。
            const bool passTimed =
                gpuPassTimer.BeginPass(commandBuffer, renderPassName);

            VulkanDebug::ScopedRegion passRegion(
                commandBuffer,
                renderPassName,
                VulkanDebug::DebugCategory::ePass);

            if (renderPass.type == VL::RenderGraphPassType::AutoExposure)
            {
                PassBarrier::PrepareForPass(commandBuffer, renderGraph, passIndex, swapChainImageIndex);
                renderGraph.GetAutoExposure()->Record(commandBuffer, swapChainImageIndex,
                    currentRenderScene.camera.exposure, exposureDeltaSeconds);
                gpuPassTimer.EndPass(commandBuffer, passTimed);
                continue;
            }
            VL::PassRuntimeContext passContext{
                commandBuffer,
                renderPass,
                renderGraph,
                passIndex,
                swapChainImageIndex,
                currentRenderScene,
                currentResolvedRenderScene,
                *this,
                renderToViewport
            };
            // 阴影 pass 把 before/after draw 数写回这里；非阴影 pass 不接收统计指针。
            VL::ShadowCasterCullStats shadowCasterCullStats;
            shadowCasterCullStats.cascadeIndex = renderPass.shadowCascadeIndex;
            if (renderPass.type == VL::RenderGraphPassType::Shadow)
            {
                passContext.shadowCasterCullStats = &shadowCasterCullStats;
            }
            passRuntime.RecordPass(renderPassName, passContext);
            if (renderPass.type == VL::RenderGraphPassType::Shadow &&
                shadowCasterCullStats.cascadeIndex <
                    VL::CsmSettings::MaxCascadeCount)
            {
                frameShadowCasterCullStats[shadowCasterCullStats.cascadeIndex] =
                    shadowCasterCullStats;
            }
            gpuPassTimer.EndPass(commandBuffer, passTimed);
        }

        // 与 GPU 表同周期（120 帧）输出；只有 --gpu-stats 打开时才打印。
        if (gpuPassStatsLoggingEnabled)
        {
            ++shadowCasterCullStatsFramesSinceLog;
            if (shadowCasterCullStatsFramesSinceLog >=
                VL::kGpuPassTimingWindowFrames)
            {
                shadowCasterCullStatsFramesSinceLog = 0;
                LogShadowCasterCullStats();
            }
        }
        }

        if (uiOverlayRenderer.IsInitialized() && currentUiRenderSnapshot != nullptr)
        {
            if (viewportOutput != nullptr)
            {
                // 场景 attachment 写入先完成，RmlUi 图像再采样；布局和句柄只在 RT 消费。
                vk::ImageMemoryBarrier barrier;
                barrier.setImage(viewportOutput->image)
                    .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})
                    .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                    .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                    .setOldLayout(vk::ImageLayout::eColorAttachmentOptimal)
                    .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
                    .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
                    .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                    vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, barrier);
            }
            VulkanDebug::ScopedRegion uiRegion(
                commandBuffer,
                "UI Overlay",
                VulkanDebug::DebugCategory::eDefault);
            const RenderResource* debugDepth = nullptr;
            if (!offlineMode && currentUiRenderSnapshot->debugDepthTest)
                debugDepth = PassBarrier::BeginSceneDepthRead(commandBuffer, renderGraph, swapChainImageIndex);
            uiOverlayRenderer.Record(
                commandBuffer,
                swapChainImageIndex,
                *currentUiRenderSnapshot,
                viewportOutput ? viewportOutput->imageView : vk::ImageView{},
                viewportOutput ? viewportOutput->sampler : vk::Sampler{},
                debugDepth ? debugDepth->GetShaderDescriptorView() : vk::ImageView{},
                debugDepth ? debugDepth->sampler : vk::Sampler{});
            if (debugDepth) PassBarrier::EndSceneDepthRead(commandBuffer, renderGraph, swapChainImageIndex);
        }
    }

    const std::optional<std::string> screenshotPath =
        std::move(pendingScreenshotPath);
    pendingScreenshotPath.reset();

    bool screenshotRecorded = false;
    std::string screenshotRecordError;
    if (screenshotPath.has_value())
    {
        screenshotRecorded = rendererBackend->RecordSwapchainScreenshot(
            frameContext,
            screenshotPath.value(),
            screenshotRecordError);
        if (!screenshotRecorded)
        {
            std::cout << "[Diagnostics][Warning] Screenshot record failed: " <<
                screenshotRecordError << std::endl;
        }
    }

    // 本帧查询随 SubmitFrame 进入 GPU；下次复用该 frame slot 时（fence 已等待）再读回。
    gpuPassTimer.EndFrame();
    // RenderSystem only records pass commands. Queue, semaphore, fence,
    // swapchain, present, and retire epoch details stay backend-owned.
    rendererBackend->SubmitFrame(frameContext, currentFrame);
    ++submittedFrameSerial;
    submittedCameraGeneration = currentRenderScene.worldGeneration;
    submittedViewProjection = currentRenderScene.camera.viewProjection;
    if (!offlineMode)
        for (const auto& material : currentResolvedRenderScene.materialGroups)
            for (const auto& instance : material.materialInstances)
                for (const auto& draw : instance.draws)
                {
                    const auto& packet = currentRenderScene.drawPackets.at(draw.drawPacketIndex);
                    if (packet.skinPose && draw.objectResourceEntry)
                        draw.objectResourceEntry->GetResources().submittedSkin.Commit(
                            packet.skinPose, packet.model, submittedFrameSerial);
                }
    if (!offlineMode && renderGraph.GetAutoExposure())
        renderGraph.GetAutoExposure()->OnFrameSubmitted();

    if (screenshotRecorded)
    {
        std::string screenshotError;
        if (rendererBackend->CompleteSwapchainScreenshot(screenshotError))
        {
            std::cout << "[Diagnostics][Info] Screenshot saved: " <<
                screenshotPath.value() << std::endl;
        }
        else
        {
            std::cout << "[Diagnostics][Warning] Screenshot failed: " <<
                screenshotError << std::endl;
        }
    }

    currentFrame = currentFrame + 1;
}

void RenderSystem::RenderInitialize()
{
    InitializeFrameResources();
    ValidateFrameResourceDescriptors();
}

VL::RendererDescriptorContext RenderSystem::BuildRendererDescriptorContext() const
{
    return BuildRendererDescriptorContext(
        VL::RendererResourceCache::GetInstance(),
        RenderGraph::GetInstance(),
        GetLightBufferInfo());
}

VL::RendererDescriptorContext
RenderSystem::BuildRendererDescriptorContext(
    const VL::RendererResourceCache& resourceCache,
    RenderGraph& renderGraph,
    const std::vector<vk::DescriptorBufferInfo>&
        lightBufferInfos) const
{
    VL::RendererDescriptorContext descriptorContext;
    descriptorContext.globalUniformBufferInfos = &GetUBOGlobalBufferInfo();
    descriptorContext.lightBufferInfos =
        &lightBufferInfos;
    descriptorContext.resourceCache =
        &resourceCache;
    Renderpass* canonicalShadowPass =
        renderGraph.FindCanonicalShadowPass();
    if (canonicalShadowPass != nullptr)
    {
        std::shared_ptr<MaterialInstance> shadowMaterialInstance =
            canonicalShadowPass->materialInstance.lock();
        if (shadowMaterialInstance)
        {
            descriptorContext.commonOpaqueShadowMaterial =
                shadowMaterialInstance->GetBaseMaterial().lock();
        }
    }
    return descriptorContext;
}

// 风时间按固定 1/60 s 步长推进，步进量来自 EngineLoop 的 RuntimeClock 真实 delta。
// master 的 SpeedTree 风时钟是墙钟；当前分支若每渲染帧都加 1/60，在 240 FPS 下会比
// master 快 4 倍。这里保留固定步长（CWindStateMgr/SceneWindRuntime 的数值稳定），
// 只让“每秒推进多少步”跟随引擎真实时间。
void RenderSystem::AdvanceSceneWindTick()
{
    constexpr double kSceneWindTickSeconds = 1.0 / 60.0;
    // 一步同时推进场级解析（头发）和每 profile 的 CWindStateMgr（植被）。
    // 两个消费者必须看到同一个 currentWindTimeSeconds，否则阵风相位会分叉。
    const VL::ResolvedSceneWind resolvedWind =
        sceneWindRuntime.Advance(currentRenderScene.wind, currentWindTimeSeconds);
    currentRenderScene.windTimeSeconds = currentWindTimeSeconds;
    currentRenderScene.resolvedWind = resolvedWind;
    windResponseProfiles.AdvanceToSceneWind(
        currentWindTimeSeconds, currentRenderScene.wind);
    currentWindTimeSeconds += kSceneWindTickSeconds;
}

void RenderSystem::AdvanceSceneWind()
{
    constexpr double kSceneWindTickSeconds = 1.0 / 60.0;
    if (!windClockInitialized)
    {
        windClockInitialized = true;
        // 首帧只做 time=0 的目标采样；此时 GT 可能已经积累了一帧 delta，清零避免
        // 同一渲染帧里立刻再走一步，导致第一帧强度从目标值跳开。
        windPendingDeltaSeconds.store(0.0f, std::memory_order_relaxed);
        windTickAccumulator = 0.0;
        AdvanceSceneWindTick();
        return;
    }

    // exchange 而不是 load 后清零：GT 可能在读取后立刻追加下一帧 delta；
    // 原子交换保证每一份 GT delta 只被 RT 消费一次。
    const double elapsed = std::max(0.0, static_cast<double>(
        windPendingDeltaSeconds.exchange(0.0f, std::memory_order_relaxed)));
    // 调试断点/长卡顿后最多追 0.25 s（15 步），避免一次补几百步让 gust 连续爆发。
    windTickAccumulator = std::min(windTickAccumulator + elapsed, 0.25);
    // 一帧内可以补多步；严格按时间顺序推进，保持 CWindStateMgr 的 RNG/gust 状态机顺序。
    while (windTickAccumulator >= kSceneWindTickSeconds)
    {
        AdvanceSceneWindTick();
        windTickAccumulator -= kSceneWindTickSeconds;
    }
}

void RenderSystem::ShutdownRenderObject()
{
    // trace 可能还有 1..2 个 frame slot pending；这里显式等一次队列并写盘。
    // 调用方（EngineLoop::Shutdown）已停止渲染线程，且 backend 仍然有效。
    if (const auto& hairSimulation = RenderGraph::GetInstance().GetHairSimulation())
        hairSimulation->FinishTrace(true);
    offlineCoordinator.reset();
    offlineDisplay.Shutdown();
    materialInstancePreviewAdapter.Reset();
    if (shaderReloadCoordinator != nullptr)
    {
        shaderReloadCoordinator->SetUiOverlayParticipant(nullptr);
    }
    uiOverlayRenderer.Shutdown();
    currentUiRenderSnapshot.reset();
    ShutdownFrameResources();
}

void RenderSystem::InitializeFrameResources()
{
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem cannot initialize frame resources without a renderer backend");
    }
    if (pipelineFactory == nullptr)
    {
        throw std::runtime_error("RenderSystem cannot initialize procedural sky IBL without a pipeline factory");
    }

    frameResources.Initialize(*rendererBackend);
    frameResources.EnsureLightCapacity(currentRenderScene.lights.size(), *rendererBackend);
    editorViewportGrid.EnsureCreated(*rendererBackend);
    proceduralSkyCubeGenerator.Initialize(
        *pipelineFactory,
        *rendererBackend);
    environmentIblBaker.Initialize(
        *pipelineFactory,
        *rendererBackend,
        frameResources.GetGlobalUniformBufferInfos());
    if (shaderReloadCoordinator != nullptr)
    {
        shaderReloadCoordinator->RegisterComputeParticipant(
            &proceduralSkyCubeGenerator);
        shaderReloadCoordinator->RegisterComputeParticipant(
            &skyShReloadParticipant);
        shaderReloadCoordinator->RegisterComputeParticipant(
            &prefilterReloadParticipant);
        shaderReloadCoordinator->RegisterComputeParticipant(&exposureHistogramReloadParticipant);
        shaderReloadCoordinator->RegisterComputeParticipant(&exposureAdaptReloadParticipant);
    }
    environmentGpuTimer.Initialize(*rendererBackend);
    // query pool 在首次采集时才创建；--gpu-stats 关闭时这里只记录能力快照。
    gpuPassTimer.Initialize(*rendererBackend, MAX_FRAMES_IN_FLIGHT);
}

void RenderSystem::ShutdownFrameResources()
{
    if (rendererBackend == nullptr)
    {
        return;
    }

    // 先销毁统计 query pool，再释放 swapchain 相关帧资源。
    gpuPassTimer.Shutdown(*rendererBackend);
    environmentGpuTimer.Shutdown(*rendererBackend);
    if (shaderReloadCoordinator != nullptr)
    {
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &proceduralSkyCubeGenerator);
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &skyShReloadParticipant);
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &prefilterReloadParticipant);
        shaderReloadCoordinator->UnregisterComputeParticipant(&exposureHistogramReloadParticipant);
        shaderReloadCoordinator->UnregisterComputeParticipant(&exposureAdaptReloadParticipant);
    }
    environmentIblBaker.Shutdown(*rendererBackend);
    proceduralSkyCubeGenerator.Shutdown(*rendererBackend);
    environmentUpdateScheduler.Reset();
    environmentUpdateSourceCube.reset();
    
    frameResources.Shutdown(*rendererBackend);
    editorViewportGrid.Shutdown(*rendererBackend);
}

void RenderSystem::ValidateFrameResourceDescriptors()
{
    if (!frameResources.IsInitialized())
    {
        throw std::runtime_error("RenderSystem cannot expose frame resource descriptors before frame resources are initialized");
    }
}

std::pair<float, float> RenderSystem::ComputeMinMaxAlongAxis(const Eigen::Vector3f& aabbMin, const Eigen::Vector3f& aabbMax, const Eigen::Vector3f& axis) const
{
    Eigen::Vector3f dir = axis;
    float len = dir.norm();
    if (len > 0.0f)
    {
        dir /= len;
    }
    std::array<Eigen::Vector3f, 8> corners = {
        Eigen::Vector3f(aabbMin.x(), aabbMin.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMin.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMin.x(), aabbMax.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMax.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMin.x(), aabbMin.y(), aabbMax.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMin.y(), aabbMax.z()),
        Eigen::Vector3f(aabbMin.x(), aabbMax.y(), aabbMax.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMax.y(), aabbMax.z())
    };
    float minProj = dir.dot(corners[0]);
    float maxProj = minProj;
    for (size_t i = 1; i < corners.size(); ++i)
    {
        float proj = dir.dot(corners[i]);
        minProj = std::min(minProj, proj);
        maxProj = std::max(maxProj, proj);
    }
    return { minProj, maxProj };
}

std::optional<std::pair<float, float>> RenderSystem::ComputeCascadeLightSpaceZBounds(
    const Eigen::Matrix3f& worldToShadowMatrix,
    float minX,
    float maxX,
    float minY,
    float maxY)
{
    if (currentRenderScene.drawPackets.empty())
    {
        return std::nullopt;
    }

    // 投影只取决于光源朝向：四个级联共用同一份 worldToShadowMatrix，因此每帧
    // 只投影一次并缓存；逐级联只按相机 slice 的 XY 范围过滤，再合并 Z。
    // 之前每个级联都重扫全部 draw packet，等于 4 倍的投影计算。
    if (!casterLightSpaceBoundsValid)
    {
        const Eigen::Vector3f lightSpaceXAxis = worldToShadowMatrix.row(0).transpose();
        const Eigen::Vector3f lightSpaceYAxis = worldToShadowMatrix.row(1).transpose();
        const Eigen::Vector3f lightSpaceZAxis = worldToShadowMatrix.row(2).transpose();
        const Eigen::Vector3f absLightSpaceXAxis = lightSpaceXAxis.cwiseAbs();
        const Eigen::Vector3f absLightSpaceYAxis = lightSpaceYAxis.cwiseAbs();
        const Eigen::Vector3f absLightSpaceZAxis = lightSpaceZAxis.cwiseAbs();

        casterLightSpaceBounds.clear();
        casterLightSpaceBounds.reserve(currentRenderScene.drawPackets.size());
        for (const VL::RenderDrawPacket& drawPacket : currentRenderScene.drawPackets)
        {
            // Current limitation: SpeedTree world bounds describe the undeformed
            // mesh and exclude vertex-shader wind displacement. The foliage bounds
            // contract must eventually provide expanded caster bounds so strong
            // wind cannot clip branch shadows from the CSM light-space Z range.
            const Eigen::Vector3f center =
                (drawPacket.worldBoundsMin + drawPacket.worldBoundsMax) * 0.5f;
            const Eigen::Vector3f extent =
                (drawPacket.worldBoundsMax - drawPacket.worldBoundsMin) * 0.5f;

            ShadowCasterLightSpaceBounds bounds;
            const float projectedCenterX = lightSpaceXAxis.dot(center);
            const float projectedRadiusX = absLightSpaceXAxis.dot(extent);
            bounds.minX = projectedCenterX - projectedRadiusX;
            bounds.maxX = projectedCenterX + projectedRadiusX;
            const float projectedCenterY = lightSpaceYAxis.dot(center);
            const float projectedRadiusY = absLightSpaceYAxis.dot(extent);
            bounds.minY = projectedCenterY - projectedRadiusY;
            bounds.maxY = projectedCenterY + projectedRadiusY;
            const float projectedCenterZ = lightSpaceZAxis.dot(center);
            const float projectedRadiusZ = absLightSpaceZAxis.dot(extent);
            bounds.minZ = projectedCenterZ - projectedRadiusZ;
            bounds.maxZ = projectedCenterZ + projectedRadiusZ;
            casterLightSpaceBounds.push_back(bounds);
        }
        casterLightSpaceBoundsValid = true;
    }

    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();
    bool hasCaster = false;
    for (const ShadowCasterLightSpaceBounds& bounds : casterLightSpaceBounds)
    {
        if (bounds.maxX < minX || bounds.minX > maxX)
        {
            continue;
        }
        if (bounds.maxY < minY || bounds.minY > maxY)
        {
            continue;
        }
        minZ = std::min(minZ, bounds.minZ);
        maxZ = std::max(maxZ, bounds.maxZ);
        hasCaster = true;
    }

    if (!hasCaster)
    {
        return std::nullopt;
    }

    return std::make_pair(minZ, maxZ);
}

std::array<Eigen::Vector3f, 8> RenderSystem::BuildWorldCorners(const Eigen::Vector3f& localMin, const Eigen::Vector3f& localMax, const Eigen::Matrix4f& modelMatrix) const
{
    return {
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMin.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMin.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMax.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMax.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMin.y(), localMax.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMin.y(), localMax.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMax.y(), localMax.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMax.y(), localMax.z(), 1.0f)).head<3>()
    };
}

void RenderSystem::ComputeAabbFromCorners(const std::array<Eigen::Vector3f, 8>& corners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax)
{
    outMin = corners[0];
    outMax = corners[0];
    for (const auto& corner : corners)
    {
        outMin = outMin.cwiseMin(corner);
        outMax = outMax.cwiseMax(corner);
    }
}

std::shared_ptr<Texture> RenderSystem::GetActiveEnvironmentCube()
{
    return GetEnvironmentCube(
        currentRenderScene,
        VL::RendererResourceCache::GetInstance());
}

std::shared_ptr<Texture> RenderSystem::GetEnvironmentCube(
    const VL::RenderScene& renderScene,
    const VL::RendererResourceCache& resourceCache) const
{
    switch(renderScene.environment.type)
    {
        case VL::EnvironmentType::None:
            // 只为保持实时 descriptor 合同而绑定已有 cubemap；强度为零，
            // 不进行 IBL 更新且跳过 sky，不能将其视为环境补光。
            return proceduralSkyCubeGenerator.GetActiveEnvironmentCube();
        case VL::EnvironmentType::ProceduralSky:
            return proceduralSkyCubeGenerator.GetActiveEnvironmentCube();
        case VL::EnvironmentType::Hdri:
            const std::shared_ptr<Texture>* environmentCube = 
                resourceCache.GetWorldTexture(
                    "environmentCube");
            if(environmentCube == nullptr || *environmentCube == nullptr)
            {
                throw std::runtime_error("Environment cube not found");
            }
            return *environmentCube;
    }
    throw std::runtime_error("Unknown environment type");
}

void RenderSystem::PrepareEnvironmentResources()
{
    PrepareEnvironmentResources(
        currentRenderScene,
        VL::RendererResourceCache::GetInstance());
}

void RenderSystem::PrepareEnvironmentResources(
    const VL::RenderScene& renderScene,
    VL::RendererResourceCache& resourceCache) const
{
    std::shared_ptr<Texture> environmentCube =
        GetEnvironmentCube(renderScene, resourceCache);
    resourceCache.BindWorldTexture(
        "environmentCube",
        environmentCube);

    std::shared_ptr<Texture> prefilteredEnvironmentCube =
        environmentIblBaker
            .GetPrefilteredEnvironmentCube();
    if (prefilteredEnvironmentCube == nullptr)
    {
        throw std::runtime_error("Prefiltered environment cube is not initialized");
    }
    resourceCache.BindWorldTexture(
        "prefilteredEnvironmentCube",
        prefilteredEnvironmentCube);
}

void RenderSystem::RecordHairSimulation(vk::CommandBuffer commandBuffer, uint32_t frameSlot)
{
    const auto& hairSimulation = RenderGraph::GetInstance().GetHairSimulation();
    if (!hairSimulation)
        return;
    // GT 侧只登记请求；这里在渲染线程把它交给 pass。trace 的资源创建/拷贝
    // 全部发生在 Record 内，Console 线程不触碰 GPU。
    std::optional<VL::Hair::HairTraceRequest> traceRequest;
    {
        std::lock_guard<std::mutex> lock(hairTraceRequestMutex);
        traceRequest = std::move(pendingHairTraceRequest);
        pendingHairTraceRequest.reset();
    }
    if (traceRequest)
        hairSimulation->BeginTrace(*traceRequest);
    // 复位请求只在存在 pass 时消费；没有 pass 就保留到下一个可录制帧，
    // 避免把请求丢在无 World/无 graph 的帧里。
    std::optional<std::string> resetRequest;
    {
        std::lock_guard<std::mutex> lock(hairResetRequestMutex);
        resetRequest = std::move(pendingHairResetRequest);
        pendingHairResetRequest.reset();
    }
    if (resetRequest)
        hairSimulation->RequestReset(std::move(*resetRequest));
    // 实时参数覆盖与复位同构：GT 只登记，渲染线程每帧取走一次。没有 pass 时
    // 上面的提前 return 会保留队列，避免把请求丢在无 World/无 graph 的帧里。
    std::vector<VL::Hair::HairLiveParameterRequest> liveParameters;
    {
        std::lock_guard<std::mutex> lock(hairLiveParameterMutex);
        liveParameters.swap(pendingHairLiveParameters);
    }
    for (const auto& parameter : liveParameters)
        hairSimulation->RequestLiveParameter(parameter);
    // 每帧只调用一次，且在所有 graphics pass 之前：本帧所有读取 current positions 的
    // vertex shader 都看到同一份已经完成 dispatch 的位置。
    hairSimulation->Record(
        commandBuffer, currentResolvedRenderScene, currentRenderScene, frameSlot);
}

void RenderSystem::RecordSkinDeform(vk::CommandBuffer commandBuffer, uint32_t swapchainImageIndex)
{
    if (!prerenderSkinEnabled)
        return;
    const auto& skinDeform = RenderGraph::GetInstance().GetSkinDeform();
    if (!skinDeform)
        return;

    // 预蒙皮 compute 读的 palette 必须在本帧任何 draw 之前写入当前 image 槽。
    // draw 路径在预蒙皮开启时不再重复上传，因此这里覆盖所有蒙皮 draw。
    std::unordered_set<VL::RendererObjectGpuResources*> uploaded;
    for (const auto& materialGroup : currentResolvedRenderScene.materialGroups)
    {
        for (const auto& instanceGroup : materialGroup.materialInstances)
        {
            for (const auto& draw : instanceGroup.draws)
            {
                if (!draw.objectResourceEntry)
                    continue;
                VL::RendererObjectGpuResources& resources =
                    draw.objectResourceEntry->GetResources();
                if (!resources.skinDeform.HasResources())
                    continue;
                if (!uploaded.insert(&resources).second)
                    continue;
                const auto& packet = currentRenderScene.drawPackets.at(draw.drawPacketIndex);
                if (!packet.skinPose)
                    continue;
                resources.submittedSkin.RetainIfSubmittedAt(submittedFrameSerial);
                frameResources.UpdateObjectSkinPalette(
                    swapchainImageIndex, resources, packet);
            }
        }
    }

    skinDeform->Record(commandBuffer, currentResolvedRenderScene, swapchainImageIndex);
}

void RenderSystem::RecordEnvironmentIbl(vk::CommandBuffer commandBuffer, uint32_t swapchainImageIndex)
{
    if (currentRenderScene.environment.type == VL::EnvironmentType::None) return;
    PROFILE_SCOPE("EnvironmentIbl");
    environmentGpuTimer.BeginFrame(commandBuffer, swapchainImageIndex);

    // Sky Pass 每帧直接读取最新参数；cubemap、SH 和 prefilter 只在源数据 dirty 时
    // 启动新代际，避免相机移动或单纯强度变化触发昂贵重建。
    environmentUpdateState.Observe(currentRenderScene.environment);
    if (environmentUpdateState.IsDirty())
    {
        const uint64_t requestedGeneration = environmentUpdateState.BeginUpdate();
        const bool startedNewGeneration = environmentUpdateScheduler.RequestUpdate(
            requestedGeneration,
            currentRenderScene.environment,
            environmentIblBaker.GetPrefilterMipCount());
        if (startedNewGeneration)
        {
            // 程序化天空先写 pending cubemap；HDRI 本身已经完整，可以直接作为冻结输入。
            environmentUpdateSourceCube =
                currentRenderScene.environment.type == VL::EnvironmentType::ProceduralSky
                ? proceduralSkyCubeGenerator.GetPendingEnvironmentCube()
                : GetActiveEnvironmentCube();
        }
    }

    VL::EnvironmentUpdateFramePlan framePlan = environmentUpdateScheduler.BuildFramePlan();
    if (!framePlan.HasWork())
    {
        RefreshEnvironmentUpdateDiagnostics();
        return;
    }
    if (!environmentUpdateSourceCube)
    {
        throw std::runtime_error("Environment update source cube is missing.");
    }

    const VL::EnvironmentSnapshot& pendingSnapshot =
        environmentUpdateScheduler.GetPendingSnapshot();
    if (framePlan.cubemapFaceCount > 0)
    {
        // 这里的 for 只消费 Scheduler 为“当前渲染帧”分配的 face 批次，
        // 并不是在一帧内固定遍历 cubemap 的 6 个面。默认 cubemapFacesPerFrame == 1：
        //
        // 渲染帧        cubemapFaceCount        本帧录制
        // Frame 1              1                 Face 0
        // Frame 2              1                 Face 1
        // Frame 3              1                 Face 2
        // Frame 4              1                 Face 3
        // Frame 5              1                 Face 4
        // Frame 6              1                 Face 5
        //
        // 如果预算改为 2，这个 for 才会每帧循环两次，并用 3 帧完成全部 6 个面。
        environmentGpuTimer.BeginProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Cubemap);
        for (uint32_t faceOffset = 0; faceOffset < framePlan.cubemapFaceCount; ++faceOffset)
        {
            PROFILE_SCOPE("EnvironmentIbl::CubemapFace");
            proceduralSkyCubeGenerator.RecordFace(
                commandBuffer,
                swapchainImageIndex,
                pendingSnapshot.skyParameters,
                framePlan.cubemapFaces[faceOffset]);
        }
        environmentGpuTimer.EndProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Cubemap);
    }

    if (framePlan.projectSphericalHarmonics)
    {
        PROFILE_SCOPE("EnvironmentIbl::SH");
        environmentGpuTimer.BeginProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::SphericalHarmonics);
        environmentIblBaker.RecordSphericalHarmonics(
            commandBuffer,
            environmentUpdateSourceCube,
            swapchainImageIndex);
        environmentGpuTimer.EndProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::SphericalHarmonics);
    }

    if (!framePlan.prefilterMips.empty())
    {
        environmentGpuTimer.BeginProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Prefilter);
        for (uint32_t mipLevel : framePlan.prefilterMips)
        {
            PROFILE_SCOPE("EnvironmentIbl::PrefilterMip");
            environmentIblBaker.RecordPrefilterMip(
                commandBuffer,
                environmentUpdateSourceCube,
                swapchainImageIndex,
                mipLevel);
        }
        environmentGpuTimer.EndProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Prefilter);
    }

    if (framePlan.commit)
    {
        PROFILE_SCOPE("EnvironmentIbl::Commit");
        environmentGpuTimer.BeginProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Commit);
        if (pendingSnapshot.type == VL::EnvironmentType::ProceduralSky)
        {
            proceduralSkyCubeGenerator.RecordCommit(commandBuffer);
        }
        environmentIblBaker.RecordCommit(commandBuffer);
        environmentGpuTimer.EndProduct(
            commandBuffer,
            swapchainImageIndex,
            VL::EnvironmentGpuProduct::Commit);
        environmentUpdateScheduler.CompleteCommit(framePlan.generation);
        environmentUpdateState.CompleteUpdate(framePlan.generation);
        environmentUpdateSourceCube.reset();
    }

    RefreshEnvironmentUpdateDiagnostics();
}

void RenderSystem::RefreshEnvironmentUpdateDiagnostics()
{
    VL::EnvironmentUpdateDiagnostics snapshot;
    snapshot.progress = environmentUpdateScheduler.GetProgress();
    snapshot.gpuTiming = environmentGpuTimer.GetSnapshot();

    std::lock_guard<std::mutex> lock(environmentDiagnosticsMutex);
    environmentDiagnostics = std::move(snapshot);
}
