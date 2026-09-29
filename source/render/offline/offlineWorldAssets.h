#pragma once

#include <cstdint>
#include <memory>
#include <filesystem>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "mesh/loader/common/meshAssetLoader.h"
#include "core/renderer.h"

namespace VL
{

// 候选阶段可写；正式发布后只通过 shared_ptr<const> 读取。模型导入结果
// 与 GPU renderable 来自同一次 MeshAssetLoader 调用，不回读设备内存。
struct OfflineWorldAssets
{
    struct Section
    {
        std::shared_ptr<const MeshAssetImportResult> imported;
        size_t sectionIndex = 0;
    };
    struct GroomAsset
    {
        std::filesystem::path descriptorPath;
        std::filesystem::path sourcePath;
        std::string descriptorDigest;
        std::string sourceDigest;
        std::shared_ptr<const std::string> bytes;
    };
    struct ColorAsset
    {
        std::filesystem::path descriptorPath;
        std::filesystem::path sourcePath;
        std::string descriptorDigest;
        std::string sourceDigest;
        std::shared_ptr<const std::string> bytes;
        std::string extension;
        bool srgb = false;
        bool repeat = true;
        bool linear = true;
    };
    uint64_t worldGeneration = 0;
    std::string scenePath;
    nlohmann::json sceneJson;
    std::unordered_map<std::string, std::shared_ptr<const MeshAssetImportResult>>
        meshImports;
    std::unordered_map<std::string, Section> sections;
    std::unordered_map<std::string, nlohmann::json> effectiveMaterialInstances;
    std::unordered_map<std::string, nlohmann::json> materialDefinitions;
    std::unordered_map<std::string, nlohmann::json> materialInstanceSources;
    bool realtimePrepared = false;
    std::unordered_map<std::string, std::shared_ptr<const Offline::Mesh>> coreMeshes;
    std::unordered_map<std::string, nlohmann::json> modelProfiles;
    std::unordered_map<std::string, std::filesystem::path> modelProfilePaths;
    std::unordered_map<std::string, std::string> modelProfileDigests;
    std::unordered_map<std::string, GroomAsset> groomAssets;
    std::unordered_map<std::string, ColorAsset> colorAssets;
};

} // namespace VL
