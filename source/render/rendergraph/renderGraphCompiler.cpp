#include "render/rendergraph/renderGraphCompiler.h"

#include <algorithm>
#include <cmath>
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
    Attachment,
    Storage
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
        "A2B10G10R10_UNORM_PACK32",
        "R8G8B8A8_SRGB",
        "R8G8B8A8_UNORM",
        "R16G16B16A16_SFLOAT",
        "R32_SFLOAT",
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
        "transferDst",
        "storage"
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

RuntimeResult<CompiledStencilCompareOp> CompileStencilCompareOp(
    const nlohmann::json& stencilNode,
    const std::string& passName)
{
    if (!stencilNode.contains("compareOp"))
    {
        return RuntimeResult<CompiledStencilCompareOp>::Success(
            CompiledStencilCompareOp::Always);
    }
    if (!stencilNode["compareOp"].is_string())
    {
        return RuntimeResult<CompiledStencilCompareOp>::Failure(
            MakeCompileError(
                "RenderGraphCompiler.InvalidPipelineState",
                "Render graph pass stencil.compareOp must be a string.",
                passName));
    }

    const std::string compareOp =
        stencilNode["compareOp"].get<std::string>();
    static const std::unordered_map<std::string, CompiledStencilCompareOp>
        kCompareOps = {
            {"never", CompiledStencilCompareOp::Never},
            {"less", CompiledStencilCompareOp::Less},
            {"equal", CompiledStencilCompareOp::Equal},
            {"lessOrEqual", CompiledStencilCompareOp::LessOrEqual},
            {"greater", CompiledStencilCompareOp::Greater},
            {"notEqual", CompiledStencilCompareOp::NotEqual},
            {"greaterOrEqual", CompiledStencilCompareOp::GreaterOrEqual},
            {"always", CompiledStencilCompareOp::Always}};
    const auto compareIt = kCompareOps.find(compareOp);
    if (compareIt == kCompareOps.end())
    {
        return RuntimeResult<CompiledStencilCompareOp>::Failure(
            MakeCompileError(
                "RenderGraphCompiler.InvalidPipelineState",
                "Render graph pass stencil.compareOp is unsupported.",
                passName));
    }
    return RuntimeResult<CompiledStencilCompareOp>::Success(
        compareIt->second);
}

RuntimeResult<CompiledStencilOp> CompileStencilOp(
    const nlohmann::json& stencilNode,
    const char* fieldName,
    const std::string& passName)
{
    if (!stencilNode.contains(fieldName))
    {
        return RuntimeResult<CompiledStencilOp>::Success(
            CompiledStencilOp::Keep);
    }
    if (!stencilNode[fieldName].is_string())
    {
        return RuntimeResult<CompiledStencilOp>::Failure(
            MakeCompileError(
                "RenderGraphCompiler.InvalidPipelineState",
                "Render graph pass stencil operation must be a string: " +
                    std::string(fieldName),
                passName));
    }

    const std::string op = stencilNode[fieldName].get<std::string>();
    static const std::unordered_map<std::string, CompiledStencilOp>
        kStencilOps = {
            {"keep", CompiledStencilOp::Keep},
            {"zero", CompiledStencilOp::Zero},
            {"replace", CompiledStencilOp::Replace},
            {"incrementAndClamp", CompiledStencilOp::IncrementAndClamp},
            {"decrementAndClamp", CompiledStencilOp::DecrementAndClamp},
            {"invert", CompiledStencilOp::Invert},
            {"incrementAndWrap", CompiledStencilOp::IncrementAndWrap},
            {"decrementAndWrap", CompiledStencilOp::DecrementAndWrap}};
    const auto opIt = kStencilOps.find(op);
    if (opIt == kStencilOps.end())
    {
        return RuntimeResult<CompiledStencilOp>::Failure(
            MakeCompileError(
                "RenderGraphCompiler.InvalidPipelineState",
                "Render graph pass stencil operation is unsupported: " +
                    std::string(fieldName),
                passName));
    }
    return RuntimeResult<CompiledStencilOp>::Success(opIt->second);
}

RuntimeResult<uint8_t> CompileStencilMaskField(
    const nlohmann::json& stencilNode,
    const char* fieldName,
    uint8_t defaultValue,
    const std::string& passName)
{
    if (!stencilNode.contains(fieldName))
    {
        return RuntimeResult<uint8_t>::Success(defaultValue);
    }
    if (!stencilNode[fieldName].is_number_unsigned())
    {
        return RuntimeResult<uint8_t>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidPipelineState",
            "Render graph pass stencil mask/reference must be an unsigned "
            "integer in [0, 255]: " + std::string(fieldName),
            passName));
    }
    const uint64_t value =
        stencilNode[fieldName].get<uint64_t>();
    if (value > 0xFFu)
    {
        return RuntimeResult<uint8_t>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidPipelineState",
            "Render graph pass stencil mask/reference exceeds 255: " +
                std::string(fieldName),
            passName));
    }
    return RuntimeResult<uint8_t>::Success(
        static_cast<uint8_t>(value));
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

    if (stateNode.contains("stencil"))
    {
        const nlohmann::json& stencilNode = stateNode["stencil"];
        if (!stencilNode.is_object())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                MakeCompileError(
                    "RenderGraphCompiler.InvalidPipelineState",
                    "Render graph pass stencil state must be an object.",
                    passName));
        }
        if (stencilNode.contains("testEnable") &&
            !stencilNode["testEnable"].is_boolean())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                MakeCompileError(
                    "RenderGraphCompiler.InvalidPipelineState",
                    "Render graph pass stencil.testEnable must be boolean.",
                    passName));
        }

        const char* maskFields[] = {
            "writeMask", "compareMask", "reference"};
        for (const char* fieldName : maskFields)
        {
            if (stencilNode.contains(fieldName) &&
                !stencilNode[fieldName].is_number_unsigned())
            {
                return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                    MakeCompileError(
                        "RenderGraphCompiler.InvalidPipelineState",
                        "Render graph pass stencil mask/reference must be an "
                        "unsigned integer: " + std::string(fieldName),
                        passName));
            }
        }

        pipelineState.stencilTestEnable =
            stencilNode.value("testEnable", false);
        auto writeMaskResult = CompileStencilMaskField(
            stencilNode, "writeMask", pipelineState.stencilWriteMask,
            passName);
        auto compareMaskResult = CompileStencilMaskField(
            stencilNode, "compareMask", pipelineState.stencilCompareMask,
            passName);
        auto referenceResult = CompileStencilMaskField(
            stencilNode, "reference", pipelineState.stencilReference,
            passName);
        if (writeMaskResult.IsFailure())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                writeMaskResult.Error());
        }
        if (compareMaskResult.IsFailure())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                compareMaskResult.Error());
        }
        if (referenceResult.IsFailure())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                referenceResult.Error());
        }
        pipelineState.stencilWriteMask = writeMaskResult.Value();
        pipelineState.stencilCompareMask = compareMaskResult.Value();
        pipelineState.stencilReference = referenceResult.Value();

        auto compareOpResult =
            CompileStencilCompareOp(stencilNode, passName);
        auto failOpResult =
            CompileStencilOp(stencilNode, "failOp", passName);
        auto passOpResult =
            CompileStencilOp(stencilNode, "passOp", passName);
        auto depthFailOpResult =
            CompileStencilOp(stencilNode, "depthFailOp", passName);
        if (compareOpResult.IsFailure())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                compareOpResult.Error());
        }
        if (failOpResult.IsFailure())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                failOpResult.Error());
        }
        if (passOpResult.IsFailure())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                passOpResult.Error());
        }
        if (depthFailOpResult.IsFailure())
        {
            return RuntimeResult<CompiledGraphicsPipelineState>::Failure(
                depthFailOpResult.Error());
        }
        pipelineState.stencilCompareOp = compareOpResult.Value();
        pipelineState.stencilFailOp = failOpResult.Value();
        pipelineState.stencilPassOp = passOpResult.Value();
        pipelineState.stencilDepthFailOp = depthFailOpResult.Value();
    }
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
        const std::string source = inputNode.value(
            "source",
            std::string("renderGraph"));
        if (source != "renderGraph" && source != "worldTexture")
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedInputSource",
                "Render graph pass input source must be renderGraph or worldTexture.",
                passName + ":" + resourceName));
        }
        if (source == "renderGraph" &&
            resourceNames.find(resourceName) == resourceNames.end())
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassInputDescriptor>>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnknownInputResource",
                "Render graph pass input references a resource that is not declared.",
                passName + ":" + resourceName));
        }

        CompiledRenderGraphPassInputDescriptor descriptor;
        descriptor.resource = resourceName;
        descriptor.source = source;
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
        output.stencilLoadOp =
            outputNode.value("stencilLoadOp", std::string("dontCare"));
        output.stencilStoreOp =
            outputNode.value("stencilStoreOp", std::string("dontCare"));
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
        if (!IsSupportedLoadOp(output.stencilLoadOp))
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedStencilLoadOp",
                "Render graph pass output stencilLoadOp is not supported.",
                passName + ":" + output.resource + ":" + output.stencilLoadOp));
        }
        if (!IsSupportedStoreOp(output.stencilStoreOp))
        {
            return RuntimeResult<std::vector<CompiledRenderGraphPassOutput>>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedStencilStoreOp",
                "Render graph pass output stencilStoreOp is not supported.",
                passName + ":" + output.resource + ":" + output.stencilStoreOp));
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

RuntimeResult<void> ValidateRequiredSingletonPassType(
    const CompiledRenderGraph& compiledGraph,
    RenderGraphPassType requiredType)
{
    size_t passCount = 0;
    std::string passNames;
    for (const CompiledRenderGraphPass& pass : compiledGraph.passes)
    {
        if (pass.type != requiredType)
        {
            continue;
        }

        if (!passNames.empty())
        {
            passNames += ", ";
        }
        passNames += pass.name;
        ++passCount;
    }

    if (passCount == 1)
    {
        return RuntimeResult<void>::Success();
    }

    const std::string typeName(
        RenderGraphPassTypeToString(requiredType));
    return RuntimeResult<void>::Failure(MakeCompileError(
        "RenderGraphCompiler.InvalidSingletonPassType",
        "Render graph must contain exactly one pass of type '" +
            typeName + "'.",
        passNames.empty()
            ? typeName + ":count=0"
            : typeName + ":count=" +
                std::to_string(passCount) +
                ":passes=" + passNames));
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
        if (resourceNode.contains("clearColor"))
        {
            const nlohmann::json& clearColor = resourceNode["clearColor"];
            if (!clearColor.is_array() || clearColor.size() != 4)
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidResourceClearColor",
                    "Render graph resource clearColor must be a four-component array.",
                    resource.name));
            }
            for (size_t component = 0; component < 4; ++component)
            {
                if (!clearColor[component].is_number() ||
                    !std::isfinite(clearColor[component].get<double>()))
                {
                    return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                        "RenderGraphCompiler.InvalidResourceClearColor",
                        "Render graph resource clearColor components must be finite numbers.",
                        resource.name));
                }
                resource.clearColor[component] =
                    clearColor[component].get<float>();
            }
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
        if (resourceNode.contains("initialLayout"))
        {
            if (!resourceNode["initialLayout"].is_string() ||
                resourceNode["initialLayout"].get<std::string>() != "shaderRead")
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.UnsupportedInitialLayout",
                    "Only initialLayout=shaderRead is supported.",
                    resource.name));
            }
            resource.initializeShaderRead = true;
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
        const std::string passTypeName =
            passNode["type"].get<std::string>();
        const std::optional<RenderGraphPassType> passType =
            ParseRenderGraphPassType(passTypeName);
        if (!passType.has_value())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.UnsupportedPassType",
                "Render graph pass type is not supported.",
                pass.name + ":" + passTypeName));
        }
        pass.type = *passType;
        if (pass.type == RenderGraphPassType::AutoExposure)
        {
            // 专用曝光节点不是通用 Compute authoring：一张 sceneColor 输入，
            // 一张初始化为 shaderRead 的 1x1 R32 输出；历史由节点 owner 私有持有。
            const auto& outputs = outputResult.Value();
            const auto& inputs = inputResult.Value();
            if (inputs.size() != 1 || inputs[0].source != "renderGraph" ||
                inputs[0].binding != 0 || outputs.size() != 1 ||
                (outputs.size() == 1 && (outputs[0].layer != 0 ||
                    outputs[0].loadOp != "clear" || outputs[0].storeOp != "store")) ||
                passNode.value("needMsaa", false) || passNode.value("needCreateMaterial", false))
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidAutoExposure", "AutoExposure requires one graph input/output and no MSAA/material.", pass.name));
            const CompiledRenderGraphResource* target = nullptr;
            const CompiledRenderGraphResource* source = nullptr;
            for (const auto& resource : compiledGraph.resources)
            {
                if (resource.name == outputs[0].resource) target = &resource;
                if (resource.name == inputs[0].resource) source = &resource;
            }
            if (!target || !source || target == source ||
                source->isSwapchain || IsDepthFormatString(source->format) ||
                source->type != "texture2D" ||
                target->format != "R32_SFLOAT" || target->type != "texture2D" ||
                !target->initializeShaderRead || !target->hasFixedWidth || !target->hasFixedHeight ||
                target->widthValue != 1 || target->heightValue != 1 ||
                std::find(source->usage.begin(), source->usage.end(), "sampled") == source->usage.end() ||
                std::find(target->usage.begin(), target->usage.end(), "storage") == target->usage.end() ||
                std::find(target->usage.begin(), target->usage.end(), "sampled") == target->usage.end())
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidAutoExposure", "AutoExposure requires sampled scene color and initialized 1x1 R32 storage/sampled output.", pass.name));
            if (previousResourceUses[inputs[0].resource] == ResourcePreviousUse::None)
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidAutoExposure", "AutoExposure scene color must be produced earlier in this graph.", pass.name));
        }
        if (pass.type == RenderGraphPassType::VolumetricFog)
        {
            // 体积雾注入是专用 compute 节点：输出必须是初始化到 shaderRead
            // 的 storage+sampled texture2DArray（每个数组层保存一个深度层），
            // 且不参与 MSAA / pass material。
            const auto& outputs = outputResult.Value();
            const auto& inputs = inputResult.Value();
            if (outputs.size() != 1 || inputs.empty() ||
                passNode.value("needMsaa", false) ||
                passNode.value("needCreateMaterial", false))
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidVolumetricFog",
                    "VolumetricFog requires exactly one output, at least one input, and no MSAA/material.",
                    pass.name));
            }
            const CompiledRenderGraphResource* target = nullptr;
            for (const auto& resource : compiledGraph.resources)
            {
                if (resource.name == outputs[0].resource) target = &resource;
            }
            if (!target || target->type != "texture2DArray" ||
                !target->initializeShaderRead || target->arrayLayers < 2 ||
                std::find(target->usage.begin(), target->usage.end(), "storage") == target->usage.end() ||
                std::find(target->usage.begin(), target->usage.end(), "sampled") == target->usage.end())
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidVolumetricFog",
                    "VolumetricFog output must be an initialized storage+sampled texture2DArray with at least two layers.",
                    pass.name));
            }
            bool hasSceneDepthInput = false;
            for (const CompiledRenderGraphPassInputDescriptor& input : inputs)
            {
                if (input.source != "renderGraph")
                {
                    return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                        "RenderGraphCompiler.InvalidVolumetricFog",
                        "VolumetricFog inputs must be graph resources with explicit bindings.",
                        pass.name));
                }
                hasSceneDepthInput = hasSceneDepthInput ||
                    input.resource == "sceneDepth";
            }
            if (!hasSceneDepthInput)
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidVolumetricFog",
                    "VolumetricFog requires sceneDepth as an input.",
                    pass.name));
            }
        }
        if (pass.type == RenderGraphPassType::FogComposite)
        {
            // 合成 pass 只允许写 sceneColor；fog volume 必须作为采样输入，
            // 否则上一阶段的 storage 布局不会被转换回 shaderRead。
            const auto& outputs = outputResult.Value();
            const auto& inputs = inputResult.Value();
            if (outputs.size() != 1 ||
                outputs[0].resource != "sceneColor" ||
                outputs[0].loadOp != "load" ||
                inputs.size() != 2 ||
                passNode.value("needMsaa", false) ||
                passNode.value("needCreateMaterial", false))
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidFogComposite",
                    "FogComposite requires fog volume + sceneDepth inputs and a single sceneColor LOAD output.",
                    pass.name));
            }
        }
        pass.needMsaa = passNode.value("needMsaa", false);
        pass.needCreateMaterial = passNode.value("needCreateMaterial", false);
        pass.materialInstancePath = passNode.value("materialInstancePath", std::string());
        if (pass.type == RenderGraphPassType::Shadow &&
            !pass.needCreateMaterial)
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
            if (inputDescriptor.source == "renderGraph")
            {
                pass.inputResources.push_back(inputDescriptor.resource);
            }
        }
        pass.outputResources = std::move(outputResult.Value());

        // 同一 pass 不能既把资源作为 sampled input，又把它作为 attachment。
        // 典型错误是 decal 直接采样 sceneDepth 同时把它写回；这种反馈环在
        // Vulkan 上是未定义行为，必须在编译期拒绝。
        for (const std::string& inputResource : pass.inputResources)
        {
            for (const CompiledRenderGraphPassOutput& output :
                 pass.outputResources)
            {
                if (inputResource == output.resource)
                {
                    return RuntimeResult<CompiledRenderGraph>::Failure(
                        MakeCompileError(
                            "RenderGraphCompiler.PassFeedbackLoop",
                            "Render graph pass cannot sample and attach the "
                            "same resource.",
                            pass.name + ":" + inputResource));
                }
            }
        }

        // stencil test 只有在 pass 真正挂载带 stencil 分量的深度附件时
        // 才有意义；否则 Vulkan 管线创建会失败或行为未定义。
        if (pass.pipelineState.stencilTestEnable)
        {
            bool hasStencilAttachment = false;
            for (const CompiledRenderGraphPassOutput& output :
                 pass.outputResources)
            {
                const auto formatIt =
                    resourceFormats.find(output.resource);
                if (formatIt != resourceFormats.end() &&
                    formatIt->second == "D24_UNORM_S8_UINT")
                {
                    hasStencilAttachment = true;
                    break;
                }
            }
            if (!hasStencilAttachment)
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(
                    MakeCompileError(
                        "RenderGraphCompiler.MissingStencilAttachment",
                        "Render graph pass enables stencil test without a "
                        "depth-stencil attachment.",
                        pass.name));
            }
        }

        for (const std::string& inputResource : pass.inputResources)
        {
            if (previousResourceUses[inputResource] == ResourcePreviousUse::Attachment)
            {
                AddBarrier(
                    pass,
                    inputResource,
                    CompiledRenderGraphBarrierType::AttachmentToShaderRead);
            }
            else if (previousResourceUses[inputResource] == ResourcePreviousUse::Storage)
                AddBarrier(pass, inputResource, CompiledRenderGraphBarrierType::StorageToShaderRead);
            AddResourceAccess(
                compiledGraph.resourceUsagePlans.at(resourceUsagePlanIndices.at(inputResource)),
                pass.name,
                passIndex,
                CompiledRenderGraphAccessType::Read);
        }
        for (const CompiledRenderGraphPassOutput& output : pass.outputResources)
        {
            // 存储写 pass（AutoExposure / VolumetricFog）输出的是 storage image，
            // 不参与 framebuffer attachment 布局；统一从 shaderRead 转到 GENERAL。
            const bool isStorageWritePass =
                pass.type == RenderGraphPassType::AutoExposure ||
                pass.type == RenderGraphPassType::VolumetricFog;
            if (isStorageWritePass)
                AddBarrier(pass, output.resource, CompiledRenderGraphBarrierType::ShaderReadToStorage);
            if (!isStorageWritePass && output.resource != "swapChain" &&
                output.loadOp == "load" &&
                previousResourceUses[output.resource] == ResourcePreviousUse::Sampled)
            {
                AddBarrier(
                    pass,
                    output.resource,
                    CompiledRenderGraphBarrierType::ShaderReadToAttachment);
            }
            else if (!isStorageWritePass &&
                output.resource != "swapChain" &&
                output.loadOp == "load" &&
                previousResourceUses[output.resource] == ResourcePreviousUse::Attachment)
            {
                AddBarrier(
                    pass,
                    output.resource,
                    CompiledRenderGraphBarrierType::AttachmentToAttachment);
            }
            else if (!isStorageWritePass &&
                output.resource != "swapChain" &&
                output.loadOp == "load" &&
                previousResourceUses[output.resource] ==
                    ResourcePreviousUse::None)
            {
                // 第一次写入就 LOAD：图像可能从未初始化。显式做
                // UNDEFINED -> attachment，避免 loadOp=LOAD 与 initialLayout
                // 不一致，也让后续没有生产者的切片处于合法布局。
                AddBarrier(
                    pass,
                    output.resource,
                    CompiledRenderGraphBarrierType::UndefinedToAttachment);
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
            const bool isStorageWritePass =
                pass.type == RenderGraphPassType::AutoExposure ||
                pass.type == RenderGraphPassType::VolumetricFog;
            previousResourceUses[output.resource] = isStorageWritePass
                ? ResourcePreviousUse::Storage : ResourcePreviousUse::Attachment;
        }

        passNames.insert(pass.name);
        compiledGraph.passOrder.push_back(pass.name);
        compiledGraph.passes.push_back(std::move(pass));
        ++passIndex;
    }

    const CompiledRenderGraphPass* exposurePass = nullptr;
    for (const auto& pass : compiledGraph.passes)
    {
        if (pass.type != RenderGraphPassType::AutoExposure) continue;
        if (exposurePass)
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidAutoExposure", "Only one AutoExposure update per graph is supported.", pass.name));
        exposurePass = &pass;
    }
    if (exposurePass)
    {
        const auto& output = exposurePass->outputResources[0].resource;
        const auto& uses = compiledGraph.resourceUsagePlans.at(resourceUsagePlanIndices.at(output));
        if (uses.accesses.size() < 2 || uses.accesses.front().passName != exposurePass->name ||
            uses.accesses.back().accessType != CompiledRenderGraphAccessType::Read)
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidAutoExposure", "Exposure output must be written before sampling and end in shaderRead.", output));
        for (const auto& use : uses.accesses)
            if (use.accessType == CompiledRenderGraphAccessType::Write && use.passName != exposurePass->name)
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidAutoExposure", "Exposure output cannot have another writer.", output));
    }

    // 体积雾注入/合成各只允许一个：注入端按层写同一张图，合成端只做一次
    // 相邻层插值；出现第二个节点会让 storage 写入互相覆盖。
    const CompiledRenderGraphPass* fogInjectionPass = nullptr;
    size_t fogInjectionIndex = 0;
    const CompiledRenderGraphPass* fogCompositePass = nullptr;
    size_t fogCompositeIndex = 0;
    for (size_t passIndex = 0; passIndex < compiledGraph.passes.size(); ++passIndex)
    {
        const auto& pass = compiledGraph.passes[passIndex];
        if (pass.type == RenderGraphPassType::VolumetricFog)
        {
            if (fogInjectionPass)
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.DuplicateVolumetricFog",
                    "Only one VolumetricFog pass per graph is supported.",
                    pass.name));
            fogInjectionPass = &pass;
            fogInjectionIndex = passIndex;
        }
        if (pass.type == RenderGraphPassType::FogComposite)
        {
            if (fogCompositePass)
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.DuplicateFogComposite",
                    "Only one FogComposite pass per graph is supported.",
                    pass.name));
            fogCompositePass = &pass;
            fogCompositeIndex = passIndex;
        }
    }
    if ((fogInjectionPass == nullptr) != (fogCompositePass == nullptr))
    {
        return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
            "RenderGraphCompiler.IncompleteVolumetricFog",
            "VolumetricFog and FogComposite must be declared together.",
            fogInjectionPass ? fogInjectionPass->name : fogCompositePass->name));
    }
    if (fogInjectionPass && fogInjectionIndex > fogCompositeIndex)
    {
        return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
            "RenderGraphCompiler.InvalidVolumetricFogOrder",
            "VolumetricFog must run before FogComposite.",
            fogInjectionPass->name));
    }
    if (fogInjectionPass)
    {
        const std::string& fogResource =
            fogInjectionPass->outputResources[0].resource;
        if (std::find(
                fogCompositePass->inputResources.begin(),
                fogCompositePass->inputResources.end(),
                fogResource) ==
            fogCompositePass->inputResources.end())
        {
            return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                "RenderGraphCompiler.InvalidVolumetricFogOrder",
                "FogComposite must sample the VolumetricFog output.",
                fogCompositePass->name));
        }
    }

    auto geometryPassResult = ValidateRequiredSingletonPassType(
        compiledGraph,
        RenderGraphPassType::Geometry);
    if (geometryPassResult.IsFailure())
    {
        return RuntimeResult<CompiledRenderGraph>::Failure(
            geometryPassResult.Error());
    }
    auto forwardOpaquePassResult =
        ValidateRequiredSingletonPassType(
            compiledGraph,
            RenderGraphPassType::ForwardOpaque);
    if (forwardOpaquePassResult.IsFailure())
    {
        return RuntimeResult<CompiledRenderGraph>::Failure(
            forwardOpaquePassResult.Error());
    }
    auto forwardTransparentPassResult =
        ValidateRequiredSingletonPassType(
            compiledGraph,
            RenderGraphPassType::ForwardTransparent);
    if (forwardTransparentPassResult.IsFailure())
    {
        return RuntimeResult<CompiledRenderGraph>::Failure(
            forwardTransparentPassResult.Error());
    }

    return RuntimeResult<CompiledRenderGraph>::Success(std::move(compiledGraph));
}

} // namespace VL
