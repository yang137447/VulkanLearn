#include "hair/sdfBuilder.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <Eigen/Core>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
using VL::Hair::SdfField;
using VL::Hair::SdfGridSpec;
using VL::Hair::SdfTriangleMesh;

constexpr float kDefaultCellSize = 1.0f; // 厘米：头发碰撞的可用起点
constexpr float kDefaultBand = 4.0f;
constexpr std::uint64_t kDefaultMaxVoxels = 4ull * 1024ull * 1024ull;

struct Options
{
    std::filesystem::path input;
    std::filesystem::path output;
    float cellSize = kDefaultCellSize;
    float band = kDefaultBand;
    std::uint64_t maxVoxels = kDefaultMaxVoxels;
    std::vector<Eigen::Vector3f> samples;
    bool help = false;
};

struct ObjMesh
{
    SdfTriangleMesh mesh;
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
};

int Fail(const std::string& message)
{
    std::cerr << "vl_build_sdf: " << message << '\n';
    return 1;
}

int Usage()
{
    std::cout <<
        "Usage: vl_build_sdf --input <model.obj> --output <field.sdf>\n"
        "                    [--cell-size <cm>] [--max-voxels <n>] [--band <cm>]\n"
        "                    [--sample <x,y,z>]...\n"
        "\n"
        "Offline signed distance field builder for hair-vs-body collision.\n"
        "The OBJ is read in its own units (the female body asset is centimeters);\n"
        "negative distances are inside the mesh, positive outside, clamped to +/-band.\n"
        "The grid is the mesh AABB padded by --band on every side, so samples outside\n"
        "the domain are always farther than band from the surface and read as +band.\n";
    return 0;
}

bool ParseFloat(const std::string& text, float& value)
{
    errno = 0;
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || (end != nullptr && *end != '\0') || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

bool ParsePositiveU64(const std::string& text, std::uint64_t& value)
{
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || (end != nullptr && *end != '\0') || parsed == 0 ||
        errno == ERANGE)
        return false;
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ParsePoint(const std::string& text, Eigen::Vector3f& point)
{
    std::array<float, 3> values{};
    std::size_t start = 0;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (axis < 2)
        {
            const std::size_t comma = text.find(',', start);
            if (comma == std::string::npos)
                return false;
            if (!ParseFloat(text.substr(start, comma - start), values[axis]))
                return false;
            start = comma + 1;
        }
        else if (!ParseFloat(text.substr(start), values[axis]))
        {
            return false;
        }
    }
    point = Eigen::Vector3f(values[0], values[1], values[2]);
    return true;
}

double Milliseconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

bool ParseArguments(const std::vector<std::string>& args, Options& options, std::string& error)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string& argument = args[i];
        if (argument == "--help" || argument == "-h")
        {
            options.help = true;
            continue;
        }
        if (argument != "--input" && argument != "--output" && argument != "--cell-size" &&
            argument != "--band" && argument != "--max-voxels" && argument != "--sample")
        {
            error = "unknown argument: " + argument;
            return false;
        }
        if (i + 1 >= args.size())
        {
            error = "missing value for " + argument;
            return false;
        }
        const std::string& value = args[++i];
        if (argument == "--input")
            options.input = std::filesystem::u8path(value);
        else if (argument == "--output")
            options.output = std::filesystem::u8path(value);
        else if (argument == "--cell-size")
        {
            if (!ParseFloat(value, options.cellSize) || !(options.cellSize > 0.0f))
            {
                error = "--cell-size must be a finite positive number of centimeters";
                return false;
            }
        }
        else if (argument == "--band")
        {
            if (!ParseFloat(value, options.band) || !(options.band > 0.0f))
            {
                error = "--band must be a finite positive number of centimeters";
                return false;
            }
        }
        else if (argument == "--max-voxels")
        {
            if (!ParsePositiveU64(value, options.maxVoxels))
            {
                error = "--max-voxels must be a positive integer";
                return false;
            }
        }
        else if (argument == "--sample")
        {
            Eigen::Vector3f point;
            if (!ParsePoint(value, point))
            {
                error = "--sample expects x,y,z in centimeters";
                return false;
            }
            options.samples.push_back(point);
        }
    }
    if (options.help)
        return true;
    if (options.input.empty())
    {
        error = "--input is required";
        return false;
    }
    if (options.output.empty())
    {
        error = "--output is required";
        return false;
    }
    return true;
}

std::string LineContext(const std::filesystem::path& path, std::size_t line)
{
    return " (" + path.u8string() + ":" + std::to_string(line) + ")";
}

// 只读取 OBJ 的 v / f：vt、vn、材质、分组等行全部忽略。f 支持 v、v/vt、v//vn、
// v/vt/vn 形式以及负数相对索引，多边形按扇形三角化。顶点必须在引用它的面之前
// 出现（标准 OBJ 顺序），因此越界索引可以立刻带行号拒绝。
VL::RuntimeResult<ObjMesh> LoadObj(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return VL::RuntimeResult<ObjMesh>::Failure(
            VL::MakeRuntimeError("Hair.ObjRead", "cannot open OBJ file", path.u8string()));

    ObjMesh result;
    std::string line;
    std::size_t lineNumber = 0;
    bool hasBounds = false;
    // 面顶点列表在循环外复用，避免每个 f 行都分配一次（正式资产约 47 万面）。
    std::vector<std::uint32_t> polygon;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::size_t begin = 0;
        while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t'))
            ++begin;
        if (begin >= line.size() || line[begin] == '#')
            continue;
        const bool isVertex = line.compare(begin, 1, "v") == 0 &&
            begin + 1 < line.size() && (line[begin + 1] == ' ' || line[begin + 1] == '\t');
        const bool isFace = line.compare(begin, 1, "f") == 0 &&
            begin + 1 < line.size() && (line[begin + 1] == ' ' || line[begin + 1] == '\t');
        if (isVertex)
        {
            const char* cursor = line.c_str() + begin + 1;
            Eigen::Vector3f position = Eigen::Vector3f::Zero();
            for (int axis = 0; axis < 3; ++axis)
            {
                errno = 0;
                char* end = nullptr;
                const float value = std::strtof(cursor, &end);
                if (end == cursor || !std::isfinite(value))
                {
                    return VL::RuntimeResult<ObjMesh>::Failure(VL::MakeRuntimeError(
                        "Hair.ObjParse", "vertex has a missing or non-finite coordinate" +
                            LineContext(path, lineNumber), path.u8string()));
                }
                position[axis] = value;
                cursor = end;
            }
            result.mesh.positions.push_back(position);
            if (!hasBounds)
            {
                result.boundsMin = position;
                result.boundsMax = position;
                hasBounds = true;
            }
            else
            {
                result.boundsMin = result.boundsMin.cwiseMin(position);
                result.boundsMax = result.boundsMax.cwiseMax(position);
            }
            continue;
        }
        if (!isFace)
            continue;

        polygon.clear();
        std::size_t position = begin + 1;
        while (position < line.size())
        {
            while (position < line.size() &&
                (line[position] == ' ' || line[position] == '\t'))
                ++position;
            if (position >= line.size())
                break;
            std::size_t end = position;
            while (end < line.size() && line[end] != ' ' && line[end] != '\t')
                ++end;
            const std::string token = line.substr(position, end - position);
            position = end;
            const std::size_t slash = token.find('/');
            const std::string indexText =
                slash == std::string::npos ? token : token.substr(0, slash);
            errno = 0;
            char* parseEnd = nullptr;
            const long raw = std::strtol(indexText.c_str(), &parseEnd, 10);
            if (parseEnd == indexText.c_str() || (parseEnd != nullptr && *parseEnd != '\0') ||
                raw == 0 || errno == ERANGE)
            {
                return VL::RuntimeResult<ObjMesh>::Failure(VL::MakeRuntimeError(
                    "Hair.ObjParse", "face has an invalid vertex index '" + token + "'" +
                        LineContext(path, lineNumber), path.u8string()));
            }
            if (raw < 0)
            {
                const long resolved =
                    static_cast<long>(result.mesh.positions.size()) + raw;
                if (resolved < 0)
                {
                    return VL::RuntimeResult<ObjMesh>::Failure(VL::MakeRuntimeError(
                        "Hair.ObjParse", "face references a negative index before enough vertices" +
                            LineContext(path, lineNumber), path.u8string()));
                }
                polygon.push_back(static_cast<std::uint32_t>(resolved));
            }
            else
            {
                const long resolved = raw - 1;
                if (resolved < 0 || resolved >= static_cast<long>(result.mesh.positions.size()))
                {
                    return VL::RuntimeResult<ObjMesh>::Failure(VL::MakeRuntimeError(
                        "Hair.ObjParse", "face references vertex " + std::to_string(raw) +
                            " after only " + std::to_string(result.mesh.positions.size()) +
                            " vertices" + LineContext(path, lineNumber), path.u8string()));
                }
                polygon.push_back(static_cast<std::uint32_t>(resolved));
            }
        }
        if (polygon.size() < 3)
        {
            return VL::RuntimeResult<ObjMesh>::Failure(VL::MakeRuntimeError(
                "Hair.ObjParse", "face needs at least three vertices" +
                    LineContext(path, lineNumber), path.u8string()));
        }
        for (std::size_t corner = 1; corner + 1 < polygon.size(); ++corner)
        {
            result.mesh.indices.push_back(polygon[0]);
            result.mesh.indices.push_back(polygon[corner]);
            result.mesh.indices.push_back(polygon[corner + 1]);
        }
    }

    if (result.mesh.positions.empty())
    {
        return VL::RuntimeResult<ObjMesh>::Failure(
            VL::MakeRuntimeError("Hair.ObjParse", "OBJ file has no vertices", path.u8string()));
    }
    if (result.mesh.indices.empty())
    {
        return VL::RuntimeResult<ObjMesh>::Failure(
            VL::MakeRuntimeError("Hair.ObjParse", "OBJ file has no faces", path.u8string()));
    }
    return VL::RuntimeResult<ObjMesh>::Success(std::move(result));
}

int Run(const std::vector<std::string>& args)
{
    if (args.empty())
        return Usage();
    Options options;
    std::string argumentError;
    if (!ParseArguments(args, options, argumentError))
        return Fail(argumentError);
    if (options.help)
        return Usage();

    const auto start = std::chrono::steady_clock::now();
    const auto loaded = LoadObj(options.input);
    if (loaded.IsFailure())
        return Fail(loaded.Error().message);
    const auto& obj = loaded.Value();
    const auto parseEnd = std::chrono::steady_clock::now();

    // 域 = 网格 AABB 每侧外扩 band，保证域外点距表面一定超过 band，
    // 这样采样器把域外读成 +band 才与截断规则一致。
    const Eigen::Vector3f paddedMin = obj.boundsMin - Eigen::Vector3f::Constant(options.band);
    const Eigen::Vector3f paddedMax = obj.boundsMax + Eigen::Vector3f::Constant(options.band);
    const Eigen::Vector3f extent = paddedMax - paddedMin;
    std::array<std::uint32_t, 3> dims{1, 1, 1};
    for (int axis = 0; axis < 3; ++axis)
    {
        const double axisExtent = static_cast<double>(extent[axis]);
        const double cell = static_cast<double>(options.cellSize);
        const double count = std::ceil(axisExtent / cell);
        if (!(count >= 1.0) || !std::isfinite(count) || count > 1.0e9)
            return Fail("domain is degenerate on axis " + std::to_string(axis));
        dims[axis] = static_cast<std::uint32_t>(count);
        if (dims[axis] < 2)
        {
            // 单个体素无法表示距离梯度，也说明 cellSize 相对域过大；这是退化域。
            return Fail("cell size " + std::to_string(options.cellSize) +
                " cm makes axis " + std::to_string(axis) +
                " degenerate (fewer than two samples); decrease --cell-size");
        }
    }
    const std::uint64_t voxelCount = static_cast<std::uint64_t>(dims[0]) * dims[1] * dims[2];
    if (voxelCount > options.maxVoxels)
    {
        // 明确失败而不是悄悄放粗格距：碰撞精度由调用方决定。
        return Fail("cell size " + std::to_string(options.cellSize) + " cm needs " +
            std::to_string(voxelCount) + " voxels, above --max-voxels " +
            std::to_string(options.maxVoxels) + "; increase --cell-size or --max-voxels");
    }

    SdfGridSpec grid;
    grid.minCorner = paddedMin;
    grid.cellSize = options.cellSize;
    grid.dims = dims;
    grid.band = options.band;
    const auto built = VL::Hair::BuildSdf(obj.mesh, grid);
    if (built.IsFailure())
        return Fail(built.Error().message);
    const auto& field = built.Value();
    const auto buildEnd = std::chrono::steady_clock::now();

    const auto written = VL::Hair::WriteSdfFile(field, options.output);
    if (written.IsFailure())
        return Fail(written.Error().message);
    const auto writeEnd = std::chrono::steady_clock::now();

    // 统计与采样都从磁盘读回，确保报告描述的是实际写出的文件而不是内存副本。
    const auto reloaded = VL::Hair::LoadSdfFile(options.output);
    if (reloaded.IsFailure())
        return Fail("written SDF could not be read back: " + reloaded.Error().message);
    const SdfField& stored = reloaded.Value();

    std::uint64_t inside = 0;
    std::uint64_t outside = 0;
    std::uint64_t surface = 0;
    std::uint64_t negativeBand = 0;
    std::uint64_t positiveBand = 0;
    for (float distance : stored.distances)
    {
        if (distance < 0.0f) ++inside;
        else if (distance > 0.0f) ++outside;
        else ++surface;
        if (distance <= -stored.band + 1.0e-6f) ++negativeBand;
        if (distance >= stored.band - 1.0e-6f) ++positiveBand;
    }

    nlohmann::json samples = nlohmann::json::array();
    for (const Eigen::Vector3f& point : options.samples)
    {
        const float distance = VL::Hair::SampleSdf(stored, point);
        const bool outsideDomain = point.x() < stored.origin.x() ||
            point.y() < stored.origin.y() || point.z() < stored.origin.z() ||
            point.x() > stored.origin.x() + stored.cellSize * stored.dims[0] ||
            point.y() > stored.origin.y() + stored.cellSize * stored.dims[1] ||
            point.z() > stored.origin.z() + stored.cellSize * stored.dims[2];
        samples.push_back({
            {"point", {point.x(), point.y(), point.z()}},
            {"distance", distance},
            {"outsideDomain", outsideDomain},
            {"truncated", std::fabs(distance) >= stored.band - 1.0e-6f}});
    }

    nlohmann::json report{
        {"input", options.input.u8string()},
        {"output", options.output.u8string()},
        {"bytes", std::filesystem::file_size(options.output)},
        {"vertices", field.mesh.vertexCount},
        {"triangles", field.mesh.triangleCount},
        {"degenerateTriangles", field.mesh.degenerateTriangleCount},
        {"openEdges", field.mesh.openEdgeCount},
        {"nonManifoldEdges", field.mesh.nonManifoldEdgeCount},
        {"dims", {stored.dims[0], stored.dims[1], stored.dims[2]}},
        {"voxels", stored.distances.size()},
        {"origin", {stored.origin.x(), stored.origin.y(), stored.origin.z()}},
        {"cellSize", stored.cellSize},
        {"band", stored.band},
        {"insideVoxels", inside},
        {"outsideVoxels", outside},
        {"surfaceVoxels", surface},
        {"insideFraction", static_cast<double>(inside) /
            static_cast<double>(stored.distances.size())},
        {"negativeBandVoxels", negativeBand},
        {"positiveBandVoxels", positiveBand},
        {"parseMs", Milliseconds(parseEnd - start)},
        {"buildMs", Milliseconds(buildEnd - parseEnd)},
        {"writeMs", Milliseconds(writeEnd - buildEnd)},
        {"totalMs", Milliseconds(writeEnd - start)},
        {"samples", samples}};
    std::cout << report.dump(2) << '\n';
    return 0;
}

#ifdef _WIN32
std::string ToUtf8(const wchar_t* text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(length) - 1);
    return result;
}
#endif
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i)
    {
#ifdef _WIN32
        arguments.push_back(ToUtf8(argv[i]));
#else
        arguments.push_back(argv[i]);
#endif
    }
    try
    {
        return Run(arguments);
    }
    catch (const std::exception& error)
    {
        std::cerr << "vl_build_sdf: " << error.what() << '\n';
        return 1;
    }
}
