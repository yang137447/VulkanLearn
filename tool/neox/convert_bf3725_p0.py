# -*- coding: utf-8 -*-

import argparse
import io
import json
import os
import sys

import numpy as np
from PIL import Image


def ensure_directory(path):
    if os.path.isdir(path):
        return
    try:
        os.makedirs(path)
    except OSError:
        if not os.path.isdir(path):
            raise


def load_rgba(path):
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.uint8)


def save_rgba(path, pixels):
    Image.fromarray(np.asarray(pixels, dtype=np.uint8), "RGBA").save(path)


def make_rgba(red, green, blue, alpha=255):
    height, width = red.shape
    result = np.empty((height, width, 4), dtype=np.uint8)
    result[:, :, 0] = red
    result[:, :, 1] = green
    result[:, :, 2] = blue
    result[:, :, 3] = alpha
    return result


def reconstruct_normal(normal_source):
    # NeoX 只把切线空间法线 XY 写入 R/G；Z 在离线阶段重建，避免运行时重复开方。
    normal_xy = normal_source[:, :, :2].astype(np.float32) / 255.0 * 2.0 - 1.0
    normal_z = np.sqrt(
        np.maximum(
            0.0,
            1.0
            - normal_xy[:, :, 0] * normal_xy[:, :, 0]
            - normal_xy[:, :, 1] * normal_xy[:, :, 1],
        )
    )
    normal_z = np.clip(normal_z * 127.5 + 127.5, 0.0, 255.0).astype(np.uint8)
    opaque = np.full(normal_z.shape, 255, dtype=np.uint8)
    return make_rgba(normal_source[:, :, 0], normal_source[:, :, 1], normal_z, opaque)


def write_json(path, value):
    serialized = json.dumps(value, ensure_ascii=False, indent=4)
    with io.open(path, "w", encoding="utf-8") as stream:
        stream.write(serialized.decode("utf-8"))


def main():
    parser = argparse.ArgumentParser(
        description="Convert NeoX b_f_3725 P0 skin textures to VulkanLearn assets."
    )
    parser.add_argument(
        "--source-root",
        default=r"K:\future\res",
    )
    parser.add_argument("--resource-root", required=True)
    parser.add_argument("--overwrite", action="store_true")
    arguments = parser.parse_args()

    source_root = os.path.abspath(arguments.source_root)
    resource_root = os.path.abspath(arguments.resource_root)
    generated_root = os.path.join(resource_root, "generated", "neox", "b_f_3725")
    descriptor_root = os.path.join(resource_root, "textures", "neox", "b_f_3725")
    material_root = os.path.join(resource_root, "materials", "neox", "b_f_3725")
    for directory in (generated_root, descriptor_root, material_root):
        ensure_directory(directory)

    # b_f_3725_high_0 的 MTG 明确引用 nb_f_2023 皮肤组；不能用服装目录下
    # b_f_3725001* 通用贴图代替，否则身体会得到错误的颜色、法线和皮肤遮罩。
    skin_input_paths = {
        "baseColor": os.path.join(
            source_root,
            r"character\players2021\nb_f_2023\nb_f_2023002a.tga",
        ),
        "paramMap": os.path.join(
            source_root,
            r"character\players2021\nb_f_2023\nb_f_2023002m.tga",
        ),
        "normalMap": os.path.join(
            source_root,
            r"character\players2021\nb_f_2023\nb_f_2023002n.tga",
        ),
        "detailMap": os.path.join(
            source_root,
            r"common\textures\skin_detial_n.tga",
        ),
    }
    # 旧通用 PBR/Emission 输出保留给审计和兼容检查，但不再绑定到 P0 Skin MI。
    legacy_input_paths = {
        "baseColor": os.path.join(
            source_root,
            r"character\players2021\b_f_3725\textures\b_f_3725001a.tga",
        ),
        "paramMap": os.path.join(
            source_root,
            r"character\players2021\b_f_3725\textures\b_f_3725001m.tga",
        ),
    }
    for name, path in skin_input_paths.items():
        if not os.path.isfile(path):
            raise RuntimeError("Missing source texture %s: %s" % (name, path))
    for name, path in legacy_input_paths.items():
        if not os.path.isfile(path):
            raise RuntimeError("Missing legacy source texture %s: %s" % (name, path))

    base_color = load_rgba(skin_input_paths["baseColor"])
    param_map = load_rgba(skin_input_paths["paramMap"])
    normal_source = load_rgba(skin_input_paths["normalMap"])
    detail_source = load_rgba(skin_input_paths["detailMap"])
    legacy_base_color = load_rgba(legacy_input_paths["baseColor"])
    legacy_param_map = load_rgba(legacy_input_paths["paramMap"])

    normal_map = reconstruct_normal(normal_source)
    detail_normal = reconstruct_normal(detail_source)
    detail_normal[:, :, 3] = detail_source[:, :, 2]
    white = np.full(param_map.shape[:2], 255, dtype=np.uint8)
    skin_aux = make_rgba(
        normal_source[:, :, 2],
        normal_source[:, :, 3],
        np.zeros(normal_source.shape[:2], dtype=np.uint8),
        np.full(normal_source.shape[:2], 255, dtype=np.uint8),
    )
    skin_mask = make_rgba(
        param_map[:, :, 2],
        white,
        white,
        white,
    )

    # P0 使用真实 Skin 资源；旧通用 PBR/Emission 只作为兼容审计输出保留。
    outputs = {
        "baseColor": (
            "T_b_f_3725_body_BaseColor.png",
            make_rgba(base_color[:, :, 0], base_color[:, :, 1], base_color[:, :, 2]),
        ),
        "pbrParamMap": (
            "T_b_f_3725_body_PBR.png",
            make_rgba(
                legacy_param_map[:, :, 0],
                legacy_param_map[:, :, 1],
                legacy_param_map[:, :, 3],
            ),
        ),
        "normalMap": (
            "T_b_f_3725_body_Normal.png",
            normal_map,
        ),
        "skinParamMap": (
            "T_b_f_3725_body_SkinParam.png",
            param_map,
        ),
        "skinMaskMap": (
            "T_b_f_3725_body_SkinMask.png",
            skin_mask,
        ),
        "skinAuxMap": (
            "T_b_f_3725_body_SkinAux.png",
            skin_aux,
        ),
        "skinDetailMap": (
            "T_b_f_3725_body_DetailNormal.png",
            detail_normal,
        ),
        "emissionMaskMap": (
            "T_b_f_3725_body_EmissionMask.png",
            make_rgba(
                255 - legacy_base_color[:, :, 3],
                255 - legacy_base_color[:, :, 3],
                255 - legacy_base_color[:, :, 3],
            ),
        ),
    }

    for key, value in outputs.items():
        destination = os.path.join(generated_root, value[0])
        if os.path.exists(destination) and not arguments.overwrite:
            raise RuntimeError("Output exists: %s" % destination)
        save_rgba(destination, value[1])

    descriptor_names = {
        "baseColor": "T_b_f_3725_body_BaseColor",
        "pbrParamMap": "T_b_f_3725_body_PBR",
        "normalMap": "T_b_f_3725_body_Normal",
        "skinParamMap": "T_b_f_3725_body_SkinParam",
        "skinMaskMap": "T_b_f_3725_body_SkinMask",
        "skinAuxMap": "T_b_f_3725_body_SkinAux",
        "skinDetailMap": "T_b_f_3725_body_DetailNormal",
        "emissionMaskMap": "T_b_f_3725_body_EmissionMask",
    }
    descriptor_semantics = {
        "baseColor": ("srgb", {"r": "baseColor.r", "g": "baseColor.g", "b": "baseColor.b", "a": "opaque"}),
        "pbrParamMap": ("linear", {"r": "roughness", "g": "metallic", "b": "ambientOcclusion", "a": "reserved"}),
        "normalMap": ("linear", {"r": "normal.x", "g": "normal.y", "b": "normal.z", "a": "reserved"}),
        "skinParamMap": ("linear", {"r": "roughness", "g": "metallic", "b": "skinColorMask", "a": "ambientOcclusion"}),
        "skinMaskMap": ("linear", {"r": "skinMask", "g": "reserved", "b": "reserved", "a": "reserved"}),
        "skinAuxMap": ("linear", {"r": "curvature", "g": "detailNormalMask", "b": "reserved", "a": "reserved"}),
        "skinDetailMap": ("linear", {"r": "detailNormal.x", "g": "detailNormal.y", "b": "detailNormal.z", "a": "poreModulation"}),
        "emissionMaskMap": ("linear", {"r": "emissionMask", "g": "emissionMask", "b": "emissionMask", "a": "reserved"}),
    }
    descriptor_wrap_modes = {
        "baseColor": "clamp",
        "normalMap": "clamp",
        "skinParamMap": "repeat",
        "skinMaskMap": "repeat",
        "skinAuxMap": "clamp",
        "skinDetailMap": "repeat",
        "pbrParamMap": "repeat",
        "emissionMaskMap": "repeat",
    }
    descriptors = {}
    for key, descriptor_name in descriptor_names.items():
        transfer, channels = descriptor_semantics[key]
        descriptor = {
            "name": descriptor_name,
            "type": "texture",
            "source": "generated/neox/b_f_3725/" + outputs[key][0],
            "colorSpace": transfer,
            "mipmaps": True,
            "filter": "linear",
            "wrapMode": descriptor_wrap_modes[key],
            "channelsDescription": channels,
        }
        descriptor_path = os.path.join(descriptor_root, descriptor_name + ".json")
        if os.path.exists(descriptor_path) and not arguments.overwrite:
            raise RuntimeError("Descriptor exists: %s" % descriptor_path)
        write_json(descriptor_path, descriptor)
        descriptors[key] = "textures/neox/b_f_3725/" + descriptor_name + ".json"

    material_instance = {
        "name": "b_f_3725 Body P0 Preintegrated Skin",
        "type": "materialInstance",
        "material": "shader/glsl/M_neoxSkin.json",
        "skinLut": "skinLuts/PSL_skin.json",
        # 身体槽没有源 ParamTable 数值覆盖；角色光照基线由 M_neoxSkin 统一持有，
        # MI 只绑定这张身体实际使用的 Skin 贴图，避免重复默认值被后续迁移误当成槽位特例。
        "macros": {
            "USE_ALBEDO_MAP": 1,
            "USE_NORMAL_MAP": 1,
            "USE_SKIN_PARAM_MAP": 1,
            "USE_SKIN_AUX_MAP": 1,
            "USE_SKIN_DETAIL_MAP": 1,
        },
        "textures": {
            "albedoMap": descriptors["baseColor"],
            "normalMap": descriptors["normalMap"],
            "skinParamMap": descriptors["skinParamMap"],
            "skinAuxMap": descriptors["skinAuxMap"],
            "skinDetailMap": descriptors["skinDetailMap"],
        },
    }
    material_path = os.path.join(material_root, "MI_b_f_3725_body_p0.json")
    if os.path.exists(material_path) and not arguments.overwrite:
        raise RuntimeError("Material instance exists: %s" % material_path)
    write_json(material_path, material_instance)

    manifest = {
        "name": "b_f_3725 P0 body texture conversion",
        "sourceRoot": source_root,
        "materialInstance": "materials/neox/b_f_3725/MI_b_f_3725_body_p0.json",
        "shadingModel": "PreintegratedSkin",
        "skinLut": "skinLuts/PSL_skin.json",
        "boundTextures": {
            "albedoMap": descriptors["baseColor"],
            "normalMap": descriptors["normalMap"],
            "skinParamMap": descriptors["skinParamMap"],
            "skinAuxMap": descriptors["skinAuxMap"],
            "skinDetailMap": descriptors["skinDetailMap"],
        },
        "compatibilityTextures": {
            "pbrParamMap": descriptors["pbrParamMap"],
            "emissionMaskMap": descriptors["emissionMaskMap"],
            "skinMaskMap": descriptors["skinMaskMap"],
        },
        "sourceSemantics": {
            "baseColor": "character\\players2021\\nb_f_2023\\nb_f_2023002a.tga; RGB=baseColor, source is opaque",
            "normalMap": "character\\players2021\\nb_f_2023\\nb_f_2023002n.tga; R/G=normalXY,B=curvature,A=detailNormalMask",
            "detailMap": "common\\textures\\skin_detial_n.tga; R/G=detailNormalXY,B=poreModulation",
            "paramMap": "R=roughness,G=metallic,B=skinMask,A=ambientOcclusion",
            "legacyCompatibility": "b_f_3725\\textures\\b_f_3725001a.tga / b_f_3725001m.tga only feed unbound PBR/Emission audit assets",
        },
    }
    manifest_path = os.path.join(generated_root, "conversion-manifest.json")
    if os.path.exists(manifest_path) and not arguments.overwrite:
        raise RuntimeError("Manifest exists: %s" % manifest_path)
    write_json(manifest_path, manifest)

    print("Converted b_f_3725 P0 body assets")
    print("Material: %s" % material_path)
    print("Manifest: %s" % manifest_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())





