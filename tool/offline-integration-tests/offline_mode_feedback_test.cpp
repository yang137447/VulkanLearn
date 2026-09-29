#include <gtest/gtest.h>

#include "ui/uiRenderSnapshot.h"

namespace VL
{
TEST(OfflineModeFeedback, RejectionKeepsEngineModeAndPublishedResult)
{
    UiViewModelSnapshot previous;
    previous.offlineMode = true;
    previous.offlineCompletedSamples = 4;
    previous.offlineRequestedMode = 0;
    UiViewModelSnapshot next;
    next.offlineMode = true;
    next.offlineCompletedSamples = 4;
    next.offlineModeFeedback = OfflineModeCommandFeedback{
        0, "Realtime implementation unavailable: M_offlineCloth"};

    ReconcileOfflineModeFeedback(next, previous);

    EXPECT_TRUE(next.offlineMode);
    EXPECT_EQ(next.offlineCompletedSamples, 4);
    EXPECT_EQ(next.offlineRequestedMode, -1);
    EXPECT_EQ(next.offlineUiNotice,
        "Realtime implementation unavailable: M_offlineCloth");
}

TEST(OfflineModeFeedback, SuccessClearsOldRejection)
{
    UiViewModelSnapshot previous;
    previous.offlineMode = true;
    previous.offlineRequestedMode = 0;
    previous.offlineUiNotice = "Previous rejection";
    UiViewModelSnapshot next;
    next.offlineMode = false;
    next.offlineModeFeedback = OfflineModeCommandFeedback{0, {}};

    ReconcileOfflineModeFeedback(next, previous);

    EXPECT_FALSE(next.offlineMode);
    EXPECT_EQ(next.offlineRequestedMode, -1);
    EXPECT_TRUE(next.offlineUiNotice.empty());
}

TEST(OfflineModeFeedback, UnrelatedResultDoesNotConsumePendingRequest)
{
    UiViewModelSnapshot previous;
    previous.offlineMode = false;
    previous.offlineRequestedMode = 1;
    UiViewModelSnapshot next;
    next.offlineMode = false;
    next.offlineModeFeedback = OfflineModeCommandFeedback{
        0, "Unrelated rejection"};

    ReconcileOfflineModeFeedback(next, previous);

    EXPECT_EQ(next.offlineRequestedMode, 1);
    EXPECT_TRUE(next.offlineUiNotice.empty());
}

TEST(OfflineModeFeedback, NoPendingRequestIgnoresFeedback)
{
    UiViewModelSnapshot previous;
    previous.offlineUiNotice = "Existing setting notice";
    UiViewModelSnapshot next;
    next.offlineModeFeedback = OfflineModeCommandFeedback{
        -1, "Unrelated rejection"};

    ReconcileOfflineModeFeedback(next, previous);

    EXPECT_EQ(next.offlineRequestedMode, -1);
    EXPECT_EQ(next.offlineUiNotice, "Existing setting notice");
}
} // namespace VL
