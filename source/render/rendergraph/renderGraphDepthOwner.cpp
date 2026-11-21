#include "render/rendergraph/renderGraphDepthOwner.h"

#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace VL
{
namespace
{
std::mutex depthOwnerMutex;
std::map<vk::Image, std::shared_ptr<void>> depthOwners;
}

void RegisterRenderGraphDepthOwner(vk::Image image, std::shared_ptr<void> owner)
{
    if (!image || !owner)
        throw std::invalid_argument("Graph depth registration requires its actual image leaf owner");
    std::lock_guard<std::mutex> lock(depthOwnerMutex);
    if (!depthOwners.emplace(image, std::move(owner)).second)
        throw std::runtime_error("Duplicate graph depth leaf owner");
}

std::shared_ptr<const void> CaptureRenderGraphDepthOwner(vk::Image image)
{
    std::lock_guard<std::mutex> lock(depthOwnerMutex);
    const auto found = depthOwners.find(image);
    return found == depthOwners.end() ? std::shared_ptr<const void>{} : found->second;
}

std::shared_ptr<void> ReleaseRenderGraphDepthOwner(vk::Image image)
{
    std::lock_guard<std::mutex> lock(depthOwnerMutex);
    const auto found = depthOwners.find(image);
    if (found == depthOwners.end())
        return {};
    auto owner = std::move(found->second);
    depthOwners.erase(found);
    return owner;
}
}
