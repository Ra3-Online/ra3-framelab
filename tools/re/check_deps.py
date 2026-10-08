#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_deps.py -- verify Ra3FpsTest.exe is actually portable.

2026-09-17 / portable-GUI session (Claude)

WHY THIS EXISTS
    The whole point of Ra3FpsTest.exe is that it gets carried to a machine that has
    NOTHING installed except the game. Four ways that promise breaks silently, all of
    which can still produce an exe that compiles successfully:

      1. A missing /MT.  cl.exe defaults to the dynamic CRT, so the exe quietly needs
         msvcp140.dll / vcruntime140.dll / ucrtbase.dll. On a clean machine it then fails
         with "找不到 vcruntime140.dll" -- and on this machine it works perfectly, so
         nobody notices until the tool is already on the other box.

      2. The DLL resource did not get embedded.  rc.exe resolves the RCDATA path relative
         to its working directory; get that wrong and rc still exits 0 and produces a .res
         with no payload. The exe builds, the GUI opens, and only at "开始游戏" does it
         say the DLL is missing.

      3. An absolute path from the build machine leaked in.  The most common source is the
         linker's PDB reference, which bakes in something like
         an absolute build-directory path ending in "Ra3FpsTest.pdb". Harmless functionally, but the user's
         requirement was explicit: the tool must not carry this machine's paths.

      4. An invalid startup manifest. rc.exe does not validate its XML; a double hyphen
         in a comment can make Windows reject the exe before wWinMain. Parse the actual
         RT_MANIFEST/1 and ask Windows to resolve it, without running the program.

    All four are checked here, and any failure stops the Windows build/package.

USAGE
    python check_deps.py <Ra3FpsTest.exe> <Ra3FrameLab.dll>

EXIT
    0 = portable, non-zero = at least one check failed (details on stdout)
"""

import hashlib
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Everything here ships with Windows itself. Anything else means a dependency.
SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll",
    "ole32.dll", "oleaut32.dll", "comctl32.dll", "shlwapi.dll", "version.dll",
    "ntdll.dll", "ws2_32.dll", "imm32.dll", "winmm.dll", "uxtheme.dll",
    "comdlg32.dll", "msimg32.dll", "rpcrt4.dll", "dbghelp.dll", "powrprof.dll",
    "setupapi.dll", "cfgmgr32.dll", "bcrypt.dll", "crypt32.dll", "sechost.dll",
    "win32u.dll", "gdi32full.dll", "msvcp_win.dll", "ucrtbase.dll",  # ucrtbase: see note
}

# ucrtbase.dll and the api-ms-win-crt-* forwarders ARE part of Windows 10+, but an /MT
# build must not need them at all. Flag them so a regression to the dynamic CRT is caught
# rather than waved through.
CRT_RED_FLAGS = ("ucrtbase.dll", "msvcp_win.dll")
CRT_RED_PREFIX = "api-ms-win-crt"

# tools/gui/gui_res.h and ra3fps_gui.rc use this numeric RT_RCDATA resource.
IDR_FRAMELAB_DLL = 101
RT_RCDATA = 10
RT_MANIFEST = 24


def embedded_resource(d, resource_type, resource_id):
    """Extract one unambiguous, file-backed numeric resource without loading the PE."""
    label = "resource %d/%d" % (resource_type, resource_id)
    def checked(offset, size, limit=len(d)):
        if offset < 0 or size < 0 or offset > limit or size > limit - offset:
            raise ValueError("PE/resource range exceeds its file or directory bounds")
        return offset

    checked(0, 64)
    if d[:2] != b"MZ":
        raise ValueError("missing DOS signature")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    checked(pe, 24)
    if pe < 64 or d[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    checked(opt, optsz)
    if optsz < 2 or not 1 <= nsec <= 96:
        raise ValueError("invalid PE section count or optional header")
    magic = struct.unpack_from("<H", d, opt)[0]
    if magic not in (0x10B, 0x20B):
        raise ValueError("unsupported PE optional header")
    dd = opt + (96 if magic == 0x10B else 112)
    checked(dd - 4, 28, opt + optsz)
    if struct.unpack_from("<I", d, dd - 4)[0] < 3:
        raise ValueError("PE has no resource data directory")
    section_table = opt + optsz
    checked(section_table, nsec * 40)
    header_size = struct.unpack_from("<I", d, opt + 60)[0]
    if header_size < section_table + nsec * 40 or header_size > len(d):
        raise ValueError("invalid PE SizeOfHeaders")
    sections = []
    for i in range(nsec):
        vs, va, rs, raw = struct.unpack_from("<IIII", d, section_table + i * 40 + 8)
        span = max(vs, rs)
        if va + span > 0x100000000:
            raise ValueError("PE section RVA overflow")
        if rs:
            checked(raw, rs)
            if raw < header_size:
                raise ValueError("PE section overlaps headers")
        for old_va, old_span, old_raw, old_rs in sections:
            if span and old_span and max(va, old_va) < min(va + span, old_va + old_span):
                raise ValueError("overlapping PE section RVAs")
            if rs and old_rs and max(raw, old_raw) < min(raw + rs, old_raw + old_rs):
                raise ValueError("overlapping PE section file ranges")
        sections.append((va, span, raw, rs))

    def file_range(rva, size):
        if size <= 0 or rva + size > 0x100000000:
            raise ValueError("empty or overflowing resource RVA range")
        matches = []
        if rva < header_size and size <= header_size - rva:
            matches.append(rva)
        for va, span, raw, rs in sections:
            if va <= rva < va + span:
                if size > rs - (rva - va):
                    raise ValueError("resource reaches an unbacked PE section tail")
                matches.append(raw + rva - va)
        if len(matches) != 1:
            raise ValueError("resource RVA is missing or ambiguous")
        return checked(matches[0], size)

    resource_rva, resource_size = struct.unpack_from("<II", d, dd + 16)
    base = file_range(resource_rva, resource_size)
    metadata = []

    def resource_range(rel, size):
        checked(rel, size, resource_size)
        start = base + rel
        for old_start, old_end in metadata:
            if max(start, old_start) < min(start + size, old_end):
                raise ValueError("overlapping or cyclic resource metadata")
        metadata.append((start, start + size))
        return start

    def directory(rel):
        checked(rel, 16, resource_size)
        named, ids = struct.unpack_from("<HH", d, base + rel + 12)
        start = resource_range(rel, 16 + (named + ids) * 8)
        entries = {}
        for i in range(named + ids):
            key, target = struct.unpack_from("<II", d, start + 16 + i * 8)
            if bool(key & 0x80000000) != (i < named):
                raise ValueError("resource named/ID entry counts disagree")
            if key & 0x80000000:
                name_rel = key & 0x7FFFFFFF
                checked(name_rel, 2, resource_size)
                count = struct.unpack_from("<H", d, base + name_rel)[0]
                checked(name_rel + 2, count * 2, resource_size)
                key = d[base + name_rel + 2:base + name_rel + 2 + count * 2].decode("utf-16-le")
            elif key > 0xFFFF:
                raise ValueError("resource numeric ID exceeds 16 bits")
            if key in entries:
                raise ValueError("duplicate resource entry")
            entries[key] = target
        return entries

    def subdirectory(entries, key):
        target = entries.get(key)
        if target is None or not target & 0x80000000:
            raise ValueError("missing %s directory" % label)
        return directory(target & 0x7FFFFFFF)

    names = subdirectory(directory(0), resource_type)
    languages = subdirectory(names, resource_id)
    if len(languages) != 1:
        raise ValueError("%s must have exactly one language" % label)
    language, target = next(iter(languages.items()))
    if not isinstance(language, int) or target & 0x80000000:
        raise ValueError("invalid %s language or data entry" % label)
    entry = resource_range(target, 16)
    data_rva, data_size, _, reserved = struct.unpack_from("<IIII", d, entry)
    if reserved:
        raise ValueError("nonzero resource data entry reserved field")
    offset = file_range(data_rva, data_size)
    if any(max(offset, start) < min(offset + data_size, end) for start, end in metadata):
        raise ValueError("%s payload overlaps resource metadata" % label)
    return d[offset:offset + data_size]


def embedded_framelab_dll(d):
    return embedded_resource(d, RT_RCDATA, IDR_FRAMELAB_DLL)


def validate_manifest(d):
    """Parse the actual startup manifest, including comments, before Windows sees it."""
    root = ET.fromstring(embedded_resource(d, RT_MANIFEST, 1))
    if root.tag != "{urn:schemas-microsoft-com:asm.v1}assembly":
        raise ValueError("startup manifest root is not an asm.v1 assembly")
    if root.get("manifestVersion") != "1.0":
        raise ValueError("unsupported startup manifest version")
    return root


def validate_activation_context(exe_path):
    """Ask Windows to resolve RT_MANIFEST/1; never execute the GUI or DLL."""
    import ctypes
    from ctypes import wintypes

    class ACTCTXW(ctypes.Structure):
        _fields_ = [
            ("cbSize", wintypes.ULONG), ("dwFlags", wintypes.DWORD),
            ("lpSource", wintypes.LPCWSTR), ("wProcessorArchitecture", wintypes.WORD),
            ("wLangId", wintypes.WORD), ("lpAssemblyDirectory", wintypes.LPCWSTR),
            ("lpResourceName", ctypes.c_void_p), ("lpApplicationName", wintypes.LPCWSTR),
            ("hModule", wintypes.HMODULE),
        ]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateActCtxW.argtypes = [ctypes.POINTER(ACTCTXW)]
    kernel.CreateActCtxW.restype = wintypes.HANDLE
    kernel.ReleaseActCtx.argtypes = [wintypes.HANDLE]
    kernel.ReleaseActCtx.restype = None
    context = ACTCTXW()
    context.cbSize = ctypes.sizeof(context)
    # Select the EXE's startup resource. Let Windows infer the architecture: forcing
    # x86 here from 64-bit Python is rejected with ERROR_INVALID_PARAMETER.
    context.dwFlags = 0x008  # ACTCTX_FLAG_RESOURCE_NAME_VALID
    context.lpSource = str(Path(exe_path).resolve())
    context.lpResourceName = 1  # MAKEINTRESOURCE(CREATEPROCESS_MANIFEST_RESOURCE_ID)
    handle = kernel.CreateActCtxW(ctypes.byref(context))
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    kernel.ReleaseActCtx(handle)


def pe_layout(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    machine = struct.unpack_from("<H", d, pe + 4)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dd = opt + (96 if magic == 0x10B else 112)
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        va, vs, raw, rs = struct.unpack_from("<IIII", d, o + 12)
        secs.append((va, max(vs, rs), raw))
    return machine, dd, secs


def rva_to_off(secs, rva):
    for va, size, raw in secs:
        if va <= rva < va + size:
            return raw + (rva - va)
    return None


def imported_dlls(d):
    machine, dd, secs = pe_layout(d)
    imp_rva = struct.unpack_from("<I", d, dd + 8)[0]
    if not imp_rva:
        return []
    off = rva_to_off(secs, imp_rva)
    names = []
    while True:
        ent = d[off:off + 20]
        if len(ent) < 20 or ent == b"\0" * 20:
            break
        nm_rva = struct.unpack_from("<I", ent, 12)[0]
        if nm_rva == 0:
            break
        no = rva_to_off(secs, nm_rva)
        names.append(d[no:d.index(b"\0", no)].decode("latin1"))
        off += 20
    return names


def main():
    if len(sys.argv) < 3:
        print("usage: check_deps.py <Ra3FpsTest.exe> <Ra3FrameLab.dll>")
        return 2
    exe_path, dll_path = sys.argv[1], sys.argv[2]
    exe = open(exe_path, "rb").read()
    dll = open(dll_path, "rb").read()
    failures = []

    # --- 1. must be x86: a 64-bit process cannot LoadLibrary into the 32-bit game --------
    machine, _, _ = pe_layout(exe)
    if machine == 0x14C:
        print("OK   exe is i386 (x86) -- can inject into the 32-bit game")
    else:
        print("FAIL exe machine = 0x%04X, expected 0x014C (i386)" % machine)
        failures.append("machine")

    # --- 2. imports must all be OS-provided ---------------------------------------------
    names = imported_dlls(exe)
    print("      imports: %s" % ", ".join(names))
    bad = []
    for n in names:
        low = n.lower()
        if low in CRT_RED_FLAGS or low.startswith(CRT_RED_PREFIX):
            bad.append(n + " (C runtime -- /MT missing?)")
        elif low not in SYSTEM_DLLS:
            bad.append(n + " (not a Windows component)")
    if bad:
        print("FAIL non-portable imports: %s" % ", ".join(bad))
        failures.append("imports")
    else:
        print("OK   every import is a Windows component -- no runtime to install")

    # rc.exe can embed malformed XML successfully. Check the final PE, not only its source.
    try:
        validate_manifest(exe)
        print("OK   RT_MANIFEST/1 startup manifest is well-formed XML")
        if sys.platform == "win32":
            validate_activation_context(exe_path)
            print("OK   Windows resolves the embedded startup activation context")
        else:
            print("SKIP Windows activation-context resolution (requires Windows)")
    except (ValueError, struct.error, ET.ParseError, OSError) as error:
        print("FAIL startup manifest/activation context: %s" % error)
        failures.append("manifest")

    # --- 3. the patcher DLL must really be inside ---------------------------------------
    # Check the GUI's actual resource, not an incidental byte sequence elsewhere in the PE.
    try:
        payload = embedded_framelab_dll(exe)
        payload_sha = hashlib.sha256(payload).hexdigest()
        dll_sha = hashlib.sha256(dll).hexdigest()
        if payload != dll or payload_sha != dll_sha:
            raise ValueError("embedded DLL differs: resource %d bytes/%s, supplied %d bytes/%s"
                             % (len(payload), payload_sha, len(dll), dll_sha))
        print("OK   RCDATA/101 is the complete supplied DLL (%d bytes, SHA-256 %s)"
              % (len(payload), payload_sha))
    except (ValueError, struct.error) as error:
        print("FAIL patcher DLL resource: %s" % error)
        failures.append("payload")

    # --- 4. no path from the build machine ----------------------------------------------
    # Two tiers, because a hard FAIL has to be something that is unambiguously wrong:
    #
    #   FAIL needles -- the current checkout path or a linker debug reference.
    #     User-profile paths are also rejected regardless of drive letter/account name.
    #   WARN -- a generic drive-letter path scan. It is a warning and not a failure on purpose: the
    #     embedded DLL is a blob of machine code, and machine code can accidentally contain
    #     the byte pattern of a drive letter. Reporting the offsets lets a human judge.
    #
    # NOTE: the product/company name "Ra3 Frame Lab" is deliberately NOT a needle. It lives
    # in the version resource (which Windows stores as UTF-16) and is supposed to be there --
    # an earlier version of this check flagged it and that was a false positive.
    failures_local = []
    needles = [str(Path(__file__).resolve().parents[2]), str(Path(__file__).resolve().parents[2]).replace("\\", "/"), ".pdb"]
    for n in needles:
        if n.encode("utf-8") in exe:
            failures_local.append(n)
        wide = n.encode("utf-16-le")
        if wide in exe:
            failures_local.append(n + " (utf-16)")
    import re
    if re.search(rb"[A-Za-z]:[\\/]Users[\\/]", exe, re.I):
        failures_local.append("user-profile path (ascii)")
    if re.search(r"[A-Za-z]:[\\/]Users[\\/]", exe.decode("utf-16-le", errors="ignore"), re.I):
        failures_local.append("user-profile path (utf-16)")
    if failures_local:
        print("FAIL build-machine path leaked into the exe: %s" % ", ".join(sorted(set(failures_local))))
        print("     fix: link without /DEBUG (no PDB reference), or link with /Brepro")
        failures.append("paths")
    else:
        print("OK   no build-machine path in the exe (no PDB reference, no drive letters)")

    # Generic drive-letter scan, ASCII and UTF-16LE, as a warning only.
    import re
    hits = []
    for m in re.finditer(rb"[A-Za-z]:\\[A-Za-z]", exe):
        hits.append("0x%X/ascii" % m.start())
    # Decode the whole blob as UTF-16LE (ignoring the half-characters) and scan the text form.
    # Doing it this way avoids hand-building a UTF-16 regex, where the NUL bytes would be read
    # as regex syntax.
    wide_text = exe.decode("utf-16-le", errors="ignore")
    for m in re.finditer(r"[A-Za-z]:\\[A-Za-z]", wide_text):
        hits.append("0x%X/utf-16" % (m.start() * 2))
    if hits:
        print("WARN possible drive letter(s) at %s -- usually random bytes inside the embedded DLL,"
              % ", ".join(hits[:6]))
        print("     not a real path. Inspect before shipping if the count looks large.")
    else:
        print("OK   no drive-letter pattern anywhere in the exe")

    print("")
    if failures:
        print("---- check_deps FAILED (%s) ----" % ", ".join(failures))
        return 1
    print("---- check_deps OK: portable, self-contained ----")
    return 0


if __name__ == "__main__":
    sys.exit(main())
