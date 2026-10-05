#!/usr/bin/env python3
"""Record a Wii U candidate's exact files, verify them, and promote an approved draft."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

REPOSITORY = "Roastedd/CoffeeFlix"
ASSETS = ("coffeeflix.zip", "coffeeflix.wuhb", "coffeeflix-tiramisu.zip")
MANIFEST = "release-manifest.json"
CHECKSUMS = "SHA256SUMS.txt"
CHECKS = ("install_aroma", "install_tiramisu", "playback", "navigation",
          "settings_persistence", "update_install", "public_source_review", "preview_feedback")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", value):
        raise ValueError("Use a numeric version such as 2.4.1")
    return tuple(map(int, value.split(".")))


def file_record(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        raise ValueError(f"Missing, empty or linked asset: {path.name}")
    return {"name": path.name, "size": path.stat().st_size, "sha256": digest(path)}


def create(directory, release_version, commit):
    version(release_version)
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("A full source commit SHA is required")
    if any((directory / name).exists() for name in (MANIFEST, CHECKSUMS)):
        raise ValueError("Candidate metadata already exists; use a new directory for a new candidate")
    manifest = {"schema": 1, "platform": "wiiu", "version": release_version,
                "source_commit": commit,
                "created_at": datetime.now(timezone.utc).isoformat(),
                "assets": [file_record(directory / name) for name in ASSETS]}
    (directory / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n")
    (directory / CHECKSUMS).write_text(checksum_text(manifest))
    return manifest


def checksum_text(manifest):
    return "".join(f"{item['sha256']}  {item['name']}\n" for item in manifest["assets"])


def verify(directory):
    manifest = json.loads((directory / MANIFEST).read_text())
    if manifest.get("schema") != 1 or manifest.get("platform") != "wiiu":
        raise ValueError("Unsupported release manifest")
    version(manifest["version"])
    if not re.fullmatch(r"[0-9a-f]{40}", manifest["source_commit"]):
        raise ValueError("Invalid source commit")
    if [item["name"] for item in manifest["assets"]] != list(ASSETS):
        raise ValueError("Unexpected or missing release assets")
    for expected in manifest["assets"]:
        if file_record(directory / expected["name"]) != expected:
            raise ValueError(f"Asset changed after candidate creation: {expected['name']}")
    if (directory / CHECKSUMS).read_text() != checksum_text(manifest):
        raise ValueError("Checksums do not match the manifest")
    return manifest


def validate_approval(data):
    if data.get("schema") != 1 or "candidate" not in data:
        raise ValueError("Unsupported approval document")
    candidate = data["candidate"]
    if candidate is None:
        return None
    if not isinstance(candidate, dict) or not isinstance(candidate.get("tag"), str) or not candidate["tag"].startswith("v"):
        raise ValueError("Approval must identify a v-prefixed release tag")
    version(candidate["tag"][1:])
    if not re.fullmatch(r"[0-9a-f]{64}", candidate.get("manifest_sha256", "")):
        raise ValueError("Approval must pin the manifest's SHA-256")
    for key in ("tested_by", "devices", "notes"):
        if not isinstance(candidate.get(key), str) or not candidate[key].strip():
            raise ValueError(f"Approval requires {key}")
    tested_at = datetime.fromisoformat(candidate["tested_at"].replace("Z", "+00:00"))
    if tested_at.tzinfo is None or tested_at > datetime.now(timezone.utc):
        raise ValueError("Testing timestamp must include a timezone and not be in the future")
    if candidate.get("blockers") != []:
        raise ValueError("Release blockers must be an empty list")
    if any(candidate.get("checks", {}).get(check) is not True for check in CHECKS):
        raise ValueError("Every required device/release check must pass")
    return candidate


class GitHub:
    def api(self, endpoint, payload=None):
        command = ["gh", "api", f"repos/{REPOSITORY}/{endpoint}"]
        if payload is not None:
            command += ["--method", "PATCH", "--input", "-"]
        result = subprocess.run(command, input=json.dumps(payload) if payload is not None else None,
                                text=True, capture_output=True, check=True)
        return json.loads(result.stdout)

    def download(self, tag, directory):
        subprocess.run(["gh", "release", "download", tag, "--repo", REPOSITORY,
                        "--dir", str(directory)], check=True)


def release_identity(release):
    # Downloads change download_count. Only compare fields that define the release.
    return {"id": release["id"], "tag": release["tag_name"], "draft": release["draft"],
            "prerelease": release["prerelease"], "body": release.get("body"),
            "assets": sorted((a["id"], a["name"], a["size"], a.get("digest"), a["updated_at"])
                             for a in release["assets"])}


def promote(approval, github, publish=False):
    candidate = validate_approval(approval)
    if candidate is None:
        return "No approved candidate; no release this run."
    tag = candidate["tag"]
    release = github.api(f"releases/tags/{tag}")
    if release["tag_name"] != tag or release["prerelease"]:
        raise ValueError("Expected the approved stable release tag")
    if not release["draft"]:
        return f"{tag} is already published; nothing changed."
    latest = github.api("releases/latest")
    if not latest["tag_name"].startswith("v") or version(tag[1:]) <= version(latest["tag_name"][1:]):
        raise ValueError("Candidate must be newer than the current stable release")
    names = [asset["name"] for asset in release["assets"]]
    if sorted(names) != sorted((*ASSETS, MANIFEST, CHECKSUMS)):
        raise ValueError("Draft must contain exactly the reviewed release files")
    with tempfile.TemporaryDirectory(prefix="coffeeflix-candidate-") as temp:
        directory = Path(temp)
        github.download(tag, directory)
        if digest(directory / MANIFEST) != candidate["manifest_sha256"]:
            raise ValueError("Draft manifest differs from the approved candidate")
        manifest = verify(directory)
        if manifest["version"] != tag[1:]:
            raise ValueError("Manifest version does not match the release tag")
        commit = github.api(f"commits/{tag}")["sha"]
        if commit != manifest["source_commit"]:
            raise ValueError("Public source tag does not match the tested candidate")
        # A concurrent edit invalidates the approval; do not silently publish it.
        if release_identity(github.api(f"releases/{release['id']}")) != release_identity(release):
            raise ValueError("Draft changed during verification; retry after reviewing it")
        if publish:
            result = github.api(f"releases/{release['id']}", {"draft": False, "make_latest": "true"})
            if result.get("draft") is not False:
                raise ValueError("GitHub did not confirm publication")
            return f"Published {tag} using the approved files."
    return f"{tag} verified; dry run only."


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("create", "verify"):
        sub = commands.add_parser(name)
        sub.add_argument("--directory", type=Path, default=Path("."))
        if name == "create":
            sub.add_argument("--version", required=True)
            sub.add_argument("--commit", required=True)
    sub = commands.add_parser("promote")
    sub.add_argument("--approval", type=Path, default=Path("release/approved.json"))
    sub.add_argument("--publish", action="store_true", help="Publish only after every approval and integrity check passes")
    sub.add_argument("--scheduled", action="store_true", help="Require automatic publication to be explicitly enabled")
    args = parser.parse_args()
    if args.command == "create":
        create(args.directory, args.version, args.commit)
        print("Candidate manifest and checksums created; hardware testing is still required.")
    elif args.command == "verify":
        manifest = verify(args.directory)
        print(f"Verified {manifest['version']} against its manifest; this does not certify device testing.")
    else:
        approval = json.loads(args.approval.read_text())
        if args.scheduled and approval.get("automatic") is not True:
            print("Automatic publication is disabled; use a manual dry run first.")
            return
        print(promote(approval, GitHub(), args.publish))


if __name__ == "__main__":
    main()
