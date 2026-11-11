#include "render/rendergraph/renderGraphCompiler.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "terrain/terrainFeedback.h"
#include "render/rendergraph/renderGraphCompilerInternal.h"

namespace VL
{
using namespace RenderGraphCompilerDetail;

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
        // tile 光照聚类不写任何图资源（输出是帧拥有的 SSBO），是唯一允许
        // 空 output 的 pass 类型；其余 pass 仍必须至少写一个资源。
        const bool allowsEmptyOutput =
            passNode.value("type", std::string()) == "lightCulling";
        if (outputResult.Value().empty() && !allowsEmptyOutput)
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
        if (pass.type == RenderGraphPassType::LightCulling)
        {
            // 聚类只消费帧级 buffer（灯光/网格），不采样图资源，也不建
            // VkRenderPass。任何 input/output/MSAA/material 都是作者错误。
            if (!inputResult.Value().empty() ||
                !outputResult.Value().empty() ||
                passNode.value("needMsaa", false) ||
                passNode.value("needCreateMaterial", false))
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidLightCulling",
                    "LightCulling must not declare graph inputs/outputs, MSAA, or a pass material.",
                    pass.name));
            }
        }
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
        if (pass.type == RenderGraphPassType::ParticleSoftDepth)
        {
            const auto& outputs = outputResult.Value();
            const auto& inputs = inputResult.Value();
            if (outputs.size() != 1 || inputs.empty() ||
                passNode.value("needMsaa", false) ||
                passNode.value("needCreateMaterial", false))
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidParticleSoftDepth",
                    "ParticleSoftDepth requires exactly one output, at least one input, and no MSAA/material.",
                    pass.name));
            }
            const CompiledRenderGraphResource* target = nullptr;
            for (const auto& resource : compiledGraph.resources)
            {
                if (resource.name == outputs[0].resource) target = &resource;
            }
            if (!target || target->type != "texture2D" ||
                target->format != "R32_SFLOAT" ||
                !target->initializeShaderRead ||
                std::find(target->usage.begin(), target->usage.end(), "storage") == target->usage.end() ||
                std::find(target->usage.begin(), target->usage.end(), "sampled") == target->usage.end())
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidParticleSoftDepth",
                    "ParticleSoftDepth output must be an initialized storage+sampled R32_SFLOAT texture2D.",
                    pass.name));
            }
            bool hasSceneDepthInput = false;
            for (const CompiledRenderGraphPassInputDescriptor& input : inputs)
            {
                if (input.source != "renderGraph")
                {
                    return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                        "RenderGraphCompiler.InvalidParticleSoftDepth",
                        "ParticleSoftDepth inputs must be graph resources with explicit bindings.",
                        pass.name));
                }
                hasSceneDepthInput = hasSceneDepthInput ||
                    input.resource == "sceneDepth";
            }
            if (!hasSceneDepthInput)
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidParticleSoftDepth",
                    "ParticleSoftDepth requires sceneDepth as an input.",
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

        if (pass.type == RenderGraphPassType::TerrainFeedback)
        {
            // feedback 合同：1..4 个 worldTexture 输入（覆盖页 page table，
            // L4b 起再加层 catalog / ID / 权重），每个输入对应一张固定
            // 128×128 RGBA8 颜色输出，最后再加一张 128×128 深度输出。
            // 深度测试让最近的地形页赢得 feedback texel，避免后绘制的远页
            // 覆盖近页 miss。禁止 MSAA/swapChain/GBuffer。
            const auto& outputs = pass.outputResources;
            const auto& inputs = pass.inputDescriptors;
            bool invalidInputs = inputs.empty() ||
                inputs.size() > 4 ||
                // 输出 = 1..5 张颜色附件 + 1 张深度；覆盖页 RT 不需要
                // 对应的 worldTexture 输入，所以不能把颜色数钉在输入数上。
                outputs.size() < 2 ||
                outputs.size() > 6;
            for (size_t inputIndex = 0;
                 !invalidInputs && inputIndex < inputs.size();
                 ++inputIndex)
            {
                invalidInputs =
                    inputs[inputIndex].source != "worldTexture" ||
                    inputs[inputIndex].binding != inputIndex;
            }
            if (pass.needMsaa ||
                !pass.needCreateMaterial ||
                pass.materialInstancePath.empty() ||
                !pass.pipelineState.useVertexInput ||
                !pass.pipelineState.depthTestEnable ||
                !pass.pipelineState.depthWriteEnable ||
                pass.pipelineState.depthCompareOp !=
                    CompiledDepthCompareOp::Less ||
                invalidInputs)
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidTerrainFeedback",
                    "TerrainFeedback requires 1..4 worldTexture inputs with bindings 0..N-1, "
                    "1..5 fixed color outputs and one fixed depth output with depth test.",
                    pass.name));
            }

            std::vector<const CompiledRenderGraphResource*> colorTargets;
            const CompiledRenderGraphResource* depthTarget = nullptr;
            for (const CompiledRenderGraphPassOutput& output : outputs)
            {
                if (output.layer != 0 ||
                    output.loadOp != "clear" ||
                    output.storeOp != "store")
                {
                    return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                        "RenderGraphCompiler.InvalidTerrainFeedback",
                        "TerrainFeedback outputs must be layer 0 with clear/store.",
                        pass.name + ":" + output.resource));
                }
                const CompiledRenderGraphResource* resource = nullptr;
                for (const auto& candidate : compiledGraph.resources)
                {
                    if (candidate.name == output.resource)
                    {
                        resource = &candidate;
                        break;
                    }
                }
                if (resource == nullptr)
                    continue;
                if (IsDepthFormatString(resource->format))
                    depthTarget = resource;
                else
                    colorTargets.push_back(resource);
            }
            if (colorTargets.empty() ||
                colorTargets.size() > 5 ||
                depthTarget == nullptr)
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidTerrainFeedback",
                    "TerrainFeedback requires 1..5 color outputs plus one depth output.",
                    pass.name));
            }
            for (const CompiledRenderGraphResource* colorTarget :
                 colorTargets)
            {
                if (colorTarget == depthTarget ||
                    colorTarget->isSwapchain ||
                    colorTarget->type != "texture2D" ||
                    colorTarget->format != "R8G8B8A8_UNORM" ||
                    !colorTarget->hasFixedWidth ||
                    !colorTarget->hasFixedHeight ||
                    colorTarget->widthValue !=
                        static_cast<float>(TerrainFeedbackResolution) ||
                    colorTarget->heightValue !=
                        static_cast<float>(TerrainFeedbackResolution) ||
                    std::find(
                        colorTarget->usage.begin(),
                        colorTarget->usage.end(),
                        "colorAttachment") == colorTarget->usage.end() ||
                    std::find(
                        colorTarget->usage.begin(),
                        colorTarget->usage.end(),
                        "transferSrc") == colorTarget->usage.end())
                {
                    return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                        "RenderGraphCompiler.InvalidTerrainFeedback",
                        "TerrainFeedback color outputs must be fixed 128x128 R8G8B8A8_UNORM colorAttachment+transferSrc textures.",
                        pass.name + ":" + colorTarget->name));
                }
            }
            if (depthTarget->isSwapchain ||
                depthTarget->type != "texture2D" ||
                depthTarget->format != "D32_SFLOAT" ||
                !depthTarget->hasFixedWidth ||
                !depthTarget->hasFixedHeight ||
                depthTarget->widthValue !=
                    static_cast<float>(TerrainFeedbackResolution) ||
                depthTarget->heightValue !=
                    static_cast<float>(TerrainFeedbackResolution) ||
                std::find(
                    depthTarget->usage.begin(),
                    depthTarget->usage.end(),
                    "depthStencilAttachment") == depthTarget->usage.end())
            {
                return RuntimeResult<CompiledRenderGraph>::Failure(MakeCompileError(
                    "RenderGraphCompiler.InvalidTerrainFeedback",
                    "TerrainFeedback depth output must be a fixed 128x128 D32_SFLOAT "
                    "depthStencilAttachment texture.",
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
                pass.type == RenderGraphPassType::ParticleSoftDepth ||
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
                pass.type == RenderGraphPassType::ParticleSoftDepth ||
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
