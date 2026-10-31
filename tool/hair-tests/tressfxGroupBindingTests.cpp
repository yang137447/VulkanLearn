#include "hair/tressfxGroupBinding.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using VL::Hair::BuildTressfxGroupBinding;
using VL::Hair::TressfxGroupDeclaration;

TEST(TressfxGroupBinding, ExpandsRangesInDeclarationOrder)
{
    const auto binding = BuildTressfxGroupBinding(
        {{"fringe", {{0, 4}}}, {"side", {{6, 8}}}}, 10, "key", "test");
    ASSERT_EQ(binding->guideGroupIds.size(), 10u);
    EXPECT_EQ(binding->guideGroupIds,
        (std::vector<uint32_t>{1, 1, 1, 1, 0, 0, 2, 2, 0, 0}));
    EXPECT_EQ(binding->groupNames, (std::vector<std::string>{"fringe", "side"}));
    EXPECT_EQ(binding->key, "key");
    EXPECT_EQ(binding->GuideCount(), 10u);
}

TEST(TressfxGroupBinding, EmptyDeclarationIsAllDefault)
{
    const auto binding = BuildTressfxGroupBinding({}, 3, {}, "test");
    EXPECT_TRUE(binding->groupNames.empty());
    EXPECT_EQ(binding->guideGroupIds, (std::vector<uint32_t>{0u, 0u, 0u}));
    EXPECT_EQ(binding->GuideCount(), 3u);
}

TEST(TressfxGroupBinding, RejectsRangePastGuideCount)
{
    try
    {
        BuildTressfxGroupBinding({{"tail", {{2, 4}}}}, 3, "key", " (TF_hair.json)");
        FAIL() << "range past the guide count was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("\"tail\""), std::string::npos);
        EXPECT_NE(message.find("TF_hair.json"), std::string::npos);
    }
}

TEST(TressfxGroupBinding, RejectsCrossGroupOverlap)
{
    EXPECT_THROW(BuildTressfxGroupBinding(
        {{"a", {{0, 4}}}, {"b", {{3, 6}}}}, 6, "key", "test"),
        std::runtime_error);
}

TEST(TressfxGroupBinding, AdjacentRangesBelongingToDifferentGroupsAreValid)
{
    const auto binding = BuildTressfxGroupBinding(
        {{"a", {{0, 2}}}, {"b", {{2, 4}}}}, 4, "key", "test");
    EXPECT_EQ(binding->guideGroupIds, (std::vector<uint32_t>{1, 1, 2, 2}));
}

TEST(TressfxGroupBinding, PreviewValidationAcceptsGroupCountChangesWithinCapacity)
{
    VL::Hair::TressfxGroupBinding twoGroups;
    twoGroups.guideGroupIds = {0u, 1u, 2u};
    twoGroups.groupNames = {"fringe", "side"};
    twoGroups.key = "two";
    std::string error;
    EXPECT_TRUE(VL::Hair::ValidateTressfxGroupPreviewBinding(twoGroups, 3, error));

    // 同一 guide 映射下增加一个命名组（N+1）和减少一个组（N-1）都必须有效；
    // 运行时更新会在同一预留容量内原地重写颜色/参数表。
    VL::Hair::TressfxGroupBinding threeGroups = twoGroups;
    threeGroups.groupNames.push_back("crown");
    EXPECT_TRUE(VL::Hair::ValidateTressfxGroupPreviewBinding(threeGroups, 3, error));

    VL::Hair::TressfxGroupBinding oneGroup = twoGroups;
    oneGroup.groupNames = {"fringe"};
    oneGroup.guideGroupIds = {0u, 1u, 0u};
    EXPECT_TRUE(VL::Hair::ValidateTressfxGroupPreviewBinding(oneGroup, 3, error));
}

TEST(TressfxGroupBinding, PreviewValidationRejectsOverflowWithoutMutatingTheBinding)
{
    VL::Hair::TressfxGroupBinding binding;
    binding.groupNames.assign(VL::Hair::TressfxGroupPreviewNamedGroupCapacity + 1, "g");
    binding.guideGroupIds = {0u, 1u};
    const std::vector<uint32_t> idsBefore = binding.guideGroupIds;
    const std::vector<std::string> namesBefore = binding.groupNames;

    std::string error;
    EXPECT_FALSE(VL::Hair::ValidateTressfxGroupPreviewBinding(binding, 2, error));
    EXPECT_NE(error.find("capacity"), std::string::npos);
    EXPECT_EQ(binding.guideGroupIds, idsBefore);
    EXPECT_EQ(binding.groupNames, namesBefore);
}

TEST(TressfxGroupBinding, PreviewValidationRejectsUnknownGuideGroupId)
{
    VL::Hair::TressfxGroupBinding binding;
    binding.groupNames = {"fringe"};
    binding.guideGroupIds = {0u, 2u};
    std::string error;
    EXPECT_FALSE(VL::Hair::ValidateTressfxGroupPreviewBinding(binding, 2, error));
    EXPECT_NE(error.find("exceeds the declared group count"), std::string::npos);
}

TEST(TressfxGroupBinding, RuntimePreviewRequestValidatesValueSemantics)
{
    VL::Hair::TressfxGroupBindingPreviewRequest request;
    request.objectName = "preview_model";
    request.worldGeneration = 7;
    request.draftRevision = 3;
    request.guideGroupIds = {0u, 1u};
    request.groupNames = {"fringe"};
    request.bindingKey = "draft:3";
    std::string error;
    EXPECT_TRUE(VL::Hair::ValidateTressfxGroupBindingPreviewRequest(request, error));

    request.guideGroupIds = {0u, 2u};
    EXPECT_FALSE(VL::Hair::ValidateTressfxGroupBindingPreviewRequest(request, error));
    request.guideGroupIds = {0u, 1u};
    request.worldGeneration = 0;
    EXPECT_FALSE(VL::Hair::ValidateTressfxGroupBindingPreviewRequest(request, error));
    request.worldGeneration = 7;
    request.objectName.clear();
    EXPECT_FALSE(VL::Hair::ValidateTressfxGroupBindingPreviewRequest(request, error));
}

} // namespace
