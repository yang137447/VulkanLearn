#include "rodGhostPrototypeMath.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

using namespace R1;

std::ofstream Open(const std::filesystem::path& path)
{
    std::ofstream output(path);
    if (!output)
        throw std::runtime_error("OUTPUT_FAILURE");
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << std::setprecision(17);
    return output;
}

void WriteVec(std::ostream& output, const Vec& value)
{
    output << ',' << value.x() << ',' << value.y() << ',' << value.z();
}

struct Evidence
{
    std::ofstream inputs;
    std::ofstream values;
    std::ofstream checks;
    std::ofstream differences;
    std::ofstream parameters;
    int count = 0;
    int failures = 0;
    explicit Evidence(const std::filesystem::path& directory)
        : inputs(Open(directory / "inputs.csv")), values(Open(directory / "evaluations.csv")),
          checks(Open(directory / "checks.csv")), differences(Open(directory / "finite-differences.csv")),
          parameters(Open(directory / "parameters.csv"))
    {
        inputs << "case,kind,index,x,y,z,qw,qx,qy,qz,mass,k,ghost_x,ghost_y,ghost_z,ell,"
                  "nu0_x,nu0_y,nu0_z,phi0_x,phi0_y,phi0_z\n";
        values << "case,kind,index,U_ss,U_bt,U_G0,U_g,fx,fy,fz,taux,tauy,tauz\n";
        checks << "case,check,status,error,scale,scaled_error,tolerance\n";
        differences << "case,kind,index,axis,free,epsilon,delta,Uplus,Uminus,analytic,FD,error,scale,required\n";
        parameters << "case,GA1,GA2,EA,EI1,EI2,GJ,gx,gy,gz,margin,root_position,root_frame,x1_free\n";
    }
    void Check(const std::string& name, const std::string& check, double error, double scale, double tolerance)
    {
        const double relative = error / scale;
        const bool pass = std::isfinite(relative) && relative <= tolerance;
        checks << name << ',' << check << ',' << (pass ? "PASS" : "FAIL") << ','
               << error << ',' << scale << ',' << relative << ',' << tolerance << '\n';
        ++count;
        if (!pass) ++failures;
    }
    Evaluation Run(const std::string& name, const Model& model, const State& state,
                   const std::vector<Vec>& ghost, const Vec& gravity)
    {
        parameters << name;
        WriteVec(parameters, model.shearStretch);
        WriteVec(parameters, model.bendTwist);
        WriteVec(parameters, gravity);
        parameters << ',' << model.margin << ",0,0,1\n";
        parameters.flush();
        for (size_t node = 0; node < state.positions.size(); ++node)
        {
            inputs << name << ",node," << node;
            WriteVec(inputs, state.positions[node]);
            inputs << ",NA,NA,NA,NA," << model.masses[node] << ',' << model.ghostStiffness[node];
            WriteVec(inputs, ghost[node]);
            inputs << ",NA,NA,NA,NA,NA,NA,NA\n";
            inputs << name << ",authored_node," << node;
            WriteVec(inputs, model.authored.positions[node]);
            inputs << ",NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA\n";
        }
        for (size_t segment = 0; segment < state.frames.size(); ++segment)
        {
            const auto& frame = state.frames[segment];
            inputs << name << ",frame," << segment << ",NA,NA,NA," << frame.w() << ',' << frame.x()
                   << ',' << frame.y() << ',' << frame.z() << ",NA,NA,NA,NA,NA," << model.lengths[segment];
            WriteVec(inputs, model.restStrains[segment]);
            if (segment < model.restRotations.size()) WriteVec(inputs, model.restRotations[segment]);
            else inputs << ",NA,NA,NA";
            inputs << '\n';
            const auto& restFrame = model.authored.frames[segment];
            inputs << name << ",authored_frame," << segment << ",NA,NA,NA,"
                   << restFrame.w() << ',' << restFrame.x() << ',' << restFrame.y() << ',' << restFrame.z()
                   << ",NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA\n";
        }
        inputs.flush();
        const auto result = Evaluate(model, state, ghost, gravity);
        for (size_t node = 0; node < state.positions.size(); ++node)
        {
            values << name << ",node," << node << ',' << result.shearStretch << ',' << result.bendTwist
                   << ',' << result.ghost << ',' << result.gravity;
            WriteVec(values, result.force[node]);
            values << ",0,0,0\n";
        }
        for (size_t segment = 0; segment < state.frames.size(); ++segment)
        {
            values << name << ",frame," << segment << ',' << result.shearStretch << ',' << result.bendTwist
                   << ',' << result.ghost << ',' << result.gravity << ",0,0,0";
            WriteVec(values, result.bodyTorque[segment]);
            values << '\n';
        }
        values << name << ",root_constraint,0," << result.shearStretch << ',' << result.bendTwist
               << ',' << result.ghost << ',' << result.gravity;
        WriteVec(values, -result.force[0]);
        WriteVec(values, -result.bodyTorque[0]);
        values << '\n';
        return result;
    }
};

struct Scales
{
    double length;
    double force;
    double torque;
    double energy;
};

Scales Scale(const Model& model, const Vec& gravity, double appliedTorque = 0.0)
{
    double length = 0.0, lever = 0.0, bending = 0.0, spring = 0.0, weight = 0.0;
    for (size_t segment = 0; segment < model.lengths.size(); ++segment)
    {
        length = std::max(length, model.lengths[segment]);
        lever = std::max(lever, model.lengths[segment] * model.shearStretch.maxCoeff());
        if (segment + 1 < model.lengths.size())
            bending = std::max(bending, model.bendTwist.maxCoeff() /
                (0.5 * (model.lengths[segment] + model.lengths[segment + 1])));
    }
    for (size_t node = 0; node < model.masses.size(); ++node)
    {
        weight = std::max(weight, model.masses[node] * gravity.norm());
        if (node > 0) spring = std::max(spring, model.ghostStiffness[node] * length);
    }
    const double force = model.shearStretch.maxCoeff() + spring + weight;
    const double torque = lever + bending + std::abs(appliedTorque);
    return {length, force, torque, force * length + torque};
}

void FD(Evidence& evidence, const std::string& name, const Model& model, const State& state,
        const std::vector<Vec>& ghost, const Vec& gravity, const Evaluation& exact)
{
    const auto scale = Scale(model, gravity);
    for (double epsilon : {1e-4, 1e-5, 1e-6})
        for (int kind = 0; kind < 2; ++kind)
            for (size_t index = 0; index < (kind == 0 ? state.positions.size() : state.frames.size()); ++index)
                for (int axis = 0; axis < 3; ++axis)
                {
                    State plus = state, minus = state;
                    const double delta = epsilon * (kind == 0 ? scale.length : 1.0);
                    if (kind == 0)
                    {
                        plus.positions[index][axis] += delta;
                        minus.positions[index][axis] -= delta;
                    }
                    else
                    {
                        Vec increment = Vec::Zero();
                        increment[axis] = delta;
                        plus.frames[index] = Unit(state.frames[index] * ExpR(increment, model.margin));
                        minus.frames[index] = Unit(state.frames[index] * ExpR(-increment, model.margin));
                    }
                    const double positive = Evaluate(model, plus, ghost, gravity).Energy();
                    const double negative = Evaluate(model, minus, ghost, gravity).Energy();
                    const double finite = -(positive - negative) / (2.0 * delta);
                    const double analytic = kind == 0 ? exact.force[index][axis] : exact.bodyTorque[index][axis];
                    const double units = kind == 0 ? scale.force : scale.torque;
                    const double error = std::abs(finite - analytic);
                    evidence.differences << name << ',' << kind << ',' << index << ',' << axis << ','
                        << (index != 0) << ',' << epsilon << ',' << delta << ',' << positive << ',' << negative
                        << ',' << analytic << ',' << finite << ',' << error << ',' << units << ',' << (epsilon < 1e-4) << '\n';
                    if (epsilon < 1e-4)
                        evidence.Check(name, "FD-" + std::to_string(kind) + "-" + std::to_string(index) + "-" +
                            std::to_string(axis) + "-" + std::to_string(epsilon), error, units, 1e-7);
                }
}

void StaticExtras(Evidence& evidence, Model model, const State& authored, const State& current,
                  const Evaluation& nonaxis)
{
    const auto scale = Scale(model, Vec::Zero());
    Vec sumForce = Vec::Zero(), sumMoment = Vec::Zero();
    for (size_t node = 0; node < current.positions.size(); ++node)
    {
        sumForce += nonaxis.force[node];
        sumMoment += current.positions[node].cross(nonaxis.force[node]);
    }
    for (size_t segment = 0; segment < current.frames.size(); ++segment)
        sumMoment += current.frames[segment] * nonaxis.bodyTorque[segment];
    evidence.Check("internal", "force_sum", sumForce.norm(), scale.force, 1e-10);
    evidence.Check("internal", "moment_sum", sumMoment.norm(), scale.force * scale.length + scale.torque, 1e-10);
    const Quat rotation = ExpR(Vec(.3, -.2, .4), .1);
    State transformed = current;
    for (auto& point : transformed.positions) point = rotation * point + Vec(.2, -.3, .1);
    for (auto& frame : transformed.frames) frame = Unit(rotation * frame);
    for (int variant = 0; variant < 2; ++variant)
    {
        State state = variant == 0 ? transformed : current;
        if (variant == 1)
            for (size_t segment = 0; segment < state.frames.size(); segment += 2)
                state.frames[segment].coeffs() *= -1.0;
        const std::string name = variant == 0 ? "rigid_transform" : "q_sign";
        const auto value = evidence.Run(name, model, state, state.positions, Vec::Zero());
        evidence.Check(name, "energy", std::abs(value.Energy() - nonaxis.Energy()), scale.energy, 1e-10);
        for (size_t node = 0; node < state.positions.size(); ++node)
        {
            const Vec expected = variant == 0 ? Vec(rotation * nonaxis.force[node]) : nonaxis.force[node];
            evidence.Check(name, "force-" + std::to_string(node), (value.force[node] - expected).norm(), scale.force, 1e-10);
        }
        for (size_t segment = 0; segment < state.frames.size(); ++segment)
        {
            Vec expected = current.frames[segment] * nonaxis.bodyTorque[segment];
            if (variant == 0) expected = rotation * expected;
            evidence.Check(name, "world_torque-" + std::to_string(segment),
                (state.frames[segment] * value.bodyTorque[segment] - expected).norm(), scale.torque, 1e-10);
        }
    }
    for (size_t node = 0; node < model.masses.size(); ++node)
    {
        model.masses[node] = .01 * (node + 1);
        if (node > 0) model.ghostStiffness[node] = 7.0;
    }
    const Vec gravity(0, -9.8, 0);
    const auto gravityScale = Scale(model, gravity);
    auto ghost = authored.positions;
    const auto defaultValue = evidence.Run("gravity_default", model, authored, ghost, gravity);
    for (size_t node = 1; node < ghost.size(); ++node)
    {
        evidence.Check("gravity_default", "expected_mg-" + std::to_string(node),
            (defaultValue.force[node] - model.masses[node] * gravity).norm(), gravityScale.force, 1e-10);
        ghost[node] -= model.masses[node] * gravity / model.ghostStiffness[node];
    }
    const auto supported = evidence.Run("explicit_support", model, authored, ghost, gravity);
    for (size_t node = 1; node < ghost.size(); ++node)
        evidence.Check("explicit_support", "free_force-" + std::to_string(node),
                       supported.force[node].norm(), gravityScale.force, 1e-10);
    for (size_t segment = 1; segment < authored.frames.size(); ++segment)
        evidence.Check("explicit_support", "free_torque-" + std::to_string(segment),
                       supported.bodyTorque[segment].norm(), gravityScale.torque, 1e-10);
    const auto external = evidence.Run("nonaxis_external", model, current, authored.positions, gravity);
    FD(evidence, "nonaxis_external", model, current, authored.positions, gravity, external);
    const Quat quarter = ExpR(Vec(0, 0, Pi / 2), .1);
    evidence.Check("full_angle", "rotation", (quarter * Vec::UnitX() - Vec::UnitY()).norm(), 1.0, 1e-10);
    evidence.Check("full_angle", "LogR", (LogR(quarter, .1) - Vec(0, 0, Pi / 2)).norm(), 1.0, 1e-10);
    evidence.Check("full_angle", "Jr_zero", (RightJacobian(Vec::Zero()) - Mat::Identity()).norm(), 1.0, 1e-10);
    for (int variant = 0; variant < 8; ++variant)
    {
        bool rejected = false;
        try
        {
            State invalid = authored;
            if (variant == 0) invalid.frames[1].coeffs().setZero();
            if (variant == 1) invalid.frames[1].coeffs() *= 1.000001;
            if (variant == 2) invalid.frames[1].coeffs()[0] = std::numeric_limits<double>::quiet_NaN();
            if (variant == 3) invalid.positions[1] = invalid.positions[0];
            if (variant == 4) invalid.positions[1].x() = std::numeric_limits<double>::infinity();
            if (variant == 5) invalid.frames[1] = invalid.frames[0] * Quat(Eigen::AngleAxisd(Pi - .05, Vec::UnitX()));
            if (variant == 6) ExpR(Vec(Pi - .05, 0, 0), .1);
            if (variant == 7) AuthoredModel(invalid, Vec(-1, 1, 1), Vec::Ones(), .1);
            Evaluate(model, invalid, authored.positions, Vec::Zero());
        }
        catch (const std::runtime_error&) { rejected = true; }
        evidence.Check("domain", "expected_rejection-" + std::to_string(variant), rejected ? 0.0 : 1.0, 1.0, 0.0);
    }
}

int Verify(const std::filesystem::path& directory)
{
    Evidence evidence(directory);
    State authored;
    authored.positions = {Vec(0, 0, 0), Vec(.05, .02, .3), Vec(.1, -.02, .6), Vec(.08, .04, .9)};
    for (const Vec& angle : {Vec(.1, -.15, .05), Vec(.25, .05, .2), Vec(-.1, .2, .3)})
        authored.frames.push_back(ExpR(angle, .1));
    Model model = AuthoredModel(authored, Vec(3, 4, 5), Vec(.2, .3, .4), .1);
    const auto zero = evidence.Run("authored_zero", model, authored, authored.positions, Vec::Zero());
    const auto scale = Scale(model, Vec::Zero());
    evidence.Check("authored_zero", "elastic_energy", zero.Energy(), scale.energy, 1e-12);
    for (size_t node = 1; node < zero.force.size(); ++node)
        evidence.Check("authored_zero", "force-" + std::to_string(node), zero.force[node].norm(), scale.force, 1e-10);
    for (size_t segment = 1; segment < zero.bodyTorque.size(); ++segment)
        evidence.Check("authored_zero", "torque-" + std::to_string(segment), zero.bodyTorque[segment].norm(), scale.torque, 1e-10);
    State current = authored;
    current.positions[1] += Vec(.01, -.02, .005);
    current.positions[2] += Vec(-.015, .01, -.01);
    current.positions[3] += Vec(.02, .005, .015);
    for (size_t segment = 0; segment < current.frames.size(); ++segment)
        current.frames[segment] = Unit(current.frames[segment] * ExpR(Vec(.04 * (segment + 1), -.03, .05), .1));
    const auto nonaxis = evidence.Run("nonaxis", model, current, authored.positions, Vec::Zero());
    FD(evidence, "nonaxis", model, current, authored.positions, Vec::Zero(), nonaxis);
    StaticExtras(evidence, model, authored, current, nonaxis);
    State straight;
    for (int node = 0; node < 4; ++node) straight.positions.push_back(Vec(0, 0, node));
    straight.frames.assign(3, Quat::Identity());
    const auto twistModel = AuthoredModel(straight, Vec(3, 4, 5), Vec(1, 1.5, 2), .1);
    State twisted = straight;
    for (int segment = 0; segment < 3; ++segment) twisted.frames[segment] = ExpR(Vec(0, 0, .1 * segment), .1);
    const auto twist = evidence.Run("pure_twist", twistModel, twisted, straight.positions, Vec::Zero());
    evidence.Check("pure_twist", "energy", std::abs(twist.bendTwist - .02), .02, 1e-9);
    evidence.Check("pure_twist", "root_torque", (twist.bodyTorque[0] - Vec(0, 0, .2)).norm(), .2, 1e-9);
    evidence.Check("pure_twist", "tip_torque", (twist.bodyTorque[2] + Vec(0, 0, .2)).norm(), .2, 1e-9);
    for (int variant = 0; variant < 2; ++variant)
    {
        State rest = straight, loaded = straight;
        const std::vector<double> angles = variant == 0 ? std::vector<double>{0, 0, 0} :
                                                         std::vector<double>{0, .07, .16};
        for (int segment = 0; segment < 3; ++segment)
        {
            rest.frames[segment] = ExpR(Vec(0, 0, angles[segment]), .1);
            loaded.frames[segment] = ExpR(Vec(0, 0, angles[segment] + .1 * segment), .1);
        }
        const std::string name = variant == 0 ? "twist_zero_rest" : "twist_nonzero_rest";
        const auto material = AuthoredModel(rest, Vec(3, 4, 5), Vec(1, 1.5, 2), .1);
        const auto value = evidence.Run(name, material, loaded, rest.positions, Vec::Zero());
        double theta = 0.0;
        for (int joint = 0; joint < 2; ++joint)
        {
            const double increment = (LogR(loaded.frames[joint].conjugate() * loaded.frames[joint + 1], .1) -
                                      material.restRotations[joint]).z();
            theta += increment;
            evidence.Check(name, "joint_moment-" + std::to_string(joint), std::abs(2 * increment - .2), .2, 1e-9);
        }
        evidence.Check(name, "Theta_T_Leff_GJ", std::abs(theta - .2), .2, 1e-9);
        evidence.Check(name, "energy", std::abs(value.bendTwist - .02), .02, 1e-9);
        evidence.Check(name, "interior_torque", value.bodyTorque[1].norm(), .2, 1e-10);
        evidence.Check(name, "loaded_tip_torque", (value.bodyTorque[2] + Vec(0, 0, .2)).norm(), .2, 1e-10);
        evidence.Check(name, "root_torque", (value.bodyTorque[0] - Vec(0, 0, .2)).norm(), .2, 1e-9);
        Eigen::Matrix2d matrix;
        matrix << 4, -2, -2, 2;
        const Eigen::Vector2d rhs(0, .2);
        const Eigen::Vector2d solution = matrix.fullPivLu().solve(rhs);
        evidence.Check(name, "dense_solution", (solution - Eigen::Vector2d(.1, .2)).norm(),
                       Eigen::Vector2d(.1, .2).norm(), 1e-9);
        evidence.Check(name, "dense_residual", (matrix * solution - rhs).norm(),
                       matrix.norm() * solution.norm() + rhs.norm(), 1e-10);
    }
    auto summary = Open(directory / "summary.csv");
    summary << "scope,checks,failures\nR1-G-E-v1," << evidence.count << ',' << evidence.failures << '\n';
    std::cout << "R1-G-E-v1 checks=" << evidence.count << " failures=" << evidence.failures << '\n';
    return evidence.failures ? 2 : 0;
}

using Dense = Eigen::MatrixXd;
using Unknown = Eigen::VectorXd;

struct StepContext
{
    Model model;
    State old;
    std::vector<Vec> velocity;
    std::vector<Vec> ghost;
    double step;
    Scales scale;
};

struct Objective
{
    State state;
    Evaluation energy;
    Unknown gradient;
    double value;
    double forceResidual = 0.0;
    double torqueResidual = 0.0;
};

Objective At(const StepContext& context, const Unknown& unknown)
{
    Objective result;
    result.state = context.old;
    result.gradient = Unknown::Zero(15);
    for (int node = 1; node < 4; ++node)
        result.state.positions[node] = context.scale.length * unknown.segment<3>(3 * (node - 1));
    for (int segment = 1; segment < 3; ++segment)
        result.state.frames[segment] = Unit(context.old.frames[segment] *
            ExpR(unknown.segment<3>(9 + 3 * (segment - 1)), .1));
    result.energy = Evaluate(context.model, result.state, context.ghost, Vec(0, -1, 0));
    double objective = result.energy.Energy();
    for (int node = 1; node < 4; ++node)
    {
        const Vec delta = result.state.positions[node] - context.old.positions[node];
        const Vec prediction = delta - context.step * context.velocity[node];
        objective += .5 * context.model.masses[node] * prediction.squaredNorm() /
                     (context.step * context.step) + .5 * .002 * delta.squaredNorm() / context.step;
        const Vec residual = context.model.masses[node] * prediction / (context.step * context.step) +
                             .002 * delta / context.step - result.energy.force[node];
        result.gradient.segment<3>(3 * (node - 1)) = residual * context.scale.length / context.scale.energy;
        result.forceResidual = std::max(result.forceResidual, residual.norm() / context.scale.force);
    }
    for (int segment = 1; segment < 3; ++segment)
    {
        const Vec increment = unknown.segment<3>(9 + 3 * (segment - 1));
        result.gradient.segment<3>(9 + 3 * (segment - 1)) =
            -RightJacobian(increment).transpose() * result.energy.bodyTorque[segment] / context.scale.energy;
        result.torqueResidual = std::max(result.torqueResidual,
            result.energy.bodyTorque[segment].norm() / context.scale.torque);
    }
    result.value = objective / context.scale.energy;
    Require(std::isfinite(result.value) && result.gradient.allFinite());
    return result;
}

Dense Hessian(const StepContext& context, const Unknown& unknown, double epsilon)
{
    Dense result(15, 15);
    for (int coordinate = 0; coordinate < 15; ++coordinate)
    {
        Unknown plus = unknown, minus = unknown;
        plus[coordinate] += epsilon;
        minus[coordinate] -= epsilon;
        result.col(coordinate) = (At(context, plus).gradient - At(context, minus).gradient) / (2 * epsilon);
    }
    return result;
}

double Norm2(const Dense& matrix)
{
    return Eigen::JacobiSVD<Dense>(matrix).singularValues()[0];
}

State DynamicAuthored()
{
    State state;
    state.positions = {Vec(0, 0, 0), Vec(.01, 0, .1), Vec(.02, .005, .2), Vec(.025, .015, .3)};
    state.frames = {Quat::Identity(), ExpR(Vec(.03, -.02, .04), .1), ExpR(Vec(.05, .01, .08), .1)};
    return state;
}

std::vector<Vec> GhostAt(const State& authored, double time)
{
    auto ghost = authored.positions;
    for (int node = 1; node < 4; ++node) ghost[node] += time * Vec(.002, -.001, .003);
    return ghost;
}

void WriteState(std::ostream& stream, double step, int tick, double time, const State& state,
                const std::vector<Vec>& velocity)
{
    for (int node = 0; node < 4; ++node)
    {
        stream << step << ',' << tick << ',' << time << ',' << node;
        WriteVec(stream, state.positions[node]);
        WriteVec(stream, velocity[node]);
        if (node < 3)
        {
            const auto& frame = state.frames[node];
            stream << ',' << frame.w() << ',' << frame.x() << ',' << frame.y() << ',' << frame.z();
            const Mat rotation = frame.toRotationMatrix();
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column) stream << ',' << rotation(row, column);
        }
        else stream << ",NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA";
        stream << '\n';
    }
    stream.flush();
}

int Trajectory(const std::filesystem::path& directory)
{
    const State authored = DynamicAuthored();
    Model model = AuthoredModel(authored, Vec(.3, .4, 1), Vec(.001, .0015, .002), .1);
    model.masses.assign(4, .001);
    model.ghostStiffness = {0, .05, .05, .05};
    const Scales scale = Scale(model, Vec(0, -1, 0));
    auto manifest = Open(directory / "manifest.txt");
    manifest << "scope=R1-G-T-G-M-v1\nN=3\nroot=x0,q0; x1 free\nS=.3,.4,1 N\nB=.001,.0015,.002 Nm2\n"
             << "m=.001 kg each\ngamma=.002 kg/s movable\nk=.05 N/m movable\ng=0,-1,0 m/s2\n"
             << "ghost_speed=.002,-.001,.003 m/s\nT=.04 s\nh=.01,.005,.0025\n"
             << "chart=full-angle right; fixed old-q per step\nNewton=40\nArmijo=25,c1=1e-4,alpha=2^-j\n"
             << "Hessian_epsilon=1e-5,5e-6\nshift=max(0,1e-6-lambda_min);solver_only\n"
             << "three_residual_tolerances=1e-10\nsecond_order_tolerances=1e-6\nbalance_tolerance=1e-10\n"
             << "L_ref=" << scale.length << "\nF_ref=" << scale.force << "\nTau_ref=" << scale.torque
             << "\nE_ref=" << scale.energy << "\n";
    manifest.flush();
    Evidence evidence(directory);
    evidence.Run("dynamic_authored", model, authored, authored.positions, Vec(0, -1, 0));
    auto states = Open(directory / "states.csv");
    states << "h,tick,time,node,x,y,z,vx,vy,vz,qw,qx,qy,qz,R00,R01,R02,R10,R11,R12,R20,R21,R22\n";
    auto iterations = Open(directory / "iterations.csv");
    iterations << "h,tick,iteration,Phi_over_E,gradient_inf,force_scaled,torque_scaled,solver_shift,alpha\n";
    auto reports = Open(directory / "steps.csv");
    reports << "h,tick,time,status,iterations,gradient_inf,force_scaled,torque_scaled,Phi_initial,Phi_final,"
               "lambda_min,Hnorm,SH,H_asymmetry,H_difference,lambda_over_SH,qq_lambda_min,"
               "K_old,K_next,Uss,Ubt,UG0,Ug,Wghost,damping_loss,inertial_loss,D_U,D_nl,"
               "deltaH,balance_absolute,balance_scale,balance_scaled,balance_raw_ratio\n";
    auto spectra = Open(directory / "hessians.csv");
    spectra << "h,tick,row,column,H1,H2\n";
    auto eigenvalues = Open(directory / "eigenvalues.csv");
    eigenvalues << "h,tick,index,eigenvalue\n";
    std::vector<State> endings;
    for (double step : {.01, .005, .0025})
    {
        State old = authored;
        std::vector<Vec> velocity(4, Vec::Zero());
        WriteState(states, step, 0, 0, old, velocity);
        const int ticks = static_cast<int>(std::round(.04 / step));
        for (int tick = 1; tick <= ticks; ++tick)
        {
            const double time = tick * step;
            StepContext context{model, old, velocity, GhostAt(authored, time), step, scale};
            Unknown unknown = Unknown::Zero(15);
            for (int node = 1; node < 4; ++node)
                unknown.segment<3>(3 * (node - 1)) = old.positions[node] / scale.length;
            const double initial = At(context, unknown).value;
            int iteration = 0;
            try
            {
                for (; iteration < 40; ++iteration)
                {
                    const auto value = At(context, unknown);
                    const double gradient = value.gradient.lpNorm<Eigen::Infinity>();
                    if (gradient <= 1e-10 && value.forceResidual <= 1e-10 && value.torqueResidual <= 1e-10)
                        break;
                    const Dense raw = Hessian(context, unknown, 1e-5);
                    const Dense symmetric = .5 * (raw + raw.transpose());
                    Eigen::SelfAdjointEigenSolver<Dense> decomposition(symmetric);
                    Require(decomposition.info() == Eigen::Success);
                    const double shift = std::max(0.0, 1e-6 - decomposition.eigenvalues()[0]);
                    const Unknown direction = (symmetric + shift * Dense::Identity(15, 15)).fullPivLu().solve(-value.gradient);
                    Require(direction.allFinite());
                    bool accepted = false;
                    double alpha = 1.0;
                    for (int backtrack = 0; backtrack < 25; ++backtrack, alpha *= .5)
                    {
                        const Unknown trial = unknown + alpha * direction;
                        try
                        {
                            if (At(context, trial).value <= value.value + 1e-4 * alpha * value.gradient.dot(direction))
                            {
                                unknown = trial;
                                accepted = true;
                                break;
                            }
                        }
                        catch (const std::runtime_error&) {}
                    }
                    iterations << step << ',' << tick << ',' << iteration << ',' << value.value << ','
                        << gradient << ',' << value.forceResidual << ',' << value.torqueResidual << ','
                        << shift << ',' << (accepted ? alpha : 0) << '\n';
                    iterations.flush();
                    if (!accepted) throw std::runtime_error("SOLVER_FAILURE: Armijo exhausted");
                }
                if (iteration == 40) throw std::runtime_error("SOLVER_FAILURE: iteration limit");
                const auto value = At(context, unknown);
                const Dense first = Hessian(context, unknown, 1e-5);
                const Dense second = Hessian(context, unknown, 5e-6);
                const Dense symmetric = .5 * (first + first.transpose());
                Eigen::SelfAdjointEigenSolver<Dense> eig(symmetric);
                const double norm = eig.eigenvalues().cwiseAbs().maxCoeff();
                const double hscale = std::max(1.0, norm);
                const double asymmetry = Norm2(first - first.transpose()) / hscale;
                const double difference = Norm2(first - second) / hscale;
                const double minimum = eig.eigenvalues()[0];
                const double qqMinimum = Eigen::SelfAdjointEigenSolver<Dense>(symmetric.bottomRightCorner(6, 6)).eigenvalues()[0];
                for (int row = 0; row < 15; ++row)
                {
                    eigenvalues << step << ',' << tick << ',' << row << ',' << eig.eigenvalues()[row] << '\n';
                    for (int column = 0; column < 15; ++column)
                        spectra << step << ',' << tick << ',' << row << ',' << column << ','
                                << first(row, column) << ',' << second(row, column) << '\n';
                }
                std::vector<Vec> nextVelocity(4, Vec::Zero());
                double oldK = 0.0, nextK = 0.0, damping = 0.0, inertial = 0.0, ghostWork = 0.0, forceWork = 0.0;
                const auto oldGhost = GhostAt(authored, time - step);
                const double oldEnergy = Evaluate(model, old, oldGhost, Vec(0, -1, 0)).Energy();
                for (int node = 1; node < 4; ++node)
                {
                    const Vec delta = value.state.positions[node] - old.positions[node];
                    nextVelocity[node] = delta / step;
                    oldK += .5 * .001 * velocity[node].squaredNorm();
                    nextK += .5 * .001 * nextVelocity[node].squaredNorm();
                    damping += step * .002 * nextVelocity[node].squaredNorm();
                    inertial += .5 * .001 * (nextVelocity[node] - velocity[node]).squaredNorm();
                    forceWork += value.energy.force[node].dot(delta);
                    ghostWork += (.05 * (context.ghost[node] - value.state.positions[node])).dot(context.ghost[node] - oldGhost[node]);
                }
                const double remainder = value.energy.Energy() - oldEnergy + forceWork;
                const double nonlinear = remainder - ghostWork;
                const double deltaH = nextK + value.energy.Energy() - oldK - oldEnergy;
                const double balance = deltaH - ghostWork + damping + inertial - nonlinear;
                const double rawScale = std::abs(nextK + value.energy.Energy()) + std::abs(oldK + oldEnergy) +
                    std::abs(ghostWork) + damping + inertial + std::abs(nonlinear);
                const double balanceScale = scale.energy + rawScale;
                const bool pass = value.gradient.lpNorm<Eigen::Infinity>() <= 1e-10 &&
                    value.forceResidual <= 1e-10 && value.torqueResidual <= 1e-10 &&
                    asymmetry <= 1e-6 && difference <= 1e-6 && minimum / hscale > 1e-6 &&
                    std::abs(balance) / balanceScale <= 1e-10 && value.value <= initial + 1e-12;
                reports << step << ',' << tick << ',' << time << ',' << (pass ? "PASS" : "FAIL") << ',' << iteration
                    << ',' << value.gradient.lpNorm<Eigen::Infinity>() << ',' << value.forceResidual << ',' << value.torqueResidual
                    << ',' << initial << ',' << value.value << ',' << minimum << ',' << norm << ',' << hscale
                    << ',' << asymmetry << ',' << difference << ',' << minimum / hscale << ',' << qqMinimum
                    << ',' << oldK << ',' << nextK << ',' << value.energy.shearStretch << ',' << value.energy.bendTwist
                    << ',' << value.energy.ghost << ',' << value.energy.gravity << ',' << ghostWork << ',' << damping
                    << ',' << inertial << ',' << remainder << ',' << nonlinear << ',' << deltaH << ',' << balance
                    << ',' << balanceScale << ',' << std::abs(balance) / balanceScale << ','
                    << (rawScale > 0 ? std::abs(balance) / rawScale : 0) << '\n';
                reports.flush();
                WriteState(states, step, tick, time, value.state, nextVelocity);
                if (!pass) throw std::runtime_error("STEP_GATE_FAILURE");
                old = value.state;
                velocity = nextVelocity;
            }
            catch (const std::exception& error)
            {
                const auto last = At(context, unknown);
                WriteState(states, step, -tick, time, last.state, velocity);
                auto failure = Open(directory / "step-failure.txt");
                failure << "h=" << step << "\ntick=" << tick << "\niterations=" << iteration << "\nreason=" << error.what()
                        << "\ngradient=" << last.gradient.lpNorm<Eigen::Infinity>()
                        << "\nforce=" << last.forceResidual << "\ntorque=" << last.torqueResidual << '\n';
                throw;
            }
        }
        endings.push_back(old);
        std::cout << "R1 h=" << step << " steps=" << ticks << " completed\n";
    }
    auto refinement = Open(directory / "refinement.csv");
    refinement << "pair,position_error,rotation_error\n";
    std::vector<double> positionErrors, rotationErrors;
    for (int pair = 0; pair < 2; ++pair)
    {
        double position = 0.0, rotation = 0.0;
        for (int node = 1; node < 4; ++node)
            position += (endings[pair].positions[node] - endings[pair + 1].positions[node]).squaredNorm();
        for (int segment = 1; segment < 3; ++segment)
            rotation = std::max(rotation, LogR(endings[pair].frames[segment].conjugate() *
                endings[pair + 1].frames[segment], .1).norm());
        positionErrors.push_back(std::sqrt(position) / scale.length);
        rotationErrors.push_back(rotation);
        refinement << pair << ',' << positionErrors.back() << ',' << rotation << '\n';
    }
    bool passed = true;
    auto summary = Open(directory / "summary.csv");
    summary << "item,status,value\ncompleted_steps,PASS,28\n";
    for (int kind = 0; kind < 2; ++kind)
    {
        const auto& errors = kind == 0 ? positionErrors : rotationErrors;
        const bool measurable = errors[0] > 1e-9 && errors[1] > 1e-9;
        const double order = measurable ? std::log2(errors[0] / errors[1]) : 0.0;
        const bool pass = measurable && errors[1] < errors[0] && order >= .5 && order <= 1.5;
        summary << (kind == 0 ? "position_order" : "rotation_order") << ','
                << (pass ? "PASS" : (measurable ? "FAIL" : "INCONCLUSIVE")) << ',' << order << '\n';
        passed = passed && pass;
    }
    summary << "oracle_final_state,NOT_RUN,external_independent_comparison_required\n"
            << "behavior,NOT_RUN,outside_scope\nrotational_inertia,UNSUPPORTED,quasi_static_frames\n";
    return passed ? 0 : 2;
}

int main(int argc, char** argv)
{
    bool created = false;
    try
    {
        if (argc != 3 || (std::string(argv[1]) != "--verify" && std::string(argv[1]) != "--trajectory"))
            throw std::runtime_error("usage: vl_hair_rod_ghost --verify|--trajectory NEW_DIRECTORY");
        if (!std::filesystem::create_directories(argv[2])) throw std::runtime_error("output directory already exists");
        created = true;
        return std::string(argv[1]) == "--verify" ? Verify(argv[2]) : Trajectory(argv[2]);
    }
    catch (const std::exception& error)
    {
        std::cerr << "R1 FAIL: " << error.what() << '\n';
        if (created)
        {
            std::ofstream failure(std::filesystem::path(argv[2]) / "failure.txt");
            failure << error.what() << '\n';
        }
        return 2;
    }
}
