"""从 Houdini 里直接把 erode 子网络各求解节点的真实参数读出来。

`heightfield_erode::2.0` 内部是 SOP 子网络：
    solve_erosion/d/s/
        erode_hydro          heightfield_erode_hydro
        erode_thermal        heightfield_erode_thermal
        precipitation        heightfield_erode_precipitation
        evaporate            heightfield_erode_precipitation
        compute_stability_mask/blur_grid / blur_flow_paths

本脚本打印每个求解节点的全部参数（名字 + 求值），写入 JSON，
作为 Blender GN 重写的唯一权威依据。
"""

from __future__ import annotations

import argparse
import json
import os
import sys

ERODE_ROOT = "/obj/Erosion_Examples/heightfield_erode1"
SOLVERS = (
    "solve_erosion/d/s/erode_hydro",
    "solve_erosion/d/s/erode_thermal",
    "solve_erosion/d/s/precipitation",
    "solve_erosion/d/s/evaporate",
    "solve_erosion/d/s/compute_stability_mask/blur_grid",
    "solve_erosion/d/s/compute_stability_mask/blur_flow_paths",
)


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hip", required=True)
    parser.add_argument("--root", default=ERODE_ROOT)
    parser.add_argument("--out", required=True)
    return parser.parse_args(argv)


def dump_node(node) -> dict:
    record = {
        "path": node.path(),
        "type": node.type().name(),
        "parms": {},
        "inputs": [],
    }
    for parm in node.parms():
        key = parm.name()
        try:
            record["parms"][key] = parm.eval()
        except Exception as error:  # noqa: BLE001
            record["parms"][key] = f"<error: {error}>"
    for index, upstream in enumerate(node.inputs()):
        if upstream is not None:
            record["inputs"].append({"index": index,
                                     "path": upstream.path(),
                                     "type": upstream.type().name()})
    return record


def main() -> int:
    args = _parse_args()
    import hou

    hou.hipFile.load(os.path.abspath(args.hip), suppress_save_prompt=True,
                     ignore_load_warnings=True)

    report: dict = {"hip": args.hip, "root": args.root, "solvers": {}}
    for relative in SOLVERS:
        path = f"{args.root}/{relative}"
        node = hou.node(path)
        if node is None:
            print(f"SOLVER missing {path}")
            continue
        # 这些内层节点处在 solve_erosion 的循环上下文里，单独 cook 会失败；
        # 但它们的参数是静态数据，不需要 cook 也能读。
        report["solvers"][relative] = dump_node(node)
        print(f"SOLVER {relative} ({node.type().name()}) "
              f"parms={len(report['solvers'][relative]['parms'])}")

    root = hou.node(args.root)
    if root is not None:
        report["root_record"] = dump_node(root)

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, default=str)
    print(f"WROTE {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
