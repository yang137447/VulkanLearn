#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "particles/particleLimits.h"

namespace VL
{

struct ParticleSystemAsset;

class ParticleAssetLoader
{
public:
    // 读取并完整校验；失败时抛出带路径的异常，调用发生在 World 候选提交之前。
    static ParticleSystemAsset Prepare(const std::string& path);
    // 解析已经读取的 JSON；编辑器字段写入后复用同一校验入口。
    static ParticleSystemAsset ParseJson(
        const nlohmann::json& json,
        std::string_view sourcePath);
    static void Validate(const ParticleSystemAsset& asset);
};

struct ParticleColorKey
{
    float time = 0.0f;
    Eigen::Vector4f color = Eigen::Vector4f::Ones();
};

struct ParticleSizeKey
{
    float time = 0.0f;
    float size = 1.0f;
};

// 粒子归一化年龄 lifeT 对应的时间倍率。scale=0 冻结本帧，1 正常，
// >1 加速衰老；time/scale 都参与作者校验并打包进 GPU 曲线表。
struct ParticleLifetimeScaleKey
{
    float time = 0.0f;
    float scale = 1.0f;
};

// 单级 LOD：相机距离越过 distance 后，连续发射预算乘 spawnRateScale。
struct ParticleLodTier
{
    float distance = 0.0f;
    float spawnRateScale = 1.0f;
};

// 单个发射器的作者数据。多发射器 PS 里每个 entries 共享同一套字段；
// localPosition 是相对 particleSystem 场景对象的局部偏移（cm）。
struct ParticleEmitterAsset
{
    std::string name;
    Eigen::Vector3f localPosition = Eigen::Vector3f::Zero();
    // true 时模拟状态（位置/速度/重力/drag）保存在发射器局部空间，
    // 只有渲染时乘 localToWorld。v1 不允许与世界空间碰撞组合。
    bool localSpace = false;

    // billboard 是默认路径；ribbon 用当前点与最近 3 个历史点组成相机朝向
    // 条带；mesh 用静态网格按粒子实例化。ribbonWidth 是世界空间半宽（cm），
    // ribbonSegments 是段数 1..3，ribbonFade 是尾端 alpha 衰减强度。
    std::string renderMode = "billboard"; // billboard|ribbon|mesh
    // 仅 billboard 使用：camera 跟随相机平面，velocity 让 quad 的法线对齐
    // 粒子速度。ribbon/mesh 各自使用自己的朝向逻辑。
    std::string billboardAlignment = "camera";
    float ribbonWidth = 5.0f;
    uint32_t ribbonSegments = 3;
    float ribbonFade = 1.0f;
    // mesh 渲染：路径相对资源根，scale 是网格局部缩放，alignment 是
    // world|camera|velocity。v1 不使用 size 曲线，也不支持 localSpace。
    std::string meshPath;
    Eigen::Vector3f meshScale = Eigen::Vector3f::Ones();
    std::string meshAlignment = "world";

    // 固定包围球使用发射器局部空间的中心与半尺寸（cm）。启用后替代
    // 自动半径估算，剔除、距离淡出、minScreenSize 与跨发射器排序共用它。
    bool useFixedBounds = false;
    Eigen::Vector3f fixedBoundsCenter = Eigen::Vector3f::Zero();
    Eigen::Vector3f fixedBoundsExtent =
        Eigen::Vector3f(100.0f, 100.0f, 100.0f);

    uint32_t maxParticles = 2048;
    // burst=true 时只在首帧把整池粒子一次性喷出，之后死亡粒子不再补发。
    // burstInterval > 0 时按该周期整池重触发；0 表示只触发一次。
    bool burst = false;
    float burstInterval = 0.0f;
    float spawnRate = 200.0f;
    float lifetimeMin = 1.0f;
    float lifetimeMax = 1.5f;
    // 按 lifeT = age / lifetime 采样时间倍率，乘到本帧模拟 delta 与年龄
    // 推进上。空表示恒为 1；prewarm 不应用该曲线。
    std::vector<ParticleLifetimeScaleKey> lifetimeScaleKeys;

    // 世界单位是 cm；速度与重力也按 cm/s、cm/s^2 作者化。
    Eigen::Vector3f velocityMin =
        Eigen::Vector3f(-30.0f, 50.0f, -30.0f);
    Eigen::Vector3f velocityMax =
        Eigen::Vector3f(30.0f, 150.0f, 30.0f);
    Eigen::Vector3f gravity =
        Eigen::Vector3f(0.0f, -100.0f, 0.0f);
    float drag = 0.0f;
    // 力模块统一在模拟空间解释：localSpace 发射器用局部坐标/局部力，
    // 世界空间发射器用世界坐标/世界力。strength 为加速度量纲（cm/s^2）。
    float curlNoiseStrength = 0.0f;
    float curlNoiseFrequency = 0.1f;
    float curlNoiseSpeed = 0.0f;
    float vortexStrength = 0.0f;
    float vortexRadius = 50.0f;
    Eigen::Vector3f vortexAxis = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    Eigen::Vector3f attractorPosition = Eigen::Vector3f::Zero();
    float attractorStrength = 0.0f;
    float attractorRadius = 100.0f;
    Eigen::Vector3f wind = Eigen::Vector3f::Zero();
    float shapeRadius = 10.0f;
    // spawn 形状：所有形状都在发射器局部空间生成初始位置，再走现有的
    // localSpace/localToWorld、prewarm 与碰撞流程。sphere 是 v1 默认值。
    // box 的 extent 是三轴半尺寸（cm）；cone/cylinder 沿 +Y 展开；
    // disc/ring 位于 XZ 平面 y=0；ring 的 innerRadius 必须小于 radius。
    std::string shapeType = "sphere";
    Eigen::Vector3f shapeExtent = Eigen::Vector3f(50.0f, 50.0f, 50.0f);
    float shapeHeight = 100.0f;
    float shapeInnerRadius = 50.0f;

    // 世界空间碰撞。plane 的法线在解析时归一化，offset 是沿法线的
    // 世界距离；box 是世界轴对齐 AABB（cm）；capsule 是线段 AB 外扩
    // capsuleRadius 的胶囊。碰撞字段对 type=none 不产生运行时开销，
    // 但仍参与有限性与范围校验。
    std::string collisionType = "none"; // none|plane|box|capsule
    Eigen::Vector3f collisionPlaneNormal = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    float collisionPlaneOffset = 0.0f;
    Eigen::Vector3f collisionBoxMin =
        Eigen::Vector3f(-100.0f, -100.0f, -100.0f);
    Eigen::Vector3f collisionBoxMax =
        Eigen::Vector3f(100.0f, 100.0f, 100.0f);
    // 胶囊线段端点在世界空间（cm），A/B 允许重合退化成一个球。
    Eigen::Vector3f collisionCapsuleA = Eigen::Vector3f(0.0f, -50.0f, 0.0f);
    Eigen::Vector3f collisionCapsuleB = Eigen::Vector3f(0.0f, 50.0f, 0.0f);
    float collisionCapsuleRadius = 20.0f;
    float collisionBounce = 0.3f;
    float collisionFriction = 0.2f;
    float collisionRadius = 0.0f;
    bool killOnCollision = false;
    // CCD 只作用于运行时帧积分：按速度与碰撞半径把一帧拆成最多 8 个子步。
    // prewarm 的解析积分仍只在末尾做一次碰撞。
    bool collisionCcd = false;

    // Billboard 半边长从 sizeStart 线性过渡到 sizeEnd。
    float sizeStart = 20.0f;
    float sizeEnd = 5.0f;
    // 每个 billboard 绕自身中心在相机平面内旋转。rotation 是初始角，
    // rotationSpeed 按粒子年龄线性累加；randomStart 用稳定粒子相位打散初始角。
    float rotation = 0.0f;
    float rotationSpeed = 0.0f;
    bool rotationRandom = false;
    float softness = 0.5f;
    // 软粒子：片元采样 sceneDepth，在几何交线附近淡出；距离单位为 cm。
    bool softParticles = true;
    float softParticleDistance = 25.0f;
    // alpha billboard 必须开启：按相机视图深度做 back-to-front GPU 排序。
    // additive 发射器应保持 false，排序不会改变加法混合结果，只会增加 dispatch。
    bool sortParticles = false;
    float opacity = 1.0f;
    // 0 = 关闭。maxDrawDistance 按包围球最近点做距离剔除，
    // fadeDistance 在该阈值之前形成线性 alpha 淡出。
    float maxDrawDistance = 0.0f;
    float fadeDistance = 0.0f;
    // 0 = 关闭。minScreenSize 剔除投影直径过小的发射器；
    // maxScreenSize 只封顶单个 billboard 的屏幕半边长，不裁剪粒子中心。
    float minScreenSize = 0.0f;
    float maxScreenSize = 0.0f;
    // LOD 只降低连续发射预算：发射器中心超过 lodDistance(cm) 后
    // spawnRate 乘 lodSpawnRateScale；burst、prewarm 与渲染不受影响。
    // lodTiers 非空时优先于这两个兼容字段，按距离选择已越过的最远档位。
    float lodDistance = 0.0f;
    float lodSpawnRateScale = 1.0f;
    std::vector<ParticleLodTier> lodTiers;
    Eigen::Vector4f colorStart =
        Eigen::Vector4f(1.0f, 0.6f, 0.15f, 1.0f);
    Eigen::Vector4f colorEnd =
        Eigen::Vector4f(0.4f, 0.05f, 0.0f, 0.0f);
    bool additive = true;

    // 非空时覆盖 colorStart/colorEnd 与 size start/end 的线性端点。
    std::vector<ParticleColorKey> colorKeys;
    std::vector<ParticleSizeKey> sizeKeys;

    // 可选 flipbook 图集：texture 相对资源根；columns/rows 组成 atlas。
    // start/end 是 atlas 内的闭区间帧号；end 在 JSON 省略时解析为最后一帧。
    // fps=0 时按粒子寿命播放一轮；fps>0 时按模拟时间推进，loop 决定是否回绕。
    // randomStartFrame 只影响 fps>0 的时间起步相位，不改变帧区间本身。
    std::string texturePath;
    uint32_t flipbookColumns = 1;
    uint32_t flipbookRows = 1;
    float flipbookFramesPerSecond = 0.0f;
    uint32_t flipbookStartFrame = 0;
    // 0 是合法帧号；ParseJson 在作者省略 endFrame 时把它解析为
    // columns*rows-1，Validate 只按已解析值检查区间。
    uint32_t flipbookEndFrame = 0;
    bool flipbookLoop = true;
    bool flipbookRandomStartFrame = true;
    // 非空时必须与帧区间长度 endFrame-startFrame+1 完全一致。
    // 每项是 >0 的相对时长权重：fps>0 时一轮固定为 frameCount/fps 秒，
    // 每帧实际秒数 = weight / totalWeight * frameCount / fps；
    // fps=0 时整轮按粒子寿命播放，权重只决定各帧占比。
    std::vector<float> flipbookFrameDurations;
    // 非空时同样要求长度匹配。每项 [u,v] 是精灵图上的锚点，
    // 渲染时移动 billboard quad 中心，使该锚点落在粒子中心。
    std::vector<Eigen::Vector2f> flipbookFramePivots;
    // 非空时同样要求长度匹配。每项 (u0,v0,u1,v1) 是 sprite 空间的
    // UV 子矩形，用于逐帧不同尺寸的精灵；分量都在 [0,1] 且 u0<u1、v0<v1。
    // GPU 表每帧一个 vec4，因此只允许帧区间不超过 64。
    std::vector<Eigen::Vector4f> flipbookFrameUVs;
    // 非空时同样要求长度匹配。每项是叠加到逐粒子旋转上的角度（度），
    // 按当前整数局部帧号取值；GPU 表每 vec4 打包 4 帧，最多 64 帧。
    std::vector<float> flipbookFrameRotations;
};

// PS_*.json 的 CPU 侧冻结数据。粒子模拟与渲染只消费这份值，
// 不在每帧重新解析作者 JSON。没有 emitters 数组的旧资产会被解析成
// 单个 implicit emitter，因此所有消费方只需要遍历 emitters。
struct ParticleSystemAsset
{
    std::string name;
    // v1 = 扁平 emitter 字段；v2 = emitters[].modules 模块栈。
    // 运行时消费的仍是展平后的 ParticleEmitterAsset，模块顺序只属于作者层。
    uint32_t version = 1;
    // v2 系统级用户参数；当前存储并校验，供后续模块引用。
    std::unordered_map<std::string, float> userParameters;
    // 相对资源根的逻辑路径，用作运行时发射器身份的一部分。
    std::string sourcePath;
    std::vector<ParticleEmitterAsset> emitters;
};

} // namespace VL
