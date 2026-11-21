#include <Eigen/Dense>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Vector3 = Eigen::Vector3d;
using Matrix3 = Eigen::Matrix3d;
using Vector = Eigen::VectorXd;
using Matrix = Eigen::MatrixXd;

struct Particle
{
    Vector3 position = Vector3::Zero();
    Vector3 velocity = Vector3::Zero();
    double mass = 1.0;
    double damping = 0.0;
    bool fixed = false;
};

struct Edge
{
    int first;
    int second;
    double rest;
    double stiffness;
};

struct Chain
{
    std::vector<Particle> particles;
    std::vector<Edge> edges;
};

struct BlockSystem
{
    std::vector<std::array<Matrix3, 7>> blocks;
    Vector rhs;

    explicit BlockSystem(int count) : blocks(count), rhs(Vector::Zero(3 * count))
    {
        for (auto& row : blocks)
            for (auto& block : row)
                block.setZero();
    }

    Matrix3& At(int row, int column)
    {
        return blocks.at(row).at(column - row + 3);
    }

    Matrix Dense() const
    {
        const int count = static_cast<int>(blocks.size());
        Matrix dense = Matrix::Zero(3 * count, 3 * count);
        for (int row = 0; row < count; ++row)
            for (int column = std::max(0, row - 3); column <= std::min(count - 1, row + 3); ++column)
                dense.block<3, 3>(3 * row, 3 * column) = blocks[row][column - row + 3];
        return dense;
    }

    Vector Multiply(const Vector& value) const
    {
        Vector result = Vector::Zero(value.size());
        const int count = static_cast<int>(blocks.size());
        for (int row = 0; row < count; ++row)
            for (int column = std::max(0, row - 3); column <= std::min(count - 1, row + 3); ++column)
                result.segment<3>(3 * row) += blocks[row][column - row + 3] * value.segment<3>(3 * column);
        return result;
    }
};

void Validate(const Chain& chain, double step)
{
    if (chain.particles.empty() || !std::isfinite(step) || step <= 0.0)
        throw std::runtime_error("invalid count or step");
    for (const auto& particle : chain.particles)
        if (!particle.position.allFinite() || !particle.velocity.allFinite() ||
            !std::isfinite(particle.mass) || particle.mass <= 0.0 ||
            !std::isfinite(particle.damping) || particle.damping < 0.0)
            throw std::runtime_error("invalid particle");
    for (const auto& edge : chain.edges)
    {
        if (edge.first < 0 || edge.second >= static_cast<int>(chain.particles.size()) ||
            edge.second <= edge.first || edge.second - edge.first > 3 ||
            !std::isfinite(edge.rest) || edge.rest <= 0.0 ||
            !std::isfinite(edge.stiffness) || edge.stiffness < 0.0)
            throw std::runtime_error("invalid edge");
        const double length = (chain.particles[edge.second].position - chain.particles[edge.first].position).norm();
        if (!std::isfinite(length) || length <= 0.0)
            throw std::runtime_error("degenerate edge");
    }
}

BlockSystem Assemble(const Chain& chain, double step, const Vector3& acceleration)
{
    Validate(chain, step);
    if (!acceleration.allFinite())
        throw std::runtime_error("invalid acceleration");
    const int count = static_cast<int>(chain.particles.size());
    BlockSystem system(count);
    for (int index = 0; index < count; ++index)
    {
        const auto& particle = chain.particles[index];
        system.At(index, index) = (particle.mass + step * particle.damping) * Matrix3::Identity();
        system.rhs.segment<3>(3 * index) = particle.mass * (particle.velocity + step * acceleration);
    }
    for (const auto& edge : chain.edges)
    {
        const Vector3 offset = chain.particles[edge.second].position - chain.particles[edge.first].position;
        const double length = offset.norm();
        const Vector3 direction = offset / length;
        const Matrix3 contribution = step * step * edge.stiffness * direction * direction.transpose();
        const Vector3 force = edge.stiffness * (length - edge.rest) * direction;
        system.At(edge.first, edge.first) += contribution;
        system.At(edge.second, edge.second) += contribution;
        system.At(edge.first, edge.second) -= contribution;
        system.At(edge.second, edge.first) -= contribution;
        system.rhs.segment<3>(3 * edge.first) += step * force;
        system.rhs.segment<3>(3 * edge.second) -= step * force;
    }
    for (int fixed = 0; fixed < count; ++fixed)
    {
        if (!chain.particles[fixed].fixed)
            continue;
        for (int neighbor = std::max(0, fixed - 3); neighbor <= std::min(count - 1, fixed + 3); ++neighbor)
        {
            if (neighbor == fixed)
                continue;
            system.rhs.segment<3>(3 * neighbor) -= system.At(neighbor, fixed) * chain.particles[fixed].velocity;
            system.At(neighbor, fixed).setZero();
            system.At(fixed, neighbor).setZero();
        }
        system.At(fixed, fixed).setIdentity();
        system.rhs.segment<3>(3 * fixed) = chain.particles[fixed].velocity;
    }
    return system;
}

Vector SolveBand(const BlockSystem& system)
{
    const int count = static_cast<int>(system.blocks.size());
    std::vector<std::array<Matrix3, 4>> lower(count);
    for (auto& row : lower)
        for (auto& block : row)
            block.setZero();
    for (int row = 0; row < count; ++row)
    {
        for (int column = std::max(0, row - 3); column <= row; ++column)
        {
            Matrix3 remainder = system.blocks[row][column - row + 3];
            for (int inner = std::max(0, row - 3); inner < column; ++inner)
                remainder -= lower[row][row - inner] * lower[column][column - inner].transpose();
            if (column == row)
            {
                Eigen::LLT<Matrix3> diagonal(remainder);
                if (diagonal.info() != Eigen::Success)
                    throw std::runtime_error("block Cholesky failed");
                lower[row][0] = diagonal.matrixL();
            }
            else
                lower[row][row - column] =
                    lower[column][0].triangularView<Eigen::Lower>().solve(remainder.transpose()).transpose();
        }
    }
    Vector result = system.rhs;
    for (int row = 0; row < count; ++row)
    {
        Vector3 value = result.segment<3>(3 * row);
        for (int column = std::max(0, row - 3); column < row; ++column)
            value -= lower[row][row - column] * result.segment<3>(3 * column);
        result.segment<3>(3 * row) = lower[row][0].triangularView<Eigen::Lower>().solve(value);
    }
    for (int row = count - 1; row >= 0; --row)
    {
        Vector3 value = result.segment<3>(3 * row);
        for (int column = row + 1; column <= std::min(count - 1, row + 3); ++column)
            value -= lower[column][column - row].transpose() * result.segment<3>(3 * column);
        result.segment<3>(3 * row) = lower[row][0].transpose().triangularView<Eigen::Upper>().solve(value);
    }
    if (!result.allFinite())
        throw std::runtime_error("nonfinite solution");
    return result;
}

struct DenseResult
{
    Matrix matrix;
    Vector rhs;
    Vector velocity;
    double condition = 0.0;
    double rhsEvaluationScale = 0.0;
};

DenseResult DenseReference(const Chain& chain, double step, const Vector3& acceleration)
{
    const int dimension = 3 * static_cast<int>(chain.particles.size());
    Matrix mass = Matrix::Zero(dimension, dimension);
    Matrix damping = mass;
    Matrix stiffness = mass;
    Vector oldVelocity(dimension);
    Vector force = Vector::Zero(dimension);
    double springForceScale = 0.0;
    for (int index = 0; index < static_cast<int>(chain.particles.size()); ++index)
    {
        mass.block<3, 3>(3 * index, 3 * index) = chain.particles[index].mass * Matrix3::Identity();
        damping.block<3, 3>(3 * index, 3 * index) = chain.particles[index].damping * Matrix3::Identity();
        oldVelocity.segment<3>(3 * index) = chain.particles[index].velocity;
        force.segment<3>(3 * index) = chain.particles[index].mass * acceleration;
    }
    for (const auto& edge : chain.edges)
    {
        Matrix incidence = Matrix::Zero(3, dimension);
        incidence.block<3, 3>(0, 3 * edge.first) = -Matrix3::Identity();
        incidence.block<3, 3>(0, 3 * edge.second) = Matrix3::Identity();
        const Vector3 delta = chain.particles[edge.second].position - chain.particles[edge.first].position;
        const Vector3 unit = delta.normalized();
        stiffness += edge.stiffness * incidence.transpose() * (unit * unit.transpose()) * incidence;
        force -= incidence.transpose() * (edge.stiffness * (delta.norm() - edge.rest) * unit);
        springForceScale += std::sqrt(2.0) * edge.stiffness * std::abs(delta.norm() - edge.rest);
    }
    DenseResult result;
    const Matrix original = mass + step * damping + step * step * stiffness;
    const Vector originalRhs = mass * oldVelocity + step * force;
    result.rhsEvaluationScale = (mass * oldVelocity).norm() + step * springForceScale;
    for (const auto& particle : chain.particles)
        result.rhsEvaluationScale += step * particle.mass * acceleration.norm();
    std::vector<int> free;
    std::vector<int> fixed;
    for (int coordinate = 0; coordinate < dimension; ++coordinate)
        (chain.particles[coordinate / 3].fixed ? fixed : free).push_back(coordinate);
    Matrix reduced(free.size(), free.size());
    Vector reducedRhs(free.size());
    result.matrix = Matrix::Identity(dimension, dimension);
    result.rhs = oldVelocity;
    result.velocity = oldVelocity;
    for (int row = 0; row < static_cast<int>(free.size()); ++row)
    {
        reducedRhs[row] = originalRhs[free[row]];
        for (int coordinate : fixed)
        {
            reducedRhs[row] -= original(free[row], coordinate) * oldVelocity[coordinate];
            result.rhsEvaluationScale += std::abs(original(free[row], coordinate) * oldVelocity[coordinate]);
        }
        for (int column = 0; column < static_cast<int>(free.size()); ++column)
            reduced(row, column) = original(free[row], free[column]);
    }
    for (int coordinate : free)
        result.matrix.row(coordinate).setZero();
    if (!free.empty())
    {
        Eigen::FullPivLU<Matrix> decomposition(reduced);
        if (!decomposition.isInvertible())
            throw std::runtime_error("dense reference singular");
        const Vector solution = decomposition.solve(reducedRhs);
        Eigen::JacobiSVD<Matrix> singular(reduced);
        result.condition = singular.singularValues()[0] / singular.singularValues().tail(1)[0];
        for (int row = 0; row < static_cast<int>(free.size()); ++row)
        {
            result.rhs[free[row]] = reducedRhs[row];
            result.velocity[free[row]] = solution[row];
            for (int column = 0; column < static_cast<int>(free.size()); ++column)
                result.matrix(free[row], free[column]) = reduced(row, column);
        }
    }
    return result;
}

std::ofstream OpenCsv(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    if (!stream)
        throw std::runtime_error("cannot open output: " + path.string());
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << std::setprecision(17);
    return stream;
}

void WriteUnsupported(std::ostream& summary)
{
    for (const std::string name : {"completeAMS", "integrity", "ghostDynamics", "angular", "preload",
                                  "materialTwist", "volume", "inextensibility", "FLIP_PIC",
                                  "hairHairCollision", "hairSolidCollision"})
        summary << name << ",UNSUPPORTED,NA,outside_L2-local-001\n";
}

void Check(std::ostream& output, const std::string& name, double error, double tolerance, int& failures)
{
    const bool passed = std::isfinite(error) && error <= tolerance;
    output << name << ',' << (passed ? "PASS" : "FAIL") << ',' << error << ',' << tolerance << '\n';
    std::cout << name << '=' << (passed ? "PASS" : "FAIL") << " error=" << error << '\n';
    if (!passed)
        ++failures;
}

void WriteSystem(std::ostream& matrices, std::ostream& vectors, const std::string& name,
                 const BlockSystem& system, const Vector& velocity, const DenseResult& reference)
{
    const Matrix dense = system.Dense();
    for (int row = 0; row < dense.rows(); ++row)
    {
        vectors << name << ',' << row << ',' << system.rhs[row] << ',' << reference.rhs[row]
                << ',' << velocity[row] << ',' << reference.velocity[row] << '\n';
        for (int column = 0; column < dense.cols(); ++column)
            if (dense(row, column) != 0.0 || reference.matrix(row, column) != 0.0)
                matrices << name << ',' << row << ',' << column << ',' << dense(row, column)
                         << ',' << reference.matrix(row, column) << '\n';
    }
}

void WriteInput(std::ostream& nodes, std::ostream& edges, const std::string& name, const Chain& chain)
{
    for (int index = 0; index < static_cast<int>(chain.particles.size()); ++index)
    {
        const auto& particle = chain.particles[index];
        nodes << name << ',' << index << ',' << particle.fixed << ',' << particle.mass << ',' << particle.damping;
        for (int axis = 0; axis < 3; ++axis)
            nodes << ',' << particle.position[axis];
        for (int axis = 0; axis < 3; ++axis)
            nodes << ',' << particle.velocity[axis];
        nodes << '\n';
    }
    for (const auto& edge : chain.edges)
        edges << name << ',' << edge.first << ',' << edge.second << ',' << edge.rest << ',' << edge.stiffness << '\n';
}

double RelativeResidual(const BlockSystem& system, const Vector& solution)
{
    double squaredNorm = 0.0;
    for (const auto& row : system.blocks)
        for (const auto& block : row)
            squaredNorm += block.squaredNorm();
    return (system.Multiply(solution) - system.rhs).norm() /
           (std::sqrt(squaredNorm) * solution.norm() + system.rhs.norm() + 1e-30);
}

Chain MakeFixture(int count, int span)
{
    Chain chain;
    for (int index = 0; index < count; ++index)
    {
        Particle particle;
        particle.position = Vector3(0.1 * index, 0.03 * std::sin(0.8 * index), 0.04 * std::cos(0.6 * index));
        particle.velocity = Vector3(0.02 * index, -0.01 * index, 0.03);
        particle.mass = 0.01 * (1.0 + 0.2 * index);
        particle.damping = 0.03;
        chain.particles.push_back(particle);
    }
    for (int first = 0; first < count; ++first)
        for (int distance = 1; distance <= 3; ++distance)
            if ((span == 0 || span == distance) && first + distance < count)
                chain.edges.push_back({first, first + distance,
                    0.97 * (chain.particles[first + distance].position - chain.particles[first].position).norm(),
                    11.0 + 7.0 * distance});
    return chain;
}

int Verify(const std::filesystem::path& directory)
{
    auto checks = OpenCsv(directory / "checks.csv");
    auto cases = OpenCsv(directory / "cases.csv");
    auto matrices = OpenCsv(directory / "matrices.csv");
    auto vectors = OpenCsv(directory / "vectors.csv");
    auto nodes = OpenCsv(directory / "nodes.csv");
    auto edges = OpenCsv(directory / "edges.csv");
    checks << "check,status,error,tolerance\n";
    cases << "case,h,ax,ay,az,condition_free_mass_matrix\n";
    matrices << "case,row,column,band,independent_dense\n";
    vectors << "case,row,band_rhs,independent_dense_rhs,band_velocity,dense_velocity\n";
    nodes << "case,node,fixed,mass,damping,x,y,z,vx,vy,vz\n";
    edges << "case,first,second,rest,stiffness\n";
    int failures = 0;
    const double step = 0.007;
    const Vector3 acceleration(0.2, -9.8, 0.4);
    for (int count : {1, 2, 3, 4, 8, 32})
        for (int span : {0, 1, 2, 3})
        {
            Chain chain = MakeFixture(count, span);
            if (count > 1)
            {
                chain.particles[0].fixed = true;
                chain.particles[0].velocity = Vector3(0.07, -0.03, 0.02);
                chain.particles[count - 1].fixed = true;
            }
            const std::string name = "N" + std::to_string(count) + "_span" + std::to_string(span);
            const auto system = Assemble(chain, step, acceleration);
            const Vector velocity = SolveBand(system);
            const auto reference = DenseReference(chain, step, acceleration);
            WriteInput(nodes, edges, name, chain);
            WriteSystem(matrices, vectors, name, system, velocity, reference);
            cases << name << ',' << step << ",0.2,-9.8,0.4," << reference.condition << '\n';
            Check(checks, name + "_assembly", (system.Dense() - reference.matrix).norm() /
                  (reference.matrix.norm() + 1e-30), 1e-12, failures);
            Check(checks, name + "_rhs", (system.rhs - reference.rhs).norm() /
                  (reference.rhs.norm() + 1e-30), 1e-12, failures);
            Check(checks, name + "_residual", RelativeResidual(system, velocity), 1e-10, failures);
            Check(checks, name + "_solution", (velocity - reference.velocity).norm() /
                  (reference.velocity.norm() + 1e-30), 1e-9, failures);
            Chain freeChain = chain;
            for (auto& particle : freeChain.particles)
                particle.fixed = false;
            const auto freeSystem = Assemble(freeChain, step, Vector3::Zero());
            Matrix connectivity = freeSystem.Dense();
            for (int index = 0; index < count; ++index)
                connectivity.block<3, 3>(3 * index, 3 * index) -=
                    (freeChain.particles[index].mass + step * freeChain.particles[index].damping) * Matrix3::Identity();
            connectivity /= -step * step;
            const Matrix translation = Matrix::Identity(3, 3).replicate(count, 1);
            Check(checks, name + "_C_translation", (connectivity * translation).norm(), 1e-10, failures);
            Check(checks, name + "_C_symmetric", (connectivity - connectivity.transpose()).norm(), 1e-10, failures);
            Eigen::SelfAdjointEigenSolver<Matrix> spectrum(connectivity);
            Check(checks, name + "_C_NSD", std::max(0.0, spectrum.eigenvalues().maxCoeff()), 1e-10, failures);
            Vector3 totalForce = Vector3::Zero();
            for (int index = 0; index < count; ++index)
                totalForce += (freeSystem.rhs.segment<3>(3 * index) -
                    freeChain.particles[index].mass * freeChain.particles[index].velocity) / step;
            Check(checks, name + "_force_balance", totalForce.norm(), 1e-11, failures);
            double outside = 0.0;
            for (int row = 0; row < count; ++row)
                for (int column = 0; column < count; ++column)
                    if (std::abs(row - column) > 3)
                        outside = std::max(outside, connectivity.block<3, 3>(3 * row, 3 * column).norm());
            Check(checks, name + "_outside_band", outside, 0.0, failures);
        }
    Chain analytic;
    analytic.particles.resize(2);
    analytic.particles[0].fixed = true;
    analytic.particles[1].position = Vector3(1.1, 0.0, 0.0);
    analytic.edges.push_back({0, 1, 1.0, 100.0});
    const auto analyticSystem = Assemble(analytic, 0.1, Vector3::Zero());
    const Vector analyticVelocity = SolveBand(analyticSystem);
    const auto analyticDense = DenseReference(analytic, 0.1, Vector3::Zero());
    WriteInput(nodes, edges, "paper_hand_example", analytic);
    WriteSystem(matrices, vectors, "paper_hand_example", analyticSystem, analyticVelocity, analyticDense);
    cases << "paper_hand_example,0.1,0,0,0," << analyticDense.condition << '\n';
    Check(checks, "paper_hand_velocity", std::abs(analyticVelocity[3] + 0.5), 1e-12, failures);
    Check(checks, "paper_hand_position", std::abs(1.1 + 0.1 * analyticVelocity[3] - 1.05), 1e-12, failures);
    for (double extension : {-0.1, 0.1})
    {
        Chain pair;
        pair.particles.resize(2);
        pair.particles[0].mass = 0.02;
        pair.particles[1].mass = 0.07;
        const Vector3 direction = Vector3(1.0, 2.0, -3.0).normalized();
        pair.particles[1].position = (1.0 + extension) * direction;
        pair.edges.push_back({0, 1, 1.0, 19.0});
        const Vector solution = SolveBand(Assemble(pair, step, Vector3::Zero()));
        const double inverseMassSum = 1.0 / 0.02 + 1.0 / 0.07;
        const double relative = -step * 19.0 * extension * inverseMassSum /
                                (1.0 + step * step * 19.0 * inverseMassSum);
        const std::string name = extension < 0.0 ? "compression" : "extension";
        Check(checks, name + "_relative_velocity",
              ((solution.segment<3>(3) - solution.segment<3>(0)) - relative * direction).norm(), 1e-12, failures);
        Check(checks, name + "_momentum",
              (0.02 * solution.segment<3>(0) + 0.07 * solution.segment<3>(3)).norm(), 1e-12, failures);
        const double epsilon = 1e-6;
        Vector3 numericalForce;
        for (int axis = 0; axis < 3; ++axis)
        {
            Vector3 plus = pair.particles[0].position;
            Vector3 minus = plus;
            plus[axis] += epsilon;
            minus[axis] -= epsilon;
            const double plusEnergy = 0.5 * 19.0 * std::pow((pair.particles[1].position - plus).norm() - 1.0, 2);
            const double minusEnergy = 0.5 * 19.0 * std::pow((pair.particles[1].position - minus).norm() - 1.0, 2);
            numericalForce[axis] = -(plusEnergy - minusEnergy) / (2.0 * epsilon);
        }
        Check(checks, name + "_energy_gradient", (numericalForce - 19.0 * extension * direction).norm(), 1e-8, failures);
    }
    Chain free = MakeFixture(1, 0);
    free.particles[0].damping = 0.0;
    Check(checks, "free_uniform_motion", (SolveBand(Assemble(free, step, Vector3::Zero())) -
          free.particles[0].velocity).norm(), 1e-12, failures);
    Check(checks, "free_gravity", (SolveBand(Assemble(free, step, acceleration)) -
          (free.particles[0].velocity + step * acceleration)).norm(), 1e-12, failures);
    const Vector3 tilted = Vector3(1.0, 2.0, -3.0).normalized();
    const Matrix3 projection = tilted * tilted.transpose();
    Check(checks, "projection_rank1", (projection * projection - projection).norm(), 1e-12, failures);
    Check(checks, "projection_cross_coordinate", std::abs((projection * Vector3::UnitX()).y() - 1.0 / 7.0), 1e-12, failures);
    Chain invalid = analytic;
    invalid.particles[1].position.setZero();
    bool rejected = false;
    try { Assemble(invalid, step, Vector3::Zero()); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(checks, "zero_length_rejected", rejected ? 0.0 : 1.0, 0.0, failures);
    Chain tiny = analytic;
    tiny.particles[1].position = Vector3(1e-12, 2e-12, -1e-12);
    tiny.edges[0].rest = tiny.particles[1].position.norm();
    Check(checks, "short_positive_edge_finite",
          SolveBand(Assemble(tiny, step, Vector3::Zero())).norm(), 0.0, failures);
    auto summary = OpenCsv(directory / "summary.csv");
    summary << "item,status,value,detail\nnumericalFixtures," << (failures ? "FAIL" : "PASS")
            << ',' << failures << ",failed_checks\n";
    WriteUnsupported(summary);
    return failures ? 2 : 0;
}

std::vector<Vector3> Positions(const Chain& chain)
{
    std::vector<Vector3> result;
    for (const auto& particle : chain.particles)
        result.push_back(particle.position);
    return result;
}

double Displacement(const Chain& chain, const std::vector<Vector3>& reference, double length)
{
    double maximum = 0.0;
    for (int index = 0; index < static_cast<int>(chain.particles.size()); ++index)
        maximum = std::max(maximum, (chain.particles[index].position - reference[index]).norm() / length);
    return maximum;
}

struct ShapeMetrics
{
    double maximum = 0.0;
    double tip = 0.0;
    double rms = 0.0;
    Vector3 centroid = Vector3::Zero();
    double speed = 0.0;
    double kinetic = 0.0;
    double potential = 0.0;
    double springEnergy = 0.0;
    double stretchMin = 0.0;
    double stretchMax = 0.0;
    double allSpanMin = 0.0;
    double allSpanMax = 0.0;
    double height = 0.0;
    double betaMaxDelta = 0.0;
    double betaRmsDelta = 0.0;
    double betaSum = 0.0;
    double curvatureMax = 0.0;
    double curvatureRms = 0.0;
};

ShapeMetrics Measure(const Chain& chain, const std::vector<Vector3>& reference,
                     const std::vector<Vector3>& authored, double length)
{
    ShapeMetrics result;
    const int count = static_cast<int>(chain.particles.size());
    int movableCount = 0;
    double low = chain.particles[0].position.y();
    double high = low;
    double authoredLow = authored[0].y();
    double authoredHigh = authoredLow;
    double curvatureSquared = 0.0;
    double curvatureWeight = 0.0;
    for (int index = 0; index < count; ++index)
    {
        const auto& particle = chain.particles[index];
        const Vector3 delta = (particle.position - reference[index]) / length;
        result.maximum = std::max(result.maximum, delta.norm());
        result.speed = std::max(result.speed, particle.velocity.norm() / length);
        result.kinetic += 0.5 * particle.mass * particle.velocity.squaredNorm();
        result.potential += particle.mass * 9.8 * particle.position.y();
        low = std::min(low, particle.position.y());
        high = std::max(high, particle.position.y());
        authoredLow = std::min(authoredLow, authored[index].y());
        authoredHigh = std::max(authoredHigh, authored[index].y());
        if (!particle.fixed)
        {
            ++movableCount;
            result.centroid += delta;
            result.rms += delta.squaredNorm();
        }
        if (index == count - 1)
            result.tip = delta.norm();
        if (index > 0 && index + 1 < count)
        {
            const Vector3 previous = particle.position - chain.particles[index - 1].position;
            const Vector3 next = chain.particles[index + 1].position - particle.position;
            const Vector3 previousReference = reference[index] - reference[index - 1];
            const Vector3 nextReference = reference[index + 1] - reference[index];
            if (previous.norm() <= 0.0 || next.norm() <= 0.0 ||
                previousReference.norm() <= 0.0 || nextReference.norm() <= 0.0)
                throw std::runtime_error("degenerate angle segment");
            const double beta = std::acos(std::clamp(previous.normalized().dot(next.normalized()), -1.0, 1.0));
            const double referenceBeta = std::acos(std::clamp(
                previousReference.normalized().dot(nextReference.normalized()), -1.0, 1.0));
            const double deltaBeta = std::abs(beta - referenceBeta);
            const double weight = 0.5 * ((authored[index] - authored[index - 1]).norm() +
                                         (authored[index + 1] - authored[index]).norm());
            const double curvature = beta / weight;
            result.betaSum += beta;
            result.betaMaxDelta = std::max(result.betaMaxDelta, deltaBeta);
            result.betaRmsDelta += deltaBeta * deltaBeta;
            result.curvatureMax = std::max(result.curvatureMax, curvature);
            curvatureSquared += weight * curvature * curvature;
            curvatureWeight += weight;
        }
    }
    result.centroid /= movableCount;
    result.rms = std::sqrt(result.rms / movableCount);
    result.betaRmsDelta = std::sqrt(result.betaRmsDelta / (count - 2));
    result.curvatureRms = std::sqrt(curvatureSquared / curvatureWeight);
    result.height = (high - low) / (authoredHigh - authoredLow);
    for (const auto& edge : chain.edges)
    {
        const double current = (chain.particles[edge.second].position - chain.particles[edge.first].position).norm();
        const double strain = current / edge.rest - 1.0;
        result.allSpanMin = std::min(result.allSpanMin, strain);
        result.allSpanMax = std::max(result.allSpanMax, strain);
        result.springEnergy += 0.5 * edge.stiffness * (current - edge.rest) * (current - edge.rest);
        if (edge.second - edge.first == 1)
        {
            result.stretchMin = std::min(result.stretchMin, strain);
            result.stretchMax = std::max(result.stretchMax, strain);
        }
    }
    return result;
}

Chain MakeHelix()
{
    Chain chain;
    const double pi = std::acos(-1.0);
    for (int index = 0; index < 32; ++index)
    {
        const double fraction = index / 31.0;
        Particle particle;
        particle.position = Vector3(0.03 * (std::cos(4.0 * pi * fraction) - 1.0),
                                    -0.25 * fraction, 0.03 * std::sin(4.0 * pi * fraction));
        particle.mass = 0.001 / 32.0;
        particle.damping = 10.0 * particle.mass;
        particle.fixed = index < 2;
        chain.particles.push_back(particle);
    }
    for (int index = 0; index < 32; ++index)
        for (int span = 1; span <= 3 && index + span < 32; ++span)
            chain.edges.push_back({index, index + span,
                (chain.particles[index + span].position - chain.particles[index].position).norm(), 100.0});
    return chain;
}

int Helix(const std::filesystem::path& directory)
{
    constexpr int stepsPerSecond = 1920;
    constexpr int staticEnd = 3 * stepsPerSecond;
    constexpr int windEnd = staticEnd + stepsPerSecond / 2;
    constexpr int totalSteps = windEnd + 10 * stepsPerSecond;
    constexpr int denseStride = 240;
    const double step = 1.0 / stepsPerSecond;
    Chain chain = MakeHelix();
    const auto authored = Positions(chain);
    auto snapshot = authored;
    auto staticTailStart = authored;
    auto recoveryTailStart = authored;
    double length = 0.0;
    for (int index = 1; index < 32; ++index)
        length += (authored[index] - authored[index - 1]).norm();
    auto parameters = OpenCsv(directory / "parameters.csv");
    parameters << "key,value,unit\nscope,L2-local-001,identifier\nN,32,particles\n"
               << "total_mass_including_roots,0.001,kg\nmass_per_node," << 0.001 / 32.0
               << ",kg\ndamping_per_node," << 10.0 * 0.001 / 32.0
               << ",kg_per_s\nstiffness_all_spans,100,N_per_m\nradius,0.03,m\n"
               << "axial_drop,0.25,m\nturns,2,count\nframeDt," << 1.0 / 240.0
               << ",s\nsubsteps,8,count\nh," << step << ",s\nL," << length
               << ",m\nfixed_indices,0|1,index\nmovable_indices,2..31,index\n"
               << "gravity_y,-9.8,m_per_s2\nprobe_z,20,m_per_s2\nstatic_duration,3,s\n"
               << "probe_duration,0.5,s\nrecovery_duration,10,s\ndense_stride,240,substeps\n"
               << "band_residual_tolerance,1e-10,ratio\ndense_solution_tolerance,1e-9,ratio\n";
    auto nodes = OpenCsv(directory / "nodes.csv");
    auto edges = OpenCsv(directory / "edges.csv");
    nodes << "case,node,fixed,mass,damping,x,y,z,vx,vy,vz\n";
    edges << "case,first,second,rest,stiffness\n";
    WriteInput(nodes, edges, "helix32_authored", chain);
    auto trajectory = OpenCsv(directory / "trajectory.csv");
    trajectory << "step,time,phase,node,fixed,x,y,z,vx,vy,vz\n";
    auto metrics = OpenCsv(directory / "metrics.csv");
    metrics << "step,time,phase,qualified_settled,disp_authored,disp_snapshot,tip_authored,rms_authored,"
               "centroid_authored_x,centroid_authored_y,centroid_authored_z,"
               "centroid_snapshot_x,centroid_snapshot_y,centroid_snapshot_z,"
               "max_speed_over_L,kinetic_J,gravity_potential_J,local_spring_J,"
               "adjacent_strain_min,adjacent_strain_max,all_span_strain_min,all_span_strain_max,"
               "height_ratio,beta_authored_max_delta,beta_authored_rms_delta,beta_snapshot_max_delta,"
               "beta_snapshot_rms_delta,beta_sum,curvature_max_per_m,curvature_weighted_rms_per_m,"
               "root_error_m,linear_residual\n";
    auto denseChecks = OpenCsv(directory / "dense-checkpoints.csv");
    denseChecks << "step,input_time,relative_residual,relative_difference,assembly_difference,"
                   "rhs_relative_to_result,rhs_relative_to_evaluation_scale,condition\n";
    auto matrices = OpenCsv(directory / "matrices.csv");
    auto vectors = OpenCsv(directory / "vectors.csv");
    matrices << "case,row,column,band,independent_dense\n";
    vectors << "case,row,band_rhs,independent_dense_rhs,band_velocity,dense_velocity\n";
    double staticSpeed = 0.0;
    double staticDrift = 0.0;
    double recoverySpeed = 0.0;
    double recoveryDrift = 0.0;
    double maximumResidual = 0.0;
    double maximumDifference = 0.0;
    double maximumCondition = 0.0;
    double maximumRhsScaled = 0.0;
    int failedDenseChecks = 0;
    double minimumStrain = 0.0;
    double maximumStrain = 0.0;
    double allSpanMin = 0.0;
    double allSpanMax = 0.0;
    double maximumRootError = 0.0;
    double windPeak = 0.0;
    double dynamicPeak = 0.0;
    double recoveryTail = 0.0;
    Vector3 windPeakCentroid = Vector3::Zero();
    std::vector<double> recoveryDisplacements;
    bool settled = false;
    ShapeMetrics staticShape;
    ShapeMetrics currentAuthored;
    ShapeMetrics currentSnapshot;
    for (int tick = 0; tick <= totalSteps; ++tick)
    {
        double residual = 0.0;
        if (tick > 0)
        {
            const bool probeActive = tick > staticEnd && tick <= windEnd;
            const Vector3 acceleration(0.0, -9.8, probeActive ? 20.0 : 0.0);
            const auto system = Assemble(chain, step, acceleration);
            const Vector velocity = SolveBand(system);
            residual = RelativeResidual(system, velocity);
            maximumResidual = std::max(maximumResidual, residual);
            if (!std::isfinite(residual) || residual > 1e-10)
                throw std::runtime_error("helix linear residual failed at step " + std::to_string(tick));
            if (tick == 1 || tick % denseStride == 0)
            {
                const auto reference = DenseReference(chain, step, acceleration);
                const double difference = (velocity - reference.velocity).norm() /
                                          (reference.velocity.norm() + 1e-30);
                const double assemblyDifference = (system.Dense() - reference.matrix).norm() /
                                                  (reference.matrix.norm() + 1e-30);
                const double rhsDifference = (system.rhs - reference.rhs).norm() /
                                             (reference.rhs.norm() + 1e-30);
                const double rhsScaled = (system.rhs - reference.rhs).norm() /
                                         (reference.rhsEvaluationScale + reference.rhs.norm() + 1e-30);
                maximumDifference = std::max(maximumDifference, difference);
                maximumCondition = std::max(maximumCondition, reference.condition);
                maximumRhsScaled = std::max(maximumRhsScaled, rhsScaled);
                denseChecks << tick << ',' << (tick - 1) * step << ',' << residual << ',' << difference
                            << ',' << assemblyDifference << ',' << rhsDifference << ',' << rhsScaled
                            << ',' << reference.condition << '\n';
                const std::string name = "step" + std::to_string(tick);
                WriteSystem(matrices, vectors, name, system, velocity, reference);
                WriteInput(nodes, edges, name, chain);
                if (!std::isfinite(difference) || difference > 1e-9 ||
                    assemblyDifference > 1e-12 || rhsScaled > 1e-12)
                    ++failedDenseChecks;
            }
            for (int index = 0; index < 32; ++index)
            {
                chain.particles[index].velocity = velocity.segment<3>(3 * index);
                if (!chain.particles[index].fixed)
                    chain.particles[index].position += step * chain.particles[index].velocity;
                if (!chain.particles[index].position.allFinite() || !chain.particles[index].velocity.allFinite())
                    throw std::runtime_error("nonfinite helix state at step " + std::to_string(tick));
            }
        }
        if (tick == staticEnd - stepsPerSecond)
            staticTailStart = Positions(chain);
        currentAuthored = Measure(chain, authored, authored, length);
        if (tick >= staticEnd - stepsPerSecond && tick <= staticEnd)
        {
            staticSpeed = std::max(staticSpeed, currentAuthored.speed);
            staticDrift = std::max(staticDrift, Displacement(chain, staticTailStart, length));
        }
        if (tick == staticEnd)
        {
            snapshot = Positions(chain);
            staticShape = currentAuthored;
            settled = staticSpeed <= 1e-3 && staticDrift <= 1e-4;
            auto baseline = OpenCsv(directory / "snapshot.csv");
            baseline << "node,x,y,z,vx,vy,vz,qualified_settled\n";
            for (int index = 0; index < 32; ++index)
            {
                baseline << index;
                for (int axis = 0; axis < 3; ++axis)
                    baseline << ',' << chain.particles[index].position[axis];
                for (int axis = 0; axis < 3; ++axis)
                    baseline << ',' << chain.particles[index].velocity[axis];
                baseline << ',' << settled << '\n';
            }
            std::cout << "static checkpoint: " << (settled ? "SETTLED" : "STATIC_NOT_SETTLED")
                      << " Dmax=" << staticShape.maximum << " tail_speed/L=" << staticSpeed
                      << " tail_drift/L=" << staticDrift << '\n';
        }
        currentSnapshot = Measure(chain, snapshot, authored, length);
        if (tick >= staticEnd && tick <= windEnd && currentSnapshot.maximum >= windPeak)
        {
            windPeak = currentSnapshot.maximum;
            windPeakCentroid = currentSnapshot.centroid;
        }
        if (tick >= staticEnd)
            dynamicPeak = std::max(dynamicPeak, currentSnapshot.maximum);
        if (tick >= windEnd)
            recoveryDisplacements.push_back(currentSnapshot.maximum);
        if (tick == totalSteps - stepsPerSecond)
            recoveryTailStart = Positions(chain);
        if (tick >= totalSteps - stepsPerSecond)
        {
            recoveryTail = std::max(recoveryTail, currentSnapshot.maximum);
            recoverySpeed = std::max(recoverySpeed, currentAuthored.speed);
            recoveryDrift = std::max(recoveryDrift, Displacement(chain, recoveryTailStart, length));
        }
        minimumStrain = std::min(minimumStrain, currentAuthored.stretchMin);
        maximumStrain = std::max(maximumStrain, currentAuthored.stretchMax);
        allSpanMin = std::min(allSpanMin, currentAuthored.allSpanMin);
        allSpanMax = std::max(allSpanMax, currentAuthored.allSpanMax);
        double rootError = 0.0;
        for (int index = 0; index < 2; ++index)
            rootError = std::max(rootError, (chain.particles[index].position - authored[index]).norm());
        maximumRootError = std::max(maximumRootError, rootError);
        const char* phase = tick <= staticEnd ? "static" : (tick <= windEnd ? "probe" : "recovery");
        for (int index = 0; index < 32; ++index)
        {
            const auto& particle = chain.particles[index];
            trajectory << tick << ',' << tick * step << ',' << phase << ',' << index << ',' << particle.fixed;
            for (int axis = 0; axis < 3; ++axis)
                trajectory << ',' << particle.position[axis];
            for (int axis = 0; axis < 3; ++axis)
                trajectory << ',' << particle.velocity[axis];
            trajectory << '\n';
        }
        metrics << tick << ',' << tick * step << ',' << phase << ',' << (tick >= staticEnd && settled)
                << ',' << currentAuthored.maximum << ',' << currentSnapshot.maximum << ',' << currentAuthored.tip
                << ',' << currentAuthored.rms;
        for (int axis = 0; axis < 3; ++axis)
            metrics << ',' << currentAuthored.centroid[axis];
        for (int axis = 0; axis < 3; ++axis)
            metrics << ',' << currentSnapshot.centroid[axis];
        metrics << ',' << currentAuthored.speed << ',' << currentAuthored.kinetic << ',' << currentAuthored.potential
                << ',' << currentAuthored.springEnergy << ',' << currentAuthored.stretchMin
                << ',' << currentAuthored.stretchMax << ',' << currentAuthored.allSpanMin << ',' << currentAuthored.allSpanMax
                << ',' << currentAuthored.height << ',' << currentAuthored.betaMaxDelta << ',' << currentAuthored.betaRmsDelta
                << ',' << currentSnapshot.betaMaxDelta << ',' << currentSnapshot.betaRmsDelta << ',' << currentAuthored.betaSum
                << ',' << currentAuthored.curvatureMax << ',' << currentAuthored.curvatureRms << ',' << rootError << ',' << residual << '\n';
    }
    int lastOutside = -1;
    for (int index = 0; index < static_cast<int>(recoveryDisplacements.size()); ++index)
        if (recoveryDisplacements[index] > 0.005)
            lastOutside = index;
    const int startIndex = lastOutside + 1;
    const bool recoveryObserved = startIndex < static_cast<int>(recoveryDisplacements.size()) &&
        (static_cast<int>(recoveryDisplacements.size()) - 1 - startIndex) >= stepsPerSecond;
    const double recoveryTime = recoveryObserved ? startIndex * step : -1.0;
    const double stretch = std::max(-minimumStrain, maximumStrain);
    auto summary = OpenCsv(directory / "summary.csv");
    summary << "item,status,value,detail\nscope,INFO,L2-local-001,local_distance_spring_only\n"
            << "completedSubsteps,INFO," << totalSteps << ",count\n"
            << "numericalTrajectory," << (failedDenseChecks ? "FAIL" : "PASS")
            << ',' << failedDenseChecks << ",failed_dense_checkpoints\n"
            << "staticConvergence," << (settled ? "PASS" : "FAIL") << ',' << staticSpeed << ",tail_speed_over_L\n"
            << "staticTailDrift,INFO," << staticDrift << ",relative_to_2s_state_over_L\n"
            << "Dmax," << (staticShape.maximum <= 0.015 && settled ? "PASS" : "FAIL")
            << ',' << staticShape.maximum << ",authored_reference_limit_0.015_requires_settled\n"
            << "Dtip,INFO," << staticShape.tip << ",static_authored_over_L\n"
            << "Drms,INFO," << staticShape.rms << ",static_movable_authored_over_L\n"
            << "H,INFO," << staticShape.height << ",static_y_span_ratio_not_volume\n"
            << "D_wind," << (settled ? "INFO" : "INCONCLUSIVE") << ',' << windPeak
            << ",relative_to_3s_snapshot_qualified_only_if_settled\n"
            << "D_dyn," << (!settled ? "INCONCLUSIVE" : (dynamicPeak >= 0.02 && dynamicPeak <= 0.08 ? "PASS" : "FAIL"))
            << ',' << dynamicPeak << ",relative_to_3s_snapshot_includes_recovery\n"
            << "T_rec," << (!settled ? "INCONCLUSIVE" : (recoveryObserved && recoveryTime <= 0.5 ? "PASS" : "FAIL"))
            << ',' << (recoveryObserved ? std::to_string(recoveryTime) : "NA")
            << ",suffix_within_0.005L_minimum_1s_until_10s_endpoint\n"
            << "R_tail," << (!settled ? "INCONCLUSIVE" : (recoveryTail <= 0.005 ? "PASS" : "FAIL"))
            << ',' << recoveryTail << ",last_1s_max_over_L\n"
            << "R_end," << (settled ? "INFO" : "INCONCLUSIVE") << ',' << currentSnapshot.maximum << ",endpoint_over_L\n"
            << "stretch," << (stretch <= 0.01 ? "PASS" : "FAIL") << ',' << stretch
            << ",all_phases_adjacent_solver_rest_equals_authored\n"
            << "signedAdjacentMin,INFO," << minimumStrain << ",strain\n"
            << "signedAdjacentMax,INFO," << maximumStrain << ",strain\n"
            << "signedAllSpanMin,INFO," << allSpanMin << ",strain\n"
            << "signedAllSpanMax,INFO," << allSpanMax << ",strain\n"
            << "rootError," << (maximumRootError <= 1e-12 ? "PASS" : "FAIL") << ',' << maximumRootError << ",m\n"
            << "maxLinearResidual,PASS," << maximumResidual << ",every_substep_limit_1e-10\n"
            << "maxDenseDifference," << (maximumDifference <= 1e-9 ? "PASS" : "FAIL")
            << ',' << maximumDifference << ",stride240_and_first_step_limit_1e-9\n"
            << "maxRhsEvaluationScaled," << (maximumRhsScaled <= 1e-12 ? "PASS" : "FAIL")
            << ',' << maximumRhsScaled << ",forward_assembly_scale_not_cancellation_prone_result_norm\n"
            << "maxDenseCondition,INFO," << maximumCondition << ",free_mass_matrix_sampled\n"
            << "recoveryTailSpeed,INFO," << recoverySpeed << ",max_speed_over_L\n"
            << "recoveryTailDrift,INFO," << recoveryDrift << ",relative_to_12.5s_state_over_L\n"
            << "staticTailEndKinetic,INFO," << staticShape.kinetic << ",J\n"
            << "recoveryTailEndKinetic,INFO," << currentAuthored.kinetic << ",J\n"
            << "windPeakCentroidX,INFO," << windPeakCentroid.x() << ",over_L\n"
            << "windPeakCentroidY,INFO," << windPeakCentroid.y() << ",over_L\n"
            << "windPeakCentroidZ,INFO," << windPeakCentroid.z() << ",over_L\n";
    if (windPeakCentroid.norm() > 0.0)
        summary << "windPeakCentroidCosZ,INFO," << windPeakCentroid.z() / windPeakCentroid.norm() << ",dimensionless\n";
    else
        summary << "windPeakCentroidCosZ,INCONCLUSIVE,NA,zero_centroid\n";
    summary << "halfStep,NOT_RUN,NA,one_fixed_behavior_case_only\n"
            << "negativeZProbe,NOT_RUN,NA,one_fixed_behavior_case_only\n"
            << "geometricDihedral,NOT_RUN,NA,positions_exported_not_material_twist\n"
            << "energyBalance,NOT_RUN,NA,energies_only_no_work_balance_claim\n";
    WriteUnsupported(summary);
    const bool behaviorPassed = settled && staticShape.maximum <= 0.015 && dynamicPeak >= 0.02 &&
        dynamicPeak <= 0.08 && recoveryObserved && recoveryTime <= 0.5 && recoveryTail <= 0.005 && stretch <= 0.01;
    summary << "behaviorScreen," << (behaviorPassed ? "PASS" : "FAIL")
            << ",NA,engineering_screen_not_paper_validation\n";
    std::cout << "L2-local-001 numericalTrajectory=" << (failedDenseChecks ? "FAIL" : "PASS")
              << " behaviorScreen=" << (behaviorPassed ? "PASS" : "FAIL")
              << " Dmax=" << staticShape.maximum << " diagnostic_D_dyn=" << dynamicPeak
              << " stretch=" << stretch << " maxResidual=" << maximumResidual
              << " maxDenseDifference=" << maximumDifference << '\n';
    return failedDenseChecks ? 2 : (behaviorPassed ? 0 : 3);
}

int main(int argc, char** argv)
{
    bool outputCreated = false;
    try
    {
        if (argc != 3 || (std::string(argv[1]) != "--verify" && std::string(argv[1]) != "--helix"))
            throw std::runtime_error("usage: vl_hair_ams_local_block7 --verify|--helix NEW_OUTPUT_DIRECTORY");
        const std::filesystem::path directory(argv[2]);
        if (!std::filesystem::create_directories(directory))
            throw std::runtime_error("output directory already exists");
        outputCreated = true;
        return std::string(argv[1]) == "--verify" ? Verify(directory) : Helix(directory);
    }
    catch (const std::exception& error)
    {
        std::cerr << "L2-local-001 FAIL: " << error.what() << "\n";
        if (outputCreated)
        {
            std::ofstream failure(std::filesystem::path(argv[2]) / "failure.txt");
            failure << "scope=L2-local-001\nstatus=FAIL\nreason=" << error.what() << '\n';
        }
        return 2;
    }
}
