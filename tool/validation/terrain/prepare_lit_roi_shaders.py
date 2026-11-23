"""Compile test-only crop variants of the existing deferred-lighting and tone-map passes."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[3]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--project",type=Path,default=ROOT.parent/"p5s/C010")
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--x",type=int,default=576)
    parser.add_argument("--y",type=int,default=296)
    args=parser.parse_args()
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    project=args.project.resolve();glsl=project/"shader/glsl"
    inputs={}
    def expand(path):
        path=path.resolve();inputs[str(path)]=digest(path)
        text=path.read_text(encoding="utf-8")
        return re.sub(r'#include\s+"([^"]+)"',lambda m:expand(path.parent/m[1]),text)
    prefix=f"""
vec2 TerrainLitLocalUv(vec2 originalUv)
{{
    return (originalUv*vec2(1280.0,720.0)-vec2({args.x}.0,{args.y}.0))/vec2(128.0);
}}
"""
    lighting=expand(glsl/"pass/deferredLighting.frag")
    lighting=lighting.replace("gl_FragCoord",f"(gl_FragCoord+vec4({args.x}.0,{args.y}.0,0.0,0.0))")
    patches=[]
    for name in ("gbufferA","gbufferB","gbufferC","gbufferD","gbufferE","gbufferF","gbufferVelocity","sceneColorBase","sceneDepth"):
        pattern=rf"texture\({name},\s*(uv|inUV)\)"
        lighting,count=re.subn(pattern,lambda m:f"texture({name}, TerrainLitLocalUv({m[1]}))",lighting)
        if count!=1:raise ValueError("Lighting crop anchor changed: "+name)
    lighting=lighting.replace("#version 450","#version 450\n"+prefix,1)
    tone=expand(glsl/"pass/toneMapping.frag")
    tone=tone.replace("gl_FragCoord",f"(gl_FragCoord+vec4({args.x}.0,{args.y}.0,0.0,0.0))")
    tone=tone.replace("texture(sceneColor, inUV)","texture(sceneColor, TerrainLitLocalUv(inUV))")
    tone=tone.replace("texture(sceneColor, sceneUV)","texture(sceneColor, TerrainLitLocalUv(sceneUV))")
    tone=tone.replace("textureSize(sceneColor, 0)","ivec2(1280,720)")
    tone=tone.replace("#version 450","#version 450\n"+prefix,1)
    vertex=expand(glsl/"pass/deferredLighting.vert")
    commands=[]
    for name,fragment in (("deferredLighting",lighting),("toneMapping",tone)):
        vert=output/(name+".vert");frag=output/(name+".frag")
        vert.write_text(vertex,encoding="utf-8");frag.write_text(fragment,encoding="utf-8")
        for command in (["glslangValidator","-V","--target-env","vulkan1.4","-l",str(vert),str(frag),"-o",str(output/(name+".linked.spv"))],
                        ["spirv-val","--target-env","vulkan1.4",str(output/(name+".linked.spv"))]):
            result=subprocess.run(command,capture_output=True,text=True)
            commands.append({"argv":command,"exitCode":result.returncode,"log":result.stdout+result.stderr})
            result.check_returncode()
    frame_path=ROOT/"source/render/frame/renderFrameRecorder.cpp"
    original=frame_path.read_text(encoding="utf-8")
    anchor="            ctx.passRuntime.RecordPass(renderPassName, passContext);"
    if original.count(anchor)!=1:raise ValueError("Frame after-pass call site changed")
    modified='#include "render/diagnostics/terrainLitCaptureHooks.h"\n'+original.replace(
        anchor,anchor+"\n            VL::RecordOptionalTerrainLitAfterPass(passContext);",1)
    patch="".join(difflib.unified_diff(original.splitlines(True),modified.splitlines(True),
        fromfile="a/source/render/frame/renderFrameRecorder.cpp",tofile="b/source/render/frame/renderFrameRecorder.cpp"))
    (output/"optional-lit-afterpass-request.patch").write_text(patch,encoding="utf-8")
    header=ROOT/"source/render/diagnostics/terrainCaptureHooks.h"
    report={"gpuExecuted":False,"productionWrites":[],"frozenBaseHookSha256":digest(header),
        "baseHookChanged":False,"rect":[args.x,args.y,128,128],"fullViewport":[1280,720],
        "shaderSourceInputs":inputs,"commands":commands,
        "sourceOfLighting":"Existing actual deferredLighting and toneMapping only; no alternative material evaluator",
        "localInputs":["reference GBufferA/B/C/D/E/F/Velocity","sceneColorBase","sceneDepth","sceneColor"],
        "sharedActualInputs":["Shadow maps","DBuffer including emissive","actual global/light/IBL constants","current auto exposure","current bloom","distortion/selection"],
        "postprocessScope":"Reference with identical actual postprocess inputs. Independently recomputed bloom/fog/distortion/dynamics is not covered; these inputs must be retained and separately reported.",
        "newHookDelta":{"baseHeaderChanged":False,"newSourceFiles":["source/render/diagnostics/terrainLitCaptureHooks.h",
            "source/render/diagnostics/terrainLitCaptureHooks.cpp"],"frameCallback":"RecordOptionalTerrainLitAfterPass(passContext) immediately after actual pass completes; adapter filters lighting/toneMapping",
            "status":"private request only, not applied"}}
    (output/"prepared.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(json.dumps({"gpuExecuted":False,"cropShadersCompiledAndLinked":2,"baseHookChanged":False,"output":str(output)}))


if __name__=="__main__":
    main()
