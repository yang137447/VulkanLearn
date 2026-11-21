#include "hair/rod/rodContactImplicit.h"
#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodSectionInertia.h"
#include "hair/rod/rodSmoothSdf.h"
#include "hair/sdfBuilder.h"
#include "hair/tfxLoader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

namespace
{
using namespace VL::Hair::Rod;
using Json = nlohmann::json;
constexpr std::array<uint32_t, 8> SourceIds{584, 33486, 39210, 8924, 4006, 5651, 5660, 120};
constexpr double StepSeconds = 1.0 / 240.0;
constexpr double SdfDifferenceStep = 1.0e-6;

std::ofstream Open(const std::filesystem::path& path, bool binary = false)
{
    std::ofstream output(path, binary ? std::ios::out | std::ios::binary : std::ios::out);
    if (!output)
        throw std::runtime_error("Cannot create output " + path.string());
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << std::setprecision(17);
    return output;
}

Vec3 Vector(const Json& value)
{
    if (!value.is_array() || value.size() != 3)
        throw std::runtime_error("Expected finite authored vec3");
    Vec3 result(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
    if (!result.allFinite())
        throw std::runtime_error("Nonfinite authored vec3");
    return result;
}

struct GridSample
{
    bool valid = false;
    bool outside = false;
    bool positiveSaturated = false;
    bool negativeSaturated = false;
    bool nearCellSeam = false;
    double distance = 0.0;
    Vec3 gradient = Vec3::Zero();
};

struct SdfCounters
{
    uint64_t rawQueries = 0;
    uint64_t hessianCenters = 0;
    uint64_t hessianCentersCrossingCell = 0;
    uint64_t hessianCentersCrossingUnavailable = 0;

    Json Difference(const SdfCounters& before) const
    {
        return {{"raw_queries_including_FD_stencil", rawQueries - before.rawQueries},
            {"H_FD_centers", hessianCenters - before.hessianCenters},
            {"H_FD_centers_crossing_cell_seam", hessianCentersCrossingCell - before.hessianCentersCrossingCell},
            {"H_FD_centers_crossing_unavailable", hessianCentersCrossingUnavailable - before.hessianCentersCrossingUnavailable}};
    }
};

struct GridSI
{
    std::array<uint32_t, 3> dimensions{};
    Vec3 origin = Vec3::Zero();
    double cell = 0;
    double band = 0;
    std::vector<double> values;
    mutable SdfCounters counters;

    GridSample Sample(const Vec3& point) const
    {
        if (!point.allFinite())
            throw std::invalid_argument("SDF query is not finite");
        GridSample result;
        result.distance = band;
        const Vec3 coordinate = (point - origin) / cell - Vec3::Constant(.5);
        std::array<int, 3> lower{};
        Vec3 fraction;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (coordinate[axis] < 0 || coordinate[axis] > dimensions[axis] - 1)
            {
                result.outside = true;
                return result;
            }
            lower[axis] = std::min(static_cast<int>(std::floor(coordinate[axis])),
                static_cast<int>(dimensions[axis]) - 2);
            fraction[axis] = coordinate[axis] - lower[axis];
            result.nearCellSeam = result.nearCellSeam ||
                std::abs(coordinate[axis] - std::round(coordinate[axis])) < SdfDifferenceStep / cell;
        }
        result.distance = 0;
        bool allPositive = true;
        bool allNegative = true;
        for (int corner = 0; corner < 8; ++corner)
        {
            const int offsetX = corner & 1;
            const int offsetY = (corner >> 1) & 1;
            const int offsetZ = (corner >> 2) & 1;
            const size_t index = static_cast<size_t>(lower[0] + offsetX) +
                static_cast<size_t>(dimensions[0]) *
                (static_cast<size_t>(lower[1] + offsetY) +
                    static_cast<size_t>(dimensions[1]) * static_cast<size_t>(lower[2] + offsetZ));
            const double distance = values.at(index);
            allPositive = allPositive && distance >= band - 1e-10;
            allNegative = allNegative && distance <= -band + 1e-10;
            const Vec3 weights(
                offsetX ? fraction.x() : 1 - fraction.x(),
                offsetY ? fraction.y() : 1 - fraction.y(),
                offsetZ ? fraction.z() : 1 - fraction.z());
            result.distance += distance * weights.prod();
            result.gradient.x() += distance * (offsetX ? 1 : -1) / cell * weights.y() * weights.z();
            result.gradient.y() += distance * (offsetY ? 1 : -1) / cell * weights.x() * weights.z();
            result.gradient.z() += distance * (offsetZ ? 1 : -1) / cell * weights.x() * weights.y();
        }
        result.positiveSaturated = allPositive;
        result.negativeSaturated = allNegative;
        result.valid = !allPositive;
        return result;
    }
};

struct BodyPose
{
    Vec3 translation = Vec3::Zero();
    Vec3 velocity = Vec3::Zero();
    double intrusion = 0;
    double intrusionSpeed = 0;
};

BodyPose Pose(const std::string& scenario, double time)
{
    BodyPose result;
    if (scenario == "intrusion" && time >= 3 && time <= 6)
    {
        if (time < 4)
        {
            result.intrusion = .001 * (1 - std::cos(Pi * (time - 3)));
            result.intrusionSpeed = .001 * Pi * std::sin(Pi * (time - 3));
        }
        else if (time <= 5)
            result.intrusion = .002;
        else
        {
            result.intrusion = .001 * (1 + std::cos(Pi * (time - 5)));
            result.intrusionSpeed = -.001 * Pi * std::sin(Pi * (time - 5));
        }
    }
    if (scenario == "withdraw")
    {
        result.translation.y() = -.5 * std::clamp(time - 3.0, 0.0, 1.0);
        if (time > 3 && time <= 4)
            result.velocity.y() = -.5;
    }
    else if (scenario == "moving" || scenario == "head")
    {
        result.translation.y() = .005 * (1 - std::cos(Pi * time));
        result.velocity.y() = .005 * Pi * std::sin(Pi * time);
    }
    return result;
}

struct Inputs
{
    VL::Hair::TfxAsset groom;
    GridSI grid;
    std::vector<std::pair<std::string, CollisionCapsule>> capsules;
    double sdfMargin = .01;
    std::shared_ptr<const SmoothSdfSampler> smooth;
    mutable std::array<uint64_t, 7> smoothStatuses{};
    mutable uint64_t smoothCertifiedPositiveTruncated = 0;
    mutable double smoothMinimumPositiveTruncatedBound = std::numeric_limits<double>::infinity();
};

Inputs Load(const std::filesystem::path& resources, const std::filesystem::path& scenePath,
    bool sdfPrimary)
{
    Inputs result;
    auto groom = VL::Hair::LoadTfx(resources / "Maps/SC_tressfx_female/Source/female_guides.tfx");
    if (groom.IsFailure())
        throw std::runtime_error(VL::FormatRuntimeError(groom.Error()));
    result.groom = std::move(groom.Value());
    if (result.groom.guideCount != 40334 || result.groom.pointsPerStrand != 32)
        throw std::runtime_error("Unexpected real groom counts");
    auto sdf = VL::Hair::LoadSdfFile(resources / "Maps/SC_tressfx_female/Collision/female_head.sdf");
    if (sdf.IsFailure())
        throw std::runtime_error(VL::FormatRuntimeError(sdf.Error()));
    const auto& field = sdf.Value();
    result.grid.dimensions = field.dims;
    result.grid.origin = field.origin.cast<double>() * .01;
    result.grid.cell = static_cast<double>(field.cellSize) * .01;
    result.grid.band = static_cast<double>(field.band) * .01;
    for (float value : field.distances)
        result.grid.values.push_back(static_cast<double>(value) * .01);
    std::ifstream sceneInput(resources / scenePath);
    const Json scene = Json::parse(sceneInput);
    for (const auto& object : scene.at("objects"))
        if (object.value("name", "") == "Female_rod_validation")
        {
            if (Vector(object.at("position")).norm() != 0 || Vector(object.at("rotation")).norm() != 0 ||
                (Vector(object.at("scale")) - Vec3::Ones()).norm() != 0)
                throw std::runtime_error("Contact fixture requires identity bind-body TRS");
            const auto& collision = object.at("tressfx").at("hairCollision");
            result.sdfMargin = collision.at("mesh").at("margin").get<double>() * .01;
            size_t index = 0;
            for (const auto& capsule : collision.value("capsules", Json::array()))
            {
                CollisionCapsule geometry;
                geometry.a = Vector(capsule.at("a")) * .01;
                geometry.b = Vector(capsule.at("b")) * .01;
                geometry.radius = capsule.at("radius").get<double>() * .01;
                result.capsules.emplace_back("capsule-" + std::to_string(index++) + "-" +
                    capsule.value("bone", ""), geometry);
            }
        }
    if ((!sdfPrimary && result.capsules.size() != 4) || (sdfPrimary && !result.capsules.empty()))
        throw std::runtime_error("Scene capsules do not match explicitly selected geometry");
    if (std::abs(result.sdfMargin - Material{}.radius) > 1.0e-12)
        throw std::runtime_error("Isolated SDF clearance must equal fixed effective-bundle radius");
    return result;
}

std::vector<ContactConstraint> Constraints(
    const Inputs& assets, const std::string& component, const BodyPose& pose)
{
    std::vector<ContactConstraint> result;
    if (component != "sdf")
        for (const auto& item : assets.capsules)
        {
            CollisionCapsule capsule = item.second;
            capsule.a += pose.translation;
            capsule.b += pose.translation;
            capsule.velocityA = pose.velocity;
            capsule.velocityB = pose.velocity;
            result.push_back(MakeCapsuleContact(item.first, capsule));
        }
    if (component != "capsules")
    {
        if (assets.smooth)
        {
            ContactConstraint contact;
            contact.identity = SmoothSdfIdentity;
            contact.sample = [&assets, pose](const Vec3& point)
            {
                const auto raw = assets.smooth->Sample(point - pose.translation);
                ++assets.smoothStatuses[static_cast<int>(raw.status)];
                ContactGeometry geometry;
                if (raw.status == SmoothSdfStatus::PositiveSaturated)
                    return geometry;
                const double roundingReserve = 128 * std::numeric_limits<double>::epsilon() *
                    std::max({std::abs(raw.minimumCoefficient), std::abs(raw.maximumCoefficient), Material{}.radius});
                if (raw.status == SmoothSdfStatus::TruncatedSupport &&
                    raw.minimumCoefficient - Material{}.radius > roundingReserve &&
                    std::isfinite(raw.value) && raw.value - Material{}.radius > 0)
                {
                    ++assets.smoothCertifiedPositiveTruncated;
                    assets.smoothMinimumPositiveTruncatedBound = std::min(
                        assets.smoothMinimumPositiveTruncatedBound,
                        raw.minimumCoefficient - Material{}.radius - roundingReserve);
                    return geometry;
                }
                if (raw.status != SmoothSdfStatus::Available)
                    throw std::domain_error("Smooth SDF coverage/domain status=" +
                        std::to_string(static_cast<int>(raw.status)));
                if (pose.intrusion != 0 || pose.intrusionSpeed != 0)
                    throw std::domain_error("Smooth SDF artificial intrusion is not registered");
                geometry.valid = true;
                geometry.distance = raw.value;
                geometry.gradient = raw.gradient;
                geometry.hessian = raw.hessian;
                geometry.surfaceVelocity = pose.velocity;
                return geometry;
            };
            result.push_back(std::move(contact));
            return result;
        }
        const auto query = [&assets, pose](const Vec3& point)
        {
            ++assets.grid.counters.rawQueries;
            const GridSample sample = assets.grid.Sample(point - pose.translation);
            CollisionSample value;
            value.valid = sample.valid;
            value.distance = sample.distance;
            value.gradient = sample.gradient;
            value.colliderVelocity = pose.velocity;
            if (pose.intrusion != 0 || pose.intrusionSpeed != 0)
            {
                const auto& center = assets.groom.positions.at(584 * 32 + 2);
                const Vec3 offset = point - Vec3(center[0], center[1], center[2]) * .01;
                const double radiusSquared = .004 * .004;
                const double fraction = offset.squaredNorm() / radiusSquared;
                if (fraction < 1)
                {
                    const double weight = std::pow(1 - fraction, 3);
                    const Vec3 gradientWeight = -6 * std::pow(1 - fraction, 2) * offset / radiusSquared;
                    value.distance -= pose.intrusion * weight;
                    value.gradient -= pose.intrusion * gradientWeight;
                    if (value.gradient.squaredNorm() > 1e-20)
                        value.colliderVelocity += pose.intrusionSpeed * weight *
                            value.gradient / value.gradient.squaredNorm();
                }
            }
            return value;
        };
        auto contact = MakeRawSdfContact("female-head-sdf-profile-radius", query, SdfDifferenceStep);
        const auto sample = contact.sample;
        contact.sample = [sample, &assets, pose](const Vec3& point)
        {
            ++assets.grid.counters.hessianCenters;
            const GridSample center = assets.grid.Sample(point - pose.translation);
            if (center.valid)
            {
                assets.grid.counters.hessianCentersCrossingCell += center.nearCellSeam;
                bool unavailable = false;
                for (int axis = 0; axis < 3; ++axis)
                    unavailable = unavailable ||
                        !assets.grid.Sample(point - pose.translation + SdfDifferenceStep * Vec3::Unit(axis)).valid ||
                        !assets.grid.Sample(point - pose.translation - SdfDifferenceStep * Vec3::Unit(axis)).valid;
                assets.grid.counters.hessianCentersCrossingUnavailable += unavailable;
            }
            return sample(point);
        };
        result.push_back(std::move(contact));
    }
    return result;
}

RestState Rest(const Inputs& assets, uint32_t source)
{
    std::vector<Vec3> positions;
    std::vector<double> markers;
    for (size_t point = 0; point < 32; ++point)
    {
        const auto& value = assets.groom.positions.at(static_cast<size_t>(source) * 32 + point);
        positions.emplace_back(.01 * value[0], .01 * value[1], .01 * value[2]);
        markers.push_back(value[3]);
    }
    return BuildSectionInertiaRestState(positions, Material{}, {}, markers);
}

bool Finite(const State& state)
{
    for (const auto& value : state.positions)
        if (!value.allFinite()) return false;
    for (const auto& value : state.velocities)
        if (!value.allFinite()) return false;
    for (const auto& value : state.orientations)
        if (!value.coeffs().allFinite()) return false;
    for (const auto& value : state.angularVelocities)
        if (!value.allFinite()) return false;
    return true;
}

double Deviation(const RestState& rest, const State& state, const Vec3& translation = Vec3::Zero())
{
    double maximum = 0;
    for (size_t point = 0; point < state.positions.size(); ++point)
        maximum = std::max(maximum, (state.positions[point] - translation - rest.positions[point]).norm());
    return maximum;
}

double Potential(const RestState& rest, const State& state,
    const StepInput& input, const ProgressiveSupport& support)
{
    double value = EvaluateEnergy(rest, state).Total() +
        EvaluateProgressiveSupport(rest, state, input.root, support).potential;
    for (size_t point = 0; point < state.positions.size(); ++point)
        value -= rest.masses[point] * input.gravity.dot(state.positions[point]);
    return value;
}

double Kinetic(const RestState& rest, const State& state)
{
    double value = 0;
    for (size_t point = 0; point < state.positions.size(); ++point)
        value += .5 * rest.masses[point] * state.velocities[point].squaredNorm();
    for (size_t segment = 0; segment < state.orientations.size(); ++segment)
        value += .5 * state.angularVelocities[segment].dot(
            rest.inertias[segment].cwiseProduct(state.angularVelocities[segment]));
    return value;
}

template <typename Value>
void Binary(std::ostream& output, const Value& value)
{
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void WriteState(std::ostream& output, uint64_t step, double time, Status status, const State& state)
{
    Binary(output, step);
    Binary(output, time);
    Binary(output, static_cast<uint32_t>(status));
    Binary(output, uint32_t{0});
    for (const auto& value : state.positions)
        for (int axis = 0; axis < 3; ++axis) Binary(output, value[axis]);
    for (const auto& value : state.velocities)
        for (int axis = 0; axis < 3; ++axis) Binary(output, value[axis]);
    for (const auto& value : state.orientations)
    {
        Binary(output, value.w());
        Binary(output, value.x());
        Binary(output, value.y());
        Binary(output, value.z());
    }
    for (const auto& value : state.angularVelocities)
        for (int axis = 0; axis < 3; ++axis) Binary(output, value[axis]);
}

Json Scan(const RestState& rest, const State& state, const Inputs& assets,
    const BodyPose& pose, const std::vector<ContactConstraint>& constraints,
    const StepInput& input, std::ostream* details = nullptr, const std::string& phase = "")
{
    double minimum = std::numeric_limits<double>::infinity();
    double pinnedMinimum = std::numeric_limits<double>::infinity();
    unsigned invalid = 0, domain = 0, outside = 0, saturated = 0, negative = 0, seams = 0;
    for (size_t point = 0; point < state.positions.size(); ++point)
    {
        const GridSample raw = assets.grid.Sample(state.positions[point] - pose.translation);
        outside += raw.outside;
        saturated += raw.positiveSaturated;
        negative += raw.negativeSaturated;
        seams += raw.nearCellSeam;
        for (const auto& constraint : constraints)
        {
            try
            {
                const auto geometry = constraint.sample(state.positions[point]);
                const double norm = geometry.gradient.norm();
                const double gap = geometry.distance - constraint.distanceScale * input.contactMargin;
                if (!geometry.valid)
                    ++invalid;
                else if (norm <= 1.0e-14)
                    ++domain;
                else if (rest.inverseMasses[point] == 0)
                    pinnedMinimum = std::min(pinnedMinimum, gap / norm);
                else
                    minimum = std::min(minimum, gap / norm);
                if (details)
                    *details << phase << ',' << point << ',' << constraint.identity << ','
                        << (rest.inverseMasses[point] == 0) << ',' << geometry.valid << ','
                        << gap << ',' << norm << ','
                        << (norm > 0 ? gap / norm : 0) << ','
                        << raw.outside << ',' << raw.positiveSaturated << ','
                        << raw.negativeSaturated << ',' << raw.nearCellSeam << '\n';
            }
            catch (const std::exception& error)
            {
                ++domain;
                if (details)
                    *details << phase << ',' << point << ',' << constraint.identity << ','
                        << (rest.inverseMasses[point] == 0) << ",0,NA,NA,NA,"
                        << raw.outside << ',' << raw.positiveSaturated << ','
                        << raw.negativeSaturated << ',' << raw.nearCellSeam << ',' << error.what() << '\n';
            }
        }
    }
    return {{"minimum_movable_geometric_gap_m", std::isfinite(minimum) ? Json(minimum) : Json(nullptr)},
        {"minimum_pinned_geometric_gap_m", std::isfinite(pinnedMinimum) ? Json(pinnedMinimum) : Json(nullptr)},
        {"unavailable_constraints", invalid}, {"geometry_domain_failures", domain},
        {"sdf_points_outside", outside}, {"sdf_points_positive_saturated", saturated},
        {"sdf_points_negative_saturated", negative}, {"sdf_points_near_cell_seam", seams}};
}

void Records(std::ostream& output, uint64_t step, const ContactImplicitDiagnostics& result)
{
    for (const auto& record : result.contacts)
        output << step << ',' << record.point << ',' << record.identity << ',' << record.valid << ','
            << record.active << ',' << record.pinnedExcluded << ',' << record.rawGap << ','
            << record.geometricGap << ',' << record.reactionMultiplier << ','
            << record.force.norm() << ',' << record.complementarity << ','
            << record.surfaceVelocity.x() << ',' << record.surfaceVelocity.y() << ','
            << record.surfaceVelocity.z() << ',' << static_cast<int>(result.status) << ','
            << record.gradient.x() << ',' << record.gradient.y() << ',' << record.gradient.z() << ','
            << record.force.x() << ',' << record.force.y() << ',' << record.force.z() << '\n';
}

Json StateJson(const State& state)
{
    Json value{{"positions_m", Json::array()}, {"velocities_m_s", Json::array()},
        {"orientations_wxyz", Json::array()}, {"omega_body_rad_s", Json::array()}};
    for (const auto& point : state.positions)
        value["positions_m"].push_back({point.x(), point.y(), point.z()});
    for (const auto& velocity : state.velocities)
        value["velocities_m_s"].push_back({velocity.x(), velocity.y(), velocity.z()});
    for (const auto& orientation : state.orientations)
        value["orientations_wxyz"].push_back({orientation.w(), orientation.x(), orientation.y(), orientation.z()});
    for (const auto& omega : state.angularVelocities)
        value["omega_body_rad_s"].push_back({omega.x(), omega.y(), omega.z()});
    return value;
}

Json RunCase(const Inputs& assets, uint32_t source, const std::string& component,
    const std::string& scenario, unsigned steps, const std::filesystem::path& root, bool preflight,
    bool traceFirst = false, unsigned traceFromStep = std::numeric_limits<unsigned>::max())
{
    const auto directory = root / (std::to_string(source) + "-" + component + "-" + scenario);
    std::filesystem::create_directory(directory);
    const RestState rest = Rest(assets, source);
    const SdfCounters initialCounters = assets.grid.counters;
    State state = MakeAuthoredState(rest);
    const auto support = BuildProgressiveSupport(rest);
    StepInput input;
    input.stepSeconds = StepSeconds;
    input.root.position = rest.positions.front();
    input.root.orientation = rest.orientations.front();
    input.contactMargin = Material{}.radius;
    const auto initialConstraints = Constraints(assets, component, Pose(scenario, 0));
    auto gaps = Open(directory / "gaps.csv");
    gaps << "phase,point,identity,pinned,valid,rawGapM,rawGradientNorm,geometricGapM,sdfOutside,sdfPositiveSaturated,sdfNegativeSaturated,sdfNearCellSeam,error\n";
    auto repairRows = Open(directory / "repair.csv");
    repairRows << "point,sourceW,xBefore,yBefore,zBefore,xAfter,yAfter,zAfter,dx,dy,dz,vPreserved,omegaPreserved\n";
    Json report{{"source_guide_id", source}, {"points", 32}, {"component", component},
        {"scenario", scenario}, {"step_seconds", StepSeconds}, {"requested_steps", steps},
        {"source_length_m", rest.totalLength}, {"quality_pass", false}};
    report["kineticMathVersion"] = SectionInertiaKineticMathVersion;
    report["field_sampler_identity"] = assets.smooth ? SmoothSdfIdentity : "trilinearGradientCentralFD1umV1";
    report["director_inertia_model"] = "segmentMass*(r^2/4,r^2/4,r^2/2)";
    report["translational_mass_model"] = "unchanged endpoint row-sum lumped";
    report["common_contact_margin_m"] = input.contactMargin;
    report["clearance_application"] = "raw SDF and original capsule geometry; common Gap subtracts fixed profile radius once";
    report["node_masses_kg"] = rest.masses;
    report["segment_lengths_m"] = rest.lengths;
    report["segment_inertia_kg_m2"] = Json::array();
    for (const auto& inertia : rest.inertias)
        report["segment_inertia_kg_m2"].push_back({inertia.x(), inertia.y(), inertia.z()});
    report["before_repair"] = Scan(rest, state, assets, Pose(scenario, 0), initialConstraints, input, &gaps, "authored");
    if (preflight)
    {
        report["phase"] = "PREFLIGHT_ONLY";
        report["SDF_adapter_counters"] = assets.grid.counters.Difference(initialCounters);
        auto json = Open(directory / "report.json");
        json << report.dump(2);
        return report;
    }
    const State beforeRepair = state;
    const double energyBefore = Potential(rest, state, input, support);
    const auto repair = RepairInitialContactOverlap(rest, state, input, support, initialConstraints);
    report["repair_status"] = static_cast<int>(repair.status);
    report["repair_iterations"] = repair.iterations;
    report["repair_delta_potential_J"] = repair.potentialEnergyChange;
    report["repair_independent_delta_potential_J"] = Potential(rest, state, input, support) - energyBefore;
    report["repair_Dmax_over_L"] = Deviation(rest, state) / rest.totalLength;
    report["after_repair"] = Scan(rest, state, assets, Pose(scenario, 0), initialConstraints, input, &gaps, "repaired");
    for (size_t point = 0; point < state.positions.size(); ++point)
    {
        const auto& marker = assets.groom.positions.at(static_cast<size_t>(source) * 32 + point);
        repairRows << point << ',' << marker[3];
        for (int axis = 0; axis < 3; ++axis) repairRows << ',' << beforeRepair.positions[point][axis];
        for (int axis = 0; axis < 3; ++axis) repairRows << ',' << state.positions[point][axis];
        for (int axis = 0; axis < 3; ++axis) repairRows << ',' << state.positions[point][axis] - beforeRepair.positions[point][axis];
        repairRows << ',' << (state.velocities[point] == beforeRepair.velocities[point]) << ','
            << (point >= state.angularVelocities.size() ||
                state.angularVelocities[point] == beforeRepair.angularVelocities[point]) << '\n';
    }
    auto states = Open(directory / "states.bin", true);
    states.write("RDCV3\0\0\0", 8);
    Binary(states, uint32_t{1});
    Binary(states, source);
    Binary(states, uint32_t{32});
    Binary(states, uint32_t{31});
    WriteState(states, 0, 0, repair.status, state);
    auto telemetry = Open(directory / "steps.csv");
    telemetry << "step,time,status,newton,backtracks,normResidual,rx,rq,maxPenetrationM,maxComplementarity,active,dependent,minLambda,maxForceN,contactPowerW,potentialJ,kineticJ,energyLedgerRemainderJ,supportDriveW,supportDissipationW,DmaxOverL,finite,wallStepMs,reason\n";
    auto contacts = Open(directory / "contacts.csv");
    auto forces = Open(directory / "external-forces.csv");
    forces << "step,time,point,airSpeedMps,fx,fy,fz\n";
    contacts << "step,point,identity,valid,active,pinnedExcluded,rawGapM,geometricGapM,lambdaN,forceNormN,complementarity,surfaceVx,surfaceVy,surfaceVz,status,gx,gy,gz,fx,fy,fz\n";
    unsigned acceptedSteps = 0;
    bool firstActiveSaved = false;
    double previousEnergy = Potential(rest, state, input, support) + Kinetic(rest, state);
    double maximumStepMilliseconds = 0;
    double maximumPinRelativeError = 0;
    double maximumPinSecantVelocityError = 0;
    double maximumPinAnalyticVelocityDifference = 0;
    Vec3 acceptedTranslation = Vec3::Zero();
    report["motion_model"] = scenario == "head" ? "coherent root/pins/support/body/SDF translation" :
        (scenario == "static" ? "static" : "nonphysical collider-only stress; not head-motion qualification");
    const auto begin = std::chrono::steady_clock::now();
    if (repair.status == Status::Success)
        for (unsigned step = 1; step <= steps; ++step)
        {
            const double time = step * StepSeconds;
            const BodyPose pose = Pose(scenario, time);
            if (scenario == "head")
            {
                input.root.position = rest.positions.front() + pose.translation;
                input.root.linearVelocity = pose.velocity;
            }
            const auto constraints = Constraints(assets, component, pose);
            const State old = state;
            if (scenario == "wind")
            {
                const double airSpeed = time >= 3 && time <= 3.5 ? 20 : 0;
                input.externalForces.resize(rest.positions.size());
                for (size_t point = 0; point < rest.positions.size(); ++point)
                {
                    const size_t first = point == 0 ? 0 : point - 1;
                    const size_t last = std::min(point + 1, old.positions.size() - 1);
                    const Vec3 tangent = (old.positions[last] - old.positions[first]).normalized();
                    input.externalForces[point] = ProgressiveAirDragForce(support, rest.masses[point],
                        Vec3(airSpeed, 0, 0) - old.velocities[point], tangent);
                    forces << step << ',' << time << ',' << point << ',' << airSpeed;
                    for (int axis = 0; axis < 3; ++axis)
                        forces << ',' << input.externalForces[point][axis];
                    forces << '\n';
                }
            }
            const auto started = std::chrono::steady_clock::now();
            const auto result = StepImplicitMomentumContact(rest, state, input, support, constraints);
            if (result.status == Status::Success && scenario == "head")
                for (size_t point = 0; point < rest.positions.size(); ++point)
                    if (rest.inverseMasses[point] == 0)
                    {
                        maximumPinRelativeError = std::max(maximumPinRelativeError,
                            (state.positions[point] - pose.translation - rest.positions[point]).norm());
                        const Vec3 secant = (pose.translation -
                            Pose(scenario, time - StepSeconds).translation) / StepSeconds;
                        maximumPinSecantVelocityError = std::max(maximumPinSecantVelocityError,
                            (state.velocities[point] - secant).norm());
                        maximumPinAnalyticVelocityDifference = std::max(maximumPinAnalyticVelocityDifference,
                            (state.velocities[point] - pose.velocity).norm());
                    }
            if (result.status == Status::Success && scenario == "head")
                acceptedTranslation = pose.translation;
            const double milliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            maximumStepMilliseconds = std::max(maximumStepMilliseconds, milliseconds);
            const double potential = Potential(rest, state, input, support);
            const double kinetic = Kinetic(rest, state);
            const double ledger = potential + kinetic - previousEnergy -
                StepSeconds * (result.contactPower + result.supportDrivePower - result.supportDissipation);
            unsigned active = 0;
            double minimumLambda = 0, maximumForce = 0;
            for (const auto& record : result.contacts)
            {
                active += record.active;
                minimumLambda = std::min(minimumLambda, record.reactionMultiplier);
                maximumForce = std::max(maximumForce, record.force.norm());
            }
            const bool captureFirstActive = active > 0 && !firstActiveSaved;
            if (captureFirstActive)
            {
                Json first{{"step", step}, {"attempted_time", time}, {"status", static_cast<int>(result.status)},
                    {"old", StateJson(old)}, {"candidate_available", result.status == Status::Success},
                    {"candidate", result.status == Status::Success ? StateJson(state) : Json(nullptr)},
                    {"records", Json::array()}, {"common_margin_m", input.contactMargin},
                    {"kineticMathVersion", SectionInertiaKineticMathVersion}};
                if (result.status == Status::Success)
                {
                    auto residual = EvaluateProgressiveImplicitResidual(rest, old, state, input, support);
                    for (const auto& record : result.contacts)
                    {
                        const auto found = std::find_if(constraints.begin(), constraints.end(),
                            [&](const ContactConstraint& value) { return value.identity == record.identity; });
                        const auto geometry = found->sample(state.positions[record.point]);
                        Json entry{{"point", record.point}, {"identity", record.identity}, {"active", record.active},
                            {"pinnedExcluded", record.pinnedExcluded}, {"lambda_N", record.reactionMultiplier},
                            {"raw_distance_m", geometry.distance}, {"raw_gap_m", record.rawGap},
                            {"geometric_gap_m", record.geometricGap}, {"complementarity", record.complementarity},
                            {"gradient", {geometry.gradient.x(), geometry.gradient.y(), geometry.gradient.z()}},
                            {"force_N", {record.force.x(), record.force.y(), record.force.z()}},
                            {"surface_velocity_m_s", {geometry.surfaceVelocity.x(), geometry.surfaceVelocity.y(), geometry.surfaceVelocity.z()}},
                            {"hessian_rows", Json::array()}};
                        for (int row = 0; row < 3; ++row)
                            entry["hessian_rows"].push_back({geometry.hessian(row, 0),
                                geometry.hessian(row, 1), geometry.hessian(row, 2)});
                        first["records"].push_back(std::move(entry));
                        if (!record.pinnedExcluded)
                            residual[record.point].head<3>() -= record.force;
                    }
                    first["physical_residual_after_contact_force"] = Json::array();
                    for (const auto& row : residual)
                        first["physical_residual_after_contact_force"].push_back(
                            {row[0], row[1], row[2], row[3], row[4], row[5]});
                    first["geometry_capture"] = "same frozen raw query re-evaluated at accepted candidate";
                }
                else
                    first["capture_limit"] = "This compact record omits rejected state; see rejected-candidate-trace.json for true candidate and raw geometry, never substitute rollback";
                auto firstOutput = Open(directory / "first-active-contact.json");
                firstOutput << first.dump(2);
                firstActiveSaved = true;
                Records(contacts, step, result);
                report["first_active_step"] = step;
                report["first_active_candidate_saved"] = result.status == Status::Success;
            }
            telemetry << step << ',' << time << ',' << static_cast<int>(result.status) << ','
                << result.newtonIterations << ',' << result.backtracks << ',' << result.normalizedResidual << ','
                << result.maximumLinearResidual << ',' << result.maximumAngularResidual << ','
                << result.maximumGeometricPenetration << ',' << result.maximumComplementarity << ','
                << active << ',' << result.dependentNormals << ',' << minimumLambda << ',' << maximumForce << ','
                << result.contactPower << ',' << potential << ',' << kinetic << ',' << ledger << ','
                << result.supportDrivePower << ',' << result.supportDissipation << ','
                << Deviation(rest, state, acceptedTranslation) / rest.totalLength << ',' << Finite(state) << ',' << milliseconds
                << ',' << result.failureReason << '\n';
            WriteState(states, step, result.status == Status::Success ? time : acceptedSteps * StepSeconds, result.status, state);
            if (step == 1 || step % 240 == 0 || step == steps || result.status != Status::Success)
            {
                Records(contacts, step, result);
                Scan(rest, state, assets, pose, constraints, input, &gaps,
                    result.status == Status::Success ? "accepted-step-" + std::to_string(step) : "rolled-back-step-" + std::to_string(step));
            }
            if (result.status != Status::Success || !Finite(state) || (traceFirst && step == 1) ||
                captureFirstActive || step >= traceFromStep)
            {
                State replay = old;
                ContactTrace trace;
                const auto traced = StepImplicitMomentumContactWithTrace(
                    rest, replay, input, support, constraints, trace);
                Json rejected{{"capture", "deterministic same-input replay with trace"},
                    {"original_status", static_cast<int>(result.status)},
                    {"replay_status", static_cast<int>(traced.status)},
                    {"matching_status", result.status == traced.status},
                    {"matching_published_state", StateJson(replay) == StateJson(state)},
                    {"old", StateJson(trace.old)}, {"candidate", StateJson(trace.finalCandidate)},
                    {"failure_reason", trace.failureReason}, {"iterations", Json::array()},
                    {"constraint_identities", Json::array()}};
                for (const auto& constraint : constraints)
                    rejected["constraint_identities"].push_back(constraint.identity);
                if (trace.finalCandidate.positions.size() == rest.positions.size() &&
                    trace.finalActive.size() == rest.positions.size())
                {
                    auto reactionResidual = EvaluateProgressiveImplicitResidual(
                        rest, old, trace.finalCandidate, input, support);
                    rejected["candidate_geometry"] = Json::array();
                    double maximumLinear = 0, maximumAngular = 0;
                    for (size_t point = 0; point < rest.positions.size(); ++point)
                    {
                        for (size_t identity = 0; identity < constraints.size(); ++identity)
                        {
                            ContactGeometry geometry;
                            try
                            {
                                geometry = constraints[identity].sample(trace.finalCandidate.positions[point]);
                            }
                            catch (const std::exception& error)
                            {
                                rejected["candidate_geometry"].push_back({{"point", point},
                                    {"identity", constraints[identity].identity},
                                    {"valid", false}, {"sampling_error", error.what()}});
                                continue;
                            }
                            double multiplier = 0;
                            for (int slot = 0; slot < 3; ++slot)
                                if (trace.finalActive[point].identities[slot] == static_cast<int>(identity))
                                    multiplier = trace.finalActive[point].multipliers[slot];
                            const Vec3 force = multiplier * geometry.gradient;
                            reactionResidual[point].head<3>() -= force;
                            Json geometryRow{{"point", point}, {"identity", constraints[identity].identity},
                                {"valid", geometry.valid}, {"pinned", rest.inverseMasses[point] == 0},
                                {"raw_phi_m", geometry.distance},
                                {"raw_gap_m", geometry.distance - input.contactMargin},
                                {"gradient", {geometry.gradient.x(), geometry.gradient.y(), geometry.gradient.z()}},
                                {"hessian_row_major", Json::array()},
                                {"lambda_N", multiplier}, {"force_N", {force.x(), force.y(), force.z()}},
                                {"surface_velocity_mps", {geometry.surfaceVelocity.x(), geometry.surfaceVelocity.y(),
                                    geometry.surfaceVelocity.z()}},
                                {"relative_normal_velocity_mps", geometry.gradient.dot(
                                    (trace.finalCandidate.positions[point] - old.positions[point]) / StepSeconds -
                                    geometry.surfaceVelocity)},
                                {"contact_power_W", force.dot(geometry.surfaceVelocity)}};
                            for (int row = 0; row < 3; ++row)
                                for (int column = 0; column < 3; ++column)
                                    geometryRow["hessian_row_major"].push_back(geometry.hessian(row, column));
                            rejected["candidate_geometry"].push_back(geometryRow);
                        }
                        if (rest.inverseMasses[point] != 0)
                            maximumLinear = std::max(maximumLinear, reactionResidual[point].head<3>().norm());
                        if (point > 0)
                            maximumAngular = std::max(maximumAngular, reactionResidual[point].tail<3>().norm());
                    }
                    rejected["candidate_physical_reaction_residual_N"] = maximumLinear;
                    rejected["candidate_physical_angular_residual_Nm"] = maximumAngular;
                }
                for (const auto& iteration : trace.iterations)
                {
                    Json entry{{"candidate", StateJson(iteration.candidate)},
                        {"normalized_residual", iteration.normalizedResidual},
                        {"active", Json::array()}, {"previous_active", Json::array()},
                        {"excluded", iteration.excluded},
                        {"changed_nodes", iteration.changedNodes},
                        {"dependent_normals", iteration.dependentNormals},
                        {"force_scale", iteration.system.forceScale},
                        {"length_scale", iteration.system.lengthScale},
                        {"blocks", Json::array()}, {"trials", Json::array()}};
                    for (const auto& node : iteration.active)
                        entry["active"].push_back({{"identities", node.identities},
                            {"multipliers", {node.multipliers.x(), node.multipliers.y(), node.multipliers.z()}}});
                    for (const auto& node : iteration.previousActive)
                        entry["previous_active"].push_back({{"identities", node.identities},
                            {"multipliers", {node.multipliers.x(), node.multipliers.y(), node.multipliers.z()}}});
                    for (size_t block = 0; block < iteration.system.diagonal.size(); ++block)
                    {
                        Json value{{"lower", Json::array()}, {"diagonal", Json::array()},
                            {"upper", Json::array()}, {"residual", Json::array()}, {"direction", Json::array()}};
                        for (int row = 0; row < 9; ++row)
                        {
                            value["residual"].push_back(iteration.system.residual[block][row]);
                            if (block < iteration.direction.size())
                                value["direction"].push_back(iteration.direction[block][row]);
                            for (int column = 0; column < 9; ++column)
                            {
                                value["lower"].push_back(iteration.system.lower[block](row, column));
                                value["diagonal"].push_back(iteration.system.diagonal[block](row, column));
                                value["upper"].push_back(iteration.system.upper[block](row, column));
                            }
                        }
                        entry["blocks"].push_back(value);
                    }
                    for (const auto& trial : iteration.trials)
                        entry["trials"].push_back({{"alpha", trial.alpha}, {"merit", trial.merit},
                            {"normalized_residual", trial.normalizedResidual},
                            {"domain_failure", trial.domainFailure}, {"accepted", trial.accepted}});
                    rejected["iterations"].push_back(entry);
                }
                const std::string traceName = result.status != Status::Success ? "rejected-candidate-trace.json" :
                    (step >= traceFromStep ? "step-" + std::to_string(step) + "-trace.json" :
                        (captureFirstActive ? "first-active-trace.json" : "first-step-trace.json"));
                auto traceOutput = Open(directory / traceName);
                traceOutput << rejected.dump(2);
            }
            if (result.status != Status::Success || !Finite(state))
            {
                report["failure_step"] = step;
                report["failure_status"] = static_cast<int>(result.status);
                report["failure_reason"] = result.failureReason;
                report["failure_contact_record_state"] = "candidate records; published state atomically rolled back";
                break;
            }
            ++acceptedSteps;
            previousEnergy = potential + kinetic;
        }
    report["accepted_steps"] = acceptedSteps;
    report["accepted_seconds"] = acceptedSteps * StepSeconds;
    report["completed_protocol"] = repair.status == Status::Success && acceptedSteps == steps;
    report["eligible_for_long"] = repair.status == Status::Success && acceptedSteps == 8 && Finite(state);
    report["elapsed_seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    report["maximum_step_ms"] = maximumStepMilliseconds;
    report["final_Dmax_over_L"] = Deviation(rest, state, acceptedTranslation) / rest.totalLength;
    report["finite_published_state"] = Finite(state);
    report["SDF_adapter_counters"] = assets.grid.counters.Difference(initialCounters);
    report["smooth_sampler_cumulative_status_counts"] = assets.smoothStatuses;
    report["smooth_certified_positive_truncated_queries"] = assets.smoothCertifiedPositiveTruncated;
    report["smooth_minimum_certified_positive_truncated_gap_bound_m"] =
        std::isfinite(assets.smoothMinimumPositiveTruncatedBound) ?
        Json(assets.smoothMinimumPositiveTruncatedBound) : Json(nullptr);
    report["maximum_pin_body_relative_position_error_m"] = maximumPinRelativeError;
    report["maximum_pin_secant_velocity_error_m_per_s"] = maximumPinSecantVelocityError;
    report["maximum_pin_analytic_velocity_difference_m_per_s"] = maximumPinAnalyticVelocityDifference;
    report["shape_reference"] = scenario == "head" ? "same-pose authored rest" : "authored bind rest";
    auto output = Open(directory / "report.json");
    output << report.dump(2);
    std::cout << "source=" << source << " component=" << component << " scenario=" << scenario
        << " repair=" << static_cast<int>(repair.status) << " accepted=" << acceptedSteps << '/' << steps << std::endl;
    return report;
}

int Run(int argc, char** argv)
{
    std::filesystem::path resources = "../VukanLearnResources", output, probeReport;
    std::string mode = "preflight", scenarioFilter = "all", geometry = "combined", caseSet = "all";
    std::string fieldSampler = "trilinear";
    std::filesystem::path scene = "Maps/SC_tressfx_rod_validation/SC_tressfx_rod_static.json";
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (index + 1 >= argc)
            throw std::runtime_error("Expected --resource-root/--output/--mode/--probe-report value");
        const std::string value = argv[++index];
        if (argument == "--resource-root") resources = value;
        else if (argument == "--output") output = value;
        else if (argument == "--mode") mode = value;
        else if (argument == "--probe-report") probeReport = value;
        else if (argument == "--scenario") scenarioFilter = value;
        else if (argument == "--geometry") geometry = value;
        else if (argument == "--scene") scene = value;
        else if (argument == "--case-set") caseSet = value;
        else if (argument == "--field-sampler") fieldSampler = value;
        else throw std::runtime_error("Unknown option " + argument);
    }
    if (output.empty() || std::filesystem::exists(output) ||
        (mode != "preflight" && mode != "probe" && mode != "long") ||
        (geometry != "combined" && geometry != "sdf") ||
        (caseSet != "all" && caseSet != "capsule-failures" && caseSet != "capsule-prefix" &&
            caseSet != "sdf-exclusion-prefix" && caseSet != "sdf-exclusion-long" &&
            caseSet != "smooth-8924-prefix" && caseSet != "smooth-formal-long") ||
        (fieldSampler != "trilinear" && fieldSampler != SmoothSdfIdentity) ||
        scene.is_absolute() ||
        (scenarioFilter != "all" && scenarioFilter != "static" && scenarioFilter != "withdraw" &&
            scenarioFilter != "moving" && scenarioFilter != "head" &&
            scenarioFilter != "wind" && scenarioFilter != "intrusion"))
        throw std::runtime_error("New output directory and registered mode required");
    if (mode == "long" && (scenarioFilter == "withdraw" || scenarioFilter == "moving"))
        throw std::runtime_error("Collider-only stress is archived; use explicitly registered coherent head protocol");
    if ((caseSet == "capsule-failures" || caseSet == "capsule-prefix") &&
        (mode != "probe" || geometry != "combined"))
        throw std::runtime_error("Capsule-failure trace requires original combined-scene probe mode");
    if ((caseSet == "sdf-exclusion-prefix" || caseSet == "sdf-exclusion-long") &&
        (mode != "long" || geometry != "sdf" || scenarioFilter != "wind"))
        throw std::runtime_error("Exclusion replay requires registered SDF-only wind prefix");
    if ((caseSet == "smooth-8924-prefix" || caseSet == "smooth-formal-long") &&
        (geometry != "sdf" || fieldSampler != SmoothSdfIdentity ||
            (mode == "long" && scenarioFilter != "wind")))
        throw std::runtime_error("Smooth8924 prefix requires explicit smooth SDF-primary identity");
    std::filesystem::create_directories(output);
    Inputs assets = Load(resources, scene, geometry == "sdf");
    if (fieldSampler == SmoothSdfIdentity)
    {
        SmoothSdfGridSI smoothGrid;
        smoothGrid.dimensions = assets.grid.dimensions;
        smoothGrid.origin = assets.grid.origin;
        smoothGrid.cellSize = assets.grid.cell;
        smoothGrid.band = assets.grid.band;
        smoothGrid.coefficients = assets.grid.values;
        assets.smooth = std::make_shared<SmoothSdfSampler>(std::move(smoothGrid));
    }
    Json summary{{"mode", mode}, {"algorithm", "cosseratImplicitV3-contact-KKT"},
        {"profile", "effectiveBundleV1"}, {"source_ids", SourceIds},
        {"geometry_selection", geometry}, {"scene", scene.generic_string()}, {"case_set", caseSet},
        {"field_sampler_identity", fieldSampler},
        {"smooth_coverage_policy", "positive-support-convex-lower-bound-and-finite-scalar-double128epsV2; other unavailable throws"},
        {"kineticMathVersion", SectionInertiaKineticMathVersion},
        {"points", 32}, {"step_seconds", StepSeconds}, {"quality_pass", false},
        {"sdf_cell_m", assets.grid.cell}, {"sdf_band_m", assets.grid.band},
        {"sdf_dims", assets.grid.dimensions}, {"sdf_margin_m", assets.sdfMargin},
        {"capsule_margin_m", Material{}.radius}, {"SDF_H_FD_step_m", SdfDifferenceStep},
        {"clearance_application", "common StepInput radius; no SDF pre-offset and no capsule radius inflation"},
        {"cases", Json::array()}};
    Json qualified;
    if (mode == "long")
    {
        if (probeReport.empty())
            throw std::runtime_error("Long mode requires actual successful matching-geometry probe report");
        std::ifstream input(probeReport);
        qualified = Json::parse(input);
        if (geometry == "sdf" &&
            (qualified.value("geometry_selection", "") != geometry ||
                qualified.value("scene", "") != scene.generic_string()))
            throw std::runtime_error("SDF-primary long must use matching scene/geometry probe");
        if (fieldSampler == SmoothSdfIdentity &&
            qualified.value("field_sampler_identity", "") != fieldSampler)
            throw std::runtime_error("Smooth field long requires its own actual short probe");
    }
    for (uint32_t source : SourceIds)
    {
        if ((caseSet == "capsule-failures" || caseSet == "capsule-prefix") &&
            source != 39210 && source != 8924 && source != 4006)
            continue;
        if ((caseSet == "sdf-exclusion-prefix" || caseSet == "sdf-exclusion-long") &&
            source != 5651 && source != 5660)
            continue;
        if (caseSet == "smooth-8924-prefix" && source != 8924)
            continue;
        if (caseSet == "smooth-formal-long" && source != 8924 && source != 5651 && source != 5660)
            continue;
        if (mode == "long")
        {
            const bool eligible = std::any_of(qualified.at("cases").begin(), qualified.at("cases").end(),
                [source, &geometry](const Json& value) { return value.at("source_guide_id") == source &&
                    value.at("component") == geometry && value.value("eligible_for_long", false); });
            if (!eligible)
            {
                summary["cases"].push_back({{"source_guide_id", source}, {"status", "SKIPPED_FAILED_MATCHING_GEOMETRY_PROBE"}});
                continue;
            }
            for (const std::string scenario : {"static", "head", "wind", "intrusion"})
                if (scenarioFilter == "all" || scenarioFilter == scenario)
                    summary["cases"].push_back(RunCase(assets, source, geometry, scenario,
                        caseSet == "smooth-8924-prefix" ? 724 :
                            (caseSet == "smooth-formal-long" ? 3240 :
                                (caseSet == "sdf-exclusion-prefix" ? (source == 5651 ? 720 : 726) : 3288)),
                        output, false, false,
                        caseSet == "sdf-exclusion-prefix" || caseSet == "smooth-8924-prefix" ?
                            720 : std::numeric_limits<unsigned>::max()));
        }
        else
            for (const std::string component : {"capsules", "sdf", "combined"})
                if ((geometry != "sdf" || component == "sdf") &&
                    (caseSet != "capsule-failures" && caseSet != "capsule-prefix" || component != "sdf"))
                    summary["cases"].push_back(RunCase(assets, source, component,
                        scenarioFilter == "head" ? "head" : "static",
                        mode == "probe" ? (caseSet == "capsule-failures" ? 1 : 8) : 0,
                        output, mode == "preflight", caseSet != "all"));
        auto checkpoint = Open(output / "summary.json");
        checkpoint << summary.dump(2);
    }
    return 0;
}
}

int main(int argc, char** argv)
{
    try
    {
        return Run(argc, argv);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
        return 2;
    }
}
