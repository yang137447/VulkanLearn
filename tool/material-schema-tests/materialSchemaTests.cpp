#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "material/materialDescriptorSchema.h"
#include "material/loader/materialInstanceResolver.h"
#include "material/validation/materialAssetValidator.h"

namespace
{

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireThrows(
    const std::function<void()>& callback,
    const std::string& message)
{
    try
    {
        callback();
    }
    catch (const std::exception&)
    {
        return;
    }
    throw std::runtime_error(message);
}

nlohmann::json BuildMaterialDefinition()
{
    return {
        {"name", "M_schemaTest"},
        {"type", "material"},
        {"shadingModel", "DefaultLit"},
        {"renderStates", nlohmann::json::object()},
        {"macros", nlohmann::json::object()},
        {"parameters", {
            {"u_packed", {
                {"type", "vec4"},
                {"description", "packed authoring values"},
                {"default", {1.0, 2.0, 3.0, 4.0}},
                {"channels", {
                    {"x", {{"name", "first"}, {"description", "first value"}, {"range", {{"min", 0.0}, {"max", 1.0}}}}},
                    {"y", {{"name", "second"}, {"description", "second value"}, {"range", {{"min", 0.0}, {"max", 2.0}}}}},
                    {"z", {{"name", "third"}, {"description", "third value"}, {"range", {{"min", 0.0}, {"max", 3.0}}}}},
                    {"w", {{"name", "fourth"}, {"description", "fourth value"}, {"range", {{"min", 0.0}, {"max", 4.0}}}}}}}}},
            {"u_scalar", {
                {"type", "float"},
                {"description", "scalar authoring value"},
                {"default", 1.0}}}}},
        {"textures", nlohmann::json::object()}};
}

TEST(MaterialSchema, PreviewUsesFrozenDefinitionDefaults)
{
    nlohmann::json frozen = BuildMaterialDefinition();
    frozen["parameters"]["u_scalar"]["default"] = 0.25;
    frozen["modelProfile"] = "Common/Profiles/frozen.json";
    const nlohmann::json draft{
        {"type", "materialInstance"}, {"name", "MI_schemaTest"},
        {"material", "shader/glsl/M_schemaTest.json"}};
    const auto resolved = MaterialInstanceResolver::ResolveWithDefinition(
        "MI_schemaTest.json", draft, frozen);
    EXPECT_EQ(resolved.effectiveMaterialInstanceJson["parameters"]["u_scalar"], 0.25);
    EXPECT_EQ(resolved.effectiveMaterialInstanceJson["modelProfile"],
              "Common/Profiles/frozen.json");
    EXPECT_EQ(resolved.materialJson, frozen);
}

TEST(MaterialSchema, PreviewSparseOverrideKeepsFrozenDefaults)
{
    const auto frozen = BuildMaterialDefinition();
    const nlohmann::json draft{
        {"type", "materialInstance"},
        {"material", "shader/glsl/M_schemaTest.json"},
        {"parameters", {{"u_scalar", 0.5}}}};
    const auto resolved = MaterialInstanceResolver::ResolveWithDefinition(
        "MI_schemaTest.json", draft, frozen);
    EXPECT_EQ(resolved.effectiveMaterialInstanceJson["parameters"]["u_scalar"], 0.5);
    EXPECT_EQ(resolved.effectiveMaterialInstanceJson["parameters"]["u_packed"],
              frozen["parameters"]["u_packed"]["default"]);
}

TEST(MaterialSchema, InvalidDraftResolutionLeavesPreviousValuesUntouched)
{
    const auto frozen = BuildMaterialDefinition();
    nlohmann::json source{
        {"type", "materialInstance"},
        {"material", "shader/glsl/M_schemaTest.json"},
        {"parameters", {{"u_scalar", 0.5}}}};
    const auto active = MaterialInstanceResolver::ResolveWithDefinition(
        "MI_schemaTest.json", source, frozen).effectiveMaterialInstanceJson;
    source["parameters"]["unknown"] = 2.0;
    EXPECT_THROW(MaterialInstanceResolver::ResolveWithDefinition(
        "MI_schemaTest.json", source, frozen), std::runtime_error);
    EXPECT_EQ(active["parameters"]["u_scalar"], 0.5);
    EXPECT_FALSE(active["parameters"].contains("unknown"));
}

void TestChannelsRetention()
{
    const nlohmann::json materialJson = BuildMaterialDefinition();
    MaterialAssetValidator::ValidateDefinition(
        materialJson,
        "shader/glsl/M_schemaTest.json");
    const VL::MaterialDescriptorSchema schema =
        VL::MaterialDescriptorSchema::Build(
            materialJson,
            "shader/glsl/M_schemaTest.json");

    const auto& parameters = schema.GetParameters();
    Require(parameters.size() == 2, "material schema parameter count mismatch");
    const auto packedIt = std::find_if(
        parameters.begin(),
        parameters.end(),
        [](const VL::MaterialParameterSchemaEntry& entry) {
            return entry.name == "u_packed";
        });
    Require(packedIt != parameters.end(), "packed parameter was not retained");
    Require(
        packedIt->description == "packed authoring values",
        "parameter description was not retained");
    Require(
        packedIt->channels.size() == 4,
        "channel metadata was not retained in x/y/z/w order");
    Require(
        packedIt->channels[0].name == "first" &&
            packedIt->channels[0].description == "first value" &&
            packedIt->channels[0].min == 0.0f &&
            packedIt->channels[0].max == 1.0f,
        "channel name, description, and range were not retained");
}

void TestChannelsValidation()
{
    nlohmann::json materialJson = BuildMaterialDefinition();
    materialJson["parameters"]["u_packed"]["channels"].erase("w");
    RequireThrows(
        [&materialJson]() {
            MaterialAssetValidator::ValidateDefinition(
                materialJson,
                "shader/glsl/M_schemaTest.json");
        },
        "missing vec4 channel metadata was accepted");

    materialJson = BuildMaterialDefinition();
    materialJson["parameters"]["u_packed"]["channels"].erase("w");
    materialJson["parameters"]["u_packed"]["channels"]["q"] =
        {"name", "unknown"};
    RequireThrows(
        [&materialJson]() {
            MaterialAssetValidator::ValidateDefinition(
                materialJson,
                "shader/glsl/M_schemaTest.json");
        },
        "unknown vec4 channel metadata was accepted");

    materialJson = BuildMaterialDefinition();
    materialJson["parameters"]["u_packed"]["channels"]["z"]["description"] = "   ";
    RequireThrows(
        [&materialJson]() {
            MaterialAssetValidator::ValidateDefinition(
                materialJson,
                "shader/glsl/M_schemaTest.json");
        },
        "empty vec4 channel description was accepted");

    materialJson = BuildMaterialDefinition();
    materialJson["parameters"]["u_packed"]["channels"]["z"]["range"] = {
        {"min", 4.0}, {"max", 3.0}};
    RequireThrows(
        [&materialJson]() {
            MaterialAssetValidator::ValidateDefinition(
                materialJson,
                "shader/glsl/M_schemaTest.json");
        },
        "inverted channel range was accepted");

    materialJson = BuildMaterialDefinition();
    materialJson["parameters"]["u_packed"]["channels"]["x"]["range"]["max"] = 0.5;
    RequireThrows(
        [&materialJson]() {
            MaterialAssetValidator::ValidateDefinition(
                materialJson,
                "shader/glsl/M_schemaTest.json");
        },
        "default outside channel range was accepted");

    materialJson = BuildMaterialDefinition();
    materialJson["parameters"]["u_scalar"]["channels"] = {
        {"x", {{"name", "scalar"}, {"description", "scalar value"}, {"range", {{"min", 0.0}, {"max", 1.0}}}}}};
    RequireThrows(
        [&materialJson]() {
            MaterialAssetValidator::ValidateDefinition(
                materialJson,
                "shader/glsl/M_schemaTest.json");
        },
        "scalar channels metadata was accepted");
}

void TestParameterMetadataValidation()
{
    nlohmann::json materialJson = BuildMaterialDefinition();
    materialJson["parameters"]["u_scalar"].erase("description");
    RequireThrows(
        [&materialJson]() {
            MaterialAssetValidator::ValidateDefinition(
                materialJson,
                "shader/glsl/M_schemaTest.json");
        },
        "missing scalar parameter description was accepted");

    materialJson = BuildMaterialDefinition();
    materialJson["parameters"]["u_packed"].erase("channels");
    RequireThrows(
        [&materialJson]() {
            MaterialAssetValidator::ValidateDefinition(
                materialJson,
                "shader/glsl/M_schemaTest.json");
        },
        "missing vector channel metadata was accepted");
}

void TestColorUsesVec4Layout()
{
    nlohmann::json materialJson = BuildMaterialDefinition();
    materialJson["parameters"]["u_color"] = {
        {"type", "color"},
        {"description", "color authoring value"},
        {"default", {0.25, 0.5, 0.75, 1.0}}};
    materialJson["parameters"]["u_color"]["channels"] = {
        {"x", {{"name", "red"}, {"description", "red color channel"}, {"range", {{"min", 0.0}, {"max", 1.0}}}}},
        {"y", {{"name", "green"}, {"description", "green color channel"}, {"range", {{"min", 0.0}, {"max", 1.0}}}}},
        {"z", {{"name", "blue"}, {"description", "blue color channel"}, {"range", {{"min", 0.0}, {"max", 1.0}}}}},
        {"w", {{"name", "alpha"}, {"description", "alpha channel"}, {"range", {{"min", 0.0}, {"max", 1.0}}}}}};

    MaterialAssetValidator::ValidateDefinition(
        materialJson,
        "shader/glsl/M_schemaTest.json");
    const VL::MaterialDescriptorSchema schema =
        VL::MaterialDescriptorSchema::Build(
            materialJson,
            "shader/glsl/M_schemaTest.json");

    const auto colorIt = std::find_if(
        schema.GetParameters().begin(),
        schema.GetParameters().end(),
        [](const VL::MaterialParameterSchemaEntry& entry) {
            return entry.name == "u_color";
        });
    Require(colorIt != schema.GetParameters().end(), "color parameter was not retained");
    Require(
        colorIt->glslType == "vec4" && colorIt->size == 16u,
        "color parameter did not reuse the vec4 GPU layout");
}

} // namespace

TEST(MaterialSchema, ChannelsRetention)
{
    TestChannelsRetention();
}

TEST(MaterialSchema, ChannelsValidation)
{
    TestChannelsValidation();
}

TEST(MaterialSchema, ColorUsesVec4Layout)
{
    TestColorUsesVec4Layout();
}

TEST(MaterialSchema, ParameterMetadataValidation)
{
    TestParameterMetadataValidation();
}

TEST(MaterialSchema, OfflineModelIdentityIsExplicit)
{
    auto material = BuildMaterialDefinition();
    material["offlineModel"] = "ClothFiber";
    EXPECT_NO_THROW(MaterialAssetValidator::ValidateDefinition(
        material, "shader/glsl/M_schemaTest.json"));
    material["offlineModel"] = "DefaultLit";
    EXPECT_THROW(MaterialAssetValidator::ValidateDefinition(
        material, "shader/glsl/M_schemaTest.json"), std::runtime_error);
}

TEST(MaterialSchema, ModelProfileIsSparseAssetReference)
{
    auto material = BuildMaterialDefinition();
    material["modelProfile"] = "Common/Profiles/cloth.json";
    MaterialAssetValidator::ValidateDefinition(
        material, "shader/glsl/M_schemaTest.json");
    nlohmann::json instance = {
        {"type", "materialInstance"},
        {"material", "shader/glsl/M_schemaTest.json"},
        {"modelProfile", "Common/Profiles/cloth-variant.json"}};
    EXPECT_NO_THROW(MaterialAssetValidator::ValidateInstanceHeader(
        instance, "MI_test.json"));
    EXPECT_NO_THROW(MaterialAssetValidator::ValidateInstanceOverrides(
        material, instance, "MI_test.json"));
    instance["modelProfile"] = material["modelProfile"];
    EXPECT_THROW(MaterialAssetValidator::ValidateInstanceOverrides(
        material, instance, "MI_test.json"), std::runtime_error);
}
