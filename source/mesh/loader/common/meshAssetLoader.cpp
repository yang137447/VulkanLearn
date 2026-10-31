#include "meshAssetLoader.h"

#include <exception>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "../../../commonFunction.h"
#include "../mesh/assimpModelImporter.h"
#include "../hair/tressfxModelImporter.h"
#include "../speedtree/speedTreeModelImporter.h"
#include "../../validation/meshAssetValidator.h"
#include "meshAssetResolver.h"
#include "../animation/assimpSkeletalImporter.h"
#include "animation/animationSkeleton.h"
#include "animation/animationMontage.h"
#include "animation/animationRetarget.h"
#include "animation/animationSequenceData.h"
#include "animation/skeletalPlayback.h"
#include "shader/build/contentHash.h"
#include "hair/tressfxBehavior.h"
#include "hair/tressfxGroupSimulation.h"
#include "hair/collisionMeshObj.h"
#include "hair/tfxBoneLoader.h"

namespace
{
// SK/sidecar 是显式资源路径；解析后的内存预算防止异常文件拖垮导入。
constexpr uintmax_t MaxSkeletonJsonBytes = 4ull * 1024 * 1024;
constexpr uintmax_t MaxSequenceJsonBytes = 8ull * 1024 * 1024;
// Montage 只含 section/notify/窗口编排；4 MB 已远高于真实资产（约 4–8 KB）。
constexpr uintmax_t MaxMontageJsonBytes = 4ull * 1024 * 1024;
// SK 与 FBX 的时长一致性；与 schema 的 1e-5 秒容差一致。
constexpr double SequenceDurationTolerance = 1e-5;
thread_local std::vector<std::string> skeletalImportStack;

class SkeletalImportScope
{
public:
    explicit SkeletalImportScope(std::string path) : path(std::move(path))
    {
        if (std::find(skeletalImportStack.begin(), skeletalImportStack.end(), this->path) !=
            skeletalImportStack.end())
            throw std::runtime_error("Recursive skeletal animation sourceModel reference: " + this->path);
        skeletalImportStack.push_back(this->path);
    }
    ~SkeletalImportScope()
    {
        if (!skeletalImportStack.empty())
            skeletalImportStack.pop_back();
    }
private:
    std::string path;
};

nlohmann::json LoadJsonFile(const std::string& absolutePath, const std::string& logicalPath,
    std::string_view assetLabel, std::string& identity)
{
    std::ifstream file(absolutePath, std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error(std::string(assetLabel) + " file not found: " + logicalPath);
    }

    try
    {
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        identity = VL::ContentHasher::HashString(bytes).ToHex();
        return nlohmann::json::parse(bytes);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            std::string(assetLabel) + " JSON parse failed: " + logicalPath +
            " error=" + exception.what());
    }
}

nlohmann::json LoadBoundedJsonFile(const std::string& logicalPath, std::string_view assetLabel,
    uintmax_t maxBytes, std::string& identity)
{
    const std::string absolutePath = CommonFunction::Path(logicalPath);
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(absolutePath, error);
    if (error)
    {
        throw std::runtime_error(std::string(assetLabel) + " file is not readable: " + logicalPath);
    }
    if (size > maxBytes)
    {
        throw std::runtime_error(std::string(assetLabel) + " exceeds " +
            std::to_string(maxBytes) + " bytes: " + logicalPath);
    }
    return LoadJsonFile(absolutePath, logicalPath, assetLabel, identity);
}

void RequireImportFile(const std::string& logicalPath, std::string_view assetLabel)
{
    std::string absolutePath;
    try
    {
        absolutePath = CommonFunction::Path(logicalPath);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error(std::string(assetLabel) + " does not exist: " + logicalPath);
    }
    if (!std::filesystem::is_regular_file(absolutePath))
    {
        throw std::runtime_error(std::string(assetLabel) + " does not exist: " + logicalPath);
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
        if (buildPlan.assetType == "tressfx")
            return std::make_unique<TressfxModelImporter>();
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
    std::string descriptorIdentity;
    nlohmann::json meshObjectJson = LoadJsonFile(absoluteMeshPath, meshPath, "Mesh asset", descriptorIdentity);
    MeshAssetResolveResult resolveResult = MeshAssetResolver::Resolve(meshPath, meshObjectJson);

    MeshAssetLoadRequest loadRequest;
    loadRequest.buildPlan = MeshAssetValidator::BuildLoadPlan(meshPath, resolveResult.effectiveMeshAssetJson);
    loadRequest.importRequest = BuildModelImportRequest(loadRequest.buildPlan);
    loadRequest.descriptorIdentity = std::move(descriptorIdentity);
    return loadRequest;
}

void MeshAssetLoader::PrepareTressfxPlacement(
    MeshAssetLoadRequest& request, const nlohmann::json& sceneObject)
{
    if (request.buildPlan.assetType != "tressfx")
    {
        if (sceneObject.contains("tressfx"))
            throw std::runtime_error("tressfx behavior requires a TressFX asset");
        return;
    }
    const auto behavior = VL::Hair::ReadTressfxPlacementBehavior(
        sceneObject.value("tressfx", nlohmann::json::object()),
        request.buildPlan.meshAssetPath);
    request.tressfxEffective = VL::Hair::ResolveTressfxBehavior(
        request.buildPlan.tressfxSimulation, behavior, request.buildPlan.meshAssetPath);
    request.tressfxGroupConfigs = VL::Hair::ResolveTressfxGroupSimulationConfigs(
        request.buildPlan.tressfxSimulation, request.buildPlan.tressfxGuideGroups,
        behavior, request.buildPlan.meshAssetPath);
    const auto& config = *request.tressfxEffective;
    auto resources = std::make_shared<TressfxPlacementResources>();
    if (config.collisionMesh)
    {
        const std::string path = CommonFunction::Path(config.collisionMesh->path);
        const auto digest = VL::ContentHasher::HashFile(path);
        auto parsed = VL::Hair::LoadCollisionMeshObj(std::filesystem::u8path(path));
        if (parsed.IsFailure())
            throw std::runtime_error(parsed.Error().message);
        if (VL::ContentHasher::HashFile(path) != digest)
            throw std::runtime_error("TressFX collision mesh changed during preparation");
        resources->collisionMesh = std::make_shared<const VL::Hair::SdfTriangleMesh>(
            std::move(parsed.Value()));
        resources->identity += "|proxyBlake3=" + digest.ToHex();
        resources->sourceInputs.emplace_back(config.collisionMesh->path, digest.ToHex());
    }
    if (config.skinning && !config.skinning->boneWeightsPath.empty())
    {
        const std::filesystem::path logical(config.skinning->boneWeightsPath);
        if (logical.is_absolute() || logical.has_root_directory())
            throw std::runtime_error("TressFX boneWeights must be resource-relative");
        for (const auto& part : logical)
            if (part == "..")
                throw std::runtime_error("TressFX boneWeights must not escape resource root");
        const std::string path = CommonFunction::Path(config.skinning->boneWeightsPath);
        const auto digest = VL::ContentHasher::HashFile(path);
        auto parsed = VL::Hair::LoadTfxBone(std::filesystem::u8path(path));
        if (parsed.IsFailure())
            throw std::runtime_error(parsed.Error().message);
        if (VL::ContentHasher::HashFile(path) != digest)
            throw std::runtime_error("TressFX boneWeights changed during preparation");
        resources->boneAsset = std::make_shared<const VL::Hair::TfxBoneAsset>(
            std::move(parsed.Value()));
        resources->identity += "|weightsBlake3=" + digest.ToHex();
        resources->sourceInputs.emplace_back(config.skinning->boneWeightsPath, digest.ToHex());
    }
    request.tressfxResources = std::move(resources);
}

void MeshAssetLoader::ValidateTressfxPlacementSources(const MeshAssetLoadRequest& request)
{
    if (!request.tressfxResources)
        return;
    for (const auto& input : request.tressfxResources->sourceInputs)
        if (VL::ContentHasher::HashFile(CommonFunction::Path(input.first)).ToHex() != input.second)
            throw std::runtime_error("TressFX placement input changed before publication: " + input.first);
}

void MeshAssetLoader::ValidateImportRequest(const MeshAssetLoadRequest& loadRequest)
{
    const MeshAssetBuildPlan& plan = loadRequest.buildPlan;
    if (plan.IsSkeletal())
    {
        RequireImportFile(plan.modelDataPath, "Skeletal mesh source");
        // IsSkeletal() 等价于 skeletonPath 非空：骨骼网格的骨架来源只有 SK。
        RequireImportFile(plan.skeletonPath, "Skeleton asset");
        if (!plan.animationSourceModelPath.empty())
        {
            RequireImportFile(plan.animationSourceModelPath, "Animation source model");
            RequireImportFile(plan.retargetProfilePath, "Retarget profile");
        }
        for (const MeshAnimationClipPlan& clip : plan.animationClips)
        {
            RequireImportFile(clip.path, "Animation source");
            if (!clip.sequencePath.empty())
                RequireImportFile(clip.sequencePath, "Animation sequence");
        }
        for (const MeshAnimationMontagePlan& montage : plan.animationMontages)
        {
            RequireImportFile(montage.path, "Animation montage");
        }
        return;
    }
    CreateAndValidateModelImporter(loadRequest);
}

void MeshAssetLoader::ValidateSectionLoadable(const MeshAssetLoadRequest& loadRequest)
{
    if (loadRequest.buildPlan.IsSkeletal())
    {
        ImportSections(loadRequest);
        return;
    }
    std::unique_ptr<IModelImporter> modelImporter = CreateAndValidateModelImporter(loadRequest);
    const ModelResource modelResource = modelImporter->Import(loadRequest.importRequest);
    MeshAssetValidator::BuildSectionLoadPlans(loadRequest.buildPlan, modelResource);
}

MeshAssetImportResult MeshAssetLoader::ImportSections(const MeshAssetLoadRequest& loadRequest)
{
    if (loadRequest.buildPlan.IsSkeletal())
    {
        ValidateImportRequest(loadRequest);
        SkeletalImportScope importScope(loadRequest.buildPlan.meshAssetPath);
        using namespace VL::Animation;
        auto asset = std::make_shared<SkeletalAsset>();
        // 导入期会补写摘要与缓存键，因此保留一份计划副本随结果发布。
        MeshAssetBuildPlan plan = loadRequest.buildPlan;
        if (!loadRequest.descriptorIdentity.empty())
            asset->sourceInputs.emplace_back(plan.meshAssetPath, loadRequest.descriptorIdentity);

        // SK 必须先于任何 clip 加载：它提供根骨、槽位、曲线注册表与权威校验清单。
        // 骨骼网格必须带 SK（校验层强制），因此这里不做无 SK 的回退分支。
        std::string skeletonDigest;
        const nlohmann::json skeletonJson = LoadBoundedJsonFile(
            plan.skeletonPath, "Skeleton asset", MaxSkeletonJsonBytes, skeletonDigest);
        SkeletonAsset skeleton = ParseSkeleton(skeletonJson, plan.skeletonPath);
        skeleton.sourceDigest = skeletonDigest;
        plan.skeletonDigest = skeletonDigest;
        plan.modelCacheKey += "|skeletonBlake3=" + skeletonDigest;
        asset->skeleton = std::move(skeleton);
        asset->sourceInputs.emplace_back(plan.skeletonPath, skeletonDigest);

        asset->sourceInputs.emplace_back(plan.modelDataPath,
            VL::ContentHasher::HashFile(loadRequest.importRequest.absoluteModelPath).ToHex());
        asset->mesh = AssimpSkeletalImporter::ReadMesh(
            loadRequest.importRequest.absoluteModelPath, asset->skeleton.root);
        try
        {
            ValidateSkeletonAgainstMesh(asset->skeleton, asset->mesh, plan.modelDataPath);
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error(std::string(error.what()) +
                " skeleton=" + plan.skeletonPath + " model=" + plan.modelDataPath);
        }

        if (!plan.animationSourceModelPath.empty())
        {
            MeshAssetLoadRequest sourceRequest = MeshAssetLoader::Prepare(
                plan.animationSourceModelPath);
            if (!sourceRequest.buildPlan.IsSkeletal())
                throw std::runtime_error(
                    "Retarget animation sourceModel must be a skeletal mesh: " +
                    plan.animationSourceModelPath);
            if (!sourceRequest.buildPlan.animationSourceModelPath.empty())
                throw std::runtime_error(
                    "Retarget animation sourceModel must not itself be retargeted: " +
                    plan.animationSourceModelPath);
            ValidateImportRequest(sourceRequest);
            MeshAssetImportResult sourceResult = ImportSections(sourceRequest);
            if (!sourceResult.modelResource.skeletalAsset)
                throw std::runtime_error(
                    "Retarget animation sourceModel did not produce a skeletal asset: " +
                    plan.animationSourceModelPath);
            asset->animationSource = sourceResult.modelResource.skeletalAsset;

            std::string profileDigest;
            const nlohmann::json profileJson = LoadBoundedJsonFile(
                plan.retargetProfilePath, "Retarget profile", MaxSkeletonJsonBytes, profileDigest);
            RetargetProfile profile = ParseRetargetProfile(profileJson, plan.retargetProfilePath);
            if (profile.sourceSkeleton != asset->animationSource->skeleton.id ||
                profile.targetSkeleton != asset->skeleton.id)
                throw std::runtime_error(
                    "Retarget profile skeleton identity does not match source/target assets: " +
                    plan.retargetProfilePath);
            profile.Validate(asset->animationSource->mesh, asset->mesh);
            asset->retargetProfile = std::make_shared<const RetargetProfile>(std::move(profile));
            plan.retargetProfileDigest = profileDigest;
            plan.modelCacheKey += "|retargetBlake3=" + profileDigest;
            asset->sourceInputs.emplace_back(plan.retargetProfilePath, profileDigest);
            for (const auto& input : asset->animationSource->sourceInputs)
            {
                asset->sourceInputs.push_back(input);
                // 动作源每项字节身份进入目标键；仅路径相同不能复用旧动作/姿态资源。
                plan.modelCacheKey += "|animationInput=" + input.first + ":" + input.second;
            }
        }

        for (MeshAnimationClipPlan& clipPlan : plan.animationClips)
        {
            asset->sourceInputs.emplace_back(clipPlan.path,
                VL::ContentHasher::HashFile(CommonFunction::Path(clipPlan.path)).ToHex());
            Clip clip = AssimpSkeletalImporter::ReadClip(
                CommonFunction::Path(clipPlan.path), asset->mesh, clipPlan.id);

            if (!clipPlan.sequencePath.empty())
            {
                std::string sequenceDigest;
                const nlohmann::json sequenceJson = LoadBoundedJsonFile(
                    clipPlan.sequencePath, "Animation sequence", MaxSequenceJsonBytes,
                    sequenceDigest);
                SequenceData sequence = ParseSequence(sequenceJson, clipPlan.sequencePath);
                if (sequence.id != clipPlan.id)
                    throw std::runtime_error(
                        "Animation sequence '" + clipPlan.sequencePath + "' id '" +
                        sequence.id + "' does not match skeletal clip id '" + clipPlan.id + "'");
                ValidateSequenceAgainstSkeleton(
                    sequence, asset->skeleton, clipPlan.sequencePath);
                if (std::abs(static_cast<double>(sequence.duration) -
                    static_cast<double>(clip.raw.duration)) > SequenceDurationTolerance)
                    throw std::runtime_error(
                        "Animation sequence '" + clipPlan.sequencePath + "' duration " +
                        std::to_string(sequence.duration) +
                        " does not match FBX clip '" + clipPlan.id + "' duration " +
                        std::to_string(clip.raw.duration));
                sequence.sourceDigest = sequenceDigest;
                clip.sequence = std::move(sequence);
                clipPlan.sequenceDigest = sequenceDigest;
                plan.modelCacheKey +=
                    "|sequenceBlake3=" + clipPlan.id + ":" + sequenceDigest;
                asset->sourceInputs.emplace_back(clipPlan.sequencePath, sequenceDigest);
            }
            asset->clips.push_back(std::move(clip));
        }

        // Montage 在全部 clip/sidecar 就绪后加载：它只引用 sidecar 路径，
        // 不复制曲线，也不在导入期采样窗口时间。
        if (!plan.animationMontages.empty())
        {
            MontageImportContext context;
            context.skeleton = &asset->skeleton;
            for (size_t index = 0; index < plan.animationClips.size(); ++index)
            {
                const MeshAnimationClipPlan& clipPlan = plan.animationClips[index];
                if (!clipPlan.sequencePath.empty())
                    context.sequences.push_back({clipPlan.sequencePath,
                        &asset->clips[index].sequence, clipPlan.sequenceDigest});
            }
            for (const Clip& clip : asset->clips)
                context.clipIds.push_back(clip.id);

            for (MeshAnimationMontagePlan& montagePlan : plan.animationMontages)
            {
                std::string montageDigest;
                const nlohmann::json montageJson = LoadBoundedJsonFile(
                    montagePlan.path, "Animation montage", MaxMontageJsonBytes,
                    montageDigest);
                Montage montage = ParseMontage(montageJson, montagePlan.path);
                if (montage.id != montagePlan.id)
                    throw std::runtime_error(
                        "Animation montage '" + montagePlan.path + "' id '" +
                        montage.id + "' does not match skinning.montages id '" +
                        montagePlan.id + "'");
                montage.derivedClipId = BindMontage(montage, context, montagePlan.path);
                montage.sourcePath = montagePlan.path;
                montage.sourceDigest = montageDigest;
                montagePlan.digest = montageDigest;
                plan.modelCacheKey +=
                    "|montageBlake3=" + montagePlan.id + ":" + montageDigest;
                asset->sourceInputs.emplace_back(montagePlan.path, montageDigest);
                asset->montages.push_back(std::move(montage));
            }
        }

        // Assimp 读取期间源可能被替换；只有整份清单仍一致才发布导入后的不可变资产。
        for (const auto& input : asset->sourceInputs)
            if (VL::ContentHasher::HashFile(CommonFunction::Path(input.first)).ToHex() != input.second)
                throw std::runtime_error("Skeletal source changed during import: " + input.first);
        MeshAssetImportResult result;
        result.buildPlan = std::move(plan);
        result.modelResource.skeletalAsset = asset;
        for (const auto& section : asset->mesh.sections)
        {
            MeshSection geometry;
            geometry.vertices = section.vertices;
            geometry.indices = section.indices;
            geometry.sectionName = section.name;
            geometry.materialSlotName = section.materialSlot;
            result.modelResource.sections.push_back(std::move(geometry));
            auto& slots = result.modelResource.sourceMaterialSlotNames;
            if (std::find(slots.begin(), slots.end(), section.materialSlot) == slots.end())
                slots.push_back(section.materialSlot);
        }
        result.sectionPlans = MeshAssetValidator::BuildSectionLoadPlans(result.buildPlan, result.modelResource);
        return result;
    }
    std::unique_ptr<IModelImporter> modelImporter = CreateAndValidateModelImporter(loadRequest);
    MeshAssetImportResult importResult;
    importResult.buildPlan = loadRequest.buildPlan;
    const std::string tfxDigest = loadRequest.buildPlan.assetType == "tressfx"
        ? VL::ContentHasher::HashFile(loadRequest.importRequest.absoluteModelPath).ToHex() : std::string();
    importResult.modelResource = modelImporter->Import(loadRequest.importRequest);
    if (!tfxDigest.empty())
    {
        if (VL::ContentHasher::HashFile(loadRequest.importRequest.absoluteModelPath).ToHex() != tfxDigest)
            throw std::runtime_error("TressFX geometry changed during import");
        importResult.buildPlan.modelCacheKey += "|tfxBlake3=" + tfxDigest;
    }
    importResult.sectionPlans = MeshAssetValidator::BuildSectionLoadPlans(
        importResult.buildPlan,
        importResult.modelResource);
    return importResult;
}
