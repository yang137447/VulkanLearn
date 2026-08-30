#pragma once

// 文件职责：定义由 M_ 资产推导出的 Material Set 1 完整描述符契约，统一参数布局、
// 纹理 binding 和逐 Pass 反射校验；不创建 Vulkan descriptor 或上传 MI 数据。
// File responsibility: Defines the complete Material Set 1 descriptor contract derived from an M_ asset,
// unifying parameter layout, texture bindings, and per-pass reflection validation without creating descriptors.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "shaderReflect.h"

namespace VL
{

// 一个 packed 参数分量的 authoring 元数据；不参与 UBO layout 或 shader ABI。
struct MaterialParameterChannelSchemaEntry
{
    std::string name;
    std::string description;
    std::optional<float> min;
    std::optional<float> max;
};

// 一个材质 UBO 参数的 std140 布局信息。
// std140 layout information for one material UBO parameter.
struct MaterialParameterSchemaEntry
{
    std::string name;
    std::string glslType;
    // 按 x/y/z/w 顺序保留 M_ 通道元数据；为空表示资产未声明。
    std::vector<MaterialParameterChannelSchemaEntry> channels;
    uint32_t size = 0;
    uint32_t offset = 0;
};

// 一张材质纹理在 Material Set 1 中的声明信息。
// Declaration of one material texture in Material Set 1.
struct MaterialTextureSchemaEntry
{
    std::string name;
    std::string glslType;
    uint32_t binding = 0;
};

// 从一份 M_ 定义构建 Material Set 1 的唯一契约。
// GLSL 生成、管线布局、MI UBO 打包和逐 Pass 反射校验共用此对象，保证 binding 与偏移一致。
// The single Material Set 1 contract derived from an M_ definition.
// GLSL generation, pipeline layout, MI UBO packing, and per-pass validation share this object.
class MaterialDescriptorSchema
{
public:
    // 输入必须已经通过 MaterialAssetValidator；此处只推导确定性的 std140 和 descriptor 布局。
    // Input must pass MaterialAssetValidator first; this step only derives deterministic layout data.
    static MaterialDescriptorSchema Build(
        const nlohmann::json& materialJson,
        std::string_view materialPath);

    const std::vector<MaterialParameterSchemaEntry>& GetParameters() const { return parameters; }
    const std::vector<MaterialTextureSchemaEntry>& GetTextures() const { return textures; }
    const std::vector<ShaderBinding>& GetSetBindings() const { return setBindings; }
    const ShaderBinding* FindBinding(uint32_t bindingIndex) const;

    // 反射只描述当前 Pass 实际使用的资源，允许缺少未使用项；出现的 Set 1 binding 必须匹配完整契约。
    // Reflection may omit unused entries, but every reflected Set 1 binding must match the full contract.
    void ValidateShaderBindings(
        const std::vector<ShaderBinding>& shaderBindings,
        std::string_view shaderDisplayName) const;

    // 按 schema 校验 MI 有效值；只有被选中材质 Pass 实际使用的纹理才要求 MI 提供。
    // Validates effective MI values; only textures used by selected material passes are required.
    void ValidateInstanceValues(
        const nlohmann::json& parametersJson,
        const nlohmann::json& texturesJson,
        const std::vector<ShaderBinding>& activeShaderBindings,
        std::string_view materialInstancePath) const;

private:
    std::string sourcePath;
    std::vector<MaterialParameterSchemaEntry> parameters;
    std::vector<MaterialTextureSchemaEntry> textures;
    std::vector<ShaderBinding> setBindings;
};

} // namespace VL
