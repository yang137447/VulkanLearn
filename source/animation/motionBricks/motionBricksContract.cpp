#include "motionBricksContract.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace VL::Animation::MotionBricks
{
namespace
{
using json = nlohmann::json;

const json& RequireMember(const json& object, const char* key, const std::string& context)
{
    const auto it = object.find(key);
    if (it == object.end())
        throw std::invalid_argument(context + " is missing '" + key + "'");
    return *it;
}

std::string RequireString(const json& object, const char* key, const std::string& context)
{
    const json& value = RequireMember(object, key, context);
    if (!value.is_string())
        throw std::invalid_argument(context + "." + key + " must be a string");
    return value.get<std::string>();
}

int64_t RequireInteger(const json& object, const char* key, const std::string& context)
{
    const json& value = RequireMember(object, key, context);
    if (!value.is_number_integer())
        throw std::invalid_argument(context + "." + key + " must be an integer");
    return value.get<int64_t>();
}

double RequireNumber(const json& object, const char* key, const std::string& context)
{
    const json& value = RequireMember(object, key, context);
    if (!value.is_number())
        throw std::invalid_argument(context + "." + key + " must be a number");
    return value.get<double>();
}

TensorMode ParseTensorMode(const std::string& value, const std::string& context)
{
    if (value == "input") return TensorMode::Input;
    if (value == "output") return TensorMode::Output;
    throw std::invalid_argument(context + " has unsupported mode '" + value + "'");
}

TensorDataType ParseTensorDataType(const std::string& value, const std::string& context)
{
    if (value == "float32") return TensorDataType::Float32;
    if (value == "int32") return TensorDataType::Int32;
    throw std::invalid_argument(context + " has unsupported dtype '" + value + "'");
}

std::string TensorModeName(TensorMode mode)
{
    return mode == TensorMode::Input ? "input" : "output";
}

std::string TensorDataTypeName(TensorDataType dtype)
{
    return dtype == TensorDataType::Float32 ? "float32" : "int32";
}

std::string ShapeText(const std::vector<int64_t>& shape)
{
    std::ostringstream text;
    text << '[';
    for (size_t index = 0; index < shape.size(); ++index)
    {
        if (index != 0) text << ',';
        text << shape[index];
    }
    text << ']';
    return text.str();
}

struct ExpectedTensor
{
    const char* name;
    TensorMode mode;
    std::vector<int64_t> shape;
    TensorDataType dtype;
};

const std::vector<ExpectedTensor>& SomaTensorSurface()
{
    static const std::vector<ExpectedTensor> expected = {
        {"global_root_values", TensorMode::Input, {1, 8, 5}, TensorDataType::Float32},
        {"has_global_root_values", TensorMode::Input, {1, 8}, TensorDataType::Int32},
        {"local_root_values", TensorMode::Input, {1, 8, 4}, TensorDataType::Float32},
        {"has_local_root_values", TensorMode::Input, {1, 8}, TensorDataType::Int32},
        {"local_poses", TensorMode::Input, {1, 8, 267}, TensorDataType::Float32},
        {"has_local_poses", TensorMode::Input, {1, 8}, TensorDataType::Int32},
        {"allowed_pred_num_tokens", TensorMode::Input, {1, 11}, TensorDataType::Int32},
        {"pred_global_motions", TensorMode::Output, {1, 64, 366}, TensorDataType::Float32},
        {"pred_num_tokens", TensorMode::Output, {1}, TensorDataType::Int32},
    };
    return expected;
}
}

int ValidFrameCount(int tokenCount)
{
    if (tokenCount < kMinTokenCount || tokenCount > kMaxTokenCount)
        throw std::invalid_argument("MotionBricks token count must be 6 to 16");
    return tokenCount * kPosesPerToken - 2 * kBoundaryFrames;
}

ModelContract ParseModelManifest(const std::filesystem::path& manifestPath)
{
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input)
        throw std::invalid_argument("Could not open MotionBricks manifest: " + manifestPath.string());

    json root;
    try
    {
        input >> root;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument("Invalid MotionBricks manifest " + manifestPath.string() +
            ": " + error.what());
    }
    if (!root.is_object())
        throw std::invalid_argument("MotionBricks manifest must be a JSON object: " +
            manifestPath.string());

    const std::string context = "manifest " + manifestPath.filename().string();
    if (RequireInteger(root, "schema_version", context) != 1)
        throw std::invalid_argument(context + " has an unsupported schema_version");

    struct TimingField
    {
        const char* key;
        double expected;
    };
    for (const TimingField field : {
        TimingField{"sample_rate", kSampleRate},
        TimingField{"frames_per_token", kPosesPerToken},
        TimingField{"context_frames", kContextPoses},
        TimingField{"target_capacity", kTargetCapacity},
        TimingField{"min_pred_tokens", kMinTokenCount},
        TimingField{"max_pred_tokens", kMaxTokenCount}})
    {
        const double actual = RequireNumber(root, field.key, context);
        if (std::abs(actual - field.expected) > 1e-6)
            throw std::invalid_argument(context + "." + field.key + " must be " +
                std::to_string(field.expected));
    }

    ModelContract contract;
    contract.name = RequireString(root, "name", context);
    contract.onnxFilename = RequireString(root, "onnx_filename", context);
    if (contract.name.empty())
        throw std::invalid_argument(context + ".name cannot be empty");

    const json& precision = RequireMember(root, "precision_variants", context);
    if (!precision.is_object())
        throw std::invalid_argument(context + ".precision_variants must be an object");
    const json& fp16 = RequireMember(precision, "fp16", context + ".precision_variants");
    if (!fp16.is_object())
        throw std::invalid_argument(context + ".precision_variants.fp16 must be an object");
    contract.fp16OnnxFilename = RequireString(fp16, "onnx_filename",
        context + ".precision_variants.fp16");

    const std::filesystem::path mainFilename(contract.onnxFilename);
    const std::filesystem::path fp16Filename(contract.fp16OnnxFilename);
    if (contract.onnxFilename.empty() || mainFilename.has_parent_path() ||
        mainFilename.filename() != mainFilename)
        throw std::invalid_argument(context + ".onnx_filename must be a file name");
    if (contract.fp16OnnxFilename.empty() || fp16Filename.has_parent_path() ||
        fp16Filename.filename() != fp16Filename)
        throw std::invalid_argument(
            context + ".precision_variants.fp16.onnx_filename must be a file name");

    const json& jointNames = RequireMember(root, "joint_names", context);
    if (!jointNames.is_array() || jointNames.empty())
        throw std::invalid_argument(context + ".joint_names must be a non-empty array");
    std::set<std::string> uniqueJoints;
    for (const json& joint : jointNames)
    {
        if (!joint.is_string() || joint.get<std::string>().empty())
            throw std::invalid_argument(context + ".joint_names must contain non-empty strings");
        if (!uniqueJoints.insert(joint.get<std::string>()).second)
            throw std::invalid_argument(context + " has duplicate joint name '" +
                joint.get<std::string>() + "'");
        contract.jointNames.push_back(joint.get<std::string>());
    }

    const json& tensorSpecs = RequireMember(root, "tensor_specs", context);
    if (!tensorSpecs.is_array() || tensorSpecs.empty())
        throw std::invalid_argument(context + ".tensor_specs must be a non-empty array");
    std::set<std::string> uniqueTensors;
    for (size_t index = 0; index < tensorSpecs.size(); ++index)
    {
        const json& entry = tensorSpecs[index];
        if (!entry.is_object())
            throw std::invalid_argument(context + ".tensor_specs entries must be objects");
        const std::string tensorContext = context + ".tensor_specs[" + std::to_string(index) + "]";

        TensorSpec spec;
        spec.name = RequireString(entry, "name", tensorContext);
        if (spec.name.empty())
            throw std::invalid_argument(tensorContext + ".name cannot be empty");
        if (!uniqueTensors.insert(spec.name).second)
            throw std::invalid_argument(context + " has duplicate tensor name '" + spec.name + "'");
        spec.mode = ParseTensorMode(RequireString(entry, "mode", tensorContext), tensorContext);
        spec.dtype = ParseTensorDataType(RequireString(entry, "dtype", tensorContext), tensorContext);

        const json& shape = RequireMember(entry, "shape", tensorContext);
        if (!shape.is_array() || shape.empty())
            throw std::invalid_argument(tensorContext + ".shape must be a non-empty array");
        for (const json& dimension : shape)
        {
            if (!dimension.is_number_integer() || dimension.get<int64_t>() <= 0)
                throw std::invalid_argument(tensorContext +
                    ".shape dimensions must be positive integers");
            spec.shape.push_back(dimension.get<int64_t>());
        }
        contract.tensorSpecs.push_back(std::move(spec));
    }

    return contract;
}

std::filesystem::path ResolveModelPath(const std::filesystem::path& manifestPath,
    const std::string& filename)
{
    const std::filesystem::path value(filename);
    if (filename.empty() || value.has_parent_path() || value.filename() != value)
        throw std::invalid_argument("MotionBricks model file name must not contain directories");
    return manifestPath.parent_path() / value;
}

void ValidateSomaContract(const ModelContract& contract)
{
    if (contract.jointNames.size() != 30)
        throw std::invalid_argument("SOMA MotionBricks contract must have 30 joints, got " +
            std::to_string(contract.jointNames.size()));

    const auto& expected = SomaTensorSurface();
    if (contract.tensorSpecs.size() != expected.size())
        throw std::invalid_argument("SOMA MotionBricks contract must have " +
            std::to_string(expected.size()) + " tensors, got " +
            std::to_string(contract.tensorSpecs.size()));

    for (const ExpectedTensor& expectedSpec : expected)
    {
        const TensorSpec* actual = nullptr;
        for (const TensorSpec& spec : contract.tensorSpecs)
            if (spec.name == expectedSpec.name)
            {
                actual = &spec;
                break;
            }
        if (!actual)
            throw std::invalid_argument("SOMA MotionBricks contract is missing tensor '" +
                std::string(expectedSpec.name) + "'");
        if (actual->mode != expectedSpec.mode)
            throw std::invalid_argument("SOMA tensor '" + actual->name + "' must be " +
                TensorModeName(expectedSpec.mode));
        if (actual->dtype != expectedSpec.dtype)
            throw std::invalid_argument("SOMA tensor '" + actual->name + "' must be " +
                TensorDataTypeName(expectedSpec.dtype));
        if (actual->shape != expectedSpec.shape)
            throw std::invalid_argument("SOMA tensor '" + actual->name + "' must have shape " +
                ShapeText(expectedSpec.shape) + ", got " + ShapeText(actual->shape));
    }
}
}
