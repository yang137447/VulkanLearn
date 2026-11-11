#pragma once

#include "hair/tfxLoader.h"

namespace VL::Hair
{
// 静态 guide 的 CPU 派生数据，供 ribbon 顶点展开和后续模拟上传消费。
// 每点对应左右两个逻辑 ribbon 顶点，尚不创建 Vulkan buffer，也不执行相机朝向展开。
struct StrandGeometry
{
    std::vector<std::array<float, 4>> tangents;
    // 每点存到下一点的距离；tip 为 0 占位，不连接下一根 guide。
    std::vector<float> restLengths;
    std::vector<std::uint32_t> ribbonIndices;
    float maxRestLength = 0;
};

// 输入必须来自成功的 ParseTfx/LoadTfx；这里只校验派生几何的新约束：
// 线段长度须非零且可用 float 表示，内点不得发生无法定义切线的精确折返。
RuntimeResult<StrandGeometry> PrepareStrandGeometry(
    const TfxAsset& asset, const std::string& sourcePath = {});
}
