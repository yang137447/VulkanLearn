#include "hair/collisionMeshObj.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <vector>

namespace VL::Hair
{
namespace
{
RuntimeError ObjError(std::string message, const std::string& sourcePath, std::size_t line = 0)
{
    if (line != 0)
        message += " (" + (sourcePath.empty() ? std::string("<memory>") : sourcePath) +
            ":" + std::to_string(line) + ")";
    return MakeRuntimeError("Hair.CollisionMeshObj", std::move(message), sourcePath);
}

bool IsSpace(char value)
{
    return value == ' ' || value == '\t';
}

void Tokenize(std::string_view line, std::vector<std::string_view>& tokens)
{
    tokens.clear();
    std::size_t position = 0;
    while (position < line.size())
    {
        while (position < line.size() && IsSpace(line[position]))
            ++position;
        if (position >= line.size())
            break;
        std::size_t end = position;
        while (end < line.size() && !IsSpace(line[end]))
            ++end;
        tokens.push_back(line.substr(position, end - position));
        position = end;
    }
}

bool ParseFloat(std::string_view token, float& value)
{
    const std::string text(token);
    errno = 0;
    char* end = nullptr;
    value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || end != text.c_str() + text.size() || errno == ERANGE ||
        !std::isfinite(value))
        return false;
    return true;
}

bool ParseIndex(std::string_view token, long& value)
{
    const std::string text(token);
    errno = 0;
    char* end = nullptr;
    value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || end != text.c_str() + text.size() || errno == ERANGE)
        return false;
    return true;
}

// OBJ 面 token 的顶点部分：v | v/vt | v//vn | v/vt/vn。这里只消费斜杠前的索引。
std::string_view FaceIndexToken(std::string_view token)
{
    const std::size_t slash = token.find('/');
    return slash == std::string_view::npos ? token : token.substr(0, slash);
}
}

RuntimeResult<SdfTriangleMesh> ParseCollisionMeshObj(
    std::string_view text, std::string_view sourcePathView)
{
    const std::string sourcePath(sourcePathView);
    if (text.empty())
        return RuntimeResult<SdfTriangleMesh>::Failure(
            ObjError("collision mesh OBJ is empty", sourcePath));

    // UTF-8 BOM 在文本文件里常见但不属于 OBJ 语法；只在文件首部跳过。
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb &&
        static_cast<unsigned char>(text[2]) == 0xbf)
        text.remove_prefix(3);

    SdfTriangleMesh mesh;
    std::vector<std::string_view> tokens;
    std::size_t lineNumber = 0;
    std::size_t position = 0;
    while (position <= text.size())
    {
        const std::size_t lineEnd = text.find('\n', position);
        std::string_view line = lineEnd == std::string_view::npos
            ? text.substr(position) : text.substr(position, lineEnd - position);
        position = lineEnd == std::string_view::npos ? text.size() + 1 : lineEnd + 1;
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        Tokenize(line, tokens);
        if (tokens.empty() || tokens.front().front() == '#')
            continue;

        const std::string_view directive = tokens.front();
        if (directive == "v")
        {
            if (tokens.size() < 4)
                return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
                    "vertex requires three coordinates", sourcePath, lineNumber));
            Eigen::Vector3f position3;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (!ParseFloat(tokens[static_cast<std::size_t>(axis) + 1],
                        position3[axis]))
                    return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
                        "vertex has a malformed or non-finite coordinate", sourcePath,
                        lineNumber));
            }
            if (mesh.positions.size() >= TressfxCollisionMeshMaxVertices)
                return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
                    "collision mesh exceeds the vertex budget of " +
                        std::to_string(TressfxCollisionMeshMaxVertices),
                    sourcePath, lineNumber));
            mesh.positions.push_back(position3);
            continue;
        }
        if (directive == "f")
        {
            if (tokens.size() < 4)
                return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
                    "face requires at least three vertex indices", sourcePath, lineNumber));
            std::vector<std::uint32_t> polygon;
            polygon.reserve(tokens.size() - 1);
            for (std::size_t index = 1; index < tokens.size(); ++index)
            {
                const std::string_view indexToken = FaceIndexToken(tokens[index]);
                long raw = 0;
                if (indexToken.empty() || !ParseIndex(indexToken, raw) || raw == 0)
                    return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
                        "face has an invalid vertex index '" + std::string(tokens[index]) + "'",
                        sourcePath, lineNumber));
                long resolved = 0;
                if (raw < 0)
                {
                    resolved = static_cast<long>(mesh.positions.size()) + raw;
                    if (resolved < 0)
                        return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
                            "face negative index '" + std::string(indexToken) +
                                "' resolves before the first vertex",
                            sourcePath, lineNumber));
                }
                else
                {
                    resolved = raw - 1;
                    if (resolved >= static_cast<long>(mesh.positions.size()))
                        return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
                            "face index '" + std::string(indexToken) +
                                "' references a vertex that has not been read yet",
                            sourcePath, lineNumber));
                }
                polygon.push_back(static_cast<std::uint32_t>(resolved));
            }
            const std::size_t triangleCount = polygon.size() - 2;
            if (mesh.indices.size() / 3 + triangleCount > TressfxCollisionMeshMaxTriangles)
                return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
                    "collision mesh exceeds the triangle budget of " +
                        std::to_string(TressfxCollisionMeshMaxTriangles),
                    sourcePath, lineNumber));
            // 扇形三角化：与离线 vl_build_sdf / vl_build_collision_proxy 的读取规则一致。
            for (std::size_t corner = 1; corner + 1 < polygon.size(); ++corner)
            {
                mesh.indices.push_back(polygon[0]);
                mesh.indices.push_back(polygon[corner]);
                mesh.indices.push_back(polygon[corner + 1]);
            }
            continue;
        }

        // 标准 OBJ 的其他指令（o/g/s/vt/vn/usemtl/mtllib/...）一律忽略；只有
        // 首 token 不是纯字母的行才是真正的 malformed 输入。
        bool alphabetic = true;
        for (char character : directive)
        {
            if (!((character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z')))
            {
                alphabetic = false;
                break;
            }
        }
        if (!alphabetic)
            return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
                "unsupported OBJ line '" + std::string(line) + "'", sourcePath, lineNumber));
    }

    if (mesh.positions.empty())
        return RuntimeResult<SdfTriangleMesh>::Failure(
            ObjError("collision mesh OBJ has no vertices", sourcePath));
    if (mesh.indices.empty())
        return RuntimeResult<SdfTriangleMesh>::Failure(
            ObjError("collision mesh OBJ has no faces", sourcePath));
    return RuntimeResult<SdfTriangleMesh>::Success(std::move(mesh));
}

RuntimeResult<SdfTriangleMesh> LoadCollisionMeshObj(const std::filesystem::path& path)
{
    const std::string source = path.u8string();
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0)
        return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
            "collision mesh OBJ is missing, empty or unreadable", source));
    if (size > TressfxCollisionMeshMaxBytes)
        return RuntimeResult<SdfTriangleMesh>::Failure(ObjError(
            "collision mesh OBJ exceeds the " +
                std::to_string(TressfxCollisionMeshMaxBytes) + "-byte budget",
            source));
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return RuntimeResult<SdfTriangleMesh>::Failure(
            ObjError("cannot open collision mesh OBJ", source));
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())))
        return RuntimeResult<SdfTriangleMesh>::Failure(
            ObjError("cannot read the complete collision mesh OBJ", source));
    return ParseCollisionMeshObj(text, source);
}
}
