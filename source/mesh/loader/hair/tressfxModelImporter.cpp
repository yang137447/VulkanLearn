#include "mesh/loader/hair/tressfxModelImporter.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>

#include "hair/collisionMeshObj.h"
#include "hair/strandGeometry.h"

void TressfxModelImporter::ValidateImportRequest(const ModelImportRequest& request) const
{
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(request.absoluteModelPath)))
        throw std::runtime_error("TressFX model source not found: " + request.buildPlan.modelDataPath);
    // 作者声明 collision.mesh 时，代理 OBJ 是必需输入；缺失或不可读在加载期
    // 失败，而不是运行期静默退回只有胶囊的路径。
    if (request.buildPlan.tressfxSimulation.collisionMesh)
    {
        if (request.absoluteCollisionMeshPath.empty() ||
            !std::filesystem::is_regular_file(
                std::filesystem::u8path(request.absoluteCollisionMeshPath)))
            throw std::runtime_error(
                "TressFX collision mesh source not found: " +
                request.buildPlan.tressfxSimulation.collisionMesh->path);
    }
}

ModelResource TressfxModelImporter::Import(const ModelImportRequest& request)
{
    ValidateImportRequest(request);
    const auto parsed = VL::Hair::LoadTfx(std::filesystem::u8path(request.absoluteModelPath));
    if (parsed.IsFailure())
        throw std::runtime_error("TressFX model import failed: " + parsed.Error().message);
    const auto prepared = VL::Hair::PrepareStrandGeometry(parsed.Value(), request.absoluteModelPath);
    if (prepared.IsFailure())
        throw std::runtime_error("TressFX model geometry failed: " + prepared.Error().message);
    const auto& asset = parsed.Value();
    const auto& geometry = prepared.Value();
    // 共享 Vertex 较宽，导入预算单独限制派生流，避免合法 TFX 膨胀成数 GB 顶点。
    constexpr size_t MaxRibbonBytes = 512ull * 1024 * 1024;
    if (asset.positions.size() * 2 * sizeof(Vertex) +
        geometry.ribbonIndices.size() * sizeof(uint32_t) > MaxRibbonBytes)
        throw std::runtime_error("TressFX model ribbon exceeds 512 MiB import budget");

    ModelResource result;
    MeshSection section;
    section.sectionName = "guides";
    section.materialSlotName = "hair";
    section.isTressfx = true;
    section.tressfxPointsPerStrand = asset.pointsPerStrand;
    section.tressfxSimulation = request.buildPlan.tressfxSimulation;
    if (request.buildPlan.tressfxSimulation.collisionMesh)
    {
        // 代理解析一次并随导入结果共享；RenderableObject 每帧只消费三角形汤，
        // 不会重新读文件，也不会在每个场景 placement 上复制顶点。
        auto collisionMesh = VL::Hair::LoadCollisionMeshObj(
            std::filesystem::u8path(request.absoluteCollisionMeshPath));
        if (collisionMesh.IsFailure())
            throw std::runtime_error("TressFX collision mesh import failed: " +
                collisionMesh.Error().message);
        section.tressfxCollisionMesh = std::make_shared<const VL::Hair::SdfTriangleMesh>(
            std::move(collisionMesh.Value()));
    }
    section.tressfxInverseMass.reserve(asset.positions.size());
    section.indices = geometry.ribbonIndices;
    section.vertices.reserve(asset.positions.size() * 2);
    for (size_t point = 0; point < asset.positions.size(); ++point)
    {
        const size_t strand = point / asset.pointsPerStrand;
        const auto& position = asset.positions[point];
        section.tressfxInverseMass.push_back(position[3]);
        const auto& tangent = geometry.tangents[point];
        const double radius = static_cast<double>(request.buildPlan.strandRadius) *
            (asset.strandThickness.empty() ? 1.0 : asset.strandThickness[strand]);
        if (!std::isfinite(radius) || radius > std::numeric_limits<float>::max())
            throw std::runtime_error("TressFX model strand radius is not representable");
        const float localRadius = static_cast<float>(radius);
        section.boundsPadding = std::max(section.boundsPadding, localRadius);
        const float along = static_cast<float>(point % asset.pointsPerStrand) /
            static_cast<float>(asset.pointsPerStrand - 1);
        Eigen::Vector2f uv = Eigen::Vector2f::Zero();
        if (!asset.vertexUVs.empty())
            uv = Eigen::Vector2f(asset.vertexUVs[point][0], asset.vertexUVs[point][1]);
        else if (!asset.strandUVs.empty())
            uv = Eigen::Vector2f(asset.strandUVs[strand][0], asset.strandUVs[strand][1]);
        Eigen::Vector4f color = Eigen::Vector4f::Ones();
        if (!asset.vertexColors.empty())
            color = Eigen::Vector4f(asset.vertexColors[point].data());
        for (int side = 0; side < 2; ++side)
        {
            Vertex vertex(Eigen::Vector3f(position.data()), Eigen::Vector3f::UnitZ(),
                color, uv, Eigen::Vector4f(tangent[0], tangent[1], tangent[2], 1.0f));
            // location 7 在发丝工厂之前承载有符号半径/根梢参数；工厂之后才变成 ribbon UV。
            // position.w 是逆质量，不拷进齐次坐标或半径；没有源颜色时保持白色乘数。
            vertex.texCoord1 = Eigen::Vector2f(side == 0 ? -localRadius : localRadius, along);
            section.vertices.push_back(vertex);
        }
    }
    result.sourceMaterialSlotNames.push_back(section.materialSlotName);
    result.sections.push_back(std::move(section));
    return result;
}
