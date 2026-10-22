#include "meshAssetValidator.h"

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    Eigen::Vector3f ReadSimulationVector(
        const nlohmann::json& json,
        std::string_view field,
        std::string_view meshAssetPath)
    {
        if (!json.contains(field) || !json.at(field).is_array() ||
            json.at(field).size() != 3)
            throw std::runtime_error(
                "TressFX simulation field must be a 3 element array: " +
                std::string(field) + " " + std::string(meshAssetPath));
        Eigen::Vector3f value;
        for (size_t i = 0; i < 3; ++i)
        {
            if (!json.at(field)[i].is_number())
                throw std::runtime_error(
                    "TressFX simulation vector contains a non-number: " +
                    std::string(field) + " " + std::string(meshAssetPath));
            value[static_cast<Eigen::Index>(i)] =
                json.at(field)[i].get<float>();
            if (!std::isfinite(value[static_cast<Eigen::Index>(i)]))
                throw std::runtime_error(
                    "TressFX simulation vector must be finite: " +
                    std::string(field) + " " + std::string(meshAssetPath));
        }
        return value;
    }

    void ReadTressfxSimulation(
        const nlohmann::json& json,
        MeshAssetBuildPlan& loadPlan,
        std::string_view meshAssetPath)
    {
        if (!json.contains("simulation"))
            return;
        const auto& simulation = json.at("simulation");
        if (!simulation.is_object())
            throw std::runtime_error(
                "TressFX simulation must be an object: " + std::string(meshAssetPath));
        auto& config = loadPlan.tressfxSimulation;
        config.enabled = simulation.value("enabled", true);
        if (simulation.contains("gravity"))
            config.gravity = ReadSimulationVector(simulation, "gravity", meshAssetPath);
        if (simulation.contains("wind"))
            config.wind = ReadSimulationVector(simulation, "wind", meshAssetPath);
        config.damping = simulation.value("damping", config.damping);
        config.iterations = simulation.value("iterations", config.iterations);
        config.rootFixedPoints = simulation.value("rootFixedPoints", config.rootFixedPoints);
        config.maxDt = simulation.value("maxDt", config.maxDt);
        config.bendStiffness = simulation.value("bendStiffness", config.bendStiffness);
        config.windGust = simulation.value("windGust", config.windGust);
        config.windFrequency = simulation.value("windFrequency", config.windFrequency);
        config.fixedStep = simulation.value("fixedStep", config.fixedStep);
        config.maxSubsteps = simulation.value("maxSubsteps", config.maxSubsteps);
        if (simulation.contains("backend"))
        {
            // 只接受两个已实现的 backend；未知值必须在资产边界失败，而不是静默回退。
            const auto& backend = simulation.at("backend");
            const std::string value = backend.is_string()
                ? backend.get<std::string>() : std::string();
            if (value == "cpu")
                config.backend = TressfxSimulationBackend::Cpu;
            else if (value == "gpu")
                config.backend = TressfxSimulationBackend::Gpu;
            else
                throw std::runtime_error(
                    "TressFX simulation backend must be \"cpu\" or \"gpu\": " +
                    std::string(meshAssetPath));
        }
        // GPU 上一个作者 iteration 会展开成一次长 dispatch；上限防止异常值直接 TDR。
        if (config.iterations == 0 || config.iterations > 16)
            throw std::runtime_error(
                "TressFX simulation iterations must be in [1, 16]: " +
                std::string(meshAssetPath));
        if (!std::isfinite(config.damping) || config.damping < 0.0f ||
            config.damping > 1.0f ||
            config.rootFixedPoints > 32 || !std::isfinite(config.maxDt) ||
            config.maxDt <= 0.0f || config.maxDt > 0.25f)
            throw std::runtime_error(
                "TressFX simulation has invalid damping/rootFixedPoints/maxDt: " +
                std::string(meshAssetPath));
        if (!std::isfinite(config.bendStiffness) || config.bendStiffness < 0.0f ||
            config.bendStiffness > 1.0f)
            throw std::runtime_error(
                "TressFX simulation bendStiffness must be in [0, 1]: " +
                std::string(meshAssetPath));
        if (!std::isfinite(config.windGust) || config.windGust < 0.0f ||
            config.windGust > 1.0f)
            throw std::runtime_error(
                "TressFX simulation windGust must be in [0, 1]: " +
                std::string(meshAssetPath));
        if (!std::isfinite(config.windFrequency) || config.windFrequency < 0.0f ||
            config.windFrequency > 10.0f)
            throw std::runtime_error(
                "TressFX simulation windFrequency must be in [0, 10]: " +
                std::string(meshAssetPath));
        if (!std::isfinite(config.fixedStep) || config.fixedStep <= 0.0f ||
            config.fixedStep > 0.1f)
            throw std::runtime_error(
                "TressFX simulation fixedStep must be in (0, 0.1]: " +
                std::string(meshAssetPath));
        if (config.maxSubsteps == 0 || config.maxSubsteps > 8)
            throw std::runtime_error(
                "TressFX simulation maxSubsteps must be in [1, 8]: " +
                std::string(meshAssetPath));
    }

    void ReadTressfxCollision(
        const nlohmann::json& json,
        MeshAssetBuildPlan& loadPlan,
        std::string_view meshAssetPath)
    {
        auto& capsules = loadPlan.tressfxSimulation.capsules;
        capsules.clear();
        if (!json.contains("collision"))
            return;
        const auto& collision = json.at("collision");
        if (!collision.is_object())
            throw std::runtime_error(
                "TressFX collision must be an object: " + std::string(meshAssetPath));
        for (auto field = collision.begin(); field != collision.end(); ++field)
        {
            if (field.key() != "capsules")
                throw std::runtime_error(
                    "TressFX collision has unknown field \"" + field.key() + "\": " +
                    std::string(meshAssetPath));
        }
        if (!collision.contains("capsules") || !collision.at("capsules").is_array())
            throw std::runtime_error(
                "TressFX collision requires a capsules array: " +
                std::string(meshAssetPath));
        const auto& array = collision.at("capsules");
        if (array.size() > VL::Hair::TressfxMaxColliders)
            throw std::runtime_error(
                "TressFX collision supports at most " +
                std::to_string(VL::Hair::TressfxMaxColliders) + " capsules: " +
                std::string(meshAssetPath));
        for (const auto& item : array)
        {
            if (!item.is_object())
                throw std::runtime_error(
                    "TressFX collision capsule must be an object: " +
                    std::string(meshAssetPath));
            for (auto field = item.begin(); field != item.end(); ++field)
            {
                if (field.key() != "a" && field.key() != "b" && field.key() != "radius")
                    throw std::runtime_error(
                        "TressFX collision capsule has unknown field \"" +
                        field.key() + "\": " + std::string(meshAssetPath));
            }
            if (!item.contains("a") || !item.contains("b") || !item.contains("radius"))
                throw std::runtime_error(
                    "TressFX collision capsule requires a, b and radius: " +
                    std::string(meshAssetPath));
            VL::Hair::TressfxCollider collider;
            collider.a = ReadSimulationVector(item, "a", meshAssetPath);
            collider.b = ReadSimulationVector(item, "b", meshAssetPath);
            if (!item.at("radius").is_number())
                throw std::runtime_error(
                    "TressFX collision capsule radius must be a number: " +
                    std::string(meshAssetPath));
            collider.radius = item.at("radius").get<float>();
            if (!std::isfinite(collider.radius) || collider.radius <= 0.0f)
                throw std::runtime_error(
                    "TressFX collision capsule radius must be finite and positive: " +
                    std::string(meshAssetPath));
            capsules.push_back(collider);
        }
    }

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
        if (optionsJson.contains("includedSectionNames"))
        {
            if (!optionsJson["includedSectionNames"].is_array() ||
                optionsJson["includedSectionNames"].empty())
            {
                throw std::runtime_error(
                    "Mesh asset importOptions.includedSectionNames must be a non-empty array of strings: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
            }
            for (const auto& sectionName : optionsJson["includedSectionNames"])
            {
                if (!sectionName.is_string() || sectionName.get<std::string>().empty())
                {
                    throw std::runtime_error(
                        "Mesh asset importOptions.includedSectionNames must contain non-empty strings: " +
                        std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
                }
                importOptions.includedSectionNames.push_back(sectionName.get<std::string>());
            }
        }
        return importOptions;
    }

    std::string ReadOptionalAnimationJsonPath(
        const nlohmann::json& json,
        std::string_view fieldName,
        std::string_view meshAssetPath,
        std::string_view modelDataPath)
    {
        const std::string field(fieldName);
        if (!json.contains(field))
        {
            return {};
        }
        if (!json.at(field).is_string())
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must be a resource-relative JSON path: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }
        const std::string value = json.at(field).get<std::string>();
        if (value.empty())
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must not be empty: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }
        const std::filesystem::path path(value);
        if (path.is_absolute() || path.has_root_directory())
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must be resource-relative: " +
                std::string(meshAssetPath) + " value=" + value);
        }
        for (const std::filesystem::path& part : path)
        {
            if (part == "..")
            {
                throw std::runtime_error(
                    "Mesh asset field \"" + field + "\" must not escape its resource root: " +
                    std::string(meshAssetPath) + " value=" + value);
            }
        }
        if (path.extension() != ".json")
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must reference a .json asset: " +
                std::string(meshAssetPath) + " value=" + value);
        }
        return value;
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
    if (loadPlan.assetType != "mesh" && loadPlan.assetType != "speedtree" &&
        loadPlan.assetType != "tressfx")
    {
        throw std::runtime_error(
            "Mesh asset type must be \"mesh\", \"speedtree\" or \"tressfx\": " +
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
    if (loadPlan.assetType == "tressfx")
    {
        // 半径是作者数据，在描述符边界收口；shader 不逐帧修补非法宽度。
        if (!effectiveMeshAssetJson.contains("strandRadius") ||
            !effectiveMeshAssetJson.at("strandRadius").is_number())
            throw std::runtime_error("TressFX mesh requires numeric strandRadius: " +
                std::string(meshAssetPath));
        const double radius = effectiveMeshAssetJson.at("strandRadius").get<double>();
        loadPlan.strandRadius = static_cast<float>(radius);
        if (!std::isfinite(radius) || !std::isfinite(loadPlan.strandRadius) ||
            loadPlan.strandRadius <= 0.0f)
            throw std::runtime_error("TressFX strandRadius must be finite and positive: " +
                std::string(meshAssetPath));
        if (std::filesystem::path(loadPlan.modelDataPath).extension() != ".tfx" ||
            loadPlan.materialSlots.size() != 1 || effectiveMeshAssetJson.contains("importOptions"))
            throw std::runtime_error(
                "TressFX requires a .tfx source, one material slot and no importOptions: " +
                std::string(meshAssetPath));
        ReadTressfxSimulation(effectiveMeshAssetJson, loadPlan, meshAssetPath);
        ReadTressfxCollision(effectiveMeshAssetJson, loadPlan, meshAssetPath);
        std::ostringstream identity;
        identity << "|tressfx-static-v1|radius=" << std::hexfloat << loadPlan.strandRadius
                 << "|sim=" << (loadPlan.tressfxSimulation.enabled ? 1 : 0)
                 << "|g=" << loadPlan.tressfxSimulation.gravity.x() << ","
                 << loadPlan.tressfxSimulation.gravity.y() << ","
                 << loadPlan.tressfxSimulation.gravity.z()
                 << "|w=" << loadPlan.tressfxSimulation.wind.x() << ","
                 << loadPlan.tressfxSimulation.wind.y() << ","
                 << loadPlan.tressfxSimulation.wind.z()
                 << "|d=" << loadPlan.tressfxSimulation.damping
                 << "|i=" << loadPlan.tressfxSimulation.iterations
                 << "|r=" << loadPlan.tressfxSimulation.rootFixedPoints
                 << "|dt=" << loadPlan.tressfxSimulation.maxDt
                 << "|bend=" << loadPlan.tressfxSimulation.bendStiffness
                 << "|gust=" << loadPlan.tressfxSimulation.windGust
                 << "|freq=" << loadPlan.tressfxSimulation.windFrequency
                 << "|fixed=" << loadPlan.tressfxSimulation.fixedStep
                 << "|substeps=" << loadPlan.tressfxSimulation.maxSubsteps
                 << "|colliders=" << loadPlan.tressfxSimulation.capsules.size();
        for (const VL::Hair::TressfxCollider& capsule :
             loadPlan.tressfxSimulation.capsules)
        {
            identity << "|cap=" << capsule.a.x() << "," << capsule.a.y() << ","
                     << capsule.a.z() << ":" << capsule.b.x() << "," << capsule.b.y()
                     << "," << capsule.b.z() << ":" << capsule.radius;
        }
        identity << "|backend=" << (loadPlan.tressfxSimulation.backend ==
                        TressfxSimulationBackend::Gpu ? "gpu" : "cpu");
        loadPlan.modelCacheKey += identity.str();
    }
    if (effectiveMeshAssetJson.contains("skinning"))
    {
        const auto& skinning = effectiveMeshAssetJson.at("skinning");
        if (loadPlan.assetType != "mesh" || !skinning.is_object() ||
            !skinning.contains("clips") || !skinning.at("clips").is_array())
            throw std::runtime_error(
                "Mesh skinning requires an object with a clips array: " +
                std::string(meshAssetPath));
        if (loadPlan.importOptions.generateSmoothNormals ||
            !loadPlan.importOptions.includedSectionNames.empty())
            throw std::runtime_error("Skeletal import does not support static importOptions");

        // skinning.root 已删除。显式拒绝而不是静默忽略，避免旧资源带着错误的
        // 根骨假设继续导入；根骨唯一来源是 skinning.skeleton 指向的 SK。
        if (skinning.contains("root"))
            throw std::runtime_error(
                "Mesh skinning.root is no longer supported; use skinning.skeleton: " +
                std::string(meshAssetPath));
        loadPlan.skeletonPath = ReadOptionalAnimationJsonPath(
            skinning, "skeleton", meshAssetPath, loadPlan.modelDataPath);
        if (loadPlan.skeletonPath.empty())
            throw std::runtime_error(
                "Mesh skinning requires skinning.skeleton to identify the skeleton: " +
                std::string(meshAssetPath));
        loadPlan.modelCacheKey += "|skeleton=" + loadPlan.skeletonPath;

        if (skinning.contains("sourceModel"))
        {
            loadPlan.animationSourceModelPath = ReadOptionalAnimationJsonPath(
                skinning, "sourceModel", meshAssetPath, loadPlan.modelDataPath);
            if (loadPlan.animationSourceModelPath == loadPlan.meshAssetPath)
                throw std::runtime_error(
                    "Mesh skinning.sourceModel must not reference the target mesh asset: " +
                    std::string(meshAssetPath));
            loadPlan.modelCacheKey += "|animationSourceModel=" +
                loadPlan.animationSourceModelPath;
            if (!skinning.contains("retarget"))
                throw std::runtime_error(
                    "Mesh skinning.sourceModel requires skinning.retarget: " +
                    std::string(meshAssetPath));
        }
        if (skinning.contains("retarget"))
        {
            loadPlan.retargetProfilePath = ReadOptionalAnimationJsonPath(
                skinning, "retarget", meshAssetPath, loadPlan.modelDataPath);
            if (loadPlan.animationSourceModelPath.empty())
                throw std::runtime_error(
                    "Mesh skinning.retarget requires skinning.sourceModel: " +
                    std::string(meshAssetPath));
            loadPlan.modelCacheKey += "|retarget=" + loadPlan.retargetProfilePath;
        }

        for (const auto& clip : skinning.at("clips"))
        {
            if (!clip.is_object() || !clip.contains("id") || !clip.at("id").is_string() ||
                clip.at("id").get<std::string>().empty() ||
                !clip.contains("path") || !clip.at("path").is_string() ||
                clip.at("path").get<std::string>().empty())
                throw std::runtime_error("Skeletal clip requires nonempty id/path: " +
                    std::string(meshAssetPath));
            MeshAnimationClipPlan clipPlan;
            clipPlan.id = clip.at("id").get<std::string>();
            clipPlan.path = clip.at("path").get<std::string>();
            for (const auto& existing : loadPlan.animationClips)
                if (existing.id == clipPlan.id)
                    throw std::runtime_error("Duplicate skeletal clip id: " + clipPlan.id);
            loadPlan.modelCacheKey += "|clip=" + clipPlan.id + ":" + clipPlan.path;
            // sequence 是可选 sidecar；缺失表示该 clip 没有曲线/notify，行为与旧资源一致。
            if (clip.contains("sequence"))
            {
                clipPlan.sequencePath = ReadOptionalAnimationJsonPath(
                    clip, "sequence", meshAssetPath, loadPlan.modelDataPath);
                loadPlan.modelCacheKey +=
                    "|sequence=" + clipPlan.id + ":" + clipPlan.sequencePath;
            }
            loadPlan.animationClips.push_back(std::move(clipPlan));
        }

        // montages 是可选清单；路径级 key 与 clip/sequence 同规则，内容摘要由 loader 追加。
        // 上方已强制要求 skinning.skeleton，因此 Montage 的 slot/骨名校验总有 SK 可用。
        if (skinning.contains("montages"))
        {
            if (!skinning.at("montages").is_array())
                throw std::runtime_error(
                    "Mesh skinning.montages must be an array: " +
                    std::string(meshAssetPath));
            for (const auto& montage : skinning.at("montages"))
            {
                if (!montage.is_object() || !montage.contains("id") ||
                    !montage.at("id").is_string() ||
                    montage.at("id").get<std::string>().empty() ||
                    !montage.contains("path") || !montage.at("path").is_string() ||
                    montage.at("path").get<std::string>().empty())
                    throw std::runtime_error(
                        "Skeletal montage requires nonempty id/path: " +
                        std::string(meshAssetPath));
                MeshAnimationMontagePlan montagePlan;
                montagePlan.id = montage.at("id").get<std::string>();
                for (const auto& existing : loadPlan.animationMontages)
                    if (existing.id == montagePlan.id)
                        throw std::runtime_error(
                            "Duplicate skeletal montage id: " + montagePlan.id);
                montagePlan.path = ReadOptionalAnimationJsonPath(
                    montage, "path", meshAssetPath, loadPlan.modelDataPath);
                loadPlan.modelCacheKey += "|montage=" + montagePlan.path;
                loadPlan.animationMontages.push_back(std::move(montagePlan));
            }
        }
        if (!loadPlan.animationSourceModelPath.empty() &&
            (!loadPlan.animationClips.empty() || !loadPlan.animationMontages.empty()))
            throw std::runtime_error(
                "Retargeted target mesh must not declare target-owned clips or montages; "
                "author them on skinning.sourceModel: " + std::string(meshAssetPath));
    }
    if (loadPlan.importOptions.generateSmoothNormals)
    {
        loadPlan.modelCacheKey += "|generateSmoothNormals";
    }
    for (const std::string& sectionName : loadPlan.importOptions.includedSectionNames)
    {
        loadPlan.modelCacheKey += "|section=" + sectionName;
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
        sectionPlan.materialSlotIndex = static_cast<uint32_t>(sourceSlotIndex);
        sectionPlan.materialSlotName = materialSlot.name;
        sectionPlan.materialInstancePath = materialSlot.materialInstancePath;
        sectionPlans.push_back(sectionPlan);
    }
    return sectionPlans;
}
