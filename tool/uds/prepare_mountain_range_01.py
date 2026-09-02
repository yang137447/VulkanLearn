from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


def build_packed_pbr(resource_root: Path) -> Path:
    texture_root = resource_root / "Maps/SC_uds_mountain_range/Source/Textures/uds/mountain_range_01"
    roughness_path = texture_root / "roughness_range01.jpg"
    ambient_occlusion_path = texture_root / "ambientOcclusion_range01.jpg"
    output_path = texture_root / "pbrParam_range01.png"

    roughness = Image.open(roughness_path).convert("L")
    ambient_occlusion = Image.open(ambient_occlusion_path).convert("L")
    if roughness.size != ambient_occlusion.size:
        raise RuntimeError(
            "Mountain Range roughness and ambient occlusion dimensions do not match"
        )

    metallic = Image.new("L", roughness.size, 0)
    alpha = Image.new("L", roughness.size, 255)
    packed = Image.merge("RGBA", (roughness, metallic, ambient_occlusion, alpha))
    packed.info.clear()
    packed.save(output_path, optimize=True)
    return output_path


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Pack Mountain Range 01 roughness and AO for VulkanLearn."
    )
    parser.add_argument(
        "--resource-root",
        type=Path,
        required=True,
        help="Path to the VukanLearnResources repository root.",
    )
    args = parser.parse_args()

    output_path = build_packed_pbr(args.resource_root.resolve())
    print(output_path)


if __name__ == "__main__":
    main()
