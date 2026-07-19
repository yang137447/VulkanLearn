#include "materialParameterIncludeGenerator.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include "../../commonFunction.h"
#include "../materialAssetUtils.h"
#include "../materialDescriptorSchema.h"
#include "../validation/materialAssetValidator.h"

namespace
{
    std::filesystem::path FindShaderGlslRoot(const std::filesystem::path& materialFilePath)
    {
        std::filesystem::path current = materialFilePath.parent_path();
        while (!current.empty())
        {
            if (current.filename() == "glsl")
            {
                return current;
            }
            current = current.parent_path();
        }
        throw std::runtime_error("Material definition must live under shader/glsl: " + MaterialAssetUtils::ToGenericString(materialFilePath));
    }

    std::string BuildShadingModelIncludePath(const std::filesystem::path& materialFilePath)
    {
        const std::filesystem::path outputDir = materialFilePath.parent_path() / "generate";
        const std::filesystem::path shadingModelPath = FindShaderGlslRoot(materialFilePath) / "common" / "shadingModel.glsl";
        return MaterialAssetUtils::ToGenericString(std::filesystem::relative(shadingModelPath, outputDir));
    }

    std::string BuildParamterIncludeSource(const nlohmann::json& materialJson, const std::filesystem::path& materialFilePath)
    {
        MaterialAssetValidator::ValidateDefinition(materialJson, MaterialAssetUtils::ToGenericString(materialFilePath));
        const VL::MaterialDescriptorSchema descriptorSchema =
            VL::MaterialDescriptorSchema::Build(
                materialJson,
                MaterialAssetUtils::ToGenericString(materialFilePath));
        const std::string materialName = materialJson["name"].get<std::string>();
        const std::string guardName = "VL_GENERATED_" + MaterialAssetUtils::ToUpperIdentifier(materialName) + "_PARAMTERS";
        const std::string shadingModelDefine = MaterialAssetUtils::ShadingModelToShaderDefine(materialJson["shadingModel"].get<std::string>());

        std::ostringstream stream;
        stream << "#ifndef " << guardName << "\n";
        stream << "#define " << guardName << "\n\n";
        stream << "// Generated from " << MaterialAssetUtils::ToGenericString(materialFilePath) << ".\n";
        stream << "// Do not edit by hand.\n\n";
        stream << "#include \"" << BuildShadingModelIncludePath(materialFilePath) << "\"\n\n";
        stream << "#ifndef MATERIAL_SHADING_MODEL\n";
        stream << "#define MATERIAL_SHADING_MODEL " << shadingModelDefine << "\n";
        stream << "#endif\n\n";

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

        if (!descriptorSchema.GetParameters().empty())
        {
            stream << "layout(set = 1, binding = 0) uniform UBOMIParamters {\n";
            for (const VL::MaterialParameterSchemaEntry& parameter : descriptorSchema.GetParameters())
            {
                stream << "    " << parameter.glslType << " " << parameter.name << ";\n";
            }
            stream << "};\n\n";
        }

        for (const VL::MaterialTextureSchemaEntry& texture : descriptorSchema.GetTextures())
        {
            stream << "layout(set = 1, binding = " << texture.binding << ") uniform "
                   << texture.glslType << " " << texture.name << ";\n";
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
