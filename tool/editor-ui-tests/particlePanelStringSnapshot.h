#pragma once

// Source-level snapshot of the particle panel's UI contract strings.
//
// The particle panel builds its Rml markup as C++ string literals. Splitting the
// implementation into several translation units must not change any id, action
// name, data-* attribute or visible string, so the tests read every
// `particlePanel*.cpp` file, decode its literals and compare a frozen
// multiset. Extraction is order-insensitive across files because the split
// re-groups literals by translation unit; the multiset (not the sequence) is the
// invariant.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace VL::EditorUi::ParticlePanelSnapshot
{
struct Literal
{
    std::string value;
    size_t offset = 0;
};

struct SourceSet
{
    std::vector<std::string> files;
    std::vector<std::string> literals;
    std::vector<std::string> rmlIds;
    std::vector<std::string> idReferences;
    std::vector<std::string> actions;
    std::vector<std::string> dataAttributes;
};

inline std::string DecodeEscape(char escaped)
{
    switch (escaped)
    {
    case 'n': return "\n";
    case 't': return "\t";
    case 'r': return "\r";
    case '0': return std::string(1, '\0');
    case '\\': return "\\";
    case '"': return "\"";
    case '\'': return "'";
    default: return std::string(1, '\\') + escaped;
    }
}

// C++ lexical scan: skips directives and comments, handles char literals and
// escape sequences. The result is the decoded literal values in source order.
inline std::vector<Literal> ExtractLiterals(std::string_view text)
{
    std::vector<Literal> literals;
    size_t index = 0;
    bool lineStart = true;
    while (index < text.size())
    {
        const char character = text[index];
        if (character == '\n')
        {
            lineStart = true;
            ++index;
            continue;
        }
        if (lineStart)
        {
            size_t probe = index;
            while (probe < text.size() &&
                (text[probe] == ' ' || text[probe] == '\t'))
                ++probe;
            if (probe < text.size() && text[probe] == '#')
            {
                while (index < text.size() && text[index] != '\n')
                    ++index;
                continue;
            }
            lineStart = false;
        }
        if (character == '/' && index + 1 < text.size() && text[index + 1] == '/')
        {
            while (index < text.size() && text[index] != '\n')
                ++index;
            continue;
        }
        if (character == '/' && index + 1 < text.size() && text[index + 1] == '*')
        {
            index += 2;
            while (index + 1 < text.size() &&
                !(text[index] == '*' && text[index + 1] == '/'))
                ++index;
            index = std::min(text.size(), index + 2);
            continue;
        }
        if (character == '\'')
        {
            ++index;
            while (index < text.size() && text[index] != '\'')
            {
                if (text[index] == '\\' && index + 1 < text.size())
                    ++index;
                ++index;
            }
            ++index;
            continue;
        }
        if (character == '"')
        {
            const size_t literalOffset = index;
            ++index;
            std::string value;
            while (index < text.size() && text[index] != '"')
            {
                if (text[index] == '\\' && index + 1 < text.size())
                {
                    value += DecodeEscape(text[index + 1]);
                    index += 2;
                }
                else
                {
                    value.push_back(text[index]);
                    ++index;
                }
            }
            ++index;
            literals.push_back({ std::move(value), literalOffset });
            continue;
        }
        ++index;
    }
    return literals;
}

inline void SortUnique(std::vector<std::string>& values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

inline void CollectAttributeValues(
    std::string_view literal, std::string_view attribute,
    std::vector<std::string>& output)
{
    const std::string needle = std::string(attribute) + "=\"";
    size_t position = 0;
    while ((position = literal.find(needle, position)) != std::string_view::npos)
    {
        // `id="` must not match the tail of `data-id="` or a longer name.
        if (position > 0)
        {
            const char previous = literal[position - 1];
            if (std::isalnum(static_cast<unsigned char>(previous)) ||
                previous == '-' || previous == '_')
            {
                position += needle.size();
                continue;
            }
        }
        const size_t valueStart = position + needle.size();
        const size_t valueEnd = literal.find('"', valueStart);
        if (valueEnd == std::string_view::npos) return;
        output.emplace_back(literal.substr(valueStart, valueEnd - valueStart));
        position = valueEnd + 1;
    }
}

inline void CollectDataAttributes(
    std::string_view literal, std::vector<std::string>& output)
{
    size_t position = 0;
    while ((position = literal.find("data-", position)) != std::string_view::npos)
    {
        size_t cursor = position + 5;
        while (cursor < literal.size() &&
            (std::islower(static_cast<unsigned char>(literal[cursor])) ||
                literal[cursor] == '-'))
            ++cursor;
        if (cursor < literal.size() && literal[cursor] == '=' &&
            cursor > position + 5)
        {
            output.emplace_back(literal.substr(position, cursor - position));
        }
        position = cursor;
    }
}

// Identifier that directly precedes the opening parenthesis of the call whose
// first argument is this literal, or an empty view when it is not a first
// argument. Used to collect Rml element ids passed to panel helpers.
inline std::string_view CallNameForFirstArgument(
    std::string_view text, size_t literalOffset)
{
    size_t cursor = literalOffset;
    while (cursor > 0 &&
        (text[cursor - 1] == ' ' || text[cursor - 1] == '\t' ||
            text[cursor - 1] == '\n' || text[cursor - 1] == '\r'))
        --cursor;
    if (cursor == 0 || text[cursor - 1] != '(')
        return {};
    size_t end = cursor - 1;
    size_t begin = end;
    while (begin > 0 &&
        (std::isalnum(static_cast<unsigned char>(text[begin - 1])) ||
            text[begin - 1] == '_'))
        --begin;
    return text.substr(begin, end - begin);
}

inline bool IsIdHelper(std::string_view call)
{
    return call == "SetDisabled" || call == "GetElementById" ||
        call == "SetControlValue" || call == "SetProperty" ||
        call == "SetClass";
}

inline void CollectIdListIds(
    std::string_view text, std::vector<std::string>& ids)
{
    constexpr std::string_view marker = "const char* id :";
    size_t position = 0;
    while ((position = text.find(marker, position)) != std::string_view::npos)
    {
        const size_t brace = text.find('{', position);
        if (brace == std::string_view::npos) return;
        size_t depth = 0;
        size_t cursor = brace;
        for (; cursor < text.size(); ++cursor)
        {
            if (text[cursor] == '{') ++depth;
            else if (text[cursor] == '}')
            {
                if (--depth == 0) break;
            }
        }
        for (const Literal& literal :
            ExtractLiterals(text.substr(brace, cursor - brace + 1)))
            ids.push_back(literal.value);
        position = cursor;
    }
}

inline SourceSet Load(const std::filesystem::path& panelDirectory)
{
    SourceSet result;
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(panelDirectory))
    {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("particlePanel", 0) == 0 &&
            entry.path().extension() == ".cpp")
            paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths)
    {
        result.files.push_back(path.filename().string());
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const std::string text = buffer.str();
        std::vector<Literal> literals = ExtractLiterals(text);
        for (const Literal& literal : literals)
        {
            result.literals.push_back(literal.value);
            CollectAttributeValues(literal.value, "id", result.rmlIds);
            CollectAttributeValues(literal.value, "data-action", result.actions);
            CollectDataAttributes(literal.value, result.dataAttributes);
            const std::string_view call =
                CallNameForFirstArgument(text, literal.offset);
            if (IsIdHelper(call))
                result.idReferences.push_back(literal.value);
        }
        CollectIdListIds(text, result.idReferences);
    }
    std::sort(result.files.begin(), result.files.end());
    std::sort(result.literals.begin(), result.literals.end());
    SortUnique(result.rmlIds);
    SortUnique(result.idReferences);
    SortUnique(result.actions);
    SortUnique(result.dataAttributes);
    return result;
}

inline std::vector<std::string> ToSortedVector(
    const std::vector<std::string>& values)
{
    std::vector<std::string> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}
}
