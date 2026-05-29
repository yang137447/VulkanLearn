#include "meshAssetLoader.h"

#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "../../../commonFunction.h"
#include "../mesh/assimpModelImporter.h"
#include "../speedtree/speedTreeModelImporter.h"
#include "../../validation/meshAssetValidator.h"
#include "meshAssetResolver.h"

namespace
{
    nlohmann::json LoadJsonFile(const std::string& absolutePath, const std::string& logicalPath, std::string_view assetLabel)
    {
        std::ifstream file(absolutePath);
        if (!file.is_open())
        {
            throw std::runtime_error(std::string(assetLabel) + " file not found: " + logicalPath);
        }

        try
        {
            return nlohmann::json::parse(file);
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                std::string(assetLabel) + " JSON parse failed: " + logicalPath +
                " error=" + exception.what());
        }
    }

    ModelImportRequest BuildModelImportRequest(const MeshAssetBuildPlan& loadPlan)
    {
        ModelImportRequest request;
        request.buildPlan = loadPlan;
        request.absoluteModelPath = CommonFunction::Path(loadPlan.modelDataPath);
        return request;
    }

    // 这里集中保留三层设计的落点，避免过度拆分:
    // 1) Interface: IModelImporter 统一导入契约。
    // 2) Strategy: AssimpModelImporter / SpeedTreeModelImporter 按 assetType 切换行为。
    // 3) Adapter: 每个策略内部再委托 SourceAdapter，隔离第三方格式细节。
    std::unique_ptr<IModelImporter> CreateModelImporterForAssetType(const MeshAssetBuildPlan& buildPlan)
    {
        if (buildPlan.assetType == "mesh")
        {
            return std::make_unique<AssimpModelImporter>();
        }
        if (buildPlan.assetType == "speedtree")
        {
            return std::make_unique<SpeedTreeModelImporter>();
        }
        throw std::runtime_error("No model importer registered for mesh asset type: " + buildPlan.assetType);
    }

    std::unique_ptr<IModelImporter> CreateAndValidateModelImporter(const MeshAssetLoadRequest& loadRequest)
    {
        std::unique_ptr<IModelImporter> modelImporter =
            CreateModelImporterForAssetType(loadRequest.buildPlan);
        modelImporter->ValidateImportRequest(loadRequest.importRequest);
        return modelImporter;
    }
}

MeshAssetLoadRequest MeshAssetLoader::Prepare(std::string_view meshAssetPath)
{
    const std::string meshPath(meshAssetPath);
    const std::string absoluteMeshPath = CommonFunction::Path(meshPath);
    nlohmann::json meshObjectJson = LoadJsonFile(absoluteMeshPath, meshPath, "Mesh asset");
    MeshAssetResolveResult resolveResult = MeshAssetResolver::Resolve(meshPath, meshObjectJson);

    MeshAssetLoadRequest loadRequest;
    loadRequest.buildPlan = MeshAssetValidator::BuildLoadPlan(meshPath, resolveResult.effectiveMeshAssetJson);
    loadRequest.importRequest = BuildModelImportRequest(loadRequest.buildPlan);
    return loadRequest;
}

void MeshAssetLoader::ValidateImportRequest(const MeshAssetLoadRequest& loadRequest)
{
    CreateAndValidateModelImporter(loadRequest);
}

void MeshAssetLoader::ValidateSectionLoadable(const MeshAssetLoadRequest& loadRequest)
{
    std::unique_ptr<IModelImporter> modelImporter = CreateAndValidateModelImporter(loadRequest);
    const ModelResource modelResource = modelImporter->Import(loadRequest.importRequest);
    MeshAssetValidator::BuildSectionLoadPlans(loadRequest.buildPlan, modelResource);
}

MeshAssetImportResult MeshAssetLoader::ImportSections(const MeshAssetLoadRequest& loadRequest)
{
    std::unique_ptr<IModelImporter> modelImporter = CreateAndValidateModelImporter(loadRequest);
    MeshAssetImportResult importResult;
    importResult.buildPlan = loadRequest.buildPlan;
    importResult.modelResource = modelImporter->Import(loadRequest.importRequest);
    importResult.sectionPlans = MeshAssetValidator::BuildSectionLoadPlans(
        importResult.buildPlan,
        importResult.modelResource);
    return importResult;
}
