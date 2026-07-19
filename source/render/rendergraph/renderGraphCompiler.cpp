#include "render/rendergraph/renderGraphCompiler.h"

#include <unordered_map>
#include <unordered_set>

namespace VL
{
namespace
{

enum class ResourcePreviousUse
{
    None,
    Sampled,
    Attachment
};

RuntimeError MakeCompileError(
    const std::string& code,
    const std::string& message,
    const std::string& sourceContext = {})
{
    return MakeRuntimeError(code, message, {}, sourceContext);
}

bool HasStringField(const nlohmann::json& node, const char* fieldName)
{
    return node.contains(fieldName) && node[fieldName].is_string();
}

bool IsSupportedFormat(const std::string& format)
{
    static const std::unordered_set<std::string> kSupportedFormats = {
        "R8G8B8A8_SRGB",
        "R8G8B8A8_UNORM",
        "R16G16B16A16_SFLOAT",
        "D16_UNORM",
        "D32_SFLOAT",
        "D24_UNORM_S8_UINT"
    };
    return kSupportedFormats.find(format) != kSupportedFormats.end();
}

bool IsSupportedResourceType(const std::string& type)
{
    return type == "texture2D" || type == "texture2DArray";
}

bool IsSupportedPassType(const std::string& type)
{
    return type == "shadow" || type == "geometry" || type == "postProcess";
}

bool IsDepthFormatString(const std::string& format)
{
    return format == "D16_UNORM" ||
        format == "D32_SFLOAT" ||
        format == "D24_UNORM_S8_UINT";
}

bool IsSupportedUsage(const std::string& usage)
{
    static const std::unordered_set<std::string> kSupportedUsages = {
        "colorAttachment",
        "depthStencilAttachment",
        "sampled",
        "present",
        "transferSrc",
        "transferDst"
    };
    return kSupportedUsages.find(usage) != kSupportedUsages.end();
}

bool IsSupportedLoadOp(const std::string& loadOp)
{
    return loadOp == "clear" || loadOp == "load" || loadOp == "dontCare";
}

bool IsSupportedStoreOp(const std::string& storeOp)
{
    return storeOp == "store" || storeOp == "dontCare";
}

RuntimeResult<CompiledDepthCompareOp> CompileDepthCompareOp(
    const nlohmann::json& stateNode,
    const std::string& passName)
{
    if (!stateNode.contains("depthCompareOp"))
    {
        return RuntimeResult<CompiledDepthCompareOp>::Success(
            CompiledDepthCompareOp::Less);
    }
    if (!stateNode["depthCompareOp"].is_string())
    {
        return RuntimeResult<CompiledDepthCompareOp>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidPipelineState",
            "Render graph pass depthCompareOp must be a string.",
            passName));
    }

    const std::string compareOp = stateNode["depthCompareOp"].get<std::string>();
    if (compareOp == "less")
    {
        return RuntimeResult<CompiledDepthCompareOp>::Success(CompiledDepthCompareOp::Less);
    }
    if (compareOp == "lessOrEqual")
    {
        return RuntimeResult<CompiledDepthCompareOp>::Success(CompiledDepthCompareOp::LessOrEqual);
    }
    if (compareOp == "equal")
    {
        return RuntimeResult<CompiledDepthCompareOp>::Success(CompiledDepthCompareOp::Equal);
    }
    if (compareOp == "greater")
    {
        return RuntimeResult<CompiledDepthCompareOp>::Success(CompiledDepthCompareOp::Greater);
    }
    if (compareOp == "greaterOrEqual")
    {
        return RuntimeResult<CompiledDepthCompareOp>::Success(CompiledDepthCompareOp::GreaterOrEqual);
    }
    if (compareOp == "always")
    {
        return RuntimeResult<CompiledDepthCompareOp>::Success(CompiledDepthCompareOp::Always);
    }
    return RuntimeResult<CompiledDepthCompareOp>::Failure(MakeCompileError(
        "RenderGraphCompiler.InvalidPipelineState",
        "Render graph pass depthCompareOp is unsupported.",
        passName));
}

RuntimeResult<CompiledGraphicsPipelineState> CompilePipelineState(
    const nlohmann::json& passNode,
    const std::string& passName)
{
    CompiledGraphicsPipelineState pipelineState;
    if (!passNode.contains("pipelineState"))
    {
        return RuntimeResult<CompiledGraphicsPipelineState>::Success(std::move(pipelineState));
    }

    const nlohmann::json& stateNode = passNode["pipelineState"];
    if (!stateNode.is_object())
    {
        return RuntimeResult<CompiledGraphicsPipelineState>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidPipelineState",
            "Render graph pass pipelineState must be an object.",
            passName));
    }

    const char* boolFields[] = {
        "useVertexInput",
        "depthTestEnable",
        "depthWriteEnable"
    };
    for (const char* fieldName : boolFields)
    {
        if (stateNode.contains(fieldName) && !stateNode[fieldName].is_boolean())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidPipelineState",
                "Render graph pass pipeline state field must be boolean: " +
                    std::string(fieldName),
                passName));
        }
    }
    auto depthCompareResult = CompileDepthCompareOp(stateNode, passName);
    if (depthCompareResult.IsFailure())
    {
        return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
            depthCompareResult.Error());
    }

    pipelineState.useVertexInput =
        stateNode.value("useVertexInput", pipelineState.useVertexInput);
    pipelineState.depthTestEnable =
        stateNode.value("depthTestEnable", pipelineState.depthTestEnable);
    pipelineState.depthWriteEnable =
        stateNode.value("depthWriteEnable", pipelineState.depthWriteEnable);
    pipelineState.depthCompareOp = depthCompareResult.Value();
    return RuntimeResult<CompiledGraphicsPipelineState>::Success(std::move(pipelineState));
}

RuntimeResult<void> CompileResourceSize(
    const nlohmann::json& resourceNode,
    CompiledRenderGraphResource& resource)
{
    if (resourceNode.contains("widthSize"))
    {
        if (!resourceNode["widthSize"].is_number())
        {
            return RuntimeResult<void>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidResourceSize",
                "Render graph resource widthSize must be numeric.",
                resource.name));
        }
        resource.hasFixedWidth = true;
        resource.widthValue = resourceNode["widthSize"].get<float>();
    }
    else if (resourceNode.contains("widthScale"))
    {
        if (!resourceNode["widthScale"].is_number())
        {
            return RuntimeResult<void>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidResourceSize",
                "Render graph resource widthScale must be numeric.",
                resource.name));
        }
        resource.hasFixedWidth = false;
        resource.widthValue = resourceNode["widthScale"].get<float>();
    }
    else
    {
        return RuntimeResult<void>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidResourceSize",
            "Render graph resource must provide widthSize or widthScale.",
            resource.name));
    }

    if (resourceNode.contains("heightSize"))
    {
        if (!resourceNode["heightSize"].is_number())
        {
            return RuntimeResult<void>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidResourceSize",
                "Render graph resource heightSize must be numeric.",
                resource.name));
        }
        resource.hasFixedHeight = true;
        resource.heightValue = resourceNode["heightSize"].get<float>();
    }
    else if (resourceNode.contains("heightScale"))
    {
        if (!resourceNode["heightScale"].is_number())
        {
            return RuntimeResult<void>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidResourceSize",
                "Render graph resource heightScale must be numeric.",
                resource.name));
        }
        resource.hasFixedHeight = false;
        resource.heightValue = resourceNode["heightScale"].get<float>();
    }
    else
    {
        return RuntimeResult<void>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidResourceSize",
            "Render graph resource must provide heightSize or heightScale.",
            resource.name));
    }

    if (resource.widthValue <= 0.0f || resource.heightValue <= 0.0f)
    {
        return RuntimeResult<void>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidResourceSize",
            "Render graph resource size or scale must be positive.",
            resource.name));
    }

    return RuntimeResult<void>::Success();
}

RuntimeResult<std::vector<std::string>> CompileUsageList(
    const nlohmann::json& resourceNode,
    const std::string& resourceName)
{
    if (!resourceNode.contains("usage") || !resourceNode["usage"].is_array())
    {
        return RuntimeResult<std::vector<std::string>>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidResourceUsage",
            "Render graph resource must provide a usage array.",
            resourceName));
    }

    std::vector<std::string> usageList;
    for (const nlohmann::json& usageNode : resourceNode["usage"])
    {
        if (!usageNode.is_string())
        {
            return RuntimeResult<std::vector<std::string>>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidResourceUsage",
                "Render graph resource usage entries must be strings.",
                resourceName));
        }
        std::string usage = usageNode.get<std::string>();
        if (!IsSupportedUsage(usage))
        {
            return RuntimeResult<std::vector<std::string>>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedResourceUsage",
                "Render graph resource usage is not supported.",
                resourceName + ":" + usage));
        }
        usageList.push_back(std::move(usage));
    }
    return RuntimeResult<std::vector<std::string>>::Success(std::move(usageList));
}

RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>> CompilePassInputs(
    const nlohmann::json& passNode,
    const std::string& passName,
    const std::unordered_set<std::string>& resourceNames)
{
    if (!passNode.contains("input") || !passNode["input"].is_array())
    {
        return RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidPassInput",
            "Render graph pass must provide an input array.",
            passName));
    }

    std::vector<CompiledRenderGraphPassInputDescriptor> inputs;
    std::unordered_set<uint32_t> inputBindings;
    uint32_t defaultBinding = 0;
    for (const nlohmann::json& inputNode : passNode["input"])
    {
        if (!HasStringField(inputNode, "resource"))
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidPassInput",
                "Render graph pass input must name a resource.",
                passName));
        }

        const std::string resourceName = inputNode["resource"].get<std::string>();
        if (resourceNames.find(resourceName) == resourceNames.end())
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnknownInputResource",
                "Render graph pass input references a resource that is not declared.",
                passName + ":" + resourceName));
        }

        CompiledRenderGraphPassInputDescriptor descriptor;
        descriptor.resource = resourceName;
        if (inputNode.contains("binding"))
        {
            if (!inputNode["binding"].is_number_unsigned())
            {
                return RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidPassInputBinding",
                    "Render graph pass input binding must be an unsigned integer.",
                    passName + ":" + resourceName));
            }
            descriptor.binding = inputNode["binding"].get<uint32_t>();
        }
        else
        {
            descriptor.binding = defaultBinding;
        }

        if (inputBindings.find(descriptor.binding) != inputBindings.end())
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>>::Failure(MakeCompileError(
                "RenderGraphCompiler.DuplicatePassInputBinding",
                "Render graph pass input descriptor bindings must be unique.",
                passName + ":" + resourceName + ":" + std::to_string(descriptor.binding)));
        }

        inputBindings.insert(descriptor.binding);
        inputs.push_back(std::move(descriptor));
        ++defaultBinding;
    }
    return RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>>::Success(std::move(inputs));
}

RuntimeResult<std::vector<CompiledRenderGraphPassOutput>> CompilePassOutputs(
    const nlohmann::json& passNode,
    const std::string& passName,
    const std::unordered_set<std::string>& resourceNames,
    const std::unordered_map<std::string, uint32_t>& resourceArrayLayers)
{
    if (!passNode.contains("output") || !passNode["output"].is_array())
    {
        return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidPassOutput",
            "Render graph pass must provide an output array.",
            passName));
    }

    std::vector<CompiledRenderGraphPassOutput> outputs;
    for (const nlohmann::json& outputNode : passNode["output"])
    {
        if (!HasStringField(outputNode, "resource"))
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidPassOutput",
                "Render graph pass output must name a resource.",
                passName));
        }

        CompiledRenderGraphPassOutput output;
        output.resource = outputNode["resource"].get<std::string>();
        if (resourceNames.find(output.resource) == resourceNames.end())
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnknownOutputResource",
                "Render graph pass output references a resource that is not declared.",
                passName + ":" + output.resource));
        }
        if (outputNode.contains("layer"))
        {
            if (!outputNode["layer"].is_number_unsigned())
            {
                return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidPassOutputLayer",
                    "Render graph pass output layer must be an unsigned integer.",
                    passName + ":" + output.resource));
            }
            output.layer = outputNode["layer"].get<uint32_t>();
        }
        const auto layerCountIt = resourceArrayLayers.find(output.resource);
        const uint32_t layerCount =
            layerCountIt != resourceArrayLayers.end() ? layerCountIt->second : 1;
        if (output.layer >= layerCount)
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidPassOutputLayer",
                "Render graph pass output layer exceeds the resource arrayLayers.",
                passName + ":" + output.resource + ":" + std::to_string(output.layer)));
        }
        output.loadOp = outputNode.value("loadOp", std::string("clear"));
        output.storeOp = outputNode.value("storeOp", std::string("store"));
        if (!IsSupportedLoadOp(output.loadOp))
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedLoadOp",
                "Render graph pass output loadOp is not supported.",
                passName + ":" + output.resource + ":" + output.loadOp));
        }
        if (!IsSupportedStoreOp(output.storeOp))
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedStoreOp",
                "Render graph pass output storeOp is not supported.",
                passName + ":" + output.resource + ":" + output.storeOp));
        }
        outputs.push_back(std::move(output));
    }
    return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Success(std::move(outputs));
}

void AddResourceAccess(
    CompiledRenderGraphResourceUsagePlan& usagePlan,
    const std::string& passName,
    uint32_t passIndex,
    CompiledRenderGraphAccessType accessType)
{
    CompiledRenderGraphResourceAccess access;
    access.passName = passName;
    access.passIndex = passIndex;
    access.accessType = accessType;
    usagePlan.accesses.push_back(std::move(access));

    if (usagePlan.firstPassIndex < 0 ||
        static_cast<int>(passIndex) < usagePlan.firstPassIndex)
    {
        usagePlan.firstPassIndex = static_cast<int>(passIndex);
    }
    if (usagePlan.lastPassIndex < 0 ||
        static_cast<int>(passIndex) > usagePlan.lastPassIndex)
    {
        usagePlan.lastPassIndex = static_cast<int>(passIndex);
    }
}

void AddBarrier(
    CompiledRenderGraphPass& pass,
    const std::string& resourceName,
    CompiledRenderGraphBarrierType barrierType)
{
    CompiledRenderGraphBarrier barrier;
    barrier.resource = resourceName;
    barrier.type = barrierType;
    pass.barriersBeforePass.push_back(std::move(barrier));
}

} // namespace

RuntimeResult<CompiledRenderGraph> RenderGraphCompiler::Compile(const nlohmann::json& renderGraphJson) const
{
    if (!renderGraphJson.contains("resources") || !renderGraphJson["resources"].is_array())
    {
        return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
            "RenderGraphCompiler.MissingResources",
            "Render graph config must contain a resources array."));
    }
    if (!renderGraphJson.contains("passes") || !renderGraphJson["passes"].is_array())
    {
        return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
            "RenderGraphCompiler.MissingPasses",
            "Render graph config must contain a passes array."));
    }

    CompiledRenderGraph compiledGraph;
    std::unordered_set<std::string> resourceNames;
    std::unordered_map<std::string, std::string> resourceFormats;
    std::unordered_map<std::string, uint32_t> resourceArrayLayers;
    std::unordered_map<std::string, size_t> resourceUsagePlanIndices;

    for (const nlohmann::json& resourceNode : renderGraphJson["resources"])
    {
        if (!resourceNode.is_object() || !HasStringField(resourceNode, "name"))
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidResource",
                "Render graph resource entries must be objects with a string name."));
        }

        CompiledRenderGraphResource resource;
        resource.name = resourceNode["name"].get<std::string>();
        if (resource.name.empty() || resourceNames.find(resource.name) != resourceNames.end())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.DuplicateResource",
                "Render graph resource names must be non-empty and unique.",
                resource.name));
        }
        if (!HasStringField(resourceNode, "format"))
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidResourceFormat",
                "Render graph resource must provide a format string.",
                resource.name));
        }
        resource.type = resourceNode.value("type", std::string("texture2D"));
        if (!IsSupportedResourceType(resource.type))
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedResourceType",
                "Render graph resource type is not supported.",
                resource.name + ":" + resource.type));
        }
        if (resourceNode.contains("arrayLayers"))
        {
            if (!resourceNode["arrayLayers"].is_number_unsigned())
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidResourceArrayLayers",
                    "Render graph resource arrayLayers must be an unsigned integer.",
                    resource.name));
            }
            resource.arrayLayers = resourceNode["arrayLayers"].get<uint32_t>();
        }
        if (resource.arrayLayers < 1)
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidResourceArrayLayers",
                "Render graph resource arrayLayers must be at least 1.",
                resource.name));
        }
        if (resource.type == "texture2D" && resource.arrayLayers != 1)
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidResourceArrayLayers",
                "Only texture2DArray resources may declare arrayLayers greater than 1.",
                resource.name));
        }
        const std::string format = resourceNode["format"].get<std::string>();
        if (!IsSupportedFormat(format))
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedResourceFormat",
                "Render graph resource format is not supported.",
                resource.name + ":" + format));
        }

        auto usageResult = CompileUsageList(resourceNode, resource.name);
        if (usageResult.IsFailure())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(usageResult.Error());
        }

        auto sizeResult = CompileResourceSize(resourceNode, resource);
        if (sizeResult.IsFailure())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(sizeResult.Error());
        }

        resource.format = format;
        resource.usage = std::move(usageResult.Value());
        resource.isSwapchain = resource.name == "swapChain";
        resourceNames.insert(resource.name);
        resourceFormats.emplace(resource.name, resource.format);
        resourceArrayLayers.emplace(resource.name, resource.arrayLayers);

        CompiledRenderGraphResourceUsagePlan usagePlan;
        usagePlan.resource = resource.name;
        resourceUsagePlanIndices.emplace(resource.name, compiledGraph.resourceUsagePlans.size());
        compiledGraph.resourceUsagePlans.push_back(std::move(usagePlan));
        compiledGraph.resources.push_back(std::move(resource));
    }

    std::unordered_set<std::string> passNames;
    std::unordered_map<std::string, ResourcePreviousUse> previousResourceUses;
    uint32_t passIndex = 0;
    for (const nlohmann::json& passNode : renderGraphJson["passes"])
    {
        if (!passNode.is_object() || !HasStringField(passNode, "name"))
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidPass",
                "Render graph pass entries must be objects with a string name."));
        }

        CompiledRenderGraphPass pass;
        pass.name = passNode["name"].get<std::string>();
        if (pass.name.empty() || passNames.find(pass.name) != passNames.end())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.DuplicatePass",
                "Render graph pass names must be non-empty and unique.",
                pass.name));
        }

        auto inputResult = CompilePassInputs(passNode, pass.name, resourceNames);
        if (inputResult.IsFailure())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(inputResult.Error());
        }
        auto outputResult = CompilePassOutputs(passNode, pass.name, resourceNames, resourceArrayLayers);
        if (outputResult.IsFailure())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(outputResult.Error());
        }
        if (outputResult.Value().empty())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.EmptyPassOutput",
                "Render graph pass must write at least one output resource.",
                pass.name));
        }

        if (!HasStringField(passNode, "type"))
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidPassType",
                "Render graph pass must provide a type string.",
                pass.name));
        }
        pass.type = passNode["type"].get<std::string>();
        if (!IsSupportedPassType(pass.type))
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedPassType",
                "Render graph pass type is not supported.",
                pass.name + ":" + pass.type));
        }
        pass.needMsaa = passNode.value("needMsaa", false);
        pass.needCreateMaterial = passNode.value("needCreateMaterial", false);
        pass.materialInstancePath = passNode.value("materialInstancePath", std::string());
        if (pass.type == "shadow" && !pass.needCreateMaterial)
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.MissingShadowPassMaterial",
                "Shadow passes require the common opaque pass material.",
                pass.name));
        }
        if (pass.needCreateMaterial && pass.materialInstancePath.empty())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.MissingPassMaterial",
                "Render graph pass with needCreateMaterial must provide materialInstancePath.",
                pass.name));
        }
        auto pipelineStateResult = CompilePipelineState(passNode, pass.name);
        if (pipelineStateResult.IsFailure())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(pipelineStateResult.Error());
        }
        pass.pipelineState = std::move(pipelineStateResult.Value());
        pass.inputDescriptors = std::move(inputResult.Value());
        pass.inputResources.reserve(pass.inputDescriptors.size());
        for (const CompiledRenderGraphPassInputDescriptor& inputDescriptor : pass.inputDescriptors)
        {
            pass.inputResources.push_back(inputDescriptor.resource);
        }
        pass.outputResources = std::move(outputResult.Value());
        for (const std::string& inputResource : pass.inputResources)
        {
            if (previousResourceUses[inputResource] == ResourcePreviousUse::Attachment)
            {
                AddBarrier(
                    pass,
                    inputResource,
                    CompiledRenderGraphBarrierType::AttachmentToShaderRead);
            }
            AddResourceAccess(
                compiledGraph.resourceUsagePlans.at(resourceUsagePlanIndices.at(inputResource)),
                pass.name,
                passIndex,
                CompiledRenderGraphAccessType::Read);
        }
        for (const CompiledRenderGraphPassOutput& output : pass.outputResources)
        {
            if (output.resource != "swapChain" &&
                output.loadOp == "load" &&
                previousResourceUses[output.resource] == ResourcePreviousUse::Sampled)
            {
                AddBarrier(
                    pass,
                    output.resource,
                    CompiledRenderGraphBarrierType::ShaderReadToAttachment);
            }
            AddResourceAccess(
                compiledGraph.resourceUsagePlans.at(resourceUsagePlanIndices.at(output.resource)),
                pass.name,
                passIndex,
                CompiledRenderGraphAccessType::Write);
            pass.outputsSwapchain = pass.outputsSwapchain || output.resource == "swapChain";
            const auto formatIt = resourceFormats.find(output.resource);
            const bool isDepthOutput =
                formatIt != resourceFormats.end() && IsDepthFormatString(formatIt->second);
            pass.outputsDepth = pass.outputsDepth || isDepthOutput;
            if (!isDepthOutput)
            {
                ++pass.colorOutputCount;
            }
        }

        for (const std::string& inputResource : pass.inputResources)
        {
            previousResourceUses[inputResource] = ResourcePreviousUse::Sampled;
        }
        for (const CompiledRenderGraphPassOutput& output : pass.outputResources)
        {
            previousResourceUses[output.resource] = ResourcePreviousUse::Attachment;
        }

        passNames.insert(pass.name);
        compiledGraph.passOrder.push_back(pass.name);
        compiledGraph.passes.push_back(std::move(pass));
        ++passIndex;
    }

    return RuntimeResult<CompiledRenderGraph>::Success(std::move(compiledGraph));
}

} // namespace VL
