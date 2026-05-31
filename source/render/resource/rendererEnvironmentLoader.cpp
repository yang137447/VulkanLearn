#include "render/resource/rendererEnvironmentLoader.h"

#include <memory>
#include <string>

#include "pipeline/brdfLutGenerator.h"
#include "pipeline/environmentCubemapGenerator.h"
#include "pipeline/environmentPrefilterGenerator.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererResourceCache.h"
#include "texture.h"

namespace VL
{

RendererEnvironmentLoader::RendererEnvironmentLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
{
}

void RendererEnvironmentLoader::LoadGlobalResources() const
{
    if (RendererResourceCache::GetInstance().HasGlobalTexture("brdfLut"))
    {
        return;
    }

    RendererResourceCache::GetInstance().BindGlobalTexture(
        "brdfLut",
        BrdfLutGenerator::Generate(pipelineFactory, rendererBackend));
}

void RendererEnvironmentLoader::LoadEnvironmentObject(const nlohmann::basic_json<>& node) const
{
    const std::string environmentHdrPath = node.value("hdrPath", std::string());
    const uint32_t environmentCubeSize = node.value("cubeSize", 512u);

    if (environmentHdrPath.empty())
    {
        return;
    }

    RendererResourceCache& resourceCache = RendererResourceCache::GetInstance();
    std::shared_ptr<Texture> environmentCube =
        EnvironmentCubemapGenerator::Generate(
            environmentHdrPath,
            environmentCubeSize,
            pipelineFactory,
            rendererBackend);
    resourceCache.BindWorldTexture("environmentCube", environmentCube);
    if (environmentCube != nullptr)
    {
        resourceCache.BindWorldTexture(
            "prefilteredEnvironmentCube",
            EnvironmentPrefilterGenerator::Generate(
                *environmentCube,
                environmentCubeSize,
                pipelineFactory,
                rendererBackend));
    }
}

} // namespace VL
