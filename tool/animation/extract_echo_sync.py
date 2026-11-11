"""Extract UE Sequence Player sync metadata from an exported AnimBP T3D.

The AnimBP owns SyncGroup/role/start-position/loop; the AnimSequence itself
does not. This parser is read-only and only consumes UE's text export.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


NODE = re.compile(r"Node=\((.*)\)\s*$")
GROUP = re.compile(r'GroupName="([^"]*)"')
ROLE = re.compile(r"GroupRole=([A-Za-z0-9_]+)")
METHOD = re.compile(r"Method=([A-Za-z0-9_]+)")
SCOPE = re.compile(r"SyncGroupScope=(?:EAnimSyncGroupScope::)?([A-Za-z0-9_]+)")
SEQUENCE = re.compile(r"Sequence=\"/Script/Engine\.AnimSequence'([^']+)'\"")
START = re.compile(r"StartPosition=([-+0-9.eE]+)")
LOOP = re.compile(r"bLoopAnimation=(True|False)")

ROLES = {
    "CanBeLeader", "AlwaysFollower", "AlwaysLeader",
    "TransitionLeader", "TransitionFollower", "ExclusiveAlwaysLeader",
}
METHODS = {"DoNotSync", "SyncGroup", "Graph"}


def parse_node(line: str):
    match = NODE.search(line)
    if not match:
        return None
    node = match.group(1)
    sequence = SEQUENCE.search(node)
    if not sequence:
        return None
    path = sequence.group(1)
    clip = path.rsplit("/", 1)[-1].split(".", 1)[0]
    group = GROUP.search(node)
    role = ROLE.search(node)
    method = METHOD.search(node)
    scope = SCOPE.search(node)
    start = START.search(node)
    loop = LOOP.search(node)
    return {
        "clip": clip,
        "syncGroup": group.group(1) if group else "",
        "syncRole": role.group(1) if role else "CanBeLeader",
        "syncMethod": method.group(1) if method else "DoNotSync",
        # UE 5.8 的 T3D 不一定导出该字段；Echo 资产按 Local 显式落盘，
        # 不从运行时猜默认值。
        "syncGroupScope": scope.group(1) if scope else "Local",
        "startPosition": float(start.group(1)) if start else 0.0,
        "loop": loop.group(1) == "True" if loop else True,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--t3d", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    players = {}
    for line in args.t3d.read_text(encoding="utf-8").splitlines():
        record = parse_node(line)
        if record is None:
            continue
        clip = record.pop("clip")
        if record["syncRole"] not in ROLES:
            raise RuntimeError("Unknown UE sync role %r for %s" %
                               (record["syncRole"], clip))
        if record["syncMethod"] not in METHODS:
            raise RuntimeError("Unknown UE sync method %r for %s" %
                               (record["syncMethod"], clip))
        if record["syncGroupScope"] not in ("Local", "Component"):
            raise RuntimeError("Unknown UE sync group scope %r for %s" %
                               (record["syncGroupScope"], clip))
        players.setdefault(clip, []).append(record)
    document = {
        "format": "vl.echo.anim-players.v1",
        "source": str(args.t3d),
        "players": players,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8", newline="\n")
    print(json.dumps({"players": len(players), "bindings": sum(
        len(value) for value in players.values())}, ensure_ascii=False))


if __name__ == "__main__":
    main()
