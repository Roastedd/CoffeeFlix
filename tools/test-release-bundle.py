#!/usr/bin/env python3
"""Release safety checks against local files and a fake GitHub; never publishes."""
import copy
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("bundle", Path(__file__).with_name("release-bundle.py"))
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class FakeGitHub:
    def __init__(self, directory):
        self.directory = directory
        self.published = False
        self.latest = "v2.4.0"
        self.commit = "a" * 40
        self.changed = False
        self.release = {"id": 42, "tag_name": "v2.4.1", "draft": True, "prerelease": False,
                        "assets": [{"id": i, "name": name, "size": (directory / name).stat().st_size,
                                    "digest": "sha256:" + bundle.digest(directory / name),
                                    "updated_at": "2026-01-01T00:00:00Z", "download_count": 0}
                                   for i, name in enumerate((*bundle.ASSETS, bundle.MANIFEST, bundle.CHECKSUMS))]}

    def api(self, endpoint, payload=None):
        if payload is not None:
            self.published = True
            return {**self.release, "draft": False}
        if endpoint == "releases/latest":
            return {"tag_name": self.latest}
        if endpoint.startswith("commits/"):
            return {"sha": self.commit}
        result = copy.deepcopy(self.release)
        if endpoint == "releases/42":
            result["assets"][0]["download_count"] += 1
            if self.changed:
                result["assets"][0]["id"] = 900
        return result

    def download(self, tag, directory):
        for path in self.directory.iterdir():
            shutil.copyfile(path, directory / path.name)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        for name in bundle.ASSETS:
            (self.directory / name).write_bytes(b"test candidate " + name.encode())
        bundle.create(self.directory, "2.4.1", "a" * 40)
        self.github = FakeGitHub(self.directory)
        self.approval = {"schema": 1, "candidate": {
            "tag": "v2.4.1", "manifest_sha256": bundle.digest(self.directory / bundle.MANIFEST),
            "tested_by": "Test fixture", "tested_at": "2026-01-01T00:00:00Z", "devices": "Test fixture",
            "notes": "Simulated testing only", "blockers": [], "checks": dict.fromkeys(bundle.CHECKS, True)}}

    def test_no_candidate_needs_no_network(self):
        self.assertIn("No approved", bundle.promote({"schema": 1, "candidate": None}, None, True))

    def test_dry_run_never_publishes(self):
        self.assertIn("dry run", bundle.promote(self.approval, self.github))
        self.assertFalse(self.github.published)

    def test_approved_exact_files_publish(self):
        self.assertIn("Published", bundle.promote(self.approval, self.github, True))
        self.assertTrue(self.github.published)

    def test_missing_device_check_blocks(self):
        self.approval["candidate"]["checks"]["install_tiramisu"] = False
        with self.assertRaises(ValueError):
            bundle.promote(self.approval, self.github, True)
        self.assertFalse(self.github.published)

    def test_blocker_blocks(self):
        self.approval["candidate"]["blockers"] = ["Playback freezes"]
        with self.assertRaises(ValueError):
            bundle.promote(self.approval, self.github, True)

    def test_changed_binary_blocks(self):
        (self.directory / bundle.ASSETS[0]).write_bytes(b"different build")
        with self.assertRaisesRegex(ValueError, "Asset changed"):
            bundle.promote(self.approval, self.github, True)
        self.assertFalse(self.github.published)

    def test_changed_manifest_blocks(self):
        with (self.directory / bundle.MANIFEST).open("a") as stream:
            stream.write("\n")
        with self.assertRaisesRegex(ValueError, "manifest differs"):
            bundle.promote(self.approval, self.github, True)

    def test_wrong_source_blocks(self):
        self.github.commit = "b" * 40
        with self.assertRaisesRegex(ValueError, "source tag"):
            bundle.promote(self.approval, self.github, True)

    def test_older_version_blocks(self):
        self.github.latest = "v2.5.0"
        with self.assertRaisesRegex(ValueError, "newer"):
            bundle.promote(self.approval, self.github, True)

    def test_changed_remote_asset_blocks(self):
        self.github.changed = True
        with self.assertRaisesRegex(ValueError, "changed during"):
            bundle.promote(self.approval, self.github, True)

    def test_repeated_publish_is_noop(self):
        self.github.release["draft"] = False
        self.assertIn("already published", bundle.promote(self.approval, self.github, True))
        self.assertFalse(self.github.published)

    def test_existing_candidate_cannot_be_overwritten(self):
        with self.assertRaisesRegex(ValueError, "already exists"):
            bundle.create(self.directory, "2.4.2", "b" * 40)

    def test_unexpected_asset_blocks(self):
        self.github.release["assets"].append({"name": "unreviewed.apk"})
        with self.assertRaisesRegex(ValueError, "exactly"):
            bundle.promote(self.approval, self.github, True)


if __name__ == "__main__":
    unittest.main()
