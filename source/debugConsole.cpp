#include "debugConsole.h"
#include "engine/modelPreviewRequest.h"

#include <algorithm>
#include <conio.h>
#include <cctype>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "engine/runtimeCommand.h"
#include "editor/runtime/materialInstanceEditorRuntime.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "particles/particleDump.h"
#include "particles/particleRuntimeCommand.h"
#include "world/world.h"

namespace
{

bool ParseConsoleFloat(const std::string& token, float& outValue)
{
    try
    {
        size_t consumed = 0;
        const float value = std::stof(token, &consumed);
        if (consumed != token.size())
        {
            return false;
        }
        outValue = value;
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool ParseConsoleInt(const std::string& token, int& outValue)
{
    try
    {
        size_t consumed = 0;
        const int value = std::stoi(token, &consumed);
        if (consumed != token.size())
        {
            return false;
        }
        outValue = value;
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

} // namespace

DebugConsole::DebugConsole(VL::CommandBus& commandBus, VL::Editor::MaterialInstanceEditorRuntime* runtime)
    : commandBus(commandBus), editorRuntime(runtime)
{
}

void DebugConsole::Initialize()
{
    std::cout << "Debug console ready. Type 'help' for available commands." << std::endl;
    PrintPrompt();
}

void DebugConsole::Update()
{
    for (auto iterator = pendingEditorResults.begin(); iterator != pendingEditorResults.end();)
    {
        const auto result = editorRuntime->GetCommandResult(*iterator);
        if (result && VL::IsFinalEditorCommandStatus(result->status))
        {
            std::cout << VL::EditorCommandJsonCodec::EncodeResultText(*result) << std::endl;
            iterator = pendingEditorResults.erase(iterator);
        }
        else ++iterator;
    }
    while (_kbhit())
    {
        const int ch = _getch();
        HandleInput(ch);
    }
}

// 启动脚本入口：直接走 ProcessCommand，避开 _kbhit/_getch 交互输入路径。
// 这里刻意不做任何额外解析或校验，脚本命令与手输命令必须完全同构。
void DebugConsole::SubmitCommandLine(const std::string& line)
{
    if (line.empty())
    {
        return;
    }

    std::cout << line << std::endl;
    ProcessCommand(line);
}

void DebugConsole::HandleInput(int ch)
{
    if (ch == 0 || ch == 224)
    {
        if (_kbhit())
        {
            (void)_getch();
        }
        return;
    }

    if (ch == '\r' || ch == '\n')
    {
        std::cout << std::endl;
        if (!currentLine.empty())
        {
            ProcessCommand(currentLine);
            currentLine.clear();
        }
        PrintPrompt();
        return;
    }

    if (ch == '\b')
    {
        if (!currentLine.empty())
        {
            currentLine.pop_back();
            std::cout << "\b \b" << std::flush;
        }
        return;
    }

    if (std::isprint(static_cast<unsigned char>(ch)))
    {
        currentLine.push_back(static_cast<char>(ch));
        std::cout << static_cast<char>(ch) << std::flush;
    }
}

void DebugConsole::ProcessCommand(const std::string& line)
{
    std::istringstream commandStream(line);
    std::string command;
    commandStream >> command;

    if (command == "editor")
    {
        if (!editorRuntime)
        {
            std::cout << "Editor document host unavailable; enable --dev-ui or use vl_asset_edit." << std::endl;
            return;
        }
        std::string request;
        std::getline(commandStream, request);
        try
        {
            const auto submitted = editorRuntime->SubmitJson(request);
            std::cout << VL::EditorCommandJsonCodec::EncodeResultText(submitted.result) << std::endl;
            if (submitted.admission == VL::EditorCommandAdmission::Queued)
                pendingEditorResults.push_back(submitted.commandId);
        }
        catch (const std::exception& error) { std::cout << "Editor request rejected: " << error.what() << std::endl; }
        return;
    }

    if (command == "debugdraw")
    {
        std::string arguments, error;
        std::getline(commandStream, arguments);
        VL::RuntimeCommand request;
        if (!VL::Debug::ParseControl(arguments, request.debugDrawControl, error))
        { std::cout << error << std::endl; return; }
        request.type = VL::RuntimeCommandType::ControlDebugDraw;
        request.sourceText = line;
        commandBus.Queue(std::move(request));
        return;
    }
    if (command == "motiondebug")
    {
        std::string value, extra;
        if (!(commandStream >> value) || (value != "0" && value != "1") || (commandStream >> extra))
        {
            std::cout << "Usage: motiondebug <0|1>" << std::endl;
            return;
        }
        VL::RuntimeCommand request;
        request.type = VL::RuntimeCommandType::ControlMotionDebug;
        request.intValue = value == "1" ? 1 : 0;
        request.sourceText = line;
        commandBus.Queue(std::move(request));
        return;
    }
    if (command == "animation")
    {
        std::string action, extra;
        VL::RuntimeCommand request;
        request.type = VL::RuntimeCommandType::ControlAnimation;
        if (!(commandStream >> action) ||
            (action != "play" && action != "pause" && action != "seek" &&
             action != "clip" && action != "get") ||
            ((action == "seek" || action == "clip") && !(commandStream >> request.floatValue)) ||
            (commandStream >> extra) || !std::isfinite(request.floatValue))
        {
            std::cout << "Usage: animation <play|pause|get|seek seconds|clip index>" << std::endl;
            return;
        }
        request.stringValue = action == "pause" ? "play" : action;
        if (action == "play") request.floatValue = 1;
        request.sourceText = line;
        commandBus.Queue(std::move(request));
        return;
    }
    if (command == "debugview")
    {
        int mode = 0;
        if (!(commandStream >> mode))
        {
            std::cout << "Usage: debugview <mode>" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SetDebugViewMode;
        runtimeCommand.intValue = mode;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "tonemap")
    {
        int mode = 0;
        if (!(commandStream >> mode))
        {
            std::cout << "Usage: tonemap <mode>\n"
                      << "  0: Linear clamp\n"
                      << "  1: Reinhard\n"
                      << "  2: Hable\n"
                      << "  3: ACES" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SetToneMappingMode;
        runtimeCommand.intValue = mode;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "bloom")
    {
        std::string field;
        float value = 0.0f;
        if (!(commandStream >> field >> value))
        {
            std::cout << "Usage: bloom <strength|threshold|knee|clamp> <value>" << std::endl;
            return;
        }

        if (field == "strength")
        {
            VL::RuntimeCommand runtimeCommand;
            runtimeCommand.type = VL::RuntimeCommandType::SetBloomParameter;
            runtimeCommand.bloomParameter = VL::BloomParameter::Strength;
            runtimeCommand.floatValue = value;
            runtimeCommand.sourceText = line;
            commandBus.Queue(std::move(runtimeCommand));
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SetBloomParameter;
        runtimeCommand.floatValue = value;
        runtimeCommand.sourceText = line;
        if (field == "threshold")
        {
            runtimeCommand.bloomParameter = VL::BloomParameter::Threshold;
        }
        else if (field == "knee")
        {
            runtimeCommand.bloomParameter = VL::BloomParameter::Knee;
        }
        else if (field == "clamp")
        {
            runtimeCommand.bloomParameter = VL::BloomParameter::Clamp;
        }
        else
        {
            std::cout << "Unknown bloom field: " << field << std::endl;
            return;
        }

        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "help")
    {
        PrintHelp();
        return;
    }

    if (command == "offline")
    {
        std::string action;
        commandStream >> action;
        VL::RuntimeCommand runtimeCommand;
        if (action == "on" || action == "off")
        {
            runtimeCommand.type = VL::RuntimeCommandType::SetOfflineMode;
            runtimeCommand.intValue = action == "on" ? 1 : 0;
        }
        else if (action == "samples" || action == "bounces" ||
                 action == "seed" || action == "lightSamples")
        {
            if (!(commandStream >> runtimeCommand.intValue))
            {
                std::cout << "Usage: offline " << action << " <integer>" << std::endl;
                return;
            }
            runtimeCommand.type = VL::RuntimeCommandType::SetOfflineSetting;
            runtimeCommand.stringValue = action;
        }
        else if (action == "aov")
        {
            std::string value;
            commandStream >> value;
            if (value != "combined" && value != "direct" && value != "indirect")
            {
                std::cout << "Usage: offline aov <combined|direct|indirect>" << std::endl;
                return;
            }
            runtimeCommand.type = VL::RuntimeCommandType::SetOfflineAov;
            runtimeCommand.intValue = value == "combined" ? 0 : value == "direct" ? 1 : 2;
        }
        else if (action == "exposure")
        {
            if (!(commandStream >> runtimeCommand.floatValue))
            {
                std::cout << "Usage: offline exposure <positive-float>" << std::endl;
                return;
            }
            runtimeCommand.type = VL::RuntimeCommandType::SetOfflineExposure;
        }
        else if (action == "pause" || action == "resume" ||
                 action == "stop" || action == "restart")
        {
            runtimeCommand.type = VL::RuntimeCommandType::ControlOfflineSession;
            runtimeCommand.intValue = action == "pause" ? 0 : action == "resume" ? 1 :
                action == "stop" ? 2 : 3;
        }
        else if (action == "export")
            runtimeCommand.type = VL::RuntimeCommandType::ExportOffline;
        else
        {
            std::cout << "Usage: offline <on|off|samples|bounces|seed|lightSamples|"
                         "aov|exposure|pause|resume|stop|restart|export>" << std::endl;
            return;
        }
        // 控制台和开发 UI 共用命令执行边界，不直接读取活动 World/film。
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "loadworld" || command == "loadscene")
    {
        std::string scenePath;
        if (!(commandStream >> scenePath))
        {
            std::cout << "Usage: loadworld <scene-relative-or-absolute-path>" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::LoadWorld;
        runtimeCommand.stringValue = scenePath;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    // previewmodel <scene> <model>：把模型并入场景后加载。
    // 这是"模型编辑器"要用的加载机制，先用命令暴露出来，因为它可以在不变 UI 的情况下
    // 被独立验证（`--console-command` + 截图），而 UI 建在未验证的加载路径上会很贵。
    if (command == "previewmodel")
    {
        std::string scenePath;
        std::string modelPath;
        if (!(commandStream >> scenePath >> modelPath))
        {
            std::cout << "Usage: previewmodel <scene-path> <model-path>" << std::endl;
            std::cout << "  Loads the scene with the model injected as one extra scene object."
                      << std::endl;
            return;
        }

        // 注入对象必须自带 name 与 type：校验器按这两个字段分派对象类型，
        // 缺任意一个都会在校验阶段报错（而不是被静默忽略）。
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::LoadWorld;
        runtimeCommand.stringValue = scenePath;
        runtimeCommand.extraSceneObjects = VL::BuildModelPreviewObjects(modelPath);
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "environment")
    {
        float value = 0.0f;
        if (!(commandStream >> value))
        {
            std::cout << "Usage: environment <intensity>" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SetEnvironmentIntensity;
        runtimeCommand.floatValue = value;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "windgust")
    {
        std::string mode;
        if (!(commandStream >> mode))
        {
            std::cout << "Usage: windgust <on|off|once>" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        if (mode == "on")
        {
            runtimeCommand.type = VL::RuntimeCommandType::SetSpeedTreeGustingEnabled;
            runtimeCommand.intValue = 1;
        }
        else if (mode == "off")
        {
            runtimeCommand.type = VL::RuntimeCommandType::SetSpeedTreeGustingEnabled;
            runtimeCommand.intValue = 0;
        }
        else if (mode == "once")
        {
            runtimeCommand.type = VL::RuntimeCommandType::ForceSpeedTreeGust;
        }
        else
        {
            std::cout << "Usage: windgust <on|off|once>" << std::endl;
            return;
        }
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "windstrength")
    {
        float value = 0.0f;
        if (!(commandStream >> value))
        {
            std::cout << "Usage: windstrength <0..1>" << std::endl;
            return;
        }
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
        {
            std::cout << "windstrength must be a finite value in range 0..1." << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SetSpeedTreeStrength;
        runtimeCommand.floatValue = value;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "screenshot")
    {
        std::string path;
        commandStream >> path;
        if (path.empty())
        {
            path = "hair_debug.bmp";
        }
        const std::string extension =
            path.size() >= 4 ? path.substr(path.size() - 4) : std::string();
        if (extension != ".bmp" && extension != ".BMP")
        {
            const size_t extensionOffset = path.find_last_of('.');
            if (extensionOffset == std::string::npos)
            {
                path += ".bmp";
            }
            else
            {
                path.replace(extensionOffset, std::string::npos, ".bmp");
            }
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::CaptureScreenshot;
        runtimeCommand.stringValue = std::move(path);
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particledump")
    {
        std::string fileName = "particles.json";
        std::string token;
        long long maxPerEmitter = VL::kParticleDumpDefaultDetails;
        if (commandStream >> token)
        {
            fileName = token;
            if (commandStream >> token)
            {
                try
                {
                    size_t consumed = 0;
                    maxPerEmitter = std::stoll(token, &consumed);
                    if (consumed != token.size())
                    {
                        throw std::invalid_argument("trailing characters");
                    }
                }
                catch (const std::exception&)
                {
                    std::cout
                        << "Usage: particledump [file.json] [maxPerEmitter]"
                        << std::endl;
                    return;
                }
                std::string extra;
                if (commandStream >> extra)
                {
                    std::cout
                        << "Usage: particledump [file.json] [maxPerEmitter]"
                        << std::endl;
                    return;
                }
            }
        }
        maxPerEmitter = std::clamp(
            maxPerEmitter,
            0LL,
            static_cast<long long>(VL::kParticleDumpMaxDetails));

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::DumpParticles;
        runtimeCommand.stringValue = std::move(fileName);
        runtimeCommand.intValue = static_cast<int>(maxPerEmitter);
        runtimeCommand.sourceText = line;
        std::cout
            << "Particle dump queued: " << runtimeCommand.stringValue
            << " (max " << maxPerEmitter << " particles/emitter)"
            << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particlestats")
    {
        std::string extra;
        if (commandStream >> extra)
        {
            std::cout << "Usage: particlestats" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::ReportParticleStats;
        runtimeCommand.sourceText = line;
        std::cout << "Particle stats queued." << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particlespawn")
    {
        std::string name;
        std::string assetPath;
        if (!(commandStream >> name >> assetPath))
        {
            std::cout
                << "Usage: particlespawn <name> <PS_asset_path> [x y z]"
                << std::endl;
            return;
        }

        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        float x = 0.0f;
        if (commandStream >> x)
        {
            if (!(commandStream >> position.y() >> position.z()))
            {
                std::cout
                    << "Usage: particlespawn <name> <PS_asset_path> [x y z]"
                    << std::endl;
                return;
            }
            position.x() = x;
        }
        std::string extra;
        if (commandStream >> extra)
        {
            std::cout
                << "Usage: particlespawn <name> <PS_asset_path> [x y z]"
                << std::endl;
            return;
        }

        std::string validationError;
        if (!VL::ValidateParticleSpawnRequest(
                name,
                assetPath,
                position,
                validationError))
        {
            std::cout << validationError << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::SpawnParticleSystem;
        runtimeCommand.stringValue = name;
        runtimeCommand.secondaryStringValue = assetPath;
        runtimeCommand.particleSpawnPosition = position;
        runtimeCommand.sourceText = line;
        std::cout
            << "Particle spawn queued: " << name << " -> " << assetPath
            << " at (" << position.x() << ", " << position.y() << ", "
            << position.z() << ")" << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particledestroy")
    {
        std::string name;
        std::string extra;
        if (!(commandStream >> name) || (commandStream >> extra))
        {
            std::cout << "Usage: particledestroy <name>" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::DestroyParticleSystem;
        runtimeCommand.stringValue = std::move(name);
        runtimeCommand.sourceText = line;
        std::cout
            << "Particle destroy queued: " << runtimeCommand.stringValue
            << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particlerestart")
    {
        std::string name;
        std::string extra;
        if (!(commandStream >> name) || (commandStream >> extra))
        {
            std::cout << "Usage: particlerestart <name>" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::RestartParticleSystem;
        runtimeCommand.stringValue = std::move(name);
        runtimeCommand.sourceText = line;
        std::cout
            << "Particle restart queued: " << runtimeCommand.stringValue
            << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particleparam")
    {
        const char* usage =
            "Usage: particleparam <name> spawnRateScale <scale> | "
            "colorTint <r> <g> <b> <a> | emitter <index> <0|1> | reset";

        std::string name;
        std::string parameter;
        if (!(commandStream >> name >> parameter))
        {
            std::cout << usage << std::endl;
            return;
        }

        std::vector<std::string> arguments;
        std::string token;
        while (commandStream >> token)
        {
            arguments.push_back(token);
        }

        std::string validationError;
        if (!VL::ValidateParticleParameterName(parameter, validationError))
        {
            std::cout << usage << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::SetParticleSystemParameter;
        runtimeCommand.stringValue = name;
        runtimeCommand.secondaryStringValue = parameter;

        if (parameter == "spawnRateScale")
        {
            float scale = 0.0f;
            if (arguments.size() != 1 ||
                !ParseConsoleFloat(arguments[0], scale))
            {
                std::cout << usage << std::endl;
                return;
            }
            if (!VL::ValidateParticleSpawnRateScale(scale, validationError))
            {
                std::cout << validationError << std::endl;
                return;
            }
            runtimeCommand.floatValue = scale;
        }
        else if (parameter == "colorTint")
        {
            if (arguments.size() != 4)
            {
                std::cout << usage << std::endl;
                return;
            }
            Eigen::Vector4f colorTint = Eigen::Vector4f::Ones();
            if (!ParseConsoleFloat(arguments[0], colorTint.x()) ||
                !ParseConsoleFloat(arguments[1], colorTint.y()) ||
                !ParseConsoleFloat(arguments[2], colorTint.z()) ||
                !ParseConsoleFloat(arguments[3], colorTint.w()))
            {
                std::cout << usage << std::endl;
                return;
            }
            if (!VL::ValidateParticleColorTint(colorTint, validationError))
            {
                std::cout << validationError << std::endl;
                return;
            }
            runtimeCommand.particleColorTint = colorTint;
        }
        else if (parameter == "emitter")
        {
            int index = 0;
            int enabled = 0;
            if (arguments.size() != 2 ||
                !ParseConsoleInt(arguments[0], index) ||
                !ParseConsoleInt(arguments[1], enabled))
            {
                std::cout << usage << std::endl;
                return;
            }
            if (index < 0)
            {
                std::cout
                    << "Particle emitter index must be non-negative."
                    << std::endl;
                return;
            }
            if (!VL::ValidateParticleEmitterIndex(
                    static_cast<uint32_t>(index),
                    validationError) ||
                !VL::ValidateParticleEmitterEnabledValue(
                    enabled,
                    validationError))
            {
                std::cout << validationError << std::endl;
                return;
            }
            runtimeCommand.intValue = index;
            runtimeCommand.intValue2 = enabled;
        }
        else
        {
            if (!arguments.empty())
            {
                std::cout << usage << std::endl;
                return;
            }
        }

        runtimeCommand.sourceText = line;
        std::cout
            << "Particle parameter queued: " << name << " " << parameter
            << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particleemitter")
    {
        const char* usage =
            "Usage: particleemitter <system> spawn <sourceIndex> [name] | "
            "destroy <index|name> | restart <index|name> | "
            "enable <index|name> <0|1>";
        std::string systemName;
        std::string operation;
        if (!(commandStream >> systemName >> operation))
        {
            std::cout << usage << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.stringValue = systemName;
        runtimeCommand.sourceText = line;
        if (operation == "spawn")
        {
            int sourceIndex = 0;
            if (!(commandStream >> sourceIndex) || sourceIndex < 0)
            {
                std::cout << usage << std::endl;
                return;
            }
            std::string name;
            commandStream >> name;
            std::string extra;
            if (commandStream >> extra)
            {
                std::cout << usage << std::endl;
                return;
            }
            runtimeCommand.type =
                VL::RuntimeCommandType::SpawnParticleEmitter;
            runtimeCommand.intValue = sourceIndex;
            runtimeCommand.particleEmitterName = std::move(name);
            std::cout << "Particle emitter spawn queued: "
                << systemName << " source=" << sourceIndex << std::endl;
            commandBus.Queue(std::move(runtimeCommand));
            return;
        }

        if (operation != "destroy" && operation != "restart" &&
            operation != "enable")
        {
            std::cout << usage << std::endl;
            return;
        }

        std::string identity;
        if (!(commandStream >> identity))
        {
            std::cout << usage << std::endl;
            return;
        }
        int index = 0;
        if (ParseConsoleInt(identity, index))
        {
            if (index < 0)
            {
                std::cout
                    << "Particle emitter index must be non-negative."
                    << std::endl;
                return;
            }
            runtimeCommand.intValue = index;
        }
        else
        {
            runtimeCommand.intValue = -1;
            runtimeCommand.particleEmitterName = identity;
        }

        if (operation == "enable")
        {
            int enabled = 0;
            if (!(commandStream >> enabled) ||
                (enabled != 0 && enabled != 1))
            {
                std::cout << usage << std::endl;
                return;
            }
            runtimeCommand.type =
                VL::RuntimeCommandType::SetParticleSystemParameter;
            runtimeCommand.secondaryStringValue = "emitter";
            runtimeCommand.intValue2 = enabled;
        }
        else
        {
            runtimeCommand.type = operation == "destroy"
                ? VL::RuntimeCommandType::DestroyParticleEmitter
                : VL::RuntimeCommandType::RestartParticleEmitter;
        }
        std::string extra;
        if (commandStream >> extra)
        {
            std::cout << usage << std::endl;
            return;
        }
        std::cout << "Particle emitter " << operation
            << " queued: " << systemName << " " << identity << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particlebudget")
    {
        const char* usage =
            "Usage: particlebudget <system|world> <maxTotal> <emitterScale>";
        std::string name;
        int maxTotal = 0;
        float emitterScale = 1.0f;
        std::string maxToken;
        std::string scaleToken;
        if (!(commandStream >> name >> maxToken >> scaleToken) ||
            !ParseConsoleInt(maxToken, maxTotal))
        {
            std::cout << usage << std::endl;
            return;
        }
        if (maxTotal < 0 || maxTotal > 1000000)
        {
            std::cout
                << "Particle total budget must be in [0, 1000000]."
                << std::endl;
            return;
        }
        if (!ParseConsoleFloat(scaleToken, emitterScale) ||
            !std::isfinite(emitterScale) ||
            emitterScale < 0.0f || emitterScale > 16.0f)
        {
            std::cout
                << "Particle emitter budget scale must be finite in [0, 16]."
                << std::endl;
            return;
        }
        std::string extra;
        if (commandStream >> extra)
        {
            std::cout << usage << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::SetParticleSystemBudget;
        runtimeCommand.stringValue = name;
        runtimeCommand.intValue = maxTotal;
        runtimeCommand.floatValue = emitterScale;
        runtimeCommand.sourceText = line;
        std::cout << "Particle budget queued: " << name
            << " maxTotal=" << maxTotal
            << " emitterScale=" << emitterScale << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particleuser")
    {
        const char* usage =
            "Usage: particleuser <system> <key> <value> | "
            "particleuser <system> clear <key>";
        std::string name;
        std::string first;
        if (!(commandStream >> name >> first))
        {
            std::cout << usage << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::SetParticleSystemUserParameter;
        runtimeCommand.stringValue = name;
        runtimeCommand.sourceText = line;
        if (first == "clear")
        {
            std::string key;
            std::string extra;
            if (!(commandStream >> key) || (commandStream >> extra))
            {
                std::cout << usage << std::endl;
                return;
            }
            runtimeCommand.intValue2 = 1;
            runtimeCommand.particleUserParameterName = std::move(key);
        }
        else
        {
            float value = 0.0f;
            std::string extra;
            if (!ParseConsoleFloat(first, value) ||
                !std::isfinite(value) || std::abs(value) > 1.0e6f ||
                (commandStream >> extra))
            {
                std::cout << usage << std::endl;
                return;
            }
            runtimeCommand.particleUserParameterName = first;
            runtimeCommand.floatValue = value;
        }
        if (runtimeCommand.particleUserParameterName.empty() ||
            runtimeCommand.particleUserParameterName.size() > 64)
        {
            std::cout
                << "Particle user parameter key requires 1..64 characters."
                << std::endl;
            return;
        }
        std::cout << "Particle user parameter queued: " << name
            << " " << runtimeCommand.particleUserParameterName << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particledata")
    {
        const char* usage =
            "Usage: particledata <name> <value> | "
            "particledata <name> clear | particledata";
        std::string name;
        if (!(commandStream >> name))
        {
            VL::RuntimeCommand runtimeCommand;
            runtimeCommand.type =
                VL::RuntimeCommandType::ReportParticleDataChannels;
            runtimeCommand.sourceText = line;
            std::cout << "Particle data channels queued for dump."
                << std::endl;
            commandBus.Queue(std::move(runtimeCommand));
            return;
        }
        std::string second;
        std::string extra;
        if (!(commandStream >> second) || (commandStream >> extra))
        {
            std::cout << usage << std::endl;
            return;
        }
        std::string validationError;
        if (!VL::ValidateParticleDataChannelName(
                name, validationError))
        {
            std::cout << validationError << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::SetParticleDataChannel;
        runtimeCommand.particleDataChannelName = name;
        runtimeCommand.sourceText = line;
        if (second == "clear")
        {
            runtimeCommand.particleDataChannelClear = true;
            std::cout << "Particle data channel clear queued: "
                << name << std::endl;
        }
        else
        {
            float value = 0.0f;
            if (!ParseConsoleFloat(second, value))
            {
                std::cout << usage << std::endl;
                return;
            }
            if (!VL::ValidateParticleDataChannelValue(
                    value, validationError))
            {
                std::cout << validationError << std::endl;
                return;
            }
            runtimeCommand.floatValue = value;
            std::cout << "Particle data channel set queued: "
                << name << "=" << value << std::endl;
        }
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particleattach")
    {
        const char* usage =
            "Usage: particleattach <system> mesh <target> "
            "[x y z [rx ry rz [sx sy sz]]] | "
            "particleattach <system> component <blueprint> <component> "
            "[x y z [rx ry rz [sx sy sz]]] | "
            "particleattach <system> detach";
        std::string name;
        std::string kind;
        if (!(commandStream >> name >> kind))
        {
            std::cout << usage << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::SetParticleSystemAttachment;
        runtimeCommand.stringValue = name;
        runtimeCommand.sourceText = line;
        if (kind == "detach")
        {
            runtimeCommand.secondaryStringValue = "detach";
            std::string extra;
            if (commandStream >> extra)
            {
                std::cout << usage << std::endl;
                return;
            }
            std::cout << "Particle detach queued: " << name << std::endl;
            commandBus.Queue(std::move(runtimeCommand));
            return;
        }
        if (kind != "mesh" && kind != "component")
        {
            std::cout << usage << std::endl;
            return;
        }
        runtimeCommand.secondaryStringValue = kind;
        if (!(commandStream >> runtimeCommand.particleAttachmentTarget))
        {
            std::cout << usage << std::endl;
            return;
        }
        if (kind == "component" &&
            !(commandStream >> runtimeCommand.particleAttachmentComponent))
        {
            std::cout << usage << std::endl;
            return;
        }
        std::vector<std::string> values;
        std::string token;
        while (commandStream >> token) values.push_back(token);
        if (!values.empty() && values.size() != 3 &&
            values.size() != 6 && values.size() != 9)
        {
            std::cout << usage << std::endl;
            return;
        }
        for (size_t index = 0; index < values.size(); ++index)
        {
            float value = 0.0f;
            if (!ParseConsoleFloat(values[index], value))
            {
                std::cout << usage << std::endl;
                return;
            }
            if (index < 3) runtimeCommand.particleAttachmentPosition[index] = value;
            else if (index < 6) runtimeCommand.particleAttachmentRotation[index - 3] = value;
            else runtimeCommand.particleAttachmentScale[index - 6] = value;
        }
        if (!runtimeCommand.particleAttachmentScale.allFinite() ||
            std::abs(runtimeCommand.particleAttachmentScale.x()) < 1.0e-6f ||
            std::abs(runtimeCommand.particleAttachmentScale.y()) < 1.0e-6f ||
            std::abs(runtimeCommand.particleAttachmentScale.z()) < 1.0e-6f)
        {
            std::cout
                << "Particle attachment scale axes must have absolute value >= 1e-6."
                << std::endl;
            return;
        }
        std::cout << "Particle attachment queued: " << name
            << " -> " << kind << ":" << runtimeCommand.particleAttachmentTarget
            << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particletelemetry")
    {
        std::string name;
        std::string extra;
        if ((commandStream >> name) && (commandStream >> extra))
        {
            std::cout << "Usage: particletelemetry [system]" << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::ReportParticleTelemetry;
        runtimeCommand.stringValue = std::move(name);
        runtimeCommand.sourceText = line;
        std::cout << "Particle telemetry queued." << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "camera")
    {
        std::string mode;
        if (!(commandStream >> mode))
        {
            std::cout << "Usage: camera <get|position|lookat|pose> ..." << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        if (mode == "get")
        {
            runtimeCommand.type = VL::RuntimeCommandType::GetCameraState;
        }
        else if (mode == "position")
        {
            runtimeCommand.type = VL::RuntimeCommandType::SetCameraPosition;
            if (!(commandStream >> runtimeCommand.cameraPositionValue.x() >>
                    runtimeCommand.cameraPositionValue.y() >>
                    runtimeCommand.cameraPositionValue.z()))
            {
                std::cout << "Usage: camera position <x> <y> <z>" << std::endl;
                return;
            }
        }
        else if (mode == "lookat")
        {
            runtimeCommand.type = VL::RuntimeCommandType::SetCameraLookAt;
            if (!(commandStream >> runtimeCommand.cameraLookAtValue.x() >>
                    runtimeCommand.cameraLookAtValue.y() >>
                    runtimeCommand.cameraLookAtValue.z()))
            {
                std::cout << "Usage: camera lookat <x> <y> <z>" << std::endl;
                return;
            }
        }
        else if (mode == "pose")
        {
            runtimeCommand.type = VL::RuntimeCommandType::SetCameraPose;
            if (!(commandStream >> runtimeCommand.cameraPositionValue.x() >>
                    runtimeCommand.cameraPositionValue.y() >>
                    runtimeCommand.cameraPositionValue.z() >>
                    runtimeCommand.cameraLookAtValue.x() >>
                    runtimeCommand.cameraLookAtValue.y() >>
                    runtimeCommand.cameraLookAtValue.z()))
            {
                std::cout << "Usage: camera pose <px> <py> <pz> <tx> <ty> <tz>" << std::endl;
                return;
            }
        }
        else
        {
            std::cout << "Usage: camera <get|position|lookat|pose> ..." << std::endl;
            return;
        }

        if (!runtimeCommand.cameraPositionValue.allFinite() ||
            !runtimeCommand.cameraLookAtValue.allFinite())
        {
            std::cout << "Camera values must be finite." << std::endl;
            return;
        }
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "reloadassembly")
    {
        // 独立包验证入口：从当前配置的 assembly 路径重新加载 Gameplay。
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::ReloadAssembly;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "injectinput")
    {
        std::string action, mode;
        if (!(commandStream >> action >> mode) || action.empty() ||
            (mode != "down" && mode != "up"))
        {
            std::cout << "Usage: injectinput <action> <down|up>" << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::InjectInput;
        runtimeCommand.stringValue = action;
        runtimeCommand.intValue = mode == "down" ? 1 : 0;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "shaderreload")
    {
        std::string scope;
        if (!(commandStream >> scope))
        {
            std::cout << "Usage: shaderreload <changed|all|help>" << std::endl;
            return;
        }
        if (scope == "help")
        {
            PrintShaderReloadHelp();
            return;
        }
        if (scope != "changed" && scope != "all")
        {
            std::cout << "Usage: shaderreload <changed|all|help>" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::ReloadShaders;
        runtimeCommand.shaderReloadScope =
            scope == "all"
                ? VL::RuntimeShaderReloadScope::All
                : VL::RuntimeShaderReloadScope::Changed;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "shadercache")
    {
        std::string action;
        if (!(commandStream >> action) || action != "stats")
        {
            std::cout << "Usage: shadercache stats" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::ReportShaderCacheStatistics;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    std::cout << "Unknown command: " << command << std::endl;
}

void DebugConsole::PrintHelp() const
{
    std::cout << "Available commands:\n";
    std::cout << "  animation <get|play|pause|seek seconds|clip index> - control native skeletal playback\n";
    std::cout << "  motiondebug <0|1> - actual history and predicted trajectory overlay\n";
    std::cout << "  debugdraw <on|off|status|demo [overlay/depth]|category name on/off|owner id/*|clear [owner/category name]>\n";
    std::cout << "  editor <JSON> - submit an editor asset command and print its structured result\n";
    std::cout << "  debugview <mode> - set debug view mode\n";
    std::cout << "    0: Full\n";
    std::cout << "    1: BaseColor\n";
    std::cout << "    2: Emissive\n";
    std::cout << "    3: Normal\n";
    std::cout << "    4: Roughness\n";
    std::cout << "    5: Metallic\n";
    std::cout << "    6: AO\n";
    std::cout << "    7: Shadow\n";
    std::cout << "    8: Direct Lighting\n";
    std::cout << "    9: Indirect Diffuse\n";
    std::cout << "    10: Indirect Specular\n";
    std::cout << "    11: Shadow Cascade Index\n";
    std::cout << "    12: Shading Model\n";
    std::cout << "    13: Subsurface Weight\n";
    std::cout << "    14: Transmission Weight\n";
    std::cout << "    15: SSS Asset ID\n";
    std::cout << "    16: Local SSS Response\n";
    std::cout << "    17: Diffuse Before SSS\n";
    std::cout << "    18: Diffuse After SSS\n";
    std::cout << "    19: SSS Pixel Radius\n";
    std::cout << "    20: SSS Valid Weight\n";
    std::cout << "    64: Cloth Shading Model\n";
    std::cout << "    65: Cloth Sheen Color\n";
    std::cout << "    66: Cloth Sheen Roughness\n";
    std::cout << "    67: Cloth Charlie D\n";
    std::cout << "    68: Cloth Neubelt Visibility\n";
    std::cout << "    69: Cloth Directional Albedo\n";
    std::cout << "    70: Cloth Base Energy Scale\n";
    std::cout << "    71: Cloth Direct Sheen\n";
    std::cout << "    72: Cloth Indirect Sheen\n";
    std::cout << "    73: Cloth IBL Fallback\n";
    std::cout << "    74: Skin Direct Diffuse\n";
    std::cout << "    75: Skin Transmission\n";
    std::cout << "    76: Skin Shadow Visibility\n";
    std::cout << "    77: Skin IBL Diffuse\n";
    std::cout << "    78: Skin IBL Specular\n";
    std::cout << "    79: Skin Virtual Light\n";
    std::cout << "    90: Deferred Velocity (zero=0.5, XY gain=20)\n";
    std::cout << "  tonemap <mode> - set tone mapping mode\n";
    std::cout << "    0: Linear clamp\n";
    std::cout << "    1: Reinhard\n";
    std::cout << "    2: Hable\n";
    std::cout << "    3: ACES\n";
    std::cout << "  bloom strength <value> - set bloom composite strength\n";
    std::cout << "  bloom threshold <value> - set bloom threshold\n";
    std::cout << "  bloom knee <value> - set bloom soft knee\n";
    std::cout << "  bloom clamp <value> - set bloom fireflies clamp\n";
    std::cout << "  loadworld <scene-path> - request scene load through WorldTransitionCoordinator\n";
    std::cout << "  offline <on|off|samples N|bounces N|seed N|lightSamples N|aov MODE|exposure X|pause|resume|stop|restart|export>\n";
    std::cout << "  environment <value> - set unified sky and IBL intensity\n";
    std::cout << "  windgust <on|off|once> - toggle or trigger one SpeedTree gust\n";
    std::cout << "  windstrength <0..1> - set the SpeedTree base strength target\n";
    std::cout << "  screenshot [file.bmp] - capture the presented swapchain image\n";
    std::cout << "  particledump [file.json] [maxPerEmitter] - read back GPU particle pools into Generated/ParticleDumps\n";
    std::cout << "  particlestats - print last frame particle emitter/draw/sort budget plus particle pass GPU ms\n";
    std::cout << "  particlespawn <name> <PS_asset_path> [x y z] - add a particle system instance to the active World\n";
    std::cout << "  particledestroy <name> - remove a particle system instance from the active World\n";
    std::cout << "  particlerestart <name> - reset one particle system instance (simulation time, prewarm, cull)\n";
    std::cout << "  particleparam <name> <spawnRateScale <scale>|colorTint <r> <g> <b> <a>|emitter <index> <0|1>|reset> - runtime-only particle overrides\n";
    std::cout << "  particleemitter <system> <spawn <sourceIndex> [name]|destroy <index|name>|restart <index|name>|enable <index|name> <0|1>> - emitter lifecycle\n";
    std::cout << "  particlebudget <system|world> <maxTotal> <emitterScale> - runtime particle budget\n";
    std::cout << "  particleuser <system> [<key> <value>|clear <key>] - runtime named user parameter\n";
    std::cout << "  particledata <name> <value> | particledata <name> clear | particledata - World-global particle data channel\n";
    std::cout << "  particleattach <system> <mesh <target>|component <blueprint> <component>|detach> [x y z [rx ry rz [sx sy sz]]] - attach to scene object/component\n";
    std::cout << "  particletelemetry [system] - print CPU-authored counts and renderer-fed alive/budget data\n";
    std::cout << "  camera get - print the active camera pose\n";
    std::cout << "  camera position <x> <y> <z> - set camera position\n";
    std::cout << "  camera lookat <x> <y> <z> - aim camera at a world-space target\n";
    std::cout << "  camera pose <px> <py> <pz> <tx> <ty> <tz> - set position and look-at\n";
    std::cout << "  shaderreload <changed|all> - transactionally publish live Graphics, Compute, and UI shaders\n";
    std::cout << "  reloadassembly - reload the configured Gameplay assembly in the live World\n";
    std::cout << "  injectinput <action> <down|up> - inject a gameplay action for automation (no OS focus required)\n";
    std::cout << "  shaderreload help - show hot-reload triggers by resource type\n";
    std::cout << "  shadercache stats - report shader build cache statistics\n";
}

void DebugConsole::PrintShaderReloadHelp() const
{
    std::cout << "Shader hot-reload triggers:\n";
    std::cout << "  Material/Graphics - save .vert, .frag, or a depended-on .glsl file\n";
    std::cout << "  Compute           - save .comp or a depended-on .glsl file\n";
    std::cout << "  UI Overlay        - save uiOverlay .vert/.frag or a depended-on .glsl file\n";
    std::cout << "  Material M_*.json - save the definition under shader/glsl; rebuilds World/Graph resources and migrates compatible live MI state\n";
    std::cout << "  Shared include    - save .glsl; only live dependents are rebuilt\n";
    std::cout << "Manual commands:\n";
    std::cout << "  shaderreload changed - rebuild live dependents changed since the committed manifest\n";
    std::cout << "  shaderreload all     - rebuild all live Graphics, Compute, and UI shaders\n";
    std::cout << "  M_*.json schema reload is automatic; it has no manual shaderreload scope\n";
    std::cout << "Not watched by this system: MI_*.json, scene JSON, RenderGraph JSON, shader/spv outputs\n";
    std::cout << "Regular Graphics/Compute/UI replacement requires ABI compatibility.\n";
}

void DebugConsole::PrintPrompt() const
{
    std::cout << "debug> " << std::flush;
}
