#include "animation/motionBricks/motionBricksContract.h"
#include "animation/motionBricks/onnxRuntimeApi.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace
{
using namespace VL::Animation::MotionBricks;
using json = nlohmann::json;

std::filesystem::path FixturePath(const char* name)
{
    return std::filesystem::path(VL_MOTIONBRICKS_FIXTURE_DIR) / name;
}

json LoadJson(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open fixture " + path.string());
    json value;
    input >> value;
    return value;
}

void FlattenFloats(const json& value, std::vector<float>& output)
{
    if (value.is_number())
    {
        output.push_back(value.get<float>());
        return;
    }
    for (const json& child : value)
        FlattenFloats(child, output);
}

void FlattenInts(const json& value, std::vector<int32_t>& output)
{
    if (value.is_number_integer())
    {
        output.push_back(value.get<int32_t>());
        return;
    }
    for (const json& child : value)
        FlattenInts(child, output);
}

OnnxTensor FloatTensor(std::string name, std::vector<int64_t> shape, const json& values)
{
    OnnxTensor tensor;
    tensor.name = std::move(name);
    tensor.dtype = TensorDataType::Float32;
    tensor.shape = std::move(shape);
    FlattenFloats(values, tensor.floats);
    EXPECT_EQ(tensor.floats.size(), tensor.ElementCount()) << tensor.name;
    return tensor;
}

OnnxTensor IntTensor(std::string name, std::vector<int64_t> shape, const json& values)
{
    OnnxTensor tensor;
    tensor.name = std::move(name);
    tensor.dtype = TensorDataType::Int32;
    tensor.shape = std::move(shape);
    FlattenInts(values, tensor.ints);
    EXPECT_EQ(tensor.ints.size(), tensor.ElementCount()) << tensor.name;
    return tensor;
}

std::filesystem::path ModelPathFromEnvironment()
{
    const char* value = std::getenv("VULKANLEARN_MOTIONBRICKS_SOMA_ONNX");
    if (!value || !*value) return {};
    return std::filesystem::u8path(value);
}

TEST(MotionBricksOnnx, LoadsSomaModelAndMatchesOfficialInference)
{
    const OnnxRuntimeStatus runtime = ProbeOnnxRuntime();
    if (!runtime.available)
        GTEST_SKIP() << runtime.error;
    EXPECT_FALSE(runtime.version.empty());

    const std::filesystem::path modelPath = ModelPathFromEnvironment();
    if (modelPath.empty())
        GTEST_SKIP() << "Set VULKANLEARN_MOTIONBRICKS_SOMA_ONNX to mmm_v6_soma_fp32.onnx";
    if (!std::filesystem::is_regular_file(modelPath))
        GTEST_SKIP() << "SOMA ONNX model is missing: " << modelPath.string();

    const ModelContract contract = ParseModelManifest(FixturePath("soma_n1.json"));
    ValidateSomaContract(contract);
    OnnxSession session(modelPath, &contract);

    const json fixture = LoadJson(FixturePath("soma_golden.json"));
    const json& encoded = fixture["encoded"];
    std::vector<OnnxTensor> inputs;
    inputs.push_back(FloatTensor("global_root_values", {1, 8, 5},
        encoded["global_root_values"]));
    inputs.push_back(IntTensor("has_global_root_values", {1, 8},
        encoded["has_global_root_values"]));
    inputs.push_back(FloatTensor("local_root_values", {1, 8, 4},
        encoded["local_root_values"]));
    inputs.push_back(IntTensor("has_local_root_values", {1, 8},
        encoded["has_local_root_values"]));
    inputs.push_back(FloatTensor("local_poses", {1, 8, 267},
        encoded["local_poses"]));
    inputs.push_back(IntTensor("has_local_poses", {1, 8},
        encoded["has_local_poses"]));
    inputs.push_back(IntTensor("allowed_pred_num_tokens", {1, 11},
        encoded["allowed_pred_num_tokens"]));

    std::vector<OnnxTensor> outputs;
    session.Run(inputs, outputs);
    ASSERT_EQ(outputs.size(), 2u);

    const OnnxTensor* motions = nullptr;
    const OnnxTensor* tokens = nullptr;
    for (const OnnxTensor& tensor : outputs)
    {
        if (tensor.name == "pred_global_motions") motions = &tensor;
        if (tensor.name == "pred_num_tokens") tokens = &tensor;
    }
    ASSERT_TRUE(motions);
    ASSERT_TRUE(tokens);
    EXPECT_EQ(motions->shape, (std::vector<int64_t>{1, 64, 366}));
    EXPECT_EQ(tokens->shape, (std::vector<int64_t>{1}));
    ASSERT_EQ(tokens->ints.size(), 1u);

    const json inference = LoadJson(FixturePath("soma_inference_golden.json"));
    EXPECT_EQ(tokens->ints[0], inference["token_count"].get<int32_t>());
    ASSERT_EQ(motions->floats.size(), 64u * 366u);
    std::vector<float> expected;
    FlattenFloats(inference["pred_global_motions"], expected);
    ASSERT_EQ(expected.size(), motions->floats.size());

    float maximumDifference = 0;
    size_t worstIndex = 0;
    for (size_t index = 0; index < expected.size(); ++index)
    {
        ASSERT_TRUE(std::isfinite(motions->floats[index])) << "index " << index;
        const float difference = std::abs(motions->floats[index] - expected[index]);
        if (difference > maximumDifference)
        {
            maximumDifference = difference;
            worstIndex = index;
        }
    }
    EXPECT_LT(maximumDifference, 1e-3f)
        << "worst index " << worstIndex << " expected " << expected[worstIndex] <<
        " actual " << motions->floats[worstIndex];
}

TEST(MotionBricksOnnx, RejectsMissingModel)
{
    const OnnxRuntimeStatus runtime = ProbeOnnxRuntime();
    if (!runtime.available)
        GTEST_SKIP() << runtime.error;
    EXPECT_THROW(OnnxSession(std::filesystem::path("missing.onnx")),
        std::invalid_argument);
}
}
