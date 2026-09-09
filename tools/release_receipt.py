"""Bind a successful local native build to its exact inputs and runtime files.

XA DevHub Build Receipt v1.00
Created by: XA DevHub contributors
Last Updated: 2026-09-08 16:30:00
Release notes: record successful builds for separate, non-building packaging.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

RUNTIME_FILES = (
    "devhub.exe", "dpp.dll", "libcrypto-3-x64.dll", "libssl-3-x64.dll",
    "opus.dll", "sqlite3.dll", "SQLiteCpp.dll", "z.dll",
)
RECEIPT_NAME = "devhub-build-receipt.json"


def regular_path(path: Path) -> Path:
    """Reject links and Windows junctions along a read/write path."""
    for item in (path, *path.parents):
        if item.is_symlink() or (item.exists() and getattr(item.lstat(), "st_file_attributes", 0) & 0x400):
            raise RuntimeError(f"Linked/reparse path is not allowed: {item}")
    return path


def sha256_file(path: Path) -> str:
    regular_path(path)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_inputs(project: Path) -> dict[str, str]:
    """Hash native inputs and build verification sources, excluding local backups."""
    paths = [project / name for name in (
        "1. build.py", "CMakeLists.txt", "vcpkg.json", "vcpkg-configuration.json",
        "tools/release_receipt.py",
    )]
    for directory in ("src", "include", "vcpkg-overlays", "tests"):
        root = project / directory
        regular_path(root)
        if not root.exists():
            continue
        for path in root.rglob("*"):
            relative = path.relative_to(root)
            if any(p in {"backups", "__pycache__", "current functions", "function history", "errors"} for p in relative.parts):
                continue
            if path.suffix.lower() in {".cpp", ".h", ".hpp", ".rc", ".ico", ".cmake", ".json", ".py", ".ps1"} and path.is_file():
                paths.append(path)
    return {p.relative_to(project).as_posix(): sha256_file(p) for p in sorted(set(paths))}


def write_receipt(project: Path, output: Path, version: str, inputs: dict[str, str]) -> None:
    """Called only after all required build, test, migration and restart gates pass."""
    if build_inputs(project) != inputs:
        raise RuntimeError("Build inputs changed during the build; rerun step 1.")
    payload = {
        "schema": "xa-devhub.build/v1", "version": version, "verified": True,
        "inputs": inputs,
        "runtime": {name: sha256_file(output / name) for name in RUNTIME_FILES},
    }
    path = regular_path(output / RECEIPT_NAME)
    temporary = regular_path(path.with_suffix(".tmp"))
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def verify_receipt(project: Path, output: Path, version: str) -> dict:
    path = regular_path(output / RECEIPT_NAME)
    if not path.is_file():
        raise RuntimeError('Missing verified build receipt. Run python "1. build.py" first.')
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema") != "xa-devhub.build/v1" or payload.get("version") != version or payload.get("verified") is not True:
        raise RuntimeError("Build receipt is incompatible or has the wrong version; rerun step 1.")
    if payload.get("inputs") != build_inputs(project):
        raise RuntimeError("Source or build checks changed since the verified build; rerun step 1.")
    if payload.get("runtime") != {name: sha256_file(output / name) for name in RUNTIME_FILES}:
        raise RuntimeError("Runtime files changed since the verified build; rerun step 1.")
    return payload
