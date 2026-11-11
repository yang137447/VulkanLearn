#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "particles/particleAsset.h"

namespace VL
{
namespace ParticleAssetDetail
{

// JSON 值读取 helper。错误文本与拆分前逐字一致，调用方只负责组织字段。
Eigen::Vector3f ReadVec3(
    const nlohmann::json& object,
    const char* field,
    const Eigen::Vector3f& fallback);
Eigen::Vector2f ReadVec2(
    const nlohmann::json& object,
    const char* field,
    const Eigen::Vector2f& fallback);
Eigen::Vector2f ReadVec2Tangent(
    const nlohmann::json& object,
    const char* field);
Eigen::Vector4f ReadVec4(
    const nlohmann::json& object,
    const char* field,
    const Eigen::Vector4f& fallback);
Eigen::Vector4f ReadVec4Tangent(
    const nlohmann::json& object,
    const char* field);
float ReadFiniteFloat(
    const nlohmann::json& object,
    const char* field,
    float fallback);
float ReadFiniteNumber(
    const nlohmann::json& value,
    const char* field);
uint32_t ReadCountField(
    const nlohmann::json& object,
    const char* field,
    uint32_t fallback);
int ReadIntField(
    const nlohmann::json& object,
    const char* field,
    int fallback,
    int minimum,
    int maximum);
uint32_t ReadFrameIndex(
    const nlohmann::json& object,
    const char* field,
    uint32_t fallback);
Eigen::Vector3f NormalizeParticleCollisionPlaneNormal(
    const Eigen::Vector3f& normal,
    const char* field);

// v2 用户参数绑定：任何以 '$' 开头的字符串在展平后的 emitter JSON 中
// 替换为同名数值参数。
void SubstituteParticleUserParameters(
    nlohmann::json& value,
    const std::unordered_map<std::string, float>& parameters,
    const std::string& fieldPath,
    std::unordered_set<std::string>* references = nullptr);

// 碰撞 SDF / 三角网格资产在加载期冻结；路径相对资源根。
std::shared_ptr<const ParticleSdfAsset> LoadParticleSdfAsset(
    const std::string& path,
    const std::filesystem::path& resourceRoot);
std::shared_ptr<const ParticleSdfAsset> LoadParticleCollisionMeshAsset(
    const std::string& path,
    const std::filesystem::path& resourceRoot,
    float minimumPadding);

// v1 扁平 emitter 与 v2 展平后的 emitter 共用同一解析入口。
ParticleEmitterAsset ParseEmitter(
    const nlohmann::json& json,
    size_t index,
    bool implicit,
    const std::filesystem::path& resourceRoot,
    const ParticleCollisionResourceCache* collisionCache);

void ValidateEmitter(const ParticleEmitterAsset& emitter);

// v2 模块只做作者层组织；运行时仍消费展平后的 flat emitter。
const std::unordered_set<std::string>& ParticleModuleTypes();
void MergeParticleModuleFields(
    nlohmann::json& target,
    const nlohmann::json& source,
    const std::string& moduleType,
    const std::string& path);
nlohmann::json FlattenParticleEmitterV2(
    const nlohmann::json& emitter,
    size_t index);
std::unordered_map<std::string, float> ParseParticleUserParameters(
    const nlohmann::json& json);

} // namespace ParticleAssetDetail
} // namespace VL
