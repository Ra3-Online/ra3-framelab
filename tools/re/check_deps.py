#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_deps.py -- verify Ra3FpsTest.exe is actually portable.

2026-09-17 / portable-GUI session (Claude)

WHY THIS EXISTS
    The whole point of Ra3FpsTest.exe is that it gets carried to a machine that has
    NOTHING installed except the game. Three ways that promise breaks silently, all of
    which still produce an exe that builds, runs on THIS machine, and looks fine:

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

    So all three are checked here, and any of them FAILS the build.

USAGE
    python check_deps.py <Ra3FpsTest.exe> <Ra3FrameLab.dll>

EXIT
    0 = portable, non-zero = at least one check failed (details on stdout)
"""

import os
import struct
import sys
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

    # --- 3. the patcher DLL must really be inside ---------------------------------------
    # RCDATA is stored verbatim, so a slice from the middle of the DLL must appear in the
    # exe. A slice, not the header: every PE shares the same DOS stub bytes.
    mid = len(dll) // 2
    probe = dll[mid:mid + 64]
    if probe and probe in exe:
        print("OK   patcher DLL payload found inside the exe (%d bytes embedded)" % len(dll))
    else:
        print("FAIL the patcher DLL is NOT embedded -- rc.exe did not pick up the RCDATA")
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
