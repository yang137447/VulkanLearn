#include "render/frame/terrainCompositedQualityFrame.h"
#include "render/frame/terrainCompositedReadiness.h"
#include "render/frame/terrainCompositedLoading.h"
#include "render/frame/terrainCompositedFrameLifetime.h"
#include "render/frame/terrainCompositedCaptureLifecycle.h"
#include "render/frame/renderFrameRecorder.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/pass/passRuntime.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/terrainCompositedService.h"
#include "render/resource/resourceRetireQueue.h"
#include "pipeline/pipelineFactory.h"
#include "pipeline/pipelineBase.h"
#include "material.h"
#include "materialInstance.h"
#include "texture.h"
#include "renderGraph.h"
#include "shaderCompiler.h"
#include "commonFunction.h"
#include "core/logStore.h"
#include "core/runtimeEvidence.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <utility>
#include <nlohmann/json.hpp>

namespace VL
{
namespace
{
std::mutex observerMutex;
TerrainCompositedQualityFrame::DepthOwnerResolver depthOwnerResolver;
std::optional<TerrainQualityObservation> observation;
std::weak_ptr<TerrainCompositedService> readinessService;
std::weak_ptr<TerrainQualityValidation> readinessValidation;
std::optional<std::pair<uint64_t, uint64_t>> failedSession;

double NowMs()
{
    return TerrainCompositedQualityMonotonicMs();
}

void QualityEvent(const char* name, uint64_t world, uint64_t pose, uint64_t epoch = 0)
{
    const auto event = nlohmann::json{
        {"event", name}, {"worldGeneration", world}, {"poseId", pose},
        {"submissionEpoch", epoch}, {"monotonicMs", NowMs()}};
    std::cout << "[RVT_QUALITY_EVENT] " << event.dump() << '\n';
    RuntimeEvidence::Record("RVT_QUALITY_EVENT", event);
}

void QualityFrameError(const std::exception& error)
{
    std::cerr << "[RVT_QUALITY_INCOMPLETE] " << error.what() << '\n';
    try
    {
        RuntimeEvidence::Record("RVT_QUALITY_INCOMPLETE", {{"error", error.what()}});
    }
    catch (...) {}
}

void CaptureEvent(nlohmann::json event)
{
    event["monotonicMs"] = NowMs();
    std::cout << "[RVT_QUALITY_EVENT] " << event.dump() << '\n';
    RuntimeEvidence::Record("RVT_QUALITY_EVENT", event);
}

void DepthBarrier(vk::CommandBuffer command, vk::Image image, bool sampled)
{
    vk::ImageMemoryBarrier barrier;
    barrier.setImage(image).setSubresourceRange({vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1})
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setOldLayout(sampled ? vk::ImageLayout::eDepthStencilAttachmentOptimal :
            vk::ImageLayout::eDepthStencilReadOnlyOptimal)
        .setNewLayout(sampled ? vk::ImageLayout::eDepthStencilReadOnlyOptimal :
            vk::ImageLayout::eDepthStencilAttachmentOptimal)
        .setSrcAccessMask(sampled ? vk::AccessFlagBits::eDepthStencilAttachmentWrite :
            vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(sampled ? vk::AccessFlagBits::eShaderRead :
            vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite);
    command.pipelineBarrier(sampled ?
        vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests :
        vk::PipelineStageFlagBits::eComputeShader,
        sampled ? vk::PipelineStageFlagBits::eComputeShader :
        vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
        {}, {}, {}, barrier);
}

void WinnerBarrier(vk::CommandBuffer command, vk::Image image, bool sampled)
{
    vk::ImageMemoryBarrier barrier;
    barrier.setImage(image).setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setOldLayout(sampled ? vk::ImageLayout::eColorAttachmentOptimal :
            vk::ImageLayout::eShaderReadOnlyOptimal)
        .setNewLayout(sampled ? vk::ImageLayout::eShaderReadOnlyOptimal :
            vk::ImageLayout::eColorAttachmentOptimal)
        .setSrcAccessMask(sampled ? vk::AccessFlagBits::eColorAttachmentWrite : vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(sampled ? vk::AccessFlagBits::eShaderRead : vk::AccessFlagBits::eColorAttachmentWrite);
    command.pipelineBarrier(sampled ? vk::PipelineStageFlagBits::eColorAttachmentOutput :
        vk::PipelineStageFlagBits::eComputeShader, sampled ? vk::PipelineStageFlagBits::eComputeShader :
        vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {}, barrier);
}
}

bool TerrainCompositedQualityReady(uint64_t worldGeneration, const Eigen::Matrix4f& viewProjection)
{
    std::shared_ptr<TerrainQualityValidation> validation;
    std::shared_ptr<TerrainCompositedService> producer;
    {
        std::lock_guard<std::mutex> lock(observerMutex);
        validation = readinessValidation.lock();
        producer = readinessService.lock();
    }
    return validation && producer && producer->IsInteractiveReady() &&
        validation->Ready(worldGeneration, viewProjection, TerrainCompositedQualityInvalidation());
}

struct TerrainCompositedQualityFrame::State
{
    static std::weak_ptr<State> active;
    RendererBackendVulkan* backend = nullptr;
    std::optional<TerrainRvtBudgetLease> cpuLease;
    std::optional<TerrainRvtBudgetLease> frameLease;
    ShaderCompiler compiler;
    std::unique_ptr<PipelineFactory> factory;
    std::unique_ptr<TerrainCompositedQualityGpu> quality;
    std::unique_ptr<TerrainCompositedLoading> loading;
    TerrainCompositedFrameLifetime frameLifetimes;
    TerrainCompositedCaptureLifecycle captureLifecycle;
    uint64_t worldGeneration = 0;
    uint64_t graphGeneration = 0;
    uint64_t lastEpoch = 0;
    uint64_t pose = 1;
    Eigen::Matrix4f camera = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f captureCamera = Eigen::Matrix4f::Identity();
    std::shared_ptr<TerrainQualityValidation> validation = std::make_shared<TerrainQualityValidation>();
    TerrainQualityValidationStamp validationStamp;
    TerrainQualityValidationStamp captureStamp;
    std::filesystem::path output;
    bool failed = false;
    double lastAttemptMs = -1000;
    uint32_t exportedCaptures = 0;
    bool explicitArrivalInFrame = false;
    std::array<vk::ImageView, TerrainQualityAbi::Surfaces> pageTableViews{};
    size_t pageTableCount = 0;
    uint64_t presentGeneration = 0;
    vk::Extent2D renderExtent;
    TerrainQualityAbi::Counts lastCounts;
    uint64_t lastObservedEpoch = 0;
    uint64_t lastObservedPose = 0;
    uint64_t lastObservedResourceVersion = 0;
    bool failurePublished = false;
    bool blackEvidenceQueued = false;
    bool readyEvidenceQueued = false;
    uint64_t lastObservedSubmissionEpoch = 0;
    uint64_t lastObservedFrame = 0;

    ~State()
    {
        if (captureLifecycle.Pending())
            RuntimeEvidence::MarkIncomplete();
    }

    void CheckConvergenceFailure(const std::shared_ptr<TerrainCompositedService>& producer)
    {
        if (!producer || !producer->QualityConvergenceFailed())
            return;
        failed = true;
        validation->Revoke();
        const std::string message = "Terrain.QualityConvergenceFailed world=" +
            std::to_string(worldGeneration) + " pose=" + std::to_string(pose);
        if (!failurePublished)
        {
            failurePublished = true;
            const auto event = nlohmann::json{
                {"event", "quality_failed"}, {"errorCode", "Terrain.QualityConvergenceFailed"},
                {"failed", true}, {"worldGeneration", worldGeneration}, {"poseId", pose},
                {"monotonicMs", NowMs()},
                {"arrivedMonotonicMs", validation->PoseArrivedMonotonicMs()},
                {"lastObservedPoseId", lastObservedPose},
                {"lastObservedResourceVersion", lastObservedResourceVersion},
                {"countsCurrentValidation", lastObservedPose == pose &&
                    lastObservedResourceVersion == validationStamp.resourceVersion},
                {"completedEpoch", lastObservedEpoch}, {"visible", lastCounts.visibleSamples},
                {"validAtTargetPlusBias", lastCounts.validAtTargetPlusBias},
                {"unresolved", lastCounts.unresolved}, {"incorrectIdentity", lastCounts.incorrectIdentity},
                {"rootOnlySamples", lastCounts.rootOnlySamples}, {"complete", lastCounts.complete},
                {"requestedCaptures", quality ? quality->Requested() : 0},
                {"completedCaptures", quality ? quality->Completed() : 0},
                {"deferredCaptures", quality ? quality->Deferred() : 0}};
            ProcessLog().Append(LogSeverity::Error, "Terrain.Quality", message);
            std::cerr << "[RVT_QUALITY_FAILURE] " << event.dump() << std::endl;
            RuntimeEvidence::Record("RVT_QUALITY_FAILURE", event);
        }
        throw std::runtime_error(message);
    }

    void Consume(uint64_t completedEpoch, const std::shared_ptr<TerrainCompositedService>& producer)
    {
        if (!quality || failed) return;
        try
        {
            auto result = quality->TryConsume(completedEpoch, NowMs());
            if (!result) return;
            lastCounts = result->counts;
            lastObservedEpoch = result->completedEpoch;
            lastObservedSubmissionEpoch = result->submissionEpoch;
            lastObservedFrame = result->snapshot->frame;
            lastObservedPose = result->snapshot->poseId;
            lastObservedResourceVersion = captureStamp.resourceVersion;
            const auto sequence = TerrainQualityAbi::Join(result->words[TerrainQualityAbi::SequenceLow],
                result->words[TerrainQualityAbi::SequenceHigh]);
            const auto stem = output / ("capture-" + std::to_string(sequence));
            const bool exportRaw = !output.empty() && quality->ExportAvailable();
            if (exportRaw)
            {
                std::ofstream binary(stem.string() + ".bin", std::ios::binary);
                std::ofstream metadata(stem.string() + ".json");
                quality->WriteReadback(*result, binary, metadata);
                ++exportedCaptures;
            }
            const bool accepted = producer && producer->ObserveQuality(*result);
            validation->Observe(captureStamp, captureCamera,
                result->submissionEpoch, result->completedEpoch,
                result->completedMonotonicMs, result->counts, accepted);
            captureLifecycle.Completed(sequence, result->snapshot->worldGeneration,
                result->snapshot->poseId, result->snapshot->frame,
                result->submissionEpoch, result->completedEpoch, CaptureEvent);
            std::lock_guard<std::mutex> lock(observerMutex);
            observation = TerrainQualityObservation{result->snapshot->worldGeneration,
                result->snapshot->poseId, result->snapshot->frame, result->submissionEpoch,
                result->completedEpoch, result->completedMonotonicMs, result->counts};
            std::cout << "[RVT_QUALITY_CAPTURE] frame=" << result->snapshot->frame
                << " epoch=" << result->submissionEpoch
                << " binary=" << (exportRaw ? stem.string() + ".bin" : "not-exported")
                << " visible=" << result->counts.visibleSamples
                << " complete=" << result->counts.complete << '\n';
        }
        catch (const std::exception& error)
        {
            failed = true;
            validation->Revoke();
            RuntimeEvidence::MarkIncomplete();
            QualityFrameError(error);
            throw;
        }
    }
};

std::weak_ptr<TerrainCompositedQualityFrame::State> TerrainCompositedQualityFrame::State::active;

void TerrainCompositedQualityFrame::CloseAfterDeviceIdle(const char* reason)
{
    auto session = State::active.lock();
    if (!session)
        return;
    std::exception_ptr failure;
    try { session->Consume(session->lastEpoch, {}); }
    catch (...) { failure = std::current_exception(); }
    session->frameLifetimes.Complete(session->lastEpoch);
    if (session->captureLifecycle.Pending())
    {
        session->quality.reset();
        try { session->captureLifecycle.Close(reason, CaptureEvent); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    }
    State::active.reset();
    if (failure)
    {
        RuntimeEvidence::MarkIncomplete();
        std::rethrow_exception(failure);
    }
}

TerrainCompositedQualityFrame::TerrainCompositedQualityFrame() = default;
TerrainCompositedQualityFrame::~TerrainCompositedQualityFrame() { Abort(); }
TerrainCompositedQualityFrame::TerrainCompositedQualityFrame(TerrainCompositedQualityFrame&&) noexcept = default;
TerrainCompositedQualityFrame& TerrainCompositedQualityFrame::operator=(TerrainCompositedQualityFrame&&) noexcept = default;

void TerrainCompositedQualityFrame::SetDepthOwnerResolver(DepthOwnerResolver resolver)
{
    std::lock_guard<std::mutex> lock(observerMutex);
    depthOwnerResolver = std::move(resolver);
    failedSession.reset();
}

std::optional<TerrainQualityObservation> TerrainCompositedQualityFrame::TakeObservation(uint64_t generation)
{
    std::lock_guard<std::mutex> lock(observerMutex);
    if (!observation || observation->worldGeneration != generation) return {};
    return std::exchange(observation, {});
}

TerrainCompositedQualityFrame TerrainCompositedQualityFrame::BeginFrame(
    RenderFrameRecorderContext& context, const RendererFrameContext& frame,
    const std::shared_ptr<TerrainCompositedService>& service)
{
    TerrainCompositedQualityFrame transaction;
    auto session = State::active.lock();
    const uint64_t completedEpoch = std::max(frame.completedEpoch,
        ResourceRetireQueue::GetInstance().GetLastCompletedEpoch());
    if (session)
        session->frameLifetimes.Complete(completedEpoch);
    const char* destination = std::getenv("VULKANLEARN_RVT_QUALITY_DIR");
    if (context.offlineMode || !service || !service->HasSurfaces())
    {
        if (session)
            session->captureLifecycle.Close(context.offlineMode ? "offline_mode" : "world_unavailable",
                CaptureEvent);
        State::active.reset();
        std::lock_guard<std::mutex> lock(observerMutex);
        readinessValidation.reset();
        readinessService.reset();
        return transaction;
    }
    const auto& graph = RenderGraph::GetInstance();
    {
        std::lock_guard<std::mutex> lock(observerMutex);
        if (failedSession == std::make_pair(context.renderScene.worldGeneration, graph.GetOwnerGeneration()))
        {
            if (session)
                session->captureLifecycle.Close("session_failed", CaptureEvent);
            return transaction;
        }
    }
    if (session && (session->worldGeneration != context.renderScene.worldGeneration ||
        session->graphGeneration != graph.GetOwnerGeneration()))
    {
        session->Consume(completedEpoch, {});
        session->captureLifecycle.Close(
            session->worldGeneration != context.renderScene.worldGeneration ?
                "world_replaced" : "graph_replaced", CaptureEvent);
        session.reset();
    }
    if (!session)
    {
        session = std::make_shared<State>();
        session->backend = &context.backend;
        session->worldGeneration = context.renderScene.worldGeneration;
        session->graphGeneration = graph.GetOwnerGeneration();
        session->presentGeneration = context.backend.GetPresentTargetGeneration();
        session->renderExtent = context.renderExtent;
        session->frameLease = service->BudgetOwner()->TryReserve({0, 256 * 1024, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
        if (!session->frameLease)
            throw std::runtime_error("Quality frame state exceeds the frozen World CPU budget");
        if (destination && *destination)
            session->output = std::filesystem::absolute(destination);
        session->camera = context.renderScene.camera.viewProjection;
        State::active = session;
        QualityEvent("world_frame_arrived", session->worldGeneration, 0);
    }
    if (session->failed)
    {
        session->captureLifecycle.Close("session_failed", CaptureEvent);
        return transaction;
    }
    session->CheckConvergenceFailure(service);
    session->frameLifetimes.Prepare(frame.frameIndex);
    session->camera = context.renderScene.camera.viewProjection;
    const auto previousInvalidationSequence = session->validationStamp.invalidationSequence;
    session->validationStamp = session->validation->Begin(session->worldGeneration,
        session->graphGeneration, session->camera, TerrainCompositedQualityInvalidation(), NowMs());
    session->explicitArrivalInFrame =
        session->validationStamp.invalidationSequence != previousInvalidationSequence;
    const auto presentGeneration = context.backend.GetPresentTargetGeneration();
    if (session->presentGeneration != presentGeneration || session->renderExtent != context.renderExtent)
    {
        session->validationStamp = session->validation->BindingsChanged(session->camera,
            session->explicitArrivalInFrame ? session->validation->PoseArrivedMonotonicMs() : NowMs());
        session->presentGeneration = presentGeneration;
        session->renderExtent = context.renderExtent;
    }
    session->pose = session->validationStamp.poseId;
    service->SetQualityPose(session->worldGeneration, session->pose,
        session->validation->PoseArrivedMonotonicMs());
    session->Consume(completedEpoch, service);
    session->CheckConvergenceFailure(service);
    {
        std::lock_guard<std::mutex> lock(observerMutex);
        if (readinessValidation.lock() != session->validation || readinessService.lock() != service)
        {
            observation.reset();
            readinessService = service;
        }
        readinessValidation = session->validation;
    }
    transaction.state = std::move(session);
    transaction.service = service;
    transaction.frameNumber = context.currentFrame;
    transaction.drawEpoch = uint64_t(context.currentFrame) + 1;
    transaction.poseId = transaction.state->pose;
    transaction.renderWidth = context.renderExtent.width;
    transaction.renderHeight = context.renderExtent.height;
    transaction.frameSlot = frame.frameIndex;
    ResourceRetireQueue::GetInstance().RetireShared("TerrainQualityFrame",
        transaction.state->worldGeneration,
        transaction.drawEpoch + MAX_FRAMES_IN_FLIGHT, transaction.state);
    return transaction;
}

void TerrainCompositedQualityFrame::RecordAfterFeedback(PassRuntimeContext& context)
{
    if (!state || state->failed || handle || context.renderPass.type != RenderGraphPassType::TerrainFeedback)
        return;
    auto producer = service.lock();
    if (!producer) return;
    state->CheckConvergenceFailure(producer);
    bool recordingStarted = false;
    try
    {
        auto snapshot = producer->CaptureQualitySnapshot(frameNumber, poseId, drawEpoch, renderWidth, renderHeight);
        std::array<TerrainQualityAbi::Surface, TerrainQualityAbi::Surfaces> identities{};
        std::array<vk::ImageView, TerrainQualityAbi::Surfaces> pageTableViews{};
        if (snapshot->surfaces.size() > identities.size())
            throw std::runtime_error("Quality frame surface count exceeds the frozen ABI");
        for (size_t index = 0; index < snapshot->surfaces.size(); ++index)
        {
            identities[index] = snapshot->surfaces[index].data;
            if (!snapshot->surfaces[index].pageTable)
                throw std::runtime_error("Quality draw snapshot has no actual surface page table");
            pageTableViews[index] = snapshot->surfaces[index].pageTable->GetDescriptorInfo().imageView;
        }
        const double resourceArrivedMs = state->explicitArrivalInFrame
            ? state->validation->PoseArrivedMonotonicMs() : NowMs();
        if (state->pageTableCount &&
            (state->pageTableCount != snapshot->surfaces.size() ||
                state->pageTableViews != pageTableViews))
            state->validation->BindingsChanged(state->camera, resourceArrivedMs);
        state->pageTableViews = pageTableViews;
        state->pageTableCount = snapshot->surfaces.size();
        state->validationStamp = state->validation->Resources(
            identities.data(), snapshot->surfaces.size(), resourceArrivedMs);
        state->explicitArrivalInFrame = false;
        state->pose = poseId = state->validationStamp.poseId;
        snapshot->poseId = poseId;
        producer->SetQualityPose(state->worldGeneration, poseId,
            state->validation->PoseArrivedMonotonicMs());
        if (NowMs() - state->lastAttemptMs < 250 ||
            (state->quality && state->quality->Pending()))
            return;
        state->lastAttemptMs = NowMs();
        const auto& depth = context.renderGraph.GetResourcesResolve().at("terrainFeedbackDepth")
            .at(context.swapChainImageIndex);
        std::shared_ptr<const void> graphOwner;
        {
            std::lock_guard<std::mutex> lock(observerMutex);
            if (depthOwnerResolver) graphOwner = depthOwnerResolver(depth);
        }
        if (!graphOwner)
            throw std::runtime_error("Quality capture requires the graph resource's actual leaf lifetime owner");
        const auto& descriptions = context.renderGraph.GetCompiledRenderGraph().resources;
        const auto description = std::find_if(descriptions.begin(), descriptions.end(),
            [](const CompiledRenderGraphResource& row) { return row.name == "terrainFeedbackDepth"; });
        vk::ImageUsageFlags usage;
        if (description != descriptions.end())
            for (const auto& flag : description->usage)
            {
                if (flag == "sampled") usage |= vk::ImageUsageFlagBits::eSampled;
                if (flag == "depthStencilAttachment") usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
                if (flag == "transferSrc") usage |= vk::ImageUsageFlagBits::eTransferSrc;
                if (flag == "transferDst") usage |= vk::ImageUsageFlagBits::eTransferDst;
            }
        if (!(usage & vk::ImageUsageFlagBits::eSampled))
            throw std::runtime_error("Graph feedback depth was not created with sampled usage");
        snapshot->finalFeedbackDepth = {{depth.sampler, depth.GetShaderDescriptorView(),
            vk::ImageLayout::eDepthStencilReadOnlyOptimal}, std::move(graphOwner), depth.image,
            {depth.width, depth.height}, depth.format,
            usage,
            vk::SampleCountFlagBits::e1};
        if (!TerrainQualityDepthReadable(snapshot->finalFeedbackDepth))
            throw std::runtime_error("Quality replay requires an exact sampled single-sample D32 depth leaf");
        std::array<TerrainCompositedQualityDepthBinding, 2> winners;
        for (uint32_t index = 0; index < winners.size(); ++index)
        {
            const auto& image = context.renderGraph.GetResourcesResolve()
                .at(index == 0 ? "terrainQualityWinner0" : "terrainQualityWinner1")
                .at(context.swapChainImageIndex);
            std::shared_ptr<const void> owner;
            {
                std::lock_guard<std::mutex> lock(observerMutex);
                if (depthOwnerResolver) owner = depthOwnerResolver(image);
            }
            if (!owner || image.format != vk::Format::eR32G32B32A32Uint ||
                image.width != 128 || image.height != 128 || image.arrayLayers != 1)
                throw std::runtime_error("Quality winner requires exact graph-owned single-layer RGBA32Uint images");
            winners[index] = {{image.sampler, image.GetShaderDescriptorView(),
                vk::ImageLayout::eShaderReadOnlyOptimal}, std::move(owner), image.image,
                {image.width, image.height}, image.format,
                vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                vk::SampleCountFlagBits::e1};
        }
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            snapshot->cameraPosition[axis] = context.renderScene.camera.position[axis];
            snapshot->cameraLookAt[axis] = context.renderScene.camera.position[axis] +
                context.renderScene.camera.forward[axis];
        }
        if (!state->quality)
        {
            QualityEvent("pipeline_prepare_started", state->worldGeneration, poseId);
            const auto budget = producer->BudgetOwner();
            const auto root = std::filesystem::path(CommonFunction::GetProjectPath());
            const auto manifest = root / "shader/spv/shader-build-cache.json";
            const uint64_t manifestBytes = std::filesystem::exists(manifest) ? std::filesystem::file_size(manifest) : 0;
            if (manifestBytes > 4 * 1024 * 1024)
                throw std::runtime_error("Quality compiler manifest exceeds bounded CPU preparation domain");
            state->cpuLease = budget->TryReserve({0, 1024 * 1024 + manifestBytes * 16, 0, 0},
                TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
            if (!state->cpuLease) throw std::runtime_error("Quality compiler preparation exceeds World CPU budget");
            state->compiler.Initialize(root / "shader");
            state->factory = state->backend->CreatePipelineFactory();
            state->factory->SetShaderCompiler(&state->compiler);
            TerrainCompositedQualityLimits limits;
            limits.metadataOverhead = {TerrainQualityAbi::CaptureMetadataGpuBytes, 256 * 1024, 0, 0};
            for (const auto* name : {"terrainQualityWinner0", "terrainQualityWinner1"})
                for (const auto& image : context.renderGraph.GetResourcesResolve().at(name))
                {
                    std::shared_ptr<const void> owner;
                    {
                        std::lock_guard<std::mutex> lock(observerMutex);
                        if (depthOwnerResolver) owner = depthOwnerResolver(image);
                    }
                    if (!owner)
                        throw std::runtime_error("All swapchain winner images must retain their actual graph leaf");
                    vk::ImageCreateInfo create;
                    create.setImageType(vk::ImageType::e2D).setExtent({image.width, image.height, 1})
                        .setMipLevels(1).setArrayLayers(image.arrayLayers)
                        .setFormat(image.format).setSamples(vk::SampleCountFlagBits::e1)
                        .setTiling(vk::ImageTiling::eOptimal)
                        .setUsage(vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled)
                        .setSharingMode(vk::SharingMode::eExclusive).setInitialLayout(vk::ImageLayout::eUndefined);
                    limits.importedWinnerGpuBytes += state->backend->QueryImageMemoryRequirements(create).size;
                    limits.importedWinnerOwners.push_back(std::move(owner));
                }
            state->quality = std::make_unique<TerrainCompositedQualityGpu>(
                *state->backend, *state->factory, budget, limits);
            if (!state->output.empty())
                std::filesystem::create_directories(state->output);
            QualityEvent("pipeline_prepare_completed", state->worldGeneration, poseId);
        }
        recordingStarted = true;
        const auto captureWorld = snapshot->worldGeneration;
        const auto capturePose = snapshot->poseId;
        const auto captureFrame = snapshot->frame;
        const auto captureEpoch = snapshot->drawSubmissionEpoch;
        auto recorded = state->quality->Record(context.commandBuffer, std::move(snapshot), NowMs(), winners);
        if (!recorded)
        {
            recordingStarted = false;
            return;
        }
        handle = *recorded;
        state->captureLifecycle.Recorded(handle, captureWorld, capturePose, captureFrame, captureEpoch);
        state->captureStamp = state->validationStamp;
        state->captureCamera = state->camera;
        DepthBarrier(context.commandBuffer, depth.image, true);
        for (const auto& winner : winners)
            WinnerBarrier(context.commandBuffer, winner.image, true);
        state->quality->RecordResolveAndCopy(context.commandBuffer, handle);
        for (const auto& winner : winners)
            WinnerBarrier(context.commandBuffer, winner.image, false);
        DepthBarrier(context.commandBuffer, depth.image, false);
    }
    catch (const std::exception& error)
    {
        const bool commandsRecorded = recordingStarted;
        Abort();
        state->validation->Revoke();
        if (commandsRecorded)
        {
            state->failed = true;
            {
                std::lock_guard<std::mutex> lock(observerMutex);
                failedSession = std::make_pair(state->worldGeneration, state->graphGeneration);
            }
        }
        else if (!state->quality)
        {
            state->quality.reset();
            state->factory.reset();
            state->cpuLease.reset();
        }
        state->lastAttemptMs = NowMs();
        QualityFrameError(error);
        if (commandsRecorded) throw;
    }
}

void TerrainCompositedQualityFrame::RecordLoading(
    RenderFrameRecorderContext& context, vk::CommandBuffer command,
    const TerrainCompositedLoadingTarget& target)
{
    if (!state || context.offlineMode)
        return;
    auto producer = service.lock();
    if (!producer || !producer->HasSurfaces())
        return;
    const bool ready = TerrainCompositedQualityReady(state->worldGeneration, state->camera);
    auto queueEvidence = [&](bool loading)
    {
        const char* directory = std::getenv("VULKANLEARN_RVT_QUALITY_EVIDENCE_DIR");
        if (!directory || !*directory)
            return;
        const auto path = std::filesystem::absolute(directory) /
            (loading ? "first-black.bmp" : "first-ready.bmp");
        {
            std::lock_guard<std::mutex> lock(context.pendingScreenshotPathMutex);
            if (context.pendingScreenshotPath)
                throw std::runtime_error("Quality evidence conflicts with an existing screenshot request");
            context.pendingScreenshotPath = path.string();
        }
        const auto event = nlohmann::json{
            {"event", loading ? "loading_black_drawn" : "loading_released"},
            {"worldGeneration", state->worldGeneration}, {"poseId", poseId},
            {"frame", frameNumber}, {"submissionEpoch", drawEpoch},
            {"observationFrame", state->lastObservedFrame},
            {"observationSubmissionEpoch", state->lastObservedSubmissionEpoch},
            {"observationCompletedEpoch", state->lastObservedEpoch},
            {"monotonicMs", NowMs()}, {"ready", ready}, {"loading", loading},
            {"visibleSamples", state->lastCounts.visibleSamples},
            {"validAtTargetPlusBias", state->lastCounts.validAtTargetPlusBias},
            {"rootOnlySamples", state->lastCounts.rootOnlySamples},
            {"complete", state->lastCounts.complete},
            {"screenshot", path.string()}, {"screenshotState", "queued-for-this-submission"}};
        std::cout << "[RVT_QUALITY_PRESENT] " << event.dump() << std::endl;
        RuntimeEvidence::Record("RVT_QUALITY_PRESENT", event);
    };
    if (ready)
    {
        if (!state->readyEvidenceQueued)
        {
            queueEvidence(false);
            state->readyEvidenceQueued = true;
        }
        return;
    }
    if (!state->loading)
        state->loading = std::make_unique<TerrainCompositedLoading>(producer->BudgetOwner(),
            TerrainRvtCost{TerrainQualityAbi::LoadingMetadataEntryGpuBytes,
                TerrainQualityAbi::LoadingMetadataEntryCpuBytes, 0, 0});
    loadingLifetime = state->loading->Record(context.backend, command,
        target, drawEpoch, true, false);
    if (!state->blackEvidenceQueued)
    {
        queueEvidence(true);
        state->blackEvidenceQueued = true;
    }
}

void TerrainCompositedQualityFrame::Submitted(uint64_t epoch) noexcept
{
    if (!state) return;
    std::shared_ptr<const void> qualityLifetime;
    if (handle)
    {
        try
        {
            qualityLifetime = state->quality->Submitted(handle, epoch);
            state->captureLifecycle.Submitted(handle, epoch, CaptureEvent);
        }
        catch (const std::exception& error)
        {
            state->failed = true;
            state->validation->Revoke();
            RuntimeEvidence::MarkIncomplete();
            QualityFrameError(error);
        }
        handle = 0;
    }
    try
    {
        state->frameLifetimes.Submitted(frameSlot, epoch,
            std::move(qualityLifetime), std::move(loadingLifetime));
    }
    catch (const std::exception& error)
    {
        state->failed = true;
        state->validation->Revoke();
        QualityFrameError(error);
    }
    state->lastEpoch = epoch;
}

void TerrainCompositedQualityFrame::Abort() noexcept
{
    loadingLifetime.reset();
    if (!state || !handle) return;
    try
    {
        state->quality->Abort(handle);
        state->captureLifecycle.Close("frame_aborted", CaptureEvent);
    }
    catch (const std::exception& error)
    {
        RuntimeEvidence::MarkIncomplete();
        QualityFrameError(error);
    }
    catch (...) { RuntimeEvidence::MarkIncomplete(); }
    handle = 0;
}
}
