#pragma once

#include <string>
#include <string_view>

namespace VL::Hair
{
// 多材质 section/draw 可重复引用同一 placement；独立 placement 的 GPU 发丝已在
// renderable key 层拆成各自的对象/SSBO，但此处仍保留为运行期安全网：若同一
// RenderableObject 被不同 sceneObjectIdentity 复用，停用其模拟并保留最后位置。
class TressfxSimulationOwner
{
public:
    void Observe(std::string_view identity)
    {
        if (!initialized)
        {
            ownerIdentity = identity;
            initialized = true;
        }
        else if (ownerIdentity != identity)
        {
            conflict = true;
        }
    }

    bool HasConflict() const { return conflict; }
    const std::string& OwnerIdentity() const { return ownerIdentity; }
    void Merge(const TressfxSimulationOwner& observed)
    {
        if (observed.initialized)
            Observe(observed.ownerIdentity);
        conflict |= observed.conflict;
    }

private:
    std::string ownerIdentity;
    bool initialized = false;
    bool conflict = false;
};
}
