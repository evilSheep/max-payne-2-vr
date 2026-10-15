#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install hash-pinned Microsoft June 2010 x86 shader DLLs beside a title."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import struct
import subprocess
import sys
import tempfile


PAYLOADS = (
    {
        "cab": "Jun2010_d3dx9_43_x86.cab",
        "cab_sha256": "fcc6cf0966b4853d6fa3d32ab299cde5a9824feaecb0d4f34ea452fb9fd1c867",
        "dll": "D3DX9_43.dll",
        "dll_sha256": "0b28546be22c71834501f7d7185ede5d79742457331c7ee09efc14490dd64f5f",
    },
    {
        "cab": "Jun2010_D3DCompiler_43_x86.cab",
        "cab_sha256": "417eebd5b19f45c67c94c2d2ba8b774c0fc6d958b896d7b1ac12cf5a0ea06e0e",
        "dll": "D3DCompiler_43.dll",
        "dll_sha256": "2f23182ec6f4889397ac4bf03d62536136c5bdba825c7d2c4ef08c827f3a8a1c",
    },
)
MAX_DLL_SIZE = 32 * 1024 * 1024


class InstallError(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_plain_file(path: Path, label: str) -> None:
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError as exc:
        raise InstallError(f"{label} missing: {path.name}") from exc
    if not stat.S_ISREG(mode):
        raise InstallError(f"{label} must be a regular non-symlink file: {path.name}")


def validate_pe32_x86_dll(data: bytes, name: str) -> None:
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise InstallError(f"{name} is not a PE image")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset > len(data) - 24 or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise InstallError(f"{name} has an invalid PE header offset")
    machine, _, _, _, _, optional_size, characteristics = struct.unpack_from(
        "<HHIIIHH", data, pe_offset + 4
    )
    optional_offset = pe_offset + 24
    if optional_size < 2 or optional_offset > len(data) - optional_size:
        raise InstallError(f"{name} has a truncated optional header")
    magic = struct.unpack_from("<H", data, optional_offset)[0]
    if machine != 0x014C or magic != 0x010B or not (characteristics & 0x2000):
        raise InstallError(f"{name} is not a PE32 x86 DLL")


def extract_verified_payload(redist_dir: Path, spec: dict[str, str], bsdtar: str) -> bytes:
    cab_path = redist_dir / spec["cab"]
    require_plain_file(cab_path, "CAB")
    actual_cab_hash = sha256_file(cab_path)
    if actual_cab_hash != spec["cab_sha256"]:
        raise InstallError(f"CAB hash mismatch: {spec['cab']}")

    listing = subprocess.run(
        [bsdtar, "-tf", str(cab_path)], check=False, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
    )
    if listing.returncode:
        raise InstallError(f"CAB listing failed: {spec['cab']} (exit {listing.returncode})")
    matches = [
        item.rstrip("\r") for item in listing.stdout.splitlines()
        if PurePosixPath(item.rstrip("\\/").replace("\\", "/")).name.casefold()
        == spec["dll"].casefold()
    ]
    if len(matches) != 1:
        raise InstallError(f"expected one {spec['dll']} member in {spec['cab']}, found {len(matches)}")

    extracted = subprocess.run(
        [bsdtar, "-xOf", str(cab_path), matches[0]], check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if extracted.returncode:
        raise InstallError(f"CAB extraction failed: {spec['cab']} (exit {extracted.returncode})")
    payload = extracted.stdout
    if len(payload) > MAX_DLL_SIZE:
        raise InstallError(f"extracted DLL exceeds size limit: {spec['dll']}")
    if sha256(payload) != spec["dll_sha256"]:
        raise InstallError(f"DLL hash mismatch: {spec['dll']}")
    validate_pe32_x86_dll(payload, spec["dll"])
    return payload


def inspect_targets(game_dir: Path, payloads: list[tuple[dict[str, str], bytes]]) -> list[dict]:
    try:
        entries = list(os.scandir(game_dir))
    except OSError as exc:
        raise InstallError(f"cannot list game directory: {exc}") from exc
    plan = []
    for spec, data in payloads:
        matches = [entry for entry in entries if entry.name.casefold() == spec["dll"].casefold()]
        if len(matches) > 1:
            raise InstallError(f"multiple case-insensitive target matches: {spec['dll']}")
        if not matches:
            plan.append({"spec": spec, "data": data, "path": game_dir / spec["dll"], "action": "install"})
            continue
        match = matches[0]
        if match.is_symlink():
            raise InstallError(f"refusing symlink target: {match.name}")
        if not match.is_file(follow_symlinks=False):
            raise InstallError(f"target exists but is not a regular file: {match.name}")
        path = Path(match.path)
        if sha256_file(path) != spec["dll_sha256"]:
            raise InstallError(f"existing target hash mismatch: {match.name}")
        plan.append({"spec": spec, "data": data, "path": path, "action": "present_exact"})
    return plan


def write_atomically(plan: list[dict]) -> None:
    staged: list[tuple[dict, Path, tuple[int, int]]] = []
    linked: list[tuple[Path, tuple[int, int]]] = []
    try:
        for item in plan:
            if item["action"] == "present_exact":
                continue
            fd, temp_name = tempfile.mkstemp(prefix=f".{item['spec']['dll']}.tmp-", dir=item["path"].parent)
            temp_path = Path(temp_name)
            try:
                os.fchmod(fd, 0o644)
                with os.fdopen(fd, "wb") as stream:
                    stream.write(item["data"])
                    stream.flush()
                    os.fsync(stream.fileno())
            except Exception:
                try:
                    os.close(fd)
                except OSError:
                    pass
                temp_path.unlink(missing_ok=True)
                raise
            stat = temp_path.stat()
            staged.append((item, temp_path, (stat.st_dev, stat.st_ino)))

        for item, temp_path, identity in staged:
            try:
                os.link(temp_path, item["path"])
            except FileExistsError as exc:
                raise InstallError(f"target appeared during install: {item['path'].name}") from exc
            linked.append((item["path"], identity))
    except Exception:
        for path, identity in reversed(linked):
            try:
                current = path.stat(follow_symlinks=False)
                if (current.st_dev, current.st_ino) == identity:
                    path.unlink()
            except OSError:
                pass
        raise
    finally:
        for _, temp_path, _ in staged:
            temp_path.unlink(missing_ok=True)


def run(args: argparse.Namespace) -> dict:
    game_dir = Path(args.game_dir)
    redist_dir = Path(args.redist_dir)
    if game_dir.is_symlink() or not game_dir.is_dir():
        raise InstallError("--game-dir must be an existing non-symlink directory")
    if redist_dir.is_symlink() or not redist_dir.is_dir():
        raise InstallError("--redist-dir must be an existing non-symlink directory")
    bsdtar = shutil.which("bsdtar")
    if not bsdtar:
        raise InstallError("bsdtar is required")

    # Verify and retain both payloads before any destination is inspected or changed.
    payloads = [(spec, extract_verified_payload(redist_dir, spec, bsdtar)) for spec in PAYLOADS]
    plan = inspect_targets(game_dir, payloads)
    if not args.dry_run:
        write_atomically(plan)
    return {
        "ok": True,
        "mode": "dry-run" if args.dry_run else "install",
        "libraries": [
            {"file": item["spec"]["dll"], "sha256": item["spec"]["dll_sha256"],
             "action": "would_install" if args.dry_run and item["action"] == "install" else item["action"]}
            for item in plan
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", required=True, help="existing title directory")
    parser.add_argument("--redist-dir", required=True, help="directory containing the two June 2010 CABs")
    parser.add_argument("--dry-run", action="store_true", help="verify inputs and report actions without writing")
    args = parser.parse_args()
    try:
        result = run(args)
        code = 0
    except (InstallError, OSError, subprocess.SubprocessError) as exc:
        result = {"ok": False, "error": str(exc)}
        code = 1
    print(json.dumps(result, separators=(",", ":")))
    return code


if __name__ == "__main__":
    sys.exit(main())
