#include "nativeTerrainCaptureAdapter.h"
#include "terrainEvidenceWriter.h"
#include "terrainTesCapture.h"
#include "terrainTesCapturePipeline.h"
#include "render/pass/passRuntime.h"
#include "render/frontend/renderScene.h"
#include "render/backend/rendererBackendVulkan.h"
#include "material.h"
#include "materialInstance.h"
#include "renderableObject.h"
#include "renderGraph.h"
#include "shaderCompiler.h"
#include "pipeline/pipelineBase.h"
#include "shader/build/contentHash.h"
#include "commonFunction.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>

namespace VL
{
namespace
{
using Json = nlohmann::json;
class NativeTerrainCaptureAdapter final : public ITerrainCaptureHooks
{
    Json config;
    std::string instrumentedSource;
    TerrainCaptureFrameInfo frame;
    ShaderCompiler compiler;
    std::unique_ptr<PipelineFactory> factory;
    std::shared_ptr<PipelineBase> diagnosticPipeline;
    std::unique_ptr<TerrainTesCapture> capture;
    std::shared_ptr<const TerrainTesDrawWitness> witness;
    TerrainCaptureDrawInputs chosen;
    std::shared_ptr<const MaterialInstanceStateSnapshot> materialState;
    std::string sourceFingerprint;
    std::string artifactGeneration;
    uint64_t pendingEpoch = 0;
    uint64_t submittedEpoch = 0;
    bool recording = false;
    bool drawRecorded = false;
    bool copied = false;
    bool complete = false;
    std::string failure;

    bool CameraMatches(const CameraSnapshot& camera) const
    {
        Eigen::Vector3f position, target;
        for (int axis = 0; axis < 3; ++axis)
        {
            position[axis] = config.at("cameraPosition")[axis].get<float>();
            target[axis] = config.at("cameraLookAt")[axis].get<float>();
        }
        return (camera.position-position).cwiseAbs().maxCoeff() <= .02f &&
            camera.forward.dot((target-position).normalized()) >= .999999f;
    }
    bool SameDraw(const TerrainCaptureDrawInputs& inputs) const
    {
        return inputs.packet && witness && inputs.packet->objectId == chosen.packet->objectId &&
            inputs.packet->mesh.key == chosen.packet->mesh.key &&
            inputs.drawPacketIndex == chosen.drawPacketIndex &&
            inputs.mesh.vertexBuffers == chosen.mesh.vertexBuffers &&
            inputs.mesh.vertexOffsets == chosen.mesh.vertexOffsets &&
            inputs.mesh.indexBuffer == chosen.mesh.indexBuffer && inputs.mesh.indexCount == chosen.mesh.indexCount &&
            inputs.mesh.indexOffset == chosen.mesh.indexOffset && inputs.mesh.indexType == chosen.mesh.indexType &&
            inputs.mesh.firstIndex == chosen.mesh.firstIndex && inputs.mesh.vertexOffset == chosen.mesh.vertexOffset &&
            inputs.mesh.firstInstance == chosen.mesh.firstInstance && inputs.mesh.instanceCount == chosen.mesh.instanceCount &&
            inputs.mesh.patches == chosen.mesh.patches && inputs.descriptorSetMask == chosen.descriptorSetMask &&
            inputs.descriptorSets == chosen.descriptorSets && inputs.frameSlot == chosen.frameSlot &&
            inputs.regularPipeline == chosen.regularPipeline;
    }
public:
    explicit NativeTerrainCaptureAdapter(const std::filesystem::path& path)
    {
        std::ifstream stream(path);
        stream >> config;
        if (config.at("kind") != "native-terrain-test-capture-v1")
            throw std::runtime_error("Explicit terrain capture test case kind invalid");
        const std::filesystem::path source = config.at("instrumentedTesSource").get<std::string>();
        if (ContentHasher::HashFile(source).ToHex() != config.at("instrumentedTesBlake3"))
            throw std::runtime_error("Explicit instrumented source changed");
        std::ifstream shader(source);
        instrumentedSource.assign(std::istreambuf_iterator<char>(shader), {});
        const std::filesystem::path manifest = config.at("publicationManifest").get<std::string>();
        if (ContentHasher::HashFile(manifest).ToHex() != config.at("publicationBlake3"))
            throw std::runtime_error("Explicit publication changed");
        for (const auto& file : config.at("actualFileBlake3").items())
            if (ContentHasher::HashFile(file.key()).ToHex() != file.value().get<std::string>())
                throw std::runtime_error("Explicit scene/TR/render settings input changed: " + file.key());
    }
    bool RequiresTesVertexStores() const noexcept override { return true; }
    void BeforeGeometry(PassRuntimeContext& context, const TerrainCaptureFrameInfo& actualFrame) override
    {
        if (complete || capture || !failure.empty() || !CameraMatches(context.renderScene.camera))
            return;
        if (!actualFrame.backend || !actualFrame.actualWorldBudget || !actualFrame.immutableFrameOwner ||
            !actualFrame.immutableGraphOwner || !actualFrame.submissionEpoch || !actualFrame.poseId ||
            actualFrame.worldGeneration != context.renderScene.worldGeneration ||
            actualFrame.actualBaseFineResidentPages == UINT32_MAX)
            throw std::runtime_error("Actual native capture Frame DTO is incomplete");
        const auto expectedRoot = std::filesystem::weakly_canonical(config.at("resourceRoot").get<std::string>());
        if (std::filesystem::weakly_canonical(CommonFunction::GetResourcePath()) != expectedRoot)
            throw std::runtime_error("Native capture is running against a different actual resource root");
        const std::string group = config.at("lodGroupId");
        for (const auto& materialGroup : context.resolvedRenderScene.materialGroups)
            for (const auto& instanceGroup : materialGroup.materialInstances)
                for (const auto& draw : instanceGroup.draws)
                {
                    const auto& packet = context.renderScene.drawPackets.at(draw.drawPacketIndex);
                    if (!packet.isTerrain || packet.lodGroupId != group)
                        continue;
                    if (packet.lodLevel != 0)
                        return;
                    const auto& normal = materialGroup.material->GetRenderPipeline();
                    if (!normal || !normal->UsesTessellation())
                        throw std::runtime_error("Selected actual LOD0 draw does not use normal TES");
                    const auto& recipe = materialGroup.material->GetPipelineReloadRecipe().surface;
                    if (!recipe.shader.materialCompileRequest)
                        throw std::runtime_error("Selected material lacks its actual native compile request");
                    frame = actualFrame;
                    if (!factory)
                    {
                        compiler.Initialize(std::filesystem::path(CommonFunction::GetProjectPath())/"shader");
                        factory = frame.backend->CreatePipelineFactory();
                        factory->SetShaderCompiler(&compiler);
                    }
                    auto build = CompileTerrainTesCaptureVariant(compiler, *recipe.shader.materialCompileRequest, instrumentedSource);
                    auto artifact = BuildGraphicsShaderVariantArtifact(build, "Explicit native terrain TES capture");
                    sourceFingerprint = build.sourceFingerprint;
                    artifactGeneration = build.artifactGenerationKey;
                    auto renderPass = context.renderPass.renderPass;
                    diagnosticPipeline = CreateTerrainTesCapturePipeline(*factory, renderPass,
                        recipe.passPipelineContractKey, *normal, artifact, recipe.cullMode, recipe.blendMode);
                    chosen = BuildTerrainCaptureDrawInputs(packet, uint32_t(draw.drawPacketIndex),
                        context.swapChainImageIndex%RenderableObject::DynamicMeshFrameSlotCount,
                        context.swapChainImageIndex, *normal, context.renderPass, draw.renderableObject,
                        draw.objectResourceEntry, materialGroup.material, instanceGroup.materialInstance);
                    materialState = std::make_shared<const MaterialInstanceStateSnapshot>(
                        instanceGroup.materialInstance->CaptureStateSnapshot());
                    auto value = std::make_shared<TerrainTesDrawWitness>();
                    value->frame = frame.frame;
                    value->submissionEpoch = frame.submissionEpoch;
                    value->worldGeneration = frame.worldGeneration;
                    value->poseId = frame.poseId;
                    value->drawOrdinal = chosen.drawPacketIndex;
                    value->surfaceIndex = config.at("surfaceIndex");
                    value->actualLod = packet.lodLevel;
                    value->topology = chosen.mesh.patches ? vk::PrimitiveTopology::ePatchList : vk::PrimitiveTopology::eTriangleList;
                    value->firstPrimitive = config.value("firstPrimitive", 0u);
                    value->primitiveCount = config.value("primitiveCount", 1u);
                    value->actualIndexCount = chosen.mesh.indexCount;
                    value->actualFirstIndex = chosen.mesh.firstIndex;
                    value->actualVertexOffset = chosen.mesh.vertexOffset;
                    value->actualInstanceCount = chosen.mesh.instanceCount;
                    value->actualFirstInstance = chosen.mesh.firstInstance;
                    value->vertexBuffer = chosen.mesh.vertexBuffers[0];
                    value->patchIndexBuffer = chosen.mesh.indexBuffer;
                    value->actualDescriptorSets = chosen.descriptorSets;
                    value->actualDescriptorSetMask = chosen.descriptorSetMask;
                    value->immutableDrawOwners = {frame.immutableFrameOwner, frame.immutableGraphOwner,
                        chosen.renderableOwner, chosen.objectResourceOwner, materialState,
                        chosen.materialOwner, frame.actualWorldBudget->IdentityToken(), diagnosticPipeline};
                    value->surfaceId = config.at("surfaceId");
                    value->publicationSha256 = config.at("publicationSha256");
                    value->capturedShaderSha256 = config.at("instrumentedTesSha256");
                    value->inputClosureSha256 = config.at("inputClosureSha256");
                    std::copy(packet.model.data(), packet.model.data()+16, value->model.begin());
                    std::copy(context.renderScene.camera.viewProjection.data(),
                        context.renderScene.camera.viewProjection.data()+16, value->viewProjection.begin());
                    value->displacementScale = instanceGroup.materialInstance->GetParameter<float>("u_layerDisplacementScale");
                    value->tessRadiusCm = instanceGroup.materialInstance->GetParameter<float>("u_terrainTessRadiusM")*100;
                    value->baseFineResidentPages = frame.actualBaseFineResidentPages;
                    TerrainTesCaptureLimits limits;
                    limits.capacity = config.value("capacity", 512u);
                    limits.explicitMetadataCost = {4096, 65536, 0, 0};
                    capture = std::make_unique<TerrainTesCapture>(*frame.backend, frame.actualWorldBudget, diagnosticPipeline, limits);
                    witness = value;
                    capture->BeginOutsideRenderPass(context.commandBuffer, witness);
                    pendingEpoch = frame.submissionEpoch;
                    recording = true;
                    return;
                }
    }
    std::shared_ptr<PipelineBase> SelectActualDraw(const TerrainCaptureDrawInputs& inputs) override
    {
        if (!recording || !SameDraw(inputs)) return {};
        if (drawRecorded) throw std::runtime_error("Selected terrain capture draw was recorded twice");
        return diagnosticPipeline;
    }
    void BindActualDraw(vk::CommandBuffer command, const TerrainCaptureDrawInputs& inputs,
        const PipelineBase& actuallyBoundPipeline) override
    {
        if (!recording || !SameDraw(inputs)) throw std::runtime_error("TES capture actual bindings changed");
        capture->BindInsideActualDraw(command, actuallyBoundPipeline);
    }
    void ActualDrawRecorded(const TerrainCaptureDrawInputs& inputs) override
    {
        if (!recording || !SameDraw(inputs)) throw std::runtime_error("TES capture actual draw identity changed");
        drawRecorded = true;
    }
    void AfterGeometry(vk::CommandBuffer command) override
    {
        if (!recording) return;
        if (!drawRecorded) throw std::runtime_error("Prepared terrain draw was never recorded");
        capture->CopyOutsideRenderPass(command);
        copied = true;
        recording = false;
    }
    std::shared_ptr<const void> Submitted(uint64_t epoch) override
    {
        if (!capture || !copied || pendingEpoch != epoch) return {};
        auto lifetime = capture->Submitted(epoch);
        submittedEpoch = epoch;
        return lifetime;
    }
    void AbortUnsubmitted() override
    {
        if (!capture || submittedEpoch) return;
        capture->AbortUnsubmitted();
        capture.reset();
        witness.reset();
        failure = "Native capture frame aborted before submission";
        recording = copied = false;
    }
    void ConsumeCompleted(uint64_t epoch) override
    {
        if (!failure.empty()) throw std::runtime_error(failure);
        if (!capture || !submittedEpoch || epoch < submittedEpoch) return;
        auto readback = capture->TryConsume(epoch);
        if (!readback) return;
        try
        {
            const auto output = std::filesystem::path(config.at("output").get<std::string>());
            std::filesystem::create_directories(output);
            const auto basename = "tes-"+std::to_string(witness->worldGeneration)+"-"+
                std::to_string(witness->poseId)+"-"+std::to_string(witness->drawOrdinal);
            const auto binaryPath = output/(basename+".bin");
            const auto metadataPath = output/(basename+".native.json");
            if (std::filesystem::exists(binaryPath) || std::filesystem::exists(metadataPath))
                throw std::runtime_error("Native TES evidence destination already exists");
            std::ostringstream nativeMetadata;
            WriteTerrainEvidenceFile(binaryPath, [&](std::ostream& binary) {
                TerrainTesCapture::WriteReadback(*readback, binary, nativeMetadata);
            });
            auto metadata = Json::parse(nativeMetadata.str());
            metadata["sourceFingerprint"] = sourceFingerprint;
            metadata["artifactGenerationKey"] = artifactGeneration;
            metadata["actualLodGroupId"] = chosen.packet->lodGroupId;
            metadata["actualMeshResourceKey"] = chosen.packet->mesh.key;
            metadata["actualMaterialIdentity"] = materialState->normalizedMaterialInstanceIdentity;
            metadata["normalPipelineUsedForOtherDraws"] = true;
            WriteTerrainEvidenceFile(metadataPath, [&](std::ostream& stream) {
                stream << metadata.dump(2);
            });
            std::cout << "[RVT_TES_CAPTURE] " << metadata.dump() << '\n';
        }
        catch (const std::exception& error)
        {
            failure = std::string("Native TES evidence incomplete: ")+error.what();
            complete = false;
            throw std::runtime_error(failure);
        }
        complete = true;
        capture.reset();
        witness.reset();
        materialState.reset();
        diagnosticPipeline.reset();
        chosen = {};
        frame = {};
    }
    void CloseAfterDeviceIdle() override
    {
        if (submittedEpoch && capture) ConsumeCompleted(submittedEpoch);
        if (!submittedEpoch && capture) AbortUnsubmitted();
        capture.reset();
        diagnosticPipeline.reset();
        factory.reset();
        chosen = {};
        witness.reset();
        materialState.reset();
        frame = {};
    }
};
}
std::shared_ptr<ITerrainCaptureHooks> LoadNativeTerrainCaptureAdapter(const std::filesystem::path& path)
{
    return std::make_shared<NativeTerrainCaptureAdapter>(path);
}
}
