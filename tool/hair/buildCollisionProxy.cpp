#include "hair/collisionProxy.h"

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
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
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
using VL::Hair::CollisionProxyOptions;
using VL::Hair::SdfTriangleMesh;

// 厘米。1.5 cm 在正式身体网格上把头部区域压到几千个三角形，
// 同时保留鼻子、下巴、耳朵和肩部的主要轮廓。
constexpr float kDefaultCellSize = 1.5f;

struct Options
{
    std::filesystem::path input;
    std::filesystem::path output;
    float cellSize = kDefaultCellSize;
    // 保留命令行原文，供注释头精确记录参数（float 1.6f 展开成 1.60000002 不易读）。
    std::string cellSizeText = "1.5";
    bool hasRegion = false;
    Eigen::Vector3f regionMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f regionMax = Eigen::Vector3f::Zero();
    std::uint32_t minimumTriangleCount = 0;
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
    std::cerr << "vl_build_collision_proxy: " << message << '\n';
    return 1;
}

int Usage()
{
    std::cout <<
        "Usage: vl_build_collision_proxy --input <body.obj> --output <proxy.obj>\n"
        "                               [--cell-size <cm>] [--min-triangles <n>]\n"
        "                               [--region x0,y0,z0,x1,y1,z1]\n"
        "\n"
        "Offline vertex-clustering collision proxy builder for TressFX hair.\n"
        "The OBJ is read in its own local space and units (the female body asset is\n"
        "centimeters). Vertices snap to a world-axis grid of --cell-size, each cell is\n"
        "replaced by its vertex centroid, and triangles that collapse to a point or zero\n"
        "area, or duplicate another triangle, are dropped. The output keeps the input\n"
        "winding and is written as v/f lines only. --region keeps only triangles whose\n"
        "three vertices are inside the box (inclusive), so a cut proxy reports its open\n"
        "edges honestly. --min-triangles fails the build instead of silently coarsening\n"
        "the grid. Default cell size is 1.5 cm.\n";
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

bool ParseUnsigned(const std::string& text, std::uint64_t& value)
{
    if (text.empty() || text[0] == '-')
        return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || (end != nullptr && *end != '\0') || errno == ERANGE)
        return false;
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

// --region 的 6 个逗号分隔值：x0,y0,z0,x1,y1,z1（厘米，网格局部空间）。
bool ParseBox(const std::string& text, Eigen::Vector3f& boxMin, Eigen::Vector3f& boxMax)
{
    std::array<float, 6> values{};
    std::size_t start = 0;
    for (int i = 0; i < 6; ++i)
    {
        if (i < 5)
        {
            const std::size_t comma = text.find(',', start);
            if (comma == std::string::npos)
                return false;
            if (!ParseFloat(text.substr(start, comma - start), values[i]))
                return false;
            start = comma + 1;
        }
        else if (!ParseFloat(text.substr(start), values[i]))
        {
            return false;
        }
    }
    boxMin = Eigen::Vector3f(values[0], values[1], values[2]);
    boxMax = Eigen::Vector3f(values[3], values[4], values[5]);
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
            argument != "--region" && argument != "--min-triangles")
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
            options.cellSizeText = value;
        }
        else if (argument == "--region")
        {
            if (!ParseBox(value, options.regionMin, options.regionMax))
            {
                error = "--region expects x0,y0,z0,x1,y1,z1 in centimeters";
                return false;
            }
            options.hasRegion = true;
        }
        else if (argument == "--min-triangles")
        {
            std::uint64_t minimum = 0;
            if (!ParseUnsigned(value, minimum) ||
                minimum > std::numeric_limits<std::uint32_t>::max())
            {
                error = "--min-triangles must be a non-negative 32-bit integer";
                return false;
            }
            options.minimumTriangleCount = static_cast<std::uint32_t>(minimum);
        }
    }
    if (options.hasRegion && !(options.regionMin.x() < options.regionMax.x() &&
            options.regionMin.y() < options.regionMax.y() &&
            options.regionMin.z() < options.regionMax.z()))
    {
        error = "--region must have positive volume (x0<x1, y0<y1, z0<z1)";
        return false;
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
            VL::MakeRuntimeError("Hair.CollisionObjRead", "cannot open OBJ file", path.u8string()));

    ObjMesh result;
    std::string line;
    std::size_t lineNumber = 0;
    bool hasBounds = false;
    // 面顶点列表在循环外复用，避免每个 f 行都分配一次（正式资产约 23 万面）。
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
                        "Hair.CollisionObjParse",
                        "vertex has a missing or non-finite coordinate" +
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
                    "Hair.CollisionObjParse", "face has an invalid vertex index '" + token + "'" +
                        LineContext(path, lineNumber), path.u8string()));
            }
            if (raw < 0)
            {
                const long resolved =
                    static_cast<long>(result.mesh.positions.size()) + raw;
                if (resolved < 0)
                {
                    return VL::RuntimeResult<ObjMesh>::Failure(VL::MakeRuntimeError(
                        "Hair.CollisionObjParse",
                        "face references a negative index before enough vertices" +
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
                        "Hair.CollisionObjParse", "face references vertex " + std::to_string(raw) +
                            " after only " + std::to_string(result.mesh.positions.size()) +
                            " vertices" + LineContext(path, lineNumber), path.u8string()));
                }
                polygon.push_back(static_cast<std::uint32_t>(resolved));
            }
        }
        if (polygon.size() < 3)
        {
            return VL::RuntimeResult<ObjMesh>::Failure(VL::MakeRuntimeError(
                "Hair.CollisionObjParse", "face needs at least three vertices" +
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
            VL::MakeRuntimeError("Hair.CollisionObjParse", "OBJ file has no vertices",
                path.u8string()));
    }
    if (result.mesh.indices.empty())
    {
        return VL::RuntimeResult<ObjMesh>::Failure(
            VL::MakeRuntimeError("Hair.CollisionObjParse", "OBJ file has no faces",
                path.u8string()));
    }
    return VL::RuntimeResult<ObjMesh>::Success(std::move(result));
}

std::string RegionText(const Options& options)
{
    if (!options.hasRegion)
        return "none";
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(9) << options.regionMin.x() << ',' << options.regionMin.y() << ','
         << options.regionMin.z() << ',' << options.regionMax.x() << ','
         << options.regionMax.y() << ',' << options.regionMax.z();
    return text.str();
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
    const ObjMesh& obj = loaded.Value();
    const auto parseEnd = std::chrono::steady_clock::now();

    // 与 SDF 工具一致：与网格 AABB 完全不相交的区域没有可聚类表面，直接给出
    // 网格范围，避免只看到“区域拒绝所有三角形”的笼统消息。
    if (options.hasRegion &&
        (options.regionMax.x() < obj.boundsMin.x() || options.regionMin.x() > obj.boundsMax.x() ||
            options.regionMax.y() < obj.boundsMin.y() ||
            options.regionMin.y() > obj.boundsMax.y() ||
            options.regionMax.z() < obj.boundsMin.z() ||
            options.regionMin.z() > obj.boundsMax.z()))
    {
        return Fail("--region lies entirely outside the mesh bounds; mesh x [" +
            std::to_string(obj.boundsMin.x()) + ", " + std::to_string(obj.boundsMax.x()) +
            "], y [" + std::to_string(obj.boundsMin.y()) + ", " +
            std::to_string(obj.boundsMax.y()) + "], z [" + std::to_string(obj.boundsMin.z()) +
            ", " + std::to_string(obj.boundsMax.z()) + "]");
    }

    CollisionProxyOptions proxyOptions;
    proxyOptions.cellSize = options.cellSize;
    proxyOptions.hasRegion = options.hasRegion;
    proxyOptions.regionMin = options.regionMin;
    proxyOptions.regionMax = options.regionMax;
    proxyOptions.minimumTriangleCount = options.minimumTriangleCount;
    const auto built = VL::Hair::BuildCollisionProxy(obj.mesh, proxyOptions);
    if (built.IsFailure())
        return Fail(built.Error().message);
    const auto& proxy = built.Value();
    const auto buildEnd = std::chrono::steady_clock::now();

    // 注释头记录全部再生参数与计数，但不写时间戳或绝对构建机信息，
    // 因此同一输入与参数重新生成的文件逐字节相同。
    const std::string objectName = options.output.stem().empty()
        ? std::string("collision_proxy") : options.output.stem().u8string();
    const std::vector<std::string> commentLines{
        "vl_build_collision_proxy offline collision proxy (vertex clustering)",
        "input " + options.input.u8string(),
        "cellSize " + options.cellSizeText + " cm",
        "region " + RegionText(options),
        "minimumTriangleCount " + std::to_string(options.minimumTriangleCount),
        "inputVertices " + std::to_string(proxy.report.inputVertexCount) +
            " inputTriangles " + std::to_string(proxy.report.inputTriangleCount),
        "outputVertices " + std::to_string(proxy.report.outputVertexCount) +
            " outputTriangles " + std::to_string(proxy.report.outputTriangleCount),
        "clusters " + std::to_string(proxy.report.clusterCount) +
            " regionRejectedTriangles " +
            std::to_string(proxy.report.regionRejectedTriangleCount),
        "droppedDegenerateTriangles " +
            std::to_string(proxy.report.droppedDegenerateTriangleCount) +
            " droppedDuplicateTriangles " +
            std::to_string(proxy.report.droppedDuplicateTriangleCount),
        "openEdges " + std::to_string(proxy.report.openEdgeCount) +
            " nonManifoldEdges " + std::to_string(proxy.report.nonManifoldEdgeCount)};

    const auto written = VL::Hair::WriteCollisionProxyObj(
        proxy, options.output, objectName, commentLines);
    if (written.IsFailure())
        return Fail(written.Error().message);
    const auto writeEnd = std::chrono::steady_clock::now();

    nlohmann::json region = nullptr;
    if (options.hasRegion)
    {
        region = nlohmann::json{
            {"min", {options.regionMin.x(), options.regionMin.y(), options.regionMin.z()}},
            {"max", {options.regionMax.x(), options.regionMax.y(), options.regionMax.z()}}};
    }
    nlohmann::json inputBounds{
        {"min", {proxy.report.inputBoundsMin.x(), proxy.report.inputBoundsMin.y(),
            proxy.report.inputBoundsMin.z()}},
        {"max", {proxy.report.inputBoundsMax.x(), proxy.report.inputBoundsMax.y(),
            proxy.report.inputBoundsMax.z()}}};
    nlohmann::json outputBounds{
        {"min", {proxy.report.outputBoundsMin.x(), proxy.report.outputBoundsMin.y(),
            proxy.report.outputBoundsMin.z()}},
        {"max", {proxy.report.outputBoundsMax.x(), proxy.report.outputBoundsMax.y(),
            proxy.report.outputBoundsMax.z()}}};
    nlohmann::json report{
        {"input", options.input.u8string()},
        {"output", options.output.u8string()},
        {"bytes", std::filesystem::file_size(options.output)},
        {"objectName", objectName},
        {"cellSize", options.cellSize},
        {"region", region},
        {"minimumTriangleCount", options.minimumTriangleCount},
        {"inputVertices", proxy.report.inputVertexCount},
        {"inputTriangles", proxy.report.inputTriangleCount},
        {"regionRejectedTriangles", proxy.report.regionRejectedTriangleCount},
        {"clusters", proxy.report.clusterCount},
        {"outputVertices", proxy.report.outputVertexCount},
        {"outputTriangles", proxy.report.outputTriangleCount},
        {"droppedDegenerateTriangles", proxy.report.droppedDegenerateTriangleCount},
        {"droppedDuplicateTriangles", proxy.report.droppedDuplicateTriangleCount},
        {"openEdges", proxy.report.openEdgeCount},
        {"nonManifoldEdges", proxy.report.nonManifoldEdgeCount},
        {"inputBounds", inputBounds},
        {"outputBounds", outputBounds},
        {"parseMs", Milliseconds(parseEnd - start)},
        {"buildMs", Milliseconds(buildEnd - parseEnd)},
        {"writeMs", Milliseconds(writeEnd - buildEnd)},
        {"totalMs", Milliseconds(writeEnd - start)}};
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
        std::cerr << "vl_build_collision_proxy: " << error.what() << '\n';
        return 1;
    }
}
