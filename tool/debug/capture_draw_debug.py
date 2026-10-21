"""Capture shared DrawDebug through production console paths, outside CTest."""
import argparse
import html
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "animation"))
from capture_gasp_p2 import capture


CSHARP_SOURCE = r"""using System.Numerics;
using VulkanLearn;
namespace DrawDebugCapture;
public sealed class Shapes : Blueprint
{
    private readonly DebugDrawStyle style = new() { Duration = -1, Category = "CSharp", Color = new(0.2f, 1, 0.4f, 1) };
    protected override void BeginPlay()
    {
        DebugDraw.DrawDebugPoint(new(-220, 100, 0), 12, style);
        DebugDraw.DrawDebugPolyline([new(-150, 30, 0), new(-150, 110, 0), new(-90, 60, 0)], true, style);
        DebugDraw.DrawDebugArrow(new(-20, 30, 0), new(-20, 110, 0), 18, style);
        DebugDraw.DrawDebugCircle(new(110, 50, 0), 40, style: style);
        DebugDraw.DrawDebugSphere(new(-210, 65, -160), 40, style);
        DebugDraw.DrawDebugBox(new(-100, 65, -160), new(35, 45, 30), style: style);
        DebugDraw.DrawDebugCapsule(new(20, 70, -160), 25, 35, style: style);
        DebugDraw.DrawDebugCoordinateSystem(new(110, 30, -160), 75, style: style);
        DebugDraw.DrawDebugString(new(-220, 160, 0), "<C#> & DrawDebug", 18, style);
    }
    protected override void Tick(float dt)
    {
        // 单帧项必须每个 GT 帧重新提交，不能靠持久项掩盖显示链路的问题。
        DebugDraw.DrawDebugLine(new(-260, 30, 0), new(-180, 60, 0),
            new() { Category = "CSharp", Color = new(1, 0.2f, 0.3f, 1), Thickness = 4 });
    }
}
public sealed class DepthShapes : Blueprint
{
    [Expose(InstanceEditable = true)] public int Mode { get; set; }
    protected override void BeginPlay()
    {
        if (Mode == 0) return;
        var style = new DebugDrawStyle { Duration = -1, Thickness = 5,
            Depth = Mode == 2 ? DebugDepthMode.DepthTest : DebugDepthMode.Overlay };
        DebugDraw.DrawDebugLine(new(-150, 130, -80), new(150, 130, -80),
            style with { Color = new(1, 0, 1, 1) });
        DebugDraw.DrawDebugLine(new(-150, 90, 100), new(150, 90, -100),
            style with { Color = new(0, 1, 1, 1) });
        DebugDraw.DrawDebugLine(new(-150, 150, 80), new(150, 150, 80),
            style with { Color = new(0, 1, 0, 1) });
        DebugDraw.DrawDebugPoint(new(0, 110, -80), 16, style with { Color = new(1, 0, 0, 1) });
        DebugDraw.DrawDebugPoint(new(20, 110, 80), 16, style with { Color = new(0, 0, 1, 1) });
        DebugDraw.DrawDebugBox(new(0, 100, 0), new(80, 80, 80),
            style: style with { Color = new(1, .4f, 0, 1), Thickness = 2 });
        DebugDraw.DrawDebugString(new(-40, 100, -80), "BEHIND", 18,
            style with { Color = new(1, 1, 0, 1) });
        DebugDraw.DrawDebugString(new(-40, 70, 80), "FRONT", 18,
            style with { Color = new(1, 1, 1, 1) });
    }
}
"""


def capture_csharp(root, output, build, dotnet):
    config_path = root / "config/config.json"
    original = config_path.read_bytes()
    config = json.loads(original)
    resources = (root / config["resourcePath"]).resolve()
    relative = f"Generated/Validation/{output.name}/SC_debug.json"
    fixture = resources / relative
    if fixture.parent.exists():
        raise FileExistsError(fixture.parent)
    runtime = build / "bin/managed"
    project = output / "Capture.csproj"
    project.write_text(
        '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework>'
        '<AssemblyName>DrawDebugCapture</AssemblyName></PropertyGroup>'
        '<ItemGroup><Reference Include="VulkanLearn.Runtime"><HintPath>'
        f'{html.escape(str(runtime / "VulkanLearn.Runtime.dll"))}'
        '</HintPath><Private>false</Private></Reference></ItemGroup></Project>', encoding="utf-8")
    (output / "Capture.cs").write_text(CSHARP_SOURCE, encoding="utf-8")
    result = subprocess.run([str(dotnet), "build", str(project), "-c", "Release",
                             "-o", str(output / "managed"), "--nologo"],
                            capture_output=True, text=True, encoding="utf-8")
    (output / "managed-build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    result.check_returncode()
    scene = json.loads((resources / "Maps/SC_gasp/SC_gasp.json").read_text(encoding="utf-8"))
    scene["name"] = "DrawDebug C# Acceptance"
    scene["description"] = "公共 DrawDebug 的 C# 形状与单帧线段显示验收；非论文验证场景。"
    scene["objects"].append({"name": "C# debug shapes", "type": "blueprintInstance",
                             "id": "debug-demo", "class": "DrawDebugCapture.Shapes",
                             "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1],
                             "properties": {}, "components": []})
    fixture.parent.mkdir(parents=True)
    fixture.write_text(json.dumps(scene, ensure_ascii=False, indent=2), encoding="utf-8")
    config["scripts"] = {"enabled": True, "autoPlay": True, "runtimeDirectory": str(runtime),
                         "assembly": str(output / "managed/DrawDebugCapture.dll")}
    try:
        config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
        for threads in [1, 2]:
            image = capture(build / "bin/main.exe", root, output, f"csharp-t{threads}", relative,
                            ["animation seek 0", "camera pose -60 360 800 -60 80 -80", "debugdraw status"],
                            threads, 120, True)
            Image.fromarray(image.astype(np.uint8)).save(output / f"csharp-t{threads}.png")
        depth_scene = json.loads(json.dumps(scene))
        depth_scene["name"] = "DrawDebug Depth Acceptance"
        depth_scene["description"] = "公共 DrawDebug 可选深度的前后遮挡、穿墙线段及文字验收；非论文验证场景。"
        depth_scene["objects"] = [o for o in depth_scene["objects"] if o["type"] not in ["mesh", "blueprintInstance"]]
        depth_scene["objects"].append({"name": "occluder", "type": "mesh",
            "modelPath": "Maps/SC_gasp/Meshes/SM_ground.json", "position": [0, 100, 0],
            "rotation": [90, 0, 0], "scale": [.1, 1, .1]})
        depth_scene["objects"].append({"name": "depth drawings", "type": "blueprintInstance",
            "id": "debug-depth", "class": "DrawDebugCapture.DepthShapes", "position": [0, 0, 0],
            "rotation": [0, 0, 0], "scale": [1, 1, 1], "properties": {}, "components": []})
        images = {}
        for name, mode, threads, editor in [
                ("depth-baseline", 0, 1, False), ("depth-overlay", 1, 1, False),
                ("depth-tested", 2, 1, False), ("depth-t2", 2, 2, False),
                ("depth-editor-overlay", 1, 2, True), ("depth-editor", 2, 2, True)]:
            depth_scene["objects"][-1]["properties"]["Mode"] = mode
            path = fixture.parent / f"SC_{name}.json"
            path.write_text(json.dumps(depth_scene, ensure_ascii=False, indent=2), encoding="utf-8")
            images[name] = capture(build / "bin/main.exe", root, output, name,
                path.relative_to(resources).as_posix(), ["camera pose 0 100 800 0 100 0", "debugdraw status"],
                threads, 120, editor)
            Image.fromarray(images[name].astype(np.uint8)).save(output / f"{name}.png")
        # 纯色调试像素不受场景曝光影响；固定 ROI 排除不同场景名/帧率 HUD。
        def colors(image):
            r, g, b = np.moveaxis(image[120:620, 200:1080], 2, 0)
            return {"behindLine": int(((r > 230) & (b > 230) & (g < 30)).sum()),
                    "crossingLine": int(((g > 230) & (b > 230) & (r < 30)).sum()),
                    "frontLine": int(((g > 230) & (r < 30) & (b < 30)).sum()),
                    "behindPoint": int(((r > 230) & (g < 30) & (b < 30)).sum()),
                    "frontPoint": int(((b > 230) & (r < 30) & (g < 30)).sum()),
                    "behindText": int(((r > 230) & (g > 230) & (b < 30)).sum())}
        overlay, tested = colors(images["depth-overlay"]), colors(images["depth-tested"])
        delta = np.abs(images["depth-tested"][120:620, 200:1080] - images["depth-t2"][120:620, 200:1080])
        depth_report = {"overlayPixels": overlay, "depthPixels": tested,
                        "singleDualMaxByteError": int(delta.max())}
        depth_report["passed"] = (
            0 < tested["behindLine"] < overlay["behindLine"] * .8 and
            0 < tested["crossingLine"] < overlay["crossingLine"] * .95 and
            abs(tested["frontLine"] - overlay["frontLine"]) <= 10 and
            overlay["behindPoint"] > 100 and tested["behindPoint"] == 0 and
            tested["frontPoint"] > 100 and overlay["behindText"] > 20 and
            tested["behindText"] == 0 and int(delta.max()) == 0)
    finally:
        config_path.write_bytes(original)
    return {"fixture": relative, "configRestored": config_path.read_bytes() == original,
            "depth": depth_report}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--dotnet", type=Path, help="Also build/capture an isolated C# gameplay fixture")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if output.exists():
        parser.error("Choose a new output directory")
    output.mkdir(parents=True)
    scene = "Maps/SC_gasp/SC_gasp.json"
    setup = ["animation seek 0", "camera pose -60 360 800 -60 80 -80"]
    cases = [
        ("baseline", [], 1, False),
        ("demo", ["debugdraw demo"], 1, False),
        ("demo-t2", ["debugdraw demo"], 2, False),
        ("disabled", ["debugdraw demo", "debugdraw off"], 1, False),
        ("category", ["debugdraw demo", 'debugdraw category "DrawDebug" off'], 1, False),
        ("owner-hidden", ["debugdraw demo", "debugdraw owner missing"], 1, False),
        ("owner-demo", ["debugdraw demo", "debugdraw owner Demo"], 1, False),
        ("cleared", ["debugdraw demo", "debugdraw clear owner Demo"], 1, False),
        ("editor", ["debugdraw demo"], 2, True),
        ("motion", ["motiondebug 1", "debugdraw demo"], 2, True),
        ("depth-demo", ["debugdraw demo depth"], 2, True),
        ("depth-reload", ["debugdraw demo depth", "shaderreload all"], 2, True),
    ]
    images = {}
    for name, commands, threads, editor in cases:
        images[name] = capture(args.build.resolve() / "bin/main.exe", root, output, name,
                               scene, setup + commands + ["debugdraw status"], threads, 120, editor)
        Image.fromarray(images[name].astype(np.uint8)).save(output / f"{name}.png")
    # HUD 中的性能/时钟和异步环境采样不属于几何对照；固定区域包含完整演示图形。
    roi = np.s_[150:540, 200:1080]
    report = {"processClosure": "owned processes terminated; not graceful teardown evidence"}
    for name in ["demo-t2", "disabled", "category", "owner-hidden", "owner-demo", "cleared"]:
        reference = "demo" if name in ["demo-t2", "owner-demo"] else "baseline"
        delta = np.abs(images[name][roi] - images[reference][roi]).max(axis=2)
        report[name] = {"reference": reference, "pixelsOver4": int((delta > 4).sum()),
                        "maxByteError": int(delta.max()), "passed": int((delta > 4).sum()) <= 100}
    delta = np.abs(images["demo"][roi] - images["baseline"][roi]).max(axis=2)
    report["visible"] = {"changedPixels": int((delta > 20).sum()), "passed": int((delta > 20).sum()) > 1000}
    delta = np.abs(images["depth-demo"][120:440, 100:870] - images["depth-reload"][120:440, 100:870]).max(axis=2)
    reload_log = (output / "depth-reload.log").read_text(encoding="utf-8")
    committed = "committed=true" in reload_log
    report["uiReload"] = {"pixelsOver4": int((delta > 4).sum()), "committed": committed,
                          "passed": committed and int((delta > 4).sum()) <= 100}
    report["passed"] = all(v["passed"] for v in report.values() if isinstance(v, dict))
    if args.dotnet:
        report["csharp"] = capture_csharp(root, output, args.build.resolve(), args.dotnet.resolve())
        report["passed"] = report["passed"] and report["csharp"]["configRestored"] and report["csharp"]["depth"]["passed"]
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
