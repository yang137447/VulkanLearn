#include "eyeAuthoringAdapter.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{

void PrintUsage()
{
    std::cout << "Usage: eye_authoring_adapter --input FILE --output FILE "
                 "[--report FILE] [--strict]\n";
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string inputPath;
        std::string outputPath;
        std::string reportPath;
        VL::EyeAuthoringAdapterOptions options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index];
            if (argument == "--input" && index + 1 < argc)
            {
                inputPath = argv[++index];
            }
            else if (argument == "--output" && index + 1 < argc)
            {
                outputPath = argv[++index];
            }
            else if (argument == "--report" && index + 1 < argc)
            {
                reportPath = argv[++index];
            }
            else if (argument == "--strict")
            {
                options.strict = true;
            }
            else
            {
                PrintUsage();
                return 2;
            }
        }
        if (inputPath.empty() || outputPath.empty())
        {
            PrintUsage();
            return 2;
        }
        const VL::EyeAuthoringAdapterResult result =
            VL::ConvertEyeAuthoringFile(inputPath, options);
        std::ofstream output(outputPath);
        if (!output.is_open())
        {
            throw std::runtime_error("Failed to write migrated Eye material");
        }
        output << result.materialInstance.dump(4) << '\n';
        if (!reportPath.empty())
        {
            std::ofstream report(reportPath);
            if (!report.is_open())
            {
                throw std::runtime_error("Failed to write Eye migration report");
            }
            report << result.report.dump(4) << '\n';
        }
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Eye authoring adapter failed: " << exception.what() << '\n';
        return 1;
    }
}
