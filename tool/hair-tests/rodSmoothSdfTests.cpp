#include "hair/rod/rodSmoothSdf.h"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

namespace
{
using namespace VL::Hair::Rod;

SmoothSdfGridSI Grid(const std::function<double(const Vec3&)>& scalar)
{
    SmoothSdfGridSI grid;
    grid.dimensions = {12, 11, 10};
    grid.origin = Vec3(-1.25, -1.0, -0.75);
    grid.cellSize = 0.25;
    grid.band = 100.0;
    for (unsigned third = 0; third < grid.dimensions[2]; ++third)
        for (unsigned second = 0; second < grid.dimensions[1]; ++second)
            for (unsigned first = 0; first < grid.dimensions[0]; ++first)
                grid.coefficients.push_back(scalar(grid.origin +
                    grid.cellSize * Vec3(first + 0.5, second + 0.5, third + 0.5)));
    return grid;
}

double Cardinal(double distance)
{
    const double magnitude = std::abs(distance);
    if (magnitude < 1.0)
        return (4.0 - 6.0 * magnitude * magnitude + 3.0 * magnitude * magnitude * magnitude) / 6.0;
    if (magnitude < 2.0)
        return std::pow(2.0 - magnitude, 3) / 6.0;
    return 0.0;
}

double IndependentScalar(const SmoothSdfGridSI& grid, const Vec3& point)
{
    const Vec3 coordinate = (point - grid.origin) / grid.cellSize - Vec3::Constant(0.5);
    double result = 0.0;
    std::size_t index = 0;
    for (unsigned third = 0; third < grid.dimensions[2]; ++third)
        for (unsigned second = 0; second < grid.dimensions[1]; ++second)
            for (unsigned first = 0; first < grid.dimensions[0]; ++first)
            {
                result += grid.coefficients[index] * Cardinal(coordinate.x() - first) *
                    Cardinal(coordinate.y() - second) * Cardinal(coordinate.z() - third);
                ++index;
            }
    return result;
}

double Nonlinear(const Vec3& point)
{
    return 0.3 * std::sin(1.3 * point.x() + 0.7 * point.y()) +
        0.2 * std::cos(0.8 * point.z() - 0.6 * point.x()) +
        0.04 * point.x() * point.y() * point.z();
}

TEST(RodSmoothSdf, ConstantsAndAffineScalarsReproduceWithoutNormalizingGradient)
{
    const SmoothSdfSampler constant(Grid([](const Vec3&) { return 0.021; }));
    const Vec3 point(0.17, 0.23, 0.41);
    const auto constantSample = constant.Sample(point);
    EXPECT_EQ(constantSample.status, SmoothSdfStatus::Available);
    EXPECT_DOUBLE_EQ(constantSample.value, 0.021);
    EXPECT_EQ(constantSample.gradient, Vec3::Zero());
    EXPECT_EQ(constantSample.hessian, Mat3::Zero());
    const Vec3 gradient(0.2, -0.4, 0.9);
    const SmoothSdfSampler affine(Grid([&](const Vec3& position) { return 0.7 + gradient.dot(position); }));
    const auto sample = affine.Sample(point);
    EXPECT_NEAR(sample.value, 0.7 + gradient.dot(point), 2e-15);
    EXPECT_LT((sample.gradient - gradient).norm(), 3e-15);
    EXPECT_LT(sample.hessian.norm(), 3e-14);
    EXPECT_STREQ(SmoothSdfIdentity, "cardinalCubicBSpline64V1");
    EXPECT_EQ(SmoothSdfMathVersion, 1u);
}

TEST(RodSmoothSdf, QuadraticHasKnownSmoothingBiasAndFullExactHessian)
{
    const SmoothSdfSampler sampler(Grid([](const Vec3& point)
    {
        return point.x() * point.x() + 2 * point.y() * point.y() +
            3 * point.z() * point.z() + 0.25 * point.x() * point.y();
    }));
    const Vec3 point(0.17, 0.23, 0.41);
    const auto sample = sampler.Sample(point);
    const double expected = point.x() * point.x() + 2 * point.y() * point.y() +
        3 * point.z() * point.z() + 0.25 * point.x() * point.y() + 2 * 0.25 * 0.25;
    Mat3 hessian = Vec3(2, 4, 6).asDiagonal();
    hessian(0, 1) = hessian(1, 0) = 0.25;
    EXPECT_NEAR(sample.value, expected, 2e-15);
    EXPECT_LT((sample.gradient - Vec3(2 * point.x() + 0.25 * point.y(),
        4 * point.y() + 0.25 * point.x(), 6 * point.z())).norm(), 1e-14);
    EXPECT_LT((sample.hessian - hessian).norm(), 8e-14);
}

TEST(RodSmoothSdf, RawCoefficientsAreApproximatedRatherThanInterpolated)
{
    auto grid = Grid([](const Vec3&) { return 0.0; });
    const std::size_t index = 5 + grid.dimensions[0] * (5 + grid.dimensions[1] * 5);
    grid.coefficients[index] = 1.0;
    const Vec3 point = grid.origin + grid.cellSize * Vec3::Constant(5.5);
    const auto sample = SmoothSdfSampler(std::move(grid)).Sample(point);
    EXPECT_NEAR(sample.value, 8.0 / 27.0, 2e-16);
    EXPECT_LT(sample.value, 1.0);
}

TEST(RodSmoothSdf, ScalarAndAnalyticDerivativesMatchIndependentCardinalKernel)
{
    const auto grid = Grid(Nonlinear);
    const SmoothSdfSampler sampler(grid);
    for (const Vec3& point : {Vec3(0.17, 0.23, 0.41), Vec3(0.125, 0.23, 0.41)})
    {
        const auto sample = sampler.Sample(point);
        ASSERT_EQ(sample.status, SmoothSdfStatus::Available);
        EXPECT_NEAR(sample.value, IndependentScalar(grid, point), 2e-15);
        const double step = 2.5e-5;
        for (int axis = 0; axis < 3; ++axis)
        {
            const Vec3 displacement = step * Vec3::Unit(axis);
            const double positive = IndependentScalar(grid, point + displacement);
            const double negative = IndependentScalar(grid, point - displacement);
            EXPECT_NEAR(sample.gradient[axis], (positive - negative) / (2 * step), 2e-9);
            EXPECT_NEAR(sample.hessian(axis, axis),
                (positive - 2 * IndependentScalar(grid, point) + negative) / (step * step), 3e-6);
            const Vec3 gradientDifference = (sampler.Sample(point + displacement).gradient -
                sampler.Sample(point - displacement).gradient) / (2 * step);
            EXPECT_LT((sample.hessian.col(axis) - gradientDifference).norm(), 3e-6);
            for (int column = axis + 1; column < 3; ++column)
            {
                const Vec3 other = step * Vec3::Unit(column);
                const double mixed = (IndependentScalar(grid, point + displacement + other) -
                    IndependentScalar(grid, point + displacement - other) -
                    IndependentScalar(grid, point - displacement + other) +
                    IndependentScalar(grid, point - displacement - other)) / (4 * step * step);
                EXPECT_NEAR(sample.hessian(axis, column), mixed, 1e-6);
            }
        }
        EXPECT_EQ(sample.hessian, sample.hessian.transpose().eval());
        EXPECT_GE(sample.value + 2e-15, sample.minimumCoefficient);
        EXPECT_LE(sample.value - 2e-15, sample.maximumCoefficient);
    }
}

TEST(RodSmoothSdf, CellFacesHaveContinuousValueGradientAndHessian)
{
    const SmoothSdfSampler sampler(Grid(Nonlinear));
    const Vec3 point(0.125, 0.23, 0.41);
    for (int axis = 0; axis < 3; ++axis)
    {
        Vec3 face = point;
        face[axis] = sampler.Grid().origin[axis] + sampler.Grid().cellSize * 5.5;
        const auto left = sampler.Sample(face - 1e-9 * Vec3::Unit(axis));
        const auto right = sampler.Sample(face + 1e-9 * Vec3::Unit(axis));
        EXPECT_EQ(left.status, SmoothSdfStatus::Available);
        EXPECT_EQ(right.status, SmoothSdfStatus::Available);
        EXPECT_NEAR(left.value, right.value, 2e-9);
        EXPECT_LT((left.gradient - right.gradient).norm(), 3e-9);
        EXPECT_LT((left.hessian - right.hessian).norm(), 1e-8);
    }
}

TEST(RodSmoothSdf, StrictSupportIncludesExactUpperEndpointWithoutGhosts)
{
    auto grid = Grid([](const Vec3&) { return 0.01; });
    grid.dimensions = {4, 4, 4};
    grid.origin.setZero();
    grid.coefficients.assign(64, 0.01);
    const SmoothSdfSampler sampler(grid);
    const Vec3 lower = Vec3::Constant(0.375);
    const Vec3 upper = Vec3::Constant(0.625);
    EXPECT_EQ(sampler.Sample(lower).status, SmoothSdfStatus::Available);
    EXPECT_EQ(sampler.Sample(upper).status, SmoothSdfStatus::Available);
    EXPECT_EQ(sampler.Sample(upper).supportBase, (std::array<std::uint32_t, 3>{0, 0, 0}));
    Vec3 outside = upper;
    outside.x() = std::nextafter(upper.x(), std::numeric_limits<double>::infinity());
    EXPECT_EQ(sampler.Sample(outside).status, SmoothSdfStatus::OutsideDomain);
    outside = lower;
    outside.z() = std::nextafter(lower.z(), -std::numeric_limits<double>::infinity());
    EXPECT_EQ(sampler.Sample(outside).status, SmoothSdfStatus::OutsideDomain);
}

TEST(RodSmoothSdf, SaturationIsExplicitIncludingZeroWeightSupport)
{
    auto grid = Grid([](const Vec3&) { return 100.0; });
    const Vec3 point(0.17, 0.23, 0.41);
    auto sample = SmoothSdfSampler(grid).Sample(point);
    EXPECT_EQ(sample.status, SmoothSdfStatus::PositiveSaturated);
    EXPECT_DOUBLE_EQ(sample.value, 100.0);
    EXPECT_EQ(sample.gradient, Vec3::Zero());
    grid.coefficients.assign(grid.coefficients.size(), -100.0);
    sample = SmoothSdfSampler(grid).Sample(point);
    EXPECT_EQ(sample.status, SmoothSdfStatus::NegativeSaturated);
    EXPECT_DOUBLE_EQ(sample.value, -100.0);
    grid.dimensions = {4, 4, 4};
    grid.origin.setZero();
    grid.coefficients.assign(64, 0.0);
    grid.coefficients[0] = grid.band;
    sample = SmoothSdfSampler(grid).Sample(Vec3::Constant(0.625));
    EXPECT_EQ(sample.status, SmoothSdfStatus::TruncatedSupport);
    const double roundoff = 64 * std::numeric_limits<double>::epsilon() * grid.band;
    EXPECT_NEAR(sample.value, 0.0, roundoff);
    EXPECT_LT(sample.gradient.norm(), roundoff / grid.cellSize);
    EXPECT_LT(sample.hessian.norm(), roundoff / (grid.cellSize * grid.cellSize));
}

TEST(RodSmoothSdf, InvalidGridAndQueryInputsNeverBecomeAvailableGeometry)
{
    auto grid = Grid(Nonlinear);
    grid.dimensions[0] = 3;
    EXPECT_THROW(SmoothSdfSampler{grid}, std::invalid_argument);
    grid = Grid(Nonlinear);
    grid.coefficients.pop_back();
    EXPECT_THROW(SmoothSdfSampler{grid}, std::invalid_argument);
    grid = Grid(Nonlinear);
    grid.cellSize = 0;
    EXPECT_THROW(SmoothSdfSampler{grid}, std::invalid_argument);
    grid.cellSize = 1e-200;
    EXPECT_THROW(SmoothSdfSampler{grid}, std::invalid_argument);
    grid = Grid(Nonlinear);
    grid.coefficients[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(SmoothSdfSampler{grid}, std::invalid_argument);
    grid.coefficients[0] = 101;
    EXPECT_THROW(SmoothSdfSampler{grid}, std::invalid_argument);
    const SmoothSdfSampler sampler(Grid(Nonlinear));
    EXPECT_EQ(sampler.Sample(Vec3(std::numeric_limits<double>::infinity(), 0, 0)).status,
        SmoothSdfStatus::InvalidInput);
}

TEST(RodSmoothSdf, ArithmeticOverflowIsReportedInsteadOfFabricatingContact)
{
    auto grid = Grid([](const Vec3&) { return 0.0; });
    grid.band = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < grid.coefficients.size(); ++index)
        grid.coefficients[index] = index % 2 == 0 ? grid.band : -grid.band;
    EXPECT_EQ(SmoothSdfSampler(grid).Sample(Vec3(0.17, 0.23, 0.41)).status, SmoothSdfStatus::NonFinite);
}

TEST(RodSmoothSdf, RawPlaneClearanceCountsRadiusOnceAndRigidFrameSharesSameScalar)
{
    constexpr double radius = 0.0005;
    const SmoothSdfSampler plane(Grid([](const Vec3& point) { return point.y(); }));
    const auto contact = plane.Sample(Vec3(0.1, radius, 0.2));
    EXPECT_NEAR(contact.value, radius, 3e-16);
    EXPECT_NEAR(contact.value - radius, 0, 3e-16);
    EXPECT_LT((contact.gradient - Vec3::UnitY()).norm(), 3e-15);
    const SmoothSdfSampler sampler(Grid(Nonlinear));
    const Quat rotation(Eigen::AngleAxisd(0.7, Vec3(1, 2, -1).normalized()));
    const Vec3 translation(0.03, -0.02, 0.01);
    const Vec3 local(0.17, 0.23, 0.41);
    const Vec3 world = rotation * local + translation;
    const auto sample = sampler.Sample(rotation.conjugate() * (world - translation));
    const Vec3 worldGradient = rotation * sample.gradient;
    const Mat3 worldHessian = rotation.toRotationMatrix() * sample.hessian *
        rotation.toRotationMatrix().transpose();
    const double step = 1e-5;
    for (int axis = 0; axis < 3; ++axis)
    {
        const auto positive = sampler.Sample(rotation.conjugate() *
            (world + step * Vec3::Unit(axis) - translation));
        const auto negative = sampler.Sample(rotation.conjugate() *
            (world - step * Vec3::Unit(axis) - translation));
        EXPECT_NEAR(worldGradient[axis], (positive.value - negative.value) / (2 * step), 1e-9);
        const Vec3 difference = rotation * ((positive.gradient - negative.gradient) / (2 * step));
        EXPECT_LT((worldHessian.col(axis) - difference).norm(), 2e-9);
    }
}
}
