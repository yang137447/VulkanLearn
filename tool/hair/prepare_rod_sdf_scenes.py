import argparse
import copy
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validate_scene_schema import check, load_schema


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description="Prepare isolated SDF-primary scenes; preserve combined inputs.")
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    options = parser.parse_args()
    resources = options.resource_root.resolve()
    case_root = resources / "Maps/SC_tressfx_rod_validation"
    baseline = resources / "Maps/SC_tressfx_female/Meshes/TF_female_hair.json"
    baseline_hash = digest(baseline)
    records = []
    scene_schema = load_schema("scene.schema.json")
    for flavor in ("static", "animated"):
        source = case_root / f"SC_tressfx_rod_{flavor}.json"
        target = case_root / f"SC_tressfx_rod_sdf_{flavor}.json"
        original_hash = digest(source)
        original = json.loads(source.read_text(encoding="utf-8"))
        selected = copy.deepcopy(original)
        selected["name"] = f"SC_tressfx_rod_sdf_{flavor}"
        selected["description"] = (
            "真实Female groom独立SDF-primary几何探针：Kugelstadt和Schoemer2016 Cosserat杆模型；"
            "共享显式solver/profile描述符、2000模拟guide/40334总曲线/38334纯follow与固定物理束半径"
            "0.0005m，中心线clearance为0.05cm且只计一次。仅选择body mesh代理SDF，不叠加"
            "与作者形状冲突的四个coarse capsule；原combined场景及失败证据保留。"
            "离线场表面测点误差最大约2.376mm不是真实皮肤误差界，runtime代理场另验；"
            "本场景不预先宣称接触、风、头运动或质量PASS。"
            + ("静态绑定姿态、零风。" if flavor == "static" else
               "真实GASP Idle蒙皮源、零风；正式头运动须核根/pins与body/SDF同一变换。")
        )
        hair = [item for item in selected["objects"] if item["name"] == "Female_rod_validation"]
        assert len(hair) == 1
        collision = hair[0]["tressfx"]["hairCollision"]
        assert len(collision["capsules"]) == 4
        assert collision["mesh"]["margin"] == 0.05
        del collision["capsules"]
        problems = check(selected, scene_schema, scene_schema)
        if problems:
            raise RuntimeError(f"Scene schema rejected {target}: {problems}")
        payload = json.dumps(selected, ensure_ascii=False, indent=4) + "\n"
        if target.exists() and target.read_text(encoding="utf-8") != payload:
            raise RuntimeError(f"Refusing to overwrite different existing scene: {target}")
        target.write_text(payload, encoding="utf-8")
        assert digest(source) == original_hash
        restored = copy.deepcopy(selected)
        restored["name"] = original["name"]
        restored["description"] = original["description"]
        restored_hair = next(item for item in restored["objects"] if item["name"] == "Female_rod_validation")
        source_hair = next(item for item in original["objects"] if item["name"] == "Female_rod_validation")
        restored_hair["tressfx"]["hairCollision"]["capsules"] = source_hair["tressfx"]["hairCollision"]["capsules"]
        assert restored == original
        records.append({
            "combined_scene": source.relative_to(resources).as_posix(),
            "combined_sha256_unchanged": original_hash,
            "sdf_primary_scene": target.relative_to(resources).as_posix(),
            "sdf_primary_sha256": digest(target),
            "semantic_delta": ["name", "description", "remove hairCollision.capsules"],
            "schema_problems": problems,
        })
    assert digest(baseline) == baseline_hash
    options.report.parent.mkdir(parents=True, exist_ok=True)
    options.report.write_text(json.dumps({
        "geometry": "SDF-primary surrogate field, not exact skin",
        "quality_pass": False,
        "baseline_sha256_unchanged": baseline_hash,
        "scenes": records,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(records, indent=2))


if __name__ == "__main__":
    main()
