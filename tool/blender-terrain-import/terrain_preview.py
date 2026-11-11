"""Headless Blender preview and metrics for a terrain heightmap.

Run inside Blender:

    blender --background --python terrain_preview.py -- \
        --heightmap heightmap.png --size-m 4000 --height-m 800 \
        --out-dir artifacts\\gn_research\\preview --name v3

The script writes:

* ``<name>_hillshade.png`` -- fixed-light hillshade on a 1280x720 canvas;
* ``<name>_slope.png`` -- green/yellow/red slope ramp;
* ``<name>_3d.png`` -- Cycles CPU render with sun shadows and material AO;
* ``<name>_metrics.json`` -- JSON metrics plus timings;
* optional ``<name>_compare.png`` -- 2x2 grid: 3D baseline/candidate over
  hillshade baseline/candidate.

The heightmap is read through Blender's image API so 16-bit grayscale PNGs
work without Pillow/OpenCV.  Blender exposes image rows bottom-up; this script
flips once to the terrain convention used by VulkanLearn (row 0 at v=0).
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import tempfile
import time
import zlib
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from terrain_metrics import analyze, hillshade  # noqa: E402


PREVIEW_WIDTH = 1280
PREVIEW_HEIGHT = 720
CANVAS_BACKGROUND = np.array((10, 13, 18), dtype=np.uint8)


def _parse_args() -> argparse.Namespace:
    argv = sys.argv
    argv = argv[argv.index("--") + 1:] if "--" in argv else []
    parser = argparse.ArgumentParser(description="Render terrain previews and metrics")
    parser.add_argument("--heightmap", type=Path, required=True)
    parser.add_argument("--size-m", type=float, required=True)
    parser.add_argument("--height-m", type=float, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--name", default="terrain")
    parser.add_argument("--sea-level-m", type=float, default=0.0)
    parser.add_argument(
        "--resolution",
        type=int,
        default=1024,
        help="Maximum grid dimension; the source is never upsampled.",
    )
    parser.add_argument(
        "--compare",
        type=Path,
        default=None,
        help="Optional second heightmap for a 2x2 baseline/candidate comparison.",
    )
    return parser.parse_args(argv)


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


def _write_rgb_png(path: Path, image: np.ndarray) -> None:
    rgb = np.asarray(image, dtype=np.uint8)
    if rgb.ndim != 3 or rgb.shape[2] != 3:
        raise ValueError(f"expected an HxWx3 RGB array, got {rgb.shape}")
    height, width = rgb.shape[:2]
    rows = np.zeros((height, 1 + width * 3), dtype=np.uint8)
    rows[:, 0] = 0  # PNG filter type None
    rows[:, 1:] = rgb.reshape(height, width * 3)
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    payload = (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", header)
        + _png_chunk(b"IDAT", zlib.compress(rows.tobytes(), 6))
        + _png_chunk(b"IEND", b"")
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def _read_rgb_png(path: Path) -> np.ndarray:
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        image.colorspace_settings.name = "Non-Color"
        width, height = image.size
        pixels = np.empty(width * height * 4, dtype=np.float32)
        image.pixels.foreach_get(pixels)
        rgba = pixels.reshape(height, width, 4)[::-1]
        return np.clip(rgba[:, :, :3] * 255.0, 0.0, 255.0).astype(np.uint8)
    finally:
        bpy.data.images.remove(image)


def _load_heightmap(
    path: Path,
    max_resolution: int,
) -> tuple[np.ndarray, tuple[int, int], tuple[int, int]]:
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        image.colorspace_settings.name = "Non-Color"
        source_width, source_height = image.size
        if max_resolution > 0 and max(source_width, source_height) > max_resolution:
            scale = max_resolution / float(max(source_width, source_height))
            target_width = max(2, int(round(source_width * scale)))
            target_height = max(2, int(round(source_height * scale)))
            image.scale(target_width, target_height)
        width, height = image.size
        pixels = np.empty(width * height * 4, dtype=np.float32)
        image.pixels.foreach_get(pixels)
        # Blender image rows are bottom-up; terrain heightmaps are sampled
        # top-down by VulkanLearn and by the rest of this preview tool.
        normalized = pixels.reshape(height, width, 4)[::-1, :, 0].astype(
            np.float64, copy=False
        )
    finally:
        bpy.data.images.remove(image)
    if not np.isfinite(normalized).all():
        raise ValueError(f"heightmap contains non-finite pixels: {path}")
    return normalized, (source_width, source_height), (width, height)


def _resize_bilinear(image: np.ndarray, out_height: int, out_width: int) -> np.ndarray:
    source = np.asarray(image, dtype=np.float32)
    h, w = source.shape[:2]
    if h == out_height and w == out_width:
        return source.copy()
    if h < 1 or w < 1 or out_height < 1 or out_width < 1:
        raise ValueError("image dimensions must be positive")

    ys = np.linspace(0.0, h - 1.0, out_height, dtype=np.float32)
    xs = np.linspace(0.0, w - 1.0, out_width, dtype=np.float32)
    y0 = np.floor(ys).astype(np.int32)
    x0 = np.floor(xs).astype(np.int32)
    y1 = np.minimum(y0 + 1, h - 1)
    x1 = np.minimum(x0 + 1, w - 1)
    wy = (ys - y0)[:, None]
    wx = (xs - x0)[None, :]

    if source.ndim == 2:
        wy = wy[:, 0]
        wx = wx[0, :]
        top = source[y0, :][:, x0] * (1.0 - wx) + source[y0, :][:, x1] * wx
        bottom = source[y1, :][:, x0] * (1.0 - wx) + source[y1, :][:, x1] * wx
        return top * (1.0 - wy)[:, None] + bottom * wy[:, None]

    wy3 = wy[:, :, None]
    wx3 = wx[:, :, None]
    top = source[y0][:, x0] * (1.0 - wx3) + source[y0][:, x1] * wx3
    bottom = source[y1][:, x0] * (1.0 - wx3) + source[y1][:, x1] * wx3
    return top * (1.0 - wy3) + bottom * wy3


def _fit_to_canvas(
    image: np.ndarray,
    out_width: int,
    out_height: int,
    background: np.ndarray = CANVAS_BACKGROUND,
) -> np.ndarray:
    source = np.asarray(image)
    if source.ndim == 2:
        source = np.repeat(source[:, :, None], 3, axis=2)
    if source.ndim != 3 or source.shape[2] != 3:
        raise ValueError(f"expected an HxW or HxWx3 image, got {source.shape}")

    h, w = source.shape[:2]
    scale = min(out_width / float(w), out_height / float(h))
    target_width = max(1, int(round(w * scale)))
    target_height = max(1, int(round(h * scale)))
    resized = _resize_bilinear(source, target_height, target_width)
    resized = np.clip(resized, 0.0, 255.0).astype(np.uint8)

    canvas = np.empty((out_height, out_width, 3), dtype=np.uint8)
    canvas[:] = background
    x0 = (out_width - target_width) // 2
    y0 = (out_height - target_height) // 2
    canvas[y0:y0 + target_height, x0:x0 + target_width] = resized
    return canvas


def _gray_to_rgb(values: np.ndarray) -> np.ndarray:
    gray = np.clip(np.asarray(values) * 255.0, 0.0, 255.0).astype(np.uint8)
    return np.repeat(gray[:, :, None], 3, axis=2)


def _slope_rgb(height_m: np.ndarray, cell_size_m: float) -> np.ndarray:
    gy, gx = np.gradient(height_m, cell_size_m, cell_size_m, edge_order=1)
    slope_deg = np.degrees(np.arctan(np.hypot(gx, gy)))
    stops = (
        (0.0, (36.0, 140.0, 62.0)),
        (25.0, (225.0, 205.0, 70.0)),
        (45.0, (220.0, 82.0, 36.0)),
        (60.0, (150.0, 20.0, 15.0)),
    )
    rgb = np.empty(slope_deg.shape + (3,), dtype=np.float64)
    for channel in range(3):
        rgb[:, :, channel] = np.interp(
            slope_deg,
            [stop[0] for stop in stops],
            [stop[1][channel] for stop in stops],
        )
    # A little relief shading keeps the slope bands readable on smooth areas.
    shade = hillshade(height_m, cell_size_m)
    rgb *= (0.76 + 0.24 * shade)[:, :, None]
    return np.clip(rgb, 0.0, 255.0).astype(np.uint8)


def _reset_scene() -> bpy.types.Scene:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 32
    scene.cycles.use_denoising = True
    scene.cycles.use_adaptive_sampling = True
    scene.cycles.adaptive_threshold = 0.02
    scene.cycles.max_bounces = 4
    scene.cycles.diffuse_bounces = 2
    scene.cycles.glossy_bounces = 2
    scene.cycles.transmission_bounces = 0
    scene.cycles.transparent_max_bounces = 2
    scene.render.resolution_x = PREVIEW_WIDTH
    scene.render.resolution_y = PREVIEW_HEIGHT
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.use_persistent_data = True
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.color_depth = "8"
    scene.render.image_settings.compression = 20
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    return scene


def _build_terrain_mesh(
    name: str,
    height_m: np.ndarray,
    size_m: float,
) -> tuple[bpy.types.Object, float]:
    started = time.perf_counter()
    height, width = height_m.shape
    if height < 2 or width < 2:
        raise ValueError("terrain must be at least 2x2")

    x = np.linspace(-0.5 * size_m, 0.5 * size_m, width, dtype=np.float32)
    # Row 0 is north (+Y), matching the top-down heightmap convention.
    y = np.linspace(0.5 * size_m, -0.5 * size_m, height, dtype=np.float32)
    xx, yy = np.meshgrid(x, y)

    vertices = np.empty((height * width, 3), dtype=np.float32)
    vertices[:, 0] = xx.reshape(-1)
    vertices[:, 1] = yy.reshape(-1)
    vertices[:, 2] = height_m.reshape(-1).astype(np.float32, copy=False)

    face_rows = np.arange(height - 1, dtype=np.int32)[:, None]
    face_cols = np.arange(width - 1, dtype=np.int32)[None, :]
    top_left = (face_rows * width + face_cols).reshape(-1)
    face_count = top_left.size
    loop_indices = np.empty((face_count, 4), dtype=np.int32)
    loop_indices[:, 0] = top_left
    loop_indices[:, 1] = top_left + width       # bottom-left (south)
    loop_indices[:, 2] = top_left + width + 1   # bottom-right
    loop_indices[:, 3] = top_left + 1           # top-right

    mesh = bpy.data.meshes.new(name + "_mesh")
    mesh.vertices.add(height * width)
    mesh.vertices.foreach_set("co", vertices.reshape(-1))
    mesh.loops.add(face_count * 4)
    mesh.loops.foreach_set("vertex_index", loop_indices.reshape(-1))
    mesh.polygons.add(face_count)
    mesh.polygons.foreach_set(
        "loop_start", np.arange(0, face_count * 4, 4, dtype=np.int32)
    )
    mesh.polygons.foreach_set("loop_total", np.full(face_count, 4, dtype=np.int32))
    mesh.polygons.foreach_set("use_smooth", np.ones(face_count, dtype=bool))
    mesh.update(calc_edges=False)

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj, time.perf_counter() - started


def _make_terrain_material(name: str, size_m: float) -> bpy.types.Material:
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()

    output = nodes.new("ShaderNodeOutputMaterial")
    output.location = (640, 0)
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.location = (380, 0)
    bsdf.inputs["Base Color"].default_value = (0.36, 0.375, 0.39, 1.0)
    bsdf.inputs["Metallic"].default_value = 0.0
    bsdf.inputs["Roughness"].default_value = 0.72
    bsdf.inputs["Specular IOR Level"].default_value = 0.24

    noise = nodes.new("ShaderNodeTexNoise")
    noise.location = (-500, 180)
    noise.inputs["Scale"].default_value = 2.2
    noise.inputs["Detail"].default_value = 4.0
    noise.inputs["Roughness"].default_value = 0.55
    noise.inputs["Distortion"].default_value = 0.04

    roughness = nodes.new("ShaderNodeValToRGB")
    roughness.location = (-250, 220)
    roughness.color_ramp.elements[0].position = 0.28
    roughness.color_ramp.elements[0].color = (0.58, 0.58, 0.58, 1.0)
    roughness.color_ramp.elements[1].position = 0.78
    roughness.color_ramp.elements[1].color = (0.92, 0.92, 0.92, 1.0)

    bump = nodes.new("ShaderNodeBump")
    bump.location = (-250, -60)
    bump.inputs["Strength"].default_value = 0.08
    bump.inputs["Distance"].default_value = max(0.5, size_m * 0.0015)

    ao = nodes.new("ShaderNodeAmbientOcclusion")
    ao.location = (-500, -280)
    ao.samples = 4
    ao.inputs["Distance"].default_value = max(1.0, size_m * 0.008)

    ao_mix = nodes.new("ShaderNodeMixRGB")
    ao_mix.location = (80, 0)
    ao_mix.blend_type = "MULTIPLY"
    ao_mix.inputs["Fac"].default_value = 0.45
    ao_mix.inputs["Color1"].default_value = (0.36, 0.375, 0.39, 1.0)

    links.new(noise.outputs["Fac"], roughness.inputs["Fac"])
    links.new(roughness.outputs["Color"], bsdf.inputs["Roughness"])
    links.new(noise.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    links.new(ao.outputs["Color"], ao_mix.inputs["Color2"])
    links.new(ao_mix.outputs["Color"], bsdf.inputs["Base Color"])
    links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])
    return material


def _look_at(obj: bpy.types.Object, target: np.ndarray) -> None:
    from mathutils import Vector

    direction = Vector(target.tolist()) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def _setup_scene_objects(
    scene: bpy.types.Scene,
    terrain: bpy.types.Object,
    height_m: np.ndarray,
    size_m: float,
) -> None:
    from mathutils import Vector

    relief = float(height_m.max() - height_m.min())
    target_z = float(height_m.min()) + 0.45 * relief
    target = np.array((0.0, 0.0, target_z), dtype=np.float64)

    distance = max(size_m * 1.75, relief * 4.0)
    camera_azimuth = math.radians(135.0)
    camera_elevation = math.radians(28.0)
    camera_offset = np.array(
        (
            math.sin(camera_azimuth) * math.cos(camera_elevation),
            math.cos(camera_azimuth) * math.cos(camera_elevation),
            math.sin(camera_elevation),
        ),
        dtype=np.float64,
    ) * distance
    camera_location = target + camera_offset

    camera_data = bpy.data.cameras.new("VL_TerrainPreviewCamera")
    camera_data.lens = 35.0
    camera_data.sensor_width = 36.0
    camera_data.clip_start = max(1.0, size_m * 0.005)
    camera_data.clip_end = max(10000.0, size_m * 12.0)
    camera = bpy.data.objects.new("VL_TerrainPreviewCamera", camera_data)
    bpy.context.collection.objects.link(camera)
    camera.location = Vector(camera_location.tolist())
    _look_at(camera, target)
    scene.camera = camera

    sun_data = bpy.data.lights.new("VL_TerrainPreviewSun", "SUN")
    sun_data.energy = 3.2
    sun_data.angle = math.radians(2.0)
    sun_data.use_shadow = True
    sun = bpy.data.objects.new("VL_TerrainPreviewSun", sun_data)
    bpy.context.collection.objects.link(sun)
    sun_azimuth = math.radians(225.0)
    sun_elevation = math.radians(45.0)
    to_sun = Vector(
        (
            math.sin(sun_azimuth) * math.cos(sun_elevation),
            math.cos(sun_azimuth) * math.cos(sun_elevation),
            math.sin(sun_elevation),
        )
    )
    sun.rotation_mode = "QUATERNION"
    sun.rotation_quaternion = (-to_sun).to_track_quat("-Z", "Y")

    world = bpy.data.worlds.new("VL_TerrainPreviewWorld")
    world.use_nodes = True
    background = world.node_tree.nodes.get("Background")
    background.inputs["Color"].default_value = (0.025, 0.035, 0.055, 1.0)
    background.inputs["Strength"].default_value = 0.35
    scene.world = world

    material = _make_terrain_material("VL_TerrainPreviewMaterial", size_m)
    terrain.data.materials.append(material)


def _render_3d(
    height_m: np.ndarray,
    size_m: float,
    output_path: Path,
    label: str,
) -> dict:
    # Blender resolves a relative render filepath against its own current
    # directory (and can differ from the Python process cwd).  Always hand the
    # renderer an absolute path.
    output_path = output_path.resolve()
    _reset_scene()
    scene = bpy.context.scene
    terrain, mesh_seconds = _build_terrain_mesh(
        f"VL_TerrainPreview_{label}", height_m, size_m
    )
    _setup_scene_objects(scene, terrain, height_m, size_m)
    scene.render.filepath = str(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    render_started = time.perf_counter()
    bpy.ops.render.render(write_still=True)
    render_seconds = time.perf_counter() - render_started
    print(
        f"[terrain_preview] {label} 3D: {height_m.shape[1]}x{height_m.shape[0]} "
        f"grid, mesh {mesh_seconds:.2f}s, Cycles render {render_seconds:.2f}s"
    )
    return {
        "mesh_build_sec": float(mesh_seconds),
        "render_3d_sec": float(render_seconds),
        "grid_shape": [int(height_m.shape[0]), int(height_m.shape[1])],
    }


def _compose_compare(
    baseline_3d: np.ndarray,
    candidate_3d: np.ndarray,
    baseline_hillshade: np.ndarray,
    candidate_hillshade: np.ndarray,
) -> np.ndarray:
    cell_width = PREVIEW_WIDTH // 2
    cell_height = PREVIEW_HEIGHT // 2
    canvas = np.empty((PREVIEW_HEIGHT, PREVIEW_WIDTH, 3), dtype=np.uint8)
    canvas[:] = CANVAS_BACKGROUND

    cells = (
        (0, 0, baseline_3d),
        (cell_width, 0, candidate_3d),
        (0, cell_height, baseline_hillshade),
        (cell_width, cell_height, candidate_hillshade),
    )
    for x, y, image in cells:
        canvas[y:y + cell_height, x:x + cell_width] = _fit_to_canvas(
            image, cell_width, cell_height
        )

    # Two-pixel separators make the row/column meaning unambiguous.
    canvas[:, cell_width - 1:cell_width + 1] = (52, 58, 68)
    canvas[cell_height - 1:cell_height + 1, :] = (52, 58, 68)
    return canvas


def _height_metrics(
    normalized: np.ndarray,
    size_m: float,
    height_scale_m: float,
    sea_level_m: float,
) -> tuple[np.ndarray, float, dict]:
    height_m = normalized * height_scale_m
    cell_size_m = size_m / float(max(1, height_m.shape[1] - 1))
    metrics = analyze(height_m, cell_size_m, sea_level_m)
    return height_m, cell_size_m, metrics


def main() -> int:
    args = _parse_args()
    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    total_started = time.perf_counter()
    timings: dict[str, float] = {}

    load_started = time.perf_counter()
    normalized, source_size, processed_size = _load_heightmap(
        args.heightmap, args.resolution
    )
    timings["load_sec"] = time.perf_counter() - load_started
    height_m = normalized * args.height_m
    cell_size_m = args.size_m / float(max(1, height_m.shape[1] - 1))

    metrics_started = time.perf_counter()
    metrics = analyze(height_m, cell_size_m, args.sea_level_m)
    timings["metrics_sec"] = time.perf_counter() - metrics_started

    hillshade_started = time.perf_counter()
    shade = hillshade(height_m, cell_size_m)
    timings["hillshade_sec"] = time.perf_counter() - hillshade_started
    hillshade_rgb = _fit_to_canvas(
        _gray_to_rgb(shade), PREVIEW_WIDTH, PREVIEW_HEIGHT
    )
    _write_rgb_png(out_dir / f"{args.name}_hillshade.png", hillshade_rgb)

    slope_started = time.perf_counter()
    slope_rgb = _fit_to_canvas(
        _slope_rgb(height_m, cell_size_m), PREVIEW_WIDTH, PREVIEW_HEIGHT
    )
    timings["slope_sec"] = time.perf_counter() - slope_started
    _write_rgb_png(out_dir / f"{args.name}_slope.png", slope_rgb)

    render_info = _render_3d(
        height_m,
        args.size_m,
        out_dir / f"{args.name}_3d.png",
        "baseline",
    )
    timings["mesh_build_sec"] = render_info["mesh_build_sec"]
    timings["render_3d_sec"] = render_info["render_3d_sec"]

    if args.compare is not None:
        compare_started = time.perf_counter()
        candidate_normalized, candidate_source, candidate_processed = _load_heightmap(
            args.compare, args.resolution
        )
        candidate_height_m, candidate_cell_size_m, candidate_metrics = _height_metrics(
            candidate_normalized, args.size_m, args.height_m, args.sea_level_m
        )
        candidate_shade = hillshade(candidate_height_m, candidate_cell_size_m)
        candidate_hillshade_rgb = _gray_to_rgb(candidate_shade)

        with tempfile.TemporaryDirectory(prefix="vl_terrain_compare_") as temp_dir:
            candidate_3d_path = Path(temp_dir) / "candidate_3d.png"
            candidate_render_info = _render_3d(
                candidate_height_m,
                args.size_m,
                candidate_3d_path,
                "candidate",
            )
            candidate_3d_rgb = _read_rgb_png(candidate_3d_path)
            baseline_3d_rgb = _read_rgb_png(out_dir / f"{args.name}_3d.png")
            compare_image = _compose_compare(
                baseline_3d_rgb,
                candidate_3d_rgb,
                _gray_to_rgb(shade),
                candidate_hillshade_rgb,
            )
            _write_rgb_png(out_dir / f"{args.name}_compare.png", compare_image)
            print(
                f"[terrain_preview] wrote "
                f"{out_dir / (args.name + '_compare.png')}"
            )

        timings["compare_sec"] = time.perf_counter() - compare_started
        timings["compare_candidate_render_3d_sec"] = candidate_render_info[
            "render_3d_sec"
        ]
        metrics_payload = dict(metrics)
        metrics_payload["meta"] = {
            "source": str(args.heightmap.resolve()),
            "source_size": [int(source_size[0]), int(source_size[1])],
            "processed_size": [int(processed_size[0]), int(processed_size[1])],
            "size_m": float(args.size_m),
            "height_scale_m": float(args.height_m),
            "cell_size_m": float(cell_size_m),
            "sea_level_m": float(args.sea_level_m),
            "preview_resolution": [PREVIEW_WIDTH, PREVIEW_HEIGHT],
            "render_engine": "CYCLES",
            "render_samples": 32,
            "timings_sec": timings,
            "compare": {
                "source": str(args.compare.resolve()),
                "source_size": [
                    int(candidate_source[0]),
                    int(candidate_source[1]),
                ],
                "processed_size": [
                    int(candidate_processed[0]),
                    int(candidate_processed[1]),
                ],
                "metrics": candidate_metrics,
            },
        }
    else:
        metrics_payload = dict(metrics)
        metrics_payload["meta"] = {
            "source": str(args.heightmap.resolve()),
            "source_size": [int(source_size[0]), int(source_size[1])],
            "processed_size": [int(processed_size[0]), int(processed_size[1])],
            "size_m": float(args.size_m),
            "height_scale_m": float(args.height_m),
            "cell_size_m": float(cell_size_m),
            "sea_level_m": float(args.sea_level_m),
            "preview_resolution": [PREVIEW_WIDTH, PREVIEW_HEIGHT],
            "render_engine": "CYCLES",
            "render_samples": 32,
            "timings_sec": timings,
        }

    timings["total_sec"] = time.perf_counter() - total_started
    metrics_path = out_dir / f"{args.name}_metrics.json"
    metrics_path.write_text(
        json.dumps(metrics_payload, ensure_ascii=False, indent=2, allow_nan=False)
        + "\n",
        encoding="utf-8",
    )
    print(f"[terrain_preview] wrote {out_dir / (args.name + '_hillshade.png')}")
    print(f"[terrain_preview] wrote {out_dir / (args.name + '_slope.png')}")
    print(f"[terrain_preview] wrote {out_dir / (args.name + '_3d.png')}")
    print(f"[terrain_preview] wrote {metrics_path}")
    print(
        "[terrain_preview] metrics: "
        f"radial_symmetry={metrics['radial_symmetry']:.4f}, "
        f"anisotropy={metrics['anisotropy']:.4f}, "
        f"ridge_density={metrics['ridge_density']:.4f}, "
        f"drainage_length_km={metrics['drainage_length_km']:.3f}"
    )
    print(
        f"[terrain_preview] total {timings['total_sec']:.2f}s, "
        f"3D render {timings['render_3d_sec']:.2f}s"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
