#include "render/backend/rendererObjectResourceRegistry.h"

#include <utility>

#include "materialInstance.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererDescriptorContext.h"
#include "render/backend/rendererObjectResourceManager.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/frontend/renderScene.h"
#include "render/resource/rendererResourceCache.h"
#include "renderableObject.h"

#include <memory>
#include <stdexcept>

namespace VL
{

RendererObjectResourceEntry::RendererObjectResourceEntry(std::string objectName)
    : objectName(std::move(objectName))
{
}

RendererObjectResourceEntry::~RendererObjectResourceEntry()
{
    Shutdown();
}

void RendererObjectResourceEntry::Initialize(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext,
    MaterialInstance& materialInstance,
    MaterialShadowCasterKind shadowCasterKind)
{
    this->rendererBackend = &rendererBackend;

    RendererObjectResourceManager resourceManager;
    resourceManager.InitializeObjectResources(
        rendererBackend,
        descriptorContext,
        objectName,
        materialInstance,
        shadowCasterKind,
        resources);
}

void RendererObjectResourceEntry::Shutdown()
{
    RendererObjectResourceManager resourceManager;
    resourceManager.ShutdownObjectResources(rendererBackend, resources);
    rendererBackend = nullptr;
}

void RendererObjectResourceRegistry::InitializeResolvedSceneResources(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext,
    const RenderScene& renderScene,
    ResolvedRenderScene& resolvedRenderScene,
    RendererResourceCache& resourceCache,
    bool prerenderSkin) const
{
    for (ResolvedMaterialGroup& materialGroup : resolvedRenderScene.materialGroups)
    {
        for (ResolvedMaterialInstanceGroup& materialInstanceGroup : materialGroup.materialInstances)
        {
            materialInstanceGroup.materialInstance->RenderInitialize(rendererBackend);
            for (ResolvedDrawPacket& draw : materialInstanceGroup.draws)
            {
                if (draw.drawPacketIndex >= renderScene.drawPackets.size())
                {
                    throw std::runtime_error(
                        "Resolved draw packet references an invalid RenderDrawPacket index");
                }

                const RenderDrawPacket& drawPacket =
                    renderScene.drawPackets[draw.drawPacketIndex];
                std::shared_ptr<RendererObjectResourceEntry> objectResourceEntry;
                const std::shared_ptr<RendererObjectResourceEntry>* cachedEntry =
                    resourceCache.GetObjectResource(drawPacket.debugName);
                if (cachedEntry != nullptr && *cachedEntry != nullptr)
                {
                    objectResourceEntry = *cachedEntry;
                    // 运行时 spawn 的骨骼网格可能先以“无 pose”帧创建对象资源包，
                    // 之后 pose 才到达。对象资源包的 Initialize 是一次性的（已初始化
                    // 就不再补 buffer），直接在旧包上补会破坏描述符布局，因此这里丢弃
                    // 旧包、按带 pre-deform 的路径重建。旧包只被上一帧 resolved scene
                    // 引用，重建发生在本帧 resolve 之后、录制之前。
                    const auto& existingResources = objectResourceEntry->GetResources();
                    if (drawPacket.skinPose && existingResources.initialized &&
                        existingResources.skinDeformVertexCount == 0)
                    {
                        objectResourceEntry = std::make_shared<RendererObjectResourceEntry>(
                            drawPacket.debugName);
                        resourceCache.BindObjectResource(
                            drawPacket.debugName, objectResourceEntry);
                    }
                }
                else
                {
                    objectResourceEntry =
                        std::make_shared<RendererObjectResourceEntry>(drawPacket.debugName);
                    resourceCache.BindObjectResource(drawPacket.debugName, objectResourceEntry);
                }

                // 将 ResolvedScene 已确定的 caster 类型传给 GPU 资源层，确保描述符
                // 分配与随后 DrawShadowScene 使用的是同一份路由结果。
                if (drawPacket.skinPose && !objectResourceEntry->GetResources().initialized)
                {
                    const auto renderable = draw.renderableObject.lock();
                    if (!renderable || !renderable->GetSkinInfluenceInfo().buffer)
                        throw std::runtime_error("Skinned draw is missing its immutable influence stream");
                    auto& resources = objectResourceEntry->GetResources();
                    resources.skinJointCount = drawPacket.skinPose->matrices.size();
                    resources.skinInfluences = renderable->GetSkinInfluenceInfo();
                    // GPU 预蒙皮的对象级输入：compute 直接读同一份顶点流，输出
                    // 按 gl_VertexIndex 寻址，因此顶点数必须在这里冻结。
                    resources.prerenderSkin = prerenderSkin;
                    resources.skinDeformVertexCount = renderable->GetVertexCount();
                    resources.skinSourceVertices = renderable->GetVertexBufferInfo();
                }
                if (!objectResourceEntry->GetResources().initialized)
                {
                    const auto renderable = draw.renderableObject.lock();
                    if (renderable && renderable->IsTressfx())
                        objectResourceEntry->GetResources().tressfxPositions =
                            renderable->GetTressfxPositionInfo();
                }
                try
                {
                    objectResourceEntry->Initialize(
                        rendererBackend,
                        descriptorContext,
                        *materialInstanceGroup.materialInstance,
                        materialGroup.shadowCaster.kind);
                }
                catch (const std::exception& error)
                {
                    // 对象资源包初始化错误默认没有对象身份，排查 spawn/动态对象时必须带上。
                    throw std::runtime_error(std::string(error.what()) +
                        " object=" + drawPacket.debugName);
                }
                draw.objectResourceEntry = objectResourceEntry;
            }
        }
    }
}

} // namespace VL
