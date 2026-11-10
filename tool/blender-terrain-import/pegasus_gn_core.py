"""Composable terrain-shape node groups built from mainstream algorithms.

The graph is assembled from the same primitives used by production terrain
generators, so a preset is a set of weights instead of a hand-tuned one-off:

* ``fBm``            - Mandelbrot / Musgrave fractional Brownian motion.
* ``billow``         - ``|2n-1|`` octave sum, rounded cloud-like mass.
* ``ridged``         - ``(1-|2n-1|)^2`` octaves (Musgrave 1988), sharp crests.
* ``cellular``       - Worley / Voronoi F1, karst towers and lava cones.
* ``dune``           - anisotropic ridged noise along a wind axis.
* ``domain warp``    - Perlin 1985 / Quilez warping of the sample coordinates.
* ``strata``         - elevation-warped banding (rock layers, mesas).
* ``coast``          - warped low-frequency continents plus an inland ramp.

Presets live in :data:`PRESETS`; ``build_terrain_group`` composes only the
primitives whose weight is non-zero, so the exported graph stays readable.
Slope, curvature and flow masks are *not* produced here: after ``Set Position``
a Geometry Nodes normal still describes the flat grid, so those masks are
derived from the rasterized height field by the Python producer.
"""

from __future__ import annotations

from dataclasses import dataclass

import bpy


@dataclass(frozen=True)
class Preset:
    """Weights and scales of the shape primitives for one terrain archetype."""

    name: str
    coast_tilt: float = 0.62
    coast_bias: float = -0.16
    shore_softness: float = 0.16
    plateau_ramp: float = 0.12
    hills: float = 0.10
    warp_strength: float = 0.135
    warp_detail: float = 0.035
    mountain_amp: float = 0.85
    contrast_power: float = 1.65
    ridged_weight: float = 1.0
    ridged_scale: float = 1.25
    fbm_weight: float = 0.30
    fbm_scale: float = 0.75
    billow_weight: float = 0.0
    billow_scale: float = 1.4
    cellular_weight: float = 0.0
    cellular_scale: float = 3.0
    dune_weight: float = 0.0
    dune_angle_deg: float = 35.0
    dune_stretch: float = 0.18
    terrace_strength: float = 0.10
    terrace_bands: float = 9.0
    detail_strength: float = 0.018
    detail_scale: float = 26.0
    ridge_anisotropy: float = 0.55


PRESETS: dict[str, Preset] = {
    # Sharp alpine crests over broad massifs.
    "alpine": Preset(
        name="alpine",
        coast_tilt=0.35, coast_bias=0.30, shore_softness=0.24,
        ridged_weight=1.0, ridged_scale=1.25,
        fbm_weight=0.32, fbm_scale=0.70,
        billow_weight=0.10,
        plateau_ramp=0.14, hills=0.10,
        mountain_amp=0.74, terrace_strength=0.05, ridge_anisotropy=0.52,
    ),
    # Rolling coastal ranges with wide beaches and bays.
    "coastal": Preset(
        name="coastal",
        coast_tilt=0.78, coast_bias=-0.22, shore_softness=0.22,
        ridged_weight=0.55, ridged_scale=1.05,
        fbm_weight=0.85, fbm_scale=0.85,
        billow_weight=0.35, billow_scale=1.8,
        plateau_ramp=0.06, hills=0.14,
        mountain_amp=0.78, terrace_strength=0.06, ridge_anisotropy=0.62,
        detail_strength=0.014,
    ),
    # Layered plateaus cut by deep meandering canyons.
    "canyon": Preset(
        name="canyon",
        coast_tilt=0.30, coast_bias=0.18, shore_softness=0.30,
        ridged_weight=0.22, ridged_scale=0.90,
        fbm_weight=0.55, fbm_scale=0.55,
        billow_weight=0.20,
        plateau_ramp=0.42, hills=0.04,
        mountain_amp=0.52, terrace_strength=0.62, terrace_bands=13.0,
        ridge_anisotropy=0.45,
        detail_strength=0.010,
    ),
    # Wind-aligned dune seas.
    "dune": Preset(
        name="dune",
        coast_tilt=0.20, coast_bias=0.28, shore_softness=0.35,
        ridged_weight=0.0,
        fbm_weight=0.30, fbm_scale=0.5,
        dune_weight=1.0, dune_angle_deg=28.0, dune_stretch=0.14,
        plateau_ramp=0.10, hills=0.03,
        mountain_amp=0.42, terrace_strength=0.0,
        detail_strength=0.006,
        warp_strength=0.05,
    ),
    # Wet tropical karst towers and sinkholes.
    "karst": Preset(
        name="karst",
        coast_tilt=0.55, coast_bias=-0.10, shore_softness=0.20,
        ridged_weight=0.25, ridged_scale=1.6,
        fbm_weight=0.45, fbm_scale=1.2,
        cellular_weight=0.95, cellular_scale=5.0,
        plateau_ramp=0.10, hills=0.06,
        mountain_amp=1.05, terrace_strength=0.14, terrace_bands=7.0,
        detail_strength=0.012,
    ),
    # Volcanic cones and broad lava fields.
    "volcanic": Preset(
        name="volcanic",
        coast_tilt=0.48, coast_bias=-0.05,
        ridged_weight=0.55, ridged_scale=1.5,
        fbm_weight=0.40, fbm_scale=0.65,
        cellular_weight=0.70, cellular_scale=2.6,
        plateau_ramp=0.16, hills=0.05,
        mountain_amp=0.82, terrace_strength=0.18, terrace_bands=6.0,
        detail_strength=0.014,
    ),
}


def new_node(tree, node_type: str, location: tuple[int, int]):
    node = tree.nodes.new(node_type)
    node.location = location
    return node


def link(tree, output_socket, input_socket) -> None:
    tree.links.new(output_socket, input_socket)


def math_node(tree, operation: str, a=None, b=None, c=None,
              location: tuple[int, int] = (0, 0)):
    node = tree.nodes.new("ShaderNodeMath")
    node.operation = operation
    node.location = location
    for index, value in enumerate((a, b, c)):
        if value is None:
            continue
        if isinstance(value, (int, float)):
            node.inputs[index].default_value = float(value)
        else:
            tree.links.new(value, node.inputs[index])
    return node


def vector_math(tree, operation: str, a=None, b=None,
                location: tuple[int, int] = (0, 0)):
    node = tree.nodes.new("ShaderNodeVectorMath")
    node.operation = operation
    node.location = location
    for index, value in enumerate((a, b)):
        if value is None:
            continue
        if isinstance(value, (tuple, list)):
            node.inputs[index].default_value = tuple(float(v) for v in value)
        else:
            tree.links.new(value, node.inputs[index])
    return node


def map_range(tree, value_socket, from_min: float, from_max: float,
              to_min: float = 0.0, to_max: float = 1.0,
              location: tuple[int, int] = (0, 0), clamp: bool = True,
              interpolation: str = "LINEAR"):
    node = tree.nodes.new("ShaderNodeMapRange")
    node.location = location
    node.clamp = clamp
    node.interpolation_type = interpolation
    node.inputs["From Min"].default_value = float(from_min)
    node.inputs["From Max"].default_value = float(from_max)
    node.inputs["To Min"].default_value = float(to_min)
    node.inputs["To Max"].default_value = float(to_max)
    tree.links.new(value_socket, node.inputs["Value"])
    return node


def smoothstep(tree, value_socket, edge0: float, edge1: float,
               location: tuple[int, int] = (0, 0)):
    return map_range(tree, value_socket, edge0, edge1, 0.0, 1.0, location,
                     interpolation="SMOOTHSTEP").outputs["Result"]


def noise(tree, vector_socket, scale: float, detail: float, roughness: float,
          w: float, location: tuple[int, int] = (0, 0),
          distortion: float = 0.0):
    node = tree.nodes.new("ShaderNodeTexNoise")
    node.location = location
    node.noise_dimensions = "4D"
    node.inputs["W"].default_value = float(w)
    node.inputs["Scale"].default_value = float(scale)
    node.inputs["Detail"].default_value = float(detail)
    node.inputs["Roughness"].default_value = float(roughness)
    node.inputs["Distortion"].default_value = float(distortion)
    tree.links.new(vector_socket, node.inputs["Vector"])
    return node


def _octave_sum(tree, uv, seed: float, scale: float, octaves: int,
                gain: float, lacunarity: float, transform: str,
                base_location: tuple[int, int]):
    """Sums octaves of a noise field with an optional ridge/billow transform."""
    accumulator = None
    total_weight = 0.0
    for index in range(octaves):
        octave_scale = scale * (lacunarity ** index)
        weight = gain ** index
        total_weight += weight
        sample = noise(tree, uv, octave_scale, 2.0, 0.5, seed + index * 17.0,
                       (base_location[0], base_location[1] - index * 210))
        doubled = math_node(tree, "MULTIPLY", sample.outputs["Fac"], 2.0,
                            location=(base_location[0] + 170, base_location[1] - index * 210))
        centered = math_node(tree, "SUBTRACT", doubled.outputs[0], 1.0,
                             location=(base_location[0] + 330, base_location[1] - index * 210))
        if transform == "ridged":
            absolute = math_node(tree, "ABSOLUTE", centered.outputs[0],
                                 location=(base_location[0] + 490, base_location[1] - index * 210))
            inverted = math_node(tree, "SUBTRACT", 1.0, absolute.outputs[0],
                                 location=(base_location[0] + 650, base_location[1] - index * 210))
            shaped = math_node(tree, "POWER", inverted.outputs[0], 2.0,
                               location=(base_location[0] + 810, base_location[1] - index * 210))
            value = shaped.outputs[0]
        elif transform == "billow":
            absolute = math_node(tree, "ABSOLUTE", centered.outputs[0],
                                 location=(base_location[0] + 490, base_location[1] - index * 210))
            value = absolute.outputs[0]
        else:
            value = sample.outputs["Fac"]
        term = math_node(tree, "MULTIPLY", value, weight,
                         location=(base_location[0] + 810, base_location[1] - index * 210))
        if accumulator is None:
            accumulator = term.outputs[0]
        else:
            accumulator = math_node(
                tree, "ADD", accumulator, term.outputs[0],
                location=(base_location[0] + 980, base_location[1] - index * 210)).outputs[0]
    normalized = math_node(tree, "MULTIPLY", accumulator, 1.0 / total_weight,
                           location=(base_location[0] + 1150, base_location[1]))
    return normalized.outputs[0]


def _fbm_field(tree, uv, seed: float, scale: float, location: tuple[int, int]):
    return _octave_sum(tree, uv, seed, scale, 5, 0.5, 2.0, "fbm", location)


def _billow_field(tree, uv, seed: float, scale: float, location: tuple[int, int]):
    field = _octave_sum(tree, uv, seed, scale, 4, 0.5, 2.1, "billow", location)
    inverted = math_node(tree, "SUBTRACT", 1.0, field, location=(location[0] + 1150, location[1] - 300))
    return inverted.outputs[0]


def _ridged_field(tree, uv, seed: float, scale: float,
                  location: tuple[int, int], anisotropy: float = 0.55):
    stretched = vector_math(tree, "MULTIPLY", uv, (1.0, anisotropy, 1.0), location)
    return _octave_sum(tree, stretched.outputs["Vector"], seed, scale, 4, 0.5, 2.05,
                       "ridged", (location[0] + 180, location[1]))


def _cellular_field(tree, uv, seed: float, scale: float,
                    location: tuple[int, int], sharpness: float = 1.8):
    primary = new_node(tree, "ShaderNodeTexVoronoi", location)
    primary.voronoi_dimensions = "4D"
    primary.feature = "F1"
    primary.inputs["W"].default_value = seed
    primary.inputs["Scale"].default_value = scale
    primary.inputs["Randomness"].default_value = 1.0
    link(tree, uv, primary.inputs["Vector"])
    towers = map_range(tree, primary.outputs["Distance"], 0.0, 0.48, 1.0, 0.0,
                       (location[0] + 260, location[1]), interpolation="SMOOTHSTEP")
    towers_sharp = math_node(tree, "POWER", towers.outputs["Result"], sharpness,
                             location=(location[0] + 440, location[1]))
    secondary = new_node(tree, "ShaderNodeTexVoronoi", (location[0], location[1] - 260))
    secondary.voronoi_dimensions = "4D"
    secondary.feature = "F1"
    secondary.inputs["W"].default_value = seed + 7.0
    secondary.inputs["Scale"].default_value = scale * 2.7
    link(tree, uv, secondary.inputs["Vector"])
    detail = map_range(tree, secondary.outputs["Distance"], 0.0, 0.45, 0.25, 0.0,
                       (location[0] + 260, location[1] - 260), interpolation="SMOOTHSTEP")
    combined = math_node(tree, "ADD", towers_sharp.outputs[0], detail.outputs["Result"],
                         location=(location[0] + 620, location[1]))
    scaled = math_node(tree, "MULTIPLY", combined.outputs[0], 0.72,
                       location=(location[0] + 800, location[1]))
    return scaled.outputs[0]


def _dune_field(tree, uv, seed: float, angle_deg: float, stretch: float,
                location: tuple[int, int]):
    rotate = new_node(tree, "ShaderNodeVectorRotate", location)
    rotate.rotation_type = "Z_AXIS"
    rotate.inputs["Angle"].default_value = math_radians(angle_deg)
    rotate.inputs["Center"].default_value = (0.5, 0.5, 0.0)
    link(tree, uv, rotate.inputs["Vector"])
    stretched = vector_math(tree, "MULTIPLY", rotate.outputs["Vector"],
                            (1.0, stretch, 1.0), (location[0] + 200, location[1]))
    field = _octave_sum(tree, stretched.outputs["Vector"], seed, 9.0, 3, 0.5, 2.1,
                        "ridged", (location[0] + 400, location[1]))
    return field


def math_radians(degrees: float) -> float:
    return degrees * 3.141592653589793 / 180.0


def _build_warp(tree, uv_socket, seed: int, preset: Preset):
    coarse = noise(tree, uv_socket, 2.2, 4.0, 0.55, seed * 1.31,
                   (-1180, 460), distortion=0.35)
    fine = noise(tree, uv_socket, 6.4, 3.0, 0.50, seed * 2.73, (-1180, 240))
    coarse_offset = vector_math(tree, "SUBTRACT", coarse.outputs["Color"], (0.5, 0.5, 0.5),
                                (-980, 460))
    coarse_scaled = vector_math(tree, "MULTIPLY", coarse_offset.outputs["Vector"],
                                (preset.warp_strength, preset.warp_strength, 0.0),
                                (-800, 460))
    fine_offset = vector_math(tree, "SUBTRACT", fine.outputs["Color"], (0.5, 0.5, 0.5),
                              (-980, 240))
    fine_scaled = vector_math(tree, "MULTIPLY", fine_offset.outputs["Vector"],
                              (preset.warp_detail, preset.warp_detail, 0.0),
                              (-800, 240))
    summed = vector_math(tree, "ADD", coarse_scaled.outputs["Vector"],
                         fine_scaled.outputs["Vector"], (-620, 360))
    warped = vector_math(tree, "ADD", uv_socket, summed.outputs["Vector"], (-440, 360))
    return warped.outputs["Vector"]


def _build_coast(tree, warped_uv, seed: int, preset: Preset):
    continents = noise(tree, warped_uv, 1.55, 5.0, 0.58, seed * 3.11,
                       (-620, 120), distortion=0.25)
    bays = noise(tree, warped_uv, 0.82, 3.0, 0.55, seed * 4.37, (-620, -80))
    separate = new_node(tree, "ShaderNodeSeparateXYZ", (-620, -260))
    link(tree, warped_uv, separate.inputs["Vector"])
    tilt = math_node(tree, "SUBTRACT", 1.0, separate.outputs["Y"], location=(-440, -260))
    tilt_scaled = math_node(tree, "MULTIPLY", tilt.outputs[0], preset.coast_tilt,
                            location=(-260, -260))
    continent_scaled = math_node(tree, "MULTIPLY", continents.outputs["Fac"], 0.92,
                                 location=(-440, 120))
    bay_offset = math_node(tree, "SUBTRACT", 0.5, bays.outputs["Fac"], location=(-440, -80))
    bay_scaled = math_node(tree, "MULTIPLY", bay_offset.outputs[0], 0.42,
                           location=(-260, -80))
    total = math_node(tree, "ADD", continent_scaled.outputs[0], tilt_scaled.outputs[0],
                      location=(-100, -140))
    total2 = math_node(tree, "ADD", total.outputs[0], bay_scaled.outputs[0],
                       location=(80, -140))
    biased = math_node(tree, "ADD", total2.outputs[0], preset.coast_bias,
                       location=(260, -140))
    land = map_range(tree, biased.outputs[0], 0.30, 0.30 + preset.shore_softness,
                     0.0, 1.0, (440, -140), interpolation="SMOOTHSTEP")
    return land.outputs["Result"], biased.outputs[0]


def _build_strata(tree, elevation_socket, field_socket, warped_uv, seed: int,
                  preset: Preset):
    if preset.terrace_strength <= 0.0:
        return elevation_socket
    band_noise = noise(tree, warped_uv, 2.6, 3.0, 0.5, seed * 7.9, (60, -620))
    band_offset = math_node(tree, "SUBTRACT", band_noise.outputs["Fac"], 0.5,
                            location=(240, -620))
    band_offset_scaled = math_node(tree, "MULTIPLY", band_offset.outputs[0], 1.35,
                                   location=(400, -620))
    phase = math_node(tree, "MULTIPLY", elevation_socket, preset.terrace_bands,
                      location=(400, -800))
    phase2 = math_node(tree, "ADD", phase.outputs[0], band_offset_scaled.outputs[0],
                       location=(580, -800))
    band_index = math_node(tree, "FLOOR", phase2.outputs[0], location=(760, -800))
    band_value = math_node(tree, "DIVIDE", band_index.outputs[0], preset.terrace_bands,
                           location=(940, -800))
    strength = math_node(tree, "MULTIPLY", field_socket, preset.terrace_strength,
                         location=(1120, -1000))
    delta = math_node(tree, "SUBTRACT", band_value.outputs[0], elevation_socket,
                      location=(1120, -800))
    blended = math_node(tree, "MULTIPLY", delta.outputs[0], strength.outputs[0],
                        location=(1300, -800))
    return math_node(tree, "ADD", elevation_socket, blended.outputs[0],
                     location=(1480, -800)).outputs[0]


def build_terrain_group(size_m: float, resolution: int, seed: int,
                        height_scale_m: float, archetype: str = "alpine"):
    """Builds ``VL_Pegasus_Terrain_GN`` for one terrain archetype."""
    if archetype not in PRESETS:
        raise KeyError(f"unknown terrain archetype {archetype!r}; "
                       f"choose from {sorted(PRESETS)}")
    preset = PRESETS[archetype]
    tree = bpy.data.node_groups.new("VL_Pegasus_Terrain_GN", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")

    group_out = new_node(tree, "NodeGroupOutput", (3000, 0))
    grid = new_node(tree, "GeometryNodeMeshGrid", (-1600, 800))
    grid.inputs["Size X"].default_value = float(size_m)
    grid.inputs["Size Y"].default_value = float(size_m)
    grid.inputs["Vertices X"].default_value = int(resolution)
    grid.inputs["Vertices Y"].default_value = int(resolution)

    position = new_node(tree, "GeometryNodeInputPosition", (-1600, 600))
    uv = vector_math(tree, "DIVIDE", position.outputs["Position"],
                     (size_m, size_m, size_m), (-1420, 600))
    uv_centered = vector_math(tree, "ADD", uv.outputs["Vector"], (0.5, 0.5, 0.5),
                              (-1240, 600))

    warped_uv = _build_warp(tree, uv_centered.outputs["Vector"], seed, preset)
    land, coast_field = _build_coast(tree, warped_uv, seed, preset)
    plateau = smoothstep(tree, coast_field, 0.34, 0.86, (-260, -260))

    # Weighted primitive stack.
    fields = []
    if preset.ridged_weight > 0.0:
        fields.append((preset.ridged_weight,
                       _ridged_field(tree, warped_uv, seed * 5.17,
                                       preset.ridged_scale, (-620, 40),
                                       preset.ridge_anisotropy)))
    if preset.fbm_weight > 0.0:
        fields.append((preset.fbm_weight,
                       _fbm_field(tree, warped_uv, seed * 8.3,
                                  preset.fbm_scale, (-620, -420))))
    if preset.billow_weight > 0.0:
        fields.append((preset.billow_weight,
                       _billow_field(tree, warped_uv, seed * 8.3,
                                     preset.billow_scale, (-620, -880))))
    if preset.cellular_weight > 0.0:
        fields.append((preset.cellular_weight,
                       _cellular_field(tree, warped_uv, seed * 6.1,
                                       preset.cellular_scale, (-620, -1340))))
    if preset.dune_weight > 0.0:
        fields.append((preset.dune_weight,
                       _dune_field(tree, warped_uv, seed * 4.7,
                                   preset.dune_angle_deg, preset.dune_stretch,
                                   (-620, -1800))))

    total_weight = sum(weight for weight, _ in fields) or 1.0
    combined = None
    for index, (weight, socket) in enumerate(fields):
        term = math_node(tree, "MULTIPLY", socket, weight / total_weight,
                         location=(1160, 260 - index * 260))
        if combined is None:
            combined = term.outputs[0]
        else:
            combined = math_node(
                tree, "ADD", combined, term.outputs[0],
                location=(1340, 260 - index * 260)).outputs[0]
    if combined is None:
        combined = math_node(tree, "MULTIPLY", 1.0, 0.0, location=(1160, 260)).outputs[0]

    hills = noise(tree, warped_uv, 1.45, 3.0, 0.5, seed * 10.9, (-80, 460))
    hills_offset = math_node(tree, "SUBTRACT", hills.outputs["Fac"], 0.38,
                             location=(100, 460))
    hills_scaled = math_node(tree, "MULTIPLY", hills_offset.outputs[0], preset.hills,
                             location=(280, 460))

    # Averaging primitives pulls the median toward 0.5, which reads as a raised
    # plateau.  A smooth contrast curve restores valleys at 0 and crests at 1.
    contrast = math_node(tree, "POWER", combined, preset.contrast_power,
                         location=(1340, 260))
    relief = math_node(tree, "MULTIPLY", contrast.outputs[0],
                       preset.mountain_amp, location=(1520, 260))
    inland = math_node(tree, "MULTIPLY", plateau, preset.plateau_ramp,
                       location=(1520, 460))
    base = math_node(tree, "ADD", hills_scaled.outputs[0], inland.outputs[0],
                     location=(1700, 460))
    relief_full = math_node(tree, "ADD", relief.outputs[0], base.outputs[0],
                            location=(1700, 260))
    # Wide shore ramp: without it the land mask multiplies a 400 m relief into a
    # near-vertical wall at the waterline.
    shore = smoothstep(tree, coast_field, 0.30, 0.74, (1520, 60))
    elevation_land = math_node(tree, "MULTIPLY", relief_full.outputs[0], shore,
                               location=(1880, 260))

    terraced = _build_strata(tree, elevation_land.outputs[0],
                             combined, warped_uv, seed, preset)

    detail = noise(tree, warped_uv, preset.detail_scale, 4.0, 0.55, seed * 11.3,
                   (1880, 620))
    detail_centered = math_node(tree, "SUBTRACT", detail.outputs["Fac"], 0.5,
                                location=(2060, 620))
    detail_scaled = math_node(tree, "MULTIPLY", detail_centered.outputs[0],
                              preset.detail_strength, location=(2240, 620))
    detail_gated = math_node(tree, "MULTIPLY", detail_scaled.outputs[0], land,
                             location=(2420, 620))
    with_detail = math_node(tree, "ADD", terraced, detail_gated.outputs[0],
                            location=(2420, 400))
    height = map_range(tree, with_detail.outputs[0], 0.0, 1.06, 0.0, 1.0,
                       (2600, 400), interpolation="LINEAR")
    height_scaled = math_node(tree, "MULTIPLY", height.outputs["Result"],
                              float(height_scale_m), location=(2600, 200))
    combine = new_node(tree, "ShaderNodeCombineXYZ", (2600, 40))
    link(tree, height_scaled.outputs[0], combine.inputs["Z"])

    set_position = new_node(tree, "GeometryNodeSetPosition", (2600, -140))
    link(tree, grid.outputs["Mesh"], set_position.inputs["Geometry"])
    link(tree, combine.outputs["Vector"], set_position.inputs["Offset"])

    attributes = (
        ("vl_height", height.outputs["Result"], -320),
        ("vl_coast", land, -460),
        ("vl_relief", combined, -600),
        ("vl_inland", plateau, -740),
    )
    geometry = set_position.outputs["Geometry"]
    for name, socket, y in attributes:
        store = new_node(tree, "GeometryNodeStoreNamedAttribute", (2800, y))
        store.data_type = "FLOAT"
        store.domain = "POINT"
        store.inputs["Name"].default_value = name
        link(tree, geometry, store.inputs["Geometry"])
        link(tree, socket, store.inputs["Value"])
        geometry = store.outputs["Geometry"]
    link(tree, geometry, group_out.inputs["Geometry"])
    return tree


TERRAIN_ATTRIBUTES = ("vl_height", "vl_coast", "vl_relief", "vl_inland")
