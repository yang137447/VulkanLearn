"""Capture a dedicated C#/Jolt moving-character fixture through production engine paths."""

import argparse
import html
import json
import subprocess
from pathlib import Path

from capture_gasp_p2 import capture


CAPTURE_SOURCE = r"""using System;
using System.Numerics;
using VulkanLearn;
namespace GaspP4Capture;
// 仅验收夹具：绕圈输入经过生产 Character/Jolt，正式 GASP 控制器保持原样。
public sealed class CircleCharacter : Blueprint
{
    private float elapsed;
    protected override void BeginPlay()
    {
        GetComponent<MeshComponent>("visual").Animation.Play("M_Neutral_Walk_Loop_F");
    }
    protected override void Tick(float dt)
    {
        elapsed += dt;
        var direction = new Vector3(MathF.Cos(elapsed), 0, -MathF.Sin(elapsed));
        Character!.Move(direction * 200);
        Transform.Rotation = new(0, MathF.Atan2(-direction.X, -direction.Z) * 180 / MathF.PI, 0);
    }
}
public sealed class Floor : Blueprint { }
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--build", default="build", type=Path)
    parser.add_argument("--dotnet", default="dotnet", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    fixtures = args.resources.resolve() / "Generated/Validation" / output.name
    if output.exists() or fixtures.exists():
        parser.error("Choose new capture directories")
    output.mkdir(parents=True)
    runtime = args.build.resolve() / "bin/managed"
    bridge = runtime / "VulkanLearn.Runtime.dll"
    project = output / "Capture.csproj"
    project.write_text(
        '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework>'
        '<Nullable>enable</Nullable><AssemblyName>GaspP4Capture</AssemblyName></PropertyGroup>'
        f'<ItemGroup><Reference Include="VulkanLearn.Runtime"><HintPath>{html.escape(str(bridge))}'
        '</HintPath><Private>false</Private></Reference></ItemGroup></Project>', encoding="utf-8")
    (output / "Capture.cs").write_text(CAPTURE_SOURCE, encoding="utf-8")
    result = subprocess.run(
        [str(args.dotnet), "build", str(project), "-c", "Release", "-o", str(output / "managed"), "--nologo"],
        capture_output=True, text=True, encoding="utf-8")
    (output / "managed-build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    result.check_returncode()
    scene = json.loads((args.resources / "Maps/SC_gasp/SC_gasp.json").read_text(encoding="utf-8"))
    scene["name"] = "GASP P4 Trajectory Acceptance"
    scene["description"] = "GASP P4 独立 C#/Jolt 圆周输入验收夹具；青色为物理后实际根历史，橙色为未求未来碰撞的预测；非论文场景，不代表人工键鼠验收。"
    scene["objects"] = [obj for obj in scene["objects"] if obj["name"] != "mannequin"]
    scene["objects"] += [
        {"type": "blueprintInstance", "id": "p4-circle", "name": "P4 circle input fixture",
         "class": "GaspP4Capture.CircleCharacter", "position": [0, 90, 0], "rotation": [0, 0, 0],
         "scale": [1, 1, 1], "properties": {},
         "physics": {"motion": "character", "shape": "capsule", "radius": 30, "halfHeight": 60},
         "components": [{"id": "visual", "type": "mesh", "position": [0, -90, 0], "rotation": [0, 180, 0],
                         "modelPath": "Maps/SC_gasp/Meshes/SM_mannequin.json"}]},
        {"type": "blueprintInstance", "id": "p4-floor", "name": "P4 collision floor",
         "class": "GaspP4Capture.Floor", "position": [0, -50, 0], "rotation": [0, 0, 0],
         "scale": [1, 1, 1], "properties": {}, "components": [],
         "physics": {"shape": "box", "halfExtents": [2000, 50, 2000]}}
    ]
    for obj in scene["objects"]:
        if obj["type"] == "camera":
            obj["position"], obj["look_at"] = [650, 650, 750], [0, 20, -150]
            obj["fov"] = 48
            obj["exposure"]["ev100"] = 3
        if obj["type"] == "directionalLight":
            obj["intensity"] = 10
    fixtures.mkdir(parents=True)
    relative = f"Generated/Validation/{output.name}/SC_circle.json"
    (args.resources / relative).write_text(json.dumps(scene, ensure_ascii=False, indent=2), encoding="utf-8")
    config_path = root / "config/config.json"
    original = config_path.read_bytes()
    config = json.loads(original)
    config["scripts"] = {"enabled": True, "autoPlay": True, "runtimeDirectory": str(runtime),
                         "assembly": str(output / "managed/GaspP4Capture.dll")}
    report = {"fixture": relative, "source": "isolated circle input, production C#/Jolt/animation",
              "processClosure": "owned processes terminated after captures; not graceful teardown evidence"}
    try:
        config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
        for threads in [1, 2]:
            capture(args.build.resolve() / "bin/main.exe", root, output, f"circle-t{threads}", relative,
                    ["motiondebug 1"], threads, 180, True)
    finally:
        config_path.write_bytes(original)
    report["configRestored"] = config_path.read_bytes() == original
    (output / "capture.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
