#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"

namespace VL
{

enum class CompiledFrameGraphAccessType
{
    Read,
    Write
};

enum class CompiledFrameGraphBarrierType
{
    AttachmentToShaderRead,
    ShaderReadToAttachment
};

struct CompiledFrameGraphBarrier
{
    std::string resource;
    CompiledFrameGraphBarrierType type = CompiledFrameGraphBarrierType::AttachmentToShaderRead;
};

struct CompiledFrameGraphResourceAccess
{
    std::string passName;
    uint32_t passIndex = 0;
    CompiledFrameGraphAccessType accessType = CompiledFrameGraphAccessType::Read;
};

struct CompiledFrameGraphResource
{
    std::string name;
    std::string format;
    std::vector<std::string> usage;
    // Fixed values come from widthSize/heightSize; scaled values come from
    // widthScale/heightScale and are resolved against the current window size
    // when RenderGraph creates swapchain-sized resources.
    bool hasFixedWidth = false;
    float widthValue = 1.0f;
    bool hasFixedHeight = false;
    float heightValue = 1.0f;
    bool isSwapchain = false;
};

struct CompiledFrameGraphResourceUsagePlan
{
    std::string resource;
    std::vector<CompiledFrameGraphResourceAccess> accesses;
    int firstPassIndex = -1;
    int lastPassIndex = -1;
};

struct CompiledFrameGraphPassOutput
{
    std::string resource;
    std::string loadOp = "clear";
    std::string storeOp = "store";
};

struct CompiledFrameGraphPassInputDescriptor
{
    std::string resource;
    uint32_t binding = 0;
};

struct CompiledFrameGraphPass
{
    std::string name;
    bool needMsaa = false;
    bool needCreateMaterial = false;
    bool outputsSwapchain = false;
    bool outputsDepth = false;
    uint32_t colorOutputCount = 0;
    std::string materialInstancePath;
    std::vector<std::string> inputResources;
    std::vector<CompiledFrameGraphPassInputDescriptor> inputDescriptors;
    std::vector<CompiledFrameGraphPassOutput> outputResources;
    std::vector<CompiledFrameGraphBarrier> barriersBeforePass;
};

struct CompiledFrameGraph
{
    std::vector<CompiledFrameGraphResource> resources;
    std::vector<CompiledFrameGraphPass> passes;
    std::vector<std::string> passOrder;
    std::vector<CompiledFrameGraphResourceUsagePlan> resourceUsagePlans;
};

// Validates renderGraphConfig and produces a self-contained renderer-owned
// execution plan. V1 still allocates Vulkan objects in RenderGraph, but pass
// order, input/output resources, descriptor bindings, sizes, and attachment
// counts now come from this compiled view instead of ad hoc raw JSON reads.
class FrameGraphCompiler
{
public:
    RuntimeResult<CompiledFrameGraph> Compile(const nlohmann::json& renderGraphJson) const;
};

} // namespace VL
