#include "material/materialDescriptorSchema.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include "commonFunction.h"
#include "material/materialAssetUtils.h"

namespace VL
{
namespace
{

uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

struct ParameterTypeLayout
{
    uint32_t size;
    uint32_t alignment;
};

ParameterTypeLayout GetParameterTypeLayout(std::string_view type)
{
    if (type == "float")
    {
        return {4u, 4u};
    }
    if (type == "vec2")
    {
        return {8u, 8u};
    }
    if (type == "vec3")
    {
        return {12u, 16u};
    }
    if (type == "vec4")
    {
        return {16u, 16u};
    }
    throw std::runtime_error("Unsupported material parameter type: " + std::string(type));
}

uint32_t ParameterTypeRank(std::string_view type)
{
    if (type == "vec4") return 0;
    if (type == "vec3") return 1;
    if (type == "vec2") return 2;
    return 3;
}

bool CompareParameterSchemaEntries(
    const MaterialParameterSchemaEntry& lhs,
    const MaterialParameterSchemaEntry& rhs)
{
    const uint32_t lhsRank = ParameterTypeRank(lhs.glslType);
    const uint32_t rhsRank = ParameterTypeRank(rhs.glslType);
    return lhsRank != rhsRank ? lhsRank < rhsRank : lhs.name < rhs.name;
}

bool BindingLayoutMatches(const ShaderBinding& schema, const ShaderBinding& shader)
{
    return HasSameShaderBindingLayout(schema, shader) &&
        (shader.stageFlags & ~schema.stageFlags) == vk::ShaderStageFlags{};
}

std::string DescribeBinding(const ShaderBinding& binding)
{
    std::ostringstream stream;
    stream << "name=" << binding.name
           << ", type=" << static_cast<uint32_t>(binding.type)
           << ", stages=" << static_cast<uint32_t>(binding.stageFlags)
           << ", size=" << binding.size
           << ", memberCount=" << binding.memberCount
           << ", members=[";
    for (size_t index = 0; index < binding.memberNames.size(); ++index)
    {
        if (index > 0) stream << "; ";
        stream << binding.memberNames[index];
        if (index < binding.members.size()) stream << ":size=" << binding.members[index];
        if (index < binding.memberOffsets.size()) stream << ":offset=" << binding.memberOffsets[index];
    }
    stream << "]";
    return stream.str();
}

} // namespace

MaterialDescriptorSchema MaterialDescriptorSchema::Build(
    const nlohmann::json& materialJson,
    std::string_view materialPath)
{
    MaterialDescriptorSchema schema;
    schema.sourcePath = std::string(materialPath);

    for (const auto& [name, parameterJson] : materialJson.at("parameters").items())
    {
        MaterialParameterSchemaEntry entry;
        entry.name = name;
        entry.glslType = parameterJson.at("type").get<std::string>();
        entry.size = GetParameterTypeLayout(entry.glslType).size;
        schema.parameters.push_back(std::move(entry));
    }

    std::sort(
        schema.parameters.begin(),
        schema.parameters.end(),
        CompareParameterSchemaEntries);

    if (!schema.parameters.empty())
    {
        ShaderBinding uboBinding;
        uboBinding.set = MaterialSetIndex;
        uboBinding.binding = 0;
        uboBinding.type = vk::DescriptorType::eUniformBuffer;
        uboBinding.stageFlags =
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
        // GLSL block沒有 instance name；SPIR-V descriptor binding name 因而為空。
        // Block type 名 UBOMIParamters 只用於生成 GLSL，不是 runtime resource name。
        uboBinding.name.clear();

        uint32_t offset = 0;
        for (MaterialParameterSchemaEntry& parameter : schema.parameters)
        {
            const ParameterTypeLayout typeLayout = GetParameterTypeLayout(parameter.glslType);
            offset = AlignUp(offset, typeLayout.alignment);
            parameter.offset = offset;
            uboBinding.members.push_back(typeLayout.size);
            uboBinding.memberOffsets.push_back(offset);
            uboBinding.memberNames.push_back(parameter.name);
            offset += typeLayout.size;
        }
        uboBinding.memberCount = static_cast<uint32_t>(schema.parameters.size());
        uboBinding.size = AlignUp(offset, 16u);
        schema.setBindings.push_back(std::move(uboBinding));
    }

    uint32_t textureBinding = 1;
    for (const auto& [name, textureJson] : materialJson.at("textures").items())
    {
        MaterialTextureSchemaEntry texture;
        texture.name = name;
        texture.glslType = "sampler2D";
        texture.binding = textureBinding++;
        schema.textures.push_back(texture);

        ShaderBinding textureBindingDesc;
        textureBindingDesc.set = MaterialSetIndex;
        textureBindingDesc.binding = texture.binding;
        textureBindingDesc.type = vk::DescriptorType::eCombinedImageSampler;
        textureBindingDesc.stageFlags =
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
        textureBindingDesc.memberCount = 0;
        textureBindingDesc.size = 0;
        textureBindingDesc.name = texture.name;
        schema.setBindings.push_back(std::move(textureBindingDesc));
    }

    return schema;
}

const ShaderBinding* MaterialDescriptorSchema::FindBinding(uint32_t bindingIndex) const
{
    for (const ShaderBinding& binding : setBindings)
    {
        if (binding.binding == bindingIndex)
        {
            return &binding;
        }
    }
    return nullptr;
}

void MaterialDescriptorSchema::ValidateShaderBindings(
    const std::vector<ShaderBinding>& shaderBindings,
    std::string_view shaderDisplayName) const
{
    for (const ShaderBinding& shaderBinding : shaderBindings)
    {
        if (shaderBinding.set != MaterialSetIndex)
        {
            continue;
        }

        const ShaderBinding* schemaBinding = FindBinding(shaderBinding.binding);
        if (schemaBinding == nullptr || !BindingLayoutMatches(*schemaBinding, shaderBinding))
        {
            throw std::runtime_error(
                "Shader Set 1 binding is incompatible with material schema: " +
                std::string(shaderDisplayName) + " Set 1 Binding " +
                std::to_string(shaderBinding.binding) + " from " + sourcePath +
                ". Expected {" +
                (schemaBinding ? DescribeBinding(*schemaBinding) : std::string("missing")) +
                "}, reflected {" + DescribeBinding(shaderBinding) + "}");
        }
    }
}

void MaterialDescriptorSchema::ValidateInstanceValues(
    const nlohmann::json& parametersJson,
    const nlohmann::json& texturesJson,
    const std::vector<ShaderBinding>& activeShaderBindings,
    std::string_view materialInstancePath) const
{
    if (!parametersJson.is_object() || parametersJson.size() != parameters.size())
    {
        throw std::runtime_error(
            "Material instance parameter set does not match schema: " + std::string(materialInstancePath));
    }
    for (const MaterialParameterSchemaEntry& parameter : parameters)
    {
        if (!parametersJson.contains(parameter.name) ||
            !MaterialAssetUtils::MaterialParameterValueMatchesType(
                parametersJson.at(parameter.name),
                parameter.glslType))
        {
            throw std::runtime_error(
                "Material instance parameter does not match schema: " + parameter.name + " in " +
                std::string(materialInstancePath));
        }
    }

    for (const ShaderBinding& binding : activeShaderBindings)
    {
        if (binding.set == MaterialSetIndex &&
            binding.type == vk::DescriptorType::eCombinedImageSampler &&
            (!texturesJson.is_object() || !texturesJson.contains(binding.name)))
        {
            throw std::runtime_error(
                "Missing texture used by selected material pass: " + binding.name + " in " +
                std::string(materialInstancePath));
        }
    }
}

} // namespace VL
