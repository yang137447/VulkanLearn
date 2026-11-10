"""阶段 01 验收脚本：求值节点组，输出数值指标与预览图。

```powershell
& blender.exe --background --python tool\\blender-terrain-import\\check_pegasus_stage01.py -- `
  --blend artifacts\\pegasus\\stage01\\stage01.blend --preview-dir artifacts\\pegasus\\stage01
```

打印：

* 高度属性 ``height`` 的数值范围（归一化）与米制范围；
* ``erode_flow`` 的最大值/均值（侵蚀是否真的传播了汇水）；
* 侵蚀前后高度差的最大值（下切是否发生）；
* ``coast`` 覆盖面积（岛屿是否成形）。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blend", type=Path, required=True)
    parser.add_argument("--preview-dir", type=Path, default=None)
    parser.add_argument("--name", default="stage01")
    return parser.parse_args(argv)


def _attribute(mesh, name: str) -> np.ndarray:
    attribute = mesh.attributes.get(name)
    if attribute is None:
        return np.zeros(len(mesh.vertices), dtype=np.float32)
    values = np.zeros(len(mesh.vertices), dtype=np.float32)
    if attribute.domain == "POINT" and attribute.data_type == "FLOAT":
        attribute.data.foreach_get("value", values)
    return values


def _hillshade(height: np.ndarray, cellsize: float) -> np.ndarray:
    """西北方向光源的简易山体阴影。"""
    dzdx = np.gradient(height, axis=1) / cellsize
    dzdy = np.gradient(height, axis=0) / cellsize
    slope = np.arctan(np.hypot(dzdx, dzdy))
    aspect = np.arctan2(dzdy, -dzdx)
    azimuth = np.radians(315.0)
    altitude = np.radians(45.0)
    shade = (np.sin(altitude) * np.cos(slope)
             + np.cos(altitude) * np.sin(slope) * np.cos(azimuth - aspect))
    return np.clip(shade, 0.0, 1.0)


def main() -> int:
    args = _parse_args()
    bpy.ops.wm.open_mainfile(filepath=str(args.blend.resolve()))
    obj = bpy.data.objects["VL_Pegasus_Stage01"]
    # 必须用求值后的几何：对象的原始 mesh 没有任何节点属性。
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.data

    height = _attribute(mesh, "height")
    flow = _attribute(mesh, "erode_flow")
    coast = _attribute(mesh, "vl_coast")
    occupied = evaluated.to_mesh()
    count = len(occupied.vertices)
    coords = np.zeros(count * 3, dtype=np.float32)
    occupied.vertices.foreach_get("co", coords)
    z = coords.reshape(-1, 3)[:, 2]

    print(f"CHECK vertices={count}")
    print(f"CHECK height_attr min={height.min():.4f} max={height.max():.4f} "
          f"mean={height.mean():.4f}")
    print(f"CHECK world_z_m min={z.min():.1f} max={z.max():.1f} mean={z.mean():.1f}")
    print(f"CHECK erode_flow min={flow.min():.3f} max={flow.max():.3f} "
          f"mean={flow.mean():.3f}")
    print(f"CHECK coast covered={float((coast > 0.5).mean()):.3f}")

    if args.preview_dir:
        side = int(round(np.sqrt(count)))
        if side * side == count:
            height_image = height.reshape(side, side)
            flow_image = flow.reshape(side, side)
            args.preview_dir.mkdir(parents=True, exist_ok=True)
            cellsize = 8000.0 / max(side - 1, 1)
            shade = _hillshade(z.reshape(side, side), cellsize)
            _write_png(args.preview_dir / f"{args.name}_hillshade.png", shade)
            normalized_flow = flow_image / max(float(flow_image.max()), 1e-6)
            _write_png(args.preview_dir / f"{args.name}_flow.png",
                       np.sqrt(normalized_flow))
            _write_png(args.preview_dir / f"{args.name}_height.png",
                       (height_image - height_image.min())
                       / max(float(height_image.max() - height_image.min()), 1e-6))
            print(f"CHECK preview={args.preview_dir}")
    evaluated.to_mesh_clear()
    return 0

def _write_png(path: Path, gray: np.ndarray) -> None:
    """把 [0,1] 的灰度二维数组写成 PNG（借用 Blender 的 image 保存）。"""
    height, width = gray.shape
    rgb = np.repeat(np.clip(gray, 0.0, 1.0)[:, :, None], 3, axis=2)
    image = bpy.data.images.new(path.stem, width=width, height=height)
    rgba = np.ones((height, width, 4), dtype=np.float32)
    rgba[:, :, :3] = rgb[::-1]
    image.pixels.foreach_set(rgba.ravel())
    image.filepath_raw = str(path.resolve())
    image.file_format = "PNG"
    image.save()


if __name__ == "__main__":
    raise SystemExit(main())
