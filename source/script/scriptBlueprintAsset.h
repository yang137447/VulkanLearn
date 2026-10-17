#pragma once

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include "script/scriptScene.h"
#include "shader/build/contentHash.h"

namespace VL
{
inline std::string ScriptContentDomain(const std::filesystem::path& path)
{
    if (path.empty()) return {};
    auto domain = path.begin()->string();
    for (char& character : domain)
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return domain;
}
// GT 加载入口解析一次资产，输出隔离的有效声明；作者引用与稀疏覆写始终保留在原 Scene。
// 不创建托管实例、导入网格或访问活动 World。
inline std::filesystem::path ResolveScriptAssetPath(const std::filesystem::path& root,
    std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    const std::filesystem::path logical(path);
    if (path.empty() || path.find('\0') != std::string::npos || logical.is_absolute() ||
        logical.has_root_name() || logical.has_root_directory())
        throw std::invalid_argument("Blueprint asset path must be resource-root-relative");
    for (const auto& part : logical)
        if (part == "..") throw std::invalid_argument("Blueprint asset path cannot contain traversal");
    const auto physicalRoot = std::filesystem::weakly_canonical(root);
    const auto physical = std::filesystem::weakly_canonical(physicalRoot / logical);
    const auto relative = physical.lexically_relative(physicalRoot);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
        logical.extension() != ".json")
        throw std::invalid_argument("Blueprint asset reference escapes resource root or is not JSON");
    return physical;
}

inline void ValidateScriptBlueprintAsset(const nlohmann::json& asset)
{
    if (!asset.is_object() || asset.value("type", "") != "blueprint" ||
        !asset.contains("name") || !asset.at("name").is_string() ||
        asset.at("name").get<std::string>().empty())
        throw std::invalid_argument("Blueprint requires blueprint type and name");
    for (const auto& field : asset.items())
        if (field.key() != "type" && field.key() != "name" && field.key() != "description" &&
            field.key() != "class" && field.key() != "properties" && field.key() != "components" &&
            field.key() != "rotation" && field.key() != "scale" && field.key() != "physics" &&
            field.key() != "instanceEditable" && field.key() != "componentInstanceEditable" &&
            field.key() != "scriptComponents")
            throw std::invalid_argument("Unsupported Blueprint asset field: " + field.key());
    if (asset.contains("instanceEditable"))
    {
        const auto& exposed = asset.at("instanceEditable");
        if (!exposed.is_array() || exposed.size() > 128)
            throw std::invalid_argument("instanceEditable requires at most 128 property names");
        std::set<std::string> names;
        for (const auto& name : exposed)
            if (!name.is_string() || name.get<std::string>().empty() ||
                name.get<std::string>().find('\0') != std::string::npos ||
                !names.insert(name.get<std::string>()).second)
                throw std::invalid_argument("instanceEditable requires unique non-empty property names");
    }
    auto declaration = asset;
    declaration["type"] = "blueprintInstance";
    declaration["id"] = "asset-validation";
    declaration.erase("componentInstanceEditable");
    ValidateScriptBlueprint(declaration);
    ValidateScriptComponentPermissions(asset.value("components", nlohmann::json::array()),
        asset.value("componentInstanceEditable", nlohmann::json::object()));
    for (const char* field : {"rotation", "scale"})
        if (asset.contains(field))
        {
            const auto& vector = asset.at(field);
            if (!vector.is_array() || vector.size() != 3)
                throw std::invalid_argument("Blueprint asset transform requires vec3");
            for (const auto& value : vector)
                if (!value.is_number() || !std::isfinite(value.get<float>()) ||
                    (std::string(field) == "scale" && std::abs(value.get<float>()) < 1e-6f))
                    throw std::invalid_argument("Invalid Blueprint asset transform");
        }
}

// 资产白名单缺省为空；合成前验证原始稀疏覆写，不能把资产默认值误判成实例写入。
inline void ValidateScriptInstanceOverrides(const nlohmann::json& asset, const nlohmann::json& instance)
{
    const auto allowed = asset.value("instanceEditable", nlohmann::json::array());
    for (const auto& property : instance.at("properties").items())
        if (std::find(allowed.begin(), allowed.end(), property.key()) == allowed.end())
            throw std::invalid_argument("Blueprint property is not Instance Editable: " + property.key());
    auto components = asset.value("components", nlohmann::json::array());
    ApplyScriptComponentOverrides(components, asset.value("componentInstanceEditable", nlohmann::json::object()),
        instance.value("componentOverrides", nlohmann::json::object()));
    auto logic = asset.value("scriptComponents", nlohmann::json::array());
    ApplyScriptLogicOverrides(logic, instance.value("scriptComponentOverrides", nlohmann::json::object()));
}

// 文件预算与重复键检查在解析边界完成，避免损坏资产在下游变成部分默认值。
struct ScriptAssetJsonGuard
{
    std::vector<std::set<std::string>> keys;
    bool operator()(int depth, nlohmann::json::parse_event_t event, nlohmann::json& value)
    {
        using Event = nlohmann::json::parse_event_t;
        if ((event == Event::object_start || event == Event::array_start) && depth >= 64)
            throw std::invalid_argument("Blueprint asset exceeds JSON depth 64");
        if (event == Event::object_start) keys.emplace_back();
        if (event == Event::object_end) keys.pop_back();
        if (event == Event::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("Duplicate Blueprint asset key");
        return true;
    }
};

inline nlohmann::json ReadScriptBlueprintAsset(const std::filesystem::path& root,
    const std::string& path, const std::string& ownerPath = {}, std::string* digest = nullptr)
{
    const auto physical = ResolveScriptAssetPath(root, path);
    if (!std::filesystem::is_regular_file(physical) ||
        std::filesystem::file_size(physical) > 4 * 1024 * 1024)
        throw std::invalid_argument("Blueprint asset must be a regular JSON file of at most 4 MiB");
    // 正式内容不可依赖 Generated；Common 不能反向引用场景专属资产。
    const auto owner = ownerPath.empty() ? std::filesystem::path() :
        std::filesystem::weakly_canonical(std::filesystem::path(root) / ownerPath)
            .lexically_relative(std::filesystem::weakly_canonical(root));
    const auto reference = physical.lexically_relative(std::filesystem::weakly_canonical(root));
    if (!owner.empty() && ScriptContentDomain(owner) != "generated" && ScriptContentDomain(reference) == "generated")
        throw std::invalid_argument("Formal scenes cannot reference Generated Blueprint assets");
    std::ifstream file(physical, std::ios::binary);
    std::string text;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount())
    {
        text.append(buffer, static_cast<size_t>(file.gcount()));
        if (text.size() > 4 * 1024 * 1024) throw std::invalid_argument("Blueprint asset exceeds 4 MiB");
    }
    if (file.bad() || !file.eof()) throw std::runtime_error("Cannot read Blueprint asset");
    auto asset = nlohmann::json::parse(text, ScriptAssetJsonGuard{});
    if (digest) *digest = ContentHasher::HashString(text).ToHex();
    ValidateScriptBlueprintAsset(asset);
    for (const auto& component : asset.value("components", nlohmann::json::array()))
        for (const char* field : {"modelPath", "materialOverride"})
            if (component.contains(field))
            {
                const auto dependency = ResolveScriptAssetPath(root, component.at(field).get<std::string>());
                const auto relative = dependency.lexically_relative(std::filesystem::weakly_canonical(root));
                if (!std::filesystem::is_regular_file(dependency))
                    throw std::invalid_argument("Missing Blueprint component asset");
                if ((ScriptContentDomain(reference) != "generated" && ScriptContentDomain(relative) == "generated") ||
                    (ScriptContentDomain(reference) == "common" && ScriptContentDomain(relative) == "maps"))
                    throw std::invalid_argument("Blueprint asset dependency violates content ownership");
            }
    if (!owner.empty() && ScriptContentDomain(owner) == "common" && ScriptContentDomain(reference) == "maps")
        throw std::invalid_argument("Common cannot reference scene-owned Blueprint assets");
    return asset;
}

inline nlohmann::json ResolveScriptBlueprint(const std::filesystem::path& root,
    const nlohmann::json& authored, const std::string& scenePath = {})
{
    ValidateScriptBlueprint(authored);
    if (!authored.contains("blueprintPath"))
    {
        // 模板身份、默认层和权限由解析器生成；作者输入不能借内部标记跳过实例校验。
        auto effective = authored;
        effective.erase("__templatePath");
        effective.erase("__instanceEditable");
        effective.erase("__componentInstanceEditable");
        effective.erase("__scriptComponentAsset");
        effective.erase("__propertyDefaults");
        effective.erase("__logicPropertyDefaults");
        return effective;
    }
    const auto asset = ReadScriptBlueprintAsset(root, authored.at("blueprintPath"), scenePath);
    ValidateScriptInstanceOverrides(asset, authored);
    auto effective = authored;
    effective.erase("blueprintPath");
    effective.erase("componentOverrides");
    effective.erase("scriptComponentOverrides");
    effective["class"] = asset.at("class");
    effective["__scriptComponentAsset"] = true;
    effective["__propertyDefaults"] = asset.at("properties");
    effective["__logicPropertyDefaults"] = nlohmann::json::object();
    for (const auto& node : asset.value("scriptComponents", nlohmann::json::array()))
        effective["__logicPropertyDefaults"][node.at("id").get<std::string>()] = node.at("properties");
    effective["__instanceEditable"] = asset.value("instanceEditable", nlohmann::json::array());
    effective["components"] = asset.value("components", nlohmann::json::array());
    effective["scriptComponents"] = asset.value("scriptComponents", nlohmann::json::array());
    ApplyScriptLogicOverrides(effective["scriptComponents"],
        authored.value("scriptComponentOverrides", nlohmann::json::object()));
    effective["__componentInstanceEditable"] = asset.value("componentInstanceEditable", nlohmann::json::object());
    ApplyScriptComponentOverrides(effective["components"], effective["__componentInstanceEditable"],
        authored.value("componentOverrides", nlohmann::json::object()));
    if (asset.contains("physics")) effective["physics"] = asset.at("physics");
    effective["properties"] = asset.at("properties");
    for (const auto& property : authored.at("properties").items())
        effective["properties"][property.key()] = property.value();
    ValidateScriptBlueprint(effective);
    return effective;
}

inline nlohmann::json ResolveScriptBlueprintScene(const std::filesystem::path& root,
    const nlohmann::json& scene, const std::string& scenePath)
{
    auto effective = scene;
    effective.erase("__scriptAssets");
    effective["__scriptAssets"] = nlohmann::json::object();
    auto templates = nlohmann::json::object();
    auto paths = scene.value("spawnAssets", nlohmann::json::array());
    if (!paths.is_array() || paths.size() > 128) throw std::invalid_argument("spawnAssets requires at most 128 paths");
    for (auto& object : effective.at("objects"))
        if (object.value("type", "") == "blueprintInstance")
        {
            ValidateScriptBlueprint(object);
            object.erase("__templatePath");
            object.erase("__instanceEditable");
            object.erase("__componentInstanceEditable");
            object.erase("__scriptComponentAsset");
            object.erase("__propertyDefaults");
            object.erase("__logicPropertyDefaults");
            if (object.contains("blueprintPath")) paths.push_back(object.at("blueprintPath"));
        }
    for (const auto& path : paths)
    {
        const auto name = path.get<std::string>();
        if (templates.contains(name)) continue;
        if (templates.size() >= 128) throw std::invalid_argument("Too many Blueprint spawn templates");
        std::string digest;
        auto asset = ReadScriptBlueprintAsset(root, name, scenePath, &digest);
        effective["__scriptAssets"][name] = digest;
        templates[name] = std::move(asset);
    }
    for (auto& object : effective.at("objects"))
        if (object.value("type", "") == "blueprintInstance" && object.contains("blueprintPath"))
        {
            const auto& asset = templates.at(object.at("blueprintPath").get<std::string>());
            ValidateScriptInstanceOverrides(asset, object);
            object["__instanceEditable"] = asset.value("instanceEditable", nlohmann::json::array());
            object.erase("blueprintPath");
            object["class"] = asset.at("class");
            object["__scriptComponentAsset"] = true;
            object["__propertyDefaults"] = asset.at("properties");
            object["__logicPropertyDefaults"] = nlohmann::json::object();
            for (const auto& node : asset.value("scriptComponents", nlohmann::json::array()))
                object["__logicPropertyDefaults"][node.at("id").get<std::string>()] = node.at("properties");
            object["components"] = asset.value("components", nlohmann::json::array());
            object["scriptComponents"] = asset.value("scriptComponents", nlohmann::json::array());
            ApplyScriptLogicOverrides(object["scriptComponents"],
                object.value("scriptComponentOverrides", nlohmann::json::object()));
            object.erase("scriptComponentOverrides");
            object["__componentInstanceEditable"] = asset.value("componentInstanceEditable", nlohmann::json::object());
            ApplyScriptComponentOverrides(object["components"], object["__componentInstanceEditable"],
                object.value("componentOverrides", nlohmann::json::object()));
            object.erase("componentOverrides");
            if (asset.contains("physics")) object["physics"] = asset.at("physics");
            auto properties = asset.at("properties");
            for (const auto& item : object.at("properties").items()) properties[item.key()] = item.value();
            object["properties"] = std::move(properties);
            ValidateScriptBlueprint(object);
        }
    // 预加载模板复用正式 mesh 计划，隐藏于候选 World；Spawn 只克隆这些已验证的资源键。
    size_t index = 0;
    for (auto& item : templates.items())
    {
        auto asset = item.value();
        std::string id;
        bool collision;
        do
        {
            id = "~template." + std::to_string(index++);
            collision = false;
            for (const auto& object : effective.at("objects"))
                if (object.value("type", "") == "blueprintInstance" && object.at("id") == id) collision = true;
        } while (collision);
        asset["type"] = "blueprintInstance"; asset["id"] = id; asset["position"] = {0, 0, 0};
        asset["__instanceEditable"] = asset.value("instanceEditable", nlohmann::json::array());
        asset["__componentInstanceEditable"] = asset.value("componentInstanceEditable", nlohmann::json::object());
        asset.erase("componentInstanceEditable");
        if (!asset.contains("rotation")) asset["rotation"] = {0, 0, 0};
        if (!asset.contains("scale")) asset["scale"] = {1, 1, 1};
        asset["__templatePath"] = item.key();
        effective["objects"].push_back(std::move(asset));
    }
    return effective;
}
} // namespace VL
