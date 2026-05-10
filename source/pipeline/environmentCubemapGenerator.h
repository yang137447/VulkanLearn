#pragma once

#include <cstdint>
#include <string>

class PipelineFactory;

class EnvironmentCubemapGenerator
{
public:
    // 把场景里声明的经纬度 HDR/EXR 环境图转换成 6 面 cubemap 调试输出。
    static void Generate(const std::string& hdrPath, uint32_t cubeSize, PipelineFactory& pipelineFactory);
};
