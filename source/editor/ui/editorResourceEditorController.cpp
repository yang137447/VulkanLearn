#include "editorResourceEditorController.h"

#include <algorithm>
#include <iterator>

namespace VL::EditorUi
{

bool EditorResourceEditorController::Open(const std::string& path, EditorAssetKind kind)
{
    if (path.empty())
    {
        return false;
    }

    const auto found = std::find_if(tabs.begin(), tabs.end(),
        [&path](const EditorResourceEditorTab& tab) { return tab.path == path; });
    if (found != tabs.end())
    {
        activeIndex = static_cast<size_t>(std::distance(tabs.begin(), found));
        return true;
    }

    if (tabs.size() >= MaxTabs)
    {
        return false;
    }

    tabs.push_back({path, kind, nullptr, true});
    activeIndex = tabs.size() - 1;
    return true;
}

bool EditorResourceEditorController::Close(const std::string& path)
{
    const auto found = std::find_if(tabs.begin(), tabs.end(),
        [&path](const EditorResourceEditorTab& tab) { return tab.path == path; });
    if (found == tabs.end())
    {
        return false;
    }

    const size_t closedIndex = static_cast<size_t>(std::distance(tabs.begin(), found));
    const bool wasActive = closedIndex == activeIndex;
    if (wasActive) reloadRequested = false;
    tabs.erase(found);

    if (tabs.empty())
    {
        activeIndex = InvalidIndex;
        reloadRequested = false;
    }
    else if (activeIndex == InvalidIndex)
    {
        activeIndex = 0;
    }
    else if (closedIndex < activeIndex)
    {
        --activeIndex;
    }
    else if (wasActive)
    {
        activeIndex = std::min(closedIndex, tabs.size() - 1);
    }

    return true;
}

bool EditorResourceEditorController::Activate(const std::string& path)
{
    const auto found = std::find_if(tabs.begin(), tabs.end(),
        [&path](const EditorResourceEditorTab& tab) { return tab.path == path; });
    if (found == tabs.end())
    {
        return false;
    }

    activeIndex = static_cast<size_t>(std::distance(tabs.begin(), found));
    return true;
}

const EditorResourceEditorTab* EditorResourceEditorController::ActiveTab() const
{
    if (activeIndex == InvalidIndex || activeIndex >= tabs.size())
    {
        return nullptr;
    }
    return &tabs[activeIndex];
}

bool EditorResourceEditorController::SetDocument(
    std::shared_ptr<const EditorJsonDocumentSnapshot> snapshot, bool loading)
{
    if (snapshot == nullptr)
    {
        return false;
    }

    EditorResourceEditorTab* active = activeIndex == InvalidIndex || activeIndex >= tabs.size()
        ? nullptr
        : &tabs[activeIndex];
    if (active == nullptr || active->path != snapshot->assetPath)
    {
        return false;
    }

    active->snapshot = std::move(snapshot);
    active->loading = loading;
    return true;
}

std::string EditorResourceEditorController::RequestReload()
{
    if (activeIndex == InvalidIndex || activeIndex >= tabs.size())
    {
        return {};
    }

    reloadRequested = true;
    tabs[activeIndex].loading = true;
    return tabs[activeIndex].path;
}

std::string EditorResourceEditorController::ConsumeReloadRequest()
{
    if (!reloadRequested)
    {
        return {};
    }

    reloadRequested = false;
    const EditorResourceEditorTab* active = ActiveTab();
    return active == nullptr ? std::string{} : active->path;
}

} // namespace VL::EditorUi
