#pragma once

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "animation/skeletalPlayback.h"
#include "hair/tressfxProxySkinning.h"
#include "hair/tressfxRootWeights.h"
#include "hair/tressfxSkinning.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "render/resource/placementBindingCacheKey.h"
#include "renderableObject.h"
#include "scene/placementBinding.h"

namespace VL
{

struct CachedTressfxPlacementBinding
{
    // 缓存不持有场景实例身份或 GPU 资源，只共享已冻结的 CPU 解析结果。
    Hair::TressfxSkinningBinding skinning;
    std::shared_ptr<const Hair::SdfProxySkinTransfer> proxy;
};

using PlacementBindingCache = std::unordered_map<
    std::string, std::shared_ptr<const CachedTressfxPlacementBinding>>;

// 身体各 section 先变换到统一绑定空间，再拼 CSR；发根与 SDF 代理必须消费
// 同一份视图，否则 meshToModel 不同的 section 会产生两套不一致的权重。
struct PlacementTressfxBodySkin
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<uint32_t> offsets;
    std::vector<Hair::SdfProxySkinSourceInfluence> influences;
};

inline bool BuildPlacementTressfxBodySkin(const Animation::SkeletalAsset& skeletal,
    PlacementTressfxBodySkin& body, std::string& error)
{
    body.offsets.push_back(0);
    for (const Animation::SkinSection& section : skeletal.mesh.sections)
    {
        if (section.influenceOffsets.size() != section.vertices.size() + 1)
        {
            error = "TressFX placement source section has an invalid influence CSR: " +
                section.name;
            return false;
        }
        for (size_t vertex = 0; vertex < section.vertices.size(); ++vertex)
        {
            body.positions.push_back((section.meshToModel *
                section.vertices[vertex].position.homogeneous()).head<3>());
            for (uint32_t influence = section.influenceOffsets[vertex];
                 influence < section.influenceOffsets[vertex + 1]; ++influence)
            {
                const Animation::Influence& source = section.influences.at(influence);
                body.influences.push_back({source.joint, source.weight});
            }
            body.offsets.push_back(static_cast<uint32_t>(body.influences.size()));
        }
    }
    return true;
}

inline bool ResolveTressfxBindingForPlacement(
    const PlacementBinding& binding, const RenderableObject& hairObject,
    const RenderableObject& sourceObject, PlacementBindingCache& cache,
    Hair::TressfxSkinningBinding& resolved,
    std::shared_ptr<const Hair::SdfProxySkinTransfer>& proxy, std::string& error)
{
    const auto& imported = hairObject.GetImported();
    if (imported == nullptr)
    {
        error = "TressFX placement binding has no imported data";
        return false;
    }
    const MeshAssetBuildPlan& hairPlan = imported->buildPlan;
    const TressfxSimulationConfig& config = hairObject.GetTressfxSimulationConfig();
    if (!config.skinning)
    {
        error = "TressFX placement binding has no effective skinning configuration";
        return false;
    }
    const TressfxSimulationSkinning& skinning = *config.skinning;
    const auto& resources = hairObject.GetTressfxResources();
    const auto& sourceImported = sourceObject.GetImported();
    if (sourceImported == nullptr || !sourceImported->modelResource.skeletalAsset)
    {
        error = "TressFX placement binding source object has no skeletal asset";
        return false;
    }
    const Animation::SkeletalAsset& skeletal =
        *sourceImported->modelResource.skeletalAsset;
    // sourceInputs 在正式导入时捕获模型、SK 等字节摘要；这里不在 RT 读文件。
    const std::string sourceIdentity = nlohmann::json::array({
        sourceImported->buildPlan.modelCacheKey, skeletal.skeleton.sourceDigest,
        skeletal.sourceInputs}).dump();
    const std::string cacheKey = MakePlacementBindingCacheKey(
        hairPlan.modelCacheKey, binding, config,
        resources ? resources->identity : std::string{}, sourceIdentity);
    if (const auto cached = cache.find(cacheKey); cached != cache.end())
    {
        resolved = cached->second->skinning;
        resolved.sourceSceneObject = binding.sourceSceneObject;
        proxy = cached->second->proxy;
        return true;
    }

    std::vector<std::string> jointNames;
    jointNames.reserve(skeletal.skeleton.joints.size());
    for (const Animation::SkeletonJoint& joint : skeletal.skeleton.joints)
        jointNames.push_back(joint.name);
    resolved = Hair::TressfxSkinningBinding{};
    proxy.reset();
    resolved.enabled = true;
    resolved.sourceSceneObject = binding.sourceSceneObject;
    resolved.rootBone = binding.rootBone.empty() ? skinning.rootBone : binding.rootBone;
    resolved.jointIndex = Hair::ResolveTressfxRootJoint(
        jointNames, binding.sourceSceneObject, resolved.rootBone, hairPlan.meshAssetPath);
    resolved.capsuleJointIndices.assign(config.capsules.size(), -1);
    for (size_t index = 0; index < config.capsules.size(); ++index)
    {
        const Hair::TressfxCollider& capsule = config.capsules[index];
        if (!capsule.bone.empty())
            resolved.capsuleJointIndices[index] = static_cast<int32_t>(
                Hair::ResolveTressfxJointByName(jointNames, binding.sourceSceneObject,
                    capsule.bone, "hairCollision.capsules[" + std::to_string(index) +
                        "].bone", hairPlan.meshAssetPath));
    }

    const bool hasBoneFile = !skinning.boneWeightsPath.empty();
    const int weightChoice = Hair::ResolveTressfxRootWeightChoice(false,
        skinning.rootWeights == TressfxRootWeightSource::Mesh,
        skinning.rootWeightsAuthored, binding.weights, hasBoneFile);
    if (weightChoice < 0)
    {
        error = "TressFX placement root weights conflict: mesh derivation "
            "cannot be combined with skinning.boneWeights";
        return false;
    }
    if (hasBoneFile)
    {
        if (!resources || !resources->boneAsset)
        {
            error = "TressFX placement skinning.boneWeights has no prepared input";
            return false;
        }
        std::vector<std::string> unmappedBones;
        auto mapped = Hair::MapTfxBoneToSkeleton(*resources->boneAsset, jointNames,
            hairObject.GetTressfxGuideCount(), unmappedBones, skinning.boneWeightsPath);
        if (mapped.IsFailure())
        {
            error = "TressFX placement bone weights failed: " +
                FormatRuntimeError(mapped.Error());
            return false;
        }
        for (const std::string& bone : unmappedBones)
            std::cerr << "[Placement] tressfx boneWeights: unknown bone \""
                      << bone << "\" skipped: " << hairPlan.meshAssetPath << std::endl;
        resolved.strandBones = std::move(mapped.Value());
    }

    // off 仍解析完整骨骼权重，但不转移碰撞代理，也不创建其 GPU 描述符。
    const bool needsProxy = config.enabled && config.collisionMesh.has_value();
    if (weightChoice == 1 || needsProxy)
    {
        PlacementTressfxBodySkin body;
        if (!BuildPlacementTressfxBodySkin(skeletal, body, error))
            return false;
        const Hair::SdfProxySkinSource view{
            body.positions.data(), body.positions.size(), body.offsets.data(),
            body.influences.data(), static_cast<uint32_t>(jointNames.size())};
        if (weightChoice == 1)
        {
            const MeshSection& section = imported->modelResource.sections.at(
                hairObject.GetImportedSectionIndex());
            if (section.tressfxRootPositions.empty())
            {
                error = "TressFX placement binding has no imported guide roots";
                return false;
            }
            const Hair::TressfxRootWeightInput input{
                &section.tressfxRootPositions, &view, 2.0f};
            auto derived = Hair::BuildTressfxRootWeights(input);
            if (derived.IsFailure())
            {
                error = "TressFX placement root weights failed: " +
                    FormatRuntimeError(derived.Error());
                return false;
            }
            resolved.strandBones = std::move(derived.Value().strandBones);
            std::cout << "[Placement] tressfx root weights: strands="
                      << resolved.strandBones.size()
                      << " maxNearest=" << derived.Value().maxNearestDistanceCm
                      << " fallback=" << derived.Value().unmatchedCount << std::endl;
        }
        if (needsProxy)
        {
            if (!resources || !resources->collisionMesh)
            {
                error = "TressFX placement hairCollision.mesh has no prepared input";
                return false;
            }
            proxy = std::make_shared<const Hair::SdfProxySkinTransfer>(
                Hair::BuildTressfxProxySkinTransfer(
                    resources->collisionMesh->positions, view, 2.0f));
        }
    }

    auto payload = std::make_shared<CachedTressfxPlacementBinding>();
    payload->skinning = resolved;
    payload->skinning.sourceSceneObject.clear();
    payload->proxy = proxy;
    cache[cacheKey] = std::move(payload);
    return true;
}

// 每次按当前有效配置和来源解析；不复制模板已解析的根骨、权重或代理，组件覆盖
// 因而不会被模板状态吞掉。跨实例只复用上面按内容身份冻结的 CPU 载荷。
inline bool ApplyPlacementBinding(const PlacementBinding& binding,
    const RenderableObject& templateObject, const RenderableObject* sourceObject,
    RenderableObject& instance, PlacementBindingCache& cache, std::string& error)
{
    error.clear();
    switch (binding.kind)
    {
    case PlacementBinding::Kind::None:
        return true;
    case PlacementBinding::Kind::TressfxSkinning:
    {
        if (binding.sourceSceneObject.empty())
        {
            error = "TressFX placement binding requires a source scene object";
            return false;
        }
        if (!templateObject.IsTressfx() ||
            (!instance.UsesGpuGuideSimulation() && !instance.UsesGpuHairSkinning()))
        {
            error = "TressFX placement binding requires a GPU hair renderable";
            return false;
        }
        if (sourceObject == nullptr)
        {
            error = "TressFX placement binding source draw is not available in this frame";
            return false;
        }
        Hair::TressfxSkinningBinding resolved;
        std::shared_ptr<const Hair::SdfProxySkinTransfer> proxy;
        if (!ResolveTressfxBindingForPlacement(binding, instance, *sourceObject,
                cache, resolved, proxy, error))
            return false;
        instance.SetTressfxSkinningBinding(resolved);
        if (proxy)
            instance.SetTressfxSdfProxySkin(std::move(proxy));
        return true;
    }
    }
    error = "Unsupported placement binding kind";
    return false;
}

} // namespace VL
