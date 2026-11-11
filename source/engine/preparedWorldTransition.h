#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "render/resource/rendererResourceLoadContext.h"
#include "world/worldManager.h"

class MaterialInstance;

namespace VL
{

// A fully prepared but not yet published World candidate. It owns the
// candidate resource package, pass-material bindings, generated files and
// pipeline state until WorldGraphTransactionCoordinator commits or discards
// it. No field is observable by the render thread before the commit.
struct PreparedWorldTransition
{
    std::shared_ptr<World> world;
    PreparedWorldActivation activation;
    std::shared_ptr<RendererResourceCache> resourceCache;
    std::unordered_map<
        std::string,
        std::shared_ptr<MaterialInstance>>
        passMaterialBindings;
    std::vector<PendingGeneratedResourceFile> pendingGeneratedFiles;
    PipelineFactory::GraphicsCandidateState
        graphicsCandidateState;
    std::shared_ptr<OfflineWorldAssets> offlineWorldAssets;
    // 绝对输入路径与捕获摘要；在候选提交前复核，不能在活动 owner 交换后补验。
    std::vector<std::pair<std::string, std::string>> placementSourceInputs;
};

} // namespace VL
