"""构建 VulkanLearn 地形节点组库。

用法：

```powershell
& blender.exe --background --python tool\\blender-terrain-import\\build_hf_library.py -- `
  --blend artifacts\\pegasus\\hf_library\\hf_library.blend
```

产出的 ``.blend`` 里是一组**可复用的地形算子节点组**（``VL_HF_*``），
可以像 Houdini 的 heightfield 节点那样逐个调取拼接。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))

for _stale in [m for m in list(sys.modules)
               if m.startswith(("pegasus_hf_core", "pegasus_hf_nodes",
                                "pegasus_hf_erode", "pegasus_hf_presets"))]:
    del sys.modules[_stale]

import pegasus_hf_erode as erode_nodes  # noqa: E402
import pegasus_hf_nodes as nodes  # noqa: E402
import pegasus_hf_presets as presets  # noqa: E402


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blend", type=Path, default=None)
    return parser.parse_args(argv)


def main() -> int:
    args = _parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)

    groups = list(nodes.build_all()) + list(erode_nodes.build_all())
    groups += list(presets.build_all())
    # preset 会重新构建子组，早先拿到的引用可能已被删除，按名字重取。
    names = {group.name for group in groups}
    for name in sorted(names):
        group = bpy.data.node_groups.get(name)
        if group is None:
            print(f"LIB {name}: MISSING after build")
            continue
        print(f"LIB {group.name}: inputs={len(group.interface.items_tree) - 1} "
              f"nodes={len(group.nodes)}")

    # 把库组挂在一个空对象上，保证保存后不被清理
    mesh = bpy.data.meshes.new("VL_HF_Library_Holder")
    holder = bpy.data.objects.new("VL_HF_Library", mesh)
    bpy.context.collection.objects.link(holder)

    if args.blend:
        args.blend.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.blend.resolve()))
        print(f"LIB blend={args.blend}")
    print(f"LIB total_groups={len(names)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
