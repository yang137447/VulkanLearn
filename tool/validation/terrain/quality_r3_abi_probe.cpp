#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

struct ProbeCounts
{
    uint32_t counters[7];
    bool complete;
};

extern ProbeCounts R3Recompute(const uint32_t*, size_t)
    asm("_ZN2VL17TerrainQualityAbi9RecomputeEPKjy");

int main(int argumentCount, char** arguments)
{
    const bool expectV2 = argumentCount == 2 && std::string(arguments[1]) == "v2";
    std::vector<uint32_t> words(1619200 / 4);
    words[0] = 0x31515654;
    words[1] = 2;
    words[2] = 16384;
    words[14] = 1;
    words[16] = words[17] = 128;
    words[18] = 1;
    bool newRejected = false;
    try
    {
        R3Recompute(words.data(), 1619200);
    }
    catch (const std::exception& error)
    {
        newRejected = true;
        std::cout << "R3 v2 rejection: " << error.what() << '\n';
    }
    words[1] = 1;
    bool legacyRejected = false;
    bool legacyComplete = false;
    try
    {
        legacyComplete = R3Recompute(words.data(), 1606912).complete;
    }
    catch (const std::exception&)
    {
        legacyRejected = true;
    }
    std::cout << "R3 ABI-only empty-domain control: expectedVersion=" << (expectV2 ? 2 : 1)
        << " v1Complete=" << legacyComplete << " v1Rejected=" << legacyRejected
        << " v2Rejected=" << newRejected << '\n';
    return expectV2 ? (legacyRejected && !newRejected ? 0 : 1) :
        (newRejected && legacyComplete ? 0 : 1);
}
