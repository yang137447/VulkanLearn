#include "script/scriptBuildService.h"
#include "script/scriptSourceFiles.h"
#include "shader/build/contentHash.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace VL
{
namespace
{
constexpr size_t MaximumBuildLog = 256 * 1024;
std::string XmlText(const std::string& value)
{
    std::string result;
    for (const char character : value)
    {
        if (character == '&') result += "&amp;";
        else if (character == '<') result += "&lt;";
        else if (character == '>') result += "&gt;";
        else if (character == '"') result += "&quot;";
        else result += character;
    }
    return result;
}
#ifdef _WIN32
// 句柄由编译线程独占；job关闭负责清理编译器及其子进程，失败路径也不会留下后台SDK。
struct ProcessHandle
{
    HANDLE value = nullptr;
    ~ProcessHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#endif
}

std::wstring ScriptBuildService::QuoteArgument(const std::wstring& argument)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto character : argument)
    {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

std::string ScriptBuildService::SourceDigest(const std::filesystem::path& sourceRoot)
{
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(sourceRoot);
        it != std::filesystem::recursive_directory_iterator(); ++it)
    {
        const auto name = it->path().filename().string();
        if (it->is_directory() && (name == "obj" || name == "bin" || name == ".git"))
        { it.disable_recursion_pending(); continue; }
        if (it->is_symlink()) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        const auto extension = it->path().extension().string();
        if (it->is_regular_file() && (extension == ".cs" || extension == ".csproj" ||
            extension == ".props" || extension == ".targets" || name == "global.json" || name == "NuGet.Config"))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    CanonicalFieldHasher hash("gameplay-source-v1");
    for (const auto& file : files)
    {
        hash.AddString("path", file.lexically_relative(sourceRoot).generic_string());
        hash.AddDigest("bytes", ContentHasher::HashFile(file));
    }
    return hash.Finalize().ToHex();
}

void ScriptBuildService::Configure(std::filesystem::path sdk, std::filesystem::path source,
    std::filesystem::path output, std::filesystem::path bridgePath, std::string config,
    std::filesystem::path contentRoot, std::filesystem::path apiSources)
{
    Shutdown();
    // dotnet 在项目目录启动；先冻结启动目录下的绝对路径，避免相对资源根改变输出与桥接身份。
    dotnet = std::filesystem::absolute(sdk).lexically_normal();
    project = std::filesystem::absolute(source).lexically_normal();
    outputRoot = std::filesystem::absolute(output).lexically_normal();
    bridge = std::filesystem::absolute(bridgePath).lexically_normal();
    resourceRoot = contentRoot.empty() ? std::filesystem::path() :
        std::filesystem::weakly_canonical(std::filesystem::absolute(contentRoot));
    runtimeSources = apiSources.empty() ? project.parent_path().parent_path() / "VulkanLearn.Runtime" :
        std::filesystem::absolute(apiSources).lexically_normal();
    configuration = std::move(config);
    stopping = false;
    if (Available())
    {
        bridgeDigest = ContentHasher::HashFile(bridge).ToHex();
        bridgeSourceDigest = std::filesystem::is_directory(runtimeSources) ? SourceDigest(runtimeSources) : "";
        observedDigest = CurrentSourceDigest();
    }
}

std::string ScriptBuildService::CurrentSourceDigest() const
{
    if (resourceRoot.empty()) return SourceDigest(project.parent_path().parent_path());
    CanonicalFieldHasher hash("resource-gameplay-source-v1");
    hash.AddDigest("project", ContentHasher::HashFile(project));
    for (const auto& file : CollectScriptSourceFiles(resourceRoot))
    {
        hash.AddString("path", file.lexically_relative(resourceRoot).generic_u8string());
        hash.AddDigest("bytes", ContentHasher::HashFile(file));
    }
    return hash.Finalize().ToHex();
}

bool ScriptBuildService::Available() const
{
#ifdef _WIN32
    return std::filesystem::is_regular_file(dotnet) && std::filesystem::is_regular_file(project) &&
        std::filesystem::is_regular_file(bridge);
#else
    return false;
#endif
}

bool ScriptBuildService::Request(uint64_t generation, std::string& error)
{
    error.clear();
    if (!Available()) { error = "Gameplay build requires the configured .NET SDK and source project"; return false; }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (running || completed) { error = "Gameplay build is already pending"; return false; }
    }
    if (worker.joinable()) worker.join();
    try
    {
        const auto digest = CurrentSourceDigest();
        if (!bridgeSourceDigest.empty() &&
            SourceDigest(runtimeSources) != bridgeSourceDigest)
            throw std::runtime_error("Runtime API sources changed; rebuild and restart the application");
        const auto output = outputRoot / ("candidate-" + std::to_string(++sequence) + "-" + digest.substr(0, 12));
        // 每个候选独占文件目录，不覆盖当前加载程序集或CMake的共享obj输出。
        if (std::filesystem::exists(output)) throw std::runtime_error("Build candidate directory already exists");
        std::filesystem::create_directories(output);
        // 使用当前进程的桥接副本，仍让原Gameplay项目处理自己的源码/引用/资源；不重编常驻API。
        const auto targetsPath = output / "FrozenRuntime.targets";
        std::ofstream targets(targetsPath);
        targets << "<Project><Target Name=\"UseFrozenGameplayRuntime\" BeforeTargets=\"AssignProjectConfiguration\">"
            "<ItemGroup><ProjectReference Remove=\"@(ProjectReference)\" "
            "Condition=\"'%(ProjectReference.Filename)' == 'VulkanLearn.Runtime'\"/>"
            "<Reference Include=\"VulkanLearn.Runtime\"><HintPath>" << XmlText(bridge.generic_u8string()) <<
            "</HintPath><Private>true</Private></Reference></ItemGroup></Target>";
        // 只规范独占候选中的生成源码/PDB 路径；作者源码仍保留真实路径及已有 PathMap。
        // 否则即使 Deterministic=true，每次不同的 intermediate 目录也会改变 DLL/PDB 字节。
        // Roslyn 按本机分隔符匹配来源路径；Windows 正斜杠映射无法命中生成源码的反斜杠路径。
        const auto candidateMap = XmlText(
            std::filesystem::path(output).make_preferred().u8string() + "=/_/vulkanlearn/candidate");
        targets << "<Target Name=\"NormalizeGameplayCandidatePaths\" BeforeTargets=\"CoreCompile\"><PropertyGroup>"
            "<PathMap Condition=\"'$(PathMap)' != ''\">" << candidateMap << ",$(PathMap)</PathMap>"
            "<PathMap Condition=\"'$(PathMap)' == ''\">" << candidateMap << "</PathMap>"
            "</PropertyGroup></Target></Project>";
        targets.close();
        if (!resourceRoot.empty())
        {
            // 冻结本次实际输入清单，避免 SDK glob 与监视范围不同；编译后仍复核新增、删除和改写。
            std::ofstream sources(output / "ResourceSources.props");
            sources << ScriptSourceItems(resourceRoot);
            sources.close();
            if (!sources) throw std::runtime_error("Cannot write resource C# source manifest");
        }
        stopping = false;
        running = true;
        worker = std::thread(&ScriptBuildService::Compile, this, generation, output, digest);
        return true;
    }
    catch (const std::exception& exception) { running = false; error = exception.what(); return false; }
}

void ScriptBuildService::Compile(uint64_t generation, std::filesystem::path output, std::string digest)
{
    ScriptBuildResult result;
    result.worldGeneration = generation;
    result.sourceDigest = std::move(digest);
    result.assembly = output / (project.stem().string() + ".dll");
    try
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        ProcessHandle log;
        log.value = CreateFileW((output / "build.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create build log");
        ProcessHandle input;
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ProcessHandle job;
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("Cannot create compiler process job");
        // --artifacts-path隔离项目引用的obj/bin，避免和CMake构建相互污染。
        std::wstring command = QuoteArgument(dotnet.wstring()) + L" build " + QuoteArgument(project.wstring()) +
            L" --nologo --configuration " + QuoteArgument(std::filesystem::path(configuration).wstring()) +
            L" --output " + QuoteArgument(output.wstring()) + L" --artifacts-path " +
            QuoteArgument((output / "intermediate").wstring()) +
            L" " + QuoteArgument(L"-p:CustomAfterMicrosoftCommonTargets=" + (output / "FrozenRuntime.targets").wstring()) +
            L" -p:UseSharedCompilation=false -nodeReuse:false";
        if (!resourceRoot.empty())
            command += L" " + QuoteArgument(L"-p:VulkanLearnSourceItems=" + (output / "ResourceSources.props").wstring());
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = startup.hStdError = log.value; startup.hStdInput = input.value;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(dotnet.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, project.parent_path().c_str(), &startup, &process))
            throw std::runtime_error("Cannot launch dotnet build: " + std::to_string(GetLastError()));
        ProcessHandle processHandle{process.hProcess}, threadHandle{process.hThread};
        if (!AssignProcessToJobObject(job.value, process.hProcess))
        {
            TerminateProcess(process.hProcess, 1);
            throw std::runtime_error("Cannot isolate compiler process");
        }
        ResumeThread(process.hThread);
        while (WaitForSingleObject(process.hProcess, 50) == WAIT_TIMEOUT)
            if (stopping) { TerminateJobObject(job.value, 1); result.cancelled = true; break; }
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD status = 1; GetExitCodeProcess(process.hProcess, &status);
        // 先关闭写入句柄再读诊断；保留原始文件，UI只接收有界文本。
        CloseHandle(log.value); log.value = nullptr;
        std::ifstream stream(output / "build.log", std::ios::binary);
        result.log.resize(MaximumBuildLog);
        stream.read(result.log.data(), static_cast<std::streamsize>(result.log.size()));
        result.log.resize(static_cast<size_t>(stream.gcount()));
        result.succeeded = !result.cancelled && status == 0 && std::filesystem::is_regular_file(result.assembly);
        if (result.succeeded && ContentHasher::HashFile(output / "VulkanLearn.Runtime.dll").ToHex() != bridgeDigest)
        { result.succeeded = false; result.log += "\nRuntime API changed; rebuild and restart the application."; }
        if (result.succeeded && CurrentSourceDigest() != result.sourceDigest)
        { result.succeeded = false; result.log += "\nSources changed during compilation; build again."; }
#else
        result.log = "Gameplay builds are supported on Windows.";
#endif
    }
    catch (const std::exception& exception) { result.log += "\n" + std::string(exception.what()); result.succeeded = false; }
    { std::lock_guard<std::mutex> lock(mutex); completed = std::move(result); }
    running = false;
}

std::optional<ScriptBuildResult> ScriptBuildService::Poll()
{
    std::lock_guard<std::mutex> lock(mutex);
    auto result = std::move(completed);
    completed.reset();
    return result;
}

bool ScriptBuildService::ObserveChanges(float dt)
{
    pollTime += dt;
    if (pollTime < 0.75f || !Available()) return false;
    pollTime = 0;
    const auto digest = CurrentSourceDigest();
    if (digest == observedDigest) { pendingDigest.clear(); return false; }
    if (digest != pendingDigest) { pendingDigest = digest; return false; }
    if (Busy()) return false;
    observedDigest = digest; pendingDigest.clear();
    return true;
}

void ScriptBuildService::Shutdown() noexcept
{
    stopping = true;
    if (worker.joinable()) worker.join();
    running = false;
    std::lock_guard<std::mutex> lock(mutex);
    completed.reset();
}
}
