#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

#include "particles/particleAsset.h"
#include "particles/particleEvents.h"
#include "particles/particleFlipbook.h"
#include "particles/particleFrameStats.h"
#include "particles/particleGlobalSort.h"
#include "particles/particleIndirect.h"
#include "particles/particleMaterial.h"
#include "particles/particleRenderIdentity.h"
#include "particles/particleSdf.h"
#include "render/particles/particleGpuTimer.h"

class PipelineBase;
class PipelineFactory;
class ComputePipeline;
class Texture;
class RenderGraph;
struct Renderpass;

namespace VL
{
class RendererBackendVulkan;
struct RenderScene;
struct ParticleSystemSnapshot;

// renderSystem 每帧把已构建的 CSM 数据推给粒子渲染器。粒子管线有独立的
// set 0 UBO，不能直接读取 UBOGlobal，因此这里显式拷贝所需子集。
struct ParticleShadowCascadeData
{
    std::array<Eigen::Matrix4f, 4> lightViewProj{};
    std::array<Eigen::Vector4f, 4> bias{};
    Eigen::Vector4f cascadeSplits = Eigen::Vector4f::Zero();
    // z = active cascade count, w = shadow distance
    Eigen::Vector4f csmParameters = Eigen::Vector4f::Zero();
    bool valid = false;
};

// 单发射器事件读取结果；counters 是 GPU 原子计数（可能大于 records 数量，
// 表示有界缓冲发生过截断）。
struct ParticleEventBatch
{
    std::string emitterKey;
    ParticleEventCounters counters;
    std::vector<ParticleEventRecord> records;
};

// Niagara 式最小运行时：每个场景发射器拥有一段 GPU 粒子池，compute 负责
// 生成/积分，billboard pass 直接消费同一段 storage buffer。
class ParticleRenderer
{
public:
    ~ParticleRenderer();

    // 图或 pass 句柄变化时重建管线；发射器资源由 Synchronize 独立管理。
    void EnsureInitialized(
        PipelineFactory& factory,
        RendererBackendVulkan& backend,
        RenderGraph& graph);
    void Shutdown(RendererBackendVulkan& backend);

    // shader hot reload 提交后调用：等待设备空闲，丢弃旧 pipeline、发射器池
    // 与默认纹理并回到未初始化状态；下一帧 EnsureInitialized/Synchronize
    // 会用新 shader artifact 重建。粒子年龄、排序结果与纹理缓存会一并重置。
    void InvalidatePipelines(RendererBackendVulkan& backend);

    // 冻结 RenderScene 中的发射器集合：新增/删除/参数变化只影响自己的 GPU 包。
    void Synchronize(const RenderScene& renderScene);

    // 在图形 pass 之前录制 compute；结束时把 SSBO 可见性移交给顶点着色器。
    void RecordSimulation(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const RenderScene& renderScene,
        vk::Extent2D renderExtent);

    // 由 particle pass 在已开始的 sceneColor 窗口内调用；sceneDepth 走 set 3。
    void RecordGraphics(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass);

    // 一次性 GPU 回读：调用前必须保证渲染线程与设备空闲。输出是调试快照，
    // 不参与正常帧路径；失败时返回 false 并保留一条可直接展示的原因。
    // frameStats 只用于写 dump 根部的 GPU pass 计时字段。
    bool DumpParticles(
        RendererBackendVulkan& backend,
        const std::filesystem::path& outputPath,
        uint32_t maxPerEmitter,
        const ParticleFrameStats& frameStats,
        std::string& outError);

    // 运行时 restart：按场景对象名匹配 emitter key，把这些池的模拟时间归零，
    // 下一帧重新 prewarm 并清空粒子池；其它场景对象与 PS 资产不受影响。
    // 调用方必须保证渲染线程处于安全点。
    void RestartParticleSystems(const std::string& sceneObjectName);

    // renderSystem 在 RecordSimulation 之前调用；shadowMap 本体仍通过
    // particle pass 的 set 3 输入绑定，这里只提供级联矩阵与 bias。
    void SetShadowCascadeData(const ParticleShadowCascadeData& data);

    // 一次性 GPU 回读（调用方需保证设备空闲）。供脚本/调试层消费事件。
    bool DrainParticleEvents(
        RendererBackendVulkan& readbackBackend,
        uint32_t maxPerEmitter,
        std::vector<ParticleEventBatch>& outBatches,
        std::string& outError);

    bool IsInitialized() const noexcept { return initialized; }
    bool HasEmitters() const noexcept { return !emitters.empty(); }
    const ParticleFrameStats& GetFrameStats() const noexcept
    {
        return frameStats;
    }

private:
    struct GpuBuffer
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped = nullptr;
    };

    struct GpuParticle
    {
        Eigen::Vector4f positionAge;
        Eigen::Vector4f velocityLife;
        // ribbon 历史快照：xyz = 记录时的模拟空间位置，w = 记录时的 age。
        Eigen::Vector4f history0;
        Eigen::Vector4f history1;
        Eigen::Vector4f history2;
    };

    // std140 布局必须与 shader/particle/particleCommon.glsl 保持一致。
    struct FrameUniforms
    {
        Eigen::Matrix4f viewProjection;
        Eigen::Matrix4f view;
        Eigen::Matrix4f invProjection;
        Eigen::Vector4f cameraPosition;
        Eigen::Vector4f cameraRight;
        Eigen::Vector4f cameraUp;
        Eigen::Matrix4f localToWorld;
        Eigen::Vector4f gravityDrag;
        Eigen::Vector4f velocityMinLifetimeMin;
        Eigen::Vector4f velocityMaxLifetimeMax;
        Eigen::Vector4f simulation;
        Eigen::Vector4f render;
        Eigen::Vector4f shape;
        // x = softParticleDistance, y = softParticles enabled
        Eigen::Vector4f softParams;
        Eigen::Vector4f colorStart;
        Eigen::Vector4f colorEnd;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys> colorKeys;
        std::array<Eigen::Vector4f, 2> colorKeyTimes;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys> sizeKeys;
        Eigen::Vector4f curveParams;
        // x = fps, y = hasTexture, z = startFrame, w = endFrame
        Eigen::Vector4f flipbookParams;
        // x = loop, y = randomStartFrame, z/w reserved
        Eigen::Vector4f flipbookParams2;
        // x = sort enabled, y = particle pool count, z = sorted capacity, w = reserved
        Eigen::Vector4f sortParams;
        // x = bitonic k, y = bitonic j, z = sorted capacity, w = reserved
        Eigen::Vector4f sortPass;
        // x = viewport height (px), y = max billboard half-size (px),
        // z = author spawnRate for prewarm, w reserved
        Eigen::Vector4f screenParams;
        std::array<Eigen::Vector4f, kParticleFlipbookDurationVectors>
            flipbookFrameDurations;
        std::array<Eigen::Vector4f, kParticleFlipbookPivotVectors>
            flipbookFramePivots;
        // x = hasDurations, y = hasPivots, z = frameCount, w = hasUVs
        Eigen::Vector4f flipbookParams3;
        std::array<Eigen::Vector4f, kParticleFlipbookUvVectors>
            flipbookFrameUVs;
        // x = type(0 none, 1 plane, 2 box), y = bounce, z = friction, w = radius
        Eigen::Vector4f collisionParams;
        // normal.xyz, offset
        Eigen::Vector4f collisionPlane;
        // min.xyz, killOnCollision
        Eigen::Vector4f collisionBoxMin;
        // max.xyz, reserved
        Eigen::Vector4f collisionBoxMax;
        // x = burstInterval, y/z/w reserved
        Eigen::Vector4f burstParams;
        // x = base rotation degrees, y = rotation speed degrees/sec,
        // z = random initial angle enabled, w = reserved
        Eigen::Vector4f rotationParams;
        // 每 vec4 打包 4 帧的逐帧旋转角（度）；仅 hasRotations 时读取。
        std::array<Eigen::Vector4f, kParticleFlipbookRotationVectors>
            flipbookFrameRotations;
        // x = hasRotations, y/z/w reserved
        Eigen::Vector4f flipbookParams4;
        // x = ccd enabled, y = capsule radius, z/w reserved
        Eigen::Vector4f collisionParams2;
        // 胶囊线段端点，世界空间；仅 type=capsule 时读取。
        Eigen::Vector4f collisionCapsuleA;
        Eigen::Vector4f collisionCapsuleB;
        // x = ribbon segments, y = tail fade, z = half width (cm), w reserved
        Eigen::Vector4f ribbonParams;
        // x = mesh alignment (0 world, 1 camera, 2 velocity), y/z/w reserved
        Eigen::Vector4f meshParams;
        // mesh 局部缩放；mesh particle v1 不消费 size 曲线。
        Eigen::Vector4f meshScale;
        // 场景实例级颜色 tint；rgb 与 alpha 都乘到最终片元输出。
        Eigen::Vector4f colorTint;
        // x = billboard alignment (0 camera, 1 velocity), y/z/w reserved.
        Eigen::Vector4f billboardParams;
        // x = shape code (0 sphere, 1 box, 2 cone, 3 cylinder, 4 disc,
        // 5 ring), y = height (cm), z = innerRadius (cm), w reserved.
        Eigen::Vector4f shapeParams;
        // xyz = box half extents (cm), w reserved.
        Eigen::Vector4f shapeExtent;
        // x = curl noise strength, y = frequency, z = time speed, w reserved.
        Eigen::Vector4f curlParams;
        // x = vortex strength, y = radius, z/w reserved.
        Eigen::Vector4f vortexParams;
        // xyz = normalized vortex axis (computed in shader), w = 0.
        Eigen::Vector4f vortexAxis;
        // x = attractor strength, y = radius, z/w reserved.
        Eigen::Vector4f attractorParams;
        // xyz = attractor position in simulation space, w = 0.
        Eigen::Vector4f attractorPosition;
        // xyz = constant wind acceleration in simulation space, w = 0.
        Eigen::Vector4f wind;
        // x = lifeT, y = time scale, z/w reserved. 空表由 count=0 表示，
        // 运行时倍率恒为 1；prewarm 不消费这张表。
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            lifetimeScaleKeys;
        // x = key count, y/z/w reserved.
        Eigen::Vector4f lifetimeScaleParams;
        // x = mesh lighting enabled, y = ambient, z/w reserved.
        Eigen::Vector4f meshLightParams;
        // xyz = surface-to-light direction（已取反方向光方向）.
        Eigen::Vector4f meshLightDirection;
        // xyz = directional light linear color（不乘 lux 强度）.
        Eigen::Vector4f meshLightColor;
        // mesh 材质 v1 子集。
        Eigen::Vector4f materialBaseColor;
        Eigen::Vector4f materialEmissiveStrength;
        // x = roughness, y = metallic, z = hasMaterial, w reserved.
        Eigen::Vector4f materialPbr;
        // CSM：4 级 lightViewProj、逐级 bias、split 与开关。
        std::array<Eigen::Matrix4f, 4> shadowLightViewProj{};
        std::array<Eigen::Vector4f, 4> shadowCascadeBias{};
        Eigen::Vector4f shadowCascadeSplits;
        Eigen::Vector4f shadowParams;
        // x = seed, y = fixed step seconds, z = step index, w = deterministic.
        Eigen::Vector4f determinismParams;
        // x = spawn events, y = death events, z = collision events.
        Eigen::Vector4f eventParams;
        // SDF bounds/resolution；无 SDF 时分辨率保持 0。
        Eigen::Vector4f collisionSdfBoundsMin;
        Eigen::Vector4f collisionSdfBoundsMax;
        Eigen::Vector4f collisionSdfResolution;
        // x = billboard lighting enabled, y = ambient, z/w reserved.
        // 与 mesh 共用方向光的线性色/方向，但开关和环境项独立。
        Eigen::Vector4f billboardLightParams;
        // beam 渲染：xyz 世界端点，w 分别为段数与半宽。
        Eigen::Vector4f beamStart;
        Eigen::Vector4f beamEnd;
        // x = 噪声幅度, y = 频率, z = 速度, w = 端点抖动（cm）。
        Eigen::Vector4f beamNoise;
        // x = 并行弧数量, y/z/w reserved。
        Eigen::Vector4f beamParams;
    };
    static_assert(
        sizeof(FrameUniforms) == 6320,
        "ParticleFrame std140 size must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, meshScale) == 5392,
        "ParticleFrame meshScale offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, colorTint) == 5408,
        "ParticleFrame colorTint offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, billboardParams) == 5424,
        "ParticleFrame billboardParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shapeParams) == 5440,
        "ParticleFrame shapeParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shapeExtent) == 5456,
        "ParticleFrame shapeExtent offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, curlParams) == 5472,
        "ParticleFrame curlParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, vortexParams) == 5488,
        "ParticleFrame vortexParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, vortexAxis) == 5504,
        "ParticleFrame vortexAxis offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, attractorParams) == 5520,
        "ParticleFrame attractorParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, attractorPosition) == 5536,
        "ParticleFrame attractorPosition offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, wind) == 5552,
        "ParticleFrame wind offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, lifetimeScaleKeys) == 5568,
        "ParticleFrame lifetimeScaleKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, lifetimeScaleParams) == 5696,
        "ParticleFrame lifetimeScaleParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, meshLightParams) == 5712,
        "ParticleFrame meshLightParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, meshLightDirection) == 5728,
        "ParticleFrame meshLightDirection offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, meshLightColor) == 5744,
        "ParticleFrame meshLightColor offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, materialBaseColor) == 5760,
        "ParticleFrame materialBaseColor offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, materialEmissiveStrength) == 5776,
        "ParticleFrame materialEmissiveStrength offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, materialPbr) == 5792,
        "ParticleFrame materialPbr offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shadowLightViewProj) == 5808,
        "ParticleFrame shadowLightViewProj offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shadowCascadeBias) == 6064,
        "ParticleFrame shadowCascadeBias offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shadowCascadeSplits) == 6128,
        "ParticleFrame shadowCascadeSplits offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shadowParams) == 6144,
        "ParticleFrame shadowParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, determinismParams) == 6160,
        "ParticleFrame determinismParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, eventParams) == 6176,
        "ParticleFrame eventParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, collisionSdfBoundsMin) == 6192,
        "ParticleFrame collisionSdfBoundsMin offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, collisionSdfBoundsMax) == 6208,
        "ParticleFrame collisionSdfBoundsMax offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, collisionSdfResolution) == 6224,
        "ParticleFrame collisionSdfResolution offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, billboardLightParams) == 6240,
        "ParticleFrame billboardLightParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamStart) == 6256,
        "ParticleFrame beamStart offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamEnd) == 6272,
        "ParticleFrame beamEnd offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamNoise) == 6288,
        "ParticleFrame beamNoise offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamParams) == 6304,
        "ParticleFrame beamParams offset must match particleCommon.glsl");

    struct EmitterResources
    {
        std::string key;
        ParticleEmitterAsset asset;
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        Eigen::Vector3f worldCenter = Eigen::Vector3f::Zero();
        float boundsRadius = 0.0f;
        ParticleFlipbookGpuTable flipbookTable;
        GpuBuffer particles;
        // GPU 计数/紧凑存活索引/事件缓冲：每帧由 fillBuffer 清零计数，
        // simulate 原子追加，finalize compute 把 alive 写回 indirectArgs。
        GpuBuffer counters;
        GpuBuffer aliveIndices;
        GpuBuffer events;
        /* 图形片元 overdraw 计数；每帧清零，particledump 回读。 */
        GpuBuffer debugCounters;
        // 可选 SDF 体素；无 SDF 时绑定 1 个 float 的占位 buffer。
        GpuBuffer sdfData;
        uint32_t sdfVoxelCount = 0;
        // bitonic sort 的双缓冲 pair 表；pair.xy = (viewDepth, particleIndex)。
        GpuBuffer sortPairsA;
        GpuBuffer sortPairsB;
        // draw/drawIndexed 的间接参数；当前由 CPU 在创建时写入固定计数，
        // 后续 GPU culling 可直接改写同一缓冲而不改录制路径。
        GpuBuffer indirectArgs;
        bool indirectIndexed = false;
        // renderMode/ribbonSegments/mesh fallback 任一变化都会改变命令形状；
        // 把它保留在资源包里，Synchronize 才能在池未扩容时也触发重建。
        ParticleIndirectDrawShape indirectShape;
        std::vector<GpuBuffer> frameUniforms;
        vk::DescriptorPool descriptorPool;
        std::vector<vk::DescriptorSet> computeSets;
        std::vector<vk::DescriptorSet> graphicsSets;
        std::vector<vk::DescriptorSet> sortInitSets;
        std::vector<vk::DescriptorSet> sortAtoBSets;
        std::vector<vk::DescriptorSet> sortBtoASets;
        std::vector<vk::DescriptorSet> finalizeSets;
        std::shared_ptr<Texture> texture;
        // 路径 + 文件大小 + 修改时间 + mipmap 配置；变化时重建整个发射器。
        std::string textureSourceKey;
        // mesh 路径 + 文件大小 + 修改时间；变化时重建网格缓冲。
        std::string meshSourceKey;
        GpuBuffer meshVertices;
        GpuBuffer meshIndices;
        uint32_t meshIndexCount = 0;
        Eigen::Vector3f meshScale = Eigen::Vector3f::Ones();
        std::string meshAlignment = "world";
        // mesh 顶点在局部缩放后的最大半径，用于自动包围球。
        float meshLocalBoundsRadius = 0.0f;
        // 网格导入失败时本发射器仍保持 mesh 资产，但绘制回退 billboard。
        bool meshFallbackBillboard = false;
        uint32_t sortCapacity = 0;
        // 排序结束时 pair 表的奇偶性决定 graphics 绑定 A 还是 B。
        bool sortFinalUsesA = true;
        float elapsedSeconds = 0.0f;
        // fixedTimeStep>0 时按固定步长推进；accumulator 吸收帧间余量，
        // fixedStepIndex 作为确定性随机的离散时间。
        float fixedStepAccumulator = 0.0f;
        uint32_t fixedStepIndex = 0;
        bool needsClear = true;
        bool prewarm = true;
        bool culled = false;
        // 作者可见性不是剔除结果：隐藏时同样冻结模拟，但重新显示后
        // 不能重建池，因此与 culled 分开保存。
        bool visible = true;
        // World stable slot 的 restart epoch；变化时只重置本发射器状态，
        // 不重建 GPU 资源。
        uint32_t restartEpoch = 0;
        // 发射器运行时开关与实例级覆盖；禁用沿冻结路径，重新启用不重建池。
        bool enabled = true;
        float spawnRateScale = 1.0f;
        Eigen::Vector4f colorTint = Eigen::Vector4f::Ones();
        // mesh 专用方向光缓存；每帧在 RecordSimulation 里按场景方向光重算，
        // 不参与发射器资源重建。未启用时保持旧 unlit 输出。
        bool lightingEnabled = false;
        Eigen::Vector3f lightDirection =
            Eigen::Vector3f(0.0f, -1.0f, 0.0f);
        Eigen::Vector3f lightColor = Eigen::Vector3f::Ones();
        float ambient = 0.2f;
        // 材质实例 v1 子集；加载失败保持 valid=false 的明确回退。
        ParticleMaterialParams material;
        // 单个 emitter 的 GPU 图形区间计时；模拟/排序仍由全局 pass timer
        // 与 CPU 预算统计覆盖。
        ParticleGpuTimer gpuTimer;
        // SDF 碰撞体；与资产共享只读数据。
        std::shared_ptr<const ParticleSdfAsset> sdf;
    };

    // 全局透明排序池：所有参与 billboard alpha 的发射器把自己的粒子
    // gather 到这个共享记录数组，再做一次全局 bitonic 排序与单次 draw。
    // 每个发射器仍保留自己的模拟池、UBO 与材质曲线，只是渲染阶段合并。
    struct GlobalSortPool
    {
        bool active = false;
        std::string signature;
        uint32_t capacity = 0;
        uint32_t emitterCount = 0;
        bool finalUsesA = true;
        std::vector<std::string> emitterKeys;
        std::unordered_map<std::string, uint32_t> textureSlotByKey;
        std::vector<std::shared_ptr<Texture>> textures;
        // 所有全局池 buffer 按 swapchain image 分槽，避免两个在飞帧共享
        // 同一 gather/sort/count/indirect 目标。
        std::vector<GpuBuffer> records;
        std::vector<GpuBuffer> pairsA;
        std::vector<GpuBuffer> pairsB;
        std::vector<GpuBuffer> counters;
        std::vector<GpuBuffer> indirectArgs;
        std::vector<GpuBuffer> debugCounters;
        uint32_t lastImageIndex = 0;
        uint32_t gatheredEmitters = 0;
        std::vector<GpuBuffer> uniforms;
        std::vector<vk::DescriptorSet> gatherSets;
        std::vector<vk::DescriptorSet> sortInitSets;
        std::vector<vk::DescriptorSet> sortAtoBSets;
        std::vector<vk::DescriptorSet> sortBtoASets;
        std::vector<vk::DescriptorSet> finalizeSets;
        std::vector<vk::DescriptorSet> drawSets;
        vk::DescriptorPool descriptorPool;
        ParticleGpuTimer gpuTimer;
    };

    void Initialize(
        PipelineFactory& factory,
        RendererBackendVulkan& backend,
        RenderGraph& graph);
    void CreateEmitter(const ParticleEmitterAsset& asset,
        const Eigen::Matrix4f& model, const std::string& key,
        bool visible, bool enabled, float spawnRateScale,
        const Eigen::Vector4f& colorTint);
    static void DestroyEmitterResources(
        RendererBackendVulkan& backend,
        EmitterResources& emitter);
    void CreateBuffers(EmitterResources& emitter);
    void LoadEmitterMesh(
        EmitterResources& emitter,
        const std::string& emitterKey);
    void CreateDescriptors(EmitterResources& emitter);
    static void DestroyBuffer(
        RendererBackendVulkan& backend,
        GpuBuffer& buffer);
    void RetireEmitterResources(
        std::shared_ptr<EmitterResources> emitter);
    void WarnTextureFallbackOnce(
        const std::string& key,
        const std::string& texturePath,
        const std::string& error);
    void WarnMeshFallbackOnce(
        const std::string& key,
        const std::string& meshPath,
        const std::string& error);
    void WarnMaterialFallbackOnce(
        const std::string& key,
        const std::string& materialPath,
        const std::string& error);
    const ParticleSystemAsset& ResolveParticleSystemAsset(
        const ParticleSystemSnapshot& snapshot,
        const std::unordered_map<std::string, float>& runtimeParameters);
    void WarnUserParameterFailureOnce(
        const std::string& key,
        uint64_t signature,
        const std::string& error);
    void SynchronizeGlobalSort();
    void DestroyGlobalSortPool();
    bool CreateGlobalSortPool(
        const std::vector<std::string>& emitterKeys,
        const std::vector<uint32_t>& textureSlots,
        const std::vector<std::shared_ptr<Texture>>& textures,
        uint32_t capacity);
    void RecordGlobalGather(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex);
    void RecordGlobalSort(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const RenderScene& renderScene);
    void RecordGlobalDraw(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass);
    bool IsGlobalSortEmitter(const std::string& key) const;
    void InsertBufferBarrier(
        vk::CommandBuffer commandBuffer,
        vk::PipelineStageFlags sourceStage,
        vk::AccessFlags sourceAccess,
        vk::PipelineStageFlags destinationStage,
        vk::AccessFlags destinationAccess);
    // 冻结/剔除路径不运行 compute，但仍要清掉上一帧的 alive/事件/overdraw
    // 计数，避免回读重放旧事件或发布陈旧诊断。
    static void ClearParticleRuntimeCounters(
        vk::CommandBuffer commandBuffer,
        EmitterResources& emitter);

    RendererBackendVulkan* backend = nullptr;
    ParticleRenderIdentity renderIdentity;
    bool initialized = false;

    std::shared_ptr<ComputePipeline> simulatePipeline;
    std::shared_ptr<ComputePipeline> sortPipeline;
    std::shared_ptr<ComputePipeline> sortInitPipeline;
    std::shared_ptr<ComputePipeline> finalizeCountPipeline;
    std::shared_ptr<ComputePipeline> globalGatherPipeline;
    std::shared_ptr<ComputePipeline> globalSortInitPipeline;
    std::shared_ptr<PipelineBase> additivePipeline;
    std::shared_ptr<PipelineBase> alphaPipeline;
    // ribbon 与 billboard 共用 descriptor set layout 与 particle SSBO，
    // 只替换 shader 与顶点数。
    std::shared_ptr<PipelineBase> ribbonAdditivePipeline;
    std::shared_ptr<PipelineBase> ribbonAlphaPipeline;
    // beam 与 ribbon 共用 particle pass 的 set 0/set 3；顶点数由段数决定，
    // 实例数由作者 beam.count 决定，不读取存活粒子索引。
    std::shared_ptr<PipelineBase> beamAdditivePipeline;
    std::shared_ptr<PipelineBase> beamAlphaPipeline;
    // mesh 使用静态 SM 的 Vertex 输入布局与 drawIndexed；排序仍共用
    // particle SSBO 与 sortedPairs，只替换顶点/片元 shader。
    std::shared_ptr<PipelineBase> meshAdditivePipeline;
    std::shared_ptr<PipelineBase> meshAlphaPipeline;
    std::shared_ptr<PipelineBase> globalDrawPipeline;
    std::shared_ptr<Texture> defaultTexture;
    // 发射器包整体由 shared_ptr 持有：活跃表与 retire queue 共享同一所有权，
    // 最后一个 epoch 完成后才析构 descriptor pool、buffer 与纹理。
    std::unordered_map<std::string, std::shared_ptr<EmitterResources>>
        emitters;
    // 仅缓存弱引用：仍被活跃/退休发射器持有的纹理由发射器生命周期管理，
    // Shutdown 清空索引不会提前销毁 GPU 资源。
    std::unordered_map<std::string, std::weak_ptr<Texture>>
        textureCache;
    std::unordered_set<std::string> warnedTextureFallbacks;
    std::unordered_set<std::string> warnedMeshFallbacks;
    std::unordered_set<std::string> warnedMaterialFallbacks;
    // 运行时 userParameters 重解析缓存：key = system 身份，value 保存
    // 最近一次成功解析的资产 + 失败标记，避免每帧重新 ParseJson。
    struct ParticleUserParameterCacheEntry
    {
        uint64_t signature = 0;
        const nlohmann::json* authoringJson = nullptr;
        bool attempted = false;
        bool parsed = false;
        ParticleSystemAsset asset;
    };
    std::unordered_map<std::string, ParticleUserParameterCacheEntry>
        userParameterCache;
    std::unordered_set<std::string> warnedUserParameterFailures;
    // 每帧由 RecordSimulation 更新；RecordGraphics 只消费这份远->近顺序。
    std::vector<std::string> drawOrder;
    ParticleFrameStats frameStats;
    ParticleShadowCascadeData shadowCascadeData;
    GlobalSortPool globalSortPool;
};

} // namespace VL
