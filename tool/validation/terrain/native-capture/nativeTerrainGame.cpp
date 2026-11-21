#include "nativeTerrainCaptureAdapter.h"
#include "engine/application.h"
#include "engine/nullEditorHost.h"
#include <cstdlib>
#include <memory>
#include <stdexcept>

int main(int argc, char** argv)
{
    const char* testCase = std::getenv("VULKANLEARN_TERRAIN_CAPTURE_CASE");
    if (!testCase || !*testCase)
        throw std::runtime_error("Test-only NativeGame requires an explicit terrain capture case");
    VL::RegisterTerrainCaptureHooks(VL::LoadNativeTerrainCaptureAdapter(testCase));
    return VL::RunApplication(argc, argv, [] { return std::make_unique<VL::NullEditorHost>(); });
}
