#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Precisely retarget an ASI's KERNEL32 import descriptor to mp2vrcmp.dll."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

SOURCE_DLL = b"KERNEL32.dll"  # exactly 12 bytes; replacement must preserve descriptor layout
TARGET_DLL = b"mp2vrcmp.dll"  # exactly 12 bytes
IMAGE_DIRECTORY_ENTRY_IMPORT = 1
IMAGE_ORDINAL_FLAG32 = 0x80000000


class PEError(ValueError):
    pass


def _u16(data: bytes, off: int) -> int:
    if off < 0 or off + 2 > len(data):
        raise PEError("truncated 16-bit PE field")
    return struct.unpack_from("<H", data, off)[0]


def _u32(data: bytes, off: int) -> int:
    if off < 0 or off + 4 > len(data):
        raise PEError("truncated 32-bit PE field")
    return struct.unpack_from("<I", data, off)[0]


def _cstring(data: bytes, off: int, *, limit: int = 4096) -> bytes:
    if off < 0 or off >= len(data):
        raise PEError("string RVA is outside file")
    end_limit = min(len(data), off + limit)
    end = data.find(b"\0", off, end_limit)
    if end < 0:
        raise PEError("unterminated or oversized PE string")
    return data[off:end]


def _parse_sections(data: bytes, nt: int) -> tuple[int, list[tuple[int, int, int, int]]]:
    section_count = _u16(data, nt + 6)
    opt_size = _u16(data, nt + 20)
    opt = nt + 24
    if _u16(data, opt) != 0x10B:
        raise PEError("input must be a PE32 image")
    if opt_size < 96 + 8 * (IMAGE_DIRECTORY_ENTRY_IMPORT + 1):
        raise PEError("PE32 optional header has no import directory")
    magic = _u16(data, opt)
    size_headers = _u32(data, opt + 60)
    import_rva = _u32(data, opt + 96 + IMAGE_DIRECTORY_ENTRY_IMPORT * 8)
    import_size = _u32(data, opt + 100 + IMAGE_DIRECTORY_ENTRY_IMPORT * 8)
    table = opt + opt_size
    sections = []
    for idx in range(section_count):
        off = table + idx * 40
        if off + 40 > len(data):
            raise PEError("truncated PE section table")
        virtual_size = _u32(data, off + 8)
        virtual_address = _u32(data, off + 12)
        raw_size = _u32(data, off + 16)
        raw_pointer = _u32(data, off + 20)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer, raw_size))
    if size_headers > len(data):
        raise PEError("PE headers extend past file")
    return (import_rva << 32) | import_size, sections


def _rva_to_offset(data: bytes, rva: int, sections: list[tuple[int, int, int, int]], size_headers: int) -> int:
    if rva < size_headers:
        if rva >= len(data):
            raise PEError("header RVA extends past file")
        return rva
    for va, extent, raw, raw_size in sections:
        if va <= rva < va + extent:
            delta = rva - va
            if delta >= raw_size or raw + delta >= len(data):
                raise PEError("RVA refers to an unbacked section tail")
            return raw + delta
    raise PEError(f"RVA 0x{rva:x} is not mapped by a section")


def imports(data: bytes) -> tuple[list[dict[str, object]], int]:
    if len(data) < 64 or data[:2] != b"MZ":
        raise PEError("missing DOS header")
    nt = _u32(data, 0x3C)
    if nt + 24 > len(data) or data[nt:nt + 4] != b"PE\0\0":
        raise PEError("invalid PE signature")
    dir_pack, sections = _parse_sections(data, nt)
    import_rva, import_size = dir_pack >> 32, dir_pack & 0xFFFFFFFF
    size_headers = _u32(data, nt + 24 + 60)
    if not import_rva or import_size < 20:
        raise PEError("missing import directory")
    desc = _rva_to_offset(data, import_rva, sections, size_headers)
    if import_size > len(data) - desc:
        raise PEError("import directory extends past file")
    entries = []
    terminated = False
    for off in range(desc, desc + import_size - 19, 20):
        fields = struct.unpack_from("<5I", data, off)
        if fields == (0, 0, 0, 0, 0):
            terminated = True
            break
        original_thunk, timestamp, chain, name_rva, first_thunk = fields
        dll_off = _rva_to_offset(data, name_rva, sections, size_headers)
        dll = _cstring(data, dll_off, limit=260)
        thunk_rva = original_thunk or first_thunk
        thunk_off = _rva_to_offset(data, thunk_rva, sections, size_headers)
        names = []
        for index in range(65536):
            value = _u32(data, thunk_off + index * 4)
            if value == 0:
                break
            if value & IMAGE_ORDINAL_FLAG32:
                raise PEError(f"ordinal import in {dll.decode('ascii', 'replace')}")
            ibn_off = _rva_to_offset(data, value, sections, size_headers)
            hint = _u16(data, ibn_off)
            name = _cstring(data, ibn_off + 2, limit=1024)
            try:
                names.append(name.decode("ascii"))
            except UnicodeDecodeError as exc:
                raise PEError("non-ASCII import name") from exc
        else:
            raise PEError("unterminated import thunk table")
        entries.append({"offset": off, "dll_offset": dll_off, "dll": dll, "names": names})
    if not terminated:
        raise PEError("unterminated import descriptor table")
    return entries, desc


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def expected(data: bytes, digest: str, label: str) -> None:
    if len(digest) != 64 or any(c not in "0123456789abcdefABCDEF" for c in digest):
        raise PEError(f"{label} must be a 64-character SHA-256 hex digest")
    actual = sha256(data)
    if actual.lower() != digest.lower():
        raise PEError(f"{label} mismatch: got {actual}")


def matching_descriptor(data: bytes, dll: bytes) -> tuple[dict[str, object], list[dict[str, object]]]:
    entries, _ = imports(data)
    matches = [e for e in entries if bytes(e["dll"]).lower() == dll.lower()]
    if len(matches) != 1:
        raise PEError(f"expected exactly one {dll.decode()} descriptor, found {len(matches)}")
    names = list(matches[0]["names"])
    if not names or len(set(names)) != len(names):
        raise PEError("import list is empty or contains duplicate API names")
    return matches[0], entries


def make_def(names: list[str]) -> str:
    wrapped = {
        "VirtualQuery": "mp2_VirtualQuery@12",
        "VirtualProtect": "mp2_VirtualProtect@16",
        "VirtualAlloc": "mp2_VirtualAlloc@16",
        "VirtualFree": "mp2_VirtualFree@12",
    }
    out = ["LIBRARY mp2vrcmp", "EXPORTS"]
    for name in names:
        if not name or any(c in name for c in "= @\r\n\t"):
            raise PEError(f"unsafe import name for DEF: {name!r}")
        if name in wrapped:
            out.append(f"    {name}={wrapped[name]}")
        else:
            out.append(f"    {name}=KERNEL32.{name}")
    return "\n".join(out) + "\n"


def cmd_inspect(args: argparse.Namespace) -> None:
    data = Path(args.input).read_bytes()
    desc, _ = matching_descriptor(data, SOURCE_DLL if not args.patched else TARGET_DLL)
    names = list(desc["names"])
    print(json.dumps({"sha256": sha256(data), "descriptor_offset": desc["offset"],
                      "dll_name_offset": desc["dll_offset"], "dll": bytes(desc["dll"]).decode(),
                      "imports": names}, indent=2))
    if args.def_file:
        Path(args.def_file).write_text(make_def(names), encoding="ascii")


def cmd_patch(args: argparse.Namespace) -> None:
    source = Path(args.input).read_bytes()
    expected(source, args.expected_sha256, "source SHA-256")
    desc, _ = matching_descriptor(source, SOURCE_DLL)
    out_path = Path(args.output)
    if out_path.exists():
        raise PEError(f"refusing to overwrite existing output: {out_path}")
    patched = bytearray(source)
    off = int(desc["dll_offset"])
    if source[off:off + len(SOURCE_DLL)] != SOURCE_DLL:
        raise PEError("descriptor does not contain exact KERNEL32.dll spelling")
    patched[off:off + len(SOURCE_DLL)] = TARGET_DLL
    out_bytes = bytes(patched)
    patched_desc, _ = matching_descriptor(out_bytes, TARGET_DLL)
    if list(patched_desc["names"]) != list(desc["names"]):
        raise PEError("import set changed during patch")
    out_path.write_bytes(out_bytes)
    if args.def_file:
        Path(args.def_file).write_text(make_def(list(desc["names"])), encoding="ascii")
    print(json.dumps({"source_sha256": sha256(source), "patched_sha256": sha256(out_bytes),
                      "changed_offset": off, "changed_bytes": len(TARGET_DLL),
                      "imports": list(desc["names"]), "output": str(out_path)}, indent=2))


def cmd_reverse(args: argparse.Namespace) -> None:
    source = Path(args.input).read_bytes()
    expected(source, args.expected_sha256, "patched SHA-256")
    desc, _ = matching_descriptor(source, TARGET_DLL)
    out_path = Path(args.output)
    if out_path.exists():
        raise PEError(f"refusing to overwrite existing output: {out_path}")
    restored = bytearray(source)
    off = int(desc["dll_offset"])
    if source[off:off + len(TARGET_DLL)] != TARGET_DLL:
        raise PEError("descriptor does not contain exact mp2vrcmp.dll spelling")
    restored[off:off + len(SOURCE_DLL)] = SOURCE_DLL
    out_bytes = bytes(restored)
    expected(out_bytes, args.original_sha256, "restored source SHA-256")
    matching_descriptor(out_bytes, SOURCE_DLL)
    out_path.write_bytes(out_bytes)
    print(json.dumps({"patched_sha256": sha256(source), "restored_sha256": sha256(out_bytes),
                      "changed_offset": off, "output": str(out_path)}, indent=2))


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(required=True)
    i = sub.add_parser("inspect", help="show the exact import descriptor and optionally emit the facade DEF")
    i.add_argument("input")
    i.add_argument("--patched", action="store_true", help="expect mp2vrcmp.dll instead of KERNEL32.dll")
    i.add_argument("--def-file")
    i.set_defaults(func=cmd_inspect)
    a = sub.add_parser("patch", help="rewrite only the matching descriptor name")
    a.add_argument("input")
    a.add_argument("output")
    a.add_argument("--expected-sha256", required=True)
    a.add_argument("--def-file")
    a.set_defaults(func=cmd_patch)
    r = sub.add_parser("reverse", help="restore KERNEL32.dll and verify both hashes")
    r.add_argument("input")
    r.add_argument("output")
    r.add_argument("--expected-sha256", required=True, help="expected patched input digest")
    r.add_argument("--original-sha256", required=True)
    r.set_defaults(func=cmd_reverse)
    return p


def main() -> int:
    args = parser().parse_args()
    try:
        args.func(args)
    except (OSError, PEError) as exc:
        print(f"patch_import: error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
