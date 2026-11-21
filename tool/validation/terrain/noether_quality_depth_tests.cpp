#include "render/resource/terrainCompositedQualityGpu.h"

#include <gtest/gtest.h>
#include <type_traits>

namespace
{
template<typename Handle>
Handle CpuOnlyHandle()
{
    typename Handle::CType native{};
    const uintptr_t identity = 1;
    static_assert(sizeof(native) <= sizeof(identity));
    std::memcpy(&native, &identity, sizeof(native));
    return Handle(native);
}

VL::TerrainCompositedQualityDepthBinding Fixture()
{
    VL::TerrainCompositedQualityDepthBinding depth;
    depth.graphDepthOwner = std::make_shared<const uint64_t>(1);
    depth.image = CpuOnlyHandle<vk::Image>();
    depth.descriptor = {CpuOnlyHandle<vk::Sampler>(), CpuOnlyHandle<vk::ImageView>(),
        vk::ImageLayout::eDepthStencilReadOnlyOptimal};
    depth.extent = vk::Extent2D(128, 128);
    depth.format = vk::Format::eD32Sfloat;
    depth.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
    return depth;
}

TEST(QualityBorrowedDepth, DescriptorAndGraphOwnerAreBorrowedWithoutOwningTexture)
{
    static_assert(std::is_trivially_destructible_v<vk::Image>);
    static_assert(std::is_trivially_destructible_v<vk::ImageView>);
    static_assert(std::is_trivially_destructible_v<vk::Sampler>);
    auto depth = Fixture();
    EXPECT_TRUE(VL::TerrainQualityDepthReadable(depth));
    const auto image = depth.image;
    const std::weak_ptr<const void> graph = depth.graphDepthOwner;
    auto snapshot = std::make_shared<VL::TerrainCompositedQualitySnapshot>();
    snapshot->finalFeedbackDepth = depth;
    depth.graphDepthOwner.reset();
    EXPECT_FALSE(graph.expired());
    EXPECT_EQ(snapshot->finalFeedbackDepth.image, image);
    snapshot.reset();
    EXPECT_TRUE(graph.expired());
}

TEST(QualityBorrowedDepth, MissingOwnerOrHandlesCannotBeUsed)
{
    auto depth = Fixture();
    depth.graphDepthOwner.reset();
    EXPECT_FALSE(VL::TerrainQualityDepthReadable(depth));
    depth = Fixture();
    depth.descriptor.imageView = nullptr;
    EXPECT_FALSE(VL::TerrainQualityDepthReadable(depth));
}

TEST(QualityBorrowedDepth, LayoutChangeCannotAddMissingSampledUsage)
{
    auto depth = Fixture();
    depth.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    EXPECT_FALSE(VL::TerrainQualityDepthReadable(depth));
    depth = Fixture();
    depth.descriptor.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    EXPECT_FALSE(VL::TerrainQualityDepthReadable(depth));
}

TEST(QualityBorrowedDepth, FrozenDomainRequiresSingleSampleD32)
{
    auto depth = Fixture();
    depth.extent.width = 256;
    EXPECT_FALSE(VL::TerrainQualityDepthReadable(depth));
    depth = Fixture();
    depth.samples = vk::SampleCountFlagBits::e4;
    EXPECT_FALSE(VL::TerrainQualityDepthReadable(depth));
    depth = Fixture();
    depth.format = vk::Format::eR32Sfloat;
    EXPECT_FALSE(VL::TerrainQualityDepthReadable(depth));
    depth = Fixture();
    depth.format = vk::Format::eD32SfloatS8Uint;
    EXPECT_FALSE(VL::TerrainQualityDepthReadable(depth));
}
}
