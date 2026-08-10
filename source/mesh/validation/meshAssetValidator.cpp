#include "meshAssetValidator.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace
{
    void EnsureObject(const nlohmann::json& json, std::string_view meshAssetPath)
    {
        if (!json.is_object())
        {
            throw std::runtime_error("Mesh asset must be a JSON object: " + std::string(meshAssetPath));
        }
    }

    std::string ReadRequiredString(
        const nlohmann::json& json,
        std::string_view fieldName,
        std::string_view meshAssetPath)
    {
        const std::string field(fieldName);
        if (!json.contains(field) || !json[field].is_string())
        {
            throw std::runtime_error(
                "Mesh asset is missing string field \"" + field + "\": " +
                std::string(meshAssetPath));
        }

        const std::string value = json[field].get<std::string>();
        if (value.empty())
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must not be empty: " +
                std::string(meshAssetPath));
        }
        return value;
    }

    std::vector<MeshMaterialSlot> ReadMaterialSlots(
        const nlohmann::json& json,
        std::string_view meshAssetPath,
        std::string_view modelDataPath)
    {
        if (!json.contains("materialSlots"))
        {
            throw std::runtime_error(
                "Mesh asset is missing required materialSlots. Migrate materialInstancePath to materialSlots: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }
        if (!json["materialSlots"].is_array() || json["materialSlots"].empty())
        {
            throw std::runtime_error(
                "Mesh asset materialSlots must be a non-empty ordered array: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }

        std::vector<MeshMaterialSlot> materialSlots;
        for (size_t slotIndex = 0; slotIndex < json["materialSlots"].size(); ++slotIndex)
        {
            const auto& item = json["materialSlots"][slotIndex];
            if (!item.is_object())
            {
                throw std::runtime_error(
                    "Mesh asset material slot must be an object: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slotIndex=" + std::to_string(slotIndex));
            }
            if (!item.contains("name") || !item["name"].is_string())
            {
                throw std::runtime_error(
                    "Mesh asset material slot is missing string field name: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slotIndex=" + std::to_string(slotIndex));
            }
            const std::string slotName = item["name"].get<std::string>();
            if (slotName.empty())
            {
                throw std::runtime_error(
                    "Mesh asset material slot name must not be empty: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slotIndex=" + std::to_string(slotIndex));
            }
            bool bHasDuplicateSlotName = false;
            for (const MeshMaterialSlot& materialSlot : materialSlots)
            {
                if (materialSlot.name == slotName)
                {
                    bHasDuplicateSlotName = true;
                    break;
                }
            }
            if (bHasDuplicateSlotName)
            {
                throw std::runtime_error(
                    "Mesh asset materialSlots contains a duplicate slot name: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slot=" + slotName);
            }
            if (!item.contains("materialInstancePath") || !item["materialInstancePath"].is_string())
            {
                throw std::runtime_error(
                    "Mesh asset material slot is missing string field materialInstancePath: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slot=" + slotName);
            }

            const std::string materialInstancePath = item["materialInstancePath"].get<std::string>();
            if (materialInstancePath.empty())
            {
                throw std::runtime_error(
                    "Mesh asset material slot path must not be empty: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slot=" + slotName);
            }
            MeshMaterialSlot materialSlot;
            materialSlot.name = slotName;
            materialSlot.materialInstancePath = materialInstancePath;
            materialSlots.push_back(materialSlot);
        }
        return materialSlots;
    }

    MeshImportOptions ReadImportOptions(
        const nlohmann::json& json,
        std::string_view meshAssetPath,
        std::string_view modelDataPath)
    {
        MeshImportOptions importOptions;
        if (!json.contains("importOptions"))
        {
            return importOptions;
        }
        if (!json["importOptions"].is_object())
        {
            throw std::runtime_error(
                "Mesh asset importOptions must be an object: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }

        const auto& optionsJson = json["importOptions"];
        if (optionsJson.contains("generateSmoothNormals"))
        {
            if (!optionsJson["generateSmoothNormals"].is_boolean())
            {
                throw std::runtime_error(
                    "Mesh asset importOptions.generateSmoothNormals must be a boolean: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
            }
            importOptions.generateSmoothNormals = optionsJson["generateSmoothNormals"].get<bool>();
        }
        return importOptions;
    }

    std::vector<std::string> BuildUniqueSourceMaterialSlots(const ModelResource& modelResource)
    {
        if (!modelResource.sourceMaterialSlotNames.empty())
        {
            return modelResource.sourceMaterialSlotNames;
        }

        std::vector<std::string> sourceSlots;
        for (const MeshSection& section : modelResource.sections)
        {
            const auto it = std::find(sourceSlots.begin(), sourceSlots.end(), section.materialSlotName);
            if (it == sourceSlots.end())
            {
                sourceSlots.push_back(section.materialSlotName);
            }
        }
        return sourceSlots;
    }

    size_t FindSourceSlotIndex(const std::vector<std::string>& sourceSlots, const std::string& slotName)
    {
        const auto it = std::find(sourceSlots.begin(), sourceSlots.end(), slotName);
        if (it == sourceSlots.end())
        {
            return sourceSlots.size();
        }
        return static_cast<size_t>(std::distance(sourceSlots.begin(), it));
    }

    std::string BuildSlotCountMismatchReason(
        const MeshAssetBuildPlan& buildPlan,
        size_t sourceSlotCount,
        bool bIsStrictMatch)
    {
        std::string reason = bIsStrictMatch
            ? "SpeedTree materialSlots count does not match parsed .stsdk selected LOD material slot count"
            : "Mesh materialSlots count does not match source material slot count";
        reason += ": " + buildPlan.meshAssetPath +
            " model=" + buildPlan.modelDataPath +
            " materialSlots=" + std::to_string(buildPlan.materialSlots.size()) +
            " parsedSlots=" + std::to_string(sourceSlotCount);
        return reason;
    }
}

MeshAssetBuildPlan MeshAssetValidator::BuildLoadPlan(
    std::string_view meshAssetPath,
    const nlohmann::json& effectiveMeshAssetJson)
{
    EnsureObject(effectiveMeshAssetJson, meshAssetPath);

    if (effectiveMeshAssetJson.contains("materialInstancePath"))
    {
        throw std::runtime_error(
            "Mesh asset field materialInstancePath is no longer supported. Use materialSlots instead: " +
            std::string(meshAssetPath));
    }

    MeshAssetBuildPlan loadPlan;
    loadPlan.assetType = ReadRequiredString(effectiveMeshAssetJson, "type", meshAssetPath);
    if (loadPlan.assetType != "mesh" && loadPlan.assetType != "speedtree")
    {
        throw std::runtime_error(
            "Mesh asset type must be \"mesh\" or \"speedtree\": " +
            std::string(meshAssetPath));
    }
    loadPlan.meshAssetPath = std::string(meshAssetPath);
    loadPlan.modelDataPath = ReadRequiredString(effectiveMeshAssetJson, "modelDataPath", meshAssetPath);
    loadPlan.importOptions = ReadImportOptions(
        effectiveMeshAssetJson,
        meshAssetPath,
        loadPlan.modelDataPath);
    loadPlan.materialSlots = ReadMaterialSlots(effectiveMeshAssetJson, meshAssetPath, loadPlan.modelDataPath);
    loadPlan.modelCacheKey = loadPlan.modelDataPath;
    if (loadPlan.importOptions.generateSmoothNormals)
    {
        loadPlan.modelCacheKey += "|generateSmoothNormals";
    }
    return loadPlan;
}

std::vector<MeshSectionLoadPlan> MeshAssetValidator::BuildSectionLoadPlans(
    const MeshAssetBuildPlan& buildPlan,
    const ModelResource& modelResource)
{
    if (modelResource.sections.empty())
    {
        throw std::runtime_error(
            "Model resource has no sections: " +
            buildPlan.meshAssetPath + " model=" + buildPlan.modelDataPath);
    }

    std::vector<MeshSectionLoadPlan> sectionPlans;
    sectionPlans.reserve(modelResource.sections.size());
    const std::vector<std::string> sourceSlots = BuildUniqueSourceMaterialSlots(modelResource);
    if (buildPlan.assetType == "speedtree" && buildPlan.materialSlots.size() != sourceSlots.size())
    {
        throw std::runtime_error(BuildSlotCountMismatchReason(buildPlan, sourceSlots.size(), true));
    }
    if (buildPlan.assetType != "speedtree" && buildPlan.materialSlots.size() != sourceSlots.size())
    {
        throw std::runtime_error(BuildSlotCountMismatchReason(buildPlan, sourceSlots.size(), false));
    }

    for (const MeshSection& section : modelResource.sections)
    {
        if (section.materialSlotName.empty())
        {
            throw std::runtime_error(
                "Model section is missing material slot name: " +
                buildPlan.meshAssetPath + " model=" + buildPlan.modelDataPath +
                " section=" + section.sectionName);
        }

        const size_t sourceSlotIndex = FindSourceSlotIndex(sourceSlots, section.materialSlotName);
        if (sourceSlotIndex >= buildPlan.materialSlots.size())
        {
            throw std::runtime_error(
                "Mesh section material slot cannot be mapped by source slot index: " +
                buildPlan.meshAssetPath + " model=" + buildPlan.modelDataPath +
                " section=" + section.sectionName +
                " slot=" + section.materialSlotName);
        }
        const MeshMaterialSlot& materialSlot = buildPlan.materialSlots[sourceSlotIndex];

        MeshSectionLoadPlan sectionPlan;
        sectionPlan.sectionName = section.sectionName;
        sectionPlan.materialSlotName = materialSlot.name;
        sectionPlan.materialInstancePath = materialSlot.materialInstancePath;
        sectionPlans.push_back(sectionPlan);
    }
    return sectionPlans;
}
