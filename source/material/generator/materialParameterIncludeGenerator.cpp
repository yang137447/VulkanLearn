#include "materialParameterIncludeGenerator.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../../commonFunction.h"
#include "../materialAssetUtils.h"
#include "../validation/materialAssetValidator.h"

namespace
{
    std::string ParameterGlslType(std::string_view type)
    {
        if (type == "float" || type == "vec2" || type == "vec3" || type == "vec4" || type == "int" || type == "uint")
        {
            return std::string(type);
        }
        throw std::runtime_error("Unsupported material parameter type: " + std::string(type));
    }

    std::string TextureGlslType(std::string_view type)
    {
        if (type == "sampler2D")
        {
            return "sampler2D";
        }
        throw std::runtime_error("Unsupported material texture type: " + std::string(type));
    }

    uint32_t ParameterTypeRank(std::string_view type)
    {
        if (type == "vec4")
        {
            return 0;
        }
        if (type == "vec3")
        {
            return 1;
        }
        if (type == "vec2")
        {
            return 2;
        }
        return 3;
    }

    std::vector<std::string> BuildParameterEmissionOrder(const nlohmann::json& parameters)
    {
        std::vector<std::string> names;
        names.reserve(parameters.size());
        for (const auto& [name, paramDesc] : parameters.items())
        {
            names.push_back(name);
        }

        std::sort(names.begin(), names.end(), [&parameters](const std::string& lhs, const std::string& rhs) {
            const uint32_t lhsRank = ParameterTypeRank(parameters[lhs]["type"].get<std::string>());
            const uint32_t rhsRank = ParameterTypeRank(parameters[rhs]["type"].get<std::string>());
            if (lhsRank != rhsRank)
            {
                return lhsRank < rhsRank;
            }
            return lhs < rhs;
        });
        return names;
    }

    std::string BuildParamterIncludeSource(const nlohmann::json& materialJson, const std::filesystem::path& materialFilePath)
    {
        MaterialAssetValidator::ValidateDefinition(materialJson, MaterialAssetUtils::ToGenericString(materialFilePath));
        const std::string materialName = materialJson["name"].get<std::string>();
        const std::string guardName = "VL_GENERATED_" + MaterialAssetUtils::ToUpperIdentifier(materialName) + "_PARAMTERS";
        const uint32_t shadingModelId = MaterialAssetUtils::ShadingModelToId(materialJson["shadingModel"].get<std::string>());

        std::ostringstream stream;
        stream << "#ifndef " << guardName << "\n";
        stream << "#define " << guardName << "\n\n";
        stream << "// Generated from " << MaterialAssetUtils::ToGenericString(materialFilePath) << ".\n";
        stream << "// Do not edit by hand.\n\n";
        stream << "#define MATERIAL_SHADING_MODEL " << shadingModelId << "u\n\n";

        for (const auto& [macroName, macroValue] : materialJson["macros"].items())
        {
            stream << "#ifndef " << macroName << "\n";
            stream << "#define " << macroName << " " << MaterialAssetUtils::MacroValueToString(macroValue) << "\n";
            stream << "#endif\n";
        }

        if (!materialJson["macros"].empty())
        {
            stream << "\n";
        }

        if (!materialJson["parameters"].empty())
        {
            stream << "layout(set = 1, binding = 0) uniform UBOMIParamters {\n";
            const std::vector<std::string> parameterOrder = BuildParameterEmissionOrder(materialJson["parameters"]);
            for (const std::string& name : parameterOrder)
            {
                const auto& paramDesc = materialJson["parameters"][name];
                stream << "    " << ParameterGlslType(paramDesc["type"].get<std::string>()) << " " << name << ";\n";
            }
            stream << "};\n\n";
        }

        uint32_t binding = 1;
        for (const auto& [name, textureDesc] : materialJson["textures"].items())
        {
            stream << "layout(set = 1, binding = " << binding++ << ") uniform "
                   << TextureGlslType(textureDesc["type"].get<std::string>()) << " " << name << ";\n";
        }

        stream << "\n#endif\n";
        return stream.str();
    }

    void GenerateMaterialRoot(const std::filesystem::path& materialRoot)
    {
        if (!std::filesystem::exists(materialRoot))
        {
            return;
        }

        for (const auto& entry : std::filesystem::recursive_directory_iterator(materialRoot))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const std::filesystem::path path = entry.path();
            const std::string filename = path.filename().string();
            if (path.extension() == ".json" && filename.rfind("M_", 0) == 0)
            {
                MaterialParameterIncludeGenerator::GenerateInclude(path);
            }
        }
    }
}

void MaterialParameterIncludeGenerator::GenerateAllIncludes()
{
    GenerateMaterialRoot(std::filesystem::path(CommonFunction::GetProjectPath()) / "shader" / "glsl");
}

void MaterialParameterIncludeGenerator::GenerateInclude(const std::filesystem::path& materialFilePath)
{
    std::ifstream materialFile(materialFilePath);
    if (!materialFile.is_open())
    {
        throw std::runtime_error("Failed to open material definition: " + MaterialAssetUtils::ToGenericString(materialFilePath));
    }

    nlohmann::json materialJson;
    materialFile >> materialJson;
    const std::string materialName = materialJson.at("name").get<std::string>();
    const std::filesystem::path outputPath = materialFilePath.parent_path() / "generate" / (materialName + "Paramter.glsl");

    std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream outputFile(outputPath);
    if (!outputFile.is_open())
    {
        throw std::runtime_error("Failed to write generated material include: " + MaterialAssetUtils::ToGenericString(outputPath));
    }
    outputFile << BuildParamterIncludeSource(materialJson, materialFilePath);
}
