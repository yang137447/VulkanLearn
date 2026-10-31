#include "engine/destructibleRuntime.h"

#include <exception>

#include "engine/diagnosticsSubsystem.h"

namespace VL
{

DestructibleRuntimeRegistry::DestructibleRuntimeRegistry(
    DiagnosticsSubsystem& diagnostics)
    : diagnostics(diagnostics)
{
}

DestructibleRuntimeRegistry::~DestructibleRuntimeRegistry()
{
    Shutdown();
}

void DestructibleRuntimeRegistry::Add(
    std::unique_ptr<DestructibleRuntime> runtime)
{
    if (runtime) runtimes.push_back(std::move(runtime));
}

void DestructibleRuntimeRegistry::SynchronizeWorld(World& world)
{
    for (const auto& runtime : runtimes)
    {
        try { runtime->SynchronizeWorld(world); }
        catch (const std::exception& error)
        {
            diagnostics.ReportError(
                std::string("Destructible synchronize failed: ") +
                error.what());
        }
    }
}

void DestructibleRuntimeRegistry::HandleImpacts(
    World& world, const std::vector<Physics::FractureImpact>& impacts)
{
    for (const auto& runtime : runtimes)
    {
        try { runtime->HandleImpacts(world, impacts); }
        catch (const std::exception& error)
        {
            diagnostics.ReportError(
                std::string("Destructible impact handling failed: ") +
                error.what());
        }
    }
}

void DestructibleRuntimeRegistry::Tick(World& world, float deltaSeconds)
{
    for (const auto& runtime : runtimes)
    {
        try { runtime->Tick(world, deltaSeconds); }
        catch (const std::exception& error)
        {
            diagnostics.ReportError(
                std::string("Destructible tick failed: ") + error.what());
        }
    }
}

void DestructibleRuntimeRegistry::Reset(World& world)
{
    for (const auto& runtime : runtimes)
    {
        try { runtime->Reset(world); }
        catch (const std::exception& error)
        {
            diagnostics.ReportError(
                std::string("Destructible reset failed: ") + error.what());
        }
    }
}

void DestructibleRuntimeRegistry::Shutdown()
{
    for (const auto& runtime : runtimes)
    {
        try { runtime->Shutdown(); }
        catch (const std::exception& error)
        {
            diagnostics.ReportError(
                std::string("Destructible shutdown failed: ") + error.what());
        }
    }
    runtimes.clear();
}

bool DestructibleRuntimeRegistry::LaunchImpact(
    const std::string& target, float speed,
    Physics::FractureMaterialProfile material)
{
    for (const auto& runtime : runtimes)
    {
        if (!runtime->ContainsTarget(target, material)) continue;
        try { runtime->LaunchDebugImpact(target, speed); }
        catch (const std::exception& error)
        {
            diagnostics.ReportError(
                std::string("Destructible debug impact failed: ") +
                error.what());
        }
        return true;
    }
    return false;
}

void DestructibleRuntimeRegistry::EvictFractureBudgetShards(
    const std::vector<Physics::Fracture::FractureBudgetCandidate>& candidates)
{
    for (const auto& runtime : runtimes)
    {
        try { runtime->EvictFractureBudgetShards(candidates); }
        catch (const std::exception& error)
        {
            diagnostics.ReportError(
                std::string("Destructible eviction failed: ") + error.what());
        }
    }
}

} // namespace VL
