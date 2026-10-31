#include "mesh/loader/hair/tressfxModelImporter.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "hair/strandGeometry.h"
#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxRootCloud.h"

namespace
{
    // 描述符层只能校验数值域与 64 项上限；逐顶点曲线必须与 .tfx 的
    // pointsPerStrand 等长，否则 shader 会读到错位的作者值。TFX 解析后才能
    // 拿到该值，因此在导入边界做硬校验，消息带字段、长度、资产路径。
    void ValidateStiffnessCurveLength(const char* field, const std::vector<float>& curve,
        uint32_t pointsPerStrand, const std::string& meshAssetPath,
        const std::string& modelDataPath)
    {
        if (curve.empty())
            return;
        if (curve.size() == pointsPerStrand)
            return;
        throw std::runtime_error(
            "TressFX simulation." + std::string(field) + " has " +
            std::to_string(curve.size()) + " entries but " + meshAssetPath +
            " (" + modelDataPath + ") has " + std::to_string(pointsPerStrand) +
            " points per strand");
    }

    // 组声明转成 hair 层的最小表示。几何导入只做上界校验；真正的 guide->group
    // 展开由 RendererMeshLoader 用缓存的 guideCount 生成 TressfxGroupBinding，
    // 因此组编辑不会改变这里的导入结果。
    std::vector<VL::Hair::TressfxGroupDeclaration> GroupDeclarations(
        const MeshAssetBuildPlan& plan)
    {
        std::vector<VL::Hair::TressfxGroupDeclaration> declarations;
        declarations.reserve(plan.tressfxGuideGroups.size());
        for (const TressfxGuideGroupPlan& group : plan.tressfxGuideGroups)
        {
            VL::Hair::TressfxGroupDeclaration declaration;
            declaration.name = group.name;
            declaration.ranges = group.guideRanges;
            declarations.push_back(std::move(declaration));
        }
        return declarations;
    }
} // namespace

void TressfxModelImporter::ValidateImportRequest(const ModelImportRequest& request) const
{
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(request.absoluteModelPath)))
        throw std::runtime_error("TressFX model source not found: " + request.buildPlan.modelDataPath);
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
    // 两条作者曲线都必须与这份 .tfx 的 pointsPerStrand 等长；不匹配是作者错误，
    // 不能在运行时静默截断或补零。
    ValidateStiffnessCurveLength("vspStiffness",
        request.buildPlan.tressfxSimulation.vspStiffness, asset.pointsPerStrand,
        request.buildPlan.meshAssetPath, request.buildPlan.modelDataPath);
    ValidateStiffnessCurveLength("localShapeStiffness",
        request.buildPlan.tressfxSimulation.localShapeStiffness, asset.pointsPerStrand,
        request.buildPlan.meshAssetPath, request.buildPlan.modelDataPath);
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
    // 导入结果只保留几何；有效模拟配置、代理和权重由放置加载路径持有。
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
    // 只保留 guide 根点：加载期要从绑定姿态身体派生逐 strand 权重，而位置 buffer
    // 在 GPU 侧（device-local）不可回读。点数由 .tfx 头保证与
    // guideCount * pointsPerStrand 一致，解析器已校验点数集合。
    section.tressfxRootPositions.reserve(asset.guideCount);
    for (uint32_t strand = 0; strand < asset.guideCount; ++strand)
    {
        const auto& root = asset.positions[strand * asset.pointsPerStrand];
        section.tressfxRootPositions.push_back(
            Eigen::Vector3f(root[0], root[1], root[2]));
    }
    // 几何导入结果不保存 guide->group 映射。这里只校验声明上界，
    // 避免缓存一份“无法展开分组”的几何；展开在绑定阶段完成。
    section.tressfxGuideCount = asset.guideCount;
    VL::Hair::ValidateTressfxGuideGroups(GroupDeclarations(request.buildPlan),
        asset.guideCount,
        " (" + request.buildPlan.meshAssetPath + ", " + request.buildPlan.modelDataPath + ")");
    // 笔刷选择缓存与导入结果同寿命：position 用 rest p0，growthDirection 用
    // normalize(p1-p0)；normal 在 root cloud 构建时由邻域 PCA 估计，缺失保持零。
    // 点云不含分组号，组编辑不触碰这份几何缓存。
    {
        std::vector<Eigen::Vector3f> rootDirections;
        rootDirections.reserve(asset.guideCount);
        for (uint32_t strand = 0; strand < asset.guideCount; ++strand)
        {
            const auto& root = asset.positions[strand * asset.pointsPerStrand];
            const auto& next = asset.positions[strand * asset.pointsPerStrand + 1];
            const Eigen::Vector3f rootPosition(root[0], root[1], root[2]);
            const Eigen::Vector3f nextPosition(next[0], next[1], next[2]);
            const Eigen::Vector3f delta = nextPosition - rootPosition;
            const float length = delta.norm();
            rootDirections.push_back(length > 1.0e-8f
                ? Eigen::Vector3f(delta / length) : Eigen::Vector3f::Zero());
        }
        section.tressfxRootCloud = VL::Hair::TressfxRootCloud::Build(
            section.tressfxRootPositions, rootDirections);
    }
    result.sections.push_back(std::move(section));
    return result;
}
