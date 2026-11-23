"""Prepare unapplied minimal lit scheduling/budget and thin-target source patches."""
import argparse
import difflib
import json
from pathlib import Path

from compile_frozen_cpp import digest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.mkdir(exist_ok=False, parents=True)
    before = {}
    patches = []
    def change(relative, transform):
        path = root / relative
        original = path.read_text(encoding="utf-8")
        modified = transform(original)
        if modified == original:
            raise ValueError("Patch anchor missing: "+relative)
        before[relative] = digest(path)
        patches.extend(difflib.unified_diff(original.splitlines(True), modified.splitlines(True),
                       fromfile="a/"+relative, tofile="b/"+relative))
        destination = output / "private" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(modified, encoding="utf-8")
    change("source/render/frame/renderFrameRecorder.cpp", lambda original:
        '#include "render/diagnostics/terrainLitCaptureHooks.h"\n'+original.replace(
            "                terrainCaptureBridge->BeforeGeometry(passContext, ctx, frameContext,",
            "                if (auto* lit = dynamic_cast<ITerrainLitCaptureHooks*>(terrainCaptureHooks.get()))\n"
            "                    lit->SetReferencePoolOwner(compositedTransaction.service->DiagnosticReferencePoolOwner());\n"
            "                terrainCaptureBridge->BeforeGeometry(passContext, ctx, frameContext,", 1).replace(
            "            ctx.passRuntime.RecordPass(renderPassName, passContext);",
            "            ctx.passRuntime.RecordPass(renderPassName, passContext);\n"
            "            RecordOptionalTerrainLitAfterPass(passContext);", 1))
    change("source/render/resource/terrainCompositedService.h", lambda original: original.replace(
        "    std::shared_ptr<TerrainRvtBudgetOwner> BudgetOwner() const;",
        "    std::shared_ptr<TerrainRvtBudgetOwner> BudgetOwner() const;\n"
        "    std::shared_ptr<TerrainRvtBudgetOwner> DiagnosticReferencePoolOwner() const;", 1))
    change("source/render/resource/terrainCompositedService.cpp", lambda original: original.replace(
        "TerrainRvtBudgetLease TerrainCompositedService::AdmitCpuWorkspace(uint64_t bytes)",
        "std::shared_ptr<TerrainRvtBudgetOwner> TerrainCompositedService::DiagnosticReferencePoolOwner() const\n"
        "{\n"
        "    Require(impl->frozen && impl->peakOwner, \"Diagnostic reference requires actual prepared peak/outer owners\");\n"
        "    return std::make_shared<TerrainRvtBudgetOwner>(impl->frozen->steadyCaps,\n"
        "        impl->peakOwner, impl->frozen->terrainOwner);\n"
        "}\n\n"
        "TerrainRvtBudgetLease TerrainCompositedService::AdmitCpuWorkspace(uint64_t bytes)", 1))
    tools = root / "tool/validation/terrain/native-capture"
    for suffix in ("h", "cpp"):
        relative = "source/render/diagnostics/terrainLitCaptureHooks."+suffix
        text = (tools / ("terrainLitCaptureHooks."+suffix+".in")).read_text()
        patches.extend(difflib.unified_diff([], text.splitlines(True),
                       fromfile="/dev/null", tofile="b/"+relative))
        path = output / "private" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    cmake = (
        '\nadd_executable(native_terrain_lit_capture_game\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/nativeTerrainLitGame.cpp"\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/nativeTerrainCaptureAdapter.cpp"\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/terrainTesCapture.cpp"\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/terrainTesCapturePolicy.cpp"\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/terrainTesCapturePipeline.cpp"\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/terrainReferenceAtlas.cpp"\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/terrainLitTarget.cpp"\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/terrainLitReplaySession.cpp"\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/terrainLitCaptureAdapter.cpp"\n'
        '    "${CMAKE_CURRENT_LIST_DIR}/terrainLitCase.cpp")\n'
        'target_compile_features(native_terrain_lit_capture_game PRIVATE cxx_std_17)\n'
        'target_include_directories(native_terrain_lit_capture_game PRIVATE\n'
        '    "${VL_TERRAIN_CAPTURE_ROOT}/source" "${CMAKE_CURRENT_LIST_DIR}")\n'
        'target_link_libraries(native_terrain_lit_capture_game PRIVATE vl_runtime)\n'
        'set_target_properties(native_terrain_lit_capture_game PROPERTIES\n'
        '    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")\n')
    change("tool/validation/terrain/native-capture/capture-targets.cmake", lambda original: original+cmake)
    (output / "lit-scheduling-request.patch").write_text("".join(patches), encoding="utf-8")
    report = {"gpuExecuted": False, "sharedWrites": [], "baseSourceHashes": before,
        "frozenBaseInterfaceSha256": digest(root / "source/render/diagnostics/terrainCaptureHooks.h"),
        "status": "Unapplied source request for main/Dirac/Euclid; no shared Frame/Service/CMake edits",
        "budget": "New diagnostic child uses existing steady caps with both actual peak RVT and outer terrain parents. No increased caps or unparented owner. Actual VkReq admission can fail.",
        "frameDelta": "One reference-owner callback before existing BeforeGeometry, one AfterActualPass after actual RecordPass",
        "thinCli": {"exe": "build-vt-release/bin/native_terrain_lit_capture_game.exe",
            "environment": ["VULKANLEARN_TERRAIN_CAPTURE_CASE=<existing one-patch fixture>",
                            "VULKANLEARN_TERRAIN_LIT_CASE=<native slot/page-bound lit case>"],
            "args": "Reuse first-junction-case-r2/launch.json camera/scene/worker2/180-frame arguments exactly"},
        "stillRequiredNativeInputs": ["Actual ROI tap-slot/page identity footprint and pinned original-GPU full-channel outputs",
            "Actual atlas formats/capacity matched to native descriptor resource",
            "Independent fixed camera-bound terrain ROI and silhouette masks",
            "Candidate replay vs actual final-render-output calibration; no shipping/debug material fallback"],
        "notClaimed": "No GPU run, no whole capture/lit gate PASS; private components are not a fully linked ordinary core"}
    (output / "request.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({"output": str(output), "sharedWrites": [], "baseInterfaceUnchanged": True}))


if __name__ == "__main__":
    main()
