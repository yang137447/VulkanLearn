#pragma once
#include "rodGhostPrototypeMath.h"
#include <array>
#include <filesystem>
#include <fstream>

namespace C2
{
using namespace R1;
using Vector = Eigen::VectorXd;
using Matrix = Eigen::MatrixXd;
constexpr double Spring = 3.24777679370055;
constexpr double Damping = .056989269110075014;
constexpr double Coupling = .1;

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
struct Single
{
    State state;
    Evaluation potential;
    double objective;
    Vector gradient;
    std::vector<Vec> residual;
};
struct PairValue
{
    double energy = 0.0;
    std::array<std::vector<Vec>, 3> force;
    Vector gradient;
    Matrix hessian;
};
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
Context MakeContext();
Value At(const Context&, const Vector&, double);
PairValue PairAt(const Context&, const std::array<State, 3>&, double);
Matrix Hessian(const Context&, const Vector&, double, double);
Matrix GatherQq(const Matrix&);
double Norm(const Matrix&);
double StableDelta(const Context&, const Vector&, const Vector&, const Value&, const Value&);
bool Converged(const Value&);
int PositionIndex(int, int, int);
int FrameIndex(int, int, int);
std::ofstream Open(const std::filesystem::path&);
void MatrixCsv(std::ostream&, const std::string&, const Matrix&);
void StateCsv(std::ostream&, const std::string&, int, double,
              const std::array<State, 3>&, const std::array<std::vector<Vec>, 3>&);
}
