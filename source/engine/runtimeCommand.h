#pragma once

#include <string>
#include <vector>

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
    SetDebugViewMode,
    SetEnvironmentIntensity,
    SetSpeedTreeStrength,
    SetSpeedTreeGustingEnabled,
    ForceSpeedTreeGust,
    SetToneMappingMode,
    SetBloomParameter
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
    int intValue = 0;
    float floatValue = 0.0f;
    std::string stringValue;
    std::string sourceText;
};

// Single-thread command queue for runtime console/debug requests. The console
// produces commands; EngineLoop drains them at a stable point before rendering.
class CommandBus
{
public:
    void Queue(RuntimeCommand command);
    std::vector<RuntimeCommand> Drain();

private:
    std::vector<RuntimeCommand> pendingCommands;
};

} // namespace VL
