#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"

namespace VL
{

enum class CompiledRenderGraphAccessType
{
    Read,
    Write
};

enum class CompiledRenderGraphBarrierType
{
    AttachmentToShaderRead,
    ShaderReadToAttachment
};

struct CompiledRenderGraphBarrier
{
    std::string resource;
    CompiledRenderGraphBarrierType type = CompiledRenderGraphBarrierType::AttachmentToShaderRead;
};

struct CompiledRenderGraphResourceAccess
{
    std::string passName;
    uint32_t passIndex = 0;
    CompiledRenderGraphAccessType accessType = CompiledRenderGraphAccessType::Read;
};

struct CompiledRenderGraphResource
{
    std::string name;
    std::string type = "texture2D";
    std::string format;
    std::vector<std::string> usage;
    // Fixed values come from widthSize/heightSize; scaled values come from
    // widthScale/heightScale and are resolved against the current window size
    // when RenderGraph creates swapchain-sized resources.
    bool hasFixedWidth = false;
    float widthValue = 1.0f;
    bool hasFixedHeight = false;
    float heightValue = 1.0f;
    uint32_t arrayLayers = 1;
    bool isSwapchain = false;
};

struct CompiledRenderGraphResourceUsagePlan
{
    std::string resource;
    std::vector<CompiledRenderGraphResourceAccess> accesses;
    int firstPassIndex = -1;
    int lastPassIndex = -1;
};

struct CompiledRenderGraphPassOutput
{
    std::string resource;
    uint32_t layer = 0;
    std::string loadOp = "clear";
    std::string storeOp = "store";
};

struct CompiledRenderGraphPassInputDescriptor
{
    std::string resource;
    uint32_t binding = 0;
};

enum class CompiledDepthCompareOp
{
    Less,
    LessOrEqual,
    Equal,
    Greater,
    GreaterOrEqual,
    Always
};

struct CompiledGraphicsPipelineState
{
    bool useVertexInput = true;
    bool depthTestEnable = true;
    bool depthWriteEnable = true;
    CompiledDepthCompareOp depthCompareOp = CompiledDepthCompareOp::Less;
};

struct CompiledRenderGraphPass
{
    std::string name;
    std::string type;
    bool needMsaa = false;
    bool needCreateMaterial = false;
    bool outputsSwapchain = false;
    bool outputsDepth = false;
    uint32_t colorOutputCount = 0;
    std::string materialInstancePath;
    CompiledGraphicsPipelineState pipelineState;
    std::vector<std::string> inputResources;
    std::vector<CompiledRenderGraphPassInputDescriptor> inputDescriptors;
    std::vector<CompiledRenderGraphPassOutput> outputResources;
    std::vector<CompiledRenderGraphBarrier> barriersBeforePass;
};

struct CompiledRenderGraph
{
    std::vector<CompiledRenderGraphResource> resources;
    std::vector<CompiledRenderGraphPass> passes;
    std::vector<std::string> passOrder;
    std::vector<CompiledRenderGraphResourceUsagePlan> resourceUsagePlans;
};

// Validates renderGraphConfig and produces a self-contained renderer-owned
// execution plan. V1 still allocates Vulkan objects in RenderGraph, but pass
// order, input/output resources, descriptor bindings, sizes, and attachment
// counts now come from this compiled view instead of ad hoc raw JSON reads.
class RenderGraphCompiler
{
public:
    RuntimeResult<CompiledRenderGraph> Compile(const nlohmann::json& renderGraphJson) const;
};

} // namespace VL
