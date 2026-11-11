#include "engine/window/engineWindowLifecycle.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine/editorHost.h"
#include "engine/subsystemCollection.h"
#include "engine/worldGraphTransactionCoordinator.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/renderThread.h"
#include "render/rendergraph/preparedRenderGraphState.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderSystem.h"
#include "renderGraph.h"

namespace VL
{

namespace
{

RenderGraph::TestFaultInjection BuildRenderGraphTestFaultInjection(
    const WorldGraphTransactionTestFaultInjection& injection)
{
    RenderGraph::TestFaultInjection graphFault;
    graphFault.failResourceCreationAt =
        injection.failGraphResourceCreationAt;
    graphFault.failRenderPassCreationAt =
        injection.failRenderPassCreationAt;
    graphFault.failFramebufferCreationAt =
        injection.failFramebufferCreationAt;
    graphFault.failDescriptorCreationAt =
        injection.failDescriptorCreationAt;
    graphFault.failPassMaterialContract =
        injection.failPassMaterialContract;
    return graphFault;
}

} // namespace

void WaitForEngineRenderThreadIdle(
    const EngineWindowLifecycleContext& context)
{
    RenderThread* const& renderThread = context.renderThread;
    if (renderThread && renderThread->IsRunning())
    {
        renderThread->WaitUntilIdle();
        PollEngineRenderThreadFatalError(context);
    }
}

void WaitForEngineRenderFrameSlot(
    const EngineWindowLifecycleContext& context)
{
    RenderThread* const& renderThread = context.renderThread;
    if (renderThread && renderThread->IsRunning())
    {
        renderThread->WaitForFrameSlot();
    }
    PollEngineRenderThreadFatalError(context);
}

void PollEngineRenderThreadFatalError(
    const EngineWindowLifecycleContext& context)
{
    SubsystemCollection& subsystems = *context.subsystems;
    RenderThread* const& renderThread = context.renderThread;
    int& exitCode = *context.exitCode;
    bool& shouldClose = *context.shouldClose;
    if (!renderThread || !renderThread->HasFatalError())
    {
        return;
    }

    const std::string message = renderThread->ConsumeFatalError();
    subsystems.GetDiagnosticsSubsystem().ReportError(
        "Render thread failed: " + message);
    exitCode = 2;
    shouldClose = true;
}

RuntimeResult<void> RecreateEngineRendererForWindowResize(
    const EngineWindowLifecycleContext& context,
    uint32_t width,
    uint32_t height)
{
    SubsystemCollection& subsystems = *context.subsystems;
    RendererBackendVulkan* const& rendererBackend = context.rendererBackend;
    IEditorHost* const& editorHost = context.editorHost;
    nlohmann::json& runtimeRenderGraphJson = *context.runtimeRenderGraphJson;
    const WorldGraphTransactionTestFaultInjection&
        worldGraphTransactionTestFaultInjection = *context.testFaultInjection;
    bool& shouldClose = *context.shouldClose;
    if (width == 0 || height == 0)
    {
        subsystems.GetDiagnosticsSubsystem().ReportWarning(
            "Window resize ignored because the drawable size is zero.");
        return RuntimeResult<void>::Success();
    }

    if (rendererBackend == nullptr || !(*context.rendererBackendInitialized))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.ResizeBeforeRendererInit",
            "Cannot resize renderer resources before the renderer backend is initialized."));
    }

    bool liveResizeMutationStarted = false;
    try
    {
        WaitForEngineRenderThreadIdle(context);
        if (shouldClose)
        {
            return RuntimeResult<void>::Failure(MakeRuntimeError(
                "EngineLoop.RenderThreadFailedBeforeResize",
                "Cannot resize renderer resources because the render thread failed."));
        }

        rendererBackend->WaitIdle();

        RenderGraph& renderGraph = RenderGraph::GetInstance();
        const uint64_t graphOwnerGeneration =
            renderGraph.GetOwnerGeneration();
        auto passMaterialSnapshot = renderGraph.CapturePassMaterialInstances();

        liveResizeMutationStarted = true;
        if (editorHost) editorHost->ReleaseRendererResources();
        renderGraph.Shutdown(*rendererBackend);
        RenderSystem::GetInstance().ReleaseSwapchainDependentResources();
        rendererBackend->RecreateSwapchain(
            static_cast<int>(width),
            static_cast<int>(height));
        if (worldGraphTransactionTestFaultInjection
                .failResizeAfterSwapchainRecreate)
        {
            throw std::runtime_error(
                "Injected resize failure after swapchain recreation");
        }
        renderGraph.LoadRenderGraph(
            runtimeRenderGraphJson,
            *rendererBackend);
        renderGraph.RestorePassMaterialInstances(passMaterialSnapshot);
        renderGraph.SetOwnerGeneration(
            graphOwnerGeneration);
        RenderSystem::GetInstance().RebuildSwapchainDependentResources();
    }
    catch (const std::exception& exception)
    {
        if (liveResizeMutationStarted)
        {
            shouldClose = true;
        }
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.ResizeRecreateFailed",
            exception.what()));
    }

    return RuntimeResult<void>::Success();
}

RuntimeResult<void> ReloadEngineRenderGraphResources(
    const EngineWindowLifecycleContext& context,
    VL::RenderGraphReleaseMode releaseMode)
{
    RendererBackendVulkan* const& rendererBackend = context.rendererBackend;
    nlohmann::json& runtimeRenderGraphJson = *context.runtimeRenderGraphJson;
    const WorldGraphTransactionTestFaultInjection&
        worldGraphTransactionTestFaultInjection = *context.testFaultInjection;
    bool& shouldClose = *context.shouldClose;
    if (rendererBackend == nullptr || !(*context.rendererBackendInitialized))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.RenderGraphReloadBeforeRendererInit",
            "Cannot reload render graph resources before the renderer backend is initialized."));
    }

    try
    {
        WaitForEngineRenderThreadIdle(context);
        if (shouldClose)
        {
            return RuntimeResult<void>::Failure(MakeRuntimeError(
                "EngineLoop.RenderThreadFailedBeforeRenderGraphReload",
                "Cannot reload render graph resources because the render thread failed."));
        }

        (void)releaseMode;
        RenderGraph& renderGraph =
            RenderGraph::GetInstance();
        auto candidateGraph =
            std::make_shared<
                PreparedRenderGraphState>(
                *rendererBackend);
        candidateGraph->GetGraph()
            .SetTestFaultInjection(
                BuildRenderGraphTestFaultInjection(
                    worldGraphTransactionTestFaultInjection));
        candidateGraph->Load(
            runtimeRenderGraphJson);
        candidateGraph->GetGraph()
            .RestorePassMaterialInstances(
                renderGraph
                    .CapturePassMaterialInstances());
        candidateGraph->GetGraph()
            .SetOwnerGeneration(
                renderGraph
                    .GetOwnerGeneration());
        RenderSystem::GetInstance()
            .PrepareRenderGraphReload(
                candidateGraph->GetGraph());
        if (worldGraphTransactionTestFaultInjection
                .failBeforeCommit)
        {
            throw std::runtime_error(
                "Injected failure after RenderGraph reload prepare");
        }

        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const uint64_t ownerGeneration =
            renderGraph.GetOwnerGeneration();
        const uint64_t lastUsedEpoch =
            retireQueue.GetLastSubmittedEpoch();
        std::vector<RetiredResource>
            retirement;
        retirement.reserve(1);
        retirement.push_back({
            "RenderGraphReload:State",
            ownerGeneration,
            lastUsedEpoch,
            {}});
        PreparedResourceRetirements
            preparedRetirement =
                retireQueue.PrepareRetirements(
                    std::move(retirement));
        renderGraph.SwapState(
            candidateGraph->GetGraph());
        preparedRetirement
            .GetAdditionalResource(0)
            .resource =
            std::static_pointer_cast<void>(
                std::move(candidateGraph));
        retireQueue.CommitPreparedRetirements(
            std::move(preparedRetirement));
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EngineLoop.RenderGraphReloadFailed",
            exception.what()));
    }

    return RuntimeResult<void>::Success();
}

RuntimeResult<void> ReloadEngineRenderGraphResources(
    const EngineWindowLifecycleContext& context)
{
    return ReloadEngineRenderGraphResources(
        context, VL::RenderGraphReleaseMode::Retire);
}

} // namespace VL
