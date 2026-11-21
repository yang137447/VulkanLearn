#include "terrainLitCaptureAdapter.h"
#include "render/pass/passRuntime.h"
#include "shader/build/contentHash.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace VL
{
TerrainLitSessionFactory LoadNativeTerrainLitCase(const std::filesystem::path& path)
{
    using Json=nlohmann::json;
    std::ifstream stream(path);
    stream.exceptions(std::ios::badbit|std::ios::failbit);
    Json document;stream>>document;
    if(document.at("kind")!="native-terrain-lit-replay-v1")
        throw std::runtime_error("Explicit test-only lit case kind invalid");
    for(const auto& input:document.at("pinnedInputs"))
        if(ContentHasher::HashFile(input.at("path").get<std::string>()).ToHex()!=input.at("blake3"))
            throw std::runtime_error("Pinned native lit/reference input bytes changed");
    TerrainLitReplayConfig config;
    config.rect.x=document.at("rect").at(0);config.rect.y=document.at("rect").at(1);
    config.rect.width=document.at("rect").at(2);config.rect.height=document.at("rect").at(3);
    config.expandedLightingFragment=document.at("expandedLightingFragment").get<std::string>();
    config.expandedToneMappingFragment=document.at("expandedToneMappingFragment").get<std::string>();
    config.output=document.at("output").get<std::string>();
    config.inputClosureSha256=document.at("inputClosureSha256");
    config.originalEvaluatorSha256=document.at("originalEvaluatorSha256");
    config.referencePageSetSha256=document.at("referencePageSetSha256");
    config.allActualRoiSampledSlots=document.at("actualRoiSlotFootprint").get<std::vector<uint32_t>>();
    std::vector<TerrainReferencePage> pages;
    for(const auto& page:document.at("originalGpuPages"))
    {
        TerrainReferencePage entry;
        entry.actualPhysicalSlot=page.at("actualPhysicalSlot");
        entry.originalGpuOutput=page.at("path").get<std::string>();
        entry.originalOutputBlake3=page.at("blake3");
        entry.surfaceId=page.at("surfaceId");entry.mip=page.at("mip");
        entry.x=page.at("x");entry.y=page.at("y");
        pages.push_back(std::move(entry));
    }
    const auto capacity=document.at("actualSlotCapacity").get<uint32_t>();
    const auto minimumPose=document.at("poseId").get<uint64_t>();
    return [config,document,pages,capacity,minimumPose](PassRuntimeContext& context,
        const TerrainCaptureFrameInfo& frame,std::shared_ptr<TerrainRvtBudgetOwner> pool)
        ->std::shared_ptr<TerrainLitReplaySession>
    {
        if(frame.poseId!=minimumPose||frame.actualBaseFineResidentPages!=0)return {};
        Eigen::Vector3f expected;
        for(int axis=0;axis<3;++axis)expected[axis]=document.at("cameraPosition").at(axis).get<float>();
        if((context.renderScene.camera.position-expected).cwiseAbs().maxCoeff()>.02f)return {};
        const auto& values=document.at("actualAtlasFormats");
        std::array<vk::Format,3> formats{vk::Format(values.at(0).get<uint32_t>()),
            vk::Format(values.at(1).get<uint32_t>()),vk::Format(values.at(2).get<uint32_t>())};
        auto atlas=std::make_shared<TerrainReferenceAtlas>(*frame.backend,pool,frame.actualWorldBudget,
            formats,capacity,pages);
        return std::make_shared<TerrainLitReplaySession>(frame,std::move(atlas),config);
    };
}
}
