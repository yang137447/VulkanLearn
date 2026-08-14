#include "shader/shaderAbiSignature.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>
#include <utility>

#include "shader/build/contentHash.h"

namespace VL
{
namespace
{

void AddMembers(
    CanonicalFieldHasher& hasher,
    const std::string& prefix,
    const std::vector<ShaderAbiMember>& members)
{
    hasher.AddUInt32(
        prefix + ".memberCount",
        static_cast<uint32_t>(members.size()));
    for (size_t memberIndex = 0; memberIndex < members.size(); ++memberIndex)
    {
        const std::string memberPrefix =
            prefix + ".member." + std::to_string(memberIndex);
        hasher.AddString(memberPrefix + ".name", members[memberIndex].name);
        hasher.AddUInt32(memberPrefix + ".offset", members[memberIndex].offset);
        hasher.AddUInt32(memberPrefix + ".size", members[memberIndex].size);
        hasher.AddString(memberPrefix + ".type", members[memberIndex].type);
    }
}

std::string DescribeDescriptor(const ShaderAbiDescriptor& descriptor)
{
    std::ostringstream stream;
    stream << "Set " << descriptor.set << " Binding " << descriptor.binding
           << " {type=" << static_cast<uint32_t>(descriptor.type)
           << ", count=" << descriptor.count
           << ", stages=" << static_cast<uint32_t>(descriptor.stageFlags)
           << ", blockSize=" << descriptor.blockSize
           << ", name=" << descriptor.name << "}";
    return stream.str();
}

std::string DescribeInterface(
    const char* kind,
    const ShaderAbiInterfaceVariable& variable)
{
    std::ostringstream stream;
    stream << kind << " location " << variable.location
           << " {component=" << variable.component
           << ", format=" << static_cast<uint32_t>(variable.format)
           << ", type=" << variable.type
           << ", name=" << variable.name << "}";
    return stream.str();
}

template <typename Entry, typename KeyBuilder, typename DescribeEntry>
void AppendMapDifferences(
    const std::vector<Entry>& current,
    const std::vector<Entry>& candidate,
    KeyBuilder buildKey,
    DescribeEntry describeEntry,
    const char* kind,
    std::vector<std::string>& differences)
{
    using Key = decltype(buildKey(std::declval<Entry>()));
    std::map<Key, Entry> currentByKey;
    std::map<Key, Entry> candidateByKey;
    for (const Entry& entry : current)
    {
        currentByKey.emplace(buildKey(entry), entry);
    }
    for (const Entry& entry : candidate)
    {
        candidateByKey.emplace(buildKey(entry), entry);
    }

    for (const auto& [key, currentEntry] : currentByKey)
    {
        const auto candidateIt = candidateByKey.find(key);
        if (candidateIt == candidateByKey.end())
        {
            differences.push_back(
                std::string(kind) + " removed: " + describeEntry(currentEntry));
            continue;
        }
        if (!(currentEntry == candidateIt->second))
        {
            differences.push_back(
                std::string(kind) + " changed: " + describeEntry(currentEntry) +
                " -> " + describeEntry(candidateIt->second));
        }
    }

    for (const auto& [key, candidateEntry] : candidateByKey)
    {
        if (currentByKey.find(key) == currentByKey.end())
        {
            differences.push_back(
                std::string(kind) + " added: " + describeEntry(candidateEntry));
        }
    }
}

} // namespace

bool ShaderAbiMember::operator==(const ShaderAbiMember& other) const
{
    return name == other.name &&
        offset == other.offset &&
        size == other.size &&
        type == other.type;
}

bool ShaderAbiDescriptor::operator==(const ShaderAbiDescriptor& other) const
{
    return set == other.set &&
        binding == other.binding &&
        type == other.type &&
        count == other.count &&
        stageFlags == other.stageFlags &&
        name == other.name &&
        blockSize == other.blockSize &&
        members == other.members;
}

bool ShaderAbiPushConstant::operator==(const ShaderAbiPushConstant& other) const
{
    return offset == other.offset &&
        size == other.size &&
        stageFlags == other.stageFlags &&
        members == other.members;
}

bool ShaderAbiInterfaceVariable::operator==(
    const ShaderAbiInterfaceVariable& other) const
{
    return location == other.location &&
        component == other.component &&
        format == other.format &&
        type == other.type &&
        name == other.name;
}

bool ShaderAbiSpecializationConstant::operator==(
    const ShaderAbiSpecializationConstant& other) const
{
    return constantId == other.constantId &&
        type == other.type &&
        name == other.name;
}

void ShaderAbiSignature::Normalize()
{
    std::sort(
        descriptors.begin(),
        descriptors.end(),
        [](const ShaderAbiDescriptor& lhs, const ShaderAbiDescriptor& rhs)
        {
            return std::tie(lhs.set, lhs.binding) <
                std::tie(rhs.set, rhs.binding);
        });
    std::sort(
        pushConstants.begin(),
        pushConstants.end(),
        [](const ShaderAbiPushConstant& lhs, const ShaderAbiPushConstant& rhs)
        {
            return std::tie(lhs.offset, lhs.size) <
                std::tie(rhs.offset, rhs.size);
        });
    const auto compareInterface = [](
        const ShaderAbiInterfaceVariable& lhs,
        const ShaderAbiInterfaceVariable& rhs)
    {
        return std::tie(lhs.location, lhs.component) <
            std::tie(rhs.location, rhs.component);
    };
    std::sort(vertexInputs.begin(), vertexInputs.end(), compareInterface);
    std::sort(fragmentOutputs.begin(), fragmentOutputs.end(), compareInterface);
    std::sort(
        specializationConstants.begin(),
        specializationConstants.end(),
        [](const ShaderAbiSpecializationConstant& lhs,
           const ShaderAbiSpecializationConstant& rhs)
        {
            return lhs.constantId < rhs.constantId;
        });
}

std::string ShaderAbiSignature::GetFingerprint() const
{
    ShaderAbiSignature normalized = *this;
    normalized.Normalize();

    CanonicalFieldHasher hasher("ShaderAbiSignatureV1");
    hasher.AddUInt32(
        "descriptorCount",
        static_cast<uint32_t>(normalized.descriptors.size()));
    for (size_t index = 0; index < normalized.descriptors.size(); ++index)
    {
        const ShaderAbiDescriptor& descriptor = normalized.descriptors[index];
        const std::string prefix = "descriptor." + std::to_string(index);
        hasher.AddUInt32(prefix + ".set", descriptor.set);
        hasher.AddUInt32(prefix + ".binding", descriptor.binding);
        hasher.AddUInt32(
            prefix + ".type",
            static_cast<uint32_t>(descriptor.type));
        hasher.AddUInt32(prefix + ".count", descriptor.count);
        hasher.AddUInt32(
            prefix + ".stages",
            static_cast<uint32_t>(descriptor.stageFlags));
        hasher.AddString(prefix + ".name", descriptor.name);
        hasher.AddUInt32(prefix + ".blockSize", descriptor.blockSize);
        AddMembers(hasher, prefix, descriptor.members);
    }

    hasher.AddUInt32(
        "pushConstantCount",
        static_cast<uint32_t>(normalized.pushConstants.size()));
    for (size_t index = 0; index < normalized.pushConstants.size(); ++index)
    {
        const ShaderAbiPushConstant& pushConstant =
            normalized.pushConstants[index];
        const std::string prefix = "pushConstant." + std::to_string(index);
        hasher.AddUInt32(prefix + ".offset", pushConstant.offset);
        hasher.AddUInt32(prefix + ".size", pushConstant.size);
        hasher.AddUInt32(
            prefix + ".stages",
            static_cast<uint32_t>(pushConstant.stageFlags));
        AddMembers(hasher, prefix, pushConstant.members);
    }

    const auto addInterfaces = [&hasher](
        const char* category,
        const std::vector<ShaderAbiInterfaceVariable>& variables)
    {
        hasher.AddUInt32(
            std::string(category) + "Count",
            static_cast<uint32_t>(variables.size()));
        for (size_t index = 0; index < variables.size(); ++index)
        {
            const std::string prefix =
                std::string(category) + "." + std::to_string(index);
            hasher.AddUInt32(prefix + ".location", variables[index].location);
            hasher.AddUInt32(prefix + ".component", variables[index].component);
            hasher.AddUInt32(
                prefix + ".format",
                static_cast<uint32_t>(variables[index].format));
            hasher.AddString(prefix + ".type", variables[index].type);
            hasher.AddString(prefix + ".name", variables[index].name);
        }
    };
    addInterfaces("vertexInput", normalized.vertexInputs);
    addInterfaces("fragmentOutput", normalized.fragmentOutputs);

    hasher.AddUInt32(
        "specializationConstantCount",
        static_cast<uint32_t>(normalized.specializationConstants.size()));
    for (size_t index = 0;
         index < normalized.specializationConstants.size();
         ++index)
    {
        const ShaderAbiSpecializationConstant& constant =
            normalized.specializationConstants[index];
        const std::string prefix =
            "specializationConstant." + std::to_string(index);
        hasher.AddUInt32(prefix + ".id", constant.constantId);
        hasher.AddString(prefix + ".type", constant.type);
        hasher.AddString(prefix + ".name", constant.name);
    }
    hasher.AddBool(
        "workgroupSize.present",
        normalized.workgroupSize.present);
    hasher.AddUInt32("workgroupSize.x", normalized.workgroupSize.x);
    hasher.AddUInt32("workgroupSize.y", normalized.workgroupSize.y);
    hasher.AddUInt32("workgroupSize.z", normalized.workgroupSize.z);
    return hasher.Finalize().ToHex();
}

std::vector<std::string> ShaderAbiSignature::DescribeDifferences(
    const ShaderAbiSignature& candidate) const
{
    ShaderAbiSignature currentNormalized = *this;
    ShaderAbiSignature candidateNormalized = candidate;
    currentNormalized.Normalize();
    candidateNormalized.Normalize();

    std::vector<std::string> differences;
    AppendMapDifferences(
        currentNormalized.descriptors,
        candidateNormalized.descriptors,
        [](const ShaderAbiDescriptor& descriptor)
        {
            return std::make_pair(descriptor.set, descriptor.binding);
        },
        DescribeDescriptor,
        "Descriptor",
        differences);
    AppendMapDifferences(
        currentNormalized.pushConstants,
        candidateNormalized.pushConstants,
        [](const ShaderAbiPushConstant& pushConstant)
        {
            return std::make_pair(pushConstant.offset, pushConstant.size);
        },
        [](const ShaderAbiPushConstant& pushConstant)
        {
            return "offset=" + std::to_string(pushConstant.offset) +
                ", size=" + std::to_string(pushConstant.size) +
                ", stages=" +
                std::to_string(static_cast<uint32_t>(pushConstant.stageFlags));
        },
        "Push constant",
        differences);
    AppendMapDifferences(
        currentNormalized.vertexInputs,
        candidateNormalized.vertexInputs,
        [](const ShaderAbiInterfaceVariable& variable)
        {
            return std::make_pair(variable.location, variable.component);
        },
        [](const ShaderAbiInterfaceVariable& variable)
        {
            return DescribeInterface("Vertex input", variable);
        },
        "Vertex input",
        differences);
    AppendMapDifferences(
        currentNormalized.fragmentOutputs,
        candidateNormalized.fragmentOutputs,
        [](const ShaderAbiInterfaceVariable& variable)
        {
            return std::make_pair(variable.location, variable.component);
        },
        [](const ShaderAbiInterfaceVariable& variable)
        {
            return DescribeInterface("Fragment output", variable);
        },
        "Fragment output",
        differences);
    AppendMapDifferences(
        currentNormalized.specializationConstants,
        candidateNormalized.specializationConstants,
        [](const ShaderAbiSpecializationConstant& constant)
        {
            return constant.constantId;
        },
        [](const ShaderAbiSpecializationConstant& constant)
        {
            return "id=" + std::to_string(constant.constantId) +
                ", type=" + constant.type + ", name=" + constant.name;
        },
        "Specialization constant",
        differences);
    if (!(currentNormalized.workgroupSize ==
          candidateNormalized.workgroupSize))
    {
        const auto describeWorkgroup = [](const ShaderAbiWorkgroupSize& size)
        {
            return size.present
                ? "local_size(" + std::to_string(size.x) + ", " +
                    std::to_string(size.y) + ", " +
                    std::to_string(size.z) + ")"
                : "no compute local size";
        };
        differences.push_back(
            "Workgroup size changed: " +
            describeWorkgroup(currentNormalized.workgroupSize) +
            " -> " +
            describeWorkgroup(candidateNormalized.workgroupSize));
    }
    return differences;
}

bool ShaderAbiSignature::operator==(const ShaderAbiSignature& other) const
{
    ShaderAbiSignature lhs = *this;
    ShaderAbiSignature rhs = other;
    lhs.Normalize();
    rhs.Normalize();
    return lhs.descriptors == rhs.descriptors &&
        lhs.pushConstants == rhs.pushConstants &&
        lhs.vertexInputs == rhs.vertexInputs &&
        lhs.fragmentOutputs == rhs.fragmentOutputs &&
        lhs.specializationConstants == rhs.specializationConstants &&
        lhs.workgroupSize == rhs.workgroupSize;
}

} // namespace VL
