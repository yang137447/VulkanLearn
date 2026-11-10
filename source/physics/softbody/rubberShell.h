#pragma once

#include <cstdint>
#include <vector>

#include <Eigen/Core>

#include "physics/fracture/sphericalFracture.h"

namespace VL::Physics::Softbody
{

// 橡皮气球软壳参数。网格在运行时由 icosphere 生成；粒子、边约束、弯曲约束和压力
// 都不来自预制资产。当前只做闭环球壳，不处理任意开口布料拓扑。
struct RubberShellSettings
{
    // 外半径，单位与项目一致（厘米）。
    double radius = 20.0;
    // icosphere 细分次数；2 = 162 顶点 / 320 三角面，3 = 642 / 1280。
    int subdivisions = 2;
    // 总质量按顶点均分；只影响压力/重力响应，不影响约束几何。
    double totalMass = 4.0;
    // 橡皮膜拉伸与弯曲约束的 XPBD compliance；越小越硬。
    double edgeCompliance = 2.0e-4;
    double bendCompliance = 2.0e-2;
    // 应变刚化：high strain 时按 1 / (1 + stiffening * strain) 降低有效 compliance。
    // 0 = 线性橡皮；越大越接近“拉得越长越难拉”的橡胶。
    double strainStiffening = 8.0;
    // 沿结构边相对速度的粘弹阻尼，单位 1/s；0 = 无额外能量耗散。
    double edgeDamping = 2.0;
    // 速度阻尼，单位 1/s。
    double damping = 1.5;
    // 重力，厘米/秒²；气球默认接近无重力，由场景注入。
    Eigen::Vector3d gravity = Eigen::Vector3d(0.0, -981.0, 0.0);
    // 超过该结构边应变时认为橡皮膜进入爆裂阈值。
    double burstStrain = 0.35;
    // 每次 Step 的 XPBD 迭代次数。
    int constraintIterations = 8;
};

struct ShellParticle
{
    Eigen::Vector3d position = Eigen::Vector3d::Zero();
    Eigen::Vector3d previousPosition = Eigen::Vector3d::Zero();
    Eigen::Vector3d velocity = Eigen::Vector3d::Zero();
    double inverseMass = 0.0;
    bool pinned = false;
};

// structural=true 的是膜内结构边，参与应变/爆裂判断；false 的是弯曲约束。
struct ShellDistanceConstraint
{
    uint32_t first = 0;
    uint32_t second = 0;
    double restLength = 0.0;
    double compliance = 0.0;
    bool structural = false;
};

struct ShellTriangle
{
    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t third = 0;
};

struct RubberTearSettings
{
    // 破裂后碎片的静止边长相对破裂瞬间长度的比例；<1 表示橡胶回缩。
    double restContraction = 0.75;
    // 破裂瞬间继承给碎片的粒子速度比例。
    double fragmentVelocityScale = 1.0;
};

// CPU 侧软片渲染数据。引擎把 positions/normals 转成 Vertex 后写入动态
// RenderableObject；该结构不依赖 Vulkan，也不持有 GPU handle。
struct SoftBodyRenderMesh
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> normals;
    std::vector<uint32_t> indices;
};

// 运行时橡皮膜。GT/CPU 独占；渲染线程不直接读粒子数组。
// 当前负责压力充气、XPBD 拉伸/弯曲约束、爆裂应变检测和按球面 Voronoi 的软片拆分；
// 不创建 Jolt body，也不把粒子数组暴露给渲染线程。
class RubberShell
{
public:
    static RubberShell BuildIcosphere(const RubberShellSettings& settings);

    // 按运行时球面 Voronoi 分区把膜拆成多个软片；边界沿现有三角网格边走，
    // 不做三角形级精确裁剪。每个碎片保留自己的粒子、结构边和弯曲约束。
    static std::vector<RubberShell> TearBySphericalPattern(
        const RubberShell& shell,
        const Fracture::SphericalPattern& pattern,
        const RubberTearSettings& tearSettings = {});

    // 与上面的粗粒度拆分相同，但按球面 cell 的边界平面对三角面做精确裁剪；
    // 边界顶点会投影回原球壳径向，再重建碎片内部约束。
    static std::vector<RubberShell> TearBySphericalPatternExact(
        const RubberShell& shell,
        const Fracture::SphericalPattern& pattern,
        const RubberTearSettings& tearSettings = {});

    // dt 为秒；pressure 为压力系数，正值沿外法线充气。
    void Step(double dt, double pressure);

    double ComputeVolume() const;
    double MaximumStructuralStrain() const;
    bool ShouldBurst() const;
    // 气球完整状态通常接近悬浮；爆裂后的软片由调用方重新注入重力。
    void SetGravity(const Eigen::Vector3d& gravity) { settings.gravity = gravity; }
    void SetDamping(double damping);
    // 爆裂瞬间的橡皮膜储能释放：径向伸展 + 切向随机翻卷。
    // punctureDirection 是局部空间的命中点方向（单位向量）；给定后，命中点附近的
    // 粒子会按 punctureBoost 获得额外径向速度，表现“破口处先被气流冲开”。
    // 零向量或非正 boost 时退化为均匀爆裂冲量。
    void ApplyBurstImpulse(const Eigen::Vector3d& center,
        double radialSpeed, double tangentialSpeed, uint64_t seed,
        const Eigen::Vector3d& punctureDirection = Eigen::Vector3d::Zero(),
        double punctureBoost = 0.0);
    // 漏气：整体向内的收缩冲量（与爆裂方向相反），配合零压力让壳缓慢塌陷。
    void ApplyDeflateImpulse(const Eigen::Vector3d& center,
        double inwardSpeed, double tangentialSpeed, uint64_t seed);
    // 漏气反冲：整体平移加速度 + 绕质心的刚性旋转，让气球边泄压边乱飞。
    void ApplyAerodynamicThrust(const Eigen::Vector3d& direction,
        double acceleration, const Eigen::Vector3d& spinAxis,
        double angularSpeed, double dt);
    // 漏气破口：渲染时跳过命中点附近的三角面，形成一个可见小洞。
    // 只影响 BuildRenderMesh，不改变粒子/约束，因此物理壳仍保持闭合。
    void SetRenderHole(const Eigen::Vector3d& direction, double angularRadius);
    void ApplyRigidTransform(const Eigen::Matrix4f& transform);
    void PinNearest(const Eigen::Vector3d& point, int count);
    // 轻量地面约束：只做位置/速度钳制，供 Jolt 代理负责接触事件时保持视觉软片不穿地。
    void ApplyGroundPlane(float localGroundY, float restitution = 0.12f);
    SoftBodyRenderMesh BuildRenderMesh() const;

    const RubberShellSettings& Settings() const { return settings; }
    const std::vector<ShellParticle>& Particles() const { return particles; }
    const std::vector<ShellDistanceConstraint>& Constraints() const { return constraints; }
    const std::vector<ShellTriangle>& Triangles() const { return triangles; }

private:
    void RebuildConstraints(double restLengthScale);

    RubberShellSettings settings;
    std::vector<ShellParticle> particles;
    std::vector<ShellDistanceConstraint> constraints;
    std::vector<ShellTriangle> triangles;
    Eigen::Vector3d renderHoleDirection = Eigen::Vector3d::UnitZ();
    double renderHoleAngle = 0.0;
};

} // namespace VL::Physics::Softbody
