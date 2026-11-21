#pragma once

#include <cstdint>
#include <memory>

namespace VL
{

class ShaderReloadCoordinator;

template<class Service>
void RebindTerrainCompositedReloadRegistration(
    std::weak_ptr<Service>& registered,
    std::shared_ptr<Service> selected,
    uint64_t activeGeneration,
    uint64_t resourceGeneration,
    ShaderReloadCoordinator* coordinator)
{
    if (activeGeneration == 0 || activeGeneration != resourceGeneration)
        selected.reset();
    if (auto previous = registered.lock(); previous && previous != selected)
        previous->SetShaderReloadCoordinator(nullptr);
    registered = selected;
    if (selected)
        selected->SetShaderReloadCoordinator(coordinator);
}

}
