"""Create and native-compile R10 private frontend/actual draw wiring, never edit shared sources."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
from compile_frozen_cpp import compile_frozen

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent/"native-capture"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("Private source anchor not unique: "+old[:100])
    return text.replace(old, new, 1)


DRAW_BINDINGS = """VL::TerrainMeshDrawBindings RenderableObject::DescribeDrawBindings(
    uint32_t frameSlot, bool usePatchIndices) const
{
    const uint32_t slot = frameSlot % DynamicMeshFrameSlotCount;
    VL::TerrainMeshDrawBindings result;
    const auto& vertexInfo = dynamicVertices ? dynamicVertexBufferInfos[slot] : vertexBufferInfo;
    result.vertexBuffers = {dynamicVertices ? dynamicVertexBuffers[slot] : vertexBuffer, instanceBuffer};
    result.vertexOffsets = {vertexInfo.offset, 0};
    result.patches = usePatchIndices && patchIndexBuffer != vk::Buffer{} && patchIndexCount != 0;
    if (usePatchIndices && !result.patches)
        return result;
    result.indexBuffer = result.patches ? patchIndexBuffer :
        (fixedCapacity ? dynamicIndexBuffers[slot] : indexBuffer);
    result.indexCount = result.patches ? patchIndexCount :
        static_cast<uint32_t>(fixedCapacity ? dynamicIndexCounts[slot] : GetIndices().size());
    result.instanceCount = instanceCount;
    return result;
}

void RenderableObject::Draw(
    vk::CommandBuffer& commandBuffer, uint32_t frameSlot, bool usePatchIndices)
{
    const auto bindings = DescribeDrawBindings(frameSlot, usePatchIndices);
    if (!bindings.indexCount) return;
    commandBuffer.bindVertexBuffers(0, 2, bindings.vertexBuffers.data(), bindings.vertexOffsets.data());
    commandBuffer.bindIndexBuffer(bindings.indexBuffer, bindings.indexOffset, bindings.indexType);
    commandBuffer.drawIndexed(bindings.indexCount, bindings.instanceCount, bindings.firstIndex,
        bindings.vertexOffset, bindings.firstInstance);
}"""

DRAW_HOOK = """                const auto captureHooks = domain == SurfaceDrawDomain::Geometry && drawPacket.isTerrain
                    ? AcquireTerrainCaptureHooks() : std::shared_ptr<ITerrainCaptureHooks>{};
                std::shared_ptr<PipelineBase> diagnosticPipeline;
                TerrainCaptureDrawInputs captureInputs;
                if (captureHooks)
                {
                    captureInputs = BuildTerrainCaptureDrawInputs(drawPacket,
                        static_cast<uint32_t>(draw.drawPacketIndex),
                        context.swapChainImageIndex % RenderableObject::DynamicMeshFrameSlotCount,
                        context.swapChainImageIndex, renderPipeline, renderPass,
                        drawResources.renderableObject, draw.objectResourceEntry,
                        materialGroup.material, materialInstanceGroup.materialInstance);
                    diagnosticPipeline = captureHooks->SelectActualDraw(captureInputs);
                }
                if (diagnosticPipeline)
                {
                    if (diagnosticPipeline->UsesTessellation() != renderPipelineUsesTessellation)
                        throw std::runtime_error("Diagnostic pipeline changed actual draw topology");
                    commandBuffer.bindPipeline(diagnosticPipeline->GetBindPoint(), diagnosticPipeline->GetPipeline());
                    for (uint32_t set = 0; set < 4; ++set)
                        if (captureInputs.descriptorSetMask & (1u << set))
                            commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                diagnosticPipeline->GetPipelineLayout(), set, captureInputs.descriptorSets[set], nullptr);
                    captureHooks->BindActualDraw(commandBuffer, captureInputs, *diagnosticPipeline);
                }
                drawResources.renderableObject->Draw(
                    commandBuffer,
                    context.swapChainImageIndex %
                        RenderableObject::DynamicMeshFrameSlotCount,
                    renderPipelineUsesTessellation);
                if (diagnosticPipeline)
                {
                    captureHooks->ActualDrawRecorded(captureInputs);
                    commandBuffer.bindPipeline(renderPipeline.GetBindPoint(), renderPipeline.GetPipeline());
                    for (uint32_t set = 0; set < 4; ++set)
                        if (captureInputs.descriptorSetMask & (1u << set))
                            commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                renderPipeline.GetPipelineLayout(), set, captureInputs.descriptorSets[set], nullptr);
                }"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prior-private", type=Path, default=ROOT/"artifacts/terrain-rvt-production/native-tes-capture-private-r2")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    mirror = output/"private-source"
    shutil.copytree(ROOT/"source", mirror/"source")
    names = ["source/vulkanManager.cpp", "source/pipeline/graphicsPipeline.cpp",
             "source/render/frontend/renderScene.h", "source/render/frontend/rendererFrontend.cpp",
             "source/renderableObject.h", "source/renderableObject.cpp",
             "source/render/backend/rendererDrawExecutor.cpp"]
    original = {name: (mirror/name).read_text(encoding="utf-8") for name in names}
    before = {name: digest(mirror/name) for name in names}
    for name in names[:2]:
        text = (args.prior_private/"private-source"/name).read_text(encoding="utf-8")
        text = replace_once(text, '#include "terrainTesDiagnostic.h"', '#include "render/diagnostics/terrainCaptureHooks.h"')
        text = text.replace("VL::TerrainTesDiagnostic::Requested()",
            "(VL::AcquireTerrainCaptureHooks() && VL::AcquireTerrainCaptureHooks()->RequiresTesVertexStores())")
        text = text.replace("VL::TerrainTesDiagnostic::DeviceFeatureEnabled(", "VL::PublishTerrainCaptureDeviceFeature(")
        text = text.replace("VL::TerrainTesDiagnostic::DeviceFeatureEnabled()", "VL::TerrainCaptureDeviceFeatureEnabled()")
        text = text.replace("VL::PublishTerrainCaptureDeviceFeature()", "VL::TerrainCaptureDeviceFeatureEnabled()")
        (mirror/name).write_text(text, encoding="utf-8")
    path = mirror/names[2]
    text = replace_once(original[names[2]], "    bool isTerrain = false;",
                        "    bool isTerrain = false;\n    std::string lodGroupId;\n    uint32_t lodLevel = 0;")
    path.write_text(text, encoding="utf-8")
    path = mirror/names[3]
    text = replace_once(original[names[3]], "    packet.isTerrain = drawSnapshot.isTerrain;",
                        "    packet.isTerrain = drawSnapshot.isTerrain;\n    packet.lodGroupId = drawSnapshot.lodGroupId;\n    packet.lodLevel = drawSnapshot.lodLevel;")
    path.write_text(text, encoding="utf-8")
    path = mirror/names[4]
    text = replace_once(original[names[4]], "#pragma once", '#pragma once\n#include "render/diagnostics/terrainCaptureHooks.h"')
    anchor = "    bool HasPatchIndices() const { return patchIndexCount != 0; }"
    text = replace_once(text, anchor, anchor+"""
    VL::TerrainMeshDrawBindings DescribeDrawBindings(uint32_t frameSlot, bool usePatchIndices) const;""")
    path.write_text(text, encoding="utf-8")
    path = mirror/names[5]
    text = original[names[5]]
    start = text.index("void RenderableObject::Draw(\n")
    end = text.index("\n}", start)+2
    text = text[:start]+DRAW_BINDINGS+text[end:]
    path.write_text(text, encoding="utf-8")
    path = mirror/names[6]
    text = '#include "render/diagnostics/terrainCaptureHooks.h"\n'+original[names[6]]
    start = text.index("void RendererDrawExecutor::DrawSurfaceScene(")
    head, tail = text[:start], text[start:]
    anchor = """                drawResources.renderableObject->Draw(
                    commandBuffer,
                    context.swapChainImageIndex %
                        RenderableObject::DynamicMeshFrameSlotCount,
                    renderPipelineUsesTessellation);"""
    tail = replace_once(tail, anchor, DRAW_HOOK)
    path.write_text(head+tail, encoding="utf-8")
    hookdir = mirror/"source/render/diagnostics"
    hookdir.mkdir(parents=True, exist_ok=True)
    for name in ("terrainCaptureHooks.h", "terrainCaptureHooks.cpp"):
        shutil.copy2(HERE/(name+".in"), hookdir/name)
    patches = []
    for name in names:
        text = (mirror/name).read_text(encoding="utf-8")
        patches.extend(difflib.unified_diff(original[name].splitlines(True), text.splitlines(True),
            fromfile="a/"+name, tofile="b/"+name))
    for path in sorted(hookdir.glob("*")):
        relative = path.relative_to(mirror).as_posix()
        patches.extend(difflib.unified_diff([], path.read_text(encoding="utf-8").splitlines(True),
            fromfile="/dev/null", tofile="b/"+relative))
    patch_path = output/"r10-actual-draw-wiring.patch"
    patch_path.write_text("".join(patches), encoding="utf-8")
    command = ["git", "apply", "--check", str(patch_path)]
    checked = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    (output/"patch-check.log").write_text(checked.stdout+checked.stderr, encoding="utf-8")
    checked.check_returncode()
    sources = [mirror/name for name in names if name.endswith(".cpp")]
    sources += [hookdir/"terrainCaptureHooks.cpp",
                HERE/"nativeTerrainCaptureAdapter.cpp", HERE/"nativeTerrainGame.cpp",
                HERE/"terrainTesCapture.cpp", HERE/"terrainTesCapturePolicy.cpp", HERE/"terrainTesCapturePipeline.cpp"]
    includes = ROOT/"build-vt-release/source/CMakeFiles/vl_runtime.dir/includes_CXX.rsp"
    flags = ["-std=gnu++17", "-O1", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
             "-ffunction-sections", "-fdata-sections", "-ffp-contract=off",
             "-I"+str(HERE), "-I"+str(mirror/"source"), "-I"+str(mirror/"source/pipeline"), "@"+str(includes)]
    compiler = shutil.which("g++")
    def build(item):
        index, source = item
        return str(source), compile_frozen(compiler, flags, source, output/("unit-"+str(index)+".obj"), ROOT)
    records = {}
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(build, item) for item in enumerate(sources)]
        failures = []
        for future in futures:
            try:
                name, record = future.result()
                records[name] = record
            except Exception as error:
                failures.append(str(error))
    if failures:
        (output/"compile-failures.json").write_text(json.dumps(failures, indent=2), encoding="utf-8")
        raise RuntimeError("Private native compile failed; retained logs")
    report = {"status": "r10-private-actual-draw-wiring-native-compiled", "gpuExecuted": False,
        "productionFilesWritten": [], "frameRecorderWritten": False,
        "requiresR10OrdinaryBuild": True, "patch": str(patch_path), "patchSha256": digest(patch_path),
        "sourceBaseline": before, "sharedSourceUnchanged": before == {name: digest(ROOT/name) for name in names},
        "sourceOwnedInterface": str(hookdir/"terrainCaptureHooks.h"),
        "sourceOwnedInterfaceSha256": digest(hookdir/"terrainCaptureHooks.h"),
        "compilationClosures": records, "sourceCount": len(records),
        "frameOwnerApi": ["BeforeGeometry(PassRuntimeContext&,TerrainCaptureFrameInfo)",
            "AfterGeometry(command)", "Submitted(actualEpoch)->retirementToken", "AbortUnsubmitted()",
            "ConsumeCompleted(actualCompletedEpoch)", "CloseAfterDeviceIdle()"],
        "remaining": ["Dirac private FrameRecorder lifecycle hookup", "coherent full R10 integration link/build",
                      "actual GPU TES capture", "lit reference-atlas same-input replay"]}
    (output/"build.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key:value for key,value in report.items() if key not in ("compilationClosures", "sourceBaseline")}, indent=2))


if __name__ == "__main__":
    main()
