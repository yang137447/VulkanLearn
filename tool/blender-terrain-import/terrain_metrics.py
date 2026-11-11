"""Quantitative morphology metrics for terrain acceptance.

This module is intentionally pure NumPy so it can run both inside Blender and
in a normal Python environment.  Height samples are ``z_m[y, x]``; ``x`` grows
to the right (east) and ``y`` grows down the image (south).  Every derivative
uses ``cell_size_m``, so the results remain comparable when the heightmap
resolution changes.

The two morphology fingerprints this file is meant to separate are:

* a centered, radially symmetric island/plateau with concentric terrace rings;
* a mountain range with elongated ridges and a branching drainage network.

The metrics are deliberately descriptive rather than a single quality score.
Read ``radial_symmetry``, ``anisotropy``, ``ridge_density`` and
``drainage_length_km`` together.
"""

from __future__ import annotations

from collections import deque
from typing import Iterable

import numpy as np


METRIC_VERSION = 1
_NEIGHBORS8 = (
    (-1, -1), (-1, 0), (-1, 1),
    (0, -1),           (0, 1),
    (1, -1),  (1, 0),  (1, 1),
)


def _as_height_array(z_m: np.ndarray) -> np.ndarray:
    height = np.asarray(z_m, dtype=np.float64)
    if height.ndim != 2:
        raise ValueError(f"z_m must be a 2D array, got shape {height.shape}")
    if min(height.shape) < 3:
        raise ValueError(f"z_m must be at least 3x3, got shape {height.shape}")

    finite = np.isfinite(height)
    if not finite.any():
        raise ValueError("z_m contains no finite samples")
    if not finite.all():
        # A broken source row should not silently poison a whole metric run.
        fill = float(np.median(height[finite]))
        height = np.where(finite, height, fill)
    return height


def _cell_sizes(cell_size_m: float | Iterable[float]) -> tuple[float, float]:
    values = np.asarray(cell_size_m, dtype=np.float64)
    if values.ndim == 0:
        sy = sx = float(values)
    elif values.size == 2:
        sy, sx = float(values[0]), float(values[1])
    else:
        raise ValueError("cell_size_m must be a scalar or a (row, column) pair")
    if not np.isfinite(sy) or not np.isfinite(sx) or sy <= 0.0 or sx <= 0.0:
        raise ValueError(f"cell sizes must be positive and finite, got {(sy, sx)}")
    return sy, sx


def _smooth3(height: np.ndarray, passes: int = 1) -> np.ndarray:
    """Small separable [1, 2, 1] blur used before derivatives.

    The blur removes single-texel quantization speckle without changing the
    large-scale silhouette.  It is deliberately very mild: slope percentiles
    should still describe the authored terrain, not a heavily smoothed proxy.
    """
    result = np.asarray(height, dtype=np.float64)
    for _ in range(max(0, passes)):
        padded = np.pad(result, 1, mode="reflect")
        result = (
            padded[:-2, :-2] + 2.0 * padded[:-2, 1:-1] + padded[:-2, 2:]
            + 2.0 * padded[1:-1, :-2] + 4.0 * padded[1:-1, 1:-1]
            + 2.0 * padded[1:-1, 2:]
            + padded[2:, :-2] + 2.0 * padded[2:, 1:-1] + padded[2:, 2:]
        ) / 16.0
    return result


def _laplacian(height: np.ndarray, sy: float, sx: float) -> np.ndarray:
    """Second derivative with positive values in valleys (concave up).

    A hilltop has a negative Laplacian; a valley has a positive one.  Using
    the physically scaled second derivative makes the sign convention
    independent of image resolution.
    """
    padded = np.pad(height, 1, mode="edge")
    center = padded[1:-1, 1:-1]
    d2y = (padded[2:, 1:-1] - 2.0 * center + padded[:-2, 1:-1]) / (sy * sy)
    d2x = (padded[1:-1, 2:] - 2.0 * center + padded[1:-1, :-2]) / (sx * sx)
    return d2y + d2x


def _percentile(values: np.ndarray, q: float, default: float = 0.0) -> float:
    if values.size == 0:
        return float(default)
    return float(np.percentile(values, q))


def _connected_component_lengths(mask: np.ndarray) -> list[int]:
    """Return 8-connected component sizes in pixels for a boolean mask."""
    height, width = mask.shape
    visited = np.zeros_like(mask, dtype=bool)
    components: list[int] = []

    for start_y, start_x in np.argwhere(mask):
        y0 = int(start_y)
        x0 = int(start_x)
        if visited[y0, x0]:
            continue

        visited[y0, x0] = True
        queue: deque[tuple[int, int]] = deque(((y0, x0),))
        size = 0
        while queue:
            y, x = queue.popleft()
            size += 1
            for dy, dx in _NEIGHBORS8:
                yy = y + dy
                xx = x + dx
                if (
                    0 <= yy < height
                    and 0 <= xx < width
                    and mask[yy, xx]
                    and not visited[yy, xx]
                ):
                    visited[yy, xx] = True
                    queue.append((yy, xx))
        components.append(size)
    return components


def _drainage_network(
    height: np.ndarray,
    sy: float,
    sx: float,
    relief_m: float,
) -> tuple[float, float, int, int, int]:
    """Detect simplified valley lines and measure their connected length.

    A cell is a valley-line candidate when it is within a small local-relief
    band of the 3x3 minimum on the lightly smoothed height field.  The band
    (5% of local relief) turns isolated one-texel pits into connected valley
    lines, which is necessary on a rasterized continuous surface.  Flat or
    nearly flat regions are rejected using local relief, otherwise every
    texel in a sea-level plain would be classified as a valley.  Connected
    components shorter than
    ``MIN_DRAINAGE_COMPONENT_PIXELS`` are treated as quantization noise for
    the primary metric; the unfiltered length is also reported for reference.
    """
    padded = np.pad(height, 1, mode="edge")
    stack = [padded[dy:dy + height.shape[0], dx:dx + height.shape[1]]
             for dy in range(3) for dx in range(3)]
    local_min = np.min(stack, axis=0)
    local_max = np.max(stack, axis=0)

    local_relief = local_max - local_min
    min_relief_for_valley = max(0.05, relief_m * 0.0015)
    # A zero tolerance only selects exact raster minima, which are isolated
    # points even inside a continuous valley.  A small local-relief band
    # recovers the valley floor without flooding the whole slope.
    valley_tolerance = np.maximum(1.0e-9, local_relief * 0.05)
    is_valley = (
        (height <= local_min + valley_tolerance)
        & (local_relief > min_relief_for_valley)
    )
    is_valley[[0, -1], :] = False
    is_valley[:, [0, -1]] = False

    components = _connected_component_lengths(is_valley)
    valley_pixels = int(is_valley.sum())
    min_component_pixels = 8
    kept = [size for size in components if size >= min_component_pixels]
    mean_spacing_m = 0.5 * (sy + sx)
    filtered_km = sum(kept) * mean_spacing_m / 1000.0
    all_km = valley_pixels * mean_spacing_m / 1000.0
    return filtered_km, all_km, len(components), len(kept), valley_pixels


def _radial_symmetry(
    height: np.ndarray,
    sy: float,
    sx: float,
    relief_m: float,
) -> tuple[float, int, float]:
    """Measure azimuthal variation inside concentric radius bands.

    The origin is the height-weighted centroid above the lowest sample.  For
    each radius ring we take the standard deviation and the p90-p10 spread of
    the heights inside that ring, normalize both by total relief, and average
    the rings with equal weight.  A centered cone or terrace disc produces
    rings whose heights are nearly constant, so the score approaches zero.
    An irregular mountain range produces large within-ring spread.

    Returns ``(symmetry_0_to_1, ring_count, analyzed_pixel_fraction)``.
    ``1`` is a perfect concentric disc and ``0`` has no radial symmetry.
    Lower is better for a natural mountain range.
    """
    h, w = height.shape
    weights = np.maximum(height - float(height.min()), 0.0)
    total_weight = float(weights.sum())
    yy, xx = np.indices(height.shape, dtype=np.float64)

    if total_weight > 1.0e-12:
        cy = float((weights * yy).sum() / total_weight)
        cx = float((weights * xx).sum() / total_weight)
    else:
        cy = 0.5 * (h - 1)
        cx = 0.5 * (w - 1)

    # Keep complete rings inside the sample rectangle.  Taking the nearest
    # border avoids partial rings whose azimuthal spread would be biased by
    # the rectangular crop.
    radius_limit_m = min(
        cy * sy,
        (h - 1 - cy) * sy,
        cx * sx,
        (w - 1 - cx) * sx,
    )
    if radius_limit_m <= 0.0 or relief_m <= 1.0e-12:
        return 1.0, 0, 0.0

    radius_m = np.hypot((yy - cy) * sy, (xx - cx) * sx)
    valid = radius_m <= radius_limit_m
    analyzed_fraction = float(valid.mean())
    if not valid.any():
        return 1.0, 0, 0.0

    ring_count = int(np.clip(min(h, w) // 16, 12, 32))
    ring_index = np.floor(radius_m / radius_limit_m * ring_count).astype(np.int32)
    ring_index = np.clip(ring_index, 0, ring_count - 1)

    scores: list[float] = []
    for ring in range(ring_count):
        values = height[valid & (ring_index == ring)]
        if values.size < 4:
            continue
        std_score = float(values.std()) / relief_m
        spread_score = float(
            np.percentile(values, 90) - np.percentile(values, 10)
        ) / relief_m
        score = 0.65 * std_score + 0.35 * spread_score
        scores.append(float(np.clip(score, 0.0, 1.0)))

    if not scores:
        return 1.0, 0, analyzed_fraction
    ring_spread = float(np.clip(np.mean(scores), 0.0, 1.0))
    return 1.0 - ring_spread, len(scores), analyzed_fraction


def _anisotropy(
    gy: np.ndarray,
    gx: np.ndarray,
    border_fraction: float = 0.02,
) -> float:
    """Eigenvalue ratio of the image-wide gradient structure tensor.

    The tensor is built from gradients after removing a thin border (the
    rectangular domain boundary otherwise contributes a strong artificial
    horizontal/vertical signal).  Concentric rings have gradients pointing in
    every azimuth, so lambda1/lambda2 is near 1.  A dominant ridge direction
    raises the ratio.  The value is capped at 50 for JSON-friendly output.
    """
    h, w = gy.shape
    margin = max(2, int(round(border_fraction * min(h, w))))
    if h <= 2 * margin + 1 or w <= 2 * margin + 1:
        margin = 0

    gy_c = gy[margin:h - margin, margin:w - margin]
    gx_c = gx[margin:h - margin, margin:w - margin]
    a = float(np.mean(gx_c * gx_c))
    b = float(np.mean(gx_c * gy_c))
    c = float(np.mean(gy_c * gy_c))

    trace_half = 0.5 * (a + c)
    disc = float(np.hypot(0.5 * (a - c), b))
    lambda1 = trace_half + disc
    lambda2 = trace_half - disc
    if lambda1 <= 1.0e-15:
        return 1.0
    epsilon = max(1.0e-15, lambda1 * 1.0e-6)
    return float(np.clip(lambda1 / max(lambda2, epsilon), 1.0, 50.0))


def hillshade(
    z_m: np.ndarray,
    cell_size_m: float,
    azimuth_deg: float = 315.0,
    altitude_deg: float = 40.0,
) -> np.ndarray:
    """Return a fixed-light grayscale hillshade in the range 0..1.

    ``azimuth_deg`` is measured clockwise from north (image up); the default
    315 degrees puts the light in the north-west.  ``altitude_deg`` is measured
    from the horizon.  The surface normal is derived from physically scaled
    gradients, then dotted with the light direction.
    """
    height = _as_height_array(z_m)
    sy, sx = _cell_sizes(cell_size_m)
    smooth = _smooth3(height, passes=1)
    gy, gx = np.gradient(smooth, sy, sx, edge_order=1)

    azimuth = np.deg2rad(float(azimuth_deg))
    altitude = np.deg2rad(float(altitude_deg))
    # Row-down coordinate system: north is -row, so the y component of the
    # light direction is negative for a north-west light.
    light = np.array(
        (
            np.sin(azimuth) * np.cos(altitude),
            -np.cos(azimuth) * np.cos(altitude),
            np.sin(altitude),
        ),
        dtype=np.float64,
    )
    normal = np.stack((-gx, -gy, np.ones_like(gx)), axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    return np.clip(normal @ light, 0.0, 1.0)


def analyze(
    z_m: np.ndarray,
    cell_size_m: float,
    sea_level_m: float = 0.0,
) -> dict:
    """Compute JSON-serializable terrain acceptance metrics.

    Definitions
    -----------
    ``min/max/mean/p05/p95_height_m``
        Height distribution in metres.
    ``sea_fraction``
        Fraction of samples at or below ``sea_level_m``.
    ``relief_m``
        ``max_height_m - min_height_m``.
    ``slope_deg_*``
        Slope magnitude ``atan(|grad z|)`` in degrees.  p50/p90/p99 are
        reported because a single mean hides cliff distribution.
    ``ridge_density``
        Fraction of samples with negative Laplacian (convex, hilltop/ridge)
        sharper than 0.20 * p95(|Laplacian|), and gradient magnitude above
        p60 of the field.  This is a curvature/steepness proxy, not a
        hydrological ridge extraction.
    ``valley_density``
        The same test for positive Laplacian (concave-up valleys).
    ``drainage_length_km``
        Total length of 8-connected 3x3-minimum valley lines (allowing a 5%
        local-relief band so raster minima connect into lines), counting only
        components of at least 8 pixels.  A pixel is approximately one cell
        long; diagonal steps are not given a separate sqrt(2) weight.
    ``radial_symmetry``
        ``1 -`` the equal-weight mean of normalized height spread inside
        12..32 concentric rings around the height-weighted centroid.  A
        perfect concentric disc is near 1; a natural irregular mountain range
        is lower.  Lower is better.  ``radial_ring_spread`` is the raw
        un-inverted spread for diagnostics.
    ``anisotropy``
        Larger eigenvalue / smaller eigenvalue of the gradient structure
        tensor (border cropped).  Around 1 means isotropic (rings or noise);
        >1.3 means a dominant ridge direction.
    """
    height = _as_height_array(z_m)
    sy, sx = _cell_sizes(cell_size_m)
    smooth = _smooth3(height, passes=1)

    gy, gx = np.gradient(smooth, sy, sx, edge_order=1)
    gradient = np.hypot(gx, gy)
    slope_deg = np.degrees(np.arctan(gradient))
    laplacian = _laplacian(smooth, sy, sx)

    min_height = float(height.min())
    max_height = float(height.max())
    mean_height = float(height.mean())
    relief_m = max_height - min_height

    curvature_scale = max(1.0e-15, _percentile(np.abs(laplacian), 95.0))
    steep_scale = max(1.0e-15, _percentile(gradient, 60.0))
    ridge_mask = (laplacian < -0.20 * curvature_scale) & (gradient > steep_scale)
    valley_mask = (laplacian > 0.20 * curvature_scale) & (gradient > steep_scale)

    drainage_km, drainage_all_km, components, kept_components, valley_pixels = (
        _drainage_network(smooth, sy, sx, relief_m)
    )
    radial_symmetry, rings, radial_fraction = _radial_symmetry(
        height, sy, sx, relief_m
    )
    anisotropy = _anisotropy(gy, gx)

    slope_p50 = _percentile(slope_deg, 50.0)
    slope_p90 = _percentile(slope_deg, 90.0)
    slope_p99 = _percentile(slope_deg, 99.0)
    return {
        "metric_version": METRIC_VERSION,
        "grid_shape": [int(height.shape[0]), int(height.shape[1])],
        "cell_size_m": [float(sy), float(sx)],
        "min_height_m": min_height,
        "max_height_m": max_height,
        "mean_height_m": mean_height,
        "p05_height_m": _percentile(height, 5.0),
        "p95_height_m": _percentile(height, 95.0),
        "sea_level_m": float(sea_level_m),
        "sea_fraction": float(np.mean(height <= float(sea_level_m))),
        "relief_m": float(relief_m),
        "slope_deg": {
            "p50": slope_p50,
            "p90": slope_p90,
            "p99": slope_p99,
        },
        "slope_deg_p50": slope_p50,
        "slope_deg_p90": slope_p90,
        "slope_deg_p99": slope_p99,
        "slope_mean": float(slope_deg.mean()),
        "ridge_density": float(ridge_mask.mean()),
        "valley_density": float(valley_mask.mean()),
        "drainage_length_km": float(drainage_km),
        "drainage_length_all_km": float(drainage_all_km),
        "drainage_component_count": int(components),
        "drainage_kept_component_count": int(kept_components),
        "drainage_valley_pixels": int(valley_pixels),
        "radial_symmetry": float(radial_symmetry),
        "radial_ring_spread": float(1.0 - radial_symmetry),
        "radial_symmetry_rings": int(rings),
        "radial_symmetry_pixel_fraction": float(radial_fraction),
        "anisotropy": float(anisotropy),
    }
