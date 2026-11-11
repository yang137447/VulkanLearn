"""Simplified PE_heightfield_paths / Linescape river pass.

The Houdini HDA connects authored waypoints with ``findshortestpath`` over the
heightfield, projects the path, cuts the river bed and blurs the banks.  This
script keeps that data flow for the offline Pegasus pipeline:

    heightfield -> waypoint path search -> projected corridor
                -> carve height -> river mask

The path is deterministic and may optionally be supplied as a JSON waypoint
list in metres: ``[[x, z], ...]``.  Without it, a high-to-low route across the
map is chosen by a terrain cost search.
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
from png16_io import read_gray16, write_gray16  # noqa: E402


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--heightmap", type=Path, required=True)
    parser.add_argument("--meta", type=Path, required=True,
                        help="heightmap_shape.json with low/high in metres")
    parser.add_argument("--output-heightmap", type=Path, required=True)
    parser.add_argument("--output-meta", type=Path, required=True)
    parser.add_argument("--output-mask", type=Path, required=True)
    parser.add_argument("--output-path", type=Path, required=True)
    parser.add_argument("--size-m", type=float, default=4000.0)
    parser.add_argument("--width-m", type=float, default=110.0)
    parser.add_argument(
        "--water-width-m", type=float, default=None,
        help="水面核心宽度；省略时取 width-m × 0.55")
    parser.add_argument("--depth-m", type=float, default=6.0)
    parser.add_argument("--bank-m", type=float, default=160.0)
    parser.add_argument("--search-resolution", type=int, default=512)
    parser.add_argument(
        "--meander-amplitude-m", type=float, default=260.0,
        help="自动主河道的蜿蜒振幅")
    parser.add_argument("--meander-waves", type=float, default=2.2,
                        help="主河道蜿蜒波数")
    parser.add_argument("--tributary-amplitude-m", type=float, default=180.0)
    parser.add_argument(
        "--branch-count", type=int, default=3,
        help="自动分支数；Houdini HDA 用 foreach 10 次生成多路径河网")
    parser.add_argument(
        "--valley-snap-m", type=float, default=45.0,
        help="中心线投影到谷底时的横向搜索半径；0 关闭投影")
    parser.add_argument("--valley-snap-step-m", type=float, default=5.0)
    parser.add_argument(
        "--smooth-iterations", type=int, default=2,
        help="Chaikin 平滑次数；0 保留栅格最短路径的台阶")
    parser.add_argument(
        "--elevation-weight", type=float, default=9.0,
        help="路径代价里的绝对高度权重；越大越贴谷底")
    parser.add_argument(
        "--uphill-weight", type=float, default=18.0,
        help="逆坡代价权重；越大越不翻山")
    parser.add_argument("--output-water-mask", type=Path, default=None,
                        help="河道核心 mask；省略时写到 output-mask 同目录的 *_water.png")
    parser.add_argument("--waypoints", type=Path, default=None,
                        help="optional [[x,z], ...] waypoint file in metres")
    return parser.parse_args()


def _resize(values: np.ndarray, target: int) -> np.ndarray:
    source = np.asarray(values, dtype=np.float64)
    if source.shape == (target, target):
        return source
    image = Image.fromarray(
        (np.clip(source, 0.0, 1.0) * 65535.0).astype(np.uint16))
    image = image.resize((target, target), Image.Resampling.BILINEAR)
    return np.asarray(image, dtype=np.float64) / 65535.0


def _perpendicular(start: list[float],
                   end: list[float]) -> tuple[float, float]:
    dx, dz = end[0] - start[0], end[1] - start[1]
    length = math.hypot(dx, dz) or 1.0
    return -dz / length, dx / length


def _meander_polyline(start: list[float], end: list[float],
                      amplitude_m: float, waves: float, phase: float,
                      count: int) -> list[list[float]]:
    """Smooth analytic centreline, windowed to exactly hit both endpoints."""
    nx, nz = _perpendicular(start, end)
    points: list[list[float]] = []
    for index in range(count):
        t = index / max(count - 1, 1)
        window = math.sin(math.pi * t)
        offset = amplitude_m * window * math.sin(
            2.0 * math.pi * waves * t + phase)
        points.append([
            start[0] + (end[0] - start[0]) * t + nx * offset,
            start[1] + (end[1] - start[1]) * t + nz * offset,
        ])
    points[0] = list(start)
    points[-1] = list(end)
    return points


def _sample_field(field: np.ndarray, x: float, z: float,
                  size_m: float) -> float:
    side = field.shape[0]
    fx = min(max((x + size_m * 0.5) / size_m * (side - 1), 0.0), side - 1.0)
    fz = min(max((z + size_m * 0.5) / size_m * (side - 1), 0.0), side - 1.0)
    x0, z0 = int(fx), int(fz)
    x1 = min(x0 + 1, side - 1)
    z1 = min(z0 + 1, side - 1)
    ax, az = fx - x0, fz - z0
    top = field[z0, x0] * (1.0 - ax) + field[z0, x1] * ax
    bottom = field[z1, x0] * (1.0 - ax) + field[z1, x1] * ax
    return float(top * (1.0 - az) + bottom * az)


def _segments_cross(a: list[float], b: list[float],
                    c: list[float], d: list[float]) -> bool:
    def orientation(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])

    o1 = orientation(a, b, c)
    o2 = orientation(a, b, d)
    o3 = orientation(c, d, a)
    o4 = orientation(c, d, b)
    return (o1 * o2 < 0.0) and (o3 * o4 < 0.0)


def _path_crosses(path: list[list[float]],
                  host: list[list[float]],
                  stride: int = 3) -> bool:
    """True when ``path`` crosses ``host`` before its final (shared) point."""
    for index in range(0, len(path) - 1 - stride, stride):
        a, b = path[index], path[index + stride]
        for other in range(0, len(host) - 1, stride):
            if _segments_cross(a, b, host[other], host[other + stride]):
                return True
    return False


def _snap_to_valley(points: list[list[float]], height_m: np.ndarray,
                    size_m: float, radius_m: float,
                    step_m: float) -> list[list[float]]:
    """Light-weight ``heightfield_project``: pull the curve into the valley.

    The search stays local (±radius) so the authored meander survives and the
    curve never jumps to an unrelated channel.
    """
    if radius_m <= 0.0 or len(points) < 3:
        return [list(point) for point in points]
    steps = max(1, int(round(radius_m / max(step_m, 1e-6))))
    low, high = float(height_m.min()), float(height_m.max())
    normalized = (height_m - low) / max(high - low, 1e-9)
    half = size_m * 0.5
    result: list[list[float]] = [list(points[0])]
    for index in range(1, len(points) - 1):
        previous, current, following = (
            points[index - 1], points[index], points[index + 1])
        tx = following[0] - previous[0]
        tz = following[1] - previous[1]
        length = math.hypot(tx, tz) or 1.0
        nx, nz = -tz / length, tx / length
        best_cost = math.inf
        best_x, best_z = current
        for k in range(-steps, steps + 1):
            offset = k * step_m
            x = min(max(current[0] + nx * offset, -half), half)
            z = min(max(current[1] + nz * offset, -half), half)
            cost = (_sample_field(normalized, x, z, size_m)
                    + 0.35 * abs(offset) / max(radius_m, 1e-9))
            if cost < best_cost:
                best_cost = cost
                best_x, best_z = x, z
        result.append([best_x, best_z])
    result.append(list(points[-1]))
    return result


def _auto_curves(height_m: np.ndarray, size_m: float,
                 meander_m: float, waves: float,
                 tributary_m: float,
                 branch_count: int) -> list[list[list[float]]]:
    side = height_m.shape[0]
    cell = size_m / max(side - 1, 1)
    band = max(2, side // 4)
    low, high = float(height_m.min()), float(height_m.max())
    west = height_m[:, :band]
    east = height_m[:, -band:]
    start_y, start_x = np.unravel_index(int(np.argmax(west)), west.shape)
    end_y, end_x = np.unravel_index(int(np.argmin(east)), east.shape)
    start = [-size_m * 0.5 + start_x * cell,
             -size_m * 0.5 + start_y * cell]
    end = [-size_m * 0.5 + (side - band + end_x) * cell,
           -size_m * 0.5 + end_y * cell]
    count = max(48, int(size_m / cell / 4.0))
    main = _meander_polyline(start, end, meander_m, waves, 0.6, count)
    # Houdini's paths HDA runs a 10-iteration foreach and keeps every
    # each-to-each path, so the result is a small dendritic network rather
    # than a single river.  Mirror that with one trunk plus N branches that
    # enter from the highest point on the remaining borders.
    bands = (
        ("north", height_m[:band, :], "x", 0, 0),
        ("south", height_m[-band:, :], "x", side - band, 0),
        ("west", height_m[:, :band], "z", 0, 0),
        ("east", height_m[:, -band:], "z", 0, side - band),
    )
    sources: list[tuple[str, float, list[float]]] = []
    for name, values, axis, y_offset, x_offset in bands:
        rows, columns = values.shape
        length = columns if axis == "x" else rows
        third = max(1, length // 3)
        for part in range(3):
            start = part * third
            stop = length if part == 2 else start + third
            sub = (values[:, start:stop] if axis == "x"
                   else values[start:stop, :])
            y, x = np.unravel_index(int(np.argmax(sub)), sub.shape)
            if axis == "x":
                x = x + start
            else:
                y = y + start
            point = [-size_m * 0.5 + (x + x_offset) * cell,
                     -size_m * 0.5 + (y + y_offset) * cell]
            sources.append((name, float(sub.max()), point))
    # Never start a branch on top of the trunk's own outlet/inlet: the branch
    # would run parallel to the trunk and pinch into a sliver island.
    sources = [
        (name, elevation, point) for name, elevation, point in sources
        if min(math.hypot(point[0] - end_point[0], point[1] - end_point[1])
               for end_point in (main[0], main[-1])) > size_m * 0.12
    ]
    branches: list[list[list[float]]] = []
    join_fractions = (0.30, 0.55, 0.78, 0.42, 0.66)
    used: set[str] = set()
    for index in range(max(0, branch_count)):
        join_t = join_fractions[index % len(join_fractions)]
        join = main[int(round(join_t * (len(main) - 1)))]
        # Pick the *nearest* unused border source instead of the highest one:
        # a branch that has to cross the trunk to reach its confluence creates
        # impossible braided loops.  Elevation only breaks ties.
        best = None
        for name, elevation, point in sources:
            if name in used:
                continue
            distance = math.hypot(point[0] - join[0], point[1] - join[1])
            if distance < size_m * 0.28:
                continue
            height_norm = (elevation - low) / max(high - low, 1e-9)
            score = distance / max(size_m, 1e-9) - 0.35 * height_norm
            if best is None or score < best[0]:
                best = (score, name, point)
        if best is None:
            break
        _, name, source = best
        used.add(name)
        span = math.hypot(join[0] - source[0], join[1] - source[1])
        amplitude = min(tributary_m * (1.0 - 0.15 * index), span * 0.22)
        branch_waves = max(0.6, min(2.2, span / 1300.0))
        phase = 1.9 + index * 0.7
        for shrink in (1.0, 0.7, 0.45, 0.25, 0.0):
            candidate = _meander_polyline(
                source, join, amplitude * shrink, branch_waves, phase, count)
            if not _path_crosses(candidate, main):
                break
        branches.append(candidate)
    return [main] + branches


def _nearest_passable(path: list[list[float]], height_m: np.ndarray,
                      size_m: float) -> tuple[int, int]:
    side = height_m.shape[0]
    x, z = path
    gx = int(round((x + size_m * 0.5) / size_m * (side - 1)))
    gz = int(round((z + size_m * 0.5) / size_m * (side - 1)))
    return max(0, min(side - 1, gx)), max(0, min(side - 1, gz))


def _shortest_segment(height_m: np.ndarray, size_m: float,
                      start_point: list[float],
                      goal_point: list[float],
                      elevation_weight: float,
                      uphill_weight: float) -> list[list[float]]:
    side = height_m.shape[0]
    cell = size_m / max(side - 1, 1)
    start_x, start_z = _nearest_passable(start_point, height_m, size_m)
    goal_x, goal_z = _nearest_passable(goal_point, height_m, size_m)
    height_norm = (height_m - height_m.min()) / max(
        height_m.max() - height_m.min(), 1e-9)
    start = (start_z, start_x)
    goal = (goal_z, goal_x)
    distance = {start: 0.0}
    previous: dict[tuple[int, int], tuple[int, int]] = {}
    queue = [(0.0, start)]
    neighbors = ((1, 0), (-1, 0), (0, 1), (0, -1),
                 (1, 1), (1, -1), (-1, 1), (-1, -1))
    while queue:
        _, current = heapq.heappop(queue)
        if current == goal:
            break
        cz, cx = current
        for dz, dx in neighbors:
            nz, nx = cz + dz, cx + dx
            if nz < 0 or nx < 0 or nz >= side or nx >= side:
                continue
            dh = float(height_m[nz, nx] - height_m[cz, cx])
            slope = abs(dh) / (cell * (1.41421356 if dz and dx else 1.0))
            step = (1.41421356 if dz and dx else 1.0)
            # Height plus uphill penalty keeps the route in valleys.  The
            # elevation term dominates on gentle terrain, otherwise the
            # 8-neighbour search just returns a staircase straight line.
            step_cost = step * (0.10 + elevation_weight
                                * float(height_norm[nz, nx])
                                + 2.0 * slope
                                + uphill_weight * max(0.0, dh) / max(cell, 1e-9))
            candidate = distance[current] + step_cost
            neighbor = (nz, nx)
            if candidate < distance.get(neighbor, math.inf):
                distance[neighbor] = candidate
                previous[neighbor] = current
                heapq.heappush(queue, (candidate, neighbor))
    if goal not in distance:
        return [start_point, goal_point]
    path = []
    current = goal
    while True:
        path.append(current)
        if current == start:
            break
        current = previous[current]
    path.reverse()
    return [[-size_m * 0.5 + x * cell, -size_m * 0.5 + z * cell]
            for z, x in path]


def _shortest_path(height_m: np.ndarray, size_m: float,
                   waypoint_m: list[list[float]],
                   elevation_weight: float,
                   uphill_weight: float) -> list[list[float]]:
    result: list[list[float]] = []
    for index in range(len(waypoint_m) - 1):
        segment = _shortest_segment(
            height_m, size_m, waypoint_m[index], waypoint_m[index + 1],
            elevation_weight, uphill_weight)
        if result:
            result.extend(segment[1:])
        else:
            result.extend(segment)
    return result


def _chaikin(points: list[list[float]],
             iterations: int) -> list[list[float]]:
    """Corner-cutting subdivision; removes the 8-neighbour grid staircase."""
    values = [(float(p[0]), float(p[1])) for p in points]
    for _ in range(max(0, iterations)):
        if len(values) < 3:
            break
        smoothed = [values[0]]
        for first, second in zip(values[:-1], values[1:]):
            smoothed.append((
                0.75 * first[0] + 0.25 * second[0],
                0.75 * first[1] + 0.25 * second[1]))
            smoothed.append((
                0.25 * first[0] + 0.75 * second[0],
                0.25 * first[1] + 0.75 * second[1]))
        smoothed.append(values[-1])
        values = smoothed
    return [[x, z] for x, z in values]


def _rasterize(paths: list[list[list[float]]], size_m: float, side: int,
               width_m: float, blur_m: float) -> np.ndarray:
    """Draw the paths at ``width_m`` and blur the edge over ``blur_m``.

    The result is normalized so the channel centre reaches 1.0.  Without the
    normalization a wide blur on a narrow line never gets close to 1 and both
    the carve depth and the shader weight are silently scaled down.
    """
    cell = size_m / max(side - 1, 1)
    image = Image.new("L", (side, side), 0)
    draw = ImageDraw.Draw(image)
    width_px = max(1, int(round(width_m / cell)))
    for path in paths:
        points = [
            ((x + size_m * 0.5) / size_m * (side - 1),
             (z + size_m * 0.5) / size_m * (side - 1))
            for x, z in path]
        if len(points) > 1:
            draw.line(points, fill=255, width=width_px, joint="curve")
    if blur_m > 0.0:
        image = image.filter(
            ImageFilter.GaussianBlur(radius=max(1.0, blur_m / cell)))
    values = np.asarray(image, dtype=np.float64) / 255.0
    peak = float(values.max())
    if peak > 1.0e-6:
        values = values / peak
    return np.clip(values, 0.0, 1.0)


def main() -> None:
    args = _parse_args()
    low, high = json.loads(args.meta.read_text(encoding="utf-8-sig")).values()
    low, high = float(low), float(high)
    raw = read_gray16(args.heightmap).astype(np.float64) / 65535.0
    height_m = low + raw * (high - low)
    search = _resize(
        (height_m - low) / max(high - low, 1e-9),
        args.search_resolution)
    search_m = low + search * (high - low)

    solved: list[list[list[float]]] = []
    if args.waypoints is not None:
        loaded = json.loads(args.waypoints.read_text(encoding="utf-8-sig"))
        waypoints = (loaded if loaded and isinstance(loaded[0][0], list)
                     else [loaded])
        for index, points in enumerate(waypoints):
            if index > 0 and solved:
                # Re-attach tributaries to the *solved* trunk instead of the
                # authored waypoint, otherwise the confluence is a kink.
                host = solved[0]
                end = points[-1]
                nearest = min(
                    host,
                    key=lambda p: (p[0] - end[0]) ** 2 + (p[1] - end[1]) ** 2)
                points = list(points[:-1]) + [nearest]
            solved.append(_shortest_path(
                search_m, args.size_m, points,
                args.elevation_weight, args.uphill_weight))
    else:
        curves = _auto_curves(
            search_m, args.size_m, args.meander_amplitude_m,
            args.meander_waves, args.tributary_amplitude_m,
            args.branch_count)
        trunk = _snap_to_valley(
            curves[0], search_m, args.size_m,
            args.valley_snap_m, args.valley_snap_step_m)
        solved.append(trunk)
        for curve in curves[1:]:
            end = curve[-1]
            nearest = min(
                trunk,
                key=lambda p: (p[0] - end[0]) ** 2 + (p[1] - end[1]) ** 2)
            branch = list(curve[:-1]) + [nearest]
            solved.append(_snap_to_valley(
                branch, search_m, args.size_m,
                args.valley_snap_m, args.valley_snap_step_m))
        waypoints = [[path[0], path[-1]] for path in solved]
    paths = [_chaikin(points, args.smooth_iterations) for points in solved]

    side = height_m.shape[0]
    water_width_m = (args.water_width_m if args.water_width_m is not None
                     else args.width_m * 0.55)
    # Wide, soft corridor = valley carve + bank ring.
    corridor = _rasterize(paths, args.size_m, side, args.width_m,
                          args.bank_m)
    # Narrow core = the water surface itself.
    channel = _rasterize(paths, args.size_m, side, water_width_m,
                         max(8.0, water_width_m * 0.35))
    # Silt/gravel only lives on the banks; keep the wet core for water.
    bank = np.clip(corridor * (1.0 - channel), 0.0, 1.0)

    carved = height_m - args.depth_m * corridor
    out_low, out_high = float(carved.min()), float(carved.max())
    normalized = (carved - out_low) / max(out_high - out_low, 1e-9)
    write_gray16(args.output_heightmap,
                 (np.clip(normalized, 0.0, 1.0) * 65535.0).astype(np.uint16))
    args.output_meta.write_text(
        json.dumps({"low": out_low, "high": out_high}, indent=2) + "\n",
        encoding="utf-8")
    write_gray16(args.output_mask,
                 (bank * 65535.0).astype(np.uint16))
    water_mask = args.output_water_mask
    if water_mask is None:
        water_mask = args.output_mask.with_name(
            args.output_mask.stem + "_water.png")
    write_gray16(water_mask, (channel * 65535.0).astype(np.uint16))
    args.output_path.write_text(
        json.dumps({
            "widthM": args.width_m,
            "waterWidthM": water_width_m,
            "depthM": args.depth_m,
            "bankM": args.bank_m,
            "waypoints": waypoints,
            "paths": paths,
            "path": paths[0],
            "waterMask": str(water_mask),
        }, indent=2) + "\n",
        encoding="utf-8")
    print(f"river paths={len(paths)} points={sum(len(p) for p in paths)} "
          f"bank max={bank.max():.3f} water max={channel.max():.3f} "
          f"height {out_low:.2f}..{out_high:.2f} m")


if __name__ == "__main__":
    main()
