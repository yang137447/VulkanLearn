#include "terrain/terrainLayerPageManager.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace VL
{
TerrainLayerPageManager::TerrainLayerPageManager(uint32_t physicalPageCount)
    : physicalPageCount(physicalPageCount)
{
    // 至少要有一个普通槽位 + 一个 fallback 槽位。
    if (physicalPageCount < 2)
        throw std::invalid_argument(
            "TerrainLayerPageManager requires at least 2 physical pages");
    if (physicalPageCount > 4096)
        throw std::invalid_argument(
            "TerrainLayerPageManager physical page count exceeds the CPU budget");

    layerSourceResolution.assign(MaxLayers, 1024u);
    RebuildLayerLayout();

    physicalOccupancy.assign(physicalPageCount, 0);
    physicalQuarantined.assign(physicalPageCount, 0);
    physicalReleaseEpoch.assign(physicalPageCount, 0);
    freePhysicalPageCount = UsablePhysicalPageCount();
    // 小物理池（测试/低预算档）必须让预算跟着池大小收缩，否则只会分配失败。
    residentCapacity = std::min(DefaultResidentCapacity, UsablePhysicalPageCount());
    // fallback 物理页永远占用：缺页时 GPU 采样的保底内容，绝不参与分配。
    physicalOccupancy[FallbackPhysicalPage()] = 1;
}

void TerrainLayerPageManager::ConfigureLayers(
    const std::vector<TerrainLayerDesc>& layers)
{
    if (layers.size() > MaxLayers)
        throw std::runtime_error(
            "TerrainLayerPageManager supports at most 256 layers; got " +
            std::to_string(layers.size()));
    // 虚拟页号依赖 layer 分辨率；已有页状态时重配置会让旧 key 失效。
    if (!pages.empty())
        throw std::runtime_error(
            "TerrainLayerPageManager::ConfigureLayers must run before any page request");

    layerSourceResolution.assign(MaxLayers, 1024u);
    layerSourceSet.assign(MaxLayers, 0u);
    for (size_t index = 0; index < layers.size(); ++index)
    {
        const uint32_t resolution = layers[index].sourceResolution;
        if (!IsSupportedTerrainLayerSourceResolution(resolution))
            throw std::runtime_error(
                "Terrain layer sourceResolution must be 512, 1024 or 2048; got " +
                std::to_string(resolution));
        layerSourceResolution[index] = resolution;
        layerSourceSet[index] = layers[index].sourceSet;
    }
    RebuildLayerLayout();
}

void TerrainLayerPageManager::RebuildLayerLayout()
{
    layerPageTableBase.assign(MaxLayers, 0u);
    // 每个 mip 一个统一 stride：混合分辨率时用该 mip 的最大页数，保证
    // “层号 * stride + 层内页号”两两不相交；全同分辨率时与设计公式一致。
    layerMipStride.assign(MaxMipLevel + 1, 0u);
    uint32_t base = 0;
    for (uint32_t layer = 0; layer < MaxLayers; ++layer)
    {
        layerPageTableBase[layer] = base;
        base += TerrainLayerTotalPages(layerSourceResolution[layer]);
        for (uint32_t mip = 0; mip <= MaxMipLevel; ++mip)
        {
            layerMipStride[mip] = std::max(
                layerMipStride[mip],
                TerrainLayerPagesPerMip(layerSourceResolution[layer], mip));
        }
    }
}

uint32_t TerrainLayerPageManager::PagesPerSideForSource(
    uint32_t sourceResolution,
    uint32_t mip)
{
    return TerrainLayerPagesPerSide(sourceResolution, mip);
}

uint32_t TerrainLayerPageManager::PagesPerMipForSource(
    uint32_t sourceResolution,
    uint32_t mip)
{
    return TerrainLayerPagesPerMip(sourceResolution, mip);
}

uint32_t TerrainLayerPageManager::TotalPagesForSource(uint32_t sourceResolution)
{
    return TerrainLayerTotalPages(sourceResolution);
}

TerrainLayerPageManager::PhysicalPageAddress
TerrainLayerPageManager::DecodePhysicalPage(uint32_t physicalPage)
{
    PhysicalPageAddress address;
    address.slice = physicalPage / PagesPerAtlasSlice;
    const uint32_t tile = physicalPage % PagesPerAtlasSlice;
    address.tileY = tile / TilesPerAtlasSide;
    address.tileX = tile % TilesPerAtlasSide;
    return address;
}

TerrainLayerPageManager::PageTableLayout
TerrainLayerPageManager::PageTableLayoutForEntries(uint32_t entryCount)
{
    PageTableLayout layout;
    if (entryCount == 0)
        return layout;
    // 4096 是 Vulkan 保证的最大 2D 尺寸；1K 层的 mip0 是 16384 项 -> 4096×4。
    constexpr uint32_t MaxWidth = 4096;
    layout.width = std::min(entryCount, MaxWidth);
    layout.height = (entryCount + layout.width - 1) / layout.width;
    return layout;
}

TerrainLayerPageManager::PageTableLayout
TerrainLayerPageManager::PageTableLayoutForMip(uint32_t mip) const
{
    if (mip > MaxMipLevel)
        throw std::out_of_range(
            "Terrain layer page table mip must be <= " +
            std::to_string(MaxMipLevel) + "; got " +
            std::to_string(mip));
    return PageTableLayoutForEntries(VirtualPageTableSize(mip));
}

std::array<float, 4> TerrainLayerPageManager::EncodePageTableEntry(
    uint32_t physicalPage,
    uint32_t mip,
    bool valid,
    uint32_t priority)
{
    // 无效项必须四通道全零：B 通道是 shader 判断 fallback 的唯一依据，
    // 只清 R 会让旧物理页仍看起来有效。
    if (!valid)
        return {0.0f, 0.0f, 0.0f, 0.0f};
    return {
        static_cast<float>(physicalPage),
        static_cast<float>(mip),
        1.0f,
        static_cast<float>(priority)};
}

uint32_t TerrainLayerPageManager::LayerSourceResolution(uint32_t layerId) const
{
    if (layerId >= MaxLayers)
        throw std::out_of_range("Terrain layer id must be < 256");
    return layerSourceResolution[layerId];
}

uint32_t TerrainLayerPageManager::LayerSourceSet(uint32_t layerId) const
{
    return layerId < layerSourceSet.size() ? layerSourceSet[layerId] : 0u;
}

uint32_t TerrainLayerPageManager::PagesPerSide(
    uint32_t layerId,
    uint32_t mip) const
{
    return PagesPerSideForSource(LayerSourceResolution(layerId), mip);
}

uint32_t TerrainLayerPageManager::PagesPerMip(
    uint32_t layerId,
    uint32_t mip) const
{
    return PagesPerMipForSource(LayerSourceResolution(layerId), mip);
}

uint32_t TerrainLayerPageManager::TotalPagesForLayer(uint32_t layerId) const
{
    return TotalPagesForSource(LayerSourceResolution(layerId));
}

uint32_t TerrainLayerPageManager::PageTableBase(uint32_t layerId) const
{
    if (layerId >= MaxLayers)
        throw std::out_of_range("Terrain layer id must be < 256");
    return layerPageTableBase[layerId];
}

uint64_t TerrainLayerPageManager::VirtualPageIndex(
    const TerrainLayerPageKey& key) const
{
    ValidateKey(key);
    // 设计 §4.4：每个 mip 独立编号，pageTableBase 不参与；stride 取该 mip
    // 所有层的最大页数，混合 512/1K/2K 时也不会别名。
    return static_cast<uint64_t>(key.layerId) * layerMipStride[key.mip] +
        static_cast<uint64_t>(key.pageY) * PagesPerSide(key.layerId, key.mip) +
        key.pageX;
}

uint32_t TerrainLayerPageManager::VirtualPagesPerLayer(uint32_t mip) const
{
    return mip <= MaxMipLevel ? layerMipStride[mip] : 0u;
}

uint32_t TerrainLayerPageManager::VirtualPageTableSize(uint32_t mip) const
{
    return VirtualPagesPerLayer(mip) * MaxLayers;
}

uint64_t TerrainLayerPageManager::FlatPageIndex(
    const TerrainLayerPageKey& key) const
{
    ValidateKey(key);
    const uint32_t resolution = LayerSourceResolution(key.layerId);
    return static_cast<uint64_t>(PageTableBase(key.layerId)) +
        TerrainLayerMipPageBase(resolution, key.mip) +
        static_cast<uint64_t>(key.pageY) * PagesPerSide(key.layerId, key.mip) +
        key.pageX;
}

void TerrainLayerPageManager::ValidateKey(const TerrainLayerPageKey& key) const
{
    if (key.layerId >= MaxLayers)
        throw std::out_of_range(
            "Terrain layer page layerId must be < 256; got " +
            std::to_string(key.layerId));
    const uint32_t mipCount = TerrainLayerMipCount(LayerSourceResolution(key.layerId));
    if (key.mip >= mipCount)
        throw std::out_of_range(
            "Terrain layer page mip exceeds the layer mip chain; layer=" +
            std::to_string(key.layerId) + " mip=" + std::to_string(key.mip) +
            " mipCount=" + std::to_string(mipCount));
    const uint32_t side = PagesPerSide(key.layerId, key.mip);
    if (key.pageX >= side || key.pageY >= side)
        throw std::out_of_range(
            "Terrain layer page coordinate is outside the mip; layer=" +
            std::to_string(key.layerId) + " mip=" + std::to_string(key.mip) +
            " page=" + std::to_string(key.pageX) + "," +
            std::to_string(key.pageY) + " side=" + std::to_string(side));
}

TerrainLayerPageManager::PageState& TerrainLayerPageManager::AcquirePageState(
    const TerrainLayerPageKey& key)
{
    auto found = pages.find(key);
    if (found != pages.end())
        return found->second;
    PageState state;
    state.key = key;
    auto inserted = pages.emplace(key, std::move(state));
    return inserted.first->second;
}

bool TerrainLayerPageManager::RequestPage(
    const TerrainLayerPageKey& key,
    uint64_t frame,
    uint32_t priority)
{
    return RequestPageInternal(
        key, frame, priority, false, std::numeric_limits<uint64_t>::max());
}

bool TerrainLayerPageManager::RequestPageAtEpoch(
    const TerrainLayerPageKey& key,
    uint64_t frame,
    uint32_t priority,
    uint64_t completedEpoch)
{
    return RequestPageInternal(key, frame, priority, true, completedEpoch);
}

bool TerrainLayerPageManager::RequestPageInternal(
    const TerrainLayerPageKey& key,
    uint64_t frame,
    uint32_t priority,
    bool useEpochWindow,
    uint64_t completedEpoch)
{
    ValidateKey(key);
    PageState& page = AcquirePageState(key);
    ++stats.requestCount;
    if (page.resident)
    {
        ++stats.hitCount;
        page.lastUsedFrame = std::max(page.lastUsedFrame, frame);
        // priority 只升不降：feedback/预取可以累计重要性，避免抖动。
        page.priority = std::max(page.priority, priority);
        return true;
    }

    ++stats.missCount;
    // 容量满时先按 epoch-aware LRU 淘汰；没有安全候选就推迟，不能强行
    // 覆盖仍可能被 in-flight 帧采样的物理页。
    while (residentPageCount >= residentCapacity)
    {
        const uint64_t evictionEpoch =
            useEpochWindow ? completedEpoch : std::numeric_limits<uint64_t>::max();
        if (!EvictLeastRecentlyUsed(evictionEpoch))
        {
            ++stats.deferredCount;
            ++page.deferredRequestCount;
            return false;
        }
    }
    return MakeResident(page, frame, priority, completedEpoch);
}

bool TerrainLayerPageManager::MakeResident(
    PageState& page,
    uint64_t frame,
    uint32_t priority,
    uint64_t completedEpoch)
{
    uint32_t physicalPage = InvalidPhysicalPage;
    if (!AllocatePhysicalPage(physicalPage, completedEpoch))
    {
        ++stats.deferredCount;
        ++page.deferredRequestCount;
        return false;
    }

    page.resident = true;
    page.physicalPage = physicalPage;
    page.lastUsedFrame = std::max(page.lastUsedFrame, frame);
    page.priority = std::max(page.priority, priority);
    // 新页以本帧已提交 epoch 起算：内容要到该帧提交后才可能被 GPU 看到，
    // 用旧 epoch 会让它一出生就过了安全窗。
    // 还没有提交过（加载期）时退回已消费 feedback epoch，与 P3 语义一致。
    const uint64_t residentEpoch = gpuLastSubmittedEpoch != 0
        ? gpuLastSubmittedEpoch
        : gpuLastConsumedFeedbackEpoch;
    page.lastSeenEpoch = std::max(page.lastSeenEpoch, residentEpoch);
    page.lastTouchedEpoch = std::max(page.lastTouchedEpoch, residentEpoch);
    ++page.contentRevision;
    // 驻留映射变化：即使像素内容已在旧物理页上传过，页表也必须指向新槽位。
    ++page.pageTableRevision;
    page.deferredRequestCount = 0;
    ++residentPageCount;
    return true;
}

bool TerrainLayerPageManager::AllocatePhysicalPage(
    uint32_t& physicalPage,
    uint64_t completedEpoch)
{
    // 过期 quarantine 与普通空槽位等价；先回收保证 free 计数一致。
    ReclaimExpiredQuarantines(completedEpoch);
    for (uint32_t slot = 0; slot < UsablePhysicalPageCount(); ++slot)
    {
        if (physicalOccupancy[slot] != 0)
            continue;
        // 从低槽位分配，保证测试和画面结果可复现。
        physicalOccupancy[slot] = 1;
        --freePhysicalPageCount;
        physicalPage = slot;
        return true;
    }
    return false;
}

void TerrainLayerPageManager::QuarantinePhysicalPage(
    uint32_t physicalPage,
    uint64_t lastSeenEpoch)
{
    if (physicalPage >= physicalPageCount) return;
    if (physicalPage == FallbackPhysicalPage()) return;
    if (physicalQuarantined[physicalPage] != 0) return;
    // 覆盖“本帧及之前所有已提交帧”，再留 framesInFlight + 1 个 epoch 余量。
    const uint64_t lastGpuEpoch = std::max(lastSeenEpoch, gpuLastSubmittedEpoch);
    physicalQuarantined[physicalPage] = 1;
    physicalReleaseEpoch[physicalPage] =
        lastGpuEpoch + static_cast<uint64_t>(gpuFramesInFlight) + 1u;
    ++quarantinedPhysicalPageCount;
}

void TerrainLayerPageManager::ReleasePhysicalPage(PageState& page)
{
    if (page.physicalPage >= physicalPageCount) return;
    // 只隔离不立即清零：旧 in-flight 帧仍可能采样旧内容；新页会整体覆盖。
    QuarantinePhysicalPage(page.physicalPage, page.lastSeenEpoch);
    page.physicalPage = InvalidPhysicalPage;
    page.resident = false;
    // 内容失效：再驻留时 revision 继续前进，旧上传快照无法误标已同步。
    ++page.contentRevision;
    // 淘汰必须产生一条页表失效项，否则 GPU 仍会按旧映射采样隔离槽位。
    ++page.pageTableRevision;
}

bool TerrainLayerPageManager::MarkPageUsedAtEpoch(
    const TerrainLayerPageKey& key,
    uint64_t epoch,
    uint32_t priority)
{
    auto found = pages.find(key);
    if (found == pages.end() || !found->second.resident)
        return false;
    PageState& page = found->second;
    page.lastSeenEpoch = std::max(page.lastSeenEpoch, epoch);
    page.lastTouchedEpoch = std::max(page.lastTouchedEpoch, epoch);
    page.priority = std::max(page.priority, priority);
    return true;
}

bool TerrainLayerPageManager::CanEvictPageAt(
    const TerrainLayerPageKey& key,
    uint64_t completedEpoch) const
{
    if (IsPageProtected(key)) return false;
    const PageState* page = FindPage(key);
    if (page == nullptr || !page->resident) return false;
    if (page->referenceCount > 0 || page->uploadPinCount > 0) return false;
    return page->lastSeenEpoch + EvictionWindowFrames() <= completedEpoch;
}

bool TerrainLayerPageManager::EvictPageAt(PageState& page)
{
    if (!page.resident) return false;
    if (IsPageProtected(page.key)) return false;
    // 被引用或正被上传批次 pin 住的页不能淘汰。
    if (page.referenceCount > 0 || page.uploadPinCount > 0) return false;
    ReleasePhysicalPage(page);
    --residentPageCount;
    ++stats.evictionCount;
    return true;
}

bool TerrainLayerPageManager::EvictPage(const TerrainLayerPageKey& key)
{
    auto found = pages.find(key);
    if (found == pages.end()) return false;
    return EvictPageAt(found->second);
}

TerrainLayerPageManager::PageState* TerrainLayerPageManager::FindEvictionCandidate(
    uint64_t completedEpoch)
{
    const uint64_t window = EvictionWindowFrames();
    auto candidate = pages.end();
    for (auto iterator = pages.begin(); iterator != pages.end(); ++iterator)
    {
        const PageState& page = iterator->second;
        if (!page.resident) continue;
        if (IsPageProtected(iterator->first)) continue;
        if (page.referenceCount > 0 || page.uploadPinCount > 0) continue;
        // 安全窗之外才允许进入候选；window 用加法比较，避免下溢。
        if (page.lastSeenEpoch + window > completedEpoch) continue;
        if (candidate == pages.end())
        {
            candidate = iterator;
            continue;
        }
        const PageState& current = candidate->second;
        // 先比优先级（低者先淘汰），再比最近触碰，再比安全窗起点，
        // 最后回退到旧帧号；全相同时 std::map 的 key 顺序保证可复现。
        if (page.priority < current.priority)
        {
            candidate = iterator;
            continue;
        }
        if (page.priority != current.priority) continue;
        if (page.lastTouchedEpoch < current.lastTouchedEpoch)
        {
            candidate = iterator;
            continue;
        }
        if (page.lastTouchedEpoch != current.lastTouchedEpoch) continue;
        if (page.lastSeenEpoch < current.lastSeenEpoch)
        {
            candidate = iterator;
            continue;
        }
        if (page.lastSeenEpoch != current.lastSeenEpoch) continue;
        if (page.lastUsedFrame < current.lastUsedFrame)
            candidate = iterator;
    }
    return candidate == pages.end() ? nullptr : &candidate->second;
}

bool TerrainLayerPageManager::EvictLeastRecentlyUsed(uint64_t completedEpoch)
{
    PageState* candidate = FindEvictionCandidate(completedEpoch);
    if (candidate == nullptr) return false;
    return EvictPageAt(*candidate);
}

bool TerrainLayerPageManager::PinPageForUpload(const TerrainLayerPageKey& key)
{
    auto found = pages.find(key);
    if (found == pages.end() || !found->second.resident)
        return false;
    ++found->second.uploadPinCount;
    return true;
}

bool TerrainLayerPageManager::UnpinPage(const TerrainLayerPageKey& key)
{
    auto found = pages.find(key);
    if (found == pages.end() || found->second.uploadPinCount == 0)
        return false;
    --found->second.uploadPinCount;
    return true;
}

bool TerrainLayerPageManager::IsPagePinned(const TerrainLayerPageKey& key) const
{
    const PageState* page = FindPage(key);
    return page != nullptr && page->uploadPinCount > 0;
}

bool TerrainLayerPageManager::AddPageReference(const TerrainLayerPageKey& key)
{
    auto found = pages.find(key);
    if (found == pages.end() || !found->second.resident)
        return false;
    ++found->second.referenceCount;
    return true;
}

bool TerrainLayerPageManager::ReleasePageReference(const TerrainLayerPageKey& key)
{
    auto found = pages.find(key);
    if (found == pages.end() || found->second.referenceCount == 0)
        return false;
    --found->second.referenceCount;
    return true;
}

void TerrainLayerPageManager::SetGpuEpochContext(
    uint64_t lastSubmittedEpoch,
    uint32_t framesInFlight)
{
    SetGpuEpochContext(lastSubmittedEpoch, 0, framesInFlight);
}

void TerrainLayerPageManager::SetGpuEpochContext(
    uint64_t lastSubmittedEpoch,
    uint64_t lastConsumedFeedbackEpoch,
    uint32_t framesInFlight)
{
    gpuLastSubmittedEpoch = lastSubmittedEpoch;
    gpuLastConsumedFeedbackEpoch = lastConsumedFeedbackEpoch;
    gpuFramesInFlight = framesInFlight;
}

void TerrainLayerPageManager::SetProtectedPages(
    const std::map<TerrainLayerPageKey, uint32_t>& protectedPageSet)
{
    protectedPages.clear();
    for (const auto& entry : protectedPageSet)
        protectedPages.insert(entry.first);
}

bool TerrainLayerPageManager::IsPageProtected(
    const TerrainLayerPageKey& key) const
{
    return protectedPages.find(key) != protectedPages.end();
}

uint64_t TerrainLayerPageManager::EvictionWindowFrames() const
{
    return std::max<uint64_t>(
        EvictionHysteresisFrames,
        static_cast<uint64_t>(gpuFramesInFlight) + 1u);
}

void TerrainLayerPageManager::SetResidentCapacity(uint32_t capacity)
{
    // 0 会让请求永远腾不出空间；上限是可用物理页数。
    residentCapacity = std::clamp(capacity, 1u, UsablePhysicalPageCount());
    while (residentPageCount > residentCapacity)
    {
        if (!EvictLeastRecentlyUsed(std::numeric_limits<uint64_t>::max()))
            break;
    }
}

void TerrainLayerPageManager::SetResidentCapacityAtEpoch(
    uint32_t capacity,
    uint64_t completedEpoch)
{
    // 降预算只淘汰安全窗外的页；没有安全候选时允许暂时超预算。
    residentCapacity = std::clamp(capacity, 1u, UsablePhysicalPageCount());
    while (residentPageCount > residentCapacity)
    {
        if (!EvictLeastRecentlyUsed(completedEpoch))
            break;
    }
}

uint64_t TerrainLayerPageManager::PhysicalPageReleaseEpoch(
    uint32_t physicalPage) const
{
    if (physicalPage >= physicalPageCount) return 0;
    return physicalQuarantined[physicalPage] != 0
        ? physicalReleaseEpoch[physicalPage]
        : 0;
}

bool TerrainLayerPageManager::IsPhysicalPageAvailable(
    uint32_t physicalPage,
    uint64_t completedEpoch) const
{
    if (physicalPage >= physicalPageCount) return false;
    if (physicalPage == FallbackPhysicalPage()) return false;
    if (physicalOccupancy[physicalPage] == 0) return true;
    return physicalQuarantined[physicalPage] != 0 &&
        physicalReleaseEpoch[physicalPage] <= completedEpoch;
}

uint32_t TerrainLayerPageManager::ReclaimExpiredQuarantines(
    uint64_t completedEpoch)
{
    uint32_t reclaimed = 0;
    const uint32_t fallback = FallbackPhysicalPage();
    for (uint32_t slot = 0; slot < physicalPageCount; ++slot)
    {
        if (slot == fallback) continue;
        if (physicalQuarantined[slot] == 0) continue;
        if (physicalReleaseEpoch[slot] > completedEpoch) continue;
        physicalQuarantined[slot] = 0;
        physicalReleaseEpoch[slot] = 0;
        physicalOccupancy[slot] = 0;
        --quarantinedPhysicalPageCount;
        ++freePhysicalPageCount;
        ++reclaimed;
    }
    return reclaimed;
}

TerrainLayerUploadBatch TerrainLayerPageManager::BuildUploadBatch(
    uint32_t maxPages)
{
    TerrainLayerUploadBatch batch;
    batch.batchId = nextUploadBatchId++;
    for (const auto& entry : pages)
    {
        const PageState& page = entry.second;
        // 只上传常驻页；淘汰后 contentRevision 会前进但内容已不可达。
        if (!page.resident) continue;
        if (page.contentRevision <= page.uploadedRevision) continue;
        PageUpload upload;
        upload.key = page.key;
        upload.virtualPageIndex = VirtualPageIndex(page.key);
        upload.physicalPage = page.physicalPage;
        upload.revision = page.contentRevision;
        batch.uploads.push_back(upload);
    }
    // 按物理页号升序，保证截断与测试结果稳定；剩余页下一批继续。
    std::sort(
        batch.uploads.begin(),
        batch.uploads.end(),
        [](const PageUpload& lhs, const PageUpload& rhs)
        {
            if (lhs.physicalPage != rhs.physicalPage)
                return lhs.physicalPage < rhs.physicalPage;
            return lhs.key < rhs.key;
        });
    if (maxPages != 0 && batch.uploads.size() > maxPages)
        batch.uploads.resize(maxPages);

    // 被 maxPages 截断的页不能先写页表：GPU 会指向尚未上传/仍为旧内容的
    // 物理页。这里把它们排除在“本批已上传集合”之外，页表项留到下一批。
    std::vector<TerrainLayerPageKey> uploadedKeys;
    uploadedKeys.reserve(batch.uploads.size());
    for (const PageUpload& upload : batch.uploads)
        uploadedKeys.push_back(upload.key);

    for (const auto& entry : pages)
    {
        const PageState& page = entry.second;
        if (page.pageTableRevision <= page.pageTableUploadedRevision)
            continue;

        PageTableUpdate update;
        update.key = page.key;
        update.virtualPageIndex = VirtualPageIndex(page.key);
        update.revision = page.pageTableRevision;
        if (page.resident)
        {
            // 内容仍脏但本批被截断 -> 页表也必须一起延后。
            const bool contentDirty =
                page.contentRevision > page.uploadedRevision;
            bool included = !contentDirty;
            if (contentDirty)
            {
                for (const TerrainLayerPageKey& key : uploadedKeys)
                {
                    if (key == page.key)
                    {
                        included = true;
                        break;
                    }
                }
            }
            if (!included)
                continue;
            update.valid = true;
            update.physicalPage = page.physicalPage;
            update.entry = EncodePageTableEntry(
                page.physicalPage,
                page.key.mip,
                true,
                page.priority);
        }
        else
        {
            // 淘汰失效项与 atlas 上传无关，可以独立写入，不受截断影响。
            update.valid = false;
            update.entry = EncodePageTableEntry(0, 0, false, 0);
        }
        batch.pageTableUpdates.push_back(update);
    }

    // 每页只在本批 pin 一次；即使页已被其他批次 pin 住，本批也要独立持有一份，
    // 否则先结束的批次会把仍在使用的页解 pin。
    std::vector<TerrainLayerPageKey> pagesToPin = uploadedKeys;
    for (const PageTableUpdate& update : batch.pageTableUpdates)
    {
        if (!update.valid)
            continue;
        pagesToPin.push_back(update.key);
    }
    for (const TerrainLayerPageKey& key : pagesToPin)
    {
        auto found = pages.find(key);
        if (found == pages.end() || !found->second.resident) continue;
        bool alreadyPinned = false;
        for (const TerrainLayerPageKey& pinned : batch.newlyPinnedPages)
        {
            if (pinned == key)
            {
                alreadyPinned = true;
                break;
            }
        }
        if (alreadyPinned) continue;
        ++found->second.uploadPinCount;
        batch.newlyPinnedPages.push_back(key);
    }
    return batch;
}

void TerrainLayerPageManager::CommitUploadBatch(
    const TerrainLayerUploadBatch& batch)
{
    for (const PageUpload& upload : batch.uploads)
    {
        auto found = pages.find(upload.key);
        if (found == pages.end()) continue;
        PageState& page = found->second;
        // 收集后页被淘汰或重新驻留到别的物理页、或内容又变脏时，
        // 这条旧上传不能把新内容标记为已同步。
        if (!page.resident) continue;
        if (page.physicalPage != upload.physicalPage) continue;
        if (page.contentRevision != upload.revision) continue;
        page.uploadedRevision = upload.revision;
        ++stats.uploadedPageCount;
    }
    for (const PageTableUpdate& update : batch.pageTableUpdates)
    {
        auto found = pages.find(update.key);
        if (found == pages.end()) continue;
        PageState& page = found->second;
        if (page.pageTableRevision != update.revision) continue;
        if (update.valid)
        {
            // 有效项只能确认仍指向同一物理页的映射；被淘汰/重驻留的旧项作废。
            if (!page.resident || page.physicalPage != update.physicalPage)
                continue;
        }
        else
        {
            // 失效项只在页仍未驻留时成立；同帧重新驻留会推进 revision。
            if (page.resident) continue;
        }
        page.pageTableUploadedRevision = update.revision;
        ++stats.uploadedPageTableEntryCount;
    }
    for (const TerrainLayerPageKey& key : batch.newlyPinnedPages)
        UnpinPage(key);
}

void TerrainLayerPageManager::AbortUploadBatch(
    const TerrainLayerUploadBatch& batch)
{
    // 上传失败：不清任何 revision，只解除本批新加的 pin，便于下一帧重试。
    for (const TerrainLayerPageKey& key : batch.newlyPinnedPages)
        UnpinPage(key);
}

uint32_t TerrainLayerPageManager::DirtyPageCount() const
{
    uint32_t count = 0;
    for (const auto& entry : pages)
    {
        const PageState& page = entry.second;
        if (page.resident && page.contentRevision > page.uploadedRevision)
            ++count;
    }
    return count;
}

uint32_t TerrainLayerPageManager::DirtyPageTableEntryCount() const
{
    uint32_t count = 0;
    for (const auto& entry : pages)
    {
        const PageState& page = entry.second;
        if (page.pageTableRevision > page.pageTableUploadedRevision)
            ++count;
    }
    return count;
}

bool TerrainLayerPageManager::IsResident(const TerrainLayerPageKey& key) const
{
    const PageState* page = FindPage(key);
    return page != nullptr && page->resident;
}

const TerrainLayerPageManager::PageState* TerrainLayerPageManager::FindPage(
    const TerrainLayerPageKey& key) const
{
    auto found = pages.find(key);
    return found != pages.end() ? &found->second : nullptr;
}

TerrainLayerPageManager::Stats TerrainLayerPageManager::SnapshotStats() const
{
    Stats snapshot = stats;
    snapshot.residentPageCount = residentPageCount;
    snapshot.residentCapacity = residentCapacity;
    snapshot.freePhysicalPageCount = freePhysicalPageCount;
    snapshot.quarantinedPhysicalPageCount = quarantinedPhysicalPageCount;
    return snapshot;
}

} // namespace VL
