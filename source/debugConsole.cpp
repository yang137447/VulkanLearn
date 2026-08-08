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

    if (command == "reloadstress")
    {
        std::string scenePath;
        int reloadCount = 20;
        if (!(commandStream >> scenePath))
        {
            std::cout << "Usage: reloadstress <scene-path> [count]" << std::endl;
            return;
        }
        commandStream >> std::ws;
        if (!commandStream.eof() && !(commandStream >> reloadCount))
        {
            std::cout << "Usage: reloadstress <scene-path> [count]" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::RunWorldReloadStress;
        runtimeCommand.stringValue = scenePath;
        runtimeCommand.intValue = reloadCount;
        runtimeCommand.sourceText = line;
        commandBus.Queue(std::move(runtimeCommand));
        return;
    }

    if (command == "lightstress")
    {
        int reloadCount = 3;
        commandStream >> std::ws;
        if (!commandStream.eof() && !(commandStream >> reloadCount))
        {
            std::cout << "Usage: lightstress [count]" << std::endl;
            return;
        }

        VL::RuntimeCommand runtimeCommand;
        runtimeCommand.type = VL::RuntimeCommandType::RunGeneratedHighLightReloadStress;
        runtimeCommand.intValue = reloadCount;
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
    std::cout << "  reloadstress <scene-path> [count] - reload a scene once per frame for validation\n";
    std::cout << "  lightstress [count] - generate a high-light scene and reload it for validation\n";
    std::cout << "  environment <value> - set unified sky and IBL intensity\n";
    std::cout << "  windgust <on|off|once> - toggle or trigger one SpeedTree gust\n";
    std::cout << "  windstrength <0..1> - set the SpeedTree base strength target\n";
}

void DebugConsole::PrintPrompt() const
{
    std::cout << "debug> " << std::flush;
}
