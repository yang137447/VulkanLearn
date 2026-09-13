#pragma once

#include <optional>
#include <string>
#include <vector>

namespace VL
{

class EngineLoop;

enum class DeveloperUiLaunchMode
{
    UseConfig,
    Enabled,
    Disabled
};

// Owns command-line launch parsing and translation into startup runtime
// commands. It does not initialize platform, config, renderer, or world state.
struct LaunchOptions
{
    bool showHelp = false;
    bool runShaderReloadTest = false;
    bool runShaderComputeReloadTest = false;
    bool runWorldGraphTransactionTest = false;
    bool forceShaderRebuild = false;
    bool exitAfterTests = false;
    DeveloperUiLaunchMode developerUiMode = DeveloperUiLaunchMode::UseConfig;
    std::optional<int> workerThreadCountOverride;
    std::string initialSceneOverride;
    // 启动期控制台脚本：让 shading model showcase / debug 采集可以复现，
    // 不必依赖手敲交互控制台。命令文本与手输完全同一条解析路径。
    std::vector<std::string> consoleCommands;
    // 脚本命令延迟提交的帧数。首帧时环境贴图、CLUT 与 swapchain 内容都还没稳定，
    // 立即 screenshot 只会得到无效画面，因此默认等一小段时间再提交。
    int consoleCommandDelayFrames = 60;
    std::string errorMessage;
};

LaunchOptions ParseLaunchOptions(int argc, char** argv);
void PrintLaunchUsage();
void QueueLaunchCommands(EngineLoop& engineLoop, const LaunchOptions& options);

} // namespace VL
