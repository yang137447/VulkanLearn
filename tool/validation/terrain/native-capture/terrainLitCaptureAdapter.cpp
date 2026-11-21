#include "terrainLitCaptureAdapter.h"
#include "render/diagnostics/terrainLitCaptureHooks.h"
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
        if(!actualTes||!factory)throw std::invalid_argument("Explicit lit test adapter requires TES and native session factory");
    }
    bool RequiresTesVertexStores()const noexcept override {return actualTes->RequiresTesVertexStores();}
    void BeforeGeometry(PassRuntimeContext& context,const TerrainCaptureFrameInfo& frame)override
    {
        actualTes->BeforeGeometry(context,frame);
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
        auto pipeline=actualTes->SelectActualDraw(draw);
        if(pipeline)selected=true;
        return pipeline;
    }
    void BindActualDraw(vk::CommandBuffer command,const TerrainCaptureDrawInputs& draw,
        const PipelineBase& pipeline)override {actualTes->BindActualDraw(command,draw,pipeline);}
    void ActualDrawRecorded(const TerrainCaptureDrawInputs& draw)override {actualTes->ActualDrawRecorded(draw);}
    void AfterGeometry(vk::CommandBuffer command)override {actualTes->AfterGeometry(command);}
    void AfterActualPass(PassRuntimeContext& context)override
    {
        if(session&&selected&&!epoch&&!completed)session->AfterActualPass(context);
    }
    std::shared_ptr<const void> Submitted(uint64_t actualEpoch)override
    {
        auto flight=std::make_shared<CombinedFlight>();
        flight->tes=actualTes->Submitted(actualEpoch);
        if(session&&selected)
        {
            flight->lit=session->Submitted(actualEpoch);
            epoch=actualEpoch;
        }
        return flight->tes||flight->lit?flight:std::shared_ptr<const void>{};
    }
    void AbortUnsubmitted()override
    {
        actualTes->AbortUnsubmitted();
        if(session&&!epoch){session->AbortProvenUnsubmitted();session.reset();}
    }
    void ConsumeCompleted(uint64_t completedEpoch)override
    {
        actualTes->ConsumeCompleted(completedEpoch);
        if(session&&epoch&&session->ConsumeCompleted(completedEpoch))completed=true;
    }
    void CloseAfterDeviceIdle()override
    {
        actualTes->CloseAfterDeviceIdle();
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
