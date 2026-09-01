# -*- coding: utf-8 -*-

import argparse
import io
import json
import os
import sys

from PIL import Image


def ensure_directory(path):
    if os.path.isdir(path):
        return
    try:
        os.makedirs(path)
    except OSError:
        if not os.path.isdir(path):
            raise


def write_json(path, value):
    serialized = json.dumps(value, ensure_ascii=False, indent=4)
    with io.open(path, "w", encoding="utf-8") as stream:
        stream.write(serialized.decode("utf-8"))


def save_rgba(source_path, destination_path):
    Image.open(source_path).convert("RGBA").save(destination_path)


def main():
    parser = argparse.ArgumentParser(
        description="Convert NeoX b_f_3725 high_1 Pearl textures to VulkanLearn assets."
    )
    parser.add_argument(
        "--source-root",
        default=r"K:\future\res\character\players2021\b_f_3725",
    )
    parser.add_argument("--resource-root", required=True)
    parser.add_argument("--overwrite", action="store_true")
    arguments = parser.parse_args()

    source_root = os.path.abspath(arguments.source_root)
    resource_root = os.path.abspath(arguments.resource_root)
    source_texture_root = os.path.join(source_root, "textures")
    generated_root = os.path.join(resource_root, "generated", "neox", "b_f_3725")
    descriptor_root = os.path.join(resource_root, "textures", "neox", "b_f_3725")
    material_root = os.path.join(resource_root, "materials", "neox", "b_f_3725")
    for directory in (generated_root, descriptor_root, material_root):
        ensure_directory(directory)

    inputs = {
        "color": os.path.join(source_texture_root, "b_f_3725_pearl_a.tga"),
        "noise": os.path.join(source_texture_root, "b_f_3725_pearl_m.tga"),
    }
    for name, path in inputs.items():
        if not os.path.isfile(path):
            raise RuntimeError("Missing source texture %s: %s" % (name, path))

    # Pearl 颜色图保持 sRGB，噪声图保持线性；这里只转换容器格式，不重排或臆造源通道。
    output_names = {
        "color": "T_b_f_3725_pearl_Color.png",
        "noise": "T_b_f_3725_pearl_Noise.png",
    }
    for name, source_path in inputs.items():
        destination_path = os.path.join(generated_root, output_names[name])
        if os.path.exists(destination_path) and not arguments.overwrite:
            raise RuntimeError("Output exists: %s" % destination_path)
        save_rgba(source_path, destination_path)

    descriptor_names = {
        "color": "T_b_f_3725_pearl_Color",
        "noise": "T_b_f_3725_pearl_Noise",
    }
    descriptors = {}
    for name, descriptor_name in descriptor_names.items():
        descriptor = {
            "name": descriptor_name,
            "type": "texture",
            "source": "generated/neox/b_f_3725/" + output_names[name],
            "colorSpace": "srgb" if name == "color" else "linear",
            "mipmaps": True,
            "filter": "linear",
            "wrapMode": "repeat",
            "channelsDescription": {
                "r": "pearlColor.r" if name == "color" else "pearlNoise.r",
                "g": "pearlColor.g" if name == "color" else "pearlNoise.g",
                "b": "pearlColor.b" if name == "color" else "pearlNoise.b",
                "a": "opaque",
            },
        }
        descriptor_path = os.path.join(descriptor_root, descriptor_name + ".json")
        if os.path.exists(descriptor_path) and not arguments.overwrite:
            raise RuntimeError("Descriptor exists: %s" % descriptor_path)
        write_json(descriptor_path, descriptor)
        descriptors[name] = "textures/neox/b_f_3725/" + descriptor_name + ".json"

    # 参数顺序来自 b_f_3725_high001.mtg：
    # u_pearlSurface=(contrast, fresnel, brightness, emissive amount)，
    # u_pearlPbr=(roughness, metallic, UV grid, reserved)。AlphaRef 使用 NeoX 8-bit 阈值归一化。
    # USE_2U_MIX 依赖 glTF primitive 的真实 UV1；该工具不会复制 UV0 生成第二套坐标。
    # 源槽同时启用 PLAYERS_SELF，因此保留 Pearl noise 静态分支；这不是按运行质量动态切换的全局开关。
    material_instance = {
        "name": "b_f_3725 high_1 Pearl Subsurface Billboard Bake",
        "type": "materialInstance",
        "material": "shader/glsl/M_neoxPearl.json",
        "macros": {
            "USE_PEARL_NOISE_MAP": 1,
        },
        "textures": {
            "pearlColorMap": descriptors["color"],
            "pearlNoiseMap": descriptors["noise"],
        },
        "parameters": {
            "u_alphaClipThreshold": 103.0 / 255.0,
            "u_pearlSurface": [1.02, 6.47, 1.73, 0.48],
            "u_pearlPbr": [0.24, 0.41, 2.0, 0.0],
        },
    }
    material_path = os.path.join(material_root, "MI_b_f_3725_high_1_pearl.json")
    if os.path.exists(material_path) and not arguments.overwrite:
        raise RuntimeError("Material instance exists: %s" % material_path)
    write_json(material_path, material_instance)

    # 源 Billboard 顶点展开已在静态 glTF 当前 Pose 中烘焙；manifest 明确记录该有意差异，
    # 避免后续把片元 Pearl MF 误认为包含动态相机朝向和 Velocity 修正。
    manifest = {
        "name": "b_f_3725 high_1 Pearl conversion",
        "sourceMaterial": "b_f_3725_high_1",
        "sourceTechnique": "shader\\pbr_subsurface_billboard.fx::TShader",
        "sourceRenderState": {
            "transparentMode": 3,
            "alphaRef": 103,
            "cullBack": False,
        },
        "sourceMacros": {
            "USE_2U_MIX": True,
        },
        "targetMaterial": "materials/neox/b_f_3725/MI_b_f_3725_high_1_pearl.json",
        "targetShadingModel": "Subsurface",
        "targetRenderMode": "OpaqueClip",
        "targetCullMode": "None",
        "staticBakeNote": "NeoX Billboard vertex expansion is baked into the exported glTF current pose.",
        "uvContract": "UV0=sphere mask/fake normal, UV1=2x2 Pearl MatCap sampling; no synthetic UV1.",
    }
    manifest_path = os.path.join(generated_root, "high-1-pearl-manifest.json")
    if os.path.exists(manifest_path) and not arguments.overwrite:
        raise RuntimeError("Manifest exists: %s" % manifest_path)
    write_json(manifest_path, manifest)

    print("Converted b_f_3725 high_1 Pearl assets")
    print("Material: %s" % material_path)
    print("Manifest: %s" % manifest_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
