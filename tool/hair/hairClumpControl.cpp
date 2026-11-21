#include "hairClumpControl.h"
#include "rodGhostPrototypeMath.h"
#include <Eigen/Eigenvalues>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace C0
{
using namespace R1;
using Vector = Eigen::VectorXd;
using Matrix = Eigen::MatrixXd;
constexpr double Spring = 3.24777679370055;
constexpr double Damping = .056989269110075014;

std::ofstream Open(const std::filesystem::path& path)
{
    std::ofstream output(path);
    if (!output) throw std::runtime_error("OUTPUT_FAILURE");
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << std::setprecision(17);
    return output;
}

struct Context
{
    Model model;
    State old;
    std::vector<Vec> velocity;
    double length;
    double forceScale;
    double torqueScale;
    double energyScale;
    double step;
    Vec probe = Vec::Zero();
};

struct Value
{
    State state;
    Evaluation energy;
    Vector gradient;
    double phi;
    double forceResidual = 0.0;
    double torqueResidual = 0.0;
    double merit = 0.0;
};

Value At(const Context& context, const Vector& unknown)
{
    Value value;
    value.state = context.old;
    value.gradient = Vector::Zero(15);
    for (int node = 1; node < 4; ++node)
        value.state.positions[node] = context.length * unknown.segment<3>(3 * (node - 1));
    for (int segment = 1; segment < 3; ++segment)
        value.state.frames[segment] = Unit(context.old.frames[segment] *
            ExpR(unknown.segment<3>(9 + 3 * (segment - 1)), .1));
    value.energy = Evaluate(context.model, value.state, context.model.authored.positions, Vec(0, -9.8, 0));
    double phi = value.energy.Energy();
    for (int node = 1; node < 4; ++node)
    {
        const Vec delta = value.state.positions[node] - context.old.positions[node];
        const Vec error = delta - context.step * context.velocity[node];
        phi += .5 * .001 * error.squaredNorm() / (context.step * context.step) +
               .5 * Damping * delta.squaredNorm() / context.step;
        phi -= .001 * context.probe.dot(value.state.positions[node]);
        const Vec residual = .001 * error / (context.step * context.step) +
                             Damping * delta / context.step - value.energy.force[node] - .001 * context.probe;
        value.gradient.segment<3>(3 * (node - 1)) = residual * context.length / context.energyScale;
        value.forceResidual = std::max(value.forceResidual, residual.norm() / context.forceScale);
        value.merit += .5 * residual.squaredNorm() / (context.forceScale * context.forceScale);
    }
    for (int segment = 1; segment < 3; ++segment)
    {
        const Vec eta = unknown.segment<3>(9 + 3 * (segment - 1));
        value.gradient.segment<3>(9 + 3 * (segment - 1)) =
            -RightJacobian(eta).transpose() * value.energy.bodyTorque[segment] / context.energyScale;
        value.torqueResidual = std::max(value.torqueResidual,
            value.energy.bodyTorque[segment].norm() / context.torqueScale);
        value.merit += .5 * value.energy.bodyTorque[segment].squaredNorm() /
                       (context.torqueScale * context.torqueScale);
    }
    value.phi = phi / context.energyScale;
    value.merit += .5 * value.gradient.squaredNorm();
    Require(std::isfinite(value.phi) && value.gradient.allFinite());
    return value;
}

double StableDelta(const Context& context, const Vector& base, const Vector& trial,
                   const Value& first, const Value& second)
{
    long double delta = 0.0L;
    for (int segment = 0; segment < 3; ++segment)
    {
        const double length = context.model.lengths[segment];
        const Vec edgeA = first.state.positions[segment + 1] - first.state.positions[segment];
        const Vec edgeB = second.state.positions[segment + 1] - second.state.positions[segment];
        const Vec strainA = first.state.frames[segment].conjugate() * edgeA / length - context.model.restStrains[segment];
        const Vec strainB = second.state.frames[segment].conjugate() * edgeB / length - context.model.restStrains[segment];
        for (int axis = 0; axis < 3; ++axis)
            delta += .5L * length * (static_cast<long double>(strainB[axis]) - strainA[axis]) *
                     context.model.shearStretch[axis] * (static_cast<long double>(strainB[axis]) + strainA[axis]);
        if (segment < 2)
        {
            const Vec angleA = LogR(first.state.frames[segment].conjugate() * first.state.frames[segment + 1], .1) -
                               context.model.restRotations[segment];
            const Vec angleB = LogR(second.state.frames[segment].conjugate() * second.state.frames[segment + 1], .1) -
                               context.model.restRotations[segment];
            const double dual = .5 * (length + context.model.lengths[segment + 1]);
            for (int axis = 0; axis < 3; ++axis)
                delta += .5L * (static_cast<long double>(angleB[axis]) - angleA[axis]) *
                    context.model.bendTwist[axis] * (static_cast<long double>(angleB[axis]) + angleA[axis]) / dual;
        }
    }
    for (int node = 1; node < 4; ++node)
        for (int axis = 0; axis < 3; ++axis)
        {
            const long double dx = context.length *
                (static_cast<long double>(trial[3 * (node - 1) + axis]) - base[3 * (node - 1) + axis]);
            const long double offsetA = context.model.authored.positions[node][axis] - first.state.positions[node][axis];
            const long double offsetB = context.model.authored.positions[node][axis] - second.state.positions[node][axis];
            delta -= .5L * Spring * dx * (offsetB + offsetA);
            if (axis == 1) delta += .001L * 9.8L * dx;
            delta -= .001L * context.probe[axis] * dx;
            const long double deltaA = first.state.positions[node][axis] - context.old.positions[node][axis];
            const long double deltaB = second.state.positions[node][axis] - context.old.positions[node][axis];
            const long double predictionA = deltaA - context.step * context.velocity[node][axis];
            const long double predictionB = deltaB - context.step * context.velocity[node][axis];
            delta += .5L * .001L / (context.step * context.step) * dx * (predictionB + predictionA);
            delta += .5L * Damping / context.step * dx * (deltaB + deltaA);
        }
    Require(std::isfinite(delta));
    return static_cast<double>(delta / context.energyScale);
}

Matrix Hessian(const Context& context, const Vector& unknown, double epsilon)
{
    Matrix matrix(15, 15);
    for (int coordinate = 0; coordinate < 15; ++coordinate)
    {
        Vector plus = unknown, minus = unknown;
        plus[coordinate] += epsilon;
        minus[coordinate] -= epsilon;
        matrix.col(coordinate) = (At(context, plus).gradient - At(context, minus).gradient) / (2 * epsilon);
    }
    Require(matrix.allFinite());
    return matrix;
}

double Norm(const Matrix& matrix)
{
    Eigen::JacobiSVD<Matrix> decomposition(matrix);
    Require(decomposition.info() == Eigen::Success && decomposition.singularValues().allFinite());
    return decomposition.singularValues()[0];
}

bool Converged(const Value& value)
{
    return value.gradient.lpNorm<Eigen::Infinity>() <= 1e-10 &&
           value.forceResidual <= 1e-10 && value.torqueResidual <= 1e-10;
}

struct Entry
{
    bool allowed;
    double rho;
    double minimum;
    double asymmetry;
    double difference;
    double directionResidual;
};

Entry NearRoot(const Context& context, const Vector& unknown, const Value& base,
               const Vector& direction, const Matrix& raw, const Matrix& symmetric,
               const Eigen::VectorXd& eigenvalues, double shift)
{
    Entry entry{false, std::max({base.gradient.lpNorm<Eigen::Infinity>(), base.forceResidual, base.torqueResidual}),
                eigenvalues[0], 0.0, 0.0, 0.0};
    const Matrix shifted = symmetric + shift * Matrix::Identity(15, 15);
    const double denominator = shifted.norm() * direction.norm() + base.gradient.norm();
    entry.directionResidual = denominator > 0.0 ?
        (shifted * direction + base.gradient).norm() / denominator : 0.0;
    if (entry.rho <= 1e-8 && direction.norm() <= 1e-6 && entry.minimum >= 0.0)
    {
        const Matrix fine = Hessian(context, unknown, 5e-6);
        const double scale = std::max(1.0, eigenvalues.cwiseAbs().maxCoeff());
        entry.asymmetry = Norm(raw - raw.transpose()) / scale;
        entry.difference = Norm(raw - fine) / scale;
        entry.allowed = entry.asymmetry <= 1e-6 && entry.difference <= 1e-6 &&
                        std::isfinite(entry.directionResidual) && direction.allFinite();
    }
    return entry;
}

void StateCsv(std::ostream& output, const std::string& kind, int tick, const Context& context,
              const State& state, const std::vector<Vec>& velocity)
{
    for (int node = 0; node < 4; ++node)
    {
        output << kind << ',' << tick << ',' << tick * context.step << ',' << node;
        for (int axis = 0; axis < 3; ++axis) output << ',' << state.positions[node][axis];
        for (int axis = 0; axis < 3; ++axis) output << ',' << velocity[node][axis];
        if (node < 3)
        {
            const auto& frame = state.frames[node];
            output << ',' << frame.w() << ',' << frame.x() << ',' << frame.y() << ',' << frame.z();
        }
        else output << ",NA,NA,NA,NA";
        output << '\n';
    }
    output.flush();
}

void MatrixCsv(std::ostream& output, int tick, const std::string& kind, const Matrix& matrix)
{
    for (int row = 0; row < matrix.rows(); ++row)
        for (int column = 0; column < matrix.cols(); ++column)
            output << tick << ',' << kind << ',' << row << ',' << column << ',' << matrix(row, column) << '\n';
}

Context MakeContext(int guide, double step)
{
    State authored;
    authored.positions = {Vec(0, 0, 0), Vec(.01, 0, .1), Vec(.02, .005, .2), Vec(.025, .015, .3)};
    authored.frames = {Quat::Identity(), ExpR(Vec(.03, -.02, .04), .1), ExpR(Vec(.05, .01, .08), .1)};
    Model model = AuthoredModel(authored, Vec(.3, .4, 1), Vec(.001, .0015, .002), .1);
    model.masses.assign(4, .001);
    model.ghostStiffness = {0, Spring, Spring, Spring};
    const Vec offsets[] = {Vec(0, 0, 0), Vec(.06, 0, 0), Vec(.03, .052, 0)};
    for (auto& position : authored.positions) position += offsets[guide];
    model.authored = authored;
    const double length = *std::max_element(model.lengths.begin(), model.lengths.end());
    const double forceScale = 1.0 + Spring * length + .001 * (9.8 + 20.0);
    double joint = 0.0;
    for (int index = 0; index < 2; ++index)
        joint = std::max(joint, .002 / (.5 * (model.lengths[index] + model.lengths[index + 1])));
    const double torqueScale = length + joint;
    return Context{model, authored, std::vector<Vec>(4, Vec::Zero()), length, forceScale,
                   torqueScale, forceScale * length + torqueScale, step};
}

struct StartupResult
{
    State state;
    double seconds;
    double length;
    std::vector<State> trajectory;
    std::vector<double> speeds;
};

bool RawCheckpoint(int tick)
{
    if (tick == 1 || tick % 100 == 0 || tick == 13700) return true;
    for (int boundary : {3000, 3100, 3200, 3500, 3600, 3700})
        if (std::abs(tick - boundary) <= 1) return true;
    return false;
}

StartupResult RunStartup(const std::filesystem::path& directory, int guide, double step, bool control = false)
{
    std::filesystem::create_directory(directory);
    Context context = MakeContext(guide, step);
    const int planned = control ? 13700 : static_cast<int>(std::round(.04 / step));
    auto manifest = Open(directory / "manifest.txt");
    manifest << "scope=" << (control ? "C0-independent-control-v3" : "C0-independent-startup-v3")
             << "\nsolver_dof=15\nguide=" << guide << "\nh=" << step
             << "\nplanned_steps=" << planned << "\nend=" << (control ? 13.7 : .04) << "\nroot=x0,q0;x1=free\n"
             << "S=.3,.4,1;B=.001,.0015,.002;m=.001\nk=" << Spring << "\ngamma=" << Damping
             << "\ng=0,-9.8,0\nprobe=" << (control ? "+X20_async" : "0")
             << ";coupling=none;ghost=authored_stationary_unshifted\n"
             << "Newton=40;backtrack=25;Armijo=1e-4;margin=.1;qnorm=1e-12\n"
             << "residuals=1e-10;H_eps=1e-5,5e-6;H_asym_diff=1e-6;lambda_over_SH>1e-6\n"
             << "nearroot_rho=1e-8;full_direction=1e-6;unshift_PSD;merit_half_or_three_residuals\n"
             << "Lref=" << context.length << "\nFref=" << context.forceScale
             << "\nTauref=" << context.torqueScale << "\nEref=" << context.energyScale << '\n';
    if (control)
        manifest << "probe_first_step=" << 3001 + 100 * guide << "\nprobe_last_step=" << 3500 + 100 * guide
                 << "\nstatic_qualification=2..3;snapshot=3;tail=12.7..13.7;own_recovery=3.5+guide*.1\n"
                 << "raw_H=1,every100,all_six_switch_boundaries_plusminus1,13700\n"
                 << "all_steps=two_unshift_H_gates,spectrum,three_residuals,domain,objective,energy\n";
    manifest.flush();
    auto states = Open(directory / "states.csv");
    states << "kind,tick,time,node,x,y,z,vx,vy,vz,qw,qx,qy,qz\n";
    auto reports = Open(directory / "steps.csv");
    reports << "tick,time,status,iterations,gradient_inf,force_scaled,torque_scaled,lambda_min,SH,H_asym,H_difference,qq_min,phi_initial,phi_final,balance_J,balance_scaled,root_error,qroot_error,qnorm_error\n";
    auto trials = Open(directory / "trials.csv");
    trials << "tick,iteration,backtrack,alpha,stable_delta,Armijo_rhs,mode,rho_base,merit_base,merit_trial,full_direction_norm,unshift_lambda_min,H_asym,H_diff,direction_residual,trial_gradient,trial_force,trial_torque,accepted,domain_valid\n";
    auto directions = Open(directory / "directions.csv");
    directions << "tick,iteration,coordinate,z,gradient,direction,shift\n";
    auto matrices = Open(directory / "hessians.csv");
    matrices << "tick,kind,row,column,value\n";
    auto spectra = Open(directory / "spectra.csv");
    spectra << "tick,index,eigenvalue\n";
    auto residuals = Open(directory / "residuals.csv");
    residuals << "tick,coordinate,kind,gradient_z,physical_residual,scale\n";
    auto energy = Open(directory / "energy.csv");
    energy << "tick,time,probe_ax,K_old,K_next,U_ss,U_bt,U_G0,U_g,Wprobe,damping_loss,inertial_loss,Dnl,deltaH,balance_J,balance_scale,balance_scaled\n";
    auto boundaries = Open(directory / "root-reactions.csv");
    boundaries << "tick,fx,fy,fz,tau_body_x,tau_body_y,tau_body_z\n";
    StateCsv(states, "accepted", 0, context, context.old, context.velocity);
    const auto started = std::chrono::steady_clock::now();
    int tick = 1;
    Vector failureUnknown = Vector::Zero(15);
    std::vector<State> trajectory;
    std::vector<double> speeds;
    if (control)
    {
        trajectory.reserve(planned + 1);
        speeds.reserve(planned + 1);
        trajectory.push_back(context.old);
        speeds.push_back(0.0);
    }
    double arcLength = 0.0;
    for (double length : context.model.lengths) arcLength += length;
    try
    {
        for (; tick <= planned; ++tick)
        {
            if (control)
                context.probe = tick >= 3001 + 100 * guide && tick <= 3500 + 100 * guide ?
                    Vec(20, 0, 0) : Vec::Zero();
            Vector unknown = Vector::Zero(15);
            for (int node = 1; node < 4; ++node)
                unknown.segment<3>(3 * (node - 1)) = context.old.positions[node] / context.length;
            failureUnknown = unknown;
            Value last = At(context, unknown);
            const double initial = last.phi;
            int iteration = 0;
            for (; iteration < 40; ++iteration)
            {
                last = At(context, unknown);
                if (Converged(last)) break;
                const Matrix raw = Hessian(context, unknown, 1e-5);
                const Matrix symmetric = .5 * (raw + raw.transpose());
                Eigen::SelfAdjointEigenSolver<Matrix> eig(symmetric);
                Require(eig.info() == Eigen::Success && eig.eigenvalues().allFinite());
                const double shift = std::max(0.0, 1e-6 - eig.eigenvalues()[0]);
                const Matrix shifted = symmetric + shift * Matrix::Identity(15, 15);
                Eigen::FullPivLU<Matrix> solve(shifted);
                Require(solve.isInvertible());
                const Vector direction = solve.solve(-last.gradient);
                Require(direction.allFinite());
                const Entry entry = NearRoot(context, unknown, last, direction, raw, symmetric, eig.eigenvalues(), shift);
                for (int coordinate = 0; coordinate < 15; ++coordinate)
                    directions << tick << ',' << iteration << ',' << coordinate << ',' << unknown[coordinate]
                               << ',' << last.gradient[coordinate] << ',' << direction[coordinate] << ',' << shift << '\n';
                bool accepted = false;
                double alpha = 1.0;
                for (int backtrack = 0; backtrack < 25; ++backtrack, alpha *= .5)
                {
                    try
                    {
                        const Vector trial = unknown + alpha * direction;
                        const auto candidate = At(context, trial);
                        const double stable = StableDelta(context, unknown, trial, last, candidate);
                        accepted = entry.allowed ? (candidate.merit <= .5 * last.merit || Converged(candidate)) :
                            stable <= 1e-4 * alpha * last.gradient.dot(direction);
                        trials << tick << ',' << iteration << ',' << backtrack << ',' << alpha << ',' << stable
                               << ',' << 1e-4 * alpha * last.gradient.dot(direction) << ','
                               << (entry.allowed ? "nearroot_merit" : "energy_Armijo") << ',' << entry.rho
                               << ',' << last.merit << ',' << candidate.merit << ',' << direction.norm()
                               << ',' << entry.minimum << ',' << entry.asymmetry << ',' << entry.difference
                               << ',' << entry.directionResidual << ',' << candidate.gradient.lpNorm<Eigen::Infinity>()
                               << ',' << candidate.forceResidual << ',' << candidate.torqueResidual << ',' << accepted << ",1\n";
                        if (accepted) break;
                    }
                    catch (const std::runtime_error&)
                    {
                        trials << tick << ',' << iteration << ',' << backtrack << ',' << alpha
                               << ",NA,NA,domain_reject,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,0,0\n";
                    }
                }
                trials.flush();
                directions.flush();
                if (!accepted) throw std::runtime_error("SOLVER_FAILURE: ARMIJO_EXHAUSTED");
                unknown += alpha * direction;
                failureUnknown = unknown;
            }
            last = At(context, unknown);
            StateCsv(states, "candidate_old_velocity", tick, context, last.state, context.velocity);
            if (!Converged(last)) throw std::runtime_error("SOLVER_FAILURE: ITERATION_LIMIT40");
            const Matrix first = Hessian(context, unknown, 1e-5);
            const Matrix second = Hessian(context, unknown, 5e-6);
            if (!control || RawCheckpoint(tick))
            {
                MatrixCsv(matrices, tick, "accepted_H1", first);
                MatrixCsv(matrices, tick, "accepted_H2", second);
            }
            const Matrix symmetric = .5 * (first + first.transpose());
            Eigen::SelfAdjointEigenSolver<Matrix> eig(symmetric);
            Eigen::SelfAdjointEigenSolver<Matrix> qq(symmetric.bottomRightCorner(6, 6));
            Require(eig.info() == Eigen::Success && qq.info() == Eigen::Success &&
                    eig.eigenvalues().allFinite() && qq.eigenvalues().allFinite());
            const double hscale = std::max(1.0, eig.eigenvalues().cwiseAbs().maxCoeff());
            const double asym = Norm(first - first.transpose()) / hscale;
            const double difference = Norm(first - second) / hscale;
            for (int index = 0; index < 15; ++index)
                spectra << tick << ',' << index << ',' << eig.eigenvalues()[index] << '\n';
            std::vector<Vec> nextVelocity(4, Vec::Zero());
            double oldK = 0.0, nextK = 0.0, damping = 0.0, inertial = 0.0, forceWork = 0.0, probeWork = 0.0;
            const double oldEnergy = Evaluate(context.model, context.old, context.model.authored.positions, Vec(0, -9.8, 0)).Energy();
            for (int node = 1; node < 4; ++node)
            {
                const Vec delta = last.state.positions[node] - context.old.positions[node];
                nextVelocity[node] = delta / step;
                oldK += .5 * .001 * context.velocity[node].squaredNorm();
                nextK += .5 * .001 * nextVelocity[node].squaredNorm();
                damping += step * Damping * nextVelocity[node].squaredNorm();
                inertial += .5 * .001 * (nextVelocity[node] - context.velocity[node]).squaredNorm();
                forceWork += last.energy.force[node].dot(delta);
                probeWork += .001 * context.probe.dot(delta);
            }
            const double remainder = last.energy.Energy() - oldEnergy + forceWork;
            const double deltaH = nextK + last.energy.Energy() - oldK - oldEnergy;
            const double balance = deltaH - probeWork + damping + inertial - remainder;
            const double balanceScale = context.energyScale + std::abs(nextK + last.energy.Energy()) +
                std::abs(oldK + oldEnergy) + std::abs(probeWork) + damping + inertial + std::abs(remainder);
            double qnorm = 0.0;
            for (const auto& frame : last.state.frames) qnorm = std::max(qnorm, std::abs(frame.norm() - 1.0));
            const double rootError = (last.state.positions[0] - context.model.authored.positions[0]).norm();
            const double qrootError = (last.state.frames[0].coeffs() - context.model.authored.frames[0].coeffs()).norm();
            const bool pass = asym <= 1e-6 && difference <= 1e-6 &&
                eig.eigenvalues()[0] / hscale > 1e-6 && std::abs(balance) / balanceScale <= 1e-10 &&
                last.phi <= initial + 1e-12 && rootError == 0.0 && qrootError == 0.0 && qnorm <= 1e-12;
            reports << tick << ',' << tick * step << ',' << (pass ? "PASS" : "FAIL") << ',' << iteration
                    << ',' << last.gradient.lpNorm<Eigen::Infinity>() << ',' << last.forceResidual << ',' << last.torqueResidual
                    << ',' << eig.eigenvalues()[0] << ',' << hscale << ',' << asym << ',' << difference
                    << ',' << qq.eigenvalues()[0] << ',' << initial << ',' << last.phi << ',' << balance
                    << ',' << std::abs(balance) / balanceScale << ',' << rootError << ',' << qrootError << ',' << qnorm << '\n';
            reports.flush();
            energy << tick << ',' << tick * step << ',' << context.probe.x() << ',' << oldK << ',' << nextK
                   << ',' << last.energy.shearStretch << ',' << last.energy.bendTwist << ',' << last.energy.ghost
                   << ',' << last.energy.gravity << ',' << probeWork << ',' << damping << ',' << inertial
                   << ',' << remainder << ',' << deltaH << ',' << balance << ',' << balanceScale
                   << ',' << std::abs(balance) / balanceScale << '\n';
            for (int coordinate = 0; coordinate < 15; ++coordinate)
            {
                const bool translation = coordinate < 9;
                const double residual = translation ? last.gradient[coordinate] * context.energyScale / context.length :
                    last.energy.bodyTorque[1 + (coordinate - 9) / 3][(coordinate - 9) % 3];
                residuals << tick << ',' << coordinate << ',' << (translation ? "force" : "body_torque")
                          << ',' << last.gradient[coordinate] << ',' << residual
                          << ',' << (translation ? context.forceScale : context.torqueScale) << '\n';
            }
            boundaries << tick;
            for (int axis = 0; axis < 3; ++axis) boundaries << ',' << -last.energy.force[0][axis];
            for (int axis = 0; axis < 3; ++axis) boundaries << ',' << -last.energy.bodyTorque[0][axis];
            boundaries << '\n';
            if (!pass) throw std::runtime_error("NUMERIC_STEP_GATE_FAILURE");
            context.old = last.state;
            context.velocity = nextVelocity;
            StateCsv(states, "accepted", tick, context, context.old, context.velocity);
            if (control)
            {
                trajectory.push_back(context.old);
                double speed = 0.0;
                for (const auto& velocity : context.velocity) speed = std::max(speed, velocity.norm() / arcLength);
                speeds.push_back(speed);
                if (tick == 3000 || tick == 3500 + 100 * guide || tick % 1000 == 0 || tick == planned)
                    std::cout << "C0 control guide=" << guide << " accepted=" << tick
                              << " probe_ax=" << context.probe.x() << std::endl;
            }
        }
    }
    catch (const std::exception& error)
    {
        auto failure = Open(directory / "failure.txt");
        failure << "reason=" << error.what() << "\ntick=" << tick << "\naccepted_steps=" << tick - 1 << '\n';
        const auto failed = At(context, failureUnknown);
        failure << "gradient_inf=" << failed.gradient.lpNorm<Eigen::Infinity>()
                << "\nforce_scaled=" << failed.forceResidual << "\ntorque_scaled=" << failed.torqueResidual << '\n';
        StateCsv(states, "failed_trial_old_velocity", tick, context, failed.state, context.velocity);
        MatrixCsv(matrices, tick, "failed_H1", Hessian(context, failureUnknown, 1e-5));
        MatrixCsv(matrices, tick, "failed_H2", Hessian(context, failureUnknown, 5e-6));
        auto summary = Open(directory / "summary.csv");
        summary << "item,status,value\n" << (control ? "numericalControl" : "numericalStartup")
                << ",FAIL," << tick << "\naccepted_steps,INFO," << tick - 1 << '\n';
        throw;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value\n" << (control ? "numericalControl" : "numericalStartup")
            << ",PASS," << planned << "\nwall_seconds,INFO," << seconds
            << (control ? "\nbehavior,PENDING,0\n" : "\ncontrol,NOT_RUN,0\n")
            << "independentAudit,PENDING,0\n";
    std::cout << "C0 " << (control ? "control" : "startup") << " guide=" << guide << " h=" << step << " steps=" << planned
              << " wall_seconds=" << seconds << std::endl;
    return StartupResult{context.old, seconds, context.length, std::move(trajectory), std::move(speeds)};
}

int Startup(const std::filesystem::path& directory)
{
    auto checkpoint = Open(directory / "checkpoint.csv");
    checkpoint << "guide,h,steps,wall_seconds,status\n";
    double coarseSeconds = 0.0;
    int runs = 0;
    std::vector<std::vector<StartupResult>> results(3);
    try
    {
        for (int level = 0; level < 3; ++level)
        {
            const double steps[] = {.001, .0005, .00025};
            const std::string labels[] = {"h001", "h0005", "h00025"};
            for (int guide = 0; guide < 3; ++guide)
            {
                const auto target = directory / ("guide" + std::to_string(guide) + "-" + labels[level]);
                const auto result = RunStartup(target, guide, steps[level]);
                results[guide].push_back(result);
                if (level == 0) coarseSeconds += result.seconds;
                checkpoint << guide << ',' << steps[level] << ',' << std::round(.04 / steps[level])
                           << ',' << result.seconds << ",PASS\n";
                checkpoint.flush();
                ++runs;
            }
        }
    }
    catch (...)
    {
        auto summary = Open(directory / "summary.csv");
        summary << "item,status,value\nstartup,FAIL," << runs << "\ncontrol,NOT_RUN,0\nindependentAudit,PENDING,0\n";
        throw;
    }
    auto refinement = Open(directory / "refinement.csv");
    refinement << "guide,metric,error_h_h2,error_h2_h4,order,status\n";
    bool refinementPass = true;
    for (int guide = 0; guide < 3; ++guide)
    {
        double positionErrors[2], rotationErrors[2];
        for (int level = 0; level < 2; ++level)
        {
            const auto& first = results[guide][level];
            const auto& second = results[guide][level + 1];
            double square = 0.0, rotation = 0.0;
            for (int node = 1; node < 4; ++node)
                square += (first.state.positions[node] - second.state.positions[node]).squaredNorm();
            for (int segment = 1; segment < 3; ++segment)
                rotation = std::max(rotation, LogR(first.state.frames[segment].conjugate() *
                                                   second.state.frames[segment], .1).norm());
            positionErrors[level] = std::sqrt(square) / first.length;
            rotationErrors[level] = rotation;
        }
        for (int metric = 0; metric < 2; ++metric)
        {
            const double* errors = metric == 0 ? positionErrors : rotationErrors;
            const double order = std::log2(errors[0] / errors[1]);
            const bool pass = errors[0] > errors[1] && errors[1] > 1e-9 &&
                              std::isfinite(order) && order >= .5 && order <= 1.5;
            refinementPass = refinementPass && pass;
            refinement << guide << ',' << (metric == 0 ? "position_over_Lref" : "SO3_radians")
                       << ',' << errors[0] << ',' << errors[1] << ',' << order << ',' << (pass ? "PASS" : "FAIL") << '\n';
        }
    }
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value\nstartup," << (refinementPass ? "PASS" : "FAIL") << ',' << runs
            << "\ncoarse_three_guide_w40_seconds,INFO," << coarseSeconds
            << "\ncontrol_cost_seconds_estimate,INFO," << coarseSeconds * 342.5
            << "\ncontrol,NOT_RUN,0\nindependentAudit,PENDING,0\n";
    return refinementPass ? 0 : 2;
}

double Distance(const State& state, const State& reference, double length)
{
    double maximum = 0.0;
    for (int node = 0; node < 4; ++node)
        maximum = std::max(maximum, (state.positions[node] - reference.positions[node]).norm() / length);
    return maximum;
}

struct Behavior
{
    double staticSpeed = 0.0;
    double staticDrift = 0.0;
    double staticShape = 0.0;
    double staticMaximum = 0.0;
    double dynamic = 0.0;
    double dynamicGroupOn = 0.0;
    double tail = 0.0;
    double strainMin = 0.0;
    double strainMax = 0.0;
    double recovery = 0.0;
    bool settled = false;
    bool recoverySuffix = false;
    bool pass = false;
};

Behavior Measure(const std::filesystem::path& directory, int guide, const StartupResult& result)
{
    const Context authored = MakeContext(guide, .001);
    double length = 0.0;
    for (double segment : authored.model.lengths) length += segment;
    const auto& states = result.trajectory;
    const State& snapshot = states.at(3000);
    auto metrics = Open(directory / "metrics.csv");
    metrics << "tick,time,phase,probe_ax,D_authored,D_snapshot,speed_over_L,static_drift,adjacent_signed_min,adjacent_signed_max,root_error,settled_qualified,centroid_x,centroid_y,centroid_z\n";
    auto baseline = Open(directory / "settled-snapshot.csv");
    baseline << "kind,tick,time,node,x,y,z,vx,vy,vz,qw,qx,qy,qz\n";
    std::vector<Vec> snapshotVelocity(4, Vec::Zero());
    for (int node = 1; node < 4; ++node)
        snapshotVelocity[node] = (states[3000].positions[node] - states[2999].positions[node]) / .001;
    Behavior behavior;
    for (int tick = 2000; tick <= 3000; ++tick)
    {
        behavior.staticSpeed = std::max(behavior.staticSpeed, result.speeds.at(tick));
        behavior.staticDrift = std::max(behavior.staticDrift, Distance(states[tick], states[2000], length));
    }
    behavior.settled = behavior.staticSpeed <= 1e-3 && behavior.staticDrift <= 1e-4;
    behavior.staticShape = Distance(snapshot, states[0], length);
    StateCsv(baseline, behavior.settled ? "qualified_settled" : "unqualified_3s_snapshot",
             3000, authored, snapshot, snapshotVelocity);
    const int firstProbe = 3001 + 100 * guide;
    const int off = 3500 + 100 * guide;
    int lastOutside = off - 1;
    for (int tick = 0; tick <= 13700; ++tick)
    {
        const double displacement = Distance(states[tick], snapshot, length);
        const double fromAuthored = Distance(states[tick], states[0], length);
        if (tick <= 3000) behavior.staticMaximum = std::max(behavior.staticMaximum, fromAuthored);
        if (tick >= firstProbe - 1) behavior.dynamic = std::max(behavior.dynamic, displacement);
        if (tick >= 3000) behavior.dynamicGroupOn = std::max(behavior.dynamicGroupOn, displacement);
        if (tick >= off && displacement > .005) lastOutside = tick;
        if (tick >= 12700) behavior.tail = std::max(behavior.tail, displacement);
        double strainMin = 0.0, strainMax = 0.0;
        for (int segment = 0; segment < 3; ++segment)
        {
            const double strain = (states[tick].positions[segment + 1] - states[tick].positions[segment]).norm() /
                                  authored.model.lengths[segment] - 1.0;
            strainMin = std::min(strainMin, strain);
            strainMax = std::max(strainMax, strain);
        }
        behavior.strainMin = std::min(behavior.strainMin, strainMin);
        behavior.strainMax = std::max(behavior.strainMax, strainMax);
        const std::string phase = tick < firstProbe ? "static" : (tick <= off ? "probe" : "recovery");
        Vec centroid = Vec::Zero();
        for (int node = 1; node < 4; ++node) centroid += states[tick].positions[node] / 3.0;
        metrics << tick << ',' << tick * .001 << ',' << phase << ','
                << (tick >= firstProbe && tick <= off ? 20 : 0) << ',' << fromAuthored << ',' << displacement
                << ',' << result.speeds[tick] << ','
                << (tick >= 2000 && tick <= 3000 ? Distance(states[tick], states[2000], length) : 0.0)
                << ',' << strainMin << ',' << strainMax
                << ',' << (states[tick].positions[0] - states[0].positions[0]).norm()
                << ',' << (tick >= 3000 && behavior.settled)
                << ',' << centroid.x() << ',' << centroid.y() << ',' << centroid.z() << '\n';
    }
    const int suffixStart = lastOutside + 1;
    behavior.recovery = (suffixStart - off) * .001;
    behavior.recoverySuffix = suffixStart <= 12700;
    const double stretch = std::max(-behavior.strainMin, behavior.strainMax);
    behavior.pass = behavior.settled && behavior.staticShape <= .015 &&
                    behavior.dynamic >= .02 && behavior.dynamic <= .08 &&
                    behavior.recoverySuffix && behavior.recovery <= .5 && behavior.tail <= .005 && stretch <= .01;
    auto summary = Open(directory / "behavior-summary.csv");
    summary << "item,status,value\nstaticEligibility," << (behavior.settled ? "PASS" : "STATIC_NOT_SETTLED")
            << ',' << behavior.staticSpeed << "\nstaticTailDrift,INFO," << behavior.staticDrift
            << "\nDmax," << (behavior.staticShape <= .015 ? "PASS" : "FAIL") << ',' << behavior.staticShape
            << "\nD_static_all_samples,INFO," << behavior.staticMaximum
            << "\nDdyn," << (!behavior.settled ? "INCONCLUSIVE" :
                (behavior.dynamic >= .02 && behavior.dynamic <= .08 ? "PASS" : "FAIL")) << ',' << behavior.dynamic
            << "\nDdyn_from_group_on,INFO," << behavior.dynamicGroupOn
            << "\nTrec," << (!behavior.settled ? "INCONCLUSIVE" :
                (behavior.recoverySuffix && behavior.recovery <= .5 ? "PASS" : "FAIL")) << ',' << behavior.recovery
            << "\nrecovery_suffix_at_least_1s,INFO," << behavior.recoverySuffix
            << "\nRtail," << (!behavior.settled ? "INCONCLUSIVE" : (behavior.tail <= .005 ? "PASS" : "FAIL"))
            << ',' << behavior.tail << "\nstretch," << (stretch <= .01 ? "PASS" : "FAIL") << ',' << stretch
            << "\nsignedStrainMin,INFO," << behavior.strainMin << "\nsignedStrainMax,INFO," << behavior.strainMax
            << "\nbehavior," << (behavior.pass ? "PASS" : "FAIL") << ",13700\nindependentAudit,PENDING,0\n";
    return behavior;
}

struct Section
{
    double normalized;
    int segment;
    double fraction;
};

std::vector<Section> Sections(const Model& model)
{
    double length = 0.0;
    for (double segment : model.lengths) length += segment;
    std::vector<Section> sections;
    for (double normalized : {1.0 / 3.0, 2.0 / 3.0, 1.0})
    {
        double remaining = normalized * length;
        int segment = 0;
        while (segment < 2 && remaining > model.lengths[segment])
        {
            remaining -= model.lengths[segment];
            ++segment;
        }
        sections.push_back(Section{normalized, segment, std::clamp(remaining / model.lengths[segment], 0.0, 1.0)});
    }
    return sections;
}

Vec Sample(const State& state, const Section& section)
{
    return (1.0 - section.fraction) * state.positions[section.segment] +
           section.fraction * state.positions[section.segment + 1];
}

int Nearest(const std::vector<Vec>& points, int guide)
{
    int nearest = -1;
    double minimum = std::numeric_limits<double>::infinity();
    for (int other = 0; other < 3; ++other)
    {
        if (other == guide) continue;
        const double square = (points[guide] - points[other]).squaredNorm();
        const double tie = 64.0 * std::numeric_limits<double>::epsilon() * std::max(square, minimum);
        if (nearest < 0 || square < minimum - tie)
        {
            minimum = square;
            nearest = other;
        }
    }
    return nearest;
}

void NeighborRows(std::ostream& output, const std::string& kind, int index, int tick,
                  const std::vector<Vec>& points, const std::vector<Vec>& authored)
{
    for (int guide = 0; guide < 3; ++guide)
    {
        const int fixed = Nearest(authored, guide);
        const int selected = Nearest(points, guide);
        const double baseline = (authored[guide] - authored[fixed]).norm();
        const double fixedDistance = (points[guide] - points[fixed]).norm();
        const double selectedDistance = (points[guide] - points[selected]).norm();
        const double selectedBaseline = (authored[guide] - authored[selected]).norm();
        output << kind << ',' << index << ',' << tick << ',' << tick * .001 << ',' << guide << ','
               << fixed << ',' << baseline << ',' << fixedDistance << ',' << fixedDistance / baseline << ','
               << selected << ',' << selectedBaseline << ',' << selectedDistance << ',' << selectedDistance / selectedBaseline << '\n';
    }
}

void Proxies(const std::filesystem::path& directory, const std::vector<StartupResult>& guides)
{
    const auto sections = Sections(MakeContext(0, .001).model);
    auto mapping = Open(directory / "authored-section-map.csv");
    mapping << "section,u,segment,fraction,A0,projection\n";
    for (int index = 0; index < 3; ++index)
        mapping << index << ',' << sections[index].normalized << ',' << sections[index].segment << ','
                << sections[index].fraction << ",.00156,authored_XY\n";
    auto proxy = Open(directory / "proxy-sections.csv");
    proxy << "tick,time,section,u,p0x,p0y,p0z,p1x,p1y,p1z,p2x,p2y,p2z,signed_XY_area,unsigned_XY_area,A0,area_ratio,signed_area_ratio,orientation_flip,degenerate,centroid_x,centroid_y,centroid_z,d01,d02,d12\n";
    auto neighbors = Open(directory / "proxy-neighbors.csv");
    neighbors << "kind,index,tick,time,guide,fixed_nn,authored_fixed_distance,fixed_distance,fixed_ratio,reselected_nn,authored_selected_distance,reselected_distance,reselected_ratio\n";
    for (int tick = 0; tick <= 13700; ++tick)
    {
        for (int index = 0; index < 3; ++index)
        {
            std::vector<Vec> points, authored;
            Vec centroid = Vec::Zero();
            proxy << tick << ',' << tick * .001 << ',' << index << ',' << sections[index].normalized;
            for (int guide = 0; guide < 3; ++guide)
            {
                points.push_back(Sample(guides[guide].trajectory[tick], sections[index]));
                authored.push_back(Sample(guides[guide].trajectory[0], sections[index]));
                centroid += points.back() / 3.0;
                for (int axis = 0; axis < 3; ++axis) proxy << ',' << points.back()[axis];
            }
            const double area = .5 * (points[1] - points[0]).cross(points[2] - points[0]).z();
            const bool degenerate = std::abs(area) <= 64.0 * std::numeric_limits<double>::epsilon() * .00156;
            proxy << ',' << area << ',' << std::abs(area) << ",.00156," << std::abs(area) / .00156
                  << ',' << area / .00156 << ',' << (area < 0.0) << ',' << degenerate;
            for (int axis = 0; axis < 3; ++axis) proxy << ',' << centroid[axis];
            proxy << ',' << (points[0] - points[1]).norm() << ',' << (points[0] - points[2]).norm()
                  << ',' << (points[1] - points[2]).norm() << '\n';
            NeighborRows(neighbors, "authored_arc_section", index, tick, points, authored);
        }
        for (int node = 0; node < 4; ++node)
        {
            std::vector<Vec> points, authored;
            for (int guide = 0; guide < 3; ++guide)
            {
                points.push_back(guides[guide].trajectory[tick].positions[node]);
                authored.push_back(guides[guide].trajectory[0].positions[node]);
            }
            NeighborRows(neighbors, "corresponding_node", node, tick, points, authored);
        }
    }
}

int Control(const std::filesystem::path& directory)
{
    auto manifest = Open(directory / "manifest.txt");
    manifest << "scope=C0-independent-control;C1=static_only\nh=.001;steps_per_guide=13700;guide_steps=41100\n"
             << "guide0=[3,3.5);guide1=[3.1,3.6);guide2=[3.2,3.7);acceleration=+X20\n"
             << "solver=three_independent15DOF;coupling=none;physical_state=continuous\n"
             << "schedule_uses_step_start_time;step3001_spans_3.000_to_3.001\n"
             << "proxy=fixed_authored_arc_u_1/3_2/3_1;projection=fixed_authored_XY;A0=.00156\n"
             << "NN=authored_fixed_identity_and_current_reselection;ties=stable_lowest_ID_64eps_squared_distance\n"
             << "proxy_area_NN=report_only;degeneracy=64eps*A0_numerical_flag;volume=UNSUPPORTED\n"
             << "storage=states_and_speeds_only;Hessian=step_local;sequential_independent_guide_execution\n";
    manifest.flush();
    auto schedule = Open(directory / "schedule.csv");
    schedule << "guide,offset_x,offset_y,offset_z,first_probe_step,last_probe_step,on_time,off_time,acceleration_x,steps,end_time\n";
    for (int guide = 0; guide < 3; ++guide)
    {
        const auto context = MakeContext(guide, .001);
        schedule << guide;
        for (int axis = 0; axis < 3; ++axis) schedule << ',' << context.old.positions[0][axis];
        schedule << ',' << 3001 + 100 * guide << ',' << 3500 + 100 * guide
                 << ',' << (3000 + 100 * guide) * .001 << ',' << (3500 + 100 * guide) * .001
                 << ",20,13700,13.7\n";
    }
    schedule.flush();
    std::vector<StartupResult> guides;
    std::vector<Behavior> behaviors;
    try
    {
        for (int guide = 0; guide < 3; ++guide)
        {
            const auto target = directory / ("guide" + std::to_string(guide));
            guides.push_back(RunStartup(target, guide, .001, true));
            behaviors.push_back(Measure(target, guide, guides.back()));
        }
    }
    catch (...)
    {
        auto summary = Open(directory / "summary.csv");
        summary << "item,status,value\ncontrol,FAIL," << guides.size()
                << "\ncompleted_guide_trajectories,INFO," << guides.size()
                << "\nindependentAudit,PENDING,0\ncoupled_dynamics,NOT_RUN,0\nvolume,UNSUPPORTED,0\n";
        throw;
    }
    Proxies(directory, guides);
    auto metrics = Open(directory / "guide-summary.csv");
    metrics << "guide,Dmax,D_static_all_samples,Ddyn,Ddyn_from_group_on,Trec,valid_recovery_suffix,Rtail,stretch,static_speed,static_drift,settled,behavior\n";
    int worstStatic = 0, worstDynamic = 0, minimumDynamic = 0, worstRecovery = 0, worstTail = 0, worstStretch = 0;
    bool pass = true;
    double seconds = 0.0;
    for (int guide = 0; guide < 3; ++guide)
    {
        const auto& value = behaviors[guide];
        const double stretch = std::max(-value.strainMin, value.strainMax);
        metrics << guide << ',' << value.staticShape << ',' << value.staticMaximum << ',' << value.dynamic
                << ',' << value.dynamicGroupOn << ',' << value.recovery << ',' << value.recoverySuffix << ','
                << value.tail << ',' << stretch << ',' << value.staticSpeed << ',' << value.staticDrift
                << ',' << value.settled << ',' << (value.pass ? "PASS" : "FAIL") << '\n';
        if (value.staticShape > behaviors[worstStatic].staticShape) worstStatic = guide;
        if (value.dynamic > behaviors[worstDynamic].dynamic) worstDynamic = guide;
        if (value.dynamic < behaviors[minimumDynamic].dynamic) minimumDynamic = guide;
        if (value.recovery > behaviors[worstRecovery].recovery) worstRecovery = guide;
        if (value.tail > behaviors[worstTail].tail) worstTail = guide;
        if (stretch > std::max(-behaviors[worstStretch].strainMin, behaviors[worstStretch].strainMax)) worstStretch = guide;
        pass = pass && value.pass;
        seconds += guides[guide].seconds;
    }
    int lastOutside = 3699;
    double arcLength = 0.0;
    for (double segment : MakeContext(0, .001).model.lengths) arcLength += segment;
    for (int tick = 3700; tick <= 13700; ++tick)
        for (int guide = 0; guide < 3; ++guide)
        {
            if (Distance(guides[guide].trajectory[tick], guides[guide].trajectory[3000], arcLength) > .005)
                lastOutside = tick;
        }
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value\nnumericalTrajectory,PASS,41100\nbehavior," << (pass ? "PASS" : "FAIL")
            << ",3\nsolver_wall_seconds,INFO," << seconds << "\nworst_Dmax_guide,INFO," << worstStatic
            << "\nworst_Ddyn_upper_guide,INFO," << worstDynamic << "\nworst_Trec_guide,INFO," << worstRecovery
            << "\nminimum_Ddyn_guide,INFO," << minimumDynamic
            << "\nworst_Rtail_guide,INFO," << worstTail << "\nworst_stretch_guide,INFO," << worstStretch
            << "\ngroup_recovery_from_3.7_report_only,INFO," << (lastOutside + 1 - 3700) * .001
            << "\ngroup_recovery_suffix_at_least_1s_report_only,INFO," << (lastOutside + 1 <= 12700)
            << "\nproxy,REPORT_ONLY,0\nindependentAudit,PENDING,0\ncoupled_dynamics,NOT_RUN,0\nvolume,UNSUPPORTED,0\n";
    return pass ? 0 : 3;
}
}
