#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "editor/ui/tressfxGroupEditor.h"

namespace
{
using nlohmann::json;
using VL::Editor::TressfxGroupEditor;

json Group(const std::string& name, json ranges)
{
    return {{"name", name}, {"guideRanges", std::move(ranges)}};
}

json GroupsDocument(json groups)
{
    json document = {{"type", "tressfx"}, {"modelDataPath", "Maps/SC_x/Source/hair.tfx"},
        {"strandRadius", 0.05}};
    document["groups"] = std::move(groups);
    return document;
}
} // namespace

TEST(TressfxGroupEditor, LoadsGroupsAndCountsNamedAndDefaultGuides)
{
    const json document = GroupsDocument(json::array({
        Group("fringe", {{0, 10}, {20, 30}}),
        Group("side", {{10, 20}})}));
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(document, 100, error)) << error;
    ASSERT_EQ(editor.Groups().size(), 2u);
    EXPECT_EQ(editor.Groups()[0].name, "fringe");
    EXPECT_EQ(editor.GuideCount(), 100u);
    EXPECT_EQ(editor.GuideCountFor(0), 20u);
    EXPECT_EQ(editor.GuideCountFor(1), 10u);
    EXPECT_EQ(editor.DefaultGuideCount(), 70u);
}

TEST(TressfxGroupEditor, LoadWithoutGroupsLeavesEverythingInDefault)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load({{"type", "tressfx"}}, 42, error)) << error;
    EXPECT_TRUE(editor.Groups().empty());
    EXPECT_EQ(editor.DefaultGuideCount(), 42u);
    EXPECT_EQ(editor.GuideCountFor(0), 0u);
}

TEST(TressfxGroupEditor, UnknownGuideCountSkipsUpperBoundButReportsNoDefaultCount)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load({{"groups", json::array({Group("fringe", {{0, 10}})})}},
        0, error)) << error;
    EXPECT_EQ(editor.DefaultGuideCount(), 0u);
    EXPECT_EQ(editor.GuideCountFor(0), 10u);
}

TEST(TressfxGroupEditor, RejectsReservedDuplicateAndEmptyNames)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(GroupsDocument(json::array({Group("fringe", {{0, 2}})})),
        100, error)) << error;

    EXPECT_FALSE(editor.AddGroup("Default", {10, 12}, error));
    EXPECT_FALSE(editor.AddGroup(" unassigned ", {10, 12}, error));
    EXPECT_FALSE(editor.AddGroup("   ", {10, 12}, error));
    // 名称比较 Trim 后大小写不敏感。
    EXPECT_FALSE(editor.AddGroup(" FRINGE ", {10, 12}, error));
    EXPECT_FALSE(editor.RenameGroup(0, "", error));
    EXPECT_FALSE(editor.RenameGroup(0, "default", error));
    // 前后空白是行内错误，不能靠静默 Trim 通过；只有合法名称才写入 Trim 后的值。
    EXPECT_FALSE(editor.RenameGroup(0, " crown ", error));
    EXPECT_NE(error.find("whitespace"), std::string::npos);
    EXPECT_FALSE(editor.AddGroup(" crown ", {10, 12}, error));
    EXPECT_NE(error.find("whitespace"), std::string::npos);
    EXPECT_TRUE(editor.RenameGroup(0, "crown", error)) << error;
    EXPECT_EQ(editor.Groups()[0].name, "crown");
}

TEST(TressfxGroupEditor, NewGroupRequiresAValidNonEmptyFirstRange)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(json::object(), 100, error)) << error;
    EXPECT_FALSE(editor.AddGroup("fringe", {5, 5}, error));
    EXPECT_FALSE(editor.AddGroup("fringe", {6, 5}, error));
    EXPECT_FALSE(editor.AddGroup("fringe", {0, 101}, error));
    EXPECT_TRUE(editor.AddGroup("fringe", {0, 10}, error)) << error;
    EXPECT_EQ(editor.GuideCountFor(0), 10u);
}

TEST(TressfxGroupEditor, EmptyGroupCanBeCreatedAndSaved)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(json::object(), 100, error)) << error;
    ASSERT_TRUE(editor.AddEmptyGroup("fringe", error)) << error;
    ASSERT_EQ(editor.Groups().size(), 1u);
    EXPECT_TRUE(editor.Groups()[0].guideRanges.empty());
    json document = GroupsDocument(json::array());
    editor.WriteGroupArray(document);
    ASSERT_EQ(document["groups"].size(), 1u);
    EXPECT_EQ(document["groups"][0]["guideRanges"], json::array());
}

TEST(TressfxGroupEditor, RejectsOverlappingRangesWithinAndAcrossGroups)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(json::object(), 100, error)) << error;
    ASSERT_TRUE(editor.AddGroup("fringe", {0, 10}, error)) << error;

    // 组内重叠。
    EXPECT_FALSE(editor.AddRange(0, {5, 12}, error));
    // 跨组重叠。
    EXPECT_FALSE(editor.AddGroup("side", {9, 12}, error));
    // 首尾相接允许，之后又给 fringe 加相邻区间也允许。
    ASSERT_TRUE(editor.AddGroup("side", {10, 20}, error)) << error;
    ASSERT_TRUE(editor.AddRange(0, {20, 30}, error)) << error;
    EXPECT_FALSE(editor.AddRange(0, {19, 21}, error));
    EXPECT_EQ(editor.Groups()[0].guideRanges.size(), 2u);
}

TEST(TressfxGroupEditor, RangesStaySortedAfterInsert)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(json::object(), 100, error)) << error;
    ASSERT_TRUE(editor.AddGroup("fringe", {50, 60}, error)) << error;
    ASSERT_TRUE(editor.AddRange(0, {0, 10}, error)) << error;
    ASSERT_TRUE(editor.AddRange(0, {20, 30}, error)) << error;
    ASSERT_EQ(editor.Groups()[0].guideRanges.size(), 3u);
    EXPECT_EQ(editor.Groups()[0].guideRanges[0], (std::pair<uint32_t, uint32_t>{0u, 10u}));
    EXPECT_EQ(editor.Groups()[0].guideRanges[1], (std::pair<uint32_t, uint32_t>{20u, 30u}));
    EXPECT_EQ(editor.Groups()[0].guideRanges[2], (std::pair<uint32_t, uint32_t>{50u, 60u}));
}

TEST(TressfxGroupEditor, RemovingLastRangeKeepsAnEmptyGroup)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(GroupsDocument(json::array({Group("fringe", {{0, 10}, {20, 30}})})),
        100, error)) << error;
    ASSERT_TRUE(editor.RemoveRange(0, 1, error)) << error;
    ASSERT_EQ(editor.Groups().size(), 1u);
    EXPECT_EQ(editor.Groups()[0].guideRanges.size(), 1u);
    ASSERT_TRUE(editor.RemoveRange(0, 0, error)) << error;
    ASSERT_EQ(editor.Groups().size(), 1u);
    EXPECT_TRUE(editor.Groups()[0].guideRanges.empty());
    EXPECT_EQ(editor.DefaultGuideCount(), 100u);
    EXPECT_FALSE(editor.RemoveRange(0, 0, error));
}

TEST(TressfxGroupEditor, DeleteGroupRenumbersFollowingGroups)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(GroupsDocument(json::array({
        Group("alpha", {{0, 5}}),
        Group("beta", {{5, 15}}),
        Group("gamma", {{15, 18}})})), 100, error)) << error;
    ASSERT_EQ(editor.GuideCountFor(2), 3u);
    EXPECT_EQ(editor.DefaultGuideCount(), 82u);
    editor.DeleteGroup(1);
    ASSERT_EQ(editor.Groups().size(), 2u);
    EXPECT_EQ(editor.Groups()[0].name, "alpha");
    EXPECT_EQ(editor.Groups()[1].name, "gamma");
    // 运行时组号 k+1..N 随 vector erase 前移。
    EXPECT_EQ(editor.GuideCountFor(1), 3u);
    EXPECT_EQ(editor.DefaultGuideCount(), 92u);
}

TEST(TressfxGroupEditor, CompressGuidesMergesConsecutiveAndDeduplicates)
{
    EXPECT_EQ(TressfxGroupEditor::CompressGuides({3, 1, 2, 2, 7, 8, 10}),
        (std::vector<std::pair<uint32_t, uint32_t>>{{1, 4}, {7, 9}, {10, 11}}));
    EXPECT_TRUE(TressfxGroupEditor::CompressGuides({}).empty());
    EXPECT_EQ(TressfxGroupEditor::CompressGuides({4}),
        (std::vector<std::pair<uint32_t, uint32_t>>{{4, 5}}));
}

TEST(TressfxGroupEditor, GroupColorIsStableAcrossCallsAndDependsOnName)
{
    const auto fringe = TressfxGroupEditor::GroupColor("fringe");
    EXPECT_EQ(fringe, TressfxGroupEditor::GroupColor("fringe"));
    bool foundDifferent = false;
    for (const char* name : {"side", "back", "crown", "tail", "bob"})
    {
        if (TressfxGroupEditor::GroupColor(name) != fringe)
        {
            foundDifferent = true;
            break;
        }
    }
    EXPECT_TRUE(foundDifferent);
}

TEST(TressfxGroupEditor, GroupColorDelegatesToTheSharedGpuPalette)
{
    EXPECT_EQ(TressfxGroupEditor::GroupColor("fringe"),
        VL::Hair::TressfxGroupColorForName("fringe"));
    EXPECT_EQ(VL::Hair::TressfxDefaultGroupColor(),
        (VL::Hair::TressfxGroupColor{120, 128, 136}));
}

TEST(TressfxGroupEditor, OnlyTheSixLiveParametersCanBeOverridden)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(GroupsDocument(json::array({Group("fringe", {{0, 4}})})),
        100, error)) << error;
    const std::vector<const char*> allowed = {"damping", "bendStiffness",
        "globalShapeStiffness", "globalShapeEffectiveRange", "vspCoefficient",
        "vspAccelThresholdMax"};
    for (const char* field : allowed)
    {
        SCOPED_TRACE(field);
        EXPECT_TRUE(editor.SetOverride(0, field, 0.25));
        EXPECT_TRUE(editor.Groups()[0].simulationOverrides.contains(field));
    }
    for (const char* field : {"enabled", "gravity", "iterations", "windGain", "backend"})
    {
        SCOPED_TRACE(field);
        EXPECT_FALSE(editor.SetOverride(0, field, 0.5));
    }
    EXPECT_TRUE(editor.SetOverride(0, "damping", std::nullopt));
    EXPECT_FALSE(editor.Groups()[0].simulationOverrides.contains("damping"));
    EXPECT_FALSE(editor.SetOverride(0, "damping",
        std::numeric_limits<double>::quiet_NaN()));
    editor.ClearOverrides(0);
    EXPECT_TRUE(editor.Groups()[0].simulationOverrides.empty());
}

TEST(TressfxGroupEditor, WriteGroupArrayWritesGroupsWithoutColorAndKeepsDefaults)
{
    json document = GroupsDocument(json::array({Group("fringe", {{0, 4}, {8, 12}})}));
    document["simulationDefaults"] = {{"damping", 0.035}};
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(document, 100, error)) << error;
    ASSERT_TRUE(editor.RenameGroup(0, "crown", error)) << error;
    ASSERT_TRUE(editor.SetOverride(0, "bendStiffness", 0.4));
    editor.WriteGroupArray(document);

    ASSERT_TRUE(document.contains("groups"));
    ASSERT_EQ(document["groups"].size(), 1u);
    EXPECT_EQ(document["groups"][0]["name"], "crown");
    EXPECT_EQ(document["groups"][0]["guideRanges"].size(), 2u);
    EXPECT_EQ(document["groups"][0]["guideRanges"][1][0], 8u);
    EXPECT_DOUBLE_EQ(document["groups"][0]["simulationOverrides"]["bendStiffness"]
        .get<double>(), 0.4);
    // 颜色是预览状态，不写资产；simulationDefaults 归资产默认值所有。
    EXPECT_FALSE(document["groups"][0].contains("color"));
    EXPECT_DOUBLE_EQ(document["simulationDefaults"]["damping"].get<double>(), 0.035);

    editor.DeleteGroup(0);
    editor.WriteGroupArray(document);
    EXPECT_FALSE(document.contains("groups"));
    EXPECT_TRUE(document.contains("simulationDefaults"));
}

TEST(TressfxGroupEditor, EmptyOverridesAreNotWrittenBack)
{
    json document = GroupsDocument(json::array({Group("fringe", {{0, 4}})}));
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(document, 100, error)) << error;
    editor.ClearOverrides(0);
    editor.WriteGroupArray(document);
    EXPECT_FALSE(document["groups"][0].contains("simulationOverrides"));
}

TEST(TressfxGroupEditor, LoadRejectsInvalidRangeShapesAndOverlaps)
{
    TressfxGroupEditor editor;
    std::string error;
    EXPECT_FALSE(editor.Load(GroupsDocument(json::array({Group("fringe", {{0, 101}})})),
        100, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(editor.Load(GroupsDocument(json::array({Group("fringe", {{5, 2}})})),
        100, error));
    EXPECT_FALSE(editor.Load(GroupsDocument(json::array({
        Group("fringe", {{0, 10}}), Group(" FRINGE ", {{10, 20}})})), 100, error));
    EXPECT_FALSE(editor.Load(GroupsDocument(json::array({
        Group("fringe", {{0, 10}}), Group("side", {{9, 20}})})), 100, error));
    EXPECT_TRUE(editor.Load(GroupsDocument(json::array({Group("fringe", json::array())})),
        100, error)) << error;
    EXPECT_TRUE(editor.Groups()[0].guideRanges.empty());
    json unknownField = Group("fringe", {{0, 10}});
    unknownField["color"] = "red";
    EXPECT_FALSE(editor.Load(GroupsDocument(json::array({unknownField})), 100, error));
    json badOverride = Group("fringe", {{0, 10}});
    badOverride["simulationOverrides"] = {{"gravity", 1.0}};
    EXPECT_FALSE(editor.Load(GroupsDocument(json::array({badOverride})), 100, error));
}

TEST(TressfxGroupEditor, LoadAcceptsRepeatedAndHalfOpenTouchingRanges)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(GroupsDocument(json::array({
        Group("fringe", {{0, 2}, {2, 4}, {10, 12}})})), 100, error)) << error;
    EXPECT_EQ(editor.GuideCountFor(0), 6u);
    EXPECT_EQ(editor.DefaultGuideCount(), 94u);
}

TEST(TressfxGroupEditor, LoadRejectsNamesThatOnlyDifferAfterWhitespaceTrim)
{
    // 资产层不接受前后空白；编辑器如果静默 Trim 后放行，保存结果就通不过下一次导入。
    TressfxGroupEditor editor;
    std::string error;
    EXPECT_FALSE(editor.Load(GroupsDocument(json::array({
        Group(" fringe ", {{0, 2}})})), 100, error));
    EXPECT_NE(error.find("whitespace"), std::string::npos);
}

TEST(TressfxGroupEditor, AddRangeMergesTouchingRanges)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(GroupsDocument(json::array({Group("fringe", {{0, 2}})})),
        100, error)) << error;
    ASSERT_TRUE(editor.AddRange(0, {2, 4}, error)) << error;
    ASSERT_EQ(editor.Groups()[0].guideRanges.size(), 1u);
    EXPECT_EQ(editor.Groups()[0].guideRanges[0], (std::pair<uint32_t, uint32_t>{0, 4}));
    ASSERT_TRUE(editor.AddRange(0, {8, 10}, error)) << error;
    ASSERT_TRUE(editor.AddRange(0, {4, 8}, error)) << error;
    ASSERT_EQ(editor.Groups()[0].guideRanges.size(), 1u);
    EXPECT_EQ(editor.Groups()[0].guideRanges[0], (std::pair<uint32_t, uint32_t>{0, 10}));
}

TEST(TressfxGroupEditor, RangeLimitRejectsWithoutMutatingTheDraft)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load({{"type", "tressfx"}}, 100000, error)) << error;
    // 首个区间之后每次加两个相隔的 guide，保证不会被合并。
    ASSERT_TRUE(editor.AddGroup("fringe", {0, 1}, error)) << error;
    for (size_t index = 1; index < TressfxGroupEditor::MaxGuideRanges; ++index)
        ASSERT_TRUE(editor.AddRange(0, {static_cast<uint32_t>(index * 2),
            static_cast<uint32_t>(index * 2 + 1)}, error)) << index;
    ASSERT_EQ(editor.Groups()[0].guideRanges.size(), TressfxGroupEditor::MaxGuideRanges);
    double before = static_cast<double>(editor.Groups()[0].guideRanges.size());
    EXPECT_FALSE(editor.AddRange(0, {90000, 90010}, error));
    EXPECT_NE(error.find("limit"), std::string::npos);
    EXPECT_EQ(static_cast<double>(editor.Groups()[0].guideRanges.size()), before);
}

TEST(TressfxGroupEditor, ApplyPaintedGuidesMovesReplacementsAndKeepsEmptyGroups)
{
    TressfxGroupEditor editor;
    std::string error;
    ASSERT_TRUE(editor.Load(GroupsDocument(json::array({
        Group("alpha", {{0, 2}}), Group("beta", {{2, 4}})})), 10, error)) << error;
    ASSERT_TRUE(editor.ApplyPaintedGuides(1, {1, 3}, {}, error)) << error;
    ASSERT_EQ(editor.Groups().size(), 2u);
    EXPECT_EQ(editor.Groups()[0].name, "alpha");
    EXPECT_EQ(editor.Groups()[0].guideRanges,
        (std::vector<std::pair<uint32_t, uint32_t>>{{0, 1}}));
    EXPECT_EQ(editor.Groups()[1].name, "beta");
    EXPECT_EQ(editor.Groups()[1].guideRanges,
        (std::vector<std::pair<uint32_t, uint32_t>>{{1, 4}}));

    ASSERT_TRUE(editor.ApplyPaintedGuides(1, {}, {1, 2, 3}, error)) << error;
    ASSERT_EQ(editor.Groups().size(), 2u);
    EXPECT_EQ(editor.Groups()[0].name, "alpha");
    EXPECT_EQ(editor.Groups()[1].name, "beta");
    EXPECT_TRUE(editor.Groups()[1].guideRanges.empty());
    EXPECT_EQ(editor.GuideGroupIds(), (std::vector<uint32_t>{1, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
}
