#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "hair/tressfxFollowBinding.h"

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3)
            throw std::runtime_error("expected source.tfx output-prefix");
        std::ifstream input(argv[1], std::ios::binary);
        float version = 0;
        uint32_t count = 0, points = 0, offset = 0;
        input.read(reinterpret_cast<char*>(&version), 4);
        input.read(reinterpret_cast<char*>(&count), 4);
        input.read(reinterpret_cast<char*>(&points), 4);
        input.read(reinterpret_cast<char*>(&offset), 4);
        if (!input || version < 4 || count != 40334 || points != 32)
            throw std::runtime_error("unexpected actual groom identity");
        input.seekg(offset);
        std::vector<Eigen::Vector3f> rest;
        std::vector<Eigen::Vector3f> roots;
        rest.reserve(static_cast<size_t>(count) * points);
        for (uint32_t guide = 0; guide < count; ++guide)
            for (uint32_t point = 0; point < points; ++point)
            {
                float packed[4];
                input.read(reinterpret_cast<char*>(packed), sizeof(packed));
                rest.emplace_back(packed[0], packed[1], packed[2]);
                if (point == 0)
                    roots.push_back(rest.back());
            }
        if (!input)
            throw std::runtime_error("truncated source positions");
        const auto binding = VL::Hair::BuildTressfxFollowBinding(roots, rest, points, 2000);
        const std::string prefix = argv[2];
        const auto write = [&](const char* suffix, const void* data, size_t bytes)
        {
            std::ofstream output(prefix + suffix, std::ios::binary);
            output.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
            if (!output)
                throw std::runtime_error("binding export failed");
        };
        write(".selected.bin", binding.simulationGuideIndices.data(), binding.simulationGuideIndices.size() * 4);
        write(".parents.bin", binding.followGuideIndex.data(), binding.followGuideIndex.size() * 4);
        write(".offsets.bin", binding.offsets.data(), binding.offsets.size() * 16);
        write(".references.bin", binding.frameReferences.data(), binding.frameReferences.size() * 16);
        std::cout << "actualCPUbinding guides=" << count << " selected=2000 candidates=16\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
