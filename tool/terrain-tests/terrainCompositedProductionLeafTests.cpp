#include <gtest/gtest.h>

#include "render/resource/terrainCompositedProductionGpu.h"
#include "terrainCompositedProductionFixture.h"

#include <chrono>
#include <future>
#include <thread>

namespace
{
using Resources = VL::TerrainCompositedProductionGpuResources;
using Phase = VL::TerrainRvtResourcePhase;

VL::TerrainRvtBudgetCaps LeafCaps()
{
    VL::TerrainRvtBudgetCaps caps;
    caps.totalGpuBytes = caps.inputGpuBytes = caps.workspaceGpuBytes = 64 * 1024 * 1024;
    caps.totalCpuBytes = caps.inputCpuBytes = caps.workspaceCpuBytes = 64 * 1024 * 1024;
    return caps;
}

class TerrainProductionLeaf : public testing::Test
{
protected:
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("vl-production-leaf-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    VL::TerrainRvtBudgetOwner budget{LeafCaps()};
    Resources::WorldScope scope = std::make_shared<int>(1);
    VL::TerrainCompositedProductionClosure closure;
    VL::TerrainCompositedProductionAllocationQuery queries;
    size_t reads = 0;
    size_t uploads = 0;
    size_t destroyed = 0;
    size_t imageQueries = 0;
    size_t failUpload = SIZE_MAX;
    bool corrupt = false;
    bool checkLastRef = false;
    uint64_t expectedLastGpu = 0;

    void SetUp() override
    {
        closure = TerrainProductionFixture::Closure(directory, {"surface"});
        queries.image = [&](const auto& desc, bool)
        {
            ++imageQueries;
            return VL::TerrainRvtCost{desc.ByteSize() + 4096, 128, 0, 0};
        };
        queries.transfer = [](uint64_t bytes)
        { return VL::TerrainRvtCost{bytes + 256, bytes + 256, 0, 0}; };
    }

    void TearDown() override
    {
        EXPECT_EQ(budget.Statistics().current, VL::TerrainRvtCost{});
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    std::shared_ptr<Resources> Restore(std::shared_ptr<const Resources> prior = {},
        Resources::WorldScope token = {}, VL::TerrainRvtBudgetOwner* owner = nullptr)
    {
        return Resources::RestoreImages(closure,
            [&](const auto& object)
            {
                ++reads;
                auto bytes = VL::ReadBinaryFile(directory / object.relativePath);
                if (corrupt) bytes[0] ^= 1;
                return bytes;
            }, owner ? *owner : budget, queries,
            [&](const HostImage&, const auto&, bool)
            {
                if (++uploads == failUpload)
                    throw std::runtime_error("injected upload failure");
                auto marker = new uint64_t(uploads);
                std::shared_ptr<Texture> texture(reinterpret_cast<Texture*>(marker),
                    [this](Texture* value)
                    {
                        if (checkLastRef)
                            EXPECT_EQ(budget.Statistics().current.gpuBytes, expectedLastGpu);
                        ++destroyed;
                        delete reinterpret_cast<uint64_t*>(value);
                    });
                return Resources::UploadedImage{texture,
                    vk::Image(reinterpret_cast<VkImage>(marker)),
                    vk::ImageView(reinterpret_cast<VkImageView>(marker)),
                    vk::Sampler(reinterpret_cast<VkSampler>(marker))};
            }, std::move(prior), token ? std::move(token) : scope);
    }

    void ChangeAlbedo()
    {
        for (auto& object : closure.objects)
            if (object.objectId == "albedo")
            {
                auto bytes = VL::ReadBinaryFile(directory / object.relativePath);
                bytes[0] ^= 1;
                VL::TerrainCompositedProductionGpu::PopulateCapturedImageMetadata(object, bytes);
                object.relativePath = "objects/" + object.digest.ToHex() + ".bin";
                TerrainProductionFixture::WriteFixtureBytes(directory / object.relativePath, bytes);
            }
    }
};

TEST_F(TerrainProductionLeaf, MaterialAliasRetainsOnlyOneLeafAndDestroysTextureBeforeLease)
{
    auto snapshot = Restore();
    const auto allocation = snapshot->ImageAllocation("albedo");
    auto material = snapshot->Image("albedo");
    std::weak_ptr<Resources> weak = snapshot;
    const auto count = uploads;
    snapshot.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(destroyed, count - 1);
    EXPECT_EQ(budget.Statistics().current, allocation.actualCost);
    expectedLastGpu = allocation.actualCost.gpuBytes;
    checkLastRef = true;
    material.reset();
    checkLastRef = false;
    EXPECT_EQ(budget.Statistics().current, VL::TerrainRvtCost{});
}

TEST_F(TerrainProductionLeaf, SourceMapAliasDoesNotRetainImagesOrClosure)
{
    auto snapshot = Restore();
    const auto cost = snapshot->SourceMapAllocation("surface").actualCost;
    auto material = snapshot->SourceMap("surface");
    snapshot.reset();
    EXPECT_EQ(budget.Statistics().current, cost);
    EXPECT_EQ(destroyed, uploads - 1);
}

TEST_F(TerrainProductionLeaf, SameVerifiedInputReusesActualAllocationWithoutReadQueryOrUpload)
{
    auto old = Restore();
    old->SetPhase(Phase::Active);
    const auto allocation = old->ImageAllocation("albedo");
    const auto previous = budget.Statistics().current;
    const auto readCount = reads;
    const auto uploadCount = uploads;
    const auto queryCount = imageQueries;
    auto next = Restore(old);
    EXPECT_EQ(reads, readCount);
    EXPECT_EQ(uploads, uploadCount);
    EXPECT_EQ(imageQueries, queryCount);
    const auto shared = next->ImageAllocation("albedo");
    EXPECT_EQ(shared.allocationIdentity, allocation.allocationIdentity);
    EXPECT_EQ(shared.image, allocation.image);
    EXPECT_EQ(shared.view, allocation.view);
    EXPECT_EQ(shared.sampler, allocation.sampler);
    EXPECT_EQ(shared.actualCost, allocation.actualCost);
    EXPECT_EQ(next->SourceMapAllocation("surface").allocationIdentity,
        old->SourceMapAllocation("surface").allocationIdentity);
    EXPECT_EQ(budget.Statistics().current.gpuBytes, previous.gpuBytes);
    EXPECT_EQ(budget.Statistics().current.cpuBytes,
        previous.cpuBytes + next->SnapshotMetadataCost().cpuBytes);
    next->SetPhase(Phase::Active);
    old->SetPhase(Phase::Retiring);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)].gpuBytes, previous.gpuBytes);
    old.reset();
    EXPECT_EQ(destroyed, 0u);
    EXPECT_THROW(next->SetPhase(Phase::Candidate), std::logic_error);
}

TEST_F(TerrainProductionLeaf, ReplacedImageLivesUntilLastMaterialAndGpuEpochReference)
{
    auto old = Restore();
    auto material = old->Image("albedo");
    auto gpuEpoch = material;
    const auto allocation = old->ImageAllocation("albedo");
    const auto oldTotal = budget.Statistics().current.gpuBytes;
    const auto uploadCount = uploads;
    ChangeAlbedo();
    auto next = Restore(old);
    EXPECT_EQ(uploads, uploadCount + 1);
    EXPECT_NE(next->ImageAllocation("albedo").allocationIdentity, allocation.allocationIdentity);
    EXPECT_EQ(next->ImageAllocation("normal").allocationIdentity,
        old->ImageAllocation("normal").allocationIdentity);
    EXPECT_EQ(budget.Statistics().current.gpuBytes, oldTotal + allocation.actualCost.gpuBytes);
    old.reset();
    EXPECT_EQ(destroyed, 0u);
    material.reset();
    EXPECT_EQ(destroyed, 0u);
    gpuEpoch.reset();
    EXPECT_EQ(destroyed, 1u);
    EXPECT_EQ(budget.Statistics().current.gpuBytes, oldTotal);
}

TEST_F(TerrainProductionLeaf, EqualBytesInNewImageAreChargedTwice)
{
    auto old = Restore();
    const auto total = budget.Statistics().current.gpuBytes;
    auto fresh = Restore();
    EXPECT_NE(old->ImageAllocation("albedo").allocationIdentity,
        fresh->ImageAllocation("albedo").allocationIdentity);
    EXPECT_EQ(budget.Statistics().current.gpuBytes, 2 * total);
}

TEST_F(TerrainProductionLeaf, DifferentWorldOrBudgetAndRetiringPriorCannotShare)
{
    auto old = Restore();
    auto otherWorld = Restore(old, std::make_shared<int>(2));
    EXPECT_NE(old->ImageAllocation("albedo").allocationIdentity,
        otherWorld->ImageAllocation("albedo").allocationIdentity);
    VL::TerrainRvtBudgetOwner otherBudget(LeafCaps());
    auto otherOwner = Restore(old, scope, &otherBudget);
    EXPECT_NE(old->ImageAllocation("albedo").allocationIdentity,
        otherOwner->ImageAllocation("albedo").allocationIdentity);
    otherOwner.reset();
    EXPECT_EQ(otherBudget.Statistics().current, VL::TerrainRvtCost{});
    old->SetPhase(Phase::Retiring);
    auto revived = Restore(old);
    EXPECT_NE(old->ImageAllocation("albedo").allocationIdentity,
        revived->ImageAllocation("albedo").allocationIdentity);
}

TEST_F(TerrainProductionLeaf, IndependentOwnerLifetimeReuseMustNotCrossLedger)
{
    std::optional<VL::TerrainRvtBudgetOwner> facade;
    facade.emplace(LeafCaps());
    auto first = Restore({}, scope, &*facade);
    first->SetPhase(Phase::Active);
    const auto oldGpu = facade->Statistics().current.gpuBytes;
    const auto beforeUploads = uploads;
    auto oldAddress = &*facade;
    facade.reset();
    facade.emplace(LeafCaps());
    ASSERT_EQ(oldAddress, &*facade);
    auto second = Restore(first, scope, &*facade);
    EXPECT_EQ(facade->Statistics().current.gpuBytes, oldGpu);
    EXPECT_GT(uploads, beforeUploads);
    EXPECT_NE(first->ImageAllocation("albedo").allocationIdentity,
        second->ImageAllocation("albedo").allocationIdentity);
    second.reset();
    first.reset();
    EXPECT_EQ(facade->Statistics().current, VL::TerrainRvtCost{});
}

TEST_F(TerrainProductionLeaf, AccountingIdentityFollowsLeaseStateRatherThanFacadeLifetime)
{
    std::optional<VL::TerrainRvtBudgetOwner> facade;
    facade.emplace(LeafCaps());
    const auto originalDomain = facade->IdentityToken();
    const auto repeatedDomain = facade->IdentityToken();
    EXPECT_FALSE(originalDomain.owner_before(repeatedDomain));
    EXPECT_FALSE(repeatedDomain.owner_before(originalDomain));
    std::weak_ptr<const void> domain;
    auto material = std::shared_ptr<Texture>{};
    {
        VL::TerrainRvtBudgetOwner owner(LeafCaps());
        domain = owner.IdentityToken();
        auto snapshot = Restore({}, scope, &owner);
        material = snapshot->Image("albedo");
    }
    EXPECT_FALSE(domain.expired());
    material.reset();
    EXPECT_TRUE(domain.expired());
    const auto address = &*facade;
    facade.reset();
    facade.emplace(LeafCaps());
    ASSERT_EQ(&*facade, address);
    const auto replacementDomain = facade->IdentityToken();
    EXPECT_TRUE(originalDomain.owner_before(replacementDomain) ||
        replacementDomain.owner_before(originalDomain));
    EXPECT_EQ(facade->Statistics().current, VL::TerrainRvtCost{});
}

TEST_F(TerrainProductionLeaf, WorldScopesWithEqualPointersButDifferentControlBlocksCannotShare)
{
    auto original = Restore();
    Resources::WorldScope differentOwner(scope.get(), [](const void*) {});
    ASSERT_EQ(differentOwner.get(), scope.get());
    const auto before = uploads;
    auto next = Restore(original, differentOwner);
    EXPECT_GT(uploads, before);
    EXPECT_NE(original->ImageAllocation("albedo").allocationIdentity,
        next->ImageAllocation("albedo").allocationIdentity);
}

TEST_F(TerrainProductionLeaf, WorldScopeAliasPointersShareOnlyTheirCommonControlBlock)
{
    auto owner = std::make_shared<std::array<int, 2>>();
    Resources::WorldScope firstScope(owner, &(*owner)[0]);
    Resources::WorldScope secondScope(owner, &(*owner)[1]);
    ASSERT_NE(firstScope.get(), secondScope.get());
    auto original = Restore({}, firstScope);
    const auto before = uploads;
    auto next = Restore(original, secondScope);
    EXPECT_EQ(uploads, before);
    EXPECT_EQ(original->ImageAllocation("albedo").allocationIdentity,
        next->ImageAllocation("albedo").allocationIdentity);
}

TEST_F(TerrainProductionLeaf, NonOwningWorldScopeAliasesCannotEstablishSharedDomain)
{
    Resources::WorldScope emptyOwner(std::shared_ptr<const void>{}, scope.get());
    ASSERT_TRUE(emptyOwner);
    ASSERT_EQ(emptyOwner.use_count(), 0);
    auto original = Restore({}, emptyOwner);
    const auto before = uploads;
    auto next = Restore(original, emptyOwner);
    EXPECT_GT(uploads, before);
    EXPECT_NE(original->ImageAllocation("albedo").allocationIdentity,
        next->ImageAllocation("albedo").allocationIdentity);
}

TEST_F(TerrainProductionLeaf, FailedRevisionPreservesPriorAllocationAndBudget)
{
    auto old = Restore();
    old->SetPhase(Phase::Active);
    const auto before = budget.Statistics().current;
    ChangeAlbedo();
    failUpload = uploads + 1;
    EXPECT_THROW(Restore(old), std::runtime_error);
    EXPECT_EQ(budget.Statistics().current, before);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)].gpuBytes, before.gpuBytes);
    failUpload = SIZE_MAX;
    corrupt = true;
    EXPECT_THROW(Restore(old), std::runtime_error);
    EXPECT_EQ(budget.Statistics().current, before);
}

TEST_F(TerrainProductionLeaf, SamplerAndViewTypeChangesRequireNewAllocation)
{
    auto old = Restore();
    for (auto& object : closure.objects)
        if (object.objectId == "albedo")
            object.image.address = VL::TerrainCompositedProductionAddress::Repeat;
    auto sampler = Restore(old);
    EXPECT_NE(old->ImageAllocation("albedo").allocationIdentity,
        sampler->ImageAllocation("albedo").allocationIdentity);
    auto extra = closure.Object("normal");
    extra.objectId = "optional";
    extra.relativePath = "objects/optional.bin";
    TerrainProductionFixture::WriteFixtureBytes(directory / extra.relativePath,
        VL::ReadBinaryFile(directory / closure.Object("normal").relativePath));
    closure.objects.push_back(extra);
    closure.surfaces.front().inputs.globalMasks = "optional";
    auto plane = Restore(sampler);
    closure.surfaces.front().inputs.globalMasks.clear();
    closure.surfaces.front().inputs.layerHeight = "optional";
    auto array = Restore(plane);
    EXPECT_NE(plane->ImageAllocation("optional").allocationIdentity,
        array->ImageAllocation("optional").allocationIdentity);
}

TEST_F(TerrainProductionLeaf, ModifiedGpuLeafCannotReuseFrozenDigestThroughAnotherSnapshot)
{
    auto old = Restore();
    auto same = Restore(old);
    const auto count = uploads;
    old->InvalidateImageReuse("albedo");
    old->InvalidateSourceMapReuse("surface");
    auto next = Restore(same);
    EXPECT_EQ(uploads, count + 2);
    EXPECT_NE(same->ImageAllocation("albedo").allocationIdentity,
        next->ImageAllocation("albedo").allocationIdentity);
    EXPECT_NE(same->SourceMapAllocation("surface").allocationIdentity,
        next->SourceMapAllocation("surface").allocationIdentity);
    EXPECT_EQ(same->ImageAllocation("normal").allocationIdentity,
        next->ImageAllocation("normal").allocationIdentity);
}

TEST_F(TerrainProductionLeaf, ChangedSourceMapGetsOwnLeaseAndOldMapSurvivesEpoch)
{
    auto old = Restore();
    auto epoch = old->SourceMap("surface");
    const auto oldCost = old->SourceMapAllocation("surface").actualCost;
    const auto count = uploads;
    closure.surfaces.front().globalToSourceSlice[8] = 0;
    auto next = Restore(old);
    EXPECT_EQ(uploads, count + 1);
    EXPECT_NE(old->SourceMapAllocation("surface").allocationIdentity,
        next->SourceMapAllocation("surface").allocationIdentity);
    old.reset();
    const auto beforeRelease = budget.Statistics().current;
    epoch.reset();
    EXPECT_EQ(budget.Statistics().current.gpuBytes, beforeRelease.gpuBytes - oldCost.gpuBytes);
    EXPECT_EQ(budget.Statistics().current.cpuBytes, beforeRelease.cpuBytes - oldCost.cpuBytes);
}

TEST_F(TerrainProductionLeaf, ObjectIdentityMustMatchEvenForIdenticalPayloadAndImageMetadata)
{
    auto old = Restore();
    for (auto& object : closure.objects)
        if (object.objectId == "albedo")
            object.objectId = "renamed-albedo";
    closure.surfaces.front().inputs.layerAlbedo = "renamed-albedo";
    const auto count = uploads;
    auto next = Restore(old);
    EXPECT_EQ(uploads, count + 1);
    EXPECT_NE(old->ImageAllocation("albedo").allocationIdentity,
        next->ImageAllocation("renamed-albedo").allocationIdentity);
}

TEST_F(TerrainProductionLeaf, NoExplicitWorldScopeMeansNoSharing)
{
    auto old = Restore();
    const auto before = uploads;
    auto next = Resources::RestoreImages(closure,
        [&](const auto& object) { return VL::ReadBinaryFile(directory / object.relativePath); },
        budget, queries,
        [&](const HostImage&, const auto&, bool)
        {
            ++uploads;
            auto marker = std::make_shared<int>(1);
            return Resources::UploadedImage{
                std::shared_ptr<Texture>(marker, reinterpret_cast<Texture*>(marker.get()))};
        }, old);
    EXPECT_GT(uploads, before);
    EXPECT_NE(old->ImageAllocation("albedo").allocationIdentity,
        next->ImageAllocation("albedo").allocationIdentity);
}

TEST_F(TerrainProductionLeaf, AdmissionReservesAllImagesBeforeAnyUploadOrRead)
{
    auto limits = LeafCaps();
    limits.inputGpuBytes = 1;
    VL::TerrainRvtBudgetOwner tiny(limits);
    EXPECT_THROW(Restore({}, {}, &tiny), std::runtime_error);
    EXPECT_EQ(reads, 0u);
    EXPECT_EQ(uploads, 0u);
    EXPECT_EQ(tiny.Statistics().current, VL::TerrainRvtCost{});
    limits = LeafCaps();
    limits.inputCpuBytes = Resources::MetadataBytes(closure) +
        VL::TerrainRvtBudgetOwner::MetadataBytesPerLease - 1;
    VL::TerrainRvtBudgetOwner noMetadata(limits);
    imageQueries = 0;
    EXPECT_THROW(Restore({}, {}, &noMetadata), std::runtime_error);
    EXPECT_EQ(imageQueries, 0u);
    EXPECT_EQ(noMetadata.Statistics().current, VL::TerrainRvtCost{});
}

TEST_F(TerrainProductionLeaf, CandidateAndActiveUsesDelayRetiringChargeUntilEveryUseRetires)
{
    auto old = Restore();
    old->SetPhase(Phase::Active);
    auto candidate = Restore(old);
    const auto total = budget.Statistics().current.gpuBytes;
    old->SetPhase(Phase::Retiring);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)].gpuBytes, total);
    candidate->SetPhase(Phase::Active);
    auto material = candidate->Image("albedo");
    const auto cost = candidate->ImageAllocation("albedo").actualCost;
    candidate->SetPhase(Phase::Retiring);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Retiring)].gpuBytes, total);
    old.reset();
    candidate.reset();
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Retiring)], cost);
}

TEST_F(TerrainProductionLeaf, PartialUploadFailureDropsOnlyNewLeaves)
{
    auto old = Restore();
    const auto before = budget.Statistics().current;
    ChangeAlbedo();
    closure.surfaces.front().globalToSourceSlice[8] = 0;
    const auto count = destroyed;
    failUpload = uploads + 2;
    EXPECT_THROW(Restore(old), std::runtime_error);
    EXPECT_EQ(destroyed, count + 1);
    EXPECT_EQ(budget.Statistics().current, before);
}

TEST_F(TerrainProductionLeaf, LongEditChainRetainsOnlyCurrentAndExplicitEpochLeaves)
{
    auto current = Restore();
    current->SetPhase(Phase::Active);
    const auto base = budget.Statistics().current;
    auto epoch = current->Image("albedo");
    const auto epochCost = current->ImageAllocation("albedo").actualCost;
    const auto originalMetadata = current->SnapshotMetadataCost();
    uint64_t steadyCpu = 0;
    for (size_t revision = 0; revision < 120; ++revision)
    {
        ChangeAlbedo();
        auto next = Restore(current);
        next->SetPhase(Phase::Active);
        current->SetPhase(Phase::Retiring);
        current = std::move(next);
        EXPECT_EQ(budget.Statistics().current.gpuBytes, base.gpuBytes + epochCost.gpuBytes);
        const auto expectedCpu = base.cpuBytes - originalMetadata.cpuBytes +
            current->SnapshotMetadataCost().cpuBytes +
            current->ImageAllocation("albedo").actualCost.cpuBytes;
        EXPECT_EQ(budget.Statistics().current.cpuBytes, expectedCpu);
        if (revision == 0)
            steadyCpu = expectedCpu;
        EXPECT_EQ(budget.Statistics().current.cpuBytes, steadyCpu);
        EXPECT_EQ(budget.Statistics().allocations, closure.objects.size() - 2 + 3);
    }
    epoch.reset();
    EXPECT_EQ(budget.Statistics().current.gpuBytes, base.gpuBytes);
    EXPECT_EQ(budget.Statistics().current.cpuBytes, steadyCpu - epochCost.cpuBytes);
    current.reset();
    EXPECT_EQ(destroyed, uploads);
}

TEST_F(TerrainProductionLeaf, ProductionTaskStrongSnapshotKeepsWholeInputUntilCompletion)
{
    auto snapshot = Restore();
    VL::TerrainCompositedOwnedBakeInput task;
    task.owner = snapshot;
    const auto total = budget.Statistics().current;
    snapshot.reset();
    EXPECT_EQ(budget.Statistics().current, total);
    EXPECT_EQ(destroyed, 0u);
    task.owner.reset();
    EXPECT_EQ(destroyed, uploads);
}

TEST_F(TerrainProductionLeaf, TypedUsesKeepOnlyExactLeavesActiveAfterOriginalSnapshotDies)
{
    auto original = Restore();
    original->SetPhase(Phase::Active);
    std::weak_ptr<Resources> closureOwner = original;
    auto image = original->RetainImageUse("albedo", Phase::Candidate);
    auto sourceMap = original->RetainSourceMapUse("surface", Phase::Candidate);
    const auto imageCost = image->GetAllocation().actualCost;
    const auto sourceCost = sourceMap->GetAllocation().actualCost;
    auto texture = image->GetTexture();
    EXPECT_EQ(image->GetAllocation().allocationIdentity,
        original->ImageAllocation("albedo").allocationIdentity);
    EXPECT_EQ(texture.get(), original->Image("albedo").get());
    const auto count = uploads;
    image->SetPhase(Phase::Active);
    sourceMap->SetPhase(Phase::Active);
    original->SetPhase(Phase::Retiring);
    original.reset();
    EXPECT_TRUE(closureOwner.expired());
    EXPECT_EQ(destroyed, count - 2);
    EXPECT_EQ(budget.Statistics().current.gpuBytes, imageCost.gpuBytes + sourceCost.gpuBytes);
    EXPECT_EQ(budget.Statistics().current.cpuBytes, imageCost.cpuBytes + sourceCost.cpuBytes);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)], budget.Statistics().current);
    image.reset();
    EXPECT_EQ(destroyed, count - 2);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Retiring)], imageCost);
    texture.reset();
    EXPECT_EQ(destroyed, count - 1);
    sourceMap.reset();
    EXPECT_EQ(budget.Statistics().current, VL::TerrainRvtCost{});
}

TEST_F(TerrainProductionLeaf, ForkedUsePromotesIndependentlyOfRetiringOldRevision)
{
    auto original = Restore();
    original->SetPhase(Phase::Active);
    auto old = original->RetainImageUse("normal", Phase::Active);
    const auto cost = old->GetAllocation().actualCost;
    original.reset();
    auto next = old->Retain(Phase::Candidate);
    EXPECT_EQ(next->Phase(), Phase::Candidate);
    EXPECT_EQ(old->Phase(), Phase::Active);
    next->SetPhase(Phase::Active);
    old->SetPhase(Phase::Retiring);
    EXPECT_EQ(old->Phase(), Phase::Retiring);
    EXPECT_EQ(next->Phase(), Phase::Active);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)], cost);
    old.reset();
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)], cost);
    auto epoch = next->GetTexture();
    next->SetPhase(Phase::Retiring);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Retiring)], cost);
    next.reset();
    EXPECT_EQ(budget.Statistics().current, cost);
    epoch.reset();
    EXPECT_EQ(budget.Statistics().current, VL::TerrainRvtCost{});
}

TEST_F(TerrainProductionLeaf, TypedUseRejectsRegressionInvalidPhaseAndRetiringRetention)
{
    auto original = Restore();
    auto use = original->RetainImageUse("albedo", Phase::Candidate);
    EXPECT_NO_THROW(use->SetPhase(Phase::Candidate));
    use->SetPhase(Phase::Active);
    EXPECT_THROW(use->SetPhase(Phase::Candidate), std::logic_error);
    EXPECT_THROW(use->SetPhase(Phase::Count), std::logic_error);
    EXPECT_THROW(use->SetPhase(static_cast<Phase>(-1)), std::logic_error);
    EXPECT_EQ(use->Phase(), Phase::Active);
    EXPECT_THROW(original->RetainImageUse("missing", Phase::Active), std::out_of_range);
    EXPECT_THROW(original->RetainSourceMapUse("missing", Phase::Active), std::out_of_range);
    const auto before = budget.Statistics().current;
    EXPECT_THROW(use->Retain(Phase::Count), std::logic_error);
    EXPECT_THROW(original->RetainSourceMapUse("surface", Phase::Count), std::logic_error);
    EXPECT_EQ(budget.Statistics().current, before);
    use->SetPhase(Phase::Retiring);
    EXPECT_THROW(use->Retain(Phase::Candidate), std::logic_error);
    original->SetPhase(Phase::Retiring);
    EXPECT_THROW(original->RetainImageUse("normal", Phase::Active), std::logic_error);
    EXPECT_THROW(original->RetainSourceMapUse("surface", Phase::Active), std::logic_error);
    EXPECT_EQ(budget.Statistics().current, before);
}

TEST_F(TerrainProductionLeaf, FailedCandidateUsesDoNotRetireActiveWorldLeaves)
{
    auto original = Restore();
    original->SetPhase(Phase::Active);
    auto active = original->RetainSourceMapUse("surface", Phase::Active);
    original.reset();
    const auto cost = active->GetAllocation().actualCost;
    {
        auto candidate = active->Retain(Phase::Candidate);
        EXPECT_EQ(candidate->GetAllocation().allocationIdentity,
            active->GetAllocation().allocationIdentity);
        EXPECT_EQ(budget.Statistics().current, cost);
    }
    EXPECT_EQ(active->Phase(), Phase::Active);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)], cost);
}

TEST_F(TerrainProductionLeaf, LeafOnlyWorldSnapshotsReplaceCoverageFor120RevisionsWithoutClosureRetention)
{
    struct Revision
    {
        std::optional<VL::TerrainRvtBudgetLease> metadata;
        std::map<std::string, std::shared_ptr<Resources::PhaseUse>> uses;
        std::map<std::string, std::shared_ptr<Texture>> aliases;

        void SetPhase(Phase phase)
        {
            metadata->SetPhase(phase);
            for (auto& entry : uses)
                entry.second->SetPhase(phase);
        }
    };
    const auto reserveRevision = [&](size_t count)
    {
        auto revision = std::make_unique<Revision>();
        revision->metadata = budget.TryReserve(
            {0, sizeof(Revision) + count * (Resources::PhaseUse::MetadataBytes() + 256), 0, 0},
            VL::TerrainRvtCostClass::Input, Phase::Candidate);
        if (!revision->metadata)
            throw std::runtime_error("test revision metadata admission failed");
        return revision;
    };
    auto original = Restore();
    original->SetPhase(Phase::Active);
    const auto originalTotal = budget.Statistics().current;
    const auto originalMetadata = original->SnapshotMetadataCost();
    const auto originalCoverage = original->ImageAllocation("coverage-ids").actualCost;
    const auto imageCount = uploads - 1;
    auto current = reserveRevision(imageCount + 1);
    for (const auto& object : closure.objects)
        if (object.kind == VL::TerrainCompositedProductionObjectKind::Image)
        {
            current->uses[object.objectId] = original->RetainImageUse(object.objectId, Phase::Candidate);
            current->aliases[object.objectId] = original->Image(object.objectId);
        }
    current->uses["source-map"] = original->RetainSourceMapUse("surface", Phase::Candidate);
    current->aliases["source-map"] = original->SourceMap("surface");
    current->SetPhase(Phase::Active);
    std::weak_ptr<Resources> originalWeak = original;
    original->SetPhase(Phase::Retiring);
    original.reset();
    EXPECT_TRUE(originalWeak.expired());
    const auto steady = budget.Statistics().current;
    EXPECT_EQ(steady.gpuBytes, originalTotal.gpuBytes);
    EXPECT_EQ(steady.cpuBytes, originalTotal.cpuBytes - originalMetadata.cpuBytes +
        current->metadata->Cost().cpuBytes + VL::TerrainRvtBudgetOwner::MetadataBytesPerLease);
    EXPECT_EQ(budget.Statistics().allocations, imageCount + 2);
    for (size_t revision = 0; revision < 120; ++revision)
    {
        auto producer = Restore();
        auto next = reserveRevision(current->uses.size());
        for (const auto& entry : current->uses)
        {
            const auto& id = entry.first;
            if (id == "coverage-ids" || id == "coverage-weights")
            {
                next->uses[id] = producer->RetainImageUse(id, Phase::Candidate);
                next->aliases[id] = producer->Image(id);
                EXPECT_NE(next->uses[id]->GetAllocation().allocationIdentity,
                    entry.second->GetAllocation().allocationIdentity);
            }
            else
            {
                next->uses[id] = entry.second->Retain(Phase::Candidate);
                next->aliases[id] = entry.second->GetTexture();
                EXPECT_EQ(next->uses[id]->GetAllocation().allocationIdentity,
                    entry.second->GetAllocation().allocationIdentity);
            }
        }
        auto oldEpoch = current->aliases.at("coverage-ids");
        std::weak_ptr<Texture> oldCoverage = oldEpoch;
        std::weak_ptr<Resources> producerWeak = producer;
        producer.reset();
        EXPECT_TRUE(producerWeak.expired());
        next->SetPhase(Phase::Active);
        current->SetPhase(Phase::Retiring);
        EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)], steady);
        current.reset();
        EXPECT_FALSE(oldCoverage.expired());
        EXPECT_EQ(budget.Statistics().current.gpuBytes, steady.gpuBytes + originalCoverage.gpuBytes);
        EXPECT_EQ(budget.Statistics().current.cpuBytes, steady.cpuBytes + originalCoverage.cpuBytes);
        EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Retiring)], originalCoverage);
        oldEpoch.reset();
        EXPECT_TRUE(oldCoverage.expired());
        current = std::move(next);
        EXPECT_EQ(budget.Statistics().current, steady);
        EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)], steady);
        EXPECT_EQ(budget.Statistics().allocations, imageCount + 2);
    }
    current.reset();
    EXPECT_EQ(destroyed, uploads);
    EXPECT_EQ(budget.Statistics().current, VL::TerrainRvtCost{});
}

TEST_F(TerrainProductionLeaf, PreparedTransitionCancellationDoesNotChangeUseOrBudgetPhases)
{
    auto original = Restore();
    original->SetPhase(Phase::Active);
    auto old = original->RetainImageUse("albedo", Phase::Active);
    auto next = old->Retain(Phase::Candidate);
    original.reset();
    const auto before = budget.Statistics();
    {
        auto ticket = Resources::PrepareTransitions({{old, Phase::Retiring}, {next, Phase::Active}}, budget);
        ASSERT_TRUE(ticket);
        EXPECT_EQ(old->Phase(), Phase::Active);
        EXPECT_EQ(next->Phase(), Phase::Candidate);
        ASSERT_TRUE(ticket->TryLockLeavesNoThrow());
        ASSERT_EQ(ticket->BudgetChanges().size(), 1u);
        EXPECT_EQ(ticket->BudgetChanges().front().phase, Phase::Active);
        ticket->CancelNoThrow();
        EXPECT_TRUE(ticket->BudgetChanges().empty());
        EXPECT_EQ(old->Phase(), Phase::Active);
        EXPECT_EQ(next->Phase(), Phase::Candidate);
        EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)].gpuBytes,
            before.byPhase[size_t(Phase::Active)].gpuBytes);
    }
    EXPECT_EQ(budget.Statistics().current, before.current);
}

TEST_F(TerrainProductionLeaf, SecondInvalidTransitionCannotPartiallyPromoteFirstUse)
{
    auto original = Restore();
    auto first = original->RetainImageUse("albedo", Phase::Candidate);
    auto second = original->RetainImageUse("normal", Phase::Active);
    original.reset();
    const auto before = budget.Statistics();
    {
        auto ticket = Resources::PrepareTransitions({{first, Phase::Active}, {second, Phase::Candidate}}, budget);
        ASSERT_TRUE(ticket);
        EXPECT_FALSE(ticket->TryLockLeavesNoThrow());
        EXPECT_TRUE(ticket->BudgetChanges().empty());
        EXPECT_EQ(first->Phase(), Phase::Candidate);
        EXPECT_EQ(second->Phase(), Phase::Active);
        EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)].gpuBytes,
            before.byPhase[size_t(Phase::Active)].gpuBytes);
    }
    EXPECT_EQ(budget.Statistics().current, before.current);
    EXPECT_FALSE(Resources::PrepareTransitions({{first, Phase::Active}, {nullptr, Phase::Active}}, budget));
    EXPECT_FALSE(Resources::PrepareTransitions({{first, Phase::Active}, {second, Phase::Count}}, budget));
    EXPECT_FALSE(Resources::PrepareTransitions({{first, Phase::Active}, {first, Phase::Retiring}}, budget));
    EXPECT_EQ(budget.Statistics().current, before.current);
}

TEST_F(TerrainProductionLeaf, MergedRuntimeBatchFailureLeavesOldUseAndTablePhasesUnchanged)
{
    auto original = Restore();
    original->SetPhase(Phase::Active);
    auto old = original->RetainImageUse("albedo", Phase::Active);
    auto next = old->Retain(Phase::Candidate);
    original.reset();
    auto oldTable = budget.TryReserve({4096, 32, 0, 0}, VL::TerrainRvtCostClass::Workspace, Phase::Active);
    auto nextTable = budget.TryReserve({4096, 32, 0, 0}, VL::TerrainRvtCostClass::Workspace, Phase::Candidate);
    ASSERT_TRUE(oldTable);
    ASSERT_TRUE(nextTable);
    const auto before = budget.Statistics();
    {
        auto ticket = Resources::PrepareTransitions({{old, Phase::Retiring}, {next, Phase::Active}}, budget);
        ASSERT_TRUE(ticket);
        ASSERT_TRUE(ticket->TryLockLeavesNoThrow());
        auto merged = ticket->BudgetChanges();
        merged.push_back({*oldTable, Phase::Retiring});
        merged.push_back({*nextTable, Phase::Active});
        auto runtimeBatch = VL::TerrainRvtBudgetPhaseBatch::Prepare(merged, budget);
        ASSERT_TRUE(runtimeBatch);
        VL::TerrainRvtBudgetTestAccess::FailTryLockAfter(0);
        EXPECT_FALSE(runtimeBatch->TryCommitNoThrow());
        ticket->CancelNoThrow();
        EXPECT_EQ(old->Phase(), Phase::Active);
        EXPECT_EQ(next->Phase(), Phase::Candidate);
        EXPECT_EQ(oldTable->Phase(), Phase::Active);
        EXPECT_EQ(nextTable->Phase(), Phase::Candidate);
        EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)].gpuBytes,
            before.byPhase[size_t(Phase::Active)].gpuBytes);
    }
    EXPECT_EQ(budget.Statistics().current, before.current);
    auto retry = Resources::PrepareTransitions({{old, Phase::Retiring}, {next, Phase::Active}}, budget);
    ASSERT_TRUE(retry);
    ASSERT_TRUE(retry->TryLockLeavesNoThrow());
    auto merged = retry->BudgetChanges();
    merged.push_back({*oldTable, Phase::Retiring});
    merged.push_back({*nextTable, Phase::Active});
    auto runtimeBatch = VL::TerrainRvtBudgetPhaseBatch::Prepare(merged, budget);
    ASSERT_TRUE(runtimeBatch);
    ASSERT_TRUE(runtimeBatch->TryCommitNoThrow());
    VL::TerrainRvtBudgetTestAccess::WithStateLock(budget, [&] { retry->FinalizeNoThrow(); });
    EXPECT_EQ(old->Phase(), Phase::Retiring);
    EXPECT_EQ(next->Phase(), Phase::Active);
    EXPECT_EQ(oldTable->Phase(), Phase::Retiring);
    EXPECT_EQ(nextTable->Phase(), Phase::Active);
}

TEST_F(TerrainProductionLeaf, SharedLeafStaysActiveWhileReplacedLeafRetiresUntilEpochLastReference)
{
    auto original = Restore();
    original->SetPhase(Phase::Active);
    auto oldShared = original->RetainImageUse("normal", Phase::Active);
    auto nextShared = oldShared->Retain(Phase::Candidate);
    auto oldCoverage = original->RetainImageUse("coverage-ids", Phase::Active);
    auto epoch = oldCoverage->GetTexture();
    const auto coverageCost = oldCoverage->GetAllocation().actualCost;
    const auto sharedCost = oldShared->GetAllocation().actualCost;
    original.reset();
    {
        auto ticket = Resources::PrepareTransitions(
            {{oldShared, Phase::Retiring}, {nextShared, Phase::Active}, {oldCoverage, Phase::Retiring}}, budget);
        ASSERT_TRUE(ticket);
        ASSERT_TRUE(ticket->TryLockLeavesNoThrow());
        ASSERT_EQ(ticket->BudgetChanges().size(), 2u);
        auto batch = VL::TerrainRvtBudgetPhaseBatch::Prepare(ticket->BudgetChanges(), budget);
        ASSERT_TRUE(batch);
        ASSERT_TRUE(batch->TryCommitNoThrow());
        ticket->FinalizeNoThrow();
        EXPECT_EQ(oldShared->Phase(), Phase::Retiring);
        EXPECT_EQ(nextShared->Phase(), Phase::Active);
        EXPECT_EQ(oldCoverage->Phase(), Phase::Retiring);
        EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)], sharedCost);
        EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Retiring)], coverageCost);
    }
    oldShared.reset();
    oldCoverage.reset();
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Retiring)], coverageCost);
    epoch.reset();
    EXPECT_EQ(budget.Statistics().current, sharedCost);
}

TEST_F(TerrainProductionLeaf, TransitionPrepareMetadataFailureAndDuplicatesLeaveUsesUnchanged)
{
    auto original = Restore();
    auto use = original->RetainImageUse("albedo", Phase::Candidate);
    original.reset();
    auto caps = LeafCaps();
    caps.workspaceCpuBytes = 1;
    VL::TerrainRvtBudgetOwner tiny(caps);
    EXPECT_FALSE(Resources::PrepareTransitions({{use, Phase::Active}}, tiny));
    EXPECT_EQ(tiny.Statistics().current, VL::TerrainRvtCost{});
    EXPECT_EQ(use->Phase(), Phase::Candidate);
    auto ticket = Resources::PrepareTransitions({{use, Phase::Active}, {use, Phase::Active}}, budget);
    ASSERT_TRUE(ticket);
    ASSERT_TRUE(ticket->TryLockLeavesNoThrow());
    EXPECT_EQ(ticket->BudgetChanges().size(), 1u);
    ticket.reset();
    EXPECT_EQ(use->Phase(), Phase::Candidate);
    auto empty = Resources::PrepareTransitions({}, budget);
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->TryLockLeavesNoThrow());
    EXPECT_TRUE(empty->BudgetChanges().empty());
    empty->FinalizeNoThrow();
    EXPECT_FALSE(empty->TryLockLeavesNoThrow());
}

TEST_F(TerrainProductionLeaf, ConcurrentLeafLockContentionReturnsFalseAndReleasesEarlierLocks)
{
    auto original = Restore();
    auto first = original->RetainImageUse("albedo", Phase::Candidate);
    auto second = original->RetainImageUse("normal", Phase::Candidate);
    original.reset();
    std::promise<bool> ready;
    auto acquired = ready.get_future();
    std::promise<void> release;
    auto mayRelease = release.get_future();
    std::thread blocker([&]
    {
        auto ticket = Resources::PrepareTransitions({{second, Phase::Active}}, budget);
        const bool locked = ticket && ticket->TryLockLeavesNoThrow();
        ready.set_value(locked);
        mayRelease.wait();
    });
    const bool blocking = acquired.get();
    EXPECT_TRUE(blocking);
    auto ticket = Resources::PrepareTransitions({{first, Phase::Active}, {second, Phase::Active}}, budget);
    EXPECT_TRUE(bool(ticket));
    if (ticket)
    {
        EXPECT_FALSE(ticket->TryLockLeavesNoThrow());
        EXPECT_TRUE(ticket->BudgetChanges().empty());
        EXPECT_EQ(first->Phase(), Phase::Candidate);
    }
    release.set_value();
    blocker.join();
    EXPECT_EQ(second->Phase(), Phase::Candidate);
    ASSERT_TRUE(ticket);
    EXPECT_TRUE(ticket->TryLockLeavesNoThrow());
    ticket->CancelNoThrow();
}

TEST_F(TerrainProductionLeaf, ConcurrentUseMutationBetweenPrepareAndLockIsRevalidated)
{
    auto original = Restore();
    auto use = original->RetainImageUse("albedo", Phase::Candidate);
    original.reset();
    auto ticket = Resources::PrepareTransitions({{use, Phase::Active}}, budget);
    ASSERT_TRUE(ticket);
    std::thread mutation([&] { use->SetPhase(Phase::Retiring); });
    mutation.join();
    EXPECT_FALSE(ticket->TryLockLeavesNoThrow());
    EXPECT_TRUE(ticket->BudgetChanges().empty());
    EXPECT_EQ(use->Phase(), Phase::Retiring);
}

TEST_F(TerrainProductionLeaf, ConcurrentSnapshotDestructionCannotChangePreparedLeafAggregation)
{
    auto original = Restore();
    auto use = original->RetainImageUse("albedo", Phase::Candidate);
    auto ticket = Resources::PrepareTransitions({{use, Phase::Active}}, budget);
    ASSERT_TRUE(ticket);
    ASSERT_TRUE(ticket->TryLockLeavesNoThrow());
    ASSERT_EQ(ticket->BudgetChanges().size(), 1u);
    EXPECT_EQ(ticket->BudgetChanges().front().phase, Phase::Candidate);
    std::promise<void> started;
    auto entered = started.get_future();
    std::promise<void> finished;
    auto done = finished.get_future();
    std::thread destruction([snapshot = std::move(original), &started, &finished]() mutable
    {
        started.set_value();
        snapshot.reset();
        finished.set_value();
    });
    entered.wait();
    EXPECT_EQ(done.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    auto batch = VL::TerrainRvtBudgetPhaseBatch::Prepare(ticket->BudgetChanges(), budget);
    EXPECT_TRUE(bool(batch));
    if (batch && batch->TryCommitNoThrow())
        ticket->FinalizeNoThrow();
    else
        ticket->CancelNoThrow();
    destruction.join();
    EXPECT_EQ(use->Phase(), Phase::Active);
    EXPECT_EQ(budget.Statistics().byPhase[size_t(Phase::Active)].gpuBytes,
        use->GetAllocation().actualCost.gpuBytes);
}

TEST_F(TerrainProductionLeaf, PreparedTicketKeepsUseAliveUntilCancelAndUnlocksBeforeDroppingLastReference)
{
    auto original = Restore();
    auto use = original->RetainImageUse("albedo", Phase::Candidate);
    const auto leafCost = use->GetAllocation().actualCost;
    original.reset();
    std::weak_ptr<Resources::PhaseUse> weak = use;
    auto ticket = Resources::PrepareTransitions({{use, Phase::Active}}, budget);
    ASSERT_TRUE(ticket);
    use.reset();
    EXPECT_FALSE(weak.expired());
    ASSERT_TRUE(ticket->TryLockLeavesNoThrow());
    ticket.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(budget.Statistics().current, VL::TerrainRvtCost{});
    EXPECT_GT(leafCost.gpuBytes, 0u);
}

TEST_F(TerrainProductionLeaf, ConcurrentRetainWaitsForJointSwitchAndRejectsRetiredSourceUse)
{
    auto original = Restore();
    original->SetPhase(Phase::Active);
    auto old = original->RetainImageUse("albedo", Phase::Active);
    auto next = old->Retain(Phase::Candidate);
    original.reset();
    auto ticket = Resources::PrepareTransitions({{old, Phase::Retiring}, {next, Phase::Active}}, budget);
    ASSERT_TRUE(ticket);
    ASSERT_TRUE(ticket->TryLockLeavesNoThrow());
    std::promise<void> started;
    auto entered = started.get_future();
    std::promise<bool> rejected;
    auto result = rejected.get_future();
    std::thread retention([&]
    {
        started.set_value();
        try
        {
            auto retained = old->Retain(Phase::Candidate);
            rejected.set_value(false);
        }
        catch (const std::logic_error&)
        {
            rejected.set_value(true);
        }
    });
    entered.wait();
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    auto batch = VL::TerrainRvtBudgetPhaseBatch::Prepare(ticket->BudgetChanges(), budget);
    EXPECT_TRUE(bool(batch));
    const bool committed = batch && batch->TryCommitNoThrow();
    EXPECT_TRUE(committed);
    if (committed)
        ticket->FinalizeNoThrow();
    else
        ticket->CancelNoThrow();
    retention.join();
    EXPECT_TRUE(result.get());
    EXPECT_EQ(old->Phase(), Phase::Retiring);
    EXPECT_EQ(next->Phase(), Phase::Active);
}

TEST_F(TerrainProductionLeaf, MovedLockedTicketRetainsOwnershipAndReleasesLocksBeforeUses)
{
    auto original = Restore();
    auto use = original->RetainImageUse("albedo", Phase::Candidate);
    original.reset();
    auto ticket = Resources::PrepareTransitions({{use, Phase::Active}}, budget);
    ASSERT_TRUE(ticket);
    ASSERT_TRUE(ticket->TryLockLeavesNoThrow());
    {
        auto moved = std::move(*ticket);
        ticket.reset();
        ASSERT_EQ(moved.BudgetChanges().size(), 1u);
        auto batch = VL::TerrainRvtBudgetPhaseBatch::Prepare(moved.BudgetChanges(), budget);
        ASSERT_TRUE(batch);
        ASSERT_TRUE(batch->TryCommitNoThrow());
        moved.FinalizeNoThrow();
        moved.FinalizeNoThrow();
        EXPECT_EQ(use->Phase(), Phase::Active);
    }
    use.reset();
    EXPECT_EQ(budget.Statistics().current, VL::TerrainRvtCost{});
}

TEST_F(TerrainProductionLeaf, PreparationObservesBudgetPhaseAlreadyPromotedOutsideLeafUses)
{
    auto original = Restore();
    auto use = original->RetainImageUse("albedo", Phase::Candidate);
    budget.SetPhase(Phase::Active);
    auto ticket = Resources::PrepareTransitions({{use, Phase::Active}}, budget);
    ASSERT_TRUE(ticket);
    ASSERT_TRUE(ticket->TryLockLeavesNoThrow());
    ASSERT_EQ(ticket->BudgetChanges().size(), 1u);
    EXPECT_EQ(ticket->BudgetChanges().front().phase, Phase::Active);
    auto batch = VL::TerrainRvtBudgetPhaseBatch::Prepare(ticket->BudgetChanges(), budget);
    ASSERT_TRUE(batch);
    ASSERT_TRUE(batch->TryCommitNoThrow());
    ticket->FinalizeNoThrow();
    EXPECT_EQ(use->Phase(), Phase::Active);
}
}
