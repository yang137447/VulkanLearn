#include "render/rendergraph/renderGraphCompiler.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "terrain/terrainFeedback.h"
#include "render/rendergraph/renderGraphCompilerInternal.h"

namespace VL
{
namespace RenderGraphCompilerDetail
{

RuntimeError MakeCompileError(
    const std::string& code,
    const std::string& message,
    const std::string& sourceContext)
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
        "D32_SFLOAT_S8_UINT",
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
        format == "D32_SFLOAT_S8_UINT" ||
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

} // namespace RenderGraphCompilerDetail
} // namespace VL
