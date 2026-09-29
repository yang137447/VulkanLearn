#pragma once

#include <algorithm>
#include <array>
#include <initializer_list>
#include <stdexcept>

#include "core/renderer.h"

namespace VL::Offline::Test
{
inline YarnMaterial TableIIYarn(double coverage, Vec3 dye, double kd,
                                double gammaS, double gammaV,
                                std::initializer_list<double> offsets,
                                std::initializer_list<double> lengths)
{
    if (offsets.size() < 2 || offsets.size() > 8 ||
        lengths.size() != offsets.size() - 1)
        throw std::logic_error("invalid Table II tangent curve");
    YarnMaterial yarn{coverage, dye, kd, gammaS, gammaV};
    yarn.controlPoints = static_cast<int>(offsets.size());
    std::copy(offsets.begin(), offsets.end(), yarn.offsets.begin());
    std::copy(lengths.begin(), lengths.end(), yarn.lengths.begin());
    resampleYarn(yarn, kTangentSamples);
    return yarn;
}

inline const std::array<FabricMaterial, 6>& TableIIFabrics()
{
    // 期刊 Table II 的零长度段与偏移量是公式回归输入，不能从几何重新推断。
    static const std::array<FabricMaterial, 6> fabrics{{
        {"cloth-b-linen", "Linen Plain", 1.46,
         {TableIIYarn(0.33, {0.06, 0.24, 0.30}, 0.3, 12, 24,
                      {-25, 25}, {1}),
          TableIIYarn(0.33, {0.06, 0.24, 0.30}, 0.3, 12, 24,
                      {-25, 25}, {1})}, ""},
        {"cloth-b-silk-crepe", "Silk Crepe de Chine", 1.345,
         {TableIIYarn(0.75, {0.12, 0.114, 0.006}, 0.2, 5, 10,
                      {-35, -35, 35, 35}, {1, 1, 1}),
          TableIIYarn(0.25, {0.16, 0.152, 0.008}, 0.3, 18, 32,
                      {0, 0}, {1})}, ""},
        {"cloth-b-satin-front", "Polyester Satin Charmeuse (front)", 1.539,
         {TableIIYarn(0.90, {0.035, 0.01295, 0.0105}, 0.1, 2.5, 5,
                      {-32, -32, -18, 0, 0, 18, 32, 32},
                      {1.33, 0.66, 2, 2, 2, 0.66, 1.33}),
          TableIIYarn(0.10, {0.2, 0.074, 0.06}, 0.7, 30, 60,
                      {0, 0}, {1})}, ""},
        {"cloth-b-satin-back", "Polyester Satin Charmeuse (back)", 1.539,
         {TableIIYarn(0.67, {0.035, 0.01295, 0.0105}, 0.1, 2.5, 5,
                      {-30, -30, 30, 30, -5, -5, 5, 5},
                      {1.33, 1.33, 1.33, 0, 0.67, 0.67, 0.67}),
          TableIIYarn(0.33, {0.2, 0.074, 0.06}, 0.7, 30, 60,
                      {0, 0}, {3})}, ""},
        {"cloth-b-silk-shot", "Silk Shot Fabric", 1.345,
         {TableIIYarn(0.86, {0.02, 0.2, 0.08}, 0.1, 4, 8,
                      {-25, -25, 25, 25}, {1.33, 2.67, 1.33}),
          TableIIYarn(0.14, {0.6, 0.0, 0.06}, 0.1, 5, 10,
                      {0, 0}, {1})}, ""},
        {"cloth-b-velvet", "Velvet", 1.46,
         {TableIIYarn(0.5, {0.015, 0.006, 0.0}, 0.1, 6, 12,
                      {-90, -50}, {1}),
          TableIIYarn(0.5, {0.015, 0.006, 0.0}, 0.1, 6, 12,
                      {-90, -55, 55, 90}, {0.5, 0, 0.5})},
         "original spatial groom texture unavailable"}
    }};
    return fabrics;
}
} // namespace VL::Offline::Test
