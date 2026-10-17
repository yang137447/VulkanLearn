#pragma once

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace VL
{
// 纯值 Action/Axis 映射；SDL 只负责提供物理键集合，UI 屏蔽时仍更新基线以免恢复焦点产生假按下。
class GameplayInputMap
{
public:
    GameplayInputMap() { Configure(nlohmann::json::object()); }
    void Configure(const nlohmann::json& config)
    {
        if (!config.is_object()) throw std::invalid_argument("gameplayInput requires an object");
        auto nextActions = config.value("actions", nlohmann::json{
            {"Jump", {"Space"}}, {"Interact", {"F"}}, {"Fire", {"MouseLeft"}}});
        auto nextAxes = config.value("axes", nlohmann::json{
            {"MoveForward", {{{"key", "W"}, {"scale", 1}}, {{"key", "S"}, {"scale", -1}}}},
            {"MoveRight", {{{"key", "D"}, {"scale", 1}}, {{"key", "A"}, {"scale", -1}}}},
            {"MoveUp", {{{"key", "E"}, {"scale", 1}}, {{"key", "Q"}, {"scale", -1}}}},
            {"LookX", {{{"key", "MouseX"}, {"scale", 1}}}},
            {"LookY", {{{"key", "MouseY"}, {"scale", 1}}}}});
        if (!nextActions.is_object() || !nextAxes.is_object() ||
            nextActions.size() > 64 || nextAxes.size() > 64)
            throw std::invalid_argument("Input supports at most 64 actions and 64 axes");
        std::set<std::string> nextKeys;
        for (const auto& group : {nextActions, nextAxes})
            for (const auto& item : group.items())
                if (item.key().empty() || item.key().size() > 128 || !item.value().is_array() ||
                    item.value().empty() || item.value().size() > 16)
                    throw std::invalid_argument("Input name/binding budget exceeded");
        for (const auto& item : nextActions.items())
            for (const auto& key : item.value())
            {
                if (!key.is_string() || key.get<std::string>().empty() ||
                    key == "MouseX" || key == "MouseY")
                    throw std::invalid_argument("Action requires button names");
                nextKeys.insert(key.get<std::string>());
            }
        for (const auto& item : nextAxes.items())
            for (const auto& binding : item.value())
            {
                if (!binding.is_object() || !binding.contains("key") || !binding.at("key").is_string() ||
                    binding.at("key").get<std::string>().empty() || !binding.contains("scale") ||
                    !binding.at("scale").is_number() || !std::isfinite(binding.at("scale").get<float>()) ||
                    std::abs(binding.at("scale").get<float>()) > 10000)
                    throw std::invalid_argument("Axis requires key and finite scale within +/-10000");
                nextKeys.insert(binding.at("key").get<std::string>());
            }
        actions = std::move(nextActions); axes = std::move(nextAxes); keys = std::move(nextKeys);
        previous.clear(); previousAllowed.clear();
    }
    const std::set<std::string>& Keys() const { return keys; }
    nlohmann::json Sample(const std::set<std::string>& held, bool keyboard, bool pointer,
        float mouseX = 0, float mouseY = 0)
    {
        auto result = nlohmann::json{{"actions", nlohmann::json::object()}, {"axes", nlohmann::json::object()}};
        std::set<std::string> allowed;
        for (const auto& key : keys)
            if (key.rfind("Mouse", 0) == 0 ? pointer : keyboard) allowed.insert(key);
        for (const auto& item : actions.items())
        {
            bool down = false, wasDown = false, pressed = false, released = false;
            for (const auto& keyNode : item.value())
            {
                const auto key = keyNode.get<std::string>();
                if (allowed.count(key) && held.count(key)) down = true;
                if (!allowed.count(key) || !previousAllowed.count(key)) continue;
                if (previous.count(key)) wasDown = true;
                pressed |= held.count(key) && !previous.count(key);
                released |= !held.count(key) && previous.count(key);
            }
            // 边沿只由连续获准的物理源产生；鼠标被 UI 占用不阻断同一 action 的键盘源。
            // 第一次采样只建立基线；失焦/恢复权限本身都不会产生按下或松开。
            result["actions"][item.key()] = {{"held", down},
                {"pressed", down && !wasDown && pressed}, {"released", !down && released}};
        }
        for (const auto& item : axes.items())
        {
            float value = 0;
            for (const auto& binding : item.value())
            {
                const auto key = binding.at("key").get<std::string>();
                if (!allowed.count(key)) continue;
                const float raw = key == "MouseX" ? mouseX : key == "MouseY" ? mouseY :
                    (held.count(key) ? 1.0f : 0.0f);
                value += raw * binding.at("scale").get<float>();
            }
            result["axes"][item.key()] = value;
        }
        previous = held; previousAllowed = std::move(allowed);
        return result;
    }
private:
    nlohmann::json actions, axes;
    std::set<std::string> keys, previous, previousAllowed;
};
} // namespace VL
