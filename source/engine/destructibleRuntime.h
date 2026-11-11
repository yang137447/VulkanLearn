#pragma once

#include <memory>
#include <string>
#include <vector>

#include "physics/fracture/fractureBudget.h"
#include "physics/fracture/fractureTypes.h"
#include "physics/runtime/fractureImpactWorld.h"

namespace VL
{

class World;
class DiagnosticsSubsystem;

// 可破坏目标的统一生命周期。几何求解器可以由具体 runtime 选择，
// 但 EngineLoop 只认识这组四方法，不再为每种材料手写编排。
class DestructibleRuntime
{
public:
    virtual ~DestructibleRuntime() = default;

    virtual bool ContainsTarget(
        const std::string& name,
        Physics::FractureMaterialProfile material) const = 0;
    virtual void SynchronizeWorld(World& world) = 0;
    virtual void HandleImpacts(World& world,
        const std::vector<Physics::FractureImpact>& impacts) = 0;
    virtual void Tick(World& world, float deltaSeconds) = 0;
    virtual void Reset(World& world) = 0;
    virtual void Shutdown() = 0;
    virtual void LaunchDebugImpact(const std::string& target, float speed) = 0;
    virtual void EvictFractureBudgetShards(
        const std::vector<Physics::Fracture::FractureBudgetCandidate>&
            candidates) = 0;
};

// 唯一编排者，同时作为预算回收 dispatcher。每个子 runtime 的异常被隔离，
// 单个材料失败不会把整帧物理/渲染主循环打挂。
class DestructibleRuntimeRegistry final
    : public Physics::Fracture::FractureEvictionDispatcher
{
public:
    explicit DestructibleRuntimeRegistry(DiagnosticsSubsystem& diagnostics);
    ~DestructibleRuntimeRegistry() override;

    void Add(std::unique_ptr<DestructibleRuntime> runtime);
    void SynchronizeWorld(World& world);
    void HandleImpacts(World& world,
        const std::vector<Physics::FractureImpact>& impacts);
    void Tick(World& world, float deltaSeconds);
    void Reset(World& world);
    void Shutdown();
    bool LaunchImpact(const std::string& target, float speed,
        Physics::FractureMaterialProfile material);

    void EvictFractureBudgetShards(
        const std::vector<Physics::Fracture::FractureBudgetCandidate>&
            candidates) override;

private:
    DiagnosticsSubsystem& diagnostics;
    std::vector<std::unique_ptr<DestructibleRuntime>> runtimes;
};

} // namespace VL
