#pragma once

#include "render/offline/offlineWorldAssets.h"

namespace VL
{
std::shared_ptr<const Offline::ColorMap> DecodeOfflineColorImage(
    const OfflineWorldAssets::ColorAsset& asset);
}
