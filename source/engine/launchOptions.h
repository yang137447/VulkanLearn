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
    bool offlineRender = false;
    bool exitAfterTests = false;
    // 逐 pass GPU 计时：默认关闭以保证普通日志与逐帧路径无额外开销；
    // 打开后每 120 帧向日志打印一张 [GPU] 表，控制台 gpustats 可随时按需查看。
    bool gpuStats = false;
    // GPU 预蒙皮 A/B：默认关闭。打开后蒙皮材质使用读 pre-skin buffer 的变体，
    // 并在每帧 pass 循环前录制一次骨混合 compute；关闭时没有 buffer/pass/变体。
    bool prerenderSkin = false;
    // 机器可读性能时间序列：每行一个 perf JSON，每秒追加一行。
    // 路径为空表示不写；打开该通道同时按需启用 GPU 采集。
    std::optional<std::string> perfJsonlPath;
    // 自动化输入验收：启动后主动抢占 OS 键盘焦点，仅显式 flag 生效。
    bool focusWindow = false;
    DeveloperUiLaunchMode developerUiMode = DeveloperUiLaunchMode::UseConfig;
    std::optional<int> workerThreadCountOverride;
    // 帧率验收工具：0 = 不限速（默认行为完全不变），正整数表示渲染主循环的目标节拍。
    // 只约束渲染帧节拍，time.delta 与动画/脚本 tick 的 dt 仍随真实帧间隔变化；
    // ScriptRuntime 内部的固定 60 Hz 物理子步保持不变。
    int frameLimitHz = 0;
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
