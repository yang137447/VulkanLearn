#include "debugConsole.h"

#include <conio.h>
#include <cctype>
#include <iostream>
#include <sstream>

#include "renderSystem.h"
#include "materialInstance.h"
#include "renderGraph.h"

namespace
{
    std::shared_ptr<MaterialInstance> GetPassMaterialInstance(const char* passName)
    {
        auto& renderpasses = RenderGraph::GetInstance().GetRenderpasses();
        auto passIt = renderpasses.find(passName);
        if (passIt == renderpasses.end())
        {
            return nullptr;
        }

        return passIt->second.materialInstance.lock();
    }
}

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

        auto mi = GetPassMaterialInstance("toneMapping");
        if (mi)
        {
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
            auto mi = GetPassMaterialInstance("toneMapping");
            if (!mi)
            {
                std::cout << "Tone mapping material instance is expired." << std::endl;
                return;
            }
            if (!mi->HasParameter("u_toneMappingParams"))
            {
                std::cout << "Parameter 'u_toneMappingParams' not found in tone mapping material." << std::endl;
                return;
            }

            Eigen::Vector4f params = mi->GetParameter<Eigen::Vector4f>("u_toneMappingParams");
            params.y() = value;
            mi->SetParameter("u_toneMappingParams", params);
            std::cout << "Bloom strength set to " << value << std::endl;
            return;
        }

        auto mi = GetPassMaterialInstance("bloomPrefilter");
        if (!mi)
        {
            std::cout << "Bloom prefilter material instance is expired." << std::endl;
            return;
        }
        if (!mi->HasParameter("u_bloomPrefilterParams"))
        {
            std::cout << "Parameter 'u_bloomPrefilterParams' not found in bloom prefilter material." << std::endl;
            return;
        }

        Eigen::Vector4f params = mi->GetParameter<Eigen::Vector4f>("u_bloomPrefilterParams");
        if (field == "threshold")
        {
            params.x() = value;
        }
        else if (field == "knee")
        {
            params.y() = value;
        }
        else if (field == "clamp")
        {
            params.z() = value;
        }
        else
        {
            std::cout << "Unknown bloom field: " << field << std::endl;
            return;
        }

        mi->SetParameter("u_bloomPrefilterParams", params);
        std::cout << "Bloom " << field << " set to " << value << std::endl;
        return;
    }

    if (command == "help")
    {
        PrintHelp();
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

        renderSystem.SetEnvironmentIntensity(value);
        std::cout << "Environment intensity set to " << value << std::endl;
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
    std::cout << "  bloom strength <value> - set bloom composite strength\n";
    std::cout << "  bloom threshold <value> - set bloom threshold\n";
    std::cout << "  bloom knee <value> - set bloom soft knee\n";
    std::cout << "  bloom clamp <value> - set bloom fireflies clamp\n";
    std::cout << "  environment <value> - set unified sky and IBL intensity\n";
}

void DebugConsole::PrintPrompt() const
{
    std::cout << "debug> " << std::flush;
}
