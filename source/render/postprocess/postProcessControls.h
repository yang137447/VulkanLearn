#pragma once

#include <memory>
#include <string>

#include <Eigen/Dense>

class MaterialInstance;

namespace VL
{

std::shared_ptr<MaterialInstance> GetPassMaterialInstance(const char* passName);
bool SetPassVectorComponent(
    const char* passName,
    const char* parameterName,
    int componentIndex,
    float value,
    const char* missingPassMessage,
    std::string& outMessage);
bool SetPassVector(
    const char* passName,
    const char* parameterName,
    const Eigen::Vector4f& value,
    std::string& outMessage);

// Owns the CPU-side tone mapping / bloom values and the pass-material plumbing
// that writes them into the render graph's pass material instances.
class PostProcessControls
{
public:
    bool SetToneMappingMode(int mode, std::string& outMessage);
    bool SetBloomStrength(float value, std::string& outMessage);
    bool SetBloomThreshold(float value, std::string& outMessage);
    bool SetBloomKnee(float value, std::string& outMessage);
    bool SetBloomClamp(float value, std::string& outMessage);

    int GetToneMappingMode() const noexcept { return toneMappingMode; }
    float GetBloomStrength() const noexcept { return bloomStrength; }
    float GetBloomThreshold() const noexcept { return bloomThreshold; }
    float GetBloomKnee() const noexcept { return bloomKnee; }
    float GetBloomClamp() const noexcept { return bloomClamp; }

private:
    int toneMappingMode = 3;
    float bloomStrength = 0.08f;
    float bloomThreshold = 1.0f;
    float bloomKnee = 0.5f;
    float bloomClamp = 12.0f;
};

} // namespace VL
