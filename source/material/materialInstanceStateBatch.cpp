#include "material/materialInstanceStateBatch.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace VL
{
struct MaterialInstanceStateBatch::Entry
{
    std::shared_ptr<MaterialInstance> material;
    std::unique_lock<std::shared_mutex> lock;
    decltype(MaterialInstance::parameters) parameters;
    decltype(MaterialInstance::floatParameters) floats;
    decltype(MaterialInstance::vec2Parameters) vec2;
    decltype(MaterialInstance::vec3Parameters) vec3;
    decltype(MaterialInstance::vec4Parameters) vec4;
    decltype(MaterialInstance::textures) textures;
    decltype(MaterialInstance::textureBindingIdentities) identities;
    decltype(MaterialInstance::floatParameters) expectedFloats;
    decltype(MaterialInstance::vec2Parameters) expectedVec2;
    decltype(MaterialInstance::vec3Parameters) expectedVec3;
    decltype(MaterialInstance::vec4Parameters) expectedVec4;
    decltype(MaterialInstance::textures) expectedTextures;
    uint64_t expectedBindingVersion = 0;
    bool textureChanged = false;
};

namespace
{
template<typename Map>
bool SameValues(const Map& left, const Map& right) noexcept
{
    if (left.size() != right.size())
        return false;
    for (const auto& entry : left)
    {
        const auto found = right.find(entry.first);
        if (found == right.end())
            return false;
        using Value = typename Map::mapped_type;
        if constexpr (std::is_same_v<Value, float>)
        {
            if (entry.second != found->second)
                return false;
        }
        else if (!(entry.second.array() == found->second.array()).all())
            return false;
    }
    return true;
}
}

MaterialInstanceStateBatch::MaterialInstanceStateBatch(std::vector<Change> changes)
{
    std::sort(changes.begin(), changes.end(), [](const auto& left, const auto& right) {
        return std::less<MaterialInstance*>{}(left.material.get(), right.material.get());
    });
    entries.reserve(changes.size());
    std::set<MaterialInstance*> unique;
    for (const auto& change : changes)
    {
        if (!change.material || !unique.insert(change.material.get()).second)
            throw std::invalid_argument("Material state transaction needs distinct live instances");
        auto entry = std::make_unique<Entry>();
        entry->material = change.material;
        auto& material = *change.material;
        std::shared_lock<std::shared_mutex> lock(material.stateMutex);
        entry->parameters = material.parameters;
        entry->floats = entry->expectedFloats = material.floatParameters;
        entry->vec2 = entry->expectedVec2 = material.vec2Parameters;
        entry->vec3 = entry->expectedVec3 = material.vec3Parameters;
        entry->vec4 = entry->expectedVec4 = material.vec4Parameters;
        entry->textures = entry->expectedTextures = material.textures;
        entry->identities = material.textureBindingIdentities;
        entry->expectedBindingVersion = material.textureBindingVersion.load(std::memory_order_acquire);
        for (const auto& value : change.parameters)
        {
            const auto declared = entry->parameters.find(value.first);
            if (declared == entry->parameters.end())
                throw std::invalid_argument("Material transaction parameter is undeclared: " + value.first);
            std::visit([&](const auto& number) {
                using Value = std::decay_t<decltype(number)>;
                if constexpr (std::is_same_v<Value, float>)
                {
                    if (declared->second.type != ParamType::Float || !std::isfinite(number))
                        throw std::invalid_argument("Material transaction scalar type/value mismatch");
                    entry->floats.at(value.first) = number;
                }
                else
                {
                    constexpr ParamType type = std::is_same_v<Value, Eigen::Vector2f> ? ParamType::Vec2 :
                        std::is_same_v<Value, Eigen::Vector3f> ? ParamType::Vec3 : ParamType::Vec4;
                    if (declared->second.type != type || !number.allFinite())
                        throw std::invalid_argument("Material transaction vector type/value mismatch");
                    if constexpr (type == ParamType::Vec2) entry->vec2.at(value.first) = number;
                    if constexpr (type == ParamType::Vec3) entry->vec3.at(value.first) = number;
                    if constexpr (type == ParamType::Vec4) entry->vec4.at(value.first) = number;
                }
            }, value.second);
        }
        for (const auto& texture : change.textures)
        {
            const auto found = entry->textures.find(texture.first);
            if (found == entry->textures.end())
                throw std::invalid_argument("Material transaction texture is undeclared: " + texture.first);
            entry->textureChanged |= found->second != texture.second;
            found->second = texture.second;
        }
        entries.push_back(std::move(entry));
    }
}

MaterialInstanceStateBatch::~MaterialInstanceStateBatch() { CancelNoThrow(); }

bool MaterialInstanceStateBatch::TryLockNoThrow() noexcept
{
    if (committed)
        return false;
    if (locked)
        return true;
    try
    {
        for (auto& entry : entries)
        {
            entry->lock = std::unique_lock<std::shared_mutex>(entry->material->stateMutex, std::try_to_lock);
            if (!entry->lock.owns_lock())
            {
                CancelNoThrow();
                return false;
            }
            const auto& material = *entry->material;
            if (material.textureBindingVersion.load(std::memory_order_acquire) != entry->expectedBindingVersion ||
                material.textures != entry->expectedTextures || material.parameters.size() != entry->parameters.size() ||
                !SameValues(material.floatParameters, entry->expectedFloats) ||
                !SameValues(material.vec2Parameters, entry->expectedVec2) ||
                !SameValues(material.vec3Parameters, entry->expectedVec3) ||
                !SameValues(material.vec4Parameters, entry->expectedVec4))
            {
                CancelNoThrow();
                return false;
            }
        }
        locked = true;
        return true;
    }
    catch (...)
    {
        CancelNoThrow();
        return false;
    }
}

void MaterialInstanceStateBatch::CommitNoThrow() noexcept
{
    if (!locked || committed)
        return;
    for (auto& entry : entries)
    {
        auto& material = *entry->material;
        material.parameters.swap(entry->parameters);
        material.floatParameters.swap(entry->floats);
        material.vec2Parameters.swap(entry->vec2);
        material.vec3Parameters.swap(entry->vec3);
        material.vec4Parameters.swap(entry->vec4);
        material.textures.swap(entry->textures);
        material.textureBindingIdentities.swap(entry->identities);
        if (entry->textureChanged)
            material.textureBindingVersion.fetch_add(1, std::memory_order_release);
    }
    committed = true;
    CancelNoThrow();
}

void MaterialInstanceStateBatch::CancelNoThrow() noexcept
{
    for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
        if ((*entry)->lock.owns_lock())
            (*entry)->lock.unlock();
    locked = false;
}

uint64_t MaterialInstanceStateBatch::MetadataBytes() const noexcept
{
    uint64_t bytes = sizeof(*this) + entries.capacity() * sizeof(std::unique_ptr<Entry>);
    for (const auto& entry : entries)
        bytes += sizeof(Entry) + 1024 +
            (entry->parameters.size() + entry->floats.size() + entry->vec2.size() + entry->vec3.size() +
             entry->vec4.size() + entry->textures.size() + entry->identities.size() +
             entry->expectedFloats.size() + entry->expectedVec2.size() + entry->expectedVec3.size() +
             entry->expectedVec4.size() + entry->expectedTextures.size()) * 512;
    return bytes;
}
}
