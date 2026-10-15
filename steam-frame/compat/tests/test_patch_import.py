# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("patch_import", ROOT / "patch_import.py")
patch_import = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = patch_import
spec.loader.exec_module(patch_import)


def fixture(dlls=(b"KERNEL32.dll", b"USER32.dll"), ordinal=False):
    """Small PE32 with two descriptors, one section, and independent import tables."""
    data = bytearray(0x1000)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x14C, 1, 0, 0, 0, 0xE0, 0x010F)
    opt = 0x98
    struct.pack_into("<H", data, opt, 0x10B)
    struct.pack_into("<I", data, opt + 56, 0x2000)
    struct.pack_into("<I", data, opt + 60, 0x200)
    struct.pack_into("<I", data, opt + 92, 16)
    struct.pack_into("<II", data, opt + 96 + 8, 0x1000, 60)
    sec = opt + 0xE0
    data[sec:sec + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", data, sec + 8, 0x800, 0x1000, 0x800, 0x200)
    # Descriptor table at raw 0x200, mapped RVA 0x1000.
    for i, dll in enumerate(dlls):
        d_off = 0x200 + i * 20
        name_rva = 0x1100 + i * 0x40
        thunk_rva = 0x1200 + i * 0x40
        struct.pack_into("<5I", data, d_off, thunk_rva, 0, 0, name_rva, thunk_rva + 0x10)
        name_off = 0x300 + i * 0x40
        data[name_off:name_off + len(dll) + 1] = dll + b"\0"
        thunk_off = 0x400 + i * 0x40
        value = 0x80000007 if ordinal and i == 0 else 0x1300 + i * 0x40
        struct.pack_into("<II", data, thunk_off, value, 0)
        if not value & 0x80000000:
            ibn_off = 0x500 + i * 0x40
            struct.pack_into("<H", data, ibn_off, 0)
            data[ibn_off + 2:ibn_off + 8] = b"FooApi\0"
    return bytes(data)


class ImportPatcherTests(unittest.TestCase):
    def test_patch_changes_only_the_one_descriptor_name_and_round_trips(self):
        source = fixture()
        entry, _ = patch_import.matching_descriptor(source, patch_import.SOURCE_DLL)
        patched = bytearray(source)
        off = entry["dll_offset"]
        patched[off:off + 12] = patch_import.TARGET_DLL
        self.assertEqual(bytes(patched[:off]), source[:off])
        self.assertEqual(bytes(patched[off:off + 12]), patch_import.TARGET_DLL)
        self.assertEqual(bytes(patched[off + 12:]), source[off + 12:])
        self.assertEqual(patch_import.make_def(["FooApi", "VirtualQuery", "VirtualProtect"]),
                         "LIBRARY mp2vrcmp\nEXPORTS\n    FooApi=KERNEL32.FooApi\n"
                         "    VirtualQuery=mp2_VirtualQuery@12\n"
                         "    VirtualProtect=mp2_VirtualProtect@16\n")
        self.assertEqual(patch_import.matching_descriptor(bytes(patched), patch_import.TARGET_DLL)[0]["names"], ["FooApi"])
        restored = bytearray(patched)
        restored[off:off + 12] = patch_import.SOURCE_DLL
        self.assertEqual(bytes(restored), source)

    def test_patcher_cli_requires_hash_and_refuses_unknown_or_overwrite(self):
        source = fixture()
        with tempfile.TemporaryDirectory() as td:
            src, out = Path(td) / "in.asi", Path(td) / "out.asi"
            src.write_bytes(source)
            def run(*args):
                return subprocess.run([sys.executable, str(ROOT / "patch_import.py"), *map(str, args)],
                                      capture_output=True, text=True)
            wrong = run("patch", src, out, "--expected-sha256", "0" * 64)
            self.assertEqual(wrong.returncode, 2)
            self.assertFalse(out.exists())
            digest = hashlib.sha256(source).hexdigest()
            ok = run("patch", src, out, "--expected-sha256", digest)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            patched = out.read_bytes()
            reverse = Path(td) / "restored.asi"
            reverse_result = run("reverse", out, reverse, "--expected-sha256",
                                 hashlib.sha256(patched).hexdigest(), "--original-sha256", digest)
            self.assertEqual(reverse_result.returncode, 0, reverse_result.stderr)
            self.assertEqual(reverse.read_bytes(), source)
            already_patched = run("patch", out, Path(td) / "bad.asi", "--expected-sha256",
                                  hashlib.sha256(patched).hexdigest())
            self.assertEqual(already_patched.returncode, 2)
            self.assertFalse((Path(td) / "bad.asi").exists())
            overwrite = run("patch", src, out, "--expected-sha256", digest)
            self.assertEqual(overwrite.returncode, 2)

    def test_rejects_multiple_target_descriptors_and_ordinal_thunks(self):
        with self.assertRaises(patch_import.PEError):
            patch_import.matching_descriptor(fixture((b"KERNEL32.dll", b"kernel32.DLL")), patch_import.SOURCE_DLL)
        with self.assertRaisesRegex(patch_import.PEError, "ordinal"):
            patch_import.imports(fixture(ordinal=True))

    def test_rejects_malformed_descriptor_and_non_pe32(self):
        with self.assertRaises(patch_import.PEError):
            patch_import.imports(b"MZ" + bytes(100))
        bad = bytearray(fixture())
        struct.pack_into("<H", bad, 0x98, 0x20B)
        with self.assertRaisesRegex(patch_import.PEError, "PE32"):
            patch_import.imports(bytes(bad))


if __name__ == "__main__":
    unittest.main()
