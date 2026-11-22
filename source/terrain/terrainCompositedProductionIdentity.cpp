#include "terrain/terrainCompositedProductionIdentity.h"

#include <algorithm>
#include <set>
#include <string>

namespace VL
{
namespace
{

// 生产闭包只由这两个 compute 入口构成；固定数量也用于拒绝缺失/多余 kernel。
bool IsProductionComputeKernel(const std::string& normalizedKey)
{
    return normalizedKey.find("terrain/terrainCompositedBakeWorld.comp") != std::string::npos ||
        normalizedKey.find("terrain/terrainCompositedBakeWorldPadding.comp") != std::string::npos;
}

}

bool MatchesTerrainCompositedGeneratorClosure(
    const TerrainCompositedProductionClosure& expected,
    const ShaderBuildManifestSnapshot& actual)
{
    std::set<std::string> expectedSources;
    expectedSources.insert(expected.Object(expected.generator.computeSource).digest.ToHex());
    for (const std::string& id : expected.generator.includeSources)
        expectedSources.insert(expected.Object(id).digest.ToHex());

    // publication 在冻结时把每个 kernel 的 runtime 与 debug 输出都记录为 ShaderArtifact。
    // 这里只把它当作"允许的已发布摘要集合"，真正的身份由下面的 runtime 集合决定。
    std::set<std::string> publishedArtifacts;
    for (const std::string& id : expected.generator.compiledArtifacts)
        publishedArtifacts.insert(expected.Object(id).digest.ToHex());

    std::set<std::string> actualSources;
    std::set<std::string> runtimeArtifacts;
    uint32_t kernels = 0;
    for (const auto& entry : actual.artifacts)
    {
        const ShaderBuildArtifactRecord& artifact = entry.second;
        if (!IsProductionComputeKernel(artifact.normalizedKey))
            continue;
        ++kernels;
        for (const ShaderBuildSourceRecord& source : artifact.primarySources)
            actualSources.insert(source.digest);
        for (const ShaderDependencyRecord& dependency : artifact.dependencies)
            actualSources.insert(dependency.digest);
        // 只取 runtime* 角色：debug reflection 产物不是运行时 compute 闭包，
        // 它的重编译（例如只改换行）不能改变生产身份。
        for (const auto& output : artifact.outputs)
            if (IsShaderRuntimeOutputRole(output.first))
                runtimeArtifacts.insert(output.second.digest);
    }

    // 源码/include 闭包与 kernel 数量保持原有的严格比较，不放宽。
    if (kernels != 2 || actualSources != expectedSources ||
        runtimeArtifacts.size() != kernels)
        return false;

    // 每个 kernel 恰好一份 runtime SPIR-V，且必须逐字出现在 publication 记录的
    // artifact 摘要中。publication 里另外那份 debug 摘要允许随反射重编译变化；
    // 这正是 P4 hotreload 阻塞点，而不是运行时 SPIR-V 真的变了。
    return std::includes(publishedArtifacts.begin(), publishedArtifacts.end(),
        runtimeArtifacts.begin(), runtimeArtifacts.end());
}

}
