import json
from pathlib import Path
import subprocess
import sys

from compile_frozen_cpp import compile_frozen, digest


root = Path(__file__).resolve().parents[3]
output = root / "artifacts/terrain-rvt-production/r11-lit-native-private-r2"
build = json.loads((output / "build.json").read_text())
first = build["compilationClosures"][0]
source = root / "tool/validation/terrain/native-capture/nativeTerrainLitGame.cpp"
record = compile_frozen(first["compiler"], first["flags"], source, output / "nativeTerrainLitGame.o", root)
tests = subprocess.run([sys.executable, "-B", str(root / "tool/validation/terrain/compare_native_lit_roi_test.py"), "-v"],
                       capture_output=True, text=True)
(output / "reader-tests.log").write_text(tests.stdout+tests.stderr, encoding="utf-8")
tests.check_returncode()
request = root / "artifacts/terrain-rvt-production/r11-lit-integration-request-r2"
files = sorted(path for path in (root / "tool/validation/terrain/native-capture").iterdir() if path.is_file())
files += [root / "tool/validation/terrain" / name for name in (
    "build_lit_replay_private.py", "compare_native_lit_roi.py", "compare_native_lit_roi_test.py",
    "prepare_lit_source_request.py", "finish_lit_private_receipt.py")]
manifest = {str(path.relative_to(root)): digest(path) for path in files}
report = {"gpuExecuted": False, "privateComponentTUsCompiled": 5, "thinEntryTUCompiled": True,
          "thinEntryCompile": {key: record[key] for key in ("objectSha256", "sourceClosureSha256")},
          "readerCpuCasesPassed": 3, "componentSha256": build["componentSha256"],
          "sourceManifest": manifest, "integrationPatchSha256": digest(request / "lit-scheduling-request.patch"),
          "baseHookSha256": digest(root / "source/render/diagnostics/terrainCaptureHooks.h"),
          "unappliedSourceChanges": json.loads((request / "request.json").read_text())["baseSourceHashes"],
          "notWholeExecutableLink": True,
          "limits": ["No GPU authorization consumed", "No Frame/Service/CMake shared edits",
                     "Exact 128-square actual same-command replay with shared actual postprocess inputs",
                     "Need actual page footprint/original GPU channel outputs and actual-output calibration before visual acceptance"],
          "compileBoundary": "Five lit helper TUs against freshly hashed live-header closures plus separately compiled thin entry. No claim that current shared Frame/Service were rebuilt with the unapplied request."}
(output / "final-receipt.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(json.dumps({key: report[key] for key in (
    "privateComponentTUsCompiled", "thinEntryTUCompiled", "readerCpuCasesPassed",
    "componentSha256", "integrationPatchSha256", "baseHookSha256")}))
