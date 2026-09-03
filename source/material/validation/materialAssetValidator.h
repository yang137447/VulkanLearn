#pragma once

#include <string_view>
#include <nlohmann/json.hpp>

// Central validation rules for material definition and material instance JSON assets.
// Keep format checks here so loaders and generators can stay focused on transformation work.
// This validator rejects unknown MI_ fields instead of carrying old-format compatibility.
class MaterialAssetValidator
{
public:
    // Validates an M_*.json material definition: required fields, known shading model, and numeric macros.
    static void ValidateDefinition(
        const nlohmann::json& materialJson,
        std::string_view materialPath);

    // Validates the top-level MI_*.json shape before it is merged with its referenced M_ asset.
    static void ValidateInstanceHeader(
        const nlohmann::json& materialInstanceJson,
        std::string_view materialInstancePath);

    // Validates sparse MI overrides against fields and defaults declared by M_.
    static void ValidateInstanceOverrides(
        const nlohmann::json& materialJson,
        const nlohmann::json& materialInstanceJson,
        std::string_view materialInstancePath);

    // Validates renderer-independent pairing rules for effective material state.
    static void ValidateRenderStateCombination(
        std::string_view shadingModel,
        std::string_view renderMode,
        std::string_view materialInstancePath);

    static void ValidateRenderStateCombination(
        std::string_view shadingModel,
        std::string_view renderMode,
        std::string_view cullMode,
        std::string_view materialInstancePath);

    // Ensures an MI_ override object only targets fields declared by the referenced M_ asset.
    static void EnsureKnownOverrideKeys(
        const nlohmann::json& overrides,
        const nlohmann::json& source,
        std::string_view field,
        std::string_view materialInstancePath);
};
