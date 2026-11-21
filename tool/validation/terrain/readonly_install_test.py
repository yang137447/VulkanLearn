"""Mocked ACL lifecycle tests only; not NTFS or Game execution evidence."""

import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from rvt_delivery import artifact
from rvt_readonly_install import readonly_package, validate_owned_package


class ReadonlyInstallTests(unittest.TestCase):
    def fixture(self, root):
        package, evidence, project, resources = [root / name for name in ("game", "evidence", "project", "resources")]
        for path in (package, evidence, project, resources):
            path.mkdir()
        manifest = package / "game-export-manifest.json"
        manifest.write_text("{}")
        receipt = root / "ownership.json"
        receipt.write_text(json.dumps({
            "kind": "rvt-runner-owned-package", "status": "exported", "package": str(package.resolve()),
            "exportExitCode": 0, "verificationExitCode": 0, "manifestSha256": artifact(manifest)["sha256"]}))
        return package, receipt, evidence, project, resources

    def test_changed_export_manifest_rejected_before_acl(self):
        with tempfile.TemporaryDirectory() as temporary:
            package, receipt, evidence, _, _ = self.fixture(Path(temporary))
            (package / "game-export-manifest.json").write_text('{"changed":true}')
            with self.assertRaisesRegex(ValueError, "manifest changed"):
                validate_owned_package(package, receipt, evidence)

    @unittest.skipUnless(os.name == "nt", "Windows-only ACL method lifecycle")
    def test_game_exception_still_invokes_acl_restore(self):
        with tempfile.TemporaryDirectory() as temporary:
            package, receipt, evidence, project, resources = self.fixture(Path(temporary))
            operations = []

            def fake_run(argv, **kwargs):
                operation = argv[-1]
                operations.append(operation)
                if operation == "Protect":
                    (evidence / "package-acl-backup.json").write_text('{"sddl":"mock"}')
                    data = {"createFileDenied": True, "existingFileWriteDenied": True}
                else:
                    data = {"status": "restored", "exactSddlRestored": True}
                return SimpleNamespace(returncode=0, stdout=json.dumps(data).encode(), stderr=b"")

            with patch("rvt_readonly_install.subprocess.run", side_effect=fake_run):
                with self.assertRaisesRegex(RuntimeError, "fake game failure"):
                    with readonly_package(package, receipt, evidence, project, resources):
                        raise RuntimeError("fake game failure")
            self.assertEqual(operations, ["Protect", "Restore"])

    @unittest.skipUnless(os.name == "nt", "Windows-only ACL method lifecycle")
    def test_failed_protect_with_backup_also_restores(self):
        with tempfile.TemporaryDirectory() as temporary:
            package, receipt, evidence, project, resources = self.fixture(Path(temporary))
            operations = []

            def fake_run(argv, **kwargs):
                operation = argv[-1]
                operations.append(operation)
                if operation == "Protect":
                    (evidence / "package-acl-backup.json").write_text('{"sddl":"mock"}')
                    return SimpleNamespace(returncode=1, stdout=b"", stderr=b"mock probe failure")
                return SimpleNamespace(returncode=0, stdout=b'{"status":"restored"}', stderr=b"")

            with patch("rvt_readonly_install.subprocess.run", side_effect=fake_run):
                with self.assertRaisesRegex(ValueError, "protection/probe"):
                    with readonly_package(package, receipt, evidence, project, resources):
                        self.fail("Failed protection must not launch Game")
            self.assertEqual(operations, ["Protect", "Restore"])


if __name__ == "__main__":
    unittest.main()
