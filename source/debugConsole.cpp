#include "debugConsole.h"

#include <conio.h>
#include <cctype>
#include <iostream>
#include <sstream>

#include "renderSystem.h"
#include "materialInstance.h"
#include "renderGraph.h"

DebugConsole::DebugConsole(RenderSystem& renderSystem)
    : renderSystem(renderSystem)
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

        renderSystem.SetDebugViewMode(mode);
        std::cout << "Debug view mode set to " << mode << std::endl;
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

        auto& renderpasses = RenderGraph::GetInstance().GetRenderpasses();
        auto passIt = renderpasses.find("toneMapping");
        if (passIt != renderpasses.end())
        {
            auto mi = passIt->second.materialInstance.lock();
            if (!mi)
            {
                std::cout << "Tone mapping material instance is expired." << std::endl;
                return;
            }

            if (mi->HasParameter("u_toneMappingParams"))
            {
                Eigen::Vector4f params = mi->GetParameter<Eigen::Vector4f>("u_toneMappingParams");
                params.w() = static_cast<float>(mode);
                mi->SetParameter("u_toneMappingParams", params);
                std::cout << "Tone mapping mode set to " << mode << std::endl;
            }
            else
            {
                std::cout << "Parameter 'u_toneMappingParams' not found in tone mapping material." << std::endl;
            }
        }
        else
        {
            std::cout << "Tone mapping pass not found." << std::endl;
        }
        return;
    }

    if (command == "help")
    {
        PrintHelp();
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
    std::cout << "  tonemap <mode> - set tone mapping mode\n";
    std::cout << "    0: Linear clamp\n";
    std::cout << "    1: Reinhard\n";
    std::cout << "    2: Hable\n";
    std::cout << "    3: ACES\n";
}

void DebugConsole::PrintPrompt() const
{
    std::cout << "debug> " << std::flush;
}
