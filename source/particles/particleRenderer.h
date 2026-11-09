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
#include "particles/particleBeam.h"
#include "particles/particleDataChannel.h"
#include "particles/particleDecalGpu.h"
#include "particles/particleEvents.h"
#include "particles/particleFlipbook.h"
#include "particles/particleFrameStats.h"
#include "particles/particleGlobalSort.h"
#include "particles/particleIndirect.h"
#include "particles/particleLightGpu.h"
#include "particles/particleMaterial.h"
#include "particles/particleRenderIdentity.h"
#include "particles/particleSdf.h"
#include "particles/particleShadowCaster.h"
#include "particles/particleSkeletalPose.h"
#include "particles/particleSpawnSource.h"
#include "VertexDataStruct.h"
#include "render/particles/particleGpuTimer.h"
#include "resource/image/hostImage.h"

class PipelineBase;
class PipelineFactory;
class ComputePipeline;
class Texture;
class RenderGraph;
struct Renderpass;
struct MeshAssetImportResult;

namespace VL
{
class RendererBackendVulkan;
namespace Animation { struct SkeletalAsset; }
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

struct ParticleLightShadowCandidate
{
    std::string emitterKey;
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    float radius = 0.0f;
    bool castShadows = false;
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

    // 由 RenderSystem 在 Synchronize 之前提供本 swapchain 的 GPU light 输出
    // buffer。buffer 容量固定为 kMaxParticleLights，不做逐帧扩容。
    void SetParticleLightBuffers(std::vector<vk::Buffer> buffers);

    // 粒子图形 pass 的场景光照输入：per-image 场景 Light SSBO、per-image
    // GPU 粒子灯 SSBO，以及全局 IBL 采样器。descriptor 在发射器创建时写入，
    // 资源换代（World 重载、环境重建）会改变 generation 并触发已有发射器
    // 重建 descriptor 包；IBL 缺失时使用内部黑色 cubemap / 黑图兜底。
    void SetSceneLightingResources(
        std::vector<vk::DescriptorBufferInfo> lightInfos,
        std::vector<vk::DescriptorBufferInfo> particleLightInfos,
        vk::DescriptorImageInfo environmentCube,
        vk::DescriptorImageInfo brdfLut);

    // 屏幕空间 tile 光照列表（set 0 binding 10/11/12）。资源换代同样
    // 改变 generation，触发发射器 descriptor 包重建；缺失时使用内部
    // “单 tile + enabled=0”兜底，shader 自动回退到连续灯循环。
    void SetLightCullingResources(
        std::vector<vk::DescriptorBufferInfo> gridInfos,
        std::vector<vk::DescriptorBufferInfo> indexInfos,
        std::vector<vk::DescriptorBufferInfo> paramInfos);

    // 在图形 pass 之前录制 compute；结束时把 SSBO 可见性移交给顶点着色器。
    void RecordSimulation(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const RenderScene& renderScene,
        vk::Extent2D renderExtent);

    // 半分辨率软深度过滤 compute：读 sceneDepth，写 linear view depth。
    void RecordSoftDepth(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass,
        const RenderGraph& graph,
        const RenderScene& renderScene);

    // 由 particle pass 在已开始的 sceneColor 窗口内调用；sceneDepth 走 set 3。
    void RecordGraphics(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass);

    // 屏幕空间扰动：只绘制 renderMode=="distortion" 的 emitter，把带符号
    // UV 偏移累加进独立 distortion 附件；sceneDepth 走 set 3。
    void RecordDistortion(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass);

    // mesh 粒子作为阴影 caster 的两个阶段：
    // 1) PrepareShadow 必须在 render pass 外调用。它用有序的
    //    vkCmdUpdateBuffer 覆盖每个 caster 的 shadowViewProj / 参数，并插入
    //    uniform-read -> transfer -> uniform-read 依赖；同一 per-image UBO
    //    在一帧内会被 CSM/局部阴影多次覆盖，只能用命令缓冲保证顺序。
    // 2) RecordShadow 在 shadow render pass 已开始后调用，只绑定按
    //    vk::RenderPass 缓存的 caster 管线、描述符与网格并提交 indirect draw。
    void PrepareShadow(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Eigen::Matrix4f& viewProjection,
        int mode,
        int octant,
        const Eigen::Vector3f& lightPosition,
        float farCm,
        float biasCm);
    void RecordShadow(
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
    // 每帧 localShadow 选灯前读取；返回灯光发射器的保守包围球。
    std::vector<ParticleLightShadowCandidate>
        GetParticleLightShadowCandidates() const;
    // 把 localShadow 排名后的 slice 写回轻发射器；非投影灯传 invalid。
    void SetParticleLightShadowBinding(
        const std::string& emitterKey,
        uint32_t slice,
        float farCm,
        float biasCm,
        bool castShadows);
    void ClearParticleLightShadowBindings();

    // 一次性 GPU 回读（调用方需保证设备空闲）。供脚本/调试层消费事件。
    bool DrainParticleEvents(
        RendererBackendVulkan& readbackBackend,
        uint32_t maxPerEmitter,
        std::vector<ParticleEventBatch>& outBatches,
        std::string& outError);

    bool IsInitialized() const noexcept { return initialized; }
    bool HasEmitters() const noexcept { return !emitters.empty(); }
    // 最近一次 Synchronize 解析出的运行时资产（含 userParameters / Data
    // Channels 覆盖）。CPU decal runtime 与 GPU light 发射器都必须复用这份结果，
    // 避免同一发射器在两条输出路径上解析出不同的参数。
    const std::unordered_map<std::string, ParticleSystemAsset>&
        GetResolvedSystemAssets() const noexcept
    {
        return resolvedSystemAssets;
    }
    // renderMode="decal" 且已经分配到 GPU 池的 emitter。ParticleDecalRuntime
    // 用它决定哪些 decal 不再走 CPU 模拟；条目即使本帧没有存活粒子也会
    // 保留一个空 vector，避免路径在 0 粒子时误切回 CPU。
    const std::unordered_map<
        std::string, std::vector<ParticleDecalGpuInstance>>&
        GetDecalGpuInstances() const noexcept
    {
        return decalGpuInstances;
    }
    const ParticleFrameStats& GetFrameStats() const noexcept
    {
        return frameStats;
    }
    // 粒子级运行时命令：console/GT 在帧间调用，实际写入发生在下一次
    // RecordSimulation，保证请求缓冲不会与在飞帧并发。
    bool RequestParticleKill(
        const std::string& sceneObjectName,
        const std::string& emitterName,
        uint32_t particleIndex,
        std::string& outError);
    bool RequestParticleSpawnAt(
        const std::string& sceneObjectName,
        const std::string& emitterName,
        const Eigen::Vector3f& worldPosition,
        float lifetimeSeconds,
        std::string& outError);

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
        // ribbon 历史快照：history[0] 是上一帧，history[15] 最旧；
        // xyz = 记录时的模拟空间位置，w = 记录时的 age。
        std::array<Eigen::Vector4f, 16> history;
        // skeletalTris/Bones 的逐粒子几何源：
        // source0.xyz/source1.xyz/source2.xyz 是三角面或骨段端点；
        // source0.w 为 0 none / 1 triangle / 2 bone，source2.w 保留。
        Eigen::Vector4f source0;
        Eigen::Vector4f source1;
        Eigen::Vector4f source2;
    };

    // mesh 材质 set 1 的 std140 UBO；与 particleMesh.vert/frag 的
    // ParticleMaterialUniform 一一对应。
    struct ParticleMaterialUniforms
    {
        // xy = uvScale, zw = uvOffset
        Eigen::Vector4f uvScaleOffset;
        // x = hasAlbedo, y = hasNormal, z = hasOrm, w = hasEmissive
        Eigen::Vector4f textureFlags;
        // x = alphaClipThreshold, y = ambientOcclusion, z = normalStrength
        Eigen::Vector4f materialParams;
    };
    static_assert(
        sizeof(ParticleMaterialUniforms) == 48,
        "ParticleMaterialUniforms std140 layout must match particleMesh shaders");

    // particleDecalGather.comp 的 set 0 binding 4。std140 下四个 vec4，
    // 与 shader 的 size/rotation/colorOpacity 一一对应。
    struct DecalGatherUniforms
    {
        Eigen::Vector4f size;
        Eigen::Vector4f rotation;
        Eigen::Vector4f colorOpacity;
        Eigen::Vector4f reserved;
    };
    static_assert(
        sizeof(DecalGatherUniforms) == 64,
        "DecalGatherUniforms std140 layout must match particleDecalGather.comp");

    // particleSimulate.comp set 0 binding 9 的逐 emitter 运行时请求。
    // header = uvec4(count, 0, 0, 0)，随后最多 16 条 32 字节记录。
    struct ParticleRuntimeRequestGpu
    {
        Eigen::Vector4f positionLifetime;
        uint32_t kind = 0;
        uint32_t particleIndex = 0;
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
    };
    static_assert(
        sizeof(ParticleRuntimeRequestGpu) == 32,
        "ParticleRuntimeRequestGpu must match particleSimulate.comp std430");

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
        // x = billboard alignment (0 camera, 1 velocity, 2 fixed),
        // y/z/w reserved.
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
        // x = 并行弧数量, y = branchCount, z = branchStart,
        // w = branchLength。
        Eigen::Vector4f beamParams;
        // fixed billboard 的世界空间法线与上方向；xyz 已在 CPU 归一化。
        // 追加在 beam 之后，保持既有字段偏移不变。
        Eigen::Vector4f billboardFixedNormal;
        Eigen::Vector4f billboardFixedUp;
        // x = 分支末端横向偏移（cm，已乘模型最大列缩放），y/z/w 保留。
        // 追加在 fixed billboard 之后，避免重排既有偏移。
        Eigen::Vector4f beamBranch;
        // x = distortionStrength（屏幕空间 UV 比例），
        // y = 纹理可用（0 时回退程序化方向场），z/w 保留。
        // 追加在 beamBranch 之后，保持既有字段偏移不变。
        Eigen::Vector4f distortionParams;
        // beam 逐段曲线与逐主弧覆盖。全部追加在 distortionParams 之后，
        // 旧字段偏移保持不变。
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys> beamColorKeys;
        std::array<Eigen::Vector4f, 2> beamColorKeyTimes;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys> beamWidthKeys;
        // x = color key count, y = width key count, z/w 保留。
        Eigen::Vector4f beamCurveParams;
        std::array<Eigen::Vector4f, kMaxParticleBeamCount>
            beamArcColorStart;
        std::array<Eigen::Vector4f, kMaxParticleBeamCount>
            beamArcColorEnd;
        // x = widthScale, y = intensity, z = noisePhase, w 保留。
        std::array<Eigen::Vector4f, kMaxParticleBeamCount>
            beamArcParams;
        // x = 全局 light 槽位 base, y = 本发射器槽位容量,
        // z = cd intensity, w = cm radius。仅 light emit compute 消费。
        Eigen::Vector4f particleLightParams;
        // 阴影 caster pass 专用：每个 shadow 视图/象限录制前覆盖。
        Eigen::Matrix4f shadowViewProj;
        // x = mode 0 透视 / 1 八面体, y = 象限, z = far cm, w = bias cm。
        Eigen::Vector4f shadowCasterParams;
        // xyz = 点光位置；仅八面体模式使用。
        Eigen::Vector4f shadowLightPosition;
        // billboard/ribbon 材质近似：x = specular 强度, y = roughness,
        // z = 前向散射权重, w 保留。追加在末尾以保持既有字段偏移不变。
        Eigen::Vector4f billboardLightParams2;
        // ribbon 追加参数：x = UV 平铺次数, y = width key count, z/w 保留。
        Eigen::Vector4f ribbonUVParams;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys> ribbonWidthKeys;
        std::array<Eigen::Vector4f, 2> ribbonWidthKeyTimes;
        // SubUV 扩展：x = per-particle 播放, y = 线性混合, z = playRate。
        Eigen::Vector4f flipbookParams5;
        // x = EOO 打包纹理, y = EOO emissive 强度, z/w 保留。
        Eigen::Vector4f textureModeParams;
        // x = source mode (0 none,1 tris,2 bones,3 parent), y = parent event
        // (0 spawn,1 death,2 collision), z = perEvent, w = normalVelocityScale.
        Eigen::Vector4f spawnSourceParams;
        // xyz = parent/source offset (cm), w reserved.
        Eigen::Vector4f spawnSourceOffset;
        // x = burstDelay, y = burstPhase, z/w reserved.
        Eigen::Vector4f burstTiming;
        // x = emit spawn events for child emitters, y/z/w reserved.
        Eigen::Vector4f spawnEventOutput;
        // mesh/skeletal dissolve：x=start,y=end,z=edgeWidth,w=noiseScale。
        Eigen::Vector4f dissolveParams;
        // x=edgeStrength,y=noiseSpeed,z=enabled,w=reserved。
        Eigen::Vector4f dissolveParams2;
        // xyz=edgeColor。
        Eigen::Vector4f dissolveEdgeColor;
        // SubUV 逐寿命曲线。rate 是乘数；uv offset/scale 作用于 sprite UV。
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            flipbookPlayRateKeys;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            flipbookUvOffsetKeys;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            flipbookUvScaleKeys;
        std::array<Eigen::Vector4f, 2> flipbookRateKeyTimes;
        std::array<Eigen::Vector4f, 2> flipbookUvKeyTimes;
        // x=rateCount,y=offsetCount,z=scaleCount,w=startFrameJitter
        Eigen::Vector4f flipbookParams6;
        // ribbon 逐弧长颜色；ribbonUVParams.z 是 key count。
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            ribbonColorKeys;
        std::array<Eigen::Vector4f, 2> ribbonColorKeyTimes;
        // xyz=volume min, w=enabled；xyz=1/extent,w=intensity。
        Eigen::Vector4f translucencyVolumeMin;
        Eigen::Vector4f translucencyVolumeInvExtent;
        // 粒子灯阴影：x=slice,y=type,z=far,w=bias。
        Eigen::Vector4f particleLightShadowParams;
        // x=IES profile index, y=hasIes, z/w reserved。
        Eigen::Vector4f particleLightIesParams;
        // mesh 溶解外推：x=半径随寿命推进(0..1), y=总外推比, z=启用, w=保留。
        Eigen::Vector4f puffParams;
        // 逐粒子灯亮度/颜色曲线：keys[i].xy=(lifeT, brightness)；
        // colors[i].rgb=逐 key 颜色乘数；params.x=key count。
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            particleLightBrightnessKeys;
        Eigen::Vector4f particleLightBrightnessParams;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            particleLightBrightnessColors;
        // 颜色曲线切线。标量 size 切线塞进 sizeKeys 的 z/w，不额外占
        // 数组；全部追加在末尾，旧字段偏移不变。
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            colorTangentIn;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            colorTangentOut;
        // x = curve interpolation code (0 linear, 1 hermite, 2 bezier),
        // y/z/w reserved.
        Eigen::Vector4f curveInterpolationParams;
        // flipbook 播放相位时间线；pivotKeys.x=time、yz=pivot，
        // rotationKeys=(time, degrees, tangentIn, tangentOut)。
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            flipbookPivotKeys;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            flipbookPivotTangentIn;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            flipbookPivotTangentOut;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            flipbookRotationKeys;
        // x = pivot key count, y = rotation key count, z/w reserved.
        Eigen::Vector4f flipbookTimelineParams;
        // x = LOD renderScale, y = smoothstep blend, z/w reserved.
        Eigen::Vector4f lodParams;
        // ribbon/beam 颜色切线（标量切线存放在对应 key 的 z/w）。
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            ribbonColorTangentIn;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            ribbonColorTangentOut;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            beamColorTangentIn;
        std::array<Eigen::Vector4f, kMaxParticleCurveKeys>
            beamColorTangentOut;
        // x = ribbonPivot, y = ribbonTwist degrees, z/w reserved.
        Eigen::Vector4f ribbonShapeParams;
    };
    static_assert(
        sizeof(FrameUniforms) == 10224,
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
    static_assert(
        offsetof(FrameUniforms, billboardFixedNormal) == 6320,
        "ParticleFrame billboardFixedNormal offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, billboardFixedUp) == 6336,
        "ParticleFrame billboardFixedUp offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamBranch) == 6352,
        "ParticleFrame beamBranch offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, distortionParams) == 6368,
        "ParticleFrame distortionParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamColorKeys) == 6384,
        "ParticleFrame beamColorKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamColorKeyTimes) == 6512,
        "ParticleFrame beamColorKeyTimes offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamWidthKeys) == 6544,
        "ParticleFrame beamWidthKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamCurveParams) == 6672,
        "ParticleFrame beamCurveParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamArcColorStart) == 6688,
        "ParticleFrame beamArcColorStart offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamArcColorEnd) == 6944,
        "ParticleFrame beamArcColorEnd offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamArcParams) == 7200,
        "ParticleFrame beamArcParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, particleLightParams) == 7456,
        "ParticleFrame particleLightParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shadowViewProj) == 7472,
        "ParticleFrame shadowViewProj offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shadowCasterParams) == 7536,
        "ParticleFrame shadowCasterParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shadowLightPosition) == 7552,
        "ParticleFrame shadowLightPosition offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, billboardLightParams2) == 7568,
        "ParticleFrame billboardLightParams2 offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, ribbonUVParams) == 7584,
        "ParticleFrame ribbonUVParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, ribbonWidthKeys) == 7600,
        "ParticleFrame ribbonWidthKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, ribbonWidthKeyTimes) == 7728,
        "ParticleFrame ribbonWidthKeyTimes offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookParams5) == 7760,
        "ParticleFrame flipbookParams5 offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, textureModeParams) == 7776,
        "ParticleFrame textureModeParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, spawnSourceParams) == 7792,
        "ParticleFrame spawnSourceParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, spawnSourceOffset) == 7808,
        "ParticleFrame spawnSourceOffset offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, burstTiming) == 7824,
        "ParticleFrame burstTiming offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, spawnEventOutput) == 7840,
        "ParticleFrame spawnEventOutput offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, dissolveParams) == 7856,
        "ParticleFrame dissolveParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, dissolveParams2) == 7872,
        "ParticleFrame dissolveParams2 offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, dissolveEdgeColor) == 7888,
        "ParticleFrame dissolveEdgeColor offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookPlayRateKeys) == 7904,
        "ParticleFrame flipbookPlayRateKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookUvOffsetKeys) == 8032,
        "ParticleFrame flipbookUvOffsetKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookUvScaleKeys) == 8160,
        "ParticleFrame flipbookUvScaleKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookRateKeyTimes) == 8288,
        "ParticleFrame flipbookRateKeyTimes offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookUvKeyTimes) == 8320,
        "ParticleFrame flipbookUvKeyTimes offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookParams6) == 8352,
        "ParticleFrame flipbookParams6 offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, ribbonColorKeys) == 8368,
        "ParticleFrame ribbonColorKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, ribbonColorKeyTimes) == 8496,
        "ParticleFrame ribbonColorKeyTimes offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, translucencyVolumeMin) == 8528,
        "ParticleFrame translucencyVolumeMin offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, translucencyVolumeInvExtent) == 8544,
        "ParticleFrame translucencyVolumeInvExtent offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, particleLightShadowParams) == 8560,
        "ParticleFrame particleLightShadowParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, particleLightIesParams) == 8576,
        "ParticleFrame particleLightIesParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, puffParams) == 8592,
        "ParticleFrame puffParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, particleLightBrightnessKeys) == 8608,
        "ParticleFrame particleLightBrightnessKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, particleLightBrightnessParams) == 8736,
        "ParticleFrame particleLightBrightnessParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, particleLightBrightnessColors) == 8752,
        "ParticleFrame particleLightBrightnessColors offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, colorTangentIn) == 8880,
        "ParticleFrame colorTangentIn offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, colorTangentOut) == 9008,
        "ParticleFrame colorTangentOut offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, curveInterpolationParams) == 9136,
        "ParticleFrame curveInterpolationParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookPivotKeys) == 9152,
        "ParticleFrame flipbookPivotKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookPivotTangentIn) == 9280,
        "ParticleFrame flipbookPivotTangentIn offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookPivotTangentOut) == 9408,
        "ParticleFrame flipbookPivotTangentOut offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookRotationKeys) == 9536,
        "ParticleFrame flipbookRotationKeys offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, flipbookTimelineParams) == 9664,
        "ParticleFrame flipbookTimelineParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, lodParams) == 9680,
        "ParticleFrame lodParams offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, ribbonColorTangentIn) == 9696,
        "ParticleFrame ribbonColorTangentIn offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, ribbonColorTangentOut) == 9824,
        "ParticleFrame ribbonColorTangentOut offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamColorTangentIn) == 9952,
        "ParticleFrame beamColorTangentIn offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, beamColorTangentOut) == 10080,
        "ParticleFrame beamColorTangentOut offset must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, ribbonShapeParams) == 10208,
        "ParticleFrame ribbonShapeParams offset must match particleCommon.glsl");

    struct EmitterResources
    {
        std::string key;
        ParticleEmitterAsset asset;
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        Eigen::Vector3f worldCenter = Eigen::Vector3f::Zero();
        float boundsRadius = 0.0f;
        ParticleFlipbookGpuTable flipbookTable;
        GpuBuffer particles;
        // 当前帧父 emitter spawn/death/collision 事件；子 emitter 读取。
        GpuBuffer spawnEvents;
        // skeletalTris/skeletalBones 的 CPU/GPU 采样源。
        GpuBuffer spawnSource;
        uint32_t spawnSourceRecordCount = 0;
        bool emitSpawnEvents = false;
        // GPU 计数/紧凑存活索引/事件缓冲：每帧由 fillBuffer 清零计数，
        // simulate 原子追加，finalize compute 把 alive 写回 indirectArgs。
        GpuBuffer counters;
        GpuBuffer aliveIndices;
        GpuBuffer events;
        // F2：GT 写入的 kill/spawn 请求；header + 16 条记录。
        GpuBuffer runtimeRequests;
        uint32_t nextRuntimeSpawnSlot = 0;
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
        // light 发射器专用：UBO + 只读粒子池 + 全局 light 输出 SSBO。
        // 普通发射器不分配这组 set。
        std::vector<vk::DescriptorSet> lightEmitSets;
        std::shared_ptr<Texture> texture;
        // mesh 专用材质贴图；失败/缺失回退 ParticleRenderer 的默认纹理。
        std::shared_ptr<Texture> materialAlbedo;
        std::shared_ptr<Texture> materialNormal;
        std::shared_ptr<Texture> materialOrm;
        std::shared_ptr<Texture> materialEmissive;
        // mesh 材质 set 1：每 image 一个 set，共享同一个只读 UBO。
        GpuBuffer materialUniform;
        std::vector<vk::DescriptorSet> materialSets;
        // 作者给了 texturePath 但解码/创建失败时置 true。distortion 需要
        // 用它回退到程序化方向场，而不是把 1x1 白图当成 (1,1) 偏移。
        bool textureFallback = false;
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
        // 静态 mesh 的逐 section 资源；sectionPlans 存在时替代合并缓冲。
        struct StaticSection
        {
            GpuBuffer vertices;
            GpuBuffer indices;
            uint32_t indexCount = 0;
            ParticleMaterialParams material;
            std::shared_ptr<Texture> albedo;
            std::shared_ptr<Texture> normal;
            std::shared_ptr<Texture> orm;
            std::shared_ptr<Texture> emissive;
            GpuBuffer materialUniform;
            std::vector<vk::DescriptorSet> materialSets;
            float localBoundsRadius = 0.0f;
        };
        std::vector<StaticSection> staticSections;
        // 骨骼网格粒子的 sections：每 section 独立顶点/索引/权重/palette/
        // 材质，保证 per-section materialInstancePath 与 inverse bind 正确。
        // spawn 源/父事件三个 binding 依赖 BindParentSpawnSources 选择的父
        // 发射器；CreateDescriptors 之后由 EnsureSpawnSourceBindings 统一写入。
        bool spawnSourceBindingsDirty = true;
        struct SkeletalSection
        {
            struct Influence
            {
                uint32_t joint = 0;
                float weight = 0.0f;
            };
            // 蒙皮顶点缓冲只保存绑定姿态：解耦的粒子实例共享同一份几何，
            // 真正的形变在顶点着色器里按关节 palette 计算（GPU skinning）。
            GpuBuffer vertices;
            GpuBuffer indices;
            // CSR 影响流（顶点区间 + (关节, 权重) 对），set 2 binding 1。
            GpuBuffer skinInfluences;
            // 每个 swapchain image 一份 palette SSBO：CPU 每帧写当前姿态的
            // jointModel × inverseBind，避免与在飞帧复用同一段 buffer。
            std::vector<GpuBuffer> skinPalettes;
            std::vector<vk::DescriptorSet> paletteSets;
            std::vector<vk::DescriptorSet> materialSets;
            uint32_t indexCount = 0;
            ParticleMaterialParams material;
            std::shared_ptr<Texture> albedo;
            std::shared_ptr<Texture> normal;
            std::shared_ptr<Texture> orm;
            std::shared_ptr<Texture> emissive;
            GpuBuffer materialUniform;
            float localBoundsRadius = 0.0f;
            std::vector<Eigen::Matrix4f> inverseBinds;
            Eigen::Matrix4f meshToModel = Eigen::Matrix4f::Identity();
            // CPU 侧蒙皮源（绑定姿态顶点 + CSR 权重）；只用于构建三角面/骨段
            // spawn 源，不参与每帧顶点更新。
            std::vector<Vertex> sourceVertices;
            std::vector<uint32_t> sourceIndices;
            std::vector<uint32_t> influenceOffsets;
            std::vector<Influence> influences;
            std::vector<Vertex> skinnedVertices;
        };
        std::shared_ptr<const Animation::SkeletalAsset> skeletalAsset;
        std::size_t skeletalClipIndex = 0;
        float skeletalPoseTime = 0.0f;
        float skeletalPoseRate = 0.0f;
        bool skeletalPoseLoop = true;
        std::vector<SkeletalSection> skeletalSections;
        // 文件 + 文件大小 + 修改时间 + 片段 + 播放模式；变化时重建骨骼网格资源。
        std::string skeletalSourceKey;
        // 最近一次写入 palette 的姿态键；同一帧内多个骨骼发射器可共享采样结果。
        ParticleSkeletalPoseKey skeletalPoseKey;
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
        // 本发射器在全局 64 槽 GPU light 输出中的确定性子区间。
        uint32_t lightSlotBase = 0;
        uint32_t lightSlotCapacity = 0;
        uint32_t lightBufferGeneration = 0;
        uint32_t iesProfileIndex = 0;
        uint32_t particleLightShadowSlice = 0xFFFFFFFFu;
        float particleLightShadowFar = 0.0f;
        float particleLightShadowBias = 2.0f;
        bool particleLightCastShadows = false;
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
        // renderMode="decal" 专用 gather 输出。每个 swapchain image 一份，
        // 避免在飞帧共享同一段读回缓冲；readback 保持 host-visible 映射。
        struct DecalGatherImage
        {
            GpuBuffer params;
            GpuBuffer output;
            GpuBuffer readback;
            vk::DescriptorSet set;
        };
        std::vector<DecalGatherImage> decalGatherImages;
        // 创建 descriptor 时的场景光照代际；换代后必须重建 set 0 的
        // binding 6..9，否则会继续采样已退休的 buffer/cubemap。
        uint32_t sceneLightingGeneration = 0;
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
    // PrepareShadow 与 RecordShadow 必须使用完全相同的 caster 判定：
    // 作者开关、对应几何有效、可见/启用、本 image UBO 存在，以及包围球
    // 与当前 shadow 视图的保守相交。任何条件只改一处都会导致 UBO 更新与
    // draw 数量不一致。返回 None 表示不投影。
    static ParticleShadowCasterMode ResolveShadowCasterMode(
        const EmitterResources& emitter,
        uint32_t imageIndex,
        const Eigen::Vector3f& lightPosition,
        float farCm);
    void CreateBuffers(EmitterResources& emitter);
    void LoadEmitterMesh(
        EmitterResources& emitter,
        const std::string& emitterKey);
    void LoadSkeletalEmitterMesh(
        EmitterResources& emitter,
        const std::string& emitterKey,
        const MeshAssetImportResult& imported);
    void LoadEmitterMaterialTextures(
        EmitterResources& emitter,
        const std::string& emitterKey);
    std::shared_ptr<Texture> ResolveMaterialTexture(
        const std::string& emitterKey,
        const std::string& assetPath,
        HostImage::TextureSemantic semantic,
        const std::shared_ptr<Texture>& fallback);
    void CreateDescriptors(EmitterResources& emitter);
    void CreateDecalEmitterDescriptors(EmitterResources& emitter);
    void CreateLightEmitterDescriptors(EmitterResources& emitter);
    void ReadbackDecalGather(
        EmitterResources& emitter,
        uint32_t imageIndex);
    void RecordDecalGather(
        vk::CommandBuffer commandBuffer,
        EmitterResources& emitter,
        uint32_t imageIndex,
        float lodRenderScale);
    void CreateFallbackLightingResources();
    void DestroyFallbackLightingResources();
    void EnsureTranslucencyVolume();
    void DestroyTranslucencyVolume();
    void UpdateTranslucencyVolume(const RenderScene& renderScene);
    void RebuildIesLut();
    std::shared_ptr<PipelineBase> GetOrCreateShadowPipeline(
        const Renderpass& renderPass,
        ParticleShadowCasterMode mode);
    const vk::DescriptorBufferInfo& SceneLightInfoForImage(
        uint32_t imageIndex) const;
    const vk::DescriptorBufferInfo& ParticleLightInfoForImage(
        uint32_t imageIndex) const;
    const vk::DescriptorBufferInfo& LightGridInfoForImage(
        uint32_t imageIndex) const;
    const vk::DescriptorBufferInfo& LightIndexInfoForImage(
        uint32_t imageIndex) const;
    const vk::DescriptorBufferInfo& LightGridParamInfoForImage(
        uint32_t imageIndex) const;
    const vk::DescriptorImageInfo& EnvironmentCubeDescriptor() const;
    const vk::DescriptorImageInfo& BrdfLutDescriptor() const;
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
        const ParticleDataChannelMap& dataChannels,
        const std::unordered_map<std::string, float>& userParameters);
    void WarnUserParameterFailureOnce(
        const std::string& key,
        uint64_t signature,
        const std::string& error);
    void SynchronizeGlobalSort();
    void AssignParticleLightSlots();
    void BindParentSpawnSources();
    // 写入 compute set 0 的 spawn 源（binding 6）、父事件（binding 7）与事件
    // 输出（binding 8）。CreateDescriptors 分配 computeSets 时这些绑定还没
    // 选定，BindParentSpawnSources 之后统一写一次。
    void EnsureSpawnSourceBindings(EmitterResources& emitter);
    EmitterResources* FindEmitterBySceneAndName(
        const std::string& sceneObjectName,
        const std::string& emitterName);
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
    std::shared_ptr<ComputePipeline> particleLightEmitPipeline;
    std::shared_ptr<ComputePipeline> decalGatherPipeline;
    std::shared_ptr<ComputePipeline> softDepthPipeline;
    struct SoftDepthResources
    {
        vk::DescriptorPool descriptorPool{};
        std::vector<vk::DescriptorSet> sets;
        std::vector<GpuBuffer> uniforms;
        uint64_t signature = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    SoftDepthResources softDepthResources;
    void DestroySoftDepthResources();
    std::shared_ptr<PipelineBase> additivePipeline;
    std::shared_ptr<PipelineBase> alphaPipeline;
    // ribbon 与 billboard 共用 descriptor set layout 与 particle SSBO，
    // 只替换 shader 与顶点数。
    std::shared_ptr<PipelineBase> ribbonAdditivePipeline;
    std::shared_ptr<PipelineBase> ribbonAlphaPipeline;
    // beam 与 ribbon 共用 particle pass 的 set 0/set 3；顶点数由段数决定，
    // 实例数由 count * (1 + branchCount) 决定，不读取存活粒子索引。
    std::shared_ptr<PipelineBase> beamAdditivePipeline;
    std::shared_ptr<PipelineBase> beamAlphaPipeline;
    // distortion 与 particle pass 的 set 0 合同相同，但使用独立 renderPass
    // 与 distortion 附件；该管线只在图里存在 distortion pass 时创建。
    std::shared_ptr<PipelineBase> distortionPipeline;
    // mesh 使用静态 SM 的 Vertex 输入布局与 drawIndexed；排序仍共用
    // particle SSBO 与 sortedPairs，只替换顶点/片元 shader。
    std::shared_ptr<PipelineBase> meshAdditivePipeline;
    std::shared_ptr<PipelineBase> meshAlphaPipeline;
    // 骨骼网格完整实例（与静态 mesh 共用顶点/材质合同，额外 set 2 蒙皮）。
    std::shared_ptr<PipelineBase> skeletalMeshAdditivePipeline;
    std::shared_ptr<PipelineBase> skeletalMeshAlphaPipeline;
    // SkeletalMeshTris/Bones：逐粒子三角面/骨段，无顶点输入。
    std::shared_ptr<PipelineBase> skeletalTriangleAdditivePipeline;
    std::shared_ptr<PipelineBase> skeletalTriangleAlphaPipeline;
    std::shared_ptr<PipelineBase> skeletalBoneAdditivePipeline;
    std::shared_ptr<PipelineBase> skeletalBoneAlphaPipeline;
    // mesh/billboard/ribbon caster 管线按 shadow/localShadow 的
    // VkRenderPass 句柄 + caster 几何类型缓存；同一图里 CSM 与局部阴影
    // 使用不同 render pass，不能共用。
    std::unordered_map<uint64_t, std::shared_ptr<PipelineBase>>
        shadowPipelines;
    PipelineFactory* pipelineFactory = nullptr;
    // PrepareShadow 与 RecordShadow 之间的单帧剔除输入；两者按 pass
    // 严格交替调用，只在渲染线程使用。
    Eigen::Vector3f shadowCullLightPosition = Eigen::Vector3f::Zero();
    float shadowCullFarCm = 0.0f;
    std::shared_ptr<PipelineBase> globalDrawPipeline;
    std::shared_ptr<Texture> defaultTexture;
    // mesh 材质缺失槽位的常量默认值：白 albedo 复用 defaultTexture；
    // 法线用 (0,0,1)，ORM 白图，emissive 黑图。
    std::shared_ptr<Texture> defaultNormalTexture;
    std::shared_ptr<Texture> defaultOrmTexture;
    std::shared_ptr<Texture> defaultEmissiveTexture;
    std::shared_ptr<Texture> iesLutTexture;
    uint64_t iesLutSignature = 0;
    // 16^3 RGBA16F 世界空间辐照度体积，粒子/骨骼/ribbon 共享。
    vk::Image translucencyVolumeImage{};
    vk::DeviceMemory translucencyVolumeMemory{};
    vk::ImageView translucencyVolumeView{};
    vk::Sampler translucencyVolumeSampler{};
    vk::DescriptorImageInfo translucencyVolumeDescriptor{};
    GpuBuffer translucencyVolumeStaging{};
    bool translucencyVolumeReady = false;
    bool translucencyVolumeUploaded = false;
    uint64_t translucencyVolumeSignature = 0;
    // 发射器包整体由 shared_ptr 持有：活跃表与 retire queue 共享同一所有权，
    // 最后一个 epoch 完成后才析构 descriptor pool、buffer 与纹理。
    std::unordered_map<std::string, std::shared_ptr<EmitterResources>>
        emitters;
    std::unordered_map<std::string, ParticleSystemAsset>
        resolvedSystemAssets;
    // 上次 gather 回读的紧凑 decal 记录。RecordSimulation 在上一帧 fence
    // 之后解析；ParticleDecalRuntime 在下一帧 Update 消费，因此有 1 帧
    // 的确定性延迟，但不会读到在飞帧数据。
    std::unordered_map<
        std::string, std::vector<ParticleDecalGpuInstance>>
        decalGpuInstances;
    struct PendingParticleRequest
    {
        std::string emitterKey;
        bool kill = false;
        uint32_t particleIndex = 0;
        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        float lifetime = 0.0f;
    };
    std::vector<PendingParticleRequest> pendingParticleRequests;
    // per-swapchain-image，由 RendererFrameResources 持有；这里只保存引用。
    std::vector<vk::Buffer> particleLightBuffers;
    uint32_t particleLightBufferGeneration = 0;
    // 场景灯/粒子灯 descriptor 输入同样按 image 索引保存副本；真正的
    // GPU 资源仍由 RendererFrameResources 持有，这里只保存绑定视图。
    std::vector<vk::DescriptorBufferInfo> sceneLightInfos;
    std::vector<vk::DescriptorBufferInfo> sceneParticleLightInfos;
    std::vector<vk::DescriptorBufferInfo> lightGridInfos;
    std::vector<vk::DescriptorBufferInfo> lightIndexInfos;
    std::vector<vk::DescriptorBufferInfo> lightGridParamInfos;
    vk::DescriptorImageInfo environmentCubeDescriptor{};
    vk::DescriptorImageInfo brdfLutDescriptor{};
    uint32_t sceneLightingGeneration = 0;
    // 资源缺失时的兜底：32 字节空场景灯 header、全零粒子灯 SSBO、
    // 1x1x6 黑色 cubemap 与 1x1 黑色 BRDF LUT。避免取样未定义 descriptor。
    GpuBuffer fallbackSceneLightBuffer{};
    GpuBuffer fallbackParticleLightBuffer{};
    GpuBuffer fallbackLightGridBuffer{};
    GpuBuffer fallbackLightIndexBuffer{};
    GpuBuffer fallbackLightGridParamBuffer{};
    vk::DescriptorBufferInfo fallbackSceneLightInfo{};
    vk::DescriptorBufferInfo fallbackParticleLightInfo{};
    vk::DescriptorBufferInfo fallbackLightGridInfo{};
    vk::DescriptorBufferInfo fallbackLightIndexInfo{};
    vk::DescriptorBufferInfo fallbackLightGridParamInfo{};
    std::shared_ptr<Texture> fallbackEnvironmentCube;
    std::shared_ptr<Texture> defaultBlackTexture;
    // 仅缓存弱引用：仍被活跃/退休发射器持有的纹理由发射器生命周期管理，
    // Shutdown 清空索引不会提前销毁 GPU 资源。
    std::unordered_map<std::string, std::weak_ptr<Texture>>
        textureCache;
    std::unordered_set<std::string> warnedTextureFallbacks;
    std::unordered_set<std::string> warnedMeshFallbacks;
    std::unordered_set<std::string> warnedMaterialFallbacks;
    bool warnedMissingParticleLightBuffer = false;
    // 运行时 userParameters 重解析缓存：key = system 身份，value 保存
    // 最近一次成功解析的资产 + 失败标记，避免每帧重新 ParseJson。
    struct ParticleUserParameterCacheEntry
    {
        uint64_t signature = 0;
        uint64_t channelSignature = 0;
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
