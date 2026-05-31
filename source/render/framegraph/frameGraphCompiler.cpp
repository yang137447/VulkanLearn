#include "render/framegraph/frameGraphCompiler.h"

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

RuntimeResult<void> CompileResourceSize(
    const nlohmann::json& resourceNode,
    CompiledFrameGraphResource& resource)
{
    if (resourceNode.contains("widthSize"))
    {
        if (!resourceNode["widthSize"].is_number())
        {
            return RuntimeResult<void>::Failure(MakeCompileError(
                "FrameGraphCompiler.InvalidResourceSize",
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
                "FrameGraphCompiler.InvalidResourceSize",
                "Render graph resource widthScale must be numeric.",
                resource.name));
        }
        resource.hasFixedWidth = false;
        resource.widthValue = resourceNode["widthScale"].get<float>();
    }
    else
    {
        return RuntimeResult<void>::Failure(MakeCompileError(
            "FrameGraphCompiler.InvalidResourceSize",
            "Render graph resource must provide widthSize or widthScale.",
            resource.name));
    }

    if (resourceNode.contains("heightSize"))
    {
        if (!resourceNode["heightSize"].is_number())
        {
            return RuntimeResult<void>::Failure(MakeCompileError(
                "FrameGraphCompiler.InvalidResourceSize",
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
                "FrameGraphCompiler.InvalidResourceSize",
                "Render graph resource heightScale must be numeric.",
                resource.name));
        }
        resource.hasFixedHeight = false;
        resource.heightValue = resourceNode["heightScale"].get<float>();
    }
    else
    {
        return RuntimeResult<void>::Failure(MakeCompileError(
            "FrameGraphCompiler.InvalidResourceSize",
            "Render graph resource must provide heightSize or heightScale.",
            resource.name));
    }

    if (resource.widthValue <= 0.0f || resource.heightValue <= 0.0f)
    {
        return RuntimeResult<void>::Failure(MakeCompileError(
            "FrameGraphCompiler.InvalidResourceSize",
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
            "FrameGraphCompiler.InvalidResourceUsage",
            "Render graph resource must provide a usage array.",
            resourceName));
    }

    std::vector<std::string> usageList;
    for (const nlohmann::json& usageNode : resourceNode["usage"])
    {
        if (!usageNode.is_string())
        {
            return RuntimeResult<std::vector<std::string>>::Failure(MakeCompileError(
                "FrameGraphCompiler.InvalidResourceUsage",
                "Render graph resource usage entries must be strings.",
                resourceName));
        }
        std::string usage = usageNode.get<std::string>();
        if (!IsSupportedUsage(usage))
        {
            return RuntimeResult<std::vector<std::string>>::Failure(MakeCompileError(
                "FrameGraphCompiler.UnsupportedResourceUsage",
                "Render graph resource usage is not supported.",
                resourceName + ":" + usage));
        }
        usageList.push_back(std::move(usage));
    }
    return RuntimeResult<std::vector<std::string>>::Success(std::move(usageList));
}

RuntimeResult<std::vector<CompiledFrameGraphPassInputDescriptor>> CompilePassInputs(
    const nlohmann::json& passNode,
    const std::string& passName,
    const std::unordered_set<std::string>& resourceNames)
{
    if (!passNode.contains("input") || !passNode["input"].is_array())
    {
        return RuntimeResult<std::vector<CompiledFrameGraphPassInputDescriptor>>::Failure(MakeCompileError(
            "FrameGraphCompiler.InvalidPassInput",
            "Render graph pass must provide an input array.",
            passName));
    }

    std::vector<CompiledFrameGraphPassInputDescriptor> inputs;
    std::unordered_set<uint32_t> inputBindings;
    uint32_t defaultBinding = 0;
    for (const nlohmann::json& inputNode : passNode["input"])
    {
        if (!HasStringField(inputNode, "resource"))
        {
            return RuntimeResult<std::vector<CompiledFrameGraphPassInputDescriptor>>::Failure(MakeCompileError(
                "FrameGraphCompiler.InvalidPassInput",
                "Render graph pass input must name a resource.",
                passName));
        }

        const std::string resourceName = inputNode["resource"].get<std::string>();
        if (resourceNames.find(resourceName) == resourceNames.end())
        {
            return RuntimeResult<std::vector<CompiledFrameGraphPassInputDescriptor>>::Failure(MakeCompileError(
                "FrameGraphCompiler.UnknownInputResource",
                "Render graph pass input references a resource that is not declared.",
                passName + ":" + resourceName));
        }

        CompiledFrameGraphPassInputDescriptor descriptor;
        descriptor.resource = resourceName;
        if (inputNode.contains("binding"))
        {
            if (!inputNode["binding"].is_number_unsigned())
            {
                return RuntimeResult<std::vector<CompiledFrameGraphPassInputDescriptor>>::Failure(MakeCompileError(
                    "FrameGraphCompiler.InvalidPassInputBinding",
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
            return RuntimeResult<std::vector<CompiledFrameGraphPassInputDescriptor>>::Failure(MakeCompileError(
                "FrameGraphCompiler.DuplicatePassInputBinding",
                "Render graph pass input descriptor bindings must be unique.",
                passName + ":" + resourceName + ":" + std::to_string(descriptor.binding)));
        }

        inputBindings.insert(descriptor.binding);
        inputs.push_back(std::move(descriptor));
        ++defaultBinding;
    }
    return RuntimeResult<std::vector<CompiledFrameGraphPassInputDescriptor>>::Success(std::move(inputs));
}

RuntimeResult<std::vector<CompiledFrameGraphPassOutput>> CompilePassOutputs(
    const nlohmann::json& passNode,
    const std::string& passName,
    const std::unordered_set<std::string>& resourceNames)
{
    if (!passNode.contains("output") || !passNode["output"].is_array())
    {
        return RuntimeResult<std::vector<CompiledFrameGraphPassOutput>>::Failure(MakeCompileError(
            "FrameGraphCompiler.InvalidPassOutput",
            "Render graph pass must provide an output array.",
            passName));
    }

    std::vector<CompiledFrameGraphPassOutput> outputs;
    for (const nlohmann::json& outputNode : passNode["output"])
    {
        if (!HasStringField(outputNode, "resource"))
        {
            return RuntimeResult<std::vector<CompiledFrameGraphPassOutput>>::Failure(MakeCompileError(
                "FrameGraphCompiler.InvalidPassOutput",
                "Render graph pass output must name a resource.",
                passName));
        }

        CompiledFrameGraphPassOutput output;
        output.resource = outputNode["resource"].get<std::string>();
        if (resourceNames.find(output.resource) == resourceNames.end())
        {
            return RuntimeResult<std::vector<CompiledFrameGraphPassOutput>>::Failure(MakeCompileError(
                "FrameGraphCompiler.UnknownOutputResource",
                "Render graph pass output references a resource that is not declared.",
                passName + ":" + output.resource));
        }
        output.loadOp = outputNode.value("loadOp", std::string("clear"));
        output.storeOp = outputNode.value("storeOp", std::string("store"));
        if (!IsSupportedLoadOp(output.loadOp))
        {
            return RuntimeResult<std::vector<CompiledFrameGraphPassOutput>>::Failure(MakeCompileError(
                "FrameGraphCompiler.UnsupportedLoadOp",
                "Render graph pass output loadOp is not supported.",
                passName + ":" + output.resource + ":" + output.loadOp));
        }
        if (!IsSupportedStoreOp(output.storeOp))
        {
            return RuntimeResult<std::vector<CompiledFrameGraphPassOutput>>::Failure(MakeCompileError(
                "FrameGraphCompiler.UnsupportedStoreOp",
                "Render graph pass output storeOp is not supported.",
                passName + ":" + output.resource + ":" + output.storeOp));
        }
        outputs.push_back(std::move(output));
    }
    return RuntimeResult<std::vector<CompiledFrameGraphPassOutput>>::Success(std::move(outputs));
}

void AddResourceAccess(
    CompiledFrameGraphResourceUsagePlan& usagePlan,
    const std::string& passName,
    uint32_t passIndex,
    CompiledFrameGraphAccessType accessType)
{
    CompiledFrameGraphResourceAccess access;
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
    CompiledFrameGraphPass& pass,
    const std::string& resourceName,
    CompiledFrameGraphBarrierType barrierType)
{
    CompiledFrameGraphBarrier barrier;
    barrier.resource = resourceName;
    barrier.type = barrierType;
    pass.barriersBeforePass.push_back(std::move(barrier));
}

} // namespace

RuntimeResult<CompiledFrameGraph> FrameGraphCompiler::Compile(const nlohmann::json& renderGraphJson) const
{
    if (!renderGraphJson.contains("resources") || !renderGraphJson["resources"].is_array())
    {
        return RuntimeResult<CompiledFrameGraph>::Failure(MakeCompileError(
            "FrameGraphCompiler.MissingResources",
            "Render graph config must contain a resources array."));
    }
    if (!renderGraphJson.contains("passes") || !renderGraphJson["passes"].is_array())
    {
        return RuntimeResult<CompiledFrameGraph>::Failure(MakeCompileError(
            "FrameGraphCompiler.MissingPasses",
            "Render graph config must contain a passes array."));
    }

    CompiledFrameGraph compiledGraph;
    std::unordered_set<std::string> resourceNames;
    std::unordered_map<std::string, std::string> resourceFormats;
    std::unordered_map<std::string, size_t> resourceUsagePlanIndices;

    for (const nlohmann::json& resourceNode : renderGraphJson["resources"])
    {
        if (!resourceNode.is_object() || !HasStringField(resourceNode, "name"))
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(MakeCompileError(
                "FrameGraphCompiler.InvalidResource",
                "Render graph resource entries must be objects with a string name."));
        }

        CompiledFrameGraphResource resource;
        resource.name = resourceNode["name"].get<std::string>();
        if (resource.name.empty() || resourceNames.find(resource.name) != resourceNames.end())
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(MakeCompileError(
                "FrameGraphCompiler.DuplicateResource",
                "Render graph resource names must be non-empty and unique.",
                resource.name));
        }
        if (!HasStringField(resourceNode, "format"))
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(MakeCompileError(
                "FrameGraphCompiler.InvalidResourceFormat",
                "Render graph resource must provide a format string.",
                resource.name));
        }
        const std::string format = resourceNode["format"].get<std::string>();
        if (!IsSupportedFormat(format))
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(MakeCompileError(
                "FrameGraphCompiler.UnsupportedResourceFormat",
                "Render graph resource format is not supported.",
                resource.name + ":" + format));
        }

        auto usageResult = CompileUsageList(resourceNode, resource.name);
        if (usageResult.IsFailure())
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(usageResult.Error());
        }

        auto sizeResult = CompileResourceSize(resourceNode, resource);
        if (sizeResult.IsFailure())
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(sizeResult.Error());
        }

        resource.format = format;
        resource.usage = std::move(usageResult.Value());
        resource.isSwapchain = resource.name == "swapChain";
        resourceNames.insert(resource.name);
        resourceFormats.emplace(resource.name, resource.format);

        CompiledFrameGraphResourceUsagePlan usagePlan;
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
            return RuntimeResult<CompiledFrameGraph>::Failure(MakeCompileError(
                "FrameGraphCompiler.InvalidPass",
                "Render graph pass entries must be objects with a string name."));
        }

        CompiledFrameGraphPass pass;
        pass.name = passNode["name"].get<std::string>();
        if (pass.name.empty() || passNames.find(pass.name) != passNames.end())
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(MakeCompileError(
                "FrameGraphCompiler.DuplicatePass",
                "Render graph pass names must be non-empty and unique.",
                pass.name));
        }

        auto inputResult = CompilePassInputs(passNode, pass.name, resourceNames);
        if (inputResult.IsFailure())
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(inputResult.Error());
        }
        auto outputResult = CompilePassOutputs(passNode, pass.name, resourceNames);
        if (outputResult.IsFailure())
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(outputResult.Error());
        }
        if (outputResult.Value().empty())
        {
            return RuntimeResult<CompiledFrameGraph>::Failure(MakeCompileError(
                "FrameGraphCompiler.EmptyPassOutput",
                "Render graph pass must write at least one output resource.",
                pass.name));
        }

        pass.needMsaa = passNode.value("needMsaa", false);
        pass.needCreateMaterial = passNode.value("needCreateMaterial", false);
        pass.materialInstancePath = passNode.value("materialInstancePath", std::string());
        pass.inputDescriptors = std::move(inputResult.Value());
        pass.inputResources.reserve(pass.inputDescriptors.size());
        for (const CompiledFrameGraphPassInputDescriptor& inputDescriptor : pass.inputDescriptors)
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
                    CompiledFrameGraphBarrierType::AttachmentToShaderRead);
            }
            AddResourceAccess(
                compiledGraph.resourceUsagePlans.at(resourceUsagePlanIndices.at(inputResource)),
                pass.name,
                passIndex,
                CompiledFrameGraphAccessType::Read);
        }
        for (const CompiledFrameGraphPassOutput& output : pass.outputResources)
        {
            if (output.resource != "swapChain" &&
                output.loadOp == "load" &&
                previousResourceUses[output.resource] == ResourcePreviousUse::Sampled)
            {
                AddBarrier(
                    pass,
                    output.resource,
                    CompiledFrameGraphBarrierType::ShaderReadToAttachment);
            }
            AddResourceAccess(
                compiledGraph.resourceUsagePlans.at(resourceUsagePlanIndices.at(output.resource)),
                pass.name,
                passIndex,
                CompiledFrameGraphAccessType::Write);
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
        for (const CompiledFrameGraphPassOutput& output : pass.outputResources)
        {
            previousResourceUses[output.resource] = ResourcePreviousUse::Attachment;
        }

        passNames.insert(pass.name);
        compiledGraph.passOrder.push_back(pass.name);
        compiledGraph.passes.push_back(std::move(pass));
        ++passIndex;
    }

    return RuntimeResult<CompiledFrameGraph>::Success(std::move(compiledGraph));
}

} // namespace VL
