#pragma once

#include "render/resource/terrainCompositedBakeGpu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace TerrainPageReference
{
using Baker = VL::TerrainCompositedBakeGpu;
using Vector = std::array<double, 3>;
using Colour = std::array<double, 4>;

inline Vector Normalize(Vector value)
{
    double length = 0;
    for (const auto component : value)
        length += component * component;
    if (length <= 1.e-16)
        throw std::runtime_error("Reference encountered degenerate normal");
    for (auto& component : value)
        component /= std::sqrt(length);
    return value;
}

inline double SrgbDecode(double value)
{
    return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
}

inline double SrgbEncode(double value)
{
    value = std::clamp(value, 0., 1.);
    return value <= .0031308 ? value * 12.92 : 1.055 * std::pow(value, 1. / 2.4) - .055;
}

struct Image
{
    uint32_t width = 1, height = 1, layers = 1, mips = 1;
    bool srgb = false, repeat = false;
    std::vector<std::vector<Colour>> planes;

    const Colour& Pixel(uint32_t layer, uint32_t mip, int column, int row) const
    {
        const int columns = int(std::max(1u, width >> mip));
        const int rows = int(std::max(1u, height >> mip));
        if (repeat)
        {
            column = (column % columns + columns) % columns;
            row = (row % rows + rows) % rows;
        }
        else
        {
            column = std::clamp(column, 0, columns - 1);
            row = std::clamp(row, 0, rows - 1);
        }
        return planes[layer * mips + mip][row * columns + column];
    }

    Colour Sample(double horizontal, double vertical, uint32_t layer, double lod = 0) const
    {
        lod = std::clamp(lod, 0., double(mips - 1));
        const uint32_t first = uint32_t(std::floor(lod));
        const uint32_t last = std::min(first + 1, mips - 1);
        Colour result{};
        for (uint32_t level = 0; level < 2; ++level)
        {
            const uint32_t mip = level ? last : first;
            const double mipWeight = level ? lod - first : 1 - (lod - first);
            const double positionU = horizontal * std::max(1u, width >> mip) - .5;
            const double positionV = vertical * std::max(1u, height >> mip) - .5;
            const int column = int(std::floor(positionU)), row = int(std::floor(positionV));
            for (int offsetV = 0; offsetV < 2; ++offsetV)
                for (int offsetU = 0; offsetU < 2; ++offsetU)
                {
                    const double weight = mipWeight *
                        (offsetU ? positionU - column : 1 - (positionU - column)) *
                        (offsetV ? positionV - row : 1 - (positionV - row));
                    const auto& pixel = Pixel(layer, mip, column + offsetU, row + offsetV);
                    for (uint32_t component = 0; component < 4; ++component)
                        result[component] += weight *
                            (srgb && component < 3 ? SrgbDecode(pixel[component]) : pixel[component]);
                }
        }
        return result;
    }
};

struct Surface
{
    Baker::InputSnapshot input;
    std::map<std::string, Image> images;
    std::array<uint32_t, 4> coverageIds{};
    std::array<double, 4> coverageWeights{};
};

struct Value
{
    Vector colour{}, normal{}, orm{};
};

inline void Coverage(const Surface& surface, const std::array<double, 2>& domain,
    std::array<uint32_t, 4>& ids, std::array<double, 4>& weights)
{
    const auto& map = surface.images.at("ids");
    const auto& weightMap = surface.images.at("weights");
    const auto& input = surface.input;
    const double horizontal = std::clamp(
        std::clamp(input.coverageUvOffset[0] + domain[0] * input.coverageUvScale[0], 0., 1.) *
            map.width - .5, 0., double(map.width - 1));
    const double vertical = std::clamp(
        std::clamp(input.coverageUvOffset[1] + domain[1] * input.coverageUvScale[1], 0., 1.) *
            map.height - .5, 0., double(map.height - 1));
    const int column = int(std::floor(horizontal)), row = int(std::floor(vertical));
    std::map<uint32_t, double> mass;
    for (int offsetV = 0; offsetV < 2; ++offsetV)
        for (int offsetU = 0; offsetU < 2; ++offsetU)
        {
            const double factor =
                (offsetU ? horizontal - column : 1 - (horizontal - column)) *
                (offsetV ? vertical - row : 1 - (vertical - row));
            const auto& layers = map.Pixel(0, 0, column + offsetU, row + offsetV);
            const auto& tapWeights = weightMap.Pixel(0, 0, column + offsetU, row + offsetV);
            for (uint32_t slot = 0; slot < 4; ++slot)
                if (factor * tapWeights[slot] > 0)
                    mass[uint32_t(std::lround(layers[slot] * 255))] += factor * tapWeights[slot];
        }
    std::vector<std::pair<uint32_t, double>> ordered(mass.begin(), mass.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& first, const auto& second) {
        return first.second != second.second ? first.second > second.second : first.first < second.first;
    });
    ids = {};
    weights = {};
    for (uint32_t slot = 0; slot < std::min<size_t>(4, ordered.size()); ++slot)
    {
        ids[slot] = ordered[slot].first;
        weights[slot] = ordered[slot].second;
    }
}

inline Value Evaluate(const Surface& surface, const std::array<double, 2>& domain,
    const std::array<double, 4>& jacobian, const std::array<double, 2>& footprint)
{
    const auto& input = surface.input;
    const auto& images = surface.images;
    const std::array<double, 2> uv{
        input.surfaceUvOffset[0] + domain[0] * input.surfaceUvScale[0],
        input.surfaceUvOffset[1] + domain[1] * input.surfaceUvScale[1]};
    const auto frame = images.at("frame-normal").Sample(
        std::clamp(input.frameUvOffset[0] + domain[0] * input.frameUvScale[0], 0., 1.),
        std::clamp(input.frameUvOffset[1] + domain[1] * input.frameUvScale[1], 0., 1.), 0);
    const auto tangent = images.at("frame-tangent").Sample(
        std::clamp(input.frameUvOffset[0] + domain[0] * input.frameUvScale[0], 0., 1.),
        std::clamp(input.frameUvOffset[1] + domain[1] * input.frameUvScale[1], 0., 1.), 0);
    std::array<uint32_t, 4> ids = surface.coverageIds;
    std::array<double, 4> weights = surface.coverageWeights;
    if (input.weightSource == Baker::WeightSource::CoverageAtlas)
        Coverage(surface, domain, ids, weights);
    if (input.weightSource == Baker::WeightSource::LocalLayerArray)
    {
        ids = {};
        weights = {};
        const auto& local = images.at("local");
        const double horizontal = (1 + uv[0] * (local.width - 3)) / (local.width - 1);
        const double vertical = (1 + uv[1] * (local.height - 3)) / (local.height - 1);
        for (uint32_t layer = 0; layer < input.localLayerCount; ++layer)
        {
            const double weight = local.Sample(horizontal, vertical, layer)[0];
            for (uint32_t slot = 0; slot < 4; ++slot)
                if (weight > weights[slot])
                {
                    for (uint32_t move = 3; move > slot; --move)
                    {
                        weights[move] = weights[move - 1];
                        ids[move] = ids[move - 1];
                    }
                    weights[slot] = weight;
                    ids[slot] = input.localToGlobalLayer[layer];
                    break;
                }
        }
    }
    double total = 0;
    for (const auto weight : weights)
        total += weight;
    for (auto& weight : weights)
        weight /= total;
    const auto layerUv = [&](double horizontal, double vertical, const Colour& extra) {
        const double scale = input.uvScale * extra[2];
        return std::array<double, 2>{
            (std::cos(extra[3]) * horizontal - std::sin(extra[3]) * vertical) * scale,
            (std::sin(extra[3]) * horizontal + std::cos(extra[3]) * vertical) * scale};
    };
    const auto& catalog = images.at("catalog");
    if (input.retainedFragment.heightBlendStrength > 0)
    {
        std::array<double, 4> scores{-1.e9, -1.e9, -1.e9, -1.e9};
        for (uint32_t slot = 0; slot < 4; ++slot)
            if (weights[slot] > 1.e-5)
            {
                const auto extra = catalog.Pixel(0, 0, ids[slot], 1);
                const auto layer = layerUv(uv[0], uv[1], extra);
                const double height = images.at("height").Sample(layer[0], layer[1],
                    input.globalToSourceSlice[ids[slot]], 3)[0];
                const double displacement = catalog.Pixel(0, 0, ids[slot], 3)[3];
                scores[slot] = std::clamp((height - .5) * displacement *
                    input.retainedFragment.heightBlendStrength, -.25, .25) + 1 - weights[slot];
            }
        const double maximum = *std::max_element(scores.begin(), scores.end());
        total = 0;
        for (uint32_t slot = 0; slot < 4; ++slot)
        {
            weights[slot] *= std::clamp(scores[slot] - maximum + 1, 0., 1.);
            total += weights[slot];
        }
        for (auto& weight : weights)
            weight /= total;
    }
    const Vector bitangent{
        (frame[1] * tangent[2] - frame[2] * tangent[1]) * tangent[3],
        (frame[2] * tangent[0] - frame[0] * tangent[2]) * tangent[3],
        (frame[0] * tangent[1] - frame[1] * tangent[0]) * tangent[3]};
    Value result;
    for (uint32_t slot = 0; slot < 4; ++slot)
    {
        if (weights[slot] <= 0)
            continue;
        const uint32_t slice = input.globalToSourceSlice[ids[slot]];
        const auto extra = catalog.Pixel(0, 0, ids[slot], 1);
        const auto layer = layerUv(uv[0], uv[1], extra);
        const auto deltaU = layerUv(jacobian[0] * footprint[0] * input.surfaceUvScale[0],
            jacobian[2] * footprint[0] * input.surfaceUvScale[1], extra);
        const auto deltaV = layerUv(jacobian[1] * footprint[1] * input.surfaceUvScale[0],
            jacobian[3] * footprint[1] * input.surfaceUvScale[1], extra);
        const auto sample = [&](const std::string& name) {
            const auto& image = images.at(name);
            const double extent = std::max(
                std::hypot(deltaU[0] * image.width, deltaU[1] * image.height),
                std::hypot(deltaV[0] * image.width, deltaV[1] * image.height));
            return image.Sample(layer[0], layer[1], slice, std::max(0., std::log2(std::max(extent, 1.e-8))));
        };
        const auto colour = sample("albedo"), orm = sample("orm"), packedNormal = sample("normal");
        Vector normal{};
        for (uint32_t component = 0; component < 3; ++component)
            normal[component] = (packedNormal[0] * 2 - 1) * tangent[component] +
                (packedNormal[1] * 2 - 1) * bitangent[component] +
                (packedNormal[2] * 2 - 1) * frame[component];
        normal = Normalize(normal);
        for (uint32_t component = 0; component < 3; ++component)
        {
            result.colour[component] += colour[component] * weights[slot];
            result.orm[component] += orm[component] * weights[slot];
            result.normal[component] += normal[component] * weights[slot];
        }
    }
    result.normal = Normalize(result.normal);
    return result;
}

inline std::vector<uint8_t> Page(const VL::TerrainCompositedAdjacencyGraph& graph,
    const std::map<std::string, Surface>& surfaces, const std::string& source,
    const Baker::PageRequest& page)
{
    const uint32_t width = std::max(1u, page.virtualWidth >> page.virtualMip);
    const uint32_t height = std::max(1u, page.virtualHeight >> page.virtualMip);
    const uint32_t samples = page.areaSamplesPerAxis;
    const uint32_t activeWidth = page.repeatSurface ? Baker::StoredExtent :
        std::min(128u, width - page.pageX * 128) + 8;
    const uint32_t activeHeight = page.repeatSurface ? Baker::StoredExtent :
        std::min(128u, height - page.pageY * 128) + 8;
    const auto& sourceMatrix = surfaces.at(source).input.localToWorld;
    std::vector<uint8_t> result(Baker::PackedBytes);
    for (uint32_t row = 0; row < Baker::StoredExtent; ++row)
        for (uint32_t column = 0; column < Baker::StoredExtent; ++column)
        {
            if (column >= activeWidth || row >= activeHeight)
            {
                const uint32_t sourcePixel =
                    (std::min(row, activeHeight - 1) * Baker::StoredExtent +
                        std::min(column, activeWidth - 1)) * 4;
                const uint32_t targetPixel = (row * Baker::StoredExtent + column) * 4;
                for (uint32_t channel = 0; channel < 3; ++channel)
                    std::memcpy(result.data() + channel * Baker::ChannelBytes + targetPixel,
                        result.data() + channel * Baker::ChannelBytes + sourcePixel, 4);
                continue;
            }
            Value sum;
            for (uint32_t vertical = 0; vertical < samples; ++vertical)
                for (uint32_t horizontal = 0; horizontal < samples; ++horizontal)
                {
                    const std::array<double, 2> query{
                        (double(page.pageX * 128 + column) - 4 + (horizontal + .5) / samples) / width,
                        (double(page.pageY * 128 + row) - 4 + (vertical + .5) / samples) / height};
                    const auto address = page.repeatSurface
                        ? VL::TerrainCompositedAdjacencyAddress{source,
                            {query[0] - std::floor(query[0]), query[1] - std::floor(query[1])},
                            {1, 0, 0, 1}, 0, false}
                        : graph.Resolve(source, query);
                    const auto& target = surfaces.at(address.surfaceId);
                    auto value = Evaluate(target, address.uv, address.originalUvToResolvedUvJacobian,
                        {1. / (width * samples), 1. / (height * samples)});
                    Vector world{}, local{};
                    for (uint32_t component = 0; component < 3; ++component)
                        for (uint32_t axis = 0; axis < 3; ++axis)
                            world[component] += target.input.localToWorld[axis * 4 + component] *
                                value.normal[axis];
                    for (uint32_t component = 0; component < 3; ++component)
                        for (uint32_t axis = 0; axis < 3; ++axis)
                            local[component] += sourceMatrix[component * 4 + axis] * world[axis];
                    value.normal = Normalize(local);
                    for (uint32_t component = 0; component < 3; ++component)
                    {
                        sum.colour[component] += value.colour[component];
                        sum.normal[component] += value.normal[component];
                        sum.orm[component] += component == 1
                            ? value.orm[component] * value.orm[component] : value.orm[component];
                    }
                }
            sum.normal = Normalize(sum.normal);
            const double count = samples * samples;
            const uint32_t pixel = (row * Baker::StoredExtent + column) * 4;
            for (uint32_t component = 0; component < 3; ++component)
            {
                result[pixel + component] = uint8_t(std::lround(255 * SrgbEncode(sum.colour[component] / count)));
                result[Baker::ChannelBytes + pixel + component] = uint8_t(std::lround(
                    255 * std::clamp(sum.normal[component] * .5 + .5, 0., 1.)));
                const double orm = component == 1 ? std::sqrt(sum.orm[component] / count) : sum.orm[component] / count;
                result[2 * Baker::ChannelBytes + pixel + component] =
                    uint8_t(std::lround(255 * std::clamp(orm, 0., 1.)));
            }
            for (uint32_t channel = 0; channel < 3; ++channel)
                result[channel * Baker::ChannelBytes + pixel + 3] = 255;
        }
    return result;
}
}
