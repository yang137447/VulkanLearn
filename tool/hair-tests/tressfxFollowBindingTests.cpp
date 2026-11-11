#include "hair/tressfxFollowBinding.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
using VL::Hair::BuildTressfxFollowBinding;
using VL::Hair::TressfxFollowBinding;
using VL::Hair::TressfxFollowFrame;
using VL::Hair::TressfxMakeFollowFrame;

// 四根弯曲 guide，每根 4 点；根点在 XZ 平面上分开。
struct Fixture
{
    std::vector<Eigen::Vector3f> roots;
    std::vector<Eigen::Vector3f> rest;
    uint32_t pointsPerGuide = 4;
};

Fixture MakeFixture()
{
    Fixture fixture;
    for (int guide = 0; guide < 4; ++guide)
    {
        const float x = static_cast<float>(guide % 2) * 2.0f;
        const float z = static_cast<float>(guide / 2) * 2.0f;
        fixture.roots.push_back(Eigen::Vector3f(x, 0.0f, z));
        for (int local = 0; local < 4; ++local)
        {
            const float t = static_cast<float>(local);
            fixture.rest.push_back(Eigen::Vector3f(
                x + 0.1f * t, -1.0f * t, z + 0.2f * std::sin(t)));
        }
    }
    return fixture;
}

Eigen::Vector3f PointTangent(const Fixture& fixture, uint32_t guide, uint32_t local)
{
    const size_t base = static_cast<size_t>(guide) * fixture.pointsPerGuide;
    if (local + 1 < fixture.pointsPerGuide)
        return (fixture.rest[base + local + 1] - fixture.rest[base + local]).normalized();
    return (fixture.rest[base + local] - fixture.rest[base + local - 1]).normalized();
}

TEST(TressfxFollowBinding, ZeroOrFullCountKeepsAllGuidesSimulated)
{
    const Fixture fixture = MakeFixture();
    EXPECT_TRUE(BuildTressfxFollowBinding(
        fixture.roots, fixture.rest, fixture.pointsPerGuide, 0).simulationGuideIndices.empty());
    EXPECT_TRUE(BuildTressfxFollowBinding(
        fixture.roots, fixture.rest, fixture.pointsPerGuide, 4).simulationGuideIndices.empty());
}

TEST(TressfxFollowBinding, TwoGuidesCoverFourAndRestoreTheAuthoredShape)
{
    const Fixture fixture = MakeFixture();
    const TressfxFollowBinding binding = BuildTressfxFollowBinding(
        fixture.roots, fixture.rest, fixture.pointsPerGuide, 2);
    ASSERT_EQ(binding.simulationGuideIndices.size(), 2u);
    EXPECT_EQ(binding.followGuideIndex.size(), fixture.roots.size());
    EXPECT_EQ(binding.offsets.size(), fixture.rest.size());
    for (size_t index = 0; index < binding.simulationGuideIndices.size(); ++index)
    {
        if (index > 0)
            EXPECT_LT(binding.simulationGuideIndices[index - 1],
                binding.simulationGuideIndices[index]);
    }

    for (size_t guide = 0; guide < fixture.roots.size(); ++guide)
    {
        const uint32_t source = binding.followGuideIndex[guide];
        ASSERT_NE(std::find(binding.simulationGuideIndices.begin(),
            binding.simulationGuideIndices.end(), source),
            binding.simulationGuideIndices.end());
        const Eigen::Vector3f reference = binding.frameReferences[guide].head<3>();
        const size_t sourceBase = static_cast<size_t>(source) * fixture.pointsPerGuide;
        const size_t guideBase = guide * fixture.pointsPerGuide;
        for (uint32_t local = 0; local < fixture.pointsPerGuide; ++local)
        {
            const TressfxFollowFrame frame =
                TressfxMakeFollowFrame(PointTangent(fixture, source, local), reference);
            const Eigen::Vector4f offset = binding.offsets[guideBase + local];
            const Eigen::Vector3f reconstructed = fixture.rest[sourceBase + local] +
                frame.x * offset.x() + frame.y * offset.y() + frame.z * offset.z();
            EXPECT_NEAR((reconstructed - fixture.rest[guideBase + local]).norm(),
                0.0f, 1.0e-4f);
        }
    }
}

TEST(TressfxFollowBinding, SelectionAndOffsetsAreDeterministic)
{
    const Fixture fixture = MakeFixture();
    const TressfxFollowBinding first = BuildTressfxFollowBinding(
        fixture.roots, fixture.rest, fixture.pointsPerGuide, 2);
    const TressfxFollowBinding second = BuildTressfxFollowBinding(
        fixture.roots, fixture.rest, fixture.pointsPerGuide, 2);
    EXPECT_EQ(first.simulationGuideIndices, second.simulationGuideIndices);
    EXPECT_EQ(first.followGuideIndex, second.followGuideIndex);
    ASSERT_EQ(first.offsets.size(), second.offsets.size());
    for (size_t index = 0; index < first.offsets.size(); ++index)
        EXPECT_TRUE(first.offsets[index].isApprox(second.offsets[index], 0.0f));
}

TEST(TressfxFollowBinding, RejectsInvalidDimensions)
{
    const Fixture fixture = MakeFixture();
    EXPECT_THROW(BuildTressfxFollowBinding(
        fixture.roots, fixture.rest, 1, 2), std::invalid_argument);
    EXPECT_THROW(BuildTressfxFollowBinding(
        fixture.roots, fixture.rest, fixture.pointsPerGuide + 1, 2), std::invalid_argument);
}
} // namespace
