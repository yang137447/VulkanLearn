#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace VL::Animation::MotionBricks
{
// MotionBricks v0.2.0 shared timing. The manifest repeats these values, so a model
// exported with different timing is rejected instead of silently retimed.
inline constexpr float kSampleRate = 30.0f;
inline constexpr int kPosesPerToken = 4;
inline constexpr int kContextPoses = 4;
inline constexpr int kTargetCapacity = 4;
inline constexpr int kContextAndTargetPoses = 8;
inline constexpr int kMinTokenCount = 6;
inline constexpr int kMaxTokenCount = 16;
inline constexpr int kBoundaryFrames = kPosesPerToken - 1;
inline constexpr int kMaxValidFrames = kMaxTokenCount * kPosesPerToken - 2 * kBoundaryFrames;

enum class TensorMode
{
    Input,
    Output
};

enum class TensorDataType
{
    Float32,
    Int32
};

struct TensorSpec
{
    std::string name;
    TensorMode mode = TensorMode::Input;
    std::vector<int64_t> shape;
    TensorDataType dtype = TensorDataType::Float32;
};

struct ModelContract
{
    std::string name;
    std::string onnxFilename;
    std::string fp16OnnxFilename;
    std::vector<std::string> jointNames;
    std::vector<TensorSpec> tensorSpecs;
};

// tokenCount is the model-selected count, 6..16. The result is 18..58.
int ValidFrameCount(int tokenCount);

// Reads schema_version 1 manifests shipped next to the ONNX files. Invalid JSON,
// timing, names, shapes, modes or dtypes throw std::invalid_argument.
ModelContract ParseModelManifest(const std::filesystem::path& manifestPath);

// Returns manifestPath.parent_path() / filename. A filename with directory
// components is rejected.
std::filesystem::path ResolveModelPath(const std::filesystem::path& manifestPath,
    const std::string& filename);

// Checks the exact SOMA v0.2.0 tensor surface: 30 joints, the 7 inputs and 2 outputs
// with their fixed shapes and dtypes. This is the guard for all SOMA math below.
void ValidateSomaContract(const ModelContract& contract);
}
