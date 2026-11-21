#include <gtest/gtest.h>
#include "terrain/terrainCompositedAdjacency.h"
#include "render/resource/terrainCompositedBakeAdjacency.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "../validation/terrain/composited_adjacency_cases.h"
#include "../validation/terrain/composited_page_reference.h"
#include "../validation/terrain/composited_cpu_address_oracle.h"

namespace
{
using namespace VL;

void Horizontal(std::vector<TerrainCompositedAdjacencyLink>& links,
    const std::string& first, const std::string& second)
{
    links.push_back({first, second, TerrainCompositedSurfaceEdge::MaxU,
        TerrainCompositedSurfaceEdge::MinU, {0, 1}, {0, 1}, {1, 0, -1, 0, 1, 0}});
    links.push_back({second, first, TerrainCompositedSurfaceEdge::MinU,
        TerrainCompositedSurfaceEdge::MaxU, {0, 1}, {0, 1}, {1, 0, 1, 0, 1, 0}});
}

void Vertical(std::vector<TerrainCompositedAdjacencyLink>& links,
    const std::string& first, const std::string& second)
{
    links.push_back({first, second, TerrainCompositedSurfaceEdge::MaxV,
        TerrainCompositedSurfaceEdge::MinV, {0, 1}, {0, 1}, {1, 0, 0, 0, 1, -1}});
    links.push_back({second, first, TerrainCompositedSurfaceEdge::MinV,
        TerrainCompositedSurfaceEdge::MaxV, {0, 1}, {0, 1}, {1, 0, 0, 0, 1, 1}});
}
}

TEST(TerrainCompositedAdjacency, CoarseFootprintTraversesMoreThanNineSurfaces)
{
    std::vector<std::string> ids;
    std::vector<TerrainCompositedAdjacencyLink> links;
    for (uint32_t index = 0; index < 20; ++index)
    {
        ids.push_back("tile-" + std::to_string(index));
        if (index > 0)
            Horizontal(links, ids[index - 1], ids[index]);
    }
    TerrainCompositedAdjacencyGraph graph(ids, links);
    const auto result = graph.Resolve(ids.front(), {18.25, 0.3});
    EXPECT_EQ(result.surfaceId, ids[18]);
    EXPECT_EQ(result.hops, 18u);
    EXPECT_DOUBLE_EQ(result.uv[0], 0.25);
    EXPECT_FALSE(result.clampedAtWorldBoundary);
    const auto reverse = graph.Resolve(ids.back(), {-18.75, 0.3});
    EXPECT_EQ(reverse.surfaceId, ids.front());
    EXPECT_DOUBLE_EQ(reverse.uv[0], 0.25);
}

TEST(TerrainCompositedAdjacency, ExportedBakeChartsMatchResolveAcrossCoarseStoredPage)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    Horizontal(links, "c", "d");
    Vertical(links, "a", "c");
    Vertical(links, "b", "d");
    TerrainCompositedAdjacencyGraph graph({"a", "b", "c", "d", "disconnected"}, links);
    ASSERT_EQ(graph.StaticCharts().size(), 5u);
    const auto charts = VL::TerrainBakeAdjacency::Prepare(graph, "a");
    ASSERT_EQ(charts.size(), 4u);
    for (uint32_t pixelY = 0; pixelY < 136; ++pixelY)
        for (uint32_t pixelX = 0; pixelX < 136; ++pixelX)
        {
            const std::array<double, 2> uv{double(pixelX) - 3.875, double(pixelY) - 3.875};
            const auto expected = graph.Resolve("a", uv);
            double best = std::numeric_limits<double>::infinity();
            std::string selected;
            for (const auto& chart : charts)
            {
                const auto& mapping = chart.sourceToTarget;
                const auto& metric = chart.targetToComponent;
                const double mappedU = mapping[0] * uv[0] + mapping[1] * uv[1] + mapping[2];
                const double mappedV = mapping[3] * uv[0] + mapping[4] * uv[1] + mapping[5];
                const double deltaU = std::clamp(mappedU, 0., 1.) - mappedU;
                const double deltaV = std::clamp(mappedV, 0., 1.) - mappedV;
                const double distance = std::hypot(metric[0] * deltaU + metric[1] * deltaV,
                    metric[3] * deltaU + metric[4] * deltaV);
                if (distance < best)
                {
                    best = distance;
                    selected = chart.surfaceId;
                }
            }
            EXPECT_EQ(selected, expected.surfaceId);
        }
}

TEST(TerrainCompositedAdjacency, FloatRoutesMatchDoubleOracleAtBoundariesAndPartialRotatedScaledCharts)
{
    const auto queries = TerrainBakeRouteCases::Queries();
    for (const auto& graph : TerrainBakeRouteCases::Graphs())
        for (const auto& source : graph.SurfaceIds())
        {
            const auto charts = VL::TerrainBakeAdjacency::Prepare(graph, source);
            std::vector<VL::TerrainBakeAdjacency::PackedChart> packed;
            uint32_t sourceIndex = 0;
            for (uint32_t index = 0; index < charts.size(); ++index)
            {
                packed.push_back(VL::TerrainBakeAdjacency::Pack(charts[index]));
                if (charts[index].surfaceId == source)
                    sourceIndex = index;
            }
            for (const auto& query : queries)
            {
                const auto expected = graph.Resolve(source, {query[0], query[1]});
                const auto selected = VL::TerrainBakeAdjacency::ResolveFloat(packed, sourceIndex, query);
                ASSERT_EQ(charts[selected].surfaceId, expected.surfaceId)
                    << source << " uv=" << query[0] << "," << query[1];
            }
        }
}

TEST(TerrainCompositedAdjacency, PackedChartsRejectNonFiniteSingularAndDistanceOverflow)
{
    using namespace VL::TerrainBakeAdjacency;
    Chart chart{"a", {1, 0, 0, 0, 1, 0}, {1, 0, 0, 0, 1, 0}};
    chart.sourceToTarget[2] = std::numeric_limits<double>::infinity();
    EXPECT_THROW(Pack(chart), std::runtime_error);
    chart.sourceToTarget = {1.e-100, 0, 0, 0, 1, 0};
    EXPECT_THROW(Pack(chart), std::runtime_error);
    chart.sourceToTarget = {1.e30, 0, 0, 0, 1, 0};
    EXPECT_THROW(Pack(chart), std::runtime_error);
    chart.sourceToTarget = {1, 0, 0, 0, 1, 0};
    chart.targetToComponent = {0, 0, 0, 0, 1, 0};
    EXPECT_THROW(Pack(chart), std::runtime_error);
    EXPECT_THROW(ResolveFloat({}, 0, {0, 0}), std::runtime_error);
}

TEST(TerrainCompositedAdjacency, RepeatIsExplicitlyAllowedOnlyForIsolatedComponents)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    TerrainCompositedAdjacencyGraph graph({"a", "b", "isolated"}, links);
    EXPECT_NO_THROW(VL::TerrainBakeAdjacency::ValidateAddressMode(graph, "isolated", true));
    EXPECT_NO_THROW(VL::TerrainBakeAdjacency::ValidateAddressMode(graph, "a", false));
    EXPECT_THROW(VL::TerrainBakeAdjacency::ValidateAddressMode(graph, "a", true), std::runtime_error);
    EXPECT_THROW(VL::TerrainBakeAdjacency::ValidateAddressMode(graph, "b", true), std::runtime_error);
    TerrainCompositedAdjacencyGraph singleton({"only"}, {});
    EXPECT_NO_THROW(VL::TerrainBakeAdjacency::ValidateAddressMode(singleton, "only", true));
}

TEST(TerrainCompositedAdjacency, RectangularOptimizationRequiresACompletelyCoveredComponent)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    Vertical(links, "a", "c");
    TerrainCompositedAdjacencyGraph graph({"a", "b", "c"}, links);
    auto charts = VL::TerrainBakeAdjacency::Prepare(graph, "a");
    std::vector<VL::TerrainBakeAdjacency::PackedChart> packed;
    for (const auto& chart : charts)
        packed.push_back(VL::TerrainBakeAdjacency::Pack(chart));
    EXPECT_FALSE(VL::TerrainBakeAdjacency::RectangularBounds(charts, packed));
    Horizontal(links, "c", "d");
    Vertical(links, "b", "d");
    graph = TerrainCompositedAdjacencyGraph({"a", "b", "c", "d"}, links);
    charts = VL::TerrainBakeAdjacency::Prepare(graph, "a");
    packed.clear();
    for (const auto& chart : charts)
        packed.push_back(VL::TerrainBakeAdjacency::Pack(chart));
    ASSERT_TRUE(VL::TerrainBakeAdjacency::RectangularBounds(charts, packed));
    EXPECT_EQ(packed.front()[10], 0);
    EXPECT_EQ(packed.front()[11], 0);
    EXPECT_EQ(packed.front()[14], 2);
    EXPECT_EQ(packed.front()[15], 2);
}

TEST(TerrainCompositedAdjacency, ConsumerBilinearSupportNeverReachesCanonicalPadding)
{
    for (uint32_t extent : {1u, 2u, 7u, 127u, 128u, 129u, 259u, 8192u})
        for (uint32_t centerPage = 0; centerPage < (extent + 127) / 128; ++centerPage)
        {
            const uint32_t valid = std::min(128u, extent - centerPage * 128);
            const uint32_t evaluationExtent = valid + 8;
            for (int sample = -2048; sample <= 2048; ++sample)
            {
                const double tapUv = double(sample) / 256;
                const double globalTexel = std::clamp(
                    std::clamp(tapUv, 0., 1.) * extent, .5, double(extent) - .5);
                const double local = globalTexel - centerPage * 128;
                if (local < -3.5 || local > 131.5)
                    continue;
                const double samplerPosition = local + 4 - .5;
                const double first = std::floor(samplerPosition);
                const double last = first + (samplerPosition > first ? 1 : 0);
                EXPECT_GE(first, 0) << extent << " " << centerPage;
                EXPECT_LT(last, evaluationExtent) << extent << " " << centerPage;
            }
        }
}

TEST(TerrainCompositedAdjacency, Repeat129FractionalMipFootprintReadsStoredNineAndTen)
{
    const double centerUv = 128.75 / 129;
    const double major = std::hypot(12., 0.);
    const double minor = 1.;
    const double filteredMinor = std::max(minor, major / 8);
    const double targetMip = std::log2(filteredMinor);
    const uint32_t taps = uint32_t(std::ceil(major / std::max(filteredMinor, 1.)));
    ASSERT_EQ(taps, 8u);
    EXPECT_NEAR(targetMip, .5849625007211562, 1.e-14);
    EXPECT_GT(1 - (targetMip - std::floor(targetMip)), 0);
    for (uint32_t mip : {0u, 1u})
    {
        const uint32_t extent = 129 >> mip;
        const double centerTexel = (centerUv - std::floor(centerUv)) * extent;
        const uint32_t page = uint32_t(std::floor(centerTexel / 128));
        for (uint32_t tap = 0; tap < taps; ++tap)
        {
            const double tapUv = centerUv + 12. / 129 * ((tap + .5) / taps - .5);
            double delta = tapUv - centerUv;
            delta -= std::round(delta);
            double local = centerTexel + delta * extent - page * 128;
            if (local < -3.5 || local > 131.5)
            {
                const double wrappedTexel = (tapUv - std::floor(tapUv)) * extent;
                local = wrappedTexel - std::floor(wrappedTexel / 128) * 128;
            }
            ASSERT_GE(local, -3.5);
            ASSERT_LE(local, 131.5);
            const double samplePosition = local + 3.5;
            const double first = std::floor(samplePosition);
            const double fraction = samplePosition - first;
            EXPECT_GE(first, 0);
            EXPECT_LT(first + (fraction > 0 ? 1 : 0), 136);
            if (mip == 0 && tap == 7)
            {
                EXPECT_NEAR(local, 6, 1.e-12);
                EXPECT_EQ(first, 9);
                EXPECT_NEAR(fraction, .5, 1.e-12);
                EXPECT_GT(first, 8);
                EXPECT_DOUBLE_EQ(.5 * 129 + 3.5, 68);
                EXPECT_NEAR((1 - targetMip) / taps * fraction,
                    .02593984370492774, 1.e-14);
            }
        }
    }
}

TEST(TerrainCompositedPageReference, SourceFilteringDecodesSrgbBeforeInterpolation)
{
    TerrainPageReference::Image image;
    image.width = 2;
    image.srgb = true;
    image.planes = {{{0, 0, 0, 1}, {1, 1, 1, 1}}};
    const auto sample = image.Sample(.5, .5, 0);
    EXPECT_DOUBLE_EQ(sample[0], .5);
    EXPECT_GT(TerrainPageReference::SrgbEncode(sample[0]), .7);
}

TEST(TerrainCompositedPageReference, FractionalLodUsesBothActualNonSquareMipExtents)
{
    TerrainPageReference::Image image;
    image.width = 5;
    image.height = 3;
    image.mips = 3;
    image.repeat = true;
    image.planes = {
        std::vector<TerrainPageReference::Colour>(15, {0, 0, 0, 1}),
        {{.2, .4, .6, 1}, {.8, .6, .4, 1}},
        {{1, 1, 1, 1}}};
    const auto mipOne = image.Sample(.5, .5, 0, 1);
    const auto half = image.Sample(.5, .5, 0, 1.5);
    EXPECT_NEAR(mipOne[0], .5, 1.e-14);
    EXPECT_NEAR(mipOne[1], .5, 1.e-14);
    EXPECT_NEAR(half[0], .75, 1.e-14);
    EXPECT_NEAR(half[1], .75, 1.e-14);
    EXPECT_EQ(image.Sample(.5, .5, 0, 100), image.planes[2][0]);
    EXPECT_EQ(image.Sample(.5, .5, 0, -100), image.planes[0][0]);
}

TEST(TerrainCompositedPageReference, RepeatFilteringWrapsNegativeUvAndTexelCenters)
{
    TerrainPageReference::Image image;
    image.width = 3;
    image.repeat = true;
    image.planes = {{{.1, 0, 0, 1}, {.4, 0, 0, 1}, {.9, 0, 0, 1}}};
    EXPECT_NEAR(image.Sample(0, .5, 0)[0], .5, 1.e-14);
    EXPECT_NEAR(image.Sample(1, .5, 0)[0], .5, 1.e-14);
    EXPECT_NEAR(image.Sample(-1, .5, 0)[0], .5, 1.e-14);
    EXPECT_NEAR(image.Sample(-.5, .5, 0)[0], .4, 1.e-14);
    EXPECT_NEAR(image.Sample(1. / 6, .5, 0)[0], .1, 1.e-14);
    EXPECT_NEAR(image.Sample(5. / 6, .5, 0)[0], .9, 1.e-14);
}

TEST(TerrainCompositedPageReference, TrilinearRoughnessFilteringPrecedesAreaRms)
{
    TerrainPageReference::Image image;
    image.width = image.height = 2;
    image.mips = 2;
    image.planes = {std::vector<TerrainPageReference::Colour>(4, {1, .2, 0, 1}),
        {{1, .8, 0, 1}}};
    const double first = image.Sample(.5, .5, 0, .5)[1];
    const double second = image.Sample(.5, .5, 0, 1)[1];
    EXPECT_NEAR(first, .5, 1.e-14);
    EXPECT_NEAR(std::sqrt((first * first + second * second) / 2), std::sqrt(.445), 1.e-14);
    EXPECT_NE(std::sqrt((first * first + second * second) / 2), (first + second) / 2);
}

TEST(TerrainCompositedPageReference, CoverageReconstructsLayerIdentityBeforeRanking)
{
    TerrainPageReference::Surface surface;
    TerrainPageReference::Image ids;
    ids.width = 2;
    ids.planes = {{{3. / 255, 7. / 255, 0, 0}, {7. / 255, 3. / 255, 0, 0}}};
    auto weights = ids;
    weights.planes = {{{.75, .25, 0, 0}, {.75, .25, 0, 0}}};
    surface.images["ids"] = ids;
    surface.images["weights"] = weights;
    std::array<uint32_t, 4> selected{};
    std::array<double, 4> mass{};
    TerrainPageReference::Coverage(surface, {.5, .5}, selected, mass);
    EXPECT_EQ(selected[0], 3u);
    EXPECT_EQ(selected[1], 7u);
    EXPECT_DOUBLE_EQ(mass[0], .5);
    EXPECT_DOUBLE_EQ(mass[1], .5);
}

TEST(TerrainCompositedPageReference, RootRetainsRealNeighborBorderBeforeCanonicalPadding)
{
    using namespace TerrainPageReference;
    const auto solid = [](const Colour& colour) {
        Image image;
        image.planes = {{colour}};
        return image;
    };
    Surface first;
    first.input.globalToSourceSlice[3] = 0;
    first.images["ids"] = solid({3. / 255, 0, 0, 0});
    first.images["weights"] = solid({1, 0, 0, 0});
    first.images["frame-normal"] = solid({0, 1, 0, 1});
    first.images["frame-tangent"] = solid({1, 0, 0, 1});
    first.images["albedo"] = solid({128. / 255, 0, 0, 1});
    first.images["albedo"].srgb = true;
    first.images["normal"] = solid({.5, .5, 1, 1});
    first.images["orm"] = solid({1, .5, 0, 1});
    auto catalog = solid({0, 0, 1, 0});
    catalog.width = 256;
    catalog.height = 4;
    catalog.planes[0].resize(256 * 4, {0, 0, 1, 0});
    first.images["catalog"] = catalog;
    Surface second = first;
    second.images["albedo"] = solid({0, 0, 192. / 255, 1});
    second.images["albedo"].srgb = true;
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    TerrainCompositedAdjacencyGraph graph({"a", "b"}, links);
    Baker::PageRequest page;
    page.virtualWidth = page.virtualHeight = 1;
    const auto bytes = Page(graph, {{"a", first}, {"b", second}}, "a", page);
    const auto component = [&](uint32_t column, uint32_t channel) {
        return bytes[(4 * 136 + column) * 4 + channel];
    };
    EXPECT_EQ(component(4, 0), 128);
    EXPECT_EQ(component(4, 2), 0);
    EXPECT_EQ(component(5, 0), 0);
    EXPECT_EQ(component(5, 2), 192);
    EXPECT_EQ(component(135, 2), component(8, 2));
    EXPECT_EQ(component(0, 0), 128);
}

TEST(TerrainCompositedAdjacency, NormalConversionMatchesInverseTransposeDirectionsWithoutTranslation)
{
    const std::array<float, 16> source{0, 0, -2, 0, 0, 2, 0, 0, 2, 0, 0, 0, 17, 83, -42, 1};
    const std::array<float, 16> target{4, 0, 0, 0, 0, 0, 4, 0, 0, -4, 0, 0, -900, 10, 7, 1};
    const auto matrix = VL::TerrainBakeAdjacency::NormalToSource(source, target);
    const std::array<float, 3> normal{.3f, .4f, .5f};
    std::array<double, 3> transformed{}, expected{};
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t column = 0; column < 3; ++column)
        {
            transformed[row] += matrix[column * 4 + row] * normal[column];
            for (uint32_t axis = 0; axis < 3; ++axis)
                expected[row] += double(source[row * 4 + axis]) *
                    target[column * 4 + axis] * normal[column] / 16.;
        }
    double actualLength = 0, expectedLength = 0;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        actualLength += transformed[axis] * transformed[axis];
        expectedLength += expected[axis] * expected[axis];
    }
    for (uint32_t axis = 0; axis < 3; ++axis)
        EXPECT_NEAR(transformed[axis] / std::sqrt(actualLength),
            expected[axis] / std::sqrt(expectedLength), 1.e-7);
    EXPECT_EQ(matrix[12], 0);
    EXPECT_EQ(matrix[13], 0);
    EXPECT_EQ(matrix[14], 0);
}

TEST(TerrainCompositedAdjacency, IndependentCpuGpuOracleUsesDoubleRoutesAndCofactorNormals)
{
    using Baker = VL::TerrainCompositedBakeGpu;
    Baker::AdjacencySnapshot snapshot;
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    Vertical(links, "a", "c");
    Horizontal(links, "c", "d");
    Vertical(links, "b", "d");
    snapshot.graph = TerrainCompositedAdjacencyGraph({"a", "b", "c", "d", "isolated"}, links);
    for (const auto& id : snapshot.graph.SurfaceIds())
        snapshot.surfaces.push_back({id, Baker::InputSnapshot{}});
    Baker::PageRequest page;
    page.virtualWidth = page.virtualHeight = 1;
    const auto values = TerrainCpuAddressOracle::Build(snapshot, "a", page, 1, 1);
    ASSERT_EQ(values.size(), 9u * 9 * 16 * 5);
    const uint32_t sample = 0;
    const uint32_t offset = ((5 * 9 + 5) * 16 + sample) * 5;
    EXPECT_EQ(values[offset][2], 3);
    EXPECT_FLOAT_EQ(values[offset][0], .125f);
    EXPECT_FLOAT_EQ(values[offset][1], .125f);
    EXPECT_FLOAT_EQ(values[offset + 1][0], .25f);
    EXPECT_FLOAT_EQ(values[offset + 1][3], .25f);
    const std::array<float, 16> source{0, 0, -2, 0, 0, 2, 0, 0, 2, 0, 0, 0, 17, 83, -42, 1};
    const std::array<float, 16> target{4, 0, 0, 0, 0, 0, 4, 0, 0, -4, 0, 0, -900, 10, 7, 1};
    const auto normal = TerrainCpuAddressOracle::NormalMatrix(source, target);
    EXPECT_DOUBLE_EQ(normal[0], 0);
    EXPECT_DOUBLE_EQ(normal[3], 0);
    EXPECT_DOUBLE_EQ(normal[6], .5);
    EXPECT_DOUBLE_EQ(normal[1], -.5);
}

TEST(TerrainCompositedAdjacency, IndependentOracleIdentityRecordsUseFiveVec4AndOriginalSampleOrder)
{
    using Baker = VL::TerrainCompositedBakeGpu;
    Baker::AdjacencySnapshot snapshot;
    snapshot.graph = TerrainCompositedAdjacencyGraph({"identity"}, {});
    snapshot.surfaces.push_back({"identity", Baker::InputSnapshot{}});
    Baker::PageRequest page;
    page.virtualWidth = page.virtualHeight = 256;
    page.areaSamplesPerAxis = 4;
    const auto values = TerrainCpuAddressOracle::Build(snapshot, "identity", page, 256, 256);
    ASSERT_EQ(values.size(), 136u * 136 * 16 * 5);
    for (uint32_t sample = 0; sample < 16; ++sample)
    {
        const size_t offset = ((size_t(7) * 136 + 9) * 16 + sample) * 5;
        EXPECT_FLOAT_EQ(values[offset][0], float((5. + (sample % 4 + .5) / 4) / 256));
        EXPECT_FLOAT_EQ(values[offset][1], float((3. + (sample / 4 + .5) / 4) / 256));
        EXPECT_EQ(values[offset][2], 0);
        EXPECT_FLOAT_EQ(values[offset + 1][0], 1.f / 1024);
        EXPECT_FLOAT_EQ(values[offset + 1][1], 0);
        EXPECT_FLOAT_EQ(values[offset + 1][2], 0);
        EXPECT_FLOAT_EQ(values[offset + 1][3], 1.f / 1024);
        EXPECT_EQ(values[offset + 2], (std::array<float, 4>{1, 0, 0, 0}));
        EXPECT_EQ(values[offset + 3], (std::array<float, 4>{0, 1, 0, 0}));
        EXPECT_EQ(values[offset + 4], (std::array<float, 4>{0, 0, 1, 0}));
    }
}

TEST(TerrainCompositedAdjacency, FourCornerPathsAgreeAndExternalBoundaryClamps)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    Horizontal(links, "c", "d");
    Vertical(links, "a", "c");
    Vertical(links, "b", "d");
    TerrainCompositedAdjacencyGraph graph({"a", "b", "c", "d"}, links);
    const auto corner = graph.Resolve("a", {1.25, 1.25});
    EXPECT_EQ(corner.surfaceId, "d");
    EXPECT_DOUBLE_EQ(corner.uv[0], 0.25);
    EXPECT_DOUBLE_EQ(corner.uv[1], 0.25);
    const auto outside = graph.Resolve("a", {4.25, 1.25});
    EXPECT_EQ(outside.surfaceId, "d");
    EXPECT_TRUE(outside.clampedAtWorldBoundary);
    EXPECT_DOUBLE_EQ(outside.uv[0], 1);
    links.back().sourceUvToTargetUv[2] = 0.25;
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a", "b", "c", "d"}, links), std::exception);
}

TEST(TerrainCompositedAdjacency, RejectsMissingInverseUnknownDuplicateAndSingularLinks)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    auto malformed = links;
    malformed.pop_back();
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a", "b"}, malformed), std::exception);
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a"}, links), std::exception);
    malformed = links;
    malformed.push_back(links.front());
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a", "b"}, malformed), std::exception);
    malformed = links;
    malformed[0].sourceUvToTargetUv[0] = 0;
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a", "b"}, malformed), std::exception);
}

TEST(TerrainCompositedAdjacency, DisconnectedPlacementIsNeverInferredAsNeighbor)
{
    TerrainCompositedAdjacencyGraph graph({"a", "b"}, {});
    const auto result = graph.Resolve("a", {1.5, 0.5});
    EXPECT_EQ(result.surfaceId, "a");
    EXPECT_TRUE(result.clampedAtWorldBoundary);
    EXPECT_THROW(graph.Resolve("missing", {0, 0}), std::exception);
    EXPECT_THROW(graph.Resolve("a", {std::numeric_limits<double>::infinity(), 0}), std::exception);
    EXPECT_THROW(graph.Validate({"a"}), std::exception);
}

TEST(TerrainCompositedAdjacency, SplitEdgesCarryNonUnitFootprintJacobian)
{
    std::vector<TerrainCompositedAdjacencyLink> links{
        {"a", "b", TerrainCompositedSurfaceEdge::MaxU, TerrainCompositedSurfaceEdge::MinU,
            {0, 0.5}, {0, 1}, {1, 0, -1, 0, 2, 0}},
        {"b", "a", TerrainCompositedSurfaceEdge::MinU, TerrainCompositedSurfaceEdge::MaxU,
            {0, 1}, {0, 0.5}, {1, 0, 1, 0, 0.5, 0}},
        {"a", "c", TerrainCompositedSurfaceEdge::MaxU, TerrainCompositedSurfaceEdge::MinU,
            {0.5, 1}, {0, 1}, {1, 0, -1, 0, 2, -1}},
        {"c", "a", TerrainCompositedSurfaceEdge::MinU, TerrainCompositedSurfaceEdge::MaxU,
            {0, 1}, {0.5, 1}, {1, 0, 1, 0, 0.5, 0.5}},
        {"b", "c", TerrainCompositedSurfaceEdge::MaxV, TerrainCompositedSurfaceEdge::MinV,
            {0, 1}, {0, 1}, {1, 0, 0, 0, 1, -1}},
        {"c", "b", TerrainCompositedSurfaceEdge::MinV, TerrainCompositedSurfaceEdge::MaxV,
            {0, 1}, {0, 1}, {1, 0, 0, 0, 1, 1}}};
    TerrainCompositedAdjacencyGraph graph({"a", "b", "c"}, links);
    const auto address = graph.Resolve("a", {1.2, 0.75});
    EXPECT_EQ(address.surfaceId, "c");
    EXPECT_DOUBLE_EQ(address.uv[1], 0.5);
    EXPECT_DOUBLE_EQ(address.originalUvToResolvedUvJacobian[3], 2);
    links.resize(4);
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a", "b", "c"}, links), std::exception);
}

TEST(TerrainCompositedAdjacency, RotatedAndReflectedEdgeCoordinatesRetainTheirJacobians)
{
    std::vector<TerrainCompositedAdjacencyLink> rotated{
        {"a", "b", TerrainCompositedSurfaceEdge::MaxU, TerrainCompositedSurfaceEdge::MinV,
            {0, 1}, {0, 1}, {0, 1, 0, 1, 0, -1}},
        {"b", "a", TerrainCompositedSurfaceEdge::MinV, TerrainCompositedSurfaceEdge::MaxU,
            {0, 1}, {0, 1}, {0, 1, 1, 1, 0, 0}}};
    TerrainCompositedAdjacencyGraph graph({"a", "b"}, rotated);
    const auto address = graph.Resolve("a", {1.2, 0.7});
    EXPECT_EQ(address.surfaceId, "b");
    EXPECT_DOUBLE_EQ(address.uv[0], 0.7);
    EXPECT_NEAR(address.uv[1], 0.2, 1.e-12);
    EXPECT_EQ(address.originalUvToResolvedUvJacobian, (std::array<double, 4>{0, 1, 1, 0}));
    std::vector<TerrainCompositedAdjacencyLink> reflected{
        {"a", "b", TerrainCompositedSurfaceEdge::MaxU, TerrainCompositedSurfaceEdge::MinU,
            {0, 1}, {0, 1}, {1, 0, -1, 0, -1, 1}},
        {"b", "a", TerrainCompositedSurfaceEdge::MinU, TerrainCompositedSurfaceEdge::MaxU,
            {0, 1}, {0, 1}, {1, 0, 1, 0, -1, 1}}};
    graph = TerrainCompositedAdjacencyGraph({"a", "b"}, reflected);
    const auto mirrored = graph.Resolve("a", {1.2, 0.7});
    EXPECT_NEAR(mirrored.uv[1], 0.3, 1.e-12);
    EXPECT_DOUBLE_EQ(mirrored.originalUvToResolvedUvJacobian[3], -1);
}

TEST(TerrainCompositedAdjacency, IndividuallyInvertibleLinksCannotDisagreeAroundACornerCycle)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    Horizontal(links, "c", "d");
    Vertical(links, "a", "c");
    Vertical(links, "b", "d");
    links[4].sourceUvToTargetUv = {-1, 0, 1, 0, 1, -1};
    links[5].sourceUvToTargetUv = {-1, 0, 1, 0, 1, 1};
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a", "b", "c", "d"}, links), std::exception);
}

TEST(TerrainCompositedAdjacency, NonFiniteTransformAndOutOfRangeIntervalNeverBuild)
{
    std::vector<TerrainCompositedAdjacencyLink> links;
    Horizontal(links, "a", "b");
    auto changed = links;
    changed[0].sourceUvToTargetUv[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a", "b"}, changed), std::exception);
    changed = links;
    changed[0].sourceEdgeInterval = {-0.1, 1};
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a", "b"}, changed), std::exception);
    changed = links;
    changed[0].sourceEdgeInterval = {0.5, 0.5};
    EXPECT_THROW(TerrainCompositedAdjacencyGraph({"a", "b"}, changed), std::exception);
}
