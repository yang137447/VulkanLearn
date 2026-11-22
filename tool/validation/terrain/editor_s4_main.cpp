#include "editor/editorHost.h"
#include "engine/application.h"
#include "engine/runtimeCommand.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/resource/terrainCompositedService.h"
#include "renderSystem.h"
#include "shader/build/contentHash.h"
#include "shader/build/atomicFile.h"
#include "commonFunction.h"
#include "materialInstance.h"
#include "render/offline/offlineWorldAssets.h"
#include "script/scriptRuntime.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainCompositedVT.h"
#include "terrain/terrainCompositedRefresh.h"
#include "terrain/terrainRvtBudget.h"
#include "world/world.h"
#include "sceneNode.h"
#include "render/frame/terrainCompositedReadiness.h"
#include "engine/frame/terrainLoadingGate.h"
#include "core/runtimeEvidence.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>

namespace
{
using namespace VL;

const char* StageNames[] = {
    "baseline", "paint", "paint_undo", "paint_redo",
    "sculpt", "sculpt_undo", "sculpt_redo", "rapid",
    "material", "material_revert", "material_reject", "conflict", "conflict_recover", "save"};
const char* ReopenStages[] = {"reopen", "reload"};

class AcceptanceHost final : public IEditorHost
{
public:
    RuntimeResult<void> Initialize(const EditorHostInit& value) override
    {
        init = value;
        const char* output = std::getenv("VL_EDITOR_S4_OUTPUT");
        const char* placement = std::getenv("VL_EDITOR_S4_PLACEMENT");
        if (!output || !placement)
            throw std::runtime_error("Set VL_EDITOR_S4_OUTPUT and VL_EDITOR_S4_PLACEMENT.");
        directory = std::filesystem::absolute(output);
        identity = placement;
        if (const char* plan = std::getenv("VL_EDITOR_S4_V2_PLAN"))
        {
            std::ifstream input(plan);
            v2Plan = nlohmann::json::parse(input);
            if (v2Plan.at("kind") != "editor-s4-v2-plan" || v2Plan.at("version") != 1)
                throw std::runtime_error("Invalid V2 editor acceptance plan");
        }
        if (const char* selected = std::getenv("VL_EDITOR_S4_MODE"))
            mode = selected;
        if (mode != "edit" && mode != "cook" && mode != "reopen")
            throw std::runtime_error("Unknown S4 mode.");
        TerrainCompositedService::SetAuthoringMode(true);
        if (const char* frames = std::getenv("VL_EDITOR_S4_FRAMES"))
            interval = std::stoul(frames);
        if (const char* milliseconds = std::getenv("VL_EDITOR_S4_STAGE_MS"))
            minimumStageMs = std::stoul(milliseconds);
        if (const char* wait = std::getenv("VL_EDITOR_S4_CAPTURE_WAIT_FRAMES"))
        {
            captureWaitLimit = std::stoul(wait);
            if (captureWaitLimit < 30 || captureWaitLimit > 10000)
                throw std::runtime_error("VL_EDITOR_S4_CAPTURE_WAIT_FRAMES must be 30..10000.");
        }
        if (interval < 30 || interval > 3600)
            throw std::runtime_error("VL_EDITOR_S4_FRAMES must be 30..3600.");
        std::filesystem::create_directories(directory);
        return host.Initialize(value);
    }

    void Tick(float delta, World* world) override
    {
        host.Tick(delta, world);
        ++hostTickSerial;
        if (!world || done || mode == "cook")
            return;
        if (!v2Plan.is_null())
        {
            if (v2Plan.value("diagnosticMode", "") == "single-paint-published")
            {
                TickPublishedDiagnostic(*world);
                return;
            }
            TickV2(*world);
            return;
        }
        const size_t stage = tick / interval;
        const size_t phase = tick % interval;
        const size_t stageCount = mode == "reopen" ? std::size(ReopenStages) : std::size(StageNames);
        if (stage >= stageCount)
        {
            RuntimeCommand quit;
            quit.type = RuntimeCommandType::QuitApplication;
            init.commandBus->Queue(quit);
            done = true;
            return;
        }
        const char* stageName = mode == "reopen" ? ReopenStages[stage] : StageNames[stage];
        const TerrainCollisionInstance* target = nullptr;
        for (const auto& candidate : world->GetTerrainCollisionInstances())
            if (candidate.placementId == identity)
                target = &candidate;
        if (!target || !target->plan.desc.compositedVT.enabled ||
            target->plan.desc.layers.size() < 2 || target->plan.desc.paintPath.empty())
            throw std::runtime_error("S4 needs the specified composited placement, two layers and paintPath.");
        if (phase == 0)
        {
            stageStarted = std::chrono::steady_clock::now();
            std::cout << "EDITOR_S4_STAGE " << stageName << std::endl;
        }
        ObserveFeedback(stageName, phase, world->GetGeneration());
        const size_t capturePhase = interval >= 40 ? interval - 8 : interval - 4;
        const bool holding = phase == capturePhase &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stageStarted).count() < minimumStageMs;
        if (mode == "edit" && stage == 7 && phase <= RapidStamps)
        {
            CanonicalFieldHasher hash("editor-s4-stamp");
            if (target->plan.layerPaint)
            {
                const auto& paint = *target->plan.layerPaint;
                hash.AddBytes("weights", paint.weights.data(), paint.weights.size() * sizeof(float));
                hash.AddBytes("opacity", paint.opacity.data(), paint.opacity.size());
            }
            std::cout << "EDITOR_S4_STAMP " << nlohmann::json({
                {"completedStamp", int(phase) - 1}, {"worldHash", hash.Finalize().ToHex()},
                {"hostTickSerial", hostTickSerial},
                {"operation", phase == 0 ? "before" : (phase % 2 ? "erase" : "paint")}}).dump() << std::endl;
        }
        if (phase == 0)
        {
            if (mode == "reopen")
            {
                if (stage == 1)
                {
                    RuntimeCommand reload;
                    reload.type = RuntimeCommandType::LoadWorld;
                    reload.stringValue = world->GetScenePath();
                    init.commandBus->Queue(std::move(reload));
                }
            }
            else if (stage == 1)
                Stroke(*world, true, 0.5f);
            else if (stage == 7)
                Stroke(*world, true, 0.5f, true);
            else if (stage == 2 || stage == 3)
                Action(*world, true, stage == 2 ? TerrainBrushOperation::Undo : TerrainBrushOperation::Redo);
            else if (stage == 4)
                Stroke(*world, false, 0.5f);
            else if (stage == 5 || stage == 6)
                Action(*world, false, stage == 5 ? TerrainBrushOperation::Undo : TerrainBrushOperation::Redo);
            else if (stage == 8 || stage == 9 || stage == 10)
                Preview(*world, *target, stage);
            else if (stage == 11)
                External(*target, false);
            else if (stage == 12)
                External(*target, true);
            else if (stage == 13)
            {
                Action(*world, true, TerrainBrushOperation::SavePaint);
                Action(*world, false, TerrainBrushOperation::SaveHeightmap);
            }
        }
        if (mode == "edit" && stage == 7 && phase > 0 && phase < RapidStamps)
            Stroke(*world, true, 0.5f, phase % 2 == 0);

        if (phase >= 2 && !holding)
        {
            const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
            std::cout << "EDITOR_S4_SAMPLE " << stageName << " " << phase << std::endl;
            if (!streaming || !streaming->composited)
                throw std::runtime_error("No composited streaming service.");
            streaming->composited->ReportStats();
        }
        if (holding)
            return;
        if (phase == capturePhase)
            init.renderSystem->RequestScreenshot((directory / (std::string(stageName) + ".bmp")).string());
        if (phase == interval - 1)
        {
            CanonicalFieldHasher height("editor-s4-height");
            const auto& image = *target->plan.heightmap;
            height.AddUInt32("width", image.width);
            height.AddUInt32("height", image.height);
            std::vector<float> samples;
            samples.reserve(size_t(image.width) * image.height);
            for (uint32_t row = 0; row < image.height; ++row)
                for (uint32_t column = 0; column < image.width; ++column)
                    samples.push_back(TerrainHeightmap::ReadNormalizedPixel(image, column, row));
            height.AddBytes("normalized", samples.data(), samples.size() * sizeof(float));
            CanonicalFieldHasher persisted("editor-s4-height16");
            persisted.AddUInt32("width", image.width);
            persisted.AddUInt32("height", image.height);
            std::vector<uint16_t> quantized;
            quantized.reserve(samples.size());
            for (const auto sample : samples)
                quantized.push_back(static_cast<uint16_t>(std::clamp(std::lround(sample * 65535.0f), 0l, 65535l)));
            persisted.AddBytes("normalized16", quantized.data(), quantized.size() * sizeof(uint16_t));
            CanonicalFieldHasher paint("editor-s4-paint");
            if (target->plan.layerPaint && std::any_of(
                    target->plan.layerPaint->opacity.begin(), target->plan.layerPaint->opacity.end(),
                    [](uint8_t value) { return value != 0; }))
            {
                const auto& value = *target->plan.layerPaint;
                paint.AddBytes("weights", value.weights.data(), value.weights.size() * sizeof(float));
                paint.AddBytes("opacity", value.opacity.data(), value.opacity.size());
            }
            const auto layout = TerrainCompositedVTLayout(identity,
                target->plan.desc.compositedVT.virtualResolution.x(),
                target->plan.desc.compositedVT.virtualResolution.y());
            std::cout << "EDITOR_S4_STATE " << nlohmann::json({
                {"stage", stageName}, {"height", height.Finalize().ToHex()},
                {"height16", persisted.Finalize().ToHex()},
                {"paint", paint.Finalize().ToHex()}, {"totalPages", layout.PageCount()},
                {"placement", identity}, {"generation", world->GetGeneration()},
                {"liveUv", LiveUv(*world, *target)},
                {"materialDiskHash", ContentHasher::HashFile(
                    std::filesystem::path(CommonFunction::GetResourcePath()) /
                    target->plan.desc.materialInstancePath).ToHex()}}).dump() << std::endl;
        }
        ++tick;
    }

    void Shutdown() override { preview.reset(); host.Shutdown(); }
    void ReleaseRendererResources() override { preview.reset(); host.ReleaseRendererResources(); }
    void BeginUiActionBatch() override { host.BeginUiActionBatch(); }
    bool ApplyUiAction(const UiAction& action, World* world, RuntimeCommandExecutionResult& result) override
    { return host.ApplyUiAction(action, world, result); }
    void EndUiActionBatch(World* world) override { host.EndUiActionBatch(world); }
    bool ApplyTerrainEditUpdate(World& world) override { return host.ApplyTerrainEditUpdate(world); }
    void TickThumbnails(const std::vector<PlatformEvent>& events) override { host.TickThumbnails(events); }
    bool CanUndo(const std::string& path, bool redo) const override { return host.CanUndo(path, redo); }
    nlohmann::json DraftSceneDocument(const std::string& path, std::string& error) override
    { return host.DraftSceneDocument(path, error); }
    bool PrepareActorEdit(const std::string& path, const std::string& actor, std::string& error) override
    { return host.PrepareActorEdit(path, actor, error); }
    void RecordActorEdit(World& world, const ActorPropertyEdit& edit) override { host.RecordActorEdit(world, edit); }
    void ReplaySceneEditsAfterWorldChange(const std::string& path, uint64_t generation, World& world) override
    { host.ReplaySceneEditsAfterWorldChange(path, generation, world); }
    void ResetAuthoringSessions() override { host.ResetAuthoringSessions(); }
    std::shared_ptr<const void> BuildSceneActorSnapshots(const World& world) const override
    { return host.BuildSceneActorSnapshots(world); }
    std::shared_ptr<const void> BuildSceneModelSnapshots() const override { return host.BuildSceneModelSnapshots(); }
    void BuildUiSections(const World* world) override { host.BuildUiSections(world); }
    bool SelectSceneObjectFromPick(const MaterialInstanceSelection& value) override
    { return host.SelectSceneObjectFromPick(value); }
    void ClearMaterialInstanceSceneSelection() override { host.ClearMaterialInstanceSceneSelection(); }
    IEditorConsoleSink* ConsoleSink() override { return host.ConsoleSink(); }
    IEditorViewportOverlay* ViewportOverlay() override { return host.ViewportOverlay(); }
    bool IsEditorFlavor() const override { return host.IsEditorFlavor(); }
    bool IsEnabled() const override { return host.IsEnabled(); }
    bool IsVisible() const override { return host.IsVisible(); }

private:
    struct FrozenObservation
    {
        std::shared_ptr<const TerrainLayerPaintData> paint;
        std::shared_ptr<const HostImage> height;
        std::string digest;
        std::string surface;
        uint64_t generation = 0;
    };

    static std::string InputDigest(
        const std::shared_ptr<const TerrainLayerPaintData>& paint,
        const std::shared_ptr<const HostImage>& height)
    {
        CanonicalFieldHasher hash("editor-s4-v2-immutable-input");
        if (paint)
        {
            hash.AddBytes("weights", paint->weights.data(), paint->weights.size() * sizeof(float));
            hash.AddBytes("opacity", paint->opacity.data(), paint->opacity.size());
        }
        if (height)
            hash.AddBytes("height", height->data.data(), height->data.size());
        return hash.Finalize().ToHex();
    }

    static bool CompleteScreenshot(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        unsigned char header[14]{};
        if (!input.read(reinterpret_cast<char*>(header), sizeof(header)) ||
            header[0] != 'B' || header[1] != 'M')
            return false;
        const uint32_t bytes = uint32_t(header[2]) | uint32_t(header[3]) << 8 |
            uint32_t(header[4]) << 16 | uint32_t(header[5]) << 24;
        return bytes >= 54 && std::filesystem::file_size(path) == bytes;
    }

    void DiagnosticEvent(nlohmann::json event)
    {
        event["phase"] = diagnosticPhase;
        event["hostTickSerial"] = hostTickSerial;
        event["monotonicMs"] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::cout << "EDITOR_S4_DIAGNOSTIC " << event.dump() << std::endl;
        RuntimeEvidence::Record("EDITOR_S4_DIAGNOSTIC", event);
    }

    // 截图节拍证据：记录等待的是引擎自己的地形加载门（IsTerrainWorldLoading ->
    // TerrainCompositedQualityReady），而不是测试端猜测的黑帧阈值。
    void CaptureEvent(nlohmann::json event)
    {
        event["monotonicMs"] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::cout << "EDITOR_S4_CAPTURE " << event.dump() << std::endl;
        RuntimeEvidence::Record("EDITOR_S4_CAPTURE", event);
    }

    void FinishDiagnostic(bool success, const std::string& reason)
    {
        DiagnosticEvent({{"event", "outcome"}, {"success", success}, {"reason", reason}});
        RuntimeCommand quit;
        quit.type = RuntimeCommandType::QuitApplication;
        init.commandBus->Queue(std::move(quit));
        done = true;
    }

    void TickPublishedDiagnostic(World& world)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
        if (!streaming || !streaming->composited)
            throw std::runtime_error("Diagnostic requires the real composited Service");
        auto& service = *streaming->composited;
        const auto camera = world.GetRenderCamera();
        const bool matchingQuality = camera && TerrainCompositedQualityReady(
            world.GetGeneration(), camera->GetProjectionMatrix() * camera->GetViewMatrix());
        identity = v2Plan.at("surfaceA").get<std::string>();
        const auto target = std::find_if(world.GetTerrainCollisionInstances().begin(),
            world.GetTerrainCollisionInstances().end(),
            [&](const auto& value) { return value.placementId == identity; });
        if (target == world.GetTerrainCollisionInstances().end())
            throw std::runtime_error("Diagnostic placement is missing");
        if (diagnosticPhase.empty())
        {
            diagnosticPhase = "baseline-ready";
            diagnosticStarted = now;
            diagnosticWorld = world.GetGeneration();
            diagnosticBaseline = InputDigest(target->plan.layerPaint, target->plan.heightmap);
            frozenObservations.push_back({target->plan.layerPaint, target->plan.heightmap,
                diagnosticBaseline, identity, diagnosticWorld});
            DiagnosticEvent({{"event", "begin"}, {"worldGeneration", diagnosticWorld},
                {"inputIdentity", diagnosticBaseline}, {"waitSeconds", 120},
                {"scope", "Editor diagnostic watchdog, not Game pose latency acceptance"}});
        }
        for (const auto& frozen : frozenObservations)
            if (InputDigest(frozen.paint, frozen.height) != frozen.digest)
            {
                FinishDiagnostic(false, "Old held World input mutated");
                return;
            }
        if (world.GetGeneration() != diagnosticWorld || service.QualityConvergenceFailed())
        {
            FinishDiagnostic(false, "World changed or native quality convergence failed");
            return;
        }
        const auto budget = service.BudgetOwner()->Statistics();
        if (budget.rejected)
        {
            FinishDiagnostic(false, "Native budget rejection");
            return;
        }
        if (now - diagnosticStarted >= std::chrono::seconds(120))
        {
            service.ReportStats();
            FinishDiagnostic(false, "120s bounded diagnostic phase wait expired");
            return;
        }
        if (now - diagnosticLastReport >= std::chrono::seconds(1))
        {
            service.ReportStats();
            DiagnosticEvent({{"event", "sample"}, {"worldGeneration", diagnosticWorld},
                {"publishedRevision", service.RefreshRevision(diagnosticWorld, identity)},
                {"qualityReady", matchingQuality}, {"serviceInteractiveReady", service.IsInteractiveReady()},
                {"inputIdentity", InputDigest(target->plan.layerPaint, target->plan.heightmap)},
                {"oldSnapshotIntact", true}, {"gpuCurrentBytes", budget.current.gpuBytes},
                {"gpuPeakBytes", budget.peak.gpuBytes}, {"cpuPeakBytes", budget.peak.cpuBytes}});
            diagnosticLastReport = now;
        }
        if (diagnosticPhase == "baseline-ready")
        {
            if (!matchingQuality)
                return;
            diagnosticBaseRevision = service.RefreshRevision(diagnosticWorld, identity);
            init.renderSystem->RequestScreenshot((directory / "baseline.bmp").string());
            diagnosticPhase = "baseline-capture";
            diagnosticStarted = now;
            return;
        }
        if (diagnosticPhase == "baseline-capture" || diagnosticPhase == "paint-capture")
        {
            const auto name = diagnosticPhase == "baseline-capture" ? "baseline.bmp" : "paint.bmp";
            if (!CompleteScreenshot(directory / name))
                return;
            diagnosticTickets.clear();
            diagnosticPhase = diagnosticPhase == "baseline-capture" ? "paint-wait-published" : "undo-wait-published";
            diagnosticStarted = now;
            diagnosticExpectedRevision = service.RefreshRevision(diagnosticWorld, identity);
            if (diagnosticPhase == "paint-wait-published")
                Stroke(world, true, 0.5f);
            else
                Action(world, true, TerrainBrushOperation::Undo);
            DiagnosticEvent({{"event", "command-queued"}, {"worldGeneration", diagnosticWorld},
                {"expectedRevision", diagnosticExpectedRevision}});
            return;
        }
        if (diagnosticPhase == "undo-capture")
        {
            if (CompleteScreenshot(directory / "undo.bmp"))
                FinishDiagnostic(true, "Paint and Undo each reached real Published and complete capture");
            return;
        }
        for (const auto& observation : host.ObserveTerrainRefreshes(diagnosticWorld, identity))
        {
            if (observation.status.ticket.editSeq <= diagnosticLastPublishedSeq)
                continue;
            diagnosticTickets.insert_or_assign(observation.status.ticket.id, observation);
        }
        EditorHost::TerrainRefreshObservation* latest = nullptr;
        for (auto& entry : diagnosticTickets)
        {
            auto& observation = entry.second;
            const auto status = service.PollRefresh(observation.status.ticket);
            const auto previous = diagnosticStates.find(status.ticket.id);
            if (previous == diagnosticStates.end() || previous->second != status.state)
            {
                DiagnosticEvent({{"event", "ticket-poll"}, {"ticket", status.ticket.id},
                    {"worldGeneration", status.ticket.worldGeneration}, {"surface", status.ticket.surfaceId},
                    {"editSeq", status.ticket.editSeq}, {"sourceRevision", observation.sourceRevision},
                    {"expectedRevision", observation.expectedRevision}, {"state", int(status.state)},
                    {"publishedRevision", status.publishedRevision}, {"diagnostic", status.diagnostic},
                    {"inputIdentity", observation.inputIdentity.ToHex()}});
                diagnosticStates[status.ticket.id] = status.state;
            }
            observation.status = status;
            if (status.state == TerrainCompositedRefreshState::Failed || observation.terminalFailure)
            {
                FinishDiagnostic(false, "Actual ticket failed: " + status.diagnostic);
                return;
            }
            if (!latest || status.ticket.editSeq > latest->status.ticket.editSeq)
                latest = &observation;
        }
        if (!latest)
            return;
        const auto& status = latest->status;
        if (status.state == TerrainCompositedRefreshState::Stale)
        {
            FinishDiagnostic(false, "Actual latest ticket became stale: " + status.diagnostic);
            return;
        }
        const auto input = InputDigest(target->plan.layerPaint, target->plan.heightmap);
        const bool restored = diagnosticPhase == "undo-wait-published";
        if (!status.Acknowledges(status.ticket) ||
            status.publishedRevision <= diagnosticExpectedRevision ||
            service.RefreshRevision(diagnosticWorld, identity) < status.publishedRevision ||
            !matchingQuality)
            return;
        if (input != latest->inputIdentity.ToHex() ||
            (restored ? input != diagnosticBaseline : input == diagnosticBaseline))
        {
            FinishDiagnostic(false, "Published ticket input binding or Undo restoration mismatch");
            return;
        }
        diagnosticLastPublishedSeq = status.ticket.editSeq;
        DiagnosticEvent({{"event", "published-gate"}, {"ticket", status.ticket.id},
            {"editSeq", status.ticket.editSeq}, {"publishedRevision", status.publishedRevision},
            {"inputIdentity", input}, {"restored", restored}, {"oldSnapshotIntact", true}});
        service.ReportStats();
        init.renderSystem->RequestScreenshot((directory / (restored ? "undo.bmp" : "paint.bmp")).string());
        diagnosticPhase = restored ? "undo-capture" : "paint-capture";
        diagnosticStarted = now;
    }

    // 测试节拍：步骤末截图必须等到引擎自己的地形加载门释放后才请求。
    // 旧 harness 在固定 phase=interval-8 直接请求，绘制快照可能仍是
    // "Loading terrain..." 黑帧（门在渲染快照之后才在同一 GT tick 内释放），
    // 因而 undo/redo 像素比较量到的是加载态，而不是地形状态。
    // 返回 true 表示保持当前 phase 不前进（引擎继续 tick），等门/截图落盘。
    bool HoldCaptureUntilReady(World& world, const std::string& label, size_t stage, size_t phase)
    {
        const size_t capturePhase = interval >= 40 ? interval - 8 : interval - 4;
        if (phase < capturePhase)
            return false;
        if (captureStage != stage)
        {
            captureStage = stage;
            captureState = CaptureState::Waiting;
            captureStartedSerial = hostTickSerial;
            captureFalseStreak = 0;
        }
        if (captureState == CaptureState::Complete)
            return false;
        const auto path = directory / (label + ".bmp");
        const uint64_t waited = hostTickSerial - captureStartedSerial;
        // 连续两帧观察到门已释放才请求：engineFramePhases 在本 tick 的
        // editorHost->Tick 之前已用上一帧的 readiness 生成渲染快照，单帧
        // false 仍可能对应上一帧的 loading 快照。
        const bool loading = IsTerrainWorldLoading(&world, init.renderSystem->IsOfflineMode());
        if (captureState == CaptureState::Waiting)
        {
            captureFalseStreak = loading ? 0 : captureFalseStreak + 1;
            const bool expired = waited >= captureWaitLimit;
            if ((!loading && captureFalseStreak >= 2) || expired)
            {
                init.renderSystem->RequestScreenshot(path.string());
                captureState = CaptureState::Requested;
                captureRequestedSerial = hostTickSerial;
                CaptureEvent({{"event", "capture_requested"}, {"step", label},
                    {"gate", "IsTerrainWorldLoading(world, offlineMode)"},
                    {"worldGeneration", world.GetGeneration()}, {"phase", phase},
                    {"hostTickSerial", hostTickSerial}, {"waitedFrames", waited},
                    {"loading", loading}, {"expired", expired},
                    {"waitLimitFrames", captureWaitLimit}});
            }
        }
        if (captureState == CaptureState::Requested && CompleteScreenshot(path))
        {
            captureState = CaptureState::Complete;
            CaptureEvent({{"event", "capture_complete"}, {"step", label},
                {"worldGeneration", world.GetGeneration()}, {"phase", phase},
                {"hostTickSerial", hostTickSerial}, {"waitedFrames", waited},
                {"captureFrames", hostTickSerial - captureRequestedSerial}});
            return false;
        }
        // 第二次兜底：请求已发出但文件迟迟不完整时不能永久占住阶段。
        if (captureState == CaptureState::Requested && !CompleteScreenshot(path) &&
            waited >= captureWaitLimit + 300)
        {
            CaptureEvent({{"event", "capture_timeout"}, {"step", label},
                {"worldGeneration", world.GetGeneration()}, {"phase", phase},
                {"hostTickSerial", hostTickSerial}, {"waitedFrames", waited},
                {"screenshotComplete", false}, {"waitLimitFrames", captureWaitLimit}});
            captureState = CaptureState::Complete;
            return false;
        }
        // 只在本阶段最后一帧之后保持 phase；更早的等待仍在 180 帧预算内推进。
        return phase + 1 >= interval;
    }

    void TickV2(World& world)
    {
        const auto& steps = v2Plan.at("steps");
        const size_t stage = tick / interval;
        const size_t phase = tick % interval;
        if (stage >= steps.size())
        {
            RuntimeCommand quit;
            quit.type = RuntimeCommandType::QuitApplication;
            init.commandBus->Queue(std::move(quit));
            done = true;
            return;
        }
        const auto& step = steps.at(stage);
        const auto label = step.at("id").get<std::string>();
        for (const auto& frozen : frozenObservations)
            if (InputDigest(frozen.paint, frozen.height) != frozen.digest)
                throw std::runtime_error("Published World input snapshot mutated: " + frozen.surface);
        if (phase == 0)
        {
            std::cout << "EDITOR_S4_V2 " << nlohmann::json({
                {"event", "step"}, {"step", label}, {"generation", world.GetGeneration()},
                {"hostTickSerial", hostTickSerial}}).dump() << std::endl;
            for (const auto& candidate : world.GetTerrainCollisionInstances())
                if (candidate.placementId == v2Plan.at("surfaceA") ||
                    candidate.placementId == v2Plan.at("surfaceB"))
                {
                    const auto matching = std::count_if(frozenObservations.begin(), frozenObservations.end(),
                        [&](const auto& frozen) { return frozen.surface == candidate.placementId; });
                    if (matching >= 2)
                    {
                        const auto oldest = std::find_if(frozenObservations.begin(), frozenObservations.end(),
                            [&](const auto& frozen) { return frozen.surface == candidate.placementId; });
                        frozenObservations.erase(oldest);
                    }
                    frozenObservations.push_back({candidate.plan.layerPaint, candidate.plan.heightmap,
                        InputDigest(candidate.plan.layerPaint, candidate.plan.heightmap),
                        candidate.placementId, world.GetGeneration()});
                }
            for (const auto& operation : step.at("operations"))
            {
                const auto kind = operation.at("kind").get<std::string>();
                if (kind == "paint" || kind == "sculpt")
                {
                    identity = operation.at("surface").get<std::string>();
                    const auto name = operation.at("operation").get<std::string>();
                    const std::map<std::string, TerrainBrushOperation> names{
                        {"begin", TerrainBrushOperation::BeginStroke},
                        {"apply", TerrainBrushOperation::ApplyBrush},
                        {"end", TerrainBrushOperation::EndStroke},
                        {"cancel", TerrainBrushOperation::CancelStroke},
                        {"undo", TerrainBrushOperation::Undo}, {"redo", TerrainBrushOperation::Redo},
                        {"savePaint", TerrainBrushOperation::SavePaint},
                        {"saveHeight", TerrainBrushOperation::SaveHeightmap}};
                    overrideGeneration = operation.value("oldGeneration", false) ? initialGeneration : 0;
                    brushV = operation.value("v", 0.5f);
                    Action(world, kind == "paint", names.at(name),
                        operation.value("u", 0.5f), operation.value("erase", false));
                    overrideGeneration = 0;
                }
                else if (kind == "reloadWorld")
                {
                    initialGeneration = world.GetGeneration();
                    RuntimeCommand reload;
                    reload.type = RuntimeCommandType::LoadWorld;
                    reload.stringValue = world.GetScenePath();
                    init.commandBus->Queue(std::move(reload));
                }
                else if (kind == "hotReload" || kind == "hotReloadRestore")
                {
                    const auto project = std::filesystem::path(CommonFunction::GetProjectPath());
                    if (!std::filesystem::is_regular_file(project / "editor-s4-v2-marker.json"))
                        throw std::runtime_error("Shader mutation requires a private V2 stage marker");
                    const auto source = project / "shader/glsl/terrain/terrainCompositedBakeWorld.comp";
                    if (kind == "hotReload")
                    {
                        const auto bytes = ReadBinaryFile(source);
                        shaderBaseline.assign(bytes.begin(), bytes.end());
                        WriteTextFileAtomically(source, shaderBaseline + "\n");
                    }
                    else
                        WriteTextFileAtomically(source, shaderBaseline);
                    RuntimeCommand reload;
                    reload.type = RuntimeCommandType::ReloadShaders;
                    reload.shaderReloadScope = RuntimeShaderReloadScope::Changed;
                    init.commandBus->Queue(std::move(reload));
                }
                else if (kind == "material" || kind == "materialRestore" || kind == "materialReject")
                {
                    const auto found = std::find_if(world.GetTerrainCollisionInstances().begin(),
                        world.GetTerrainCollisionInstances().end(), [&](const auto& candidate) {
                            return candidate.placementId == v2Plan.at("surfaceA");
                        });
                    if (found == world.GetTerrainCollisionInstances().end())
                        throw std::runtime_error("Missing V2 material preview surface");
                    Preview(world, *found, kind == "material" ? 8 : kind == "materialRestore" ? 9 : 10);
                }
                else
                    throw std::runtime_error("Unknown V2 editor acceptance operation");
            }
        }
        if (phase == interval - 1 && snapshotStage != stage)
        {
            snapshotStage = stage;
            const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
            if (!streaming || !streaming->composited)
                throw std::runtime_error("V2 evidence requires the actual composited Service");
            for (const auto& candidate : world.GetTerrainCollisionInstances())
            {
                if (candidate.placementId != v2Plan.at("surfaceA") &&
                    candidate.placementId != v2Plan.at("surfaceB"))
                    continue;
                const auto baseline = streaming->composited->LoadAuthorBaseline(
                    world.GetGeneration(), candidate.placementId);
                const auto baselinePaint = candidate.plan.layerPaint;
                const auto baselineHeight = candidate.plan.heightmap;
                const auto resourceRoot = std::filesystem::path(CommonFunction::GetResourcePath());
                const auto fileDigest = [&](const std::string& path) {
                    return !path.empty() && std::filesystem::is_regular_file(resourceRoot / path) ?
                        ContentHasher::HashFile(resourceRoot / path).ToHex() : std::string{};
                };
                std::cout << "EDITOR_S4_V2 " << nlohmann::json({
                    {"event", "snapshot"}, {"step", label}, {"generation", world.GetGeneration()},
                    {"surface", candidate.placementId}, {"hostTickSerial", hostTickSerial},
                    {"inputDigest", InputDigest(baselinePaint, baselineHeight)},
                    {"paintDigest", InputDigest(baselinePaint, {})},
                    {"heightDigest", InputDigest({}, baselineHeight)},
                    {"publishedRevision", streaming->composited->RefreshRevision(
                        world.GetGeneration(), candidate.placementId)},
                    {"productionIdentity", baseline.productionIdentity.ToHex()},
                    {"authorHeightDigest", fileDigest(candidate.plan.desc.heightmapPath)},
                    {"authorPaintDigest", fileDigest(candidate.plan.desc.paintPath)},
                    {"gameManifestDigest", fileDigest(candidate.plan.desc.compositedVT.packagePath)},
                    {"immutableHeldSnapshots", frozenObservations.size()},
                    {"immutableWorldInputsIntact", true}}).dump() << std::endl;
            }
            std::cout << "EDITOR_S4_V2 " << nlohmann::json({
                {"event", "feedback"}, {"step", label}, {"generation", world.GetGeneration()},
                {"message", init.scripts ? init.scripts->View(world.GetGeneration()).feedback : ""}})
                .dump() << std::endl;
            streaming->composited->ReportStats();
        }
        if (HoldCaptureUntilReady(world, label, stage, phase))
            return;
        ++tick;
    }

    void ObserveFeedback(const char* stageName, size_t phase, uint64_t generation)
    {
        if (!init.scripts)
            return;
        const auto feedback = init.scripts->View(generation).feedback;
        const bool changed = feedback != lastFeedback;
        if (changed || phase == 0 || phase == interval - 1)
        {
            const auto now = std::chrono::steady_clock::now();
            std::cout << "EDITOR_S4_FEEDBACK " << nlohmann::json({
                {"stage", stageName}, {"message", feedback}, {"feedbackChanged", changed},
                {"phase", phase}, {"hostTickSerial", hostTickSerial},
                {"observation", "afterHostTick-beforeProducer"},
                {"stageElapsedMs", std::chrono::duration_cast<std::chrono::milliseconds>(now - stageStarted).count()},
                {"elapsedMs", std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count()}}).dump() << std::endl;
        }
        lastFeedback = feedback;
    }

    float LiveUv(const World& world, const TerrainCollisionInstance& target, const char* parameter = "u_uvScale")
    {
        const auto capture = RendererResourceCache::GetInstance().CaptureMaterialInstanceForGeneration(
            world.GetGeneration(), target.plan.desc.materialInstancePath);
        if (!capture || !capture->materialInstance)
            throw std::runtime_error("No live World-local MI for S4.");
        return capture->materialInstance->GetParameter<float>(parameter);
    }

    void Preview(const World& world, const TerrainCollisionInstance& target, size_t stage)
    {
        if (init.waitForRenderThreadIdle)
            init.waitForRenderThreadIdle();
        MaterialInstancePreviewAdapterCommand command;
        command.world = NormalizeWorldIdentity({world.GetGeneration(), world.GetScenePath()});
        command.materialInstancePath = *NormalizeMaterialInstancePath(target.plan.desc.materialInstancePath).path;
        command.bridgeLiveGeneration = 1;
        if (!preview)
        {
            preview = init.renderSystem->CreateMaterialInstancePreviewAdapter();
            const auto assets = init.renderSystem->CaptureOfflineWorldAssets();
            baselineMaterial = assets->materialInstanceSources.at(target.plan.desc.materialInstancePath);
            baselineUv = LiveUv(world, target);
            const auto connected = preview->Execute(command);
            if (connected.status != PreviewAdapterOperationStatus::Completed)
                throw std::runtime_error("S4 preview connect failed: " + connected.message);
        }
        command.type = stage == 9
            ? MaterialInstancePreviewCommandType::RestoreMaterialInstancePreviewBaseline
            : MaterialInstancePreviewCommandType::ApplyMaterialInstancePreview;
        command.documentRevision = stage - 7;
        auto draft = baselineMaterial;
        if (stage != 9)
            draft["parameters"]["u_uvScale"] = baselineUv * 1.75f;
        command.draft = MaterialInstancePreviewDraft{target.plan.desc.materialInstancePath,
            *command.documentRevision, draft.dump()};
        if (stage == 10)
            ++command.world.generation;
        const auto result = preview->Execute(command);
        std::cout << "EDITOR_S4_PREVIEW " << nlohmann::json({
            {"stage", StageNames[stage]}, {"status", ToString(result.status)},
            {"message", result.message}, {"liveSwapCommitted", result.liveSwapCommitted},
            {"replacesLiveResource", result.replacesLiveResource},
            {"baselineUv", baselineUv}, {"liveUv", LiveUv(world, target)}}).dump() << std::endl;
        if (stage == 10 ? result.status != PreviewAdapterOperationStatus::Failed || result.liveSwapCommitted
                        : result.status != PreviewAdapterOperationStatus::Completed || !result.liveSwapCommitted)
            throw std::runtime_error("S4 unexpected numeric preview outcome.");
    }

    void External(const TerrainCollisionInstance& target, bool restore)
    {
        const auto root = std::filesystem::canonical(CommonFunction::GetResourcePath());
        if (!std::filesystem::is_regular_file(root / "editor-s4-closure.json"))
            throw std::runtime_error("External injection requires the prepared closure marker.");
        const auto path = std::filesystem::weakly_canonical(root / target.plan.desc.materialInstancePath);
        const auto relative = path.lexically_relative(root);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            throw std::runtime_error("External injection escaped the copied resources.");
        if (restore)
        {
            WriteTextFileAtomically(path, externalBaseline);
        }
        else
        {
            const auto bytes = ReadBinaryFile(path);
            externalBaseline.assign(bytes.begin(), bytes.end());
            auto document = nlohmann::json::parse(externalBaseline);
            document["parameters"]["u_uvScale"] = baselineUv * 2.5f;
            WriteTextFileAtomically(path, document.dump(2));
        }
        std::cout << "EDITOR_S4_EXTERNAL " << nlohmann::json({
            {"event", restore ? "restored" : "injected"}, {"hostTickSerial", hostTickSerial},
            {"elapsedMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count()},
            {"materialDiskHash", ContentHasher::HashFile(path).ToHex()}}).dump() << std::endl;
    }

    void Action(const World& world, bool paint, TerrainBrushOperation operation, float u = 0.5f, bool erase = false)
    {
        UiAction action;
        action.type = paint ? UiActionType::TerrainPaint : UiActionType::TerrainEdit;
        action.terrainPaint.operation = action.terrainBrush.operation = operation;
        action.terrainPaint.worldGeneration = action.terrainBrush.worldGeneration = world.GetGeneration();
        if (overrideGeneration)
            action.terrainPaint.worldGeneration = action.terrainBrush.worldGeneration = overrideGeneration;
        action.terrainPaint.placementId = action.terrainBrush.placementId = identity;
        action.terrainPaint.u = action.terrainBrush.u = u;
        action.terrainPaint.v = action.terrainBrush.v = brushV;
        action.terrainPaint.radiusUv = action.terrainBrush.radiusUv = 0.025f;
        action.terrainPaint.strength = 1.0f;
        action.terrainBrush.strength = 0.02f;
        action.terrainPaint.layerIndex = 1;
        action.terrainPaint.erase = erase;
        init.commandBus->Queue(std::move(action));
    }

    void Stroke(const World& world, bool paint, float u, bool erase = false)
    {
        Action(world, paint, TerrainBrushOperation::BeginStroke, u, erase);
        Action(world, paint, TerrainBrushOperation::ApplyBrush, u, erase);
        Action(world, paint, TerrainBrushOperation::EndStroke, u, erase);
    }

    EditorHost host;
    EditorHostInit init;
    std::filesystem::path directory;
    std::string identity;
    std::string mode = "edit";
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point stageStarted = started;
    std::string lastFeedback;
    std::string externalBaseline;
    nlohmann::json baselineMaterial;
    float baselineUv = 0;
    std::unique_ptr<IMaterialInstancePreviewAdapter> preview;
    size_t tick = 0;
    size_t interval = 180;
    static constexpr size_t RapidStamps = 24;
    // 截图等待：只影响截图请求的节拍，不改 180 帧/步、步骤顺序或渲染。
    enum class CaptureState { Waiting, Requested, Complete };
    CaptureState captureState = CaptureState::Complete;
    size_t captureStage = std::numeric_limits<size_t>::max();
    size_t snapshotStage = std::numeric_limits<size_t>::max();
    uint64_t captureStartedSerial = 0;
    uint64_t captureRequestedSerial = 0;
    size_t captureFalseStreak = 0;
    size_t captureWaitLimit = 600;
    uint64_t hostTickSerial = 0;
    int64_t minimumStageMs = 2000;
    bool done = false;
    nlohmann::json v2Plan;
    std::vector<FrozenObservation> frozenObservations;
    uint64_t initialGeneration = 0;
    uint64_t overrideGeneration = 0;
    float brushV = 0.5f;
    std::string shaderBaseline;
    std::string diagnosticPhase;
    std::string diagnosticBaseline;
    uint64_t diagnosticWorld = 0;
    uint64_t diagnosticBaseRevision = 0;
    uint64_t diagnosticExpectedRevision = 0;
    uint64_t diagnosticLastPublishedSeq = 0;
    std::chrono::steady_clock::time_point diagnosticStarted;
    std::chrono::steady_clock::time_point diagnosticLastReport;
    std::map<uint64_t, EditorHost::TerrainRefreshObservation> diagnosticTickets;
    std::map<uint64_t, TerrainCompositedRefreshState> diagnosticStates;
};
}

int main(int argc, char** argv)
{
    VL::TerrainCompositedService::SetAuthoringMode(true);
    return VL::RunApplication(argc, argv, [] { return std::make_unique<AcceptanceHost>(); });
}
