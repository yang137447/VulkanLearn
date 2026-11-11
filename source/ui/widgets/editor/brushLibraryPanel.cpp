#include "ui/widgets/editor/brushLibraryPanel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

#include "ui/widgets/text/textWidgets.h"

namespace VL::UiWidgets
{
namespace
{

std::string LowerAscii(std::string value)
{
    for (char& character : value)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string Number(float value)
{
    if (!std::isfinite(value)) return "0";
    std::ostringstream stream;
    stream << std::setprecision(6) << value;
    return stream.str();
}

std::string ShapeClass(int shape)
{
    if (shape == 1) return "brush-library-stamp-square";
    if (shape == 2) return "brush-library-stamp-stamp";
    return "brush-library-stamp-circle";
}

std::string FalloffClass(int falloff)
{
    switch (falloff)
    {
    case 1: return "brush-library-stamp-falloff-linear";
    case 2: return "brush-library-stamp-falloff-constant";
    case 3: return "brush-library-stamp-falloff-lut";
    default: return "brush-library-stamp-falloff-smooth";
    }
}

std::string ShapeName(int shape)
{
    if (shape == 1) return "Square";
    if (shape == 2) return "Stamp";
    return "Circle";
}

void AppendSignaturePart(std::string& signature, const std::string& value)
{
    signature += std::to_string(value.size());
    signature += ':';
    signature += value;
    signature += ';';
}

void AppendSignaturePart(std::string& signature, float value)
{
    AppendSignaturePart(signature, Number(value));
}

bool MatchesQuery(const BrushLibraryEntry& entry, const std::string& foldedQuery)
{
    if (foldedQuery.empty()) return true;
    const std::string haystack = LowerAscii(
        entry.name + "\n" + entry.id + "\n" + entry.category);
    return haystack.find(foldedQuery) != std::string::npos;
}

std::vector<std::string> WithAllCategory(const std::vector<std::string>& categories)
{
    std::vector<std::string> result{"All"};
    result.insert(result.end(), categories.begin(), categories.end());
    return result;
}

std::string CategorySignature(
    const std::vector<std::string>& categories)
{
    std::string value;
    for (const std::string& item : categories)
        AppendSignaturePart(value, item);
    return value;
}

std::string CardMarkup(const BrushLibraryEntry& entry, bool selected)
{
    std::string classes = "brush-library-card ";
    classes += ShapeClass(entry.shape) + " " + FalloffClass(entry.falloff);
    if (selected) classes += " selected";

    const std::string meta = Number(entry.radiusCm) + " cm / " + ShapeName(entry.shape);
    return "<button class=\"" + classes +
        "\" data-action=\"brush-library-select\" data-brush-id=\"" + Text(entry.id) +
        "\" title=\"" + Text(entry.name + " - " + meta) + "\">" +
        "<div class=\"brush-library-stamp " + ShapeClass(entry.shape) + " " +
        FalloffClass(entry.falloff) + "\">" +
        "<div class=\"brush-library-stamp-ring\"></div>" +
        "<div class=\"brush-library-stamp-core\"></div></div>" +
        "<span class=\"brush-library-name\">" + Text(entry.name) + "</span>" +
        "<span class=\"brush-library-meta\">" + Text(meta) + "</span></button>";
}

} // namespace

bool BrushLibraryPanel::Attach(
    Rml::ElementDocument& documentValue,
    const std::string& hostId,
    std::string& error)
{
    error.clear();
    host = nullptr;
    document = nullptr;
    shellBuilt = false;
    builtSignature.clear();
    builtCategorySignature.clear();
    builtCards.clear();
    builtEmpty.clear();
    builtEmptyVisible = false;

    if (hostId.empty())
    {
        error = "Brush library host id is empty.";
        return false;
    }
    Rml::Element* value = documentValue.GetElementById(hostId);
    if (value == nullptr)
    {
        error = "Brush library host is missing: " + hostId;
        return false;
    }
    document = &documentValue;
    host = value;
    return true;
}

void BrushLibraryPanel::Detach()
{
    document = nullptr;
    host = nullptr;
    shellBuilt = false;
    builtSignature.clear();
    builtCategorySignature.clear();
    builtCards.clear();
    builtEmpty.clear();
    builtEmptyVisible = false;
}

void BrushLibraryPanel::SetEntries(std::vector<BrushLibraryEntry> value)
{
    entries = std::move(value);
}

void BrushLibraryPanel::SetSelectedId(std::string value)
{
    selectedId = std::move(value);
}

void BrushLibraryPanel::SetQuery(std::string value)
{
    query = std::move(value);
}

void BrushLibraryPanel::SetCategory(std::string value)
{
    category = std::move(value);
}

void BrushLibraryPanel::Build()
{
    if (document == nullptr || host == nullptr || !EnsureShell()) return;

    const std::vector<std::string> categories = Categories();
    category = EffectiveCategory();

    const std::string categorySignature = CategorySignature(categories);
    if (categorySignature != builtCategorySignature)
    {
        UpdateCategoryOptions(categories, category);
        builtCategorySignature = categorySignature;
    }
    SynchronizeCategorySelection(category);
    UpdateSearchValue();

    const std::string signature = Signature();
    if (signature == builtSignature) return;

    std::string cards = CardsMarkup();
    if (cards != builtCards)
    {
        if (Rml::Element* element = host->GetElementById("brush-library-cards"))
            element->SetInnerRML(cards);
        builtCards = std::move(cards);
    }

    std::string empty = EmptyMarkup();
    const bool emptyVisible = builtCards.empty();
    if (empty != builtEmpty)
    {
        if (Rml::Element* element = host->GetElementById("brush-library-empty"))
            element->SetInnerRML(empty);
        builtEmpty = std::move(empty);
    }
    if (emptyVisible != builtEmptyVisible)
    {
        if (Rml::Element* element = host->GetElementById("brush-library-empty"))
            element->SetProperty("display", emptyVisible ? "block" : "none");
        builtEmptyVisible = emptyVisible;
    }
    builtSignature = signature;
}

bool BrushLibraryPanel::EnsureShell()
{
    if (shellBuilt) return true;

    host->SetInnerRML(
        "<div class=\"brush-library-filters\">"
        "<input id=\"brush-library-search\" class=\"brush-library-search\" "
        "type=\"text\" data-action=\"brush-library-search\" value=\"" +
        Text(query) + "\" placeholder=\"Search brushes\"/>"
        "<select id=\"brush-library-category\" class=\"brush-library-category\" "
        "data-action=\"brush-library-category\"></select>"
        "</div>"
        "<div id=\"brush-library-cards\" class=\"brush-library-grid\"></div>"
        "<div id=\"brush-library-empty\" class=\"brush-library-empty\" "
        "style=\"display: none;\"></div>");
    shellBuilt = true;
    builtSignature.clear();
    builtCategorySignature.clear();
    builtCards.clear();
    builtEmpty.clear();
    builtEmptyVisible = false;
    return true;
}

std::vector<std::string> BrushLibraryPanel::Categories() const
{
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const BrushLibraryEntry& entry : entries)
    {
        if (entry.category.empty() || !seen.insert(entry.category).second) continue;
        result.push_back(entry.category);
    }
    return result;
}

std::string BrushLibraryPanel::EffectiveCategory() const
{
    if (category.empty() || category == "All") return "All";
    const std::vector<std::string> categories = Categories();
    if (std::find(categories.begin(), categories.end(), category) != categories.end())
        return category;
    return "All";
}

std::string BrushLibraryPanel::Signature() const
{
    std::string signature;
    AppendSignaturePart(signature, selectedId);
    AppendSignaturePart(signature, query);
    AppendSignaturePart(signature, category);
    for (const BrushLibraryEntry& entry : entries)
    {
        AppendSignaturePart(signature, entry.id);
        AppendSignaturePart(signature, entry.name);
        AppendSignaturePart(signature, entry.category);
        AppendSignaturePart(signature, std::to_string(entry.shape));
        AppendSignaturePart(signature, std::to_string(entry.falloff));
        AppendSignaturePart(signature, entry.radiusCm);
        AppendSignaturePart(signature, entry.strength);
    }
    return signature;
}

void BrushLibraryPanel::UpdateCategoryOptions(
    const std::vector<std::string>& categories,
    const std::string& selectedCategory)
{
    Rml::Element* element = host->GetElementById("brush-library-category");
    auto* selector = dynamic_cast<Rml::ElementFormControlSelect*>(element);
    if (selector == nullptr) return;

    std::string options;
    const std::vector<std::string> values = WithAllCategory(categories);
    for (const std::string& value : values)
    {
        options += "<option value=\"" + Text(value) + "\"";
        if (value == selectedCategory) options += " selected=\"selected\"";
        options += ">" + Text(value) + "</option>";
    }

    selector->RemoveAll();
    selector->SetInnerRML(options);
    // RmlUi 将 option 移入内部下拉控件；显式取值让选中项在下一帧前就完成迁移。
    selector->GetNumOptions();
}

void BrushLibraryPanel::SynchronizeCategorySelection(const std::string& categoryValue)
{
    auto* selector = dynamic_cast<Rml::ElementFormControlSelect*>(
        host->GetElementById("brush-library-category"));
    if (selector == nullptr || selector->GetValue().empty() ||
        selector->GetValue() == categoryValue) return;

    const int count = selector->GetNumOptions();
    for (int index = 0; index < count; ++index)
    {
        Rml::Element* option = selector->GetOption(index);
        if (option == nullptr ||
            option->GetAttribute<std::string>("value", "") != categoryValue) continue;
        selector->SetSelection(index);
        return;
    }
}

void BrushLibraryPanel::UpdateSearchValue()
{
    auto* input = dynamic_cast<Rml::ElementFormControl*>(
        host->GetElementById("brush-library-search"));
    if (input == nullptr || input->GetValue() == query) return;

    Rml::Context* context = document->GetContext();
    if (context != nullptr && context->GetFocusElement() == input) return;
    input->SetValue(query);
}

std::string BrushLibraryPanel::CardsMarkup() const
{
    const std::string foldedQuery = LowerAscii(query);
    std::string markup;
    for (const BrushLibraryEntry& entry : entries)
    {
        if (!MatchesQuery(entry, foldedQuery)) continue;
        if (category != "All" && entry.category != category) continue;
        markup += CardMarkup(entry, entry.id == selectedId);
    }
    return markup;
}

std::string BrushLibraryPanel::EmptyMarkup() const
{
    if (entries.empty()) return "No brushes are available.";
    return "No brushes match the current filter.";
}

} // namespace VL::UiWidgets
