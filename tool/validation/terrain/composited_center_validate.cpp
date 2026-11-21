#include "render/resource/terrainCompositedBakeGpu.h"

#include <iostream>
#include <stdexcept>

int main()
{
    using Baker = VL::TerrainCompositedBakeGpu;
    Baker::InputSnapshot input;
    auto texture = [] {
        Baker::SampledTexture result;
        result.descriptor = vk::DescriptorImageInfo(
            vk::Sampler(reinterpret_cast<VkSampler>(uintptr_t(1))),
            vk::ImageView(reinterpret_cast<VkImageView>(uintptr_t(1))),
            vk::ImageLayout::eShaderReadOnlyOptimal);
        result.width = 128;
        result.height = 128;
        return result;
    };
    for (auto* target : {&input.coveragePageTable, &input.coverageIds, &input.coverageWeights,
            &input.layerCatalog, &input.layerAlbedo, &input.layerNormal, &input.layerOrm,
            &input.surfaceLocalNormal, &input.surfaceLocalTangent})
        *target = texture();
    input.layerCatalog.width = 256;
    input.layerCatalog.height = 4;
    input.layerAlbedo.encoding = Baker::Encoding::SrgbView;
    input.retainFragmentPostprocess = true;
    input.contentIdentity = "center-filter-validation";
    input.globalToSourceSlice[13] = 0;
    Baker::PageRequest page;
    if (page.areaSamplesPerAxis != 4)
        throw std::runtime_error("PageRequest default must remain 4");
    page.virtualWidth = 4096;
    page.virtualHeight = 4096;
    uint32_t cases = 0;
    for (uint32_t mip : {0u, 1u, 2u, 3u, 12u, 31u, 32u})
        for (uint32_t samples : {0u, 1u, 2u, 4u, 8u, 9u})
        {
            page.virtualMip = mip;
            page.areaSamplesPerAxis = samples;
            const bool expected = mip <= 31 && samples >= 1 && samples <= 8 &&
                (samples != 1 || mip == 0);
            std::string error;
            const bool actual = Baker::Validate(input, page, error);
            if (actual != expected)
                throw std::runtime_error("Unexpected Validate mip=" + std::to_string(mip) +
                    " samples=" + std::to_string(samples) + ": " + error);
            ++cases;
        }
    page.virtualMip = 0;
    page.areaSamplesPerAxis = 1;
    input.contentIdentity.clear();
    std::string error;
    if (Baker::Validate(input, page, error))
        throw std::runtime_error("Center exception bypassed immutable input checks");
    std::cout << "actualProductionValidate cases=" << cases + 1 <<
        " defaultSamples=4 mip0Center=accepted coarseCenter=rejected otherInputGuards=retained\n";
}
