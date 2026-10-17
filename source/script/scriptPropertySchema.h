#pragma once

#include <cmath>
#include <limits>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "script/scriptTypeIdentity.h"

namespace VL
{
inline bool ScriptPropertyVectorType(const std::string& type)
{
    return type == "vector2" || type == "vector3" || type == "vector4" || type == "quaternion";
}

inline void ValidateScriptPropertyTreeNode(const nlohmann::json& value, size_t depth, size_t& nodes)
{
    if (depth > 16 || ++nodes > 4096 || value.is_null() || value.is_discarded())
        throw std::invalid_argument("Script property exceeds depth/node budget or contains null");
    if (value.is_number() && !std::isfinite(value.get<double>()))
        throw std::invalid_argument("Script property numbers must be finite");
    if (!value.is_structured()) return;
    if (value.size() > 1024) throw std::invalid_argument("Script property container exceeds 1024 entries");
    // 无类目录的结构入口只做宽校验；短数字数组可能是一个向量，精确预算由 schema 再核对。
    if (value.is_array() && value.size() >= 2 && value.size() <= 4)
    {
        bool numeric = true;
        for (const auto& child : value) numeric = numeric && child.is_number() && std::isfinite(child.get<double>());
        if (numeric) return;
    }
    for (const auto& child : value) ValidateScriptPropertyTreeNode(child, depth + 1, nodes);
}

inline void ValidateScriptPropertyTree(const nlohmann::json& value)
{
    size_t nodes = 0;
    ValidateScriptPropertyTreeNode(value, 0, nodes);
}

inline void ValidateScriptPropertyObject(const nlohmann::json& properties)
{
    if (!properties.is_object() || properties.size() > 128)
        throw std::invalid_argument("Script properties require at most 128 named values");
    for (const auto& property : properties.items())
    {
        if (property.key().empty() || property.key().find('\0') != std::string::npos)
            throw std::invalid_argument("Script property requires a non-empty name");
        ValidateScriptPropertyTree(property.value());
    }
}

inline const nlohmann::json& ScriptObjectField(const nlohmann::json& schema, const std::string& name)
{
    for (const auto& field : schema.at("fields"))
        if (field.at("name") == name) return field;
    throw std::runtime_error("Unknown script object field: " + name);
}

inline void ValidateScriptPropertyValueNode(const nlohmann::json& field, const nlohmann::json& value,
    size_t depth, size_t& nodes, const std::string& path)
{
    if (depth > 16 || ++nodes > 4096 || value.is_null())
        throw std::runtime_error("Invalid script property depth/node/null at " + path);
    const auto type = field.at("type").get<std::string>();
    const bool vector = ScriptPropertyVectorType(type);
    bool valid = false;
    if (type == "object" || type == "dictionary" || type == "array")
    {
        valid = type == "array" ? value.is_array() : value.is_object();
        if (!valid || value.size() > 1024)
            throw std::runtime_error("Invalid script property container at " + path);
        for (const auto& child : value.items())
        {
            const auto& descriptor = type == "object" ? ScriptObjectField(field, child.key()) : field.at("element");
            ValidateScriptPropertyValueNode(descriptor, child.value(), depth + 1, nodes, path + "/" + child.key());
        }
        return;
    }
    if (type == "string" || type == "enum" || type == "asset" || type == "blueprintReference") valid = value.is_string();
    else if (type == "bool") valid = value.is_boolean();
    else if (vector)
        valid = value.is_array() && value.size() == size_t(type == "quaternion" ? 4 : type.back() - '0');
    else if (type == "number" || type == "int" || type == "uint") valid = value.is_number();
    if (valid && vector)
        for (const auto& number : value)
            valid = valid && number.is_number() && std::isfinite(number.get<float>());
    if (valid && type == "quaternion")
    {
        double norm = 0;
        for (const auto& number : value) norm += number.get<double>() * number.get<double>();
        valid = std::abs(norm - 1.0) <= 1e-4;
    }
    if (valid && type == "enum")
    {
        const auto& options = field.at("options");
        valid = std::find(options.begin(), options.end(), value) != options.end();
    }
    if (valid && (type == "asset" || type == "blueprintReference"))
    {
        const auto text = value.get<std::string>();
        valid = text.find('\0') == std::string::npos;
        if (type == "blueprintReference") valid = valid && text.find_first_of("/\\|") == std::string::npos;
        else if (!text.empty())
        {
            valid = valid && text.front() != '/' && text.back() != '/' && text.find_first_of("\\:") == std::string::npos;
            size_t start = 0;
            while (start < text.size())
            {
                const auto end = text.find('/', start);
                const auto part = text.substr(start, end == std::string::npos ? end : end - start);
                valid = valid && !part.empty() && part != "." && part != "..";
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
    }
    if (valid && value.is_number())
    {
        const double number = value.get<double>();
        valid = std::isfinite(number);
        if (type == "int" || type == "uint")
            valid = valid && std::floor(number) == number && number >= (type == "uint" ? 0 : INT32_MIN) &&
                number <= (type == "uint" ? double(UINT32_MAX) : double(INT32_MAX));
    }
    if (!valid) throw std::runtime_error("Invalid exposed property value at " + path);
}

inline void ValidateScriptPropertyValue(const nlohmann::json& field, const nlohmann::json& value)
{
    size_t nodes = 0;
    ValidateScriptPropertyValueNode(field, value, 0, nodes, field.value("name", std::string("$")));
}

inline nlohmann::json MergeScriptPropertyValueNode(const nlohmann::json& schema,
    const nlohmann::json& baseline, const nlohmann::json& authored)
{
    const auto type = schema.at("type").get<std::string>();
    if (type == "object")
    {
        auto value = baseline;
        for (const auto& field : schema.at("fields"))
        {
            const auto name = field.at("name").get<std::string>();
            if (!value.contains(name)) value[name] = field.at("value");
        }
        for (const auto& item : authored.items())
            value[item.key()] = MergeScriptPropertyValueNode(ScriptObjectField(schema, item.key()),
                value.at(item.key()), item.value());
        return value;
    }
    if (type == "array" || type == "dictionary")
    {
        auto value = authored;
        const auto& element = schema.at("element");
        for (auto& child : value)
            child = MergeScriptPropertyValueNode(element, element.at("value"), child);
        return value;
    }
    return authored;
}

inline nlohmann::json MergeScriptPropertyValue(const nlohmann::json& schema,
    const nlohmann::json& baseline, const nlohmann::json& authored)
{
    ValidateScriptPropertyValue(schema, baseline);
    ValidateScriptPropertyValue(schema, authored);
    auto value = MergeScriptPropertyValueNode(schema, baseline, authored);
    // 部分对象合成后也受单属性总预算限制，不能靠多层各自合法的稀疏覆写膨胀。
    ValidateScriptPropertyValue(schema, value);
    return value;
}

inline nlohmann::json CompleteScriptPropertyValue(const nlohmann::json& node, const nlohmann::json& field)
{
    const auto name = field.at("name").get<std::string>();
    auto value = field.at("value");
    if (node.contains("__propertyDefaults") && node.at("__propertyDefaults").contains(name))
        value = MergeScriptPropertyValue(field, value, node.at("__propertyDefaults").at(name));
    if (node.at("properties").contains(name))
        value = MergeScriptPropertyValue(field, value, node.at("properties").at(name));
    return value;
}

inline nlohmann::json MergeSparseScriptProperty(const nlohmann::json& schema,
    const nlohmann::json& authored, const nlohmann::json& patch)
{
    if (schema.at("type") != "object") return patch;
    auto value = authored.is_object() ? authored : nlohmann::json::object();
    for (const auto& field : patch.items())
        value[field.key()] = MergeSparseScriptProperty(ScriptObjectField(schema, field.key()),
            value.value(field.key(), nlohmann::json()), field.value());
    return value;
}

inline size_t ScriptPropertyArrayIndex(const nlohmann::json& value, size_t count)
{
    if (!value.is_number_integer() || (value.is_number_integer() && value.get<double>() < 0) ||
        value.get<double>() >= double(count))
        throw std::invalid_argument("Invalid script property array index");
    return value.get<size_t>();
}

// 对象成员的 Reset 只删除作者键；集合是原子覆写，禁止沿集合下标伪造局部继承。
inline nlohmann::json ResetSparseScriptProperty(const nlohmann::json& schema,
    const nlohmann::json& authored, const nlohmann::json& path, size_t step = 0)
{
    if (schema.at("type") != "object" || step >= path.size() || !path[step].is_string())
        throw std::invalid_argument("Reset path must select object fields before any collection");
    const auto name = path[step].get<std::string>();
    const auto& field = ScriptObjectField(schema, name);
    auto result = authored.is_object() ? authored : nlohmann::json::object();
    if (step + 1 == path.size()) result.erase(name);
    else
    {
        auto child = ResetSparseScriptProperty(field, result.value(name, nlohmann::json()), path, step + 1);
        if (child.empty()) result.erase(name);
        else result[name] = std::move(child);
    }
    return result;
}

inline nlohmann::json ApplyScriptPropertyEdit(const nlohmann::json& schema,
    const nlohmann::json& authored, const nlohmann::json& effective, const nlohmann::json& request)
{
    const auto path = request.value("path", nlohmann::json::array());
    if (!path.is_array() || path.size() > 16)
        throw std::invalid_argument("Script property path requires at most 16 tokens");
    if (request.value("operation", std::string("set")) == "reset")
    {
        auto result = ResetSparseScriptProperty(schema, authored, path);
        ValidateScriptPropertyValue(schema, result);
        // null 只作为作者操作的删键结果，不是可序列化的用户数据值。
        return result.empty() ? nlohmann::json() : result;
    }
    auto current = effective;
    auto* target = &current;
    const auto* descriptor = &schema;
    const nlohmann::json numberSchema{{"type", "number"}, {"value", 0}};
    size_t atomic = path.size() + 1;
    // 对象叶子只写稀疏作者键；首次穿过集合/向量后保存完整容器，避免伪造数组式稀疏覆写。
    for (size_t step = 0; step < path.size(); ++step)
    {
        const auto type = descriptor->at("type").get<std::string>();
        if (atomic > path.size() && (type == "array" || type == "dictionary" || ScriptPropertyVectorType(type)))
            atomic = step;
        if (type == "object")
        {
            if (!path[step].is_string()) throw std::invalid_argument("Object property path requires field names");
            const auto name = path[step].get<std::string>();
            descriptor = &ScriptObjectField(*descriptor, name);
            target = &target->at(name);
        }
        else if (type == "dictionary")
        {
            if (!path[step].is_string()) throw std::invalid_argument("Dictionary property path requires string keys");
            target = &target->at(path[step].get<std::string>());
            descriptor = &descriptor->at("element");
        }
        else if (type == "array")
        {
            target = &target->at(ScriptPropertyArrayIndex(path[step], target->size()));
            descriptor = &descriptor->at("element");
        }
        else if (ScriptPropertyVectorType(type) && type != "quaternion" && step + 1 == path.size())
        {
            target = &target->at(ScriptPropertyArrayIndex(path[step], target->size()));
            descriptor = &numberSchema;
        }
        else throw std::invalid_argument("Script property path cannot descend into a scalar or quaternion");
    }
    const auto type = descriptor->at("type").get<std::string>();
    if (atomic > path.size() && (type == "array" || type == "dictionary" || ScriptPropertyVectorType(type)))
        atomic = path.size();
    const auto operation = request.value("operation", std::string("set"));
    if (operation == "set")
        *target = MergeScriptPropertyValue(*descriptor, *target, request.at("value"));
    else if (operation == "append" && type == "array")
        target->push_back(MergeScriptPropertyValue(descriptor->at("element"), descriptor->at("element").at("value"),
            request.value("value", descriptor->at("element").at("value"))));
    else if (operation == "insert" && type == "dictionary")
    {
        const auto key = request.at("key").get<std::string>();
        if (target->contains(key)) throw std::invalid_argument("Script dictionary key already exists");
        (*target)[key] = MergeScriptPropertyValue(descriptor->at("element"), descriptor->at("element").at("value"),
            request.value("value", descriptor->at("element").at("value")));
    }
    else if (operation == "remove" && type == "array")
        target->erase(ScriptPropertyArrayIndex(request.at("index"), target->size()));
    else if (operation == "remove" && type == "dictionary")
    {
        if (!target->erase(request.at("key").get<std::string>()))
            throw std::invalid_argument("Unknown script dictionary key");
    }
    else throw std::invalid_argument("Unsupported script property collection operation");
    ValidateScriptPropertyValue(schema, current);
    const size_t savedDepth = atomic <= path.size() ? atomic : path.size();
    const auto* saved = &current;
    auto result = authored;
    auto* raw = &result;
    for (size_t step = 0; step < savedDepth; ++step)
    {
        const auto name = path[step].get<std::string>();
        saved = &saved->at(name);
        if (!raw->is_object()) *raw = nlohmann::json::object();
        raw = &(*raw)[name];
    }
    if (atomic <= path.size() || type != "object") *raw = *saved;
    else *raw = MergeSparseScriptProperty(*descriptor, *raw, request.at("value"));
    ValidateScriptPropertyValue(schema, result);
    return result;
}

// GT 使用宿主一次发布的类目录校验作者数据；不构造 Blueprint，不运行 OnCreate。
inline void ValidateScriptPropertySchema(const nlohmann::json& blueprint, const nlohmann::json& classes)
{
    const nlohmann::json* schema = nullptr;
    for (const auto& type : classes)
        if (ScriptTypeNameMatches(type, blueprint.at("class").get<std::string>())) schema = &type.at("properties");
    if (!schema) throw std::runtime_error("Unknown Blueprint class");
    for (const char* layer : {"properties", "__propertyDefaults"})
    {
        if (!blueprint.contains(layer)) continue;
        ValidateScriptPropertyObject(blueprint.at(layer));
        for (const auto& property : blueprint.at(layer).items())
        {
            const nlohmann::json* field = nullptr;
            for (const auto& entry : *schema) if (entry.at("name") == property.key()) field = &entry;
            if (!field) throw std::runtime_error("Unknown exposed property: " + property.key());
            ValidateScriptPropertyValue(*field, property.value());
        }
    }
    for (const auto& name : blueprint.value("instanceEditable",
        blueprint.value("__instanceEditable", nlohmann::json::array())))
    {
        bool found = false;
        for (const auto& field : *schema) if (field.at("name") == name) found = true;
        if (!found) throw std::runtime_error("Instance Editable requires an exposed class property");
    }
}
} // namespace VL
