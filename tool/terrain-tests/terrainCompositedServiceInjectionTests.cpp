#include <gtest/gtest.h>
#include "render/resource/terrainCompositedService.cpp"

namespace VL
{
namespace
{
class TerrainCompositedServiceInjection : public testing::Test
{
protected:
    void SetUp() override
    {
        TerrainCompositedService::SetAuthoringMode(true);
        factory.reset(new PipelineFactory(&backend, device));
        service = std::make_unique<TerrainCompositedService>(backend, *factory, streaming);
        auto& state = *service->impl;
        state.worldGeneration = 42;
        TerrainRvtBudgetCaps caps;
        caps.totalCpuBytes = caps.workspaceCpuBytes = 64 * 1024 * 1024;
        state.geometryOwner = std::make_shared<TerrainRvtBudgetOwner>(caps);
        state.frozen.emplace();
        state.frozen->runtime.owner = state.geometryOwner;
        state.sourceSnapshots = std::make_unique<TerrainCompositedSourceSnapshots>(state.geometryOwner);
        TerrainBuildPlan plan;
        plan.placementId = "terrain|injection";
        plan.desc.assetPath = "TR_injection.json";
        plan.desc.materialInstancePath = "MI_injection.json";
        plan.desc.size = {1000, 1000};
        plan.desc.gridResolution = 4;
        plan.desc.chunkQuads = 2;
        plan.desc.pageTilesPerSide = 1;
        plan.desc.heightScale = 100;
        plan.desc.layers = {{"first", 0, 1, 0, 90, 0}, {"second", 0, 1, 0, 90, 0}};
        auto height = std::make_shared<HostImage>();
        height->width = height->height = 8;
        height->channels = 1;
        height->format = HostImage::PixelFormat::R32_FLOAT;
        height->rowStrideBytes = 8 * sizeof(float);
        height->data.resize(256);
        plan.heightmap = height;
        auto paint = std::make_shared<TerrainLayerPaintData>();
        paint->width = paint->height = 8;
        paint->layerCount = 2;
        paint->weights.resize(128, .5f);
        paint->opacity.resize(128, 255);
        plan.layerPaint = paint;
        oldPlan = state.sourceSnapshots->Retain(plan);
        streaming.pageManager.RegisterTerrain(oldPlan, false);
        auto surface = std::make_unique<TerrainCompositedService::Impl::Surface>();
        surface->plan = oldPlan;
        surface->desc.surfaceId = plan.placementId;
        surface->desc.width = surface->desc.height = 256;
        surface->desc.channels = {TerrainCompositedVTFormat::RGBA8Srgb,
            TerrainCompositedVTFormat::RGBA8UNorm, TerrainCompositedVTFormat::RGBA8UNorm};
        surface->input.uvScale = 64;
        material = std::make_shared<MaterialInstance>();
        material->SetParameter("u_uvScale", 64.f);
        material->SetParameter("u_detailStrength", 1.f);
        surface->materials.push_back(material);
        state.expectedSurfaces.insert(plan.placementId);
        state.adjacency = TerrainCompositedAdjacencyGraph({plan.placementId}, {});
        state.surfaces.push_back(std::move(surface));
    }

    TerrainBuildPlan EditedPlan()
    {
        auto plan = oldPlan;
        auto height = std::make_shared<HostImage>(*plan.heightmap);
        height->data[0] = 1;
        plan.heightmap = height;
        auto paint = std::make_shared<TerrainLayerPaintData>(*plan.layerPaint);
        paint->weights[0] = .75f;
        plan.layerPaint = paint;
        return plan;
    }

    TerrainCompositedRefreshTicket Queue(TerrainCompositedDependencyChange kind)
    {
        return service->QueueRefresh(TerrainCompositedRefreshCommand::Freeze(
            EditedPlan(), {oldPlan.placementId, kind, {.1, .1, .3, .3}}, 42, 1, 1));
    }

    void ExpectOld()
    {
        EXPECT_EQ(service->impl->surfaces.front()->plan.heightmap, oldPlan.heightmap);
        EXPECT_EQ(service->impl->surfaces.front()->plan.layerPaint, oldPlan.layerPaint);
        EXPECT_FLOAT_EQ(service->impl->surfaces.front()->input.uvScale, 64);
        EXPECT_FLOAT_EQ(material->GetParameter<float>("u_uvScale"), 64);
    }

    RendererBackendVulkan backend;
    vk::Device device;
    std::unique_ptr<PipelineFactory> factory;
    TerrainStreamingState streaming;
    std::unique_ptr<TerrainCompositedService> service;
    TerrainBuildPlan oldPlan;
    std::shared_ptr<MaterialInstance> material;
};

TEST_F(TerrainCompositedServiceInjection, PaintPreparationKeepsActualCurrentSurfaceAndMaterial)
{
    const auto ticket = Queue(TerrainCompositedDependencyChange::Paint);
    service->impl->PrepareRefreshes();
    ASSERT_EQ(service->PollRefresh(ticket).state, TerrainCompositedRefreshState::Prepared)
        << service->PollRefresh(ticket).diagnostic;
    ASSERT_EQ(service->impl->updates.size(), 1u);
    EXPECT_NE(service->impl->updates.begin()->second.plan.layerPaint, oldPlan.layerPaint);
    ExpectOld();
}

TEST_F(TerrainCompositedServiceInjection, SculptPreparationKeepsActualCurrentSourcesUntilCandidate)
{
    const auto ticket = Queue(TerrainCompositedDependencyChange::Sculpt);
    service->impl->PrepareRefreshes();
    ASSERT_EQ(service->PollRefresh(ticket).state, TerrainCompositedRefreshState::Prepared)
        << service->PollRefresh(ticket).diagnostic;
    ASSERT_EQ(service->impl->updates.size(), 1u);
    EXPECT_NE(service->impl->updates.begin()->second.plan.heightmap, oldPlan.heightmap);
    ExpectOld();
}

TEST_F(TerrainCompositedServiceInjection, ActualServiceDoesNotStartRuntimeBeforeInputFence)
{
    auto candidate = std::make_shared<TerrainCompositedService::Impl::WorldCandidate>(
        42, 1, std::vector<std::string>{oldPlan.placementId});
    candidate->transaction.RecordInputs();
    candidate->transaction.SubmitInputs(5);
    service->impl->worldCandidate = candidate;
    service->impl->AdvanceWorldCandidate(4);
    EXPECT_FALSE(candidate->runtimeStarted);
    EXPECT_EQ(candidate->transaction.Phase(), TerrainCompositedSnapshotPhase::InputsSubmitted);
    EXPECT_EQ(service->impl->worldCandidate, candidate);
    ExpectOld();
}

TEST_F(TerrainCompositedServiceInjection, ActualServiceBadRootStatusDiscardsCandidateAndKeepsOld)
{
    const auto ticket = Queue(TerrainCompositedDependencyChange::Paint);
    service->impl->PrepareRefreshes();
    auto candidate = std::make_shared<TerrainCompositedService::Impl::WorldCandidate>(
        42, 1, std::vector<std::string>{oldPlan.placementId});
    candidate->transaction.RecordInputs();
    candidate->transaction.SubmitInputs(5);
    ASSERT_TRUE(candidate->transaction.InputsCompleted(5));
    EXPECT_FALSE(candidate->transaction.ObserveProduction(1, oldPlan.placementId, true, 6, 2, true));
    service->impl->worldCandidate = candidate;
    service->impl->AdvanceWorldCandidate(6);
    EXPECT_FALSE(service->impl->worldCandidate);
    ASSERT_EQ(service->impl->retiredCandidates.size(), 1u);
    EXPECT_EQ(service->PollRefresh(ticket).state, TerrainCompositedRefreshState::Failed);
    ExpectOld();
}

TEST_F(TerrainCompositedServiceInjection, NativeMaterialAndPageSourceBatchRemainPendingUntilStatusAndTables)
{
    auto plan = service->impl->sourceSnapshots->Retain(EditedPlan());
    TerrainCompositedPageSourceBatch pages(streaming.pageManager, {plan});
    MaterialInstanceStateBatch materials({{material, {{"u_uvScale", 32.f}}, {}}});
    TerrainCompositedResourceSnapshot snapshot(42, 1, {oldPlan.placementId});
    snapshot.RecordInputs();
    snapshot.SubmitInputs(5);
    ASSERT_TRUE(snapshot.InputsCompleted(5));
    ASSERT_TRUE(snapshot.ObserveProduction(1, oldPlan.placementId, true, 6, 0, true));
    EXPECT_FALSE(snapshot.ConfirmTables(6, false));
    ExpectOld();
    ASSERT_TRUE(snapshot.ConfirmTables(7, true));
    ASSERT_TRUE(pages.CanCommitNoThrow());
    ASSERT_TRUE(materials.TryLockNoThrow());
    pages.CommitNoThrow();
    materials.CommitNoThrow();
    snapshot.PublishCommittedNoThrow();
    EXPECT_EQ(snapshot.Phase(), TerrainCompositedSnapshotPhase::Published);
    EXPECT_FLOAT_EQ(material->GetParameter<float>("u_uvScale"), 32.f);
    EXPECT_EQ(service->impl->surfaces.front()->plan.heightmap, oldPlan.heightmap);
}

TEST_F(TerrainCompositedServiceInjection, GeneratorChangeWaitsForActualOfferedProducerWithoutPublishing)
{
    const auto ticket = Queue(TerrainCompositedDependencyChange::Generator);
    service->impl->PrepareRefreshes();
    EXPECT_EQ(service->PollRefresh(ticket).state, TerrainCompositedRefreshState::Queued);
    EXPECT_TRUE(service->impl->updates.empty());
    ExpectOld();
}

TEST_F(TerrainCompositedServiceInjection, DynamicTypedRefreshCommitsParametersThenPublishesAfterSubmission)
{
    const auto identity = service->impl->surfaces.front()->desc.ContentHash();
    auto parameters = material->CaptureNumericParameterValues();
    parameters["u_detailStrength"] = 2.f;
    service->impl->materialIntents[oldPlan.desc.materialInstancePath] =
        std::make_shared<const MaterialInstanceStateSnapshot>(
            oldPlan.desc.materialInstancePath, parameters, MaterialInstanceStateSnapshot::TextureMap{});
    const auto ticket = Queue(TerrainCompositedDependencyChange::DynamicGlobal);
    service->impl->PrepareRefreshes();
    ASSERT_EQ(service->PollRefresh(ticket).state, TerrainCompositedRefreshState::Prepared)
        << service->PollRefresh(ticket).diagnostic;
    EXPECT_TRUE(service->impl->refreshes.at(ticket.id).dynamicCommitted);
    EXPECT_TRUE(service->impl->updates.empty());
    EXPECT_FALSE(service->impl->worldCandidate);
    EXPECT_FLOAT_EQ(material->GetParameter<float>("u_detailStrength"), 2.f);
    EXPECT_EQ(service->impl->surfaces.front()->desc.ContentHash(), identity);
    service->impl->UpdateRefreshes(false);
    EXPECT_EQ(service->PollRefresh(ticket).state, TerrainCompositedRefreshState::Prepared);
    EXPECT_EQ(service->RefreshRevision(42, oldPlan.placementId), 0u);
    service->impl->UpdateRefreshes(true);
    EXPECT_TRUE(service->PollRefresh(ticket).Acknowledges(ticket));
    EXPECT_EQ(service->RefreshRevision(42, oldPlan.placementId), 1u);
    EXPECT_FALSE(service->impl->refreshes.at(ticket.id).command.inputs);
    ExpectOld();
}

TEST_F(TerrainCompositedServiceInjection, DynamicTypedRefreshRejectsStaticBakeChangeWithoutChangingOldMaterial)
{
    auto parameters = material->CaptureNumericParameterValues();
    parameters["u_uvScale"] = 32.f;
    service->impl->materialIntents[oldPlan.desc.materialInstancePath] =
        std::make_shared<const MaterialInstanceStateSnapshot>(
            oldPlan.desc.materialInstancePath, parameters, MaterialInstanceStateSnapshot::TextureMap{});
    const auto ticket = Queue(TerrainCompositedDependencyChange::DynamicDetail);
    service->impl->PrepareRefreshes();
    EXPECT_EQ(service->PollRefresh(ticket).state, TerrainCompositedRefreshState::Failed);
    EXPECT_EQ(service->RefreshRevision(42, oldPlan.placementId), 0u);
    EXPECT_FLOAT_EQ(material->GetParameter<float>("u_detailStrength"), 1.f);
    ExpectOld();
}

TEST_F(TerrainCompositedServiceInjection, ActualAll16PublicationSharesOneChargedVerificationBatch)
{
    TerrainRvtBudgetCaps caps;
    caps.totalCpuBytes = caps.workspaceCpuBytes = 512 * 1024 * 1024;
    service->impl->frozen->runtime.owner = std::make_shared<TerrainRvtBudgetOwner>(caps);
    service->impl->store = std::make_shared<TerrainCompositedVTStore>(
        TerrainCompositedVTStore::Open("../p5r/C008/VT/775359b06fde29e5/manifest.json"));
    std::set<std::string> expected;
    uint64_t bytes = 0;
    std::shared_ptr<TerrainCompositedVerifiedInputs> session;
    for (const auto& surface : service->impl->store->Production().surfaces)
    {
        const auto baseline = service->LoadAuthorBaseline(42, surface.surfaceId);
        ASSERT_TRUE(baseline.verificationOwner);
        if (!session)
            session = baseline.verificationOwner;
        EXPECT_EQ(baseline.verificationOwner, session);
        for (const auto& dependency : surface.authorDependencies)
            expected.insert(dependency.productionObjectIds.begin(), dependency.productionObjectIds.end());
    }
    for (const auto& id : expected)
        bytes += service->impl->store->Production().Object(id).bytes;
    const auto before = session->Statistics();
    EXPECT_EQ(before.readCalls, expected.size());
    EXPECT_EQ(before.validatedBytes, bytes);
    EXPECT_GT(before.hits, 0u);
    session->Close();
    for (const auto& surface : service->impl->store->Production().surfaces)
        EXPECT_EQ(service->LoadAuthorBaseline(42, surface.surfaceId).verificationOwner, session);
    EXPECT_EQ(session->Statistics().readCalls, before.readCalls);
    EXPECT_TRUE(session->Statistics().closed);
    std::cout << "SERVICE_ACTUAL_PUBLICATION_PROFILE " << nlohmann::json{
        {"surfaces", service->impl->store->Production().surfaces.size()},
        {"readCalls", before.readCalls}, {"validatedBytes", before.validatedBytes},
        {"requestedBytes", before.requestedBytes}, {"hits", before.hits},
        {"beginNanoseconds", before.beginNanoseconds}, {"endNanoseconds", session->Statistics().endNanoseconds}
    }.dump() << std::endl;
}

TEST_F(TerrainCompositedServiceInjection, AuthorStateAndCandidateCopyDoNotPinRetiredRuntimePageTable)
{
    auto oldLeaf = std::make_shared<uint64_t>(1);
    std::weak_ptr<uint64_t> oldOwner = oldLeaf;
    auto oldTable = std::shared_ptr<Texture>(oldLeaf, reinterpret_cast<Texture*>(oldLeaf.get()));
    auto sourceLeaf = std::make_shared<uint64_t>(2);
    auto source = std::shared_ptr<Texture>(sourceLeaf, reinterpret_cast<Texture*>(sourceLeaf.get()));
    material->SetTexture("compositedPageTable", oldTable);
    material->SetTexture("layerAlbedo", source, "Generated/T_source.json", "source-key");
    auto& surface = *service->impl->surfaces.front();
    surface.materialStates.push_back(AuthorMaterialState(material->CaptureStateSnapshot(), {}));
    auto candidate = service->impl->Candidate(surface);
    {
        auto nextLeaf = std::make_shared<uint64_t>(3);
        auto nextTable = std::shared_ptr<Texture>(nextLeaf, reinterpret_cast<Texture*>(nextLeaf.get()));
        MaterialInstanceStateBatch transaction({{material, {}, {{"compositedPageTable", nextTable}}}});
        ASSERT_TRUE(transaction.TryLockNoThrow());
        transaction.CommitNoThrow();
    }
    oldTable.reset();
    oldLeaf.reset();
    EXPECT_TRUE(oldOwner.expired());
    ASSERT_EQ(candidate.materialStates.size(), 1u);
    const auto& author = *candidate.materialStates.front();
    EXPECT_EQ(author.textures.count("compositedPageTable"), 0u);
    EXPECT_EQ(author.textures.at("layerAlbedo").texture, source);
    EXPECT_EQ(author.textures.at("layerAlbedo").textureAssetIdentity, "Generated/T_source.json");
    EXPECT_EQ(author.textures.at("layerAlbedo").textureCacheIdentity, "source-key");
    EXPECT_FLOAT_EQ(std::get<float>(author.parameters.at("u_uvScale")), 64.f);
    EXPECT_FLOAT_EQ(std::get<float>(author.parameters.at("u_detailStrength")), 1.f);
}

TEST_F(TerrainCompositedServiceInjection, AuthorStateDropsRuntimeOverlayButPreservesActualImportedReplacement)
{
    auto overlayLeaf = std::make_shared<uint64_t>(1);
    auto overlay = std::shared_ptr<Texture>(overlayLeaf, reinterpret_cast<Texture*>(overlayLeaf.get()));
    material->SetTexture("layerWeightMaps", overlay, "Generated/T_weights.json", "original-key");
    const auto runtime = AuthorMaterialState(material->CaptureStateSnapshot(), overlay);
    EXPECT_EQ(runtime->textures.count("layerWeightMaps"), 0u);
    auto importedLeaf = std::make_shared<uint64_t>(2);
    auto imported = std::shared_ptr<Texture>(importedLeaf, reinterpret_cast<Texture*>(importedLeaf.get()));
    material->SetTexture("layerWeightMaps", imported, "Generated/T_new_weights.json", "new-key");
    const auto changed = AuthorMaterialState(material->CaptureStateSnapshot(), overlay);
    EXPECT_EQ(changed->textures.at("layerWeightMaps").texture, imported);
    EXPECT_EQ(changed->textures.at("layerWeightMaps").textureAssetIdentity, "Generated/T_new_weights.json");
    EXPECT_EQ(changed->textures.at("layerWeightMaps").textureCacheIdentity, "new-key");
}

TEST_F(TerrainCompositedServiceInjection, LocalPublicationOverlayRestoresActualAuthoredBindingInCandidate)
{
    const auto root = std::filesystem::absolute("../p5r/L010");
    const auto relative = "Generated/Validation/TerrainCompositedVT/Variants/"
        "RVTv2_Full4x4_R8192_Default_LocalLayerArray/MI_composited_tile_00.json";
    const auto identity = AuthoredWeightIdentity((root / relative).string());
    ASSERT_TRUE(identity);
    EXPECT_EQ(*identity, "Generated/HoudiniPegasusIsland/tile_00/Textures/"
        "T_terrain_kitedemo_weights_array.json");
    auto overlayLeaf = std::make_shared<uint64_t>(1);
    auto overlay = std::shared_ptr<Texture>(overlayLeaf, reinterpret_cast<Texture*>(overlayLeaf.get()));
    auto authorLeaf = std::make_shared<uint64_t>(2);
    auto author = std::shared_ptr<Texture>(authorLeaf, reinterpret_cast<Texture*>(authorLeaf.get()));
    material->SetTexture("layerWeightMaps", overlay);
    auto& surface = *service->impl->surfaces.front();
    surface.localWeights = overlay;
    surface.authorWeights = author;
    surface.authoredWeightIdentities.push_back(identity);
    surface.materialStates.push_back(AuthorMaterialState(
        material->CaptureStateSnapshot(), overlay, author, identity));
    const auto candidate = service->impl->Candidate(surface);
    const auto& binding = candidate.materialStates.front()->textures.at("layerWeightMaps");
    EXPECT_EQ(binding.texture, author);
    EXPECT_NE(binding.texture, overlay);
    EXPECT_EQ(binding.textureAssetIdentity, identity);
    EXPECT_EQ(candidate.authorWeights, author);
    EXPECT_EQ(candidate.authoredWeightIdentities.front(), identity);
    material->SetTexture("layerWeightMaps", overlay, identity, std::nullopt);
    const auto pending = AuthorMaterialState(material->CaptureStateSnapshot(), overlay, author, identity);
    EXPECT_EQ(pending->textures.at("layerWeightMaps").texture, author);
    EXPECT_EQ(pending->textures.at("layerWeightMaps").textureAssetIdentity, identity);
}

TEST_F(TerrainCompositedServiceInjection, LocalAuthoredReplacementIsNeverOverwrittenByPreviousAuthor)
{
    auto previousLeaf = std::make_shared<uint64_t>(1);
    auto previous = std::shared_ptr<Texture>(previousLeaf, reinterpret_cast<Texture*>(previousLeaf.get()));
    auto overlayLeaf = std::make_shared<uint64_t>(2);
    auto overlay = std::shared_ptr<Texture>(overlayLeaf, reinterpret_cast<Texture*>(overlayLeaf.get()));
    auto replacementLeaf = std::make_shared<uint64_t>(3);
    auto replacement = std::shared_ptr<Texture>(replacementLeaf, reinterpret_cast<Texture*>(replacementLeaf.get()));
    material->SetTexture("layerWeightMaps", replacement, "Generated/T_replacement.json", "replacement");
    const auto pending = AuthorMaterialState(material->CaptureStateSnapshot(), overlay,
        previous, "Generated/T_previous.json");
    EXPECT_EQ(pending->textures.at("layerWeightMaps").texture, replacement);
    EXPECT_EQ(pending->textures.at("layerWeightMaps").textureAssetIdentity, "Generated/T_replacement.json");
}
}
}
