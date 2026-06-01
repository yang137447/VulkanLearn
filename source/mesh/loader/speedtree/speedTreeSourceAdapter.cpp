#include "speedTreeSourceAdapter.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "../common/mikkTSpaceMeshGenerator.h"

namespace
{
    struct PackedSpeedTreeVertex
    {
        Vertex vertex;
        SpeedTreeVertexAux aux;
    };

    struct MaterialSection
    {
        uint32_t materialSlot = 0;
        uint32_t unknown = 0;
        uint32_t startIndex = 0;
        uint32_t indexCount = 0;
    };

    struct GeometryDescriptor
    {
        size_t descriptorOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t vertexStride = 0;
        std::vector<MaterialSection> sections;
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

    std::vector<uint8_t> ReadBinaryFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open SpeedTree source file: " + path);
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

    bool EndsWith(const std::string& value, const std::string& suffix)
    {
        return value.size() >= suffix.size() &&
            std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
    }

    std::vector<std::string> ExtractOrderedMaterialNames(const std::vector<std::string>& strings)
    {
        std::vector<std::string> materials;
        for (const std::string& rawString : strings)
        {
            const std::string value = Trim(rawString);
            if (!EndsWith(value, "_Mat"))
            {
                continue;
            }
            if (std::find(materials.begin(), materials.end(), value) == materials.end())
            {
                materials.push_back(value);
            }
        }
        return materials;
    }

    uint16_t ReadU16LE(const std::vector<uint8_t>& data, size_t offset)
    {
        if (offset + 2 > data.size())
        {
            throw std::runtime_error("ReadU16LE out of range while parsing SpeedTree source");
        }
        return static_cast<uint16_t>(data[offset]) |
            static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
    }

    uint32_t ReadU32LE(const std::vector<uint8_t>& data, size_t offset)
    {
        if (offset + 4 > data.size())
        {
            throw std::runtime_error("ReadU32LE out of range while parsing SpeedTree source");
        }
        return static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
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
            if (!IsPlausibleGeometryBlock(data, block))
            {
                continue;
            }

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
        return blocks;
    }

    std::string MaterialNameForSlot(const std::vector<std::string>& materialNames, uint32_t materialSlot)
    {
        if (materialSlot < materialNames.size())
        {
            return materialNames[materialSlot];
        }
        return "speedtree_material_slot_" + std::to_string(materialSlot);
    }

    std::vector<std::string> BuildMaterialNamesForSections(
        const std::vector<MaterialSection>& sections,
        const std::vector<std::string>& materialNames)
    {
        std::vector<std::string> usedMaterialNames;
        for (const MaterialSection& section : sections)
        {
            const std::string materialName = MaterialNameForSlot(materialNames, section.materialSlot);
            if (std::find(usedMaterialNames.begin(), usedMaterialNames.end(), materialName) == usedMaterialNames.end())
            {
                usedMaterialNames.push_back(materialName);
            }
        }
        return usedMaterialNames;
    }

    Eigen::Vector3f ConvertSpeedTreePosition(const Eigen::Vector3f& position)
    {
        return Eigen::Vector3f(position.x(), position.z(), -position.y());
    }

    Eigen::Vector3f ConvertSpeedTreeVector(const Eigen::Vector3f& vector)
    {
        return Eigen::Vector3f(vector.x(), vector.z(), -vector.y());
    }

    Eigen::Vector3f DecodeSpeedTreePackedDirection(uint8_t packedDirection)
    {
        constexpr float DirectionTableMaxIndex = 255.0f;
        constexpr float GoldenAngle = 2.39996323f;

        const float tableIndex = static_cast<float>(packedDirection);
        const float y = 1.0f - 2.0f * tableIndex / DirectionTableMaxIndex;
        const float radial = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float angle = -tableIndex * GoldenAngle;
        return Eigen::Vector3f(
            radial * std::cos(angle),
            y,
            radial * std::sin(angle));
    }

    size_t SelectHighestLodBlockIndex(const std::vector<GeometryBlock>& blocks)
    {
        size_t selectedBlockIndex = blocks.size();
        uint32_t selectedVertexCount = 0;
        for (size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex)
        {
            const GeometryBlock& block = blocks[blockIndex];
            if (block.vertexStride != 28)
            {
                continue;
            }
            if (selectedBlockIndex == blocks.size() || block.vertexCount > selectedVertexCount)
            {
                selectedBlockIndex = blockIndex;
                selectedVertexCount = block.vertexCount;
            }
        }

        if (selectedBlockIndex != blocks.size())
        {
            return selectedBlockIndex;
        }
        return 0;
    }

    PackedSpeedTreeVertex ReadStandardVertex(const std::vector<uint8_t>& data, const GeometryBlock& block, uint32_t sourceIndex)
    {
        const size_t vertexOffset = block.vertexDataOffset + static_cast<size_t>(sourceIndex) * block.vertexStride;
        PackedSpeedTreeVertex result;
        const Eigen::Vector3f sourcePosition(
            ReadHalfLE(data, vertexOffset + 0),
            ReadHalfLE(data, vertexOffset + 2),
            ReadHalfLE(data, vertexOffset + 4));
        result.vertex.position = ConvertSpeedTreePosition(sourcePosition);
        result.vertex.texCoord = Eigen::Vector2f(
            ReadHalfLE(data, vertexOffset + 6),
            ReadHalfLE(data, vertexOffset + 14));
        result.vertex.color = Eigen::Vector3f::Ones();
        result.vertex.normal = Eigen::Vector3f::Zero();
        result.vertex.tangent = Eigen::Vector4f::Zero();

        const Eigen::Vector3f sourceLodPosition(
            ReadHalfLE(data, vertexOffset + 8),
            ReadHalfLE(data, vertexOffset + 10),
            ReadHalfLE(data, vertexOffset + 12));
        result.aux.lodPosition = ConvertSpeedTreePosition(sourceLodPosition);
        result.aux.sourcePackedNormal = data[vertexOffset + 16];
        result.aux.sourcePackedBinormal = data[vertexOffset + 17];
        result.aux.sourcePackedTangent = data[vertexOffset + 18];
        result.aux.ambientOcclusion = data[vertexOffset + 19];
        result.aux.sourcePackedTbnAo = {
            result.aux.sourcePackedNormal,
            result.aux.sourcePackedBinormal,
            result.aux.sourcePackedTangent,
            result.aux.ambientOcclusion
        };
        result.aux.sourceNormal = DecodeSpeedTreePackedDirection(result.aux.sourcePackedNormal);
        result.aux.sourceBinormal = DecodeSpeedTreePackedDirection(result.aux.sourcePackedBinormal);
        result.aux.sourceTangent = DecodeSpeedTreePackedDirection(result.aux.sourcePackedTangent);
        result.aux.hasSourceNormalVector = true;
        result.aux.hasSourceBinormalVector = true;
        result.aux.hasSourceTangentVector = true;
        result.vertex.normal = result.aux.sourceNormal;
        result.aux.windBranch1 = {
            data[vertexOffset + 20],
            data[vertexOffset + 21],
            data[vertexOffset + 22],
            data[vertexOffset + 23]
        };
        result.aux.windBranch2 = {
            data[vertexOffset + 24],
            data[vertexOffset + 25],
            data[vertexOffset + 26],
            data[vertexOffset + 27]
        };
        return result;
    }

    PackedSpeedTreeVertex ReadBillboardVertex(const std::vector<uint8_t>& data, const GeometryBlock& block, uint32_t sourceIndex)
    {
        const size_t vertexOffset = block.vertexDataOffset + static_cast<size_t>(sourceIndex) * block.vertexStride;
        PackedSpeedTreeVertex result;
        const Eigen::Vector3f sourcePosition(
            ReadHalfLE(data, vertexOffset + 0),
            ReadHalfLE(data, vertexOffset + 2),
            ReadHalfLE(data, vertexOffset + 4));
        result.vertex.position = ConvertSpeedTreePosition(sourcePosition);
        result.vertex.texCoord = Eigen::Vector2f(
            ReadHalfLE(data, vertexOffset + 8),
            ReadHalfLE(data, vertexOffset + 10));
        result.vertex.color = Eigen::Vector3f::Ones();
        const Eigen::Vector3f sourceNormal(
            ReadHalfLE(data, vertexOffset + 6),
            ReadHalfLE(data, vertexOffset + 12),
            ReadHalfLE(data, vertexOffset + 14));
        result.vertex.normal = ConvertSpeedTreeVector(sourceNormal);
        if (result.vertex.normal.norm() > 0.0f)
        {
            result.vertex.normal.normalize();
        }
        result.vertex.tangent = Eigen::Vector4f::Zero();
        result.aux.lodPosition = result.vertex.position;
        result.aux.sourceNormal = result.vertex.normal;
        result.aux.hasSourceNormalVector = true;
        return result;
    }

    PackedSpeedTreeVertex ReadPackedVertex(const std::vector<uint8_t>& data, const GeometryBlock& block, uint32_t sourceIndex)
    {
        if (block.vertexStride == 16)
        {
            return ReadBillboardVertex(data, block, sourceIndex);
        }
        return ReadStandardVertex(data, block, sourceIndex);
    }

    Eigen::Vector3f SafeNormalize(const Eigen::Vector3f& value, const Eigen::Vector3f& fallback)
    {
        const float length = value.norm();
        if (length <= 0.000001f)
        {
            return fallback;
        }
        return value / length;
    }

    void GenerateSectionNormals(MeshSection& section)
    {
        std::vector<Eigen::Vector3f> normalSums(section.vertices.size(), Eigen::Vector3f::Zero());

        const uint32_t triangleIndexCount = static_cast<uint32_t>(section.indices.size() - section.indices.size() % 3);
        for (uint32_t indexIndex = 0; indexIndex < triangleIndexCount; indexIndex += 3)
        {
            const uint32_t i0 = section.indices[indexIndex + 0];
            const uint32_t i1 = section.indices[indexIndex + 1];
            const uint32_t i2 = section.indices[indexIndex + 2];
            if (i0 >= section.vertices.size() || i1 >= section.vertices.size() || i2 >= section.vertices.size())
            {
                continue;
            }

            const Eigen::Vector3f& p0 = section.vertices[i0].position;
            const Eigen::Vector3f& p1 = section.vertices[i1].position;
            const Eigen::Vector3f& p2 = section.vertices[i2].position;

            const Eigen::Vector3f edge1 = p1 - p0;
            const Eigen::Vector3f edge2 = p2 - p0;
            const Eigen::Vector3f faceNormal = edge1.cross(edge2);
            normalSums[i0] += faceNormal;
            normalSums[i1] += faceNormal;
            normalSums[i2] += faceNormal;
        }

        for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
        {
            Vertex& vertex = section.vertices[vertexIndex];
            const SpeedTreeVertexAux* aux = vertexIndex < section.speedTreeAuxVertices.size()
                ? &section.speedTreeAuxVertices[vertexIndex]
                : nullptr;
            Eigen::Vector3f normal = aux != nullptr && aux->hasSourceNormalVector
                ? aux->sourceNormal
                : vertex.normal.norm() > 0.0f
                    ? vertex.normal
                    : SafeNormalize(normalSums[vertexIndex], Eigen::Vector3f(0.0f, 1.0f, 0.0f));
            vertex.normal = SafeNormalize(normal, Eigen::Vector3f(0.0f, 1.0f, 0.0f));
        }
    }

    void RemapSectionAuxVertices(MeshSection& section, const std::vector<uint32_t>& sourceVertexIndices)
    {
        std::vector<SpeedTreeVertexAux> remappedAuxVertices;
        remappedAuxVertices.reserve(sourceVertexIndices.size());
        for (uint32_t sourceVertexIndex : sourceVertexIndices)
        {
            if (sourceVertexIndex < section.speedTreeAuxVertices.size())
            {
                remappedAuxVertices.push_back(section.speedTreeAuxVertices[sourceVertexIndex]);
            }
            else
            {
                remappedAuxVertices.emplace_back();
            }
        }
        section.speedTreeAuxVertices = std::move(remappedAuxVertices);
    }

    void GenerateSectionTbn(MeshSection& section)
    {
        GenerateSectionNormals(section);

        std::vector<uint32_t> sourceVertexIndices;
        if (MikkTSpaceMeshGenerator::Generate(section.vertices, section.indices, &sourceVertexIndices))
        {
            RemapSectionAuxVertices(section, sourceVertexIndices);
        }
    }

    uint32_t AddLocalVertex(
        const std::vector<uint8_t>& data,
        const GeometryBlock& block,
        uint32_t sourceIndex,
        std::unordered_map<uint32_t, uint32_t>& localIndexBySourceIndex,
        MeshSection& section)
    {
        const auto localIt = localIndexBySourceIndex.find(sourceIndex);
        if (localIt != localIndexBySourceIndex.end())
        {
            return localIt->second;
        }

        const PackedSpeedTreeVertex packedVertex = ReadPackedVertex(data, block, sourceIndex);
        const uint32_t localIndex = static_cast<uint32_t>(section.vertices.size());
        localIndexBySourceIndex.emplace(sourceIndex, localIndex);
        section.vertices.push_back(packedVertex.vertex);
        section.speedTreeAuxVertices.push_back(packedVertex.aux);
        return localIndex;
    }

    MeshSection BuildSection(
        const std::vector<uint8_t>& data,
        const GeometryBlock& block,
        const MaterialSection& materialSection,
        size_t blockIndex,
        size_t sectionIndex,
        const std::vector<std::string>& materialNames)
    {
        MeshSection section;
        section.sectionName =
            "speedtree_block_" + std::to_string(blockIndex) +
            "_section_" + std::to_string(sectionIndex);
        section.materialSlotName = MaterialNameForSlot(materialNames, materialSection.materialSlot);

        std::unordered_map<uint32_t, uint32_t> localIndexBySourceIndex;
        const uint32_t sectionEnd = std::min(materialSection.startIndex + materialSection.indexCount, block.indexCount);
        const uint32_t triangleEnd = sectionEnd - (sectionEnd - materialSection.startIndex) % 3;
        for (uint32_t indexIndex = materialSection.startIndex; indexIndex < triangleEnd; indexIndex += 3)
        {
            const uint32_t sourceIndex0 = ReadIndex(data, block, indexIndex + 0);
            const uint32_t sourceIndex1 = ReadIndex(data, block, indexIndex + 1);
            const uint32_t sourceIndex2 = ReadIndex(data, block, indexIndex + 2);
            if (sourceIndex0 >= block.vertexCount ||
                sourceIndex1 >= block.vertexCount ||
                sourceIndex2 >= block.vertexCount)
            {
                continue;
            }

            const uint32_t localIndex0 = AddLocalVertex(data, block, sourceIndex0, localIndexBySourceIndex, section);
            const uint32_t localIndex1 = AddLocalVertex(data, block, sourceIndex1, localIndexBySourceIndex, section);
            const uint32_t localIndex2 = AddLocalVertex(data, block, sourceIndex2, localIndexBySourceIndex, section);
            section.indices.push_back(localIndex2);
            section.indices.push_back(localIndex1);
            section.indices.push_back(localIndex0);
        }

        GenerateSectionTbn(section);
        return section;
    }
}

void SpeedTreeSourceAdapter::ValidateSource(const std::string& sourcePath, const std::string& modelDataPath) const
{
    if (!std::filesystem::exists(sourcePath))
    {
        throw std::runtime_error("SpeedTree source file not found: " + modelDataPath);
    }
}

SpeedTreeSourceData SpeedTreeSourceAdapter::ReadSource(const std::string& sourcePath, const std::string& modelDataPath) const
{
    ValidateSource(sourcePath, modelDataPath);

    const std::vector<uint8_t> data = ReadBinaryFile(sourcePath);
    const std::vector<std::string> materialNames = ExtractOrderedMaterialNames(ExtractPrintableStrings(data, 4));
    const std::vector<GeometryBlock> blocks = FindGeometryBlocks(data);
    if (blocks.empty())
    {
        throw std::runtime_error("SpeedTree parser found no geometry blocks: " + modelDataPath);
    }

    SpeedTreeSourceData sourceData;
    sourceData.materialNames = materialNames;
    const size_t blockIndex = SelectHighestLodBlockIndex(blocks);
    const GeometryBlock& block = blocks[blockIndex];
    std::vector<MaterialSection> sections = block.sections;
    if (sections.empty())
    {
        MaterialSection defaultSection;
        defaultSection.materialSlot = 0;
        defaultSection.startIndex = 0;
        defaultSection.indexCount = block.indexCount;
        sections.push_back(defaultSection);
    }
    sourceData.modelResource.sourceMaterialSlotNames = BuildMaterialNamesForSections(sections, materialNames);

    for (size_t sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex)
    {
        MeshSection section = BuildSection(data, block, sections[sectionIndex], blockIndex, sectionIndex, materialNames);
        if (!section.vertices.empty() && !section.indices.empty())
        {
            sourceData.modelResource.sections.push_back(std::move(section));
        }
    }

    if (sourceData.modelResource.sections.empty())
    {
        throw std::runtime_error("SpeedTree parser produced no renderable sections: " + modelDataPath);
    }
    return sourceData;
}
