"""Promote the audited UE level textures/materials into the existing P3 scene.

Creates dedicated assets. Scene component references, added static receivers and
environment authoring are outside scene.modify capabilities, so this importer
updates only those fields after reading the production capability projection.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

import numpy as np
from PIL import Image, ImageOps
from scipy.spatial.transform import Rotation

from build_gasp_level import BASIS, PREFIX, obj_text, placement, write_json


class AssetEditor:
    def __init__(self, executable, resources):
        self.process = subprocess.Popen(
            [str(executable), "--resource-root", str(resources)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8")
        self.command = 0
        self.responses = []

    def request(self, command, payload, revision=None):
        self.command += 1
        request = {"protocolVersion": 1, "commandId": self.command,
                   "source": "ai", "command": command, "payload": payload}
        if revision is not None:
            request["expectedDocumentRevision"] = revision
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        response = json.loads(self.process.stdout.readline())
        self.responses.append(response)
        if response.get("status") != "succeeded":
            raise RuntimeError(f"Asset command failed: {response}")
        return response

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=30)
        errors = self.process.stderr.read()
        if self.process.returncode != 0 or errors.strip():
            raise RuntimeError(f"Asset editor exit {self.process.returncode}: {errors}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--export", required=True, type=Path)
    parser.add_argument("--resources", required=True, type=Path)
    parser.add_argument("--asset-editor", required=True, type=Path)
    args = parser.parse_args()
    resources = args.resources.resolve()
    target = resources / PREFIX
    report = json.loads((args.export / "materials.json").read_text(encoding="utf-8"))
    geometry = json.loads((target / "Source/Level/level.json").read_text(encoding="utf-8"))
    scene_path = target / "SC_gasp_playground.json"
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    audit_path = resources / "Generated/Import/SC_gasp/material-import.json"
    if report["status"] != "complete" or report["errors"]:
        parser.error("A completed, error-free UE material export is required")
    # 在写任何正式资产前拒绝清屏读回；公开 UE 导出成功不证明 shader 已完成。
    for actor in report["actors"]:
        if "landscapeHeightmap" in actor:
            heightmap = np.asarray(Image.open(args.export / actor["landscapeHeightmap"]["file"]))
            if not np.any(heightmap[:, :, :2]):
                parser.error("Landscape height export contains only clear pixels")
    for path in ("Textures/Level", "Source/Textures/Level", "Materials/LevelTextured",
                 "Meshes/LevelTextured", "Source/LevelTextured"):
        if (target / path).exists():
            parser.error("Refusing to replace existing material import output: " + path)
    if audit_path.exists():
        parser.error("Material audit already exists")

    editor = AssetEditor(args.asset_editor.resolve(), resources)
    editor.request("scene.get_document", {"assetPath": f"{PREFIX}/SC_gasp_playground.json"})
    editor.request("scene.get_capabilities", {"assetPath": f"{PREFIX}/SC_gasp_playground.json"})
    editor.close()
    audit = {"source": report["level"], "textures": {}, "materials": {}, "instances": [],
             "capabilities": editor.responses, "limitations": [
                 "Static logo receiver mesh replaces the source deferred decal volume.",
                 "UE Blueprint interactions, widgets and NPC logic are not executed.",
                 "Lighting is a VL review setup, not a UE Lumen/exposure equivalence claim."]}
    source = target / "Source/Textures/Level"
    source.mkdir(parents=True)
    graph_dir = resources / "Generated/Import/SC_gasp/MaterialGraphs"
    graph_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.export / "materials.json", graph_dir / "materials.json")
    for filename in report["graphs"].values():
        shutil.copyfile(args.export / filename, graph_dir / filename)

    def texture_descriptor(name, filename, space, wrap="repeat", channels=None):
        descriptor = f"{PREFIX}/Textures/Level/{name}.json"
        value = {"name": name, "type": "texture", "source": f"{PREFIX}/Source/Textures/Level/{filename}",
                 "colorSpace": space, "mipmaps": True, "filter": "linear", "wrapMode": wrap}
        if channels:
            value["channelsDescription"] = channels
        write_json(resources / descriptor, value)
        return descriptor

    textures = {}
    names = set()
    for index, (path, record) in enumerate(report["textures"].items()):
        name = record["name"]
        if name in names:
            name += f"_{index:03d}"
        names.add(name)
        original = args.export / record["file"]
        filename = name + original.suffix.lower()
        space = "srgb" if record["srgb"] == "True" else "linear"
        if original.suffix.lower() == ".hdr":
            shutil.copyfile(original, source / filename)
            policy = "unchanged HDR; environment path has no material row flip"
        else:
            image = Image.open(original)
            # UE UV 数值保持不变；预翻源行以抵消 VL 固定的一次材质图像翻行。
            # 不在 shader 追加来源相关 UV 修补，也不滥用不存在的 T.flipY。
            ImageOps.flip(image).save(source / filename)
            policy = "offline row reversal cancels the single VL material upload flip"
        if record.get("address_x") != record.get("address_y"):
            raise ValueError("Per-axis wrap is unsupported: " + path)
        wrap = "clamp" if "TA_CLAMP" in record.get("address_x", "") else "repeat"
        descriptor = texture_descriptor(name, filename, space, wrap)
        textures[path] = descriptor
        audit["textures"][path] = {"asset": descriptor, "source": record, "conversion": policy,
                                  "dimensions": list(Image.open(original).size) if original.suffix == ".png" else [8192, 4096]}

    material_cache = {}

    def create_material(value):
        signature = json.dumps(value, sort_keys=True)
        if signature not in material_cache:
            index = len(material_cache)
            path = f"{PREFIX}/Materials/LevelTextured/MI_gasp_{index:03d}.json"
            value = {"name": f"GASP imported material {index:03d}", "type": "materialInstance", **value}
            write_json(resources / path, value)
            material_cache[signature] = path
        return material_cache[signature]

    grid_defaults = json.loads((Path(__file__).resolve().parents[2] /
                               "shader/glsl/M_gaspGrid.json").read_text())["parameters"]

    def resolved_material(path, component):
        record = report["materials"][path]
        base = path
        while "parent" in report["materials"][base]:
            base = report["materials"][base]["parent"]
        parameters = {}
        macros = {}
        if base.endswith("/M_Grid.M_Grid"):
            scalar = record["scalars"]
            for parameter, original in (("u_surfaceColor", "Surface Color"), ("u_gridColor", "Grid Color"),
                                        ("u_topSurfaceColor", "Top Surface Color"), ("u_topGridColor", "Top Grid Color")):
                parameters[parameter] = record["vectors"][original]
            parameters.update({
                "u_gridSizes": [scalar[f"Grid {i} Size"] for i in range(1, 4)],
                "u_detailScales": [scalar[name] for name in ("Albedo_Scale", "Roughness Scale", "Normal Scale")],
                "u_roughness": [scalar[name] for name in ("Min Roughness", "Max Roughness",
                                                          "Roughness Strength", "Roughness Contrast")],
                "u_normalStrength": scalar["Normal Strength"], "u_specular": scalar["Base Specular"]})
            aligned = record["switches"]["Object Aligned"]
            if aligned:
                rotation = Rotation.from_quat(component["transform"]["quaternion"]).as_matrix()
                origin = BASIS @ component["boundsOrigin"]
                for i, name in enumerate(("X", "Y", "Z")):
                    axis = BASIS @ rotation[:, i]
                    parameters[f"u_projection{name}"] = [*axis.tolist(), float(np.dot(origin, axis))]
            macros = {key: int(record["switches"][name]) for key, name in (
                ("OBJECT_ALIGNED", "Object Aligned"), ("DETAILED_SURFACE", "Detailed Surface"),
                ("DIFFERENT_TOP_COLORS", "Different Top Colors?"))}
            macros = {key: value for key, value in macros.items()
                      if value != {"OBJECT_ALIGNED": 1, "DETAILED_SURFACE": 1, "DIFFERENT_TOP_COLORS": 0}[key]}
            parameters = {key: value for key, value in parameters.items()
                          if value != grid_defaults[key]["default"]}
            bindings = {f"grid{i}Map": textures[record["textures"][f"Grid {i} Texture"]] for i in range(1, 4)}
            for name, asset in (("paintColorMap", "T_Paint_Diffuse"),
                                ("paintRoughnessMap", "T_Paint_Glossiness"), ("paintNormalMap", "T_Paint_Normal")):
                bindings[name] = textures[f"/Game/Levels/LevelPrototyping/Textures/{asset}.{asset}"]
            value = {"material": "shader/glsl/M_gaspGrid.json", "parameters": parameters, "textures": bindings}
            if macros:
                value["macros"] = macros
        elif base.endswith("/M_Solid.M_Solid") or base.endswith("/BasicShapeMaterial.BasicShapeMaterial"):
            tint = record["vectors"].get("Base Color", record["vectors"].get("Color"))
            value = {"material": "shader/glsl/M_pbr.json", "parameters": {
                "u_tintColor": tint, "u_pbrFactors": [record["scalars"]["Roughness"],
                                                    record["scalars"].get("Metallic", 0), 1, 0]}}
        elif base.endswith("/M_Transparent.M_Transparent"):
            tint = record["vectors"]["Base Color"]
            # 源 BaseColor 和 Emissive 均来自同一个颜色；1x1 linear 图保留常量发光，
            # 使用既有 PBR 公开接口，不给通用 shader 加 GASP 专用分支。
            name = f"T_TransparentEmission_{len(material_cache):03d}"
            filename = name + ".png"
            Image.new("RGB", (1, 1), tuple(round(v * 255) for v in tint[:3])).save(source / filename)
            emission = texture_descriptor(name, filename, "linear")
            value = {"material": "shader/glsl/M_pbr.json",
                     "renderStateOverrides": {"renderMode": "TransparentAlphaBlend", "cullMode": "None"},
                     "macros": {"USE_EMISSION_MAP": 1},
                     "parameters": {"u_tintColor": tint}, "textures": {"emissionMap": emission}}
        else:
            raise ValueError("No verified material mapping for visible mesh: " + base)
        result = create_material(value)
        audit["materials"].setdefault(path, []).append(result)
        return result

    components = {(item["actor"], item["component"]): item for item in report["components"]}
    mesh_cache = {}

    def mesh_descriptor(model, materials):
        key = (model, tuple(materials))
        if key not in mesh_cache:
            index = len(mesh_cache)
            path = f"{PREFIX}/Meshes/LevelTextured/SM_gasp_{index:03d}.json"
            write_json(resources / path, {"name": f"GASP textured mesh {index:03d}", "type": "mesh",
                "modelDataPath": model, "materialSlots": [
                    {"name": f"section_{i}", "materialInstancePath": material}
                    for i, material in enumerate(materials)]})
            mesh_cache[key] = path
        return mesh_cache[key]

    by_id = {obj.get("id"): obj for obj in scene["objects"]}
    for index, instance in enumerate(geometry["instances"]):
        obj = by_id[f"level-{index:03d}"]
        component = components[(instance["actor"], instance["component"])]
        if component["materials"] != instance["materials"]:
            raise ValueError("UE material component identity drift")
        node = obj["components"][0]
        old_mesh = json.loads((resources / node["modelPath"]).read_text())
        mats = [resolved_material(path, component) for path in component["materials"]]
        if len(mats) != len(old_mesh["materialSlots"]):
            raise ValueError("Source section/material count mismatch")
        node["modelPath"] = mesh_descriptor(old_mesh["modelDataPath"], mats)
        audit["instances"].append({"id": obj["id"], "mesh": node["modelPath"], "source": instance["materials"]})

    geometry_source = target / "Source/LevelTextured"
    geometry_source.mkdir(parents=True)

    def static_object(name, sections, materials, collision=False):
        text, triangles = obj_text(sections)
        model = f"{PREFIX}/Source/LevelTextured/{name}.obj"
        (resources / model).write_text(text, encoding="ascii")
        descriptor = mesh_descriptor(model, materials)
        obj = {"name": name, "type": "blueprintInstance", "id": name,
               "class": "Gasp.StaticLevelBlueprint", "properties": {},
               "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1],
               "components": [{"id": "visual", "type": "mesh", "modelPath": descriptor}]}
        if collision:
            obj["physics"] = {"motion": "static", "shape": "mesh", "component": "visual", "friction": 0.8}
        scene["objects"].append(obj)
        return triangles

    landscapes = [actor for actor in report["actors"] if "landscapeHeightmap" in actor]
    for actor in landscapes:
        rotation = Rotation.from_quat(actor["transform"]["quaternion"]).as_matrix()
        scale = np.asarray(actor["transform"]["scale"])
        translation = np.asarray(actor["transform"]["position"])
        heightmap = actor["landscapeHeightmap"]
        original = args.export / heightmap["file"]
        shutil.copyfile(original, geometry_source / original.name)
        image = np.asarray(Image.open(original).convert("RGBA"), dtype=np.int32)
        width, height = heightmap["dimensions"]
        if image.shape[:2] != (height, width):
            raise ValueError("Landscape export dimensions mismatch")
        heights = ((image[:, :, 0] << 8) + image[:, :, 1] - 32768) / 128
        # 公开导出已移除 subsection 接缝；仅在所有高度精确相等时化简为四角。
        dy, dx = np.gradient(heights)
        normal = np.stack((-dx, -dy, np.ones_like(heights)), axis=-1)
        sample_x, sample_y = np.arange(width), np.arange(height)
        if np.ptp(heights) == 0:
            sample_x, sample_y = np.array([0, width - 1]), np.array([0, height - 1])
            heights = heights[np.ix_(sample_y, sample_x)]
            normal = normal[np.ix_(sample_y, sample_x)]
        xx, yy = np.meshgrid(sample_x + heightmap["sectionBase"][0],
                             sample_y + heightmap["sectionBase"][1])
        local = np.stack((xx, yy, heights), axis=-1).reshape(-1, 3)
        vertices = ((local * scale) @ rotation.T + translation).tolist()
        normal = normal.reshape(-1, 3) / scale
        normal = normal @ rotation.T
        normal /= np.linalg.norm(normal, axis=1)[:, None]
        normals, uvs, indices = normal.tolist(), (local[:, :2] / 100).tolist(), []
        columns = len(sample_x)
        for y in range(len(sample_y) - 1):
            for x in range(columns - 1):
                a = y * columns + x
                indices.extend((a, a + 1, a + columns + 1, a, a + columns + 1, a + columns))
        component = {"transform": actor["transform"], "boundsOrigin": translation.tolist()}
        material = resolved_material(actor["landscapeMaterial"], component)
        triangles = static_object("landscape-static", [{"vertices": vertices, "normals": normals,
                                  "uvs": uvs, "indices": indices}], [material], collision=True)
        audit["landscape"] = {"components": heightmap["componentCount"], "triangles": triangles,
                              "policy": "UE public render-target RG height samples baked to static triangles; no terrain runtime",
                              "sourceBoundsOrigin": heightmap["boundsOrigin"],
                              "sourceBoundsExtent": heightmap["boundsExtent"],
                              "samplingLimitation": "UE canvas export resamples subsection edges; not a raw heightmap identity claim",
                              "boundsMin": np.min(vertices, axis=0).tolist(),
                              "boundsMax": np.max(vertices, axis=0).tolist()}

    for actor in report["actors"]:
        for decal in actor.get("decals", []):
            if not decal["material"].endswith("/Decal_ProjectLogo.Decal_ProjectLogo"):
                raise ValueError("Unsupported source decal")
            mask_path = next(path for path in textures if "T_GameAnimationSample_Logo." in path)
            image = np.asarray(Image.open(args.export / report["textures"][mask_path]["file"]).convert("RGB"))
            encoded = image[:, :, 0] / 255
            linear = np.where(encoded <= 0.04045, encoded / 12.92, ((encoded + 0.055) / 1.055) ** 2.4)
            mask = Image.fromarray(np.round(linear * 255).astype(np.uint8))
            ImageOps.flip(mask).save(source / "T_LogoOpacity.png")
            opacity = texture_descriptor("T_LogoOpacity", "T_LogoOpacity.png", "linear", "clamp")
            material = create_material({"material": "shader/glsl/M_pbr.json",
                "renderStateOverrides": {"renderMode": "TransparentAlphaBlend"},
                "macros": {"USE_ALPHA_MAP": 1}, "parameters": {"u_tintColor": decal["color"]},
                "textures": {"alphaMap": opacity}})
            size = np.asarray(decal["size"])
            rotation = Rotation.from_quat(decal["placement"]["quaternion"]).as_matrix()
            points = np.array([[-0.05, -size[1], -size[2]], [-0.05, size[1], -size[2]],
                               [-0.05, size[1], size[2]], [-0.05, -size[1], size[2]]])
            points *= decal["placement"]["scale"]
            points = points @ rotation.T + decal["placement"]["position"]
            normal = -rotation[:, 0]
            # DeferredDecal 的 UV = normalizedLocal.zy；固定平面离线展开，不新增 decal pass。
            static_object("project-logo", [{"vertices": points.tolist(), "normals": [normal.tolist()] * 4,
                "uvs": [[0, 0], [0, 1], [1, 1], [1, 0]], "indices": [0, 1, 2, 0, 2, 3]}], [material])

    sky_path = next(value["source"] for value in (
        json.loads((resources / asset).read_text()) for asset in textures.values()) if "T_sky_01_8k.hdr" in value["source"])
    for obj in scene["objects"]:
        if obj["type"] == "environment":
            obj["environment"].update({"hdrPath": sky_path, "intensity": 2, "cubeSize": 256})
    # 此处仅修改标准 API 不支持的组件引用、对象增添与环境；描述字段随后走正式 API。
    write_json(scene_path, scene)
    editor = AssetEditor(args.asset_editor.resolve(), resources)
    payload = {"assetPath": f"{PREFIX}/SC_gasp_playground.json"}
    document = editor.request("scene.get_document", payload)
    editor.request("scene.get_capabilities", payload)
    changed = editor.request("scene.modify", {**payload, "changes": [{
        "field": "description",
        "value": (
            "GASP DefaultLevel 原始 LOD0 几何、摆放、地表高度与关卡贴图迁移；"
            "三层投影网格、漆面颜色/粗糙度/法线、实例颜色、透明材质、Logo 与原始天空。"
            "静态三角碰撞配合 C# 高层状态机与原生 Motion Matching 基础搜索、局部 TRS 交叉混合，"
            "支持 Idle/Walk/Run/Jump/Fall/Land，工程验收场景，非论文验证。"
            "Logo 用固定平面替代源贴花，灯光与动作不宣称 UE 等价；"
            "可选静态支撑足部接地默认关闭；UE 蓝图交互、完整 UE FootPlacement、惯性化与步幅变形尚未实现。")
    }, {"objectIdentity": "directionalLight|sun", "field": "intensity", "value": 10},
       {"objectIdentity": "camera|camera", "field": "exposure.ev100", "value": 3}
    ]}, document["documentRevision"])
    editor.request("scene.validate", payload)
    editor.request("scene.save", payload, changed["documentRevision"])
    editor.close()
    audit["sceneAuthoring"] = editor.responses
    audit["materialCount"] = len(material_cache)
    audit["meshDescriptorCount"] = len(mesh_cache)
    audit["textureCount"] = len(textures)
    write_json(audit_path, audit)
    print(json.dumps({"textures": len(textures), "materials": len(material_cache),
                      "meshes": len(mesh_cache), "scene": str(scene_path), "landscape": audit.get("landscape")}))


if __name__ == "__main__":
    main()
