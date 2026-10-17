#include "script/scriptHost.h"

#include <stdexcept>
#include <vector>
#include <chrono>

#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>
#endif

namespace VL
{
std::filesystem::path ScriptHost::DefaultRuntimeDirectory()
{
#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
    std::vector<wchar_t> executable(32768);
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!size || size >= executable.size()) throw std::runtime_error("Script.ExecutablePath: cannot resolve executable");
    const auto directory = std::filesystem::path(std::wstring(executable.data(), size)).parent_path() / "managed";
    if (std::filesystem::is_directory(directory)) return directory;
    return VL_SCRIPT_OUTPUT_DIRECTORY;
#else
    return {};
#endif
}

void ScriptHost::Initialize(const std::filesystem::path& runtimeDirectory)
{
#ifdef VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS
    // hostfxr 库与 CoreCLR 为进程级 owner；关闭上下文不是卸载 CLR，不释放其代码页。
    // CoreCLR 的 TPA 搜索路径要求 Windows 原生分隔符，不能传播 CMake 的正斜杠路径。
    const auto directory = std::filesystem::absolute(runtimeDirectory).make_preferred();
    const auto hostPath = directory / "hostfxr.dll";
    HMODULE library = GetModuleHandleW(hostPath.c_str());
    if (!library) library = LoadLibraryW(hostPath.c_str());
    if (!library) throw std::runtime_error("Script.HostMissing: cannot load " + hostPath.string());
    const auto initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(GetProcAddress(library, "hostfxr_initialize_for_runtime_config"));
    const auto getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(GetProcAddress(library, "hostfxr_get_runtime_delegate"));
    const auto close = reinterpret_cast<hostfxr_close_fn>(GetProcAddress(library, "hostfxr_close"));
    if (!initialize || !getDelegate || !close) throw std::runtime_error("Script.HostInvalid: missing hostfxr exports");
    void* context = nullptr;
    const auto config = directory / "VulkanLearn.Runtime.runtimeconfig.json";
    // 搬移后的私有运行时优先；开发输出没有 dotnet 子目录时才使用配置期 SDK。
    const auto privateRuntime = directory / "dotnet";
    auto dotnetRoot = std::filesystem::is_directory(privateRuntime) ? privateRuntime :
        std::filesystem::path(VL_DOTNET_ROOT);
    dotnetRoot.make_preferred();
    const hostfxr_initialize_parameters parameters{sizeof(hostfxr_initialize_parameters), nullptr, dotnetRoot.c_str()};
    const int32_t status = initialize(config.c_str(), &parameters, &context);
    if (status < 0 || !context)
        throw std::runtime_error("Script.RuntimeMissing: hostfxr initialization failed, status=" + std::to_string(status));
    void* loader = nullptr;
    const auto delegateStatus = getDelegate(context, hdt_load_assembly_and_get_function_pointer, &loader);
    close(context);
    if (delegateStatus < 0 || !loader) throw std::runtime_error("Script.HostDelegate: cannot obtain assembly loader");
    void* entry = nullptr;
    const auto assembly = directory / "VulkanLearn.Runtime.dll";
    const int32_t loadStatus = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(loader)(
        assembly.c_str(), L"VulkanLearn.NativeBridge, VulkanLearn.Runtime", L"Invoke",
        UNMANAGEDCALLERSONLY_METHOD, nullptr, &entry);
    if (loadStatus < 0 || !entry)
        throw std::runtime_error("Script.AssemblyLoad: cannot load bridge, status=" + std::to_string(loadStatus));
    invoke = reinterpret_cast<Entry>(entry);
#else
    (void)runtimeDirectory;
    throw std::runtime_error("Script.Disabled: configure VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS=ON");
#endif
}

nlohmann::json ScriptHost::Invoke(nlohmann::json request) const
{
    if (!invoke) throw std::runtime_error("Script.HostUnavailable");
    const auto started = std::chrono::steady_clock::now();
    request["abi"] = 1;
    const std::string input = request.dump();
    constexpr int32_t MaximumBytes = 4 * 1024 * 1024;
    if (input.size() >= MaximumBytes) throw std::runtime_error("Script.RequestTooLarge");
    if (responseBuffer.empty()) responseBuffer.resize(MaximumBytes);
    const auto serialized = std::chrono::steady_clock::now();
    const int32_t size = invoke(input.data(), static_cast<int32_t>(input.size()), responseBuffer.data(), MaximumBytes);
    const auto invoked = std::chrono::steady_clock::now();
    if (size < 0 || size >= MaximumBytes) throw std::runtime_error("Script.ResponseTooLarge");
    auto result = nlohmann::json::parse(responseBuffer.data(), responseBuffer.data() + size);
    metrics.serializeMs += std::chrono::duration<double, std::milli>(serialized - started).count();
    metrics.managedMs += std::chrono::duration<double, std::milli>(invoked - serialized).count();
    metrics.parseMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - invoked).count();
    metrics.requestBytes += input.size(); metrics.responseBytes += size; ++metrics.calls;
    if (!result.at("ok").get<bool>())
    {
        result["operation"] = request.at("op");
        throw std::runtime_error(result.dump());
    }
    return result;
}
} // namespace VL
