#!/usr/bin/env python3
"""Generate standard cloth-showcase scenes with a swappable model slot."""

from __future__ import annotations

import json
import math
from pathlib import Path


CODE_ROOT = Path(__file__).resolve().parents[2]
ASSETS_ROOT = CODE_ROOT.parent / "VukanLearnResources"
SHOWCASE = ASSETS_ROOT / "Maps" / "SC_cloth_showcase"

# Stage constants shared by the floor and wall meshes.
#
# The backdrop is intentionally two perpendicular planes, not a cyclorama:
# the floor is a flat horizontal plane ending on the wall plane, and the wall is
# a flat vertical plane. Their 90 degree crease is the visible studio seam and
# must stay. The wall is extended 50 cm below the floor so the corner is
# watertight from any camera above the floor, but that skirt is hidden by the
# floor and never shows up as a second line.
STAGE_HALF_WIDTH = 1200.0
FLOOR_FRONT_Z = 1500.0
WALL_PLANE_Z = -500.0
WALL_SKIRT_Y = -50.0
WALL_TOP_Y = 700.0
# Dress origin is its bottom centre; keep it close to the wall so the key light
# throws the body shadow onto the backdrop.
MODEL_Z = -150.0
# Shading-model comparison row: four copies of the same outfit, left to right
# Satin / Velvet / Slanted Velvet / DefaultLit PBR.
MODEL_SPACING_X = 95.0
CAMERA_POSITION = (0.0, 85.0, 182.0)
CAMERA_LOOK_AT = (0.0, 85.0, -420.0)
SUN_POSITION = (-1500.0, 1100.0, 1800.0)
SUN_ROTATION = (-20.0, -33.11, 0.0)
SUN_FLAG_Z = 800.0
WALL_LIGHT_LEFT_X = -166.0
WALL_LIGHT_TOP_LEFT_Y = 243.0
WALL_LIGHT_END_X = 347.0
WALL_LIGHT_TOP_SLOPE = -WALL_LIGHT_TOP_LEFT_Y / (
    WALL_LIGHT_END_X - WALL_LIGHT_LEFT_X
)
WALL_LIGHT_TOP_Y = -WALL_LIGHT_TOP_SLOPE * WALL_LIGHT_END_X


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


CLOTH_LUT = "Common/Textures/T_clothAshikhminLut.json"
# Final fabric tint shared by all four variants. 1.0 (the raw texture) renders
# as almost white and flattens the sheen comparison into the tone curve shoulder.
CLOTH_ALBEDO = 0.62
SHOWCASE_MATERIALS = "Maps/SC_cloth_showcase/Materials"
SHOWCASE_MESHES = "Maps/SC_cloth_showcase/Meshes"
DRESS_FBX = "Maps/SC_cloth_showcase/Source/Models/SM_cloth_3d66Dress.fbx"

# Fabric slots and their source maps. Only these two slots follow the shading
# model; body, limbs, shoes and hat band stay PBR on every variant so the
# comparison isolates the garment closure.
FABRIC_SLOTS = {
    "m04": {
        "albedo": "Maps/SC_cloth_showcase/Textures/T_clothDress_m04_albedo_01.json",
        "normal": "Maps/SC_cloth_showcase/Textures/T_clothDress_m04_normal_01.json",
        "pbrRoughness": 0.55,
    },
    "m07": {
        "albedo": "Maps/SC_cloth_showcase/Textures/T_clothDress_m07_albedo_01.json",
        "normal": None,
        "pbrRoughness": 0.65,
    },
}

# Left-to-right order of the comparison row.
CLOTH_VARIANTS = [
    ("satin", "M_clothAshikhminSatin"),
    ("velvet", "M_clothAshikhminVelvet"),
    ("slantedVelvet", "M_clothAshikhminSlantedVelvet"),
    ("pbr", "M_pbr"),
]

BASE_MATERIAL_SLOTS = [
    ("m01", "MI_cloth_3d66Dress_m01.json"),
    ("m02", "MI_cloth_3d66Dress_m02.json"),
    ("m03", "MI_cloth_3d66Dress_m03.json"),
    ("m04", "MI_cloth_3d66Dress_m04.json"),
    ("m05", "MI_cloth_3d66Dress_m05.json"),
    ("m06", "MI_cloth_3d66Dress_m06.json"),
    ("m07", "MI_cloth_3d66Dress_m07.json"),
]


def write_cloth_variant_assets() -> None:
    materials = SHOWCASE / "Materials"
    meshes = SHOWCASE / "Meshes"
    for variant_id, material in CLOTH_VARIANTS:
        slot_paths = dict(BASE_MATERIAL_SLOTS)
        for slot_name, slot in FABRIC_SLOTS.items():
            instance_name = f"MI_cloth_3d66Dress_{variant_id}_{slot_name}"
            payload = {
                "name": instance_name,
                "type": "materialInstance",
                "material": f"shader/glsl/{material}.json",
                "macros": {"USE_ALBEDO_MAP": 1},
                "textures": {"albedoMap": slot["albedo"]},
                "parameters": {},
            }
            if slot["normal"] is not None:
                payload["macros"]["USE_NORMAL_MAP"] = 1
                payload["textures"]["normalMap"] = slot["normal"]
            if variant_id == "pbr":
                payload["parameters"] = {
                    # Keep the PBR control at the same base level as the cloth
                    # variants: a near-white albedo pushes every sheen
                    # difference into the top of the tone curve.
                    "u_tintColor": [CLOTH_ALBEDO, CLOTH_ALBEDO, CLOTH_ALBEDO, 1.0],
                    "u_pbrFactors": [slot["pbrRoughness"], 0.0, 1.0, 0.0],
                    "u_specular": 0.35,
                }
            else:
                payload["textures"]["clothLut"] = CLOTH_LUT
                payload["parameters"] = {
                    "u_baseColor": [
                        CLOTH_ALBEDO,
                        CLOTH_ALBEDO,
                        CLOTH_ALBEDO,
                        1.0,
                    ],
                }
            write_json(materials / f"{instance_name}.json", payload)
            slot_paths[slot_name] = f"{instance_name}.json"

        write_json(
            meshes / f"SM_cloth_3d66Dress_{variant_id}.json",
            {
                "name": f"3d66 dress - {variant_id}",
                "type": "mesh",
                "modelDataPath": DRESS_FBX,
                "materialSlots": [
                    {
                        "name": slot_name,
                        "materialInstancePath": f"{SHOWCASE_MATERIALS}/{file_name}",
                    }
                    for slot_name, file_name in slot_paths.items()
                ],
            },
        )


def stage_assets() -> None:
    materials = SHOWCASE / "Materials"
    meshes = SHOWCASE / "Meshes"
    write_json(
        materials / "MI_cloth_showcase_floor.json",
        {
            "name": "MI_cloth_showcase_floor",
            "type": "materialInstance",
            "material": "shader/glsl/M_pbr.json",
            "parameters": {
                # 影棚墙地是同一层漆：地面只比墙略亮，亮度差主要来自朝向和
                # 反射，不是 albedo。否则两块平面之间会出现一条硬亮带。
                "u_tintColor": [0.178, 0.154, 0.131, 1.0],
                "u_pbrFactors": [0.16, 0.0, 1.0, 0.0],
                "u_specular": 0.6,
            },
        },
    )
    write_json(
        materials / "MI_cloth_showcase_wall.json",
        {
            "name": "MI_cloth_showcase_wall",
            "type": "materialInstance",
            "material": "shader/glsl/M_pbr.json",
            "parameters": {
                "u_tintColor": [0.182, 0.158, 0.134, 1.0],
                "u_pbrFactors": [0.88, 0.0, 1.0, 0.0],
                "u_specular": 0.25,
            },
        },
    )
    write_floor(SHOWCASE / "Source" / "Models" / "showcase_floor.obj")
    write_wall(SHOWCASE / "Source" / "Models" / "showcase_wall.obj")
    write_gobo(SHOWCASE / "Source" / "Models" / "showcase_gobo.obj")
    write_json(
        materials / "MI_cloth_showcase_gobo.json",
        {
            "name": "MI_cloth_showcase_gobo",
            "type": "materialInstance",
            "material": "shader/glsl/M_pbr.json",
            "parameters": {
                "u_tintColor": [0.02, 0.02, 0.02, 1.0],
                "u_pbrFactors": [0.95, 0.0, 1.0, 0.0],
                "u_specular": 0.1,
            },
        },
    )
    write_json(
        meshes / "SM_cloth_showcase_gobo.json",
        {
            "name": "SM_cloth_showcase_gobo",
            "type": "mesh",
            "modelDataPath": (
                "Maps/SC_cloth_showcase/Source/Models/showcase_gobo.obj"
            ),
            "materialSlots": [
                {
                    "name": "gobo",
                    "materialInstancePath": (
                        "Maps/SC_cloth_showcase/Materials/"
                        "MI_cloth_showcase_gobo.json"
                    ),
                }
            ],
        },
    )
    write_cloth_variant_assets()
    write_json(
        meshes / "SM_cloth_showcase_floor.json",
        {
            "name": "SM_cloth_showcase_floor",
            "type": "mesh",
            "modelDataPath": (
                "Maps/SC_cloth_showcase/Source/Models/showcase_floor.obj"
            ),
            "materialSlots": [
                {
                    "name": "floor",
                    "materialInstancePath": (
                        "Maps/SC_cloth_showcase/Materials/"
                        "MI_cloth_showcase_floor.json"
                    ),
                }
            ],
        },
    )
    write_json(
        meshes / "SM_cloth_showcase_wall.json",
        {
            "name": "SM_cloth_showcase_wall",
            "type": "mesh",
            "modelDataPath": (
                "Maps/SC_cloth_showcase/Source/Models/showcase_wall.obj"
            ),
            "materialSlots": [
                {
                    "name": "wall",
                    "materialInstancePath": (
                        "Maps/SC_cloth_showcase/Materials/"
                        "MI_cloth_showcase_wall.json"
                    ),
                }
            ],
        },
    )


def write_floor(path: Path) -> None:
    """Flat horizontal floor, ending exactly on the wall plane."""
    x_values = [
        (-STAGE_HALF_WIDTH + index * 2.0 * STAGE_HALF_WIDTH / 32.0)
        for index in range(33)
    ]
    # (v, world Z): v only exists so the OBJ keeps a stable UV direction.
    rows = [(0.0, FLOOR_FRONT_Z), (1.0, WALL_PLANE_Z)]
    vertices = []
    uvs = []
    for x in x_values:
        u = (x + STAGE_HALF_WIDTH) / (2.0 * STAGE_HALF_WIDTH)
        for v, z in rows:
            vertices.append((x, 0.0, z))
            uvs.append((u, v))

    with path.open("w", encoding="ascii", newline="\n") as handle:
        handle.write("# Standard cloth showcase floor plane, Y-up, units in cm.\n")
        handle.write("# Two perpendicular planes: the crease is intentional.\n")
        handle.write("o showcase_floor\n")
        for vertex in vertices:
            handle.write(f"v {vertex[0]:.6f} {vertex[1]:.6f} {vertex[2]:.6f}\n")
        for uv in uvs:
            handle.write(f"vt {uv[0]:.6f} {uv[1]:.6f}\n")
        for _ in vertices:
            handle.write("vn 0.000000 1.000000 0.000000\n")
        for column in range(len(x_values) - 1):
            a = column * 2 + 1
            b = (column + 1) * 2 + 1
            c = (column + 1) * 2 + 2
            d = column * 2 + 2
            handle.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")
            handle.write(f"f {a}/{a}/{a} {c}/{c}/{c} {d}/{d}/{d}\n")


def write_wall(path: Path) -> None:
    """Flat vertical wall on z = WALL_PLANE_Z, skirt hidden under the floor."""
    path.parent.mkdir(parents=True, exist_ok=True)
    x_values = [
        (-STAGE_HALF_WIDTH + index * 2.0 * STAGE_HALF_WIDTH / 32.0)
        for index in range(33)
    ]
    y_values = [WALL_SKIRT_Y, 0.0, 350.0, WALL_TOP_Y]
    uv_span = WALL_TOP_Y
    with path.open("w", encoding="ascii", newline="\n") as handle:
        handle.write("# Standard cloth showcase wall plane, Y-up, units in cm.\n")
        handle.write("# The crease with the floor is intentional; see builder.\n")
        handle.write("o showcase_wall\n")
        for x in x_values:
            for y in y_values:
                handle.write(f"v {x:.6f} {y:.6f} {WALL_PLANE_Z:.6f}\n")
        for x in x_values:
            for y in y_values:
                u = (x + STAGE_HALF_WIDTH) / (2.0 * STAGE_HALF_WIDTH)
                handle.write(f"vt {u:.6f} {y / uv_span:.6f}\n")
        for _ in x_values:
            for _ in y_values:
                handle.write("vn 0.0 0.0 1.0\n")
        stride = len(y_values)
        for column in range(len(x_values) - 1):
            for row in range(stride - 1):
                a = column * stride + row + 1
                b = (column + 1) * stride + row + 1
                c = (column + 1) * stride + row + 2
                d = column * stride + row + 2
                handle.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")
                handle.write(f"f {a}/{a}/{a} {c}/{c}/{c} {d}/{d}/{d}\n")


def write_gobo(path: Path) -> None:
    """Back-project the wall's lit silhouette into two closed, off-camera flags."""
    path.parent.mkdir(parents=True, exist_ok=True)
    projection_fraction = (
        (SUN_FLAG_Z - SUN_POSITION[2]) / (WALL_PLANE_Z - SUN_POSITION[2])
    )

    def project_wall_point(wall_x: float, wall_y: float) -> tuple[float, float]:
        return (
            SUN_POSITION[0] + projection_fraction * (wall_x - SUN_POSITION[0]),
            SUN_POSITION[1] + projection_fraction * (wall_y - SUN_POSITION[1]),
        )

    edge_left = project_wall_point(
        -700.0, WALL_LIGHT_TOP_Y + WALL_LIGHT_TOP_SLOPE * -700.0
    )
    edge_right = project_wall_point(
        1000.0, WALL_LIGHT_TOP_Y + WALL_LIGHT_TOP_SLOPE * 1000.0
    )
    left_flag_edge = project_wall_point(WALL_LIGHT_LEFT_X, 0.0)[0]
    panels = [
        [edge_left, edge_right, (edge_right[0], 1600.0), (edge_left[0], 1600.0)],
        [
            (-1700.0, -400.0), (left_flag_edge, -400.0),
            (left_flag_edge, 1600.0), (-1700.0, 1600.0),
        ],
    ]
    corners = [
        (point[0], point[1], depth)
        for panel in panels
        for depth in (SUN_FLAG_Z - 2.0, SUN_FLAG_Z + 2.0)
        for point in panel
    ]
    box_faces = [
        (4, 3, 2, 1), (5, 6, 7, 8), (1, 2, 6, 5),
        (2, 3, 7, 6), (3, 4, 8, 7), (4, 1, 5, 8),
    ]
    faces = [
        tuple(index + panel_index * 8 for index in face)
        for panel_index in range(len(panels))
        for face in box_faces
    ]
    with path.open("w", encoding="ascii", newline="\n") as handle:
        handle.write("# Key-light gobo panel, Y-up, units in cm.\n")
        handle.write("o light_gobo\n")
        for x, y, z in corners:
            handle.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
        for face in faces:
            first, second, third = [corners[index - 1] for index in face[:3]]
            edge_a = [second[axis] - first[axis] for axis in range(3)]
            edge_b = [third[axis] - first[axis] for axis in range(3)]
            normal = [
                edge_a[1] * edge_b[2] - edge_a[2] * edge_b[1],
                edge_a[2] * edge_b[0] - edge_a[0] * edge_b[2],
                edge_a[0] * edge_b[1] - edge_a[1] * edge_b[0],
            ]
            length = math.sqrt(sum(value * value for value in normal))
            handle.write(
                "vn " + " ".join(f"{value / length:.6f}" for value in normal) + "\n"
            )
        for index, (a, b, c, d) in enumerate(faces, start=1):
            handle.write(
                f"f {a}//{index} {b}//{index} {c}//{index}\n"
                f"f {a}//{index} {c}//{index} {d}//{index}\n"
            )


def stage_objects(
    model_meshes: list[str],
    spacing: float,
    camera_position: tuple[float, float, float],
    camera_look_at: tuple[float, float, float],
) -> list[dict]:
    objects = [
        {
            "name": "ShowcaseFloor",
            "type": "mesh",
            "modelPath": "Maps/SC_cloth_showcase/Meshes/SM_cloth_showcase_floor.json",
            "position": [0.0, 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0],
        },
        {
            "name": "ShowcaseWall",
            "type": "mesh",
            "modelPath": "Maps/SC_cloth_showcase/Meshes/SM_cloth_showcase_wall.json",
            "position": [0.0, 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0],
        },
        {
            "name": "ShowcaseSunFlag",
            "type": "mesh",
            "modelPath": "Maps/SC_cloth_showcase/Meshes/SM_cloth_showcase_gobo.json",
            "position": [0.0, 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0],
        },
    ]
    half = (len(model_meshes) - 1) * 0.5
    for index, (variant_id, mesh_path) in enumerate(
        zip([v[0] for v in CLOTH_VARIANTS], model_meshes)
    ):
        objects.append(
            {
                "name": f"ShowcaseModel_{variant_id}",
                "type": "mesh",
                "modelPath": mesh_path,
                "position": [
                    (index - half) * spacing,
                    0.0,
                    MODEL_Z,
                ],
                "rotation": [0, 0, 0],
                "scale": [100.0, 100.0, 100.0],
            }
        )
    objects.extend([
        {
            # 影棚四层结构（对齐参考图的光影）：
            # 1) 大柔光主光的方向分量：不投影，负责整体塑形。
            #    它同时抬高阴影区，让下面那盏 spot 的影子变"软"。
            "name": "KeySoft",
            "type": "directionalLight",
            "position": [-260.0, 480.0, 160.0],
            "rotation": [-36.0, -22.0, 0.0],
            "color": [1.0, 0.97, 0.92],
            # 展示关卡当前只保留阳光：其余灯置 0 但保留对象，方便后续
            # 开发软阴影/补光时直接调回来。
            "intensity": 0.0,
            "intensityUnit": "lux",
            "shadow": {"castShadows": False},
        },
        {
            # 2) 同一方向的投影分量：只负责把影子落在墙上，
            #    因为 KeySoft 已经填了底，影子是灰的而不是纯黑。
            "name": "KeySpot",
            "type": "spotLight",
            "position": [-260.0, 590.0, 140.0],
            "rotation": [-37.0, -27.5, 0.0],
            "color": [1.0, 0.97, 0.92],
            "intensity": 0.0,
            "intensityUnit": "cd",
            "radius": 3500.0,
            "cone_angle_outer": 32.0,
            "cone_angle_inner": 14.0,
            "shadow": {"castShadows": True},
        },
        {
            "name": "SunPatch",
            "type": "spotLight",
            "position": list(SUN_POSITION),
            "rotation": list(SUN_ROTATION),
            "color": [1.0, 0.94, 0.84],
            "intensity": 850000.0,
            "intensityUnit": "cd",
            "radius": 8000.0,
            "cone_angle_outer": 35.0,
            "cone_angle_inner": 32.0,
            # 阳光必须投影：挡光结构靠它的 shadow map 才能切出光斑边界。
            "shadow": {"castShadows": True, "bias": 8.0},
        },
        {
            # 4) 地面洗光：垂直向下，只提亮地面，不把墙重新打平。
            "name": "FloorWash",
            "type": "spotLight",
            "position": [0.0, 420.0, 40.0],
            "rotation": [-90.0, 0.0, 0.0],
            "color": [1.0, 0.96, 0.9],
            "intensity": 0.0,
            "intensityUnit": "cd",
            "radius": 2000.0,
            "cone_angle_outer": 55.0,
            "cone_angle_inner": 30.0,
            "shadow": {"castShadows": False},
        },
        {
            # Low grazing light from the left. Ashikhmin sheen (satin's
            # anisotropic lobe, velvet's rim) only separates from plain PBR at
            # glancing angles, so the comparison row needs this to read.
            "name": "SheenLight",
            "type": "directionalLight",
            "position": [-700.0, 180.0, 260.0],
            "rotation": [-18.0, -78.0, 0.0],
            "color": [1.0, 0.95, 0.88],
            "intensity": 0.0,
            "intensityUnit": "lux",
            "shadow": {"castShadows": False},
        },
        {
            "name": "FrontFill",
            "type": "pointLight",
            "position": [0.0, 120.0, 80.0],
            "rotation": [0, 0, 0],
            "color": [1.0, 0.98, 0.95],
            "intensity": 0.0,
            "intensityUnit": "cd",
            "radius": 1200.0,
            "shadow": {"castShadows": False},
        },
        {
            "name": "Camera",
            "type": "camera",
            "fov": 35.0,
            "near_clip": 1.0,
            "far_clip": 50000.0,
            "position": list(camera_position),
            "look_at": list(camera_look_at),
            "rotation": [0, 0, 0],
            "scale": [1, 1, 1],
            "exposure": {
                "mode": "ev100",
                "ev100": 9.0,
                "aperture": 4.0,
                "shutterSeconds": 0.008,
                "iso": 100.0,
                "compensation": 0.0,
                "autoMinEv100": -10.0,
                "autoMaxEv100": 20.0,
            },
        },
        {
            "name": "ShowcaseEnvironment",
            "type": "environment",
            "environment": {
                "type": "proceduralSky",
                "cubeSize": 128,
                "intensity": 1.0,
                "skyParameters": {
                    "sunIntensity": 0.0,
                    "sunColor": [1.0, 0.96, 0.9],
                    "sunAngularRadius": 0.02,
                    "zenithColor": [0.015, 0.02, 0.03],
                    "horizonColor": [0.12, 0.13, 0.15],
                    "groundColor": [0.008, 0.009, 0.012],
                    "skyGradientExponent": 0.65,
                    "groundGradientExponent": 0.5,
                    "sunHaloExponent": 128.0,
                    "sunHaloStrength": 0.0,
                },
            },
        },
    ])
    return objects


def write_offline_showcase(interactive: dict) -> None:
    materials = SHOWCASE / "Materials"
    model_names = {
        "satin": "AshikhminSatin",
        "velvet": "AshikhminVelvet",
        "slantedVelvet": "AshikhminSlantedVelvet",
    }
    for variant, closure in model_names.items():
        realtime = json.loads(json.dumps(interactive))
        realtime["name"] = f"Cloth Showcase Direct Control - {variant}"
        realtime["description"] = (
            f"{variant} 实时直接光对照：保留实时材质与 SunPatch，环境关闭；"
            "用于离线布料诊断，非论文原图验证。"
        )
        realtime["showcase"]["defaultMaterial"] = variant
        for entry in realtime["objects"]:
            if entry["type"] == "environment":
                entry["environment"] = {"type": "none"}
        write_json(SHOWCASE / f"SC_cloth_showcase_{variant}_direct.json", realtime)
        mesh_path = SHOWCASE / "Meshes" / f"SM_cloth_3d66Dress_{variant}.json"
        mesh = json.loads(mesh_path.read_text(encoding="utf-8"))
        for slot in mesh["materialSlots"]:
            source_path = ASSETS_ROOT / slot["materialInstancePath"]
            source = json.loads(source_path.read_text(encoding="utf-8"))
            fabric = slot["name"] in FABRIC_SLOTS
            params = source.get("parameters", {})
            tint = params.get("u_baseColor", params.get("u_tintColor", [1, 1, 1, 1]))
            parameters = {"u_baseColor": tint}
            textures = {}
            albedo = source.get("textures", {}).get("albedoMap")
            if albedo:
                textures["baseColorMap"] = albedo
            if fabric:
                normal = source.get("textures", {}).get("normalMap")
                if normal:
                    textures["normalMap"] = normal
                parameters["u_f0"] = 0.05
                if variant == "satin":
                    parameters.update(
                        u_sigmaFlat=0.1, u_sigmaEnds=0.3, u_flatWeight=0.7,
                        u_fiberRotationDegrees=0.0, u_useImportedTangents=1.0,
                    )
                else:
                    parameters["u_sigma"] = 0.5
                if variant == "slantedVelvet":
                    parameters.update(u_slantDegrees=40.0, u_fiberRotationDegrees=0.0,
                                      u_useImportedTangents=1.0)
            name = source["name"] + "_offline"
            mother = f"shader/glsl/M_offline{closure if fabric else 'Lambert'}.json"
            definition = json.loads((CODE_ROOT / mother).read_text(encoding="utf-8"))
            parameters = {
                key: value for key, value in parameters.items()
                if value != definition["parameters"][key].get("default")
            }
            write_json(materials / f"{name}.json", {
                "name": name, "type": "materialInstance",
                "material": mother,
                "parameters": parameters, **({"textures": textures} if textures else {}),
            })
            slot["materialInstancePath"] = f"{SHOWCASE_MATERIALS}/{name}.json"
        mesh["name"] += " offline"
        offline_mesh = f"SM_cloth_3d66Dress_{variant}_offline.json"
        write_json(SHOWCASE / "Meshes" / offline_mesh, mesh)
        scene = json.loads(json.dumps(interactive))
        scene["objects"] = [
            entry for entry in scene["objects"] if entry["name"] != "ShowcaseReference"
        ]
        scene.pop("showcase", None)
        scene["name"] = f"Cloth Showcase Offline - {variant}"
        scene["description"] = (
            f"Ashikhmin 2000 {closure} 离线展示验证：复用实时人台、墙地和挡板几何，"
            "单聚光灯及贴图；布料显式使用导入切线，墙地和非布料槽使用 Lambert，环境关闭，"
            "不是论文原图复现，也不是整幅实时画面的严格数值对照。"
        )
        objects = []
        for entry in scene["objects"]:
            if entry["type"] == "environment":
                entry["environment"] = {"type": "none"}
            if entry["type"].endswith("Light") and entry["name"] != "SunPatch":
                continue
            if entry["name"].startswith("ShowcaseModel_"):
                if entry["name"] != f"ShowcaseModel_{variant}":
                    continue
                entry["modelPath"] = f"{SHOWCASE_MESHES}/{offline_mesh}"
            elif entry["type"] == "mesh":
                source_mesh = json.loads((ASSETS_ROOT / entry["modelPath"]).read_text(encoding="utf-8"))
                for slot in source_mesh["materialSlots"]:
                    source = json.loads(
                        (ASSETS_ROOT / slot["materialInstancePath"]).read_text(encoding="utf-8")
                    )
                    name = source["name"] + "_offline"
                    write_json(materials / f"{name}.json", {
                        "name": name, "type": "materialInstance",
                        "material": "shader/glsl/M_offlineLambert.json",
                        "parameters": {"u_baseColor": source["parameters"]["u_tintColor"]},
                    })
                    slot["materialInstancePath"] = f"{SHOWCASE_MATERIALS}/{name}.json"
                name = Path(entry["modelPath"]).stem + "_offline.json"
                write_json(SHOWCASE / "Meshes" / name, source_mesh)
                entry["modelPath"] = f"{SHOWCASE_MESHES}/{name}"
            if entry["type"] == "camera":
                entry["position"][0] = 0.0
                entry["look_at"][0] = 0.0
            objects.append(entry)
        scene["objects"] = objects
        write_json(SHOWCASE / f"SC_cloth_showcase_{variant}_offline.json", scene)


def main() -> int:
    stage_assets()
    manifest_path = SHOWCASE / "showcase_models.json"
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    else:
        manifest = {
            "stage": "standard studio",
            "models": [
                {
                    "id": "3d66Dress",
                    "displayName": "3d66 wide-brim dress",
                    "mesh": (
                        "Maps/SC_cloth_showcase/Meshes/"
                        "SM_cloth_3d66Dress.json"
                    ),
                }
            ],
        }
        write_json(manifest_path, manifest)

    for model in manifest["models"]:
        variant_meshes = [
            f"{SHOWCASE_MESHES}/SM_cloth_3d66Dress_{variant_id}.json"
            for variant_id, _ in CLOTH_VARIANTS
        ]

        # 关卡级展示场景：一个站位、四份同位置网格，运行时只保留一份可见。
        # 数据源就是本 JSON 的 showcase 块，World 解析后交给展示控制器。
        interactive_name = f"SC_cloth_showcase_{model['id']}.json"
        interactive = {
            "name": f"Cloth Showcase - {model['displayName']}",
            "type": "scene",
            "description": (
                "布料材质展示关卡：单人台，四套同位置变体（Satin / Velvet / "
                "Slanted Velvet / DefaultLit PBR）由关卡级 showcase 块声明，"
                "运行时切换可见性并驱动右侧参数面板。不是论文像素级验证场景。"
            ),
            "objects": stage_objects(
                variant_meshes,
                0.0,
                # 单人构图：距离约 700 cm，170 cm 人台占画面高度约 65%，
                # 与参考图一致（头顶留白更多、脚底不被胶囊条压住）。
                # 视点略低于人台中线，把脚底抬到胶囊条上方，避免鞋被底部 UI 压住。
                (54.0, 72.0, 620.0),
                (54.0, 74.0, MODEL_Z),
            ),
            "showcase": {
                "kind": "cloth",
                "defaultMaterial": "satin",
                "rotatePivot": [0.0, 0.0, MODEL_Z],
                "cameraObject": "Camera",
                "materials": [
                    {
                        "id": variant_id,
                        "label": label,
                        "tag": tag,
                        "object": f"ShowcaseModel_{variant_id}",
                        "sheenHex": sheen_hex,
                    }
                    for variant_id, label, tag, sheen_hex in (
                        ("satin", "Satin 缎面", "CLOTH · ASHIKHMIN 2000", "#FFF8EB"),
                        ("velvet", "Velvet 绒面", "CLOTH · ASHIKHMIN 2000", "#FFF3E0"),
                        (
                            "slantedVelvet",
                            "Slanted Velvet 斜绒",
                            "CLOTH · ASHIKHMIN 2000",
                            "#FFF8EB",
                        ),
                        ("pbr", "Default PBR", "DEFAULT LIT · PBR", "#FFF8EB"),
                    )
                ],
            },
            "worldUnit": "cm",
        }
        interactive["objects"].append({
            "name": "ShowcaseReference",
            "type": "mesh",
            "modelPath": "Maps/SC_lookdev_article/Meshes/SM_ue_color_calibrator.json",
            "position": [95.0, 30.0, MODEL_Z],
            "rotation": [-90.0, 0.0, 0.0],
            "scale": [90.0, 90.0, 90.0],
        })
        write_json(SHOWCASE / interactive_name, interactive)
        write_offline_showcase(interactive)
        model["scene"] = f"Maps/SC_cloth_showcase/{interactive_name}"
        print(f"generated {SHOWCASE / interactive_name}")

        # C# 版本：同一关卡数据，多一个蓝图实例承接 UI 事件。
        # 主场景保持无蓝图，保证 C# OFF 的构建仍能跑展示。
        csharp_name = f"SC_cloth_showcase_{model['id']}_csharp.json"
        csharp = json.loads(json.dumps(interactive))
        csharp["name"] = f"Cloth Showcase (C#) - {model['displayName']}"
        csharp["description"] = (
            "布料材质展示关卡（C# 驱动）：与主场景同一套 showcase 数据，"
            "额外的蓝图实例把 RmlUi 事件翻译成展示状态请求；"
            "需要启用 C# 宿主。不是论文像素级验证场景。"
        )
        csharp["objects"].append(
            {
                "type": "blueprintInstance",
                "id": "cloth-showcase-controller",
                "name": "ClothShowcaseController",
                "class": "Gameplay.ClothShowcaseBlueprint",
                "enabled": True,
                "position": [0.0, 0.0, 0.0],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "properties": {},
                "components": [],
            }
        )
        write_json(SHOWCASE / csharp_name, csharp)
        print(f"generated {SHOWCASE / csharp_name}")

        # 保留一份四连排对照场景，方便一次性看四种材质。
        compare_name = f"SC_cloth_showcase_compare_{model['id']}.json"
        compare = {
            "name": f"Cloth Showcase Compare - {model['displayName']}",
            "type": "scene",
            "description": (
                "布料材质四连排对照：从左到右 Satin、Velvet、Slanted Velvet、"
                "DefaultLit PBR，同一舞台同一灯光，用于快速比较三种实时布料闭包"
                "与普通 PBR 的差异。不是论文像素级验证场景。"
            ),
            "objects": stage_objects(
                variant_meshes,
                MODEL_SPACING_X,
                (0.0, 85.0, 182.0),
                (0.0, 85.0, -420.0),
            ),
            "worldUnit": "cm",
        }
        write_json(SHOWCASE / compare_name, compare)
        print(f"generated {SHOWCASE / compare_name}")

    write_json(manifest_path, manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
