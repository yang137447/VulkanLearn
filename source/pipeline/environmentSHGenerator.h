#pragma once

#include <array>
#include <string>

#include <Eigen/Dense>

class EnvironmentSHGenerator
{
public:
    // 当前先直接从场景声明的 equirect HDR/EXR 做 CPU 投影，输出可直接用于漫反射 IBL 的 SH9 系数。
    static std::array<Eigen::Vector4f, 9> Generate(const std::string& hdrPath);
};
