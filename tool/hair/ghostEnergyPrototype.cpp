#include <Eigen/Dense>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Vector3 = Eigen::Vector3d;
using Matrix3 = Eigen::Matrix3d;

struct Input
{
    std::string name;
    double mass = 1.0;
    double step = 0.1;
    double stiffness = 100.0;
    double damping = 0.0;
    Vector3 position = Vector3::Zero();
    Vector3 velocity = Vector3::Zero();
    Vector3 ghost = Vector3::Zero();
    Vector3 ghostNext = Vector3::Zero();
    Vector3 force = Vector3::Zero();
    Vector3 expected = Vector3::Zero();
    bool zeroEquilibrium = false;
    bool handOracle = true;
};

struct Result
{
    double diagonal;
    Vector3 rhs;
    Vector3 velocity;
    Vector3 denseVelocity;
    Vector3 position;
    Vector3 offset;
    Vector3 elastic;
    double energyOld;
    double energyNext;
    double externalWork;
    double ghostWork;
    double dampingLoss;
    double velocityLoss;
    double elasticLoss;
    double balance;
    double energyScale;
    double velocityScale;
    double forceScale;
    double forceResidual;
    double residual;
    double denseDifference;
    double assemblyDifference;
};

double Ratio(double numerator, double denominator)
{
    if (!std::isfinite(numerator) || !std::isfinite(denominator) || denominator < 0.0)
        return std::numeric_limits<double>::infinity();
    if (denominator == 0.0)
        return numerator == 0.0 ? 0.0 : std::numeric_limits<double>::infinity();
    return numerator / denominator;
}

void Validate(const Input& input)
{
    if (!std::isfinite(input.mass) || input.mass <= 0.0 ||
        !std::isfinite(input.step) || input.step <= 0.0 ||
        !std::isfinite(input.stiffness) || input.stiffness < 0.0 ||
        !std::isfinite(input.damping) || input.damping < 0.0 ||
        !input.position.allFinite() || !input.velocity.allFinite() ||
        !input.ghost.allFinite() || !input.ghostNext.allFinite() || !input.force.allFinite())
        throw std::runtime_error("invalid G0 input");
}

Result Evaluate(const Input& input)
{
    Validate(input);
    Result result;
    const Vector3 oldOffset = input.ghost - input.position;
    const Vector3 displacement = input.ghostNext - input.ghost;
    result.diagonal = input.mass + input.step * input.damping +
                      input.step * input.step * input.stiffness;
    result.rhs = input.mass * input.velocity + input.step * input.force +
                 input.step * input.stiffness * oldOffset +
                 input.step * input.stiffness * displacement;
    result.velocity = result.rhs / result.diagonal;
    const Matrix3 mass = input.mass * Matrix3::Identity();
    const Matrix3 damping = input.damping * Matrix3::Identity();
    const Matrix3 stiffness = input.stiffness * Matrix3::Identity();
    const Matrix3 dense = mass + input.step * damping + input.step * input.step * stiffness;
    const Vector3 denseRhs = mass * input.velocity + input.step *
        (input.force + stiffness * (input.ghostNext - input.position));
    Eigen::FullPivLU<Matrix3> decomposition(dense);
    if (!decomposition.isInvertible())
        throw std::runtime_error("noninvertible G0 matrix");
    result.denseVelocity = decomposition.solve(denseRhs);
    result.position = input.position + input.step * result.velocity;
    result.offset = input.ghostNext - result.position;
    result.elastic = input.stiffness * result.offset;
    result.energyOld = 0.5 * input.mass * input.velocity.squaredNorm() +
                       0.5 * input.stiffness * oldOffset.squaredNorm();
    result.energyNext = 0.5 * input.mass * result.velocity.squaredNorm() +
                        0.5 * input.stiffness * result.offset.squaredNorm();
    result.externalWork = input.step * input.force.dot(result.velocity);
    result.ghostWork = result.elastic.dot(displacement);
    result.dampingLoss = input.step * input.damping * result.velocity.squaredNorm();
    result.velocityLoss = 0.5 * input.mass * (result.velocity - input.velocity).squaredNorm();
    result.elasticLoss = 0.5 * input.stiffness * (result.offset - oldOffset).squaredNorm();
    result.balance = result.energyNext - result.energyOld - result.externalWork -
                     result.ghostWork + result.dampingLoss + result.velocityLoss + result.elasticLoss;
    result.energyScale = std::abs(result.energyNext) + std::abs(result.energyOld) +
                         std::abs(result.externalWork) + std::abs(result.ghostWork) +
                         result.dampingLoss + result.velocityLoss + result.elasticLoss;
    result.forceScale = input.force.norm() + input.stiffness * oldOffset.norm();
    result.forceResidual = (input.force + input.stiffness * oldOffset).norm();
    result.velocityScale = input.velocity.norm() + input.step / input.mass *
        (input.force.norm() + input.stiffness * (oldOffset.norm() + displacement.norm()));
    const Matrix3 matrix = result.diagonal * Matrix3::Identity();
    result.residual = Ratio((matrix * result.velocity - result.rhs).norm(),
        matrix.norm() * result.velocity.norm() + result.rhs.norm());
    result.denseDifference = (result.velocity - result.denseVelocity).norm();
    const double rhsScale = input.mass * input.velocity.norm() + input.step *
        (input.force.norm() + input.stiffness * (oldOffset.norm() + displacement.norm()));
    result.assemblyDifference = Ratio((result.rhs - denseRhs).norm(), rhsScale);
    if (!std::isfinite(result.diagonal) || !result.rhs.allFinite() ||
        !result.velocity.allFinite() || !result.denseVelocity.allFinite() ||
        !result.position.allFinite() || !result.elastic.allFinite() ||
        !std::isfinite(result.energyScale) || !std::isfinite(result.balance))
        throw std::runtime_error("nonfinite G0 result");
    return result;
}

std::ofstream Open(const std::filesystem::path& path)
{
    std::ofstream output(path);
    if (!output)
        throw std::runtime_error("cannot create output");
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << std::setprecision(17);
    return output;
}

void WriteVector(std::ostream& output, const Vector3& value)
{
    output << ',' << value.x() << ',' << value.y() << ',' << value.z();
}

struct Outputs
{
    std::ofstream inputs;
    std::ofstream results;
    std::ofstream checks;
    int failures = 0;
    int checkCount = 0;
    int caseCount = 0;

    explicit Outputs(const std::filesystem::path& directory)
        : inputs(Open(directory / "inputs.csv")), results(Open(directory / "results.csv")),
          checks(Open(directory / "checks.csv"))
    {
        inputs << "case,mass,h,k,gamma,x,y,z,vx,vy,vz,ghost_x,ghost_y,ghost_z,"
                  "ghost_next_x,ghost_next_y,ghost_next_z,Fx,Fy,Fz,expected_vx,expected_vy,expected_vz,"
                  "zero_equilibrium,hand_oracle\n";
        results << "case,A_diagonal,bx,by,bz,vx,vy,vz,dense_vx,dense_vy,dense_vz,"
                   "x_next,y_next,z_next,rx,ry,rz,fx,fy,fz,ghost_reaction_x,ghost_reaction_y,ghost_reaction_z,"
                   "H_old,H_next,external_work,ghost_work,damping_loss,numerical_velocity_loss,"
                   "numerical_elastic_loss,balance_error,energy_scale,input_velocity_scale,force_scale,"
                   "force_residual,linear_residual,dense_absolute_difference,dense_reference_norm,"
                   "rhs_assembly_scaled_difference,condition\n";
        checks << "case,check,status,numerator,denominator,scaled_error,tolerance\n";
    }

    void Check(const std::string& name, const std::string& check, double numerator, double denominator, double tolerance)
    {
        const double scaled = Ratio(numerator, denominator);
        const bool passed = std::isfinite(scaled) && scaled <= tolerance;
        checks << name << ',' << check << ',' << (passed ? "PASS" : "FAIL") << ','
               << numerator << ',' << denominator << ',' << scaled << ',' << tolerance << '\n';
        ++checkCount;
        if (!passed)
            ++failures;
    }

    Result Run(const Input& input)
    {
        const Result result = Evaluate(input);
        ++caseCount;
        inputs << input.name << ',' << input.mass << ',' << input.step << ',' << input.stiffness << ',' << input.damping;
        WriteVector(inputs, input.position);
        WriteVector(inputs, input.velocity);
        WriteVector(inputs, input.ghost);
        WriteVector(inputs, input.ghostNext);
        WriteVector(inputs, input.force);
        WriteVector(inputs, input.expected);
        inputs << ',' << input.zeroEquilibrium << ',' << input.handOracle << '\n';
        results << input.name << ',' << result.diagonal;
        for (const Vector3& value : {result.rhs, result.velocity, result.denseVelocity,
                                     result.position, result.offset, result.elastic, Vector3(-result.elastic)})
            WriteVector(results, value);
        results << ',' << result.energyOld << ',' << result.energyNext << ',' << result.externalWork
                << ',' << result.ghostWork << ',' << result.dampingLoss << ',' << result.velocityLoss
                << ',' << result.elasticLoss << ',' << result.balance << ',' << result.energyScale
                << ',' << result.velocityScale << ',' << result.forceScale << ',' << result.forceResidual
                << ',' << result.residual << ',' << result.denseDifference << ',' << result.denseVelocity.norm()
                << ',' << result.assemblyDifference << ",1\n";
        Check(input.name, "same_A_b_residual", result.residual, 1.0, 1e-10);
        Check(input.name, "energy_identity", std::abs(result.balance), result.energyScale, 1e-10);
        Check(input.name, "independent_rhs_assembly", result.assemblyDifference, 1.0, 1e-10);
        if (input.zeroEquilibrium)
        {
            if (input.velocity.norm() != 0.0 || (input.ghostNext - input.ghost).norm() != 0.0)
                throw std::runtime_error("zero equilibrium fixture misdeclared");
            Check(input.name, "equilibrium_force", result.forceResidual, result.forceScale, 1e-10);
            Check(input.name, "zero_velocity_scaled", result.velocity.norm(), result.velocityScale, 1e-9);
            Check(input.name, "dense_zero_velocity_scaled", result.denseVelocity.norm(), result.velocityScale, 1e-9);
        }
        else
            Check(input.name, "dense_solution_relative", result.denseDifference, result.denseVelocity.norm(), 1e-9);
        if (input.handOracle)
            Check(input.name, "hand_velocity", (result.velocity - input.expected).norm(),
                  input.zeroEquilibrium ? result.velocityScale : input.expected.norm(), 1e-9);
        return result;
    }
};

std::vector<Input> Fixtures()
{
    std::vector<Input> fixtures;
    Input stationary;
    stationary.name = "stationary";
    stationary.position.x() = 1.1;
    stationary.ghost.x() = 1.0;
    stationary.ghostNext = stationary.ghost;
    stationary.expected.x() = -0.5;
    fixtures.push_back(stationary);
    Input moving;
    moving.name = "coincident-moving";
    moving.ghostNext.x() = 0.02;
    moving.expected.x() = 0.1;
    fixtures.push_back(moving);
    moving.name = "moving-3d";
    moving.ghostNext = Vector3(0.02, 0.03, -0.01);
    moving.expected = Vector3(0.1, 0.15, -0.05);
    fixtures.push_back(moving);
    Input comoving;
    comoving.name = "comoving";
    comoving.mass = 2.0;
    comoving.step = 0.125;
    comoving.stiffness = 8.0;
    comoving.velocity = Vector3(1.0, -2.0, 3.0);
    comoving.ghostNext = comoving.step * comoving.velocity;
    comoving.expected = comoving.velocity;
    fixtures.push_back(comoving);
    Input free;
    free.name = "zero-stiffness";
    free.mass = 2.0;
    free.step = 0.5;
    free.stiffness = 0.0;
    free.damping = 3.0;
    free.velocity = Vector3(1.0, -2.0, 3.0);
    free.ghostNext = Vector3(7.0, -9.0, 2.0);
    free.force = Vector3(4.0, 2.0, -6.0);
    free.expected = Vector3(4.0, -3.0, 3.0) / 3.5;
    fixtures.push_back(free);
    for (double step : {0.001, 0.1, 2.0})
        for (double damping : {0.0, 0.3, 10.0})
        {
            Input equilibrium;
            equilibrium.name = "integrity-only-h" + std::to_string(step) + "-gamma" + std::to_string(damping);
            equilibrium.mass = 0.01;
            equilibrium.step = step;
            equilibrium.damping = damping;
            equilibrium.ghost = Vector3(0.0, 0.001, 0.0);
            equilibrium.ghostNext = equilibrium.ghost;
            equilibrium.force = Vector3(0.0, -0.1, 0.0);
            equilibrium.zeroEquilibrium = true;
            fixtures.push_back(equilibrium);
        }
    Input general;
    general.name = "nonunit-3d";
    general.mass = 2.0;
    general.step = 0.3;
    general.stiffness = 8.0;
    general.damping = 0.2;
    general.position = Vector3(0.1, -0.2, 0.3);
    general.velocity = Vector3(0.2, 0.4, -0.5);
    general.ghost = Vector3(0.3, -0.1, 0.2);
    general.ghostNext = Vector3(0.4, 0.2, -0.3);
    general.force = Vector3(0.7, -0.4, 1.1);
    general.expected = Vector3(1.33, 1.64, -2.11) / 2.78;
    fixtures.push_back(general);
    Input zero;
    zero.name = "coincident-static-zero";
    zero.zeroEquilibrium = true;
    fixtures.push_back(zero);
    free.name = "zero-k-ghost-independent";
    free.ghost = Vector3(2.0, 1.0, -3.0);
    free.ghostNext = Vector3(-1.0, 4.0, 6.0);
    fixtures.push_back(free);
    return fixtures;
}

int Verify(const std::filesystem::path& directory)
{
    Outputs output(directory);
    Result movingResult;
    for (const auto& fixture : Fixtures())
    {
        const Result result = output.Run(fixture);
        if (fixture.name == "coincident-moving")
            movingResult = result;
    }
    output.Check("coincident-moving", "H_next_hand", std::abs(movingResult.energyNext - 0.01), 0.01, 1e-9);
    output.Check("coincident-moving", "ghost_work_hand", std::abs(movingResult.ghostWork - 0.02), 0.02, 1e-9);
    output.Check("coincident-moving", "velocity_loss_hand", std::abs(movingResult.velocityLoss - 0.005), 0.005, 1e-9);
    output.Check("coincident-moving", "elastic_loss_hand", std::abs(movingResult.elasticLoss - 0.005), 0.005, 1e-9);
    auto invalidOutput = Open(directory / "invalid-inputs.csv");
    invalidOutput << "case,status\n";
    for (int variant = 0; variant < 9; ++variant)
    {
        Input invalid;
        if (variant == 0) invalid.step = 0.0;
        if (variant == 1) invalid.mass = 0.0;
        if (variant == 2) invalid.mass = -1.0;
        if (variant == 3) invalid.stiffness = -1.0;
        if (variant == 4) invalid.damping = -1.0;
        if (variant == 5) invalid.position.x() = std::numeric_limits<double>::quiet_NaN();
        if (variant == 6) invalid.ghostNext.z() = std::numeric_limits<double>::infinity();
        if (variant == 7) invalid.step = std::numeric_limits<double>::infinity();
        if (variant == 8) invalid.force.y() = std::numeric_limits<double>::quiet_NaN();
        bool rejected = false;
        try { Evaluate(invalid); }
        catch (const std::runtime_error&) { rejected = true; }
        invalidOutput << variant << ',' << (rejected ? "PASS" : "FAIL") << '\n';
        output.Check("invalid-" + std::to_string(variant), "expected_rejection", rejected ? 0.0 : 1.0, 1.0, 0.0);
    }
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value,detail\nscope,INFO,G0-only-v1,exact_zero_rest_single_particle\n"
            << "fixtures," << (output.failures ? "FAIL" : "PASS") << ',' << output.caseCount << ",fixed_inputs\n"
            << "checks," << (output.failures ? "FAIL" : "PASS") << ',' << output.checkCount << ",protocol_v1\n"
            << "failed_checks,INFO," << output.failures << ",count\n";
    for (const std::string name : {"angular", "relativeDashpot", "localBlock7Combination", "jointPreload",
                                  "ghostDynamics", "completeAMS", "materialTwist", "volume", "collision"})
        summary << name << ",UNSUPPORTED,NA,outside_G0_release\n";
    std::cout << "G0-only-v1 fixtures=" << output.caseCount << " checks=" << output.checkCount
              << " failures=" << output.failures << '\n';
    return output.failures ? 2 : 0;
}

int main(int argc, char** argv)
{
    bool created = false;
    try
    {
        if (argc != 3 || std::string(argv[1]) != "--verify")
            throw std::runtime_error("usage: vl_hair_ghost_energy --verify NEW_OUTPUT_DIRECTORY");
        if (!std::filesystem::create_directories(std::filesystem::path(argv[2])))
            throw std::runtime_error("output directory already exists");
        created = true;
        return Verify(argv[2]);
    }
    catch (const std::exception& error)
    {
        std::cerr << "G0-only-v1 FAIL: " << error.what() << '\n';
        if (created)
        {
            std::ofstream failure(std::filesystem::path(argv[2]) / "failure.txt");
            failure << error.what() << '\n';
        }
        return 2;
    }
}
