#include "terrainLitCaptureAdapter.h"
#include "render/diagnostics/terrainLitCaptureHooks.h"
#include "material.h"
#include <iostream>
#include <stdexcept>

namespace VL
{
namespace
{
struct CombinedFlight
{
    std::shared_ptr<const void> tes;
    std::shared_ptr<const void> lit;
};
class LitAdapter final : public ITerrainCaptureHooks, public ITerrainLitCaptureHooks
{
    // 允许没有 TES case：lit-only 模式下只记录 lit replay，不评估任何 TES witness。
    std::shared_ptr<ITerrainCaptureHooks> actualTes;
    TerrainLitSessionFactory factory;
    std::shared_ptr<TerrainLitReplaySession> session;
    uint64_t epoch=0;
    bool selected=false;
    bool completed=false;
    std::shared_ptr<TerrainRvtBudgetOwner> referencePool;
public:
    LitAdapter(std::shared_ptr<ITerrainCaptureHooks> tes,TerrainLitSessionFactory prepare)
        :actualTes(std::move(tes)),factory(std::move(prepare))
    {
        if(!factory)throw std::invalid_argument("Explicit lit test adapter requires a native session factory");
        std::cerr<<"[TerrainLit] mode="<<(actualTes?"tes+lit":"lit-only")
            <<" (lit-only: no TES witness is evaluated)\n";
    }
    bool RequiresTesVertexStores()const noexcept override
    {
        return actualTes?actualTes->RequiresTesVertexStores():false;
    }
    void BeforeGeometry(PassRuntimeContext& context,const TerrainCaptureFrameInfo& frame)override
    {
        if(actualTes)actualTes->BeforeGeometry(context,frame);
        if(completed||epoch)return;
        if(session)session->AbortProvenUnsubmitted();
        if(!referencePool)throw std::runtime_error("Explicit lit replay requires a legal native peak/outer reference owner");
        session=factory(context,frame,referencePool);
        selected=false;
    }
    void SetReferencePoolOwner(std::shared_ptr<TerrainRvtBudgetOwner> owner)override
    {
        if(!owner)throw std::invalid_argument("Missing native reference budget owner");
        if(!referencePool)referencePool=std::move(owner);
    }
    std::shared_ptr<PipelineBase> SelectActualDraw(const TerrainCaptureDrawInputs& draw)override
    {
        if(actualTes)
        {
            auto pipeline=actualTes->SelectActualDraw(draw);
            if(pipeline)selected=true;
            return pipeline;
        }
        // lit-only：选中实际地形 draw，但保持真实 pipeline（不改变生产拓扑/绑定）。
        auto pipeline=draw.materialOwner?draw.materialOwner->GetRenderPipeline():std::shared_ptr<PipelineBase>{};
        if(pipeline)selected=true;
        else {static uint32_t reported=0;if(reported<3){++reported;
            std::cerr<<"[TerrainLit] lit-only select miss: materialOwner="<<(draw.materialOwner!=nullptr)
                <<std::endl;}}
        return pipeline;
    }
    void BindActualDraw(vk::CommandBuffer command,const TerrainCaptureDrawInputs& draw,
        const PipelineBase& pipeline)override
    {
        if(actualTes)actualTes->BindActualDraw(command,draw,pipeline);
    }
    void ActualDrawRecorded(const TerrainCaptureDrawInputs& draw)override
    {
        if(actualTes)actualTes->ActualDrawRecorded(draw);
    }
    void AfterGeometry(vk::CommandBuffer command)override
    {
        if(actualTes)actualTes->AfterGeometry(command);
    }
    void AfterActualPass(PassRuntimeContext& context)override
    {
        if(session&&selected&&!epoch&&!completed)session->AfterActualPass(context);
    }
    std::shared_ptr<const void> Submitted(uint64_t actualEpoch)override
    {
        auto flight=std::make_shared<CombinedFlight>();
        if(actualTes)flight->tes=actualTes->Submitted(actualEpoch);
        // 一次 replay 只提交一次：后续帧的 Submitted 不能重复提交同一 session。
        if(session&&selected&&!epoch)
        {
            flight->lit=session->Submitted(actualEpoch);
            epoch=actualEpoch;
        }
        return flight->tes||flight->lit?flight:std::shared_ptr<const void>{};
    }
    void AbortUnsubmitted()override
    {
        if(actualTes)actualTes->AbortUnsubmitted();
        if(session&&!epoch){session->AbortProvenUnsubmitted();session.reset();}
    }
    void ConsumeCompleted(uint64_t completedEpoch)override
    {
        if(actualTes)actualTes->ConsumeCompleted(completedEpoch);
        if(session&&epoch&&session->ConsumeCompleted(completedEpoch))completed=true;
    }
    void CloseAfterDeviceIdle()override
    {
        if(actualTes)actualTes->CloseAfterDeviceIdle();
        if(session)
        {
            if(epoch&&!completed)completed=session->ConsumeCompleted(epoch);
            else if(!epoch)session->AbortProvenUnsubmitted();
            session.reset();
        }
    }
};
}
std::shared_ptr<ITerrainCaptureHooks> AttachNativeTerrainLitReplay(
    std::shared_ptr<ITerrainCaptureHooks> tes,TerrainLitSessionFactory factory)
{
    return std::make_shared<LitAdapter>(std::move(tes),std::move(factory));
}
}
