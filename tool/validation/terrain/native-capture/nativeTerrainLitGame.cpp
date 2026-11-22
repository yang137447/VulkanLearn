#include "nativeTerrainCaptureAdapter.h"
#include "terrainLitCaptureAdapter.h"
#include "engine/application.h"
#include "engine/nullEditorHost.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc,char** argv)
{
    // 只要求 lit case。TES case 可选：未设置时进入 lit-only 模式，
    // 不加载/不评估任何 TES witness（TES gate 仍由设置该变量的运行覆盖）。
    const char* tesCase=std::getenv("VULKANLEARN_TERRAIN_CAPTURE_CASE");
    const char* litCase=std::getenv("VULKANLEARN_TERRAIN_LIT_CASE");
    if(!litCase||!*litCase)
        throw std::runtime_error("Explicit test-only lit case required");
    std::shared_ptr<VL::ITerrainCaptureHooks> tes;
    if(tesCase&&*tesCase)
        tes=VL::LoadNativeTerrainCaptureAdapter(tesCase);
    else
        std::cerr<<"[TerrainLit] lit-only mode: VULKANLEARN_TERRAIN_CAPTURE_CASE is unset; "
                   "no TES witness will be evaluated\n";
    auto hooks=VL::AttachNativeTerrainLitReplay(std::move(tes),VL::LoadNativeTerrainLitCase(litCase));
    VL::RegisterTerrainCaptureHooks(std::move(hooks));
    return VL::RunApplication(argc,argv,[]{return std::make_unique<VL::NullEditorHost>();});
}
