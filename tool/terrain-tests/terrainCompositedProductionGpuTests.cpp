#include <gtest/gtest.h>

#include "render/resource/terrainCompositedProductionGpu.h"
#include "resource/image/hostImageVulkanPlan.h"
#include "resource/device/deviceTextureFactory.h"

#include <cstring>
#include <chrono>

namespace
{
VL::TerrainCompositedProductionObject Object(const std::string& format = "r32-float")
{
    VL::TerrainCompositedProductionObject object;
    object.objectId = "frozen";
    object.relativePath = "objects/frozen.bin";
    object.image.format = format;
    object.image.width = object.image.height = 4;
    object.image.layers = 2;
    return object;
}

TEST(TerrainCompositedProductionGpu, FloatUploadUsesR32WithoutConversion)
{
    auto object = Object();
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    image.data.resize(128);
    EXPECT_EQ(HostImageVulkan::ToVulkanFormat(image.format), vk::Format::eR32Sfloat);
    const auto regions = HostImageVulkan::BuildUploadRegions(image);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions[0].bufferOffset, 0u);
    EXPECT_EQ(regions[1].bufferOffset, 64u);
    EXPECT_EQ(regions[1].imageSubresource.baseArrayLayer, 1u);
    EXPECT_EQ(regions[1].imageExtent.width, 4u);
}

VL::TerrainRvtBudgetCaps Caps(uint64_t cpu)
{
    VL::TerrainRvtBudgetCaps caps;
    caps.totalCpuBytes = cpu + VL::TerrainRvtBudgetOwner::MetadataBytesPerLease;
    caps.workspaceCpuBytes = caps.totalCpuBytes;
    return caps;
}
}

TEST(TerrainCompositedProductionGpu, FrozenFloatWeightsPreserveEveryBitAndLayer)
{
    auto object = Object();
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    image.data.resize(128);
    for (size_t index = 0; index < 32; ++index)
    {
        const float value = float(index) / 31.f;
        std::memcpy(image.data.data() + index * 4, &value, 4);
    }
    VL::TerrainRvtBudgetOwner budget(Caps(128));
    std::vector<uint8_t> stored;
    object = VL::TerrainCompositedProductionGpu::CaptureCpuImage(object, image, budget,
        [&](const auto& metadata, const auto& payload)
        {
            EXPECT_EQ(metadata.bytes, 128u);
            EXPECT_EQ(budget.Statistics().current.cpuBytes,
                128u + VL::TerrainRvtBudgetOwner::MetadataBytesPerLease);
            stored = payload;
        });
    const auto restored = VL::TerrainCompositedProductionGpu::DecodeImage(object, stored);
    EXPECT_EQ(restored.data, image.data);
    EXPECT_EQ(restored.arrayLayers, 2u);
    EXPECT_EQ(restored.format, HostImage::PixelFormat::R32_FLOAT);
    EXPECT_EQ(budget.Statistics().current.cpuBytes, 0u);
}

TEST(TerrainCompositedProductionGpu, PaddedFrameRowsFreezeWithoutPaddingOrVerticalFlip)
{
    auto object = Object("rgba32-float");
    object.image.layers = 1;
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    image.rowStrideBytes = 80;
    image.data.resize(320, 255);
    for (uint32_t row = 0; row < 4; ++row)
        std::memset(image.data.data() + row * 80, int(row), 64);
    VL::TerrainRvtBudgetOwner budget(Caps(256));
    std::vector<uint8_t> stored;
    VL::TerrainCompositedProductionGpu::CaptureCpuImage(object, image, budget,
        [&](const auto&, const auto& payload) { stored = payload; });
    ASSERT_EQ(stored.size(), 256u);
    for (size_t index = 0; index < stored.size(); ++index)
        EXPECT_EQ(stored[index], index / 64);
}

TEST(TerrainCompositedProductionGpu, CorruptOrTruncatedImageIsRejected)
{
    auto object = Object();
    std::vector<uint8_t> bytes(128, 0);
    object.bytes = bytes.size();
    object.digest = VL::ContentHasher::HashBytes(bytes.data(), bytes.size());
    EXPECT_NO_THROW(VL::TerrainCompositedProductionGpu::ValidatePayload(object, bytes));
    bytes[33] = 1;
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::DecodeImage(object, bytes), std::runtime_error);
    bytes.pop_back();
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::DecodeImage(object, bytes), std::runtime_error);
}

TEST(TerrainCompositedProductionGpu, BlockMipChainSurvivesFreezeAndRestore)
{
    auto object = Object("bc7-srgb");
    object.image.width = 7;
    object.image.height = 5;
    object.image.mipLevels = 3;
    object.image.transfer = VL::TerrainCompositedProductionTransfer::Srgb;
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    image.data.resize(static_cast<size_t>(object.image.ByteSize()));
    for (size_t index = 0; index < image.data.size(); ++index)
        image.data[index] = uint8_t(index);
    VL::TerrainRvtBudgetOwner budget(Caps(image.data.size()));
    std::vector<uint8_t> bytes;
    object = VL::TerrainCompositedProductionGpu::CaptureCpuImage(object, image, budget,
        [&](const auto&, const auto& payload) { bytes = payload; });
    const auto restored = VL::TerrainCompositedProductionGpu::DecodeImage(object, std::move(bytes));
    EXPECT_EQ(restored.data, image.data);
    EXPECT_EQ(restored.GetMipOffset(2, 1), image.GetMipOffset(2, 1));
}

TEST(TerrainCompositedProductionGpu, AdmissionFailureNeverInvokesWriter)
{
    auto object = Object();
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    image.data.resize(128);
    VL::TerrainRvtBudgetOwner budget(Caps(127));
    bool written = false;
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::CaptureCpuImage(object, image, budget,
        [&](const auto&, const auto&) { written = true; }), std::runtime_error);
    EXPECT_FALSE(written);
    EXPECT_EQ(budget.Statistics().current.cpuBytes, 0u);
}

TEST(TerrainCompositedProductionGpu, FailedPublishReleasesTransferBudget)
{
    auto object = Object();
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    image.data.resize(128);
    VL::TerrainRvtBudgetOwner budget(Caps(128));
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::CaptureCpuImage(object, image, budget,
        [](const auto&, const auto&) { throw std::runtime_error("publish failed"); }), std::runtime_error);
    EXPECT_EQ(budget.Statistics().current.cpuBytes, 0u);
}

TEST(TerrainCompositedProductionGpu, SourceMapPreservesReorderedLocalGlobalAndSliceIdentities)
{
    VL::TerrainCompositedSurfaceProduction surface;
    surface.localToGlobal = {19, 3};
    surface.globalToSourceSlice[19] = 1;
    surface.globalToSourceSlice[3] = 0;
    const auto image = VL::TerrainCompositedProductionGpu::SourceMapImage(surface);
    const auto entry = [&](uint32_t row, uint32_t column)
    {
        std::array<float, 4> value{};
        std::memcpy(value.data(), image.data.data() + (row * 256 + column) * 16, 16);
        return value;
    };
    EXPECT_EQ(entry(0, 19), (std::array<float, 4>{1, 1, 0, 0}));
    EXPECT_EQ(entry(0, 3), (std::array<float, 4>{0, 1, 0, 0}));
    EXPECT_EQ(entry(0, 5), (std::array<float, 4>{0, 0, 0, 0}));
    EXPECT_EQ(entry(1, 0), (std::array<float, 4>{19, 1, 0, 0}));
    EXPECT_EQ(entry(1, 1), (std::array<float, 4>{3, 1, 0, 0}));
    EXPECT_EQ(entry(1, 2), (std::array<float, 4>{0, 0, 0, 0}));
}

TEST(TerrainCompositedProductionGpu, SignedFrameAndTangentHandednessRemainUnencoded)
{
    auto object = Object("rgba32-float");
    object.image.layers = 1;
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    image.data.resize(256);
    const std::array<float, 4> tangent{-0.25f, 0.125f, 0.5f, -1.f};
    std::memcpy(image.data.data(), tangent.data(), sizeof(tangent));
    VL::TerrainRvtBudgetOwner budget(Caps(256));
    std::vector<uint8_t> bytes;
    object = VL::TerrainCompositedProductionGpu::CaptureCpuImage(object, image, budget,
        [&](const auto&, const auto& payload) { bytes = payload; });
    const auto restored = VL::TerrainCompositedProductionGpu::DecodeImage(object, std::move(bytes));
    EXPECT_EQ(restored.data, image.data);
}

TEST(TerrainCompositedProductionGpu, WritesActualImmutableInputAndRejectsTraversal)
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("vl-production-object-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    struct Cleanup
    {
        std::filesystem::path directory;
        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    } cleanup{directory};
    auto object = Object();
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    image.data.resize(128, 37);
    VL::TerrainRvtBudgetOwner budget(Caps(128));
    object = VL::TerrainCompositedProductionGpu::CaptureCpuImage(object, image, budget,
        [&](const auto& metadata, const auto& bytes)
        { VL::TerrainCompositedProductionGpu::WriteInputObject(directory, metadata, bytes); });
    EXPECT_EQ(VL::ReadBinaryFile(directory / object.relativePath), image.data);
    EXPECT_NO_THROW(VL::TerrainCompositedProductionGpu::WriteInputObject(directory, object, image.data));
    auto changed = image.data;
    changed[0] = 38;
    auto collision = object;
    collision.digest = VL::ContentHasher::HashBytes(changed.data(), changed.size());
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::WriteInputObject(directory, collision, changed),
        std::runtime_error);
    object.relativePath = "../escaped.bin";
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::WriteInputObject(directory, object, image.data),
        std::runtime_error);
}

TEST(TerrainCompositedProductionGpu, MissingAllocationQueryNeverFallsBackToPayloadBudget)
{
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::TransferCost(128, {}), std::runtime_error);
    VL::TerrainCompositedProductionAllocationQuery query;
    query.transfer = [](uint64_t) { return VL::TerrainRvtCost{64, 64, 0, 0}; };
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::TransferCost(128, query), std::runtime_error);
}

TEST(TerrainCompositedProductionGpu, ImmutableWriterAcceptsShaderKindsAndRejectsKindMetadataAndIntegrity)
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("vl-production-shader-writer-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    struct Cleanup
    {
        std::filesystem::path directory;
        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    } cleanup{directory};
    for (const auto kind : {VL::TerrainCompositedProductionObjectKind::ShaderSource,
        VL::TerrainCompositedProductionObjectKind::ShaderArtifact,
        VL::TerrainCompositedProductionObjectKind::RawAuthorSource})
    {
        VL::TerrainCompositedProductionObject object;
        object.kind = kind;
        object.objectId = "shader-" + std::to_string(static_cast<uint32_t>(kind));
        const std::vector<uint8_t> bytes{3, 2, 35, 7, static_cast<uint8_t>(kind)};
        object.bytes = bytes.size();
        object.digest = VL::ContentHasher::HashBytes(bytes.data(), bytes.size());
        object.relativePath = "inputs/shaders/nested/" + object.digest.ToHex() + ".bin";
        EXPECT_NO_THROW(VL::TerrainCompositedProductionGpu::WriteInputObject(directory, object, bytes));
        EXPECT_EQ(VL::ReadBinaryFile(directory / object.relativePath), bytes);
        auto bad = object;
        bad.image.width = 1;
        EXPECT_THROW(VL::TerrainCompositedProductionGpu::WriteInputObject(directory, bad, bytes),
            std::runtime_error);
        bad = object;
        bad.kind = static_cast<VL::TerrainCompositedProductionObjectKind>(99);
        EXPECT_THROW(VL::TerrainCompositedProductionGpu::WriteInputObject(directory, bad, bytes),
            std::runtime_error);
        bad = object;
        ++bad.bytes;
        EXPECT_THROW(VL::TerrainCompositedProductionGpu::WriteInputObject(directory, bad, bytes),
            std::runtime_error);
        auto changed = bytes;
        changed[0] ^= 1;
        EXPECT_THROW(VL::TerrainCompositedProductionGpu::WriteInputObject(directory, object, changed),
            std::runtime_error);
        bad = object;
        bad.kind = VL::TerrainCompositedProductionObjectKind::Image;
        EXPECT_THROW(VL::TerrainCompositedProductionGpu::WriteInputObject(directory, bad, bytes),
            std::exception);
    }
}

TEST(TerrainCompositedProductionGpu, TransferQuoteRetainsAlignmentAndAddsCpuPayload)
{
    VL::TerrainCompositedProductionAllocationQuery query;
    query.transfer = [](uint64_t) { return VL::TerrainRvtCost{4096, 4096 + 64, 0, 0}; };
    const auto cost = VL::TerrainCompositedProductionGpu::TransferCost(128, query);
    EXPECT_EQ(cost.gpuBytes, 4096u);
    EXPECT_EQ(cost.cpuBytes, 4096u + 64u + 128u +
        VL::TerrainCompositedProductionGpu::TransferScratchCpuBytes);
}

TEST(TerrainCompositedProductionGpu, FreezesActualPlacementMappingsAndSamplingFromBakerSnapshot)
{
    VL::TerrainCompositedBakeGpu::InputSnapshot snapshot;
    snapshot.weightSource = VL::TerrainCompositedBakeGpu::WeightSource::LocalLayerArray;
    snapshot.localLayerCount = 2;
    snapshot.localToGlobalLayer[0] = 19;
    snapshot.localToGlobalLayer[1] = 3;
    snapshot.globalToSourceSlice[19] = 1;
    snapshot.globalToSourceSlice[3] = 0;
    snapshot.localToWorld[12] = 2500.f;
    snapshot.frameUvOffset = {0.125f, 0.25f};
    snapshot.frameUvScale = {0.75f, 0.5f};
    snapshot.uvScale = 64.f;
    snapshot.frozenHeightBlendFade = 1.f;
    snapshot.retainedFragment.heightBlendStrength = 0.4f;
    snapshot.retainFragmentPostprocess = true;
    VL::TerrainCompositedProductionBindings bindings;
    bindings.localLayerWeights = "merged-overlay";
    const auto surface = VL::TerrainCompositedProductionGpu::FreezeSurface(
        "surface", 5, snapshot, bindings, "Materials/MI_runtime.json");
    EXPECT_EQ(surface.localToWorld[12], 2500);
    EXPECT_EQ(surface.localToGlobal, (std::vector<uint32_t>{19, 3}));
    EXPECT_EQ(surface.globalToSourceSlice[19], 1u);
    EXPECT_EQ(surface.weightSource, VL::TerrainCompositedWeightSource::LocalLayerArray);
    EXPECT_EQ(surface.sampling.frameUvOffset[1], 0.25);
    EXPECT_EQ(surface.sampling.frameUvScale[0], 0.75);
    EXPECT_EQ(surface.inputs.localLayerWeights, "merged-overlay");
    EXPECT_EQ(surface.sourceSet, 5u);
    EXPECT_EQ(surface.runtimeMaterialInstancePath, "Materials/MI_runtime.json");
    EXPECT_EQ(surface.parameters.heightBlendStrength, double(snapshot.retainedFragment.heightBlendStrength));
}

TEST(TerrainCompositedProductionGpu, CatalogCaptureUsesSharedProjectionAndRejectsForgedMetadata)
{
    auto object = Object("rgba32-float");
    object.image.width = 256;
    object.image.height = 4;
    object.image.layers = 1;
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    image.data.resize(static_cast<size_t>(object.image.ByteSize()), 0);
    VL::TerrainRvtBudgetOwner budget(Caps(image.data.size()));
    object = VL::TerrainCompositedProductionGpu::CaptureCpuImage(object, image, budget,
        [](const auto& metadata, const auto& payload)
        { EXPECT_NO_THROW(VL::ValidateTerrainCompositedProductionPayload(metadata, payload)); });
    auto expected = object;
    VL::PopulateTerrainCompositedCatalogDigests(expected, image.data);
    EXPECT_EQ(object.baseCatalogDigest, expected.baseCatalogDigest);
    EXPECT_EQ(object.heightBlendCatalogDigest, expected.heightBlendCatalogDigest);
    EXPECT_NE(object.baseCatalogDigest, VL::ContentDigest{});
    object.baseCatalogDigest.bytes[0] ^= 1;
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::DecodeImage(object, image.data), std::runtime_error);
}

TEST(TerrainCompositedProductionGpu, AllocationCostsKeepDriverAlignmentAndExplicitOverhead)
{
    const auto cost = VL::TerrainCompositedProductionGpu::AddAllocationCosts(
        {65536, 0, 0, 0}, {4096, 1536, 0, 0});
    EXPECT_EQ(cost.gpuBytes, 69632u);
    EXPECT_EQ(cost.cpuBytes, 1536u);
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::AddAllocationCosts(
        {UINT64_MAX, 0, 0, 0}, {1, 0, 0, 0}), std::runtime_error);
}

TEST(TerrainCompositedProductionGpu, QueryAndUploadPlanIncludesFloatFormatMipsAndArrayLayers)
{
    auto object = Object();
    object.image.mipLevels = 3;
    const auto image = VL::TerrainCompositedProductionGpu::ImageLayout(object.image);
    DeviceTextureCreateOptions options;
    options.generateMipmapsOnDevice = false;
    const auto plan = DeviceTextureFactory::ImageCreateInfo(image, options);
    EXPECT_EQ(plan.format, vk::Format::eR32Sfloat);
    EXPECT_EQ(plan.arrayLayers, 2u);
    EXPECT_EQ(plan.mipLevels, 3u);
    EXPECT_EQ(plan.extent, (vk::Extent3D{4, 4, 1}));
    EXPECT_EQ(plan.usage, vk::ImageUsageFlagBits::eTransferSrc |
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);
    EXPECT_EQ(plan.tiling, vk::ImageTiling::eOptimal);
}

TEST(TerrainCompositedProductionGpu, CoverageAtlasTransferUses64BoundedChunks)
{
    VL::TerrainCompositedProductionImageDesc desc{"rgba8-unorm", 4096, 4096};
    uint64_t cursor = 0;
    uint32_t count = 0;
    VL::TerrainCompositedProductionGpu::VisitTransferChunks(desc, [&](const auto& chunk)
    {
        EXPECT_EQ(chunk.rawOffset, cursor);
        EXPECT_EQ(chunk.bytes, 1024u * 1024);
        EXPECT_EQ(chunk.copy.bufferOffset, 0u);
        EXPECT_EQ(chunk.copy.imageExtent, (vk::Extent3D{4096, 64, 1}));
        EXPECT_EQ(chunk.copy.imageOffset.y, int32_t(count * 64));
        EXPECT_EQ(chunk.copy.imageSubresource.baseArrayLayer, 0u);
        EXPECT_EQ(chunk.copy.imageSubresource.mipLevel, 0u);
        cursor += chunk.bytes;
        ++count;
    });
    EXPECT_EQ(count, 64u);
    EXPECT_EQ(cursor, 64u * 1024 * 1024);
}

TEST(TerrainCompositedProductionGpu, BoundedTransferPreservesEveryOddBlockRowMipAndLayer)
{
    for (const std::string format : {"rgba8-srgb", "rgba32-float", "r32-float",
        "r16-unorm", "bc5-unorm", "bc7-unorm", "bc7-srgb"})
    {
        SCOPED_TRACE(format);
        VL::TerrainCompositedProductionImageDesc desc{format, 4097, 1031, 2, 13};
        if (format == "rgba8-srgb" || format == "bc7-srgb")
            desc.transfer = VL::TerrainCompositedProductionTransfer::Srgb;
        auto image = VL::TerrainCompositedProductionGpu::ImageLayout(desc);
        image.data.resize(image.GetTotalBytes());
        uint64_t cursor = 0;
        const uint32_t block = HostImage::IsBlockCompressed(image.format) ? 4 : 1;
        VL::TerrainCompositedProductionGpu::VisitTransferChunks(desc, [&](const auto& chunk)
        {
            const auto mip = chunk.copy.imageSubresource.mipLevel;
            const auto layer = chunk.copy.imageSubresource.baseArrayLayer;
            const auto firstRow = uint32_t(chunk.copy.imageOffset.y) / block;
            EXPECT_EQ(chunk.rawOffset, image.GetMipOffset(mip, layer) +
                uint64_t(firstRow) * image.GetMipRowStride(mip));
            EXPECT_EQ(chunk.rawOffset, cursor);
            EXPECT_LE(chunk.bytes, VL::TerrainCompositedProductionGpu::MaxTransferBytes);
            EXPECT_EQ(chunk.bytes, uint64_t((chunk.copy.imageExtent.height + block - 1) / block) *
                image.GetMipRowStride(mip));
            EXPECT_EQ(chunk.copy.imageOffset.y % block, 0u);
            EXPECT_EQ(chunk.copy.imageExtent.width, image.GetMipWidth(mip));
            EXPECT_LE(uint32_t(chunk.copy.imageOffset.y) + chunk.copy.imageExtent.height,
                image.GetMipHeight(mip));
            EXPECT_EQ(chunk.copy.imageSubresource.layerCount, 1u);
            EXPECT_EQ(chunk.copy.bufferRowLength, 0u);
            EXPECT_EQ(chunk.copy.bufferImageHeight, 0u);
            if (block == 4)
                EXPECT_EQ(chunk.bytes % 16, 0u);
            cursor += chunk.bytes;
        });
        EXPECT_EQ(cursor, image.data.size());
    }
}

TEST(TerrainCompositedProductionGpu, FloatChunkScatterPreservesSignedZeroSubnormalsAndMipLayers)
{
    VL::TerrainCompositedProductionImageDesc desc{"r32-float", 513, 515, 3, 10};
    auto image = VL::TerrainCompositedProductionGpu::ImageLayout(desc);
    image.data.resize(image.GetTotalBytes());
    const std::array<uint32_t, 8> bits{0, 0x80000000u, 1, 0x007fffffu,
        0x00800000u, 0x3f800001u, 0xbf123456u, 0x7f7fffffu};
    for (size_t offset = 0; offset < image.data.size(); offset += 4)
        std::memcpy(image.data.data() + offset, &bits[(offset / 4) % bits.size()], 4);
    std::vector<uint8_t> restored(image.data.size(), 0);
    std::vector<uint8_t> staging(VL::TerrainCompositedProductionGpu::MaxTransferBytes);
    VL::TerrainCompositedProductionGpu::VisitTransferChunks(desc, [&](const auto& chunk)
    {
        std::memcpy(staging.data(), image.data.data() + chunk.rawOffset, chunk.bytes);
        std::memcpy(restored.data() + chunk.rawOffset, staging.data(), chunk.bytes);
    });
    EXPECT_EQ(restored, image.data);
}

TEST(TerrainCompositedProductionGpu, AtlasAdmissionChargesFullCpuButOnlyOneMiBStaging)
{
    const uint64_t payload = 64u * 1024 * 1024;
    uint64_t queried = 0;
    VL::TerrainCompositedProductionAllocationQuery query;
    query.transfer = [&](uint64_t bytes)
    {
        queried = bytes;
        return VL::TerrainRvtCost{bytes, bytes + 16, 0, 0};
    };
    const auto cost = VL::TerrainCompositedProductionGpu::TransferCost(payload, query);
    EXPECT_EQ(queried, 1024u * 1024);
    EXPECT_EQ(cost.gpuBytes, queried);
    EXPECT_EQ(cost.cpuBytes, payload + queried + 16 +
        VL::TerrainCompositedProductionGpu::TransferScratchCpuBytes);
    auto caps = Caps(256u * 1024 * 1024);
    caps.workspaceGpuBytes = caps.totalGpuBytes = 32u * 1024 * 1024;
    VL::TerrainRvtBudgetOwner budget(caps);
    auto shared = budget.TryReserve({28u * 1024 * 1024, 0, 0, 0},
        VL::TerrainRvtCostClass::Workspace, VL::TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(shared);
    auto lease = budget.TryReserve(cost, VL::TerrainRvtCostClass::Workspace,
        VL::TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(lease);
    EXPECT_EQ(budget.Statistics().current.gpuBytes, 29u * 1024 * 1024);
    lease.reset();
    shared.reset();
    EXPECT_EQ(budget.Statistics().current, VL::TerrainRvtCost{});
}

TEST(TerrainCompositedProductionGpu, ChunkQuoteRetainsAlignmentAndRejectsCpuOverflow)
{
    VL::TerrainCompositedProductionAllocationQuery query;
    query.transfer = [](uint64_t bytes) { return VL::TerrainRvtCost{bytes + 4096, bytes + 4096, 0, 0}; };
    EXPECT_EQ(VL::TerrainCompositedProductionGpu::TransferCost(64u * 1024 * 1024, query).gpuBytes,
        VL::TerrainCompositedProductionGpu::MaxTransferBytes + 4096);
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::TransferCost(UINT64_MAX, query), std::runtime_error);
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::TransferCost(0, query), std::runtime_error);
    query.transfer = [](uint64_t) { return VL::TerrainRvtCost{1024 * 1024 - 1, 1024 * 1024, 0, 0}; };
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::TransferCost(64u * 1024 * 1024, query), std::runtime_error);
}

TEST(TerrainCompositedProductionGpu, ChunkPlanningRejectsInvalidLayoutAndVisitor)
{
    VL::TerrainCompositedProductionImageDesc desc{"bc7-srgb", 7, 5, 2, 3};
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::VisitTransferChunks(desc, [](const auto&) {}),
        std::runtime_error);
    desc.transfer = VL::TerrainCompositedProductionTransfer::Srgb;
    EXPECT_THROW(VL::TerrainCompositedProductionGpu::VisitTransferChunks(desc, {}), std::runtime_error);
    uint32_t count = 0;
    VL::TerrainCompositedProductionGpu::VisitTransferChunks(desc, [&](const auto&) { ++count; });
    EXPECT_EQ(count, 6u);
}
