#include "debugConsole.h"

#include <conio.h>
#include <cctype>
#include <cmath>
#include <iostream>
#include <sstream>
#include <utility>

#include "engine/runtimeCommand.h"

DebugConsole::DebugConsole(VL::CommandBus& commandBus)
    : commandBus(commandBus)
{
}

void DebugConsole::Initialize()
{
    std::cout << "Debug console ready. Type 'help' for available commands." << std::endl;
    PrintPrompt();
}

void DebugConsole::Update()
{
    while (_kbhit())
    {
        const int ch = _getch();
        HandleInput(ch);
    }
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
    std::cout << "  environment <value> - set unified sky and IBL intensity\n";
    std::cout << "  windgust <on|off|once> - toggle or trigger one SpeedTree gust\n";
    std::cout << "  windstrength <0..1> - set the SpeedTree base strength target\n";
    std::cout << "  screenshot [file.bmp] - capture the presented swapchain image\n";
    std::cout << "  camera get - print the active camera pose\n";
    std::cout << "  camera position <x> <y> <z> - set camera position\n";
    std::cout << "  camera lookat <x> <y> <z> - aim camera at a world-space target\n";
    std::cout << "  camera pose <px> <py> <pz> <tx> <ty> <tz> - set position and look-at\n";
    std::cout << "  shaderreload <changed|all> - transactionally publish live Graphics, Compute, and UI shaders\n";
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
