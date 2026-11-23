#pragma once

#include "render/resource/terrainCompositedBakeGpu.h"
#include "render/backend/rendererBackendVulkan.h"
#include <map>
#include <cstring>
#include <cmath>
#include <stdexcept>

namespace TerrainCpuAddressOracle
{
using Baker = VL::TerrainCompositedBakeGpu;
using Matrix = std::array<double, 9>;
using Vector = std::array<float, 4>;
static_assert(sizeof(Vector) == 16, "Independent oracle vec4 std430 stride");
inline constexpr uint32_t VectorsPerSample = 5;

inline Matrix NormalMatrix(const std::array<float, 16>& source,
    const std::array<float, 16>& target)
{
    const double first = target[0], second = target[4], third = target[8];
    const double fourth = target[1], fifth = target[5], sixth = target[9];
    const double seventh = target[2], eighth = target[6], ninth = target[10];
    const double determinant = first * (fifth * ninth - sixth * eighth) -
        second * (fourth * ninth - sixth * seventh) + third * (fourth * eighth - fifth * seventh);
    const Matrix inverseTranspose{
        (fifth * ninth - sixth * eighth) / determinant,
        (sixth * seventh - fourth * ninth) / determinant,
        (fourth * eighth - fifth * seventh) / determinant,
        (third * eighth - second * ninth) / determinant,
        (first * ninth - third * seventh) / determinant,
        (second * seventh - first * eighth) / determinant,
        (second * sixth - third * fifth) / determinant,
        (third * fourth - first * sixth) / determinant,
        (first * fifth - second * fourth) / determinant};
    Matrix result{};
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t column = 0; column < 3; ++column)
            for (uint32_t axis = 0; axis < 3; ++axis)
                result[row * 3 + column] += source[row * 4 + axis] *
                    inverseTranspose[axis * 3 + column];
    return result;
}

inline std::vector<Vector> Build(const Baker::AdjacencySnapshot& snapshot,
    const std::string& sourceId, const Baker::PageRequest& page, uint32_t width, uint32_t height)
{
    const auto& ids = snapshot.graph.SurfaceIds();
    const auto& staticCharts = snapshot.graph.StaticCharts();
    const size_t sourceIndex = size_t(std::lower_bound(ids.begin(), ids.end(), sourceId) - ids.begin());
    if (sourceIndex >= ids.size() || ids[sourceIndex] != sourceId ||
        staticCharts.size() != ids.size())
        throw std::runtime_error("Independent oracle source/chart mismatch");
    std::map<std::string, uint32_t> gpuIndices;
    for (size_t index = 0; index < ids.size(); ++index)
        if (staticCharts[index].component == staticCharts[sourceIndex].component)
            gpuIndices.emplace(ids[index], uint32_t(gpuIndices.size()));
    const auto find = [&](const std::string& id) -> const Baker::InputSnapshot& {
        const auto found = std::find_if(snapshot.surfaces.begin(), snapshot.surfaces.end(),
            [&](const auto& item) { return item.surfaceId == id; });
        if (found == snapshot.surfaces.end())
            throw std::runtime_error("Independent oracle missing surface input");
        return found->input;
    };
    const auto& source = find(sourceId);
    std::map<std::string, Matrix> normalMatrices;
    for (const auto& item : gpuIndices)
        normalMatrices.emplace(item.first, NormalMatrix(source.localToWorld, find(item.first).localToWorld));
    const uint32_t samples = page.areaSamplesPerAxis;
    const uint32_t activeWidth = page.repeatSurface ? 136 : std::min(128u, width - page.pageX * 128) + 8;
    const uint32_t activeHeight = page.repeatSurface ? 136 : std::min(128u, height - page.pageY * 128) + 8;
    std::vector<Vector> result(size_t(activeWidth) * activeHeight * samples * samples * VectorsPerSample);
    for (uint32_t row = 0; row < activeHeight; ++row)
        for (uint32_t column = 0; column < activeWidth; ++column)
            for (uint32_t sample = 0; sample < samples * samples; ++sample)
            {
                const double horizontal = (double(page.pageX * 128 + column) - 4 +
                    (double(sample % samples) + .5) / samples) / width;
                const double vertical = (double(page.pageY * 128 + row) - 4 +
                    (double(sample / samples) + .5) / samples) / height;
                const auto address = page.repeatSurface
                    ? VL::TerrainCompositedAdjacencyAddress{sourceId,
                        {horizontal - std::floor(horizontal), vertical - std::floor(vertical)},
                        {1, 0, 0, 1}, 0, false}
                    : snapshot.graph.Resolve(sourceId, {horizontal, vertical});
                const auto& jacobian = address.originalUvToResolvedUvJacobian;
                const auto& normal = normalMatrices.at(address.surfaceId);
                const size_t offset = ((size_t(row) * activeWidth + column) * samples * samples + sample) * VectorsPerSample;
                result[offset] = {float(address.uv[0]), float(address.uv[1]),
                    float(gpuIndices.at(address.surfaceId)), 0};
                result[offset + 1] = {
                    float(jacobian[0] / (double(width) * samples)),
                    float(jacobian[2] / (double(width) * samples)),
                    float(jacobian[1] / (double(height) * samples)),
                    float(jacobian[3] / (double(height) * samples))};
                for (uint32_t axis = 0; axis < 3; ++axis)
                    result[offset + 2 + axis] = {float(normal[axis * 3]), float(normal[axis * 3 + 1]),
                        float(normal[axis * 3 + 2]), 0};
            }
    return result;
}

struct Buffer
{
    vk::Buffer handle;
    vk::DeviceMemory memory;
    vk::DescriptorBufferInfo descriptor;
    std::vector<uint8_t> signature;
};

inline std::map<vk::Buffer, Buffer> buffers;

inline void Prepare(VL::RendererBackendVulkan& backend, vk::Buffer parameters,
    const void* world, size_t worldBytes, const void* charts, size_t chartBytes,
    const Baker::AdjacencySnapshot& snapshot, const std::string& sourceId,
    const Baker::PageRequest& page, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> signature(worldBytes + chartBytes);
    std::memcpy(signature.data(), world, worldBytes);
    std::memcpy(signature.data() + worldBytes, charts, chartBytes);
    auto& cached = buffers[parameters];
    if (cached.signature == signature)
        return;
    const auto values = Build(snapshot, sourceId, page, width, height);
    for (const auto& value : values)
        for (const float component : value)
            if (!std::isfinite(component))
                throw std::runtime_error("Independent oracle nonfinite sample record");
    if (cached.handle)
        backend.DestroyBuffer(cached.handle, cached.memory);
    const auto allocated = backend.CreateBuffer(values.size() * sizeof(Vector),
        vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent, "IndependentCpuAddressOracleOnly");
    cached.handle = allocated.first;
    cached.memory = allocated.second;
    auto* mapped = backend.MapMemory(cached.memory, values.size() * sizeof(Vector));
    std::memcpy(mapped, values.data(), values.size() * sizeof(Vector));
    backend.UnmapMemory(cached.memory);
    cached.descriptor = vk::DescriptorBufferInfo(cached.handle, 0, values.size() * sizeof(Vector));
    cached.signature = std::move(signature);
}

inline void Release(VL::RendererBackendVulkan& backend, vk::Buffer parameters)
{
    const auto found = buffers.find(parameters);
    if (found == buffers.end())
        return;
    backend.DestroyBuffer(found->second.handle, found->second.memory);
    buffers.erase(found);
}
}
