#pragma once

#include "terrain/terrainAssetTypes.h"
#include <vector>

namespace VL
{
class TerrainPageManager;

class TerrainCompositedPageSourceBatch
{
public:
    TerrainCompositedPageSourceBatch(TerrainPageManager& manager,
        const std::vector<TerrainBuildPlan>& plans);
    bool CanCommitNoThrow() const noexcept;
    void CommitNoThrow() noexcept;
    uint64_t MetadataBytes() const noexcept;

private:
    struct Source
    {
        size_t index;
        std::shared_ptr<const HostImage> expectedHeight;
        std::shared_ptr<const TerrainLayerPaintData> expectedPaint;
        std::shared_ptr<const HostImage> height;
        std::shared_ptr<const TerrainLayerPaintData> paint;
    };
    struct Page
    {
        size_t index;
        std::vector<uint8_t> layers;
    };
    TerrainPageManager& manager;
    std::vector<Source> sources;
    std::vector<Page> pages;
    size_t expectedPlans = 0;
    size_t expectedPages = 0;
    bool committed = false;
};
}
