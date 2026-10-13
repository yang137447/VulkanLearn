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
        if (pass.at("type") == "editorGrid" || pass.at("type") == "autoExposure") continue;
        pass["needMsaa"] = false;
        for (auto& output : pass.at("output"))
            if (output.at("resource") == "swapChain") output["resource"] = "thumbnailOutput";
        passes.push_back(std::move(pass));
    }
    graph["passes"] = std::move(passes);
    return graph;
}
}
