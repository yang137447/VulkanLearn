#pragma once

#include <memory>
#include <optional>
#include <string>

#include "editor/preview/materialInstancePreviewTypes.h"

namespace VL::Editor::Preview
{

// Adapter 是稳定帧边界上的 renderer owner 扩展点。接口只接收值语义命令，
// 不把 MaterialInstance、RendererResourceCache 或 Vulkan handle 传播到 editor。
class IMaterialInstancePreviewAdapter
{
public:
    virtual ~IMaterialInstancePreviewAdapter() = default;

    virtual MaterialInstancePreviewAdapterResult Execute(
        const MaterialInstancePreviewAdapterCommand& command) = 0;

    virtual MaterialInstancePreviewAdapterResult Poll(
        PreviewOperationId operationId) = 0;

    virtual void Cancel(PreviewOperationId operationId) noexcept = 0;

    virtual MaterialInstancePreviewAdapterResult Disconnect(
        uint64_t bridgeLiveGeneration) = 0;

};

// renderer owner 在稳定边界捕获 MI 后，以 session 持有复制出的 shared_ptr。
// editor adapter 只调用值语义 draft，不直接解析 MaterialInstance 或 Vulkan 资源。
class IRendererMaterialInstancePreviewSession
{
public:
    virtual ~IRendererMaterialInstancePreviewSession() = default;

    virtual MaterialInstancePreviewAdapterResult Apply(
        const MaterialInstancePreviewDraft& draft) = 0;

    virtual MaterialInstancePreviewAdapterResult RestoreBaseline(
        const MaterialInstancePreviewDraft& baselineDraft) = 0;
};

class IRendererMaterialInstancePreviewOwner
{
public:
    virtual ~IRendererMaterialInstancePreviewOwner() = default;

    virtual std::optional<MaterialInstancePreviewWorldIdentity>
    GetActiveWorldIdentity() const = 0;

    virtual MaterialInstancePreviewAdapterResult
    CommitMaterialInstancePreviewDraft(
        const MaterialInstancePreviewWorldIdentity& world,
        const NormalizedMaterialInstancePath& materialInstancePath,
        const MaterialInstancePreviewDraft& draft) = 0;

    // 该调用必须发生在 renderer owner 的稳定边界；失败时 outSession 保持为空，
    // 调用方不能把未校验的 MI 指针或 document baseline 视为已连接。
    virtual MaterialInstancePreviewAdapterResult
    CaptureMaterialInstancePreviewSession(
        const MaterialInstancePreviewAdapterCommand& command,
        std::shared_ptr<IRendererMaterialInstancePreviewSession>& outSession) = 0;
};

// 当前 renderer 没有提供稳定帧 ownership 时，默认实现明确返回 Unavailable，
// 绝不把“未执行”伪装成预览成功。
class UnavailableMaterialInstancePreviewAdapter final
    : public IMaterialInstancePreviewAdapter
{
public:
    explicit UnavailableMaterialInstancePreviewAdapter(
        std::string diagnosticMessage = {});

    MaterialInstancePreviewAdapterResult Execute(
        const MaterialInstancePreviewAdapterCommand& command) override;

    MaterialInstancePreviewAdapterResult Poll(
        PreviewOperationId operationId) override;

    void Cancel(PreviewOperationId operationId) noexcept override;

    MaterialInstancePreviewAdapterResult Disconnect(
        uint64_t bridgeLiveGeneration) override;
private:
    std::string diagnosticMessage;
};

class RendererOwnedMaterialInstancePreviewAdapter final
    : public IMaterialInstancePreviewAdapter
{
public:
    explicit RendererOwnedMaterialInstancePreviewAdapter(
        IRendererMaterialInstancePreviewOwner& owner);

    MaterialInstancePreviewAdapterResult Execute(
        const MaterialInstancePreviewAdapterCommand& command) override;

    MaterialInstancePreviewAdapterResult Poll(
        PreviewOperationId operationId) override;

    void Cancel(PreviewOperationId operationId) noexcept override;

    MaterialInstancePreviewAdapterResult Disconnect(
        uint64_t bridgeLiveGeneration) override;

    // World 换代或 renderer shutdown 时由 owner 调用，释放旧 MI shared_ptr，
    // 不尝试把过期连接的数值写回新 World。
    void Reset() noexcept;

private:
    struct Connection
    {
        MaterialInstancePreviewWorldIdentity world;
        NormalizedMaterialInstancePath materialInstancePath;
        uint64_t bridgeLiveGeneration = 0;
        std::optional<uint64_t> documentRevision;
        std::shared_ptr<IRendererMaterialInstancePreviewSession> session;
    };

    MaterialInstancePreviewAdapterResult ExecuteConnect(
        const MaterialInstancePreviewAdapterCommand& command);
    MaterialInstancePreviewAdapterResult ExecuteLiveUpdate(
        const MaterialInstancePreviewAdapterCommand& command);
    MaterialInstancePreviewAdapterResult ValidateConnectionCommand(
        const MaterialInstancePreviewAdapterCommand& command) const;

    IRendererMaterialInstancePreviewOwner& owner;
    std::optional<Connection> connection;
};

} // namespace VL::Editor::Preview
