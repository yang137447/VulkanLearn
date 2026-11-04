#pragma once

#include <algorithm>
#include <nlohmann/json.hpp>

namespace VL
{
// 保留正式 pass 输入/binding 顺序，仅改变离屏尺寸和最终输出；无 UI、网格和自动曝光调度。
inline nlohmann::json BuildEditorThumbnailGraph(nlohmann::json graph)
{
    for (auto& resource : graph.at("resources"))
    {
        if (resource.at("name") == "swapChain")
        {
            resource["name"] = "thumbnailOutput";
            resource["format"] = "R8G8B8A8_SRGB";
            resource["usage"] = {"colorAttachment", "transferSrc"};
        }
        if (resource.contains("widthScale"))
        {
            resource["widthSize"] = std::max(1u, uint32_t(256 * resource.at("widthScale").get<double>()));
            resource["heightSize"] = std::max(1u, uint32_t(256 * resource.at("heightScale").get<double>()));
            resource.erase("widthScale");
            resource.erase("heightScale");
        }
        if (resource.at("name") == "shadowMap")
            resource["widthSize"] = resource["heightSize"] = 256;
    }
    auto passes = nlohmann::json::array();
    for (auto pass : graph.at("passes"))
    {
        // 缩略图只验证单资产形状：去掉编辑器辅助网格、自动曝光、体积雾
        // （雾需要 sceneDepth/局部阴影并且会改变资产原本的亮度）和
        // tile 光照聚类（缩略图灯数极少，shader 的 enabled=0 回退足够）。
        const std::string type = pass.at("type").get<std::string>();
        if (type == "editorGrid" || type == "autoExposure" ||
            type == "volumetricFog" || type == "fogComposite" ||
            type == "lightCulling")
        {
            continue;
        }
        pass["needMsaa"] = false;
        for (auto& output : pass.at("output"))
            if (output.at("resource") == "swapChain") output["resource"] = "thumbnailOutput";
        passes.push_back(std::move(pass));
    }
    graph["passes"] = std::move(passes);
    return graph;
}
}
