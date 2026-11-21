#include "nativeTerrainCaptureAdapter.h"
#include "terrainLitCaptureAdapter.h"
#include "engine/application.h"
#include "engine/nullEditorHost.h"
#include <cstdlib>
#include <memory>
#include <stdexcept>

int main(int argc,char** argv)
{
    const char* tesCase=std::getenv("VULKANLEARN_TERRAIN_CAPTURE_CASE");
    const char* litCase=std::getenv("VULKANLEARN_TERRAIN_LIT_CASE");
    if(!tesCase||!*tesCase||!litCase||!*litCase)
        throw std::runtime_error("Explicit test-only TES and lit cases required");
    auto hooks=VL::AttachNativeTerrainLitReplay(VL::LoadNativeTerrainCaptureAdapter(tesCase),
        VL::LoadNativeTerrainLitCase(litCase));
    VL::RegisterTerrainCaptureHooks(std::move(hooks));
    return VL::RunApplication(argc,argv,[]{return std::make_unique<VL::NullEditorHost>();});
}
