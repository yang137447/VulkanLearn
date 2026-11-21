#pragma once

#include "materialInstance.h"
#include <vector>

namespace VL
{
class MaterialInstanceStateBatch
{
public:
    struct Change
    {
        std::shared_ptr<MaterialInstance> material;
        MaterialInstanceNumericParameterValues parameters;
        std::map<std::string, std::shared_ptr<Texture>> textures;
    };
    explicit MaterialInstanceStateBatch(std::vector<Change> changes);
    ~MaterialInstanceStateBatch();
    MaterialInstanceStateBatch(const MaterialInstanceStateBatch&) = delete;
    MaterialInstanceStateBatch& operator=(const MaterialInstanceStateBatch&) = delete;
    bool TryLockNoThrow() noexcept;
    void CommitNoThrow() noexcept;
    void CancelNoThrow() noexcept;
    uint64_t MetadataBytes() const noexcept;

private:
    struct Entry;
    std::vector<std::unique_ptr<Entry>> entries;
    bool locked = false;
    bool committed = false;
};
}
