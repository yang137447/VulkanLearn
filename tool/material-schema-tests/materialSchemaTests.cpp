#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "material/materialDescriptorSchema.h"
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
                {"default", {1.0, 2.0, 3.0, 4.0}},
                {"channels", {
                    {"x", {{"name", "first"}, {"description", "first value"}, {"range", {{"min", 0.0}, {"max", 1.0}}}}},
                    {"y", {{"name", "second"}, {"description", "second value"}, {"range", {{"min", 0.0}, {"max", 2.0}}}}},
                    {"z", {{"name", "third"}, {"description", "third value"}, {"range", {{"min", 0.0}, {"max", 3.0}}}}},
                    {"w", {{"name", "fourth"}, {"description", "fourth value"}, {"range", {{"min", 0.0}, {"max", 4.0}}}}}}}}},
            {"u_scalar", {
                {"type", "float"},
                {"default", 1.0}}}}},
        {"textures", nlohmann::json::object()}};
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

} // namespace

TEST(MaterialSchema, ChannelsRetention)
{
    TestChannelsRetention();
}

TEST(MaterialSchema, ChannelsValidation)
{
    TestChannelsValidation();
}
