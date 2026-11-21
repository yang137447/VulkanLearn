#include "hairClumpCoupled.h"
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3 || (std::string(argv[1]) != "--verify" && std::string(argv[1]) != "--startup"))
            throw std::runtime_error("usage: vl_hair_clump_coupled --verify|--startup NEW_DIRECTORY");
        if (!std::filesystem::create_directories(argv[2]))
            throw std::runtime_error("output directory already exists");
        return std::string(argv[1]) == "--verify" ? C2::Verify(argv[2]) : C2::Startup(argv[2]);
    }
    catch (const std::exception& error)
    {
        std::cerr << "C2 E FAIL: " << error.what() << '\n';
        return 2;
    }
}
