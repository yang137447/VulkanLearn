import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


root = Path(__file__).resolve().parents[3]
output = root / "artifacts/terrain-rvt-production/r11-evidence-writer-private-r1"
receipt = json.loads((output / "receipt.json").read_text())
promotion = json.loads((root / "artifacts/terrain-rvt-production/r11-capture-promotion/receipt.json").read_text())
files = {key: digest(root / key) for key in promotion["files"]}
changes = {key: {"oldPromotionAfter": promotion["files"][key]["after"], "current": value}
           for key, value in files.items() if promotion["files"][key]["after"] != value}
for name in receipt["sourceManifest"]:
    path = root / name
    current = digest(path)
    if current != receipt["sourceManifest"][name]:
        raise RuntimeError(f"Fresh private receipt already stale: {name}")
    files[name] = current
files[str(Path(__file__).resolve().relative_to(root))] = digest(Path(__file__))
record = {
    "scope": "Fresh all-15 promotion-source plus full native-capture tool manifest; not a build receipt for current shared source",
    "files": files,
    "aggregateSha256": hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(),
    "changedFromOldPromotionReceipt": changes,
    "privateCompileReceiptSha256": digest(output / "receipt.json"),
    "requiredPromotion": ["Apply adapter.patch only after active native target build ends",
                          "Include terrainEvidenceWriter.h as a fresh source dependency",
                          "Freeze new source-before and perform normal native target incremental build; no object overlay"],
    "audit": "documents/rationale/terrain-rvt-quality-independent-audit.md section 13, line 1003: 17 CPU lifecycle cases are finite lifecycle evidence, not full capture acceptance",
    "gpuExecuted": False,
}
(output / "live-full-manifest.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
print(json.dumps({key: record[key] for key in ("aggregateSha256", "changedFromOldPromotionReceipt")}, indent=2))
