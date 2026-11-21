#include "hairClumpCoupled.h"
#include "rodGhostPrototypeMath.h"
#include <Eigen/Eigenvalues>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace C2
{
using namespace R1;
using Vector = Eigen::VectorXd;
using Matrix = Eigen::MatrixXd;
constexpr double Spring = 3.24777679370055;
constexpr double Damping = .056989269110075014;
constexpr double Coupling = .1;

std::ofstream Open(const std::filesystem::path& path)
{
    std::ofstream output(path);
    if (!output) throw std::runtime_error("OUTPUT_FAILURE");
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << std::setprecision(17);
    return output;
}

int PositionIndex(int guide, int node, int axis)
{
    Require(guide >= 0 && guide < 3 && node >= 1 && node <= 3 && axis >= 0 && axis < 3);
    return 15 * guide + 3 * (node - 1) + axis;
}

int FrameIndex(int guide, int segment, int axis)
{
    Require(guide >= 0 && guide < 3 && segment >= 1 && segment <= 2 && axis >= 0 && axis < 3);
    return 15 * guide + 9 + 3 * (segment - 1) + axis;
}

struct Context
{
    std::array<Model, 3> models;
    std::array<State, 3> old;
    std::array<std::vector<Vec>, 3> velocity;
    std::array<Vec, 3> probe;
    double length;
    double force0;
    double forceScale;
    double torqueScale;
    double energy0;
    double energyScale;
    double step = .001;
};

Context MakeContext()
{
    State authored;
    authored.positions = {Vec(0, 0, 0), Vec(.01, 0, .1), Vec(.02, .005, .2), Vec(.025, .015, .3)};
    authored.frames = {Quat::Identity(), ExpR(Vec(.03, -.02, .04), .1), ExpR(Vec(.05, .01, .08), .1)};
    Model model = AuthoredModel(authored, Vec(.3, .4, 1), Vec(.001, .0015, .002), .1);
    model.masses.assign(4, .001);
    model.ghostStiffness = {0, Spring, Spring, Spring};
    Context context;
    context.length = *std::max_element(model.lengths.begin(), model.lengths.end());
    context.force0 = 1.0 + Spring * context.length + .001 * 29.8;
    context.forceScale = context.force0;
    double joint = 0.0;
    for (int segment = 0; segment < 2; ++segment)
        joint = std::max(joint, .002 / (.5 * (model.lengths[segment] + model.lengths[segment + 1])));
    context.torqueScale = context.length + joint;
    context.energy0 = context.force0 * context.length + context.torqueScale;
    context.energyScale = context.forceScale * context.length + context.torqueScale;
    const Vec offsets[] = {Vec(0, 0, 0), Vec(.06, 0, 0), Vec(.03, .052, 0)};
    for (int guide = 0; guide < 3; ++guide)
    {
        context.models[guide] = model;
        context.old[guide] = authored;
        for (auto& position : context.old[guide].positions) position += offsets[guide];
        context.models[guide].authored = context.old[guide];
        context.velocity[guide].assign(4, Vec::Zero());
        context.probe[guide] = Vec::Zero();
    }
    return context;
}

struct Single
{
    State state;
    Evaluation potential;
    double objective;
    Vector gradient;
    std::vector<Vec> residual;
};

Single SingleAt(const Context& context, int guide, const Vector& unknown, double energyScale)
{
    Require(unknown.size() == 15 && unknown.allFinite());
    Single value;
    value.state = context.old[guide];
    value.gradient = Vector::Zero(15);
    value.residual.assign(4, Vec::Zero());
    for (int node = 1; node < 4; ++node)
        value.state.positions[node] = context.length * unknown.segment<3>(3 * (node - 1));
    for (int segment = 1; segment < 3; ++segment)
        value.state.frames[segment] = Unit(context.old[guide].frames[segment] *
            ExpR(unknown.segment<3>(9 + 3 * (segment - 1)), .1));
    value.potential = Evaluate(context.models[guide], value.state,
        context.models[guide].authored.positions, Vec(0, -9.8, 0));
    value.objective = value.potential.Energy();
    for (int node = 1; node < 4; ++node)
    {
        const Vec delta = value.state.positions[node] - context.old[guide].positions[node];
        const Vec error = delta - context.step * context.velocity[guide][node];
        value.objective += .5 * .001 * error.squaredNorm() / (context.step * context.step) +
                           .5 * Damping * delta.squaredNorm() / context.step -
                           .001 * context.probe[guide].dot(value.state.positions[node]);
        value.residual[node] = .001 * error / (context.step * context.step) +
            Damping * delta / context.step - value.potential.force[node] - .001 * context.probe[guide];
        value.gradient.segment<3>(3 * (node - 1)) = value.residual[node] * context.length / energyScale;
    }
    for (int segment = 1; segment < 3; ++segment)
        value.gradient.segment<3>(9 + 3 * (segment - 1)) =
            -RightJacobian(unknown.segment<3>(9 + 3 * (segment - 1))).transpose() *
            value.potential.bodyTorque[segment] / energyScale;
    Require(std::isfinite(value.objective) && value.gradient.allFinite());
    return value;
}

struct PairValue
{
    double energy = 0.0;
    std::array<std::vector<Vec>, 3> force;
    Vector gradient;
    Matrix hessian;
};

PairValue PairAt(const Context& context, const std::array<State, 3>& states, double stiffness)
{
    Require(std::isfinite(stiffness) && stiffness >= 0.0);
    PairValue value;
    value.gradient = Vector::Zero(45);
    value.hessian = Matrix::Zero(45, 45);
    for (int guide = 0; guide < 3; ++guide)
    {
        Require(states[guide].positions.size() == 4 && states[guide].frames.size() == 3);
        for (const auto& position : states[guide].positions) Require(position.allFinite());
        for (const auto& frame : states[guide].frames) Unit(frame);
        value.force[guide].assign(4, Vec::Zero());
    }
    const double khat = stiffness * context.length * context.length / context.energyScale;
    for (int first = 0; first < 3; ++first)
        for (int second = first + 1; second < 3; ++second)
            for (int node = 1; node < 4; ++node)
            {
                const Vec error = (states[first].positions[node] - states[second].positions[node]) -
                    (context.models[first].authored.positions[node] - context.models[second].authored.positions[node]);
                value.energy += .5 * stiffness * error.squaredNorm();
                value.force[first][node] -= stiffness * error;
                value.force[second][node] += stiffness * error;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const int firstIndex = PositionIndex(first, node, axis);
                    const int secondIndex = PositionIndex(second, node, axis);
                    value.hessian(firstIndex, firstIndex) += khat;
                    value.hessian(secondIndex, secondIndex) += khat;
                    value.hessian(firstIndex, secondIndex) -= khat;
                    value.hessian(secondIndex, firstIndex) -= khat;
                }
            }
    for (int guide = 0; guide < 3; ++guide)
        for (int node = 1; node < 4; ++node)
            value.gradient.segment<3>(PositionIndex(guide, node, 0)) =
                -context.length * value.force[guide][node] / context.energyScale;
    Require(std::isfinite(value.energy) && value.gradient.allFinite() && value.hessian.allFinite());
    return value;
}

struct Value
{
    std::array<Single, 3> guides;
    PairValue pair;
    Vector gradient;
    double objective = 0.0;
    double phi = 0.0;
    double forceResidual = 0.0;
    double torqueResidual = 0.0;
    double merit = 0.0;
};

Value At(const Context& context, const Vector& unknown, double stiffness)
{
    Require(unknown.size() == 45 && unknown.allFinite());
    Value value;
    value.gradient = Vector::Zero(45);
    std::array<State, 3> states;
    for (int guide = 0; guide < 3; ++guide)
    {
        value.guides[guide] = SingleAt(context, guide, unknown.segment(15 * guide, 15), context.energyScale);
        states[guide] = value.guides[guide].state;
        value.objective += value.guides[guide].objective;
        value.gradient.segment(15 * guide, 15) = value.guides[guide].gradient;
    }
    value.pair = PairAt(context, states, stiffness);
    value.objective += value.pair.energy;
    value.phi = value.objective / context.energyScale;
    value.gradient += value.pair.gradient;
    for (int guide = 0; guide < 3; ++guide)
    {
        for (int node = 1; node < 4; ++node)
        {
            value.guides[guide].residual[node] -= value.pair.force[guide][node];
            const Vec& residual = value.guides[guide].residual[node];
            value.forceResidual = std::max(value.forceResidual, residual.norm() / context.forceScale);
            value.merit += .5 * residual.squaredNorm() / (context.forceScale * context.forceScale);
        }
        for (int segment = 1; segment < 3; ++segment)
        {
            const Vec& torque = value.guides[guide].potential.bodyTorque[segment];
            value.torqueResidual = std::max(value.torqueResidual, torque.norm() / context.torqueScale);
            value.merit += .5 * torque.squaredNorm() / (context.torqueScale * context.torqueScale);
        }
    }
    value.merit += .5 * value.gradient.squaredNorm();
    Require(std::isfinite(value.phi) && std::isfinite(value.merit) && value.gradient.allFinite());
    return value;
}

Matrix Hessian(const Context& context, const Vector& unknown, double stiffness, double epsilon)
{
    Matrix matrix(45, 45);
    for (int coordinate = 0; coordinate < 45; ++coordinate)
    {
        Vector plus = unknown, minus = unknown;
        plus[coordinate] += epsilon;
        minus[coordinate] -= epsilon;
        matrix.col(coordinate) = (At(context, plus, stiffness).gradient -
                                 At(context, minus, stiffness).gradient) / (2.0 * epsilon);
    }
    Require(matrix.allFinite());
    return matrix;
}

Matrix IndependentHessian(const Context& context, const Vector& unknown, double epsilon)
{
    Matrix matrix = Matrix::Zero(45, 45);
    for (int guide = 0; guide < 3; ++guide)
        for (int coordinate = 0; coordinate < 15; ++coordinate)
        {
            Vector plus = unknown.segment(15 * guide, 15), minus = plus;
            plus[coordinate] += epsilon;
            minus[coordinate] -= epsilon;
            matrix.block(15 * guide, 15 * guide + coordinate, 15, 1) =
                (SingleAt(context, guide, plus, context.energyScale).gradient -
                 SingleAt(context, guide, minus, context.energyScale).gradient) / (2.0 * epsilon);
        }
    return matrix;
}

double Norm(const Matrix& matrix)
{
    Eigen::JacobiSVD<Matrix> decomposition(matrix);
    Require(decomposition.info() == Eigen::Success && decomposition.singularValues().allFinite());
    return decomposition.singularValues()[0];
}

Vector Fixture(const Context& context, bool general)
{
    Vector unknown = Vector::Zero(45);
    const Vec offsets[] = {Vec(.001, -.002, .003), Vec(-.002, .001, .0005), Vec(.0007, -.001, .002)};
    for (int guide = 0; guide < 3; ++guide)
    {
        for (int node = 1; node < 4; ++node)
        {
            Vec position = context.old[guide].positions[node];
            if (general) position += offsets[guide];
            unknown.segment<3>(PositionIndex(guide, node, 0)) = position / context.length;
        }
        if (general)
            for (int segment = 1; segment < 3; ++segment)
                unknown.segment<3>(FrameIndex(guide, segment, 0)) =
                    segment == 1 ? Vec(.01, -.02, .015) : Vec(-.015, .01, .02);
    }
    return unknown;
}

void MatrixCsv(std::ostream& output, const std::string& name, const Matrix& matrix)
{
    for (int row = 0; row < matrix.rows(); ++row)
        for (int column = 0; column < matrix.cols(); ++column)
            output << name << ',' << row << ',' << column << ',' << matrix(row, column) << '\n';
}

void Check(std::ostream& output, const std::string& name, double error, double scale, double tolerance, int& failures)
{
    const double ratio = error / scale;
    const bool pass = std::isfinite(ratio) && ratio <= tolerance;
    output << name << ',' << error << ',' << scale << ',' << ratio << ',' << tolerance
           << ',' << (pass ? "PASS" : "FAIL") << '\n';
    if (!pass) ++failures;
}

int Verify(const std::filesystem::path& directory)
{
    Context context = MakeContext();
    auto manifest = Open(directory / "manifest.txt");
    manifest << "scope=C2_E0_E1_E2_static_only;D=NOT_RUN\norder=guide_major_3*(9x+6eta)=45\n"
             << "h=.001,.0005,.00025;old=translated_authored;velocity=0;probe=+20X,0,-20X;roots=x0_q0_fixed\n"
             << "general_dx=(.001,-.002,.003);(-.002,.001,.0005);(.0007,-.001,.002)\n"
             << "general_eta1=(.01,-.02,.015);general_eta2=(-.015,.01,.02)\n"
             << "scale_authority=final_c2-spec_and_c2-audit-d-release;early_added_coupling_scale_superseded\n"
             << "k=.1;k0=structure_only;common_scale_uses_registered_k=.1_for_both\n"
             << "Lref=" << context.length << "\nF0=" << context.force0 << "\nFc2=" << context.forceScale
             << "\nTauc2=" << context.torqueScale << "\nE0=" << context.energy0 << "\nEc2=" << context.energyScale
             << "\nfull_energyFD_eps=1e-4,1e-5,1e-6;full_H_eps=1e-5,5e-6\n"
             << "pair_physicalFD_eps=Lc*(1e-4,1e-5,1e-6);Lc=.06;Fc=.006;Ec=.00036;Tauc=.00036\n"
             << "no_optimizer;no_time_advancement;independentAudit=PENDING;volume=UNSUPPORTED\n";
    auto checks = Open(directory / "checks.csv");
    checks << "check,error,scale,ratio,tolerance,status\n";
    auto matrices = Open(directory / "hessians.csv");
    matrices << "case,row,column,value\n";
    auto inputs = Open(directory / "inputs.csv");
    inputs << "case,guide,node,x,y,z,old_x,old_y,old_z,vx,vy,vz,qw,qx,qy,qz,old_qw,old_qx,old_qy,old_qz\n";
    auto gradients = Open(directory / "gradient.csv");
    gradients << "case,coordinate,z,g_common,g_C0_original,physical_residual,pair_gradient\n";
    auto energies = Open(directory / "energy.csv");
    energies << "case,k,guide,U_ss,U_bt,U_G0,U_g,step_objective_J,phi_common,phi_original_C0,Urel_total,phi_joint,chart_inf,force_scaled,torque_scaled,merit90\n";
    auto fd = Open(directory / "coordinate-fd.csv");
    fd << "case,epsilon,coordinate,analytic_g,energy_fd_g,error_g,physical_error,physical_scale,ratio,gate\n";
    auto pairRows = Open(directory / "pair.csv");
    pairRows << "case,a,b,node,ex,ey,ez,fa_x,fa_y,fa_z,fb_x,fb_y,fb_z,moment_x,moment_y,moment_z,exact_moment_x,exact_moment_y,exact_moment_z\n";
    auto spectra = Open(directory / "spectra.csv");
    spectra << "case,index,eigenvalue\n";
    auto forces = Open(directory / "forces.csv");
    forces << "case,guide,node,potential_fx,potential_fy,potential_fz,pair_fx,pair_fy,pair_fz,residual_x,residual_y,residual_z,body_tau_x,body_tau_y,body_tau_z,root_reaction_fx,root_reaction_fy,root_reaction_fz,root_reaction_tau_x,root_reaction_tau_y,root_reaction_tau_z\n";
    auto dofs = Open(directory / "dof-map.csv");
    dofs << "coordinate,guide,kind,node_or_segment,axis\n";
    for (int guide = 0; guide < 3; ++guide)
    {
        for (int node = 1; node < 4; ++node)
            for (int axis = 0; axis < 3; ++axis)
                dofs << PositionIndex(guide, node, axis) << ',' << guide << ",x," << node << ',' << axis << '\n';
        for (int segment = 1; segment < 3; ++segment)
            for (int axis = 0; axis < 3; ++axis)
                dofs << FrameIndex(guide, segment, axis) << ',' << guide << ",eta," << segment << ',' << axis << '\n';
    }
    int failures = 0;
    for (double step : {.001, .0005, .00025})
    {
    context.step = step;
    context.probe = {Vec(20, 0, 0), Vec::Zero(), Vec(-20, 0, 0)};
    for (bool general : {false, true})
    {
        const std::string name = std::string(general ? "general" : "authored") + "_h" + std::to_string(step);
        const Vector unknown = Fixture(context, general);
        const Value zero = At(context, unknown, 0.0);
        Vector independentGradient(45);
        double sum = 0.0;
        for (int guide = 0; guide < 3; ++guide)
        {
            const auto single = SingleAt(context, guide, unknown.segment(15 * guide, 15), context.energy0);
            sum += single.objective;
            independentGradient.segment(15 * guide, 15) = single.gradient * context.energy0 / context.energyScale;
            for (int node = 0; node < 4; ++node)
            {
                inputs << name << ',' << guide << ',' << node;
                for (int axis = 0; axis < 3; ++axis) inputs << ',' << single.state.positions[node][axis];
                for (int axis = 0; axis < 3; ++axis) inputs << ',' << context.old[guide].positions[node][axis];
                for (int axis = 0; axis < 3; ++axis) inputs << ',' << context.velocity[guide][node][axis];
                if (node < 3)
                    inputs << ',' << single.state.frames[node].w() << ',' << single.state.frames[node].x()
                           << ',' << single.state.frames[node].y() << ',' << single.state.frames[node].z()
                           << ',' << context.old[guide].frames[node].w() << ',' << context.old[guide].frames[node].x()
                           << ',' << context.old[guide].frames[node].y() << ',' << context.old[guide].frames[node].z();
                else inputs << ",NA,NA,NA,NA,NA,NA,NA,NA";
                inputs << '\n';
            }
        }
        Check(checks, name + "_k0_energy", std::abs(zero.objective - sum), context.energyScale, 1e-10, failures);
        Check(checks, name + "_k0_phi", std::abs(zero.phi - sum / context.energyScale),
              std::max(1.0, std::abs(sum / context.energyScale)), 1e-12, failures);
        Check(checks, name + "_k0_gradient", (zero.gradient - independentGradient).lpNorm<Eigen::Infinity>(),
              1.0, 1e-10, failures);
        const Value coupled = At(context, unknown, Coupling);
        for (double stiffness : {0.0, Coupling})
        {
            const auto value = At(context, unknown, stiffness);
            const std::string label = name + (stiffness == 0.0 ? "_k0" : "_k01");
            for (int guide = 0; guide < 3; ++guide)
            {
                const auto original = SingleAt(context, guide, unknown.segment(15 * guide, 15), context.energy0);
                const auto& single = value.guides[guide];
                for (int node = 0; node < 4; ++node)
                {
                    forces << label << ',' << guide << ',' << node;
                    for (const Vec& vector : {single.potential.force[node], value.pair.force[guide][node], single.residual[node]})
                        for (int axis = 0; axis < 3; ++axis) forces << ',' << vector[axis];
                    if (node < 3)
                        for (int axis = 0; axis < 3; ++axis) forces << ',' << single.potential.bodyTorque[node][axis];
                    else forces << ",NA,NA,NA";
                    if (node == 0)
                    {
                        for (int axis = 0; axis < 3; ++axis) forces << ',' << -single.potential.force[0][axis];
                        for (int axis = 0; axis < 3; ++axis) forces << ',' << -single.potential.bodyTorque[0][axis];
                    }
                    else forces << ",NA,NA,NA,NA,NA,NA";
                    forces << '\n';
                }
                energies << label << ',' << stiffness << ',' << guide << ',' << single.potential.shearStretch
                         << ',' << single.potential.bendTwist << ',' << single.potential.ghost << ',' << single.potential.gravity
                         << ',' << single.objective << ',' << single.objective / context.energyScale << ','
                         << single.objective / context.energy0 << ',' << value.pair.energy << ',' << value.phi << ','
                         << value.gradient.lpNorm<Eigen::Infinity>() << ',' << value.forceResidual << ','
                         << value.torqueResidual << ',' << value.merit << '\n';
                for (int local = 0; local < 15; ++local)
                {
                    const int coordinate = 15 * guide + local;
                    const double physical = local < 9 ? single.residual[1 + local / 3][local % 3] :
                        single.potential.bodyTorque[1 + (local - 9) / 3][(local - 9) % 3];
                    gradients << label << ',' << coordinate << ',' << unknown[coordinate] << ',' << value.gradient[coordinate]
                              << ',' << original.gradient[local] << ',' << physical << ',' << value.pair.gradient[coordinate] << '\n';
                }
            }
        }
        for (double epsilon : {1e-5, 5e-6})
        {
            const std::string tier = epsilon == 1e-5 ? "_H1" : "_H2";
            const Matrix independent = IndependentHessian(context, unknown, epsilon);
            const Matrix hzero = Hessian(context, unknown, 0.0, epsilon);
            const Matrix full = Hessian(context, unknown, Coupling, epsilon);
            const double scale = std::max(1.0, Norm(full));
            Matrix cross = hzero;
            for (int guide = 0; guide < 3; ++guide) cross.block(15 * guide, 15 * guide, 15, 15).setZero();
            Check(checks, name + tier + "_k0_blockdiag", Norm(hzero - independent), scale, 1e-6, failures);
            Check(checks, name + tier + "_k0_cross", cross.norm(), scale, 1e-10, failures);
            Check(checks, name + tier + "_coupled_assembly", Norm(full - independent - coupled.pair.hessian),
                  scale, 1e-6, failures);
            MatrixCsv(matrices, name + "_k0" + tier, hzero);
            MatrixCsv(matrices, name + "_independent" + tier, independent);
            MatrixCsv(matrices, name + "_k01" + tier, full);
            Eigen::SelfAdjointEigenSolver<Matrix> eig(.5 * (full + full.transpose()));
            Require(eig.info() == Eigen::Success && eig.eigenvalues().allFinite());
            for (int index = 0; index < 45; ++index)
                spectra << name << tier << ',' << index << ',' << eig.eigenvalues()[index] << '\n';
            Matrix qq(18, 18);
            for (int row = 0; row < 18; ++row)
                for (int column = 0; column < 18; ++column)
                {
                    const int rowIndex = FrameIndex(row / 6, 1 + (row % 6) / 3, row % 3);
                    const int columnIndex = FrameIndex(column / 6, 1 + (column % 6) / 3, column % 3);
                    qq(row, column) = .5 * (full(rowIndex, columnIndex) + full(columnIndex, rowIndex));
                }
            MatrixCsv(matrices, name + tier + "_qq_gather", qq);
            Eigen::SelfAdjointEigenSolver<Matrix> qeig(qq);
            Require(qeig.info() == Eigen::Success && qeig.eigenvalues().allFinite());
            for (int index = 0; index < 18; ++index)
                spectra << name << tier << "_qq_gather," << index << ',' << qeig.eigenvalues()[index] << '\n';
        }
        if (!general) continue;
        for (double epsilon : {1e-4, 1e-5, 1e-6})
        {
            Vector finite(45);
            for (int coordinate = 0; coordinate < 45; ++coordinate)
            {
                Vector plus = unknown, minus = unknown;
                plus[coordinate] += epsilon;
                minus[coordinate] -= epsilon;
                finite[coordinate] = (At(context, plus, Coupling).phi - At(context, minus, Coupling).phi) / (2 * epsilon);
            }
            for (int guide = 0; guide < 3; ++guide)
                for (int local = 0; local < 15; local += 3)
                {
                    const int coordinate = 15 * guide + local;
                    Vec physicalError = (finite - coupled.gradient).segment<3>(coordinate) * context.energyScale;
                    const bool position = local < 9;
                    if (position) physicalError /= context.length;
                    else
                    {
                        const Vec eta = unknown.segment<3>(coordinate);
                        physicalError = RightJacobian(eta).transpose().fullPivLu().solve(physicalError).eval();
                    }
                    const double scale = position ? context.forceScale : context.torqueScale;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        fd << "full," << epsilon << ',' << coordinate + axis << ',' << coupled.gradient[coordinate + axis]
                           << ',' << finite[coordinate + axis] << ',' << finite[coordinate + axis] - coupled.gradient[coordinate + axis]
                           << ',' << physicalError[axis] << ',' << scale << ',' << std::abs(physicalError[axis]) / scale
                           << ',' << (epsilon < 1e-4 ? "1e-7" : "REPORT_ONLY") << '\n';
                        if (epsilon < 1e-4)
                            Check(checks, "full_FD_" + std::to_string(epsilon) + "_" + std::to_string(coordinate + axis),
                                  std::abs(physicalError[axis]), scale, 1e-7, failures);
                    }
                }
        }
        std::array<State, 3> states;
        for (int guide = 0; guide < 3; ++guide) states[guide] = coupled.guides[guide].state;
        const auto pair = PairAt(context, states, Coupling);
        MatrixCsv(matrices, "pair_exact_chart", pair.hessian);
        for (int first = 0; first < 3; ++first)
            for (int second = first + 1; second < 3; ++second)
                for (int node = 1; node < 4; ++node)
                {
                    const Vec relative = states[first].positions[node] - states[second].positions[node];
                    const Vec rest = context.models[first].authored.positions[node] - context.models[second].authored.positions[node];
                    const Vec error = relative - rest;
                    const Vec forceA = -Coupling * error;
                    const Vec forceB = Coupling * error;
                    const Vec moment = states[first].positions[node].cross(forceA) + states[second].positions[node].cross(forceB);
                    const Vec exact = Coupling * relative.cross(rest);
                    const std::string label = std::to_string(first) + std::to_string(second) + "_" + std::to_string(node);
                    Check(checks, "pair_balance_" + label, (forceA + forceB).norm(), .006, 1e-10, failures);
                    Check(checks, "pair_moment_" + label, (moment - exact).norm(), .00036, 1e-10, failures);
                    pairRows << name << ',' << first << ',' << second << ',' << node;
                    for (const Vec& vector : {error, forceA, forceB, moment, exact})
                        for (int axis = 0; axis < 3; ++axis) pairRows << ',' << vector[axis];
                    pairRows << '\n';
                    const double khat = Coupling * context.length * context.length / context.energyScale;
                    Check(checks, "nonzero_crossblock_" + label,
                        (pair.hessian.block<3, 3>(PositionIndex(first, node, 0), PositionIndex(second, node, 0)) +
                         khat * Mat::Identity()).norm(), khat, 1e-9, failures);
                }
        for (double epsilon : {1e-4, 1e-5, 1e-6})
        {
            Matrix physical(27, 27);
            const double delta = epsilon * .06;
            for (int coordinate = 0; coordinate < 27; ++coordinate)
            {
                const int guide = coordinate / 9, node = 1 + (coordinate % 9) / 3, axis = coordinate % 3;
                auto plus = states, minus = states;
                plus[guide].positions[node][axis] += delta;
                minus[guide].positions[node][axis] -= delta;
                const auto positive = PairAt(context, plus, Coupling), negative = PairAt(context, minus, Coupling);
                const double forceFD = -(positive.energy - negative.energy) / (2 * delta);
                fd << "pair_physical," << epsilon << ',' << coordinate << ',' << pair.force[guide][node][axis]
                   << ',' << forceFD << ',' << forceFD - pair.force[guide][node][axis] << ','
                   << forceFD - pair.force[guide][node][axis] << ",.006,"
                   << std::abs(forceFD - pair.force[guide][node][axis]) / .006 << ','
                   << (epsilon < 1e-4 ? "1e-7" : "REPORT_ONLY") << '\n';
                if (epsilon < 1e-4)
                    Check(checks, "pair_force_FD_" + std::to_string(epsilon) + "_" + std::to_string(coordinate),
                          std::abs(forceFD - pair.force[guide][node][axis]), .006, 1e-7, failures);
                for (int row = 0; row < 27; ++row)
                    physical(row, coordinate) = -(positive.force[row / 9][1 + (row % 9) / 3][row % 3] -
                                                   negative.force[row / 9][1 + (row % 9) / 3][row % 3]) / (2 * delta);
            }
            Matrix exact(27, 27);
            for (int row = 0; row < 27; ++row)
                for (int column = 0; column < 27; ++column)
                    exact(row, column) = row % 9 == column % 9 ? (row / 9 == column / 9 ? .2 : -.1) : 0.0;
            MatrixCsv(matrices, "pair_physical_fd_" + std::to_string(epsilon), physical);
            if (epsilon < 1e-4)
                Check(checks, "pair_H_FD_" + std::to_string(epsilon), (physical - exact).norm(),
                      Coupling * std::sqrt(27.0), 1e-7, failures);
        }
        Eigen::SelfAdjointEigenSolver<Matrix> eig(pair.hessian);
        Require(eig.info() == Eigen::Success);
        const double nonzero = 3.0 * Coupling * context.length * context.length / context.energyScale;
        for (int index = 0; index < 45; ++index)
        {
            spectra << "pair_only," << index << ',' << eig.eigenvalues()[index] << '\n';
            Check(checks, "pair_spectrum_" + std::to_string(index),
                  std::abs(eig.eigenvalues()[index] - (index < 27 ? 0.0 : nonzero)),
                  nonzero, 1e-9, failures);
        }
        for (int guide = 0; guide < 3; ++guide)
            for (int segment = 1; segment < 3; ++segment)
            {
                const int index = FrameIndex(guide, segment, 0);
                Check(checks, "pair_direct_q_" + std::to_string(index),
                      pair.gradient.segment<3>(index).norm() + pair.hessian.block(index, 0, 3, 45).norm(),
                      1.0, 1e-10, failures);
            }
        for (int node = 1; node < 4; ++node)
            for (int axis = 0; axis < 3; ++axis)
            {
                Vector direction = Vector::Zero(45);
                for (int guide = 0; guide < 3; ++guide) direction[PositionIndex(guide, node, axis)] = 1.0;
                Check(checks, "pair_common_null_" + std::to_string(node) + "_" + std::to_string(axis),
                      (pair.hessian * direction).norm(), nonzero, 1e-10, failures);
            }
    }
    }
    auto handStates = context.old;
    handStates[0].positions[1] += Vec(.001, 0, 0);
    const auto hand = PairAt(context, handStates, Coupling);
    Check(checks, "hand_energy", std::abs(hand.energy - 1e-7), 1e-7, 1e-9, failures);
    const Vec expected[] = {Vec(-.0002, 0, 0), Vec(.0001, 0, 0), Vec(.0001, 0, 0)};
    for (int guide = 0; guide < 3; ++guide)
        Check(checks, "hand_force_" + std::to_string(guide), (hand.force[guide][1] - expected[guide]).norm(),
              expected[guide].norm(), 1e-9, failures);
    for (int guide = 0; guide < 3; ++guide)
        for (int node = 1; node < 4; ++node) handStates[guide].positions[node] = context.old[guide].positions[node] + Vec(.001, -.002, .003);
    const auto synchronous = PairAt(context, handStates, Coupling);
    Check(checks, "synchronous_energy", std::abs(synchronous.energy), .00036, 1e-10, failures);
    for (int guide = 0; guide < 3; ++guide)
        for (int node = 0; node < 4; ++node)
            Check(checks, "synchronous_force_" + std::to_string(guide) + "_" + std::to_string(node),
                  synchronous.force[guide][node].norm(), .006, 1e-10, failures);
    int rejected = 0;
    try { PairAt(context, context.old, -1.0); } catch (const std::runtime_error&) { ++rejected; }
    auto invalid = context.old;
    invalid[1].positions[2].x() = std::numeric_limits<double>::quiet_NaN();
    try { PairAt(context, invalid, Coupling); } catch (const std::runtime_error&) { ++rejected; }
    try { PositionIndex(3, 1, 0); } catch (const std::runtime_error&) { ++rejected; }
    Check(checks, "invalid_inputs_rejected", std::abs(rejected - 3), 1.0, 0.0, failures);
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value\nE0_E1_E2," << (failures ? "FAIL" : "PASS") << ',' << failures
            << "\nindependentAudit,PENDING,0\nD_single_step,NOT_RUN,0\nD_startup,NOT_RUN,0\nC3,NOT_RUN,0\nvolume,UNSUPPORTED,0\n";
    std::cout << "C2 E0/E1/E2 failures=" << failures << " execution_status=" << (failures ? "FAIL" : "PASS")
              << " independentAudit=PENDING" << std::endl;
    return failures ? 2 : 0;
}

double StableDelta(const Context& context, const Vector& base, const Vector& trial,
                   const Value& first, const Value& second)
{
    long double delta = 0.0L;
    for (int guide = 0; guide < 3; ++guide)
    {
        const Model& model = context.models[guide];
        const State& stateA = first.guides[guide].state;
        const State& stateB = second.guides[guide].state;
        for (int segment = 0; segment < 3; ++segment)
        {
            const double length = model.lengths[segment];
            const Vec edgeA = stateA.positions[segment + 1] - stateA.positions[segment];
            const Vec edgeB = stateB.positions[segment + 1] - stateB.positions[segment];
            const Vec strainA = stateA.frames[segment].conjugate() * edgeA / length - model.restStrains[segment];
            const Vec strainB = stateB.frames[segment].conjugate() * edgeB / length - model.restStrains[segment];
            for (int axis = 0; axis < 3; ++axis)
                delta += .5L * length * (static_cast<long double>(strainB[axis]) - strainA[axis]) *
                    model.shearStretch[axis] * (static_cast<long double>(strainB[axis]) + strainA[axis]);
            if (segment < 2)
            {
                const Vec angleA = LogR(stateA.frames[segment].conjugate() * stateA.frames[segment + 1], .1) -
                    model.restRotations[segment];
                const Vec angleB = LogR(stateB.frames[segment].conjugate() * stateB.frames[segment + 1], .1) -
                    model.restRotations[segment];
                const double dual = .5 * (length + model.lengths[segment + 1]);
                for (int axis = 0; axis < 3; ++axis)
                    delta += .5L * (static_cast<long double>(angleB[axis]) - angleA[axis]) *
                        model.bendTwist[axis] * (static_cast<long double>(angleB[axis]) + angleA[axis]) / dual;
            }
        }
        for (int node = 1; node < 4; ++node)
            for (int axis = 0; axis < 3; ++axis)
            {
                const int index = PositionIndex(guide, node, axis);
                const long double dx = context.length * (static_cast<long double>(trial[index]) - base[index]);
                const long double offsetA = model.authored.positions[node][axis] - stateA.positions[node][axis];
                const long double offsetB = model.authored.positions[node][axis] - stateB.positions[node][axis];
                delta -= .5L * Spring * dx * (offsetB + offsetA);
                if (axis == 1) delta += .001L * 9.8L * dx;
                delta -= .001L * context.probe[guide][axis] * dx;
                const long double deltaA = stateA.positions[node][axis] - context.old[guide].positions[node][axis];
                const long double deltaB = stateB.positions[node][axis] - context.old[guide].positions[node][axis];
                const long double predictionA = deltaA - context.step * context.velocity[guide][node][axis];
                const long double predictionB = deltaB - context.step * context.velocity[guide][node][axis];
                delta += .5L * .001L / (context.step * context.step) * dx * (predictionB + predictionA);
                delta += .5L * Damping / context.step * dx * (deltaB + deltaA);
            }
    }
    for (int firstGuide = 0; firstGuide < 3; ++firstGuide)
        for (int secondGuide = firstGuide + 1; secondGuide < 3; ++secondGuide)
            for (int node = 1; node < 4; ++node)
            {
                const Vec rest = context.models[firstGuide].authored.positions[node] -
                                 context.models[secondGuide].authored.positions[node];
                const Vec errorA = first.guides[firstGuide].state.positions[node] -
                                   first.guides[secondGuide].state.positions[node] - rest;
                const Vec errorB = second.guides[firstGuide].state.positions[node] -
                                   second.guides[secondGuide].state.positions[node] - rest;
                for (int axis = 0; axis < 3; ++axis)
                    delta += .5L * Coupling * (static_cast<long double>(errorB[axis]) - errorA[axis]) *
                             (static_cast<long double>(errorB[axis]) + errorA[axis]);
            }
    Require(std::isfinite(delta));
    return static_cast<double>(delta / context.energyScale);
}

bool Converged(const Value& value)
{
    return value.gradient.lpNorm<Eigen::Infinity>() <= 1e-10 &&
           value.forceResidual <= 1e-10 && value.torqueResidual <= 1e-10;
}

Matrix GatherQq(const Matrix& matrix)
{
    Matrix qq(18, 18);
    for (int row = 0; row < 18; ++row)
        for (int column = 0; column < 18; ++column)
            qq(row, column) = matrix(FrameIndex(row / 6, 1 + (row % 6) / 3, row % 3),
                                     FrameIndex(column / 6, 1 + (column % 6) / 3, column % 3));
    return qq;
}

void StateCsv(std::ostream& output, const std::string& kind, int tick, double step,
              const std::array<State, 3>& states, const std::array<std::vector<Vec>, 3>& velocity)
{
    for (int guide = 0; guide < 3; ++guide)
        for (int node = 0; node < 4; ++node)
        {
            output << kind << ',' << tick << ',' << tick * step << ',' << guide << ',' << node;
            for (int axis = 0; axis < 3; ++axis) output << ',' << states[guide].positions[node][axis];
            for (int axis = 0; axis < 3; ++axis) output << ',' << velocity[guide][node][axis];
            if (node < 3)
            {
                const auto& frame = states[guide].frames[node];
                output << ',' << frame.w() << ',' << frame.x() << ',' << frame.y() << ',' << frame.z();
            }
            else output << ",NA,NA,NA,NA";
            output << '\n';
        }
    output.flush();
}

struct DynamicResult
{
    std::array<State, 3> finalStates;
    double seconds;
    double pairEnergy;
    double maxPairError;
};

DynamicResult RunTier(const std::filesystem::path& directory, double step)
{
    std::filesystem::create_directory(directory);
    Context context = MakeContext();
    context.step = step;
    context.probe = {Vec(20, 0, 0), Vec::Zero(), Vec(-20, 0, 0)};
    const int planned = static_cast<int>(std::round(.04 / step));
    auto manifest = Open(directory / "manifest.txt");
    manifest << "scope=C2-short-operator-constant-acceleration-v1\nh=" << step
             << "\nplanned_joint_steps=" << planned << "\nend=.04;k=.1;old_initial=authored;v0=0\n"
             << "probe0=20,0,0;probe1=0,0,0;probe2=-20,0,0;active_from_first_step_no_switch\n"
             << "order=guide_major_3*(9x+6eta);roots=x0,q0;solver=one_full45_Newton_direction\n"
             << "Fref=" << context.forceScale << "\nLref=" << context.length << "\nTauref=" << context.torqueScale
             << "\nEref=" << context.energyScale << "\nS=.3,.4,1;B=.001,.0015,.002;m=.001\n"
             << "kG0=" << Spring << "\ngamma=" << Damping << "\ng=0,-9.8,0;ghost=stationary_unshifted_authored\n"
             << "Newton=40;backtrack=25;c1=1e-4;H_eps=1e-5,5e-6;shift=max(0,1e-6-lambda_min)\n"
             << "three_residuals=1e-10;H_asym_diff=1e-6;lambda_min_over_SH>1e-6;qnorm=1e-12;margin=.1\n"
             << "nearroot_rho=1e-8;full_direction_norm=1e-6;unshift_PSD;merit90_half_or_three_residuals\n"
             << "phi_final<=phi_initial+1e-12;balance_scaled<=1e-10;Rrel_error_over_.00036<=1e-10\n"
             << "raw_H=all_accepted_steps_and_all_nearroot_bases;qq=iq9..14,24..29,39..44\n"
             << "no_k0_dynamic;no_original_3s_behavior;no_longrun;independentAudit=PENDING\n";
    manifest.flush();
    auto states = Open(directory / "states.csv");
    states << "kind,tick,time,guide,node,x,y,z,vx,vy,vz,qw,qx,qy,qz\n";
    auto steps = Open(directory / "steps.csv");
    steps << "tick,time,status,iterations,chart_inf,force_scaled,torque_scaled,merit90,lambda_min,SH,H_asym,H_difference,qq_min,phi_initial,phi_final,root_x_error,root_q_error,qnorm_error,balance_scaled,Rrel_error_scaled,Urel,max_pair_error,cross_H_norm\n";
    auto directions = Open(directory / "directions.csv");
    directions << "tick,iteration,coordinate,z,g,p,shift\n";
    auto trials = Open(directory / "trials.csv");
    trials << "tick,iteration,backtrack,alpha,mode,base_rho,base_merit,trial_merit,full_direction_norm,base_lambda_min,base_H_asym,base_H_difference,direction_residual,stable_delta,Armijo_rhs,trial_phi,trial_chart,trial_force,trial_torque,accepted,reason,domain_valid\n";
    auto matrices = Open(directory / "hessians.csv");
    matrices << "case,row,column,value\n";
    auto spectra = Open(directory / "spectra.csv");
    spectra << "tick,kind,index,eigenvalue\n";
    auto residuals = Open(directory / "residuals.csv");
    residuals << "tick,coordinate,guide,kind,node_or_segment,axis,z,g,physical_residual,scale\n";
    auto forces = Open(directory / "forces.csv");
    forces << "tick,guide,node,potential_fx,potential_fy,potential_fz,pair_fx,pair_fy,pair_fz,rx,ry,rz,body_tau_x,body_tau_y,body_tau_z,root_reaction_fx,root_reaction_fy,root_reaction_fz,root_reaction_tau_x,root_reaction_tau_y,root_reaction_tau_z\n";
    auto pairs = Open(directory / "pair.csv");
    pairs << "tick,a,b,node,ex,ey,ez,fa_x,fa_y,fa_z,fb_x,fb_y,fb_z,moment_x,moment_y,moment_z,exact_moment_x,exact_moment_y,exact_moment_z\n";
    auto energy = Open(directory / "energy.csv");
    energy << "tick,time,K_old,K_next,U_old,U_next,Urel_old,Urel_next,W0,W1,W2,damping_loss,inertial_loss,R,Rrel,Rrel_exact,Rrel_error,balance_J,balance_scale,balance_scaled\n";
    auto guideEnergy = Open(directory / "guide-energy.csv");
    guideEnergy << "tick,guide,U_ss,U_bt,U_G0,U_g,K_old,K_next,Wprobe,Wcoupling_node,damping_loss,inertial_loss,chart_inf,force_scaled,torque_scaled\n";
    StateCsv(states, "initial", 0, step, context.old, context.velocity);
    const auto started = std::chrono::steady_clock::now();
    int tick = 1;
    Vector unknown = Fixture(context, false);
    double lastPairEnergy = 0.0, maxPairError = 0.0;
    int rescues = 0;
    try
    {
        for (; tick <= planned; ++tick)
        {
            unknown = Vector::Zero(45);
            for (int guide = 0; guide < 3; ++guide)
                for (int node = 1; node < 4; ++node)
                    unknown.segment<3>(PositionIndex(guide, node, 0)) = context.old[guide].positions[node] / context.length;
            StateCsv(states, "old", tick - 1, step, context.old, context.velocity);
            Value last = At(context, unknown, Coupling);
            const double initial = last.phi;
            int iteration = 0;
            for (; iteration < 40; ++iteration)
            {
                last = At(context, unknown, Coupling);
                if (Converged(last)) break;
                const Matrix raw = Hessian(context, unknown, Coupling, 1e-5);
                const Matrix symmetric = .5 * (raw + raw.transpose());
                Eigen::SelfAdjointEigenSolver<Matrix> eig(symmetric);
                Require(eig.info() == Eigen::Success && eig.eigenvalues().allFinite());
                const double shift = std::max(0.0, 1e-6 - eig.eigenvalues()[0]);
                const Matrix shifted = symmetric + shift * Matrix::Identity(45, 45);
                Eigen::FullPivLU<Matrix> solve(shifted);
                Require(solve.isInvertible());
                const Vector direction = solve.solve(-last.gradient);
                Require(direction.allFinite());
                const double rho = std::max({last.gradient.lpNorm<Eigen::Infinity>(), last.forceResidual, last.torqueResidual});
                const double denominator = shifted.norm() * direction.norm() + last.gradient.norm();
                const double directionResidual = denominator > 0.0 ?
                    (shifted * direction + last.gradient).norm() / denominator : 0.0;
                bool nearroot = false;
                double asymmetry = 0.0, difference = 0.0;
                if (rho <= 1e-8 && direction.norm() <= 1e-6 && eig.eigenvalues()[0] >= 0.0)
                {
                    const Matrix fine = Hessian(context, unknown, Coupling, 5e-6);
                    const double scale = std::max(1.0, eig.eigenvalues().cwiseAbs().maxCoeff());
                    asymmetry = Norm(raw - raw.transpose()) / scale;
                    difference = Norm(raw - fine) / scale;
                    nearroot = asymmetry <= 1e-6 && difference <= 1e-6 && std::isfinite(directionResidual);
                    const std::string label = "base_" + std::to_string(tick) + "_" + std::to_string(iteration);
                    MatrixCsv(matrices, label + "_H1", raw);
                    MatrixCsv(matrices, label + "_H2", fine);
                }
                for (int coordinate = 0; coordinate < 45; ++coordinate)
                    directions << tick << ',' << iteration << ',' << coordinate << ',' << unknown[coordinate]
                               << ',' << last.gradient[coordinate] << ',' << direction[coordinate] << ',' << shift << '\n';
                bool accepted = false;
                double alpha = 1.0;
                for (int backtrack = 0; backtrack < 25; ++backtrack, alpha *= .5)
                {
                    try
                    {
                        const Vector trial = unknown + alpha * direction;
                        const Value candidate = At(context, trial, Coupling);
                        const double stable = StableDelta(context, unknown, trial, last, candidate);
                        const double rhs = 1e-4 * alpha * last.gradient.dot(direction);
                        const bool final = Converged(candidate);
                        const bool shrink = candidate.merit <= .5 * last.merit;
                        accepted = nearroot ? (shrink || final) : stable <= rhs;
                        const std::string reason = !accepted ? "reject" :
                            (nearroot ? (shrink ? "merit_half" : "three_residuals") : "stable_Armijo");
                        trials << tick << ',' << iteration << ',' << backtrack << ',' << alpha << ','
                               << (nearroot ? "nearroot_merit" : "energy_Armijo") << ',' << rho << ',' << last.merit
                               << ',' << candidate.merit << ',' << direction.norm() << ',' << eig.eigenvalues()[0]
                               << ',' << asymmetry << ',' << difference << ',' << directionResidual << ','
                               << stable << ',' << rhs << ',' << candidate.phi << ','
                               << candidate.gradient.lpNorm<Eigen::Infinity>() << ',' << candidate.forceResidual
                               << ',' << candidate.torqueResidual << ',' << accepted << ',' << reason << ",1\n";
                        if (accepted)
                        {
                            if (nearroot) ++rescues;
                            break;
                        }
                    }
                    catch (const std::runtime_error&)
                    {
                        trials << tick << ',' << iteration << ',' << backtrack << ',' << alpha
                               << ",domain_reject," << rho << ',' << last.merit
                               << ",NA," << direction.norm() << ',' << eig.eigenvalues()[0] << ','
                               << asymmetry << ',' << difference << ',' << directionResidual
                               << ",NA,NA,NA,NA,NA,NA,0,domain_reject,0\n";
                    }
                }
                trials.flush();
                directions.flush();
                if (!accepted) throw std::runtime_error("SOLVER_FAILURE: ARMIJO_EXHAUSTED");
                unknown += alpha * direction;
            }
            last = At(context, unknown, Coupling);
            if (!Converged(last)) throw std::runtime_error("SOLVER_FAILURE: ITERATION_LIMIT40");
            const Matrix first = Hessian(context, unknown, Coupling, 1e-5);
            const Matrix second = Hessian(context, unknown, Coupling, 5e-6);
            MatrixCsv(matrices, "accepted_" + std::to_string(tick) + "_H1", first);
            MatrixCsv(matrices, "accepted_" + std::to_string(tick) + "_H2", second);
            const Matrix symmetric = .5 * (first + first.transpose());
            const Matrix qq = GatherQq(symmetric);
            MatrixCsv(matrices, "accepted_" + std::to_string(tick) + "_qq", qq);
            Eigen::SelfAdjointEigenSolver<Matrix> eig(symmetric), qeig(qq);
            Require(eig.info() == Eigen::Success && qeig.info() == Eigen::Success &&
                    eig.eigenvalues().allFinite() && qeig.eigenvalues().allFinite());
            const double hscale = std::max(1.0, eig.eigenvalues().cwiseAbs().maxCoeff());
            const double asymmetry = Norm(first - first.transpose()) / hscale;
            const double difference = Norm(first - second) / hscale;
            for (int index = 0; index < 45; ++index)
                spectra << tick << ",full," << index << ',' << eig.eigenvalues()[index] << '\n';
            for (int index = 0; index < 18; ++index)
                spectra << tick << ",qq_gather," << index << ',' << qeig.eigenvalues()[index] << '\n';
            const PairValue oldPair = PairAt(context, context.old, Coupling);
            double oldU = oldPair.energy, nextU = last.pair.energy, oldK = 0.0, nextK = 0.0;
            double damping = 0.0, inertial = 0.0, forceWork = 0.0, pairWork = 0.0;
            double rootX = 0.0, rootQ = 0.0, qnorm = 0.0, pairError = 0.0;
            std::array<double, 3> work{};
            std::array<State, 3> nextStates;
            std::array<std::vector<Vec>, 3> nextVelocity;
            for (int guide = 0; guide < 3; ++guide)
            {
                const auto& single = last.guides[guide];
                nextStates[guide] = single.state;
                nextVelocity[guide].assign(4, Vec::Zero());
                oldU += Evaluate(context.models[guide], context.old[guide],
                                 context.models[guide].authored.positions, Vec(0, -9.8, 0)).Energy();
                nextU += single.potential.Energy();
                rootX = std::max(rootX, (nextStates[guide].positions[0] - context.models[guide].authored.positions[0]).norm());
                rootQ = std::max(rootQ, (nextStates[guide].frames[0].coeffs() -
                                        context.models[guide].authored.frames[0].coeffs()).norm());
                for (const auto& frame : nextStates[guide].frames) qnorm = std::max(qnorm, std::abs(frame.norm() - 1.0));
                double guideOldK = 0.0, guideNextK = 0.0, guidePairWork = 0.0, guideDamping = 0.0, guideInertial = 0.0;
                double guideForce = 0.0, guideTorque = 0.0;
                for (int node = 1; node < 4; ++node)
                {
                    const Vec delta = nextStates[guide].positions[node] - context.old[guide].positions[node];
                    nextVelocity[guide][node] = delta / step;
                    guideOldK += .5 * .001 * context.velocity[guide][node].squaredNorm();
                    guideNextK += .5 * .001 * nextVelocity[guide][node].squaredNorm();
                    guideDamping += step * Damping * nextVelocity[guide][node].squaredNorm();
                    guideInertial += .5 * .001 * (nextVelocity[guide][node] - context.velocity[guide][node]).squaredNorm();
                    forceWork += (single.potential.force[node] + last.pair.force[guide][node]).dot(delta);
                    guidePairWork += last.pair.force[guide][node].dot(delta);
                    work[guide] += .001 * context.probe[guide].dot(delta);
                    guideForce = std::max(guideForce, single.residual[node].norm() / context.forceScale);
                }
                for (int segment = 1; segment < 3; ++segment)
                    guideTorque = std::max(guideTorque, single.potential.bodyTorque[segment].norm() / context.torqueScale);
                oldK += guideOldK;
                nextK += guideNextK;
                damping += guideDamping;
                inertial += guideInertial;
                pairWork += guidePairWork;
                guideEnergy << tick << ',' << guide << ',' << single.potential.shearStretch << ','
                            << single.potential.bendTwist << ',' << single.potential.ghost << ',' << single.potential.gravity
                            << ',' << guideOldK << ',' << guideNextK << ',' << work[guide] << ',' << guidePairWork
                            << ',' << guideDamping << ',' << guideInertial << ','
                            << last.gradient.segment(15 * guide, 15).lpNorm<Eigen::Infinity>() << ','
                            << guideForce << ',' << guideTorque << '\n';
                for (int local = 0; local < 15; ++local)
                {
                    const int coordinate = 15 * guide + local;
                    const bool position = local < 9;
                    const int nodeOrSegment = position ? 1 + local / 3 : 1 + (local - 9) / 3;
                    const int axis = local % 3;
                    const double physical = position ? single.residual[nodeOrSegment][axis] :
                        single.potential.bodyTorque[nodeOrSegment][axis];
                    residuals << tick << ',' << coordinate << ',' << guide << ',' << (position ? "force" : "body_torque")
                              << ',' << nodeOrSegment << ',' << axis << ',' << unknown[coordinate] << ','
                              << last.gradient[coordinate] << ',' << physical << ','
                              << (position ? context.forceScale : context.torqueScale) << '\n';
                }
                for (int node = 0; node < 4; ++node)
                {
                    forces << tick << ',' << guide << ',' << node;
                    for (const Vec& vector : {single.potential.force[node], last.pair.force[guide][node], single.residual[node]})
                        for (int axis = 0; axis < 3; ++axis) forces << ',' << vector[axis];
                    if (node < 3)
                        for (int axis = 0; axis < 3; ++axis) forces << ',' << single.potential.bodyTorque[node][axis];
                    else forces << ",NA,NA,NA";
                    if (node == 0)
                    {
                        for (int axis = 0; axis < 3; ++axis) forces << ',' << -single.potential.force[0][axis];
                        for (int axis = 0; axis < 3; ++axis) forces << ',' << -single.potential.bodyTorque[0][axis];
                    }
                    else forces << ",NA,NA,NA,NA,NA,NA";
                    forces << '\n';
                }
            }
            double exactRrel = 0.0;
            for (int firstGuide = 0; firstGuide < 3; ++firstGuide)
                for (int secondGuide = firstGuide + 1; secondGuide < 3; ++secondGuide)
                    for (int node = 1; node < 4; ++node)
                    {
                        const Vec relative = nextStates[firstGuide].positions[node] - nextStates[secondGuide].positions[node];
                        const Vec rest = context.models[firstGuide].authored.positions[node] -
                                         context.models[secondGuide].authored.positions[node];
                        const Vec error = relative - rest;
                        const Vec forceA = -Coupling * error, forceB = Coupling * error;
                        const Vec moment = nextStates[firstGuide].positions[node].cross(forceA) +
                                           nextStates[secondGuide].positions[node].cross(forceB);
                        const Vec exactMoment = Coupling * relative.cross(rest);
                        pairError = std::max(pairError, error.norm());
                        const Vec deltaA = nextStates[firstGuide].positions[node] - context.old[firstGuide].positions[node];
                        const Vec deltaB = nextStates[secondGuide].positions[node] - context.old[secondGuide].positions[node];
                        exactRrel -= .5 * Coupling * (deltaA - deltaB).squaredNorm();
                        pairs << tick << ',' << firstGuide << ',' << secondGuide << ',' << node;
                        for (const Vec& vector : {error, forceA, forceB, moment, exactMoment})
                            for (int axis = 0; axis < 3; ++axis) pairs << ',' << vector[axis];
                        pairs << '\n';
                    }
            const double remainder = nextU - oldU + forceWork;
            const double rrel = last.pair.energy - oldPair.energy + pairWork;
            const double probeWork = work[0] + work[1] + work[2];
            const double balance = nextK + nextU - oldK - oldU - probeWork + damping + inertial - remainder;
            const double balanceScale = context.energyScale + std::abs(nextK + nextU) +
                std::abs(oldK + oldU) + std::abs(probeWork) + damping + inertial + std::abs(remainder);
            Matrix cross = first;
            for (int guide = 0; guide < 3; ++guide) cross.block(15 * guide, 15 * guide, 15, 15).setZero();
            const bool pass = asymmetry <= 1e-6 && difference <= 1e-6 &&
                eig.eigenvalues()[0] / hscale > 1e-6 && last.phi <= initial + 1e-12 &&
                rootX == 0.0 && rootQ == 0.0 && qnorm <= 1e-12 &&
                std::abs(balance) / balanceScale <= 1e-10 && std::abs(rrel - exactRrel) / .00036 <= 1e-10;
            steps << tick << ',' << tick * step << ',' << (pass ? "PASS" : "FAIL") << ',' << iteration
                  << ',' << last.gradient.lpNorm<Eigen::Infinity>() << ',' << last.forceResidual << ','
                  << last.torqueResidual << ',' << last.merit << ',' << eig.eigenvalues()[0] << ',' << hscale
                  << ',' << asymmetry << ',' << difference << ',' << qeig.eigenvalues()[0] << ','
                  << initial << ',' << last.phi << ',' << rootX << ',' << rootQ << ',' << qnorm
                  << ',' << std::abs(balance) / balanceScale << ',' << std::abs(rrel - exactRrel) / .00036
                  << ',' << last.pair.energy << ',' << pairError << ',' << cross.norm() << '\n';
            steps.flush();
            energy << tick << ',' << tick * step << ',' << oldK << ',' << nextK << ',' << oldU << ',' << nextU
                   << ',' << oldPair.energy << ',' << last.pair.energy << ',' << work[0] << ',' << work[1]
                   << ',' << work[2] << ',' << damping << ',' << inertial << ',' << remainder << ','
                   << rrel << ',' << exactRrel << ',' << rrel - exactRrel << ',' << balance << ','
                   << balanceScale << ',' << std::abs(balance) / balanceScale << '\n';
            if (!pass) throw std::runtime_error("NUMERIC_STEP_GATE_FAILURE");
            context.old = nextStates;
            context.velocity = nextVelocity;
            StateCsv(states, "accepted", tick, step, context.old, context.velocity);
            lastPairEnergy = last.pair.energy;
            maxPairError = std::max(maxPairError, pairError);
            if (tick == 1 || tick % 20 == 0 || tick == planned)
            {
                energy.flush();
                matrices.flush();
                std::cout << "C2 D h=" << step << " accepted_joint_step=" << tick
                          << " rho=" << std::max({last.gradient.lpNorm<Eigen::Infinity>(), last.forceResidual, last.torqueResidual})
                          << " Urel=" << last.pair.energy << " max_pair_error=" << pairError << std::endl;
            }
        }
    }
    catch (const std::exception& error)
    {
        auto failure = Open(directory / "failure.txt");
        failure << "reason=" << error.what() << "\ntick=" << tick << "\nh=" << step << '\n';
        const Value failed = At(context, unknown, Coupling);
        failure << "chart_inf=" << failed.gradient.lpNorm<Eigen::Infinity>() << "\nforce_scaled="
                << failed.forceResidual << "\ntorque_scaled=" << failed.torqueResidual << '\n';
        std::array<State, 3> candidate;
        for (int guide = 0; guide < 3; ++guide) candidate[guide] = failed.guides[guide].state;
        StateCsv(states, "failed_candidate_old_velocity", tick, step, candidate, context.velocity);
        for (int coordinate = 0; coordinate < 45; ++coordinate)
            failure << "z" << coordinate << '=' << unknown[coordinate] << "\ng" << coordinate << '=' << failed.gradient[coordinate] << '\n';
        MatrixCsv(matrices, "failed_" + std::to_string(tick) + "_H1", Hessian(context, unknown, Coupling, 1e-5));
        MatrixCsv(matrices, "failed_" + std::to_string(tick) + "_H2", Hessian(context, unknown, Coupling, 5e-6));
        auto summary = Open(directory / "summary.csv");
        summary << "item,status,value\nD_numerical,FAIL," << tick << "\naccepted_joint_steps,INFO,"
                << tick - 1 << "\nindependentAudit,PENDING,0\n";
        throw;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value\nD_numerical,PASS," << planned << "\nwall_seconds,INFO," << seconds
            << "\nnearroot_acceptances,INFO," << rescues << "\nUrel_final_J,INFO," << lastPairEnergy
            << "\nmax_pair_error_m,INFO," << maxPairError << "\nindependentAudit,PENDING,0\nC3,NOT_RUN,0\nvolume,UNSUPPORTED,0\n";
    return DynamicResult{context.old, seconds, lastPairEnergy, maxPairError};
}

int Startup(const std::filesystem::path& directory)
{
    auto manifest = Open(directory / "manifest.txt");
    manifest << "scope=C2-short-operator-constant-acceleration-v1\njoint_steps=40,80,160;total=280\n"
             << "h=.001,.0005,.00025;end=.04;initial_each_tier=authored_v0\n"
             << "acceleration=+20X,0,-20X;k=.1;no_k0_dynamic;no_coupled_longrun\n"
             << "order_gate=per_guide_position_L2_over_Lref_and_max_SO3;errors_coarse>fine>1e-9;order=.5..1.5\n"
             << "independentAudit=PENDING;C3=NOT_RUN;volume=UNSUPPORTED\n";
    manifest.flush();
    std::vector<DynamicResult> results;
    int total = 0;
    try
    {
        const double steps[] = {.001, .0005, .00025};
        const std::string labels[] = {"h001", "h0005", "h00025"};
        for (int level = 0; level < 3; ++level)
        {
            results.push_back(RunTier(directory / labels[level], steps[level]));
            total += static_cast<int>(std::round(.04 / steps[level]));
        }
    }
    catch (...)
    {
        auto summary = Open(directory / "summary.csv");
        summary << "item,status,value\nD_startup,FAIL," << total
                << "\ncompleted_joint_steps_previous_tiers,INFO," << total
                << "\nindependentAudit,PENDING,0\nC3,NOT_RUN,0\nvolume,UNSUPPORTED,0\n";
        throw;
    }
    auto refinement = Open(directory / "refinement.csv");
    refinement << "guide,kind,error_h_h2,error_h2_h4,order,status\n";
    const double length = MakeContext().length;
    bool pass = true;
    double jointSquare[2] = {0.0, 0.0}, jointRotation[2] = {0.0, 0.0};
    for (int guide = 0; guide < 3; ++guide)
    {
        double position[2], rotation[2];
        for (int level = 0; level < 2; ++level)
        {
            const auto& first = results[level].finalStates[guide];
            const auto& second = results[level + 1].finalStates[guide];
            double square = 0.0, angle = 0.0;
            for (int node = 1; node < 4; ++node)
                square += (first.positions[node] - second.positions[node]).squaredNorm();
            for (int segment = 1; segment < 3; ++segment)
                angle = std::max(angle, LogR(first.frames[segment].conjugate() * second.frames[segment], .1).norm());
            position[level] = std::sqrt(square) / length;
            rotation[level] = angle;
            jointSquare[level] += square / (length * length);
            jointRotation[level] = std::max(jointRotation[level], angle);
        }
        for (int metric = 0; metric < 2; ++metric)
        {
            const double* errors = metric == 0 ? position : rotation;
            const double order = std::log2(errors[0] / errors[1]);
            const bool resolved = errors[1] > 1e-9;
            const bool valid = resolved && errors[0] > errors[1] && std::isfinite(order) && order >= .5 && order <= 1.5;
            pass = pass && valid;
            refinement << guide << ',' << (metric == 0 ? "position" : "SO3") << ',' << errors[0] << ',' << errors[1]
                       << ',' << order << ',' << (!resolved ? "ORDER_UNRESOLVED" : (valid ? "PASS" : "FAIL")) << '\n';
        }
    }
    refinement << "all,position_report_only," << std::sqrt(jointSquare[0]) << ',' << std::sqrt(jointSquare[1])
               << ',' << std::log2(std::sqrt(jointSquare[0] / jointSquare[1])) << ",REPORT_ONLY\n"
               << "all,SO3_report_only," << jointRotation[0] << ',' << jointRotation[1] << ','
               << std::log2(jointRotation[0] / jointRotation[1]) << ",REPORT_ONLY\n";
    double seconds = 0.0;
    for (const auto& result : results) seconds += result.seconds;
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value\nD_startup," << (pass ? "PASS" : "FAIL") << ',' << total
            << "\nwall_seconds,INFO," << seconds << "\nindependentAudit,PENDING,0\nC3,NOT_RUN,0\nvolume,UNSUPPORTED,0\n";
    std::cout << "C2 D joint_steps=" << total << " refinement=" << (pass ? "PASS" : "FAIL")
              << " independentAudit=PENDING" << std::endl;
    return pass ? 0 : 2;
}
}
