from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

# Bistro's Godot source still ships legacy *_Specular.dds files. The .tres
# files author them as channel-packed roughness/metallic sources:
#   roughness_texture_channel -> G (1)
#   metallic_texture_channel  -> B (2)
# This converter repacks those channels into VulkanLearn's linear pbrParamMap:
#   R=roughness, G=metallic, B=AO, A=reserved.


def canon(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]+", "_", value).strip("_")


def read_scalar(text: str, field: str, default: float) -> float:
    match = re.search(rf"^{re.escape(field)}\s*=\s*([-+0-9.eE]+)\s*$", text, flags=re.MULTILINE)
    return float(match.group(1)) if match else default


def read_ext_resources(text: str) -> dict[str, str]:
    resources: dict[str, str] = {}
    for match in re.finditer(
        r'^\[ext_resource[^\]]+path="([^"]+)"[^\]]+id="([^"]+)"\]',
        text,
        flags=re.MULTILINE,
    ):
        resources[match.group(2)] = match.group(1)
    return resources


def read_texture_field(text: str, ext_resources: dict[str, str], field: str) -> str | None:
    match = re.search(
        rf'^{re.escape(field)}\s*=\s*ExtResource\("([^"]+)"\)\s*$',
        text,
        flags=re.MULTILINE,
    )
    if not match:
        return None
    return ext_resources.get(match.group(1))


def read_channel(text: str, field: str, default: int = 0) -> int:
    match = re.search(rf"^{re.escape(field)}\s*=\s*(\d+)\s*$", text, flags=re.MULTILINE)
    return int(match.group(1)) if match else default


def is_legacy_specular_resource(resource_path: str | None) -> bool:
    if not resource_path:
        return False
    return resource_path.lower().endswith("_specular.dds")


def texture_asset_base(resource_path: str) -> str:
    file_name = resource_path.rsplit("/", 1)[-1]
    stem = file_name.rsplit(".", 1)[0]
    return re.sub(
        r"_(basecolor|normal|specular|roughness|metallic|ao|emissive)$",
        "",
        stem,
        flags=re.IGNORECASE,
    )


def load_channel_image(stage_root: Path, resource_path: str, channel: int):
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError(
            "Pillow is required to convert Godot roughness/metallic textures. "
            "Use the Codex bundled Python runtime or install Pillow."
        ) from exc

    relative = resource_path.removeprefix("res://")
    image = Image.open(stage_root / relative).convert("RGBA")
    components = image.split()
    if channel < 0 or channel > 3:
        raise RuntimeError(f"Unsupported Godot texture channel {channel}: {resource_path}")
    return components[channel]


def scale_channel(channel_image, factor: float):
    return channel_image.point(lambda value: round(value * factor))


def build_packed_map(
    stage_root: Path,
    output_path: Path,
    roughness: float,
    metallic: float,
    roughness_texture: str | None,
    roughness_channel: int,
    metallic_texture: str | None,
    metallic_channel: int,
) -> None:
    from PIL import Image

    sources = []
    roughness_image = None
    metallic_image = None
    # A source can be a *_Specular.dds; only the Godot-authored channel is read.
    if roughness_texture:
        roughness_image = load_channel_image(stage_root, roughness_texture, roughness_channel)
        sources.append(roughness_image)
    if metallic_texture:
        metallic_image = load_channel_image(stage_root, metallic_texture, metallic_channel)
        sources.append(metallic_image)

    if sources:
        source_width = max(image.width for image in sources)
        source_height = max(image.height for image in sources)
        scale = min(1.0, 512.0 / max(source_width, source_height))
        width = max(1, round(source_width * scale))
        height = max(1, round(source_height * scale))
    else:
        width = 16
        height = 16
    resampling = Image.Resampling.LANCZOS

    if roughness_image is None:
        roughness_image = Image.new("L", (width, height), round(255.0 * roughness))
    else:
        roughness_image = roughness_image.resize((width, height), resampling)
        roughness_image = scale_channel(roughness_image, roughness)

    if metallic_image is None:
        metallic_image = Image.new("L", (width, height), round(255.0 * metallic))
    else:
        metallic_image = metallic_image.resize((width, height), resampling)
        metallic_image = scale_channel(metallic_image, metallic)

    ambient_occlusion = Image.new("L", (width, height), 255)
    alpha = Image.new("L", (width, height), 255)
    packed = Image.merge("RGBA", (roughness_image, metallic_image, ambient_occlusion, alpha))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    packed.save(output_path)


def build_materials(
    resource_root: Path,
    stage_root: Path,
    material_mapping: dict[str, str],
    mi_by_slot: dict[str, Path],
    force: bool = False,
) -> dict[str, str]:
    material_output = resource_root / "materials/bistro_modular"
    texture_output = resource_root / "textures/bistro_modular"
    texture_data_output = resource_root / "textures/datas/bistro_modular/pbr"

    if force:
        import shutil

        if material_output.exists():
            shutil.rmtree(material_output)
        if texture_data_output.exists():
            shutil.rmtree(texture_data_output)
        if texture_output.exists():
            for descriptor_path in texture_output.glob("T_bistro_modular_*_Param.json"):
                descriptor_path.unlink()

    material_output.mkdir(parents=True, exist_ok=True)
    texture_output.mkdir(parents=True, exist_ok=True)

    runtime_mapping: dict[str, str] = {}
    slot_mapping: dict[str, str] = {}
    packed_count = 0
    constant_count = 0
    legacy_specular_materials: list[str] = []

    for material_path in sorted((stage_root / "Materials").glob("**/*.tres")):
        material_name = material_path.stem
        base_slot = material_mapping.get(material_name)
        if base_slot is None:
            continue
        base_path = mi_by_slot[base_slot]
        material_instance: dict[str, Any] = json.loads(base_path.read_text(encoding="utf-8"))
        text = material_path.read_text(encoding="utf-8", errors="ignore")
        ext_resources = read_ext_resources(text)

        roughness = read_scalar(text, "roughness", 1.0)
        metallic = read_scalar(text, "metallic", 0.0)
        roughness_texture = read_texture_field(text, ext_resources, "roughness_texture")
        metallic_texture = read_texture_field(text, ext_resources, "metallic_texture")
        roughness_channel = read_channel(text, "roughness_texture_channel", 0)
        metallic_channel = read_channel(text, "metallic_texture_channel", 0)
        legacy_specular = (
            is_legacy_specular_resource(roughness_texture)
            or is_legacy_specular_resource(metallic_texture)
        )
        if legacy_specular:
            legacy_specular_materials.append(material_name)

        safe_name = canon(material_name)
        source_asset = (
            texture_asset_base(metallic_texture or roughness_texture)
            if metallic_texture or roughness_texture
            else safe_name
        )
        texture_asset_name = canon(source_asset)
        material_instance["name"] = f"Bistro Modular {material_name}"
        material_instance.setdefault("macros", {})
        material_instance.setdefault("textures", {})
        material_instance.setdefault("parameters", {})
        material_instance["parameters"]["u_pbrFactors"] = [roughness, metallic, 1.0, 0.0]

        if roughness_texture or metallic_texture:
            data_name = f"{texture_asset_name}_Param.png"
            data_path = texture_data_output / data_name
            build_packed_map(
                stage_root,
                data_path,
                roughness,
                metallic,
                roughness_texture,
                roughness_channel,
                metallic_texture,
                metallic_channel,
            )
            descriptor_name = f"T_bistro_modular_{texture_asset_name}_Param.json"
            descriptor_path = texture_output / descriptor_name
            descriptor = {
                "name": f"T_bistro_modular_{texture_asset_name}_Param",
                "type": "texture",
                "source": f"textures/datas/bistro_modular/pbr/{data_name}",
                "colorSpace": "linear",
                "mipmaps": True,
                "filter": "linear",
                "wrapMode": "repeat",
            }
            descriptor_path.write_text(json.dumps(descriptor, indent=2) + "\n", encoding="utf-8")
            material_instance["macros"]["USE_PBR_MAP"] = 1
            material_instance["textures"]["pbrParamMap"] = f"textures/bistro_modular/{descriptor_name}"
            packed_count += 1
        else:
            material_instance["macros"]["USE_PBR_MAP"] = 0
            material_instance["textures"].pop("pbrParamMap", None)
            constant_count += 1
        output_name = f"MI_{safe_name}.json"
        output_path = material_output / output_name
        output_path.write_text(json.dumps(material_instance, indent=2) + "\n", encoding="utf-8")
        runtime_mapping[material_name] = f"materials/bistro_modular/{output_name}"
        slot_mapping.setdefault(base_slot, runtime_mapping[material_name])

    report = {
        "materials": len(runtime_mapping),
        "constant_pbr_materials": constant_count,
        "packed_pbr_materials": packed_count,
        "legacy_specular_pbr_materials": legacy_specular_materials,
        "mapping": runtime_mapping,
        "slot_mapping": slot_mapping,
    }
    report_path = stage_root / "material_runtime_mapping.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return slot_mapping
