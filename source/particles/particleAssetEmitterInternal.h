#pragma once

#include <filesystem>

#include "particles/particleAsset.h"

namespace VL
{
namespace ParticleAssetDetail
{

void RequireFinite(float value, const char* field);

void ParseEmitterRibbon(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterLight(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterDecal(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterSkeletalAndSpawn(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterMaterialAndBounds(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterSpawnBudget(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterMotionAndShape(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterForcesAndEvents(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterCurves(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterBeam(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ParseEmitterCollision(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter,
    const std::filesystem::path& resourceRoot,
    const ParticleCollisionResourceCache* collisionCache);

void ParseEmitterFlipbook(
    const nlohmann::json& json,
    ParticleEmitterAsset& emitter);

void ValidateEmitterFiniteFields(const ParticleEmitterAsset& emitter);
void ValidateEmitterRenderModeAndLight(const ParticleEmitterAsset& emitter);
void ValidateEmitterDecal(const ParticleEmitterAsset& emitter);
void ValidateEmitterBillboardAlignment(const ParticleEmitterAsset& emitter);
void ValidateEmitterRibbon(const ParticleEmitterAsset& emitter);
void ValidateEmitterBeam(const ParticleEmitterAsset& emitter);
void ValidateEmitterCollision(const ParticleEmitterAsset& emitter);
void ValidateEmitterDisplayAndCurves(const ParticleEmitterAsset& emitter);
void ValidateEmitterSpawnAndMesh(const ParticleEmitterAsset& emitter);
void ValidateEmitterSimulation(const ParticleEmitterAsset& emitter);
void ValidateEmitterFlipbook(const ParticleEmitterAsset& emitter);

} // namespace ParticleAssetDetail
} // namespace VL
