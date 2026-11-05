#include "particles/particleSkeletalPreflight.h"

#include <algorithm>

namespace VL
{

namespace
{

ParticleSkeletalPreflight Fail(
    const std::string& message,
    ParticleSkeletalPreflight result)
{
    result.success = false;
    result.error = message;
    return result;
}

} // namespace

ParticleSkeletalPreflight ValidateParticleSkeletalPreflight(
    const ParticleSkeletalPreflightInput& input,
    const ParticleSkeletalPreflightLimits& limits)
{
    ParticleSkeletalPreflight result;
    result.jointCount = static_cast<uint32_t>(input.jointCount);
    result.sectionCount =
        static_cast<uint32_t>(input.sections.size());

    if (input.sections.empty())
    {
        return Fail("skeletal mesh has no sections", result);
    }
    if (result.sectionCount > limits.maxSections)
    {
        return Fail(
            "skeletal mesh has " + std::to_string(result.sectionCount) +
                " sections, more than the particle limit " +
                std::to_string(limits.maxSections),
            result);
    }
    if (result.jointCount == 0)
    {
        return Fail("skeletal mesh has no joints", result);
    }
    if (result.jointCount > limits.maxJoints)
    {
        return Fail(
            "skeletal mesh has " + std::to_string(result.jointCount) +
                " joints, more than the particle palette limit " +
                std::to_string(limits.maxJoints),
            result);
    }

    // 空 section 在烘焙时被跳过；必须至少有一个 section 能出几何。
    size_t nonEmptySections = 0;
    for (size_t sectionIndex = 0;
         sectionIndex < input.sections.size();
         ++sectionIndex)
    {
        const ParticleSkeletalSectionInspection& section =
            input.sections[sectionIndex];
        const std::string prefix =
            "skeletal mesh section " + std::to_string(sectionIndex) + ": ";
        if (!section.structuralError.empty())
        {
            return Fail(prefix + section.structuralError, result);
        }
        if (section.vertexCount == 0 || section.indexCount == 0)
        {
            continue;
        }
        ++nonEmptySections;
        if (section.indexCount % 3u != 0u)
        {
            return Fail(
                prefix + "index count is not a multiple of 3", result);
        }
        if (section.influenceOffsetCount != section.vertexCount + 1u)
        {
            return Fail(
                prefix + "influence offsets do not match vertex count",
                result);
        }
        if (section.inverseBindCount != input.jointCount)
        {
            return Fail(
                prefix + "inverse bind count does not match joint count",
                result);
        }
        result.maxInfluencesPerVertex = std::max(
            result.maxInfluencesPerVertex,
            section.maxInfluencesPerVertex);
        if (section.maxInfluencesPerVertex >
            limits.maxInfluencesPerVertex)
        {
            return Fail(
                prefix + "a vertex has " +
                    std::to_string(section.maxInfluencesPerVertex) +
                    " influences, more than the limit " +
                    std::to_string(limits.maxInfluencesPerVertex),
                result);
        }
        const uint64_t triangles =
            static_cast<uint64_t>(section.indexCount) / 3u;
        if (result.triangleCount + triangles >
            static_cast<uint64_t>(limits.maxTriangles))
        {
            return Fail(
                "skeletal mesh has more than " +
                    std::to_string(limits.maxTriangles) + " triangles",
                result);
        }
        result.triangleCount += static_cast<uint32_t>(triangles);
    }
    if (nonEmptySections == 0)
    {
        return Fail("skeletal mesh has no non-empty sections", result);
    }

    // 骨段数 = 有有效父关节的子关节数，与烘焙时的遍历规则一致。
    if (input.jointParents.size() != input.jointCount)
    {
        return Fail("skeletal joint parent table does not match joint count",
            result);
    }
    uint32_t boneCount = 0;
    for (size_t jointIndex = 0;
         jointIndex < input.jointParents.size();
         ++jointIndex)
    {
        const int parent = input.jointParents[jointIndex];
        if (parent < 0 ||
            static_cast<size_t>(parent) >= input.jointCount)
        {
            continue;
        }
        if (parent == static_cast<int>(jointIndex))
        {
            return Fail(
                "skeletal joint " + std::to_string(jointIndex) +
                    " is its own parent",
                result);
        }
        ++boneCount;
    }
    if (input.wantBones && boneCount == 0)
    {
        return Fail(
            "skeletal mesh has no parented joint to build bones", result);
    }
    if (boneCount > limits.maxBones)
    {
        return Fail(
            "skeletal mesh has more than " +
                std::to_string(limits.maxBones) + " bones",
            result);
    }
    result.boneCount = boneCount;

    if (input.wantTriangles)
    {
        result.requiredMaxParticles = result.triangleCount;
    }
    else if (input.wantBones)
    {
        result.requiredMaxParticles = boneCount;
    }
    result.success = true;
    return result;
}

} // namespace VL
