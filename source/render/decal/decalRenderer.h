#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

#include "resource/image/hostImage.h"
#include "render/decal/decalLimits.h"

class PipelineBase;
class PipelineFactory;
class RenderGraph;
class Texture;
struct Renderpass;

namespace VL
{
class RendererBackendVulkan;
struct RenderScene;
struct DecalSnapshot;

// 与 shader/glsl/decal/decalCommon.glsl 的 std430 DecalInstance 一一对应。
// 公开这个布局是为了让布局/stride 回归测试直接校验 offsetof，而不是在
// 测试里复制一份容易漂移的结构体。
struct DecalGpuInstance
{
    Eigen::Matrix4f decalToWorld;
    Eigen::Matrix4f worldToDecal;
    // xyz = baseColor，w = normalStrength（0 表示程序化法线不生效）。
    Eigen::Vector4f color;
    // x = roughness, y = metallic, z = ambientOcclusion, w = opacity.
    Eigen::Vector4f surface;
    // x = edgeSoftness, y = noiseStrength, z = seed,
    // w = DecalResponse 位掩码（color/normal/roughness/metallic）。
    Eigen::Vector4f params;
    // xyz 保留；w = 1 表示当前 decal 成功加载了 normalMap，
    // shader 据此在贴图法线与程序化 FBM 法线之间选择。
    Eigen::Vector4f normal;
    // rgb = 线性 emissive，w = emissiveStrength；写端与 coverage 预乘。
    Eigen::Vector4f emissive;
};
static_assert(sizeof(DecalGpuInstance) == 208,
    "DecalGpuInstance layout must match decalCommon.glsl");

// BeforeLighting DBuffer decal 的 GPU 侧拥有者。一个 decal pass 内用
// 逐 decal 的 unit cube 绘制到 DBuffer A/B/C/D、Mask 与 Emissive；实例数据
// 在每帧固定的 SSBO 槽位中更新，material 贴图走逐 decal 的 set 1
// descriptor（per swapchain image × per instance slot）。
class DecalRenderer
{
public:
    void EnsureInitialized(
        PipelineFactory& factory,
        RendererBackendVulkan& backend,
        RenderGraph& graph);
    void Shutdown(RendererBackendVulkan& backend);
    void InvalidatePipelines(RendererBackendVulkan& backend);
    void RecordGraphics(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass,
        const RenderScene& renderScene);
    // 在 decals pass 之前写硬件 stencil bit0：unit cube 的屏幕覆盖
    // 就是 decal 投影体，camera 在体积内时仍由双面写入保持有效。
    void RecordStencil(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass,
        const RenderScene& renderScene);
    // 渲染线程在进入 decal render pass 之前调用：按 DecalSnapshot::decalId
    // 加载/复用 albedo/normal/ORM 贴图，并为已消失的 decal 退休纹理集。
    // 必须幂等；失败回退默认纹理，不能向渲染线程抛出异常。
    void Synchronize(const RenderScene& renderScene);

    bool IsInitialized() const noexcept { return initialized; }

private:
    struct GpuBuffer
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped = nullptr;
    };

    struct FrameUniforms
    {
        Eigen::Matrix4f viewProjection;
        Eigen::Matrix4f invViewProjection;
    };

    using DecalInstance = DecalGpuInstance;

    // 一个 decal 的代码维护贴图槽位；没有 UE Material Graph。
    struct DecalTextureSet
    {
        std::string albedoPath;
        std::string normalPath;
        std::string ormPath;
        std::shared_ptr<Texture> albedo;
        std::shared_ptr<Texture> normal;
        std::shared_ptr<Texture> orm;
        // 只有 normalMap 真正加载成功才为 true；失败/空路径回退默认 flat normal。
        bool hasNormalMap = false;
        // 创建该纹理集时的 World 世代，用于退休队列排障；不参与时序判断。
        uint64_t ownerGeneration = 0;
    };

    // 已经写入 descriptor 的纹理身份；用于跳过未变化的逐槽 descriptor 更新。
    struct DescriptorTextureState
    {
        const Texture* albedo = nullptr;
        const Texture* normal = nullptr;
        const Texture* orm = nullptr;

        bool operator==(const DescriptorTextureState& other) const
        {
            return albedo == other.albedo &&
                normal == other.normal &&
                orm == other.orm;
        }
    };

    void Initialize(
        PipelineFactory& factory,
        RendererBackendVulkan& backend,
        RenderGraph& graph);
    void CreateStaticGeometry(RendererBackendVulkan& backend);
    void CreateFrameResources(RendererBackendVulkan& backend);
    void CreateMaterialTextures(RendererBackendVulkan& backend);
    std::shared_ptr<Texture> ResolveTexture(
        const std::string& assetPath,
        HostImage::TextureSemantic semantic,
        const std::shared_ptr<Texture>& fallback,
        const std::string& decalName);
    void RetireTextureSet(std::shared_ptr<DecalTextureSet> textureSet);
    void WarnTextureFallbackOnce(
        const std::string& assetPath,
        const std::string& decalName,
        const std::string& error);
    void WriteMaterialDescriptors(
        uint32_t imageIndex,
        uint32_t slot,
        const DecalTextureSet& textures);
    std::vector<const DecalSnapshot*> CollectVisibleDecals(
        const RenderScene& renderScene) const;
    void UploadFrameInstances(
        uint32_t imageIndex,
        const RenderScene& renderScene,
        const std::vector<const DecalSnapshot*>& visibleDecals);
    void DestroyResources(RendererBackendVulkan& backend);
    static void DestroyBuffer(
        RendererBackendVulkan& backend,
        GpuBuffer& buffer);

    RendererBackendVulkan* backend = nullptr;
    bool initialized = false;
    std::shared_ptr<PipelineBase> pipeline;
    // mask pass 与 decals pass 共享 cube/实例 SSBO，但 render pass、
    // stencil 状态和 cull 状态不同，必须各自拥有 pipeline。
    std::shared_ptr<PipelineBase> stencilPipeline;
    GpuBuffer cubeVertices;
    GpuBuffer cubeIndices;
    std::vector<GpuBuffer> frameUniforms;
    std::vector<GpuBuffer> instanceBuffers;
    std::vector<vk::DescriptorSet> descriptorSets;
    vk::DescriptorPool descriptorPool;
    // set 1 material 贴图：image × instance slot 的独立 descriptor set，
    // 每次 draw 只绑定当前 slot，避免每帧写整批 descriptor。
    vk::DescriptorPool materialDescriptorPool;
    std::vector<std::vector<vk::DescriptorSet>> materialDescriptorSets;
    std::vector<std::vector<DescriptorTextureState>>
        descriptorTextureStates;
    // 仅保留弱引用：活跃/已退休纹理集仍持有强引用，Shutdown 清空索引
    // 不会提前销毁 GPU 资源。
    std::unordered_map<std::string, std::weak_ptr<Texture>>
        textureCache;
    std::unordered_map<uint64_t, std::shared_ptr<DecalTextureSet>>
        textureSets;
    // 粒子 decal 的稳定槽位即使短暂没有存活粒子，也保留纹理集；
    // 这样不会在粒子寿命间隙反复退休/重建默认纹理 descriptor。
    std::unordered_set<uint64_t> persistentTextureSetIds;
    // 粒子 decal 短暂进入寿命间隙时不立刻退休纹理；超过 grace 帧仍无
    // 存活 decal 才释放，避免 emitter 被删除/重命名后永久占用内存。
    std::unordered_map<uint64_t, uint32_t>
        persistentTextureMissingFrames;
    uint64_t textureSetWorldGeneration = 0;
    bool hasTextureSetWorldGeneration = false;
    std::unordered_set<std::string> warnedTextureFailures;
    std::shared_ptr<Texture> defaultAlbedoTexture;
    std::shared_ptr<Texture> defaultNormalTexture;
    std::shared_ptr<Texture> defaultOrmTexture;
};

} // namespace VL
