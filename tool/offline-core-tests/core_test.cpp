#include <cmath>

#include <gtest/gtest.h>

#include "core/renderer.h"
#include "tableIIFixture.h"

namespace
{
using namespace VL::Offline;

TEST(OfflineCore, JournalPresetsKeepDiscontinuousLengths)
{
    const auto& presets = VL::Offline::Test::TableIIFabrics();
    ASSERT_EQ(presets.size(), 6u);
    EXPECT_DOUBLE_EQ(presets[0].yarns[0].offsets[0], -25.0);
    EXPECT_DOUBLE_EQ(presets[3].yarns[0].lengths[3], 0.0);
    EXPECT_DOUBLE_EQ(presets[3].yarns[0].lengths[6], 0.67);
}

TEST(OfflineCore, AreaLightRejectsBackFace)
{
    Scene scene{};
    scene.lightCenter = {0, 2, 0};
    scene.lightWidth = scene.lightDepth = 1.0;
    aimAreaLight(scene, {0, 0, 0});

    // 单面发光的交点仍可被背面射线命中，但其立体角密度必须为零。
    EXPECT_GT(areaLightPdf(scene, {0, 0, 0}, {0, 1, 0}), 0.0);
    EXPECT_EQ(areaLightPdf(scene, {0, 4, 0}, {0, -1, 0}), 0.0);
}

TEST(OfflineCore, BKernelUsesFullSphereMeasure)
{
    Hit hit;
    hit.normal = hit.geometricNormal = {0, 1, 0};
    hit.dpdu = {1, 0, 0};
    hit.dpdv = {0, 0, 1};
    const auto& fabric = VL::Offline::Test::TableIIFabrics()[5];
    const Vec3 view = normalize({0.3, 0.9, 0.1});
    const Vec3 below = normalize({0.2, -0.3, 0.8});
    EXPECT_GT(microcylinderDirectionPdf(hit, view, below, fabric), 0.0);
    const Vec3 response = microcylinderResponse(hit, view, below, fabric);
    EXPECT_TRUE(std::isfinite(response.x));
    EXPECT_TRUE(std::isfinite(response.y));
    EXPECT_TRUE(std::isfinite(response.z));
}

TEST(OfflineCore, ColorSamplerPreservesTexelCentersAndWrap)
{
    ColorMap map;
    map.width = 2;
    map.height = 1;
    map.pixels = {{1, 0, 0}, {0, 1, 0}};
    map.repeat = true;
    map.linear = true;
    const Vec3 center = sampleColorMap(map, {0.25, 0.5});
    EXPECT_DOUBLE_EQ(center.x, 1.0);
    EXPECT_DOUBLE_EQ(center.y, 0.0);
    const Vec3 border = sampleColorMap(map, {0, 0.5});
    EXPECT_DOUBLE_EQ(border.x, 0.5);
    EXPECT_DOUBLE_EQ(border.y, 0.5);
    const Vec3 wrapped = sampleColorMap(map, {1.25, 0.5});
    EXPECT_DOUBLE_EQ(wrapped.x, center.x);
    map.repeat = false;
    EXPECT_DOUBLE_EQ(sampleColorMap(map, {0, 0.5}).x, 1.0);
    map.linear = false;
    EXPECT_DOUBLE_EQ(sampleColorMap(map, {0.76, 0.5}).y, 1.0);
}
} // namespace
