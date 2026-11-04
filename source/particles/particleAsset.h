#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "particles/particleLimits.h"

namespace VL
{

struct ParticleSystemAsset;
struct ParticleSdfAsset;
// 运行时参数重解析只替换数值，不重新读盘/烘网格；按 collision type + path
// 复用基础资产中已加载的 SDF。
using ParticleCollisionResourceCache = std::unordered_map<
    std::string,
    std::shared_ptr<const ParticleSdfAsset>>;

inline std::string ParticleCollisionResourceCacheKey(
    std::string_view type,
    std::string_view path)
{
    std::string key(type);
    key += '|';
    key += path;
    return key;
}

// billboardParams.x 的编码必须与 particle.vert / particleGlobalGather.comp
// 保持一致：0 camera、1 velocity、2 fixed。
inline int ParticleBillboardAlignmentCode(std::string_view alignment)
{
    if (alignment == "velocity")
    {
        return 1;
    }
    if (alignment == "fixed")
    {
        return 2;
    }
    return 0;
}

class ParticleAssetLoader
{
public:
    // 读取并完整校验；失败时抛出带路径的异常，调用发生在 World 候选提交之前。
    static ParticleSystemAsset Prepare(const std::string& path);
    // 解析已经读取的 JSON；编辑器字段写入后复用同一校验入口。
    // resourceRoot 为空时用 CommonFunction::Path 解析碰撞/纹理资源；
    // 测试或编辑器可传入显式资源根，仍坚持资源根相对路径契约。
    static ParticleSystemAsset ParseJson(
        const nlohmann::json& json,
        std::string_view sourcePath,
        const std::filesystem::path& resourceRoot = {},
        const ParticleCollisionResourceCache* collisionCache = nullptr);
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
// beam 渲染模式的端点与噪声参数；端点在发射器局部空间作者化，
// 渲染前由 emitter.model 变换到世界空间。
struct ParticleBeamAsset
{
    Eigen::Vector3f start = Eigen::Vector3f(-200.0f, 0.0f, 0.0f);
    Eigen::Vector3f end = Eigen::Vector3f(200.0f, 0.0f, 0.0f);
    uint32_t segments = 16;
    // 半宽（cm）；最终四边形横向半宽。
    float width = 3.0f;
    // 并行弧数量；渲染器按实例数绘制，不依赖存活粒子。
    uint32_t count = 4;
    float noiseAmplitude = 18.0f;
    float noiseFrequency = 3.0f;
    float noiseSpeed = 5.0f;
    float endpointJitter = 0.0f;
    // 分支：每条并行弧从主弧的 branchStart 附近长出 branchCount 条分叉。
    // branchLength 是主线长度的比例，branchSpread 是末端横向偏移（cm），
    // 四个字段都只改变渲染形状，不引入额外粒子实例。
    uint32_t branchCount = 0;
    float branchStart = 0.5f;
    float branchLength = 0.3f;
    float branchSpread = 20.0f;
};

// renderMode=="light" 的 CPU 灯光参数。发射器本身不写 sceneColor，也不分配
// GPU 粒子池；运行时按粒子年龄生成瞬时 PointLight。intensity 单位是 cd，
// radius 单位是 cm，颜色来自 colorStart/colorEnd 的 RGB 与 alpha。
struct ParticleLightAsset
{
    float intensity = 300.0f;
    float radius = 700.0f;
};

struct ParticleEmitterAsset
{
    std::string name;
    Eigen::Vector3f localPosition = Eigen::Vector3f::Zero();
    // true 时模拟状态（位置/速度/重力/drag）保存在发射器局部空间，
    // 只有渲染时乘 localToWorld。v1 不允许与世界空间碰撞组合。
    bool localSpace = false;

    // billboard 是默认路径；ribbon 用当前点与最近 3 个历史点组成相机朝向
    // 条带；mesh 用静态网格按粒子实例化；beam 在作者化起止点之间绘制
    // 多条带噪声的并行电弧，不消费粒子池；distortion 仍消费粒子池，
    // 但不写 sceneColor，而是在独立 pass 里把屏幕空间 UV 偏移写入
    // RGBA16F distortion 缓冲，由 toneMapping 合成。ribbonWidth 是世界空间
    // 半宽（cm），ribbonSegments 是段数 1..3，ribbonFade 是尾端 alpha 衰减强度。
    std::string renderMode = "billboard"; // billboard|ribbon|mesh|beam|distortion|light
    // 只有 renderMode=="light" 需要作者提供 light 对象；hasLight 区分
    // “省略对象”和“对象存在但数值等于默认值”。
    ParticleLightAsset light;
    bool hasLight = false;
    ParticleBeamAsset beam;
    // 仅 billboard 使用：camera 跟随相机平面，velocity 让 quad 的法线对齐
    // 粒子速度，fixed 用作者指定的发射器局部法线/上方向。ribbon/mesh/beam
    // 各自使用自己的朝向逻辑。
    std::string billboardAlignment = "camera";
    // fixed alignment 使用的发射器局部空间法线（quad 朝向）与上方向提示。
    // 上传前乘 emitter.model 线性部分，因此会跟随粒子系统/武器的旋转与缩放；
    // 运行时只做归一化与正交化。
    Eigen::Vector3f billboardNormal = Eigen::Vector3f(0.0f, 0.0f, 1.0f);
    Eigen::Vector3f billboardUp = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    // billboard/ribbon 默认 unlit；开启后按相机朝向法线接收 CSM 阴影
    // 调暗，属于显式的简化光照开关，不启用完整 PBR。
    bool billboardShadows = false;
    // billboard 专用运行时照明：以相机朝向法线作为表面法线，消费场景
    // 第一个方向光的线色；这是 sprite 的着色近似，不引入编辑器材质状态。
    bool billboardLighting = false;
    float billboardAmbient = 0.2f;
    float ribbonWidth = 5.0f;
    uint32_t ribbonSegments = 3;
    float ribbonFade = 1.0f;
    // mesh 渲染：路径相对资源根，scale 是网格局部缩放，alignment 是
    // world|camera|velocity。v1 不使用 size 曲线，也不支持 localSpace。
    std::string meshPath;
    Eigen::Vector3f meshScale = Eigen::Vector3f::Ones();
    std::string meshAlignment = "world";
    // 可选材质实例（MI_*.json）。只消费数值子集 baseColor/emissive/
    // roughness/metallic；空字符串表示使用内置 unlit/默认 PBR 回退。
    std::string materialInstancePath;
    // mesh 专用单方向光 GGX/Smith/Fresnel，并采样 CSM 阴影；光源只用
    // 场景第一个方向光的颜色与方向，不读 lux 强度。billboard/ribbon 不受影响。
    bool meshLighting = true;
    float meshAmbient = 0.2f;

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
    // capsuleRadius 的胶囊；sdf 是有限体素距离场；mesh 是三角网格，
    // 加载期烘成同一份 SDF。localSpace=true 时碰撞体按发射器局部空间
    // 解释（与局部粒子坐标同一空间）。
    // 碰撞字段对 type=none 不产生运行时开销，但仍参与有限性与范围校验。
    std::string collisionType = "none"; // none|plane|box|capsule|sdf|mesh
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
    // SDF 源路径相对资源根；解析成功后 collisionSdf 持有冻结数据。
    std::string collisionSdfPath;
    // 三角网格碰撞源路径相对资源根；加载期烘成 collisionSdf，
    // 运行时仍走 SDF 的 GPU/CPU 路径。路径变化必须触发发射器重建。
    std::string collisionMeshPath;
    std::shared_ptr<const ParticleSdfAsset> collisionSdf;

    // 事件 v1：只控制是否写 GPU 事件记录，不改变碰撞/死亡语义。
    bool emitSpawnEvent = false;
    bool emitDeathEvent = false;
    bool emitCollisionEvent = false;

    // 确定性随机：seed 参与每步随机状态派生；fixedTimeStep>0 时模拟按
    // 固定步长推进（渲染帧累积），不依赖 wall clock。
    uint32_t randomSeed = 0;
    bool deterministicRandom = false;
    float fixedTimeStep = 0.0f;

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
    // 仅 renderMode=="distortion" 使用：UV 偏移幅度（屏幕空间比例）。
    // 最终偏移在 shader 里再乘纹理/程序化梯度与覆盖率；0 表示不扰动。
    float distortionStrength = 0.02f;
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
    // v2 系统级用户参数与模块中实际出现的 `$Name` 依赖集。
    std::unordered_map<std::string, float> userParameters;
    std::unordered_set<std::string> userParameterReferences;
    // v2 且存在 userParameters 时冻结一份作者 JSON：运行时参数覆盖需要
    // 在原始模块结构上重新替换 $Name 并重新走 ParseJson。没有用户参数的
    // 资产不保留副本，避免无意义的内存占用。
    std::shared_ptr<const nlohmann::json> authoringJson;
    // 相对资源根的逻辑路径，用作运行时发射器身份的一部分。
    std::string sourcePath;
    std::vector<ParticleEmitterAsset> emitters;
};

} // namespace VL
