#include "hair/hairConsoleCommand.h"

#include "hair/tressfxSimulationHistory.h"

#include <gtest/gtest.h>

#include <string>

// `hair reset [objectName]` 的纯参数语义：Console 只做值校验，真正的
// ResetPositions 由渲染线程在下一个正 delta 帧消费。这里不驱动 GPU，
// 也不存在 runtime test flag。
namespace
{
using VL::Hair::HairResetRequest;
using VL::Hair::ParseHairResetArguments;
}

TEST(HairConsoleCommand, EmptyArgumentsTargetAllObjects)
{
    HairResetRequest request;
    request.objectName = "stale";
    EXPECT_EQ(ParseHairResetArguments("", request), nullptr);
    EXPECT_TRUE(request.objectName.empty());
}

TEST(HairConsoleCommand, WhitespaceOnlyAlsoTargetsAllObjects)
{
    HairResetRequest request;
    EXPECT_EQ(ParseHairResetArguments("  \t\r\n ", request), nullptr);
    EXPECT_TRUE(request.objectName.empty());
}

TEST(HairConsoleCommand, SingleObjectNameIsTrimmed)
{
    HairResetRequest request;
    EXPECT_EQ(ParseHairResetArguments("  preview_model\t", request), nullptr);
    EXPECT_EQ(request.objectName, "preview_model");
}

TEST(HairConsoleCommand, SecondTokenIsRejected)
{
    HairResetRequest request;
    EXPECT_NE(ParseHairResetArguments("preview_model extra", request), nullptr);
    // 失败不发布半解析结果，调用方不应把请求投递出去。
    EXPECT_TRUE(request.objectName.empty());
}

TEST(HairConsoleCommand, OverlongObjectNameIsRejected)
{
    HairResetRequest request;
    const std::string name(129, 'a');
    EXPECT_NE(ParseHairResetArguments(name, request), nullptr);
    EXPECT_TRUE(request.objectName.empty());
}

TEST(HairConsoleCommand, MaximumLengthObjectNameIsAccepted)
{
    HairResetRequest request;
    const std::string name(128, 'a');
    EXPECT_EQ(ParseHairResetArguments(name, request), nullptr);
    EXPECT_EQ(request.objectName, name);
}

TEST(HairConsoleCommand, ExplicitResetReasonKeepsItsOwnName)
{
    // trace 元数据与 stderr 都按这个名字归因；播放事件和显式请求必须可区分。
    EXPECT_STREQ(VL::Hair::GuideSimulationResetReasonName(
        VL::Hair::GuideSimulationResetReason::Requested), "requested");
}
