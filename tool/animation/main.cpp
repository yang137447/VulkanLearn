#include "ozzDependencyProbe.h"
#include "fbxInspection.h"
#include "gaspImportReport.h"
#include "ueProjectInventory.h"
#include "motionDatabaseBuild.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
int Run(const std::vector<std::string>& arguments)
{
    const bool projectInventory = arguments.size() == 5 && arguments[1] == "--project" &&
        arguments[3] == "--output";
    const bool dependencyProbe = arguments.size() == 3 && arguments[1] == "--output";
    const bool fbxInspection = arguments.size() == 5 && arguments[1] == "--fbx" &&
        arguments[3] == "--output";
    const bool gaspImport = arguments.size() == 5 && arguments[1] == "--import-gasp" &&
        arguments[3] == "--output";
    const bool skinReference = arguments.size() == 7 && arguments[1] == "--skin-gasp" &&
        arguments[3] == "--time" && arguments[5] == "--output";
    const bool motionBuild = arguments.size() == 7 && arguments[1] == "--build-motion-db" &&
        arguments[3] == "--resource-root" && arguments[5] == "--output";
    if (!projectInventory && !dependencyProbe && !fbxInspection && !gaspImport && !skinReference && !motionBuild)
    {
        std::cerr << "Usage: vl_animation_probe [--project <file.uproject> | --fbx <file.fbx>"
            " | --import-gasp <audit-directory>]"
            " --output <new-report.json>\n"
            "       vl_animation_probe --skin-gasp <audit-directory> --time <seconds> --output <new.json>\n"
            "       vl_animation_probe --build-motion-db <resource-source.json> --resource-root <root> --output <new.json>\n";
        return 2;
    }
    try
    {
        const auto path = std::filesystem::absolute(std::filesystem::u8path(arguments.back()));
        if (std::filesystem::exists(path))
        {
            throw std::runtime_error("Output already exists; choose a new report path");
        }
        if (projectInventory)
        {
            VL::AnimationPreflight::ValidateUnrealInventoryOutput(
                std::filesystem::u8path(arguments[2]), path);
        }
        if (gaspImport || skinReference)
        {
            VL::AnimationPreflight::ValidateUnrealInventoryOutput(
                std::filesystem::u8path(arguments[2]) / "audit.json", path);
        }
        if (motionBuild)
        {
            VL::AnimationPreflight::ValidateUnrealInventoryOutput(
                std::filesystem::u8path(arguments[4]) / arguments[2], path);
        }
        nlohmann::json report;
        if (motionBuild)
        {
            report = VL::AnimationPreflight::BuildMotionDatabaseReport(
                std::filesystem::u8path(arguments[4]), arguments[2]);
        }
        else if (projectInventory)
        {
            report = VL::AnimationPreflight::InventoryUnrealProject(
                std::filesystem::u8path(arguments[2]));
        }
        else if (fbxInspection)
        {
            report = VL::AnimationPreflight::InspectFbx(std::filesystem::u8path(arguments[2]));
        }
        else if (skinReference)
        {
            size_t consumed = 0;
            const float time = std::stof(arguments[4], &consumed);
            if (consumed != arguments[4].size()) throw std::invalid_argument("Invalid sample time");
            report = VL::AnimationPreflight::BuildGaspSkinReference(
                std::filesystem::u8path(arguments[2]), time);
        }
        else if (gaspImport)
        {
            report = VL::AnimationPreflight::BuildGaspImportReport(
                std::filesystem::u8path(arguments[2]));
        }
        else
        {
            report = VL::AnimationPreflight::BuildDependencyReport();
        }
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary);
        output << report.dump(2) << '\n';
        output.close();
        if (!output)
        {
            throw std::runtime_error("Cannot write animation report");
        }
        if (dependencyProbe)
        {
            std::cout << "Dependency: ozz-animation 0.16.0\n"
                      << "Joints: " << report["jointCount"]
                      << ", samples: " << report["sampleCount"]
                      << ", duration: " << report["duration"] << '\n'
                      << "Max model position error (source units): "
                      << report["maxModelPositionError"] << '\n';
        }
        else if (motionBuild)
        {
            std::cout << "Motion database frames: " << report["frames"]
                << ", dimensions: " << report["dimensions"] << ", cache: " << report["cacheStatus"] << '\n';
        }
        else if (projectInventory)
        {
            std::cout << "UE inventory files: " << report["inputFileCount"] << '\n'
                      << "Input BLAKE3: " << report["inputBlake3"] << '\n';
        }
        else if (skinReference)
        {
            std::cout << "CPU skin reference sections: " << report["sections"].size() << '\n';
        }
        else if (gaspImport)
        {
            std::cout << "Imported joints: " << report["joints"].size()
                      << ", clips: " << report["clips"].size() << '\n';
        }
        else
        {
            std::cout << "FBX nodes: " << report["nodes"].size()
                      << ", meshes: " << report["meshes"].size()
                      << ", animations: " << report["animations"].size() << '\n';
        }
        std::cout << "Report: " << path.u8string() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    // Windows CRT 的窄 argv 依赖本机代码页；在边界转为 UTF-8，保留中文项目和报告路径。
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index)
    {
        arguments.push_back(std::filesystem::path(argv[index]).u8string());
    }
    return Run(arguments);
}
#else
int main(int argc, char** argv)
{
    return Run(std::vector<std::string>(argv, argv + argc));
}
#endif
