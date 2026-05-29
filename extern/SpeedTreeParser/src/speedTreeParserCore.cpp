#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "SpeedTreeParser/speedTreeParserCore.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
    struct ObjVertex
    {
        std::array<float, 3> position{};
        std::array<float, 2> uv{};
    };

    struct WindVertex
    {
        std::array<uint8_t, 4> branch1{};
        std::array<uint8_t, 4> branch2{};
    };

    struct PrintableStringRecord
    {
        size_t offset = 0;
        std::string value;
    };

    struct MaterialSection
    {
        uint32_t materialSlot = 0;
        uint32_t unknown = 0;
        uint32_t startIndex = 0;
        uint32_t indexCount = 0;
    };

    struct GeometryBlock
    {
        size_t descriptorOffset = 0;
        size_t headerOffset = 0;
        size_t vertexDataOffset = 0;
        size_t indexDataOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t vertexStride = 0;
        uint32_t indexCount = 0;
        uint32_t indexSize = 0;
        std::vector<MaterialSection> sections;
    };

    struct Bounds
    {
        std::array<float, 3> min = {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()
        };
        std::array<float, 3> max = {
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()
        };
    };

    struct GeometryDescriptor
    {
        size_t descriptorOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t vertexStride = 0;
        std::vector<MaterialSection> sections;
    };

    std::string WideToUtf8(const std::wstring& value)
    {
#if defined(_WIN32)
        if (value.empty())
        {
            return {};
        }
        const int byteCount = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (byteCount <= 0)
        {
            return {};
        }
        std::string result(static_cast<size_t>(byteCount), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), byteCount, nullptr, nullptr);
        return result;
#else
        return std::string(value.begin(), value.end());
#endif
    }

    std::string PathToUtf8(const std::filesystem::path& path)
    {
#if defined(_WIN32)
        return WideToUtf8(path.wstring());
#else
        return path.u8string();
#endif
    }

    std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open input file: " + PathToUtf8(path));
        }

        file.unsetf(std::ios::skipws);
        return std::vector<uint8_t>(
            std::istream_iterator<uint8_t>(file),
            std::istream_iterator<uint8_t>());
    }

    bool IsPrintableStringByte(uint8_t value)
    {
        return value == 9 || value == 10 || value == 13 || (value >= 32 && value <= 126);
    }

    std::vector<std::string> ExtractPrintableStrings(const std::vector<uint8_t>& data, size_t minLength)
    {
        std::vector<std::string> strings;
        std::string current;
        for (uint8_t value : data)
        {
            if (IsPrintableStringByte(value))
            {
                current.push_back(static_cast<char>(value));
                continue;
            }

            if (current.size() >= minLength)
            {
                strings.push_back(current);
            }
            current.clear();
        }

        if (current.size() >= minLength)
        {
            strings.push_back(current);
        }
        return strings;
    }

    std::vector<PrintableStringRecord> ExtractPrintableStringRecords(const std::vector<uint8_t>& data, size_t minLength)
    {
        std::vector<PrintableStringRecord> strings;
        std::string current;
        size_t currentOffset = 0;
        bool readingString = false;

        for (size_t offset = 0; offset < data.size(); ++offset)
        {
            const uint8_t value = data[offset];
            if (IsPrintableStringByte(value))
            {
                if (!readingString)
                {
                    currentOffset = offset;
                    readingString = true;
                }
                current.push_back(static_cast<char>(value));
                continue;
            }

            if (current.size() >= minLength)
            {
                strings.push_back({ currentOffset, current });
            }
            current.clear();
            readingString = false;
        }

        if (current.size() >= minLength)
        {
            strings.push_back({ currentOffset, current });
        }
        return strings;
    }

    uint32_t ReadU32LE(const std::vector<uint8_t>& data, size_t offset)
    {
        if (offset + 4 > data.size())
        {
            throw std::runtime_error("ReadU32LE out of range");
        }
        return static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
    }

    uint16_t ReadU16LE(const std::vector<uint8_t>& data, size_t offset)
    {
        if (offset + 2 > data.size())
        {
            throw std::runtime_error("ReadU16LE out of range");
        }
        return static_cast<uint16_t>(data[offset]) |
            static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
    }

    float HalfToFloat(uint16_t value)
    {
        const float sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
        const int exponent = static_cast<int>((value >> 10u) & 0x1fu);
        const int mantissa = static_cast<int>(value & 0x03ffu);

        if (exponent == 0)
        {
            return sign * std::ldexp(static_cast<float>(mantissa), -24);
        }
        if (exponent == 31)
        {
            return mantissa == 0
                ? sign * std::numeric_limits<float>::infinity()
                : std::numeric_limits<float>::quiet_NaN();
        }
        return sign * std::ldexp(static_cast<float>(1024 + mantissa), exponent - 25);
    }

    float ReadHalfLE(const std::vector<uint8_t>& data, size_t offset)
    {
        return HalfToFloat(ReadU16LE(data, offset));
    }

    std::vector<uint32_t> ReadOffsetTable(const std::vector<uint8_t>& data)
    {
        if (data.size() < 20)
        {
            return {};
        }

        const uint32_t count = ReadU32LE(data, 16);
        const size_t tableStart = 20;
        const size_t tableEnd = tableStart + static_cast<size_t>(count) * 4;
        if (count == 0 || count > 4096 || tableEnd > data.size())
        {
            return {};
        }

        std::vector<uint32_t> offsets;
        offsets.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t offset = ReadU32LE(data, tableStart + static_cast<size_t>(i) * 4);
            if (offset >= data.size())
            {
                return {};
            }
            offsets.push_back(offset);
        }
        return offsets;
    }

    std::string Trim(const std::string& value)
    {
        const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
        if (begin >= end)
        {
            return {};
        }
        return std::string(begin, end);
    }

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    bool EndsWith(const std::string& value, const std::string& suffix)
    {
        return value.size() >= suffix.size() &&
            std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
    }

    bool IsTextureName(const std::string& value)
    {
        const std::string lower = ToLower(value);
        return EndsWith(lower, ".png") ||
            EndsWith(lower, ".tga") ||
            EndsWith(lower, ".jpg") ||
            EndsWith(lower, ".jpeg") ||
            EndsWith(lower, ".dds") ||
            EndsWith(lower, ".exr") ||
            EndsWith(lower, ".tif") ||
            EndsWith(lower, ".tiff");
    }

    std::vector<std::string> ExtractOrderedMaterialNames(const std::vector<std::string>& strings)
    {
        std::set<std::string> seen;
        std::vector<std::string> materials;
        for (const std::string& rawString : strings)
        {
            const std::string value = Trim(rawString);
            if (!EndsWith(value, "_Mat"))
            {
                continue;
            }
            if (seen.insert(value).second)
            {
                materials.push_back(value);
            }
        }
        return materials;
    }

    std::vector<std::string> ExtractStringValues(const std::vector<PrintableStringRecord>& stringRecords)
    {
        std::vector<std::string> values;
        values.reserve(stringRecords.size());
        for (const PrintableStringRecord& record : stringRecords)
        {
            values.push_back(record.value);
        }
        return values;
    }

    std::string StripXmlDeclaration(std::string value)
    {
        const std::string declaration = "<?xml version=\"1.0\"?>";
        if (value.find(declaration) == 0)
        {
            value.erase(0, declaration.size());
        }
        return Trim(value);
    }

    nlohmann::json StringRecordToJson(const PrintableStringRecord& record)
    {
        return {
            {"offset", record.offset},
            {"value", StripXmlDeclaration(Trim(record.value))}
        };
    }

    bool IsLikelyTitleName(const std::string& value)
    {
        static const std::regex titleRegex("^[A-Z][a-z]+(?: [A-Z][a-z]+)+$");
        return value.size() <= 80 && std::regex_match(value, titleRegex);
    }

    std::string HeaderAscii(const std::vector<uint8_t>& data)
    {
        const size_t count = std::min<size_t>(16, data.size());
        return std::string(reinterpret_cast<const char*>(data.data()), count);
    }

    std::map<std::string, std::string> ParseAttributes(const std::string& text)
    {
        std::map<std::string, std::string> attributes;
        static const std::regex attributeRegex("([A-Za-z0-9_]+)=\"([^\"]*)\"");
        auto begin = std::sregex_iterator(text.begin(), text.end(), attributeRegex);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it)
        {
            attributes[(*it)[1].str()] = (*it)[2].str();
        }
        return attributes;
    }

    nlohmann::json AttributesToJson(const std::map<std::string, std::string>& attributes)
    {
        nlohmann::json json = nlohmann::json::object();
        for (const auto& [name, value] : attributes)
        {
            json[name] = value;
        }
        return json;
    }

    std::vector<std::string> ExtractXmlBlocks(const std::string& text, const std::string& tagName)
    {
        std::vector<std::string> blocks;
        const std::string openTag = "<" + tagName;
        const std::string closeTag = "</" + tagName + ">";
        size_t searchOffset = 0;
        while (searchOffset < text.size())
        {
            const size_t start = text.find(openTag, searchOffset);
            if (start == std::string::npos)
            {
                break;
            }

            const size_t end = text.find(closeTag, start);
            if (end == std::string::npos)
            {
                break;
            }

            const size_t blockEnd = end + closeTag.size();
            blocks.push_back(text.substr(start, blockEnd - start));
            searchOffset = blockEnd;
        }
        return blocks;
    }

    nlohmann::json ParseVertexPackerBlock(const std::string& block)
    {
        nlohmann::json result;
        result["tag"] = "SpeedTreeVertexPacker";
        result["attributes"] = AttributesToJson(ParseAttributes(block.substr(0, block.find('>'))));
        result["streams"] = nlohmann::json::array();
        result["textures"] = nlohmann::json::array();
        result["raw"] = block;

        const std::vector<std::string> streamBlocks = ExtractXmlBlocks(block, "Stream");
        for (const std::string& streamBlock : streamBlocks)
        {
            nlohmann::json streamJson;
            streamJson["name"] = "";
            streamJson["attributes"] = nlohmann::json::array();

            const size_t streamTagEnd = streamBlock.find('>');
            if (streamTagEnd != std::string::npos)
            {
                const auto streamAttributes = ParseAttributes(streamBlock.substr(0, streamTagEnd));
                const auto nameIt = streamAttributes.find("Name");
                if (nameIt != streamAttributes.end())
                {
                    streamJson["name"] = nameIt->second;
                }
            }

            static const std::regex attributeTagRegex("<Attribute[^>]*/>");
            auto begin = std::sregex_iterator(streamBlock.begin(), streamBlock.end(), attributeTagRegex);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it)
            {
                streamJson["attributes"].push_back(AttributesToJson(ParseAttributes(it->str())));
            }
            result["streams"].push_back(streamJson);
        }
        return result;
    }

    nlohmann::json ParseTexturePackerBlock(const std::string& block)
    {
        nlohmann::json result;
        result["tag"] = "SpeedTreeTexturePacker";
        result["attributes"] = AttributesToJson(ParseAttributes(block.substr(0, block.find('>'))));
        result["streams"] = nlohmann::json::array();
        result["textures"] = nlohmann::json::array();
        result["raw"] = block;

        static const std::regex textureTagRegex("<(Texture[0-9]+)[^>]*/>");
        auto begin = std::sregex_iterator(block.begin(), block.end(), textureTagRegex);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it)
        {
            nlohmann::json textureJson = AttributesToJson(ParseAttributes(it->str()));
            textureJson["slot"] = (*it)[1].str();
            result["textures"].push_back(textureJson);
        }
        return result;
    }

    nlohmann::json BuildGlobalMetadataJson(const std::vector<PrintableStringRecord>& stringRecords)
    {
        nlohmann::json metadata;
        const std::map<std::string, std::string> keyNames = {
            {"Biome", "biome"},
            {"Description", "description"},
            {"ModelCategories", "modelCategories"},
            {"Names", "names"},
            {"ScientificNames", "scientificNames"}
        };

        for (size_t index = 0; index + 1 < stringRecords.size(); ++index)
        {
            const std::string key = Trim(stringRecords[index].value);
            const auto keyIt = keyNames.find(key);
            if (keyIt == keyNames.end())
            {
                continue;
            }

            metadata[keyIt->second] = {
                {"labelOffset", stringRecords[index].offset},
                {"valueOffset", stringRecords[index + 1].offset},
                {"value", Trim(stringRecords[index + 1].value)}
            };
        }
        return metadata;
    }

    nlohmann::json BuildMaterialAssetsJson(const std::vector<PrintableStringRecord>& stringRecords)
    {
        nlohmann::json materialAssets = nlohmann::json::array();
        for (size_t index = 0; index < stringRecords.size(); ++index)
        {
            const std::string materialName = Trim(stringRecords[index].value);
            if (!EndsWith(materialName, "_Mat"))
            {
                continue;
            }

            nlohmann::json textures = nlohmann::json::array();
            for (size_t nextIndex = index + 1; nextIndex < stringRecords.size(); ++nextIndex)
            {
                const std::string value = Trim(stringRecords[nextIndex].value);
                if (EndsWith(value, "_Mat"))
                {
                    break;
                }
                if (IsTextureName(value))
                {
                    textures.push_back({
                        {"offset", stringRecords[nextIndex].offset},
                        {"path", value}
                    });
                }
            }

            materialAssets.push_back({
                {"slot", materialAssets.size()},
                {"name", materialName},
                {"offset", stringRecords[index].offset},
                {"textures", textures}
            });
        }
        return materialAssets;
    }

    nlohmann::json BuildMetadataStringTableJson(const std::vector<PrintableStringRecord>& stringRecords, size_t metadataEndOffset)
    {
        nlohmann::json table = nlohmann::json::array();
        for (const PrintableStringRecord& record : stringRecords)
        {
            if (record.offset >= metadataEndOffset)
            {
                continue;
            }
            table.push_back(StringRecordToJson(record));
        }
        return table;
    }

    nlohmann::json BuildInterestingStringOccurrencesJson(const std::vector<PrintableStringRecord>& stringRecords)
    {
        nlohmann::json occurrences = nlohmann::json::array();
        for (const PrintableStringRecord& record : stringRecords)
        {
            const std::string value = Trim(record.value);
            if (value.empty())
            {
                continue;
            }

            std::string category;
            if (EndsWith(value, "_Mat"))
            {
                category = "material";
            }
            else if (IsTextureName(value))
            {
                category = "texture";
            }
            else if (value.find("SpeedTreeVertexPacker") != std::string::npos)
            {
                category = "vertexPackerXml";
            }
            else if (value.find("SpeedTreeTexturePacker") != std::string::npos)
            {
                category = "texturePackerXml";
            }
            else if (IsLikelyTitleName(value))
            {
                category = "title";
            }

            if (!category.empty())
            {
                occurrences.push_back({
                    {"offset", record.offset},
                    {"category", category},
                    {"value", StripXmlDeclaration(value)}
                });
            }
        }
        return occurrences;
    }

    std::string StringPreviewAtOffset(const std::vector<PrintableStringRecord>& stringRecords, size_t offset)
    {
        for (const PrintableStringRecord& record : stringRecords)
        {
            if (offset < record.offset || offset >= record.offset + record.value.size())
            {
                continue;
            }

            const size_t relativeOffset = offset - record.offset;
            std::string preview = record.value.substr(relativeOffset);
            if (preview.size() > 96)
            {
                preview = preview.substr(0, 96);
            }
            return StripXmlDeclaration(Trim(preview));
        }
        return {};
    }

    nlohmann::json BuildOffsetTableJson(const std::vector<uint32_t>& offsets, const std::vector<PrintableStringRecord>& stringRecords)
    {
        nlohmann::json entries = nlohmann::json::array();
        for (size_t index = 0; index < offsets.size(); ++index)
        {
            nlohmann::json entry = {
                {"index", index},
                {"offset", offsets[index]}
            };

            const std::string preview = StringPreviewAtOffset(stringRecords, offsets[index]);
            if (!preview.empty())
            {
                entry["stringPreview"] = preview;
            }
            entries.push_back(entry);
        }

        return {
            {"count", offsets.size()},
            {"offsets", offsets},
            {"entries", entries}
        };
    }

    nlohmann::json BuildPreprobeJson(
        const std::filesystem::path& inputPath,
        const std::vector<uint8_t>& data,
        const std::vector<PrintableStringRecord>& stringRecords,
        const std::vector<uint32_t>& offsets)
    {
        const std::vector<std::string> strings = ExtractStringValues(stringRecords);
        std::set<std::string> materials;
        std::set<std::string> textures;
        std::set<std::string> likelyTreeNames;
        const std::vector<std::string> orderedMaterials = ExtractOrderedMaterialNames(strings);
        for (const std::string& rawString : strings)
        {
            const std::string value = Trim(rawString);
            if (value.empty())
            {
                continue;
            }
            if (EndsWith(value, "_Mat"))
            {
                materials.insert(value);
            }
            if (IsTextureName(value))
            {
                textures.insert(value);
            }
            if (IsLikelyTitleName(value))
            {
                likelyTreeNames.insert(value);
            }
        }

        nlohmann::json materialSlots = nlohmann::json::array();
        for (size_t slot = 0; slot < orderedMaterials.size(); ++slot)
        {
            materialSlots.push_back({
                {"slot", slot},
                {"name", orderedMaterials[slot]}
            });
        }

        const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
        nlohmann::json vertexPackers = nlohmann::json::array();
        for (const std::string& block : ExtractXmlBlocks(text, "SpeedTreeVertexPacker"))
        {
            vertexPackers.push_back(ParseVertexPackerBlock(block));
        }

        nlohmann::json texturePackers = nlohmann::json::array();
        for (const std::string& block : ExtractXmlBlocks(text, "SpeedTreeTexturePacker"))
        {
            texturePackers.push_back(ParseTexturePackerBlock(block));
        }

        nlohmann::json result;
        result["type"] = "speedTreeStsdkPreprobe";
        result["note"] = "Generated by VulkanLearn SpeedTreeParserCore from shallow binary inspection. This is not an official SDK parse and must not define the final runtime format.";
        result["source"] = {
            {"path", PathToUtf8(inputPath)},
            {"fileName", PathToUtf8(inputPath.filename())},
            {"sizeBytes", data.size()},
            {"headerAscii", HeaderAscii(data)}
        };
        result["offsetTable"] = BuildOffsetTableJson(offsets, stringRecords);
        result["globalMetadata"] = BuildGlobalMetadataJson(stringRecords);
        result["discovered"] = {
            {"readableStringCount", strings.size()},
            {"metadataStringTable", BuildMetadataStringTableJson(stringRecords, 0x2300)},
            {"interestingStringOccurrences", BuildInterestingStringOccurrencesJson(stringRecords)},
            {"materials", std::vector<std::string>(materials.begin(), materials.end())},
            {"materialSlots", materialSlots},
            {"materialAssets", BuildMaterialAssetsJson(stringRecords)},
            {"textures", std::vector<std::string>(textures.begin(), textures.end())},
            {"likelyTreeNames", std::vector<std::string>(likelyTreeNames.begin(), likelyTreeNames.end())},
            {"vertexPackers", vertexPackers},
            {"texturePackers", texturePackers}
        };
        return result;
    }

    ObjVertex ReadStandardVertex(const std::vector<uint8_t>& data, size_t vertexOffset)
    {
        ObjVertex vertex;
        vertex.position = {
            ReadHalfLE(data, vertexOffset + 0),
            ReadHalfLE(data, vertexOffset + 2),
            ReadHalfLE(data, vertexOffset + 4)
        };
        vertex.uv = {
            ReadHalfLE(data, vertexOffset + 6),
            ReadHalfLE(data, vertexOffset + 14)
        };
        return vertex;
    }

    WindVertex ReadStandardWindVertex(const std::vector<uint8_t>& data, size_t vertexOffset)
    {
        WindVertex wind;
        wind.branch1 = {
            data[vertexOffset + 20],
            data[vertexOffset + 21],
            data[vertexOffset + 22],
            data[vertexOffset + 23]
        };
        wind.branch2 = {
            data[vertexOffset + 24],
            data[vertexOffset + 25],
            data[vertexOffset + 26],
            data[vertexOffset + 27]
        };
        return wind;
    }

    ObjVertex ReadBillboardVertex(const std::vector<uint8_t>& data, size_t vertexOffset)
    {
        ObjVertex vertex;
        vertex.position = {
            ReadHalfLE(data, vertexOffset + 0),
            ReadHalfLE(data, vertexOffset + 2),
            ReadHalfLE(data, vertexOffset + 4)
        };
        vertex.uv = {
            ReadHalfLE(data, vertexOffset + 8),
            ReadHalfLE(data, vertexOffset + 10)
        };
        return vertex;
    }

    bool HasMainMarker(const std::vector<uint8_t>& data, size_t offset)
    {
        return offset + 8 <= data.size() &&
            data[offset + 0] == 'M' &&
            data[offset + 1] == 'a' &&
            data[offset + 2] == 'i' &&
            data[offset + 3] == 'n' &&
            data[offset + 4] == 0 &&
            data[offset + 5] == 0 &&
            data[offset + 6] == 0 &&
            data[offset + 7] == 0;
    }

    std::vector<GeometryDescriptor> FindGeometryDescriptors(const std::vector<uint8_t>& data)
    {
        std::vector<GeometryDescriptor> descriptors;
        for (size_t offset = 0; offset + 24 < data.size(); ++offset)
        {
            if (!HasMainMarker(data, offset))
            {
                continue;
            }

            const uint32_t vertexStride = ReadU32LE(data, offset + 8);
            const uint32_t vertexCount = ReadU32LE(data, offset + 12);
            const uint32_t sectionCount = ReadU32LE(data, offset + 16);
            if ((vertexStride != 16 && vertexStride != 28) || vertexCount == 0 || vertexCount > 1000000 || sectionCount == 0 || sectionCount > 32)
            {
                continue;
            }

            const size_t sectionsOffset = offset + 20;
            if (sectionsOffset + static_cast<size_t>(sectionCount) * 16 > data.size())
            {
                continue;
            }

            GeometryDescriptor descriptor;
            descriptor.descriptorOffset = offset;
            descriptor.vertexCount = vertexCount;
            descriptor.vertexStride = vertexStride;

            uint32_t expectedStartIndex = 0;
            bool valid = true;
            for (uint32_t sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex)
            {
                const size_t sectionOffset = sectionsOffset + static_cast<size_t>(sectionIndex) * 16;
                MaterialSection section;
                section.materialSlot = ReadU32LE(data, sectionOffset + 0);
                section.unknown = ReadU32LE(data, sectionOffset + 4);
                section.startIndex = ReadU32LE(data, sectionOffset + 8);
                section.indexCount = ReadU32LE(data, sectionOffset + 12);

                if (section.unknown != 0 || section.indexCount == 0 || section.startIndex != expectedStartIndex)
                {
                    valid = false;
                    break;
                }

                expectedStartIndex += section.indexCount;
                descriptor.sections.push_back(section);
            }

            if (valid && expectedStartIndex > 0)
            {
                descriptors.push_back(descriptor);
            }
        }
        return descriptors;
    }

    uint32_t ReadIndex(const std::vector<uint8_t>& data, const GeometryBlock& block, uint32_t indexIndex)
    {
        const size_t offset = block.indexDataOffset + static_cast<size_t>(indexIndex) * block.indexSize;
        if (block.indexSize == 2)
        {
            return ReadU16LE(data, offset);
        }
        return ReadU32LE(data, offset);
    }

    bool IsPlausibleGeometryBlock(const std::vector<uint8_t>& data, const GeometryBlock& block)
    {
        if (block.vertexCount == 0 || block.vertexCount > 1000000)
        {
            return false;
        }
        if (block.vertexStride != 16 && block.vertexStride != 28)
        {
            return false;
        }
        if (block.indexCount == 0 || block.indexCount > 2000000 || (block.indexSize != 2 && block.indexSize != 4))
        {
            return false;
        }
        if (block.vertexDataOffset + static_cast<size_t>(block.vertexCount) * block.vertexStride > data.size())
        {
            return false;
        }
        if (block.indexDataOffset + static_cast<size_t>(block.indexCount) * block.indexSize > data.size())
        {
            return false;
        }

        const uint32_t validationCount = std::min<uint32_t>(block.indexCount, 512);
        for (uint32_t indexIndex = 0; indexIndex < validationCount; ++indexIndex)
        {
            if (ReadIndex(data, block, indexIndex) >= block.vertexCount)
            {
                return false;
            }
        }
        return true;
    }

    std::vector<GeometryBlock> FindGeometryBlocks(const std::vector<uint8_t>& data)
    {
        const std::vector<GeometryDescriptor> descriptors = FindGeometryDescriptors(data);
        std::vector<bool> usedDescriptors(descriptors.size(), false);
        std::vector<GeometryBlock> blocks;
        for (size_t offset = 0; offset + 24 < data.size(); ++offset)
        {
            if (!HasMainMarker(data, offset))
            {
                continue;
            }

            GeometryBlock block;
            block.headerOffset = offset;
            block.vertexCount = ReadU32LE(data, offset + 8);
            block.vertexStride = ReadU32LE(data, offset + 12);
            block.vertexDataOffset = offset + 16;

            const size_t vertexEnd = block.vertexDataOffset + static_cast<size_t>(block.vertexCount) * block.vertexStride;
            if (vertexEnd + 8 > data.size())
            {
                continue;
            }

            block.indexCount = ReadU32LE(data, vertexEnd);
            block.indexSize = ReadU32LE(data, vertexEnd + 4);
            block.indexDataOffset = vertexEnd + 8;
            if (IsPlausibleGeometryBlock(data, block))
            {
                for (size_t descriptorIndex = 0; descriptorIndex < descriptors.size(); ++descriptorIndex)
                {
                    const GeometryDescriptor& descriptor = descriptors[descriptorIndex];
                    if (usedDescriptors[descriptorIndex] ||
                        descriptor.vertexCount != block.vertexCount ||
                        descriptor.vertexStride != block.vertexStride)
                    {
                        continue;
                    }

                    uint32_t describedIndexCount = 0;
                    for (const MaterialSection& section : descriptor.sections)
                    {
                        describedIndexCount += section.indexCount;
                    }
                    if (describedIndexCount == block.indexCount)
                    {
                        block.descriptorOffset = descriptor.descriptorOffset;
                        block.sections = descriptor.sections;
                        usedDescriptors[descriptorIndex] = true;
                        break;
                    }
                }
                blocks.push_back(block);
            }
        }
        return blocks;
    }

    ObjVertex ReadVertex(const std::vector<uint8_t>& data, const GeometryBlock& block, uint32_t sourceIndex)
    {
        const size_t vertexOffset = block.vertexDataOffset + static_cast<size_t>(sourceIndex) * block.vertexStride;
        if (block.vertexStride == 16)
        {
            return ReadBillboardVertex(data, vertexOffset);
        }
        return ReadStandardVertex(data, vertexOffset);
    }

    std::string MaterialNameForSlot(const std::vector<std::string>& materialNames, uint32_t materialSlot)
    {
        if (materialSlot < materialNames.size())
        {
            return materialNames[materialSlot];
        }
        return "speedtree_material_slot_" + std::to_string(materialSlot);
    }

    void WriteExperimentalMtl(const std::filesystem::path& outputPath, const std::vector<std::string>& materialNames, uint32_t maxMaterialSlot)
    {
        std::filesystem::path mtlPath = outputPath;
        mtlPath.replace_extension(".mtl");
        std::ofstream mtlFile(mtlPath);
        if (!mtlFile.is_open())
        {
            throw std::runtime_error("Failed to open MTL output file: " + PathToUtf8(mtlPath));
        }

        const std::array<std::array<float, 3>, 8> colors = { {
            {0.45f, 0.27f, 0.12f},
            {0.24f, 0.36f, 0.16f},
            {0.20f, 0.50f, 0.20f},
            {0.55f, 0.65f, 0.25f},
            {0.18f, 0.42f, 0.18f},
            {0.50f, 0.35f, 0.70f},
            {0.35f, 0.28f, 0.18f},
            {0.45f, 0.45f, 0.45f}
        } };

        for (uint32_t materialSlot = 0; materialSlot <= maxMaterialSlot; ++materialSlot)
        {
            const std::array<float, 3>& color = colors[materialSlot % colors.size()];
            mtlFile
                << "newmtl " << MaterialNameForSlot(materialNames, materialSlot) << "\n"
                << "Kd " << color[0] << " " << color[1] << " " << color[2] << "\n"
                << "Ks 0.05 0.05 0.05\n"
                << "Ns 16\n\n";
        }
    }

    void IncludePoint(Bounds& bounds, const std::array<float, 3>& point)
    {
        for (size_t axis = 0; axis < 3; ++axis)
        {
            bounds.min[axis] = std::min(bounds.min[axis], point[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], point[axis]);
        }
    }

    Bounds ComputeVertexBounds(const std::vector<uint8_t>& data, const GeometryBlock& block)
    {
        Bounds bounds;
        for (uint32_t vertexIndex = 0; vertexIndex < block.vertexCount; ++vertexIndex)
        {
            IncludePoint(bounds, ReadVertex(data, block, vertexIndex).position);
        }
        return bounds;
    }

    nlohmann::json BoundsToJson(const Bounds& bounds)
    {
        return {
            {"min", bounds.min},
            {"max", bounds.max}
        };
    }

    nlohmann::json ComputeIndexStatsJson(const std::vector<uint8_t>& data, const GeometryBlock& block)
    {
        uint32_t minIndex = std::numeric_limits<uint32_t>::max();
        uint32_t maxIndex = 0;
        uint32_t degenerateTriangleCount = 0;
        const uint32_t faceIndexCount = block.indexCount - block.indexCount % 3;
        for (uint32_t indexIndex = 0; indexIndex < block.indexCount; ++indexIndex)
        {
            const uint32_t index = ReadIndex(data, block, indexIndex);
            minIndex = std::min(minIndex, index);
            maxIndex = std::max(maxIndex, index);
        }
        for (uint32_t indexIndex = 0; indexIndex < faceIndexCount; indexIndex += 3)
        {
            const uint32_t a = ReadIndex(data, block, indexIndex + 0);
            const uint32_t b = ReadIndex(data, block, indexIndex + 1);
            const uint32_t c = ReadIndex(data, block, indexIndex + 2);
            if (a == b || b == c || a == c)
            {
                ++degenerateTriangleCount;
            }
        }

        return {
            {"minIndex", minIndex},
            {"maxIndex", maxIndex},
            {"triangleCount", faceIndexCount / 3},
            {"degenerateTriangleCount", degenerateTriangleCount}
        };
    }

    nlohmann::json BuildUByteChannelStatsJson(const std::array<uint64_t, 4>& sums, const std::array<uint8_t, 4>& mins, const std::array<uint8_t, 4>& maxs, uint32_t vertexCount)
    {
        nlohmann::json channels = nlohmann::json::array();
        for (size_t channelIndex = 0; channelIndex < 4; ++channelIndex)
        {
            const double average = vertexCount == 0 ? 0.0 : static_cast<double>(sums[channelIndex]) / static_cast<double>(vertexCount);
            channels.push_back({
                {"minByte", mins[channelIndex]},
                {"maxByte", maxs[channelIndex]},
                {"avgByte", average},
                {"minNormalized", static_cast<double>(mins[channelIndex]) / 255.0},
                {"maxNormalized", static_cast<double>(maxs[channelIndex]) / 255.0},
                {"avgNormalized", average / 255.0}
            });
        }
        return channels;
    }

    nlohmann::json WindVertexToJson(const WindVertex& wind)
    {
        return {
            {"branch1", {
                {"weight", wind.branch1[0]},
                {"dir", wind.branch1[1]},
                {"offset", wind.branch1[2]},
                {"ripple", wind.branch1[3]}
            }},
            {"branch2", {
                {"weight", wind.branch2[0]},
                {"dir", wind.branch2[1]},
                {"offset", wind.branch2[2]},
                {"blend", wind.branch2[3]}
            }}
        };
    }

    nlohmann::json ComputeWindStatsJson(const std::vector<uint8_t>& data, const GeometryBlock& block)
    {
        if (block.vertexStride != 28)
        {
            return {
                {"present", false},
                {"reason", "Current parser only maps wind channels for Standard.lua stride 28 geometry."}
            };
        }

        std::array<uint64_t, 4> branch1Sums = { 0, 0, 0, 0 };
        std::array<uint64_t, 4> branch2Sums = { 0, 0, 0, 0 };
        std::array<uint8_t, 4> branch1Mins = { 255, 255, 255, 255 };
        std::array<uint8_t, 4> branch2Mins = { 255, 255, 255, 255 };
        std::array<uint8_t, 4> branch1Maxs = { 0, 0, 0, 0 };
        std::array<uint8_t, 4> branch2Maxs = { 0, 0, 0, 0 };

        uint32_t branch1WeightedVertexCount = 0;
        uint32_t branch2WeightedVertexCount = 0;
        uint32_t rippleVertexCount = 0;
        nlohmann::json samples = nlohmann::json::array();

        for (uint32_t vertexIndex = 0; vertexIndex < block.vertexCount; ++vertexIndex)
        {
            const size_t vertexOffset = block.vertexDataOffset + static_cast<size_t>(vertexIndex) * block.vertexStride;
            const WindVertex wind = ReadStandardWindVertex(data, vertexOffset);
            for (size_t channelIndex = 0; channelIndex < 4; ++channelIndex)
            {
                branch1Sums[channelIndex] += wind.branch1[channelIndex];
                branch2Sums[channelIndex] += wind.branch2[channelIndex];
                branch1Mins[channelIndex] = std::min(branch1Mins[channelIndex], wind.branch1[channelIndex]);
                branch2Mins[channelIndex] = std::min(branch2Mins[channelIndex], wind.branch2[channelIndex]);
                branch1Maxs[channelIndex] = std::max(branch1Maxs[channelIndex], wind.branch1[channelIndex]);
                branch2Maxs[channelIndex] = std::max(branch2Maxs[channelIndex], wind.branch2[channelIndex]);
            }

            if (wind.branch1[0] > 0)
            {
                ++branch1WeightedVertexCount;
            }
            if (wind.branch2[0] > 0)
            {
                ++branch2WeightedVertexCount;
            }
            if (wind.branch1[3] > 0)
            {
                ++rippleVertexCount;
            }

            if (samples.size() < 8 && (wind.branch1[0] > 0 || wind.branch2[0] > 0 || wind.branch1[3] > 0))
            {
                nlohmann::json sample = WindVertexToJson(wind);
                sample["vertexIndex"] = vertexIndex;
                samples.push_back(sample);
            }
        }

        return {
            {"present", true},
            {"encoding", "Standard.lua ubyte4 normalized at vertex byte offsets 20 and 24"},
            {"branch1Layout", {
                {"channel0", "wind_branch1_weight"},
                {"channel1", "wind_branch1_dir"},
                {"channel2", "wind_branch1_offset"},
                {"channel3", "wind_ripple"}
            }},
            {"branch2Layout", {
                {"channel0", "wind_branch2_weight"},
                {"channel1", "wind_branch2_dir"},
                {"channel2", "wind_branch2_offset"},
                {"channel3", "blend"}
            }},
            {"branch1WeightedVertexCount", branch1WeightedVertexCount},
            {"branch2WeightedVertexCount", branch2WeightedVertexCount},
            {"rippleVertexCount", rippleVertexCount},
            {"branch1ChannelStats", BuildUByteChannelStatsJson(branch1Sums, branch1Mins, branch1Maxs, block.vertexCount)},
            {"branch2ChannelStats", BuildUByteChannelStatsJson(branch2Sums, branch2Mins, branch2Maxs, block.vertexCount)},
            {"samples", samples}
        };
    }

    nlohmann::json GeometryBlocksToJson(
        const std::vector<uint8_t>& data,
        const std::vector<GeometryBlock>& blocks,
        const std::vector<std::string>& materialNames)
    {
        nlohmann::json geometryBlocks = nlohmann::json::array();
        for (size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex)
        {
            const GeometryBlock& block = blocks[blockIndex];
            const Bounds bounds = ComputeVertexBounds(data, block);
            nlohmann::json sections = nlohmann::json::array();
            for (const MaterialSection& section : block.sections)
            {
                const uint32_t sectionFaceIndexCount = section.indexCount - section.indexCount % 3;
                sections.push_back({
                    {"materialSlot", section.materialSlot},
                    {"materialName", MaterialNameForSlot(materialNames, section.materialSlot)},
                    {"startIndex", section.startIndex},
                    {"indexCount", section.indexCount},
                    {"triangleCount", sectionFaceIndexCount / 3}
                });
            }

            geometryBlocks.push_back({
                {"blockIndex", blockIndex},
                {"descriptorOffset", block.descriptorOffset},
                {"headerOffset", block.headerOffset},
                {"vertexDataOffset", block.vertexDataOffset},
                {"indexDataOffset", block.indexDataOffset},
                {"vertexCount", block.vertexCount},
                {"vertexStride", block.vertexStride},
                {"indexCount", block.indexCount},
                {"indexSize", block.indexSize},
                {"bounds", BoundsToJson(bounds)},
                {"indexStats", ComputeIndexStatsJson(data, block)},
                {"wind", ComputeWindStatsJson(data, block)},
                {"sections", sections}
            });
        }
        return geometryBlocks;
    }

    nlohmann::json BuildBinaryLayoutJson(const std::vector<uint8_t>& data, const std::vector<uint32_t>& offsets, const std::vector<GeometryBlock>& blocks)
    {
        const size_t offsetTableEnd = offsets.empty() ? 20 : 20 + offsets.size() * 4;
        nlohmann::json regions = nlohmann::json::array();
        regions.push_back({
            {"name", "header"},
            {"begin", 0},
            {"end", 16},
            {"size", 16}
        });
        regions.push_back({
            {"name", "offsetTable"},
            {"begin", 16},
            {"end", offsetTableEnd},
            {"size", offsetTableEnd - 16}
        });

        size_t firstGeometryHeader = data.size();
        for (const GeometryBlock& block : blocks)
        {
            firstGeometryHeader = std::min(firstGeometryHeader, block.headerOffset);
        }
        regions.push_back({
            {"name", "metadataAndDescriptors"},
            {"begin", offsetTableEnd},
            {"end", firstGeometryHeader},
            {"size", firstGeometryHeader - offsetTableEnd}
        });

        for (size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex)
        {
            const GeometryBlock& block = blocks[blockIndex];
            const size_t vertexEnd = block.vertexDataOffset + static_cast<size_t>(block.vertexCount) * block.vertexStride;
            const size_t indexEnd = block.indexDataOffset + static_cast<size_t>(block.indexCount) * block.indexSize;
            regions.push_back({
                {"name", "geometryBlock" + std::to_string(blockIndex) + ".vertexData"},
                {"begin", block.vertexDataOffset},
                {"end", vertexEnd},
                {"size", vertexEnd - block.vertexDataOffset}
            });
            regions.push_back({
                {"name", "geometryBlock" + std::to_string(blockIndex) + ".indexData"},
                {"begin", block.indexDataOffset},
                {"end", indexEnd},
                {"size", indexEnd - block.indexDataOffset}
            });
        }

        return {
            {"regions", regions}
        };
    }

    void ExportExperimentalObj(
        const std::filesystem::path& inputPath,
        const std::vector<uint8_t>& data,
        const std::vector<std::string>& materialNames,
        const std::filesystem::path& outputPath)
    {
        const std::vector<GeometryBlock> blocks = FindGeometryBlocks(data);
        if (blocks.empty())
        {
            throw std::runtime_error("Experimental OBJ probe did not find any geometry blocks");
        }

        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(outputPath.parent_path());
        }

        std::ofstream objFile(outputPath);
        if (!objFile.is_open())
        {
            throw std::runtime_error("Failed to open OBJ output file: " + PathToUtf8(outputPath));
        }

        uint32_t maxMaterialSlot = 0;
        for (const GeometryBlock& block : blocks)
        {
            for (const MaterialSection& section : block.sections)
            {
                maxMaterialSlot = std::max(maxMaterialSlot, section.materialSlot);
            }
        }
        WriteExperimentalMtl(outputPath, materialNames, maxMaterialSlot);
        std::filesystem::path mtlPath = outputPath;
        mtlPath.replace_extension(".mtl");

        objFile
            << "# Experimental OBJ exported by VulkanLearn SpeedTreeParserCore\n"
            << "# Source: " << PathToUtf8(inputPath) << "\n"
            << "# Geometry block count: " << blocks.size() << "\n"
            << "# This is a parser probe, not a final SpeedTree runtime conversion.\n"
            << "mtllib " << PathToUtf8(mtlPath.filename()) << "\n";

        uint32_t globalVertexBase = 1;
        for (size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex)
        {
            const GeometryBlock& block = blocks[blockIndex];
            objFile
                << "o speedtree_block_" << blockIndex
                << "_v" << block.vertexCount
                << "_i" << block.indexCount
                << "_stride" << block.vertexStride << "\n"
                << "# headerOffset=0x" << std::hex << block.headerOffset
                << " vertexDataOffset=0x" << block.vertexDataOffset
                << " indexDataOffset=0x" << block.indexDataOffset
                << std::dec << " indexSize=" << block.indexSize << "\n";

            for (uint32_t vertexIndex = 0; vertexIndex < block.vertexCount; ++vertexIndex)
            {
                const ObjVertex vertex = ReadVertex(data, block, vertexIndex);
                objFile << "v " << vertex.position[0] << " " << vertex.position[1] << " " << vertex.position[2] << "\n";
            }
            for (uint32_t vertexIndex = 0; vertexIndex < block.vertexCount; ++vertexIndex)
            {
                const ObjVertex vertex = ReadVertex(data, block, vertexIndex);
                objFile << "vt " << vertex.uv[0] << " " << (1.0f - vertex.uv[1]) << "\n";
            }

            std::vector<MaterialSection> sections = block.sections;
            if (sections.empty())
            {
                sections.push_back({ 0, 0, 0, block.indexCount });
            }

            for (const MaterialSection& section : sections)
            {
                objFile
                    << "g speedtree_block_" << blockIndex << "_slot_" << section.materialSlot << "\n"
                    << "usemtl " << MaterialNameForSlot(materialNames, section.materialSlot) << "\n";

                uint32_t sectionEnd = std::min(section.startIndex + section.indexCount, block.indexCount);
                sectionEnd -= (sectionEnd - section.startIndex) % 3;
                for (uint32_t indexIndex = section.startIndex; indexIndex < sectionEnd; indexIndex += 3)
                {
                    const uint32_t a = globalVertexBase + ReadIndex(data, block, indexIndex + 0);
                    const uint32_t b = globalVertexBase + ReadIndex(data, block, indexIndex + 1);
                    const uint32_t c = globalVertexBase + ReadIndex(data, block, indexIndex + 2);
                    if (a == b || b == c || a == c)
                    {
                        continue;
                    }
                    objFile << "f " << a << "/" << a << " " << b << "/" << b << " " << c << "/" << c << "\n";
                }
            }
            globalVertexBase += block.vertexCount;
        }
    }
}

void SpeedTreeParser::WriteProbeFiles(const ProbeOptions& options)
{
    std::filesystem::path outputPath = options.outputPath;
    if (outputPath.empty())
    {
        outputPath = options.inputPath;
        outputPath.replace_extension(".preprobe.json");
    }

    const std::vector<uint8_t> data = ReadBinaryFile(options.inputPath);
    const std::vector<PrintableStringRecord> stringRecords = ExtractPrintableStringRecords(data, options.minStringLength);
    const std::vector<std::string> strings = ExtractStringValues(stringRecords);
    const std::vector<uint32_t> offsets = ReadOffsetTable(data);
    const std::vector<std::string> materialNames = ExtractOrderedMaterialNames(strings);
    const std::vector<GeometryBlock> geometryBlocks = FindGeometryBlocks(data);
    nlohmann::json probeJson = BuildPreprobeJson(options.inputPath, data, stringRecords, offsets);
    probeJson["discovered"]["geometryBlocks"] = GeometryBlocksToJson(data, geometryBlocks, materialNames);
    probeJson["discovered"]["binaryLayout"] = BuildBinaryLayoutJson(data, offsets, geometryBlocks);

    if (!outputPath.parent_path().empty())
    {
        std::filesystem::create_directories(outputPath.parent_path());
    }
    std::ofstream outputFile(outputPath);
    if (!outputFile.is_open())
    {
        throw std::runtime_error("Failed to open output file: " + PathToUtf8(outputPath));
    }
    outputFile << probeJson.dump(2) << std::endl;

    if (!options.objOutputPath.empty())
    {
        ExportExperimentalObj(options.inputPath, data, materialNames, options.objOutputPath);
    }
}
