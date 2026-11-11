#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

#include "render/backend/resolvedRenderScene.h"

static_assert(
    std::is_same_v<
        decltype(VL::ResolvedDrawPacket::renderableObject),
        std::shared_ptr<RenderableObject>>,
    "ResolvedDrawPacket must hold a strong RenderableObject reference");

TEST(ResolvedDrawLifetime, CopiesKeepRenderableAliveUntilLastPacket)
{
    bool destroyed = false;
    {
        VL::ResolvedDrawPacket draw;
        draw.renderableObject = std::shared_ptr<RenderableObject>(
            reinterpret_cast<RenderableObject*>(1),
            [&destroyed](RenderableObject*)
            {
                destroyed = true;
            });

        VL::ResolvedDrawPacket copied = draw;
        draw.renderableObject.reset();
        EXPECT_FALSE(destroyed);
    }

    EXPECT_TRUE(destroyed);
}

