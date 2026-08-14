#pragma once

// 文件职责：定义图形管线 descriptor set layout 的显式覆盖描述，供 Material Pass 继承完整
// Set 1 契约；不持有 Vulkan layout 句柄，也不负责从 Shader 反射推导默认布局。
// File responsibility: Defines explicit descriptor set layout overrides so material passes can inherit the
// complete Set 1 contract; it owns no Vulkan layout handles and does not derive reflection defaults.

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "commonFunction.h"
#include "shaderReflect.h"

// PipelineFactory 将该描述交给 GraphicsPipeline 创建布局；未标记 override 的 set 仍走反射结果。
// PipelineFactory passes this description to GraphicsPipeline; sets without overrides use reflection results.
struct GraphicsPipelineLayoutDesc
{
    std::array<std::vector<ShaderBinding>, MAX_DESCRIPTOR_SETS> setBindings;
    std::array<bool, MAX_DESCRIPTOR_SETS> overrideSets = {};

    bool HasSetOverride(uint32_t setIndex) const
    {
        return setIndex < overrideSets.size() && overrideSets[setIndex];
    }

    std::string GetNormalizedKey() const
    {
        std::ostringstream stream;
        for (uint32_t setIndex = 0; setIndex < MAX_DESCRIPTOR_SETS; ++setIndex)
        {
            stream << "set=" << setIndex << "|override=" << overrideSets[setIndex] << ":";
            if (!overrideSets[setIndex])
            {
                continue;
            }

            for (const ShaderBinding& binding : setBindings[setIndex])
            {
                stream << binding.binding << ","
                       << static_cast<uint32_t>(binding.type) << ","
                       << binding.descriptorCount << ","
                       << static_cast<uint32_t>(binding.stageFlags) << ","
                       << binding.memberCount << ","
                       << binding.size << ","
                       << binding.name << ";";
                for (std::size_t memberIndex = 0; memberIndex < binding.members.size(); ++memberIndex)
                {
                    stream << binding.members[memberIndex] << ",";
                    if (memberIndex < binding.memberNames.size())
                    {
                        stream << binding.memberNames[memberIndex];
                    }
                    if (memberIndex < binding.memberOffsets.size())
                    {
                        stream << ",offset=" << binding.memberOffsets[memberIndex];
                    }
                    if (memberIndex < binding.memberTypes.size())
                    {
                        stream << ",type=" << binding.memberTypes[memberIndex];
                    }
                    stream << ";";
                }
            }
        }
        return stream.str();
    }
};
