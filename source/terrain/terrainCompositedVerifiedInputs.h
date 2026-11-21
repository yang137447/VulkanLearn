#pragma once

#include "shader/build/contentHash.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace VL
{
class TerrainCompositedVTStore;
class TerrainRvtBudgetOwner;

// Service/Monitor 的请求身份；它本身不携带验证证据。
struct TerrainCompositedVerifiedInputBinding
{
    std::shared_ptr<const TerrainCompositedVTStore> store;
    uint64_t worldGeneration = 0;
    ContentDigest productionIdentity;
    std::string surfaceId;
};

struct TerrainCompositedVerifiedInputStatistics
{
    uint64_t beginNanoseconds = 0;
    uint64_t endNanoseconds = 0;
    uint64_t requests = 0;
    uint64_t requestedBytes = 0;
    uint64_t readCalls = 0;
    uint64_t readAttemptBytes = 0;
    // Store 在损坏/IO 异常时不返回实际读量；此值只统计成功返回的真实 vector。
    uint64_t actualReadBytes = 0;
    uint64_t validatedBytes = 0;
    uint64_t hits = 0;
    uint64_t failures = 0;
    uint64_t verifiedObjects = 0;
    uint64_t retainedBytes = 0;
    uint64_t metadataBytes = 0;
    bool closed = false;
};

// terrain 拥有的 World 初始化批次：真实 Store.ReadInput 建立不可伪造的内部 proof。
// Service/Monitor 共用；不读任意作者路径、不清 dirty、不提供免 hash 重读权限。
class TerrainCompositedVerifiedInputs
{
public:
    using Binding = TerrainCompositedVerifiedInputBinding;
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    static constexpr std::size_t MaxObjects = 65536;
    static constexpr uint64_t MaxCpuBytes = uint64_t{512} * 1024 * 1024;
    static constexpr uint64_t ReadScratchBytes = 128 * 1024;

    static std::shared_ptr<TerrainCompositedVerifiedInputs> Begin(
        std::shared_ptr<const TerrainCompositedVTStore> openedStore,
        uint64_t worldGeneration, ContentDigest productionIdentity,
        std::shared_ptr<TerrainRvtBudgetOwner> cpuOwner,
        std::size_t maxObjects = MaxObjects);
    ~TerrainCompositedVerifiedInputs();
    TerrainCompositedVerifiedInputs(const TerrainCompositedVerifiedInputs&) = delete;
    TerrainCompositedVerifiedInputs& operator=(const TerrainCompositedVerifiedInputs&) = delete;

    // proof-only 首读后释放 bytes；Close 后仅已登记 surface/object 引用可重复消费 proof。
    // 未登记引用不能在 Close 后登记，proof 永远不能升级为 payload。
    void VerifyObject(const Binding& binding, const std::string& objectId);
    // 首读 move whole vector；命中返回同一 immutable snapshot，不再次读磁盘。
    Bytes ReadVerified(const Binding& binding, const std::string& objectId);
    bool HasVerified(const Binding& binding, const std::string& objectId) const;
    bool Matches(const Binding& binding) const;
    // Close 与读取串行；关闭后已登记引用仍可 VerifyObject/HasVerified，ReadVerified 全部拒绝。
    // 已取得 bytes 独立保活；新 World/Store/publication 必须 Begin 新批次。
    void Close();
    TerrainCompositedVerifiedInputStatistics Statistics() const;

private:
    struct Impl;
    explicit TerrainCompositedVerifiedInputs(std::unique_ptr<Impl> implementation);
    Bytes Request(const Binding& binding, const std::string& objectId, bool retain);
    std::unique_ptr<Impl> impl;
};
}
