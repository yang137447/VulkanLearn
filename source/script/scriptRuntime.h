#pragma once

#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "script/scriptHost.h"
#include "script/scriptTypes.h"
#include "core/logStore.h"

namespace VL
{
class World;
struct InputActionState;
class DiagnosticsSubsystem;
class ScriptPhysicsWorld;

// 候选 World 独占托管实例上下文；RAII 只释放自己的句柄，不持有原生 World 指针。
class ScriptWorldContext
{
public:
    ScriptWorldContext(std::shared_ptr<ScriptHost> host, uint64_t id) : host(std::move(host)), id(id) {}
    ~ScriptWorldContext();
    nlohmann::json Call(const char* operation, nlohmann::json fields = nlohmann::json::object());
    void Release(int reason) noexcept;
    nlohmann::json declarations = nlohmann::json::object();
    nlohmann::json raycasts = nlohmann::json::array();
    nlohmann::json rayHits = nlohmann::json::array();
    nlohmann::json assetRequests = nlohmann::json::array();
    nlohmann::json assetCancellations = nlohmann::json::array();
    nlohmann::json assetLoads = nlohmann::json::object();
    nlohmann::json assetResults = nlohmann::json::array();
    nlohmann::json assetUnloads = nlohmann::json::array();
    nlohmann::json dynamicAssetPaths = nlohmann::json::object();
    nlohmann::json permanentAssetPaths = nlohmann::json::object();
    std::unordered_map<std::string, size_t> stateIndices;
    std::vector<size_t> physicsIndices;
    nlohmann::json physicsUpdates = nlohmann::json::array();
    nlohmann::json TakePhysicsUpdates()
    {
        auto result = std::move(physicsUpdates);
        physicsUpdates = nlohmann::json::array();
        return result;
    }

private:
    std::shared_ptr<ScriptHost> host;
    uint64_t id;
    bool released = false;
};

// 跨 World 的脚本调度 owner；候选准备不触碰 active，Tick 在冻结渲染快照之前写 World。
class ScriptRuntimeSubsystem
{
public:
    ScriptRuntimeSubsystem() = default;
    ScriptRuntimeSubsystem(const ScriptRuntimeSubsystem&) = delete;
    ScriptRuntimeSubsystem& operator=(const ScriptRuntimeSubsystem&) = delete;
    ~ScriptRuntimeSubsystem() { Shutdown(); }
    void Initialize(bool enabled, const std::string& runtimeDirectory, const std::string& assembly);
    void PrepareWorld(World& world, const nlohmann::json& scene,
        const nlohmann::json& authoredScene = nullptr);
    bool Control(World& world, ScriptPlayState requested, std::string& error);
    void Tick(const std::shared_ptr<World>& world, float dt, const InputActionState& input,
        const DiagnosticsSubsystem& diagnostics);
    void Shutdown() noexcept;
    void SynchronizeWorld(const std::shared_ptr<World>& world);
    ScriptRuntimeView View(uint64_t generation) const;
    bool IsEditing() const { return state == ScriptPlayState::Editing; }
    const nlohmann::json& Classes() const { return classes; }
    const nlohmann::json& ComponentClasses() const { return componentClasses; }
    bool SetTimeScale(float value, std::string& error);
    nlohmann::json ContextTime() const;
    void ValidateBlueprint(const nlohmann::json& blueprint);
    void SetFeedback(std::string message) { feedback = std::move(message); }
    void SetResourceRoot(std::string root) { resourceRoot = std::move(root); }
    bool PollBlueprintAssetChanges(const World& world, float dt);
    bool ReloadAssembly(World& world, std::string& error, const std::string& candidateAssembly = {});
    nlohmann::json TakeAssetRequests(World& world);
    nlohmann::json TakeAssetCancellations(World& world);
    bool CompleteAssetLoad(World& world, uint64_t id, const std::string& path,
        const nlohmann::json& blueprint, const std::string& error = {});
    std::vector<std::string> CollectUnusedBlueprintAssets(const World& world) const;
    void ForgetBlueprintAsset(World& world, const std::string& path);

private:
    std::shared_ptr<ScriptWorldContext> CreateContext(const nlohmann::json& blueprints,
        const nlohmann::json& templates = nlohmann::json::object(), uint64_t generation = 0);
    void Apply(World& world, const nlohmann::json& response);
    void CollectDiagnostics(const nlohmann::json& response, const World* world = nullptr);
    void StepPhysics(World& world, bool advance, bool collectQueries = true);
    void RefreshPerformance(World& world, bool details = true);
    bool ReloadPlayingAssembly(World& world, std::string& error, const std::string& candidateAssembly);
    void Stop(World& world);
    std::shared_ptr<ScriptHost> host;
    std::shared_ptr<World> activeWorld;
    nlohmann::json classes = nlohmann::json::array();
    nlohmann::json componentClasses = nlohmann::json::array();
    nlohmann::json performance = nlohmann::json::object();
    ScriptTimeState time;
    double physicsMilliseconds = 0;
    double applyMilliseconds = 0, snapshotMilliseconds = 0;
    double performanceDetailElapsed = 0;
    uint64_t appliedUpdates = 0;
    unsigned detailSamples = 0;
    ScriptPlayState state = ScriptPlayState::Editing;
    inline static uint64_t nextContext = 1;
    uint64_t assemblyGeneration = 0;
    std::string assemblyPath;
    std::string feedback;
    struct PendingScriptLog
    {
        LogSeverity severity;
        std::string message;
        LogLocation location;
    };
    std::vector<PendingScriptLog> pendingDiagnostics;
    std::shared_ptr<ScriptPhysicsWorld> physics;
    std::string resourceRoot, observedAssets, pendingAssets;
    float assetPollTime = 0;
};
} // namespace VL
