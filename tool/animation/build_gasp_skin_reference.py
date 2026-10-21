"""Build a static CPU pose fixture for external Vulkan image comparison."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--resources", required=True, type=Path)
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    if not args.name.isalnum():
        parser.error("--name must be alphanumeric")
    root = args.resources.resolve()
    target = root / "Generated/Validation" / args.name
    if target.exists():
        parser.error(f"Output already exists: {target}")
    reference = json.loads(args.reference.read_text(encoding="utf-8"))
    if reference["format"] != "vl.gasp.cpu-skin-reference.v1":
        parser.error("Expected CPU skin reference")
    target.mkdir(parents=True)
    prefix = target.relative_to(root).as_posix()

    def write(name, value):
        (target / name).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n",
                                   encoding="utf-8")
        return f"{prefix}/{name}"

    material = json.loads((root / "Maps/SC_gasp/Materials/MI_mannequin.json").read_text(encoding="utf-8"))
    material.pop("macros")
    material_path = write("MI_cpu.json", material)
    scene = json.loads((root / "Maps/SC_gasp/SC_gasp.json").read_text(encoding="utf-8"))
    actor = scene["objects"].pop(0)
    for index, section in enumerate(reference["sections"]):
        obj = [f"o section{index}"]
        for vertex in section["vertices"]:
            obj.append("v " + " ".join(format(v, ".9g") for v in vertex[:3]))
            obj.append("vn " + " ".join(format(v, ".9g") for v in vertex[3:6]))
            obj.append("vt " + " ".join(format(v, ".9g") for v in vertex[6:8]))
        indices = section["indices"]
        for offset in range(0, len(indices), 3):
            # 静态导入器末尾适配 CW；先恢复源绕序，避免重复翻转。
            face = reversed(indices[offset:offset + 3])
            obj.append("f " + " ".join(f"{v+1}/{v+1}/{v+1}" for v in face))
        source = f"pose{index}.obj"
        (target / source).write_text("\n".join(obj) + "\n", encoding="ascii")
        mesh = write(f"SM_cpu{index}.json", {
            "name": "CPU Skin Reference", "type": "mesh",
            "modelDataPath": f"{prefix}/{source}",
            "materialSlots": [{"name": f"section{index}", "materialInstancePath": material_path}],
        })
        scene["objects"].insert(index, dict(actor, name=f"cpu{index}", modelPath=mesh))
    scene["description"] = "GASP P2 CPU 蒙皮固定姿态的静态渲染对照；非论文验证场景。"
    print(write("SC_cpu.json", scene))


if __name__ == "__main__":
    main()
