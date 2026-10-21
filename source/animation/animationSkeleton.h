#pragma once

#include <Eigen/Dense>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace VL::Animation
{
struct SkeletonMesh;

// SK JSON 的父空间绑定变换：row-major 4×4，平移位于 [3]/[7]/[11]，单位厘米。
// 数据由 FBX 生成一次后提交；导入时它是权威校验清单，不反向驱动几何/权重导入。
struct SkeletonJoint
{
    std::string name;
    int parent = -1;
    Eigen::Matrix4f restLocal = Eigen::Matrix4f::Identity();
};

// 骨架身份、槽位与曲线注册表。曲线值不属于这里——只在 Sequence sidecar 中保存一份。
// joints/slots/curves 的所有权在本结构；导入完成后资产按值冻结，不再回写。
struct SkeletonAsset
{
    std::string id;
    std::string root;
    std::vector<SkeletonJoint> joints;
    std::vector<std::string> slots;
    // 原始拼写保留；类型当前只允许 "Float"（对应 UE CurveMetaData）。
    std::map<std::string, std::string> curves;
    // virtualBones / additiveReferencePose 第一切片只做结构校验与原样保留，不重建、不执行。
    std::string virtualBonesJson;
    std::string additiveReferencePoseJson;
    std::string sourceFbx;
    std::string sourceFbxDigest;
    std::string sourceTool;
    // SK 文件 BLAKE3-256 hex；由 loader 在读取文件字节后写入，JSON 自身不携带该字段。
    std::string sourceDigest;

    // 关节名按大小写敏感匹配（与当前 FBX 导入器一致）；未找到返回 -1。
    int JointIndex(std::string_view name) const;
    bool HasSlot(std::string_view name) const;
    // 曲线注册查询大小写不敏感；保留注册时的原始名称/类型。
    bool HasCurve(std::string_view name) const;
};

// basePath 只用于诊断与资源路径展示；解析器不读文件、不计算摘要。
// 格式/拓扑/父索引/restLocal/slots/curves 任一非法都抛出带 basePath 与字段名的异常。
SkeletonAsset ParseSkeleton(const nlohmann::json& json, std::string_view basePath);

// FBX 导入结果仍是几何/权重/绑定 TRS 的来源，SK 是权威校验清单。
// 关节数量、顺序、名称、父子与 restLocal（容差 1e-5）任一不符即拒绝，诊断包含关节名与差异值。
void ValidateSkeletonAgainstMesh(
    const SkeletonAsset& skeleton, const SkeletonMesh& mesh, std::string_view fbxPath);
}
