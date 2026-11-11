#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "render/rendergraph/renderGraphCompiler.h"

namespace VL
{
namespace RenderGraphCompilerDetail
{

enum class ResourcePreviousUse
{
    None,
    Sampled,
    Attachment,
    Storage
};

RuntimeError MakeCompileError(
    const std::string& code,
    const std::string& message,
    const std::string& sourceContext = {});

bool HasStringField(const nlohmann::json& node, const char* fieldName);

bool IsSupportedFormat(const std::string& format);

bool IsSupportedResourceType(const std::string& type);

bool IsDepthFormatString(const std::string& format);

bool IsSupportedUsage(const std::string& usage);

bool IsSupportedLoadOp(const std::string& loadOp);

bool IsSupportedStoreOp(const std::string& storeOp);

RuntimeResult<CompiledDepthCompareOp> CompileDepthCompareOp(
    const nlohmann::json& stateNode,
    const std::string& passName);

RuntimeResult<CompiledStencilCompareOp> CompileStencilCompareOp(
    const nlohmann::json& stencilNode,
    const std::string& passName);

RuntimeResult<CompiledStencilOp> CompileStencilOp(
    const nlohmann::json& stencilNode,
    const char* fieldName,
    const std::string& passName);

RuntimeResult<uint8_t> CompileStencilMaskField(
    const nlohmann::json& stencilNode,
    const char* fieldName,
    uint8_t defaultValue,
    const std::string& passName);

RuntimeResult<CompiledGraphicsPipelineState> CompilePipelineState(
    const nlohmann::json& passNode,
    const std::string& passName);

RuntimeResult<void> CompileResourceSize(
    const nlohmann::json& resourceNode,
    CompiledRenderGraphResource& resource);

RuntimeResult<std::vector<std::string>> CompileUsageList(
    const nlohmann::json& resourceNode,
    const std::string& resourceName);

RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>> CompilePassInputs(
    const nlohmann::json& passNode,
    const std::string& passName,
    const std::unordered_set<std::string>& resourceNames);

RuntimeResult<std::vector<CompiledRenderGraphPassOutput>> CompilePassOutputs(
    const nlohmann::json& passNode,
    const std::string& passName,
    const std::unordered_set<std::string>& resourceNames,
    const std::unordered_map<std::string, uint32_t>& resourceArrayLayers);

void AddResourceAccess(
    CompiledRenderGraphResourceUsagePlan& usagePlan,
    const std::string& passName,
    uint32_t passIndex,
    CompiledRenderGraphAccessType accessType);

void AddBarrier(
    CompiledRenderGraphPass& pass,
    const std::string& resourceName,
    CompiledRenderGraphBarrierType barrierType);

RuntimeResult<void> ValidateRequiredSingletonPassType(
    const CompiledRenderGraph& compiledGraph,
    RenderGraphPassType requiredType);

} // namespace RenderGraphCompilerDetail
} // namespace VL
