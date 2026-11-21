#!/usr/bin/env python3
"""Bake the Ashikhmin/Premoze/Shirley 2000 runtime LUTs.

The runtime evaluator keeps the paper's analytic p(h) and replaces the
per-pixel hemisphere integrals g(k) and R(k) with the same coarse tables the
offline renderer uses.  This generator intentionally mirrors
source/render/offline/core/models/ashikhmin*.cpp so the realtime preview and
the offline reference share one numerical contract.

The generated GLSL header contains all three closures behind
VL_CLOTH_CLOSURE_* guards; a material variant compiles exactly one table set.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

PI = math.pi
DEG2RAD = PI / 180.0


def fmt(value: float) -> str:
    if abs(value) < 1.0e-12:
        return "0.0"
    text = f"{value:.9g}"
    if "e" in text or "E" in text:
        return text
    if "." not in text:
        text += ".0"
    return text


def build_cylinder(sigma: float, steps: int = 256):
    """Return (analytical normalization, normalized beta quadrature)."""
    entries = []
    integral = 0.0
    for index in range(steps):
        t = -8.0 + 16.0 * (index + 0.5) / steps
        slope = sigma * t
        cos_beta = 1.0 / math.sqrt(1.0 + slope * slope)
        sin_beta = slope * cos_beta
        weight = math.exp(-t * t) * sigma / (1.0 + slope * slope) * 16.0 / steps
        entries.append((sin_beta, cos_beta, weight))
        integral += weight
    normalization = 1.0 / (2.0 * integral)
    normalized = [
        (sin_beta, cos_beta, weight / integral)
        for sin_beta, cos_beta, weight in entries
    ]
    return normalization, normalized


def projected_area(entries, direction) -> float:
    kx, ky, kz = direction
    b = abs(ky)
    total = 0.0
    for sin_beta, cos_beta, weight in entries:
        a = kx * sin_beta + kz * cos_beta
        total += weight * (b + a * (PI * 0.5 + math.atan2(a, b))) * 0.25
    return total


def direction(mu: float, phi: float):
    sin_theta = math.sqrt(max(0.0, 1.0 - mu * mu))
    return (sin_theta * math.cos(phi), sin_theta * math.sin(phi), mu)


def normalize(vector):
    x, y, z = vector
    length = math.sqrt(x * x + y * y + z * z)
    if length < 1.0e-12:
        return None
    return (x / length, y / length, z / length)


def dot(a, b) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def fresnel(f0: float, cosine: float) -> float:
    return f0 + (1.0 - f0) * (1.0 - cosine) ** 5.0


def angle_between_projections(v, l) -> float:
    tv = (v[0], v[1], 0.0)
    tl = (l[0], l[1], 0.0)
    length_v = math.hypot(tv[0], tv[1])
    length_l = math.hypot(tl[0], tl[1])
    if length_v <= 1.0e-10 or length_l <= 1.0e-10:
        return None
    cosine = dot(tv, tl) / (length_v * length_l)
    return math.acos(max(-1.0, min(1.0, cosine)))


def correlation(v, l) -> float:
    angle = angle_between_projections(v, l)
    if angle is None:
        return 0.0
    width = 15.0 * DEG2RAD
    return math.exp(-0.5 * angle * angle / (width * width))


class Table2D:
    def __init__(self, mu_count: int, phi_count: int, phi_extent: float, values):
        self.mu_count = mu_count
        self.phi_count = phi_count
        self.phi_extent = phi_extent
        self.values = values

    def lookup(self, direction) -> float:
        x, y, z = direction
        mu = max(0.0, min(1.0, z)) * (self.mu_count - 1)
        if self.phi_extent >= PI * 0.999:
            phi = math.atan2(abs(y), x) / self.phi_extent * (self.phi_count - 1)
        else:
            phi = math.atan2(abs(y), abs(x)) / self.phi_extent * (self.phi_count - 1)
        phi = max(0.0, min(float(self.phi_count - 1), phi))
        i = min(int(mu), self.mu_count - 2)
        j = min(int(phi), self.phi_count - 2)
        tx = mu - i
        ty = phi - j
        a = self.values[i * self.phi_count + j]
        b = self.values[i * self.phi_count + j + 1]
        c = self.values[(i + 1) * self.phi_count + j]
        d = self.values[(i + 1) * self.phi_count + j + 1]
        return (
            a * (1.0 - tx) * (1.0 - ty)
            + b * (1.0 - tx) * ty
            + c * tx * (1.0 - ty)
            + d * tx * ty
        )


class Table1D:
    def __init__(self, values):
        self.values = values

    def lookup(self, mu: float) -> float:
        x = max(0.0, min(1.0, mu)) * (len(self.values) - 1)
        i = min(int(x), len(self.values) - 2)
        t = x - i
        return self.values[i] * (1.0 - t) + self.values[i + 1] * t


def build_satin(mu_count: int = 65, phi_count: int = 33):
    sigma_flat = 0.1
    sigma_ends = 0.3
    flat_weight = 0.7
    norm_flat, flat = build_cylinder(sigma_flat, 192)
    norm_ends, ends = build_cylinder(sigma_ends, 192)

    def mixed_area(k) -> float:
        return (
            flat_weight * projected_area(flat, k)
            + (1.0 - flat_weight) * projected_area(ends, k)
        )

    g_values = []
    phi_extent = PI * 0.5
    for i in range(mu_count):
        mu = i / (mu_count - 1)
        for j in range(phi_count):
            phi = phi_extent * j / (phi_count - 1)
            g_values.append(mixed_area(direction(mu, phi)))

    g_table = Table2D(mu_count, phi_count, phi_extent, g_values)
    normal_mean = mixed_area((0.0, 0.0, 1.0))

    def density(h) -> float:
        if h[2] <= 0.0:
            return 0.0
        slope = h[0] / h[2]
        flat_term = norm_flat * math.exp(-(slope * slope) / (sigma_flat * sigma_flat))
        end_term = norm_ends * math.exp(-(slope * slope) / (sigma_ends * sigma_ends))
        return flat_weight * flat_term + (1.0 - flat_weight) * end_term

    def specular(v, l, g_v, g_l, f0) -> float:
        h = normalize((v[0] + l[0], v[1] + l[1], v[2] + l[2]))
        if h is None:
            return 0.0
        h_dot_v = max(0.0, dot(v, h))
        return (
            density(h)
            * normal_mean
            * fresnel(f0, h_dot_v)
            / max(4.0 * g_v * g_l, 1.0e-12)
        )

    def integrate_reflectance(f0: float) -> list[float]:
        values = []
        light_steps = 64
        azimuth_steps = 128
        for i in range(mu_count):
            mu = max(1.0e-6, i / (mu_count - 1))
            for j in range(phi_count):
                phi = phi_extent * j / (phi_count - 1)
                v = direction(mu, phi)
                g_v = g_table.lookup(v)
                total = 0.0
                for light_i in range(light_steps):
                    no_l = (light_i + 0.5) / light_steps
                    sin_l = math.sqrt(max(0.0, 1.0 - no_l * no_l))
                    for azimuth_j in range(azimuth_steps):
                        light_phi = 2.0 * PI * (azimuth_j + 0.5) / azimuth_steps
                        l = (
                            sin_l * math.cos(light_phi),
                            sin_l * math.sin(light_phi),
                            no_l,
                        )
                        total += specular(v, l, g_v, g_table.lookup(l), f0) * no_l
                values.append(total * 2.0 * PI / (light_steps * azimuth_steps))
        return values

    r0_values = integrate_reflectance(0.0)
    r1_values = integrate_reflectance(1.0)
    r0_table = Table2D(mu_count, phi_count, phi_extent, r0_values)
    r1_table = Table2D(mu_count, phi_count, phi_extent, r1_values)

    def normalization(f0: float, table: Table2D) -> float:
        remaining = 0.0
        for i in range(mu_count - 1):
            mu = (i + 0.5) / (mu_count - 1)
            for j in range(phi_count - 1):
                phi = phi_extent * (j + 0.5) / (phi_count - 1)
                r = table.lookup(direction(mu, phi))
                remaining += (1.0 - r) * mu
        return 1.0 / (
            2.0 * PI * remaining / ((mu_count - 1) * (phi_count - 1))
        )

    return {
        "mu": mu_count,
        "phi": phi_count,
        "phi_extent": phi_extent,
        "g": g_values,
        "r0": r0_values,
        "r1": r1_values,
        "normal_mean": normal_mean,
        "c0": normalization(0.0, r0_table),
        "c1": normalization(1.0, r1_table),
        "sigma_flat": sigma_flat,
        "sigma_ends": sigma_ends,
        "flat_weight": flat_weight,
        "norm_flat": norm_flat,
        "norm_ends": norm_ends,
    }


def build_velvet(mu_count: int = 65):
    sigma = 0.5
    normal_steps = 512
    azimuth_steps = 256

    def density(no_h: float) -> float:
        if no_h <= 0.0 or no_h >= 1.0:
            return 0.0
        sin2 = 1.0 - no_h * no_h
        return math.exp(-(no_h * no_h) / (sigma * sigma * sin2))

    density_integral = 0.0
    projected_integral = 0.0
    for i in range(normal_steps):
        mu = (i + 0.5) / normal_steps
        p = density(mu)
        density_integral += p
        projected_integral += mu * p
    normalization = normal_steps / (2.0 * PI * density_integral)
    normal_mean = projected_integral / density_integral

    g_values = []
    for index in range(mu_count):
        no_k = index / (mu_count - 1)
        sin_k = math.sqrt(max(0.0, 1.0 - no_k * no_k))
        total = 0.0
        for i in range(256):
            no_h = (i + 0.5) / 256.0
            sin_h = math.sqrt(max(0.0, 1.0 - no_h * no_h))
            p = density(no_h)
            for j in range(128):
                phi = 2.0 * PI * (j + 0.5) / 128.0
                total += p * max(0.0, no_k * no_h + sin_k * sin_h * math.cos(phi))
        g_values.append(normalization * 2.0 * PI * total / (256 * 128))
    g_table = Table1D(g_values)

    def specular(v, l, g_v, g_l, f0) -> float:
        no_v = v[2]
        no_l = l[2]
        if no_v <= 0.0 or no_l <= 0.0:
            return 0.0
        h = normalize((v[0] + l[0], v[1] + l[1], v[2] + l[2]))
        if h is None:
            return 0.0
        p = density(h[2])
        if p <= 0.0:
            return 0.0
        p_v = no_v * normal_mean / max(g_v, 1.0e-12)
        p_l = no_l * normal_mean / max(g_l, 1.0e-12)
        t = correlation(v, l)
        correlated = (1.0 - t) + t / max(p_v, p_l, 1.0e-4)
        return (
            normalization
            * p
            * normal_mean
            * fresnel(f0, max(0.0, dot(v, h)))
            * correlated
            / max(4.0 * g_v * g_l, 1.0e-12)
        )

    def integrate_reflectance(f0: float) -> list[float]:
        values = []
        light_steps = 64
        azimuth_steps = 128
        for index in range(mu_count):
            no_v = max(1.0e-6, index / (mu_count - 1))
            v = (math.sqrt(max(0.0, 1.0 - no_v * no_v)), 0.0, no_v)
            g_v = g_table.lookup(no_v)
            total = 0.0
            for i in range(light_steps):
                no_l = (i + 0.5) / light_steps
                sin_l = math.sqrt(max(0.0, 1.0 - no_l * no_l))
                for j in range(azimuth_steps):
                    phi = 2.0 * PI * (j + 0.5) / azimuth_steps
                    l = (sin_l * math.cos(phi), sin_l * math.sin(phi), no_l)
                    total += specular(v, l, g_v, g_table.lookup(no_l), f0) * no_l
            values.append(total * 2.0 * PI / (light_steps * azimuth_steps))
        return values

    r0_values = integrate_reflectance(0.0)
    r1_values = integrate_reflectance(1.0)
    r0_table = Table1D(r0_values)
    r1_table = Table1D(r1_values)

    def normalization_constant(table: Table1D) -> float:
        remaining = 0.0
        for i in range(mu_count - 1):
            mu = (i + 0.5) / (mu_count - 1)
            remaining += (1.0 - table.lookup(mu)) * mu
        return 1.0 / (2.0 * PI * remaining / (mu_count - 1))

    return {
        "mu": mu_count,
        "phi": 1,
        "g": g_values,
        "r0": r0_values,
        "r1": r1_values,
        "normal_mean": normal_mean,
        "c0": normalization_constant(r0_table),
        "c1": normalization_constant(r1_table),
        "sigma": sigma,
        "density_normalization": normalization,
    }


def build_slanted_velvet(mu_count: int = 65, phi_count: int = 65):
    sigma = 0.5
    slant_degrees = 40.0
    angle = (slant_degrees - 90.0) * DEG2RAD
    cosine = math.cos(angle)
    sine = math.sin(angle)
    normalization, entries = build_cylinder(sigma, 256)
    phi_extent = PI

    def rotate_local(d):
        return (
            cosine * d[0] - sine * d[2],
            d[1],
            sine * d[0] + cosine * d[2],
        )

    g_values = []
    for i in range(mu_count):
        mu = i / (mu_count - 1)
        for j in range(phi_count):
            phi = phi_extent * j / (phi_count - 1)
            g_values.append(projected_area(entries, rotate_local(direction(mu, phi))))
    g_table = Table2D(mu_count, phi_count, phi_extent, g_values)

    normal_mean = 0.0
    for sin_beta, cos_beta, weight in entries:
        del sin_beta
        normal_mean += weight * cos_beta * (PI * 0.25) * math.cos(angle)
    normal_mean *= 2.0

    def density(h) -> float:
        local = rotate_local(h)
        if local[2] <= 0.0:
            return 0.0
        slope = local[0] / local[2]
        return normalization * math.exp(-(slope * slope) / (sigma * sigma))

    def specular(v, l, g_v, g_l, f0) -> float:
        no_v = v[2]
        no_l = l[2]
        if no_v <= 0.0 or no_l <= 0.0:
            return 0.0
        h = normalize((v[0] + l[0], v[1] + l[1], v[2] + l[2]))
        if h is None:
            return 0.0
        p = density(h)
        if p <= 0.0:
            return 0.0
        p_v = no_v * normal_mean / max(g_v, 1.0e-12)
        p_l = no_l * normal_mean / max(g_l, 1.0e-12)
        t = correlation(v, l)
        correlated = (1.0 - t) + t / max(p_v, p_l, 1.0e-4)
        return (
            p
            * normal_mean
            * fresnel(f0, max(0.0, dot(v, h)))
            * correlated
            / max(4.0 * g_v * g_l, 1.0e-12)
        )

    def integrate_reflectance(f0: float) -> list[float]:
        values = []
        light_steps = 64
        azimuth_steps = 128
        for i in range(mu_count):
            no_v = max(1.0e-6, i / (mu_count - 1))
            for j in range(phi_count):
                phi = phi_extent * j / (phi_count - 1)
                v = direction(no_v, phi)
                g_v = g_table.lookup(v)
                total = 0.0
                for light_i in range(light_steps):
                    no_l = (light_i + 0.5) / light_steps
                    sin_l = math.sqrt(max(0.0, 1.0 - no_l * no_l))
                    for azimuth_j in range(azimuth_steps):
                        light_phi = 2.0 * PI * (azimuth_j + 0.5) / azimuth_steps
                        l = (
                            sin_l * math.cos(light_phi),
                            sin_l * math.sin(light_phi),
                            no_l,
                        )
                        total += specular(v, l, g_v, g_table.lookup(l), f0) * no_l
                values.append(total * 2.0 * PI / (light_steps * azimuth_steps))
        return values

    r0_values = integrate_reflectance(0.0)
    r1_values = integrate_reflectance(1.0)
    r0_table = Table2D(mu_count, phi_count, phi_extent, r0_values)
    r1_table = Table2D(mu_count, phi_count, phi_extent, r1_values)

    def normalization_constant(table: Table2D) -> float:
        light_steps = 64
        azimuth_steps = 128
        remaining = 0.0
        for i in range(light_steps):
            mu = (i + 0.5) / light_steps
            for j in range(azimuth_steps):
                phi = 2.0 * PI * (j + 0.5) / azimuth_steps
                remaining += (1.0 - table.lookup(direction(mu, phi))) * mu
        remaining *= 2.0 * PI / (light_steps * azimuth_steps)
        return 1.0 / remaining

    return {
        "mu": mu_count,
        "phi": phi_count,
        "phi_extent": phi_extent,
        "g": g_values,
        "r0": r0_values,
        "r1": r1_values,
        "normal_mean": normal_mean,
        "c0": normalization_constant(r0_table),
        "c1": normalization_constant(r1_table),
        "sigma": sigma,
        "slant_degrees": slant_degrees,
        "slant_angle": angle,
        "density_normalization": normalization,
    }


def emit_header(satin, velvet, slanted) -> str:
    chunks = [
        "// Generated by tool/cloth-runtime-lut/generate_cloth_ashikhmin_lut.py.",
        "// Ashikhmin, Premoze, Shirley 2000 runtime constants; do not edit by hand.",
        "// The g/R tables live in Common/Source/Textures/T_clothAshikhminLut.exr.",
        "#ifndef VL_COMMON_CLOTH_ASHIKHMIN_CONSTANTS_GLSL",
        "#define VL_COMMON_CLOTH_ASHIKHMIN_CONSTANTS_GLSL",
        "",
        "#if defined(VL_CLOTH_CLOSURE_SATIN)",
        f"#define VL_CLOTH_NORMAL_MEAN {fmt(satin['normal_mean'])}",
        f"#define VL_CLOTH_DIFFUSE_C0 {fmt(satin['c0'])}",
        f"#define VL_CLOTH_DIFFUSE_C1 {fmt(satin['c1'])}",
        f"#define VL_CLOTH_SIGMA_FLAT {fmt(satin['sigma_flat'])}",
        f"#define VL_CLOTH_SIGMA_ENDS {fmt(satin['sigma_ends'])}",
        f"#define VL_CLOTH_FLAT_WEIGHT {fmt(satin['flat_weight'])}",
        f"#define VL_CLOTH_NORM_FLAT {fmt(satin['norm_flat'])}",
        f"#define VL_CLOTH_NORM_ENDS {fmt(satin['norm_ends'])}",
        "#elif defined(VL_CLOTH_CLOSURE_VELVET)",
        f"#define VL_CLOTH_NORMAL_MEAN {fmt(velvet['normal_mean'])}",
        f"#define VL_CLOTH_DIFFUSE_C0 {fmt(velvet['c0'])}",
        f"#define VL_CLOTH_DIFFUSE_C1 {fmt(velvet['c1'])}",
        f"#define VL_CLOTH_SIGMA {fmt(velvet['sigma'])}",
        f"#define VL_CLOTH_DENSITY_NORMALIZATION {fmt(velvet['density_normalization'])}",
        "#elif defined(VL_CLOTH_CLOSURE_SLANTED_VELVET)",
        f"#define VL_CLOTH_NORMAL_MEAN {fmt(slanted['normal_mean'])}",
        f"#define VL_CLOTH_DIFFUSE_C0 {fmt(slanted['c0'])}",
        f"#define VL_CLOTH_DIFFUSE_C1 {fmt(slanted['c1'])}",
        f"#define VL_CLOTH_SIGMA {fmt(slanted['sigma'])}",
        f"#define VL_CLOTH_SLANT_DEGREES {fmt(slanted['slant_degrees'])}",
        f"#define VL_CLOTH_SLANT_ANGLE {fmt(slanted['slant_angle'])}",
        f"#define VL_CLOTH_DENSITY_NORMALIZATION {fmt(slanted['density_normalization'])}",
        "#else",
        "#error \"ClothAshikhmin requires exactly one VL_CLOTH_CLOSURE_* macro\"",
        "#endif",
        "",
        "#define VL_CLOTH_LUT_SIZE 256.0",
        "#define VL_CLOTH_LUT_TILE 64.0",
        "",
        "#endif",
        "",
    ]
    return "\n".join(chunks)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("shader/glsl/common/clothAshikhminConstants.glsl"),
    )
    args = parser.parse_args()

    satin = build_satin()
    velvet = build_velvet()
    slanted = build_slanted_velvet()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(emit_header(satin, velvet, slanted), encoding="utf-8")
    print(f"wrote {args.output}")
    print(
        "satin eta={:.6f} c0={:.6f} c1={:.6f}".format(
            satin["normal_mean"], satin["c0"], satin["c1"]
        )
    )
    print(
        "velvet eta={:.6f} c0={:.6f} c1={:.6f}".format(
            velvet["normal_mean"], velvet["c0"], velvet["c1"]
        )
    )
    print(
        "slanted eta={:.6f} c0={:.6f} c1={:.6f}".format(
            slanted["normal_mean"], slanted["c0"], slanted["c1"]
        )
    )


if __name__ == "__main__":
    main()
