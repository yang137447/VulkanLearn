#include "terrainLitCaptureAdapter.h"
#include "render/pass/passRuntime.h"
#include "shader/build/contentHash.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace VL
{
namespace
{
using Json=nlohmann::json;
// TVQ1 owner dictionary geometry (noether_quality_abi.md): 512 rows of 16 uint32.
constexpr uint64_t kOwnerOffset=393280ull*4;
constexpr uint32_t kOwnerStride=64;
constexpr uint32_t kOwnerRows=512;
constexpr uint32_t kSampleRowWords=8;

struct KeyedPage
{
    std::string surfaceId;
    uint32_t mip=0,x=0,y=0;
    std::string path;
    std::string blake3;
};

std::string PageKey(const std::string& surfaceId,uint32_t mip,uint32_t x,uint32_t y)
{
    return surfaceId+"|"+std::to_string(mip)+"|"+std::to_string(x)+"|"+std::to_string(y);
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),{}};
}

// 从同一次运行的 TVQ1 导出里解析 slot -> (surface,mip,x,y)，避免跨运行假设 slot 稳定。
bool LoadsOwnerMap(const std::filesystem::path& captureJson,const std::filesystem::path& captureBin,
    const Eigen::Vector3f& expectedCamera,std::map<std::string,uint32_t>& keyToSlot,
    uint64_t& mappingFrame,uint64_t& mappingEpoch,double& cameraDrift)
{
    Json metadata;
    try{metadata=Json::parse(ReadText(captureJson));}catch(...){return false;}
    const auto camera=metadata.at("cameraPosition");
    cameraDrift=std::max({std::abs(camera.at(0).get<double>()-expectedCamera.x()),
        std::abs(camera.at(1).get<double>()-expectedCamera.y()),
        std::abs(camera.at(2).get<double>()-expectedCamera.z())});
    const auto& names=metadata.at("surfaceIds");
    std::ifstream binary(captureBin,std::ios::binary);
    if(!binary)return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(binary)),{});
    if(bytes.size()<kOwnerOffset+uint64_t(kOwnerRows)*kOwnerStride)return false;
    for(uint32_t row=0;row<kOwnerRows;++row)
    {
        const char* base=bytes.data()+kOwnerOffset+uint64_t(row)*kOwnerStride;
        uint32_t words[8];
        std::memcpy(words,base,sizeof(words));
        if(words[0]==0)continue;
        if(words[1]>=names.size())continue;
        const std::string& surfaceId=names.at(words[1]).get_ref<const std::string&>();
        keyToSlot[PageKey(surfaceId,words[2],words[3]&65535u,words[3]>>16)]=row;
    }
    mappingFrame=metadata.at("frame").get<uint64_t>();
    mappingEpoch=metadata.at("submissionEpoch").get<uint64_t>();
    return !keyToSlot.empty();
}
}

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
    std::vector<TerrainReferencePage> legacyPages;
    if(document.contains("originalGpuPages"))
        for(const auto& page:document.at("originalGpuPages"))
        {
            TerrainReferencePage entry;
            entry.actualPhysicalSlot=page.at("actualPhysicalSlot");
            entry.originalGpuOutput=page.at("path").get<std::string>();
            entry.originalOutputBlake3=page.at("blake3");
            entry.surfaceId=page.at("surfaceId");entry.mip=page.at("mip");
            entry.x=page.at("x");entry.y=page.at("y");
            legacyPages.push_back(std::move(entry));
        }
    std::vector<KeyedPage> keyedPages;
    if(document.contains("referencePages"))
        for(const auto& page:document.at("referencePages"))
            keyedPages.push_back({page.at("surfaceId").get<std::string>(),page.at("mip"),
                page.at("x"),page.at("y"),page.at("path").get<std::string>(),
                page.at("blake3").get<std::string>()});
    const auto capacity=document.at("actualSlotCapacity").get<uint32_t>();
    const auto minimumPose=document.at("poseId").get<uint64_t>();
    const auto minimumFrame=document.value("minimumFrame",uint64_t(0));
    const bool allowFineResident=document.value("allowFineResidentPages",false);
    const std::string captureDirectory=std::getenv("VULKANLEARN_RVT_QUALITY_DIR")
        ?std::getenv("VULKANLEARN_RVT_QUALITY_DIR"):std::string{};
    return [config,document,legacyPages,keyedPages,capacity,minimumPose,minimumFrame,
        allowFineResident,captureDirectory](PassRuntimeContext& context,
        const TerrainCaptureFrameInfo& frame,std::shared_ptr<TerrainRvtBudgetOwner> pool)
        ->std::shared_ptr<TerrainLitReplaySession>
    {
        if(frame.poseId!=minimumPose||frame.frame<minimumFrame||
            (!allowFineResident&&frame.actualBaseFineResidentPages!=0))
        {
            static uint32_t skipReported=0;
            if(skipReported<6){++skipReported;std::cerr<<"[TerrainLit] factory skip: poseId="<<frame.poseId
                <<" expected="<<minimumPose<<" frame="<<frame.frame<<" minFrame="<<minimumFrame
                <<" fineBase="<<frame.actualBaseFineResidentPages<<" allowFine="<<allowFineResident<<std::endl;}
            return {};
        }
        Eigen::Vector3f expected;
        for(int axis=0;axis<3;++axis)expected[axis]=document.at("cameraPosition").at(axis).get<float>();
        if((context.renderScene.camera.position-expected).cwiseAbs().maxCoeff()>.02f)
            return {};
        TerrainLitReplayConfig resolved=config;
        std::vector<TerrainReferencePage> pages=keyedPages.empty()?legacyPages:std::vector<TerrainReferencePage>{};
        if(!keyedPages.empty())
        {
            // 页面身份绑定：从同一次运行的已导出 TVQ1 中解析 slot 映射（帧 <= 回放帧、同一相机），
            // 再把 keyed 页面集按实际 slot 装入参考图集，避免跨运行 slot 顺序假设。
            std::map<std::string,uint32_t> keyToSlot;
            std::filesystem::path chosenJson,chosenBin;
            uint64_t mappingFrame=0,mappingEpoch=0;double chosenDrift=1e9;
            std::error_code error;
            if(!captureDirectory.empty()&&std::filesystem::is_directory(captureDirectory,error))
                for(const auto& entry:std::filesystem::directory_iterator(captureDirectory,error))
                {
                    const auto name=entry.path().filename().string();
                    if(name.rfind("capture-",0)!=0||entry.path().extension()!=".json")continue;
                    Json metadata;
                    try{metadata=Json::parse(ReadText(entry.path()));}catch(...){continue;}
                    if(metadata.value("submissionEpoch",uint64_t(0))==0)continue;
                    const auto captureFrame=metadata.value("frame",uint64_t(0));
                    if(captureFrame>frame.frame)continue;
                    std::map<std::string,uint32_t> candidate;uint64_t mf=0,me=0;double drift=1e9;
                    auto binPath=entry.path();binPath.replace_extension(".bin");
                    if(!LoadsOwnerMap(entry.path(),binPath,expected,candidate,mf,me,drift))continue;
                    if(drift>.02)continue;
                    if(mf>=mappingFrame){mappingFrame=mf;mappingEpoch=me;chosenDrift=drift;
                        keyToSlot=std::move(candidate);chosenJson=entry.path();
                        chosenBin=entry.path();chosenBin.replace_extension(".bin");}
                }
            if(keyToSlot.empty())
            {
                static uint32_t mappingReported=0;
                if(mappingReported<4){++mappingReported;std::cerr
                    <<"[TerrainLit] no qualifying same-run TVQ1 mapping (VULKANLEARN_RVT_QUALITY_DIR="
                    <<captureDirectory<<") for frame "<<frame.frame<<std::endl;}
                return {};
            }
            uint32_t maxSlot=0;
            std::cout<<"[TerrainLit] resolved keyed pages from "<<chosenJson.string()
                <<" frame="<<mappingFrame<<" epoch="<<mappingEpoch<<" drift="<<chosenDrift
                <<" keys="<<keyToSlot.size()<<std::endl;
            std::vector<uint32_t> slots;
            for(const auto& page:keyedPages)
            {
                const auto found=keyToSlot.find(PageKey(page.surfaceId,page.mip,page.x,page.y));
                if(found==keyToSlot.end())continue;
                TerrainReferencePage entry;
                entry.actualPhysicalSlot=found->second;
                entry.originalGpuOutput=page.path;
                entry.originalOutputBlake3=page.blake3;
                entry.surfaceId=page.surfaceId;entry.mip=page.mip;entry.x=page.x;entry.y=page.y;
                maxSlot=std::max(maxSlot,entry.actualPhysicalSlot);
                slots.push_back(entry.actualPhysicalSlot);
                pages.push_back(std::move(entry));
            }
            if(pages.empty())
            {
                std::cerr<<"[TerrainLit] no keyed reference page matches the resolved slot map\n";
                return {};
            }
            std::sort(slots.begin(),slots.end());slots.erase(std::unique(slots.begin(),slots.end()),slots.end());
            std::cout<<"[TerrainLit] resolved "<<pages.size()<<" pages, max slot "<<maxSlot
                <<" capacity "<<capacity<<std::endl;
            resolved.allActualRoiSampledSlots=std::move(slots);
            Json mapping=Json::object();
            for(const auto& page:pages)
                mapping[PageKey(page.surfaceId,page.mip,page.x,page.y)]=page.actualPhysicalSlot;
            resolved.resolvedPageSlotJson=mapping.dump();
            resolved.pageMappingSource=chosenJson.string()+" frame="+std::to_string(mappingFrame)
                +" epoch="+std::to_string(mappingEpoch);
        }
        const auto& values=document.at("actualAtlasFormats");
        std::array<vk::Format,3> formats{vk::Format(values.at(0).get<uint32_t>()),
            vk::Format(values.at(1).get<uint32_t>()),vk::Format(values.at(2).get<uint32_t>())};
        auto atlas=std::make_shared<TerrainReferenceAtlas>(*frame.backend,pool,frame.actualWorldBudget,
            formats,capacity,pages);
        return std::make_shared<TerrainLitReplaySession>(frame,std::move(atlas),resolved);
    };
}
}
