import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from compile_frozen_cpp import compile_frozen


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


root = Path(__file__).resolve().parents[3]
tools = root / "tool/validation/terrain/native-capture"
output = root / "artifacts/terrain-rvt-production/r11-evidence-writer-private-r1"
output.mkdir(exist_ok=False)
adapter = tools / "nativeTerrainCaptureAdapter.cpp"
original = adapter.read_text(encoding="utf-8")
patched = original.replace('#include "nativeTerrainCaptureAdapter.h"',
    '#include "nativeTerrainCaptureAdapter.h"\n#include "terrainEvidenceWriter.h"')
start = patched.index('        const auto output = std::filesystem::path', patched.index('void ConsumeCompleted'))
end = patched.index('        complete = true;', start)
block = patched[start:end]
block = block.replace('        std::ofstream binary(binaryPath, std::ios::binary);\n', '')
block = block.replace('        TerrainTesCapture::WriteReadback(*readback, binary, nativeMetadata);\n        binary.close();',
    '        WriteTerrainEvidenceFile(binaryPath, [&](std::ostream& binary) {\n'
    '            TerrainTesCapture::WriteReadback(*readback, binary, nativeMetadata);\n'
    '        });')
block = block.replace('        std::ofstream(metadataPath) << metadata.dump(2);',
    '        WriteTerrainEvidenceFile(metadataPath, [&](std::ostream& stream) {\n'
    '            stream << metadata.dump(2);\n'
    '        });')
patched = patched[:start] + '        try\n        {\n' + ''.join(
    '    '+line+'\n' for line in block.splitlines()) + (
    '        }\n        catch (const std::exception& error)\n        {\n'
    '            failure = std::string("Native TES evidence incomplete: ")+error.what();\n'
    '            complete = false;\n            throw std::runtime_error(failure);\n        }\n') + patched[end:]
patched = patched.replace('        if (!capture || !submittedEpoch || epoch < submittedEpoch) return;',
    '        if (!failure.empty()) throw std::runtime_error(failure);\n'
    '        if (!capture || !submittedEpoch || epoch < submittedEpoch) return;')
private = output / adapter.name
private.write_text(patched, encoding="utf-8")
(output / "adapter.patch").write_text("".join(difflib.unified_diff(
    original.splitlines(True), patched.splitlines(True),
    fromfile="a/tool/validation/terrain/native-capture/"+adapter.name,
    tofile="b/tool/validation/terrain/native-capture/"+adapter.name)), encoding="utf-8")
old = json.loads((root / "artifacts/terrain-rvt-production/r10-actual-draw-private-r3/adapter-final-identity-compile.json").read_text())
compiler, flags = old["compiler"], old["flags"]
closures = []
with (output / "build.log").open("w", encoding="utf-8") as log:
    test = tools / "terrainEvidenceWriterTests.cpp"
    closures.append(compile_frozen(compiler, ["-std=c++17", "-O1", "-static", "-static-libgcc",
        "-static-libstdc++", "-I"+str(tools)], test, output / "tests.o", root))
    result = subprocess.run([compiler, "-static", str(output / "tests.o"), "-o",
        str(output / "tests.exe")], stdout=log, stderr=subprocess.STDOUT)
    result.check_returncode()
    result = subprocess.run([str(output / "tests.exe"), str(output / "test-output")],
        stdout=log, stderr=subprocess.STDOUT)
    result.check_returncode()
    closures.append(compile_frozen(compiler, flags, private, output / "adapter.o", root))
files = sorted(path for path in tools.rglob("*") if path.is_file())
files += [private, output / "adapter.patch", Path(__file__).resolve()]
manifest = {str(path.relative_to(root)): digest(path) for path in files}
payload = json.dumps(manifest, sort_keys=True).encode()
receipt = {
    "scope": "Private patch only; active R11 adapter untouched. Native CPU IO regression and private adapter compile; no GPU/capture acceptance.",
    "tests": 6, "sourceManifest": manifest,
    "sourceManifestSha256": hashlib.sha256(payload).hexdigest(),
    "liveAdapterSha256": digest(adapter), "privateAdapterSha256": digest(private),
    "objects": [{key: item[key] for key in ("object", "objectSha256", "sourceClosureSha256")} for item in closures],
    "testExecutableSha256": digest(output / "tests.exe"),
    "frozenHookSha256": digest(root / "source/render/diagnostics/terrainCaptureHooks.h"),
    "lifecycleBoundary": "IO occurs only after TryConsume(completedEpoch); failure latches incomplete, retains capture owners and throws. No automatic retry/overwrite. Normal teardown remains fence/device-idle-owned.",
}
(output / "receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
print(json.dumps(receipt, indent=2))
