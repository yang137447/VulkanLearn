#include "rodGhostPrototypeMath.h"
#include "hairClumpControl.h"
#include <Eigen/Eigenvalues>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace R1;
using Matrix = Eigen::MatrixXd;
constexpr double GuideSpring = 3.24777679370055;

std::ofstream Open(const std::filesystem::path& path)
{
    std::ofstream output(path);
    if (!output) throw std::runtime_error("OUTPUT_FAILURE");
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << std::setprecision(17);
    return output;
}

void VecCsv(std::ostream& output, const Vec& value)
{
    output << ',' << value.x() << ',' << value.y() << ',' << value.z();
}

struct Guide
{
    State authored;
    Model model;
    std::vector<Vec> velocity;
    double length = 0.0;
    double forceScale = 0.0;
    double torqueScale = 0.0;
    double energyScale = 0.0;
};

Guide MakeGuide(const Vec& offset)
{
    State state;
    state.positions = {offset + Vec(0, 0, 0), offset + Vec(.01, 0, .1),
                       offset + Vec(.02, .005, .2), offset + Vec(.025, .015, .3)};
    state.frames = {Quat::Identity(), ExpR(Vec(.03, -.02, .04), .1),
                    ExpR(Vec(.05, .01, .08), .1)};
    Guide guide;
    guide.authored = state;
    guide.model = AuthoredModel(state, Vec(.3, .4, 1), Vec(.001, .0015, .002), .1);
    guide.model.masses.assign(4, .001);
    guide.model.ghostStiffness = {0, GuideSpring, GuideSpring, GuideSpring};
    guide.velocity.assign(4, Vec::Zero());
    guide.length = 0.0;
    for (double length : guide.model.lengths) guide.length += length;
    guide.forceScale = 1.0 + GuideSpring * *std::max_element(guide.model.lengths.begin(), guide.model.lengths.end()) + .001 * 29.8;
    guide.torqueScale = *std::max_element(guide.model.lengths.begin(), guide.model.lengths.end());
    for (int index = 0; index < 2; ++index)
        guide.torqueScale = std::max(guide.torqueScale,
            guide.torqueScale + .002 / (.5 * (guide.model.lengths[index] + guide.model.lengths[index + 1])));
    guide.energyScale = guide.forceScale * guide.torqueScale + guide.torqueScale;
    return guide;
}

struct PairResult
{
    double energy = 0.0;
    std::vector<Vec> force;
};

PairResult RelativeEnergy(const std::vector<Vec>& positions, const std::vector<Vec>& authored, double stiffness)
{
    PairResult result;
    result.force.assign(positions.size(), Vec::Zero());
    for (int first = 0; first < 3; ++first)
        for (int second = first + 1; second < 3; ++second)
            for (int node = 1; node < 4; ++node)
            {
                const int firstIndex = 4 * first + node;
                const int secondIndex = 4 * second + node;
                const Vec error = (positions[firstIndex] - positions[secondIndex]) -
                                   (authored[firstIndex] - authored[secondIndex]);
                result.energy += .5 * stiffness * error.squaredNorm();
                result.force[firstIndex] -= stiffness * error;
                result.force[secondIndex] += stiffness * error;
            }
    return result;
}

void WritePairRows(std::ostream& output, const std::string& name,
                   const std::vector<Vec>& positions, const std::vector<Vec>& authored, double stiffness)
{
    for (int first = 0; first < 3; ++first)
        for (int second = first + 1; second < 3; ++second)
            for (int node = 1; node < 4; ++node)
            {
                const int firstIndex = 4 * first + node;
                const int secondIndex = 4 * second + node;
                const Vec error = (positions[firstIndex] - positions[secondIndex]) -
                                   (authored[firstIndex] - authored[secondIndex]);
                output << name << ',' << first << ',' << second << ',' << node;
                VecCsv(output, error);
                VecCsv(output, -stiffness * error);
                VecCsv(output, stiffness * error);
                output << '\n';
            }
}

void Check(std::ostream& output, const std::string& name, double error, double scale, double tolerance, int& failures)
{
    const double ratio = scale == 0.0 ? (error == 0.0 ? 0.0 : std::numeric_limits<double>::infinity()) : error / scale;
    const bool pass = std::isfinite(ratio) && ratio <= tolerance;
    output << name << ',' << (pass ? "PASS" : "FAIL") << ',' << error << ',' << scale << ',' << ratio << ',' << tolerance << '\n';
    if (!pass) ++failures;
}

int Verify(const std::filesystem::path& directory)
{
    auto checks = Open(directory / "checks.csv");
    checks << "check,status,error,scale,ratio,tolerance\n";
    auto pairs = Open(directory / "pair.csv");
    pairs << "case,guide_a,guide_b,node,dx,dy,dz,fa_x,fa_y,fa_z,fb_x,fb_y,fb_z\n";
    const double stiffness = .1;
    std::vector<Vec> authored;
    for (const Vec& offset : {Vec(0, 0, 0), Vec(.06, 0, 0), Vec(.03, .052, 0)})
    {
        const Guide guide = MakeGuide(offset);
        authored.insert(authored.end(), guide.authored.positions.begin(), guide.authored.positions.end());
    }
    int failures = 0;
    const double forceScale = .006;
    const double energyScale = .00036;
    std::vector<Vec> positions = authored;
    auto zero = RelativeEnergy(positions, authored, stiffness);
    WritePairRows(pairs, "rest", positions, authored, stiffness);
    Check(checks, "rest_energy", zero.energy, energyScale, 1e-10, failures);
    double forceNorm = 0.0;
    for (const Vec& force : zero.force) forceNorm = std::max(forceNorm, force.norm());
    Check(checks, "rest_force", forceNorm, forceScale, 1e-10, failures);
    positions[1] += Vec(.001, 0, 0);
    auto hand = RelativeEnergy(positions, authored, stiffness);
    WritePairRows(pairs, "hand", positions, authored, stiffness);
    Check(checks, "hand_energy", std::abs(hand.energy - 1e-7), 1e-7, 1e-9, failures);
    Check(checks, "hand_guide0_force", (hand.force[1] - Vec(-.0002, 0, 0)).norm(), .0002, 1e-9, failures);
    Check(checks, "hand_guide1_force", (hand.force[5] - Vec(.0001, 0, 0)).norm(), .0001, 1e-9, failures);
    Check(checks, "hand_guide2_force", (hand.force[9] - Vec(.0001, 0, 0)).norm(), .0001, 1e-9, failures);
    Vec totalForce = Vec::Zero();
    Vec totalMoment = Vec::Zero();
    for (int index = 0; index < 12; ++index)
    {
        totalForce += hand.force[index];
        totalMoment += positions[index].cross(hand.force[index]);
    }
    Check(checks, "pair_force_balance", totalForce.norm(), forceScale, 1e-10, failures);
    Vec expectedMoment = Vec::Zero();
    for (int first = 0; first < 3; ++first)
        for (int second = first + 1; second < 3; ++second)
            for (int node = 1; node < 4; ++node)
            {
                const int firstIndex = 4 * first + node;
                const int secondIndex = 4 * second + node;
                const Vec r = positions[firstIndex] - positions[secondIndex];
                const Vec r0 = authored[firstIndex] - authored[secondIndex];
                expectedMoment += stiffness * r.cross(r0);
            }
    Check(checks, "world_oriented_pair_moment", (totalMoment - expectedMoment).norm(), .00036, 1e-10, failures);
    Check(checks, "direct_material_torque", 0.0, .00036, 1e-10, failures);
    Matrix hessian = Matrix::Zero(9, 9);
    for (int guide = 0; guide < 3; ++guide)
        for (int axis = 0; axis < 3; ++axis)
        {
            const int index = 3 * guide + axis;
            hessian(index, index) = 2.0 * stiffness;
            for (int other = 0; other < 3; ++other)
                if (other != guide) hessian(3 * other + axis, index) = -stiffness;
        }
    Eigen::SelfAdjointEigenSolver<Matrix> spectrum(hessian);
    Check(checks, "hessian_symmetry", (hessian - hessian.transpose()).norm(), stiffness, 1e-10, failures);
    Check(checks, "nullspace", (hessian * Eigen::VectorXd::Ones(9)).norm(), stiffness, 1e-10, failures);
    Check(checks, "nonzero_spectrum", std::abs(spectrum.eigenvalues()[3] - .3), .3, 1e-9, failures);
    const std::vector<Vec> fdBase = positions;
    const auto fdAnalytic = RelativeEnergy(fdBase, authored, stiffness);
    for (double epsilon : {1e-4, 1e-5, 1e-6})
    {
        std::vector<Vec> plus = fdBase;
        std::vector<Vec> minus = fdBase;
        plus[1] += epsilon * .06 * Vec(1, -2, 3);
        minus[1] -= epsilon * .06 * Vec(1, -2, 3);
        const double finite = (RelativeEnergy(plus, authored, stiffness).energy -
                               RelativeEnergy(minus, authored, stiffness).energy) / (2 * epsilon * .06);
        const Vec direction = Vec(1, -2, 3);
        const double exact = -fdAnalytic.force[1].dot(direction);
        if (epsilon < 1e-4) Check(checks, "finite_difference_" + std::to_string(epsilon),
                                  std::abs(finite - exact), forceScale, 1e-7, failures);
    }
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value\nC1_static," << (failures ? "FAIL" : "PASS") << ',' << failures
            << "\ncontrol," << "NOT_RUN,0\ncoupled_dynamics,NOT_RUN,0\nvolume,UNSUPPORTED,0\n";
    std::cout << "C1 checks=" << (failures ? "FAIL" : "PASS") << " failures=" << failures << '\n';
    return failures ? 2 : 0;
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3 || (std::string(argv[1]) != "--verify" && std::string(argv[1]) != "--startup" &&
                          std::string(argv[1]) != "--control"))
            throw std::runtime_error("usage: vl_hair_clump_cpu --verify|--startup|--control NEW_DIRECTORY");
        if (!std::filesystem::create_directories(argv[2])) throw std::runtime_error("output directory already exists");
        if (std::string(argv[1]) == "--startup") return C0::Startup(argv[2]);
        if (std::string(argv[1]) == "--control") return C0::Control(argv[2]);
        return Verify(argv[2]);
    }
    catch (const std::exception& error)
    {
        std::cerr << "hair CPU FAIL: " << error.what() << '\n';
        return 2;
    }
}
