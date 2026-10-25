#pragma once

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "animation/skeletalPlayback.h"
#include "hair/tressfxProxySkinning.h"
#include "hair/tressfxRootWeights.h"
#include "hair/tressfxSkinning.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "renderableObject.h"
#include "scene/placementBinding.h"

namespace VL
{

// 已解析的放置绑定载荷缓存（目前只有 TressFX 皮肤绑定）。键由
// MakePlacementBindingCacheKey 生成；值里的 sourceSceneObject 留空，应用实例时
// 再替换成该放置的父组件身份。新增类型时另加一张载荷表，不要混用。
using PlacementBindingCache = std::unordered_map<
    std::string, std::shared_ptr<const VL::Hair::TressfxSkinningBinding>>;

// 通用缓存键：模板 model key + binding kind + 作者参数 + 来源资产身份。
// 只放“解析结果相同”的维度；实例身份（sourceSceneObject）不参与，否则永不复用。
inline std::string MakePlacementBindingCacheKey(
    const std::string& templateModelKey, const PlacementBinding& binding,
    const std::string& sourceAssetIdentity)
{
    return templateModelKey + "|kind=" +
        std::to_string(static_cast<int>(binding.kind)) +
        "|rootBone=" + binding.rootBone + "|weights=" + binding.weights +
        "|sourceAsset=" + sourceAssetIdentity;
}

// TressFX 皮肤绑定 handler 的“现场解析”分支：模板 renderable 还没有解析过绑定
// 时（例如异步预加载的蓝图资产），用当前帧来源 draw 的骨骼资产 + 模板毛发的
// 根点/胶囊作者数据解析一次，并按通用缓存键复用。返回的 binding 已填好
// sourceSceneObject；调用方不缓存这个副本。
inline bool ResolveTressfxBindingForPlacement(
    const PlacementBinding& binding, const RenderableObject& templateObject,
    const RenderableObject& sourceObject, PlacementBindingCache& cache,
    VL::Hair::TressfxSkinningBinding& resolved, std::string& error)
{
    const std::shared_ptr<const MeshAssetImportResult>& imported =
        templateObject.GetImported();
    if (imported == nullptr)
    {
        error = "TressFX placement binding template has no imported data";
        return false;
    }
    const MeshAssetBuildPlan& hairPlan = imported->buildPlan;
    if (!hairPlan.tressfxSimulation.skinning)
    {
        error = "TressFX placement binding template has no simulation.skinning";
        return false;
    }
    const TressfxSimulationSkinning& skinning = *hairPlan.tressfxSimulation.skinning;
    if (!skinning.boneWeightsPath.empty())
    {
        // .tfxbone 的骨名映射发生在加载期；运行时 spawn 还没有等价的映射入口。
        error = "runtime spawn with simulation.skinning.boneWeights is not supported yet";
        return false;
    }
    const std::shared_ptr<const MeshAssetImportResult>& sourceImported =
        sourceObject.GetImported();
    if (sourceImported == nullptr || !sourceImported->modelResource.skeletalAsset)
    {
        error = "TressFX placement binding source object has no skeletal asset";
        return false;
    }
    const std::string cacheKey = MakePlacementBindingCacheKey(
        hairPlan.modelCacheKey, binding, sourceImported->buildPlan.modelCacheKey);
    if (const auto cached = cache.find(cacheKey); cached != cache.end())
    {
        resolved = *cached->second;
        resolved.sourceSceneObject = binding.sourceSceneObject;
        return true;
    }

    const Animation::SkeletalAsset& skeletal =
        *sourceImported->modelResource.skeletalAsset;
    std::vector<std::string> jointNames;
    jointNames.reserve(skeletal.skeleton.joints.size());
    for (const Animation::SkeletonJoint& joint : skeletal.skeleton.joints)
        jointNames.push_back(joint.name);

    resolved = VL::Hair::TressfxSkinningBinding{};
    resolved.enabled = true;
    resolved.sourceSceneObject = binding.sourceSceneObject;
    resolved.rootBone = binding.rootBone.empty() ? skinning.rootBone : binding.rootBone;
    resolved.jointIndex = VL::Hair::ResolveTressfxRootJoint(
        jointNames, binding.sourceSceneObject, resolved.rootBone, hairPlan.meshAssetPath);
    resolved.capsuleJointIndices.assign(
        hairPlan.tressfxSimulation.capsules.size(), -1);
    for (size_t index = 0; index < hairPlan.tressfxSimulation.capsules.size(); ++index)
    {
        const VL::Hair::TressfxCollider& capsule =
            hairPlan.tressfxSimulation.capsules[index];
        if (capsule.bone.empty())
            continue;
        resolved.capsuleJointIndices[index] = static_cast<int32_t>(
            VL::Hair::ResolveTressfxJointByName(jointNames, binding.sourceSceneObject,
                capsule.bone,
                "collision.capsules[" + std::to_string(index) + "].bone",
                hairPlan.meshAssetPath));
    }

    const int weightChoice = VL::Hair::ResolveTressfxRootWeightChoice(true,
        skinning.rootWeights == TressfxRootWeightSource::Mesh,
        skinning.rootWeightsAuthored, binding.weights, false);
    if (weightChoice < 0)
    {
        error = "TressFX placement binding root weights conflict: \"mesh\" derivation "
            "cannot be combined with simulation.skinning.boneWeights";
        return false;
    }
    if (weightChoice == 1)
    {
        const MeshSection& section =
            imported->modelResource.sections.at(templateObject.GetImportedSectionIndex());
        if (section.tressfxRootPositions.empty())
        {
            error = "TressFX placement binding template has no imported guide roots";
            return false;
        }
        // 与加载期同一套身体视图：section 顶点先乘 meshToModel，再拼 CSR 影响。
        std::vector<Eigen::Vector3f> positions;
        std::vector<uint32_t> offsets;
        std::vector<VL::Hair::SdfProxySkinSourceInfluence> influences;
        offsets.push_back(0);
        for (const Animation::SkinSection& sourceSection : skeletal.mesh.sections)
        {
            if (sourceSection.influenceOffsets.size() !=
                sourceSection.vertices.size() + 1)
            {
                error = "TressFX placement binding source section has an invalid "
                    "influence CSR: " + sourceSection.name;
                return false;
            }
            for (size_t vertex = 0; vertex < sourceSection.vertices.size(); ++vertex)
            {
                positions.push_back((sourceSection.meshToModel *
                    sourceSection.vertices[vertex].position.homogeneous()).head<3>());
                for (uint32_t influence = sourceSection.influenceOffsets[vertex];
                     influence < sourceSection.influenceOffsets[vertex + 1]; ++influence)
                {
                    const Animation::Influence& source =
                        sourceSection.influences.at(influence);
                    influences.push_back({source.joint, source.weight});
                }
                offsets.push_back(static_cast<uint32_t>(influences.size()));
            }
        }
        const VL::Hair::SdfProxySkinSource view{
            positions.data(), positions.size(), offsets.data(), influences.data(),
            static_cast<uint32_t>(skeletal.skeleton.joints.size())};
        const VL::Hair::TressfxRootWeightInput input{
            &section.tressfxRootPositions, &view, 2.0f};
        auto derived = VL::Hair::BuildTressfxRootWeights(input);
        if (derived.IsFailure())
        {
            error = "TressFX placement binding root weights failed: " +
                VL::FormatRuntimeError(derived.Error());
            return false;
        }
        resolved.strandBones = std::move(derived.Value().strandBones);
        std::cout << "[Placement] tressfx root weights: strands="
                  << resolved.strandBones.size()
                  << " maxNearest=" << derived.Value().maxNearestDistanceCm
                  << " fallback=" << derived.Value().unmatchedCount << std::endl;
    }

    cache[cacheKey] =
        std::make_shared<const VL::Hair::TressfxSkinningBinding>(resolved);
    return true;
}

// 通用放置绑定解析（渲染线程、resolve 之前）：把模板 renderable 上已经解析好的
// 放置状态复制到新实例；模板未解析时交给各 handler 现场解析。
// 新增一种 PlacementBinding::Kind 时只在这里加一个 case。
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
        if (!templateObject.IsTressfx() || !templateObject.UsesGpuGuideSimulation())
        {
            error = "TressFX placement binding requires a GPU guide template renderable";
            return false;
        }
        VL::Hair::TressfxSkinningBinding resolved;
        if (templateObject.GetTressfxSkinningBinding().enabled)
        {
            resolved = templateObject.GetTressfxSkinningBinding();
            resolved.sourceSceneObject = binding.sourceSceneObject;
        }
        else if (sourceObject != nullptr)
        {
            if (!ResolveTressfxBindingForPlacement(
                    binding, templateObject, *sourceObject, cache, resolved, error))
                return false;
        }
        else
        {
            error = "TressFX placement binding template has no resolved skinning binding "
                "and the source draw is not available in this frame";
            return false;
        }
        // 模板绑定已包含 rootBone/capsule 的关节下标与派生权重；spawn 覆写 rootBone
        // 会让关节下标与模板不一致，明确拒绝而不是给出错绑定的画面。
        if (!binding.rootBone.empty() && binding.rootBone != resolved.rootBone)
        {
            error = "TressFX placement binding rootBone override is not supported yet: " +
                binding.rootBone;
            return false;
        }
        instance.SetTressfxSkinningBinding(resolved);
        const std::shared_ptr<const VL::Hair::SdfProxySkinTransfer>& proxy =
            templateObject.GetTressfxSdfProxySkin();
        if (proxy != nullptr)
            instance.SetTressfxSdfProxySkin(proxy);
        return true;
    }
    }
    error = "Unsupported placement binding kind";
    return false;
}

} // namespace VL
