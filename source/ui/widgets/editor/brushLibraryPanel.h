#pragma once

#include <string>
#include <vector>

namespace Rml
{
class Element;
class ElementDocument;
}

namespace VL::UiWidgets
{

// 笔刷库只消费展示值；shape/falloff 使用业务方给定的整数标签，
// 由宿主解释为具体地形或材质笔刷参数。
struct BrushLibraryEntry
{
    std::string id;
    std::string name;
    std::string category;
    int shape = 0;
    int falloff = 0;
    float radiusCm = 0.0f;
    float strength = 0.0f;
};

class BrushLibraryPanel
{
public:
    bool Attach(
        Rml::ElementDocument& document,
        const std::string& hostId,
        std::string& error);
    void Detach();
    void SetEntries(std::vector<BrushLibraryEntry> entries);
    void SetSelectedId(std::string selectedId);
    void SetQuery(std::string query);
    void SetCategory(std::string category);
    void Build();

private:
    bool EnsureShell();
    std::vector<std::string> Categories() const;
    std::string EffectiveCategory() const;
    std::string Signature() const;
    void UpdateCategoryOptions(
        const std::vector<std::string>& categories,
        const std::string& category);
    void SynchronizeCategorySelection(const std::string& category);
    void UpdateSearchValue();
    std::string CardsMarkup() const;
    std::string EmptyMarkup() const;

    Rml::ElementDocument* document = nullptr;
    Rml::Element* host = nullptr;
    std::vector<BrushLibraryEntry> entries;
    std::string selectedId;
    std::string query;
    std::string category = "All";
    std::string builtSignature;
    std::string builtCategorySignature;
    std::string builtCards;
    std::string builtEmpty;
    bool shellBuilt = false;
    bool builtEmptyVisible = false;
};

} // namespace VL::UiWidgets
