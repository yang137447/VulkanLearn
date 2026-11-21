#pragma once

#include <string>
#include "terrain/terrainFaultInjection.h"

namespace VL
{
TerrainFaultRequest ParseTerrainRvtFaultCommand(const std::string& payload);

// Console 只解析 DTO；GT 命令点调用此服务排队，控制诊断不代表原生故障已触发。
class TerrainRvtFaultTestingService
{
public:
    static TerrainFaultQueueResult Submit(const TerrainFaultRequest& request);
    static std::string ControlDiagnostic(
        const TerrainFaultRequest& request, const TerrainFaultQueueResult& result);
};
}
