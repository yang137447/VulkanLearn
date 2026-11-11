#include <gtest/gtest.h>

#include "pipeline/passPipelineContractKey.h"

namespace
{
PassPipelineContractKey DefaultContract()
{
    PassPipelineContractKey contract;
    contract.renderPassCompatibilityKey.attachments.push_back(
        {vk::Format::eR16G16B16A16Sfloat,
         vk::SampleCountFlagBits::e1});
    contract.renderPassCompatibilityKey.colorAttachments.push_back(0);
    return contract;
}
} // namespace

TEST(PassPipelineContract, StencilDefaultsToDisabled)
{
    const PassPipelineContractKey contract = DefaultContract();
    EXPECT_FALSE(contract.stencilTestEnable);
    EXPECT_EQ(contract.stencilWriteMask, 0xFFu);
    EXPECT_EQ(contract.stencilCompareMask, 0xFFu);
    EXPECT_EQ(contract.stencilReference, 0u);
    EXPECT_EQ(contract.stencilCompareOp, vk::CompareOp::eAlways);
    EXPECT_EQ(contract.stencilFailOp, vk::StencilOp::eKeep);
    EXPECT_EQ(contract.stencilPassOp, vk::StencilOp::eKeep);
    EXPECT_EQ(contract.stencilDepthFailOp, vk::StencilOp::eKeep);

    const GraphicsPipelineStateDesc state =
        contract.BuildGraphicsPipelineStateDesc(
            vk::CullModeFlagBits::eBack,
            GraphicsPipelineBlendMode::Opaque);
    EXPECT_FALSE(state.bStencilTestEnable);
    EXPECT_EQ(state.stencilCompareOp, vk::CompareOp::eAlways);
}

TEST(PassPipelineContract, NormalizedKeySeparatesEveryStencilField)
{
    const PassPipelineContractKey base = DefaultContract();
    PassPipelineContractKey changed = base;
    changed.stencilTestEnable = true;
    EXPECT_NE(base.GetNormalizedKey(), changed.GetNormalizedKey());

    changed = base;
    changed.stencilWriteMask = 0x01;
    EXPECT_NE(base.GetNormalizedKey(), changed.GetNormalizedKey());

    changed = base;
    changed.stencilCompareMask = 0x01;
    EXPECT_NE(base.GetNormalizedKey(), changed.GetNormalizedKey());

    changed = base;
    changed.stencilReference = 1;
    EXPECT_NE(base.GetNormalizedKey(), changed.GetNormalizedKey());

    changed = base;
    changed.stencilCompareOp = vk::CompareOp::eNotEqual;
    EXPECT_NE(base.GetNormalizedKey(), changed.GetNormalizedKey());

    changed = base;
    changed.stencilFailOp = vk::StencilOp::eReplace;
    EXPECT_NE(base.GetNormalizedKey(), changed.GetNormalizedKey());

    changed = base;
    changed.stencilPassOp = vk::StencilOp::eReplace;
    EXPECT_NE(base.GetNormalizedKey(), changed.GetNormalizedKey());

    changed = base;
    changed.stencilDepthFailOp = vk::StencilOp::eReplace;
    EXPECT_NE(base.GetNormalizedKey(), changed.GetNormalizedKey());

    EXPECT_EQ(base, DefaultContract());
}

TEST(PassPipelineContract, BuildStateCopiesStencilGate)
{
    PassPipelineContractKey contract = DefaultContract();
    contract.stencilTestEnable = true;
    contract.stencilWriteMask = 0;
    contract.stencilCompareMask = 1;
    contract.stencilReference = 0;
    contract.stencilCompareOp = vk::CompareOp::eNotEqual;
    contract.stencilPassOp = vk::StencilOp::eKeep;

    const GraphicsPipelineStateDesc state =
        contract.BuildGraphicsPipelineStateDesc(
            vk::CullModeFlagBits::eBack,
            GraphicsPipelineBlendMode::DecalDBuffer);
    EXPECT_TRUE(state.bStencilTestEnable);
    EXPECT_EQ(state.stencilWriteMask, 0u);
    EXPECT_EQ(state.stencilCompareMask, 1u);
    EXPECT_EQ(state.stencilReference, 0u);
    EXPECT_EQ(state.stencilCompareOp, vk::CompareOp::eNotEqual);
    EXPECT_EQ(state.stencilPassOp, vk::StencilOp::eKeep);
    EXPECT_EQ(state.blendMode, GraphicsPipelineBlendMode::DecalDBuffer);
}
