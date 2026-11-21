"""CPU-only preparation: compile private instrumentation, bind actual inputs, retain missing hooks."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
from rvt_visual_oracle import source_mip2, thresholds
from height_bilinear_reference import bilinear, validate_oracle_anchors

ROOT = Path(__file__).resolve().parents[3]
MATRIX = ROOT / "artifacts/terrain-rvt-production/p5/tour-final-runtime-evidence-matrix-cpu-r2"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")


DECLARATIONS = """
layout(set=4, binding=0, std430) buffer TerrainTestTesCapture
{
    uvec4 control;
    uvec4 frameEpoch;
    uvec4 worldPose;
    uvec4 drawIdentity;
    vec4 samples[];
} terrainTestTesCapture;
"""

WRITE = """
    if (uint(gl_PrimitiveID) >= terrainTestTesCapture.control.z &&
        uint(gl_PrimitiveID) - terrainTestTesCapture.control.z < terrainTestTesCapture.control.w)
    {
        uint slot = atomicAdd(terrainTestTesCapture.control.x, 1u);
        uint capacity = uint(terrainTestTesCapture.samples.length()) / 12u;
        if (slot >= capacity)
            atomicAdd(terrainTestTesCapture.control.y, 1u);
        else
        {
            uint offset = slot * 12u;
            terrainTestTesCapture.samples[offset] = vec4(gl_TessCoord.xy, float(gl_PrimitiveID), 1.0);
            terrainTestTesCapture.samples[offset+1u] = vec4(terrainTestMacroWorld, displacementFade);
            terrainTestTesCapture.samples[offset+2u] = vec4(worldPosition, cameraDistanceCm);
            terrainTestTesCapture.samples[offset+3u] = vec4(worldNormal, u_layerDisplacementScale);
            terrainTestTesCapture.samples[offset+4u] = vec4(texCoord, texCoord1);
            terrainTestTesCapture.samples[offset+5u] = terrainTestIds;
            terrainTestTesCapture.samples[offset+6u] = terrainTestWeights;
            terrainTestTesCapture.samples[offset+7u] = terrainTestHeights;
            terrainTestTesCapture.samples[offset+8u] = terrainTestUvX;
            terrainTestTesCapture.samples[offset+9u] = terrainTestUvY;
            terrainTestTesCapture.samples[offset+10u] = terrainTestAmplitudes;
            terrainTestTesCapture.samples[offset+11u] = vec4(u_uvScale, u_detailUvScale, radiusCm, nearFade);
        }
    }
"""


def instrument(root):
    glsl = root / "shader/glsl"
    mf_path = glsl / "materialFunction/mf_terrainLayerBlend.glsl"
    tes_path = glsl / "engine/passTemplate/base.tese.glsl"
    mf = mf_path.read_text(encoding="utf-8")
    tes = tes_path.read_text(encoding="utf-8")
    require = lambda condition, message: None if condition else (_ for _ in ()).throw(ValueError(message))
    require("coverageUv = clamp(context.texCoord1," in mf, "Current fixed coverage reader required")
    signature = "float TerrainBlendedLayerDisplacementMeters("
    require(mf.count(signature) == 1, "Displacement function source anchor changed")
    globals_text = """
vec4 terrainTestIds=vec4(0), terrainTestWeights=vec4(0), terrainTestHeights=vec4(0.5);
vec4 terrainTestUvX=vec4(0), terrainTestUvY=vec4(0), terrainTestAmplitudes=vec4(0);
"""
    mf = mf.replace(signature, globals_text + "\n" + signature, 1)
    height_line = "const float height = TerrainSampleHeightAtlasOrSource(uv, layer, 2.0);"
    require(mf.count(height_line) == 1, "Actual TES height sample anchor changed")
    mf = mf.replace(height_line, height_line + """
        terrainTestIds[slot]=float(layer);
        terrainTestWeights[slot]=weights[slot];
        terrainTestHeights[slot]=height;
        terrainTestUvX[slot]=uv.x;
        terrainTestUvY[slot]=uv.y;""", 1)
    amplitude = "displacement += weights[slot] * (height - 0.5) * layerDisplacementM;"
    require(mf.count(amplitude) == 1, "Actual displacement accumulation anchor changed")
    mf = mf.replace(amplitude, "terrainTestAmplitudes[slot]=layerDisplacementM;\n        " + amplitude, 1)
    require(tes.count("void main()") == 1 and tes.count("gl_Position = clipPosition;") == 1, "TES source anchor changed")
    tes = tes.replace("void main()", DECLARATIONS + "\nvoid main()", 1)
    tes = tes.replace("const float radiusCm =", "const vec3 terrainTestMacroWorld = worldPosition;\n    const float radiusCm =", 1)
    tes = tes.replace("gl_Position = clipPosition;", "gl_Position = clipPosition;\n" + WRITE, 1)
    identities = {}

    def expand(path, overrides):
        path = path.resolve()
        text = path.read_text(encoding="utf-8")
        identities[str(path)] = digest(path)
        text = overrides.get(path, text)
        def include(match):
            target = (path.parent / match[1]).resolve()
            if not target.is_relative_to(glsl.resolve()):
                raise ValueError("Include outside frozen shader root")
            return expand(target, overrides)
        return re.sub(r'#include\s+"([^"]+)"', include, text)
    overrides = {mf_path.resolve(): mf, tes_path.resolve(): tes}
    prefix = "#version 450\n#define VL_TERRAIN_TESSELLATION 1\n"
    parameters = expand(glsl / "generate/M_terrainParamter.glsl", overrides)
    result = prefix + parameters + "\n" + expand(tes_path, overrides)
    return result, identities


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", type=Path, default=ROOT.parent / "p5s/C010")
    parser.add_argument("--coverage-manifest", type=Path, default=ROOT.parent / "p5r/C010/VT/00af1195dc7dfbb0/manifest.json")
    parser.add_argument("--local-manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    project = options.project.resolve(strict=True)
    cases = read(MATRIX / "cases.json")
    checklist = read(MATRIX / "capture-and-oracle-checklist.json")
    limits, frozen_hash = thresholds()
    assert frozen_hash == checklist["frozenSha256"]
    assert len(cases) == 40
    pose_names = {pose["name"] for pose in checklist["poses"]}
    assert len(pose_names) == 53
    coverage_cases = [case for case in cases if case["selection"] == "coverage"]
    for mode in ("CoverageAtlas", "LocalLayerArray"):
        for worker in (1, 2):
            actual = [shot["name"] for case in coverage_cases if case["mode"] == mode and case["worker"] == worker
                      for shot in case["shots"]]
            assert len(actual) == len(set(actual)) == 53 and set(actual) == pose_names
    commands = []
    def run(argv, name):
        result = subprocess.run(list(map(str, argv)), cwd=ROOT, capture_output=True, text=True)
        (output / (name+".log")).write_text(result.stdout+result.stderr, encoding="utf-8")
        commands.append({"argv": list(map(str, argv)), "exitCode": result.returncode})
        result.check_returncode()
    shader, source_inputs = instrument(project)
    shader_path = output / "actual-production-tes-instrumented.tese"
    shader_path.write_text(shader, encoding="utf-8")
    run(["glslangValidator", "-V", "--target-env", "vulkan1.4", shader_path,
         "-o", output / "actual-production-tes-instrumented.spv"], "tes-compile")
    run(["spirv-val", "--target-env", "vulkan1.4", output / "actual-production-tes-instrumented.spv"], "tes-validate")
    anchors = validate_oracle_anchors()
    publications = []
    for manifest_path in (options.coverage_manifest, options.local_manifest):
        if manifest_path is None:
            continue
        manifest_path = manifest_path.resolve(strict=True)
        document = read(manifest_path)
        rows = []
        for surface in document["production"]["surfaces"]:
            _, planes, width, height, identity = source_mip2(manifest_path, surface["surfaceId"])
            values = [bilinear(planes[0], width, height, uv)["exactR16"]
                      for uv in ((0, 0), (1, 1), (-.00096893310546875, 1), (.371, .629))]
            assert values[0] == values[1]
            rows.append({"surfaceId": surface["surfaceId"], "sourceMip2": identity,
                         "sourceDimensions": [width, height], "layers": len(planes), "cpuAnchorR16": values})
        publications.append({"manifest": str(manifest_path), "sha256": digest(manifest_path),
                             "productionHash": document["production"]["hash"],
                             "weightSource": document["production"]["surfaces"][0]["weightSource"], "surfaces": rows})
    binding = {"frame": "{nativeFrame}", "submissionEpoch": "{nativeSubmissionEpoch}",
        "worldGeneration": "{nativeWorld}", "poseId": "{nativePose}", "samplingDomain": "visible-geometry-roi",
        "captureRect": [576, 296, 128, 128], "cameraMatricesSha256": "{actualViewProjectionBytesSha256}",
        "inputClosureSha256": "{frozenSourcesLightsTransformsAndDynamicStateSha256}",
        "renderSettingsSha256": "{exposureTaaMsaaViewportAndToneMappingSha256}",
        "mode": "{CoverageAtlasOrLocalLayerArray}", "worker": "{1or2}",
        "referenceTarget": "same-frozen-frame-inputs"}
    save(output / "packet-template.json", {"version": 1, "role": "candidate", "binding": binding,
        "payload": {"path": "candidate.npz", "sha256": "{actualPayloadSha256}"},
        "nativeCompletion": {"completedEpoch": "{nativeCompletedEpoch}", "complete": True, "overflow": False},
        "provenance": {"gpuExecuted": False, "authority": "native-draw-and-lit-output",
            "heightShaderStage": "tessellation-evaluation", "drawMetadataFromSnapshot": False}})
    hook = {
        "status": "approval-required-not-applied", "productionSourceModified": False,
        "sourceHookFacts": [
            {"path": "source/vulkanManager.cpp", "anchor": "VkPhysicalDeviceFeatures logical-device enable chain",
             "fact": "Current source enables tessellationShader/fragmentStoresAndAtomics; vertexPipelineStoresAndAtomics has zero source hits. TES SSBO writes require this feature, so instrumented SPIR-V cannot be installed into the current device blindly.",
             "minimumPrivateChange": "In test-only device composition query+enable vertexPipelineStoresAndAtomics; fail unavailable if unsupported. No runtime fallback."},
            {"path": "source/pipeline/graphicsPipeline.cpp", "anchor": "CreateDescriptorSetLayouts/CreatePipelineLayout",
             "minimumPrivateChange": "Diagnostic pipeline only: append set4 binding0 storage-buffer visible to TessellationEvaluation, preserving existing sets0..3; bind the admitted per-draw capture buffer. Original default pipeline remains unchanged."},
            {"path": "source/render/frame/renderFrameRecorder.cpp", "anchor": "ctx.passRuntime.RecordPass(renderPassName, passContext);",
             "minimumPrivateChange": "Test-only approved callback receives the immutable same-frame draw snapshot, selected actual VB/patch IB, model/MI descriptors and actual topology/LOD. Before the selected normal geometry draw bind capture set4; initialize header from actual frame/epoch/World/pose/draw. No recomputed distance-based LOD."},
            {"path": "source/render/frame/renderFrameRecorder.cpp", "anchor": "after geometry then after toneMapping, before submission",
             "minimumPrivateChange": "One selected ROI/fence: actual geometry-pass side attachments or approved native readbacks for depth/world/UV/ID/LOD/topology, and same-frame final lit ROI; pin original graph/descriptor owners until completedEpoch. A separate post-ready screenshot is insufficient."}
        ],
        "testOnlyApiProposal": "BeginBoundedCapture(actualDrawSnapshot, budgetLease, frameStamp); RecordGeometryAndLitReadbacks(commandBuffer, actualGraphOwners); TryConsume(completedEpoch). These are proposals, not existing APIs.",
        "tesAllocation": {"capacity": 4096, "recordBytes": 192, "headerBytes": 64,
            "payloadBytes": 786496, "devicePlusHostPayloadBytes": 1572992,
            "header": "uvec4(count,overflow,firstPrimitive,primitiveCount), uvec4(frameLo,frameHi,epochLo,epochHi), uvec4(worldLo,worldHi,poseLo,poseHi), uvec4(drawOrdinal,surfaceIndex,actualLod,actualVkTopology)",
            "selection": "Capture an explicitly recorded primitive interval of the actual LOD0 patch draw; overflow/empty is missing evidence. Never treat subset as whole pose.",
            "admission": "Quote actual VkMemoryRequirements plus descriptors/metadata through frozen Workspace/CPU owners; no budget increase. If rejected, mark missing."},
        "rawLitGeometryRoi": "Default128x128, maximum512x512 only after actual memory admission. Preserve MSAA sample validity or declare resolved-only scope. Color/depth/identity must share actual submission identity, not feedback128² winner. A bounded ROI silhouette result does not close full-frame silhouette; cover the fixed domain with disjoint admitted tiles before aggregating.",
        "reference": "Reuse pinned GPU384 original current evaluator for same frozen base inputs; it is page-space only. Scene lit reference still needs an independent same-input lighting/geometry/TAA renderer or replay reference, not a runtime material fallback.",
        "notClaimed": ["TES instrumentation executed", "same-epoch visible ROI hook exists", "scene-lit independent reference exists", "dynamic reference exists"]}
    save(output / "hook-request.json", hook)
    patches = []
    device_path = ROOT / "source/vulkanManager.cpp"
    device = device_path.read_text(encoding="utf-8")
    anchor = "    vk::PhysicalDeviceVulkan12Features deviceVulkan12Features;"
    assert device.count(anchor) == 1
    patched = device.replace(anchor, """#if defined(VL_TERRAIN_TEST_CAPTURE)
    if (supportedFeatures.vertexPipelineStoresAndAtomics != VK_TRUE)
        throw std::runtime_error("TES capture requires vertexPipelineStoresAndAtomics");
    deviceFeatures.setVertexPipelineStoresAndAtomics(VK_TRUE);
#endif
""" + anchor, 1)
    patches.extend(difflib.unified_diff(device.splitlines(True), patched.splitlines(True),
        fromfile="a/source/vulkanManager.cpp", tofile="b/source/vulkanManager.cpp"))
    graphics_path = ROOT / "source/pipeline/graphicsPipeline.cpp"
    graphics = graphics_path.read_text(encoding="utf-8")
    anchor = """        descriptorLayoutBindings,
        shaderDisplayName);"""
    assert graphics.count(anchor) == 1
    patched = graphics.replace(anchor, """        descriptorLayoutBindings,
        shaderDisplayName
#if defined(VL_TERRAIN_TEST_CAPTURE)
        , std::any_of(descriptorLayoutBindings.begin(), descriptorLayoutBindings.end(),
            [](const ShaderBinding& binding) { return binding.set == 4; })
            ? 5u : MAX_DESCRIPTOR_SETS
#endif
        );""", 1)
    patches.extend(difflib.unified_diff(graphics.splitlines(True), patched.splitlines(True),
        fromfile="a/source/pipeline/graphicsPipeline.cpp", tofile="b/source/pipeline/graphicsPipeline.cpp"))
    patch_path = output / "test-device-layout-request.patch"
    patch_path.write_text("".join(patches), encoding="utf-8")
    run(["git", "apply", "--check", patch_path], "test-patch-applicability")
    run([sys.executable, "-B", "-m", "unittest", "discover", "-s", "tool/validation/terrain",
         "-p", "rvt_visual_oracle_test.py", "-v"], "oracle-tests")
    gpu_commands = {
        "ordinaryFinalPackageCapture": ["pwsh", "-File", str(MATRIX / "run-case.ps1"),
            "-CoveragePackage", "{finalCoveragePackage}", "-LocalPackage", "{finalLocalPackage}",
            "-PackageValidator", "{nativePackageValidator}", "-EvidenceRoot", "{newEvidenceRoot}",
            "-CaseId", "CoverageAtlas-w2-coverage-01", "-Execute"],
        "gpu384Prepare": [sys.executable, "-B", str(ROOT / "tool/validation/terrain/prepare_real_composited_numeric.py"),
            "--output", "{freshNumericDirectory}", "--coverage-manifest", str(options.coverage_manifest),
            "--local-manifest", "{actualCurrentLocalManifest}"],
        "gpu384RunAfterPrepare": [sys.executable, "-B", str(ROOT / "tool/validation/terrain/run_real_composited_numeric.py"),
            "--build-directory", "{freshNumericDirectory}", "--execute-gpu"],
        "sameEpochOracleAfterApprovedHook": [sys.executable, "-B", str(ROOT / "tool/validation/terrain/rvt_visual_oracle.py"),
            "--output", "{reportJson}", "images", "--candidate", "{actualCapturePacket}", "--reference", "{independentReferencePacket}"],
        "actualTesOracleAfterApprovedHook": [sys.executable, "-B", str(ROOT / "tool/validation/terrain/rvt_visual_oracle.py"),
            "--output", "{tesReportJson}", "tes", "--capture", "{actualTesBinary}", "--metadata", "{actualTesMetadata}",
            "--manifest", str(options.coverage_manifest)],
        "policy": "No automatic launch or matrix expansion. Select exactly one case when main grants a window. Existing normal capture command cannot produce missing visual/TES channels."
    }
    save(output / "run-commands.json", gpu_commands)
    save(output / "prepared.json", {"status": "cpu-oracle-and-shader-prepared-native-hook-not-ready",
        "gpuExecuted": False, "productionSourceModified": False, "overallP5Passed": False,
        "frozenSha256": frozen_hash, "limits": limits, "caseCount": len(cases), "posesPerModeWorker": 53,
        "project": str(project), "sourceInputs": source_inputs, "shaderSha256": digest(shader_path),
        "spirvSha256": digest(output / "actual-production-tes-instrumented.spv"),
        "publications": publications, "cpuOracleAnchors": anchors, "commands": commands,
        "remainingFacts": hook["notClaimed"]})
    print(json.dumps({"prepared": str(output), "gpuExecuted": False, "instrumentedTesCompiled": True,
                      "publications": len(publications), "nativeHookReady": False, "overallP5Passed": False}))


if __name__ == "__main__":
    main()
