#include "render/backend/rendererDescriptorPlan.h"

#include <stdexcept>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "pipeline/pipelineBase.h"
#include "shaderReflect.h"

namespace VL
{
namespace
{

void BuildPassInputDescriptorUpdates(
    const std::vector<CompiledRenderGraphPassInputDescriptor>& inputDescriptorPlan,
    RendererPassDescriptorPlan& plan)
{
    for (const CompiledRenderGraphPassInputDescriptor& inputDescriptor : inputDescriptorPlan)
    {
        RendererDescriptorUpdate update;
        update.setIndex = PassSetIndex;
        update.binding = inputDescriptor.binding;
        update.passInputBinding = inputDescriptor.binding;
        update.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        update.source = RendererDescriptorUpdateSource::PassInputTexture;
        update.resourceName = inputDescriptor.resource;
        plan.updates.push_back(std::move(update));
    }
}

void BuildMaterialDescriptorUpdates(
    const MaterialInstance& passMaterialInstance,
    RendererPassDescriptorPlan& plan)
{
    std::shared_ptr<Material> baseMaterial = passMaterialInstance.GetBaseMaterial().lock();
    if (!baseMaterial)
    {
        return;
    }

    for (const ShaderBinding& binding : baseMaterial->GetRenderPipeline()->GetShaderBindings())
    {
        RendererDescriptorUpdate update;
        update.setIndex = binding.set;
        update.binding = binding.binding;
        update.passInputBinding = binding.binding;
        update.descriptorType = binding.type;
        update.resourceName = binding.name;

        if (binding.type == vk::DescriptorType::eUniformBuffer)
        {
            if (binding.set == GlobalSetIndex)
            {
                update.source = RendererDescriptorUpdateSource::GlobalUniform;
            }
            else if (binding.set == MaterialSetIndex)
            {
                update.source = RendererDescriptorUpdateSource::MaterialUniform;
            }
            else
            {
                // Object UBO descriptors are written by RendererObjectResourceManager.
                // Pass descriptor plans only cover pass/global/material bindings.
                continue;
            }
        }
        else if (binding.type == vk::DescriptorType::eStorageBuffer)
        {
            update.source = RendererDescriptorUpdateSource::LightStorage;
        }
        else if (binding.type == vk::DescriptorType::eCombinedImageSampler)
        {
            if (binding.set == GlobalSetIndex)
            {
                update.source = RendererDescriptorUpdateSource::GlobalTexture;
            }
            else if (binding.set == PassSetIndex)
            {
                update.source = RendererDescriptorUpdateSource::PassInputTexture;
            }
            else
            {
                update.source = RendererDescriptorUpdateSource::MaterialTexture;
            }
        }
        else
        {
            continue;
        }

        plan.updates.push_back(std::move(update));
    }
}

} // namespace

void RendererDescriptorPlanCache::Clear()
{
    passPlans.clear();
}

void RendererDescriptorPlanCache::RebuildPassPlan(
    const std::string& passName,
    const std::vector<CompiledRenderGraphPassInputDescriptor>& inputDescriptorPlan,
    const std::weak_ptr<MaterialInstance>& passMaterialInstance)
{
    RendererPassDescriptorPlan plan;

    std::shared_ptr<MaterialInstance> materialInstance = passMaterialInstance.lock();
    if (materialInstance)
    {
        BuildMaterialDescriptorUpdates(*materialInstance, plan);
    }
    else
    {
        BuildPassInputDescriptorUpdates(inputDescriptorPlan, plan);
    }

    passPlans[passName] = std::move(plan);
}

const RendererPassDescriptorPlan& RendererDescriptorPlanCache::GetPassPlan(
    const std::string& passName) const
{
    auto planIt = passPlans.find(passName);
    if (planIt == passPlans.end())
    {
        throw std::runtime_error("RendererDescriptorPlanCache is missing a plan for pass: " + passName);
    }

    return planIt->second;
}

} // namespace VL
