#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "hair/rod/rodAxialKkt.h"
#include "hair/rod/rodSectionInertia.h"
#include "hair/rod/rodMath.h"

int main(int argc, char** argv)
{
    using namespace VL::Hair::Rod;
    try
    {
        if (argc != 2)
            throw std::runtime_error("expected output-directory");
        std::vector<Vec3> points;
        for (size_t point = 0; point < 5; ++point)
            points.emplace_back(0.0, 0.0, .02 * point);
        std::vector<double> inverseMasses(5, 1.0);
        inverseMasses[0] = 0;
        inverseMasses[1] = 0;
        const auto rest = BuildSectionInertiaRestState(points, {}, {}, inverseMasses);
        const auto old = MakeAuthoredState(rest);
        auto candidate = old;
        for (size_t point = 2; point < 5; ++point)
            candidate.positions[point] += Vec3(.0001, -.00005, .0002 * (point - 1));
        StepInput input;
        input.root.position = points.front();
        input.root.orientation = rest.orientations.front();
        const auto profile = BuildProgressiveSupport(rest);
        const FrozenTangentAir air{Vec3(20, 0, 0), 1.0};
        const std::vector<ContactConstraint> constraints{{"curved", [](const Vec3& position)
        {
            ContactGeometry value;
            value.valid = true;
            value.distance = position.y() + 2.0 * position.squaredNorm();
            value.gradient = Vec3::UnitY() + 4.0 * position;
            value.hessian = 4.0 * Mat3::Identity();
            return value;
        }}};
        AxialContactActiveSet active;
        active.contacts.resize(5);
        active.contacts[2].identities[0] = 0;
        active.contacts[2].multipliers[0] = .0003;
        active.tensileActive.assign(4, true);
        active.tensileActive[0] = false;
        active.tensileMultipliers.assign(4, .0002);
        const auto system = AssembleAxialContactSystem(rest, old, candidate,
            input, profile, constraints, active, &air);
        const auto base = AssembleProgressiveAirSystem(rest, old, candidate, input, profile, air);
        std::vector<float> packed(168, 0);
        const size_t row = 2;
        const auto geometry = constraints[0].sample(candidate.positions[row]);
        for (size_t axis = 0; axis < 3; ++axis)
            packed[axis] = static_cast<float>(geometry.gradient[axis]);
        packed[3] = static_cast<float>(geometry.distance - input.contactMargin);
        packed[4] = .0003f;
        packed[5] = 1;
        for (size_t first = 0; first < 3; ++first)
            for (size_t second = 0; second < 3; ++second)
                packed[6 + first * 3 + second] = static_cast<float>(geometry.hessian(first, second));
        for (size_t edgeSlot = 0; edgeSlot < 2; ++edgeSlot)
        {
            const size_t edge = row - 1 + edgeSlot;
            const auto value = EvaluateAxialLimit(old.positions[edge], old.positions[edge + 1],
                candidate.positions[edge], candidate.positions[edge + 1],
                rest.lengths[edge], active.tensileMultipliers[edge]);
            const size_t offset = 48 + edgeSlot * 20;
            for (size_t axis = 0; axis < 3; ++axis)
                packed[offset + axis] = static_cast<float>(value.direction[axis]);
            packed[offset + 3] = static_cast<float>(value.gap);
            packed[offset + 4] = static_cast<float>(active.tensileMultipliers[edge]);
            packed[offset + 5] = 1;
            packed[offset + 6] = rest.inverseMasses[edge] != 0 ? 1 : 0;
            packed[offset + 7] = rest.inverseMasses[edge + 1] != 0 ? 1 : 0;
            const Mat3 derivative = (Mat3::Identity() - value.direction * value.direction.transpose()) / value.length;
            for (size_t first = 0; first < 3; ++first)
                for (size_t second = 0; second < 3; ++second)
                    packed[offset + 8 + first * 3 + second] = static_cast<float>(derivative(first, second));
        }
        packed[88] = static_cast<float>(system.forceScale);
        packed[89] = static_cast<float>(system.lengthScale);
        std::vector<float> expected(310, 0);
        for (size_t band = 0; band < 3; ++band)
        {
            AxialContactMatrix value = band == 0 ? system.lower[row] :
                band == 1 ? system.diagonal[row] : system.upper[row];
            value.topLeftCorner<6, 6>() -= band == 0 ? base.lower[row] :
                band == 1 ? base.diagonal[row] : base.upper[row];
            for (size_t first = 0; first < 10; ++first)
                for (size_t second = 0; second < 10; ++second)
                    expected[band * 100 + first * 10 + second] = static_cast<float>(value(first, second));
        }
        AxialContactVector value = system.residual[row];
        value.head<6>() -= base.residual[row];
        for (size_t component = 0; component < 10; ++component)
            expected[300 + component] = static_cast<float>(value[component]);
        std::ofstream(std::string(argv[1]) + "/probe-input.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(packed.data()), packed.size() * 4);
        std::ofstream(std::string(argv[1]) + "/cpu-expected.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(expected.data()), expected.size() * 4);
        std::cout << "actual CPU combined-contact+incoming/outgoing axial row exported\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
