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
#include "engine/consoleCommandParsing.h"
#include "engine/editorHost.h"
#include "hair/hairConsoleCommand.h"
#include "hair/tressfxLiveParams.h"
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

void DebugConsole::ProcessCommand(const std::string& line)
{
    std::istringstream commandStream(line);
    std::string command;
    commandStream >> command;

    if (command == "editor")
    {
        if (editorConsoleSink == nullptr || !editorConsoleSink->IsAvailable())
        {
            std::cout << "Editor document host unavailable; enable --dev-ui or use vl_asset_edit." << std::endl;
            return;
        }
        std::string request;
        std::getline(commandStream, request);
        try
        {
            const auto submitted = editorConsoleSink->SubmitJson(request);
            std::cout << submitted.text << std::endl;
            if (submitted.queued)
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
    if (command == "script")
    {
        // 与编辑器 Play 按钮同一个 ScriptRuntime::Control 入口；供无 GUI 烟测与
        // 自动化脚本驱动蓝图生命周期（不是 runtime test flag）。
        std::string action, extra;
        if (!(commandStream >> action) ||
            (action != "play" && action != "pause" && action != "stop") ||
            (commandStream >> extra))
        {
            std::cout << "Usage: script <play|pause|stop>" << std::endl;
            return;
        }
        VL::RuntimeCommand request;
        request.type = VL::RuntimeCommandType::SetScriptPlayState;
        request.intValue = action == "play" ? 1 : action == "pause" ? 2 : 0;
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

    if (command == "gpustats")
    {
        // 与其它命令一样只生产意图：采集开关和表格都归 RenderSystem 所有，
        // 由 GT 的命令执行端调用，避免控制台直接接触渲染线程状态。
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::PrintGpuPassStats;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "perf")
    {
        // 机器通道只接受明确子命令；输出行由 GT 执行端统一序列化，
        // 控制台不读取 renderer 状态，也不打印任何前缀。
        std::string action;
        std::string extra;
        if (!(commandStream >> action) || action != "json" || (commandStream >> extra))
        {
            std::cout << "Usage: perf json" << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::PrintPerformanceJson;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    // mesh <transform|offset|sweep> ...：正常 Console 诊断命令，只有显式调用才会改变
    // actor placement。Console 线程不持有 World，因此这里只解析意图；身份存在性、
    // 有限值和实际写入都由 GT 的 RuntimeCommandExecutor 校验。
    // 参数在切出子串后重新解析，避免 istream 在"旋转只给 1..2 个分量"时留下半消费状态。
    if (command == "mesh")
    {
        const char* meshUsage =
            "Usage: mesh transform <identity> <px> <py> <pz> [rx ry rz]\n"
            "       mesh offset <identity> <dx> <dy> <dz>\n"
            "       mesh sweep <identityA> <identityB> <dx> <dy> <dz> <seconds>";
        std::string action;
        if (!(commandStream >> action))
        {
            std::cout << meshUsage << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SetMeshPlacement;
        runtimeCommand.sourceText = line;

        if (action == "transform" || action == "offset")
        {
            if (!(commandStream >> runtimeCommand.meshPlacement.identity))
            {
                std::cout << meshUsage << std::endl;
                return;
            }
            std::string rest;
            std::getline(commandStream, rest);
            std::istringstream arguments(rest);
            if (!(arguments >> runtimeCommand.meshPlacement.position.x() >>
                    runtimeCommand.meshPlacement.position.y() >>
                    runtimeCommand.meshPlacement.position.z()))
            {
                std::cout << meshUsage << std::endl;
                return;
            }
            float rotation[3] = {0.0f, 0.0f, 0.0f};
            int rotationCount = 0;
            while (rotationCount < 3 && (arguments >> rotation[rotationCount]))
            {
                ++rotationCount;
            }
            std::string extra;
            if ((arguments >> extra) || rotationCount == 1 || rotationCount == 2 ||
                (action == "offset" && rotationCount != 0))
            {
                std::cout << meshUsage << std::endl;
                return;
            }
            if (action == "transform")
            {
                runtimeCommand.meshPlacementMode = VL::MeshPlacementMode::Absolute;
                if (rotationCount == 3)
                {
                    runtimeCommand.meshPlacement.rotation =
                        Eigen::Vector3f(rotation[0], rotation[1], rotation[2]);
                }
            }
            else
            {
                // 增量只叠加位移：rotation/scale 保留当前值，避免一次平移把朝向复位。
                runtimeCommand.meshPlacementMode = VL::MeshPlacementMode::Offset;
                runtimeCommand.meshPlacement.delta = runtimeCommand.meshPlacement.position;
            }
            if (!runtimeCommand.meshPlacement.position.allFinite() ||
                !runtimeCommand.meshPlacement.rotation.allFinite())
            {
                std::cout << "Mesh values must be finite." << std::endl;
                return;
            }
            commandBus.Queue(std::move(runtimeCommand));
            return;
        }

        if (action == "sweep")
        {
            if (!(commandStream >> runtimeCommand.meshPlacement.identity >>
                    runtimeCommand.meshPlacement.secondIdentity))
            {
                std::cout << meshUsage << std::endl;
                return;
            }
            std::string rest;
            std::getline(commandStream, rest);
            std::istringstream arguments(rest);
            float deltaX = 0.0f;
            float deltaY = 0.0f;
            float deltaZ = 0.0f;
            float seconds = 0.0f;
            std::string extra;
            if (!(arguments >> deltaX >> deltaY >> deltaZ >> seconds) ||
                (arguments >> extra))
            {
                std::cout << meshUsage << std::endl;
                return;
            }
            if (!std::isfinite(deltaX) || !std::isfinite(deltaY) ||
                !std::isfinite(deltaZ) || !std::isfinite(seconds) || seconds <= 0.0f)
            {
                std::cout << "Mesh sweep values must be finite and seconds must be positive." << std::endl;
                return;
            }
            runtimeCommand.meshPlacementMode = VL::MeshPlacementMode::Sweep;
            runtimeCommand.meshPlacement.delta = Eigen::Vector3f(deltaX, deltaY, deltaZ);
            runtimeCommand.meshPlacement.seconds = seconds;
            commandBus.Queue(std::move(runtimeCommand));
            return;
        }

        std::cout << meshUsage << std::endl;
        return;
    }

    // hair trace <path> <frames> <guide-stride>：一次性只读 GPU 回读。
    // hair reset [objectName]：请求渲染线程在下一个正 delta 帧执行一次 ResetPositions。
    // Console 线程只解析值语义；对象选择、guide 采样、Vulkan buffer 与文件
    // 都归渲染线程的 HairSimulationPass，未激活时零开销。
    if (command == "hair")
    {
        const char* hairUsage =
            "Usage: hair trace <path> <frames> <guide-stride>\n"
            "       hair reset [objectName]\n"
            "       hair set <objectName> <field> <value...>\n"
            "       fields: gravity (x y z), damping, iterations, bendStiffness,\n"
            "               globalShapeStiffness, globalShapeEffectiveRange,\n"
            "               vspCoefficient, vspAccelThresholdMax";
        std::string action;
        if (!(commandStream >> action))
        {
            std::cout << hairUsage << std::endl;
            return;
        }
        if (action == "reset")
        {
            std::string rest;
            std::getline(commandStream, rest);
            VL::Hair::HairResetRequest reset;
            if (const char* error = VL::Hair::ParseHairResetArguments(rest, reset))
            {
                std::cout << error << std::endl;
                return;
            }
            VL::RuntimeCommand runtimeCommand;
            runtimeCommand.type = VL::RuntimeCommandType::ResetHairSimulation;
            runtimeCommand.stringValue = std::move(reset.objectName);
            runtimeCommand.sourceText = line;
            commandBus.Queue(std::move(runtimeCommand));
            return;
        }
        if (action == "set")
        {
            std::string rest;
            std::getline(commandStream, rest);
            VL::Hair::HairLiveParameterRequest request;
            if (const char* error = VL::Hair::ParseHairSetArguments(rest, request))
            {
                std::cout << error << std::endl;
                return;
            }
            VL::RuntimeCommand runtimeCommand;
            runtimeCommand.type = VL::RuntimeCommandType::SetHairSimulationParameter;
            runtimeCommand.hairLiveParameter = std::move(request);
            runtimeCommand.sourceText = line;
            commandBus.Queue(std::move(runtimeCommand));
            return;
        }
        if (action != "trace")
        {
            std::cout << hairUsage << std::endl;
            return;
        }
        std::string path;
        long long frames = 0;
        long long stride = 0;
        std::string extra;
        if (!(commandStream >> path >> frames >> stride) || path.empty() ||
            (commandStream >> extra) || frames <= 0 || stride <= 0 ||
            frames > 1000000LL || stride > 1000000LL)
        {
            std::cout << hairUsage << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::RequestHairTrace;
        runtimeCommand.stringValue = path;
        runtimeCommand.hairTraceFrames = static_cast<uint32_t>(frames);
        runtimeCommand.hairTraceGuideStride = static_cast<uint32_t>(stride);
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "help")
    {
        PrintHelp();
        return;
    }

    if (command == "quit")
    {
        std::string extra;
        if (commandStream >> extra)
        {
            std::cout << "Usage: quit" << std::endl;
            return;
        }
        VL::RuntimeCommand request;
        request.type = VL::RuntimeCommandType::QuitApplication;
        request.sourceText = line;
        commandBus.Queue(std::move(request));
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

    if (command == "softbody")
    {
        std::string action;
        std::string name;
        float speed = 800.0f;
        commandStream >> action >> name;
        if (action != "impact" || name.empty())
        {
            std::cout << "Usage: softbody impact <balloon-name> [speed-cm-per-second]" << std::endl;
            return;
        }
        if (!(commandStream >> speed)) speed = 800.0f;
        if (!std::isfinite(speed) || speed <= 0.0f || speed > 100000.0f)
        {
            std::cout << "softbody impact speed must be finite in (0, 100000]." << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::ImpactSoftBody;
        runtimeCommand.stringValue = name;
        runtimeCommand.floatValue = speed;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "glass")
    {
        std::string action;
        std::string name;
        float speed = 800.0f;
        commandStream >> action >> name;
        if (action != "impact" || name.empty())
        {
            std::cout << "Usage: glass impact <pane-name> [speed-cm-per-second]" << std::endl;
            return;
        }
        if (!(commandStream >> speed)) speed = 800.0f;
        if (!std::isfinite(speed) || speed <= 0.0f || speed > 100000.0f)
        {
            std::cout << "glass impact speed must be finite in (0, 100000]." << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::ImpactGlassPane;
        runtimeCommand.stringValue = name;
        runtimeCommand.floatValue = speed;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "shoot")
    {
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::FireProjectile;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "inputaxis")
    {
        std::string axis;
        float value = 0.0f;
        if (!(commandStream >> axis >> value) || !std::isfinite(value))
        {
            std::cout << "Usage: inputaxis <name> <value> (e.g. inputaxis MoveForward 1)"
                      << std::endl;
            return;
        }
        std::string trailing;
        if (commandStream >> trailing)
        {
            std::cout << "Usage: inputaxis <name> <value>" << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::InjectAxis;
        runtimeCommand.stringValue = axis;
        runtimeCommand.floatValue = value;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "sun")
    {
        std::string rest;
        std::getline(commandStream, rest);
        const VL::ConsoleCommandParsing::SunArgument parsed =
            VL::ConsoleCommandParsing::ParseSunIntensity(rest);
        if (!parsed.ok)
        {
            std::cout << "Usage: sun <lux> (finite value > 0)" << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SetSunIntensity;
        runtimeCommand.floatValue = parsed.lux;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "exposure")
    {
        std::string field;
        float value = 0.0f;
        if (!(commandStream >> field >> value))
        {
            std::cout << "Usage: exposure <field> <value>; fields: mode ev100 aperture "
                         "shutterSeconds iso compensation autoMinEv100 autoMaxEv100 "
                         "autoSpeedUp autoSpeedDown autoTargetGray" << std::endl;
            return;
        }
        std::string trailing;
        if ((commandStream >> trailing) || !std::isfinite(value))
        {
            std::cout << "Usage: exposure <field> <value> (finite)" << std::endl;
            return;
        }
        if (!VL::IsCameraExposureField(field))
        {
            std::cout << "Unknown exposure field: " << field << std::endl;
            std::cout << "Usage: exposure <field> <value>; fields: mode ev100 aperture "
                         "shutterSeconds iso compensation autoMinEv100 autoMaxEv100 "
                         "autoSpeedUp autoSpeedDown autoTargetGray" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SetCameraExposure;
        runtimeCommand.stringValue = field;
        runtimeCommand.floatValue = value;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "matparam")
    {
        std::string rest;
        std::getline(commandStream, rest);
        const VL::ConsoleCommandParsing::MaterialParameterArgument parsed =
            VL::ConsoleCommandParsing::ParseMaterialParameter(rest);
        if (!parsed.ok)
        {
            std::cout << "Usage: matparam <materialInstanceKey> <paramName> [v0 [v1 [v2 [v3]]]]"
                      << std::endl;
            std::cout << "  Key for a blueprint-owned mesh is "
                         "<MI path>#bp/<sceneObjectIdentity>/<slot>, e.g. "
                         "Maps/SC_gun_outline_demo/Materials/MI_gun_body.json"
                         "#bp/__blueprint/dropped-gun/gun/0" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SetMaterialParameter;
        runtimeCommand.materialInstanceKey = parsed.materialInstanceKey;
        runtimeCommand.materialParameterName = parsed.parameterName;
        runtimeCommand.materialValueCount = parsed.valueCount;
        for (uint32_t index = 0; index < parsed.valueCount; ++index)
        {
            runtimeCommand.materialValues[index] = parsed.values[index];
        }
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

    if (command == "particlekill")
    {
        std::string systemName;
        std::string emitterName;
        std::string indexToken;
        std::string extra;
        if (!(commandStream >> systemName >> emitterName >> indexToken) ||
            (commandStream >> extra))
        {
            std::cout
                << "Usage: particlekill <system> <emitter> <particleIndex>"
                << std::endl;
            return;
        }
        int particleIndex = -1;
        if (!ParseConsoleInt(indexToken, particleIndex))
        {
            std::cout
                << "Usage: particlekill <system> <emitter> <particleIndex>"
                << std::endl;
            return;
        }
        std::string validationError;
        if (!VL::ValidateParticleKillRequest(
                systemName, emitterName, particleIndex,
                validationError))
        {
            std::cout << validationError << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::KillParticle;
        runtimeCommand.stringValue = std::move(systemName);
        runtimeCommand.particleEmitterName = std::move(emitterName);
        runtimeCommand.intValue = particleIndex;
        runtimeCommand.sourceText = line;
        std::cout
            << "Particle kill queued: " << runtimeCommand.stringValue
            << "." << runtimeCommand.particleEmitterName
            << " slot=" << particleIndex << std::endl;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "particlespawnat")
    {
        const char* usage =
            "Usage: particlespawnat <system> <emitter> <x> <y> <z> "
            "[lifetime]";
        std::string systemName;
        std::string emitterName;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!(commandStream >> systemName >> emitterName >> x >> y >> z))
        {
            std::cout << usage << std::endl;
            return;
        }
        float lifetime = 0.0f;
        std::string lifetimeToken;
        if (commandStream >> lifetimeToken)
        {
            if (!ParseConsoleFloat(lifetimeToken, lifetime))
            {
                std::cout << usage << std::endl;
                return;
            }
            std::string extra;
            if (commandStream >> extra)
            {
                std::cout << usage << std::endl;
                return;
            }
        }
        const Eigen::Vector3f position(x, y, z);
        std::string validationError;
        if (!VL::ValidateParticleSpawnAtRequest(
                systemName, emitterName, position, lifetime,
                validationError))
        {
            std::cout << validationError << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::SpawnParticleAt;
        runtimeCommand.stringValue = std::move(systemName);
        runtimeCommand.particleEmitterName = std::move(emitterName);
        runtimeCommand.particleSpawnPosition = position;
        runtimeCommand.floatValue = lifetime;
        runtimeCommand.sourceText = line;
        std::cout
            << "Particle spawn-at queued: " << runtimeCommand.stringValue
            << "." << runtimeCommand.particleEmitterName
            << " at (" << x << ", " << y << ", " << z << ")"
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
            "particledata <name> vec2 <x> <y> | "
            "particledata <name> vec3 <x> <y> <z> | "
            "particledata <name> vec4 <x> <y> <z> <w> | "
            "particledata <name> quat <x> <y> <z> <w> | "
            "particledata <name> bool <true|false> | "
            "particledata <name> int <value> | "
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
        std::string validationError;
        if (!VL::ValidateParticleDataChannelName(
                name, validationError))
        {
            std::cout << validationError << std::endl;
            return;
        }
        std::vector<std::string> arguments;
        for (std::string token; commandStream >> token;)
            arguments.push_back(std::move(token));
        if (arguments.empty())
        {
            std::cout << usage << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::SetParticleDataChannel;
        runtimeCommand.particleDataChannelName = name;
        runtimeCommand.sourceText = line;
        if (arguments.size() == 1 && arguments[0] == "clear")
        {
            runtimeCommand.particleDataChannelClear = true;
            std::cout << "Particle data channel clear queued: "
                << name << std::endl;
        }
        else
        {
            VL::ParticleDataChannelValue value;
            if (!VL::ParseParticleDataChannelText(
                    arguments, value, validationError))
            {
                std::cout << validationError << std::endl;
                std::cout << usage << std::endl;
                return;
            }
            runtimeCommand.particleDataChannelValue = value;
            runtimeCommand.particleDataChannelTyped = true;
            if (value.type == VL::ParticleDataChannelType::Float)
                runtimeCommand.floatValue = value.values[0];
            std::cout << "Particle data channel set queued: "
                << name << "=" << value.ToJson().dump() << std::endl;
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

    if (command == "terrain")
    {
        // P3 可观测/验收入口：stats 只读快照；capacity 通过 atomic 排队到
        // 渲染线程帧首生效，不在 Console 线程直接改 page manager。
        std::string action;
        if (!(commandStream >> action))
        {
            std::cout
                << "Usage: terrain <stats|capacity N|parity [tileX tileZ]>"
                << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.sourceText = line;
        if (action == "stats")
        {
            runtimeCommand.type =
                VL::RuntimeCommandType::ReportTerrainStatistics;
        }
        else if (action == "capacity")
        {
            int capacity = 0;
            if (!(commandStream >> capacity) || capacity <= 0)
            {
                std::cout
                    << "Usage: terrain capacity <positive page count>"
                    << std::endl;
                return;
            }
            runtimeCommand.type =
                VL::RuntimeCommandType::SetTerrainResidentCapacity;
            runtimeCommand.intValue = capacity;
        }
        else if (action == "parity")
        {
            int tileX = 0;
            int tileZ = 0;
            if (commandStream >> tileX)
            {
                if (!(commandStream >> tileZ))
                    tileZ = 0;
            }
            if (tileX < 0 || tileX > 31 || tileZ < 0 || tileZ > 31)
            {
                std::cout
                    << "Usage: terrain parity [tileX tileZ] (0..31)"
                    << std::endl;
                return;
            }
            runtimeCommand.type =
                VL::RuntimeCommandType::RunTerrainPageBakeParity;
            runtimeCommand.intValue = tileX * 32 + tileZ;
        }
        else
        {
            std::cout
                << "Usage: terrain <stats|capacity N|parity [tileX tileZ]>"
                << std::endl;
            return;
        }
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "terrainfeedback")
    {
        // P2 只读诊断：打印最近一次已完成 fence 的 feedback 页统计，
        // 不触发驻留请求，也不进入 runtime test flag。
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::ReportTerrainFeedbackStatistics;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "terrainlayerstats" || command == "terrainlayer")
    {
        // L2/L4a：stats 只读；capacity 排队层页预算；preload 是 feedback
        // 驱动驻留的全量对照路径。
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.sourceText = line;
        if (command == "terrainlayer")
        {
            std::string action;
            if (!(commandStream >> action))
            {
                std::cout
                    << "Usage: terrainlayer <stats|capacity N|preload|forcemip N>"
                    << std::endl;
                return;
            }
            if (action == "stats")
            {
                runtimeCommand.type =
                    VL::RuntimeCommandType::ReportTerrainLayerStatistics;
            }
            else if (action == "preload")
            {
                runtimeCommand.type =
                    VL::RuntimeCommandType::RequestTerrainLayerPreload;
            }
            else if (action == "capacity")
            {
                int capacity = 0;
                if (!(commandStream >> capacity) || capacity <= 0)
                {
                    std::cout
                        << "Usage: terrainlayer capacity <positive page count>"
                        << std::endl;
                    return;
                }
                runtimeCommand.type =
                    VL::RuntimeCommandType::SetTerrainLayerResidentCapacity;
                runtimeCommand.intValue = capacity;
            }
            else if (action == "forcemip")
            {
                // L4e：调试用目标 mip 覆盖。-1 恢复自动；0..6 强制，用于在
                // 页尚未驻留时复现粗 mip 回退与 avgColor 渐变。
                int forceMip = 0;
                if (!(commandStream >> forceMip) ||
                    forceMip < -1 || forceMip > 6)
                {
                    std::cout
                        << "Usage: terrainlayer forcemip <-1|0..6>"
                        << std::endl;
                    return;
                }
                runtimeCommand.type =
                    VL::RuntimeCommandType::SetTerrainLayerForceMip;
                runtimeCommand.intValue = forceMip;
            }
            else
            {
                std::cout
                    << "Usage: terrainlayer <stats|capacity N|preload|forcemip N>"
                    << std::endl;
                return;
            }
        }
        else
        {
            runtimeCommand.type =
                VL::RuntimeCommandType::ReportTerrainLayerStatistics;
        }
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "terrainlod")
    {
        // 只读诊断：输出最近一帧每个 chunk LOD 组的选择级、相机距离和
        // 累计切换次数；不排队任何渲染状态修改。
        std::string action;
        if (!(commandStream >> action) || action != "stats")
        {
            std::cout << "Usage: terrainlod stats" << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::ReportTerrainLodStatistics;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "terrainchunk")
    {
        // 只读诊断：加载期几何 residency 的 chunk/顶点/索引字节基线，
        // 叠加最近一帧的实际 draw 数与级分布。
        std::string action;
        if (!(commandStream >> action) || action != "stats")
        {
            std::cout << "Usage: terrainchunk stats" << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::ReportTerrainChunkStatistics;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "terraincollision")
    {
        // 只读诊断：输出 Play 中已创建的地形 heightfield body 与 placement；
        // 非 Play 时明确报告没有物理世界。
        std::string action;
        if (!(commandStream >> action) || action != "stats")
        {
            std::cout << "Usage: terraincollision stats" << std::endl;
            return;
        }
        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type =
            VL::RuntimeCommandType::ReportTerrainCollisionStatistics;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    std::cout << "Unknown command: " << command << std::endl;
}
