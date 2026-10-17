#include "gameExporter.h"

#include <fstream>
#include <iostream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    try
    {
        VL::GameExportOptions options;
#ifdef _WIN32
        wchar_t executable[32768];
        const auto size = GetModuleFileNameW(nullptr, executable, 32768);
        if (!size || size >= 32768) throw std::runtime_error("Cannot locate exporter executable");
        options.binaryDirectory = std::filesystem::path(std::wstring(executable, size)).parent_path();
#else
        options.binaryDirectory = std::filesystem::absolute(argv[0]).parent_path();
#endif
#ifdef VL_EXPORT_DOTNET_ROOT
        options.dotnetRoot = VL_EXPORT_DOTNET_ROOT;
#endif
#ifndef VL_EXPORT_CSHARP_ENABLED
        options.csharpEnabled = false;
#endif
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index];
            if (argument == "--help")
            {
                std::cout << "vl_export_game --project-root <root> --initial-scene <resource-relative.json> "
                    "--output <new-directory> [--dynamic-assets <json-array-file>] "
                    "[--binary-directory <directory>] [--dotnet-root <directory>]\n";
                return 0;
            }
            if (++index == argc) throw std::invalid_argument("Missing value for " + argument);
            const std::string value = argv[index];
            if (argument == "--project-root") options.projectRoot = value;
            else if (argument == "--initial-scene") options.initialScene = value;
            else if (argument == "--output") options.outputDirectory = value;
            else if (argument == "--binary-directory") options.binaryDirectory = value;
            else if (argument == "--dotnet-root") options.dotnetRoot = value;
            else if (argument == "--dynamic-assets")
            {
                std::ifstream input(value);
                if (!input) throw std::invalid_argument("Cannot open dynamic asset list: " + value);
                options.dynamicAssets = nlohmann::json::parse(input).get<std::vector<std::string>>();
            }
            else throw std::invalid_argument("Unknown export option: " + argument);
        }
        const auto result = VL::ExportGame(options);
        std::cout << nlohmann::json{{"ok", true}, {"files", result.fileCount}, {"bytes", result.totalBytes},
            {"output", std::filesystem::absolute(options.outputDirectory).string()}}.dump() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << nlohmann::json{{"ok", false}, {"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
