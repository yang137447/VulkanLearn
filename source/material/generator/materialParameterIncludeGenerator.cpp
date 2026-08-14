#include "materialParameterIncludeGenerator.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../../commonFunction.h"
#include "../materialAssetUtils.h"
#include "../materialDescriptorSchema.h"
#include "../validation/materialAssetValidator.h"
#include "shader/build/atomicFile.h"

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

    std::filesystem::path NormalizeOutputPath(const std::filesystem::path& path)
    {
        std::error_code canonicalError;
        const std::filesystem::path canonicalPath =
            std::filesystem::weakly_canonical(path, canonicalError);
        if (!canonicalError)
        {
            return canonicalPath;
        }

        std::error_code absoluteError;
        const std::filesystem::path absolutePath =
            std::filesystem::absolute(path, absoluteError);
        if (!absoluteError)
        {
            return absolutePath.lexically_normal();
        }
        return path.lexically_normal();
    }

    std::string BuildOutputPathIdentity(const std::filesystem::path& path)
    {
        std::string identity = path.generic_string();
#if defined(_WIN32)
        std::transform(
            identity.begin(),
            identity.end(),
            identity.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
#endif
        return identity;
    }

    struct OutputBackup
    {
        std::filesystem::path path;
        bool existed = false;
        std::vector<uint8_t> bytes;
    };

    void RollbackGeneratedIncludeCommit(
        const std::vector<OutputBackup>& outputBackups)
    {
        std::vector<VL::AtomicFileWrite> restoreWrites;
        restoreWrites.reserve(outputBackups.size());
        for (const OutputBackup& backup : outputBackups)
        {
            if (backup.existed)
            {
                const bool currentMatchesBackup =
                    std::filesystem::is_regular_file(backup.path) &&
                    VL::ReadBinaryFile(backup.path) == backup.bytes;
                if (!currentMatchesBackup)
                {
                    restoreWrites.push_back({backup.path, backup.bytes});
                }
            }
        }

        if (!restoreWrites.empty())
        {
            VL::WriteFileBatchAtomically(restoreWrites);
        }

        for (const OutputBackup& backup : outputBackups)
        {
            if (!backup.existed && std::filesystem::exists(backup.path))
            {
                std::error_code removeError;
                std::filesystem::remove(backup.path, removeError);
                if (removeError)
                {
                    throw std::runtime_error(
                        "Failed to remove new generated include while rolling back: " +
                        backup.path.string() + ": " + removeError.message());
                }
            }
        }
    }

    std::vector<MaterialParameterIncludeGenerator::GeneratedIncludeCandidate>
    BuildGeneratedIncludesUnderRoot(const std::filesystem::path& materialRoot)
    {
        std::vector<MaterialParameterIncludeGenerator::GeneratedIncludeCandidate> candidates;
        if (!std::filesystem::exists(materialRoot))
        {
            return candidates;
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
                candidates.push_back(
                    MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent(path));
            }
        }
        return candidates;
    }
}

MaterialParameterIncludeGenerator::GeneratedIncludeCandidate
MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent(
    const std::filesystem::path& materialFilePath)
{
    std::ifstream materialFile(materialFilePath);
    if (!materialFile.is_open())
    {
        throw std::runtime_error(
            "Failed to open material definition: " +
            MaterialAssetUtils::ToGenericString(materialFilePath));
    }

    nlohmann::json materialJson;
    materialFile >> materialJson;
    const std::string generatedBytes =
        BuildParamterIncludeSource(materialJson, materialFilePath);
    const std::string materialName = materialJson.at("name").get<std::string>();
    const std::filesystem::path outputPath =
        materialFilePath.parent_path() /
        "generate" /
        (materialName + "Paramter.glsl");

    return {
        materialFilePath,
        outputPath,
        generatedBytes};
}

void MaterialParameterIncludeGenerator::CommitGeneratedIncludesIfChanged(
    const std::vector<GeneratedIncludeCandidate>& candidates)
{
    std::vector<VL::AtomicFileWrite> writes;
    std::vector<OutputBackup> outputBackups;
    std::vector<std::filesystem::path> normalizedOutputPaths;
    std::set<std::string> outputPathIdentities;
    normalizedOutputPaths.reserve(candidates.size());

    for (const GeneratedIncludeCandidate& candidate : candidates)
    {
        if (candidate.outputPath.empty())
        {
            throw std::runtime_error(
                "Generated material include candidate has an empty output path: " +
                MaterialAssetUtils::ToGenericString(candidate.materialSourcePath));
        }

        const std::filesystem::path normalizedPath =
            NormalizeOutputPath(candidate.outputPath);
        if (!outputPathIdentities.insert(
                BuildOutputPathIdentity(normalizedPath)).second)
        {
            throw std::runtime_error(
                "Generated material include batch contains duplicate output path: " +
                normalizedPath.string());
        }
        if (std::filesystem::exists(normalizedPath) &&
            !std::filesystem::is_regular_file(normalizedPath))
        {
            throw std::runtime_error(
                "Generated material include output is not a regular file: " +
                normalizedPath.string());
        }
        normalizedOutputPaths.push_back(normalizedPath);
    }

    writes.reserve(candidates.size());
    outputBackups.reserve(candidates.size());
    for (size_t candidateIndex = 0;
         candidateIndex < candidates.size();
         ++candidateIndex)
    {
        const GeneratedIncludeCandidate& candidate = candidates[candidateIndex];
        const std::filesystem::path& normalizedPath =
            normalizedOutputPaths[candidateIndex];
        OutputBackup backup;
        backup.path = normalizedPath;
        backup.existed = std::filesystem::is_regular_file(normalizedPath);
        if (backup.existed)
        {
            backup.bytes = VL::ReadBinaryFile(normalizedPath);
        }

        const bool unchanged =
            backup.existed &&
            backup.bytes.size() == candidate.generatedBytes.size() &&
            std::equal(
                backup.bytes.begin(),
                backup.bytes.end(),
                candidate.generatedBytes.begin());
        if (!unchanged)
        {
            writes.push_back({
                normalizedPath,
                std::vector<uint8_t>(
                    candidate.generatedBytes.begin(),
                    candidate.generatedBytes.end())});
            outputBackups.push_back(std::move(backup));
        }
    }

    if (writes.empty())
    {
        return;
    }

    try
    {
        VL::WriteFileBatchAtomically(writes);
    }
    catch (...)
    {
        try
        {
            RollbackGeneratedIncludeCommit(outputBackups);
        }
        catch (const std::exception& rollbackException)
        {
            throw std::runtime_error(
                "Generated material include commit failed and output rollback also failed: " +
                std::string(rollbackException.what()));
        }
        throw;
    }
}

void MaterialParameterIncludeGenerator::GenerateAllIncludes()
{
    const std::vector<GeneratedIncludeCandidate> candidates =
        BuildGeneratedIncludesUnderRoot(
            std::filesystem::path(CommonFunction::GetProjectPath()) /
            "shader" /
            "glsl");
    CommitGeneratedIncludesIfChanged(candidates);
}

void MaterialParameterIncludeGenerator::GenerateInclude(const std::filesystem::path& materialFilePath)
{
    const GeneratedIncludeCandidate candidate =
        BuildGeneratedIncludeContent(materialFilePath);
    CommitGeneratedIncludesIfChanged({candidate});
}
