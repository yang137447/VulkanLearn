#include "ui/uiSubsystem.h"
#include "ui/debugDrawOverlay.h"
#include "ui/gameplayHudView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <nlohmann/json.hpp>


#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeCommand.h"
#include "core/renderSession.h"
#include "platform/platformEvent.h"
#include "platform/platformWindow.h"
#include "ui/sceneObjectTypeSupport.h"
#include "ui/uiBindingUpdateScope.h"
#include "Profiler.h"
#include "ui/uiSubsystemInternal.h"

namespace VL
{

using namespace UiSubsystemDetail;

namespace
{

bool IsSceneOptionLess(const UiSceneOption& left, const UiSceneOption& right)
{
    std::string leftSortKey = left.label;
    std::string rightSortKey = right.label;
    for (char& character : leftSortKey)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    for (char& character : rightSortKey)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    // 场景显示名是面向用户的标签，排序时忽略 ASCII 大小写，避免小写资源名沉到列表末尾。
    if (leftSortKey != rightSortKey)
    {
        return leftSortKey < rightSortKey;
    }
    if (left.label != right.label)
    {
        return left.label < right.label;
    }
    return left.path < right.path;
}

std::vector<std::filesystem::path> BuildPlatformFontCandidates()
{
    std::vector<std::filesystem::path> fonts;
#if defined(_WIN32)
    fonts.emplace_back("C:/Windows/Fonts/segoeui.ttf");
    fonts.emplace_back("C:/Windows/Fonts/NotoSansSC-VF.ttf");
    fonts.emplace_back("C:/Windows/Fonts/msyh.ttc");
    fonts.emplace_back("C:/Windows/Fonts/arial.ttf");
#elif defined(__APPLE__)
    fonts.emplace_back("/System/Library/Fonts/SFNS.ttf");
    fonts.emplace_back("/System/Library/Fonts/PingFang.ttc");
#else
    fonts.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    fonts.emplace_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
#endif
    return fonts;
}

uint64_t GetFileFingerprint(const std::filesystem::directory_entry& entry)
{
    const uint64_t writeTime = static_cast<uint64_t>(entry.last_write_time().time_since_epoch().count());
    const uint64_t size = entry.is_regular_file() ? static_cast<uint64_t>(entry.file_size()) : 0;
    return writeTime ^ (size + 0x9e3779b97f4a7c15ull + (writeTime << 6u) + (writeTime >> 2u));
}

} // namespace

RuntimeResult<void> UiSubsystem::InitializeRmlUi()
{
    rmlSystemInterface.Initialize(*window, *diagnostics);
    Rml::SetSystemInterface(&rmlSystemInterface);
    // 全局渲染接口只是兜底；真正的每窗口接口在 CreateContext 时传入（RmlUi 支持每 context 接口），
    // 这正是多窗口能共用一套 RmlUi 全局状态的原因。
    Rml::SetRenderInterface(&rmlRenderInterface);
    // 进程级初始化由 RmlUiRuntime 持有（引用计数），而不是由这个"每窗口一份"的对象持有：
    // 否则第二个窗口的 Shutdown 会把 RmlUi 从第一个底下抽走。
    try
    {
        VL::Ui::RmlUiRuntime::Acquire();
    }
    catch (const std::exception& error)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.RmlInitializeFailed", error.what()));
    }
    rmlInitialized = true;

    // context 名必须唯一；窗口身份是唯一稳定的区分依据（此前硬编码 "vulkanlearn"，
    // 第二个窗口会直接撞名并拿到 nullptr）。
    rmlContext = Rml::CreateContext(
        VL::Ui::RmlUiRuntime::ContextNameFor(windowIndex),
        Rml::Vector2i(
            static_cast<int>(desc.viewportWidth),
            static_cast<int>(desc.viewportHeight)),
        &rmlRenderInterface,
        &rmlTextInputHandler);
    if (rmlContext == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.RmlContextFailed",
            "RmlUi failed to create the runtime context."));
    }
    debugDrawLabels = std::make_unique<DebugDrawLabels>();
    if (!debugDrawLabels->Initialize(rmlContext->GetName() + "-debug-labels", rmlRenderInterface))
        return RuntimeResult<void>::Failure(MakeRuntimeError("UiSubsystem.DebugLabelsFailed", "RmlUi debug label context failed"));
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> UiSubsystem::InitializeDataModel()
{
    Rml::DataModelConstructor constructor = rmlContext->CreateDataModel("game_ui");
    if (!constructor)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.DataModelFailed",
            "RmlUi failed to create the game_ui data model."));
    }

    if (!constructor.Bind("world_text", &bindingData.worldText))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.DataModelFailed", "Could not bind HUD world label."));
    }
    if (!constructor.Bind("gameplay_text", &bindingData.gameplayText))
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.DataModelFailed", "Could not bind gameplay HUD text."));

    dataModelHandle = constructor.GetModelHandle();
    return RuntimeResult<void>::Success();
}

RuntimeResult<void> UiSubsystem::LoadFonts()
{
    std::vector<std::filesystem::path> candidates = desc.fontFaces;
    std::vector<std::filesystem::path> platformCandidates = BuildPlatformFontCandidates();
    candidates.insert(candidates.end(), platformCandidates.begin(), platformCandidates.end());

    bool loadedPrimary = false;
    for (const std::filesystem::path& candidate : candidates)
    {
        std::filesystem::path fontPath = candidate;
        if (!fontPath.is_absolute())
        {
            fontPath = desc.assetRoot / fontPath;
        }
        if (!std::filesystem::exists(fontPath))
        {
            continue;
        }

        const bool fallback = loadedPrimary;
        if (Rml::LoadFontFace(fontPath.string(), fallback))
        {
            loadedPrimary = true;
        }
    }

    if (!loadedPrimary)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "UiSubsystem.FontMissing",
            "No configured or platform fallback UI font could be loaded.",
            desc.assetRoot.string()));
    }
    return RuntimeResult<void>::Success();
}

void UiSubsystem::LoadSceneOptions()
{
    bindingData.sceneOptions.clear();
    if (desc.sceneRoot.empty())
    {
        diagnostics->ReportWarning("UI scene selector disabled because sceneRoot is empty.");
        return;
    }

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        desc.sceneRoot,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    if (error)
    {
        diagnostics->ReportWarning(
            "UI scene selector could not scan scene root: " + desc.sceneRoot.string());
        return;
    }

    while (iterator != end)
    {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_regular_file(error) && entry.path().extension() == ".json")
        {
            try
            {
                std::ifstream stream(entry.path());
                nlohmann::json sceneJson;
                stream >> sceneJson;
                const bool isSceneAsset =
                    sceneJson.is_object() &&
                    sceneJson.value("type", std::string()) == "scene" &&
                    sceneJson.contains("objects") &&
                    sceneJson["objects"].is_array() &&
                    HasSupportedSceneObjectTypes(sceneJson);
                if (isSceneAsset)
                {
                    UiSceneOption option;
                    option.label = sceneJson.value(
                        "name",
                        entry.path().stem().string());
                    std::filesystem::path relativePath = std::filesystem::relative(
                        entry.path(),
                        desc.sceneRoot.parent_path(),
                        error);
                    if (error)
                    {
                        error.clear();
                        relativePath = std::filesystem::path("scenes") / entry.path().filename();
                    }
                    option.path = relativePath.generic_string();
                    option.normalizedPhysicalPath = NormalizePathForComparison(entry.path());
                    bindingData.sceneOptions.push_back(std::move(option));
                }
                else if (sceneJson.is_object() &&
                    sceneJson.value("type", std::string()) == "scene")
                {
                    diagnostics->ReportWarning(
                        "UI scene selector skipped an unsupported scene asset: " +
                        entry.path().string());
                }
            }
            catch (const std::exception& exception)
            {
                diagnostics->ReportWarning(
                    "UI scene selector skipped invalid JSON: " +
                    entry.path().string() + " (" + exception.what() + ")");
            }
        }
        error.clear();
        iterator.increment(error);
        if (error)
        {
            diagnostics->ReportWarning(
                "UI scene selector stopped while scanning: " + desc.sceneRoot.string());
            break;
        }
    }

    std::sort(
        bindingData.sceneOptions.begin(),
        bindingData.sceneOptions.end(),
        IsSceneOptionLess);
}

RuntimeResult<UiSubsystem::LocalizationDatabase> UiSubsystem::LoadLocalizationCandidate() const
{
    std::ifstream file(desc.localizationPath);
    if (!file.is_open())
    {
        return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
            "UiSubsystem.LocalizationOpenFailed",
            "Could not open UI localization file.",
            desc.localizationPath.string()));
    }

    try
    {
        nlohmann::json json;
        file >> json;
        if (!json.is_object())
        {
            return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                "UiSubsystem.LocalizationInvalidRoot",
                "UI localization root must be an object.",
                desc.localizationPath.string()));
        }

        LocalizationDatabase database;
        for (const auto& localeEntry : json.items())
        {
            if (!localeEntry.value().is_object())
            {
                return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                    "UiSubsystem.LocalizationInvalidLocale",
                    "Each UI locale must be an object.",
                    desc.localizationPath.string(),
                    localeEntry.key()));
            }

            LocalizationTable table;
            for (const auto& stringEntry : localeEntry.value().items())
            {
                if (!stringEntry.value().is_string())
                {
                    return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                        "UiSubsystem.LocalizationInvalidString",
                        "Each UI localization value must be a string.",
                        desc.localizationPath.string(),
                        localeEntry.key() + "." + stringEntry.key()));
                }
                table.emplace(stringEntry.key(), stringEntry.value().get<std::string>());
            }
            database.emplace(localeEntry.key(), std::move(table));
        }

        if (database.find(desc.defaultLocale) == database.end())
        {
            return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
                "UiSubsystem.DefaultLocaleMissing",
                "The configured default UI locale is not present in the localization file.",
                desc.localizationPath.string(),
                desc.defaultLocale));
        }
        return RuntimeResult<LocalizationDatabase>::Success(std::move(database));
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<LocalizationDatabase>::Failure(MakeRuntimeError(
            "UiSubsystem.LocalizationParseFailed",
            exception.what(),
            desc.localizationPath.string()));
    }
}

RuntimeResult<Rml::ElementDocument*> UiSubsystem::LoadDocumentCandidate()
{
    UiBindingUpdateScope bindingUpdate(updatingRuntimeBindings);
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();

    rmlSystemInterface.BeginCandidateValidation();
    Rml::ElementDocument* candidate = rmlContext->LoadDocument(desc.documentPath.generic_string());
    std::string parseError;
    const bool parseValid = rmlSystemInterface.EndCandidateValidation(parseError);
    if (candidate == nullptr || !parseValid)
    {
        if (candidate != nullptr)
        {
            rmlContext->UnloadDocument(candidate);
            rmlContext->Update();
        }
        return RuntimeResult<Rml::ElementDocument*>::Failure(MakeRuntimeError(
            "UiSubsystem.DocumentParseFailed",
            parseError.empty() ? "RmlUi returned no document." : parseError,
            desc.documentPath.string()));
    }

    std::string validationError;
    if (!ValidateDocument(*candidate, validationError))
    {
        rmlContext->UnloadDocument(candidate);
        rmlContext->Update();
        return RuntimeResult<Rml::ElementDocument*>::Failure(MakeRuntimeError(
            "UiSubsystem.DocumentValidationFailed",
            validationError,
            desc.documentPath.string()));
    }

    candidate->Hide();
    return RuntimeResult<Rml::ElementDocument*>::Success(candidate);
}

RuntimeResult<void> UiSubsystem::CommitInitialAssets()
{
    auto localizationResult = LoadLocalizationCandidate();
    if (localizationResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(localizationResult.Error());
    }
    auto documentResult = LoadDocumentCandidate();
    if (documentResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(documentResult.Error());
    }

    CommitDocumentCandidate(*documentResult.Value(), std::move(localizationResult.Value()));
    assetFingerprint = BuildAssetFingerprint();
    hotReloadStatus = desc.hotReload ? "hot reload: watching" : "hot reload: disabled";
    return RuntimeResult<void>::Success();
}

void UiSubsystem::CommitDocumentCandidate(
    Rml::ElementDocument& candidate,
    LocalizationDatabase localizationCandidate)
{
    UiBindingUpdateScope bindingUpdate(updatingRuntimeBindings);
    ResetRuntimeInputState();
    Rml::ElementDocument* previousDocument = runtimeDocument;
    runtimeDocument = &candidate;
    localizations = std::move(localizationCandidate);
    if (localizations.find(currentLocale) == localizations.end())
    {
        currentLocale = desc.defaultLocale;
    }

    Rml::ReleaseTextures(&rmlRenderInterface);
    runtimeDocument->Show();
    ApplyDocumentState();
    SyncBindingData();
    dataModelHandle.DirtyAllVariables();
    rmlContext->Update();

    if (previousDocument != nullptr)
    {
        rmlContext->UnloadDocument(previousDocument);
        rmlContext->Update();
    }
}

bool UiSubsystem::ValidateDocument(Rml::ElementDocument& document, std::string& outError) const
{
    static const char* requiredIds[] = {
        "ui-root",
        "hud"
    };
    for (const char* requiredId : requiredIds)
    {
        if (document.GetElementById(requiredId) == nullptr)
        {
            outError = std::string("Required UI element is missing: #") + requiredId;
            return false;
        }
    }
    outError.clear();
    return true;
}

void UiSubsystem::ApplyDocumentState()
{
    if (runtimeDocument == nullptr)
    {
        return;
    }

    // 文档 show/hide 改变可见 DOM 树：下一帧必须更新上下文并重发快照。
    uiUpdateScheduler.MarkMutation(UiMutationKind::DocumentVisibility);
    if (Rml::Element* hud = runtimeDocument->GetElementById("hud"))
        hud->SetProperty("display", developerUiVisible || immersiveView ? "none" : "block");
    if (editorUiShell != nullptr)
        editorUiShell->SetVisible(developerUiVisible);

}

void UiSubsystem::TryHotReload()
{
    if (!desc.hotReload || std::chrono::steady_clock::now() < nextHotReloadCheck)
    {
        return;
    }
    nextHotReloadCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    PROFILE_SCOPE("UI.HotReloadScan");

    AssetFingerprint candidateFingerprint = BuildAssetFingerprint();
    if (!HasAssetFingerprintChanged(candidateFingerprint))
    {
        return;
    }

    auto localizationResult = LoadLocalizationCandidate();
    if (localizationResult.IsFailure())
    {
        hotReloadStatus = "hot reload rollback: localization invalid";
        diagnostics->ReportRuntimeError("UI hot reload rejected", localizationResult.Error());
        return;
    }
    auto documentResult = LoadDocumentCandidate();
    if (documentResult.IsFailure())
    {
        hotReloadStatus = "hot reload rollback: document invalid";
        diagnostics->ReportRuntimeError("UI hot reload rejected", documentResult.Error());
        return;
    }

    if (developerUiEnabled && editorUiShell != nullptr)
    {
        auto editorResult = editorUiShell->ReloadDocument();
        if (editorResult.IsFailure())
        {
            rmlContext->UnloadDocument(documentResult.Value());
            hotReloadStatus = "hot reload rollback: editor invalid";
            diagnostics->ReportRuntimeError("UI hot reload rejected", editorResult.Error());
            return;
        }
        ResetRuntimeInputState();
    }
    CommitDocumentCandidate(*documentResult.Value(), std::move(localizationResult.Value()));
    assetFingerprint = std::move(candidateFingerprint);
    hotReloadStatus = "hot reload: committed";
    // 文档/本地化已经整份替换：下一帧按正常路径再更新一次并重发快照。
    uiUpdateScheduler.MarkMutation(UiMutationKind::HotReload);
    diagnostics->ReportInfo("UI hot reload committed: " + desc.documentPath.string());
}

UiSubsystem::AssetFingerprint UiSubsystem::BuildAssetFingerprint() const
{
    AssetFingerprint fingerprint;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        desc.assetRoot,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_regular_file(error))
        {
            fingerprint.emplace(entry.path().string(), GetFileFingerprint(entry));
        }
        iterator.increment(error);
    }
    return fingerprint;
}

bool UiSubsystem::HasAssetFingerprintChanged(const AssetFingerprint& candidate) const
{
    if (candidate.size() != assetFingerprint.size())
    {
        return true;
    }
    for (const auto& entry : candidate)
    {
        auto current = assetFingerprint.find(entry.first);
        if (current == assetFingerprint.end() || current->second != entry.second)
        {
            return true;
        }
    }
    return false;
}

void UiSubsystem::SyncBindingData()
{
    // 仅用于文档/语言整体重建：逐帧路径按分节版本只刷新对应绑定。
    SyncWorldTextBinding();
    SyncGameplayTextBinding(currentBuildMode == UiViewModelBuildMode::RuntimeHud
        ? hudScripts
        : lastViewModel.scripts);
}

void UiSubsystem::BuildAndPublishRenderSnapshot(bool includeUiDocuments)
{
    PROFILE_SCOPE("UI.RenderSnapshot");
    std::shared_ptr<UiRenderSnapshot> snapshot = std::make_shared<UiRenderSnapshot>();
    // 冻结快照不能跨 GT/RT 重用；仅借用上一帧容量估计，避免每个 draw 都重新分配。
    // 隐藏帧不渲染文档，不能沿用工作区那份（可达数万顶点）的容量估计，
    // 否则空快照也会每帧预留整块工作区几何。
    if (includeUiDocuments)
    {
        snapshot->vertices.reserve(previousUiVertexCount);
        snapshot->indices.reserve(previousUiIndexCount);
        snapshot->drawCommands.reserve(previousUiDrawCount);
    }
    snapshot->frameIndex = uiFrameIndex++;
    snapshot->viewportWidth = desc.viewportWidth;
    snapshot->viewportHeight = desc.viewportHeight;
    // 隐藏帧不能保留场景视口：RT 只应看到（可选的）DrawDebug 叠层。
    snapshot->sceneViewportEnabled = includeUiDocuments && IsSceneViewportEnabled();
    snapshot->sceneViewportRect = snapshot->sceneViewportEnabled &&
        editorUiShell != nullptr
        ? ClipViewportRect(
            editorUiShell->GetSceneViewportRect(),
            desc.viewportWidth,
            desc.viewportHeight)
        : UiViewportRect{
        0,
        0,
        desc.viewportWidth,
        desc.viewportHeight};
    sceneViewportRect = snapshot->sceneViewportRect;

    // BeginFrame/EndFrame 仍要成对调用：调试标签 context 的 Render 依赖渲染接口
    // 正处于帧捕获状态。隐藏帧只是不渲染 HUD/工作区文档。
    rmlRenderInterface.BeginFrame(*snapshot);
    if (includeUiDocuments)
    {
        rmlContext->Render();
    }
    const size_t firstDebugCommand = snapshot->drawCommands.size();
    // 沉浸显示与编辑器共用同一 DrawDebug 适配；控制台仍可在开发者 UI 隐藏时
    // 打开调试图形，因此隐藏帧也要保留这个叠层。
    const bool debugOverlayVisible = IsDebugDrawOverlayVisible();
    if (debugOverlayVisible)
    {
        AppendDebugDrawOverlay(*snapshot, *lastViewModel.debugDraw, lastViewModel.motionCamera, desc.srgbFramebuffer);
        debugDrawLabels->Render(*lastViewModel.debugDraw, lastViewModel.motionCamera,
            snapshot->sceneViewportRect, desc.viewportWidth, desc.viewportHeight);
        InsertDebugDrawCommands(*snapshot, firstDebugCommand);
    }
    lastPublishedHadDebugOverlay = debugOverlayVisible &&
        !(lastViewModel.debugDraw->lines.empty() &&
            lastViewModel.debugDraw->points.empty() &&
            lastViewModel.debugDraw->texts.empty());
    if (includeUiDocuments)
    {
        rmlRenderInterface.AppendTextureSnapshots(snapshot->textures);
    }
    rmlRenderInterface.EndFrame();
    previousUiVertexCount = snapshot->vertices.size();
    previousUiIndexCount = snapshot->indices.size();
    previousUiDrawCount = snapshot->drawCommands.size();
    snapshotPublisher.Publish(std::move(snapshot));
    ++refreshStats.snapshotPublishes;
}

} // namespace VL
