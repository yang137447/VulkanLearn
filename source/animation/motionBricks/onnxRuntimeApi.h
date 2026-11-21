#pragma once

#include "motionBricksContract.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace VL::Animation::MotionBricks
{
// A fixed-shape tensor. Both supported element types are four bytes, so ByteSize()
// is ElementCount() * 4. Inputs fill the vector matching dtype; Run fills outputs.
struct OnnxTensor
{
    std::string name;
    TensorDataType dtype = TensorDataType::Float32;
    std::vector<int64_t> shape;
    std::vector<float> floats;
    std::vector<int32_t> ints;

    size_t ElementCount() const;
    size_t ByteSize() const;
};

struct OnnxRuntimeStatus
{
    bool available = false;
    std::string version;
    std::string error;
};

enum class OnnxExecutionProvider
{
    Auto,
    Cpu,
    DirectML,
    Cuda
};

struct OnnxSessionOptions
{
    OnnxExecutionProvider provider = OnnxExecutionProvider::Auto;
    int deviceId = 0;
};

// Loads onnxruntime.dll lazily and reports why it is unavailable. It never throws.
OnnxRuntimeStatus ProbeOnnxRuntime();

// Loads one ONNX model and owns the OrtEnv's session. The runtime DLL is resolved
// from VULKANLEARN_ONNXRUNTIME_DLL, then the normal Windows loader search path.
// The class is move-only; Run() may be called from a worker thread.
class OnnxSession
{
public:
    // contract is optional. When supplied, Run() allocates outputs from its concrete
    // shapes (the SOMA outputs use symbolic dimensions in the ONNX graph) and the
    // session surface is validated against it.
    explicit OnnxSession(const std::filesystem::path& modelPath,
        const ModelContract* contract = nullptr,
        const OnnxSessionOptions& options = {});
    ~OnnxSession();
    OnnxSession(const OnnxSession&) = delete;
    OnnxSession& operator=(const OnnxSession&) = delete;
    OnnxSession(OnnxSession&&) noexcept;
    OnnxSession& operator=(OnnxSession&&) noexcept;

    const std::vector<TensorSpec>& Inputs() const;
    const std::vector<TensorSpec>& Outputs() const;
    // Actual provider selected after Auto fallback ("DirectML", "CUDA", or "CPU").
    const std::string& ProviderName() const;

    // Input and output names, concrete shapes and dtypes must match the session
    // contract. Output vectors are allocated from the declared output shapes.
    void Run(const std::vector<OnnxTensor>& inputs, std::vector<OnnxTensor>& outputs) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Compares the session's dynamic-shape metadata with the manifest. Negative
// dimensions in the session are treated as wildcards (the SOMA batch axis); every
// positive dimension must match. Missing, extra, or differently typed tensors throw.
void ValidateSessionAgainstContract(const OnnxSession& session, const ModelContract& contract);
}
