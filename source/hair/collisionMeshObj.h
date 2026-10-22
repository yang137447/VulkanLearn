#pragma once

#include "core/runtimeResult.h"
#include "hair/sdfBuilder.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace VL::Hair
{
// 逐帧碰撞代理的 OBJ 读取。纯 CPU、无 Vulkan 依赖，模块测试直接喂内存文本。
//
// 只接受三角形汤需要的子集：`v` 行写顶点，`f` 行写面（多边形按扇形三角化，
// 第 0 个顶点为扇心）；其余标准 OBJ 指令（`o`/`g`/`s`/`vt`/`vn`/`usemtl`/
// `mtllib` 等，首 token 全为字母）忽略，未知的非字母行按 malformed 拒绝。
// 面索引支持 `v`、`v/vt`、`v//vn`、`v/vt/vn` 四种写法以及负数相对索引
// （-1 表示当前已读到的最后一个顶点）。索引为 0、越界、或负数解析后越界
// 都在读取该行时带行号拒绝；顶点坐标必须有限。
//
// 该解析器是运行时唯一入口：TressfxModelImporter 在导入期把 OBJ 读成
// SdfTriangleMesh 并挂在 MeshSection 上，RenderableObject 只消费这份三角形汤，
// 不在每帧或每次 World 重建时重新解析文本。
inline constexpr std::uint64_t TressfxCollisionMeshMaxBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint32_t TressfxCollisionMeshMaxVertices = 4u * 1024u * 1024u;
inline constexpr std::uint32_t TressfxCollisionMeshMaxTriangles = 8u * 1024u * 1024u;

// 失败不发布部分结果。sourcePath 只用于错误消息；空表示内存夹具。
RuntimeResult<SdfTriangleMesh> ParseCollisionMeshObj(
    std::string_view text, std::string_view sourcePath = {});

// 带 64 MiB 上限的文件读取；路径不存在、不可读或超过上限都返回结构化错误。
RuntimeResult<SdfTriangleMesh> LoadCollisionMeshObj(const std::filesystem::path& path);
}
