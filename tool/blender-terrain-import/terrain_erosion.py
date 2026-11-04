"""Depression-filling, flow-accumulation and stream-power terrain erosion.

Pure numpy/standard-library implementation.  It deliberately does not import
``bpy``, does not call into scipy, and does not vendor code from another
project.  The module is designed to post-process the 16-bit heightmaps produced
by ``build_pegasus_gn.py`` at their real metre scale (a 4 km terrain at 1024
texels is ~3.9 m per texel).

The erosion model is intentionally different from droplet simulation: droplets
at 3.9 m/texel degrade into high-frequency gravel noise instead of dendritic
valleys.  Here water is routed with multiple-flow-direction accumulation and
the incision follows the stream-power law ``E = K * A^m * S^n``.  That couples
every channel to the drainage area above it, so trunk valleys deepen faster
than hillslopes and a branching network can emerge.

CLI:

    python terrain_erosion.py \
        --input heightmap16.png --output eroded16.png --flow flow16.png \
        --size-m 4000 --height-m 800 --iterations 12 --thermal 6 \
        --report report.json

``--size-m`` is the terrain extent, ``--height-m`` maps 65535 to metres.  The
heightmap is row-major with row 0 at the top (the VulkanLearn convention).
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np

try:  # Script execution (tool/blender-terrain-import is not a package).
    from png16_io import read_gray16, write_gray16
except ImportError:  # pragma: no cover - only used when imported as a module.
    from .png16_io import read_gray16, write_gray16


# Quinn (1991) multiple-flow-direction exponent.  p > 1 concentrates more flow
# into the steepest receiver; 1.1 keeps the fan-out while still producing
# visible channels.
_MFD_EXPONENT = 1.1

# Priority-flood epsilon.  Every depression is tilted by at least this much per
# step so that flow routing always has a downstream direction.  At 1e-3 m and a
# 4 km terrain a very long flat basin (1000 cells) gains at most ~1 m of fill.
_FILL_EPSILON_M = 1e-3

# Stream-power area normalization.  ``cell`` uses A / cell_area, i.e. the number
# of upstream texels.  For a 1024^2 grid this spans 1..~1e6, so sqrt(Amax)~1000
# and the default k=0.0035 produces roughly metre-scale incision on steep trunk
# channels while ridge cells (A=1 texel) stay almost untouched.  This is the
# "A / cell_size^2" form.  ``median`` and ``log`` are calmer alternatives:
# - median: A / median(A), robust when most of the map drains to one corner
# - log:    log1p(A/cell_area) / log1p(max(A/cell_area)), bounded to 0..1
AREA_NORMALIZATION_MODE = "cell"
AREA_REFERENCE_SCALE = 1.0

# Light 3x3 binomial smoothing blended after each incision pass.  The kernel is
# normalized with edge replication, so it conserves the mean height.
_SMOOTH_WEIGHT = 0.12

_DIRS8: tuple[tuple[int, int, float], ...] = (
    (-1, -1, math.sqrt(2.0)),
    (-1, 0, 1.0),
    (-1, 1, math.sqrt(2.0)),
    (0, -1, 1.0),
    (0, 1, 1.0),
    (1, -1, math.sqrt(2.0)),
    (1, 0, 1.0),
    (1, 1, math.sqrt(2.0)),
)


@dataclass
class ErosionParams:
    fill_epsilon: float = 1e-3  # metres
    iterations: int = 12  # stream-power passes
    k: float = 0.0035  # dimensionless erosion coefficient
    m: float = 0.5  # drainage-area exponent
    n: float = 1.0  # slope exponent
    dt: float = 1.0
    thermal_iterations: int = 6
    talus_deg: float = 36.0
    thermal_rate: float = 0.5
    upland_weight: float = 0.0  # reserved for future uplift/sediment coupling
    # Extra knobs used by ``erode``.  The public signature of the individual
    # functions stays as specified in the task.
    area_normalization: str = AREA_NORMALIZATION_MODE
    area_reference_scale: float = AREA_REFERENCE_SCALE
    smooth_weight: float = _SMOOTH_WEIGHT
    # "implicit" = Braun & Willett 2013 (default); "explicit" = legacy
    # capped explicit incision, kept for A/B comparison.
    method: str = "implicit"
    uplift_m: float = 0.0  # per time step, only used by the implicit solver
    # 侵蚀在低频面上进行，再把高频细节按比例贴回：D8 在几百米尺度的
    # 细节噪声上会碎成短沟，先平滑能让水系连成长树枝。
    pre_smooth_iterations: int = 0
    detail_restore: float = 0.0


def _as_heightfield(z: np.ndarray) -> np.ndarray:
    array = np.asarray(z, dtype=np.float64)
    if array.ndim != 2:
        raise ValueError(f"expected a 2-D height field, got shape {array.shape}")
    if array.shape[0] < 3 or array.shape[1] < 3:
        raise ValueError("height field must be at least 3x3")
    if not np.all(np.isfinite(array)):
        raise ValueError("height field contains NaN or infinity")
    return np.array(array, copy=True)


def fill_depressions(
    z: np.ndarray, cell_size: float, epsilon: float = 1e-3
) -> tuple[np.ndarray, np.ndarray]:
    """Barnes et al. 2014 priority-flood + epsilon depression filling.

    Returns ``(filled, fill_depth)``.  The implementation is O(N log N): every
    cell enters and leaves the heap once.  A padded flat buffer keeps the inner
    loop free of bounds checks.
    """

    array = _as_heightfield(z)
    if cell_size <= 0.0:
        raise ValueError("cell_size must be positive")
    epsilon = float(epsilon)
    if epsilon <= 0.0:
        raise ValueError("epsilon must be positive")

    height, width = array.shape
    padded_w = width + 2
    padded_h = height + 2
    count = padded_w * padded_h

    padded = np.zeros((padded_h, padded_w), dtype=np.float64)
    padded[1:-1, 1:-1] = array

    # Python lists/bytearray are noticeably faster than numpy scalar indexing
    # in this tight loop; the conversion back to numpy is cheap.
    elevations = padded.ravel().tolist()
    filled = elevations.copy()
    visited = bytearray(count)

    for x in range(padded_w):
        visited[x] = 1
        visited[(padded_h - 1) * padded_w + x] = 1
    for y in range(padded_h):
        visited[y * padded_w] = 1
        visited[y * padded_w + padded_w - 1] = 1

    heap: list[tuple[float, int]] = []
    push = heapq.heappush

    def seed(y: int, x: int) -> None:
        index = y * padded_w + x
        if visited[index]:
            return
        visited[index] = 1
        filled[index] = elevations[index]
        push(heap, (elevations[index], index))

    for x in range(1, width + 1):
        seed(1, x)
        seed(height, x)
    for y in range(2, height):
        seed(y, 1)
        seed(y, width)

    offsets = (
        -padded_w - 1,
        -padded_w,
        -padded_w + 1,
        -1,
        1,
        padded_w - 1,
        padded_w,
        padded_w + 1,
    )
    pop = heapq.heappop

    while heap:
        level, index = pop(heap)
        spill = level + epsilon
        for offset in offsets:
            neighbour = index + offset
            if visited[neighbour]:
                continue
            visited[neighbour] = 1
            ground = elevations[neighbour]
            raised = ground if ground >= spill else spill
            filled[neighbour] = raised
            push(heap, (raised, neighbour))

    filled_array = np.asarray(filled, dtype=np.float64).reshape(padded_h, padded_w)
    filled_array = filled_array[1:-1, 1:-1]
    return filled_array, filled_array - array


def _mfd_weights_and_slope(
    filled: np.ndarray, cell_size: float
) -> tuple[list[np.ndarray], np.ndarray, np.ndarray]:
    """MFD weights plus the steepest-descent drop/slope in one padded pass."""

    height, width = filled.shape
    # An infinite wall makes every out-of-bounds neighbour produce a negative
    # drop, so boundary cells never route flow or talus off the grid.  This is
    # important for mass conservation in ``thermal_erode``: edge replication
    # would let a diagonal transfer leave through one axis while remaining
    # inside the other.
    padded = np.pad(filled, 1, mode="constant", constant_values=np.inf)
    powered: list[np.ndarray] = []
    best_slope = np.zeros((height, width), dtype=np.float64)
    best_drop = np.zeros((height, width), dtype=np.float64)
    for dy, dx, distance in _DIRS8:
        neighbour = padded[
            1 + dy : 1 + dy + height, 1 + dx : 1 + dx + width
        ]
        drop = filled - neighbour
        slope = drop / (cell_size * distance)
        steeper = slope > best_slope
        best_slope[steeper] = slope[steeper]
        best_drop[steeper] = drop[steeper]
        np.maximum(slope, 0.0, out=slope)
        powered.append(np.power(slope, _MFD_EXPONENT))

    total = np.zeros((height, width), dtype=np.float64)
    for array in powered:
        total += array

    weights: list[np.ndarray] = []
    for array in powered:
        weight = np.zeros((height, width), dtype=np.float64)
        np.divide(array, total, out=weight, where=total > 0.0)
        weights.append(weight)

    np.maximum(best_slope, 0.0, out=best_slope)
    np.maximum(best_drop, 0.0, out=best_drop)
    return weights, best_drop, best_slope


def _d8_weights(filled: np.ndarray, cell_size: float) -> list[np.ndarray]:
    height, width = filled.shape
    padded = np.pad(filled, 1, mode="constant", constant_values=np.inf)
    best_slope = np.zeros((height, width), dtype=np.float64)
    best_direction = np.full((height, width), -1, dtype=np.int8)

    for direction, (dy, dx, distance) in enumerate(_DIRS8):
        neighbour = padded[
            1 + dy : 1 + dy + height, 1 + dx : 1 + dx + width
        ]
        slope = (filled - neighbour) / (cell_size * distance)
        steeper = slope > best_slope
        best_slope[steeper] = slope[steeper]
        best_direction[steeper] = direction

    weights: list[np.ndarray] = []
    for direction in range(8):
        weight = (
            (best_direction == direction) & (best_slope > 0.0)
        ).astype(np.float64)
        weights.append(weight)
    return weights


def _accumulate_from_weights(
    filled: np.ndarray, cell_size: float, weights: list[np.ndarray]
) -> np.ndarray:
    """Accumulate drainage area from precomputed receiver weights.

    The routed graph is a DAG because every edge goes to a strictly lower
    filled cell.  Instead of a Python loop over every cell, cells are peeled in
    topological layers: a cell enters the frontier once all of its donors have
    been processed.  Each layer is one vectorized scatter, which keeps the
    per-cell Python overhead out of the hot path.
    """

    height, width = filled.shape
    count = height * width
    index_grid = np.arange(count, dtype=np.int64).reshape(height, width)
    receivers: list[np.ndarray] = []
    for direction, (dy, dx, _) in enumerate(_DIRS8):
        receiver = index_grid + dy * width + dx
        receivers.append(receiver.ravel())

    weight_flat = [weight.ravel() for weight in weights]

    # Number of incoming edges per cell.  A donor with a positive weight
    # contributes exactly one edge, so bincount over receiver indices is the
    # complete in-degree ledger.
    in_degree = np.zeros(count, dtype=np.int32)
    for direction in range(8):
        weight = weight_flat[direction]
        has_edge = weight > 0.0
        if not np.any(has_edge):
            continue
        in_degree += np.bincount(
            receivers[direction][has_edge], minlength=count
        ).astype(np.int32)

    accumulation = np.full(count, cell_size * cell_size, dtype=np.float64)
    frontier = np.flatnonzero(in_degree == 0)

    while frontier.size:
        destinations: list[np.ndarray] = []
        contributions: list[np.ndarray] = []
        for direction in range(8):
            weight = weight_flat[direction][frontier]
            has_edge = weight > 0.0
            if not np.any(has_edge):
                continue
            source = frontier[has_edge]
            destinations.append(receivers[direction][source])
            contributions.append(accumulation[source] * weight[has_edge])

        if not destinations:
            break

        target = np.concatenate(destinations)
        np.add.at(accumulation, target, np.concatenate(contributions))
        np.add.at(in_degree, target, -1)
        # Only cells whose in-degree just reached zero can join the next layer.
        # Filtering before unique is cheaper than sorting every destination.
        frontier = np.unique(target[in_degree[target] == 0])

    if np.any(in_degree):
        raise RuntimeError("flow routing graph contains a cycle")

    return accumulation.reshape(height, width)


def _flow_from_filled(
    filled: np.ndarray, cell_size: float, method: str
) -> np.ndarray:
    """Single-pass accumulation on an already-filled surface."""

    if method == "mfd":
        weights, _, _ = _mfd_weights_and_slope(filled, cell_size)
    elif method == "d8":
        weights = _d8_weights(filled, cell_size)
    else:
        raise ValueError(f"unknown flow method {method!r}; expected 'mfd' or 'd8'")
    return _accumulate_from_weights(filled, cell_size, weights)


def flow_accumulation(
    z: np.ndarray, cell_size: float, method: str = "mfd"
) -> np.ndarray:
    """Drainage area in m^2 for every cell.

    Depressions are filled first (epsilon = 1e-3 m), otherwise sinks would trap
    all upstream area.  ``mfd`` is Quinn (1991) multiple flow direction with
    slope exponent p=1.1; ``d8`` routes everything to the steepest neighbour.
    """

    array = _as_heightfield(z)
    if cell_size <= 0.0:
        raise ValueError("cell_size must be positive")
    filled, _ = fill_depressions(array, cell_size, _FILL_EPSILON_M)
    return _flow_from_filled(filled, cell_size, method)


def _normalize_area(
    area: np.ndarray, cell_size: float, mode: str, scale: float
) -> np.ndarray:
    cell_area = cell_size * cell_size
    if mode == "cell":
        reference = cell_area
        normalized = area / reference
    elif mode == "median":
        reference = max(float(np.median(area)), cell_area)
        normalized = area / reference
    elif mode == "log":
        ratio = area / cell_area
        maximum = float(ratio.max())
        denominator = math.log1p(maximum) if maximum > 0.0 else 1.0
        normalized = np.log1p(ratio) / denominator
    else:
        raise ValueError(
            f"unknown area normalization {mode!r}; expected 'cell', 'median' or 'log'"
        )
    if scale <= 0.0:
        raise ValueError("area_reference_scale must be positive")
    return normalized * scale


def _smooth3x3(z: np.ndarray, weight: float) -> np.ndarray:
    if weight <= 0.0:
        return z
    weight = min(weight, 1.0)
    padded = np.pad(z, 1, mode="edge")
    blurred = (
        4.0 * padded[1:-1, 1:-1]
        + 2.0
        * (
            padded[:-2, 1:-1]
            + padded[2:, 1:-1]
            + padded[1:-1, :-2]
            + padded[1:-1, 2:]
        )
        + (
            padded[:-2, :-2]
            + padded[:-2, 2:]
            + padded[2:, :-2]
            + padded[2:, 2:]
        )
    ) / 16.0
    return z * (1.0 - weight) + blurred * weight


def _stream_power_impl(
    z: np.ndarray,
    cell_size: float,
    iterations: int,
    k: float,
    m: float,
    n: float,
    dt: float,
    fill_epsilon: float,
    area_mode: str,
    area_scale: float,
    smooth_weight: float,
) -> np.ndarray:
    if iterations < 0:
        raise ValueError("iterations must be non-negative")
    if cell_size <= 0.0:
        raise ValueError("cell_size must be positive")

    eroded = _as_heightfield(z)
    for _ in range(int(iterations)):
        filled, _ = fill_depressions(eroded, cell_size, fill_epsilon)
        weights, drop, slope = _mfd_weights_and_slope(filled, cell_size)
        area = _accumulate_from_weights(filled, cell_size, weights)

        normalized_area = _normalize_area(area, cell_size, area_mode, area_scale)
        incision = (
            k
            * np.power(normalized_area, m)
            * np.power(slope, n)
            * float(dt)
        )
        # Never cut more than half of the drop to the downstream cell.  Without
        # this cap a high-drainage cell can punch through the valley floor in a
        # single pass; the remaining relief is removed geometrically over the
        # following iterations.
        np.minimum(incision, 0.5 * drop, out=incision)
        np.maximum(incision, 0.0, out=incision)
        eroded -= incision
        eroded = _smooth3x3(eroded, smooth_weight)
    return eroded


def stream_power_erode(
    z,
    cell_size,
    iterations,
    k,
    m,
    n,
    dt=1.0,
) -> np.ndarray:
    """Iterated fill -> MFD accumulation -> stream-power incision.

    Area is normalized by ``cell_size^2`` (see ``AREA_NORMALIZATION_MODE``).
    """

    return _stream_power_impl(
        z,
        cell_size,
        iterations,
        k,
        m,
        n,
        dt,
        _FILL_EPSILON_M,
        AREA_NORMALIZATION_MODE,
        AREA_REFERENCE_SCALE,
        _SMOOTH_WEIGHT,
    )


def _d8_receivers(
    filled: np.ndarray, cell_size: float
) -> tuple[np.ndarray, np.ndarray]:
    """Steepest-descent receiver index and flow distance for every cell."""

    height, width = filled.shape
    count = height * width
    weights = _d8_weights(filled, cell_size)
    index_grid = np.arange(count, dtype=np.int64).reshape(height, width)

    receiver = np.full(count, -1, dtype=np.int64)
    distance = np.full(count, cell_size, dtype=np.float64)
    for direction, (dy, dx, step) in enumerate(_DIRS8):
        mask = weights[direction] > 0.0
        if not np.any(mask):
            continue
        flat_mask = mask.ravel()
        receiver[flat_mask] = (index_grid + dy * width + dx)[mask]
        distance[flat_mask] = cell_size * step
    return receiver, distance


def _topological_levels(receiver: np.ndarray, filled: np.ndarray) -> np.ndarray:
    """Level = number of flow steps to the outlet.

    A D8 receiver is always strictly lower in the filled surface, so visiting
    cells from high to low guarantees the receiver is already resolved.
    """

    order = np.argsort(-filled.ravel(), kind="stable")
    level = np.zeros(filled.size, dtype=np.int32)
    for index in order:
        target = receiver[index]
        if target >= 0:
            level[index] = level[target] + 1
    return level


def _solve_implicit_network(
    surface: np.ndarray,
    area: np.ndarray,
    receiver: np.ndarray,
    distance: np.ndarray,
    level: np.ndarray,
    k: float,
    m: float,
    n: float,
    dt: float,
    uplift: float,
) -> np.ndarray:
    """One implicit solve on a fixed drainage network."""

    previous = surface.ravel().copy()
    solved = previous.copy()
    area_flat = area.ravel()
    receiver_flat = receiver.ravel()
    distance_flat = np.maximum(distance.ravel(), 1.0e-6)
    lifted = previous + uplift * dt

    order = np.argsort(level, kind="stable")
    counts = np.bincount(level)
    offsets = np.concatenate(([0], np.cumsum(counts)))

    for current in range(1, counts.size):
        begin = offsets[current]
        end = offsets[current + 1]
        if begin == end:
            continue
        index = order[begin:end]
        target = receiver_flat[index]
        valid = target >= 0
        if not np.all(valid):
            index = index[valid]
            target = target[valid]
            if index.size == 0:
                continue
        downstream = solved[target]
        coefficient = (
            k
            * dt
            * np.power(np.maximum(area_flat[index], 1.0), m)
            / distance_flat[index]
        )
        if abs(n - 1.0) < 1.0e-9:
            solved[index] = (lifted[index] + coefficient * downstream) / (
                1.0 + coefficient
            )
            continue

        value = solved[index].copy()
        source = lifted[index]
        for _ in range(6):
            drop = np.maximum(value - downstream, 1.0e-6)
            slope = drop / distance_flat[index]
            residual = source - value + coefficient * drop * np.power(slope, n - 1.0)
            derivative = 1.0 + n * coefficient * np.power(slope, n - 1.0)
            value = value + residual / derivative
            value = np.maximum(value, downstream)
        solved[index] = value

    return solved.reshape(surface.shape)


def implicit_stream_power_erode(
    z: np.ndarray,
    cell_size: float,
    iterations: int = 8,
    k: float = 0.004,
    m: float = 0.5,
    n: float = 1.0,
    dt: float = 1.0,
    uplift: float = 0.0,
    fill_epsilon: float = _FILL_EPSILON_M,
) -> np.ndarray:
    """Braun & Willett (2013) implicit stream-power incision.

    ``dh/dt = U - K A^m S^n`` is solved implicitly along the D8 network: each
    node depends only on its already-solved downstream node, so one topological
    pass replaces explicit time stepping and removes the ``0.5 * drop`` cap
    that kept the explicit solver from cutting real valleys.  ``n == 1`` is
    linear; other exponents use Newton steps vectorized per flow level.
    """

    if iterations < 0:
        raise ValueError("iterations must be non-negative")
    if cell_size <= 0.0:
        raise ValueError("cell_size must be positive")

    surface = _as_heightfield(z)
    for _ in range(int(iterations)):
        filled, _ = fill_depressions(surface, cell_size, fill_epsilon)
        receiver, distance = _d8_receivers(filled, cell_size)
        area = _flow_from_filled(filled, cell_size, "d8")
        level = _topological_levels(receiver, filled)
        surface = _solve_implicit_network(
            surface, area, receiver, distance, level, k, m, n, dt, uplift
        )
    return surface


def thermal_erode(
    z,
    cell_size,
    iterations,
    talus_deg=36.0,
    rate=0.5,
) -> np.ndarray:
    """Eight-neighbour thermal (talus) relaxation with synchronous transfer.

    For every pair the unstable height is ``drop - tan(talus) * distance``.
    Each cell moves at most ``rate * max_excess`` per iteration, distributed
    proportionally over its unstable neighbours.  Transfers are then applied
    simultaneously (inflow - outflow), so total mass is conserved.
    """

    if iterations < 0:
        raise ValueError("iterations must be non-negative")
    if cell_size <= 0.0:
        raise ValueError("cell_size must be positive")
    if not 0.0 < float(rate) <= 1.0:
        raise ValueError("rate must be in (0, 1]")

    eroded = _as_heightfield(z)
    talus_slope = math.tan(math.radians(float(talus_deg)))
    height, width = eroded.shape

    for _ in range(int(iterations)):
        padded = np.pad(eroded, 1, mode="constant", constant_values=np.inf)
        candidates: list[np.ndarray] = []
        for dy, dx, distance in _DIRS8:
            neighbour = padded[
                1 + dy : 1 + dy + height, 1 + dx : 1 + dx + width
            ]
            excess = (eroded - neighbour) - talus_slope * cell_size * distance
            np.maximum(excess, 0.0, out=excess)
            candidates.append(excess)

        total_excess = np.zeros((height, width), dtype=np.float64)
        max_excess = np.zeros((height, width), dtype=np.float64)
        for excess in candidates:
            total_excess += excess
            np.maximum(max_excess, excess, out=max_excess)

        scale = np.zeros((height, width), dtype=np.float64)
        unstable = total_excess > 0.0
        scale[unstable] = (
            float(rate) * max_excess[unstable] / total_excess[unstable]
        )

        transfers = [excess * scale for excess in candidates]
        outflow = np.zeros((height, width), dtype=np.float64)
        for transfer in transfers:
            outflow += transfer

        inflow = np.zeros((height, width), dtype=np.float64)
        for transfer, (dy, dx, _) in zip(transfers, _DIRS8):
            source_rows = slice(1, None) if dy < 0 else slice(None, -1) if dy > 0 else slice(None)
            target_rows = slice(None, -1) if dy < 0 else slice(1, None) if dy > 0 else slice(None)
            source_cols = slice(1, None) if dx < 0 else slice(None, -1) if dx > 0 else slice(None)
            target_cols = slice(None, -1) if dx < 0 else slice(1, None) if dx > 0 else slice(None)
            inflow[target_rows, target_cols] += transfer[source_rows, source_cols]

        eroded = eroded + inflow - outflow

    return eroded


def erode(
    z: np.ndarray,
    cell_size: float,
    params: ErosionParams | None = None,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Stream-power incision followed by thermal relaxation.

    Returns ``(z_eroded, flow_area_m2, fill_depth_m)``.  The returned flow area
    and fill depth describe the final surface.
    """

    settings = params if params is not None else ErosionParams()
    array = _as_heightfield(z)
    if cell_size <= 0.0:
        raise ValueError("cell_size must be positive")

    if settings.method == "implicit":
        # 侵蚀在低频面上进行，避免 D8 被高频细节主导成碎沟。
        base = array
        residual = None
        if settings.pre_smooth_iterations > 0:
            for _ in range(settings.pre_smooth_iterations):
                base = _smooth3x3(base, 0.5)
            residual = array - base
        eroded = implicit_stream_power_erode(
            base,
            cell_size,
            settings.iterations,
            settings.k,
            settings.m,
            settings.n,
            settings.dt,
            settings.uplift_m,
            settings.fill_epsilon,
        )
        if residual is not None and settings.detail_restore > 0.0:
            eroded = eroded + residual * settings.detail_restore
    elif settings.method == "explicit":
        eroded = _stream_power_impl(
            array,
            cell_size,
            settings.iterations,
            settings.k,
            settings.m,
            settings.n,
            settings.dt,
            settings.fill_epsilon,
            settings.area_normalization,
            settings.area_reference_scale,
            settings.smooth_weight,
        )
    else:
        raise ValueError(
            f"unknown erosion method {settings.method!r}; expected 'implicit' or 'explicit'"
        )
    eroded = thermal_erode(
        eroded,
        cell_size,
        settings.thermal_iterations,
        settings.talus_deg,
        settings.thermal_rate,
    )

    filled, fill_depth = fill_depressions(eroded, cell_size, settings.fill_epsilon)
    flow_area = _flow_from_filled(filled, cell_size, "mfd")
    return eroded, flow_area, fill_depth


def _mean_slope_degrees(z: np.ndarray, cell_size: float) -> float:
    dy, dx = np.gradient(z, cell_size, cell_size)
    return float(np.degrees(np.arctan(np.hypot(dx, dy))).mean())


def _count_pit_violations(filled: np.ndarray, epsilon: float) -> int:
    height, width = filled.shape
    interior = filled[1:-1, 1:-1]
    minimum = np.full_like(interior, np.inf)
    for dy, dx, _ in _DIRS8:
        minimum = np.minimum(
            minimum, filled[1 + dy : 1 + dy + height - 2, 1 + dx : 1 + dx + width - 2]
        )
    return int(np.count_nonzero((interior - minimum) < epsilon * 0.5))


def _self_test(verbose: bool = True) -> dict[str, float]:
    """Synthetic checks: depression filling, mass conservation and incision."""

    resolution = 96
    cell_size = 40.0  # 96 * 40 m = 3.84 km
    coordinates = np.arange(resolution, dtype=np.float64) * cell_size
    xx, yy = np.meshgrid(coordinates, coordinates)
    centre = 0.5 * (resolution - 1) * cell_size
    radius = np.hypot(xx - centre, yy - centre)
    cone = 140.0 * np.clip(1.0 - radius / (0.42 * resolution * cell_size), 0.0, 1.0) ** 1.25

    rng = np.random.default_rng(20261104)
    cone += rng.normal(0.0, 0.35, cone.shape)
    cone[40:46, 40:46] -= 25.0  # carve a pit that priority-flood must fill

    filled, fill_depth = fill_depressions(cone, cell_size, 1e-3)
    assert fill_depth.min() >= 0.0, "fill depth must never be negative"
    pit_violations = _count_pit_violations(filled, 1e-3)
    assert pit_violations == 0, f"filled surface still has {pit_violations} pits"
    assert fill_depth.max() > 10.0, "expected the synthetic pit to be filled"

    mass_before = float(cone.sum())
    thermally_relaxed = thermal_erode(cone, cell_size, 6, 36.0, 0.5)
    mass_error = abs(float(thermally_relaxed.sum()) - mass_before) / mass_before
    assert mass_error < 1e-6, f"thermal mass error {mass_error:.3e}"

    eroded, flow_area, final_fill = erode(
        cone,
        cell_size,
        ErosionParams(iterations=8, thermal_iterations=0),
    )
    incision = cone - eroded
    total_drop = float(incision.sum())
    max_incision = float(incision.max())
    assert total_drop > 0.0, "stream power did not lower the terrain"
    assert max_incision > 0.02, f"incision too small: {max_incision}"
    assert final_fill.min() >= -1e-9, "final fill depth is negative"

    flow_median = float(np.median(flow_area))
    flow_p99 = float(np.percentile(flow_area, 99))
    concentration = flow_p99 / max(flow_median, 1e-9)
    assert concentration > 8.0, (
        f"flow is not concentrated enough (p99/median={concentration:.2f})"
    )

    # A foot ring should show azimuthal variation once valleys form.  Measure
    # the standard deviation of incision at mid-radius.
    foot = (radius > 18 * cell_size) & (radius < 26 * cell_size)
    foot_variation = float(incision[foot].std())
    assert foot_variation > 1e-4, (
        f"no gullies at the foot ring (incision std={foot_variation:.4e})"
    )

    result = {
        "pit_violations": float(pit_violations),
        "max_fill_depth_m": float(fill_depth.max()),
        "thermal_mass_relative_error": mass_error,
        "total_incision_m": total_drop,
        "max_incision_m": max_incision,
        "flow_p99_over_median": concentration,
        "foot_incision_std_m": foot_variation,
    }
    if verbose:
        print("self-test: PASS")
        for name, value in result.items():
            print(f"  {name}: {value:.6g}")
    return result


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pure-numpy stream-power terrain erosion for PNG16 heightmaps."
    )
    parser.add_argument("--input", type=Path, help="input 16-bit heightmap PNG")
    parser.add_argument("--output", type=Path, help="output eroded 16-bit PNG")
    parser.add_argument("--flow", type=Path, help="output flow-accumulation PNG")
    parser.add_argument("--report", type=Path, help="optional JSON report")
    parser.add_argument("--size-m", type=float, default=4000.0)
    parser.add_argument("--height-m", type=float, default=800.0)
    parser.add_argument("--iterations", type=int, default=12)
    parser.add_argument("--thermal", type=int, default=6)
    parser.add_argument("--k", type=float, default=0.0035)
    parser.add_argument("--m", type=float, default=0.5)
    parser.add_argument("--n", type=float, default=1.0)
    parser.add_argument("--dt", type=float, default=1.0)
    parser.add_argument("--talus-deg", type=float, default=36.0)
    parser.add_argument("--thermal-rate", type=float, default=0.5)
    parser.add_argument("--method", choices=("implicit", "explicit"), default="implicit")
    parser.add_argument("--uplift", type=float, default=0.0,
                        help="uplift in metres per step (implicit solver only)")
    parser.add_argument("--pre-smooth", type=int, default=0,
                        help="binomial smoothing passes before erosion")
    parser.add_argument("--detail-restore", type=float, default=0.0,
                        help="fraction of the removed high-frequency detail added back")
    parser.add_argument(
        "--area-normalization",
        choices=("cell", "median", "log"),
        default=AREA_NORMALIZATION_MODE,
    )
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    if args.self_test:
        _self_test()
        return 0
    if args.input is None or args.output is None or args.flow is None:
        print("error: --input, --output and --flow are required", file=sys.stderr)
        return 2

    started = time.perf_counter()
    quantized = read_gray16(args.input)
    height, width = quantized.shape
    if args.size_m <= 0.0:
        raise ValueError("--size-m must be positive")
    if args.height_m <= 0.0:
        raise ValueError("--height-m must be positive")
    cell_size = args.size_m / float(max(width - 1, 1))
    heights = quantized.astype(np.float64) / 65535.0 * args.height_m
    decode_seconds = time.perf_counter() - started

    params = ErosionParams(
        iterations=args.iterations,
        k=args.k,
        m=args.m,
        n=args.n,
        dt=args.dt,
        thermal_iterations=args.thermal,
        talus_deg=args.talus_deg,
        thermal_rate=args.thermal_rate,
        area_normalization=args.area_normalization,
        method=args.method,
        uplift_m=args.uplift,
        pre_smooth_iterations=args.pre_smooth,
        detail_restore=args.detail_restore,
    )

    stream_started = time.perf_counter()
    if params.method == "implicit":
        base = heights
        residual = None
        if params.pre_smooth_iterations > 0:
            for _ in range(params.pre_smooth_iterations):
                base = _smooth3x3(base, 0.5)
            residual = heights - base
        stream_power = implicit_stream_power_erode(
            base,
            cell_size,
            params.iterations,
            params.k,
            params.m,
            params.n,
            params.dt,
            params.uplift_m,
            params.fill_epsilon,
        )
        if residual is not None and params.detail_restore > 0.0:
            stream_power = stream_power + residual * params.detail_restore
    else:
        stream_power = _stream_power_impl(
            heights,
            cell_size,
            params.iterations,
            params.k,
            params.m,
            params.n,
            params.dt,
            params.fill_epsilon,
            params.area_normalization,
            params.area_reference_scale,
            params.smooth_weight,
        )
    stream_seconds = time.perf_counter() - stream_started

    thermal_started = time.perf_counter()
    eroded = thermal_erode(
        stream_power,
        cell_size,
        params.thermal_iterations,
        params.talus_deg,
        params.thermal_rate,
    )
    thermal_seconds = time.perf_counter() - thermal_started

    analysis_started = time.perf_counter()
    filled, fill_depth = fill_depressions(eroded, cell_size, params.fill_epsilon)
    flow_area = _flow_from_filled(filled, cell_size, "mfd")
    analysis_seconds = time.perf_counter() - analysis_started

    max_flow = float(flow_area.max())
    cell_area = cell_size * cell_size
    if max_flow > 0.0:
        flow_visual = np.log1p(flow_area / cell_area) / math.log1p(max_flow / cell_area)
    else:
        flow_visual = np.zeros_like(flow_area)

    write_started = time.perf_counter()
    write_gray16(args.output, eroded / args.height_m * 65535.0)
    write_gray16(args.flow, np.clip(flow_visual, 0.0, 1.0) * 65535.0)
    write_seconds = time.perf_counter() - write_started

    incision = heights - eroded
    slope_before = _mean_slope_degrees(heights, cell_size)
    slope_after = _mean_slope_degrees(eroded, cell_size)
    percentiles = np.percentile(flow_area, [50.0, 90.0, 99.0, 100.0])
    report = {
        "input": str(args.input),
        "output": str(args.output),
        "flow_output": str(args.flow),
        "grid": {"width": width, "height": height, "count": int(width * height)},
        "size_m": args.size_m,
        "height_m": args.height_m,
        "cell_size_m": cell_size,
        "cell_area_m2": cell_area,
        "parameters": asdict(params),
        "timing_seconds": {
            "decode": decode_seconds,
            "stream_power": stream_seconds,
            "thermal": thermal_seconds,
            "analysis_fill_and_flow": analysis_seconds,
            "write": write_seconds,
            "total": time.perf_counter() - started,
        },
        "fill_volume_m3": float(fill_depth.sum()) * cell_area,
        "max_fill_depth_m": float(fill_depth.max()),
        "remaining_pit_violations": _count_pit_violations(
            filled, params.fill_epsilon
        ),
        "max_incision_m": float(max(incision.max(), 0.0)),
        "mean_incision_m": float(incision[incision > 0.0].mean())
        if np.any(incision > 0.0)
        else 0.0,
        "total_incision_volume_m3": float(incision.sum()) * cell_area,
        "flow_area_m2": {
            "p50": float(percentiles[0]),
            "p90": float(percentiles[1]),
            "p99": float(percentiles[2]),
            "max": float(percentiles[3]),
        },
        "mean_slope_deg_before": slope_before,
        "mean_slope_deg_after": slope_after,
        "mean_slope_delta_deg": slope_after - slope_before,
        "height_min_after_m": float(eroded.min()),
        "height_max_after_m": float(eroded.max()),
        "flow_visualization": "log1p(area/cell_area) normalized to 0..65535",
    }

    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8"
        )

    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
