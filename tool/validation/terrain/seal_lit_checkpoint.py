import hashlib
import json
from pathlib import Path

from compile_frozen_cpp import digest


root = Path(__file__).resolve().parents[3]
output = root / "artifacts/terrain-rvt-production/r11-lit-native-private-r2"
files = list((root / "tool/validation/terrain/native-capture").glob("*"))
files += [root / "tool/validation/terrain" / name for name in (
    "compare_native_lit_roi.py", "compare_native_lit_roi_test.py", "run_native_lit_roi.ps1",
    "prepare_lit_source_request.py", "build_lit_replay_private.py",
    "finish_lit_private_receipt.py", "seal_lit_checkpoint.py")]
manifest = {str(path.relative_to(root)): digest(path) for path in sorted(files) if path.is_file()}
drift = {}
for path in sorted(output.glob("*.compile-closure.json")):
    closure = json.loads(path.read_text())
    for name, expected in closure["filesBeforeAndAfterPreprocessing"].items():
        if digest(Path(name)) != expected:
            drift[name] = {"compiled": expected, "current": digest(Path(name))}
    if digest(Path(closure["object"])) != closure["objectSha256"]:
        raise RuntimeError("Compiled object changed")
if drift:
    raise RuntimeError("Compiled source/header closure drift: "+json.dumps(drift))
record = {"scope": "Fresh lit-only preparation checkpoint, not GPU acceptance",
          "sourceManifest": manifest,
          "sourceManifestSha256": hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest(),
          "compileReceiptSha256": digest(output / "final-receipt.json"),
          "compilationClosureDrift": drift, "gpuExecuted": False,
          "p2Boundary": "Writer header and active Adapter/ReferenceAtlas TU untouched in this continuation; lit changes have their own new manifest instead of reusing P2 full-manifest identity"}
(output / "checkpoint.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
print(json.dumps({"sourceManifestSha256": record["sourceManifestSha256"], "compileClosureDrift": drift}))
