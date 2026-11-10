"""把 Project Pegasus 的 Houdini 资产解码成可移植的"配方"文本。

Houdini 的 .hip/.hda 是二进制容器（见 ``houdini_archive``）。本脚本把每个
SOP 节点网络还原成人类可读、可 diff 的配方：

```text
<asset>/hdaroot/node.net      节点连接（哪根输出接到哪个输入）
<asset>/hdaroot/node.order    节点顺序
<asset>/hdaroot/<node>.init   节点类型
<asset>/hdaroot/<node>.parm   参数值（含 VEX snippet）
```

输出目录默认在仓库外的 ``D:\\YYBWorkSpace\\Pegasus\\decoded``，不入库：
Pegasus 教程资产有独立授权条款，解码结果只用于本机把实现翻译成
Blender 几何节点。

用法：

    py -3 tool\\houdini-terrain-import\\decode_pegasus.py --check
    py -3 tool\\houdini-terrain-import\\decode_pegasus.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import houdini_archive as ha  # noqa: E402

DEFAULT_SOURCE = Path(r"D:\YYBWorkSpace\Pegasus")
DEFAULT_OUTPUT = Path(r"D:\YYBWorkSpace\Pegasus\decoded")

# 节点网络里真正决定地形生成的 SOP 类型：几何节点移植时逐个对照这些。
TERRAIN_SOP_TYPES = (
    "heightfield", "heightfield_erode", "heightfield_maskbyfeature",
    "heightfield_noise", "heightfield_distort", "heightfield_blur",
    "heightfield_remap", "heightfield_resample", "heightfield_slump",
    "heightfield_layer", "heightfield_copylayer", "heightfield_layerclear",
    "heightfield_curvature", "heightfield_project", "heightfield_maskbyobject",
    "heightfield_cutoutbyobject", "heightfield_file", "heightfield_output",
    "attribwrangle", "volumewrangle", "attribexpression", "labs_",
)


@dataclass
class SopNode:
    name: str
    kind: str = ""
    parms: dict[str, str] = field(default_factory=dict)
    snippet: str = ""


@dataclass
class NodeNetwork:
    asset: str
    order: list[str] = field(default_factory=list)
    links: list[tuple[str, str]] = field(default_factory=list)
    nodes: dict[str, SopNode] = field(default_factory=dict)

    def terrain_nodes(self) -> list[SopNode]:
        return [node for node in self.nodes.values()
                if any(marker in node.kind for marker in TERRAIN_SOP_TYPES)]

    def to_dict(self) -> dict:
        return {
            "asset": self.asset,
            "order": self.order,
            "links": [{"from": src, "to": dst} for src, dst in self.links],
            "nodes": [
                {"name": node.name, "type": node.kind,
                 "parms": node.parms, "snippet": node.snippet}
                for node in self.nodes.values()
            ],
        }


def _parse_parm_block(text: str) -> dict[str, str]:
    """解析 ``<node>.parm``。

    格式为每个参数一行：``name\\t[ .. ]\\t( value )``；但 ``snippet`` 这类
    字符串参数的值可以跨行、内部含括号，所以不能用行正则切分——改为找到
    ``(`` 之后扫描到匹配的 ``)``。
    """
    parms: dict[str, str] = {}
    cursor = 0
    head = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\t\[[^\]]*\]\t\(\t?", re.M)
    while True:
        match = head.search(text, cursor)
        if not match:
            break
        value_start = match.end()
        value_end = _find_closing_paren(text, value_start)
        if value_end < 0:
            break
        value = text[value_start:value_end]
        if value.endswith("\t"):
            value = value[:-1]
        parms[match.group(1)] = value
        cursor = value_end + 1
    return parms


def _find_closing_paren(text: str, start: int) -> int:
    """从 ``start`` 起找匹配的右括号，跳过引号内的括号。"""
    depth = 0
    in_string = False
    index = start
    while index < len(text):
        char = text[index]
        if char == '"' and (index == 0 or text[index - 1] != "\\"):
            in_string = not in_string
        elif not in_string:
            if char == "(":
                depth += 1
            elif char == ")":
                if depth == 0:
                    return index
                depth -= 1
        index += 1
    return -1


def _parse_inputs(def_text: str) -> list[tuple[int, str, str]]:
    """从 ``<node>.def`` 解析连线：``inputs { <槽> <源节点> <输出> <flag> }``。

    返回 ``(输入槽, 源节点, 输出名)``。``outputsNamed3`` 给出每个输出索引的
    名字，用来把 ``<输出>`` 换算成可读名字。
    """
    outputs: dict[str, str] = {}
    named = re.search(r"outputsNamed3\s*\{(.*?)\}", def_text, re.S)
    if named:
        for index, name in re.findall(r'(\d+)\s+"([^"]+)"', named.group(1)):
            outputs[index] = name

    links: list[tuple[int, str, str]] = []
    block = re.search(r"\ninputs\s*\{(.*?)\}", def_text, re.S)
    if not block:
        return links
    for line in block.group(1).splitlines():
        tokens = line.split()
        if len(tokens) < 2 or not tokens[0].isdigit():
            continue
        slot = int(tokens[0])
        source = tokens[1]
        out_index = tokens[2] if len(tokens) > 2 else "0"
        links.append((slot, source, outputs.get(out_index, f"output{out_index}")))
    return links


def decode_asset(path: Path) -> NodeNetwork:
    return _decode_members(path.name, ha.open_archive(path).members)


def decode_asset_from_tmp(members: list[bytes]) -> NodeNetwork:
    """测试辅助：用内存里的成员块构造网络，不落盘。"""
    return _decode_members("synthetic.hda", _members_from_blobs(members))


def _members_from_blobs(blobs: list[bytes]) -> list[ha.ArchiveMember]:
    records = ha._split_subrecords(b"".join(blobs))
    return [ha.ArchiveMember(source="synthetic.hda", index=index,
                             data=payload, name=name)
            for index, (name, payload) in enumerate(records)]


def _decode_members(asset_name: str, members: list[ha.ArchiveMember]) -> NodeNetwork:
    network = NodeNetwork(asset=asset_name)

    for member in members:
        name = member.name or ""
        # .hda 的节点路径以 hdaroot 为根；.hip 的以 obj/ 等 Houdini 网络根开头。
        if not (name.startswith("hdaroot") or name.startswith("obj/")
                or name.startswith("mat/") or name.startswith("stage/")):
            continue
        # 网络层级：hdaroot/a/b/<node>.<kind> 里的 a/b 是子网络路径。
        root, _, node_path = name.partition("/")
        if not name.startswith("hdaroot"):
            node_path = name
        node_path = node_path.rsplit(".", 1)[0] if "." in node_path else node_path
        if name.endswith(".init"):
            node = node_path or "<root>"
            entry = network.nodes.setdefault(node, SopNode(name=node))
            entry.kind = member.text.splitlines()[0].split("=", 1)[-1].strip()
        elif name.endswith(".parm"):
            node = node_path or "<root>"
            entry = network.nodes.setdefault(node, SopNode(name=node))
            entry.parms.update(_parse_parm_block(member.text))
            entry.snippet = entry.parms.get("snippet", "")
        elif name.endswith(".def"):
            node = node_path or "<root>"
            for slot, source, output in _parse_inputs(member.text):
                network.links.append((f"{source}.{output}", f"{node}:{slot}"))
        elif name.endswith(".order"):
            network.order = [line.strip() for line in member.text.splitlines()
                             if line.strip() and not line.strip().isdigit()]

    # 根节点的名字在文件名里，留作标记，不参与节点表。
    network.nodes.pop("<root>", None)
    return network


def iter_assets(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*")
                  if path.is_file()
                  and path.suffix.lower() in (".hip", ".hda")
                  # 跳过被 rglob 当成目录名一部分的临时/备份文件
                  and not path.name.startswith("_"))


def write_network(network: NodeNetwork, output: Path) -> Path:
    target = output / Path(network.asset).stem
    target.mkdir(parents=True, exist_ok=True)
    lines = [f"# {network.asset}", ""]
    if network.order:
        lines.append("## node order")
        lines.extend(network.order)
        lines.append("")
    if network.links:
        lines.append("## links")
        for src, slot in network.links:
            lines.append(f"{slot} <- {src}")
        lines.append("")
    for node in network.nodes.values():
        lines.append(f"## {node.name}  ({node.kind})")
        for key, value in node.parms.items():
            if key == "snippet":
                continue
            lines.append(f"  {key} = {value}")
        if node.snippet:
            lines.append("  --- VEX ---")
            lines.extend(f"  {line}" for line in node.snippet.splitlines())
        lines.append("")
    (target / "network.txt").write_text("\n".join(lines), encoding="utf-8")
    (target / "network.json").write_text(
        json.dumps(network.to_dict(), ensure_ascii=False, indent=2),
        encoding="utf-8")
    return target


def main(argv: list[str] | None = None) -> int:
    # Windows 控制台默认 GBK，节点名/snippet 里有非 ASCII 字符时直接崩。
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(
        description="Decode Project Pegasus Houdini assets into readable recipes.")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true",
                        help="only summarise the assets, do not write files")
    parser.add_argument("--json", action="store_true",
                        help="machine-readable summary")
    args = parser.parse_args(argv)

    assets = iter_assets(args.source.resolve())
    if not assets:
        print(f"error: no .hip/.hda under {args.source}", file=sys.stderr)
        return 1

    summary = []
    for asset in assets:
        network = decode_asset(asset)
        nodes = network.terrain_nodes()
        snips = sum(1 for node in network.nodes.values() if node.snippet)
        summary.append({
            "asset": asset.name,
            "nodes": len(network.nodes),
            "links": len(network.links),
            "terrainNodes": [f"{n.name}:{n.kind}" for n in nodes],
            "vexNodes": snips,
        })
        if not args.check:
            write_network(network, args.output.resolve())

    if args.json:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return 0
    for row in summary:
        print(f"{row['asset']}")
        print(f"  nodes={row['nodes']} links={row['links']} vex={row['vexNodes']}")
        for node in row["terrainNodes"]:
            print(f"    {node}")
    if not args.check:
        print(f"wrote {len(summary)} recipes to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
