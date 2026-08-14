#pragma once

#include <string>
#include <vector>

#include "baseStructs.h"
#include "ui/uiAction.h"

namespace VL
{

enum class RuntimeCommandType
{
    LoadWorld,
    RunWorldReloadStress,
    RunWorldReloadFailureRollbackTest,
    RunGeneratedMaterialFailureRollbackTest,
    RunGeneratedMeshFailureRollbackTest,
    RunGeneratedTextureFailureRollbackTest,
    RunGeneratedHighLightReloadStress,
    RunResizeStress,
    RunRenderGraphReloadStress,
    RunFrameSmokeTest,
    RunEnvironmentUpdateStress,
    RunShaderReloadTest,
    RunShaderAutoReloadTest,
    RunShaderComputeReloadTest,
    RunShaderDefinitionReloadTest,
    RunWorldGraphTransactionTest,
    RunShaderUiReloadTest,
    RunShaderShutdownInflightTest,
    SetDebugViewMode,
    SetEnvironmentIntensity,
    SetProceduralSkyParameters,
    SetSpeedTreeStrength,
    SetSpeedTreeGustingEnabled,
    ForceSpeedTreeGust,
    SetToneMappingMode,
    SetBloomParameter,
    ReloadShaders,
    ReportShaderCacheStatistics
};

enum class RuntimeShaderReloadScope
{
    Changed,
    All
};

enum class BloomParameter
{
    Strength,
    Threshold,
    Knee,
    Clamp
};

struct RuntimeCommand
{
    RuntimeCommandType type = RuntimeCommandType::SetDebugViewMode;
    BloomParameter bloomParameter = BloomParameter::Strength;
    RuntimeShaderReloadScope shaderReloadScope =
        RuntimeShaderReloadScope::Changed;
    int intValue = 0;
    float floatValue = 0.0f;
    // 环境压力测试通过值语义投递完整天空参数，执行端只在 active World 所有权侧写回。
    SkyParametersGPU skyParametersValue;
    std::string stringValue;
    std::string sourceText;
};

// Single-thread command queue for runtime console/debug requests. The console
// produces commands; EngineLoop drains them at a stable point before rendering.
class CommandBus
{
public:
    void Queue(RuntimeCommand command);
    void Queue(UiAction action);
    std::vector<RuntimeCommand> Drain();
    std::vector<UiAction> DrainUiActions();

private:
    std::vector<RuntimeCommand> pendingCommands;
    std::vector<UiAction> pendingUiActions;
};

} // namespace VL
