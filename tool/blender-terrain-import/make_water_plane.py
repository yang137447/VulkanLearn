"""Generate the flat ocean plane mesh used by the singleWaterLayer pass.

The mesh is authored directly in world units (centimetres) so the scene object
needs no scale. Geometry stays flat: the water material perturbs the shading
normal only, so a coarse grid is enough and keeps the horizon far away without
adding vertex cost.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def build_plane(half_extent_cm: float, segments: int) -> tuple[list, list, list]:
    positions: list[tuple[float, float, float]] = []
    tex_coords: list[tuple[float, float]] = []
    step = (half_extent_cm * 2.0) / segments
    for row in range(segments + 1):
        z = -half_extent_cm + step * row
        for column in range(segments + 1):
            x = -half_extent_cm + step * column
            positions.append((x, 0.0, z))
            tex_coords.append((column / segments, row / segments))

    faces: list[tuple[int, int, int, int]] = []
    stride = segments + 1
    for row in range(segments):
        for column in range(segments):
            v0 = row * stride + column
            v1 = v0 + 1
            v2 = v0 + stride + 1
            v3 = v0 + stride
            # Clockwise seen from above: the mesh loader flips CCW->CW for
            # Vulkan front-face, so emitting world-CW here lands on the
            # engine's front-facing side for a +Y facing plane.
            faces.append((v0, v3, v2, v1))
    return positions, tex_coords, faces


def write_obj(
    output: Path,
    positions: list[tuple[float, float, float]],
    tex_coords: list[tuple[float, float]],
    faces: list[tuple[int, int, int, int]],
    material_name: str,
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Flat ocean plane for the singleWaterLayer pass.",
        "# Units: centimetres, +Y up, faces wound CW seen from above.",
        f"mtllib {output.stem}.mtl",
        "o water_plane",
    ]
    for x, y, z in positions:
        lines.append(f"v {x:.3f} {y:.3f} {z:.3f}")
    for u, v in tex_coords:
        lines.append(f"vt {u:.6f} {v:.6f}")
    lines.append("vn 0.000000 1.000000 0.000000")
    lines.append(f"usemtl {material_name}")
    lines.append("s off")
    normal_index = 1
    for face in faces:
        corners = " ".join(
            f"{index + 1}/{index + 1}/{normal_index}" for index in face
        )
        lines.append(f"f {corners}")
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")

    mtl = output.with_suffix(".mtl")
    mtl.write_text(
        "\n".join(
            [
                "# Placeholder material; the scene binds MI_water.json instead.",
                "newmtl " + material_name,
                "Kd 0.050 0.180 0.240",
                "Ks 0.000 0.000 0.000",
                "d 1.0",
                "illum 1",
                "",
            ]
        ),
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="destination .obj path inside the resources worktree",
    )
    parser.add_argument(
        "--half-extent-cm",
        type=float,
        default=800000.0,
        help="half size of the square plane in centimetres (default 8 km)",
    )
    parser.add_argument("--segments", type=int, default=16)
    parser.add_argument("--material-name", default="Water")
    args = parser.parse_args()

    positions, tex_coords, faces = build_plane(
        args.half_extent_cm, args.segments
    )
    write_obj(args.output, positions, tex_coords, faces, args.material_name)
    print(
        f"wrote {args.output} "
        f"({len(positions)} verts, {len(faces)} quads, "
        f"{args.half_extent_cm * 2.0 / 100.0:.1f} m across)"
    )


if __name__ == "__main__":
    main()
