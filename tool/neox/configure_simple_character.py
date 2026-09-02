# -*- coding: utf-8 -*-

"""Generate a static VulkanLearn character package from NeoX high MTG files."""

from __future__ import print_function

import argparse
import hashlib
import io
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import OrderedDict

import numpy as np
from PIL import Image


CHARACTER_NAME = "simple_character"
SOURCE_ROOT_DEFAULT = r"K:\future\res"
RESOURCE_ROOT_DEFAULT = r"D:\YYBWorkSpace\GitHub\VukanLearnResources"
GLTF_RELATIVE = "Maps/SC_simple_character/Source/Models/simple_character/simple_character.gltf"
AUDIT_RELATIVE = "Maps/SC_simple_character/Source/Models/simple_character/simple_character.audit.json"
MODEL_RELATIVE = "Maps/SC_simple_character/Meshes/SM_simple_character.json"
HAIR_CORE_GLTF_RELATIVE = "Maps/SC_simple_character/Source/Models/simple_character/simple_character_hair_core.gltf"
HAIR_CORE_MODEL_RELATIVE = "Maps/SC_simple_character/Meshes/SM_simple_character_hair_core.json"
GROUND_OBJ_RELATIVE = "Maps/SC_simple_character/Source/Models/simple_character/simple_character_ground.obj"
GROUND_MTL_RELATIVE = "Maps/SC_simple_character/Source/Models/simple_character/simple_character_ground.mtl"
GROUND_MODEL_RELATIVE = "Maps/SC_simple_character/Meshes/SM_simple_character_ground.json"
GROUND_MI_RELATIVE = "Maps/SC_simple_character/Materials/MI_simple_character_ground.json"
SCENE_RELATIVE = "Maps/SC_simple_character/SC_simple_character.json"
MATERIAL_RELATIVE_ROOT = "Maps/SC_simple_character/Materials"
TEXTURE_RELATIVE_ROOT = "Maps/SC_simple_character/Textures"
TEXTURE_SOURCE_RELATIVE_ROOT = "Maps/SC_simple_character/Source/Textures"
GENERATED_RELATIVE_ROOT = "Generated/Import/simple_character"


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Configure the simple_character NeoX static character package."
    )
    parser.add_argument("--source-root", default=SOURCE_ROOT_DEFAULT)
    parser.add_argument("--resource-root", default=RESOURCE_ROOT_DEFAULT)
    parser.add_argument("--overwrite", action="store_true")
    return parser.parse_args()


def read_json(path):
    with io.open(path, "r", encoding="utf-8") as stream:
        return json.load(stream)


def normalize_json_text(value):
    if isinstance(value, OrderedDict):
        return OrderedDict((normalize_json_text(key), normalize_json_text(item)) for key, item in value.items())
    if isinstance(value, dict):
        return dict((normalize_json_text(key), normalize_json_text(item)) for key, item in value.items())
    if isinstance(value, list):
        return [normalize_json_text(item) for item in value]
    if isinstance(value, tuple):
        return tuple(normalize_json_text(item) for item in value)
    if isinstance(value, str):
        return value.decode("utf-8")
    return value


def write_json(path, value, overwrite):
    if os.path.exists(path) and not overwrite:
        raise RuntimeError("Output exists: %s" % path)
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    serialized = json.dumps(normalize_json_text(value), ensure_ascii=False, indent=4)
    if not isinstance(serialized, unicode):
        serialized = serialized.decode("utf-8")
    with io.open(path, "w", encoding="utf-8") as stream:
        stream.write(serialized)
        stream.write(u"\n")


def write_text(path, value, overwrite):
    if os.path.exists(path) and not overwrite:
        raise RuntimeError("Output exists: %s" % path)
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    if not isinstance(value, unicode):
        value = value.decode("utf-8")
    with io.open(path, "w", encoding="utf-8") as stream:
        stream.write(value)


def parse_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def parse_color(value, default):
    if not value:
        return list(default)
    parts = [parse_float(part) for part in value.split(",")]
    if len(parts) != 4:
        return list(default)
    return parts


def source_path(source_root, relative_path):
    return os.path.join(source_root, relative_path.replace("/", os.sep).replace("\\", os.sep))


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def normalize_source_reference(value):
    return value.replace("\\", "/") if value else None


def collect_source_materials(source_root):
    candidates = {}
    scan_roots = [
        os.path.join(source_root, "character", "players2021", "b_f_3694"),
        os.path.join(source_root, "character", "players2021", "h_f_4047"),
        os.path.join(source_root, "character", "players2021", "nf2022_f_01"),
    ]
    for scan_root in scan_roots:
        for directory, _, filenames in os.walk(scan_root):
            for filename in filenames:
                if not filename.lower().endswith(".mtg"):
                    continue
                path = os.path.join(directory, filename)
                document = ET.parse(path).getroot()
                priority = 0 if "high001" in filename.lower() else (1 if "_high" in filename.lower() else 2)
                for material in document.findall(".//Material"):
                    name = material.get("Name")
                    if name:
                        candidates.setdefault(name, []).append((priority, path, material))
    materials = {}
    material_sources = {}
    for name, entries in candidates.items():
        name_key = name.lower()
        def candidate_key(item):
            priority, path, _ = item
            filename = os.path.splitext(os.path.basename(path))[0].lower()
            shadow_priority = 1 if "_shadow" in filename else 0
            exact_priority = 0 if filename.startswith(name_key) else 1
            return (shadow_priority, exact_priority, priority, filename)
        _, path, material = sorted(entries, key=candidate_key)[0]
        materials[name] = material
        material_sources[name] = path
    return materials, material_sources


def collect_gltf_slots(audit):
    slots = []
    for mesh in audit.get("gltfValidation", []):
        for primitive in mesh.get("primitives", []):
            name = primitive.get("materialName")
            if not name:
                raise RuntimeError("glTF audit contains an unnamed material slot")
            if name not in slots:
                slots.append(name)
    if not slots:
        raise RuntimeError("glTF audit contains no material slots")
    return slots


def material_contract(material, source_file):
    technique = material.find("Technique")
    states = material.find("RenderStates")
    transparent = states.find("TransparentMode") if states is not None else None
    parameters = {}
    parameter_table = material.find("ParamTable")
    if parameter_table is not None:
        for parameter in parameter_table:
            parameters[parameter.tag] = parameter.get("Value")
    macros = {}
    shader_macro = material.find("ShaderMacro")
    if shader_macro is not None:
        for macro in shader_macro:
            macros[macro.get("Key")] = macro.get("Val")
    raw_mode = int(parse_float(transparent.get("TransparentMode"), 0)) if transparent is not None else 0
    new_mode = int(parse_float(transparent.get("NewTransparentMode"), 0)) if transparent is not None else 0
    alpha_ref = int(parse_float(states.get("AlphaRef"), 0)) if states is not None else 0
    # NeoX 优先使用新字段；只有旧字段为 0 且存在 AlphaRef 时，才推导 AlphaClip。
    # 不能把旧 mode 1 的非零 AlphaRef 一律改成 mode 3，否则会误伤连续透明材质。
    if new_mode > 0:
        effective_mode = new_mode
    elif raw_mode > 0:
        effective_mode = raw_mode
    elif alpha_ref > 0:
        effective_mode = 3
    else:
        effective_mode = 1
    return {
        "name": material.get("Name"),
        "sourceFile": source_file,
        "sourceTechnique": technique.get("TechName") if technique is not None else "",
        "shaderMacros": macros,
        "parameters": parameters,
        "textureParameters": {
            key: normalize_source_reference(value)
            for key, value in parameters.items()
            if value and value.lower().endswith((".tga", ".png", ".jpg", ".jpeg", ".array", ".cube"))
        },
        "renderState": {
            "transparentModeRaw": raw_mode,
            "newTransparentModeRaw": new_mode,
            "effectiveTransparentMode": effective_mode,
            "alphaRefRaw": alpha_ref,
            "alphaValRaw": int(parse_float(states.get("AlphaVal"), 255)) if states is not None else 255,
            "cullBack": states.get("CullBack", "True") == "True" if states is not None else True,
        },
    }


def load_rgba(path):
    if not os.path.isfile(path):
        raise RuntimeError("Missing source texture: %s" % path)
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.uint8)


def make_rgba(red, green, blue, alpha):
    result = np.empty((red.shape[0], red.shape[1], 4), dtype=np.uint8)
    result[:, :, 0] = red
    result[:, :, 1] = green
    result[:, :, 2] = blue
    result[:, :, 3] = alpha
    return result


def reconstruct_normal(normal):
    normal_xy = normal[:, :, :2].astype(np.float32) / 255.0 * 2.0 - 1.0
    normal_z = np.sqrt(np.maximum(
        0.0,
        1.0 - normal_xy[:, :, 0] * normal_xy[:, :, 0] - normal_xy[:, :, 1] * normal_xy[:, :, 1],
    ))
    normal_z = np.clip(normal_z * 127.5 + 127.5, 0.0, 255.0).astype(np.uint8)
    return make_rgba(normal[:, :, 0], normal[:, :, 1], normal_z, np.full(normal_z.shape, 255, dtype=np.uint8))


def texture_descriptor(name, source, color_space, channels):
    return {
        "name": name,
        "type": "texture",
        "source": source,
        "colorSpace": color_space,
        "mipmaps": True,
        "filter": "linear",
        "wrapMode": "repeat",
        "channelsDescription": channels,
    }


class TextureWriter(object):
    def __init__(self, source_root, resource_root, overwrite):
        self.source_root = source_root
        self.resource_root = resource_root
        self.overwrite = overwrite
        self.cache = {}
        self.stats = {}

    def write(self, source_reference, kind):
        source_reference = normalize_source_reference(source_reference)
        if not source_reference or source_reference.lower().endswith((".array", ".cube")):
            return None
        key = (source_reference, kind)
        if key in self.cache:
            return self.cache[key]
        source = source_path(self.source_root, source_reference)
        pixels = load_rgba(source)
        if kind == "normal":
            pixels = reconstruct_normal(pixels)
            color_space = "linear"
            channels = {"r": "normal.x", "g": "normal.y", "b": "normal.z", "a": "opaque"}
        elif kind == "pbr":
            pixels = make_rgba(pixels[:, :, 0], pixels[:, :, 1], pixels[:, :, 3], pixels[:, :, 2])
            color_space = "linear"
            channels = {"r": "roughness", "g": "metallic", "b": "ambientOcclusion", "a": "sourceParamAux"}
        elif kind == "mask":
            color_space = "linear"
            channels = {"r": "coverage", "g": "coverage", "b": "coverage", "a": "opacity"}
        else:
            color_space = "srgb"
            channels = {"r": "baseColor.r", "g": "baseColor.g", "b": "baseColor.b", "a": "opacity"}
        stem = re.sub(r"[^A-Za-z0-9_]+", "_", os.path.splitext(os.path.basename(source_reference))[0])
        digest = hashlib.sha1((source_reference + ":" + kind).encode("utf-8")).hexdigest()[:8]
        asset_name = "T_%s_%s_%s" % (CHARACTER_NAME, stem, digest)
        generated_relative = TEXTURE_SOURCE_RELATIVE_ROOT + "/" + asset_name + ".png"
        descriptor_relative = TEXTURE_RELATIVE_ROOT + "/" + asset_name + ".json"
        generated_path = os.path.join(self.resource_root, generated_relative.replace("/", os.sep))
        descriptor_path = os.path.join(self.resource_root, descriptor_relative.replace("/", os.sep))
        if os.path.exists(generated_path) and not self.overwrite:
            raise RuntimeError("Output exists: %s" % generated_path)
        parent = os.path.dirname(generated_path)
        if not os.path.isdir(parent):
            os.makedirs(parent)
        Image.fromarray(pixels, "RGBA").save(generated_path)
        write_json(descriptor_path, texture_descriptor(asset_name, generated_relative, color_space, channels), self.overwrite)
        result = descriptor_relative
        self.cache[key] = result
        self.stats[source_reference] = {"kind": kind, "target": result}
        return result


def param_value(contract, name, default=None):
    return contract["parameters"].get(name, default)


def average_pbr(source_root, contract):
    reference = param_value(contract, "ParamMap")
    if not reference:
        return [0.5, 0.0, 1.0]
    pixels = load_rgba(source_path(source_root, reference))
    return [
        float(np.mean(pixels[:, :, 0])) / 255.0,
        float(np.mean(pixels[:, :, 1])) / 255.0,
        float(np.mean(pixels[:, :, 3])) / 255.0,
    ]


def render_overrides(contract, base_mode=None, base_cull="Back"):
    mode = contract["renderState"]["effectiveTransparentMode"]
    mode_map = {
        1: "Opaque",
        2: "TransparentAlphaBlend",
        3: "OpaqueClip",
        4: "TransparentAlphaBlendWriteDepth",
        5: "TransparentAdditive",
        6: "TransparentAlphaBlendWriteDepth",
        7: "TransparentAlphaBlend",
        8: "OpaqueClip",
    }
    target_mode = mode_map.get(mode, "Opaque")
    target_cull = "Back" if contract["renderState"]["cullBack"] else "None"
    result = {}
    if target_mode != (base_mode or "Opaque"):
        result["renderMode"] = target_mode
    if target_cull != base_cull:
        result["cullMode"] = target_cull
    return result


def fixed_render_overrides(render_mode, cull_mode, base_mode="Opaque", base_cull="Back"):
    # MI 只写与母材质默认值不同的状态，避免资产校验器把重复默认值视为冗余。
    result = {}
    if render_mode != base_mode:
        result["renderMode"] = render_mode
    if cull_mode != base_cull:
        result["cullMode"] = cull_mode
    return result


def remove_default_parameters(instance):
    material_path = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "..", "shader", "glsl",
        os.path.basename(instance["material"]),
    )
    material_defaults = read_json(material_path).get("parameters", {})
    instance["parameters"] = dict(
        (name, value)
        for name, value in instance.get("parameters", {}).items()
        if name not in material_defaults or value != material_defaults[name].get("default")
    )
    return instance


def remove_default_macros(instance):
    material_path = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "..", "shader", "glsl",
        os.path.basename(instance["material"]),
    )
    material_defaults = read_json(material_path).get("macros", {})
    instance["macros"] = dict(
        (name, value)
        for name, value in instance.get("macros", {}).items()
        if name not in material_defaults or value != material_defaults[name]
    )
    if not instance["macros"]:
        instance.pop("macros", None)
    if not instance.get("parameters"):
        instance.pop("parameters", None)
    return instance


def build_material_instance(slot, contract, source_root, texture_writer, role):
    technique = contract["sourceTechnique"].lower()
    parameters = contract["parameters"]
    pbr = average_pbr(source_root, contract)
    textures = {}
    macros = {}
    if "pbr_skin" in technique:
        material = "shader/glsl/M_preintegratedSkin.json"
        if param_value(contract, "Tex0"):
            textures["albedoMap"] = texture_writer.write(param_value(contract, "Tex0"), "color")
        if param_value(contract, "NormalMap"):
            textures["normalMap"] = texture_writer.write(param_value(contract, "NormalMap"), "normal")
        if param_value(contract, "ParamMap"):
            textures["pbrParamMap"] = texture_writer.write(param_value(contract, "ParamMap"), "pbr")
        macros = {"USE_ALBEDO_MAP": 1, "USE_NORMAL_MAP": 1, "USE_PBR_MAP": 1}
        values = {
            "u_tintColor": [1.0, 1.0, 1.0, 1.0],
            "u_pbrFactors": [pbr[0], pbr[1], pbr[2], 0.0],
            "u_skinCharacterLighting": [1.0, 1.0, 1.0, 0.0],
        }
        shading_family = "PreintegratedSkin"
    elif "pbr_silk" in technique:
        material = "shader/glsl/M_cloth.json"
        textures["albedoMap"] = texture_writer.write(param_value(contract, "Tex0"), "color")
        textures["normalMap"] = texture_writer.write(param_value(contract, "NormalMap"), "normal")
        macros = {"USE_ALBEDO_MAP": 1, "USE_NORMAL_MAP": 1}
        values = {
            "u_tintColor": [1.0, 1.0, 1.0, 1.0],
            "u_pbrFactors": [pbr[0], 0.0, pbr[2], 0.0],
            "u_clothSheenColor": [0.5, 0.5, 0.5, 1.0],
            "u_clothSheenRoughness": pbr[0],
        }
        shading_family = "Cloth"
    elif "pbr_hair_transparent" in technique:
        material = "shader/glsl/M_hair.json"
        textures["albedoMap"] = texture_writer.write(param_value(contract, "Tex0"), "color")
        textures["normalMap"] = texture_writer.write(param_value(contract, "NormalMap"), "normal")
        macros = {"USE_ALBEDO_MAP": 1, "USE_NORMAL_MAP": 1}
        values = {
            "u_tintColor": [1.0, 1.0, 1.0, 1.0],
            "u_pbrFactors": [pbr[0], 0.0, pbr[2], 0.5],
            "u_hairScattering": [parse_float(param_value(contract, "u_scatter"), 0.35), 0.35, 0.22, 0.25],
            "u_hairCoverage": [1.0, 0.35, 1.0, 0.0],
            "u_alphaClipThreshold": parse_float(param_value(contract, "u_two_pass_clip_value"), 0.5),
        }
        shading_family = "Hair"
    elif "pbr_eye" in technique:
        material = "shader/glsl/M_eye.json"
        iris = param_value(contract, "t_iris")
        textures["irisColorMap"] = texture_writer.write(iris, "color")
        macros = {"USE_IRIS_COLOR_MAP": 1, "USE_SCLERA_COLOR_MAP": 0}
        values = {
            "u_eyeIrisColor": [1.0, 1.0, 1.0, 1.0],
            "u_eyeScleraColor": [0.88, 0.9, 0.94, 1.0],
            "u_eyeSurface": [0.08, 1.0, 1.0, 0.0],
            "u_eyeGeometry": [0.003, 0.006, 0.002, 0.0005],
            "u_eyePupilDilation": parse_float(param_value(contract, "u_pipil_scale"), 0.5),
        }
        shading_family = "Eye"
    elif "pbr_simple" in technique:
        material = "shader/glsl/M_pbr.json"
        textures["albedoMap"] = texture_writer.write(param_value(contract, "Tex0"), "color")
        macros = {"USE_ALBEDO_MAP": 1}
        values = {"u_tintColor": [1.0, 1.0, 1.0, 1.0], "u_pbrFactors": [0.5, 0.0, 1.0, 0.0]}
        shading_family = "DefaultLit"
    elif "pbr_crystal" in technique:
        material = "shader/glsl/M_thinTranslucent.json"
        base = param_value(contract, "t_basecolor") or param_value(contract, "Tex0")
        mask = param_value(contract, "Tex0") if param_value(contract, "t_basecolor") else None
        textures["baseColorMap"] = texture_writer.write(base, "color")
        textures["normalMap"] = texture_writer.write(param_value(contract, "NormalMap"), "normal")
        textures["transmittanceMap"] = texture_writer.write(mask, "mask") if mask else None
        macros = {"USE_BASE_COLOR_MAP": 1, "USE_NORMAL_MAP": 1}
        if textures["transmittanceMap"]:
            macros["USE_TRANSMITTANCE_MAP"] = 1
        values = {
            "u_baseColorOpacity": parse_color(param_value(contract, "u_base_color"), [0.1, 0.1, 0.1, 1.0]),
            "u_transmittanceColorCoverage": parse_color(param_value(contract, "u_crystal_color"), [0.2, 0.3, 0.5, 1.0]),
            "u_surfaceFactors": [
                parse_float(param_value(contract, "u_base_roughness"), 0.3),
                parse_float(param_value(contract, "u_base_metallic"), 0.0),
                parse_float(param_value(contract, "u_base_specular"), 0.5),
                1.0,
            ],
        }
        shading_family = "ThinTranslucent"
    else:
        material = "shader/glsl/M_pbr.json"
        textures["albedoMap"] = texture_writer.write(param_value(contract, "Tex0"), "color")
        textures["normalMap"] = texture_writer.write(param_value(contract, "NormalMap"), "normal")
        textures["pbrParamMap"] = texture_writer.write(param_value(contract, "ParamMap"), "pbr") if param_value(contract, "ParamMap") else None
        macros = {"USE_ALBEDO_MAP": 1, "USE_NORMAL_MAP": 1}
        if textures["pbrParamMap"]:
            macros["USE_PBR_MAP"] = 1
        values = {"u_tintColor": [1.0, 1.0, 1.0, 1.0], "u_pbrFactors": [pbr[0], pbr[1], pbr[2], 0.0]}
        shading_family = "DefaultLit"

    material_default_modes = {
        "PreintegratedSkin": "Opaque",
        "Cloth": "Opaque",
        "Hair": "OpaqueClip",
        "Eye": "ForwardOpaque",
        "ThinTranslucent": "ThinTranslucent",
        "DefaultLit": "Opaque",
    }
    default_mode = material_default_modes.get(shading_family, "Opaque")
    state_overrides = render_overrides(contract, default_mode)
    target_render_mode = state_overrides.get("renderMode", default_mode)
    if target_render_mode == "OpaqueClip" and shading_family in (
            "PreintegratedSkin", "Cloth", "Hair", "DefaultLit"):
        values["u_alphaClipThreshold"] = parse_float(param_value(contract, "u_two_pass_clip_value"), contract["renderState"]["alphaRefRaw"] / 255.0)
    instance = {
        "name": "%s %s" % (CHARACTER_NAME, slot),
        "type": "materialInstance",
        "material": material,
        "macros": macros,
        "textures": dict((key, value) for key, value in textures.items() if value),
        "parameters": values,
    }
    if state_overrides:
        instance["renderStateOverrides"] = state_overrides
    if role == "skin":
        instance["skinLut"] = "Common/Profiles/SkinLuts/PSL_skin.json"
    if role == "eye":
        instance["eyeProfile"] = "Common/Profiles/Eye/EP_human_default.json"
        instance["subsurfaceProfile"] = "Common/Profiles/Subsurface/SSP_skin.json"
    return remove_default_macros(remove_default_parameters(instance)), shading_family


def build_hair_core_gltf(source_gltf, slot_name):
    materials = source_gltf.get("materials", [])
    indices = [index for index, material in enumerate(materials) if material.get("name") == slot_name]
    if len(indices) != 1:
        raise RuntimeError("Hair core slot must resolve exactly once: %s" % slot_name)
    target_index = indices[0]
    nodes = []
    meshes = []
    for source_node in source_gltf.get("nodes", []):
        source_mesh_index = source_node.get("mesh")
        if source_mesh_index is None:
            continue
        selected = []
        for primitive in source_gltf["meshes"][source_mesh_index].get("primitives", []):
            if primitive.get("material") == target_index:
                primitive = dict(primitive)
                primitive["material"] = 0
                attributes = dict(primitive.get("attributes", {}))
                texcoord_names = sorted(
                    name for name in attributes
                    if name.startswith("TEXCOORD_") and name[9:].isdigit()
                )
                if texcoord_names:
                    # 子 glTF 可能只保留源模型的 UV1；Assimp 要求 TEXCOORD_N 从 0 连续编号。
                    normalized_attributes = OrderedDict(
                        (name, value)
                        for name, value in attributes.items()
                        if name not in texcoord_names
                    )
                    for index, name in enumerate(texcoord_names):
                        normalized_attributes["TEXCOORD_%d" % index] = attributes[name]
                    primitive["attributes"] = normalized_attributes
                selected.append(primitive)
        if not selected:
            continue
        source_mesh = source_gltf["meshes"][source_mesh_index]
        mesh = {"primitives": selected, "name": source_mesh.get("name", "") + "_HairCore"}
        meshes.append(mesh)
        node = {"name": source_node.get("name", "") + "_HairCore", "mesh": len(meshes) - 1}
        for field in ("translation", "rotation", "scale", "matrix", "extras"):
            if field in source_node:
                node[field] = source_node[field]
        nodes.append(node)
    if not nodes:
        raise RuntimeError("No hair core primitives found for %s" % slot_name)
    return {
        "asset": source_gltf.get("asset", {"version": "2.0"}),
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "materials": [materials[target_index]],
        "meshes": meshes,
        "accessors": source_gltf.get("accessors", []),
        "bufferViews": source_gltf.get("bufferViews", []),
        "buffers": source_gltf.get("buffers", []),
    }


def build_scene():
    return {
        "name": CHARACTER_NAME,
        "type": "scene",
        "objects": [
            {
                "name": CHARACTER_NAME,
                "type": "mesh",
                "modelPath": MODEL_RELATIVE,
                "position": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "rotation": [0.0, 0.0, 0.0],
            },
            {
                "name": CHARACTER_NAME + "_HairCore",
                "type": "mesh",
                "modelPath": HAIR_CORE_MODEL_RELATIVE,
                "position": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "rotation": [0.0, 0.0, 0.0],
            },
            {
                "name": CHARACTER_NAME + "_Ground",
                "type": "mesh",
                "modelPath": GROUND_MODEL_RELATIVE,
                "position": [0.0, -0.03, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "rotation": [0.0, 0.0, 0.0],
            },
            {
                "name": "SimpleCharacter_Key",
                "type": "directionalLight",
                "position": [0.0, 1.0, 0.0],
                "rotation": [-35.0, 25.0, 0.0],
                "color": [1.0, 0.96, 0.9],
                "intensity": 6.0,
                "shadow": {
                    "castShadows": True,
                    "dynamicShadowDistance": 300.0,
                    "dynamicShadowCascades": 4,
                    "cascadeDistributionExponent": 1.6,
                    "cascadeTransitionFraction": 0.0,
                    "shadowDistanceFadeoutFraction": 0.23,
                    "shadowBias": 0.69,
                    "shadowSlopeBias": 0.48,
                    "shadowCascadeBiasDistribution": 0.0,
                },
            },
            {
                "name": "SimpleCharacter_Fill",
                "type": "pointLight",
                "position": [-1.5, 1.2, 2.0],
                "rotation": [0.0, 0.0, 0.0],
                "color": [0.35, 0.55, 1.0],
                "intensity": 0.0,
                "radius": 5.0,
            },
            {
                "name": "SimpleCharacter_Camera",
                "type": "camera",
                "fov": 42.0,
                "near_clip": 0.05,
                "far_clip": 100.0,
                "position": [0.0, 2.2, 0.95],
                "look_at": [0.0, 0.0, 0.85],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            },
            {
                "name": "SimpleCharacter_Environment",
                "type": "environment",
                "environment": {
                    "type": "proceduralSky",
                    "cubeSize": 128,
                    "intensity": 0.45,
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
            },
        ],
    }


def write_ground_assets(resource_root, overwrite):
    ground_directory = os.path.join(resource_root, "Maps", "SC_simple_character", "Source", "Models", CHARACTER_NAME)
    write_text(
        os.path.join(ground_directory, os.path.basename(GROUND_MTL_RELATIVE)),
        "newmtl simple_character_ground\n"
        "Kd 0.16 0.18 0.22\n"
        "Ka 0.16 0.18 0.22\n"
        "Ks 0.25 0.25 0.25\n"
        "Ns 32.0\n"
        "d 1.0\n"
        "illum 2\n",
        overwrite,
    )
    write_text(
        os.path.join(ground_directory, os.path.basename(GROUND_OBJ_RELATIVE)),
        "# simple_character presentation ground\n"
        "mtllib simple_character_ground.mtl\n"
        "o simple_character_ground\n"
        "v -4.0 0.0 -4.0\n"
        "v 4.0 0.0 -4.0\n"
        "v 4.0 0.0 4.0\n"
        "v -4.0 0.0 4.0\n"
        "vn 0.0 1.0 0.0\n"
        "vt 0.0 0.0\n"
        "vt 1.0 0.0\n"
        "vt 1.0 1.0\n"
        "vt 0.0 1.0\n"
        "usemtl simple_character_ground\n"
        # 按 +Y 方向绕序，避免默认 Back-face culling 从上方看不到地面。
        "f 1/1/1 4/4/1 3/3/1 2/2/1\n",
        overwrite,
    )
    write_json(os.path.join(resource_root, GROUND_MODEL_RELATIVE.replace("/", os.sep)), {
        "name": CHARACTER_NAME + " Ground",
        "type": "mesh",
        "modelDataPath": GROUND_OBJ_RELATIVE,
        "materialSlots": [{
            "name": "simple_character_ground",
            "materialInstancePath": GROUND_MI_RELATIVE,
        }],
    }, overwrite)
    write_json(os.path.join(resource_root, GROUND_MI_RELATIVE.replace("/", os.sep)), {
        "name": CHARACTER_NAME + " Ground Material Instance",
        "type": "materialInstance",
        "material": "shader/glsl/M_pbr.json",
        "parameters": {
            "u_tintColor": [0.16, 0.18, 0.22, 1.0],
            "u_pbrFactors": [0.32, 0.0, 0.75, 0.0],
        },
    }, overwrite)


def main():
    arguments = parse_arguments()
    source_root = os.path.abspath(arguments.source_root)
    resource_root = os.path.abspath(arguments.resource_root)
    audit = read_json(os.path.join(resource_root, AUDIT_RELATIVE.replace("/", os.sep)))
    source_gltf = read_json(os.path.join(resource_root, GLTF_RELATIVE.replace("/", os.sep)))
    source_materials, material_sources = collect_source_materials(source_root)
    slots = collect_gltf_slots(audit)
    texture_writer = TextureWriter(source_root, resource_root, arguments.overwrite)
    material_slots = []
    manifest_slots = []
    hair_fringe_slot = "h_f_3694_high_1"
    for slot in slots:
        if slot not in source_materials:
            raise RuntimeError("No source MTG material found for glTF slot: %s" % slot)
        source_file = material_sources[slot]
        contract = material_contract(source_materials[slot], source_file)
        technique = contract["sourceTechnique"].lower()
        role = "eye" if "pbr_eye" in technique else ("skin" if "pbr_skin" in technique else "")
        instance, shading_family = build_material_instance(slot, contract, source_root, texture_writer, role)
        if shading_family == "ThinTranslucent":
            # Crystal 的源模式保留在审计清单，目标统一进入 ThinTranslucent 路由，避免材质模型与 RenderMode 冲突。
            instance["renderStateOverrides"] = fixed_render_overrides(
                "ThinTranslucent",
                "Back" if contract["renderState"]["cullBack"] else "None",
                base_mode="ThinTranslucent",
            )
        if shading_family == "Eye":
            # Eye 由 ForwardOpaque 专用路由消费；不能按普通 mode 1 降成 Deferred Opaque。
            instance["renderStateOverrides"] = fixed_render_overrides(
                "ForwardOpaque",
                "Back" if contract["renderState"]["cullBack"] else "None",
                base_mode="ForwardOpaque",
            )
        if slot == hair_fringe_slot:
            # mode 8 按规范拆成 Fringe/Blend 与独立 Core/AlphaClip 两份事务性材质。
            instance["renderStateOverrides"] = fixed_render_overrides(
                "TransparentAlphaBlend", "None", base_mode="OpaqueClip"
            )
        if not instance.get("renderStateOverrides"):
            instance.pop("renderStateOverrides", None)
        filename = "MI_%s.json" % re.sub(r"[^A-Za-z0-9_]+", "_", slot).strip("_")
        relative = MATERIAL_RELATIVE_ROOT + "/" + filename
        write_json(os.path.join(resource_root, relative.replace("/", os.sep)), instance, arguments.overwrite)
        material_slots.append({"name": slot, "materialInstancePath": relative})
        manifest_slots.append({
            "slot": slot,
            "sourceFile": source_file,
            "sourceFileSha256": sha256_file(source_file),
            "sourceTechnique": contract["sourceTechnique"],
            "sourceShaderMacros": contract["shaderMacros"],
            "sourceParameters": contract["parameters"],
            "sourceTextureParameters": contract["textureParameters"],
            "sourceRenderState": contract["renderState"],
            "targetMaterialInstance": relative,
            "targetShadingFamily": shading_family,
        })
    mesh_asset = {
        "name": CHARACTER_NAME,
        "type": "mesh",
        "modelDataPath": GLTF_RELATIVE,
        "materialSlots": material_slots,
    }
    write_json(os.path.join(resource_root, MODEL_RELATIVE.replace("/", os.sep)), mesh_asset, arguments.overwrite)

    if hair_fringe_slot in slots:
        hair_core_instance = {
            "name": CHARACTER_NAME + " Hair Core",
            "type": "materialInstance",
            "material": "shader/glsl/M_hair.json",
            "macros": {"USE_ALBEDO_MAP": 1, "USE_NORMAL_MAP": 1},
            "textures": {},
            "parameters": {"u_alphaClipThreshold": 0.45},
        }
        hair_core_overrides = fixed_render_overrides("OpaqueClip", "None", base_mode="OpaqueClip")
        if hair_core_overrides:
            hair_core_instance["renderStateOverrides"] = hair_core_overrides
        fringe_filename = "MI_%s.json" % re.sub(r"[^A-Za-z0-9_]+", "_", hair_fringe_slot).strip("_")
        hair_core_filename = "MI_%s_core.json" % re.sub(r"[^A-Za-z0-9_]+", "_", hair_fringe_slot).strip("_")
        fringe_instance_path = os.path.join(resource_root, MATERIAL_RELATIVE_ROOT, fringe_filename)
        fringe_instance = read_json(fringe_instance_path)
        hair_core_instance["textures"] = fringe_instance.get("textures", {})
        hair_core_instance["parameters"].update({
            "u_tintColor": [1.0, 1.0, 1.0, 1.0],
            "u_pbrFactors": [0.32, 0.0, 1.0, 0.5],
            "u_hairScattering": [0.35, 0.35, 0.22, 0.25],
            "u_hairCoverage": [1.0, 0.35, 1.0, 0.0],
        })
        hair_core_instance = remove_default_macros(remove_default_parameters(hair_core_instance))
        write_json(os.path.join(resource_root, MATERIAL_RELATIVE_ROOT, hair_core_filename), hair_core_instance, arguments.overwrite)
        hair_core = build_hair_core_gltf(source_gltf, hair_fringe_slot)
        write_json(os.path.join(resource_root, HAIR_CORE_GLTF_RELATIVE.replace("/", os.sep)), hair_core, arguments.overwrite)
        write_json(os.path.join(resource_root, HAIR_CORE_MODEL_RELATIVE.replace("/", os.sep)), {
            "name": CHARACTER_NAME + " Hair Core",
            "type": "mesh",
            "modelDataPath": HAIR_CORE_GLTF_RELATIVE,
            "materialSlots": [{
                "name": hair_fringe_slot,
                "materialInstancePath": MATERIAL_RELATIVE_ROOT + "/" + hair_core_filename,
            }],
        }, arguments.overwrite)

    manifest = {
        "name": CHARACTER_NAME + " NeoX migration",
        "sourceBlend": audit.get("sourceBlend"),
        "sourceRoot": source_root,
        "slotCount": len(slots),
        "fallbackSlotCount": 0,
        "uvPolicy": audit.get("uvPolicy"),
        "intentionalDifferences": [
            "当前 VulkanLearn 使用静态 glTF；不导出骨骼、动画和运行时角色装配。",
            "NeoX Crystal 使用现有 ThinTranslucent 近似；不恢复 SceneColor 折射和源深度偏移。",
            "NeoX Eye 的 MatCap、cube IBL 与 glitter array 不直接进入当前 Eye Material；使用 VulkanLearn Eye profile 和场景环境替代。",
            "NeoX Hair mode 8 使用 Hair-only OpaqueClip Core 叠加 TransparentAlphaBlend Fringe。",
            "角色专属光照参数暂使用公共默认值 [1,1,1,0]，待固定机位 RenderDoc 校准后再覆盖。",
        ],
        "slots": manifest_slots,
        "textures": texture_writer.stats,
    }
    write_json(os.path.join(resource_root, GENERATED_RELATIVE_ROOT, CHARACTER_NAME + "-migration.json"), manifest, arguments.overwrite)
    write_ground_assets(resource_root, arguments.overwrite)
    write_json(os.path.join(resource_root, SCENE_RELATIVE.replace("/", os.sep)), build_scene(), arguments.overwrite)
    print("Configured %s with %d real material slots" % (CHARACTER_NAME, len(slots)))
    print("Generated texture assets: %d" % len(texture_writer.stats))
    print("Scene: %s" % os.path.join(resource_root, SCENE_RELATIVE.replace("/", os.sep)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
