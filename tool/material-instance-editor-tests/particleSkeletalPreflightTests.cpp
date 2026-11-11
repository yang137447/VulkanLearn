#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "particles/particleSkeletalPreflight.h"

namespace
{

using namespace VL;

// 最小可用的描述表：3 个关节、两条骨段、一个非空 section（1 个三角形）。
ParticleSkeletalPreflightInput MinimalInput()
{
    ParticleSkeletalPreflightInput input;
    input.jointCount = 3;
    input.jointParents = {-1, 0, 1};
    ParticleSkeletalSectionInspection section;
    section.vertexCount = 3;
    section.indexCount = 3;
    section.influenceCount = 3;
    section.inverseBindCount = 3;
    section.influenceOffsetCount = 4;
    section.maxInfluencesPerVertex = 1;
    input.sections.push_back(section);
    return input;
}

} // namespace

TEST(ParticleSkeletalPreflight, AcceptsConsistentSkeletalMesh)
{
    ParticleSkeletalPreflightInput input = MinimalInput();
    input.wantTriangles = true;
    const ParticleSkeletalPreflight result =
        ValidateParticleSkeletalPreflight(input);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.jointCount, 3u);
    EXPECT_EQ(result.sectionCount, 1u);
    EXPECT_EQ(result.triangleCount, 1u);
    EXPECT_EQ(result.boneCount, 2u);
    EXPECT_EQ(result.requiredMaxParticles, 1u);

    input.wantTriangles = false;
    input.wantBones = true;
    const ParticleSkeletalPreflight bones =
        ValidateParticleSkeletalPreflight(input);
    ASSERT_TRUE(bones.ok()) << bones.error;
    EXPECT_EQ(bones.requiredMaxParticles, 2u);
}

TEST(ParticleSkeletalPreflight, RejectsStructuralSectionError)
{
    ParticleSkeletalPreflightInput input = MinimalInput();
    input.sections[0].structuralError =
        "influence references out-of-range joint 99";
    const ParticleSkeletalPreflight result =
        ValidateParticleSkeletalPreflight(input);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("out-of-range joint"), std::string::npos)
        << result.error;
}

TEST(ParticleSkeletalPreflight, RejectsInfluenceOffsetMismatch)
{
    ParticleSkeletalPreflightInput input = MinimalInput();
    input.sections[0].influenceOffsetCount = 3;
    const ParticleSkeletalPreflight result =
        ValidateParticleSkeletalPreflight(input);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("influence offsets"), std::string::npos)
        << result.error;
}

TEST(ParticleSkeletalPreflight, RejectsInverseBindCountMismatch)
{
    ParticleSkeletalPreflightInput input = MinimalInput();
    input.sections[0].inverseBindCount = 2;
    const ParticleSkeletalPreflight result =
        ValidateParticleSkeletalPreflight(input);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("inverse bind"), std::string::npos)
        << result.error;
}

TEST(ParticleSkeletalPreflight, RejectsJointPaletteOverflow)
{
    const ParticleSkeletalPreflightInput input = MinimalInput();
    ParticleSkeletalPreflightLimits limits;
    limits.maxJoints = 2;
    const ParticleSkeletalPreflight result =
        ValidateParticleSkeletalPreflight(input, limits);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("palette limit"), std::string::npos)
        << result.error;
}

TEST(ParticleSkeletalPreflight, RejectsTooManyInfluencesPerVertex)
{
    ParticleSkeletalPreflightInput input = MinimalInput();
    input.sections[0].maxInfluencesPerVertex = 17;
    const ParticleSkeletalPreflight result =
        ValidateParticleSkeletalPreflight(input);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("influences"), std::string::npos)
        << result.error;
}

TEST(ParticleSkeletalPreflight, RejectsBoneModeWithoutParentedJoint)
{
    ParticleSkeletalPreflightInput input = MinimalInput();
    input.jointParents = {-1, -1, -1};
    input.wantBones = true;
    const ParticleSkeletalPreflight bones =
        ValidateParticleSkeletalPreflight(input);
    EXPECT_FALSE(bones.ok());
    EXPECT_NE(bones.error.find("parented joint"), std::string::npos)
        << bones.error;
    // 三角面模式不关心父子关系。
    input.wantBones = false;
    input.wantTriangles = true;
    EXPECT_TRUE(ValidateParticleSkeletalPreflight(input).ok());
}

TEST(ParticleSkeletalPreflight, RejectsSelfParentedJoint)
{
    ParticleSkeletalPreflightInput input = MinimalInput();
    input.jointParents[0] = 0;
    const ParticleSkeletalPreflight result =
        ValidateParticleSkeletalPreflight(input);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("own parent"), std::string::npos)
        << result.error;
}

TEST(ParticleSkeletalPreflight, SkipsEmptySectionsButRequiresOneNonEmpty)
{
    ParticleSkeletalPreflightInput input = MinimalInput();
    ParticleSkeletalSectionInspection empty;
    input.sections.push_back(empty);
    EXPECT_TRUE(ValidateParticleSkeletalPreflight(input).ok());

    ParticleSkeletalPreflightInput allEmpty;
    allEmpty.jointCount = 1;
    allEmpty.jointParents = {-1};
    allEmpty.sections.push_back(empty);
    const ParticleSkeletalPreflight result =
        ValidateParticleSkeletalPreflight(allEmpty);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("non-empty"), std::string::npos)
        << result.error;
}
