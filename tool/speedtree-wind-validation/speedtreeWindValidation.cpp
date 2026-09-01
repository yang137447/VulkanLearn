#include <SpeedTreeWind.h>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "mesh/loader/speedtree/speedTreeSourceAdapter.h"
#include "render/foliage/speedTreeWindSystem.h"
#include "render/foliage/speedTreeWindTransform.h"
#include "speedTreeWindValidation.h"

namespace
{
    using Vec3 = Eigen::Vector3f;

    std::filesystem::path PathFromCommandLineText(const std::string& text)
    {
        try
        {
            return std::filesystem::u8path(text);
        }
        catch (const std::filesystem::filesystem_error&)
        {
#if defined(_WIN32)
            const int wideLength = MultiByteToWideChar(
                CP_ACP,
                0,
                text.data(),
                static_cast<int>(text.size()),
                nullptr,
                0);
            if (wideLength > 0)
            {
                std::wstring widePath(static_cast<size_t>(wideLength), L'\0');
                MultiByteToWideChar(
                    CP_ACP,
                    0,
                    text.data(),
                    static_cast<int>(text.size()),
                    widePath.data(),
                    wideLength);
                return std::filesystem::path(widePath);
            }
#endif
            throw;
        }
    }

    nlohmann::json ToJson(const Vec3& value)
    {
        return {value.x(), value.y(), value.z()};
    }

    nlohmann::json ToJson(const SpeedTreeWindCurve& curve)
    {
        return nlohmann::json(curve.values);
    }

    nlohmann::json ToJson(const SpeedTreeWindBranchConfig& branch)
    {
        return {
            {"bend", ToJson(branch.bend)},
            {"oscillation", ToJson(branch.oscillation)},
            {"speed", ToJson(branch.speed)},
            {"turbulence", ToJson(branch.turbulence)},
            {"flexibility", ToJson(branch.flexibility)},
            {"independence", branch.independence}};
    }

    nlohmann::json ToJson(const SpeedTreeWindRippleConfig& ripple)
    {
        return {
            {"planar", ToJson(ripple.planar)},
            {"directional", ToJson(ripple.directional)},
            {"speed", ToJson(ripple.speed)},
            {"flexibility", ToJson(ripple.flexibility)},
            {"shimmer", ripple.shimmer},
            {"independence", ripple.independence}};
    }

    nlohmann::json ToJson(const SpeedTreeWindConfig& wind)
    {
        return {
            {"common", {
                {"strengthResponse", wind.common.strengthResponse},
                {"directionResponse", wind.common.directionResponse},
                {"gustFrequency", wind.common.gustFrequency},
                {"gustStrengthMin", wind.common.gustStrengthMin},
                {"gustStrengthMax", wind.common.gustStrengthMax},
                {"gustDurationMin", wind.common.gustDurationMin},
                {"gustDurationMax", wind.common.gustDurationMax},
                {"gustRiseScalar", wind.common.gustRiseScalar},
                {"gustFallScalar", wind.common.gustFallScalar}}},
            {"shared", ToJson(wind.shared)},
            {"branch1", ToJson(wind.branch1)},
            {"branch2", ToJson(wind.branch2)},
            {"ripple", ToJson(wind.ripple)},
            {"sharedStartHeight", wind.sharedStartHeight},
            {"branch1StretchLimit", wind.branch1StretchLimit},
            {"branch2StretchLimit", wind.branch2StretchLimit},
            {"doShared", wind.doShared},
            {"doBranch1", wind.doBranch1},
            {"doBranch2", wind.doBranch2},
            {"doRipple", wind.doRipple},
            {"doShimmer", wind.doShimmer}};
    }

    struct ScalarStats
    {
        float minimum = std::numeric_limits<float>::max();
        float maximum = std::numeric_limits<float>::lowest();
        double sum = 0.0;
        size_t count = 0;

        void Add(float value)
        {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
            sum += value;
            ++count;
        }

        nlohmann::json ToJson() const
        {
            return {
                {"min", count == 0 ? 0.0f : minimum},
                {"max", count == 0 ? 0.0f : maximum},
                {"mean", count == 0 ? 0.0 : sum / static_cast<double>(count)},
                {"count", count}};
        }
    };

    nlohmann::json BuildExportJson(
        const std::filesystem::path& assetPath,
        const SpeedTreeSourceData& source,
        bool includeGeometry)
    {
        const ModelResource& model = source.modelResource;
        nlohmann::json result = {
            {"schema", "vulkanlearn.speedtree.wind-inspection.v1"},
            {"asset", assetPath.u8string()},
            {"formatVersion", {source.formatVersionMajor, source.formatVersionMinor}},
            {"vertexPacker", source.vertexPackerProgram},
            {"materials", source.materialNames},
            {"bounds", {
                {"min", ToJson(model.speedTreeSourceBoundsMin)},
                {"max", ToJson(model.speedTreeSourceBoundsMax)}}},
            {"wind", ToJson(model.speedTreeWind)},
            {"sections", nlohmann::json::array()}};

        for (const MeshSection& section : model.sections)
        {
            ScalarStats branch1Weight;
            ScalarStats branch2Weight;
            ScalarStats rippleWeight;
            ScalarStats branch1Direction;
            ScalarStats branch2Direction;
            ScalarStats branch1Noise;
            ScalarStats branch2Noise;
            ScalarStats branch2Blend;
            for (const Vertex& vertex : section.vertices)
            {
                branch1Weight.Add(vertex.speedTreeWindBranch1.x());
                branch2Weight.Add(vertex.speedTreeWindBranch2.x());
                rippleWeight.Add(vertex.speedTreeWindBranch1.w());
                branch1Direction.Add(vertex.speedTreeWindBranch1.y());
                branch2Direction.Add(vertex.speedTreeWindBranch2.y());
                branch1Noise.Add(vertex.speedTreeWindBranch1.z());
                branch2Noise.Add(vertex.speedTreeWindBranch2.z());
                branch2Blend.Add(vertex.speedTreeWindBranch2.w());
            }

            nlohmann::json sectionJson = {
                {"name", section.sectionName},
                {"material", section.materialSlotName},
                {"vertexCount", section.vertices.size()},
                {"indexCount", section.indices.size()},
                {"windVertexStats", {
                    {"branch1Weight", branch1Weight.ToJson()},
                    {"branch2Weight", branch2Weight.ToJson()},
                    {"rippleWeight", rippleWeight.ToJson()},
                    {"branch1PackedDirection", branch1Direction.ToJson()},
                    {"branch2PackedDirection", branch2Direction.ToJson()},
                    {"branch1NoiseOffset", branch1Noise.ToJson()},
                    {"branch2NoiseOffset", branch2Noise.ToJson()},
                    {"branch2Blend", branch2Blend.ToJson()}}}};
            if (includeGeometry)
            {
                nlohmann::json positions = nlohmann::json::array();
                nlohmann::json normals = nlohmann::json::array();
                nlohmann::json windBranch1 = nlohmann::json::array();
                nlohmann::json windBranch2 = nlohmann::json::array();
                positions.get_ref<nlohmann::json::array_t&>().reserve(section.vertices.size() * 3);
                normals.get_ref<nlohmann::json::array_t&>().reserve(section.vertices.size() * 3);
                windBranch1.get_ref<nlohmann::json::array_t&>().reserve(section.vertices.size() * 4);
                windBranch2.get_ref<nlohmann::json::array_t&>().reserve(section.vertices.size() * 4);
                for (const Vertex& vertex : section.vertices)
                {
                    positions.push_back(vertex.position.x());
                    positions.push_back(vertex.position.y());
                    positions.push_back(vertex.position.z());
                    normals.push_back(vertex.normal.x());
                    normals.push_back(vertex.normal.y());
                    normals.push_back(vertex.normal.z());
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        windBranch1.push_back(vertex.speedTreeWindBranch1[channel]);
                        windBranch2.push_back(vertex.speedTreeWindBranch2[channel]);
                    }
                }
                sectionJson["geometry"] = {
                    {"positions", std::move(positions)},
                    {"normals", std::move(normals)},
                    {"windBranch1", std::move(windBranch1)},
                    {"windBranch2", std::move(windBranch2)},
                    {"indices", section.indices}};
            }
            result["sections"].push_back(std::move(sectionJson));
        }
        return result;
    }

    constexpr float GoldenAngle = 2.39996323f;
    constexpr float DirectionTableStep = 0.0078125f;
    constexpr float RuntimeSdkWindIndependence = 1.0f;
    constexpr float ValidationTolerance = 2.0e-4f;

    struct StagePositions
    {
        Vec3 ripple = Vec3::Zero();
        Vec3 branch2 = Vec3::Zero();
        Vec3 branch1 = Vec3::Zero();
        Vec3 shared = Vec3::Zero();
    };

    struct Sample
    {
        std::string label;
        size_t sectionIndex = 0;
        size_t vertexIndex = 0;
        Vertex vertex;
    };

    struct SourceVertexInput
    {
        Vec3 position = Vec3::Zero();
        float rippleWeight = 0.0f;
        float branch1Weight = 0.0f;
        float branch2Weight = 0.0f;
        uint8_t branch1Direction = 0;
        uint8_t branch1Offset = 0;
        uint8_t branch2Direction = 0;
        uint8_t branch2Offset = 0;
    };

    struct ErrorAccumulator
    {
        float maximum = 0.0f;
        std::string worstLabel;
        bool failed = false;

        void Add(const std::string& label, float error)
        {
            if (error > maximum)
            {
                maximum = error;
                worstLabel = label;
            }
            failed = failed || error > ValidationTolerance;
        }
    };

    class DisjointSet
    {
    public:
        explicit DisjointSet(size_t size)
            : parent(size), rank(size, 0)
        {
            std::iota(parent.begin(), parent.end(), 0);
        }

        size_t Find(size_t index)
        {
            if (parent[index] != index)
            {
                parent[index] = Find(parent[index]);
            }
            return parent[index];
        }

        void Join(size_t left, size_t right)
        {
            left = Find(left);
            right = Find(right);
            if (left == right)
            {
                return;
            }
            if (rank[left] < rank[right])
            {
                std::swap(left, right);
            }
            parent[right] = left;
            if (rank[left] == rank[right])
            {
                ++rank[left];
            }
        }

    private:
        std::vector<size_t> parent;
        std::vector<uint8_t> rank;
    };

    struct PositionKey
    {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;

        bool operator==(const PositionKey& other) const
        {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct PositionKeyHash
    {
        size_t operator()(const PositionKey& key) const
        {
            size_t value = std::hash<int32_t>()(key.x);
            value ^= std::hash<int32_t>()(key.y) + 0x9e3779b9 + (value << 6) + (value >> 2);
            value ^= std::hash<int32_t>()(key.z) + 0x9e3779b9 + (value << 6) + (value >> 2);
            return value;
        }
    };

    PositionKey MakePositionKey(const Vec3& position)
    {
        constexpr float QuantizationScale = 8192.0f;
        return {
            static_cast<int32_t>(std::lround(position.x() * QuantizationScale)),
            static_cast<int32_t>(std::lround(position.y() * QuantizationScale)),
            static_cast<int32_t>(std::lround(position.z() * QuantizationScale))};
    }

    struct GridKey
    {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;

        bool operator==(const GridKey& other) const
        {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct GridKeyHash
    {
        size_t operator()(const GridKey& key) const
        {
            size_t value = std::hash<int32_t>()(key.x);
            value ^= std::hash<int32_t>()(key.y) + 0x9e3779b9 + (value << 6) + (value >> 2);
            value ^= std::hash<int32_t>()(key.z) + 0x9e3779b9 + (value << 6) + (value >> 2);
            return value;
        }
    };

    struct AlphaImage
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
    };

    GridKey MakeGridKey(const Vec3& position, float cellSize)
    {
        return {
            static_cast<int32_t>(std::floor(position.x() / cellSize)),
            static_cast<int32_t>(std::floor(position.y() / cellSize)),
            static_cast<int32_t>(std::floor(position.z() / cellSize))};
    }

    AlphaImage LoadAlphaImage(const std::filesystem::path& imagePath)
    {
        std::ifstream stream(imagePath, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            throw std::runtime_error(
                "Failed to load Oak Cluster Branch alpha texture: " + imagePath.string());
        }
        const std::streamsize byteCount = stream.tellg();
        if (byteCount < 18)
        {
            throw std::runtime_error("Oak Cluster Branch TGA is truncated.");
        }
        stream.seekg(0, std::ios::beg);
        std::vector<uint8_t> source(static_cast<size_t>(byteCount));
        if (!stream.read(reinterpret_cast<char*>(source.data()), byteCount) || source.size() < 18)
        {
            throw std::runtime_error("Oak Cluster Branch TGA is truncated.");
        }

        const uint8_t idLength = source[0];
        const uint8_t colorMapType = source[1];
        const uint8_t imageType = source[2];
        const int width = static_cast<int>(source[12] | (source[13] << 8));
        const int height = static_cast<int>(source[14] | (source[15] << 8));
        const uint8_t pixelDepth = source[16];
        const uint8_t descriptor = source[17];
        if (colorMapType != 0 || imageType != 2 || pixelDepth != 32 ||
            width <= 0 || height <= 0)
        {
            throw std::runtime_error(
                "Oak Cluster Branch alpha validation requires an uncompressed 32-bit TGA.");
        }

        constexpr size_t Channels = 4;
        const size_t pixelDataOffset = 18 + idLength;
        const size_t requiredSize = pixelDataOffset +
            static_cast<size_t>(width) * static_cast<size_t>(height) * Channels;
        if (requiredSize > source.size())
        {
            throw std::runtime_error("Oak Cluster Branch TGA pixel data is truncated.");
        }

        AlphaImage image;
        image.width = width;
        image.height = height;
        image.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * Channels);
        const bool topOrigin = (descriptor & 0x20u) != 0;
        const bool rightOrigin = (descriptor & 0x10u) != 0;
        for (int y = 0; y < height; ++y)
        {
            const int sourceY = topOrigin ? y : height - 1 - y;
            for (int x = 0; x < width; ++x)
            {
                const int sourceX = rightOrigin ? width - 1 - x : x;
                const size_t sourceOffset = pixelDataOffset +
                    (static_cast<size_t>(sourceY) * static_cast<size_t>(width) +
                     static_cast<size_t>(sourceX)) * Channels;
                const size_t destinationOffset =
                    (static_cast<size_t>(y) * static_cast<size_t>(width) +
                     static_cast<size_t>(x)) * Channels;
                image.rgba[destinationOffset + 0] = source[sourceOffset + 2];
                image.rgba[destinationOffset + 1] = source[sourceOffset + 1];
                image.rgba[destinationOffset + 2] = source[sourceOffset + 0];
                image.rgba[destinationOffset + 3] = source[sourceOffset + 3];
            }
        }
        return image;
    }

    int WrapImageCoordinate(int coordinate, int size)
    {
        const int wrapped = coordinate % size;
        return wrapped < 0 ? wrapped + size : wrapped;
    }

    float SampleAlpha(const AlphaImage& image, const Eigen::Vector2f& uv)
    {
        const float textureX = (uv.x() - std::floor(uv.x())) * image.width - 0.5f;
        const float textureY = (uv.y() - std::floor(uv.y())) * image.height - 0.5f;
        const int x0 = static_cast<int>(std::floor(textureX));
        const int y0 = static_cast<int>(std::floor(textureY));
        const float fractionX = textureX - std::floor(textureX);
        const float fractionY = textureY - std::floor(textureY);

        const int wrappedX0 = WrapImageCoordinate(x0, image.width);
        const int wrappedX1 = WrapImageCoordinate(x0 + 1, image.width);
        const int wrappedY0 = WrapImageCoordinate(y0, image.height);
        const int wrappedY1 = WrapImageCoordinate(y0 + 1, image.height);
        const auto alphaAt = [&image](int x, int y)
        {
            constexpr size_t Channels = 4;
            const size_t offset =
                (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
                 static_cast<size_t>(x)) * Channels;
            return static_cast<float>(image.rgba[offset + 3]) / 255.0f;
        };
        const float top = alphaAt(wrappedX0, wrappedY0) * (1.0f - fractionX) +
            alphaAt(wrappedX1, wrappedY0) * fractionX;
        const float bottom = alphaAt(wrappedX0, wrappedY1) * (1.0f - fractionX) +
            alphaAt(wrappedX1, wrappedY1) * fractionX;
        return top * (1.0f - fractionY) + bottom * fractionY;
    }

    struct ConnectionPair
    {
        size_t firstComponent = 0;
        size_t secondComponent = 0;
        size_t firstVertex = 0;
        size_t secondVertex = 0;
        float restDistance = 0.0f;
        std::array<float, 4> stageDistances = {};
        float maximumGrowth = 0.0f;
        std::array<float, 4> firstBranch1 = {};
        std::array<float, 4> firstBranch2 = {};
        std::array<float, 4> secondBranch1 = {};
        std::array<float, 4> secondBranch2 = {};
    };

    struct CrossConnectionPair
    {
        size_t firstComponent = 0;
        size_t secondComponent = 0;
        size_t firstVertex = 0;
        size_t secondVertex = 0;
        float restDistance = 0.0f;
        std::array<float, 4> stageDistances = {};
        float maximumGrowth = 0.0f;
        std::array<float, 4> firstBranch1 = {};
        std::array<float, 4> firstBranch2 = {};
        std::array<float, 4> secondBranch1 = {};
        std::array<float, 4> secondBranch2 = {};
    };

    struct VisibleAttachmentSample
    {
        size_t triangleIndex = 0;
        size_t barkVertex = 0;
        Eigen::Vector3f barycentric = Eigen::Vector3f::Zero();
        Eigen::Vector2f uv = Eigen::Vector2f::Zero();
        float alpha = 0.0f;
        float semanticRootDistance = 0.0f;
        float restDistance = 0.0f;
        std::array<float, 4> stageDistances = {};
        float maximumGrowth = 0.0f;
    };

    Vec3 MapVector(const ::float3& value)
    {
        return Vec3(value.x, value.y, value.z);
    }

    Vec3 ConvertSourceVector(const Vec3& source)
    {
        return Vec3(source.x(), source.z(), -source.y());
    }

    Vec3 ConvertEngineVector(const Vec3& engine)
    {
        return Vec3(engine.x(), -engine.z(), engine.y());
    }

    std::pair<Vec3, Vec3> ConvertSourceBounds(const Vec3& sourceMin, const Vec3& sourceMax)
    {
        return {
            Vec3(sourceMin.x(), sourceMin.z(), -sourceMax.y()),
            Vec3(sourceMax.x(), sourceMax.z(), -sourceMin.y())};
    }

    Vec3 Normalize(const Vec3& value)
    {
        const float length = value.norm();
        if (length <= 0.0f)
        {
            return Vec3::Zero();
        }
        return value / length;
    }

    float Fraction(float value)
    {
        return value - std::floor(value);
    }

    float NoiseHash(const Eigen::Vector2f& value)
    {
        return Fraction(
            10000.0f *
            std::sin(17.0f * value.x() + value.y() * 0.1f) *
            (0.1f + std::abs(std::sin(value.y() * 13.0f + value.x()))));
    }

    float QNoise(const Eigen::Vector2f& value)
    {
        const Eigen::Vector2f cell(value.array().floor());
        const Eigen::Vector2f fraction(value.x() - cell.x(), value.y() - cell.y());
        const float a = NoiseHash(cell);
        const float b = NoiseHash(cell + Eigen::Vector2f(1.0f, 0.0f));
        const float c = NoiseHash(cell + Eigen::Vector2f(0.0f, 1.0f));
        const float d = NoiseHash(cell + Eigen::Vector2f(1.0f, 1.0f));
        const Eigen::Vector2f smooth = fraction.cwiseProduct(fraction).cwiseProduct(
            Eigen::Vector2f(3.0f, 3.0f) - 2.0f * fraction);
        return a + (b - a) * smooth.x() +
            (c - a) * smooth.y() * (1.0f - smooth.x()) +
            (d - b) * smooth.x() * smooth.y();
    }

    // This is the RuntimeSdkNoise2DFlat fallback copied as a scalar reference.
    // The output components are wind coefficients, not a geometric vector, so
    // the source-to-engine conversion applies only to the sampling position.
    Vec3 RuntimeSdkNoiseSource(const Vec3& position)
    {
        const Eigen::Vector2f xy(position.x(), position.y());
        return Vec3(
            QNoise(xy * 20.0f) - 0.5f,
            QNoise(Eigen::Vector2f(xy.y(), xy.x()) * 10.0f),
            0.0f);
    }

    Vec3 RuntimeSdkNoiseEngine(const Vec3& position)
    {
        return RuntimeSdkNoiseSource(ConvertEngineVector(position));
    }

    Vec3 DecodeDirectionSource(uint8_t packed)
    {
        const float index = static_cast<float>(packed);
        const float sourceZ = 0.99609375f - DirectionTableStep * index;
        const float radial = std::sqrt(std::max(1.0f - sourceZ * sourceZ, 0.0f));
        const float angle = index * GoldenAngle;
        return Vec3(
            std::cos(angle) * radial,
            std::sin(angle) * radial,
            sourceZ);
    }

    Vec3 DecodeNoiseOffsetSource(uint8_t packed, const Vec3& sourceExtents)
    {
        float value = static_cast<float>(packed);
        const float sourceZ = std::floor(value / 81.0f);
        value -= sourceZ * 81.0f;
        const float sourceY = std::floor(value / 9.0f);
        const float sourceX = value - sourceY * 9.0f;
        const Vec3 normalized(sourceX / 8.0f, sourceY / 8.0f, sourceZ / 2.0f);
        return normalized.cwiseProduct(sourceExtents);
    }

    uint8_t QuantizeByte(float normalized)
    {
        const float rounded = std::floor(normalized * 255.0f + 0.5f);
        return static_cast<uint8_t>(std::clamp(rounded, 0.0f, 255.0f));
    }

    Vec3 EvaluateSdkBranch(
        const Vec3& currentPosition,
        const Vec3& windDirection,
        const Vec3& globalNoisePosition,
        const Vec3& branchDirection,
        float branchWeight,
        const Vec3& branchNoiseOffset,
        float stretchLimit,
        const SpeedTree::SWindBranchStateRuntimeSdk& state)
    {
        const float length = branchWeight * stretchLimit;
        if (length <= 0.0f)
        {
            return currentPosition;
        }

        const Vec3 up(0.0f, 0.0f, 1.0f);
        const Vec3 anchor = currentPosition - branchDirection * length;
        const Vec3 relative = currentPosition - anchor;
        const Vec3 effectiveWind = Normalize(
            windDirection + up * branchDirection.dot(windDirection) * branchDirection.dot(windDirection));
        const Vec3 noisePosition = globalNoisePosition +
            MapVector(state.m_vNoisePosTurbulence) +
            branchNoiseOffset * state.m_fIndependence +
            effectiveWind * (state.m_fFlexibility * branchWeight);
        const Vec3 noise = RuntimeSdkNoiseSource(noisePosition);
        const Vec3 turbulentOscillation = up * state.m_fTurbulence;
        Vec3 motion = (effectiveWind * noise.x() + turbulentOscillation * noise.y()) *
            state.m_fOscillation;
        motion += effectiveWind * state.m_fBend * (1.0f - noise.z());
        motion *= branchWeight;
        return Normalize(relative + motion) * length + anchor;
    }

    StagePositions EvaluateSdkVertex(
        const SourceVertexInput& vertex,
        const SpeedTree::SWindStateRuntimeSdk& state,
        const Vec3& sourceInstancePosition)
    {
        const Vec3 up(0.0f, 0.0f, 1.0f);
        const Vec3 windDirection = MapVector(state.m_vWindDirection);
        const Vec3 sourceExtents = MapVector(state.m_vBoundingBoxMax) - MapVector(state.m_vBoundingBoxMin);
        const Vec3 globalNoisePosition = sourceInstancePosition * RuntimeSdkWindIndependence;
        Vec3 windyPosition = vertex.position;
        StagePositions stages;

        const Vec3 rippleNoisePosition = globalNoisePosition +
            MapVector(state.m_sRipple.m_vNoisePosTurbulence) +
            windyPosition * state.m_sRipple.m_fIndependence +
            windDirection * (state.m_sRipple.m_fFlexibility * vertex.rippleWeight);
        const Vec3 rippleNoise = RuntimeSdkNoiseSource(rippleNoisePosition);
        const Vec3 rippleMotion = (
            windDirection * ((rippleNoise.x() + 0.25f) * state.m_sRipple.m_fDirectional) +
            up * (rippleNoise.y() * state.m_sRipple.m_fPlanar)) * vertex.rippleWeight;
        windyPosition += rippleMotion;
        stages.ripple = windyPosition;

        windyPosition = EvaluateSdkBranch(
            windyPosition, windDirection, globalNoisePosition,
            DecodeDirectionSource(vertex.branch2Direction), vertex.branch2Weight,
            DecodeNoiseOffsetSource(vertex.branch2Offset, sourceExtents),
            state.m_fBranch2StretchLimit, state.m_sBranch2);
        stages.branch2 = windyPosition;

        windyPosition = EvaluateSdkBranch(
            windyPosition, windDirection, globalNoisePosition,
            DecodeDirectionSource(vertex.branch1Direction), vertex.branch1Weight,
            DecodeNoiseOffsetSource(vertex.branch1Offset, sourceExtents),
            state.m_fBranch1StretchLimit, state.m_sBranch1);
        stages.branch1 = windyPosition;

        const float length = windyPosition.norm();
        const float weight = std::pow(std::max(
            windyPosition.z() - MapVector(state.m_vBoundingBoxMax).z() * state.m_fSharedHeightStart,
            0.0f) / MapVector(state.m_vBoundingBoxMax).z(), 2.0f);
        const Vec3 sharedNoisePosition = globalNoisePosition +
            MapVector(state.m_sShared.m_vNoisePosTurbulence) +
            windDirection * (state.m_sShared.m_fFlexibility * weight);
        const Vec3 sharedNoise = RuntimeSdkNoiseSource(sharedNoisePosition);
        const Vec3 sharedTurbulence = windDirection.cross(up) * state.m_sShared.m_fTurbulence;
        Vec3 sharedMotion = (
            windDirection * sharedNoise.x() + sharedTurbulence * sharedNoise.y()) *
            state.m_sShared.m_fOscillation;
        sharedMotion += windDirection * state.m_sShared.m_fBend * (1.0f - sharedNoise.z());
        sharedMotion *= weight;
        stages.shared = length > 0.0f ? Normalize(windyPosition + sharedMotion) * length : windyPosition;
        return stages;
    }

    Vec3 DecodeDirectionEngine(float normalizedPackedDirection)
    {
        return ConvertSourceVector(DecodeDirectionSource(QuantizeByte(normalizedPackedDirection)));
    }

    Vec3 DecodeNoiseOffsetEngine(float normalizedPackedOffset, const Vec3& engineExtents)
    {
        const Vec3 sourceExtents(engineExtents.x(), engineExtents.z(), engineExtents.y());
        return ConvertSourceVector(DecodeNoiseOffsetSource(
            QuantizeByte(normalizedPackedOffset), sourceExtents));
    }

    Vec3 EvaluateEngineBranch(
        const Vec3& currentPosition,
        const Vec3& windDirection,
        const Vec3& globalNoisePosition,
        const Eigen::Vector4f& packedBranch,
        float stretchLimit,
        const Vec3& engineExtents,
        const Eigen::Vector4f& noiseState,
        const Eigen::Vector4f& responseState)
    {
        const float branchWeight = packedBranch.x();
        const float length = branchWeight * stretchLimit;
        if (length <= 0.0f)
        {
            return currentPosition;
        }

        const Vec3 up(0.0f, 1.0f, 0.0f);
        const Vec3 branchDirection = DecodeDirectionEngine(packedBranch.y());
        const Vec3 branchNoiseOffset = DecodeNoiseOffsetEngine(packedBranch.z(), engineExtents);
        const Vec3 anchor = currentPosition - branchDirection * length;
        const Vec3 relative = currentPosition - anchor;
        const Vec3 effectiveWind = Normalize(
            windDirection + up * branchDirection.dot(windDirection) * branchDirection.dot(windDirection));
        const Vec3 noisePosition = globalNoisePosition + noiseState.head<3>() +
            branchNoiseOffset * noiseState.w() + effectiveWind * (responseState.w() * branchWeight);
        const Vec3 noise = RuntimeSdkNoiseEngine(noisePosition);
        const Vec3 turbulentOscillation = up * responseState.z();
        Vec3 motion = (effectiveWind * noise.x() + turbulentOscillation * noise.y()) * responseState.y();
        motion += effectiveWind * responseState.x() * (1.0f - noise.z());
        motion *= branchWeight;
        return Normalize(relative + motion) * length + anchor;
    }

    StagePositions EvaluateVulkanLearnVertex(
        const Vertex& vertex,
        const SpeedTreeWindStateGPU& state,
        const Vec3& engineInstancePosition)
    {
        // TODO: 当前这里只执行 VulkanLearn GLSL 公式的 CPU 转写。后续增加独立的
        // Vulkan GPU 验证路径，执行复用生产 GLSL 的 SPIR-V，将形变结果写入 SSBO
        // 并 readback，与官方 SDK CPU 结果比较。
        const Vec3 windDirection = Normalize(state.windVector.head<3>());
        const Vec3 globalNoisePosition = engineInstancePosition * state.branchStretchLimits.z();
        const Vec3 boundsMin = state.treeBoundsMin.head<3>();
        const Vec3 boundsMax = state.treeBoundsMax.head<3>();
        const Vec3 engineExtents = boundsMax - boundsMin;
        Vec3 windyPosition = vertex.position;
        StagePositions stages;

        const Eigen::Vector4f& branch1 = vertex.speedTreeWindBranch1;
        const Eigen::Vector4f& branch2 = vertex.speedTreeWindBranch2;
        const Vec3 rippleNoisePosition = globalNoisePosition +
            state.rippleNoisePosTurbulenceIndependence.head<3>() +
            windyPosition * state.rippleNoisePosTurbulenceIndependence.w() +
            windDirection * state.ripplePlanarDirectionalFlexibilityShimmer.z() * branch1.w();
        const Vec3 rippleNoise = RuntimeSdkNoiseEngine(rippleNoisePosition);
        const Vec3 rippleMotion = (
            windDirection * (rippleNoise.x() + 0.25f) * state.ripplePlanarDirectionalFlexibilityShimmer.y() +
            Vec3(0.0f, 1.0f, 0.0f) * rippleNoise.y() * state.ripplePlanarDirectionalFlexibilityShimmer.x()) * branch1.w();
        windyPosition += rippleMotion;
        stages.ripple = windyPosition;

        windyPosition = EvaluateEngineBranch(
            windyPosition, windDirection, globalNoisePosition, branch2,
            state.branchStretchLimits.y(), engineExtents,
            state.branch2NoisePosTurbulenceIndependence,
            state.branch2BendOscillationTurbulenceFlexibility);
        stages.branch2 = windyPosition;
        windyPosition = EvaluateEngineBranch(
            windyPosition, windDirection, globalNoisePosition, branch1,
            state.branchStretchLimits.x(), engineExtents,
            state.branch1NoisePosTurbulenceIndependence,
            state.branch1BendOscillationTurbulenceFlexibility);
        stages.branch1 = windyPosition;

        const float length = windyPosition.norm();
        const float maxHeight = boundsMax.y();
        const float weight = std::pow(std::max(
            windyPosition.y() - maxHeight * state.treeExtentsSharedHeightStart.w(),
            0.0f) / maxHeight, 2.0f);
        const Vec3 sharedNoisePosition = globalNoisePosition +
            state.sharedNoisePosTurbulenceIndependence.head<3>() +
            windDirection * state.sharedBendOscillationTurbulenceFlexibility.w() * weight;
        const Vec3 sharedNoise = RuntimeSdkNoiseEngine(sharedNoisePosition);
        const Vec3 sharedTurbulence = windDirection.cross(Vec3(0.0f, 1.0f, 0.0f)) *
            state.sharedBendOscillationTurbulenceFlexibility.z();
        Vec3 sharedMotion = (
            windDirection * sharedNoise.x() + sharedTurbulence * sharedNoise.y()) *
            state.sharedBendOscillationTurbulenceFlexibility.y();
        sharedMotion += windDirection *
            state.sharedBendOscillationTurbulenceFlexibility.x() * (1.0f - sharedNoise.z());
        sharedMotion *= weight;
        stages.shared = length > 0.0f ? Normalize(windyPosition + sharedMotion) * length : windyPosition;
        return stages;
    }

    void CopySdkBranchConfig(
        const SpeedTreeWindBranchConfig& source,
        SpeedTree::SConfigRuntimeSdk::SBranchWindLevel& target)
    {
        for (size_t i = 0; i < 20; ++i)
        {
            target.m_afBend[i] = source.bend.values[i];
            target.m_afOscillation[i] = source.oscillation.values[i];
            target.m_afSpeed[i] = source.speed.values[i];
            target.m_afTurbulence[i] = source.turbulence.values[i];
            target.m_afFlexibility[i] = source.flexibility.values[i];
        }
        target.m_fIndependence = source.independence;
    }

    SpeedTreeWindConfig MakeValidationConfig()
    {
        SpeedTreeWindConfig config;
        config.common.strengthResponse = 1.7f;
        config.common.directionResponse = 2.3f;
        config.common.gustFrequency = 0.0f;
        config.common.gustStrengthMin = 0.12f;
        config.common.gustStrengthMax = 0.38f;
        config.common.gustDurationMin = 0.35f;
        config.common.gustDurationMax = 0.65f;
        config.common.gustRiseScalar = 0.75f;
        config.common.gustFallScalar = 0.5f;
        for (size_t i = 0; i < 20; ++i)
        {
            const float x = static_cast<float>(i) / 19.0f;
            const float wobble = 0.03f * std::sin(static_cast<float>(i) * 0.7f);
            config.shared.bend.values[i] = 0.10f + 0.22f * x + wobble;
            config.shared.oscillation.values[i] = 0.12f + 0.18f * x;
            config.shared.speed.values[i] = 0.45f + 0.25f * x;
            config.shared.turbulence.values[i] = 0.16f + 0.11f * x;
            config.shared.flexibility.values[i] = 0.20f + 0.16f * x;
            config.branch1.bend.values[i] = 0.14f + 0.30f * x;
            config.branch1.oscillation.values[i] = 0.11f + 0.23f * x;
            config.branch1.speed.values[i] = 0.55f + 0.33f * x;
            config.branch1.turbulence.values[i] = 0.18f + 0.17f * x;
            config.branch1.flexibility.values[i] = 0.24f + 0.20f * x;
            config.branch2.bend.values[i] = 0.18f + 0.34f * x;
            config.branch2.oscillation.values[i] = 0.15f + 0.26f * x;
            config.branch2.speed.values[i] = 0.65f + 0.39f * x;
            config.branch2.turbulence.values[i] = 0.22f + 0.19f * x;
            config.branch2.flexibility.values[i] = 0.28f + 0.22f * x;
            config.ripple.planar.values[i] = 0.20f + 0.15f * x;
            config.ripple.directional.values[i] = 0.24f + 0.18f * x;
            config.ripple.speed.values[i] = 0.80f + 0.42f * x;
            config.ripple.flexibility.values[i] = 0.32f + 0.25f * x;
        }
        config.shared.independence = 0.37f;
        config.branch1.independence = 0.61f;
        config.branch2.independence = 0.79f;
        config.ripple.independence = 0.43f;
        config.ripple.shimmer = 0.26f;
        config.sharedStartHeight = 0.23f;
        config.branch1StretchLimit = 1.25f;
        config.branch2StretchLimit = 0.82f;
        config.doShared = true;
        config.doBranch1 = true;
        config.doBranch2 = true;
        config.doRipple = true;
        config.doShimmer = true;
        return config;
    }

    SpeedTree::SConfigRuntimeSdk MakeSdkConfig(
        const SpeedTreeWindConfig& source,
        const Vec3& sourceBoundsMin,
        const Vec3& sourceBoundsMax)
    {
        SpeedTree::SConfigRuntimeSdk config;
        config.m_fStrengthResponse = source.common.strengthResponse;
        config.m_fDirectionResponse = source.common.directionResponse;
        config.m_fGustFrequency = source.common.gustFrequency;
        config.m_fGustStrengthMin = source.common.gustStrengthMin;
        config.m_fGustStrengthMax = source.common.gustStrengthMax;
        config.m_fGustDurationMin = source.common.gustDurationMin;
        config.m_fGustDurationMax = source.common.gustDurationMax;
        config.m_fGustRiseScalar = source.common.gustRiseScalar;
        config.m_fGustFallScalar = source.common.gustFallScalar;
        config.m_vBoundingBoxMin = ::float3(
            sourceBoundsMin.x(), sourceBoundsMin.y(), sourceBoundsMin.z());
        config.m_vBoundingBoxMax = ::float3(
            sourceBoundsMax.x(), sourceBoundsMax.y(), sourceBoundsMax.z());
        CopySdkBranchConfig(source.shared, config.m_sShared);
        CopySdkBranchConfig(source.branch1, config.m_sBranch1);
        CopySdkBranchConfig(source.branch2, config.m_sBranch2);
        for (size_t i = 0; i < 20; ++i)
        {
            config.m_sRipple.m_afPlanar[i] = source.ripple.planar.values[i];
            config.m_sRipple.m_afDirectional[i] = source.ripple.directional.values[i];
            config.m_sRipple.m_afSpeed[i] = source.ripple.speed.values[i];
            config.m_sRipple.m_afFlexibility[i] = source.ripple.flexibility.values[i];
        }
        config.m_sRipple.m_fIndependence = source.ripple.independence;
        config.m_sRipple.m_fShimmer = source.ripple.shimmer;
        config.m_fSharedHeightStart = source.sharedStartHeight;
        config.m_fBranch1StretchLimit = source.branch1StretchLimit;
        config.m_fBranch2StretchLimit = source.branch2StretchLimit;
        config.m_bDoShared = source.doShared;
        config.m_bDoBranch1 = source.doBranch1;
        config.m_bDoBranch2 = source.doBranch2;
        config.m_bDoRipple = source.doRipple;
        config.m_bDoShimmer = source.doShimmer;
        config.m_fWindIndependence = RuntimeSdkWindIndependence;
        return config;
    }

    void CompareScalar(ErrorAccumulator& result, const std::string& label, float sdk, float vulkanLearn)
    {
        result.Add(label, std::abs(sdk - vulkanLearn));
    }

    void CompareVector(ErrorAccumulator& result, const std::string& label, const Vec3& sdk, const Vec3& vulkanLearn)
    {
        result.Add(label, (sdk - vulkanLearn).cwiseAbs().maxCoeff());
    }

    void CompareBranchState(
        ErrorAccumulator& result,
        const std::string& label,
        const SpeedTree::SWindBranchStateRuntimeSdk& sdk,
        const Eigen::Vector4f& vulkanLearn)
    {
        CompareScalar(result, label + ".bend", sdk.m_fBend, vulkanLearn.x());
        CompareScalar(result, label + ".oscillation", sdk.m_fOscillation, vulkanLearn.y());
        CompareScalar(result, label + ".turbulence", sdk.m_fTurbulence, vulkanLearn.z());
        CompareScalar(result, label + ".flexibility", sdk.m_fFlexibility, vulkanLearn.w());
    }

    void CompareCpuState(
        ErrorAccumulator& result,
        const SpeedTree::SWindStateRuntimeSdk& sdk,
        const SpeedTreeWindStateGPU& vulkanLearn)
    {
        const auto convertedBounds = ConvertSourceBounds(
            MapVector(sdk.m_vBoundingBoxMin), MapVector(sdk.m_vBoundingBoxMax));
        CompareVector(result, "windDirection", ConvertSourceVector(MapVector(sdk.m_vWindDirection)), vulkanLearn.windVector.head<3>());
        CompareScalar(result, "combinedStrength", sdk.m_fWindStrength, vulkanLearn.windVector.w());
        CompareVector(result, "boundsMin", convertedBounds.first, vulkanLearn.treeBoundsMin.head<3>());
        CompareVector(result, "boundsMax", convertedBounds.second, vulkanLearn.treeBoundsMax.head<3>());
        CompareScalar(result, "sharedHeightStart", sdk.m_fSharedHeightStart, vulkanLearn.treeExtentsSharedHeightStart.w());
        CompareScalar(result, "branch1StretchLimit", sdk.m_fBranch1StretchLimit, vulkanLearn.branchStretchLimits.x());
        CompareScalar(result, "branch2StretchLimit", sdk.m_fBranch2StretchLimit, vulkanLearn.branchStretchLimits.y());
        CompareVector(result, "sharedNoisePosition", ConvertSourceVector(MapVector(sdk.m_sShared.m_vNoisePosTurbulence)), vulkanLearn.sharedNoisePosTurbulenceIndependence.head<3>());
        CompareVector(result, "branch1NoisePosition", ConvertSourceVector(MapVector(sdk.m_sBranch1.m_vNoisePosTurbulence)), vulkanLearn.branch1NoisePosTurbulenceIndependence.head<3>());
        CompareVector(result, "branch2NoisePosition", ConvertSourceVector(MapVector(sdk.m_sBranch2.m_vNoisePosTurbulence)), vulkanLearn.branch2NoisePosTurbulenceIndependence.head<3>());
        CompareVector(result, "rippleNoisePosition", ConvertSourceVector(MapVector(sdk.m_sRipple.m_vNoisePosTurbulence)), vulkanLearn.rippleNoisePosTurbulenceIndependence.head<3>());
        CompareScalar(result, "sharedIndependence", sdk.m_sShared.m_fIndependence, vulkanLearn.sharedNoisePosTurbulenceIndependence.w());
        CompareScalar(result, "branch1Independence", sdk.m_sBranch1.m_fIndependence, vulkanLearn.branch1NoisePosTurbulenceIndependence.w());
        CompareScalar(result, "branch2Independence", sdk.m_sBranch2.m_fIndependence, vulkanLearn.branch2NoisePosTurbulenceIndependence.w());
        CompareScalar(result, "rippleIndependence", sdk.m_sRipple.m_fIndependence, vulkanLearn.rippleNoisePosTurbulenceIndependence.w());
        CompareBranchState(result, "shared", sdk.m_sShared, vulkanLearn.sharedBendOscillationTurbulenceFlexibility);
        CompareBranchState(result, "branch1", sdk.m_sBranch1, vulkanLearn.branch1BendOscillationTurbulenceFlexibility);
        CompareBranchState(result, "branch2", sdk.m_sBranch2, vulkanLearn.branch2BendOscillationTurbulenceFlexibility);
        CompareScalar(result, "ripplePlanar", sdk.m_sRipple.m_fPlanar, vulkanLearn.ripplePlanarDirectionalFlexibilityShimmer.x());
        CompareScalar(result, "rippleDirectional", sdk.m_sRipple.m_fDirectional, vulkanLearn.ripplePlanarDirectionalFlexibilityShimmer.y());
        CompareScalar(result, "rippleFlexibility", sdk.m_sRipple.m_fFlexibility, vulkanLearn.ripplePlanarDirectionalFlexibilityShimmer.z());
        CompareScalar(result, "rippleShimmer", sdk.m_sRipple.m_fShimmer, vulkanLearn.ripplePlanarDirectionalFlexibilityShimmer.w());
    }

    void MergeErrors(ErrorAccumulator& total, const ErrorAccumulator& current)
    {
        if (current.maximum > total.maximum)
        {
            total.maximum = current.maximum;
            total.worstLabel = current.worstLabel;
        }
        total.failed = total.failed || current.failed;
    }

    SourceVertexInput ToSourceInput(const Vertex& vertex)
    {
        SourceVertexInput result;
        result.position = ConvertEngineVector(vertex.position);
        result.rippleWeight = vertex.speedTreeWindBranch1.w();
        result.branch1Weight = vertex.speedTreeWindBranch1.x();
        result.branch2Weight = vertex.speedTreeWindBranch2.x();
        result.branch1Direction = QuantizeByte(vertex.speedTreeWindBranch1.y());
        result.branch1Offset = QuantizeByte(vertex.speedTreeWindBranch1.z());
        result.branch2Direction = QuantizeByte(vertex.speedTreeWindBranch2.y());
        result.branch2Offset = QuantizeByte(vertex.speedTreeWindBranch2.z());
        return result;
    }

    std::vector<Sample> SelectGenericSamples(const ModelResource& model)
    {
        std::vector<Sample> samples;
        constexpr size_t MaximumSampleCount = 4;
        for (size_t sectionIndex = 0;
             sectionIndex < model.sections.size() && samples.size() < MaximumSampleCount;
             ++sectionIndex)
        {
            const MeshSection& section = model.sections[sectionIndex];
            if (section.vertices.empty())
            {
                continue;
            }

            size_t bestVertexIndex = 0;
            float bestWeight = -1.0f;
            for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
            {
                const Vertex& vertex = section.vertices[vertexIndex];
                const float weight =
                    vertex.speedTreeWindBranch1.x() +
                    vertex.speedTreeWindBranch2.x() +
                    vertex.speedTreeWindBranch1.w();
                if (weight > bestWeight)
                {
                    bestWeight = weight;
                    bestVertexIndex = vertexIndex;
                }
            }

            samples.push_back({
                "generic-section-" + std::to_string(sectionIndex),
                sectionIndex,
                bestVertexIndex,
                section.vertices[bestVertexIndex]});
        }

        if (samples.empty())
        {
            throw std::runtime_error("SpeedTree asset has no vertices for numeric validation.");
        }
        return samples;
    }

    std::vector<Sample> SelectSamples(const ModelResource& model, bool useOakReferenceSamples)
    {
        if (!useOakReferenceSamples)
        {
            return SelectGenericSamples(model);
        }

        struct FixedVertex
        {
            const char* label;
            size_t sectionIndex;
            size_t vertexIndex;
        };
        constexpr std::array<FixedVertex, 4> fixedVertices = {{
            {"trunk-low-branch-weight", 0, 0},
            {"branch1-branch2", 1, 2733},
            {"ripple-high", 1, 125},
            {"shared-high-canopy", 2, 62198}}};

        std::vector<Sample> samples;
        samples.reserve(fixedVertices.size());
        for (const FixedVertex& fixed : fixedVertices)
        {
            if (fixed.sectionIndex >= model.sections.size())
            {
                throw std::runtime_error("Oak fixed wind sample section is missing: " + std::string(fixed.label));
            }
            const MeshSection& section = model.sections[fixed.sectionIndex];
            if (fixed.vertexIndex >= section.vertices.size())
            {
                throw std::runtime_error("Oak fixed wind sample vertex is missing: " + std::string(fixed.label));
            }
            samples.push_back({
                fixed.label,
                fixed.sectionIndex,
                fixed.vertexIndex,
                section.vertices[fixed.vertexIndex]});
        }
        return samples;
    }

    void ValidateLayerCoverage(
        const Sample& sample,
        const StagePositions& stages)
    {
        constexpr float MinimumObservedMotion = 1.0e-5f;
        float observedMotion = 0.0f;
        if (sample.label == "branch1-branch2")
        {
            observedMotion = std::min(
                (stages.branch2 - stages.ripple).norm(),
                (stages.branch1 - stages.branch2).norm());
        }
        else if (sample.label == "ripple-high")
        {
            observedMotion = (stages.ripple - sample.vertex.position).norm();
        }
        else if (sample.label == "shared-high-canopy")
        {
            observedMotion = (stages.shared - stages.branch1).norm();
        }
        else
        {
            return;
        }

        if (observedMotion <= MinimumObservedMotion)
        {
            throw std::runtime_error(
                "Fixed sample no longer exercises its intended wind layer: " + sample.label);
        }
    }

    void PrintConnectivitySummary(const ModelResource& model)
    {
        std::cout << "mesh connectivity:\n";
        for (size_t sectionIndex = 0; sectionIndex < model.sections.size(); ++sectionIndex)
        {
            const MeshSection& section = model.sections[sectionIndex];
            DisjointSet components(section.vertices.size());
            for (size_t index = 0; index + 2 < section.indices.size(); index += 3)
            {
                components.Join(section.indices[index], section.indices[index + 1]);
                components.Join(section.indices[index], section.indices[index + 2]);
            }

            // Oak's index buffer is intentionally expanded to triangle corners.
            // Weld coincident rest positions only for connectivity inspection;
            // render vertices remain untouched so authored UV/TBN/wind seams stay intact.
            std::unordered_map<PositionKey, size_t, PositionKeyHash> vertexByPosition;
            for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
            {
                const PositionKey key = MakePositionKey(section.vertices[vertexIndex].position);
                const auto inserted = vertexByPosition.emplace(key, vertexIndex);
                if (!inserted.second)
                {
                    components.Join(vertexIndex, inserted.first->second);
                }
            }

            std::unordered_map<size_t, size_t> componentSizes;
            for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
            {
                ++componentSizes[components.Find(vertexIndex)];
            }

            std::vector<size_t> sizes;
            sizes.reserve(componentSizes.size());
            for (const auto& entry : componentSizes)
            {
                sizes.push_back(entry.second);
            }
            std::sort(sizes.begin(), sizes.end(), std::greater<size_t>());

            size_t tinyCount = 0;
            size_t smallCount = 0;
            size_t mediumCount = 0;
            size_t largeCount = 0;
            for (size_t size : sizes)
            {
                if (size <= 4)
                {
                    ++tinyCount;
                }
                else if (size <= 16)
                {
                    ++smallCount;
                }
                else if (size <= 64)
                {
                    ++mediumCount;
                }
                else
                {
                    ++largeCount;
                }
            }

            std::cout << "  section=" << sectionIndex
                << " material=" << section.materialSlotName
                << " vertices=" << section.vertices.size()
                << " components=" << sizes.size()
                << " sizes[1..4/5..16/17..64/65+]="
                << tinyCount << "/" << smallCount << "/"
                << mediumCount << "/" << largeCount
                << " largest=";
            const size_t shown = std::min<size_t>(sizes.size(), 6);
            for (size_t index = 0; index < shown; ++index)
            {
                std::cout << (index == 0 ? "" : ",") << sizes[index];
            }
            std::cout << '\n';
        }
    }

    std::vector<ConnectionPair> FindBarkConnectionPairs(
        const MeshSection& section,
        const SpeedTreeWindStateGPU& state,
        const Vec3& engineInstancePosition)
    {
        constexpr float SearchDistance = 0.35f;
        constexpr float GridCellSize = SearchDistance;

        DisjointSet components(section.vertices.size());
        for (size_t index = 0; index + 2 < section.indices.size(); index += 3)
        {
            components.Join(section.indices[index], section.indices[index + 1]);
            components.Join(section.indices[index], section.indices[index + 2]);
        }

        std::unordered_map<PositionKey, size_t, PositionKeyHash> vertexByPosition;
        for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
        {
            const PositionKey key = MakePositionKey(section.vertices[vertexIndex].position);
            const auto inserted = vertexByPosition.emplace(key, vertexIndex);
            if (!inserted.second)
            {
                components.Join(vertexIndex, inserted.first->second);
            }
        }

        std::vector<size_t> componentByVertex(section.vertices.size());
        for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
        {
            componentByVertex[vertexIndex] = components.Find(vertexIndex);
        }

        std::unordered_map<GridKey, std::vector<size_t>, GridKeyHash> spatialGrid;
        std::unordered_map<uint64_t, ConnectionPair> closestPairByComponents;
        for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
        {
            const Vec3& position = section.vertices[vertexIndex].position;
            const GridKey gridKey = MakeGridKey(position, GridCellSize);
            for (int32_t z = -1; z <= 1; ++z)
            {
                for (int32_t y = -1; y <= 1; ++y)
                {
                    for (int32_t x = -1; x <= 1; ++x)
                    {
                        const GridKey neighborKey = {
                            gridKey.x + x,
                            gridKey.y + y,
                            gridKey.z + z};
                        const auto neighborIt = spatialGrid.find(neighborKey);
                        if (neighborIt == spatialGrid.end())
                        {
                            continue;
                        }
                        for (size_t otherVertex : neighborIt->second)
                        {
                            const size_t firstComponent = componentByVertex[vertexIndex];
                            const size_t secondComponent = componentByVertex[otherVertex];
                            if (firstComponent == secondComponent)
                            {
                                continue;
                            }
                            const float distance =
                                (position - section.vertices[otherVertex].position).norm();
                            if (distance > SearchDistance)
                            {
                                continue;
                            }

                            const uint32_t componentLow = static_cast<uint32_t>(
                                std::min(firstComponent, secondComponent));
                            const uint32_t componentHigh = static_cast<uint32_t>(
                                std::max(firstComponent, secondComponent));
                            const uint64_t pairKey =
                                (static_cast<uint64_t>(componentLow) << 32) |
                                static_cast<uint64_t>(componentHigh);
                            const auto pairIt = closestPairByComponents.find(pairKey);
                            if (pairIt == closestPairByComponents.end() ||
                                distance < pairIt->second.restDistance)
                            {
                                closestPairByComponents[pairKey] = {
                                    firstComponent,
                                    secondComponent,
                                    vertexIndex,
                                    otherVertex,
                                    distance};
                            }
                        }
                    }
                }
            }
            spatialGrid[gridKey].push_back(vertexIndex);
        }

        std::vector<ConnectionPair> pairs;
        pairs.reserve(closestPairByComponents.size());
        for (auto& entry : closestPairByComponents)
        {
            ConnectionPair pair = entry.second;
            const Vertex& firstVertex = section.vertices[pair.firstVertex];
            const Vertex& secondVertex = section.vertices[pair.secondVertex];
            for (size_t channel = 0; channel < 4; ++channel)
            {
                pair.firstBranch1[channel] = firstVertex.speedTreeWindBranch1[channel];
                pair.firstBranch2[channel] = firstVertex.speedTreeWindBranch2[channel];
                pair.secondBranch1[channel] = secondVertex.speedTreeWindBranch1[channel];
                pair.secondBranch2[channel] = secondVertex.speedTreeWindBranch2[channel];
            }
            const StagePositions firstStages = EvaluateVulkanLearnVertex(
                section.vertices[pair.firstVertex], state, engineInstancePosition);
            const StagePositions secondStages = EvaluateVulkanLearnVertex(
                section.vertices[pair.secondVertex], state, engineInstancePosition);
            pair.stageDistances = {
                (firstStages.ripple - secondStages.ripple).norm(),
                (firstStages.branch2 - secondStages.branch2).norm(),
                (firstStages.branch1 - secondStages.branch1).norm(),
                (firstStages.shared - secondStages.shared).norm()};
            for (float distance : pair.stageDistances)
            {
                pair.maximumGrowth = std::max(
                    pair.maximumGrowth,
                    distance - pair.restDistance);
            }
            pairs.push_back(pair);
        }

        std::sort(
            pairs.begin(),
            pairs.end(),
            [](const ConnectionPair& left, const ConnectionPair& right)
            {
                return left.maximumGrowth > right.maximumGrowth;
            });
        return pairs;
    }

    void PrintBarkConnectionPairs(const std::vector<ConnectionPair>& pairs)
    {
        std::cout << "bark connection candidates=" << pairs.size() << " worst growth:\n";
        const size_t shown = std::min<size_t>(pairs.size(), 5);
        for (size_t pairIndex = 0; pairIndex < shown; ++pairIndex)
        {
            const ConnectionPair& pair = pairs[pairIndex];
            std::cout << "  vertices=" << pair.firstVertex << "/" << pair.secondVertex
                << " components=" << pair.firstComponent << "/" << pair.secondComponent
                << " rest=" << pair.restDistance
                << " ripple=" << pair.stageDistances[0]
                << " branch2=" << pair.stageDistances[1]
                << " branch1=" << pair.stageDistances[2]
                << " shared=" << pair.stageDistances[3]
                << " growth=" << pair.maximumGrowth
                << " weights=("
                << pair.firstBranch1[0] << "," << pair.firstBranch2[0] << "," << pair.firstBranch1[3]
                << ")/("
                << pair.secondBranch1[0] << "," << pair.secondBranch2[0] << "," << pair.secondBranch1[3]
                << ")\n";
        }
    }

    std::vector<size_t> BuildWeldedComponentIds(const MeshSection& section)
    {
        DisjointSet components(section.vertices.size());
        for (size_t index = 0; index + 2 < section.indices.size(); index += 3)
        {
            components.Join(section.indices[index], section.indices[index + 1]);
            components.Join(section.indices[index], section.indices[index + 2]);
        }
        std::unordered_map<PositionKey, size_t, PositionKeyHash> vertexByPosition;
        for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
        {
            const PositionKey key = MakePositionKey(section.vertices[vertexIndex].position);
            const auto inserted = vertexByPosition.emplace(key, vertexIndex);
            if (!inserted.second)
            {
                components.Join(vertexIndex, inserted.first->second);
            }
        }

        std::vector<size_t> componentByVertex(section.vertices.size());
        for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
        {
            componentByVertex[vertexIndex] = components.Find(vertexIndex);
        }
        return componentByVertex;
    }

    std::vector<CrossConnectionPair> FindClusterRootAttachmentPairs(
        const MeshSection& firstSection,
        const MeshSection& secondSection,
        const SpeedTreeWindStateGPU& state,
        const Vec3& engineInstancePosition)
    {
        constexpr float SearchDistance = 0.35f;
        constexpr float GridCellSize = SearchDistance;
        const std::vector<size_t> firstComponents = BuildWeldedComponentIds(firstSection);
        const std::vector<size_t> secondComponents = BuildWeldedComponentIds(secondSection);

        std::unordered_map<GridKey, std::vector<size_t>, GridKeyHash> secondGrid;
        for (size_t vertexIndex = 0; vertexIndex < secondSection.vertices.size(); ++vertexIndex)
        {
            const GridKey key = MakeGridKey(
                secondSection.vertices[vertexIndex].position, GridCellSize);
            secondGrid[key].push_back(vertexIndex);
        }

        std::unordered_map<uint64_t, CrossConnectionPair> closestPairByComponents;
        for (size_t firstVertex = 0; firstVertex < firstSection.vertices.size(); ++firstVertex)
        {
            // A spatially nearby card point is not necessarily its attachment.
            // The authored root has no child-branch or ripple displacement and
            // inherits the same packed Branch1 transform as the parent bark.
            if (firstSection.vertices[firstVertex].speedTreeWindBranch1.w() > 0.0f ||
                firstSection.vertices[firstVertex].speedTreeWindBranch2.x() > 0.0f)
            {
                continue;
            }
            const Vec3& firstPosition = firstSection.vertices[firstVertex].position;
            const GridKey gridKey = MakeGridKey(firstPosition, GridCellSize);
            for (int32_t z = -1; z <= 1; ++z)
            {
                for (int32_t y = -1; y <= 1; ++y)
                {
                    for (int32_t x = -1; x <= 1; ++x)
                    {
                        const GridKey neighborKey = {
                            gridKey.x + x,
                            gridKey.y + y,
                            gridKey.z + z};
                        const auto neighborIt = secondGrid.find(neighborKey);
                        if (neighborIt == secondGrid.end())
                        {
                            continue;
                        }
                        for (size_t secondVertex : neighborIt->second)
                        {
                            if (secondSection.vertices[secondVertex].speedTreeWindBranch2.x() > 0.0f)
                            {
                                continue;
                            }
                            const Eigen::Vector4f& firstBranch1 =
                                firstSection.vertices[firstVertex].speedTreeWindBranch1;
                            const Eigen::Vector4f& secondBranch1 =
                                secondSection.vertices[secondVertex].speedTreeWindBranch1;
                            if ((firstBranch1.head<3>().array() !=
                                 secondBranch1.head<3>().array()).any())
                            {
                                continue;
                            }
                            const float distance = (
                                firstPosition - secondSection.vertices[secondVertex].position).norm();
                            if (distance > SearchDistance)
                            {
                                continue;
                            }
                            const uint64_t pairKey =
                                (static_cast<uint64_t>(static_cast<uint32_t>(
                                    firstComponents[firstVertex])) << 32) |
                                static_cast<uint64_t>(static_cast<uint32_t>(
                                    secondComponents[secondVertex]));
                            const auto pairIt = closestPairByComponents.find(pairKey);
                            if (pairIt == closestPairByComponents.end() ||
                                distance < pairIt->second.restDistance)
                            {
                                closestPairByComponents[pairKey] = {
                                    firstComponents[firstVertex],
                                    secondComponents[secondVertex],
                                    firstVertex,
                                    secondVertex,
                                    distance};
                            }
                        }
                    }
                }
            }
        }

        std::vector<CrossConnectionPair> pairs;
        pairs.reserve(closestPairByComponents.size());
        for (auto& entry : closestPairByComponents)
        {
            CrossConnectionPair pair = entry.second;
            const Vertex& firstVertex = firstSection.vertices[pair.firstVertex];
            const Vertex& secondVertex = secondSection.vertices[pair.secondVertex];
            for (size_t channel = 0; channel < 4; ++channel)
            {
                pair.firstBranch1[channel] = firstVertex.speedTreeWindBranch1[channel];
                pair.firstBranch2[channel] = firstVertex.speedTreeWindBranch2[channel];
                pair.secondBranch1[channel] = secondVertex.speedTreeWindBranch1[channel];
                pair.secondBranch2[channel] = secondVertex.speedTreeWindBranch2[channel];
            }
            const StagePositions firstStages = EvaluateVulkanLearnVertex(
                firstSection.vertices[pair.firstVertex], state, engineInstancePosition);
            const StagePositions secondStages = EvaluateVulkanLearnVertex(
                secondSection.vertices[pair.secondVertex], state, engineInstancePosition);
            pair.stageDistances = {
                (firstStages.ripple - secondStages.ripple).norm(),
                (firstStages.branch2 - secondStages.branch2).norm(),
                (firstStages.branch1 - secondStages.branch1).norm(),
                (firstStages.shared - secondStages.shared).norm()};
            for (float distance : pair.stageDistances)
            {
                pair.maximumGrowth = std::max(
                    pair.maximumGrowth,
                    distance - pair.restDistance);
            }
            pairs.push_back(pair);
        }
        std::sort(
            pairs.begin(),
            pairs.end(),
            [](const CrossConnectionPair& left, const CrossConnectionPair& right)
            {
                return left.maximumGrowth > right.maximumGrowth;
            });
        return pairs;
    }

    bool SamePackedBranch1(const Vertex& left, const Vertex& right)
    {
        return (left.speedTreeWindBranch1.head<3>().array() ==
                right.speedTreeWindBranch1.head<3>().array()).all();
    }

    StagePositions InterpolateStages(
        const StagePositions& first,
        const StagePositions& second,
        const StagePositions& third,
        const Eigen::Vector3f& barycentric)
    {
        StagePositions result;
        result.ripple = first.ripple * barycentric.x() +
            second.ripple * barycentric.y() + third.ripple * barycentric.z();
        result.branch2 = first.branch2 * barycentric.x() +
            second.branch2 * barycentric.y() + third.branch2 * barycentric.z();
        result.branch1 = first.branch1 * barycentric.x() +
            second.branch1 * barycentric.y() + third.branch1 * barycentric.z();
        result.shared = first.shared * barycentric.x() +
            second.shared * barycentric.y() + third.shared * barycentric.z();
        return result;
    }

    std::vector<VisibleAttachmentSample> FindVisibleRootAttachmentSamples(
        const MeshSection& clusterSection,
        const MeshSection& barkSection,
        const AlphaImage& alphaImage,
        const SpeedTreeWindStateGPU& state,
        const Vec3& engineInstancePosition)
    {
        constexpr float SearchDistance = 0.35f;
        constexpr float AlphaClipThreshold = 0.1f;
        constexpr int BarycentricResolution = 12;

        std::vector<StagePositions> clusterStages(clusterSection.vertices.size());
        for (size_t vertexIndex = 0; vertexIndex < clusterSection.vertices.size(); ++vertexIndex)
        {
            clusterStages[vertexIndex] = EvaluateVulkanLearnVertex(
                clusterSection.vertices[vertexIndex], state, engineInstancePosition);
        }
        std::vector<StagePositions> barkStages(barkSection.vertices.size());
        for (size_t vertexIndex = 0; vertexIndex < barkSection.vertices.size(); ++vertexIndex)
        {
            barkStages[vertexIndex] = EvaluateVulkanLearnVertex(
                barkSection.vertices[vertexIndex], state, engineInstancePosition);
        }

        std::unordered_map<GridKey, std::vector<size_t>, GridKeyHash> barkGrid;
        for (size_t vertexIndex = 0; vertexIndex < barkSection.vertices.size(); ++vertexIndex)
        {
            const Vertex& vertex = barkSection.vertices[vertexIndex];
            if (vertex.speedTreeWindBranch2.x() > 0.0f ||
                vertex.speedTreeWindBranch1.w() > 0.0f)
            {
                continue;
            }
            barkGrid[MakeGridKey(vertex.position, SearchDistance)].push_back(vertexIndex);
        }

        std::vector<VisibleAttachmentSample> samples;
        for (size_t triangleIndex = 0;
             triangleIndex * 3 + 2 < clusterSection.indices.size();
             ++triangleIndex)
        {
            const size_t firstIndex = clusterSection.indices[triangleIndex * 3 + 0];
            const size_t secondIndex = clusterSection.indices[triangleIndex * 3 + 1];
            const size_t thirdIndex = clusterSection.indices[triangleIndex * 3 + 2];
            const Vertex& firstVertex = clusterSection.vertices[firstIndex];
            const Vertex& secondVertex = clusterSection.vertices[secondIndex];
            const Vertex& thirdVertex = clusterSection.vertices[thirdIndex];
            if (!SamePackedBranch1(firstVertex, secondVertex) ||
                !SamePackedBranch1(firstVertex, thirdVertex))
            {
                continue;
            }

            VisibleAttachmentSample bestSample;
            bool foundSample = false;
            for (int secondAmount = 0; secondAmount <= BarycentricResolution; ++secondAmount)
            {
                for (int thirdAmount = 0;
                     thirdAmount <= BarycentricResolution - secondAmount;
                     ++thirdAmount)
                {
                    const float secondWeight =
                        static_cast<float>(secondAmount) / BarycentricResolution;
                    const float thirdWeight =
                        static_cast<float>(thirdAmount) / BarycentricResolution;
                    const Eigen::Vector3f barycentric(
                        1.0f - secondWeight - thirdWeight,
                        secondWeight,
                        thirdWeight);
                    const Eigen::Vector2f uv =
                        firstVertex.texCoord * barycentric.x() +
                        secondVertex.texCoord * barycentric.y() +
                        thirdVertex.texCoord * barycentric.z();
                    const float alpha = SampleAlpha(alphaImage, uv);
                    if (alpha <= AlphaClipThreshold)
                    {
                        continue;
                    }

                    const Vec3 restPosition =
                        firstVertex.position * barycentric.x() +
                        secondVertex.position * barycentric.y() +
                        thirdVertex.position * barycentric.z();
                    const GridKey gridKey = MakeGridKey(restPosition, SearchDistance);
                    float nearestDistance = SearchDistance;
                    size_t nearestBarkVertex = barkSection.vertices.size();
                    for (int32_t z = -1; z <= 1; ++z)
                    {
                        for (int32_t y = -1; y <= 1; ++y)
                        {
                            for (int32_t x = -1; x <= 1; ++x)
                            {
                                const GridKey neighborKey = {
                                    gridKey.x + x,
                                    gridKey.y + y,
                                    gridKey.z + z};
                                const auto neighborIt = barkGrid.find(neighborKey);
                                if (neighborIt == barkGrid.end())
                                {
                                    continue;
                                }
                                for (size_t barkVertexIndex : neighborIt->second)
                                {
                                    const Vertex& barkVertex = barkSection.vertices[barkVertexIndex];
                                    if (!SamePackedBranch1(firstVertex, barkVertex))
                                    {
                                        continue;
                                    }
                                    const float distance =
                                        (restPosition - barkVertex.position).norm();
                                    if (distance < nearestDistance)
                                    {
                                        nearestDistance = distance;
                                        nearestBarkVertex = barkVertexIndex;
                                    }
                                }
                            }
                        }
                    }

                    if (nearestBarkVertex == barkSection.vertices.size() ||
                        (foundSample && nearestDistance >= bestSample.restDistance))
                    {
                        continue;
                    }
                    foundSample = true;
                    bestSample.triangleIndex = triangleIndex;
                    bestSample.barkVertex = nearestBarkVertex;
                    bestSample.barycentric = barycentric;
                    bestSample.uv = uv;
                    bestSample.alpha = alpha;
                    bestSample.restDistance = nearestDistance;
                }
            }

            if (!foundSample)
            {
                continue;
            }

            const StagePositions visibleStages = InterpolateStages(
                clusterStages[firstIndex],
                clusterStages[secondIndex],
                clusterStages[thirdIndex],
                bestSample.barycentric);
            const StagePositions& barkVertexStages = barkStages[bestSample.barkVertex];
            bestSample.stageDistances = {
                (visibleStages.ripple - barkVertexStages.ripple).norm(),
                (visibleStages.branch2 - barkVertexStages.branch2).norm(),
                (visibleStages.branch1 - barkVertexStages.branch1).norm(),
                (visibleStages.shared - barkVertexStages.shared).norm()};
            for (float distance : bestSample.stageDistances)
            {
                bestSample.maximumGrowth = std::max(
                    bestSample.maximumGrowth,
                    distance - bestSample.restDistance);
            }
            samples.push_back(bestSample);
        }

        std::sort(
            samples.begin(),
            samples.end(),
            [](const VisibleAttachmentSample& left, const VisibleAttachmentSample& right)
            {
                return left.maximumGrowth > right.maximumGrowth;
            });
        return samples;
    }

    void PrintVisibleRootAttachmentSamples(
        const std::vector<VisibleAttachmentSample>& samples)
    {
        std::cout << "alpha-visible root candidates=" << samples.size() << " worst growth:\n";
        const size_t shown = std::min<size_t>(samples.size(), 5);
        for (size_t sampleIndex = 0; sampleIndex < shown; ++sampleIndex)
        {
            const VisibleAttachmentSample& sample = samples[sampleIndex];
            std::cout << "  triangle=" << sample.triangleIndex
                      << " barkVertex=" << sample.barkVertex
                      << " visibleRootOffset=" << sample.semanticRootDistance
                      << " rest=" << sample.restDistance
                      << " ripple=" << sample.stageDistances[0]
                      << " branch2=" << sample.stageDistances[1]
                      << " branch1=" << sample.stageDistances[2]
                      << " shared=" << sample.stageDistances[3]
                      << " growth=" << sample.maximumGrowth
                      << " alpha=" << sample.alpha
                      << " uv=" << sample.uv.x() << "," << sample.uv.y()
                      << '\n';
        }
    }

    void ValidateVisibleRootAttachmentSamples(
        const std::vector<VisibleAttachmentSample>& samples)
    {
        constexpr float VisibleRootGrowthTolerance = 0.05f;
        if (samples.empty())
        {
            throw std::runtime_error(
                "Oak alpha-visible root regression found no Cluster Branch samples.");
        }

        const float maximumGrowth = samples.front().maximumGrowth;
        std::cout << "alpha-visible root regression candidates=" << samples.size()
                  << " maximumGrowth=" << maximumGrowth
                  << " tolerance=" << VisibleRootGrowthTolerance
                  << " result=" << (maximumGrowth <= VisibleRootGrowthTolerance ? "PASS" : "FAIL")
                  << '\n';
        if (maximumGrowth > VisibleRootGrowthTolerance)
        {
            throw std::runtime_error(
                "Oak alpha-visible root deformation exceeded the authored connection tolerance.");
        }
    }

    std::vector<VisibleAttachmentSample> FindComponentVisibleRootSamples(
        const MeshSection& clusterSection,
        const MeshSection& barkSection,
        const std::vector<CrossConnectionPair>& rootPairs,
        const AlphaImage& alphaImage,
        const SpeedTreeWindStateGPU& state,
        const Vec3& engineInstancePosition)
    {
        constexpr float AlphaClipThreshold = 0.1f;
        constexpr int BarycentricResolution = 16;
        const std::vector<size_t> componentByVertex =
            BuildWeldedComponentIds(clusterSection);

        std::unordered_map<size_t, size_t> rootPairByComponent;
        for (size_t pairIndex = 0; pairIndex < rootPairs.size(); ++pairIndex)
        {
            const CrossConnectionPair& pair = rootPairs[pairIndex];
            const auto existing = rootPairByComponent.find(pair.firstComponent);
            if (existing == rootPairByComponent.end() ||
                pair.restDistance < rootPairs[existing->second].restDistance)
            {
                rootPairByComponent[pair.firstComponent] = pairIndex;
            }
        }

        std::unordered_map<size_t, VisibleAttachmentSample> bestByComponent;
        for (size_t triangleIndex = 0;
             triangleIndex * 3 + 2 < clusterSection.indices.size();
             ++triangleIndex)
        {
            const size_t firstIndex = clusterSection.indices[triangleIndex * 3 + 0];
            const size_t secondIndex = clusterSection.indices[triangleIndex * 3 + 1];
            const size_t thirdIndex = clusterSection.indices[triangleIndex * 3 + 2];
            const size_t component = componentByVertex[firstIndex];
            const auto rootPairIt = rootPairByComponent.find(component);
            if (rootPairIt == rootPairByComponent.end())
            {
                continue;
            }
            const CrossConnectionPair& rootPair = rootPairs[rootPairIt->second];
            const Vertex& firstVertex = clusterSection.vertices[firstIndex];
            const Vertex& secondVertex = clusterSection.vertices[secondIndex];
            const Vertex& thirdVertex = clusterSection.vertices[thirdIndex];
            const Vec3& semanticRootPosition =
                clusterSection.vertices[rootPair.firstVertex].position;

            for (int secondAmount = 0; secondAmount <= BarycentricResolution; ++secondAmount)
            {
                for (int thirdAmount = 0;
                     thirdAmount <= BarycentricResolution - secondAmount;
                     ++thirdAmount)
                {
                    const float secondWeight =
                        static_cast<float>(secondAmount) / BarycentricResolution;
                    const float thirdWeight =
                        static_cast<float>(thirdAmount) / BarycentricResolution;
                    const Eigen::Vector3f barycentric(
                        1.0f - secondWeight - thirdWeight,
                        secondWeight,
                        thirdWeight);
                    const Eigen::Vector2f uv =
                        firstVertex.texCoord * barycentric.x() +
                        secondVertex.texCoord * barycentric.y() +
                        thirdVertex.texCoord * barycentric.z();
                    const float alpha = SampleAlpha(alphaImage, uv);
                    if (alpha <= AlphaClipThreshold)
                    {
                        continue;
                    }

                    const Vec3 restPosition =
                        firstVertex.position * barycentric.x() +
                        secondVertex.position * barycentric.y() +
                        thirdVertex.position * barycentric.z();
                    const float semanticRootDistance =
                        (restPosition - semanticRootPosition).norm();
                    const auto existing = bestByComponent.find(component);
                    if (existing != bestByComponent.end() &&
                        semanticRootDistance >= existing->second.semanticRootDistance)
                    {
                        continue;
                    }

                    VisibleAttachmentSample sample;
                    sample.triangleIndex = triangleIndex;
                    sample.barkVertex = rootPair.secondVertex;
                    sample.barycentric = barycentric;
                    sample.uv = uv;
                    sample.alpha = alpha;
                    sample.semanticRootDistance = semanticRootDistance;
                    sample.restDistance =
                        (restPosition - barkSection.vertices[rootPair.secondVertex].position).norm();
                    bestByComponent[component] = sample;
                }
            }
        }

        std::vector<StagePositions> clusterStages(clusterSection.vertices.size());
        for (size_t vertexIndex = 0; vertexIndex < clusterSection.vertices.size(); ++vertexIndex)
        {
            clusterStages[vertexIndex] = EvaluateVulkanLearnVertex(
                clusterSection.vertices[vertexIndex], state, engineInstancePosition);
        }
        std::vector<StagePositions> barkStages(barkSection.vertices.size());
        for (size_t vertexIndex = 0; vertexIndex < barkSection.vertices.size(); ++vertexIndex)
        {
            barkStages[vertexIndex] = EvaluateVulkanLearnVertex(
                barkSection.vertices[vertexIndex], state, engineInstancePosition);
        }

        std::vector<VisibleAttachmentSample> samples;
        samples.reserve(bestByComponent.size());
        for (auto& entry : bestByComponent)
        {
            VisibleAttachmentSample sample = entry.second;
            const size_t firstIndex = clusterSection.indices[sample.triangleIndex * 3 + 0];
            const size_t secondIndex = clusterSection.indices[sample.triangleIndex * 3 + 1];
            const size_t thirdIndex = clusterSection.indices[sample.triangleIndex * 3 + 2];
            const StagePositions visibleStages = InterpolateStages(
                clusterStages[firstIndex],
                clusterStages[secondIndex],
                clusterStages[thirdIndex],
                sample.barycentric);
            const StagePositions& barkVertexStages = barkStages[sample.barkVertex];
            sample.stageDistances = {
                (visibleStages.ripple - barkVertexStages.ripple).norm(),
                (visibleStages.branch2 - barkVertexStages.branch2).norm(),
                (visibleStages.branch1 - barkVertexStages.branch1).norm(),
                (visibleStages.shared - barkVertexStages.shared).norm()};
            for (float distance : sample.stageDistances)
            {
                sample.maximumGrowth = std::max(
                    sample.maximumGrowth,
                    distance - sample.restDistance);
            }
            samples.push_back(sample);
        }

        std::sort(
            samples.begin(),
            samples.end(),
            [](const VisibleAttachmentSample& left, const VisibleAttachmentSample& right)
            {
                return left.maximumGrowth > right.maximumGrowth;
            });
        return samples;
    }

    void PrintCrossSectionConnectionPairs(
        const char* label,
        const std::vector<CrossConnectionPair>& pairs)
    {
        std::cout << label << " connection candidates=" << pairs.size() << " worst growth:\n";
        const size_t shown = std::min<size_t>(pairs.size(), 5);
        for (size_t pairIndex = 0; pairIndex < shown; ++pairIndex)
        {
            const CrossConnectionPair& pair = pairs[pairIndex];
            std::cout << "  vertices=" << pair.firstVertex << "/" << pair.secondVertex
                << " components=" << pair.firstComponent << "/" << pair.secondComponent
                << " rest=" << pair.restDistance
                << " ripple=" << pair.stageDistances[0]
                << " branch2=" << pair.stageDistances[1]
                << " branch1=" << pair.stageDistances[2]
                << " shared=" << pair.stageDistances[3]
                << " growth=" << pair.maximumGrowth
                << " weights=("
                << pair.firstBranch1[0] << "," << pair.firstBranch2[0] << "," << pair.firstBranch1[3]
                << ")/("
                << pair.secondBranch1[0] << "," << pair.secondBranch2[0] << "," << pair.secondBranch1[3]
                << ")\n";
        }
    }

    void ValidateRootAttachmentPairs(const std::vector<CrossConnectionPair>& pairs)
    {
        constexpr float RootAttachmentGrowthTolerance = 0.05f;
        if (pairs.empty())
        {
            throw std::runtime_error(
                "Oak root attachment regression found no semantically matching Cluster Branch/Bark pairs.");
        }

        const float maximumGrowth = pairs.front().maximumGrowth;
        std::cout << "root attachment regression candidates=" << pairs.size()
                  << " maximumGrowth=" << maximumGrowth
                  << " tolerance=" << RootAttachmentGrowthTolerance
                  << " result=" << (maximumGrowth <= RootAttachmentGrowthTolerance ? "PASS" : "FAIL")
                  << '\n';
        if (maximumGrowth > RootAttachmentGrowthTolerance)
        {
            throw std::runtime_error(
                "Oak root attachment deformation exceeded the authored connection tolerance.");
        }
    }

    void PrintClusterComponentWindSummary(
        const MeshSection& section,
        const SpeedTreeWindStateGPU& state)
    {
        DisjointSet components(section.vertices.size());
        for (size_t index = 0; index + 2 < section.indices.size(); index += 3)
        {
            components.Join(section.indices[index], section.indices[index + 1]);
            components.Join(section.indices[index], section.indices[index + 2]);
        }
        std::unordered_map<PositionKey, size_t, PositionKeyHash> vertexByPosition;
        for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
        {
            const PositionKey key = MakePositionKey(section.vertices[vertexIndex].position);
            const auto inserted = vertexByPosition.emplace(key, vertexIndex);
            if (!inserted.second)
            {
                components.Join(vertexIndex, inserted.first->second);
            }
        }

        std::unordered_map<size_t, std::vector<size_t>> verticesByComponent;
        for (size_t vertexIndex = 0; vertexIndex < section.vertices.size(); ++vertexIndex)
        {
            verticesByComponent[components.Find(vertexIndex)].push_back(vertexIndex);
        }
        std::vector<std::pair<size_t, std::vector<size_t>>> sortedComponents;
        sortedComponents.reserve(verticesByComponent.size());
        for (auto& entry : verticesByComponent)
        {
            sortedComponents.push_back(std::move(entry));
        }
        std::sort(
            sortedComponents.begin(),
            sortedComponents.end(),
            [](const auto& left, const auto& right)
            {
                return left.second.size() > right.second.size();
            });

        std::cout << "cluster component wind samples:\n";
        const size_t shown = std::min<size_t>(sortedComponents.size(), 20);
        for (size_t componentIndex = 0; componentIndex < shown; ++componentIndex)
        {
            const auto& component = sortedComponents[componentIndex];
            float branch1Min = 1.0f;
            float branch1Max = 0.0f;
            float branch2Min = 1.0f;
            float branch2Max = 0.0f;
            float rippleMin = 1.0f;
            float rippleMax = 0.0f;
            Vec3 branch1AnchorMean = Vec3::Zero();
            Vec3 branch2AnchorMean = Vec3::Zero();
            size_t branch1AnchorCount = 0;
            size_t branch2AnchorCount = 0;
            for (size_t vertexIndex : component.second)
            {
                const Vertex& vertex = section.vertices[vertexIndex];
                branch1Min = std::min(branch1Min, vertex.speedTreeWindBranch1.x());
                branch1Max = std::max(branch1Max, vertex.speedTreeWindBranch1.x());
                branch2Min = std::min(branch2Min, vertex.speedTreeWindBranch2.x());
                branch2Max = std::max(branch2Max, vertex.speedTreeWindBranch2.x());
                rippleMin = std::min(rippleMin, vertex.speedTreeWindBranch1.w());
                rippleMax = std::max(rippleMax, vertex.speedTreeWindBranch1.w());
                if (vertex.speedTreeWindBranch1.x() > 0.0f)
                {
                    branch1AnchorMean += vertex.position -
                        DecodeDirectionEngine(vertex.speedTreeWindBranch1.y()) *
                        vertex.speedTreeWindBranch1.x() * state.branchStretchLimits.x();
                    ++branch1AnchorCount;
                }
                if (vertex.speedTreeWindBranch2.x() > 0.0f)
                {
                    branch2AnchorMean += vertex.position -
                        DecodeDirectionEngine(vertex.speedTreeWindBranch2.y()) *
                        vertex.speedTreeWindBranch2.x() * state.branchStretchLimits.y();
                    ++branch2AnchorCount;
                }
            }
            if (branch1AnchorCount > 0)
            {
                branch1AnchorMean /= static_cast<float>(branch1AnchorCount);
            }
            if (branch2AnchorCount > 0)
            {
                branch2AnchorMean /= static_cast<float>(branch2AnchorCount);
            }
            float branch1AnchorSpread = 0.0f;
            float branch2AnchorSpread = 0.0f;
            for (size_t vertexIndex : component.second)
            {
                const Vertex& vertex = section.vertices[vertexIndex];
                if (vertex.speedTreeWindBranch1.x() > 0.0f)
                {
                    const Vec3 anchor = vertex.position -
                        DecodeDirectionEngine(vertex.speedTreeWindBranch1.y()) *
                        vertex.speedTreeWindBranch1.x() * state.branchStretchLimits.x();
                    branch1AnchorSpread = std::max(
                        branch1AnchorSpread, (anchor - branch1AnchorMean).norm());
                }
                if (vertex.speedTreeWindBranch2.x() > 0.0f)
                {
                    const Vec3 anchor = vertex.position -
                        DecodeDirectionEngine(vertex.speedTreeWindBranch2.y()) *
                        vertex.speedTreeWindBranch2.x() * state.branchStretchLimits.y();
                    branch2AnchorSpread = std::max(
                        branch2AnchorSpread, (anchor - branch2AnchorMean).norm());
                }
            }

            std::cout << "  component=" << component.first
                << " vertices=" << component.second.size()
                << " b1=" << branch1Min << ".." << branch1Max
                << " b2=" << branch2Min << ".." << branch2Max
                << " ripple=" << rippleMin << ".." << rippleMax
                << " anchorSpread1=" << branch1AnchorSpread
                << " anchorSpread2=" << branch2AnchorSpread << '\n';
        }
    }

    std::filesystem::path ResolveOakAsset(int argc, char** argv)
    {
        if (argc > 1)
        {
            return std::filesystem::path(argv[1]);
        }
        std::ifstream configFile(std::filesystem::current_path() / "config" / "config.json");
        if (!configFile.is_open())
        {
            throw std::runtime_error("Pass the Oak .stsdk path as argv[1] or run from the project root.");
        }
        const nlohmann::json config = nlohmann::json::parse(configFile);
        return std::filesystem::path(config.at("resourcePath").get<std::string>()) /
            "models" / "datas" / "Oak_Complex_Rules.stsdk";
    }

    void PrintSample(const Sample& sample)
    {
        std::cout << "  " << sample.label
            << " section=" << sample.sectionIndex
            << " vertex=" << sample.vertexIndex
            << " branch1Weight=" << sample.vertex.speedTreeWindBranch1.x()
            << " branch2Weight=" << sample.vertex.speedTreeWindBranch2.x()
            << " rippleWeight=" << sample.vertex.speedTreeWindBranch1.w() << '\n';
    }

    void ValidateGustDisableReset(
        const SpeedTreeWindConfig& sourceConfig,
        const Vec3& sourceBoundsMin,
        const Vec3& sourceBoundsMax)
    {
        SpeedTreeWindConfig config = sourceConfig;
        config.common.gustFrequency = 0.0f;
        config.common.gustStrengthMin = 0.4f;
        config.common.gustStrengthMax = 0.4f;
        config.common.gustDurationMin = 10.0f;
        config.common.gustDurationMax = 10.0f;
        config.common.gustRiseScalar = 0.01f;
        config.common.gustFallScalar = 0.01f;

        SpeedTreeWindSystem system;
        system.Configure(config, sourceBoundsMin, sourceBoundsMax);
        system.ForceGust();
        system.SetGustingEnabled(false);
        system.AdvanceTo(0.5);
        system.SetGustingEnabled(true);
        system.AdvanceTo(2.0);

        if (std::abs(system.GetGpuState().windVector.w() - 0.5f) > ValidationTolerance)
        {
            throw std::runtime_error("SpeedTree gust disable reset regression failed.");
        }
        std::cout << "gust-disable-reset result=PASS\n";
    }

    void ValidateQuaternionDirectionResponse(
        const SpeedTreeWindConfig& sourceConfig,
        const Vec3& sourceBoundsMin,
        const Vec3& sourceBoundsMax)
    {
        SpeedTreeWindConfig config = sourceConfig;
        config.common.directionResponse = 2.0f;
        config.common.gustFrequency = 0.0f;

        SpeedTreeWindSystem rightAngleSystem;
        rightAngleSystem.Configure(config, sourceBoundsMin, sourceBoundsMax);
        rightAngleSystem.SetDirection(Vec3::UnitZ());
        rightAngleSystem.AdvanceTo(0.75);
        const Vec3 rightAngleMidpoint =
            rightAngleSystem.GetGpuState().windVector.head<3>();
        const Vec3 expectedRightAngleMidpoint = Normalize(Vec3(1.0f, 0.0f, 1.0f));
        if ((rightAngleMidpoint - expectedRightAngleMidpoint).norm() > ValidationTolerance)
        {
            throw std::runtime_error(
                "SpeedTree quaternion 90-degree direction response regression failed.");
        }
        rightAngleSystem.AdvanceTo(1.5);
        if ((rightAngleSystem.GetGpuState().windVector.head<3>() - Vec3::UnitZ()).norm() >
            ValidationTolerance)
        {
            throw std::runtime_error(
                "SpeedTree quaternion 90-degree direction target regression failed.");
        }

        SpeedTreeWindSystem oppositeSystem;
        oppositeSystem.Configure(config, sourceBoundsMin, sourceBoundsMax);
        oppositeSystem.SetDirection(-Vec3::UnitX());
        oppositeSystem.AdvanceTo(1.0);
        const Vec3 oppositeMidpoint = oppositeSystem.GetGpuState().windVector.head<3>();
        if (std::abs(oppositeMidpoint.norm() - 1.0f) > ValidationTolerance ||
            (oppositeMidpoint + Vec3::UnitZ()).norm() > ValidationTolerance)
        {
            throw std::runtime_error(
                "SpeedTree quaternion 180-degree direction response regression failed.");
        }
        oppositeSystem.AdvanceTo(2.0);
        if ((oppositeSystem.GetGpuState().windVector.head<3>() + Vec3::UnitX()).norm() >
            ValidationTolerance)
        {
            throw std::runtime_error(
                "SpeedTree quaternion 180-degree direction target regression failed.");
        }

        SpeedTreeWindSystem retargetSystem;
        retargetSystem.Configure(config, sourceBoundsMin, sourceBoundsMax);
        retargetSystem.SetDirection(-Vec3::UnitX());
        retargetSystem.AdvanceTo(0.5);
        const Vec3 directionBeforeRetarget =
            retargetSystem.GetGpuState().windVector.head<3>();
        retargetSystem.SetDirection(Vec3::UnitZ());
        const Vec3 directionAfterRetarget =
            retargetSystem.GetGpuState().windVector.head<3>();
        if ((directionAfterRetarget - directionBeforeRetarget).norm() > ValidationTolerance)
        {
            throw std::runtime_error(
                "SpeedTree quaternion direction retarget introduced an immediate jump.");
        }
        retargetSystem.AdvanceTo(10.0);
        if ((retargetSystem.GetGpuState().windVector.head<3>() - Vec3::UnitZ()).norm() >
            ValidationTolerance)
        {
            throw std::runtime_error(
                "SpeedTree quaternion direction retarget regression failed.");
        }

        std::cout << "quaternion-direction-response result=PASS\n";
    }

    void ValidateMultipleWindProfiles(
        const SpeedTreeWindConfig& oakConfig,
        const Vec3& sourceBoundsMin,
        const Vec3& sourceBoundsMax)
    {
        SpeedTreeWindProfile oakProfile;
        oakProfile.key = "oak";
        oakProfile.config = oakConfig;
        oakProfile.sourceBoundsMin = sourceBoundsMin;
        oakProfile.sourceBoundsMax = sourceBoundsMax;

        SpeedTreeWindProfile secondProfile = oakProfile;
        secondProfile.key = "second-species";
        secondProfile.config.branch1StretchLimit += 3.0f;
        secondProfile.sourceBoundsMax.x() += 5.0f;

        std::unordered_map<std::string, SpeedTreeWindProfile> profiles;
        profiles.emplace(oakProfile.key, oakProfile);
        profiles.emplace(secondProfile.key, secondProfile);

        SpeedTreeWindProfileSet profileSet;
        profileSet.Configure(profiles);
        if (profileSet.GetProfileCount() != 2 ||
            profileSet.FindGpuState("missing") != nullptr ||
            !profileSet.SetStrength(0.73f))
        {
            throw std::runtime_error("SpeedTree multi-profile setup regression failed.");
        }
        profileSet.AdvanceTo(0.5);

        const SpeedTreeWindStateGPU* oakState = profileSet.FindGpuState("oak");
        const SpeedTreeWindStateGPU* secondState = profileSet.FindGpuState("second-species");
        if (oakState == nullptr || secondState == nullptr ||
            std::abs(oakState->windVector.w() - secondState->windVector.w()) > ValidationTolerance ||
            std::abs(oakState->branchStretchLimits.x() - secondState->branchStretchLimits.x()) < 1.0f ||
            std::abs(oakState->treeBoundsMax.x() - secondState->treeBoundsMax.x()) < 1.0f)
        {
            throw std::runtime_error("SpeedTree wind profiles did not retain independent authored state.");
        }

        profileSet.Reset();
        if (profileSet.GetProfileCount() != 0 || profileSet.SetStrength(0.5f))
        {
            throw std::runtime_error("SpeedTree multi-profile reset regression failed.");
        }
        std::cout << "multi-profile isolation profiles=2 result=PASS\n";
    }

    void ValidateObjectLocalWindDirectionTransform()
    {
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        model.block<3, 3>(0, 0) =
            Eigen::AngleAxisf(0.73f, Eigen::Vector3f::UnitY()).toRotationMatrix() * 1.5f;
        const Eigen::Matrix3f worldToLocalDirection =
            VL::BuildSpeedTreeWorldToLocalDirectionMatrix(model);
        const std::array<Vec3, 2> worldDirections = {
            Normalize(Vec3(0.37f, 0.81f, -0.22f)),
            Normalize(Vec3(-0.61f, 0.28f, 0.74f))};

        Vec3 previousLocalDirection = Vec3::Zero();
        for (size_t index = 0; index < worldDirections.size(); ++index)
        {
            const Vec3 localDirection = VL::TransformSpeedTreeWindDirectionToLocal(
                worldToLocalDirection,
                worldDirections[index]);
            const Vec3 recoveredWorldDirection = Normalize(
                model.block<3, 3>(0, 0) * localDirection);
            if ((recoveredWorldDirection - worldDirections[index]).norm() > ValidationTolerance)
            {
                throw std::runtime_error("SpeedTree object-local wind direction transform regression failed.");
            }
            if (index > 0 &&
                (localDirection - previousLocalDirection).norm() <= ValidationTolerance)
            {
                throw std::runtime_error("SpeedTree real-time wind direction update regression failed.");
            }
            previousLocalDirection = localDirection;
        }
        std::cout << "object-local-wind-direction result=PASS\n";
    }
}

int RunSpeedTreeWindValidation(int argc, char** argv)
{
    try
    {
        std::string assetArgumentText;
        std::string exportJsonPathText;
        bool includeGeometry = false;
        bool exportOnly = false;
        for (int argumentIndex = 1; argumentIndex < argc; ++argumentIndex)
        {
            const std::string argument = argv[argumentIndex];
            if (argument == "--help" || argument == "-h")
            {
                std::cout
                    << "Usage: speedtree_wind_validation [asset.stsdk] "
                    << "[--export-json output.json] [--include-geometry] [--export-only]\n";
                return 0;
            }
            if (argument == "--export-json")
            {
                if (argumentIndex + 1 >= argc)
                {
                    throw std::runtime_error("--export-json requires an output path");
                }
                exportJsonPathText = argv[++argumentIndex];
                continue;
            }
            if (argument == "--include-geometry")
            {
                includeGeometry = true;
                continue;
            }
            if (argument == "--export-only")
            {
                exportOnly = true;
                continue;
            }
            if (!assetArgumentText.empty())
            {
                throw std::runtime_error("Only one SpeedTree asset path may be provided");
            }
            assetArgumentText = argument;
        }

        const std::filesystem::path assetPath = assetArgumentText.empty()
            ? ResolveOakAsset(0, nullptr)
            : PathFromCommandLineText(assetArgumentText);
        SpeedTreeSourceAdapter adapter;
        const std::string assetPathUtf8 = assetPath.u8string();
        const SpeedTreeSourceData source = adapter.ReadSource(assetPathUtf8, assetPathUtf8);

        if (!exportJsonPathText.empty())
        {
            const std::filesystem::path exportJsonPath = PathFromCommandLineText(exportJsonPathText);
            const std::filesystem::path parent = exportJsonPath.parent_path();
            if (!parent.empty())
            {
                std::filesystem::create_directories(parent);
            }
            std::ofstream output(exportJsonPath, std::ios::out | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error("Failed to open JSON export path: " + exportJsonPath.string());
            }
            output << BuildExportJson(assetPath, source, includeGeometry).dump(includeGeometry ? -1 : 2) << '\n';
            std::cout << "exported=" << exportJsonPath.u8string() << '\n';
            if (exportOnly)
            {
                return 0;
            }
        }

        const bool useOakReferenceValidation =
            assetPath.filename().string() == "Oak_Complex_Rules.stsdk";
        const std::vector<Sample> samples =
            SelectSamples(source.modelResource, useOakReferenceValidation);

        std::cout << std::fixed << std::setprecision(8);
        std::cout << "SpeedTree v10 SDK/VulkanLearn wind numeric validation\n";
        std::cout << "asset=" << assetPathUtf8 << "\n";
        std::cout << "representative vertices:\n";
        for (const Sample& sample : samples)
        {
            PrintSample(sample);
        }
        PrintConnectivitySummary(source.modelResource);

        const SpeedTreeWindConfig& validationConfig =
            source.modelResource.speedTreeWind;
        ValidateObjectLocalWindDirectionTransform();
        ValidateQuaternionDirectionResponse(
            validationConfig,
            source.modelResource.speedTreeSourceBoundsMin,
            source.modelResource.speedTreeSourceBoundsMax);
        ValidateMultipleWindProfiles(
            validationConfig,
            source.modelResource.speedTreeSourceBoundsMin,
            source.modelResource.speedTreeSourceBoundsMax);
        ValidateGustDisableReset(
            validationConfig,
            source.modelResource.speedTreeSourceBoundsMin,
            source.modelResource.speedTreeSourceBoundsMax);
        const SpeedTree::SConfigRuntimeSdk sdkConfig = MakeSdkConfig(
            validationConfig,
            source.modelResource.speedTreeSourceBoundsMin,
            source.modelResource.speedTreeSourceBoundsMax);
        SpeedTree::CWindStateMgr sdkWind;
        sdkWind.Configure(sdkConfig);
        sdkWind.SetSeed(137u);
        const Vec3 sourceInitialDirection(1.0f, 0.0f, 0.0f);
        sdkWind.SetInitialDirection(sourceInitialDirection.data());
        sdkWind.ForceStrength(0.5f);
        sdkWind.Tick(0.0f);

        SpeedTreeWindSystem vulkanLearnWind;
        vulkanLearnWind.Reset();
        vulkanLearnWind.Configure(
            validationConfig,
            source.modelResource.speedTreeSourceBoundsMin,
            source.modelResource.speedTreeSourceBoundsMax);
        vulkanLearnWind.SetStrength(0.5f);
        vulkanLearnWind.AdvanceTo(0.0);

        ErrorAccumulator cpuErrors;
        const std::array<float, 7> sampleTimes = {0.0f, 0.25f, 0.5f, 1.0f, 1.5f, 2.25f, 3.5f};
        for (float time : sampleTimes)
        {
            if (time == 0.25f)
            {
                sdkWind.SetStrength(0.82f);
                vulkanLearnWind.SetStrength(0.82f);
            }
            if (time == 1.0f)
            {
                sdkWind.GustImmediately();
                vulkanLearnWind.ForceGust();
            }
            sdkWind.Tick(time);
            vulkanLearnWind.AdvanceTo(time);
            const auto* sdkState = reinterpret_cast<const SpeedTree::SWindStateRuntimeSdk*>(sdkWind.GetShaderConstants());
            if (sdkState == nullptr)
            {
                throw std::runtime_error("SpeedTree SDK did not return Runtime SDK shader state.");
            }
            ErrorAccumulator frameErrors;
            CompareCpuState(frameErrors, *sdkState, vulkanLearnWind.GetGpuState());
            MergeErrors(cpuErrors, frameErrors);
            std::cout << "cpu-state t=" << time
                << " maxError=" << frameErrors.maximum
                << " worst=" << frameErrors.worstLabel << '\n';
        }

        const auto* sdkState = reinterpret_cast<const SpeedTree::SWindStateRuntimeSdk*>(sdkWind.GetShaderConstants());
        const Vec3 sourceInstancePosition(1.75f, -2.25f, 0.85f);
        const Vec3 engineInstancePosition = ConvertSourceVector(sourceInstancePosition);
        if (useOakReferenceValidation)
        {
            const std::vector<CrossConnectionPair> clusterToBarkPairs =
                FindClusterRootAttachmentPairs(
                    source.modelResource.sections[1],
                    source.modelResource.sections[0],
                    vulkanLearnWind.GetGpuState(),
                    engineInstancePosition);
            PrintCrossSectionConnectionPairs("cluster-root-to-bark-root", clusterToBarkPairs);
            ValidateRootAttachmentPairs(clusterToBarkPairs);
            const std::filesystem::path resourceRoot =
                assetPath.parent_path().parent_path().parent_path();
            const AlphaImage clusterBranchAlpha = LoadAlphaImage(
                resourceRoot / "textures/datas/T_OakClusterBranch_BaseColor.tga");
            const std::vector<VisibleAttachmentSample> visibleAttachmentSamples =
                FindComponentVisibleRootSamples(
                    source.modelResource.sections[1],
                    source.modelResource.sections[0],
                    clusterToBarkPairs,
                    clusterBranchAlpha,
                    vulkanLearnWind.GetGpuState(),
                    engineInstancePosition);
            PrintVisibleRootAttachmentSamples(visibleAttachmentSamples);
            ValidateVisibleRootAttachmentSamples(visibleAttachmentSamples);
        }
        ErrorAccumulator vertexErrors;
        for (const Sample& sample : samples)
        {
            const StagePositions sdkStages = EvaluateSdkVertex(
                ToSourceInput(sample.vertex), *sdkState, sourceInstancePosition);
            const StagePositions vulkanLearnStages = EvaluateVulkanLearnVertex(
                sample.vertex, vulkanLearnWind.GetGpuState(), engineInstancePosition);
            ValidateLayerCoverage(sample, vulkanLearnStages);
            ErrorAccumulator sampleErrors;
            CompareVector(sampleErrors, sample.label + ".ripple", ConvertSourceVector(sdkStages.ripple), vulkanLearnStages.ripple);
            CompareVector(sampleErrors, sample.label + ".branch2", ConvertSourceVector(sdkStages.branch2), vulkanLearnStages.branch2);
            CompareVector(sampleErrors, sample.label + ".branch1", ConvertSourceVector(sdkStages.branch1), vulkanLearnStages.branch1);
            CompareVector(sampleErrors, sample.label + ".shared", ConvertSourceVector(sdkStages.shared), vulkanLearnStages.shared);
            MergeErrors(vertexErrors, sampleErrors);
            std::cout << "vertex " << sample.label
                << " maxError=" << sampleErrors.maximum
                << " worst=" << sampleErrors.worstLabel << '\n';
        }

        const bool passed = !cpuErrors.failed && !vertexErrors.failed;
        std::cout << "summary cpuMaxError=" << cpuErrors.maximum
            << " vertexMaxError=" << vertexErrors.maximum
            << " tolerance=" << ValidationTolerance
            << " result=" << (passed ? "PASS" : "FAIL") << '\n';
        return passed ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "speedtree_wind_validation: " << exception.what() << '\n';
        return 2;
    }
}
