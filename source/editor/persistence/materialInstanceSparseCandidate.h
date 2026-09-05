#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include <nlohmann/json.hpp>

namespace VL::Editor::Persistence
{
    class MaterialInstancePersistenceError : public std::runtime_error
    {
    public:
        explicit MaterialInstancePersistenceError(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    enum class MaterialInstanceNumericType
    {
        Float,
        Vec2,
        Vec3,
        Vec4,
        Color,
    };

    struct MaterialInstanceColor
    {
        std::array<float, 4> components{};
        bool operator==(const MaterialInstanceColor& other) const noexcept
        {
            return components == other.components;
        }
    };

    using MaterialInstanceNumericValue = std::variant<
        float,
        std::array<float, 2>,
        std::array<float, 3>,
        std::array<float, 4>,
        MaterialInstanceColor>;

    using MaterialInstanceParameterMap =
        std::map<std::string, MaterialInstanceNumericValue>;
    using MaterialInstanceTextureMap = std::map<std::string, std::string>;
    using MaterialInstanceRenderStateMap = std::map<std::string, std::string>;

    struct MaterialInstanceDefaults
    {
        MaterialInstanceParameterMap parameters;
        std::map<std::string, std::optional<std::string>> textures;
        MaterialInstanceRenderStateMap renderStates;
    };

    struct MaterialInstanceSparseOverrides
    {
        MaterialInstanceParameterMap parameters;
        MaterialInstanceTextureMap textures;
        MaterialInstanceRenderStateMap renderStates;
        // 旧调用方只编辑参数/纹理时仍保留原始 renderStateOverrides；
        // 文档服务加载或首次编辑 render state 后才接管该对象的稀疏重写。
        bool renderStatesManaged = false;
    };

    struct MaterialInstanceSparseCandidate
    {
        // 保留原始 MI 根对象，使 persistence 层不会吞掉宏、渲染状态和未来字段。
        nlohmann::json sourceJson;
        MaterialInstanceSparseOverrides overrides;
    };

    MaterialInstanceNumericType GetMaterialInstanceNumericType(
        const MaterialInstanceNumericValue& value) noexcept;

    std::string_view ToString(MaterialInstanceNumericType type) noexcept;

    MaterialInstanceNumericType ParseMaterialInstanceNumericType(
        std::string_view type);

    MaterialInstanceNumericValue ParseMaterialInstanceNumericValue(
        MaterialInstanceNumericType type,
        const nlohmann::json& value,
        std::string_view fieldName = {});

    nlohmann::json SerializeMaterialInstanceNumericValue(
        const MaterialInstanceNumericValue& value);

    bool MaterialInstanceNumericValuesExactlyEqual(
        const MaterialInstanceNumericValue& left,
        const MaterialInstanceNumericValue& right) noexcept;

    bool IsMaterialInstanceTextureAssetPath(std::string_view path) noexcept;

    MaterialInstanceDefaults ParseMaterialInstanceDefaults(
        const nlohmann::json& materialDefinitionJson);

    MaterialInstanceSparseOverrides ParseMaterialInstanceSparseOverrides(
        const nlohmann::json& materialInstanceJson,
        const MaterialInstanceDefaults& defaults);

    MaterialInstanceSparseCandidate BuildMaterialInstanceSparseCandidate(
        const nlohmann::json& originalMaterialInstanceJson,
        const MaterialInstanceDefaults& defaults,
        const MaterialInstanceSparseOverrides& workingOverrides);

    nlohmann::json SerializeMaterialInstanceSparseCandidate(
        const MaterialInstanceSparseCandidate& candidate);

    std::string SerializeMaterialInstanceSparseCandidateText(
        const MaterialInstanceSparseCandidate& candidate,
        int indent = 4,
        bool appendFinalNewline = true);
}
