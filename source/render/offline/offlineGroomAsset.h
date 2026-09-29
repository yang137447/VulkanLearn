#pragma once

#include "render/offline/offlineWorldAssets.h"

namespace VL
{
// 在候选阶段冻结描述/原始字节；实际 PFM 解码只发生在 CPU prepare worker。
void FreezeOfflineMaterialTextures(OfflineWorldAssets& assets,
    const nlohmann::json& effectiveMaterial);
void ValidateOfflineTexturesStillCurrent(const OfflineWorldAssets& assets);
void PruneUnusedOfflineMaterialAssets(OfflineWorldAssets& assets);
}
