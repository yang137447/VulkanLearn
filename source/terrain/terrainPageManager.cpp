#include "terrain/terrainPageManager.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#include "terrain/terrainWeightMap.h"

namespace VL
{
namespace
{

constexpr size_t InvalidIndex = static_cast<size_t>(-1);

// BuildUploadBatch 的 pin 去重：同一页可能同时出现在页表与 atlas 上传里，
// 本批只增加一次 pin 并记录一次，Commit/Abort 才不会重复解 pin。
// 注意不能因为“页已被其他批次 pin 住”就跳过：每个 in-flight 批次都必须
// 持有自己的一次 pin，否则先结束的批次会把仍在使用的页解 pin。
void PinUploadBatchPage(
    TerrainPageManager::PageState* page,
    TerrainUploadBatch& batch)
{
    if (page == nullptr || !page->resident)
        return;
    for (const TerrainPageManager::PageKey& key : batch.newlyPinnedPages)
    {
        if (key.placementId == page->key.placementId &&
            key.tileX == page->key.tileX &&
            key.tileZ == page->key.tileZ)
        {
            return;
        }
    }
    ++page->uploadPinCount;
    batch.newlyPinnedPages.push_back(page->key);
}

} // namespace

TerrainPageManager::TerrainPageManager()
{
}

void TerrainPageManager::EnsureStorage()
{
    if (!pageTable.empty()) return;
    pageTable.assign(static_cast<size_t>(PageTableSize) * PageTableSize * 4, 0);
    idAtlas.assign(static_cast<size_t>(AtlasResolution) * AtlasResolution * 4, 0);
    weightAtlas.assign(idAtlas.size(), 0);
    physicalOccupancy.assign(static_cast<size_t>(PageSlotCount), 0);
    virtualReserved.assign(static_cast<size_t>(PageSlotCount), 0);
    physicalSlotQuarantined.assign(static_cast<size_t>(PageSlotCount), 0);
    physicalSlotReleaseEpoch.assign(static_cast<size_t>(PageSlotCount), 0);
    virtualPageTableDirty.assign(static_cast<size_t>(PageSlotCount), 0);
    physicalAtlasDirty.assign(static_cast<size_t>(PageSlotCount), 0);
    virtualPageTableRevision.assign(static_cast<size_t>(PageSlotCount), 0);
    physicalAtlasRevision.assign(static_cast<size_t>(PageSlotCount), 0);
    freePhysicalSlotCount = UsablePhysicalSlotCount;

    // fallback 只保留物理 atlas 槽位 (31,31)：shader 缺页分支直接采样它，
    // 读到全零 ID/权重。页表不再写 (31,31) 保留项，因此该虚拟坐标可以正常
    // 分配给 32×32 单地形；IsResident(31,31) 也恢复为普通页语义。
    const size_t fallbackSlot =
        static_cast<size_t>(FallbackPageY) * PageTableSize + FallbackPageX;
    physicalOccupancy[fallbackSlot] = 1;
}

void TerrainPageManager::RegisterTerrain(
    const TerrainBuildPlan& plan,
    bool preload)
{
    RegisterTerrain(plan, std::vector<uint8_t>(), preload);
}

void TerrainPageManager::RegisterTerrain(
    const TerrainBuildPlan& plan,
    const std::vector<uint8_t>& layerIdMap,
    bool preload)
{
    EnsureStorage();
    const std::string& placementId = TerrainPlacementIdentity(plan);
    // 空映射是恒等映射；非空映射必须覆盖每一层，避免 ID 图出现未定义层号。
    if (!layerIdMap.empty() && layerIdMap.size() != plan.desc.layers.size())
    {
        throw std::runtime_error(
            "Terrain layer id map does not cover every layer: assetPath=" +
            plan.desc.assetPath + " placementId=" + placementId);
    }
    const uint32_t tilesPerSide = plan.desc.pageTilesPerSide;
    const int originX = plan.desc.pageOrigin.x();
    const int originY = plan.desc.pageOrigin.y();
    if (tilesPerSide == 0 ||
        originX < 0 || originY < 0 ||
        originX + static_cast<int>(tilesPerSide) > static_cast<int>(PageTableSize) ||
        originY + static_cast<int>(tilesPerSide) > static_cast<int>(PageTableSize))
    {
        throw std::runtime_error(
            "Terrain page registration exceeds the global page table: assetPath=" +
            plan.desc.assetPath + " placementId=" + placementId);
    }
    // 注册身份是 placementId：同一 TR 的多个摆放可以共存，但同一次摆放不能重复注册。
    if (FindRegistrationIndex(placementId) != InvalidIndex)
        throw std::runtime_error(
            "Terrain placement is already registered in the page table: assetPath=" +
            plan.desc.assetPath + " placementId=" + placementId);

    // 先验证整个区域再提交，避免重叠注册留下半注册状态。
    for (uint32_t tileZ = 0; tileZ < tilesPerSide; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < tilesPerSide; ++tileX)
        {
            const uint32_t globalX = static_cast<uint32_t>(originX) + tileX;
            const uint32_t globalY = static_cast<uint32_t>(originY) + tileZ;
            const size_t slot = static_cast<size_t>(globalY) * PageTableSize + globalX;
            if (virtualReserved[slot] != 0)
                throw std::runtime_error(
                    "Terrain page regions overlap in the global page table: assetPath=" +
                    plan.desc.assetPath + " placementId=" + placementId);
        }
    }

    for (uint32_t tileZ = 0; tileZ < tilesPerSide; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < tilesPerSide; ++tileX)
        {
            const uint32_t globalX = static_cast<uint32_t>(originX) + tileX;
            const uint32_t globalY = static_cast<uint32_t>(originY) + tileZ;
            virtualReserved[static_cast<size_t>(globalY) * PageTableSize + globalX] = 1;

            PageState page;
            page.key.placementId = placementId;
            page.key.tileX = tileX;
            page.key.tileZ = tileZ;
            page.virtualPageX = globalX;
            page.virtualPageY = globalY;
            pages.push_back(std::move(page));
        }
    }

    Registration registration;
    registration.placementId = placementId;
    registration.terrainAssetPath = plan.desc.assetPath;
    registration.pageOrigin = plan.desc.pageOrigin;
    registration.tilesPerSide = tilesPerSide;
    registrations.push_back(std::move(registration));
    plans.push_back(plan);
    layerIdMaps.push_back(layerIdMap);

    // C2b 兼容：默认注册即全量常驻；测试和多地形按需加载可以关闭 preload。
    if (!preload) return;
    for (uint32_t tileZ = 0; tileZ < tilesPerSide; ++tileZ)
    {
        for (uint32_t tileX = 0; tileX < tilesPerSide; ++tileX)
        {
            // 1024 虚拟页对 1023 物理槽位：最后一批请求会触发淘汰或无法驻留。
            // 失败只记一次 deferred，然后继续注册，不把容量问题变成异常/死循环。
            if (!RequestPage(placementId, tileX, tileZ))
                ++stats.preloadDeferredCount;
        }
    }
}

void TerrainPageManager::UpdateLayerPaint(
    const std::string& placementId,
    std::shared_ptr<const TerrainLayerPaintData> paint)
{
    const size_t index = FindRegistrationIndex(placementId);
    if (index == InvalidIndex)
        throw std::runtime_error(
            "Terrain layer paint targets an unregistered placement: " + placementId);
    if (paint && (paint->Empty() ||
        paint->layerCount != plans[index].desc.layers.size()))
        throw std::runtime_error(
            "Terrain layer paint dimensions do not match the registered layer table: " +
            placementId);
    plans[index].layerPaint = std::move(paint);
}

void TerrainPageManager::UpdateHeightmap(
    const std::string& placementId,
    std::shared_ptr<const HostImage> heightmap)
{
    const size_t index = FindRegistrationIndex(placementId);
    if (index == InvalidIndex)
        throw std::runtime_error(
            "Terrain heightmap update targets an unregistered placement: " +
            placementId);
    if (!heightmap)
        throw std::runtime_error(
            "Terrain heightmap update requires a decoded heightmap: " +
            placementId);
    const std::shared_ptr<const HostImage>& previous = plans[index].heightmap;
    if (previous &&
        (previous->width != heightmap->width ||
         previous->height != heightmap->height))
    {
        // 覆盖页的 tile 划分与 UV 采样依赖固定的高度图尺寸；换尺寸会让
        // 已注册的虚拟页不再对应同一块地形区域。
        throw std::runtime_error(
            "Terrain heightmap update dimensions do not match the registered "
            "heightmap: " + placementId);
    }
    plans[index].heightmap = std::move(heightmap);
}

void TerrainPageManager::InvalidatePlacementPages(const std::string& placementId)
{
    if (placementId.empty()) return;
    for (PageState& page : pages)
    {
        if (page.key.placementId != placementId || !page.resident)
            continue;
        // 有引用/上传 pin 的页不能在这里强拆；下一轮 feedback 仍会看到旧内容，
        // 但调用方会在下一帧再次尝试失效。
        static_cast<void>(EvictPageAt(page));
    }
}

uint32_t TerrainPageManager::RequestInitialTerrainPages(
    const Registration& registration,
    uint32_t tilesPerSide,
    uint32_t priority)
{
    if (registration.tilesPerSide == 0 || tilesPerSide == 0)
        return 0;
    const uint32_t width = std::min(tilesPerSide, registration.tilesPerSide);
    const uint32_t maxTile = registration.tilesPerSide - 1;
    // 居中取块；奇数宽度时 start 略偏左，保证覆盖中心区域且不越界。
    const uint32_t startX = registration.tilesPerSide > width
        ? std::min(
            registration.tilesPerSide / 2 - width / 2,
            maxTile - (width - 1))
        : 0;
    const uint32_t startZ = startX;
    const uint32_t endX = std::min(startX + width - 1, maxTile);
    const uint32_t endZ = std::min(startZ + width - 1, maxTile);
    uint32_t residentPages = 0;
    for (uint32_t tileZ = startZ; tileZ <= endZ; ++tileZ)
    {
        for (uint32_t tileX = startX; tileX <= endX; ++tileX)
        {
            if (RequestPage(
                    registration.placementId,
                    tileX,
                    tileZ,
                    0,
                    priority))
            {
                ++residentPages;
            }
        }
    }
    return residentPages;
}

size_t TerrainPageManager::FindRegistrationIndex(const std::string& placementId) const
{
    for (size_t index = 0; index < registrations.size(); ++index)
    {
        if (registrations[index].placementId == placementId)
            return index;
    }
    return InvalidIndex;
}

TerrainPageManager::PageState* TerrainPageManager::FindPageMutable(const PageKey& key)
{
    for (PageState& page : pages)
    {
        if (page.key.tileX == key.tileX &&
            page.key.tileZ == key.tileZ &&
            page.key.placementId == key.placementId)
        {
            return &page;
        }
    }
    return nullptr;
}

const TerrainPageManager::PageState* TerrainPageManager::FindPage(const PageKey& key) const
{
    for (const PageState& page : pages)
    {
        if (page.key.tileX == key.tileX &&
            page.key.tileZ == key.tileZ &&
            page.key.placementId == key.placementId)
        {
            return &page;
        }
    }
    return nullptr;
}

TerrainPageManager::PageState* TerrainPageManager::FindPageByVirtualMutable(
    uint32_t virtualPageX,
    uint32_t virtualPageY)
{
    for (PageState& page : pages)
    {
        if (page.virtualPageX == virtualPageX && page.virtualPageY == virtualPageY)
            return &page;
    }
    return nullptr;
}

const TerrainPageManager::PageState* TerrainPageManager::FindPageByVirtual(
    uint32_t virtualPageX,
    uint32_t virtualPageY) const
{
    for (const PageState& page : pages)
    {
        if (page.virtualPageX == virtualPageX && page.virtualPageY == virtualPageY)
            return &page;
    }
    return nullptr;
}

const TerrainBuildPlan* TerrainPageManager::FindPlan(
    const std::string& placementId) const
{
    // registrations 与 plans 在 RegisterTerrain 中同步追加，索引一一对应。
    const size_t count = std::min(registrations.size(), plans.size());
    for (size_t index = 0; index < count; ++index)
    {
        if (registrations[index].placementId == placementId)
            return &plans[index];
    }
    return nullptr;
}

const std::vector<uint8_t>& TerrainPageManager::LayerIdMapForPlanIndex(
    size_t planIndex) const
{
    static const std::vector<uint8_t> EmptyMap;
    if (planIndex >= plans.size() || planIndex >= layerIdMaps.size())
        return EmptyMap;
    return layerIdMaps[planIndex];
}

const std::vector<uint8_t>& TerrainPageManager::PlacementLayerIdMap(
    const std::string& placementId) const
{
    const size_t index = FindRegistrationIndex(placementId);
    static const std::vector<uint8_t> EmptyMap;
    if (index == InvalidIndex)
        return EmptyMap;
    return LayerIdMapForPlanIndex(index);
}

bool TerrainPageManager::ReadPageLayers(
    const PageKey& key,
    bool allowRebuild,
    std::vector<uint8_t>& ids,
    std::vector<uint8_t>& weights) const
{
    const PageState* page = FindPage(key);
    if (page == nullptr) return false;
    if (page->resident)
    {
        // 常驻页的 CPU atlas 镜像与 GPU 上传内容同源，避免再采样一次高度图。
        CopyAtlasPage(page->physicalPageX, page->physicalPageY, idAtlas, ids);
        CopyAtlasPage(page->physicalPageX, page->physicalPageY, weightAtlas, weights);
        return true;
    }
    if (!allowRebuild) return false;

    const TerrainBuildPlan* plan = FindPlan(key.placementId);
    if (plan == nullptr) return false;
    const size_t registrationIndex = FindRegistrationIndex(key.placementId);
    if (registrationIndex == InvalidIndex) return false;
    // 非驻留页只做 CPU 重建，供可见层推导使用；不改变驻留/脏状态。
    const TerrainPageData data = TerrainWeightMapBuilder::BuildPage(
        *plan,
        registrations[registrationIndex].tilesPerSide,
        PageResolution,
        key.tileX,
        key.tileZ,
        LayerIdMapForPlanIndex(registrationIndex));
    ids = data.ids;
    weights = data.weights;
    return true;
}

TerrainPageManager::PageState* TerrainPageManager::FindPageByPhysicalMutable(
    uint32_t physicalPageX,
    uint32_t physicalPageY)
{
    for (PageState& page : pages)
    {
        if (page.resident &&
            page.physicalPageX == physicalPageX &&
            page.physicalPageY == physicalPageY)
        {
            return &page;
        }
    }
    return nullptr;
}

void TerrainPageManager::ZeroPageTableEntry(uint32_t virtualPageX, uint32_t virtualPageY)
{
    const size_t pageEntry =
        (static_cast<size_t>(virtualPageY) * PageTableSize + virtualPageX) * 4;
    pageTable[pageEntry + 0] = 0;
    pageTable[pageEntry + 1] = 0;
    pageTable[pageEntry + 2] = 0;
    pageTable[pageEntry + 3] = 0; // shader 以 A < 0.5 判定无效页
}

void TerrainPageManager::MarkPageTableDirty(uint32_t virtualPageX, uint32_t virtualPageY)
{
    const size_t slot =
        static_cast<size_t>(virtualPageY) * PageTableSize + virtualPageX;
    virtualPageTableDirty[slot] = 1;
    ++virtualPageTableRevision[slot];
}

void TerrainPageManager::MarkAtlasPageDirty(uint32_t physicalPageX, uint32_t physicalPageY)
{
    const size_t slot =
        static_cast<size_t>(physicalPageY) * PageTableSize + physicalPageX;
    physicalAtlasDirty[slot] = 1;
    ++physicalAtlasRevision[slot];
}

bool TerrainPageManager::AllocatePhysicalSlot(
    uint32_t& physicalPageX,
    uint32_t& physicalPageY,
    uint64_t completedEpoch)
{
    // 过期隔离槽位与普通空槽位等价。先回收，保证 free 计数、quarantine 计数
    // 和实际可分配结果一致；回收是幂等的，不会重复计数。
    ReclaimExpiredQuarantines(completedEpoch);
    // 从低槽位开始找，保证测试和画面结果可复现；fallback 槽位已标占用会被跳过。
    for (uint32_t slot = 0; slot < PageSlotCount; ++slot)
    {
        if (physicalOccupancy[slot] == 0)
        {
            physicalOccupancy[slot] = 1;
            --freePhysicalSlotCount;
        }
        else if (physicalSlotQuarantined[slot] != 0)
        {
            // 隔离期内的槽位仍可能被旧 in-flight 帧采样，不能立即覆盖。
            if (physicalSlotReleaseEpoch[slot] > completedEpoch)
                continue;
            physicalSlotQuarantined[slot] = 0;
            physicalSlotReleaseEpoch[slot] = 0;
            --quarantinedPhysicalSlotCount;
        }
        else
        {
            continue;
        }
        physicalPageX = slot % PageTableSize;
        physicalPageY = slot / PageTableSize;
        return true;
    }
    return false;
}

void TerrainPageManager::QuarantinePhysicalSlot(
    uint32_t physicalPageX,
    uint32_t physicalPageY,
    uint64_t lastSeenEpoch)
{
    const size_t slot =
        static_cast<size_t>(physicalPageY) * PageTableSize + physicalPageX;
    if (physicalOccupancy[slot] == 0) return;
    if (physicalSlotQuarantined[slot] != 0) return;
    // 公式覆盖“本帧及之前所有已提交帧”：lastSubmitted 是本帧即将提交的 epoch
    // （或上一帧），再加 rings-in-flight 的余量，completedEpoch 追平后才可复用。
    const uint64_t lastGpuEpoch =
        std::max(lastSeenEpoch, gpuLastSubmittedEpoch);
    physicalSlotQuarantined[slot] = 1;
    physicalSlotReleaseEpoch[slot] =
        lastGpuEpoch + static_cast<uint64_t>(gpuFramesInFlight) + 1u;
    ++quarantinedPhysicalSlotCount;
}

bool TerrainPageManager::MakeResident(
    size_t registrationIndex,
    PageState& page,
    uint64_t frame,
    uint32_t priority,
    uint64_t completedEpoch)
{
    const Registration& registration = registrations[registrationIndex];
    // 只烘焙当前 tile，避免为了一个页复制整张地形权重图。
    const TerrainPageData data = TerrainWeightMapBuilder::BuildPage(
        plans[registrationIndex],
        registration.tilesPerSide,
        PageResolution,
        page.key.tileX,
        page.key.tileZ,
        LayerIdMapForPlanIndex(registrationIndex));

    // 烘焙成功后再占用物理槽位，避免烘焙抛异常时泄漏槽位。
    uint32_t physicalPageX = 0;
    uint32_t physicalPageY = 0;
    if (!AllocatePhysicalSlot(physicalPageX, physicalPageY, completedEpoch))
        return false;
    page.physicalPageX = physicalPageX;
    page.physicalPageY = physicalPageY;

    // 页表项写在虚拟坐标处，但内容是 atlas 物理槽位。
    const size_t pageEntry =
        (static_cast<size_t>(page.virtualPageY) * PageTableSize + page.virtualPageX) * 4;
    pageTable[pageEntry + 0] = static_cast<uint8_t>(physicalPageX);
    pageTable[pageEntry + 1] = static_cast<uint8_t>(physicalPageY);
    pageTable[pageEntry + 2] = 0;
    pageTable[pageEntry + 3] = 255;
    MarkPageTableDirty(page.virtualPageX, page.virtualPageY);

    for (uint32_t y = 0; y < PageResolution; ++y)
    {
        const size_t srcOffset = static_cast<size_t>(y) * PageResolution * 4;
        const size_t dstOffset =
            ((static_cast<size_t>(physicalPageY) * PageResolution + y) * AtlasResolution +
             static_cast<size_t>(physicalPageX) * PageResolution) * 4;
        std::memcpy(
            idAtlas.data() + dstOffset,
            data.ids.data() + srcOffset,
            static_cast<size_t>(PageResolution) * 4);
        std::memcpy(
            weightAtlas.data() + dstOffset,
            data.weights.data() + srcOffset,
            static_cast<size_t>(PageResolution) * 4);
    }
    MarkAtlasPageDirty(physicalPageX, physicalPageY);

    page.resident = true;
    page.lastUsedFrame = frame;
    // 新驻留页以“本帧已提交 epoch”起算：这个页内容要到该帧提交后才可能被
    // GPU 看到，用已消费 feedback 的旧 epoch 会让它一出生就过了安全窗。
    // 还没有提交过（加载期 preload）时退回已消费 feedback epoch；两者都为 0
    // 时保持既有行为。
    const uint64_t residentEpoch =
        gpuLastSubmittedEpoch != 0 ? gpuLastSubmittedEpoch : gpuLastConsumedFeedbackEpoch;
    page.lastSeenEpoch = std::max(page.lastSeenEpoch, residentEpoch);
    // 新驻留页同样算一次“最近触碰”，避免它带着上一次淘汰前的旧 epoch
    // 一出生就成为 epoch-aware LRU 的最老候选。
    page.lastTouchedEpoch = std::max(page.lastTouchedEpoch, residentEpoch);
    ++page.contentRevision;
    page.priority = priority;
    ++residentCount;
    return true;
}

bool TerrainPageManager::RequestPage(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ,
    uint64_t frame,
    uint32_t priority)
{
    return RequestPageInternal(
        placementId, tileX, tileZ, frame, priority, false, 0);
}

bool TerrainPageManager::RequestPageAtEpoch(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ,
    uint64_t frame,
    uint32_t priority,
    uint64_t completedEpoch)
{
    return RequestPageInternal(
        placementId, tileX, tileZ, frame, priority, true, completedEpoch);
}

bool TerrainPageManager::RequestPageInternal(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ,
    uint64_t frame,
    uint32_t priority,
    bool useEpochWindow,
    uint64_t completedEpoch)
{
    const size_t registrationIndex = FindRegistrationIndex(placementId);
    if (registrationIndex == InvalidIndex)
        throw std::runtime_error(
            "Terrain page request for an unregistered placement: " + placementId);
    const Registration& registration = registrations[registrationIndex];
    if (tileX >= registration.tilesPerSide || tileZ >= registration.tilesPerSide)
        throw std::runtime_error(
            "Terrain page tile is outside the placement: assetPath=" +
            registration.terrainAssetPath + " placementId=" + placementId);

    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    PageState* page = FindPageMutable(key);
    if (!page)
        throw std::runtime_error(
            "Terrain page state is missing for placement: " + placementId);

    ++stats.requestCount;
    if (page->resident)
    {
        ++stats.hitCount;
        if (frame > page->lastUsedFrame) page->lastUsedFrame = frame;
        if (priority > page->priority) page->priority = priority;
        return true;
    }

    ++stats.missCount;
    // 预算满时按 LRU/优先级淘汰；streaming 入口必须使用带 epoch 的候选：
    // 本帧采样过的页可能在 GPU 上仍有 in-flight 引用，提前复用物理槽位会
    // 覆盖它们。物理槽位不足由 MakeResident 的 quarantine 检查负责，不能
    // 用“free==0”当作淘汰条件，否则会把刚隔离的槽位越淘汰越满。
    while (residentCount >= residentCapacity)
    {
        const bool evicted = useEpochWindow
            ? EvictLeastRecentlyUsed(completedEpoch)
            : EvictLeastRecentlyUsed();
        if (!evicted)
            return false;
    }
    // 非 streaming 入口（preload/加载期/测试）没有真实 in-flight 帧，
    // 允许立即回收已隔离槽位，保持旧行为；流式入口必须等安全 epoch。
    const uint64_t allocationEpoch =
        useEpochWindow ? completedEpoch : std::numeric_limits<uint64_t>::max();
    return MakeResident(
        registrationIndex, *page, frame, priority, allocationEpoch);
}

bool TerrainPageManager::MarkUsed(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ,
    uint64_t frame)
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    PageState* page = FindPageMutable(key);
    if (!page || !page->resident) return false;
    if (frame > page->lastUsedFrame) page->lastUsedFrame = frame;
    return true;
}

bool TerrainPageManager::SetPagePriority(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ,
    uint32_t priority)
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    PageState* page = FindPageMutable(key);
    if (!page) return false;
    page->priority = priority;
    return true;
}

bool TerrainPageManager::MarkPageUsedAtEpoch(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ,
    uint64_t epoch,
    uint32_t priority)
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    PageState* page = FindPageMutable(key);
    if (!page || !page->resident) return false;
    page->lastSeenEpoch = std::max(page->lastSeenEpoch, epoch);
    // recency 与安全窗分开记录：lastSeenEpoch 决定“能否淘汰”，
    // lastTouchedEpoch 决定“同优先级/同窗口内先淘汰谁”。
    page->lastTouchedEpoch = std::max(page->lastTouchedEpoch, epoch);
    page->priority = std::max(page->priority, priority);
    return true;
}

bool TerrainPageManager::CanEvictPageAt(
    const PageKey& key,
    uint64_t completedEpoch) const
{
    const PageState* page = FindPage(key);
    if (!page || !page->resident) return false;
    if (page->referenceCount > 0 || page->uploadPinCount > 0) return false;
    return page->lastSeenEpoch + EvictionWindowFrames() <= completedEpoch;
}

bool TerrainPageManager::CanEvictAt(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ,
    uint64_t completedEpoch) const
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    return CanEvictPageAt(key, completedEpoch);
}

bool TerrainPageManager::PinPageForUpload(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ)
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    PageState* page = FindPageMutable(key);
    if (!page || !page->resident) return false;
    ++page->uploadPinCount;
    return true;
}

bool TerrainPageManager::UnpinPage(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ)
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    PageState* page = FindPageMutable(key);
    if (!page || page->uploadPinCount == 0) return false;
    --page->uploadPinCount;
    return true;
}

bool TerrainPageManager::IsPagePinned(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ) const
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    const PageState* page = FindPage(key);
    return page != nullptr && page->uploadPinCount > 0;
}

void TerrainPageManager::SetGpuEpochContext(
    uint64_t lastSubmittedEpoch,
    uint64_t lastConsumedFeedbackEpoch,
    uint32_t framesInFlight)
{
    gpuLastSubmittedEpoch = lastSubmittedEpoch;
    gpuLastConsumedFeedbackEpoch = lastConsumedFeedbackEpoch;
    gpuFramesInFlight = framesInFlight;
}

uint64_t TerrainPageManager::EvictionWindowFrames() const
{
    // feedback 回读本身滞后约 2 帧；再加上提交队列里的 in-flight 帧。
    // framesInFlight=0/1 时下限 2 仍生效，避免安全窗退化成 1 帧。
    return std::max<uint64_t>(
        EvictionHysteresisFrames,
        static_cast<uint64_t>(gpuFramesInFlight) + 1u);
}

uint64_t TerrainPageManager::PhysicalSlotReleaseEpoch(
    uint32_t physicalPageX,
    uint32_t physicalPageY) const
{
    if (physicalPageX >= PageTableSize || physicalPageY >= PageTableSize ||
        physicalSlotReleaseEpoch.empty())
    {
        return 0;
    }
    const size_t slot =
        static_cast<size_t>(physicalPageY) * PageTableSize + physicalPageX;
    return physicalSlotQuarantined[slot] != 0
        ? physicalSlotReleaseEpoch[slot]
        : 0;
}

bool TerrainPageManager::IsPhysicalSlotAvailable(
    uint32_t physicalPageX,
    uint32_t physicalPageY,
    uint64_t completedEpoch) const
{
    if (physicalPageX >= PageTableSize || physicalPageY >= PageTableSize ||
        physicalOccupancy.empty())
    {
        return false;
    }
    const size_t slot =
        static_cast<size_t>(physicalPageY) * PageTableSize + physicalPageX;
    if (physicalOccupancy[slot] == 0)
        return true;
    // fallback 槽位永远保持占用且不进入 quarantine，因此不会被分配。
    return physicalSlotQuarantined[slot] != 0 &&
        physicalSlotReleaseEpoch[slot] <= completedEpoch;
}

uint32_t TerrainPageManager::ReclaimExpiredQuarantines(uint64_t completedEpoch)
{
    if (physicalSlotQuarantined.empty()) return 0;
    const size_t fallbackSlot =
        static_cast<size_t>(FallbackPageY) * PageTableSize + FallbackPageX;
    uint32_t reclaimed = 0;
    for (uint32_t slot = 0; slot < PageSlotCount; ++slot)
    {
        if (physicalSlotQuarantined[slot] == 0) continue;
        // fallback 物理槽位是缺页采样的保底内容，永不复用/回收。
        if (static_cast<size_t>(slot) == fallbackSlot) continue;
        if (physicalSlotReleaseEpoch[slot] > completedEpoch) continue;
        physicalSlotQuarantined[slot] = 0;
        physicalSlotReleaseEpoch[slot] = 0;
        physicalOccupancy[slot] = 0;
        --quarantinedPhysicalSlotCount;
        ++freePhysicalSlotCount;
        ++reclaimed;
    }
    return reclaimed;
}

bool TerrainPageManager::AddPageReference(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ)
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    PageState* page = FindPageMutable(key);
    if (!page || !page->resident) return false;
    ++page->referenceCount;
    return true;
}

bool TerrainPageManager::ReleasePageReference(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ)
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    PageState* page = FindPageMutable(key);
    if (!page || page->referenceCount == 0) return false;
    --page->referenceCount;
    return true;
}

bool TerrainPageManager::EvictPageAt(PageState& page)
{
    if (!page.resident) return false;
    if (page.referenceCount > 0) return false;
    if (page.uploadPinCount > 0) return false;

    const uint32_t physicalPageX = page.physicalPageX;
    const uint32_t physicalPageY = page.physicalPageY;
    ZeroPageTableEntry(page.virtualPageX, page.virtualPageY);
    // 只上传页表失效项。atlas 内容不清零也不上传：旧 in-flight 帧可能仍在
    // 采样该物理槽位，清零反而需要额外 barrier；槽位进入 quarantine 后，
    // 新页会用自己的完整内容覆盖它，届时再标记 atlas dirty。
    MarkPageTableDirty(page.virtualPageX, page.virtualPageY);
    // 该槽位尚未发布的 dirty 内容已不可达，撤销它可以避免上传一份马上要被
    // 覆盖的页；revision 前进让任何过期快照无法在 Commit 时误清新的 dirty。
    const size_t physicalSlot =
        static_cast<size_t>(physicalPageY) * PageTableSize + physicalPageX;
    physicalAtlasDirty[physicalSlot] = 0;
    ++physicalAtlasRevision[physicalSlot];
    // 只隔离物理槽位；virtualReserved 保留，避免其他地形注册到同一虚拟坐标。
    QuarantinePhysicalSlot(physicalPageX, physicalPageY, page.lastSeenEpoch);
    page.resident = false;
    page.physicalPageX = 0;
    page.physicalPageY = 0;
    ++page.contentRevision;
    --residentCount;
    ++stats.evictionCount;
    return true;
}

bool TerrainPageManager::EvictPage(
    const std::string& placementId,
    uint32_t tileX,
    uint32_t tileZ)
{
    PageKey key;
    key.placementId = placementId;
    key.tileX = tileX;
    key.tileZ = tileZ;
    PageState* page = FindPageMutable(key);
    if (!page) return false;
    return EvictPageAt(*page);
}

size_t TerrainPageManager::FindEvictionCandidateIndex() const
{
    size_t candidate = InvalidIndex;
    for (size_t index = 0; index < pages.size(); ++index)
    {
        const PageState& page = pages[index];
        if (!page.resident || page.referenceCount > 0 || page.uploadPinCount > 0) continue;
        if (candidate == InvalidIndex)
        {
            candidate = index;
            continue;
        }
        const PageState& current = pages[candidate];
        // 先比优先级（低者先淘汰），再比最近使用帧（旧者先淘汰）；
        // 完全相同时保持注册顺序，保证结果可复现。
        if (page.priority < current.priority)
        {
            candidate = index;
            continue;
        }
        if (page.priority == current.priority && page.lastUsedFrame < current.lastUsedFrame)
            candidate = index;
    }
    return candidate;
}

size_t TerrainPageManager::FindEvictionCandidateIndex(uint64_t completedEpoch) const
{
    size_t candidate = InvalidIndex;
    for (size_t index = 0; index < pages.size(); ++index)
    {
        const PageState& page = pages[index];
        if (!page.resident || page.referenceCount > 0 || page.uploadPinCount > 0) continue;
        // 安全窗之外才允许进入候选。
        if (page.lastSeenEpoch + EvictionWindowFrames() > completedEpoch) continue;
        if (candidate == InvalidIndex)
        {
            candidate = index;
            continue;
        }
        const PageState& current = pages[candidate];
        // epoch-aware LRU：先比优先级，再比最近 feedback 触碰（旧者先淘汰），
        // 再比安全窗起点，最后才回退到旧帧号；全部相同时保持注册顺序。
        if (page.priority < current.priority)
        {
            candidate = index;
            continue;
        }
        if (page.priority != current.priority) continue;
        if (page.lastTouchedEpoch < current.lastTouchedEpoch)
        {
            candidate = index;
            continue;
        }
        if (page.lastTouchedEpoch != current.lastTouchedEpoch) continue;
        if (page.lastSeenEpoch < current.lastSeenEpoch)
        {
            candidate = index;
            continue;
        }
        if (page.lastSeenEpoch != current.lastSeenEpoch) continue;
        if (page.lastUsedFrame < current.lastUsedFrame)
            candidate = index;
    }
    return candidate;
}

bool TerrainPageManager::EvictLeastRecentlyUsed()
{
    const size_t candidate = FindEvictionCandidateIndex();
    if (candidate == InvalidIndex) return false;
    return EvictPageAt(pages[candidate]);
}

bool TerrainPageManager::EvictLeastRecentlyUsed(uint64_t completedEpoch)
{
    const size_t candidate = FindEvictionCandidateIndex(completedEpoch);
    if (candidate == InvalidIndex) return false;
    return EvictPageAt(pages[candidate]);
}

void TerrainPageManager::SetResidentCapacity(uint32_t capacity)
{
    // 0 会让 RequestPage 永远腾不出空间；fallback 物理槽位不可用，因此上限是
    // UsablePhysicalSlotCount。降低预算时立即按 LRU 淘汰，真正限制驻留页数。
    residentCapacity = std::clamp(capacity, 1u, UsablePhysicalSlotCount);
    while (residentCount > residentCapacity)
    {
        if (!EvictLeastRecentlyUsed()) break;
    }
}

void TerrainPageManager::SetResidentCapacityAtEpoch(
    uint32_t capacity,
    uint64_t completedEpoch)
{
    // 与旧入口一样钳制到 [1, UsablePhysicalSlotCount]；区别是降预算只允许
    // 淘汰安全窗外候选。没有安全候选时保持超预算状态并返回，交给后续帧重试，
    // 避免在 feedback 安全窗内把当前可见页清掉。
    residentCapacity = std::clamp(capacity, 1u, UsablePhysicalSlotCount);
    while (residentCount > residentCapacity)
    {
        if (!EvictLeastRecentlyUsed(completedEpoch)) break;
    }
}

bool TerrainPageManager::IsResident(uint32_t virtualPageX, uint32_t virtualPageY) const
{
    if (pageTable.empty() ||
        virtualPageX >= PageTableSize ||
        virtualPageY >= PageTableSize)
    {
        return false;
    }
    // 页表 A 通道是有效位；物理槽位是否被占用与这里的查询无关。
    const size_t entry =
        (static_cast<size_t>(virtualPageY) * PageTableSize + virtualPageX) * 4;
    return pageTable[entry + 3] != 0;
}

bool TerrainPageManager::IsPageResident(const PageKey& key) const
{
    const PageState* page = FindPage(key);
    return page != nullptr && page->resident;
}

void TerrainPageManager::CopyAtlasPage(
    uint32_t physicalPageX,
    uint32_t physicalPageY,
    const std::vector<uint8_t>& atlas,
    std::vector<uint8_t>& destination) const
{
    const size_t rowBytes = static_cast<size_t>(PageResolution) * 4;
    destination.resize(rowBytes * PageResolution);
    for (uint32_t y = 0; y < PageResolution; ++y)
    {
        const size_t sourceOffset =
            ((static_cast<size_t>(physicalPageY) * PageResolution + y) * AtlasResolution +
             static_cast<size_t>(physicalPageX) * PageResolution) * 4;
        std::memcpy(
            destination.data() + static_cast<size_t>(y) * rowBytes,
            atlas.data() + sourceOffset,
            rowBytes);
    }
}

bool TerrainPageManager::CollectDirtyUploads(
    std::vector<PageTableUpload>& pageTableUploads,
    std::vector<AtlasPageUpload>& atlasUploads)
{
    pageTableUploads.clear();
    atlasUploads.clear();
    if (virtualPageTableDirty.empty() || physicalAtlasDirty.empty()) return false;

    bool hasUpdates = false;
    // 按槽位升序遍历，输出顺序稳定，便于调用方合并/去重和测试断言。
    for (uint32_t slot = 0; slot < PageSlotCount; ++slot)
    {
        if (virtualPageTableDirty[slot] != 0)
        {
            PageTableUpload upload;
            upload.virtualPageX = slot % PageTableSize;
            upload.virtualPageY = slot / PageTableSize;
            const size_t entry = static_cast<size_t>(slot) * 4;
            for (size_t channel = 0; channel < 4; ++channel)
                upload.entry[channel] = pageTable[entry + channel];
            upload.revision = virtualPageTableRevision[slot];
            pageTableUploads.push_back(std::move(upload));
            hasUpdates = true;
        }
        if (physicalAtlasDirty[slot] != 0)
        {
            AtlasPageUpload upload;
            upload.physicalPageX = slot % PageTableSize;
            upload.physicalPageY = slot / PageTableSize;
            upload.revision = physicalAtlasRevision[slot];
            CopyAtlasPage(
                upload.physicalPageX,
                upload.physicalPageY,
                idAtlas,
                upload.ids);
            CopyAtlasPage(
                upload.physicalPageX,
                upload.physicalPageY,
                weightAtlas,
                upload.weights);
            atlasUploads.push_back(std::move(upload));
            hasUpdates = true;
        }
    }
    return hasUpdates;
}

void TerrainPageManager::CommitDirtyUploads(
    const std::vector<PageTableUpload>& pageTableUploads,
    const std::vector<AtlasPageUpload>& atlasUploads)
{
    if (virtualPageTableDirty.empty() || physicalAtlasDirty.empty()) return;
    for (const PageTableUpload& upload : pageTableUploads)
    {
        if (upload.virtualPageX < PageTableSize && upload.virtualPageY < PageTableSize)
        {
            const size_t slot =
                static_cast<size_t>(upload.virtualPageY) * PageTableSize +
                upload.virtualPageX;
            // 收集后槽位又被修改时，旧上传不能清掉新的 dirty。
            if (virtualPageTableRevision[slot] != upload.revision) continue;
            virtualPageTableDirty[slot] = 0;
            if (PageState* page =
                    FindPageByVirtualMutable(upload.virtualPageX, upload.virtualPageY))
            {
                page->uploadedRevision = page->contentRevision;
            }
        }
    }
    for (const AtlasPageUpload& upload : atlasUploads)
    {
        if (upload.physicalPageX < PageTableSize && upload.physicalPageY < PageTableSize)
        {
            const size_t slot =
                static_cast<size_t>(upload.physicalPageY) * PageTableSize +
                upload.physicalPageX;
            if (physicalAtlasRevision[slot] != upload.revision) continue;
            physicalAtlasDirty[slot] = 0;
            if (PageState* page =
                    FindPageByPhysicalMutable(upload.physicalPageX, upload.physicalPageY))
            {
                page->uploadedRevision = page->contentRevision;
            }
        }
    }
}

bool TerrainPageManager::ConsumeDirtyUploads(
    std::vector<PageTableUpload>& pageTableUploads,
    std::vector<AtlasPageUpload>& atlasUploads)
{
    if (!CollectDirtyUploads(pageTableUploads, atlasUploads))
        return false;
    CommitDirtyUploads(pageTableUploads, atlasUploads);
    return true;
}

TerrainUploadBatch TerrainPageManager::BuildUploadBatch(uint32_t maxAtlasPages)
{
    TerrainUploadBatch batch;
    batch.batchId = nextUploadBatchId++;
    if (!CollectDirtyUploads(batch.pageTableUploads, batch.atlasUploads))
        return batch;

    if (maxAtlasPages != 0 && batch.atlasUploads.size() > maxAtlasPages)
    {
        // 一致性截断：atlas 上传按物理槽位升序取前 maxAtlasPages 个；凡是页表项
        // （A=255）指向“仍 dirty 但本批没包含”的物理槽位，就把该页表项也留下。
        // 这样 GPU 页表永远不会先指向一个尚未写入/已被清零的 atlas 页。
        // 被截断项保持 dirty，下一批从升序的剩余槽位继续，保证最终前进。
        std::vector<uint8_t> included(static_cast<size_t>(PageSlotCount), 0);
        for (size_t index = 0; index < maxAtlasPages; ++index)
        {
            const AtlasPageUpload& upload = batch.atlasUploads[index];
            included[static_cast<size_t>(upload.physicalPageY) * PageTableSize +
                upload.physicalPageX] = 1;
        }
        batch.atlasUploads.resize(maxAtlasPages);
        batch.pageTableUploads.erase(
            std::remove_if(
                batch.pageTableUploads.begin(),
                batch.pageTableUploads.end(),
                [this, &included](const PageTableUpload& upload)
                {
                    // 无效页表项不采样本批 atlas，可以随批次先发布。
                    if (upload.entry[3] == 0) return false;
                    const size_t slot =
                        static_cast<size_t>(upload.entry[1]) * PageTableSize +
                        upload.entry[0];
                    return physicalAtlasDirty[slot] != 0 && included[slot] == 0;
                }),
            batch.pageTableUploads.end());
    }

    // pin 本批真正涉及到的页：页表项对应的虚拟页和 atlas 页当前所属的常驻页。
    // 页表项与 atlas 上传可能指向同一页，按 PageKey 去重；原本已经 pin 的页
    // 不记入 newlyPinnedPages，避免 Commit/Abort 解开调用方自己的 pin。
    for (const PageTableUpload& upload : batch.pageTableUploads)
    {
        PinUploadBatchPage(
            FindPageByVirtualMutable(upload.virtualPageX, upload.virtualPageY),
            batch);
    }
    for (const AtlasPageUpload& upload : batch.atlasUploads)
    {
        PinUploadBatchPage(
            FindPageByPhysicalMutable(upload.physicalPageX, upload.physicalPageY),
            batch);
    }
    return batch;
}

void TerrainPageManager::CommitUploadBatch(const TerrainUploadBatch& batch)
{
    // 生产路径当前同一时刻只有一个 in-flight 批次。计数 pin 让重叠批次在 CPU
    // 侧安全，但 GPU 提交仍必须按 Build 顺序，避免旧内容晚于新内容落地。
    // 只有 revision 未变的槽位会被 CommitDirtyUploads 清 dirty；
    // 收集后又被改脏的页继续保持 dirty，等下一批携带新内容上传。
    CommitDirtyUploads(batch.pageTableUploads, batch.atlasUploads);
    for (const PageKey& key : batch.newlyPinnedPages)
        UnpinPage(key.placementId, key.tileX, key.tileZ);
}

void TerrainPageManager::AbortUploadBatch(const TerrainUploadBatch& batch)
{
    // 上传失败：不清任何 dirty，只解除本批新加的 pin，便于下一帧整批重试。
    for (const PageKey& key : batch.newlyPinnedPages)
        UnpinPage(key.placementId, key.tileX, key.tileZ);
}

bool TerrainPageManager::HasDirtyUploads() const
{
    if (virtualPageTableDirty.empty() || physicalAtlasDirty.empty()) return false;
    return std::any_of(
               virtualPageTableDirty.begin(), virtualPageTableDirty.end(),
               [](uint8_t dirty) { return dirty != 0; }) ||
        std::any_of(
            physicalAtlasDirty.begin(), physicalAtlasDirty.end(),
            [](uint8_t dirty) { return dirty != 0; });
}

TerrainFeedbackApplyStats TerrainPageManager::ApplyFeedback(
    const TerrainFeedbackFrameSummary& summary,
    uint64_t completedEpoch,
    uint32_t maxRequestsPerFrame,
    std::vector<TerrainPageRequest>& pendingRequests)
{
    TerrainFeedbackApplyStats stats;

    // 1. 先处理本帧反馈：命中页推进 lastSeenEpoch；miss 页按虚拟坐标入队。
    //    summary.pages 已按 priority 降序，但 pending 会跨帧保留，仍需重新排序。
    for (const TerrainFeedbackPageRecord& page : summary.pages)
    {
        const PageState* state =
            FindPageByVirtual(page.virtualPageX, page.virtualPageY);
        if (state == nullptr)
        {
            // 未注册页（例如 M_pbr 地形）只统计，不请求、不抛错。
            ++stats.unregisteredPages;
            continue;
        }
        if (MarkPageUsedAtEpoch(
                state->key.placementId,
                state->key.tileX,
                state->key.tileZ,
                summary.copiedEpoch,
                page.priority))
        {
            ++stats.markedPages;
        }
        if (!page.miss)
            continue;
        ++stats.missPagesSeen;
        if (state->resident)
            continue;

        bool alreadyQueued = false;
        for (TerrainPageRequest& request : pendingRequests)
        {
            if (request.placementId == state->key.placementId &&
                request.tileX == state->key.tileX &&
                request.tileZ == state->key.tileZ)
            {
                request.priority = std::max(
                    request.priority,
                    static_cast<uint32_t>(page.priority));
                alreadyQueued = true;
                break;
            }
        }
        if (alreadyQueued)
            continue;

        TerrainPageRequest request;
        request.placementId = state->key.placementId;
        request.tileX = state->key.tileX;
        request.tileZ = state->key.tileZ;
        request.priority = page.priority;
        pendingRequests.push_back(std::move(request));
        ++stats.queuedRequests;
    }

    // 2. 只保留“本帧仍然可见”的未驻留请求。相机移走后旧 miss 不应继续占用
    //    每帧烘焙预算；反馈页一旦重新可见会再次入队。
    std::unordered_set<uint32_t> visiblePages;
    visiblePages.reserve(summary.pages.size());
    for (const TerrainFeedbackPageRecord& page : summary.pages)
    {
        visiblePages.insert(
            (static_cast<uint32_t>(page.virtualPageY) << 8u) |
            page.virtualPageX);
    }
    // 丢弃已经驻留、注册已消失或本帧不再可见的旧请求。
    pendingRequests.erase(
        std::remove_if(
            pendingRequests.begin(),
            pendingRequests.end(),
            [this, &visiblePages](const TerrainPageRequest& request)
            {
                const PageKey key{
                    request.placementId, request.tileX, request.tileZ};
                const PageState* state = FindPage(key);
                if (state == nullptr || state->resident)
                    return true;
                const uint32_t pageKey =
                    (static_cast<uint32_t>(state->virtualPageY) << 8u) |
                    state->virtualPageX;
                return visiblePages.count(pageKey) == 0;
            }),
        pendingRequests.end());

    // priority 高者先请求；同 priority 时 deferred 次数少者优先，形成轮转。
    std::stable_sort(
        pendingRequests.begin(),
        pendingRequests.end(),
        [](const TerrainPageRequest& left, const TerrainPageRequest& right)
        {
            if (left.priority != right.priority)
                return left.priority > right.priority;
            if (left.deferredRequestCount != right.deferredRequestCount)
                return left.deferredRequestCount < right.deferredRequestCount;
            if (left.placementId != right.placementId)
                return left.placementId < right.placementId;
            if (left.tileZ != right.tileZ)
                return left.tileZ < right.tileZ;
            return left.tileX < right.tileX;
        });

    // 3. 本帧只尝试前 maxRequestsPerFrame 个；失败项排到未尝试项之后，
    //    这样同优先级下下一帧先轮到还没试过的页，而不是反复撞同一页。
    std::vector<TerrainPageRequest> retry;
    std::vector<TerrainPageRequest> untouched;
    retry.reserve(pendingRequests.size());
    untouched.reserve(pendingRequests.size());
    uint32_t attemptsLeft = maxRequestsPerFrame;
    for (TerrainPageRequest& request : pendingRequests)
    {
        if (attemptsLeft == 0)
        {
            untouched.push_back(std::move(request));
            continue;
        }
        --attemptsLeft;
        ++stats.attemptedRequests;
        // frame 用反馈 epoch：即使 summary 来自 2 帧前，也保持单调的 LRU 次序。
        if (RequestPageAtEpoch(
                request.placementId,
                request.tileX,
                request.tileZ,
                summary.copiedEpoch,
                request.priority,
                completedEpoch))
        {
            ++stats.succeededRequests;
            continue;
        }
        ++stats.deferredRequests;
        ++request.deferredRequestCount;
        retry.push_back(std::move(request));
    }
    pendingRequests.clear();
    pendingRequests.reserve(untouched.size() + retry.size());
    for (TerrainPageRequest& request : untouched)
        pendingRequests.push_back(std::move(request));
    for (TerrainPageRequest& request : retry)
        pendingRequests.push_back(std::move(request));
    stats.pendingRequests =
        static_cast<uint32_t>(pendingRequests.size());
    return stats;
}

} // namespace VL
