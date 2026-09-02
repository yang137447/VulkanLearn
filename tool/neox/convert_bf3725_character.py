# -*- coding: utf-8 -*-

import argparse
import hashlib
import io
import json
import os
import sys
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image


MTG_FILES = [
    r"character\players2021\b_f_3725\b_f_3725_1_high.mtg",
    r"character\players2021\b_f_3725\b_f_3725_2_high.mtg",
    r"character\players2021\b_f_3725\b_f_3725_3_high.mtg",
    r"character\players2021\b_f_3725\b_f_3725_4_high.mtg",
    r"character\players2021\b_f_3725\b_f_3725_5_high.mtg",
    r"character\players2021\b_f_3725\b_f_3725_6_high.mtg",
    r"character\players2021\b_f_3725\b_f_3725_high001.mtg",
    r"character\players2021\b_f_3725\h_f_3725_1_high.mtg",
    r"character\players2021\nf2022_f_01\nf2022_f_01_high001.mtg",
]

# 35 个真实 glTF 槽位全部绑定到已迁移 MI；只有目标近似合同一致时才共享资产，
# 源宏、Cull 或动画分支差异必须继续记录在迁移清单，不能把“共享”误当成源合同完全相同。
SLOT_TARGETS = {
    "b_f_3725_1_high": "MI_body_silk_flow.json",
    "b_f_3725_2_high_0": "MI_body_silk_flow.json",
    "b_f_3725_2_high_1": "MI_body_default_secondary.json",
    "b_f_3725_2_high_2": "MI_body_silk_emissive.json",
    "b_f_3725_3_high_0": "MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_3_high_1": "MI_body_default_clip.json",
    "b_f_3725_3_high_2": "MI_body_silk_emissive.json",
    "b_f_3725_4_high_0": "MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_4_high_1": "MI_body_silk_emissive.json",
    "b_f_3725_5_high_0": "MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_5_high_1": "MI_body_default_clip.json",
    "b_f_3725_5_high_2": "MI_body_silk_emissive.json",
    "b_f_3725_6_high_0": "MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_6_high_1": "MI_body_default_clip.json",
    "b_f_3725_6_high_2": "MI_body_silk_emissive.json",
    "b_f_3725_6_high_3": "MI_crystal_red_clip.json",
    "b_f_3725_high_0": "MI_b_f_3725_body_p0.json",
    "b_f_3725_high_1": "MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_high_2": "MI_body_default_clip.json",
    "b_f_3725_high_3": "MI_body_silk_emissive.json",
    "b_f_3725_high_4": "MI_body_silk_plain_back.json",
    "b_f_3725_high_5": "MI_body_silk_plain.json",
    "b_f_3725_high_6": "MI_body_silk_emissive_alt.json",
    "b_f_3725_high_7": "MI_body_silk_flow.json",
    "b_f_3725_high_8": "MI_body_default_secondary.json",
    "b_f_3725_high_9": "MI_crystal_red_clip.json",
    "b_f_3725_high_10": "MI_crystal_red_opaque.json",
    "b_f_3725_high_11": "MI_crystal_gold_opaque.json",
    "h_f_3725_high_0": "MI_hair_cards.json",
    "h_f_3725_high_1": "MI_hair_pearl.json",
    "h_f_3725_high_2": "MI_hair_default_sparkle.json",
    "h_f_3725_high_3": "MI_hair_silk.json",
    "07 - Default": "MI_eye.json",
    "09 - Default": "MI_face_skin.json",
    "08 - Default": "MI_eye_edge.json",
}

HAIR_MODE8_SLOT = "h_f_3725_high_0"
HAIR_MODE8_CLIP_VALUE = 0.5
HAIR_MODE8_BLEND_MI = "MI_hair_cards.json"
HAIR_MODE8_CORE_MI = "MI_hair_cards_clip.json"


def ensure_directory(path):
    if not os.path.isdir(path):
        os.makedirs(path)


def normalize_json_text(value):
    # Python 2 会把源码中的中文字符串保留为 UTF-8 bytes；写 JSON 前统一转成 unicode，
    # 这样生成的迁移清单可直接阅读，不退化成转义串或依赖系统代码页。
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
    ensure_directory(os.path.dirname(path))
    serialized = json.dumps(normalize_json_text(value), ensure_ascii=False, indent=4)
    if isinstance(serialized, str):
        serialized = serialized.decode("utf-8")
    with io.open(path, "w", encoding="utf-8") as stream:
        stream.write(serialized)
        stream.write(u"\n")


def load_rgba(path):
    if not os.path.isfile(path):
        raise RuntimeError("Missing source texture: %s" % path)
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.uint8)


def save_rgba(path, pixels, overwrite):
    if os.path.exists(path) and not overwrite:
        raise RuntimeError("Output exists: %s" % path)
    Image.fromarray(np.asarray(pixels, dtype=np.uint8), "RGBA").save(path)


def make_rgba(red, green, blue, alpha):
    result = np.empty((red.shape[0], red.shape[1], 4), dtype=np.uint8)
    result[:, :, 0] = red
    result[:, :, 1] = green
    result[:, :, 2] = blue
    result[:, :, 3] = alpha
    return result


def reconstruct_normal(normal_source):
    # NeoX 法线图以 RG 保存切线空间 XY；离线重建 Z，避免每帧重复开方并保留源 B/A 给其它资产使用。
    normal_xy = normal_source[:, :, :2].astype(np.float32) / 255.0 * 2.0 - 1.0
    normal_z = np.sqrt(np.maximum(
        0.0,
        1.0 - normal_xy[:, :, 0] * normal_xy[:, :, 0] - normal_xy[:, :, 1] * normal_xy[:, :, 1],
    ))
    normal_z = np.clip(normal_z * 127.5 + 127.5, 0.0, 255.0).astype(np.uint8)
    opaque = np.full(normal_z.shape, 255, dtype=np.uint8)
    return make_rgba(normal_source[:, :, 0], normal_source[:, :, 1], normal_z, opaque)


def descriptor(name, source, color_space, channels, wrap_mode="repeat"):
    return {
        "name": name,
        "type": "texture",
        "source": source.replace("\\", "/"),
        "colorSpace": color_space,
        "mipmaps": True,
        "filter": "linear",
        "wrapMode": wrap_mode,
        "channelsDescription": channels,
    }


def write_texture_asset(resource_root, asset_name, pixels, color_space, channels, overwrite, wrap_mode="repeat"):
    generated_name = asset_name + ".png"
    generated_relative = "Maps/SC_b_f_3725/Source/Textures/" + generated_name
    descriptor_relative = "Maps/SC_b_f_3725/Textures/" + asset_name + ".json"
    save_rgba(os.path.join(resource_root, generated_relative.replace("/", os.sep)), pixels, overwrite)
    write_json(
        os.path.join(resource_root, descriptor_relative.replace("/", os.sep)),
        descriptor(asset_name, generated_relative, color_space, channels, wrap_mode),
        overwrite,
    )
    return descriptor_relative


def convert_standard_set(source_root, resource_root, prefix, base_rel, param_rel, normal_rel, surface_rel, overwrite):
    base = load_rgba(os.path.join(source_root, base_rel))
    param = load_rgba(os.path.join(source_root, param_rel))
    normal = load_rgba(os.path.join(source_root, normal_rel))
    opaque = np.full(param.shape[:2], 255, dtype=np.uint8)
    # NeoX ParamMap: R=roughness、G=metallic、B=各材质辅助量、A=AO。
    # 目标 PBR.RGB 改为标准 roughness/metallic/AO，A 单独保留源 B，shader 不再解释旧打包。
    pbr = make_rgba(param[:, :, 0], param[:, :, 1], param[:, :, 3], param[:, :, 2])
    if surface_rel:
        surface = load_rgba(os.path.join(source_root, surface_rel))
    else:
        surface = make_rgba(
            np.zeros(param.shape[:2], dtype=np.uint8),
            np.zeros(param.shape[:2], dtype=np.uint8),
            np.zeros(param.shape[:2], dtype=np.uint8),
            opaque,
        )
    return {
        "albedoMap": write_texture_asset(resource_root, prefix + "_BaseColor", base, "srgb", {
            "r": "baseColor.r", "g": "baseColor.g", "b": "baseColor.b", "a": "opacity",
        }, overwrite),
        "pbrParamMap": write_texture_asset(resource_root, prefix + "_PBR", pbr, "linear", {
            "r": "roughness", "g": "metallic", "b": "ambientOcclusion", "a": "sourceParamAux",
        }, overwrite),
        "normalMap": write_texture_asset(resource_root, prefix + "_Normal", reconstruct_normal(normal), "linear", {
            "r": "normal.x", "g": "normal.y", "b": "normal.z", "a": "opaque",
        }, overwrite),
        "surfaceMap": write_texture_asset(resource_root, prefix + "_Surface", surface, "linear", {
            "r": "mask1", "g": "emissionPearlFlowMask", "b": "sparkleMask", "a": "detailMask",
        }, overwrite),
    }


def convert_skin_set(source_root, resource_root, prefix, base_rel, param_rel, normal_rel, detail_rel, overwrite):
    base = load_rgba(os.path.join(source_root, base_rel))
    param = load_rgba(os.path.join(source_root, param_rel))
    normal = load_rgba(os.path.join(source_root, normal_rel))
    detail = load_rgba(os.path.join(source_root, detail_rel))
    opaque = np.full(param.shape[:2], 255, dtype=np.uint8)

    # 脸部不能复用通用 PBR 打包：NeoX ParamMap.B 是 skin mask，A 才是 AO。
    # NormalMap.B/A 也必须在离线阶段拆出，避免运行时丢失 curvature/detail mask。
    normal_packed = reconstruct_normal(normal)
    skin_aux = make_rgba(
        normal[:, :, 2],
        normal[:, :, 3],
        np.zeros(param.shape[:2], dtype=np.uint8),
        opaque,
    )

    # DetailMap.RG 是细节法线 XY，B 是毛孔调制量；把可重建的 Z 离线写入
    # RGB，并将原 B 放入 A，运行时只需一次采样即可得到完整辅助语义。
    skin_detail = reconstruct_normal(detail)
    skin_detail[:, :, 3] = detail[:, :, 2]

    return {
        "albedoMap": write_texture_asset(resource_root, prefix + "_BaseColor", base, "srgb", {
            "r": "baseColor.r", "g": "baseColor.g", "b": "baseColor.b", "a": "opacity",
        }, overwrite, "clamp"),
        "normalMap": write_texture_asset(resource_root, prefix + "_Normal", normal_packed, "linear", {
            "r": "normal.x", "g": "normal.y", "b": "normal.z", "a": "opaque",
        }, overwrite, "clamp"),
        "skinParamMap": write_texture_asset(resource_root, prefix + "_SkinParam", param, "linear", {
            "r": "roughness", "g": "metallic", "b": "skinColorMask", "a": "ambientOcclusion",
        }, overwrite),
        "skinAuxMap": write_texture_asset(resource_root, prefix + "_SkinAux", skin_aux, "linear", {
            "r": "curvature", "g": "detailNormalMask", "b": "reserved", "a": "reserved",
        }, overwrite, "clamp"),
        "skinDetailMap": write_texture_asset(resource_root, prefix + "_DetailNormal", skin_detail, "linear", {
            "r": "detailNormal.x", "g": "detailNormal.y", "b": "detailNormal.z", "a": "poreModulation",
        }, overwrite),
    }


def convert_copy(source_root, resource_root, asset_name, source_rel, color_space, channels, overwrite, wrap_mode="repeat"):
    return write_texture_asset(
        resource_root,
        asset_name,
        load_rgba(os.path.join(source_root, source_rel)),
        color_space,
        channels,
        overwrite,
        wrap_mode,
    )


def parse_materials(source_root):
    rows = []
    for relative_path in MTG_FILES:
        path = os.path.join(source_root, relative_path)
        root = ET.parse(path).getroot()
        for wrapper in list(root.find("MaterialGroup")):
            material = wrapper.find("Material")
            params = dict((node.tag, node.attrib.get("Value")) for node in list(material.find("ParamTable")))
            macros = dict((node.attrib.get("Key"), node.attrib.get("Val")) for node in list(material.find("ShaderMacro")))
            states = material.find("RenderStates")
            contract = {
                "sourceFile": relative_path.replace("\\", "/"),
                "slot": material.attrib["Name"],
                "technique": material.find("Technique").attrib["TechName"],
                "macros": macros,
                "parameters": params,
                "renderState": {
                    "alphaRef": int(states.attrib["AlphaRef"]),
                    "alphaVal": int(states.attrib["AlphaVal"]),
                    "transparentMode": int(states.find("TransparentMode").attrib["TransparentMode"]),
                    "cullBack": states.attrib["CullBack"] == "True",
                },
            }
            identity = dict(contract)
            identity.pop("slot")
            identity.pop("sourceFile")
            encoded = json.dumps(identity, sort_keys=True, separators=(",", ":"))
            contract["contractSha256"] = hashlib.sha256(encoded).hexdigest()
            rows.append(contract)
    return rows


def alpha_ref(value):
    return float(value) / 255.0


def resolve_target_render_state(contract_by_slot, slots):
    contracts = [contract_by_slot[slot] for slot in slots]
    transparent_modes = set(row["renderState"]["transparentMode"] for row in contracts)
    if len(transparent_modes) != 1:
        raise RuntimeError("Shared MI mixes NeoX TransparentMode values: %s" % ", ".join(slots))

    transparent_mode = next(iter(transparent_modes))
    if transparent_mode == 0:
        # 普通材质的未设置模式按 AlphaRef 解析；Hair mode 8 使用独立双 Pass，不进入本函数。
        transparent_mode = 3 if contracts[0]["renderState"]["alphaRef"] > 0 else 1
    target_render_modes = {
        1: "Opaque",
        2: "TransparentAlphaBlend",
        3: "OpaqueClip",
        4: "TransparentAlphaBlendWriteDepth",
        5: "TransparentAdditive",
    }
    if transparent_mode not in target_render_modes:
        raise RuntimeError("Unsupported NeoX TransparentMode %d for slots: %s" % (
            transparent_mode, ", ".join(slots)))

    cull_modes = set("Back" if row["renderState"]["cullBack"] else "None" for row in contracts)
    if len(cull_modes) != 1:
        raise RuntimeError("Shared MI mixes NeoX CullBack values: %s" % ", ".join(slots))

    alpha = None
    if transparent_mode == 3:
        alpha_refs = set(row["renderState"]["alphaRef"] for row in contracts)
        if len(alpha_refs) != 1:
            raise RuntimeError("Shared OpaqueClip MI mixes AlphaRef values: %s" % ", ".join(slots))
        alpha = alpha_ref(next(iter(alpha_refs)))
    return target_render_modes[transparent_mode], next(iter(cull_modes)), alpha


def packed_instance(name, material, textures, render_mode, cull_mode, alpha, emission, sparkle):
    value = {
        "name": name,
        "type": "materialInstance",
        "material": material,
        "textures": textures,
    }
    render_defaults = {"renderMode": "Opaque", "cullMode": "Back"}
    render_overrides = {}
    if render_mode != render_defaults["renderMode"]:
        render_overrides["renderMode"] = render_mode
    if cull_mode != render_defaults["cullMode"]:
        render_overrides["cullMode"] = cull_mode
    if render_overrides:
        value["renderStateOverrides"] = render_overrides

    # MI 只写与母材质默认值不同的字段；重复默认值会被资产校验器拒绝。
    parameter_defaults = {
        "u_neoxEmission": [0.0, 1.0, 0.0, 0.0],
        "u_neoxSparkle": [100.0, 0.0, 0.0, 0.0],
    }
    authored = {
        "u_neoxEmission": emission,
        "u_neoxSparkle": sparkle,
    }
    # AlphaRef 只有在 NeoX mode 3/6/7/8 中才是裁剪阈值；mode 4 即使带非零
    # AlphaRef 也必须保留连续透明度，不能再次误写成 OpaqueClip 参数。
    if alpha is not None:
        parameter_defaults["u_alphaClipThreshold"] = 0.5
        authored["u_alphaClipThreshold"] = alpha
    parameters = {}
    for key, authored_value in authored.items():
        if authored_value != parameter_defaults[key]:
            parameters[key] = authored_value
    if parameters:
        value["parameters"] = parameters
    return value


def packed_instance_from_slots(name, material, textures, contract_by_slot, slots, emission, sparkle):
    render_mode, cull_mode, alpha = resolve_target_render_state(contract_by_slot, slots)
    return packed_instance(
        name, material, textures, render_mode, cull_mode, alpha, emission, sparkle)


def build_instances(textures, contracts):
    body = textures["body"]
    secondary = textures["secondary"]
    hair_cloth = textures["hairCloth"]
    contract_by_slot = dict((row["slot"], row) for row in contracts)
    instances = {}
    instances["MI_body_silk_flow.json"] = packed_instance_from_slots(
        "b_f_3725 shared Silk Flow/Sparkle", "shader/glsl/M_neoxSilk.json", body,
        contract_by_slot, ["b_f_3725_1_high", "b_f_3725_2_high_0", "b_f_3725_high_7"],
        [0.19, 2.0, 0.0, 0.0], [1000.0, 0.39, 0.19, 20.97])
    instances["MI_body_default_secondary.json"] = packed_instance_from_slots(
        "b_f_3725 secondary Default PBR", "shader/glsl/M_neoxDefault.json", secondary,
        contract_by_slot, ["b_f_3725_2_high_1", "b_f_3725_high_8"],
        [0.0, 1.0, 0.0, 0.0], [100.0, 0.0, 0.0, 0.0])
    instances["MI_body_silk_emissive.json"] = packed_instance_from_slots(
        "b_f_3725 shared Emissive Silk/Sparkle", "shader/glsl/M_neoxSilk.json", body,
        contract_by_slot, ["b_f_3725_2_high_2", "b_f_3725_3_high_2", "b_f_3725_4_high_1",
                           "b_f_3725_5_high_2", "b_f_3725_6_high_2", "b_f_3725_high_3"],
        [30.6, 10.0, 1.0, 0.0], [200.0, 1.0, 1.0, 51.61])
    instances["MI_body_default_clip.json"] = packed_instance_from_slots(
        "b_f_3725 shared Default Alpha Clip", "shader/glsl/M_neoxDefault.json", body,
        contract_by_slot, ["b_f_3725_3_high_1", "b_f_3725_5_high_1",
                           "b_f_3725_6_high_1", "b_f_3725_high_2"],
        [0.0, 1.0, 0.0, 0.0], [100.0, 0.0, 0.0, 0.0])
    instances["MI_body_silk_plain_back.json"] = packed_instance_from_slots(
        "b_f_3725 back-face culled Plain Silk", "shader/glsl/M_neoxSilk.json", body,
        contract_by_slot, ["b_f_3725_high_4"],
        [0.0, 1.0, 0.0, 0.0], [100.0, 0.0, 0.0, 0.0])
    instances["MI_body_silk_plain.json"] = packed_instance_from_slots(
        "b_f_3725 two-sided Plain Silk", "shader/glsl/M_neoxSilk.json", body,
        contract_by_slot, ["b_f_3725_high_5"],
        [0.0, 1.0, 0.0, 0.0], [100.0, 0.0, 0.0, 0.0])
    instances["MI_body_silk_emissive_alt.json"] = packed_instance_from_slots(
        "b_f_3725 alternate Emissive Silk", "shader/glsl/M_neoxSilk.json", body,
        contract_by_slot, ["b_f_3725_high_6"],
        [45.2, 8.71, 1.0, 0.0], [200.0, 1.0, 1.0, 33.87])
    instances["MI_hair_default_sparkle.json"] = packed_instance_from_slots(
        "h_f_3725 Default Sparkle", "shader/glsl/M_neoxDefault.json", hair_cloth,
        contract_by_slot, ["h_f_3725_high_2"],
        [0.0, 1.0, 0.0, 0.0], [100.0, 0.17, 1.0, 29.11])
    instances["MI_hair_silk.json"] = packed_instance_from_slots(
        "h_f_3725 Silk", "shader/glsl/M_neoxSilk.json", hair_cloth,
        contract_by_slot, ["h_f_3725_high_3"],
        [0.0, 1.0, 0.0, 0.0], [100.0, 0.0, 0.0, 0.0])

    instances[HAIR_MODE8_BLEND_MI] = {
        "name": "h_f_3725 Hair Cards Blend Fringe",
        "type": "materialInstance",
        "material": "shader/glsl/M_neoxHair.json",
        "textures": textures["hairCards"],
        "parameters": {
            # P5 固定机位校准：环境与相机虚拟光只保留能分离发束的灰色高光，
            # 不用整体提亮来掩盖黑发 BaseColor；方向/局部光倍率保持源合同。
            "u_hairScattering": [0.5, 0.5, 0.3, 0.3],
            "u_hairCoverage": [1.0, 0.0, 1.0, 0.0],
            "u_hairCharacterLighting": [0.25, 1.0, 0.55, 0.25],
        },
    }
    instances[HAIR_MODE8_CORE_MI] = {
        "name": "h_f_3725 Hair Cards Alpha Test Core",
        "type": "materialInstance",
        "material": "shader/glsl/M_neoxHair.json",
        "renderStateOverrides": {
            "renderMode": "OpaqueClip",
        },
        "textures": textures["hairCards"],
        "parameters": {
            # Clip 阈值和 strand variation 与母材质源默认一致，必须继承而不是在 MI 重复写入；
            # 这样重跑转换器不会重新制造冗余覆盖，也不会让两份 Hair 资源各自漂移。
            "u_hairScattering": [0.5, 0.5, 0.3, 0.3],
            "u_hairCoverage": [1.0, 0.0, 1.0, 0.0],
            "u_hairCharacterLighting": [0.25, 1.0, 0.55, 0.25],
        },
    }
    instances["MI_hair_pearl.json"] = {
        "name": "h_f_3725 Pearl",
        "type": "materialInstance",
        "material": "shader/glsl/M_neoxPearl.json",
        "textures": textures["hairPearl"],
        "macros": {"USE_PEARL_NOISE_MAP": 1},
        "parameters": {
            "u_alphaClipThreshold": alpha_ref(153),
            "u_pearlSurface": [0.72, 2.11, 1.16, 0.27],
            "u_pearlPbr": [0.49, 0.58, 2.0, 0.0],
        },
    }
    instances["MI_face_skin.json"] = {
        "name": "nf2022_f_01 Face Skin",
        "type": "materialInstance",
        "material": "shader/glsl/M_neoxSkin.json",
        "skinLut": "Common/Profiles/SkinLuts/PSL_skin.json",
        "macros": {
            "USE_ALBEDO_MAP": 1,
            "USE_NORMAL_MAP": 1,
            "USE_SKIN_PARAM_MAP": 1,
            "USE_SKIN_AUX_MAP": 1,
            "USE_SKIN_DETAIL_MAP": 1,
        },
        "textures": textures["faceSkin"],
        "parameters": {
            "u_skinColor": [0.906, 0.892, 0.859, 1.0],
            "u_skinBright": 1.1,
            "u_skinRoughnessOffset": 0.05,
            "u_skinSurface": [0.004, 1.0, 1.0, 0.0],
            "u_skinTransmissionWeight": 0.08,
            "u_skinCharacterLighting": [1.8, 1.37, 1.0, 3.0],
        },
    }
    instances["MI_eye.json"] = {
        "name": "nf2022_f_01 Eye",
        "type": "materialInstance",
        "material": "shader/glsl/M_eye.json",
        "eyeProfile": "Common/Profiles/Eye/EP_human_default.json",
        "subsurfaceProfile": "Common/Profiles/Subsurface/SSP_skin.json",
        "textures": textures["eye"],
        "parameters": {
            "u_eyeGeometry": [0.0032, 0.0060, 0.00366, 0.0006],
            "u_eyeIrisColor": [1.0, 1.0, 1.0, 1.0],
            "u_eyeScleraColor": [1.0, 1.0, 1.0, 1.0],
        },
    }
    instances["MI_eye_edge.json"] = {
        "name": "nf2022_f_01 Transparent Eye Edge",
        "type": "materialInstance",
        "material": "shader/glsl/M_pbr.json",
        "renderStateOverrides": {"renderMode": "TransparentAlphaBlend"},
        "macros": {"USE_ALBEDO_MAP": 1},
        "textures": {"albedoMap": textures["eyeEdge"]},
        "parameters": {"u_pbrFactors": [0.6, 0.0, 1.0, 0.0]},
    }
    return instances


def crystal_instance(name, values, textures, alpha, cull_mode):
    value = {
        "name": name,
        "type": "materialInstance",
        "material": "shader/glsl/M_neoxCrystal.json",
        "textures": textures,
        "parameters": values,
    }
    if cull_mode != "None":
        value["renderStateOverrides"] = {"cullMode": cull_mode}
    if alpha != 0.0:
        value["parameters"]["u_alphaClipThreshold"] = alpha
    return value


def add_crystals(instances, textures):
    instances["MI_crystal_red_clip.json"] = crystal_instance(
        "b_f_3725 Red Crystal Alpha Coverage", {
            "u_crystalBaseColorRoughness": [0.5647, 0.1725, 0.1961, 0.2],
            "u_crystalColorMetallic": [0.4941, 0.0, 0.0, 0.0],
            "u_crystalRefractionBrightness": [0.7176, 0.0, 0.0078, 1.5],
            "u_crystalSubsurfaceSpecular": [0.0706, 0.0, 0.0, 0.5],
            "u_crystalCaustic": [0.4941, 0.0, 0.0, 1.13],
            "u_crystalLayerPbr": [0.04, 0.0, 0.6, 15.0],
        }, textures["redClip"], alpha_ref(102), "None")
    instances["MI_crystal_red_opaque.json"] = crystal_instance(
        "b_f_3725 Red Crystal Opaque Source", {
            "u_crystalBaseColorRoughness": [0.1647, 0.0, 0.0, 0.5],
            "u_crystalColorMetallic": [0.7216, 0.0196, 0.0, 0.85],
            "u_crystalRefractionBrightness": [0.7216, 0.0824, 0.0824, 3.3],
            "u_crystalSubsurfaceSpecular": [0.7216, 0.0078, 0.0, 1.0],
            "u_crystalCaustic": [0.7216, 0.0196, 0.0, 1.61],
            "u_crystalLayerPbr": [0.04, 0.65, 1.0, 1.0],
        }, textures["opaque"], 0.0, "Back")
    instances["MI_crystal_gold_opaque.json"] = crystal_instance(
        "b_f_3725 Gold Crystal Two Sided", {
            "u_crystalBaseColorRoughness": [0.3686, 0.3412, 0.2706, 0.08],
            "u_crystalColorMetallic": [1.0, 0.8, 0.749, 0.63],
            "u_crystalRefractionBrightness": [0.8078, 0.749, 0.6902, 3.4],
            "u_crystalSubsurfaceSpecular": [0.9765, 0.9059, 0.9647, 0.63],
            "u_crystalCaustic": [1.0, 0.8, 0.749, 7.1],
            "u_crystalLayerPbr": [0.04, 0.26, 0.58, 1.0],
        }, textures["opaque"], 0.0, "None")


def main():
    parser = argparse.ArgumentParser(description="Convert all NeoX b_f_3725 material slots.")
    parser.add_argument("--source-root", default=r"K:\future\res")
    parser.add_argument("--resource-root", default=r"D:\YYBWorkSpace\GitHub\VukanLearnResources")
    parser.add_argument("--overwrite", action="store_true")
    arguments = parser.parse_args()
    source_root = os.path.abspath(arguments.source_root)
    resource_root = os.path.abspath(arguments.resource_root)
    generated_root = os.path.join(resource_root, "Generated", "Import", "neox", "b_f_3725")
    ensure_directory(generated_root)

    contracts = parse_materials(source_root)
    contract_slots = [row["slot"] for row in contracts]
    if len(contracts) != 35 or set(contract_slots) != set(SLOT_TARGETS):
        raise RuntimeError("MTG/glTF slot contract mismatch: parsed=%d mapped=%d" % (len(contracts), len(SLOT_TARGETS)))

    textures = {}
    textures["body"] = convert_standard_set(source_root, resource_root, "T_b_f_3725_body_primary",
        r"character\players2021\b_f_3725\textures\b_f_3725001a.tga",
        r"character\players2021\b_f_3725\textures\b_f_3725001m.tga",
        r"character\players2021\b_f_3725\textures\b_f_3725_h_001n.tga",
        r"character\players2021\b_f_3725\textures\b_f_3725001s_m.tga", arguments.overwrite)
    textures["secondary"] = convert_standard_set(source_root, resource_root, "T_b_f_3725_body_secondary",
        r"character\players2021\b_f_3725\textures\b_f_3725_1001a.tga",
        r"character\players2021\b_f_3725\textures\b_f_3725_1001m.tga",
        r"character\players2021\b_f_3725\textures\b_f_3725_1_h_001n.tga", None, arguments.overwrite)
    textures["hairCloth"] = convert_standard_set(source_root, resource_root, "T_h_f_3725_cloth",
        r"character\players2021\b_f_3725\textures\h_f_3725_1001a.tga",
        r"character\players2021\b_f_3725\textures\h_f_3725_1001m.tga",
        r"character\players2021\b_f_3725\textures\h_f_3725_1_h_001n.tga",
        r"character\players2021\b_f_3725\textures\h_f_3725_1001s_m.tga", arguments.overwrite)
    textures["faceSkin"] = convert_skin_set(source_root, resource_root, "T_nf2022_f_01_skin",
        r"character\players2021\nf2022_f_01\textures\nf2022_f_01a.tga",
        r"character\players2021\nf2022_f_01\textures\nf2022_f_01m.tga",
        r"character\players2021\nf2022_f_01\textures\nf2022_f_01n.tga",
        r"common\textures\skin_detial_n.tga", arguments.overwrite)

    textures["hairCards"] = {
        "albedoMap": convert_copy(source_root, resource_root, "T_h_f_3725_cards_BaseColor",
            r"character\players2021\b_f_3725\textures\h_f_3725001a.tga", "srgb",
            {"r": "baseColor.r", "g": "baseColor.g", "b": "baseColor.b", "a": "coverage"}, arguments.overwrite),
        "normalMap": write_texture_asset(resource_root, "T_h_f_3725_cards_Normal",
            reconstruct_normal(load_rgba(os.path.join(source_root, r"character\players2021\b_f_3725\textures\h_f_3725001n.tga"))),
            "linear", {"r": "normal.x", "g": "normal.y", "b": "normal.z", "a": "opaque"}, arguments.overwrite),
        "rdiMap": convert_copy(source_root, resource_root, "T_h_f_3725_cards_RDI",
            r"character\players2021\b_f_3725\textures\h_f_3725001m.tga", "linear",
            {"r": "root", "g": "depth", "b": "strandId", "a": "ambientOcclusion"}, arguments.overwrite),
        # Ultra High 分支使用公共高频噪声；离线复制并固定为 repeat/linear，
        # 运行时只在显式 variation 开关打开时采样。
        "noiseMap": convert_copy(source_root, resource_root, "T_h_f_3725_hair_noise",
            r"common\textures\tiling_noise_high_freq.tga", "linear",
            {"r": "noise", "g": "noise", "b": "noise", "a": "opaque"}, arguments.overwrite),
    }
    textures["hairPearl"] = {
        "pearlColorMap": convert_copy(source_root, resource_root, "T_h_f_3725_pearl_Color",
            r"character\players2021\b_f_3725\textures\h_f_3725_pearl_a.tga", "srgb",
            {"r": "pearlColor.r", "g": "pearlColor.g", "b": "pearlColor.b", "a": "coverage"}, arguments.overwrite),
        "pearlNoiseMap": convert_copy(source_root, resource_root, "T_h_f_3725_pearl_Noise",
            r"character\players2021\b_f_3725\textures\h_f_3725_pearl_m.tga", "linear",
            {"r": "pearlNoise.r", "g": "pearlNoise.g", "b": "pearlNoise.b", "a": "sourceAux"}, arguments.overwrite),
    }
    textures["eye"] = {
        "irisColorMap": convert_copy(source_root, resource_root, "T_nf2022_f_01_eye_Iris",
            r"character\players\makeup_f_textures\eyeball\pupil_003_a.tga", "srgb",
            {"r": "iris.r", "g": "iris.g", "b": "iris.b", "a": "irisMask"}, arguments.overwrite, "clamp"),
        "scleraColorMap": convert_copy(source_root, resource_root, "T_nf2022_f_01_eye_Sclera",
            r"character\players\makeup_f_textures\eyeball\nx_eye_sclera_a.tga", "srgb",
            {"r": "sclera.r", "g": "sclera.g", "b": "sclera.b", "a": "sourceAux"}, arguments.overwrite, "clamp"),
    }
    textures["eyeEdge"] = convert_copy(source_root, resource_root, "T_nf2022_f_01_eye_Edge",
        r"character\players2021\makeup2022_f_textures\new_eyeedge\nf2022_f_01_eyeedge_a.tga", "srgb",
        {"r": "edgeColor.r", "g": "edgeColor.g", "b": "edgeColor.b", "a": "opacity"}, arguments.overwrite, "clamp")
    crystal_common_textures = {
        # t_basecolor.A 在 Crystal 中是 roughness，不能复用把 A 标成 opacity 的通用 BaseColor 资产。
        "baseColorMap": convert_copy(source_root, resource_root, "T_b_f_3725_crystal_BaseColor",
            r"character\players2021\b_f_3725\textures\b_f_3725001a.tga", "srgb",
            {"r": "baseColor.r", "g": "baseColor.g", "b": "baseColor.b", "a": "roughness"}, arguments.overwrite),
        "normalMap": textures["body"]["normalMap"],
        "crystalMaskMap": convert_copy(source_root, resource_root, "T_b_f_3725_crystal_Mask",
            r"character\players2021\b_f_3725\textures\b_f_3725001b_m.tga", "linear",
            {"r": "crystalLayerMask", "g": "thickness", "b": "ambientOcclusion", "a": "coverage"}, arguments.overwrite),
    }
    crystal_detail_default = convert_copy(source_root, resource_root, "T_b_f_3725_crystal_DetailDefault",
        r"common\textures\crystal_bump_n.tga", "linear",
        {"r": "detailNormal.x", "g": "detailNormal.y", "b": "detailAmbientOcclusion", "a": "detailLayerMask"}, arguments.overwrite)
    crystal_detail_body = convert_copy(source_root, resource_root, "T_b_f_3725_crystal_DetailBody",
        r"character\players2021\b_f_3725\textures\b_f_3725_h_001n.tga", "linear",
        {"r": "detailNormal.x", "g": "detailNormal.y", "b": "detailAmbientOcclusion", "a": "detailLayerMask"}, arguments.overwrite)
    textures["crystal"] = {
        "redClip": dict(crystal_common_textures, detailNormalMap=crystal_detail_default),
        "opaque": dict(crystal_common_textures, detailNormalMap=crystal_detail_body),
    }

    instances = build_instances(textures, contracts)
    add_crystals(instances, textures["crystal"])
    material_root = os.path.join(resource_root, "Maps", "SC_b_f_3725", "Materials")
    for filename, value in instances.items():
        write_json(os.path.join(material_root, filename), value, arguments.overwrite)

    # Body Skin 与 Body Pearl 由两个专项转换器生成；全角色工具只消费它们。
    # 在写 Mesh 前强制验证，避免干净资源目录得到引用不存在 MI 的半成品。
    missing_material_instances = []
    for filename in sorted(set(SLOT_TARGETS.values())):
        if not os.path.isfile(os.path.join(material_root, filename)):
            missing_material_instances.append(filename)
    if missing_material_instances:
        raise RuntimeError(
            "Missing required material instances from specialized converters: " +
            ", ".join(missing_material_instances))

    audit_path = os.path.join(resource_root, "Maps", "SC_b_f_3725", "Source", "Models", "b_f_3725", "b_f_3725.audit.json")
    with io.open(audit_path, "r", encoding="utf-8") as stream:
        audit = json.load(stream)
    ordered_slots = []
    for mesh in audit["gltfValidation"]:
        for primitive in mesh["primitives"]:
            slot = primitive["materialName"]
            if slot not in ordered_slots:
                ordered_slots.append(slot)
    if set(ordered_slots) != set(SLOT_TARGETS):
        raise RuntimeError("glTF audit does not match the 35 migrated MTG slots")

    material_prefix = "Maps/SC_b_f_3725/Materials/"
    mesh = {
        "name": "b_f_3725 Restored Character",
        "type": "mesh",
        "modelDataPath": "Maps/SC_b_f_3725/Source/Models/b_f_3725/b_f_3725.gltf",
        "materialSlots": [{"name": slot, "materialInstancePath": material_prefix + SLOT_TARGETS[slot]} for slot in ordered_slots],
    }
    write_json(os.path.join(resource_root, "Maps", "SC_b_f_3725", "Meshes", "SM_b_f_3725_p0.json"), mesh, arguments.overwrite)

    contract_by_slot = dict((row["slot"], row) for row in contracts)
    manifest = {
        "name": "b_f_3725 full NeoX material migration",
        "slotCount": len(ordered_slots),
        "fallbackSlotCount": 0,
        "uvPolicy": "只有源网格真实存在 UV1 时才导出 TEXCOORD_1；不复制 UV0。",
        "offlineTexturePolicy": "NeoX 打包通道在离线阶段转换，运行时 MF 只消费标准 PBR/Surface/RDI 语义。",
        "intentionalDifferences": [
            "Billboard 当前 Pose 已烘焙到静态 glTF，不恢复相机朝向、上一帧 Velocity/TAA 修正和 depth offset。",
            "Silk/Default 流光与闪点保留遮罩、密度和视角响应；缺少源时钟合同，因此不伪造动画相位。",
            "Crystal 使用现有 ThinTranslucent 近似；当前渲染器没有 SceneColor 折射、旋转折射和源深度偏移。",
            "Eye 使用现有 Eye ShadingModel；源 MatCap 与自定义 cube IBL 由 VulkanLearn 眼球光照和场景环境替代。",
            "Hair RDI.B strand ID 映射为轻微粗糙度扰动，因为当前 Hair 输入没有独立 strand ID 字段。",
            "Hair mode 8 暂用 Hair-only OpaqueClip Core 资源叠加原角色 TransparentAlphaBlend Fringe 资源；不是正式 Material Multi-Pass。",
        ],
        "temporaryDualPassResources": {
            "sourceSlot": HAIR_MODE8_SLOT,
            "clipValue": HAIR_MODE8_CLIP_VALUE,
            "coreMaterialInstance": material_prefix + HAIR_MODE8_CORE_MI,
            "fringeMaterialInstance": material_prefix + HAIR_MODE8_BLEND_MI,
            "coreModel": "Maps/SC_b_f_3725/Meshes/SM_b_f_3725_hair_core.json",
        },
        "slots": [],
    }
    for slot in ordered_slots:
        row = contract_by_slot[slot]
        slot_manifest = {
            "slot": slot,
            "sourceFile": row["sourceFile"],
            "sourceTechnique": row["technique"],
            "sourceContractSha256": row["contractSha256"],
            "targetMaterialInstance": material_prefix + SLOT_TARGETS[slot],
            "sourceRenderState": row["renderState"],
        }
        if slot == HAIR_MODE8_SLOT:
            slot_manifest["temporaryDualPass"] = {
                "coreMaterialInstance": material_prefix + HAIR_MODE8_CORE_MI,
                "fringeMaterialInstance": material_prefix + HAIR_MODE8_BLEND_MI,
            }
        manifest["slots"].append(slot_manifest)
    write_json(os.path.join(generated_root, "b_f_3725-material-migration.json"), manifest, arguments.overwrite)

    print("Converted %d NeoX material slots with zero fallback" % len(ordered_slots))
    print("Shared material instances written: %d" % len(instances))
    print("Manifest: %s" % os.path.join(generated_root, "b_f_3725-material-migration.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())



