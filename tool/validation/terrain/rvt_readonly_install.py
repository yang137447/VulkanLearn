"""Recoverable NTFS WRITE-deny evidence for an exact runner-exported Game package."""

from contextlib import contextmanager
import argparse
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys

from rvt_delivery import artifact, require


def export_owned_package(exporter, stage, package, evidence, scene):
    exporter = exporter.resolve(strict=True)
    stage = stage.resolve(strict=True)
    package = package.resolve()
    evidence = evidence.resolve()
    require(not package.exists() and not evidence.exists() and
            not package.is_relative_to(stage) and not stage.is_relative_to(package) and
            not evidence.is_relative_to(package) and not package.is_relative_to(evidence),
            "Export ownership needs fresh disjoint package and evidence paths")
    evidence.mkdir(parents=True)
    executable_identity = artifact(exporter)
    results = []
    for name, argv in (
            ("export", [str(exporter), "--project-root", str(stage),
                        "--binary-directory", str(stage / "bin"), "--flavor", "game",
                        "--initial-scene", scene, "--output", str(package)]),
            ("verify", [str(exporter), "--verify-package", str(package)])):
        process = subprocess.run(argv, cwd=stage, capture_output=True, timeout=900, check=False,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        path = evidence / (name + ".log")
        path.write_bytes(process.stdout + process.stderr)
        results.append({"argv": argv, "cwd": str(stage), "exitCode": process.returncode,
                        "log": artifact(path)})
        require(process.returncode == 0, f"Actual {name} failed; no owned-ready receipt issued")
    require(artifact(exporter) == executable_identity, "Exporter changed during owned package preparation")
    receipt = {"kind": "rvt-runner-owned-package", "status": "exported", "package": str(package),
               "exportExitCode": results[0]["exitCode"], "verificationExitCode": results[1]["exitCode"],
               "manifestSha256": artifact(package / "game-export-manifest.json")["sha256"],
               "exporter": executable_identity, "processes": results,
               "scope": "New package actually exported and installation-verified; no Game/quality pass"}
    path = evidence / "package-ownership.json"
    path.write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    return path


def validate_owned_package(package, ownership, evidence):
    package = package.resolve(strict=True)
    evidence = evidence.resolve(strict=True)
    receipt = json.loads(ownership.read_text(encoding="utf-8"))
    require(receipt.get("kind") == "rvt-runner-owned-package" and receipt.get("status") == "exported"
            and Path(receipt["package"]).resolve() == package
            and receipt["exportExitCode"] == receipt["verificationExitCode"] == 0,
            "Read-only ACL requires a new package's actual runner export/verify receipt")
    require(not evidence.is_relative_to(package) and not package.is_relative_to(evidence),
            "ACL backup/evidence must be outside the installation")
    require(not ownership.resolve(strict=True).is_relative_to(package),
            "Owned export receipt must remain outside the package")
    require(artifact(package / "game-export-manifest.json")["sha256"] == receipt["manifestSha256"],
            "Owned package export manifest changed")
    return package


@contextmanager
def readonly_package(package, ownership, evidence, project, resources):
    require(os.name == "nt", "Real read-only installation evidence requires Windows NTFS")
    package = validate_owned_package(package, ownership, evidence)
    evidence = evidence.resolve(strict=True)
    require(not (evidence / "package-acl-backup.json").exists(),
            "Use fresh ACL evidence; preserve previous backup without applying it")
    script = Path(__file__).with_name("readonly_game_acl.ps1")
    common = [shutil.which("pwsh") or "powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
              "-File", str(script), "-Package", str(package), "-AllowedParent", str(package.parent),
              "-Evidence", str(evidence), "-ProjectRoot", str(project.resolve(strict=True)),
              "-ResourceRoot", str(resources.resolve(strict=True))]
    (evidence / "acl-command.json").write_text(
        json.dumps({"argv": common, "script": artifact(script)}, indent=2), encoding="utf-8")
    protected = None
    try:
        result = subprocess.run(common + ["-Operation", "Protect"], capture_output=True, timeout=60,
                                check=False, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        (evidence / "acl-protect.stdout").write_bytes(result.stdout)
        (evidence / "acl-protect.stderr").write_bytes(result.stderr)
        require(result.returncode == 0, "NTFS protection/probe failed; restoring any saved original DACL")
        protected = json.loads(result.stdout.decode("utf-8-sig").strip())
        require(protected["createFileDenied"] is True and protected["existingFileWriteDenied"] is True,
                "Actual inherited WRITE denial was not established")
        yield protected
    finally:
        if (evidence / "package-acl-backup.json").is_file():
            restore = subprocess.run(common + ["-Operation", "Restore"], capture_output=True, timeout=60,
                                     check=False, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            (evidence / "acl-restore.stdout").write_bytes(restore.stdout)
            (evidence / "acl-restore.stderr").write_bytes(restore.stderr)
            require(restore.returncode == 0,
                    "ACL restore failed; use the saved exact package-acl-backup.json before further package work")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exporter", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--execute", action="store_true")
    options = parser.parse_args()
    if not options.execute:
        print(json.dumps({"status": "planned", "package": str(options.package.resolve()),
                          "rendererStarted": False, "exportStarted": False,
                          "scope": "Will export a new owned Game package; ACL and Game run are separate"},
                         indent=2))
        return 0
    print(export_owned_package(options.exporter, options.stage, options.package, options.evidence, options.scene))
    return 0


if __name__ == "__main__":
    sys.exit(main())
