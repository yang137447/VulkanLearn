"""Build a Geometry Nodes Pegasus-style terrain source in Blender 5.2.

Run:

    blender --background --python build_pegasus_gn.py -- \
        --output-dir <resources>/Generated/BlenderPegasusGN/Source \
        --manifest  <resources>/Generated/BlenderPegasusGN/blender_gn_import.json \
        --layer-texture-root <resources>/Generated/KiteDemo/Textures/Source

The node graph owns the *shape*: domain warp, irregular coastline, ridged
multifractal ranges, a Voronoi crest network and elevation-warped strata.
Python owns everything that must be measured from the final surface - slope,
curvature and the four terrain layer masks - because Geometry Nodes normal
fields still describe the undisplaced grid.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from export_terrain import _write_gray16_png
from pegasus_gn_core import (
    PRESETS,
    TERRAIN_ATTRIBUTES,
    build_terrain_group,
    link as _link,
    new_node as _new_node,
)
from terrain_erosion import (
    ErosionDiagnostics,
    ErosionParams,
    border_fade,
    erode,
    flow_accumulation,
    river_mask,
    taper_to_sea_level,
)


def _parse_args() -> argparse.Namespace:
    argv = sys.argv
    argv = argv[argv.index("--") + 1:] if "--" in argv else []
    parser = argparse.ArgumentParser(description="Build Pegasus Geometry Nodes terrain")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--layer-texture-root", type=Path, required=True)
    parser.add_argument("--asset-name", default="blender_gn_pegasus")
    parser.add_argument("--output-relative", default="Generated/BlenderPegasusGN")
    parser.add_argument("--size-m", type=float, default=4000.0)
    parser.add_argument("--resolution", type=int, default=1024)
    parser.add_argument("--height-scale-m", type=float, default=800.0)
    parser.add_argument("--seed", type=int, default=17)
    parser.add_argument("--scatter-density", type=float, default=0.006)
    parser.add_argument("--texture-resolution", type=int, default=1024)
    parser.add_argument("--paint-resolution", type=int, default=2048)
    parser.add_argument("--material-uv-scale", type=float, default=8.0)
    parser.add_argument("--terrain-type", default="alpine", choices=sorted(PRESETS))
    parser.add_argument(
        "--boundary-width-m", type=float, default=100.0,
        help="physical-width sea-level taper at the tile edge")
    parser.add_argument(
        "--river-mask", action=argparse.BooleanOptionalAction, default=True,
        help="derive a dendritic river/valley mask from final flow accumulation")
    parser.add_argument(
        "--river-percentile", type=float, default=98.5,
        help="drainage-area percentile that starts the river mask")
    parser.add_argument("--erode", action=argparse.BooleanOptionalAction, default=True,
                        help="run stream-power erosion on the rasterized height field")
    parser.add_argument("--erosion-iterations", type=int, default=10)
    parser.add_argument("--thermal-iterations", type=int, default=6)
    parser.add_argument("--erosion-smooth", type=float, default=0.04)
    parser.add_argument("--erosion-k", type=float, default=0.2,
                        help="implicit stream-power coefficient")
    parser.add_argument("--erosion-flow-method", choices=("mfd", "d8"),
                        default="mfd",
                        help="implicit flow routing model")
    parser.add_argument("--erosion-uplift", type=float, default=0.0,
                        help="uniform uplift in metres per erosion step")
    parser.add_argument("--erosion-deposition", type=float, default=1.0,
                        help="deposition strength multiplier (0 disables)")
    parser.add_argument("--erosion-deposition-slope-deg", type=float,
                        default=15.0,
                        help="slope where deposition falls to 1/e")
    parser.add_argument("--erosion-deposition-length-m", type=float,
                        default=1000.0,
                        help="flat-ground distance where ~63%% of sediment deposits")
    parser.add_argument("--erosion-pre-smooth", type=int, default=0,
                        help="optional low-frequency pre-smoothing; MFD usually needs none")
    parser.add_argument("--erosion-detail-restore", type=float, default=0.4,
                        help="fraction of the high-frequency detail added back")
    parser.add_argument("--camera-position", type=float, nargs=3,
                        default=(0.0, 165000.0, 330000.0))
    parser.add_argument("--camera-rotation", type=float, nargs=3,
                        default=(-14.0, 0.0, 0.0))
    return parser.parse_args(argv)


def _build_scatter_group(terrain_object, marker_object, density: float, seed: int):
    tree = bpy.data.node_groups.new("VL_Pegasus_Scatter_GN", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    group_out = _new_node(tree, "NodeGroupOutput", (900, 0))
    object_info = _new_node(tree, "GeometryNodeObjectInfo", (-700, 0))
    object_info.inputs["Object"].default_value = terrain_object
    object_info.transform_space = "ORIGINAL"
    distribute = _new_node(tree, "GeometryNodeDistributePointsOnFaces", (-420, 0))
    distribute.distribute_method = "RANDOM"
    distribute.inputs["Density"].default_value = density
    distribute.inputs["Seed"].default_value = seed
    _link(tree, object_info.outputs["Geometry"], distribute.inputs["Mesh"])
    marker_info = _new_node(tree, "GeometryNodeObjectInfo", (-420, -220))
    marker_info.inputs["Object"].default_value = marker_object
    marker_info.transform_space = "ORIGINAL"
    instance = _new_node(tree, "GeometryNodeInstanceOnPoints", (-120, 0))
    _link(tree, distribute.outputs["Points"], instance.inputs["Points"])
    _link(tree, marker_info.outputs["Geometry"], instance.inputs["Instance"])
    realize = _new_node(tree, "GeometryNodeRealizeInstances", (140, 0))
    _link(tree, instance.outputs["Instances"], realize.inputs["Geometry"])
    _link(tree, realize.outputs["Geometry"], group_out.inputs["Geometry"])
    return tree


def _evaluated_mesh_arrays(obj) -> tuple[np.ndarray, dict[str, np.ndarray]]:
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    try:
        coords = np.empty((len(mesh.vertices), 3), dtype=np.float32)
        mesh.vertices.foreach_get("co", coords.ravel())
        attributes = {}
        for name in TERRAIN_ATTRIBUTES:
            attribute = mesh.attributes.get(name)
            if attribute is None:
                raise RuntimeError(f"Geometry Nodes did not publish attribute {name}")
            values = np.empty(len(mesh.vertices), dtype=np.float32)
            attribute.data.foreach_get("value", values)
            attributes[name] = values
        return coords, attributes
    finally:
        evaluated.to_mesh_clear()


def _smooth_ramp(values: np.ndarray, low_percentile: float,
                 high_percentile: float) -> np.ndarray:
    low = float(np.percentile(values, low_percentile))
    high = float(np.percentile(values, high_percentile))
    t = np.clip((values - low) / max(high - low, 1.0e-6), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _derive_masks(height_m: np.ndarray, size_m: float, coast: np.ndarray,
                  ridge_hint: np.ndarray, river: np.ndarray | None) -> dict[str, np.ndarray]:
    """Derives every layer mask from the displaced height field."""
    cell = size_m / max(height_m.shape[1] - 1, 1)
    gradient_y, gradient_x = np.gradient(height_m, cell)
    slope_deg = np.degrees(np.arctan(np.hypot(gradient_x, gradient_y)))
    curvature = (
        np.gradient(gradient_x, cell, axis=1)
        + np.gradient(gradient_y, cell, axis=0)
    )
    crest = _smooth_ramp(-curvature, 55.0, 99.0)

    land = np.clip(coast, 0.0, 1.0)
    slope_n = _smooth_ramp(slope_deg, 60.0, 97.0)
    height_n = _smooth_ramp(height_m, 55.0, 95.0)
    ridge_n = _smooth_ramp(0.6 * np.clip(ridge_hint, 0.0, 1.0) + 0.4 * crest, 70.0, 99.0)
    low_n = 1.0 - _smooth_ramp(height_m, 35.0, 80.0)

    lowland = land * low_n * (1.0 - 0.7 * slope_n)
    slope_mask = land * slope_n
    ridge_mask = land * ridge_n * _smooth_ramp(height_m, 45.0, 85.0)
    highland = land * height_n * (1.0 - 0.5 * slope_n)
    river_mask = (
        land * np.clip(river, 0.0, 1.0)
        if river is not None else np.zeros_like(height_m)
    )

    total = lowland + slope_mask + ridge_mask + highland + river_mask
    safe = np.maximum(total, 1.0e-6)
    valid = total > 1.0e-6
    masks = {
        "lowland": np.where(valid, lowland / safe, 0.0),
        "slope": np.where(valid, slope_mask / safe, 0.0),
        "ridge": np.where(valid, ridge_mask / safe, 0.0),
        "highland": np.where(valid, highland / safe, 0.0),
    }
    if river is not None:
        masks["river"] = np.where(valid, river_mask / safe, 0.0)
    return masks


def _bilinear(height_m: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
    """Samples a height field with u/v in [0, 1) and bilinear filtering."""
    rows, columns = height_m.shape
    x = np.clip(u * (columns - 1), 0.0, columns - 1.0)
    y = np.clip(v * (rows - 1), 0.0, rows - 1.0)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    x1 = np.minimum(x0 + 1, columns - 1)
    y1 = np.minimum(y0 + 1, rows - 1)
    fx = x - x0
    fy = y - y0
    top = height_m[y0, x0] * (1.0 - fx) + height_m[y0, x1] * fx
    bottom = height_m[y1, x0] * (1.0 - fx) + height_m[y1, x1] * fx
    return top * (1.0 - fy) + bottom * fy


def _normalize_relief(height: np.ndarray, coast: np.ndarray,
                      low_percentile: float = 1.0,
                      high_percentile: float = 99.0) -> np.ndarray:
    """Stretches land relief to the full 0..1 range.

    A weighted stack of noise primitives produces a narrow band of values (the
    median lands near 0.7 with a spread under 0.4), so without this the whole
    tile sits at one altitude.  Percentiles keep outliers from dominating.
    """
    land = coast > 0.05
    if not land.any():
        return height
    low = float(np.percentile(height[land], low_percentile))
    high = float(np.percentile(height[land], high_percentile))
    scaled = np.clip((height - low) / max(high - low, 1.0e-6), 0.0, 1.0)
    return scaled * np.clip(coast, 0.0, 1.0)


def _sample_scatter(height_m: np.ndarray, size_m: float, density: float,
                    seed: int) -> list[dict[str, float]]:
    """Places scatter points on the eroded surface, skipping sea and cliffs."""
    cell = size_m / max(height_m.shape[1] - 1, 1)
    gradient_y, gradient_x = np.gradient(height_m, cell)
    slope_deg = np.degrees(np.arctan(np.hypot(gradient_x, gradient_y)))
    rng = np.random.default_rng(seed)
    count = int(density * size_m * size_m)
    u = rng.random(count)
    v = rng.random(count)
    heights = _bilinear(height_m, u, v)
    slopes = _bilinear(slope_deg, u, v)
    keep = (heights > 0.5) & (slopes < 42.0)
    points = []
    for uu, vv, hh in zip(u[keep], v[keep], heights[keep]):
        points.append({
            "position": [float(uu * size_m - size_m * 0.5) * 100.0,
                         float(hh) * 100.0,
                         float(vv * size_m - size_m * 0.5) * 100.0],
            "radius": 100.0,
        })
    return points


def _write_manifest(args, height_path: Path, mask_paths: dict[str, Path],
                    include_river: bool) -> None:
    layer_specs = [
        ("Gravel Ground", "lowland", 0, [0.0, 0.45], [0.0, 30.0], 16.0),
        ("Slope Rock", "slope", 7, [0.0, 1.0], [18.0, 90.0], 14.0),
        ("Ridge Rock", "ridge", 2, [0.25, 1.0], [20.0, 90.0], 12.0),
        ("Highland Volcanic", "highland", 2, [0.45, 1.0], [0.0, 90.0], 12.0),
    ]
    if include_river:
        layer_specs.append(
            ("River Bed", "river", 6, [0.0, 1.0], [0.0, 45.0], 24.0))
    texture_base = args.layer_texture_root.resolve()
    layers = []
    for name, mask_name, texture_index, height_range, slope_range, uv_scale in layer_specs:
        layers.append({
            "name": name,
            "weightmapPath": str(mask_paths[mask_name].resolve()),
            "textures": {
                "albedo": str((texture_base / f"layer_{texture_index:02d}_albedo.png").resolve()),
                "normal": str((texture_base / f"layer_{texture_index:02d}_normal.png").resolve()),
                "pbr": str((texture_base / f"layer_{texture_index:02d}_pbr.png").resolve()),
                "height": str((texture_base / f"layer_{texture_index:02d}_height16.png").resolve()),
            },
            "heightRange": height_range,
            "slopeRange": slope_range,
            "uvScale": uv_scale,
            "uvRotation": 0.0,
            "noise": 0.03,
        })
    manifest = {
        "producer": "blender-geometry-nodes",
        "assetName": args.asset_name,
        "outputRelative": args.output_relative,
        "heightmapPath": str(height_path.resolve()),
        "size": [args.size_m * 100.0, args.size_m * 100.0],
        "heightScale": args.height_scale_m * 100.0,
        "heightOffset": 0.0,
        "textureResolution": args.texture_resolution,
        "paintResolution": args.paint_resolution,
        "materialUvScale": args.material_uv_scale,
        "invertNormalGreen": True,
        "flipVertical": False,
        "description": (
            "Blender 5.2 Geometry Nodes 生成的 Pegasus 风格地形：GN 负责域扭曲、海岸、"
            "脊状多重分形山脉、Voronoi 山脊网络与岩层阶地；坡度/曲率 mask 由最终高度场反算；"
            "侵蚀后重做边缘海平面 taper，并用汇流面积生成河网 mask；材质使用 KiteDemo PBR 占位。"
        ),
        "sceneDescription": (
            "Blender Geometry Nodes producer 验收场景：GN 地形高度场、坡度/脊线/河网 mask、"
            "KiteDemo 地表纹理数组；不依赖 Houdini 运行时。"
        ),
        "environment": {
            "type": "proceduralSky",
            "cubeSize": 128,
            "intensity": 1.0,
            "skyParameters": {
                "sunIntensity": 1.0,
                "sunColor": [22.0, 17.5, 10.0],
                "sunAngularRadius": 0.08,
                "zenithColor": [0.09, 0.32, 0.95],
                "horizonColor": [0.85, 0.78, 0.58],
                "groundColor": [0.06, 0.07, 0.055],
                "skyGradientExponent": 0.42,
                "groundGradientExponent": 0.35,
                "sunHaloExponent": 96.0,
                "sunHaloStrength": 0.45,
            },
        },
        "terrainPosition": [0.0, 0.0, 0.0],
        "cameraPosition": [float(value) for value in args.camera_position],
        "cameraRotation": [float(value) for value in args.camera_rotation],
        "gridResolution": args.resolution,
        "chunkQuads": 256 if args.resolution >= 1024 else 128,
        "lodLevels": 3,
        "lodDistances": [500000.0, 1000000.0],
        "chunkResidencyDistances": [550000.0, 1100000.0],
        "layers": layers,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8")


def main() -> int:
    args = _parse_args()
    if args.resolution < 32 or args.resolution > 2048:
        raise ValueError("resolution must be in [32, 2048]")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    terrain_group = build_terrain_group(
        args.size_m, args.resolution, args.seed, args.height_scale_m,
        args.terrain_type)
    terrain_mesh = bpy.data.meshes.new("VL_Pegasus_GN.mesh")
    terrain_object = bpy.data.objects.new("VL_Pegasus_Terrain", terrain_mesh)
    bpy.context.collection.objects.link(terrain_object)
    modifier = terrain_object.modifiers.new("VL Pegasus Geometry Nodes", "NODES")
    modifier.node_group = terrain_group
    bpy.context.view_layer.update()

    coords, attributes = _evaluated_mesh_arrays(terrain_object)
    height = np.clip(attributes["vl_height"], 0.0, 1.0).reshape(
        args.resolution, args.resolution)
    height = _normalize_relief(height, attributes["vl_coast"].reshape(height.shape))
    height_m = height * args.height_scale_m
    cell_size = args.size_m / max(args.resolution - 1, 1)
    erosion_stats = None
    if args.erode:
        diagnostics = ErosionDiagnostics()
        eroded, _, fill_depth = erode(
            height_m, cell_size,
            ErosionParams(
                iterations=args.erosion_iterations,
                thermal_iterations=args.thermal_iterations,
                smooth_weight=args.erosion_smooth,
                k=args.erosion_k,
                flow_method=args.erosion_flow_method,
                uplift_m=args.erosion_uplift,
                deposition_rate=args.erosion_deposition,
                deposition_slope_deg=args.erosion_deposition_slope_deg,
                deposition_length_m=args.erosion_deposition_length_m,
                pre_smooth_iterations=args.erosion_pre_smooth,
                detail_restore=args.erosion_detail_restore),
            diagnostics)
        height_m = np.clip(eroded, 0.0, args.height_scale_m)
        erosion_stats = {
            "iterations": args.erosion_iterations,
            "thermalIterations": args.thermal_iterations,
            "flowMethod": args.erosion_flow_method,
            "preSmoothIterations": args.erosion_pre_smooth,
            "detailRestore": args.erosion_detail_restore,
            "upliftM": args.erosion_uplift,
            "depositionRate": args.erosion_deposition,
            "depositionSlopeDeg": args.erosion_deposition_slope_deg,
            "depositionLengthM": args.erosion_deposition_length_m,
            "maxFillDepthM": float(fill_depth.max()),
            "erodedVolumeM3": diagnostics.eroded_volume_m3,
            "depositedVolumeM3": diagnostics.deposited_volume_m3,
            "exportedSedimentM3": diagnostics.exported_sediment_m3,
            "upliftVolumeM3": diagnostics.uplift_volume_m3,
        }

    # Erosion routes flow through the original rim; taper afterwards as well so
    # the exported tile cannot end in a vertical wall or single-cell teeth.
    height_m = taper_to_sea_level(
        height_m, cell_size, args.boundary_width_m)
    height = height_m / args.height_scale_m

    flow_area = flow_accumulation(
        height_m, cell_size, args.erosion_flow_method)
    flow_path = args.output_dir / "flow_accumulation.png"
    flow_visual = np.log1p(flow_area / max(cell_size * cell_size, 1.0e-6))
    flow_visual /= max(float(flow_visual.max()), 1.0e-6)
    _write_gray16_png(flow_path, np.clip(flow_visual, 0.0, 1.0) * 65535.0)
    if erosion_stats is not None:
        erosion_stats["meanFlowAreaM2"] = float(flow_area.mean())
        erosion_stats["maxFlowAreaM2"] = float(flow_area.max())
        erosion_stats["flowTexture"] = str(flow_path.resolve())

    height_path = args.output_dir / "heightmap_gn.png"
    _write_gray16_png(height_path, height * 65535.0)

    coast = attributes["vl_coast"].reshape(height.shape)
    coast_mask = np.clip(coast, 0.0, 1.0) * border_fade(
        height.shape, cell_size, args.boundary_width_m)
    river = (
        river_mask(flow_area, cell_size, coast_mask > 0.05,
                   args.river_percentile)
        if args.river_mask else None
    )
    river_coverage = (
        float(np.mean(river > 0.05)) if river is not None else 0.0)
    masks = _derive_masks(
        height_m, args.size_m, coast_mask,
        attributes["vl_relief"].reshape(height.shape), river)
    mask_paths: dict[str, Path] = {}
    for name, values in masks.items():
        path = args.output_dir / f"mask_gn_{name}.png"
        _write_gray16_png(path, np.clip(values, 0.0, 1.0) * 65535.0)
        mask_paths[name] = path
    _write_manifest(args, height_path, mask_paths, args.river_mask)

    marker_mesh = bpy.data.meshes.new("VL_Pegasus_ScatterMarker.mesh")
    marker_mesh.from_pydata([(0.0, 0.0, 0.0)], [], [])
    marker_mesh.update()
    marker_object = bpy.data.objects.new("VL_Pegasus_ScatterMarker", marker_mesh)
    bpy.context.collection.objects.link(marker_object)
    marker_object.hide_render = True
    scatter_group = _build_scatter_group(
        terrain_object, marker_object, args.scatter_density, args.seed)
    scatter_mesh = bpy.data.meshes.new("VL_Pegasus_Scatter.mesh")
    scatter_object = bpy.data.objects.new("VL_Pegasus_Scatter", scatter_mesh)
    bpy.context.collection.objects.link(scatter_object)
    scatter_modifier = scatter_object.modifiers.new("VL Pegasus Scatter", "NODES")
    scatter_modifier.node_group = scatter_group
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated_scatter = scatter_object.evaluated_get(depsgraph)
    point_data = evaluated_scatter.data
    preview_count = len(point_data.points) if hasattr(point_data, "points") else 0
    # Exported points follow the *eroded* surface; the Geometry Nodes scatter
    # object above stays in the .blend as an authoring preview only.
    scatter_points = _sample_scatter(height_m, args.size_m, args.scatter_density, args.seed)
    scatter_count = len(scatter_points)
    (args.output_dir / "scatter_points.json").write_text(
        json.dumps({"producer": "blender-geometry-nodes",
                    "coordinateSystem": "engine-y-up-cm", "points": scatter_points},
                   ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    bpy.ops.wm.save_as_mainfile(
        filepath=str((args.output_dir / "blender_pegasus_gn.blend").resolve()))
    metadata = {
        "producer": "blender-geometry-nodes",
        "blenderVersion": bpy.app.version_string,
        "nodeGroups": ["VL_Pegasus_Terrain_GN", "VL_Pegasus_Scatter_GN"],
        "resolution": args.resolution,
        "sizeMeters": args.size_m,
        "heightScaleMeters": args.height_scale_m,
        "scatterPoints": scatter_count,
        "previewScatterInstances": preview_count,
        "attributes": list(TERRAIN_ATTRIBUTES),
        "maskSource": "height-field",
        "terrainType": args.terrain_type,
        "boundaryWidthM": args.boundary_width_m,
        "riverMask": args.river_mask,
        "riverPercentile": args.river_percentile,
        "riverCoverage": river_coverage,
        "flowAccumulation": str(flow_path.resolve()),
        "shapePreset": PRESETS[args.terrain_type].__dict__,
        "erosion": erosion_stats,
        "manifest": str(args.manifest.resolve()),
    }
    (args.output_dir / "blender_pegasus_gn_metadata.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
