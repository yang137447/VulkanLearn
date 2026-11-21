#include "render/rhi/vulkan/presentTarget.h"
#include "render/frame/terrainCompositedFrameLifetime.h"

#include <gtest/gtest.h>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
struct DestroyedResource
{
    char kind;
    uintptr_t handle;
};

std::vector<DestroyedResource> destroyedResources;
std::vector<uintptr_t> replacementHandles;
VkResult createResult = VK_SUCCESS;
bool deviceAlive = true;

template<typename Handle>
Handle CpuHandle(uintptr_t identity)
{
    typename Handle::CType native{};
    static_assert(sizeof(native) <= sizeof(identity));
    std::memcpy(&native, &identity, sizeof(native));
    return Handle(native);
}

template<typename Native>
uintptr_t Identity(Native handle)
{
    uintptr_t identity = 0;
    static_assert(sizeof(handle) <= sizeof(identity));
    std::memcpy(&identity, &handle, sizeof(handle));
    return identity;
}

std::shared_ptr<VL::PresentSwapchainResources> Leaf(uintptr_t base)
{
    auto leaf = std::make_shared<VL::PresentSwapchainResources>(CpuHandle<vk::Device>(1));
    leaf->swapchain = CpuHandle<vk::SwapchainKHR>(base);
    leaf->imageViews = {CpuHandle<vk::ImageView>(base + 1), CpuHandle<vk::ImageView>(base + 2)};
    return leaf;
}

class PresentOwner : public testing::Test
{
protected:
    void SetUp() override
    {
        destroyedResources.clear();
        destroyedResources.reserve(32);
        replacementHandles.clear();
        createResult = VK_SUCCESS;
        deviceAlive = true;
    }
};
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
    VkDevice, VkImageView imageView, const VkAllocationCallbacks*)
{
    EXPECT_TRUE(deviceAlive);
    destroyedResources.push_back({'v', Identity(imageView)});
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice, VkSwapchainKHR swapchain, const VkAllocationCallbacks*)
{
    EXPECT_TRUE(deviceAlive);
    destroyedResources.push_back({'s', Identity(swapchain)});
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice, const VkSwapchainCreateInfoKHR* create,
    const VkAllocationCallbacks*, VkSwapchainKHR* swapchain)
{
    EXPECT_TRUE(deviceAlive);
    replacementHandles.push_back(Identity(create->oldSwapchain));
    if (createResult == VK_SUCCESS)
        *swapchain = static_cast<VkSwapchainKHR>(CpuHandle<vk::SwapchainKHR>(2000));
    return createResult;
}

namespace
{
TEST_F(PresentOwner, LastFrameFenceActuallyDefersNativeViewAndSwapchainDestruction)
{
    VL::PresentTarget target;
    auto leaf = Leaf(100);
    std::weak_ptr<VL::PresentSwapchainResources> weak = leaf;
    target.PublishSwapchain(leaf, {CpuHandle<vk::Image>(10), CpuHandle<vk::Image>(11)});
    VL::TerrainCompositedFrameLifetime frames;
    frames.Submitted(0, 20, {}, leaf);
    leaf.reset();
    target.DetachSwapchain();
    EXPECT_FALSE(target.swapChain);
    EXPECT_TRUE(target.imageViews.empty());
    EXPECT_TRUE(target.images.empty());
    EXPECT_EQ(target.imageCount, 0);
    EXPECT_FALSE(weak.expired());
    EXPECT_TRUE(destroyedResources.empty());
    frames.Complete(19);
    EXPECT_TRUE(destroyedResources.empty());
    frames.Complete(20);
    EXPECT_TRUE(weak.expired());
    ASSERT_EQ(destroyedResources.size(), 3);
    EXPECT_EQ(destroyedResources[0].kind, 'v');
    EXPECT_EQ(destroyedResources[0].handle, 101);
    EXPECT_EQ(destroyedResources[1].kind, 'v');
    EXPECT_EQ(destroyedResources[1].handle, 102);
    EXPECT_EQ(destroyedResources[2].kind, 's');
    EXPECT_EQ(destroyedResources[2].handle, 100);
}

TEST_F(PresentOwner, ResizePublishesNewGenerationWhileOldLeafRemainsBorrowed)
{
    VL::PresentTarget target;
    target.PublishSwapchain(Leaf(100), {CpuHandle<vk::Image>(10), CpuHandle<vk::Image>(11)});
    EXPECT_EQ(target.swapchainGeneration, 1);
    auto old = target.swapchainOwner;
    target.DetachSwapchain();
    target.PublishSwapchain(Leaf(200), {CpuHandle<vk::Image>(20), CpuHandle<vk::Image>(21)});
    EXPECT_EQ(target.swapchainGeneration, 2);
    EXPECT_NE(target.swapchainOwner, old);
    EXPECT_EQ(old->swapchain, CpuHandle<vk::SwapchainKHR>(100));
    EXPECT_EQ(target.swapChain, CpuHandle<vk::SwapchainKHR>(200));
    EXPECT_TRUE(destroyedResources.empty());
    old.reset();
    ASSERT_EQ(destroyedResources.size(), 3);
    EXPECT_EQ(destroyedResources.back().handle, 100);
    target.DetachSwapchain();
    ASSERT_EQ(destroyedResources.size(), 6);
    EXPECT_EQ(destroyedResources.back().handle, 200);
}

TEST_F(PresentOwner, FailedCandidateNeverPublishesAliasesAndReleasesPartialNativeObjects)
{
    VL::PresentTarget target;
    {
        auto leaf = Leaf(100);
        leaf->imageViews.pop_back();
        EXPECT_THROW(target.PublishSwapchain(leaf,
            {CpuHandle<vk::Image>(10), CpuHandle<vk::Image>(11)}), std::invalid_argument);
        EXPECT_EQ(target.swapchainGeneration, 0);
        EXPECT_FALSE(target.swapchainOwner);
        EXPECT_FALSE(target.swapChain);
        EXPECT_TRUE(target.imageViews.empty());
        EXPECT_TRUE(destroyedResources.empty());
    }
    ASSERT_EQ(destroyedResources.size(), 2);
    EXPECT_EQ(destroyedResources[0].handle, 101);
    EXPECT_EQ(destroyedResources[1].handle, 100);
}

TEST_F(PresentOwner, RepeatedDetachCannotDoubleDestroy)
{
    VL::PresentTarget target;
    target.PublishSwapchain(Leaf(100), {CpuHandle<vk::Image>(10), CpuHandle<vk::Image>(11)});
    target.DetachSwapchain();
    target.DetachSwapchain();
    EXPECT_EQ(destroyedResources.size(), 3);
}

TEST_F(PresentOwner, InvalidReplacementCannotOverwriteActiveOwner)
{
    VL::PresentTarget target;
    target.PublishSwapchain(Leaf(100), {CpuHandle<vk::Image>(10), CpuHandle<vk::Image>(11)});
    auto candidate = Leaf(200);
    EXPECT_THROW(target.PublishSwapchain(candidate,
        {CpuHandle<vk::Image>(20), CpuHandle<vk::Image>(21)}), std::invalid_argument);
    EXPECT_EQ(target.swapchainGeneration, 1);
    EXPECT_EQ(target.swapChain, CpuHandle<vk::SwapchainKHR>(100));
    EXPECT_EQ(target.swapchainOwner->imageViews, target.imageViews);
    EXPECT_TRUE(destroyedResources.empty());
}

TEST_F(PresentOwner, ReplacementPassesLiveOldHandleThroughActualNativeCreate)
{
    VL::PresentTarget target;
    target.PublishSwapchain(Leaf(100), {CpuHandle<vk::Image>(10), CpuHandle<vk::Image>(11)});
    std::weak_ptr<VL::PresentSwapchainResources> oldWeak = target.swapchainOwner;
    VL::TerrainCompositedFrameLifetime frames;
    frames.Submitted(0, 20, {}, target.swapchainOwner);
    auto old = target.DetachSwapchainForReplacement();
    EXPECT_FALSE(target.swapchainOwner);
    EXPECT_FALSE(oldWeak.expired());
    auto next = VL::PresentSwapchainResources::Create(
        CpuHandle<vk::Device>(1), vk::SwapchainCreateInfoKHR{}, std::move(old));
    ASSERT_EQ(replacementHandles.size(), 1);
    EXPECT_EQ(replacementHandles.front(), 100);
    EXPECT_TRUE(destroyedResources.empty());
    next->imageViews = {CpuHandle<vk::ImageView>(2001), CpuHandle<vk::ImageView>(2002)};
    target.PublishSwapchain(std::move(next), {CpuHandle<vk::Image>(20), CpuHandle<vk::Image>(21)});
    EXPECT_EQ(target.swapchainGeneration, 2);
    frames.Complete(19);
    EXPECT_TRUE(destroyedResources.empty());
    frames.Complete(20);
    EXPECT_TRUE(oldWeak.expired());
    ASSERT_EQ(destroyedResources.size(), 3);
    EXPECT_EQ(destroyedResources.back().handle, 100);
}

TEST_F(PresentOwner, InitialCreateUsesNullOldSwapchain)
{
    const auto owner = VL::PresentSwapchainResources::Create(
        CpuHandle<vk::Device>(1), vk::SwapchainCreateInfoKHR{});
    ASSERT_EQ(replacementHandles.size(), 1);
    EXPECT_EQ(replacementHandles.front(), 0);
    EXPECT_TRUE(owner->swapchain);
}

TEST_F(PresentOwner, FailedNativeReplacementLeavesOnlyBorrowedOldLeaf)
{
    VL::PresentTarget target;
    target.PublishSwapchain(Leaf(100), {CpuHandle<vk::Image>(10), CpuHandle<vk::Image>(11)});
    auto old = target.DetachSwapchainForReplacement();
    createResult = VK_ERROR_INITIALIZATION_FAILED;
    EXPECT_THROW(VL::PresentSwapchainResources::Create(
        CpuHandle<vk::Device>(1), vk::SwapchainCreateInfoKHR{}, old), vk::SystemError);
    ASSERT_EQ(replacementHandles.size(), 1);
    EXPECT_EQ(replacementHandles.front(), 100);
    EXPECT_FALSE(target.swapChain);
    EXPECT_FALSE(target.swapchainOwner);
    EXPECT_EQ(target.swapchainGeneration, 1);
    EXPECT_TRUE(destroyedResources.empty());
    old.reset();
    EXPECT_EQ(destroyedResources.size(), 3);
}

TEST_F(PresentOwner, ReplacementRejectsDifferentDeviceBeforeNativeCreate)
{
    auto old = Leaf(100);
    EXPECT_THROW(VL::PresentSwapchainResources::Create(
        CpuHandle<vk::Device>(2), vk::SwapchainCreateInfoKHR{}, old), std::invalid_argument);
    EXPECT_TRUE(replacementHandles.empty());
    EXPECT_TRUE(destroyedResources.empty());
}

TEST_F(PresentOwner, DeviceTeardownReleasesBorrowedLeavesBeforeLateOwners)
{
    auto old = Leaf(100);
    auto current = Leaf(200);
    auto borrowed = old;
    VL::ReleasePresentSwapchainsForDevice(CpuHandle<vk::Device>(1));
    ASSERT_EQ(destroyedResources.size(), 6);
    EXPECT_FALSE(old->device);
    EXPECT_FALSE(old->swapchain);
    EXPECT_TRUE(old->imageViews.empty());
    EXPECT_FALSE(current->device);
    deviceAlive = false;
    borrowed.reset();
    old.reset();
    current.reset();
    EXPECT_EQ(destroyedResources.size(), 6);
}
}
