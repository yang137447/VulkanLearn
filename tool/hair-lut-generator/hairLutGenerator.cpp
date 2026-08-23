#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "render/hair/hairAssets.h"
#include "render/hair/hairConventions.h"
#include "render/hair/hairLutCoordinates.h"

namespace
{

void PrintUsage()
{
    std::cout <<
        "Usage: hair_lut_generator --validate <metadata.json>\n"
        "       hair_lut_generator --dispatch <metadata.json>\n";
}

VL::HairAzimuthalLutMetadata LoadMetadata(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open Hair LUT metadata: " + path);
    }
    nlohmann::json json;
    file >> json;
    return VL::ParseHairAzimuthalLutMetadata(json, path);
}

void PrintDispatchPlan(const VL::HairAzimuthalLutMetadata& metadata)
{
    const uint32_t groupsX = (metadata.width + 7u) / 8u;
    const uint32_t groupsY = (metadata.height + 7u) / 8u;
    const VL::Hair::HairLutUv center =
        VL::Hair::EncodeHairAzimuthalLutUv(0.0f, 0.0f, 0.0f);
    const VL::Hair::HairLutUv upper =
        VL::Hair::EncodeHairAzimuthalLutUv(
            VL::Hair::HairPi,
            VL::Hair::HairHalfPi,
            1.0f);

    // 这里仅打印 GPU dispatch 的 frozen contract；host 工具不创建 Vulkan
    // image、不写生产 texel，也不把 CPU reference 结果伪装成 LUT 输出。
    std::cout << nlohmann::json{
        {"width", metadata.width},
        {"height", metadata.height},
        {"layers", metadata.layers},
        {"localSize", {8, 8, 1}},
        {"groups", {groupsX, groupsY, metadata.layers}},
        {"sampleContract", {
            {"centerUv", {center.u, center.v}},
            {"upperUv", {upper.u, upper.v}}}}}
                  .dump()
              << std::endl;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3 ||
        (std::string(argv[1]) != "--validate" &&
         std::string(argv[1]) != "--dispatch"))
    {
        PrintUsage();
        return 1;
    }

    try
    {
        const VL::HairAzimuthalLutMetadata metadata = LoadMetadata(argv[2]);
        if (std::string(argv[1]) == "--dispatch")
        {
            PrintDispatchPlan(metadata);
        }
        else
        {
            std::cout << "Hair LUT metadata valid: " << argv[2] << std::endl;
        }
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Hair LUT host validation failed: "
                  << exception.what() << std::endl;
        return 2;
    }
}
