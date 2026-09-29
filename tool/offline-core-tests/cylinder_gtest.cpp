#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "core/renderer.h"
#include "tableIIFixture.h"

namespace
{
using namespace VL::Offline;

Vec3 CylinderValue(const FabricMaterial& material, double normalDegrees,
                   double yarnDegrees)
{
    const double angle = normalDegrees * kPi / 180.0;
    Hit hit;
    hit.normal = hit.geometricNormal = {std::sin(angle), 0, std::cos(angle)};
    hit.dpdu = {0, 1, 0};
    hit.dpdv = {std::cos(angle), 0, -std::sin(angle)};
    hit = orientYarns(hit, nullptr, yarnDegrees);
    return microcylinderResponse(hit, {0, 0, 1}, {0, 0, 1}, material);
}

double Brightness(Vec3 value)
{
    return 0.2126 * value.x + 0.7152 * value.y + 0.0722 * value.z;
}

std::vector<double> HorizontalPeaks(const FabricMaterial& material)
{
    constexpr int steps = 641;
    std::vector<double> values;
    for (int i = 0; i < steps; ++i)
        values.push_back(Brightness(CylinderValue(material, -80 + i * 0.25, 90)));
    const double maximum = *std::max_element(values.begin(), values.end());
    std::vector<double> peaks;
    for (int i = 16; i < steps - 16; ++i)
    {
        const double angle = -80 + i * 0.25;
        if (std::abs(angle) > 60) continue;
        if (values[i] > values[i - 1] && values[i] >= values[i + 1] &&
            values[i] - std::max(values[i - 16], values[i + 16]) > maximum * 0.01)
            peaks.push_back(angle);
    }
    return peaks;
}

TEST(OfflineCylinder, TableIIHighlightsPreserveJournalPeakPositions)
{
    const auto& fabrics = VL::Offline::Test::TableIIFabrics();
    for (int index : {1, 2, 3})
    {
        FabricMaterial material = fabrics[index];
        for (auto& yarn : material.yarns) resampleYarn(yarn, 64);
        const auto peaks = HorizontalPeaks(material);
        const std::vector<double> expected =
            index == 1 ? std::vector<double>{-35, 35} :
            index == 2 ? std::vector<double>{-32, 0, 32} :
                         std::vector<double>{-30, -5, 5, 30};
        ASSERT_EQ(peaks.size(), expected.size()) << material.caseName;
        for (std::size_t i = 0; i < peaks.size(); ++i)
            EXPECT_NEAR(peaks[i], expected[i], 4.0) << material.caseName;
    }
}

TEST(OfflineCylinder, LinenYarnFramesRemainSymmetric)
{
    FabricMaterial material = VL::Offline::Test::TableIIFabrics()[0];
    for (int count : {8, 16, 32, 64})
    {
        for (auto& yarn : material.yarns) resampleYarn(yarn, count);
        for (int step = 0; step <= 160; ++step)
        {
            const double degrees = -80 + step;
            const Vec3 vertical = CylinderValue(material, degrees, 0);
            const Vec3 horizontal = CylinderValue(material, degrees, 90);
            EXPECT_TRUE(std::isfinite(vertical.x) && std::isfinite(vertical.y) &&
                        std::isfinite(vertical.z));
            EXPECT_GE(std::min({vertical.x, vertical.y, vertical.z}), 0);
            EXPECT_LT(length(vertical - horizontal), 1e-9);
        }
    }
}
} // namespace
