#pragma once

#include <set>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace VL::Editor
{
// 外部协议与资产文件共用解析预算；重复键不能靠解析器的最后写入规则偷偷覆盖意图。
struct StrictJsonGuard
{
    std::vector<std::set<std::string>> objectKeys;
    bool operator()(int depth, nlohmann::json::parse_event_t event, nlohmann::json& value)
    {
        using Event = nlohmann::json::parse_event_t;
        if ((event == Event::object_start || event == Event::array_start) && depth >= 64)
            throw std::invalid_argument("JSON exceeds depth 64");
        if (event == Event::object_start) objectKeys.emplace_back();
        if (event == Event::object_end) objectKeys.pop_back();
        if (event == Event::key && !objectKeys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("Duplicate JSON key: " + value.get<std::string>());
        return true;
    }
};

inline nlohmann::json ParseStrictJson(std::string_view text, size_t maxBytes)
{
    if (text.size() > maxBytes) throw std::invalid_argument("JSON input exceeds size limit");
    return nlohmann::json::parse(text.begin(), text.end(), StrictJsonGuard{});
}
} // namespace VL::Editor
