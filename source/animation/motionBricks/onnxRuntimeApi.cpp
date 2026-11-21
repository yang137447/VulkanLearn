#include "onnxRuntimeApi.h"

#include "onnxruntime_c_api.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace VL::Animation::MotionBricks
{
namespace
{
struct RuntimeState
{
    void* module = nullptr;
    std::filesystem::path modulePath;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtAllocator* allocator = nullptr;
    OrtMemoryInfo* memoryInfo = nullptr;
    std::string version;
    std::string error;

    bool Ready() const
    {
        return api != nullptr && env != nullptr && allocator != nullptr &&
            memoryInfo != nullptr && error.empty();
    }
};

std::string TakeErrorMessage(const OrtApi* api, OrtStatus* status)
{
    std::string message = api->GetErrorMessage(status) ? api->GetErrorMessage(status) :
        "unknown ONNX Runtime error";
    api->ReleaseStatus(status);
    return message;
}

void CheckStatus(const OrtApi* api, OrtStatus* status, const char* operation)
{
    if (!status) return;
    const std::string message = TakeErrorMessage(api, status);
    throw std::runtime_error(std::string(operation) + ": " + message);
}

void InitializeRuntime(RuntimeState& state)
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    const char* envPath = std::getenv("VULKANLEARN_ONNXRUNTIME_DLL");
    if (envPath && *envPath)
    {
        const std::filesystem::path path = std::filesystem::u8path(envPath);
        module = LoadLibraryW(path.c_str());
        if (!module)
        {
            state.error = "Could not load VULKANLEARN_ONNXRUNTIME_DLL=" + path.string() +
                " (Windows error " + std::to_string(GetLastError()) + ")";
            return;
        }
    }
    else
    {
        module = LoadLibraryW(L"onnxruntime.dll");
        if (!module)
        {
            state.error = "onnxruntime.dll was not found on the Windows loader search path "
                "(set VULKANLEARN_ONNXRUNTIME_DLL to its full path)";
            return;
        }
    }
    state.module = module;
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(module, buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (length == 0) break;
            if (length < buffer.size())
            {
                buffer.resize(length);
                state.modulePath = std::filesystem::path(buffer);
                break;
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    const auto getApiBase = reinterpret_cast<GetApiBase>(
        GetProcAddress(module, "OrtGetApiBase"));
    if (!getApiBase)
    {
        state.error = "onnxruntime.dll does not export OrtGetApiBase";
        return;
    }
    const OrtApiBase* apiBase = getApiBase();
    if (!apiBase)
    {
        state.error = "OrtGetApiBase returned null";
        return;
    }
    state.version = apiBase->GetVersionString() ? apiBase->GetVersionString() : "";
#else
    const char* envPath = std::getenv("VULKANLEARN_ONNXRUNTIME_DLL");
    void* module = dlopen(envPath && *envPath ? envPath : "libonnxruntime.so",
        RTLD_NOW | RTLD_LOCAL);
    if (!module)
    {
        const char* loadError = dlerror();
        state.error = std::string("Could not load libonnxruntime.so: ") +
            (loadError ? loadError : "unknown error");
        return;
    }
    state.module = module;
    if (envPath && *envPath) state.modulePath = std::filesystem::u8path(envPath);

    using GetApiBase = const OrtApiBase*(*)();
    const auto getApiBase = reinterpret_cast<GetApiBase>(dlsym(module, "OrtGetApiBase"));
    if (!getApiBase)
    {
        state.error = "libonnxruntime.so does not export OrtGetApiBase";
        return;
    }
    const OrtApiBase* apiBase = getApiBase();
    if (!apiBase)
    {
        state.error = "OrtGetApiBase returned null";
        return;
    }
    state.version = apiBase->GetVersionString() ? apiBase->GetVersionString() : "";
#endif

    state.api = apiBase->GetApi(ORT_API_VERSION);
    if (!state.api)
    {
        state.error = "ONNX Runtime does not provide C API version " +
            std::to_string(ORT_API_VERSION);
        return;
    }

    OrtStatus* status = state.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING,
        "VulkanLearnMotionBricks", &state.env);
    if (status)
    {
        state.error = "CreateEnv: " + TakeErrorMessage(state.api, status);
        return;
    }
    status = state.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
        &state.memoryInfo);
    if (status)
    {
        state.error = "CreateCpuMemoryInfo: " + TakeErrorMessage(state.api, status);
        return;
    }
    status = state.api->GetAllocatorWithDefaultOptions(&state.allocator);
    if (status)
        state.error = "GetAllocatorWithDefaultOptions: " +
            TakeErrorMessage(state.api, status);
}

RuntimeState& Runtime()
{
    static RuntimeState state;
    static std::once_flag once;
    std::call_once(once, [] { InitializeRuntime(state); });
    return state;
}

using AppendDmlProvider = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);

bool HasDirectMlProvider(const RuntimeState& runtime)
{
#if defined(_WIN32)
    return GetProcAddress(static_cast<HMODULE>(runtime.module),
        "OrtSessionOptionsAppendExecutionProvider_DML") != nullptr;
#else
    return dlsym(runtime.module,
        "OrtSessionOptionsAppendExecutionProvider_DML") != nullptr;
#endif
}

bool HasCudaProvider(const RuntimeState& runtime)
{
    if (runtime.modulePath.empty()) return true;
    const std::filesystem::path directory = runtime.modulePath.parent_path();
#if defined(_WIN32)
    return std::filesystem::exists(directory / "onnxruntime_providers_cuda.dll");
#else
    return std::filesystem::exists(directory / "libonnxruntime_providers_cuda.so");
#endif
}

bool TryAppendDirectMl(const RuntimeState& runtime, const OrtApi* api,
    OrtSessionOptions* options, int deviceId, std::string& error)
{
#if defined(_WIN32)
    const auto append = reinterpret_cast<AppendDmlProvider>(
        GetProcAddress(static_cast<HMODULE>(runtime.module),
            "OrtSessionOptionsAppendExecutionProvider_DML"));
    if (!append)
    {
        error = "onnxruntime.dll does not export DirectML";
        return false;
    }
#else
    const auto append = reinterpret_cast<AppendDmlProvider>(
        dlsym(runtime.module, "OrtSessionOptionsAppendExecutionProvider_DML"));
    if (!append)
    {
        error = "libonnxruntime.so does not export DirectML";
        return false;
    }
#endif
    OrtStatus* status = append(options, deviceId);
    if (!status) return true;
    error = TakeErrorMessage(api, status);
    return false;
}

bool TryAppendCuda(const OrtApi* api, OrtSessionOptions* options, int deviceId,
    std::string& error)
{
    OrtCUDAProviderOptions cuda{};
    cuda.device_id = deviceId;
    OrtStatus* status = api->SessionOptionsAppendExecutionProvider_CUDA(options, &cuda);
    if (!status) return true;
    error = TakeErrorMessage(api, status);
    return false;
}

struct TypeInfoDeleter
{
    const OrtApi* api = nullptr;
    void operator()(OrtTypeInfo* value) const
    {
        if (value && api) api->ReleaseTypeInfo(value);
    }
};

struct SessionDeleter
{
    const OrtApi* api = nullptr;
    void operator()(OrtSession* value) const
    {
        if (value && api) api->ReleaseSession(value);
    }
};

struct OptionsDeleter
{
    const OrtApi* api = nullptr;
    void operator()(OrtSessionOptions* value) const
    {
        if (value && api) api->ReleaseSessionOptions(value);
    }
};

struct ValueDeleter
{
    const OrtApi* api = nullptr;
    void operator()(OrtValue* value) const
    {
        if (value && api) api->ReleaseValue(value);
    }
};

using ValuePtr = std::unique_ptr<OrtValue, ValueDeleter>;

TensorDataType ToTensorDataType(ONNXTensorElementDataType type, const std::string& name)
{
    switch (type)
    {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
        return TensorDataType::Float32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
        return TensorDataType::Int32;
    default:
        throw std::runtime_error("ONNX tensor '" + name + "' uses unsupported element type " +
            std::to_string(static_cast<int>(type)));
    }
}

ONNXTensorElementDataType ToOrtType(TensorDataType dtype)
{
    return dtype == TensorDataType::Float32 ? ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT :
        ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
}

std::vector<TensorSpec> ReadTensorSpecs(const OrtApi* api, OrtSession* session,
    OrtAllocator* allocator, bool inputs)
{
    size_t count = 0;
    CheckStatus(api, inputs ? api->SessionGetInputCount(session, &count) :
        api->SessionGetOutputCount(session, &count), "SessionGetTensorCount");

    std::vector<TensorSpec> specs;
    specs.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        char* rawName = nullptr;
        CheckStatus(api, inputs ? api->SessionGetInputName(session, index, allocator, &rawName) :
            api->SessionGetOutputName(session, index, allocator, &rawName),
            "SessionGetTensorName");
        std::string name = rawName ? rawName : "";
        if (rawName) allocator->Free(allocator, rawName);

        OrtTypeInfo* rawTypeInfo = nullptr;
        CheckStatus(api, inputs ? api->SessionGetInputTypeInfo(session, index, &rawTypeInfo) :
            api->SessionGetOutputTypeInfo(session, index, &rawTypeInfo),
            "SessionGetTensorTypeInfo");
        std::unique_ptr<OrtTypeInfo, TypeInfoDeleter> typeInfo(rawTypeInfo,
            TypeInfoDeleter{api});
        const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
        CheckStatus(api, api->CastTypeInfoToTensorInfo(typeInfo.get(), &tensorInfo),
            "CastTypeInfoToTensorInfo");
        if (!tensorInfo)
            throw std::runtime_error("ONNX tensor '" + name + "' is not a dense tensor");

        ONNXTensorElementDataType elementType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        CheckStatus(api, api->GetTensorElementType(tensorInfo, &elementType),
            "GetTensorElementType");
        size_t dimensionCount = 0;
        CheckStatus(api, api->GetDimensionsCount(tensorInfo, &dimensionCount),
            "GetDimensionsCount");
        std::vector<int64_t> shape(dimensionCount);
        if (dimensionCount != 0)
            CheckStatus(api, api->GetDimensions(tensorInfo, shape.data(), dimensionCount),
                "GetDimensions");

        TensorSpec spec;
        spec.name = std::move(name);
        spec.mode = inputs ? TensorMode::Input : TensorMode::Output;
        spec.shape = std::move(shape);
        spec.dtype = ToTensorDataType(elementType, spec.name);
        specs.push_back(std::move(spec));
    }
    return specs;
}

std::vector<int64_t> ConcreteShape(const std::vector<int64_t>& shape,
    const std::string& name)
{
    if (shape.empty())
        throw std::invalid_argument("ONNX tensor '" + name + "' has no shape");
    std::vector<int64_t> concrete = shape;
    for (int64_t& dimension : concrete)
    {
        if (dimension < 0)
            throw std::invalid_argument("ONNX tensor '" + name +
                "' has a symbolic output shape; construct OnnxSession with a ModelContract");
        if (dimension == 0)
            throw std::invalid_argument("ONNX tensor '" + name + "' has a zero dimension");
    }
    return concrete;
}

void ValidateTensor(const OnnxTensor& tensor, const TensorSpec& spec, const char* kind)
{
    if (tensor.name != spec.name)
        throw std::invalid_argument(std::string("ONNX ") + kind + " name '" + tensor.name +
            "' does not match '" + spec.name + "'");
    if (tensor.dtype != spec.dtype)
        throw std::invalid_argument("ONNX tensor '" + tensor.name + "' has the wrong dtype");
    if (tensor.shape.size() != spec.shape.size())
        throw std::invalid_argument("ONNX tensor '" + tensor.name + "' has the wrong rank");
    for (size_t index = 0; index < spec.shape.size(); ++index)
    {
        if (spec.shape[index] >= 0 && tensor.shape[index] != spec.shape[index])
            throw std::invalid_argument("ONNX tensor '" + tensor.name +
                "' has the wrong shape");
        if (tensor.shape[index] <= 0)
            throw std::invalid_argument("ONNX tensor '" + tensor.name +
                "' needs a concrete positive shape");
    }

    const size_t elements = tensor.ElementCount();
    if (tensor.dtype == TensorDataType::Float32)
    {
        if (tensor.floats.size() != elements || !tensor.ints.empty())
            throw std::invalid_argument("ONNX tensor '" + tensor.name +
                "' float payload size does not match its shape");
    }
    else if (tensor.ints.size() != elements || !tensor.floats.empty())
    {
        throw std::invalid_argument("ONNX tensor '" + tensor.name +
            "' int32 payload size does not match its shape");
    }
}
}

size_t OnnxTensor::ElementCount() const
{
    if (shape.empty())
        throw std::invalid_argument("ONNX tensor '" + name + "' has no shape");
    size_t count = 1;
    for (int64_t dimension : shape)
    {
        if (dimension <= 0)
            throw std::invalid_argument("ONNX tensor '" + name +
                "' needs positive concrete dimensions");
        count *= static_cast<size_t>(dimension);
    }
    return count;
}

size_t OnnxTensor::ByteSize() const
{
    return ElementCount() * 4;
}

OnnxRuntimeStatus ProbeOnnxRuntime()
{
    const RuntimeState& state = Runtime();
    OnnxRuntimeStatus status;
    status.available = state.Ready();
    status.version = state.version;
    status.error = state.error;
    return status;
}

struct OnnxSession::Impl
{
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtAllocator* allocator = nullptr;
    const OrtMemoryInfo* memoryInfo = nullptr;
    OrtSession* session = nullptr;
    std::vector<TensorSpec> inputs;
    std::vector<TensorSpec> outputs;
    std::vector<TensorSpec> contractSpecs;
    std::string providerName;

    ~Impl()
    {
        if (session && api) api->ReleaseSession(session);
    }
};

OnnxSession::OnnxSession(const std::filesystem::path& modelPath,
    const ModelContract* contract, const OnnxSessionOptions& options)
{
    RuntimeState& runtime = Runtime();
    if (!runtime.Ready())
        throw std::runtime_error(runtime.error.empty() ?
            "ONNX Runtime is unavailable" : runtime.error);
    if (!std::filesystem::is_regular_file(modelPath))
        throw std::invalid_argument("ONNX model file not found: " + modelPath.string());

    impl_ = std::make_unique<Impl>();
    impl_->api = runtime.api;
    impl_->env = runtime.env;
    impl_->allocator = runtime.allocator;
    impl_->memoryInfo = runtime.memoryInfo;

    std::vector<OnnxExecutionProvider> attempts;
    switch (options.provider)
    {
    case OnnxExecutionProvider::Auto:
        // DirectML is a separate onnxruntime build and only exists when the DLL
        // exports it. CUDA lives in a sibling provider DLL; probing the file
        // avoids asking ORT to load a provider that is not installed.
        if (HasDirectMlProvider(runtime))
            attempts.push_back(OnnxExecutionProvider::DirectML);
        if (HasCudaProvider(runtime))
            attempts.push_back(OnnxExecutionProvider::Cuda);
        attempts.push_back(OnnxExecutionProvider::Cpu);
        break;
    case OnnxExecutionProvider::Cpu:
        attempts = {OnnxExecutionProvider::Cpu};
        break;
    case OnnxExecutionProvider::DirectML:
        attempts = {OnnxExecutionProvider::DirectML};
        break;
    case OnnxExecutionProvider::Cuda:
        attempts = {OnnxExecutionProvider::Cuda};
        break;
    }

    std::string lastError = "No ONNX execution provider was attempted";
    for (const OnnxExecutionProvider provider : attempts)
    {
        OrtSessionOptions* rawOptions = nullptr;
        std::unique_ptr<OrtSessionOptions, OptionsDeleter> sessionOptions(nullptr,
            OptionsDeleter{impl_->api});
        std::unique_ptr<OrtSession, SessionDeleter> session(nullptr,
            SessionDeleter{impl_->api});
        std::string providerName;
        try
        {
            CheckStatus(impl_->api, impl_->api->CreateSessionOptions(&rawOptions),
                "CreateSessionOptions");
            sessionOptions.reset(rawOptions);
            CheckStatus(impl_->api,
                impl_->api->SetSessionGraphOptimizationLevel(sessionOptions.get(),
                    ORT_ENABLE_ALL),
                "SetSessionGraphOptimizationLevel");

            if (provider == OnnxExecutionProvider::DirectML)
            {
                std::string error;
                if (!TryAppendDirectMl(runtime, impl_->api, sessionOptions.get(),
                    options.deviceId, error))
                    throw std::runtime_error("DirectML: " + error);
                providerName = "DirectML";
            }
            else if (provider == OnnxExecutionProvider::Cuda)
            {
                std::string error;
                if (!TryAppendCuda(impl_->api, sessionOptions.get(), options.deviceId,
                    error))
                    throw std::runtime_error("CUDA: " + error);
                providerName = "CUDA";
            }
            else
            {
                providerName = "CPU";
            }

            OrtSession* rawSession = nullptr;
            CheckStatus(impl_->api,
                impl_->api->CreateSession(impl_->env, modelPath.c_str(),
                    sessionOptions.get(), &rawSession),
                "CreateSession");
            session.reset(rawSession);
            auto inputSpecs = ReadTensorSpecs(impl_->api, session.get(),
                impl_->allocator, true);
            auto outputSpecs = ReadTensorSpecs(impl_->api, session.get(),
                impl_->allocator, false);
            impl_->session = session.release();
            impl_->inputs = std::move(inputSpecs);
            impl_->outputs = std::move(outputSpecs);
            impl_->providerName = std::move(providerName);
            break;
        }
        catch (const std::exception& exception)
        {
            lastError = exception.what();
            if (options.provider != OnnxExecutionProvider::Auto)
                throw;
        }
    }
    if (!impl_->session)
        throw std::runtime_error("Could not create ONNX session (" + lastError + ")");
    if (contract)
    {
        impl_->contractSpecs = contract->tensorSpecs;
        ValidateSessionAgainstContract(*this, *contract);
    }
}

OnnxSession::~OnnxSession() = default;
OnnxSession::OnnxSession(OnnxSession&&) noexcept = default;
OnnxSession& OnnxSession::operator=(OnnxSession&&) noexcept = default;

const std::vector<TensorSpec>& OnnxSession::Inputs() const
{
    if (!impl_) throw std::logic_error("ONNX session was moved from");
    return impl_->inputs;
}

const std::vector<TensorSpec>& OnnxSession::Outputs() const
{
    if (!impl_) throw std::logic_error("ONNX session was moved from");
    return impl_->outputs;
}

const std::string& OnnxSession::ProviderName() const
{
    if (!impl_) throw std::logic_error("ONNX session was moved from");
    return impl_->providerName;
}

void OnnxSession::Run(const std::vector<OnnxTensor>& inputs,
    std::vector<OnnxTensor>& outputs) const
{
    if (!impl_) throw std::logic_error("ONNX session was moved from");
    if (inputs.size() != impl_->inputs.size())
        throw std::invalid_argument("ONNX input count does not match the session");

    std::vector<const char*> inputNames;
    inputNames.reserve(inputs.size());
    std::vector<const OrtValue*> inputValues;
    inputValues.reserve(inputs.size());
    std::vector<ValuePtr> inputGuards;
    inputGuards.reserve(inputs.size());
    for (size_t index = 0; index < inputs.size(); ++index)
    {
        const OnnxTensor& tensor = inputs[index];
        ValidateTensor(tensor, impl_->inputs[index], "input");
        void* data = tensor.dtype == TensorDataType::Float32 ?
            const_cast<void*>(static_cast<const void*>(tensor.floats.data())) :
            const_cast<void*>(static_cast<const void*>(tensor.ints.data()));
        OrtValue* value = nullptr;
        CheckStatus(impl_->api,
            impl_->api->CreateTensorWithDataAsOrtValue(impl_->memoryInfo, data,
                tensor.ByteSize(), tensor.shape.data(), tensor.shape.size(),
                ToOrtType(tensor.dtype), &value),
            "CreateTensorWithDataAsOrtValue(input)");
        inputGuards.emplace_back(value, ValueDeleter{impl_->api});
        inputNames.push_back(tensor.name.c_str());
        inputValues.push_back(value);
    }

    outputs.resize(impl_->outputs.size());
    std::vector<const char*> outputNames;
    outputNames.reserve(outputs.size());
    std::vector<OrtValue*> outputValues;
    outputValues.reserve(outputs.size());
    std::vector<ValuePtr> outputGuards;
    outputGuards.reserve(outputs.size());
    for (size_t index = 0; index < outputs.size(); ++index)
    {
        const TensorSpec& sessionSpec = impl_->outputs[index];
        const TensorSpec* contractSpec = nullptr;
        for (const TensorSpec& spec : impl_->contractSpecs)
            if (spec.name == sessionSpec.name && spec.mode == TensorMode::Output)
            {
                contractSpec = &spec;
                break;
            }
        const TensorSpec& spec = contractSpec ? *contractSpec : sessionSpec;
        OnnxTensor& tensor = outputs[index];
        tensor.name = spec.name;
        tensor.dtype = spec.dtype;
        tensor.shape = ConcreteShape(spec.shape, spec.name);
        const size_t elements = tensor.ElementCount();
        void* data = nullptr;
        if (tensor.dtype == TensorDataType::Float32)
        {
            tensor.floats.assign(elements, 0.0f);
            tensor.ints.clear();
            data = tensor.floats.data();
        }
        else
        {
            tensor.ints.assign(elements, 0);
            tensor.floats.clear();
            data = tensor.ints.data();
        }
        OrtValue* value = nullptr;
        CheckStatus(impl_->api,
            impl_->api->CreateTensorWithDataAsOrtValue(impl_->memoryInfo, data,
                tensor.ByteSize(), tensor.shape.data(), tensor.shape.size(),
                ToOrtType(tensor.dtype), &value),
            "CreateTensorWithDataAsOrtValue(output)");
        outputGuards.emplace_back(value, ValueDeleter{impl_->api});
        outputNames.push_back(tensor.name.c_str());
        outputValues.push_back(value);
    }

    CheckStatus(impl_->api,
        impl_->api->Run(impl_->session, nullptr, inputNames.data(), inputValues.data(),
            inputNames.size(), outputNames.data(), outputNames.size(), outputValues.data()),
        "Run");
}

void ValidateSessionAgainstContract(const OnnxSession& session, const ModelContract& contract)
{
    auto compare = [](const std::vector<TensorSpec>& actual,
        const std::vector<TensorSpec>& expected, const char* kind)
    {
        if (actual.size() != expected.size())
            throw std::runtime_error(std::string("ONNX session has the wrong ") + kind +
                " count");
        for (const TensorSpec& expectedSpec : expected)
        {
            const TensorSpec* found = nullptr;
            for (const TensorSpec& spec : actual)
                if (spec.name == expectedSpec.name)
                {
                    found = &spec;
                    break;
                }
            if (!found)
                throw std::runtime_error("ONNX session is missing " + std::string(kind) +
                    " '" + expectedSpec.name + "'");
            if (found->dtype != expectedSpec.dtype)
                throw std::runtime_error("ONNX session " + std::string(kind) + " '" +
                    expectedSpec.name + "' has the wrong dtype");
            if (found->shape.size() != expectedSpec.shape.size())
                throw std::runtime_error("ONNX session " + std::string(kind) + " '" +
                    expectedSpec.name + "' has the wrong rank");
            for (size_t index = 0; index < expectedSpec.shape.size(); ++index)
                if (found->shape[index] >= 0 &&
                    found->shape[index] != expectedSpec.shape[index])
                    throw std::runtime_error("ONNX session " + std::string(kind) + " '" +
                        expectedSpec.name + "' has the wrong shape");
        }
    };
    std::vector<TensorSpec> expectedInputs;
    std::vector<TensorSpec> expectedOutputs;
    for (const TensorSpec& spec : contract.tensorSpecs)
        (spec.mode == TensorMode::Input ? expectedInputs : expectedOutputs).push_back(spec);
    compare(session.Inputs(), expectedInputs, "input");
    compare(session.Outputs(), expectedOutputs, "output");
}
}
