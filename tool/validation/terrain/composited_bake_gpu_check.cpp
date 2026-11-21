#include <vulkan/vulkan.hpp>
#include <array>
#include <vector>
#include <fstream>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <cmath>
#include <filesystem>
#include "../../../source/terrain/terrainHeightAtlasLayout.h"

struct Buffer
{
    vk::Buffer buffer;
    vk::DeviceMemory memory;
};
struct Image
{
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
};
struct Params
{
    std::array<uint32_t,4> page{256,128,0,0};
    std::array<uint32_t,4> coverage{2,1,128,0};
    std::array<uint32_t,4> counts{2,2,4,0};
    std::array<float,4> surface{0,0,1,1};
    std::array<float,4> coverageUv{0,0,1,1};
    std::array<float,4> frameUv{0,0,1,1};
    std::array<float,4> material{1,0,0,0};
    std::array<uint32_t,256> mapping;
    std::array<uint32_t,256> local;
};

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    try
    {
        auto instance = vk::createInstance(vk::InstanceCreateInfo{});
        auto physical = instance.enumeratePhysicalDevices().at(0);
        auto families = physical.getQueueFamilyProperties();
        uint32_t family = 0;
        while (!(families.at(family).queueFlags & vk::QueueFlagBits::eCompute))
            ++family;
        float priority = 1;
        vk::DeviceQueueCreateInfo queueInfo({},family,1,&priority);
        auto device = physical.createDevice(vk::DeviceCreateInfo({},queueInfo));
        auto queue = device.getQueue(family,0);
        auto memoryProps = physical.getMemoryProperties();
        auto memoryType = [&](uint32_t bits,vk::MemoryPropertyFlags flags) {
            for(uint32_t index=0;index<memoryProps.memoryTypeCount;++index)
                if((bits&(1u<<index)) && (memoryProps.memoryTypes[index].propertyFlags&flags)==flags)
                    return index;
            throw std::runtime_error("memory type");
        };
        std::vector<Buffer> buffers;
        auto makeBuffer = [&](size_t bytes,vk::BufferUsageFlags usage,const void* data) {
            Buffer result;
            result.buffer=device.createBuffer(vk::BufferCreateInfo({},bytes,usage));
            auto requirements=device.getBufferMemoryRequirements(result.buffer);
            result.memory=device.allocateMemory(vk::MemoryAllocateInfo(requirements.size,
                memoryType(requirements.memoryTypeBits,vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent)));
            device.bindBufferMemory(result.buffer,result.memory,0);
            if(data){auto mapped=device.mapMemory(result.memory,0,bytes);std::memcpy(mapped,data,bytes);device.unmapMemory(result.memory);}
            buffers.push_back(result);
            return result;
        };
        auto pool=device.createCommandPool(vk::CommandPoolCreateInfo({},family));
        auto command=device.allocateCommandBuffers(vk::CommandBufferAllocateInfo(pool,vk::CommandBufferLevel::ePrimary,1)).at(0);
        command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
        std::vector<Image> images;
        auto makeImage = [&](uint32_t width,uint32_t height,uint32_t layers,
            vk::Format format,const void* data,uint32_t bytesPerPixel,bool array,uint32_t mips=1,bool generateMips=false) {
            Image result;
            vk::ImageCreateInfo info({},vk::ImageType::e2D,format,
                vk::Extent3D(width,height,1),mips,layers,vk::SampleCountFlagBits::e1,
                vk::ImageTiling::eOptimal,vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eSampled);
            result.image=device.createImage(info);
            auto requirements=device.getImageMemoryRequirements(result.image);
            result.memory=device.allocateMemory(vk::MemoryAllocateInfo(requirements.size,
                memoryType(requirements.memoryTypeBits,vk::MemoryPropertyFlagBits::eDeviceLocal)));
            device.bindImageMemory(result.image,result.memory,0);
            size_t imageBytes=0;
            const uint32_t uploadedMips=generateMips?1:mips;
            for(uint32_t mip=0;mip<uploadedMips;++mip)imageBytes+=size_t(std::max(1u,width>>mip))*std::max(1u,height>>mip)*layers*bytesPerPixel;
            auto staging=makeBuffer(imageBytes,vk::BufferUsageFlagBits::eTransferSrc,data);
            vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor,0,mips,0,layers);
            vk::ImageMemoryBarrier barrier({},vk::AccessFlagBits::eTransferWrite,
                vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal,
                VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,result.image,range);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eTransfer,{},nullptr,nullptr,barrier);
            size_t offset=0;
            for(uint32_t mip=0;mip<uploadedMips;++mip){
                uint32_t mipWidth=std::max(1u,width>>mip),mipHeight=std::max(1u,height>>mip);
                vk::BufferImageCopy copy(offset,0,0,vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,mip,0,layers),
                    vk::Offset3D(0,0,0),vk::Extent3D(mipWidth,mipHeight,1));
                command.copyBufferToImage(staging.buffer,result.image,vk::ImageLayout::eTransferDstOptimal,copy);
                offset+=size_t(mipWidth)*mipHeight*layers*bytesPerPixel;
            }
            if(generateMips){
                for(uint32_t mip=1;mip<mips;++mip){
                    vk::ImageSubresourceRange previous(vk::ImageAspectFlagBits::eColor,mip-1,1,0,layers);
                    vk::ImageMemoryBarrier sourceBarrier(vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eTransferRead,
                        vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eTransferSrcOptimal,
                        VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,result.image,previous);
                    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eTransfer,{},nullptr,nullptr,sourceBarrier);
                    vk::ImageBlit blit;
                    blit.setSrcSubresource(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,mip-1,0,layers))
                        .setDstSubresource(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,mip,0,layers));
                    blit.srcOffsets[0]=vk::Offset3D(0,0,0);
                    blit.srcOffsets[1]=vk::Offset3D(std::max(1u,width>>(mip-1)),std::max(1u,height>>(mip-1)),1);
                    blit.dstOffsets[0]=vk::Offset3D(0,0,0);
                    blit.dstOffsets[1]=vk::Offset3D(std::max(1u,width>>mip),std::max(1u,height>>mip),1);
                    command.blitImage(result.image,vk::ImageLayout::eTransferSrcOptimal,
                        result.image,vk::ImageLayout::eTransferDstOptimal,blit,vk::Filter::eLinear);
                    sourceBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
                        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite)
                        .setOldLayout(vk::ImageLayout::eTransferSrcOptimal)
                        .setNewLayout(vk::ImageLayout::eTransferDstOptimal);
                    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eTransfer,{},nullptr,nullptr,sourceBarrier);
                }
            }
            barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eShaderRead)
                .setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eComputeShader,{},nullptr,nullptr,barrier);
            result.view=device.createImageView(vk::ImageViewCreateInfo({},result.image,
                array?vk::ImageViewType::e2DArray:vk::ImageViewType::e2D,format,{},range));
            images.push_back(result);
            return result;
        };
        const std::array<uint8_t,8> table{1,0,0,255,0,0,0,255};
        auto pageTable=makeImage(2,1,1,vk::Format::eR8G8B8A8Unorm,table.data(),4,false);
        std::vector<uint8_t> ids(256*128*4,0),weights(256*128*4,0);
        for(uint32_t row=0;row<128;++row)
            for(uint32_t column=0;column<256;++column)
            {
                ids[(row*256+column)*4]=column<128?207:13;
                weights[(row*256+column)*4]=255;
            }
        auto idImage=makeImage(256,128,1,vk::Format::eR8G8B8A8Unorm,ids.data(),4,false);
        auto weightImage=makeImage(256,128,1,vk::Format::eR8G8B8A8Unorm,weights.data(),4,false);
        std::vector<float> catalog(256*4*4,0);
        for(uint32_t layer=0;layer<256;++layer)catalog[(256+layer)*4+2]=1;
        auto catalogImage=makeImage(256,4,1,vk::Format::eR32G32B32A32Sfloat,catalog.data(),16,false);
        std::array<uint8_t,8> colours{128,0,0,255,0,0,192,255};
        auto albedo=makeImage(1,1,2,vk::Format::eR8G8B8A8Srgb,colours.data(),4,true);
        std::array<float,8> normals{.5f,.5f,1,1,.5f,.5f,1,1};
        auto normal=makeImage(1,1,2,vk::Format::eR32G32B32A32Sfloat,normals.data(),16,true);
        std::array<float,8> packedOrm{.2f,.4f,.6f,1,.2f,.4f,.6f,1};
        auto orm=makeImage(1,1,2,vk::Format::eR32G32B32A32Sfloat,packedOrm.data(),16,true);
        std::vector<float> localWeights(4*4*2*4,.5f);
        auto localWeightImage=makeImage(4,4,2,vk::Format::eR32G32B32A32Sfloat,localWeights.data(),16,true);
        std::array<float,4> geometric{0,1,0,1},tangentValue{1,0,0,1};
        auto frameNormal=makeImage(1,1,1,vk::Format::eR32G32B32A32Sfloat,geometric.data(),16,false);
        auto frameTangent=makeImage(1,1,1,vk::Format::eR32G32B32A32Sfloat,tangentValue.data(),16,false);
        std::vector<float> heights;
        for(uint32_t mip=0;mip<4;++mip)
            for(uint32_t layer=0;layer<2;++layer)
                for(uint32_t pixel=0;pixel<std::max(1u,8u>>mip)*std::max(1u,8u>>mip);++pixel)
                    for(uint32_t component=0;component<4;++component)
                        heights.push_back(mip==3?float(layer):float(1-layer));
        auto heightSource=makeImage(8,8,2,vk::Format::eR32G32B32A32Sfloat,heights.data(),16,true,4);
        Params params;
        params.mapping.fill(0xffffffffu);params.mapping[13]=0;params.mapping[207]=1;
        params.local.fill(0);params.local[0]=13;params.local[1]=207;
        std::string mode=argc>2?argv[2]:"coverage";
        const bool bounded=mode.rfind("bounded",0)==0;
        const bool reconstruction=mode.rfind("bounded-reconstruction",0)==0;
        const bool reconstructionCompare=mode=="bounded-reconstruction-check";
        const bool heightDiagnostic=mode.rfind("bounded-height",0)==0;
        const bool comparePlanes=mode=="bounded-compare"||heightDiagnostic||reconstructionCompare;
        if(bounded){
            params.page={256,128,0,0};params.counts[2]=4;params.coverage[3]=1;
            params.material[0]=48;
            if(mode=="bounded-right")params.page[2]=1;
            if(mode=="bounded-coarse")params.page={128,64,0,0};
            if(mode=="bounded-repeat")params.counts[3]=1;
            catalog[(256+13)*4+2]=1.3f;catalog[(256+13)*4+3]=.37f;
            catalog[(256+207)*4+2]=.7f;catalog[(256+207)*4+3]=-.81f;
            catalogImage=makeImage(256,4,1,vk::Format::eR32G32B32A32Sfloat,catalog.data(),16,false);
            std::vector<uint8_t> sourceColours;
            std::vector<float> sourceNormals,sourceOrm;
            for(uint32_t mip=0;mip<6;++mip){
                const uint32_t extent=32u>>mip;
                for(uint32_t layer=0;layer<2;++layer)
                    for(uint32_t row=0;row<extent;++row)
                        for(uint32_t column=0;column<extent;++column){
                            const float phase=6.2831853f*(float(column)+.5f)/extent;
                            const float wave=std::sin(phase+layer)*std::cos(6.2831853f*(row+.5f)/extent);
                            for(uint32_t component=0;component<3;++component)
                                sourceColours.push_back(uint8_t(50+35*component+40*layer+25*wave+7*mip));
                            sourceColours.push_back(255);
                            sourceNormals.insert(sourceNormals.end(),{
                                .5f+.22f*wave,.5f+.17f*std::cos(phase+layer),.88f,1});
                            sourceOrm.insert(sourceOrm.end(),{
                                .55f+.1f*wave,.18f+.28f*layer+.06f*mip+.08f*wave,.13f+.2f*layer,1});
                        }
            }
            albedo=makeImage(32,32,2,vk::Format::eR8G8B8A8Srgb,sourceColours.data(),4,true,6);
            normal=makeImage(32,32,2,vk::Format::eR32G32B32A32Sfloat,sourceNormals.data(),16,true,6);
            orm=makeImage(32,32,2,vk::Format::eR32G32B32A32Sfloat,sourceOrm.data(),16,true,6);
            std::vector<float> frameNormals,frameTangents;
            for(uint32_t row=0;row<8;++row)
                for(uint32_t column=0;column<8;++column){
                    const float slope=.12f+.04f*column;
                    frameNormals.insert(frameNormals.end(),{-slope,.91f,.18f+.02f*row,1});
                    frameTangents.insert(frameTangents.end(),{.96f,slope,.07f,-1});
                }
            frameNormal=makeImage(8,8,1,vk::Format::eR32G32B32A32Sfloat,frameNormals.data(),16,false);
            frameTangent=makeImage(8,8,1,vk::Format::eR32G32B32A32Sfloat,frameTangents.data(),16,false);
            for(uint32_t layer=0;layer<2;++layer)
                for(uint32_t row=0;row<4;++row)
                    for(uint32_t column=0;column<4;++column)
                        for(uint32_t component=0;component<4;++component)
                            localWeights[((layer*4+row)*4+column)*4+component]=
                                layer==0?.15f+.2f*column:.85f-.2f*column;
            localWeightImage=makeImage(4,4,2,vk::Format::eR32G32B32A32Sfloat,localWeights.data(),16,true);
        }
        if(reconstruction){
            if(argc<5)throw std::runtime_error("reconstruction requires input directory");
            const auto directory=std::filesystem::path(argv[4]);
            std::ifstream file(directory/"source-albedo.bin",std::ios::binary|std::ios::ate);
            if(!file||file.tellg()!=1024*1024*4*2)throw std::runtime_error("bad reconstruction source");
            std::vector<uint8_t> pixels(size_t(file.tellg()));file.seekg(0);
            file.read(reinterpret_cast<char*>(pixels.data()),pixels.size());
            albedo=makeImage(1024,1024,2,vk::Format::eR8G8B8A8Srgb,pixels.data(),4,true,11,true);
            params.page={4096,4096,9,11};
            if(mode=="bounded-reconstruction-mip1")params.page={2048,2048,4,5};
            if(mode=="bounded-reconstruction-mip2")params.page={1024,1024,2,2};
            if(mode=="bounded-reconstruction-mip3")params.page={512,512,1,1};
            params.material[0]=4;
            params.counts[2]=mode=="bounded-reconstruction-center"?1:4;
            for(uint32_t layer=0;layer<256;++layer){
                catalog[(256+layer)*4+2]=1;
                catalog[(256+layer)*4+3]=0;
            }
            catalogImage=makeImage(256,4,1,vk::Format::eR32G32B32A32Sfloat,catalog.data(),16,false);
            for(uint32_t layer=0;layer<2;++layer)
                for(uint32_t index=0;index<4*4*4;++index)
                    localWeights[layer*4*4*4+index]=layer==0?1.f:0.f;
            localWeightImage=makeImage(4,4,2,vk::Format::eR32G32B32A32Sfloat,localWeights.data(),16,true);
            geometric={0,1,0,1};tangentValue={1,0,0,1};
            frameNormal=makeImage(1,1,1,vk::Format::eR32G32B32A32Sfloat,geometric.data(),16,false);
            frameTangent=makeImage(1,1,1,vk::Format::eR32G32B32A32Sfloat,tangentValue.data(),16,false);
        }
        if(mode=="lod"){
            std::vector<uint8_t> mipColours;
            for(uint32_t mip=0;mip<4;++mip)
                for(uint32_t layer=0;layer<2;++layer)
                    for(uint32_t pixel=0;pixel<std::max(1u,8u>>mip)*std::max(1u,8u>>mip);++pixel){
                        mipColours.push_back(mip==0?255:0);mipColours.push_back(mip==0?0:128);
                        mipColours.push_back(0);mipColours.push_back(255);
                    }
            albedo=makeImage(8,8,2,vk::Format::eR8G8B8A8Srgb,mipColours.data(),4,true,4);
            params.material[0]=1024;params.page={1,1,0,0};
        }
        if(mode=="area"||mode=="distance"){
            params.page={1,1,0,0};
            normals={.5f,.5f,1,1,1,.5f,.5f,1};
            normal=makeImage(1,1,2,vk::Format::eR32G32B32A32Sfloat,normals.data(),16,true);
            packedOrm={.2f,.2f,.6f,1,.2f,.8f,.6f,1};
            orm=makeImage(1,1,2,vk::Format::eR32G32B32A32Sfloat,packedOrm.data(),16,true);
        }
        if(mode=="repeat")params.counts[3]=1;
        if(mode=="tail")params.page={1,1,0,0};
        if(mode=="local"||mode=="height"){params.coverage[3]=1;}
        if(mode=="height"){
            params.material[1]=1;params.material[2]=1;
            catalog[(256*3+13)*4+3]=1;catalog[(256*3+207)*4+3]=1;
            auto updated=makeImage(256,4,1,vk::Format::eR32G32B32A32Sfloat,catalog.data(),16,false);
            catalogImage=updated;
        }
        if(mode=="missing")params.mapping[13]=0xffffffffu;
        if(mode=="bounded-weighted-detail"){
            const std::array<uint32_t,4> globals{13,207,31,79};
            for(uint32_t layer=0;layer<4;++layer){
                catalog[(256+layer)*4]=float(globals[layer]);
                catalog[(256+globals[layer])*4+2]=float(1u<<layer);
                catalog[(256*3+globals[layer])*4+3]=.1f*float(layer+1);
            }
            catalogImage=makeImage(256,4,1,vk::Format::eR32G32B32A32Sfloat,catalog.data(),16,false);
            std::vector<float> detailColours,detailNormals,detailHeights,coverageWeights;
            for(uint32_t mip=0;mip<4;++mip)
                for(uint32_t layer=0;layer<4;++layer)
                    for(uint32_t pixel=0;pixel<std::max(1u,8u>>mip)*std::max(1u,8u>>mip);++pixel){
                        detailColours.insert(detailColours.end(),{.1f*float(layer+1),.1f*float(mip+1),.05f*float(layer+mip+1),1});
                        detailNormals.insert(detailNormals.end(),{.5f+.04f*float(layer+1),.5f+.03f*float(mip+1),1,1});
                        detailHeights.insert(detailHeights.end(),{.1f*float(layer+mip+1),0,0,1});
                    }
            for(uint32_t layer=0;layer<4;++layer)
                for(uint32_t pixel=0;pixel<16;++pixel)
                    coverageWeights.insert(coverageWeights.end(),{.4f-.1f*float(layer),0,0,1});
            albedo=makeImage(8,8,4,vk::Format::eR32G32B32A32Sfloat,detailColours.data(),16,true,4);
            normal=makeImage(8,8,4,vk::Format::eR32G32B32A32Sfloat,detailNormals.data(),16,true,4);
            heightSource=makeImage(8,8,4,vk::Format::eR32G32B32A32Sfloat,detailHeights.data(),16,true,4);
            localWeightImage=makeImage(4,4,4,vk::Format::eR32G32B32A32Sfloat,coverageWeights.data(),16,true);
        }
        if(mode=="bounded-coverage-helper"){
            if(argc<5)throw std::runtime_error("coverage helper needs fixture directory");
            auto load=[&](const char* name,size_t expected){
                std::ifstream file(std::filesystem::path(argv[4])/name,std::ios::binary|std::ios::ate);
                if(!file||size_t(file.tellg())!=expected)throw std::runtime_error(std::string("bad coverage fixture ")+name);
                std::vector<uint8_t> data(expected);file.seekg(0);
                file.read(reinterpret_cast<char*>(data.data()),data.size());
                return data;
            };
            auto tableData=load("table.bin",3*2*4);
            auto idData=load("ids.bin",8*8*4);
            auto weightData=load("weights.bin",8*8*16);
            auto queries=load("queries.bin",136*16);
            pageTable=makeImage(3,2,1,vk::Format::eR8G8B8A8Unorm,tableData.data(),4,false);
            idImage=makeImage(8,8,1,vk::Format::eR8G8B8A8Unorm,idData.data(),4,false);
            weightImage=makeImage(8,8,1,vk::Format::eR32G32B32A32Sfloat,weightData.data(),16,false);
            catalogImage=makeImage(136,1,1,vk::Format::eR32G32B32A32Sfloat,queries.data(),16,false);
        }
        auto paramBuffer=makeBuffer(sizeof(params),vk::BufferUsageFlagBits::eStorageBuffer,&params);
        constexpr uint32_t texels=136*136,channelBytes=texels*4,totalBytes=11*channelBytes+4;
        const uint32_t readbackPlanes=heightDiagnostic?9:reconstructionCompare?9:comparePlanes?11:3;
        std::array<vk::ImageView,6> extraViews{
            localWeightImage.view,localWeightImage.view,localWeightImage.view,
            localWeightImage.view,localWeightImage.view,catalogImage.view};
        Buffer heightReadback{};
        Buffer heightAtlasReadback{};
        uint32_t diagnosticHeightExtent=0;
        uint32_t diagnosticHeightRows=0;
        const uint32_t diagnosticHeightLayers=heightDiagnostic&&argc>5?uint32_t(std::stoul(argv[5])):2;
        if(diagnosticHeightLayers!=2&&diagnosticHeightLayers!=7)throw std::runtime_error("diagnostic height layers must be 2 or 7");
        if(reconstructionCompare){
            std::ifstream file(std::filesystem::path(argv[4])/"reconstruction-pages.bin",std::ios::binary|std::ios::ate);
            if(!file||file.tellg()!=5*channelBytes)throw std::runtime_error("bad reconstruction pages");
            std::vector<uint8_t> pixels(size_t(file.tellg()));file.seekg(0);
            file.read(reinterpret_cast<char*>(pixels.data()),pixels.size());
            extraViews[1]=makeImage(136,136,5,vk::Format::eR8G8B8A8Srgb,pixels.data(),4,true).view;
        }
        else if(comparePlanes){
            if(argc<5)throw std::runtime_error("bounded-compare needs output and atlas directory");
            const auto directory=std::filesystem::path(argv[4]);
            auto load=[&](const char* name){
                std::ifstream file(directory/name,std::ios::binary|std::ios::ate);
                if(!file)throw std::runtime_error(std::string("missing ")+name);
                std::vector<uint8_t> data(size_t(file.tellg()));file.seekg(0);
                file.read(reinterpret_cast<char*>(data.data()),data.size());return data;
            };
            const std::array<float,4> entries{2,1,3,0};
            extraViews[0]=makeImage(2,1,2,vk::Format::eR32Sfloat,entries.data(),4,true).view;
            for(uint32_t channel=0;channel<(heightDiagnostic?0u:3u);++channel){
                auto data=load(channel==0?"albedo.bin":channel==1?"normal.bin":"orm.bin");
                if(data.size()!=3*channelBytes)throw std::runtime_error("bad atlas byte size");
                extraViews[channel+1]=makeImage(136,136,3,channel==0?
                    vk::Format::eR8G8B8A8Srgb:vk::Format::eR8G8B8A8Unorm,data.data(),4,true).view;
            }
            auto heightData=load("height-source.bin");
            const bool generateHeightMips=heightDiagnostic&&mode.find("real")!=std::string::npos;
            if(heightData.size()!=size_t(generateHeightMips?1024*1024:1024*1024+512*512+256*256)*diagnosticHeightLayers*2)
                throw std::runtime_error("bad height source size");
            heightSource=makeImage(1024,1024,diagnosticHeightLayers,vk::Format::eR16Unorm,heightData.data(),2,true,3,generateHeightMips);
            if(heightDiagnostic){
                const std::array<uint32_t,7> diagnosticGlobals{13,207,31,79,101,153,231};
                std::vector<VL::TerrainHeightAtlasLayer> diagnosticLayers;
                for(uint32_t layer=0;layer<diagnosticHeightLayers;++layer)
                    diagnosticLayers.push_back({diagnosticGlobals[layer],0,layer});
                auto heightLayout=VL::BuildTerrainHeightAtlasLayout(
                    {{0,256,256,diagnosticHeightLayers}},diagnosticLayers,8*1024*1024);
                if(mode.find("pad")!=std::string::npos){
                    auto powerOfTwo=[](uint32_t value){
                        uint32_t rounded=1;
                        while(rounded<value)rounded*=2;
                        return rounded;
                    };
                    heightLayout.width=powerOfTwo(heightLayout.width);
                    heightLayout.columns=heightLayout.width/136;
                    heightLayout.height=powerOfTwo(uint32_t((heightLayout.pages.size()+heightLayout.columns-1)/heightLayout.columns)*136);
                }
                const uint32_t atlasExtent=heightLayout.width;
                diagnosticHeightExtent=atlasExtent;
                diagnosticHeightRows=heightLayout.height;
                std::vector<uint16_t> clearedHeight(size_t(atlasExtent)*diagnosticHeightRows,0);
                auto diagnosticAtlas=makeImage(atlasExtent,diagnosticHeightRows,1,vk::Format::eR16Unorm,clearedHeight.data(),2,true);
                vk::ImageMemoryBarrier sourceBarrier(vk::AccessFlagBits::eShaderRead,vk::AccessFlagBits::eTransferRead,
                    vk::ImageLayout::eShaderReadOnlyOptimal,vk::ImageLayout::eTransferSrcOptimal,
                    VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,heightSource.image,
                    vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,2,1,0,diagnosticHeightLayers));
                vk::ImageMemoryBarrier atlasBarrier(vk::AccessFlagBits::eShaderRead,vk::AccessFlagBits::eTransferWrite,
                    vk::ImageLayout::eShaderReadOnlyOptimal,vk::ImageLayout::eTransferDstOptimal,
                    VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,diagnosticAtlas.image,
                    vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1));
                std::array<vk::ImageMemoryBarrier,2> beforeCopy{sourceBarrier,atlasBarrier};
                command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eTransfer,{},nullptr,nullptr,beforeCopy);
                for(uint32_t pageIndex=0;pageIndex<heightLayout.pages.size();++pageIndex)
                    for(const auto& region:VL::BuildTerrainHeightAtlasCopies(heightLayout,pageIndex)){
                        vk::ImageCopy copy(
                            vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,2,heightLayout.pages[pageIndex].sourceSlice,1),
                            vk::Offset3D(region.sourceX,region.sourceY,0),
                            vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,0,0,1),
                            vk::Offset3D(region.destinationX,region.destinationY,0),
                            vk::Extent3D(region.width,region.height,1));
                        command.copyImage(heightSource.image,vk::ImageLayout::eTransferSrcOptimal,
                            diagnosticAtlas.image,vk::ImageLayout::eTransferDstOptimal,copy);
                    }
                heightReadback=makeBuffer(256*256*diagnosticHeightLayers*2,vk::BufferUsageFlagBits::eTransferDst,nullptr);
                vk::BufferImageCopy sourceCopy(0,0,0,
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,2,0,diagnosticHeightLayers),
                    vk::Offset3D(0,0,0),vk::Extent3D(256,256,1));
                command.copyImageToBuffer(heightSource.image,vk::ImageLayout::eTransferSrcOptimal,heightReadback.buffer,sourceCopy);
                atlasBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                    .setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(vk::ImageLayout::eTransferSrcOptimal);
                command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eTransfer,{},nullptr,nullptr,atlasBarrier);
                heightAtlasReadback=makeBuffer(size_t(atlasExtent)*diagnosticHeightRows*2,vk::BufferUsageFlagBits::eTransferDst,nullptr);
                vk::BufferImageCopy atlasCopy(0,0,0,
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,0,0,1),
                    vk::Offset3D(0,0,0),vk::Extent3D(atlasExtent,diagnosticHeightRows,1));
                command.copyImageToBuffer(diagnosticAtlas.image,vk::ImageLayout::eTransferSrcOptimal,heightAtlasReadback.buffer,atlasCopy);
                sourceBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferRead).setDstAccessMask(vk::AccessFlagBits::eShaderRead)
                    .setOldLayout(vk::ImageLayout::eTransferSrcOptimal).setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
                atlasBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferRead).setDstAccessMask(vk::AccessFlagBits::eShaderRead)
                    .setOldLayout(vk::ImageLayout::eTransferSrcOptimal).setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
                std::array<vk::ImageMemoryBarrier,2> afterCopy{sourceBarrier,atlasBarrier};
                command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eComputeShader,{},nullptr,nullptr,afterCopy);
                extraViews[4]=diagnosticAtlas.view;
            }
            else{
                auto atlasData=load("height-atlas.bin");
                if(atlasData.size()!=408*408*2)throw std::runtime_error("bad height atlas size");
                extraViews[4]=makeImage(408,408,1,vk::Format::eR16Unorm,atlasData.data(),2,true).view;
            }
            std::vector<float> heightMap(256*4,0);
            const std::array<uint32_t,7> diagnosticGlobals{13,207,31,79,101,153,231};
            for(uint32_t layer=0;layer<diagnosticHeightLayers;++layer){
                const uint32_t global=diagnosticGlobals[layer];
                heightMap[global*4]=layer*4; heightMap[global*4+1]=2;
                heightMap[global*4+2]=256;heightMap[global*4+3]=256;
            }
            extraViews[5]=makeImage(256,1,1,vk::Format::eR32G32B32A32Sfloat,heightMap.data(),16,false).view;
        }
        auto output=makeBuffer(totalBytes,vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferDst,nullptr);
        std::vector<vk::DescriptorSetLayoutBinding> bindings;
        for(uint32_t binding=0;binding<19;++binding)
            bindings.emplace_back(binding,binding<2?vk::DescriptorType::eStorageBuffer:vk::DescriptorType::eCombinedImageSampler,1,vk::ShaderStageFlagBits::eCompute);
        auto layout=device.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo({},bindings));
        auto pipelineLayout=device.createPipelineLayout(vk::PipelineLayoutCreateInfo({},layout));
        std::ifstream shaderFile(argv[1],std::ios::binary|std::ios::ate);
        std::vector<uint32_t> spirv(size_t(shaderFile.tellg())/4);shaderFile.seekg(0);shaderFile.read(reinterpret_cast<char*>(spirv.data()),spirv.size()*4);
        auto shader=device.createShaderModule(vk::ShaderModuleCreateInfo({},spirv.size()*4,spirv.data()));
        auto pipeline=device.createComputePipeline({},vk::ComputePipelineCreateInfo({},vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eCompute,shader,"main"),pipelineLayout)).value;
        std::array<vk::DescriptorPoolSize,2> sizes{
            vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer,2),
            vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler,17)};
        auto descriptorPool=device.createDescriptorPool(vk::DescriptorPoolCreateInfo({},1,sizes));
        auto descriptors=device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(descriptorPool,layout)).at(0);
        vk::SamplerCreateInfo samplerInfo({},vk::Filter::eLinear,vk::Filter::eLinear,
            vk::SamplerMipmapMode::eLinear,vk::SamplerAddressMode::eRepeat,vk::SamplerAddressMode::eRepeat,vk::SamplerAddressMode::eRepeat);
        samplerInfo.setMaxLod(100);
        auto sampler=device.createSampler(samplerInfo);
        samplerInfo.setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeV(vk::SamplerAddressMode::eClampToEdge);
        auto clampSampler=device.createSampler(samplerInfo);
        std::array<vk::DescriptorBufferInfo,2> bufferInfos{vk::DescriptorBufferInfo(paramBuffer.buffer,0,sizeof(params)),vk::DescriptorBufferInfo(output.buffer,0,totalBytes)};
        std::array<vk::ImageView,17> views{pageTable.view,idImage.view,weightImage.view,localWeightImage.view,catalogImage.view,albedo.view,normal.view,orm.view,frameNormal.view,frameTangent.view,heightSource.view};
        std::copy(extraViews.begin(),extraViews.end(),views.begin()+11);
        std::array<vk::DescriptorImageInfo,17> imageInfos;
        std::vector<vk::WriteDescriptorSet> writes(19);
        for(uint32_t binding=0;binding<19;++binding)
        {
            writes[binding].setDstSet(descriptors).setDstBinding(binding).setDescriptorCount(1);
            if(binding<2)writes[binding].setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(bufferInfos[binding]);
            else{
                const auto selectedSampler=bounded&&(binding==10||binding==11||(binding>=14&&binding<=17))?clampSampler:sampler;
                imageInfos[binding-2]=vk::DescriptorImageInfo(selectedSampler,views[binding-2],vk::ImageLayout::eShaderReadOnlyOptimal);
                writes[binding].setDescriptorType(vk::DescriptorType::eCombinedImageSampler).setImageInfo(imageInfos[binding-2]);
            }
        }
        device.updateDescriptorSets(writes,nullptr);
        command.fillBuffer(output.buffer,0,totalBytes,0);
        vk::MemoryBarrier prepare(vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eComputeShader,{},prepare,nullptr,nullptr);
        command.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline);
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,pipelineLayout,0,descriptors,nullptr);
        if (!physical.getProperties().limits.timestampComputeAndGraphics ||
            !families.at(family).timestampValidBits)
            throw std::runtime_error("GPU timestamps unavailable for bake measurement");
        auto timing = device.createQueryPool(vk::QueryPoolCreateInfo({},vk::QueryType::eTimestamp,2));
        command.resetQueryPool(timing,0,2);
        command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,timing,0);
        command.dispatch(17,17,1);
        command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,timing,1);
        vk::MemoryBarrier host(vk::AccessFlagBits::eShaderWrite|vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eHostRead);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader|vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},host,nullptr,nullptr);
        command.end();queue.submit(vk::SubmitInfo(nullptr,nullptr,command),{});queue.waitIdle();
        std::array<uint64_t,2> ticks{};
        const auto timingResult = device.getQueryPoolResults(timing,0,2,sizeof(ticks),
            ticks.data(),sizeof(uint64_t),vk::QueryResultFlagBits::e64);
        if (timingResult != vk::Result::eSuccess || ticks[1] < ticks[0])
            throw std::runtime_error("Invalid bake GPU timestamp result");
        std::cout << "gpuDispatchMs=" << double(ticks[1]-ticks[0]) *
            physical.getProperties().limits.timestampPeriod / 1000000.0 << "\n";
        device.destroyQueryPool(timing);
        if(heightDiagnostic){
            auto heightBytes=device.mapMemory(heightReadback.memory,0,256*256*diagnosticHeightLayers*2);
            std::ofstream file(std::filesystem::path(argv[4])/"gpu-source-mip2.bin",std::ios::binary);
            file.write(reinterpret_cast<const char*>(heightBytes),256*256*diagnosticHeightLayers*2);
            if(!file)throw std::runtime_error("height mip2 readback failed");
            device.unmapMemory(heightReadback.memory);
            auto atlasBytes=device.mapMemory(heightAtlasReadback.memory,0,size_t(diagnosticHeightExtent)*diagnosticHeightRows*2);
            std::ofstream atlasFile(std::filesystem::path(argv[4])/"gpu-height-atlas.bin",std::ios::binary);
            atlasFile.write(reinterpret_cast<const char*>(atlasBytes),size_t(diagnosticHeightExtent)*diagnosticHeightRows*2);
            if(!atlasFile)throw std::runtime_error("height atlas readback failed");
            device.unmapMemory(heightAtlasReadback.memory);
            std::cout<<"heightSourceLayers="<<diagnosticHeightLayers<<" heightAtlasWidth="
                <<diagnosticHeightExtent<<" heightAtlasHeight="<<diagnosticHeightRows<<"\n";
        }
        auto bytes=static_cast<uint8_t*>(device.mapMemory(output.memory,0,totalBytes));
        uint32_t status;std::memcpy(&status,bytes+readbackPlanes*channelBytes,4);
        if(!bounded&&status!=(mode=="missing"?4u:0u))throw std::runtime_error("shader diagnostic "+std::to_string(status));
        if(bounded){
            if(argc<4)throw std::runtime_error("bounded needs readback output file");
            if(status!=0)throw std::runtime_error("bounded bake diagnostic "+std::to_string(status));
            std::ofstream file(argv[3],std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes),readbackPlanes*channelBytes);
            if(!file)throw std::runtime_error("readback write failed");
        }
        else if(argc>=4){
            std::ofstream file(argv[3],std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes),3*channelBytes+4);
            if(!file)throw std::runtime_error("readback write failed");
        }
        uint32_t errors=0;
        auto decode=[](float value){value/=255;return value<=.04045f?value/12.92f:std::pow((value+.055f)/1.055f,2.4f);};
        auto encode=[](float value){return int(std::round(255*(value<=.0031308f?value*12.92f:1.055f*std::pow(value,1/2.4f)-.055f)));};
        float maxColourError=0, maxNormalError=0, maxOrmError=0;
        uint32_t routeErrors=0;
        std::array<std::array<float,5>,4> combinations{};
        for(uint32_t row=0;row<136;++row)
            for(uint32_t column=0;column<136;++column)
            {
                const uint32_t offset=(row*136+column)*4;
                if(bounded)continue;
                bool first=column<132;
                if(mode=="repeat")first=column>=4&&column<132;
                int red=first?128:0,blue=first?0:192;
                if((mode=="coverage"||mode=="repeat")&&(column==131||column==132)){
                    const float firstWeight=column==131?.875f:.125f;
                    red=encode(decode(128)*firstWeight);
                    blue=encode(decode(192)*(1-firstWeight));
                }
                if(mode=="local"){red=encode(decode(128)*.5f);blue=encode(decode(192)*.5f);}
                if(mode=="height"){red=encode(decode(128)/3);blue=encode(decode(192)*2/3);}
                if(mode=="tail"){red=-1;blue=-1;}
                if(mode=="missing")continue;
                if(mode=="distance"){
                    const bool active=column%2==0;
                    const uint32_t repetition=(column/2)%2;
                    auto& combination=combinations[repetition*2+uint32_t(active)];
                    combination[0]+=1;
                    const bool page=true;
                    const float fade=1.0f;
                    float firstWeight=.2f+float(column)/135*.6f;
                    float secondWeight=1-firstWeight;
                    if(active){
                        float firstScore=-.25f*fade+1-firstWeight;
                        float secondScore=.25f*fade+1-secondWeight;
                        float maximum=std::max(firstScore,secondScore);
                        firstWeight*=std::max(0.0f,std::min(1.0f,firstScore-maximum+1));
                        secondWeight*=std::max(0.0f,std::min(1.0f,secondScore-maximum+1));
                        float sum=firstWeight+secondWeight;firstWeight/=sum;secondWeight/=sum;
                    }
                    const float normalLength=std::sqrt(firstWeight*firstWeight+secondWeight*secondWeight);
                    int expectedColour[]{encode(decode(128)*firstWeight),0,encode(decode(192)*secondWeight)};
                    int expectedNormal[]{int(std::round((secondWeight/normalLength*.5f+.5f)*255)),
                        int(std::round((firstWeight/normalLength*.5f+.5f)*255)),128};
                    int expectedOrm[]{51,int(std::round((firstWeight*.2f+secondWeight*.8f)*255)),153};
                    bool failed=false;
                    for(uint32_t component=0;component<3;++component){
                        float colourError=std::abs(int(bytes[offset+component])-expectedColour[component]);
                        float normalError=std::abs(int(bytes[channelBytes+offset+component])-expectedNormal[component]);
                        float ormError=std::abs(int(bytes[2*channelBytes+offset+component])-expectedOrm[component]);
                        maxColourError=std::max(maxColourError,colourError);
                        maxNormalError=std::max(maxNormalError,normalError);
                        maxOrmError=std::max(maxOrmError,ormError);
                        combination[1]=std::max(combination[1],colourError);
                        combination[2]=std::max(combination[2],normalError);
                        combination[3]=std::max(combination[3],ormError);
                        failed=failed||colourError>1||normalError>1||ormError>1;
                    }
                    if(bytes[offset+3]!=(page?255:0)){++routeErrors;failed=true;}
                    if(failed){++errors;combination[4]+=1;}
                    continue;
                }
                if(mode=="lod"){
                    if(bytes[offset]!=0||bytes[offset+1]!=128||bytes[offset+2]!=0)++errors;
                    continue;
                }
                if(mode=="area"){
                    int expectedRed=column<4?128:column==4?encode(decode(128)*.5f):0;
                    int expectedBlue=column<4?0:column==4?encode(decode(192)*.5f):192;
                    int expectedNormalX=column<4?128:column==4?218:255;
                    int expectedNormalY=column<4?255:column==4?218:128;
                    int expectedRoughness=column<4?51:column==4?149:204;
                    if(std::abs(int(bytes[offset])-expectedRed)>1||std::abs(int(bytes[offset+2])-expectedBlue)>1||
                        std::abs(int(bytes[channelBytes+offset])-expectedNormalX)>1||
                        std::abs(int(bytes[channelBytes+offset+1])-expectedNormalY)>1||
                        std::abs(int(bytes[2*channelBytes+offset+1])-expectedRoughness)>1)++errors;
                    continue;
                }
                if((red>=0&&std::abs(int(bytes[offset])-red)>1)||(blue>=0&&std::abs(int(bytes[offset+2])-blue)>1)||
                    bytes[channelBytes+offset]!=128||bytes[channelBytes+offset+1]!=255||
                    bytes[channelBytes+offset+2]!=128||
                    bytes[2*channelBytes+offset]!=51||bytes[2*channelBytes+offset+1]!=102||
                    bytes[2*channelBytes+offset+2]!=153)++errors;
            }
        std::cout<<physical.getProperties().deviceName<<": texels="<<texels<<" errors="<<errors
            <<" status="<<(bounded?0:status)<<" mode="<<mode
            <<" subTexelPrecisionBits="<<physical.getProperties().limits.subTexelPrecisionBits
            <<" driverVersion="<<physical.getProperties().driverVersion
            <<" dispatch=17x17x1 readbackBytes="<<(bounded?readbackPlanes*channelBytes:3*channelBytes+4)<<"\n";
        if(mode=="distance")std::cout<<"distanceComparison maxColourByteError="<<maxColourError
            <<" maxNormalByteError="<<maxNormalError<<" maxOrmByteError="<<maxOrmError
            <<" routeErrors="<<routeErrors<<" composite=all-distance strength=0,3 fade=1\n";
        if(mode=="distance")
            for(uint32_t index=0;index<4;++index)
                std::cout<<"combination repetition="<<index/2<<" strength="<<(index%2?3:0)
                    <<" texels="<<combinations[index][0]<<" maxColourByteError="<<combinations[index][1]
                    <<" maxNormalByteError="<<combinations[index][2]<<" maxOrmByteError="<<combinations[index][3]
                    <<" errors="<<combinations[index][4]<<"\n";
        device.unmapMemory(output.memory);
        device.waitIdle();
        device.destroyDescriptorPool(descriptorPool);device.destroySampler(clampSampler);device.destroySampler(sampler);device.destroyPipeline(pipeline);
        device.destroyShaderModule(shader);device.destroyPipelineLayout(pipelineLayout);device.destroyDescriptorSetLayout(layout);
        device.destroyCommandPool(pool);
        for(auto image:images){device.destroyImageView(image.view);device.destroyImage(image.image);device.freeMemory(image.memory);}
        for(auto buffer:buffers){device.destroyBuffer(buffer.buffer);device.freeMemory(buffer.memory);}
        device.destroy();instance.destroy();
        return errors?1:0;
    }
    catch(const std::exception& exception){std::cerr<<exception.what()<<"\n";return 1;}
}
