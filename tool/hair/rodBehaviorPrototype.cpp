#include "rodGhostPrototypeMath.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

using namespace R1;
using Vector = Eigen::VectorXd;
using Matrix = Eigen::MatrixXd;
double Step = .001;
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
        const Vec error = delta - Step * context.velocity[node];
        phi += .5 * .001 * error.squaredNorm() / (Step * Step) +
               .5 * Damping * delta.squaredNorm() / Step;
        phi -= .001 * context.probe.dot(value.state.positions[node]);
        const Vec residual = .001 * error / (Step * Step) +
                             Damping * delta / Step - value.energy.force[node] - .001 * context.probe;
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
            const long double predictionA = deltaA - Step * context.velocity[node][axis];
            const long double predictionB = deltaB - Step * context.velocity[node][axis];
            delta += .5L * .001L / (Step * Step) * dx * (predictionB + predictionA);
            delta += .5L * Damping / Step * dx * (deltaB + deltaA);
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

void StateCsv(std::ostream& output, const std::string& kind, int tick, const State& state,
              const std::vector<Vec>& velocity)
{
    for (int node = 0; node < 4; ++node)
    {
        output << kind << ',' << tick << ',' << tick * Step << ',' << node;
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
    output.flush();
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

bool RawCheckpoint(int tick)
{
    return tick == 1 || tick % 100 == 0 || tick == 2999 || tick == 3001 ||
           tick == 3499 || tick == 3501 || tick == 13500;
}

double Distance(const State& state, const State& reference, double length)
{
    double result = 0.0;
    for (int node = 0; node < 4; ++node)
        result = std::max(result, (state.positions[node] - reference.positions[node]).norm() / length);
    return result;
}

int Startup(const std::filesystem::path& directory, bool behavior = false)
{
    State authored;
    authored.positions = {Vec(0, 0, 0), Vec(.01, 0, .1), Vec(.02, .005, .2), Vec(.025, .015, .3)};
    authored.frames = {Quat::Identity(), ExpR(Vec(.03, -.02, .04), .1), ExpR(Vec(.05, .01, .08), .1)};
    Model model = AuthoredModel(authored, Vec(.3, .4, 1), Vec(.001, .0015, .002), .1);
    model.masses.assign(4, .001);
    model.ghostStiffness = {0, Spring, Spring, Spring};
    const double length = *std::max_element(model.lengths.begin(), model.lengths.end());
    const double forceScale = 1.0 + Spring * length + .001 * (9.8 + 20.0);
    double torqueScale = length;
    double joint = 0.0;
    for (int index = 0; index < 2; ++index)
        joint = std::max(joint, .002 / (.5 * (model.lengths[index] + model.lengths[index + 1])));
    torqueScale += joint;
    const double energyScale = forceScale * length + torqueScale;
    auto manifest = Open(directory / "manifest.txt");
    manifest << "scope=" << (behavior ? "phase4-behavior-v3" : "phase4-startup-only-v3")
        << "\nplanned_steps=" << (behavior ? 13500 : std::round(.04 / Step)) << "\nh=" << Step
        << "\nend=" << (behavior ? 13.5 : .04) << '\n'
        << "N=3;root=x0,q0;x1=free\nq=quasi-static\n"
        << "S=.3,.4,1 N;B=.001,.0015,.002 Nm2;m=.001 kg\n"
        << "k=" << Spring << "\ngamma=" << Damping << "\ng=0,-9.8,0\n"
        << "ghost=authored,stationary,unshifted\nprobe=+X20_on_intervals_3001..3500_only_in_behavior\n"
        << "raw_H_indices=1,every100,2999,3000,3001,3499,3500,3501,13500;all_steps_two_H_gates_and_spectra\n"
        << "states_continuous;settled_tail=2..3;snapshot=3;recovery=3.5..13.5;tail=12.5..13.5\n"
        << "behavior_limits=Dmax.015;Ddyn.02..08;Trec.5;band.005;Rtail.005;stretch.01\n"
        << "Newton=40;Armijo=25;c1=1e-4;alpha=2^-j\n"
        << "H_eps=1e-5,5e-6;shift=max(0,1e-6-lambda_min)\n"
        << "three_residual_gates=1e-10;H_gates=1e-6;margin=.1\n"
        << "objective=unchanged_R1;globalization=v3_nearroot_merit_else_stableDelta\n"
        << "nearroot_rho=1e-8;full_direction_norm=1e-6;unshift_min_eigen>=0;merit_ratio<=.5\n"
        << "no_per_iteration_energy_monotonicity_claim;final_objective_bound_unchanged\n"
        << "precision=double_features_and_gradH;long_double_feature_subtraction_and_sum\n"
        << "long_double_digits=" << std::numeric_limits<long double>::digits << "\n"
        << "solver_origin=phase3_rodGhostPrototype_97f4fbfe;necessary_functions_only\n"
        << "Lref=" << length << "\nFref=" << forceScale << "\nTauref=" << torqueScale
        << "\nEref=" << energyScale << '\n';
    for (int node = 0; node < 4; ++node)
    {
        manifest << "x" << node << '=' << authored.positions[node].transpose() << '\n';
        if (node < 3) manifest << "q" << node << '=' << authored.frames[node].w() << ','
                             << authored.frames[node].vec().transpose() << '\n';
    }
    manifest.flush();
    auto states = Open(directory / "states.csv");
    states << "kind,tick,time,node,x,y,z,vx,vy,vz,qw,qx,qy,qz\n";
    auto iterations = Open(directory / "iterations.csv");
    iterations << "tick,iteration,phi,gradient_inf,force_scaled,torque_scaled,shift,alpha,trial_phi,"
                  "phi_decrease,directional_derivative,direction_norm,accepted,stable_delta,Armijo_rhs,mode,rescue_count\n";
    auto trials = Open(directory / "trials.csv");
    trials << "tick,iteration,backtrack,alpha,old_delta,stable_delta,Armijo_rhs,base_gradient,trial_gradient,"
              "mode,rho_base,merit_base,merit_trial,full_direction_norm,unshift_lambda_min,H_asym,H_diff,"
              "direction_residual,trial_force,trial_torque,reason,domain_valid\n";
    auto directions = Open(directory / "directions.csv");
    directions << "tick,iteration,coordinate,z,gradient,direction\n";
    auto matrices = Open(directory / "hessians.csv");
    matrices << "tick,kind,row,column,value\n";
    auto reports = Open(directory / "steps.csv");
    reports << "tick,time,status,iterations,gradient_inf,force_scaled,torque_scaled,lambda_min,SH,"
               "H_asym,H_difference,qq_min,phi_initial,phi_final,balance_J,balance_scaled\n";
    auto physical = Open(directory / "residuals.csv");
    physical << "tick,coordinate,kind,gradient_z,physical_residual,scale\n";
    auto energy = Open(directory / "energy.csv");
    energy << "tick,time,phase,probe_ax,K_old,K_next,U_ss,U_bt,U_G0,U_g,Wprobe,damping_loss,"
              "inertial_loss,Dnl,deltaH,balance_J,balance_scale,balance_scaled\n";
    auto metrics = Open(directory / "metrics.csv");
    metrics << "tick,time,phase,settled_qualified,D_authored,D_snapshot,speed_over_L,static_tail_drift,"
               "adjacent_signed_min,adjacent_signed_max,centroid_x,centroid_y,centroid_z,root_error\n";
    auto boundaries = Open(directory / "root-reactions.csv");
    boundaries << "tick,fx,fy,fz,tau_body_x,tau_body_y,tau_body_z\n";
    auto spectrum = Open(directory / "spectra.csv");
    spectrum << "tick,index,eigenvalue\n";
    State old = authored;
    std::vector<Vec> velocity(4, Vec::Zero());
    StateCsv(states, "accepted", 0, old, velocity);
    const auto started = std::chrono::steady_clock::now();
    int rescueCount = 0;
    const int planned = behavior ? 13500 : static_cast<int>(std::round(.04 / Step));
    double arcLength = 0.0;
    for (double segment : model.lengths) arcLength += segment;
    State tailStart = authored, snapshot = authored;
    double tailSpeed = 0.0, tailDrift = 0.0, staticShape = 0.0;
    double windPeak = 0.0, dynamicPeak = 0.0, recoveryTail = 0.0, endDisplacement = 0.0;
    double signedMin = 0.0, signedMax = 0.0, worstForce = 0.0, worstTorque = 0.0;
    int lastOutside = -1, worstIteration = 0;
    bool settled = false;
    for (int tick = 1; tick <= planned; ++tick)
    {
        Context context{model, old, velocity, length, forceScale, torqueScale, energyScale};
        if (behavior && tick >= 3001 && tick <= 3500) context.probe = Vec(20, 0, 0);
        Vector unknown = Vector::Zero(15);
        for (int node = 1; node < 4; ++node) unknown.segment<3>(3 * (node - 1)) = old.positions[node] / length;
        Value last = At(context, unknown);
        const double initial = last.phi;
        int iteration = 0;
        try
        {
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
                bool accepted = false;
                double alpha = 1.0, trialPhi = last.phi, stable = 0.0;
                for (int backtrack = 0; backtrack < 25; ++backtrack, alpha *= .5)
                {
                    try
                    {
                        const Vector trial = unknown + alpha * direction;
                        const auto candidate = At(context, trial);
                        trialPhi = candidate.phi;
                        stable = StableDelta(context, unknown, trial, last, candidate);
                        const bool finalResidual = Converged(candidate);
                        const bool meritShrink = candidate.merit <= .5 * last.merit;
                        const bool accept = entry.allowed ? (meritShrink || finalResidual) :
                            stable <= 1e-4 * alpha * last.gradient.dot(direction);
                        const std::string mode = entry.allowed ? "nearroot_merit" : "energy_Armijo";
                        const std::string reason = !accept ? "reject" : (entry.allowed ?
                            (meritShrink ? "merit_half" : "final_three_residuals") : "stable_Armijo");
                        trials << tick << ',' << iteration << ',' << backtrack << ',' << alpha << ','
                               << trialPhi - last.phi << ',' << stable << ','
                               << 1e-4 * alpha * last.gradient.dot(direction) << ','
                               << last.gradient.lpNorm<Eigen::Infinity>() << ','
                               << candidate.gradient.lpNorm<Eigen::Infinity>() << ','
                               << mode << ',' << entry.rho << ',' << last.merit << ',' << candidate.merit << ','
                               << direction.norm() << ',' << entry.minimum << ',' << entry.asymmetry << ','
                               << entry.difference << ',' << entry.directionResidual << ',' << candidate.forceResidual
                               << ',' << candidate.torqueResidual << ',' << reason << ",1\n";
                        if (accept)
                        {
                            accepted = true;
                            if (entry.allowed) ++rescueCount;
                            break;
                        }
                    }
                    catch (const std::runtime_error&) {}
                }
                iterations << tick << ',' << iteration << ',' << last.phi << ','
                    << last.gradient.lpNorm<Eigen::Infinity>() << ',' << last.forceResidual << ','
                    << last.torqueResidual << ',' << shift << ',' << alpha << ',' << trialPhi << ','
                    << last.phi - trialPhi << ',' << last.gradient.dot(direction) << ',' << direction.norm()
                    << ',' << accepted << ',' << stable << ',' << 1e-4 * alpha * last.gradient.dot(direction)
                    << ',' << (entry.allowed ? "nearroot_merit" : "energy_Armijo") << ',' << rescueCount << '\n';
                for (int coordinate = 0; coordinate < 15; ++coordinate)
                    directions << tick << ',' << iteration << ',' << coordinate << ',' << unknown[coordinate]
                               << ',' << last.gradient[coordinate] << ',' << direction[coordinate] << '\n';
                iterations.flush();
                directions.flush();
                trials.flush();
                if (!accepted) throw std::runtime_error("SOLVER_FAILURE: ARMIJO_EXHAUSTED");
                unknown += alpha * direction;
            }
            last = At(context, unknown);
            if (!Converged(last)) throw std::runtime_error("SOLVER_FAILURE: ITERATION_LIMIT40");
            const Matrix first = Hessian(context, unknown, 1e-5);
            const Matrix second = Hessian(context, unknown, 5e-6);
            if (!behavior || RawCheckpoint(tick))
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
                spectrum << tick << ',' << index << ',' << eig.eigenvalues()[index] << '\n';
            std::vector<Vec> nextVelocity(4, Vec::Zero());
            double oldK = 0.0, nextK = 0.0, damping = 0.0, inertial = 0.0, forceWork = 0.0, probeWork = 0.0;
            const double oldEnergy = Evaluate(model, old, authored.positions, Vec(0, -9.8, 0)).Energy();
            for (int node = 1; node < 4; ++node)
            {
                const Vec delta = last.state.positions[node] - old.positions[node];
                nextVelocity[node] = delta / Step;
                oldK += .5 * .001 * velocity[node].squaredNorm();
                nextK += .5 * .001 * nextVelocity[node].squaredNorm();
                damping += Step * Damping * nextVelocity[node].squaredNorm();
                inertial += .5 * .001 * (nextVelocity[node] - velocity[node]).squaredNorm();
                forceWork += last.energy.force[node].dot(delta);
                probeWork += .001 * context.probe.dot(delta);
            }
            const double remainder = last.energy.Energy() - oldEnergy + forceWork;
            const double balance = nextK + last.energy.Energy() - oldK - oldEnergy - probeWork + damping + inertial - remainder;
            const double balanceScale = energyScale + std::abs(nextK + last.energy.Energy()) +
                std::abs(oldK + oldEnergy) + std::abs(probeWork) + damping + inertial + std::abs(remainder);
            const bool pass = asym <= 1e-6 && difference <= 1e-6 &&
                eig.eigenvalues()[0] / hscale > 1e-6 && std::abs(balance) / balanceScale <= 1e-10 &&
                last.phi <= initial + 1e-12;
            reports << tick << ',' << tick * Step << ',' << (pass ? "PASS" : "FAIL") << ',' << iteration << ','
                << last.gradient.lpNorm<Eigen::Infinity>() << ',' << last.forceResidual << ',' << last.torqueResidual
                << ',' << eig.eigenvalues()[0] << ',' << hscale << ',' << asym << ',' << difference << ','
                << qq.eigenvalues()[0] << ',' << initial << ',' << last.phi << ',' << balance << ','
                << std::abs(balance) / balanceScale << '\n';
            reports.flush();
            const std::string phase = !behavior ? "startup" : (tick <= 3000 ? "static" : (tick <= 3500 ? "probe" : "recovery"));
            energy << tick << ',' << tick * Step << ',' << phase << ',' << context.probe.x() << ','
                   << oldK << ',' << nextK << ',' << last.energy.shearStretch << ',' << last.energy.bendTwist << ','
                   << last.energy.ghost << ',' << last.energy.gravity << ',' << probeWork << ',' << damping << ','
                   << inertial << ',' << remainder << ',' << nextK + last.energy.Energy() - oldK - oldEnergy
                   << ',' << balance << ',' << balanceScale << ',' << std::abs(balance) / balanceScale << '\n';
            for (int coordinate = 0; coordinate < 15; ++coordinate)
            {
                const bool translation = coordinate < 9;
                const double residual = translation ? last.gradient[coordinate] * energyScale / length :
                    last.energy.bodyTorque[1 + (coordinate - 9) / 3][(coordinate - 9) % 3];
                physical << tick << ',' << coordinate << ',' << (translation ? "force" : "body_torque") << ','
                         << last.gradient[coordinate] << ',' << residual << ',' << (translation ? forceScale : torqueScale) << '\n';
            }
            boundaries << tick;
            for (int axis = 0; axis < 3; ++axis) boundaries << ',' << -last.energy.force[0][axis];
            for (int axis = 0; axis < 3; ++axis) boundaries << ',' << -last.energy.bodyTorque[0][axis];
            boundaries << '\n';
            if (!pass) throw std::runtime_error("NUMERIC_STEP_GATE_FAILURE");
            old = last.state;
            velocity = nextVelocity;
            StateCsv(states, "accepted", tick, old, velocity);
            worstForce = std::max(worstForce, last.forceResidual);
            worstTorque = std::max(worstTorque, last.torqueResidual);
            worstIteration = std::max(worstIteration, iteration);
            if (behavior)
            {
                if (tick == 2000) tailStart = old;
                double speed = 0.0;
                for (const auto& value : velocity) speed = std::max(speed, value.norm() / arcLength);
                if (tick >= 2000 && tick <= 3000)
                {
                    tailSpeed = std::max(tailSpeed, speed);
                    tailDrift = std::max(tailDrift, Distance(old, tailStart, arcLength));
                }
                if (tick == 3000)
                {
                    snapshot = old;
                    settled = tailSpeed <= 1e-3 && tailDrift <= 1e-4;
                    staticShape = Distance(old, authored, arcLength);
                    auto baseline = Open(directory / "settled-snapshot.csv");
                    baseline << "kind,tick,time,node,x,y,z,vx,vy,vz,qw,qx,qy,qz\n";
                    StateCsv(baseline, settled ? "qualified_settled" : "unqualified_3s_snapshot", tick, old, velocity);
                    std::cout << std::setprecision(17) << "STATIC_GATE t=3 numerical=PASS settled="
                              << settled << " Dmax=" << staticShape << " tail_speed_over_L=" << tailSpeed
                              << " tail_drift_over_L=" << tailDrift << " wall_seconds="
                              << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
                              << " continuity=preserved next_probe=preauthorized_if_numerical_PASS" << std::endl;
                }
                const double displacement = Distance(old, snapshot, arcLength);
                if (tick >= 3000 && tick <= 3500) windPeak = std::max(windPeak, displacement);
                if (tick >= 3000) dynamicPeak = std::max(dynamicPeak, displacement);
                if (tick >= 3500 && displacement > .005) lastOutside = tick - 3500;
                if (tick >= 12500) recoveryTail = std::max(recoveryTail, displacement);
                endDisplacement = displacement;
                double currentMin = 0.0, currentMax = 0.0;
                for (int segment = 0; segment < 3; ++segment)
                {
                    const double strain = (old.positions[segment + 1] - old.positions[segment]).norm() / model.lengths[segment] - 1;
                    currentMin = std::min(currentMin, strain);
                    currentMax = std::max(currentMax, strain);
                }
                signedMin = std::min(signedMin, currentMin);
                signedMax = std::max(signedMax, currentMax);
                Vec centroid = Vec::Zero();
                for (int node = 1; node < 4; ++node) centroid += (old.positions[node] - snapshot.positions[node]) / (3 * arcLength);
                metrics << tick << ',' << tick * Step << ',' << phase << ',' << (tick >= 3000 && settled)
                        << ',' << Distance(old, authored, arcLength) << ',' << displacement << ',' << speed << ','
                        << (tick >= 2000 && tick <= 3000 ? Distance(old, tailStart, arcLength) : 0)
                        << ',' << currentMin << ',' << currentMax << ',' << centroid.x() << ',' << centroid.y()
                        << ',' << centroid.z() << ',' << (old.positions[0] - authored.positions[0]).norm() << '\n';
            }
            const bool checkpoint = behavior ? (tick == 40 || tick == 1000 || tick == 2000 || tick == 3000 ||
                tick == 3500 || tick == 4000 || tick == 5000 || tick == 8000 || tick == 13500) :
                (tick == planned || tick % 20 == 0);
            if (checkpoint)
            {
                energy.flush(); physical.flush(); metrics.flush(); boundaries.flush();
                std::cout << "h=" << Step << " accepted tick=" << tick << " time=" << tick * Step << '\n';
                std::cout << "worst_force=" << worstForce << " worst_torque=" << worstTorque
                          << " worst_iterations=" << worstIteration << '\n';
            }
        }
        catch (const std::exception& error)
        {
            StateCsv(states, "failed_trial_old_velocity", tick, last.state, velocity);
            MatrixCsv(matrices, tick, "failed_H1", Hessian(context, unknown, 1e-5));
            MatrixCsv(matrices, tick, "failed_H2", Hessian(context, unknown, 5e-6));
            auto failure = Open(directory / "failure.txt");
            failure << "reason=" << error.what() << "\ntick=" << tick << "\ntime=" << tick * Step
                    << "\niteration=" << iteration << "\ngradient_inf=" << last.gradient.lpNorm<Eigen::Infinity>()
                    << "\nforce_scaled=" << last.forceResidual << "\ntorque_scaled=" << last.torqueResidual
                    << "\nphi=" << last.phi << "\nwall_seconds="
                    << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << '\n';
            auto summary = Open(directory / "summary.csv");
            summary << "item,status,value\nstartup,FAIL," << tick << "\naccepted_steps,INFO," << tick - 1
                    << "\nfine_steps,NOT_RUN,0\nbehavior,NOT_RUN,0\nlongrun,NOT_RUN,0\n";
            throw;
        }
    }
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value\nnumericalTrajectory,PASS," << planned
            << "\nnearroot_acceptances,INFO," << rescueCount
            << "\nwall_seconds,INFO," << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
            << "\nmax_force_scaled,INFO," << worstForce << "\nmax_torque_scaled,INFO," << worstTorque
            << "\nmax_iterations,INFO," << worstIteration << '\n';
    if (behavior)
    {
        const int start = lastOutside + 1;
        const bool continuous = start <= 9000;
        const double recoveryTime = start * Step;
        const double stretch = std::max(-signedMin, signedMax);
        const bool passed = settled && staticShape <= .015 && dynamicPeak >= .02 && dynamicPeak <= .08 &&
                            continuous && recoveryTime <= .5 && recoveryTail <= .005 && stretch <= .01;
        summary << "staticEligibility," << (settled ? "PASS" : "FAIL") << ',' << tailSpeed
                << "\nstaticTailDrift,INFO," << tailDrift << "\nDmax," << (staticShape <= .015 ? "PASS" : "FAIL") << ',' << staticShape
                << "\nDwind," << (settled ? "INFO" : "INCONCLUSIVE") << ',' << windPeak
                << "\nDdyn," << (!settled ? "INCONCLUSIVE" : (dynamicPeak >= .02 && dynamicPeak <= .08 ? "PASS" : "FAIL")) << ',' << dynamicPeak
                << "\nTrec," << (!settled ? "INCONCLUSIVE" : (continuous && recoveryTime <= .5 ? "PASS" : "FAIL")) << ','
                << (continuous ? std::to_string(recoveryTime) : "NA")
                << "\nRtail," << (!settled ? "INCONCLUSIVE" : (recoveryTail <= .005 ? "PASS" : "FAIL")) << ',' << recoveryTail
                << "\nRend,INFO," << endDisplacement << "\nstretch," << (stretch <= .01 ? "PASS" : "FAIL") << ',' << stretch
                << "\nsignedStrainMin,INFO," << signedMin << "\nsignedStrainMax,INFO," << signedMax
                << "\nbehavior," << (passed ? "PASS" : "FAIL") << ",13500\nclump,NOT_RUN,0\nvolume,UNSUPPORTED,0\n"
                << "rotationalInertia,UNSUPPORTED,0\n";
        return passed ? 0 : 3;
    }
    summary << "behavior,NOT_RUN,0\nlongrun,NOT_RUN,0\n";
    return 0;
}

std::vector<std::string> Fields(const std::string& line)
{
    std::vector<std::string> fields;
    std::istringstream input(line);
    std::string field;
    while (std::getline(input, field, ',')) fields.push_back(field);
    return fields;
}

int Replay(const std::filesystem::path& directory)
{
    Step = .00025;
    State authored;
    authored.positions = {Vec(0, 0, 0), Vec(.01, 0, .1), Vec(.02, .005, .2), Vec(.025, .015, .3)};
    authored.frames = {Quat::Identity(), ExpR(Vec(.03, -.02, .04), .1), ExpR(Vec(.05, .01, .08), .1)};
    Model model = AuthoredModel(authored, Vec(.3, .4, 1), Vec(.001, .0015, .002), .1);
    model.masses.assign(4, .001);
    model.ghostStiffness = {0, Spring, Spring, Spring};
    const double length = *std::max_element(model.lengths.begin(), model.lengths.end());
    const double force = 1 + Spring * length + .001 * 29.8;
    double joint = 0.0;
    for (int index = 0; index < 2; ++index)
        joint = std::max(joint, .002 / (.5 * (model.lengths[index] + model.lengths[index + 1])));
    const double torque = length + joint;
    Context context{model, authored, std::vector<Vec>(4, Vec::Zero()), length, force, torque, force * length + torque};
    const std::filesystem::path source("artifacts/phase4-r1b-execution-001/startup-v2/h00025");
    std::ifstream states(source / "states.csv");
    std::ifstream directions(source / "directions.csv");
    if (!states || !directions) throw std::runtime_error("missing frozen v2 replay inputs");
    std::string line;
    int foundNodes = 0, foundCoordinates = 0;
    while (std::getline(states, line))
    {
        const auto fields = Fields(line);
        if (fields.size() != 14 || fields[0] != "accepted" || fields[1] != "56") continue;
        const int node = std::stoi(fields[3]);
        for (int axis = 0; axis < 3; ++axis)
        {
            context.old.positions[node][axis] = std::stod(fields[4 + axis]);
            context.velocity[node][axis] = std::stod(fields[7 + axis]);
        }
        if (node < 3)
            context.old.frames[node] = Quat(std::stod(fields[10]), std::stod(fields[11]),
                                            std::stod(fields[12]), std::stod(fields[13]));
        ++foundNodes;
    }
    Vector unknown = Vector::Zero(15);
    while (std::getline(directions, line))
    {
        const auto fields = Fields(line);
        if (fields.size() != 6 || fields[0] != "57" || fields[1] != "2") continue;
        unknown[std::stoi(fields[2])] = std::stod(fields[3]);
        ++foundCoordinates;
    }
    Require(foundNodes == 4 && foundCoordinates == 15);
    const auto base = At(context, unknown);
    const Matrix raw = Hessian(context, unknown, 1e-5);
    const Matrix symmetric = .5 * (raw + raw.transpose());
    Eigen::SelfAdjointEigenSolver<Matrix> eig(symmetric);
    Require(eig.info() == Eigen::Success && eig.eigenvalues().allFinite());
    const double shift = std::max(0.0, 1e-6 - eig.eigenvalues()[0]);
    const Vector direction = (symmetric + shift * Matrix::Identity(15, 15)).fullPivLu().solve(-base.gradient);
    const Entry entry = NearRoot(context, unknown, base, direction, raw, symmetric, eig.eigenvalues(), shift);
    const Vector trial = unknown + direction;
    const auto candidate = At(context, trial);
    const Matrix first = Hessian(context, trial, 1e-5);
    const Matrix second = Hessian(context, trial, 5e-6);
    const Matrix finalSymmetric = .5 * (first + first.transpose());
    Eigen::SelfAdjointEigenSolver<Matrix> finalEigen(finalSymmetric);
    Eigen::SelfAdjointEigenSolver<Matrix> qq(finalSymmetric.bottomRightCorner(6, 6));
    Require(finalEigen.info() == Eigen::Success && qq.info() == Eigen::Success &&
            finalEigen.eigenvalues().allFinite() && qq.eigenvalues().allFinite());
    const double hscale = std::max(1.0, finalEigen.eigenvalues().cwiseAbs().maxCoeff());
    const double asymmetry = Norm(first - first.transpose()) / hscale;
    const double difference = Norm(first - second) / hscale;
    const bool pass = entry.allowed && (candidate.merit <= .5 * base.merit || Converged(candidate)) &&
        Converged(candidate) && asymmetry <= 1e-6 && difference <= 1e-6 &&
        finalEigen.eigenvalues()[0] / hscale > 1e-6 && candidate.phi <= base.phi + 1e-12;
    auto report = Open(directory / "replay.csv");
    report << "status,base_rho,base_merit,trial_merit,merit_ratio,entry,full_direction_norm,"
              "base_lambda,entry_H_asym,entry_H_difference,direction_residual,"
              "trial_gradient,trial_force,trial_torque,stable_delta,old_delta,final_lambda_over_SH,H_asym,H_difference\n";
    report << (pass ? "PASS" : "FAIL") << ',' << entry.rho << ',' << base.merit << ',' << candidate.merit << ','
           << candidate.merit / base.merit << ',' << entry.allowed << ',' << direction.norm() << ',' << entry.minimum
           << ',' << entry.asymmetry << ',' << entry.difference << ',' << entry.directionResidual << ','
           << candidate.gradient.lpNorm<Eigen::Infinity>() << ',' << candidate.forceResidual << ','
           << candidate.torqueResidual << ',' << StableDelta(context, unknown, trial, base, candidate) << ','
           << candidate.phi - base.phi << ',' << finalEigen.eigenvalues()[0] / hscale << ','
           << asymmetry << ',' << difference << '\n';
    auto matrices = Open(directory / "hessians.csv");
    matrices << "tick,kind,row,column,value\n";
    MatrixCsv(matrices, 57, "base_H", raw);
    MatrixCsv(matrices, 57, "trial_H1", first);
    MatrixCsv(matrices, 57, "trial_H2", second);
    auto replayState = Open(directory / "states.csv");
    replayState << "kind,tick,time,node,x,y,z,vx,vy,vz,qw,qx,qy,qz\n";
    StateCsv(replayState, "replayed_base", 57, base.state, context.velocity);
    StateCsv(replayState, "Newton_trial_old_velocity", 57, candidate.state, context.velocity);
    auto manifest = Open(directory / "manifest.txt");
    manifest << "source=v2_frozen_h00025_step56_state_step57_iteration2_z\n"
             << "algorithm=v3;alpha=1;no_physical_continuation\n"
             << "rho_entry=1e-8;direction_entry=1e-6;merit_contraction=.5;residual_gate=1e-10\n"
             << "precision=double_features_and_gradH;long_double_stableDelta_sum\n";
    std::cout << "v3 frozen failure replay " << (pass ? "PASS" : "FAIL")
              << " force=" << candidate.forceResidual << " merit_ratio=" << candidate.merit / base.merit << '\n';
    return pass ? 0 : 2;
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3 || (std::string(argv[1]) != "--startup" && std::string(argv[1]) != "--replay" &&
                          std::string(argv[1]) != "--behavior"))
            throw std::runtime_error("usage: vl_hair_rod_behavior --startup|--replay|--behavior NEW_DIRECTORY");
        if (!std::filesystem::create_directories(argv[2])) throw std::runtime_error("output directory already exists");
        if (std::string(argv[1]) == "--replay") return Replay(argv[2]);
        if (std::string(argv[1]) == "--behavior")
        {
            Step = .001;
            return Startup(argv[2], true);
        }
        for (double step : {.001, .0005, .00025})
        {
            Step = step;
            const auto tier = std::filesystem::path(argv[2]) /
                (step == .001 ? "h001" : (step == .0005 ? "h0005" : "h00025"));
            std::filesystem::create_directory(tier);
            const int status = Startup(tier);
            if (status != 0) return status;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "R1-B FAIL: " << error.what() << '\n';
        return 2;
    }
}
