#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/backend/rendererDescriptorWriter.h"
#include "render/framegraph/frameGraphCompiler.h"

class MaterialInstance;

namespace VL
{

struct RendererPassDescriptorPlan
{
    std::vector<RendererDescriptorUpdate> updates;
};

// Backend-side descriptor write plan cache keyed by frame-graph pass name.
// RenderGraph still triggers rebuilds during the transition, but Renderpass no
// longer owns descriptor planning rules.
class RendererDescriptorPlanCache
{
public:
    void Clear();
    void RebuildPassPlan(
        const std::string& passName,
        const std::vector<CompiledFrameGraphPassInputDescriptor>& inputDescriptorPlan,
        const std::weak_ptr<MaterialInstance>& passMaterialInstance);
    const RendererPassDescriptorPlan& GetPassPlan(const std::string& passName) const;

private:
    std::unordered_map<std::string, RendererPassDescriptorPlan> passPlans;
};

} // namespace VL
