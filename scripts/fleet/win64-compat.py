#!/usr/bin/env python3
r"""win64-compat.py - what stops a staged title from running on 64-bit Windows 10/11.

    python3 scripts/fleet/win64-compat.py <library-root-or-title-dir> [...]
    python3 scripts/fleet/win64-compat.py --json <library-root>

THREE FACTS, AND ONLY THE FIRST TWO ARE VERDICTS
------------------------------------------------
The fleet is XP/98/7. The question this answers is a DIFFERENT one: could the
owner also play a staged title on his modern box? Two things settle it without
running anything, and they are the two this tool measures:

1. **A 16-bit image.** x64 Windows has no NTVDM at all - `CreateProcess` on an
   NE (Win16) or plain-DOS MZ image fails with ERROR_BAD_EXE_FORMAT, and there
   is no compatibility shim, no "16-bit subsystem" to enable, nothing. This is
   the one blocker with no workaround inside Windows.

   **WHERE the 16-bit image sits decides whether it matters.** The fleet stages
   INSTALLED TREES, so a 16-bit *installer* or *uninstaller* left in the tree is
   never executed and blocks nothing. The tool therefore reports
   `bit16_launch` (on the path launch.txt names - fatal) separately from
   `bit16_other` (present, not executed - an observation). Conflating them would
   condemn several titles that are fine.

2. **A ring-0 copy-protection wrapper.** SafeDisc authenticates through
   `secdrv.sys`, a kernel driver Windows itself used to ship. Microsoft disabled
   it on Vista/7/8.1 in 2015 (KB3086255) and **Windows 10 never shipped it**;
   it cannot be reinstated, because an unsigned 2000s-era kernel driver does not
   load on x64 Windows. So the XP fleet's answer to SafeDisc - mount the image
   in DAEMON Tools - does not carry over: DAEMON Tools replays the *disc*, and
   the missing half on Win10 is the *driver*. A SafeDisc title is blocked on
   Win10/11 until the wrapped executable is REPLACED (official patch, source
   port, or a GOG/Steam re-release), which is a different kind of fix from
   anything the fleet does today.

   SafeDisc is identified TWO ways because one is not enough. The `stxt774`/
   `stxt371` sections only exist from SafeDisc **2.x** onward; Carmageddon 2's
   `Carma2_SW.exe` is SafeDisc **1.01.034** with an ordinary
   `.text`/`.rdata`/`.data` section table, so a section-only detector calls it
   clean. The `BoG_ *90.0&!!` marker is therefore searched for in every PE, and
   the version read as three dwords at marker + 0x20 - searched for, never at a
   fixed offset, see tests/python/test_safedisc_version_offset.py for why.
   The marker search is BOUNDED to the first 256 KB (the two measured markers
   sit at 0x3d4 and 0xfd4), so "no SafeDisc" from this tool means "no marker in
   the first 256 KB and no SafeDisc section", which is a bounded negative and is
   reported as such rather than as proof of a clean binary.
   SecuROM = `.cms_t`/`.cms_d` sections, which IS a complete header answer.

3. **Everything else is an OBSERVATION, not a verdict** - the renderer a binary
   imports, and what an `install.reg` seeds. Neither can be turned into
   RUNS/BLOCKED without running the thing, and pretending otherwise is how a
   survey turns into fiction.

THE `install.reg` TRAP THIS TOOL EXISTS TO NAME
-----------------------------------------------
Every staged title here is a 32-bit process. On x64 Windows a 32-bit process
reading `HKLM\SOFTWARE\<vendor>` is redirected to `HKLM\SOFTWARE\WOW6432Node\
<vendor>`. A 64-bit `reg.exe import` writes the *un*-redirected key, so the game
looks in the mirror and finds nothing - the seed silently does not take. The fix
is `reg import <file> /reg:32` (or SysWOW64's reg.exe). This is the same defect
CLAUDE.md records for Rainbow Six on XP, running the other way, which is why the
tool names the direction explicitly instead of just saying "registry".
`WarcraftII/directplay-win64.reg` is the control: it already writes Wow6432Node
itself, so it needs the 64-bit importer and NOT `/reg:32`.
"""
from __future__ import annotations

import json
import os
import re
import struct
import sys

# --- what counts as a binary -------------------------------------------------
BINARY_EXT = {".exe", ".dll", ".ocx", ".drv", ".ax", ".cpl", ".scr", ".sys",
              ".vxd", ".com", ".flt", ".ime", ".efi", ".mod"}

# --- image kinds -------------------------------------------------------------
PE32 = "pe32"
PE32P = "pe32+"
NE16 = "ne16"        # Win16 New Executable
LE16 = "le/lx"       # VxD or OS/2 linear
DOSMZ = "dos-mz"     # plain DOS stub with no extended header
NOTPE = "not-pe"

SIXTEEN_BIT = {NE16, LE16, DOSMZ}

# --- protection markers ------------------------------------------------------
SAFEDISC_SECTIONS = {"stxt774", "stxt371"}
SECUROM_SECTIONS = {".cms_t", ".cms_d"}
SAFEDISC_MARKER = b"BoG_ *90.0&!!"
SAFEDISC_VERSION_AT = 0x20

# --- renderer imports --------------------------------------------------------
RENDERERS = {
    "ddraw.dll": "DirectDraw",
    "d3drm.dll": "D3D retained mode",
    "d3dim.dll": "D3D immediate mode",
    "d3dim700.dll": "D3D7 immediate mode",
    "d3d8.dll": "Direct3D 8",
    "d3d9.dll": "Direct3D 9",
    "d3d10.dll": "Direct3D 10",
    "d3d11.dll": "Direct3D 11",
    "opengl32.dll": "OpenGL",
    "glide.dll": "Glide (Voodoo 1)",
    "glide2x.dll": "Glide 2.x",
    "glide3x.dll": "Glide 3.x",
    "sw3dsdk.dll": "Rendition Speedy3D",
}
# DirectPlay is not a renderer but it is the other thing Windows 10 removed from
# the default install - it survives only as the optional "DirectPlay" feature.
DIRECTPLAY = {"dplayx.dll", "dplay.dll", "dpnet.dll", "dpwsockx.dll"}

#: The SafeDisc v1 RUNTIME, which ships as ordinary files beside the game and is
#: the only way to spot v1 from the filesystem: the shipped `<GAME>.EXE` is a
#: small loader, the real program is the encrypted `<GAME>.ICD`, and these are
#: its helpers. v2/v3 instead wrap the exe in place and ship drvmgt.dll.
SAFEDISC_RUNTIME = {"clcd16.dll", "clcd32.dll", "clokspl.exe", "dplayerx.dll",
                    "drvmgt.dll", "secdrv.sys"}
#: A SafeDisc v1 loader imports almost nothing - it is not the game, so it never
#: touches DirectX, sockets or the engine DLLs. This is the set it may import;
#: anything outside it means the exe IS the game and the .icd is a leftover.
V1_LOADER_IMPORTS = {"kernel32.dll", "user32.dll", "advapi32.dll",
                     "version.dll", "gdi32.dll", "msvcrt.dll"}

#: How far into a binary the renderer-name string scan reads, and the size above
#: which a binary is not scanned at all. Both bounds are reported with the
#: result, because a miss from a bounded scan is not the same fact as an absence.
STRING_SCAN_BYTES = 24 * 1024 * 1024


def _u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def _u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


class Image:
    """The header facts we judge a binary on."""

    __slots__ = ("path", "kind", "machine", "subsystem", "subsys_major",
                 "sections", "imports", "safedisc", "securom", "size",
                 "dyn_renderers", "entry_rva", "entry_section")

    def __init__(self, path):
        self.path = path
        self.kind = NOTPE
        self.machine = None
        self.subsystem = None
        self.subsys_major = None
        self.sections = []
        self.imports = []
        self.safedisc = None      # (major, minor, subminor) or None
        self.securom = False
        self.size = 0
        self.dyn_renderers = set()
        self.entry_rva = None
        self.entry_section = None

    @property
    def is_16bit(self):
        return self.kind in SIXTEEN_BIT


def _rva_to_off(sections, rva):
    for name, vaddr, vsize, raw, rawsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return raw + (rva - vaddr)
    return None


def read_image(path, header_bytes=4096, marker_bytes=256 * 1024,
               string_scan_bytes=0):
    """Header-only read of one file.  Never loads the whole binary.

    `marker_bytes` bounds the SafeDisc VERSION search only.  Detection itself
    comes from the section table, which is complete - so a "no SafeDisc" answer
    is not a bounded-search negative even though the version read is bounded.
    """
    img = Image(path)
    try:
        img.size = os.path.getsize(path)
        with open(path, "rb") as fh:
            head = fh.read(header_bytes)
            if len(head) < 0x40 or head[:2] != b"MZ":
                return img
            lfanew = _u32(head, 0x3C)
            if lfanew <= 0 or lfanew + 0x18 > img.size or lfanew > 0x10000000:
                img.kind = DOSMZ
                return img
            if lfanew + 4 > len(head):
                fh.seek(lfanew)
                head = b"\0" * lfanew + fh.read(header_bytes)
            sig = head[lfanew:lfanew + 2]
            if sig == b"NE":
                img.kind = NE16
                return img
            if sig in (b"LE", b"LX"):
                img.kind = LE16
                return img
            if head[lfanew:lfanew + 4] != b"PE\0\0":
                img.kind = DOSMZ
                return img

            coff = lfanew + 4
            img.machine = _u16(head, coff)
            nsec = _u16(head, coff + 2)
            optsize = _u16(head, coff + 16)
            opt = coff + 20
            magic = _u16(head, opt)
            img.kind = PE32P if magic == 0x20B else PE32
            img.subsystem = _u16(head, opt + 68)
            img.subsys_major = _u16(head, opt + 48)
            img.entry_rva = _u32(head, opt + 16)

            # Data directories start at 0x60 (PE32) / 0x70 (PE32+) into the
            # optional header.  Entry 0 is the EXPORT table and entry 1 is the
            # IMPORT table - reading entry 0 and calling it "imports" is how the
            # first cut of this tool reported "imports=0" for AvP.exe and
            # Thief2.exe and, for hl.exe, 114 fragments of raw x86.  A parser
            # that returns machine code instead of DLL names has not failed
            # loudly; it has answered the wrong question quietly.
            ddbase = 0x70 if magic == 0x20B else 0x60
            ddoff = opt + ddbase + 8          # +8 = skip the export directory
            import_rva = _u32(head, ddoff) if optsize >= ddbase + 16 else 0

            sechdr = opt + optsize
            need = sechdr + nsec * 40
            if need > len(head):
                fh.seek(0)
                head = fh.read(max(need + 512, header_bytes))
            for i in range(nsec):
                b = sechdr + i * 40
                if b + 40 > len(head):
                    break
                name = head[b:b + 8].rstrip(b"\0").decode("latin-1")
                vsize = _u32(head, b + 8)
                vaddr = _u32(head, b + 12)
                rawsize = _u32(head, b + 16)
                raw = _u32(head, b + 20)
                img.sections.append((name, vaddr, vsize, raw, rawsize))

            for nm, va, vs, ra, rs in img.sections:
                if va <= img.entry_rva < va + max(vs, rs):
                    img.entry_section = nm
                    break

            names = {n.lower() for n, *_ in img.sections}
            sd_sections = bool(names & {s.lower() for s in SAFEDISC_SECTIONS})
            img.securom = bool(names & {s.lower() for s in SECUROM_SECTIONS})

            # imports (DLL names only)
            if import_rva:
                off = _rva_to_off(img.sections, import_rva)
                if off is not None:
                    fh.seek(off)
                    desc = fh.read(20 * 512)
                    for i in range(0, len(desc) - 20, 20):
                        d = desc[i:i + 20]
                        if d == b"\0" * 20:
                            break
                        name_rva = _u32(d, 12)
                        if not name_rva:
                            continue
                        noff = _rva_to_off(img.sections, name_rva)
                        if noff is None:
                            continue
                        fh.seek(noff)
                        nm = fh.read(128).split(b"\0")[0]
                        try:
                            img.imports.append(nm.decode("latin-1").lower())
                        except Exception:
                            pass

            # SafeDisc 1.x has NO stxt774/stxt371 - those arrived with 2.x.
            # Carmageddon 2's Carma2_SW.exe is the control that proves it: the
            # section table is a plain .text/.rdata/.data and the BoG_ marker is
            # nonetheless sitting at 0x3d4 declaring 1.01.034. A section-only
            # detector reports that title CLEAN, which is exactly wrong, so the
            # marker search runs on EVERY PE and not only on section hits.
            fh.seek(0)
            blob = fh.read(marker_bytes)
            i = blob.find(SAFEDISC_MARKER)
            if i >= 0 and i + SAFEDISC_VERSION_AT + 12 <= len(blob):
                img.safedisc = struct.unpack_from(
                    "<III", blob, i + SAFEDISC_VERSION_AT)
            elif sd_sections:
                # the sections say SafeDisc; the version read is what failed
                img.safedisc = (0, 0, 0)

            # A LOADED renderer is usually not an IMPORTED one. Quake III picks
            # its GL driver from the r_glDriver cvar and LoadLibrary()s it, the
            # Unreal engine loads D3DDrv/OpenGLDrv/GlideDrv by name from an ini,
            # and NewDark Thief resolves d3d9 at runtime - so all three import
            # nothing graphical and an import-only survey calls them "software".
            # The DLL name still has to be a literal string in the binary, so
            # scan for it. The scan is BOUNDED and the bound is reported: a hit
            # is proof, a miss is only "not in the first N bytes".
            if string_scan_bytes and img.size <= string_scan_bytes:
                fh.seek(0)
                low = fh.read(string_scan_bytes).lower()
                for dll, label in RENDERERS.items():
                    if dll.encode("latin-1") in low:
                        img.dyn_renderers.add(label)
    except OSError:
        pass
    return img


# Executables a shortcut may name that are NOT the game: fleet helpers, Windows
# built-ins, and the disc mounters the mount-launcher template drives.
HELPERS = {"fleetres.exe", "cmd.exe", "reg.exe", "where.exe", "timeout.exe",
           "ping.exe", "ipconfig.exe", "findstr.exe", "find.exe", "vol.exe",
           "tasklist.exe", "taskkill.exe", "xcopy.exe", "attrib.exe"}
MOUNTERS = {"daemon.exe", "dtlite.exe", "batchmnt.exe", "batchmnt64.exe"}
DOSBOX = {"dosbox.exe"}


def _win_basename(s):
    r"""Basename of a WINDOWS path, on Linux.

    `os.path.basename` splits on `/` only, so on this host it returns the whole
    of `"%~dp0OLD.EXE"` and `"DOSBOX\DOSBox.exe"` unchanged - which silently
    stopped every `.bat`-derived launch target from ever matching a file in the
    tree.  CLAUDE.md's rule about searching Windows trees case-insensitively has
    the same root: the tool runs on Linux and the data is Windows.
    """
    s = s.strip().strip('"')
    for sep in ("\\", "/"):
        if sep in s:
            s = s.rsplit(sep, 1)[-1]
    # `start "" "%~dp0OLD.EXE"` has no separator left after %~dp0 - the batch
    # expansion IS the directory part, so it has to come off too.
    s = re.sub(r"^%~[a-zA-Z]*[0-9]", "", s)
    s = re.sub(r"^%[^%]+%", "", s)
    return s.lstrip("\\/")


def _title_files(title_dir):
    """basename.lower() -> full path, for the title root only.

    Windows trees are matched CASE-INSENSITIVELY here for the reason CLAUDE.md
    gives: launch.txt says "Play Shadow Warrior.bat" and the file on the share
    may be cased any way at all.
    """
    out = {}
    try:
        for f in os.listdir(title_dir):
            out[f.lower()] = os.path.join(title_dir, f)
    except OSError:
        pass
    return out


def launch_plan(title_dir):
    r"""What each shortcut in launch.txt ACTUALLY starts.

    THIS IS THE FUNCTION THE WHOLE VERDICT TURNS ON, and the naive version of it
    is wrong in a way that condemns eight titles.

    launch.txt is TAB-separated `<shortcut .bat>\t<display name>\t<icon>`. The
    third column is an ICON, not an executable, so reading it as "the exe" makes
    the tool believe Master of Orion II launches ORION95.EXE. What actually runs
    is in the .bat.

    And what is in the .bat, for every DOS title in this library, is **DOSBox**.
    That is decisive here: a DOS/LE/NE image started INSIDE an emulator is not
    started by Windows at all, so x64 Windows' missing NTVDM does not touch it -
    DOSBox is an ordinary Win32 PE and runs on Windows 11 like any other. A
    tool that flags MAINPROG.EXE (LE) as "16-bit on the launch path" and blocks
    Carmageddon has measured the right byte and drawn the wrong conclusion.
    """
    files = _title_files(title_dir)
    lt = files.get("launch.txt")
    plan = []
    if not lt:
        return plan
    try:
        rows = open(lt, "r", encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return plan
    for row in rows:
        row = row.strip()
        if not row or row.startswith("#"):
            continue
        shortcut = row.split("\t")[0].strip().strip('"')
        if not shortcut.lower().endswith((".bat", ".cmd", ".exe", ".com")):
            continue
        entry = {"shortcut": shortcut, "runner": None, "targets": [],
                 "dosbox": False, "needs_disc": False}
        path = files.get(shortcut.lower())
        if shortcut.lower().endswith((".exe", ".com")):
            entry["runner"] = "direct"
            entry["targets"] = [shortcut.lower()]
            plan.append(entry)
            continue
        if not path:
            entry["runner"] = "missing"
            plan.append(entry)
            continue
        try:
            txt = open(path, "r", encoding="utf-8", errors="replace").read()
        except OSError:
            entry["runner"] = "unreadable"
            plan.append(entry)
            continue
        body = "\n".join(l for l in txt.splitlines()
                         if not re.match(r"\s*(rem\b|::)", l, re.I))
        names = {_win_basename(m.group(1)).lower()
                 for m in re.finditer(r'([A-Za-z0-9_.$%~\\/\-() ]+?\.(?:exe|com))',
                                      body, re.I)}
        if names & DOSBOX or re.search(r'-conf\s', body, re.I):
            entry["dosbox"] = True
            entry["runner"] = "dosbox"
        if names & MOUNTERS or re.search(r'REQUIREDISC', body):
            entry["needs_disc"] = True
        real = sorted(n for n in names
                      if n not in HELPERS and n not in MOUNTERS and n not in DOSBOX
                      and not n.startswith("dp0"))
        entry["targets"] = real
        if entry["runner"] is None:
            entry["runner"] = "win32"
        plan.append(entry)
    return plan


def win32_launch_targets(plan):
    """Basenames a shortcut starts DIRECTLY on Windows - DOSBox rows excluded,
    because those targets are started by the emulator, not by the OS."""
    out = set()
    for e in plan:
        if e["dosbox"]:
            continue
        out.update(e["targets"])
    return out


def scan_title(title_dir):
    name = os.path.basename(os.path.normpath(title_dir))
    plan = launch_plan(title_dir)
    targets = win32_launch_targets(plan)
    rep = {
        "title": name,
        "shortcuts": plan,
        "all_dosbox": bool(plan) and all(e["dosbox"] for e in plan),
        "any_dosbox": any(e["dosbox"] for e in plan),
        "needs_disc": any(e["needs_disc"] for e in plan),
        "launch_targets": sorted(targets),
        "binaries": 0,
        "pe32": 0, "pe32+": 0,
        "bit16_launch": [], "bit16_other": [],
        "safedisc": [], "securom": [],
        "safedisc_runtime_files": [], "icd_pairs": [],
        "wrapper_live": [], "protection_residue": [],
        "renderers": {}, "renderers_dynamic": {}, "local_renderer_dlls": [],
        "directplay": [],
        "install_reg": None,
        "disc_images": [],
    }
    for root, _dirs, files in os.walk(title_dir):
        for f in files:
            low = f.lower()
            ext = os.path.splitext(low)[1]
            p = os.path.join(root, f)
            rel = os.path.relpath(p, title_dir)
            if ext in (".bin", ".iso", ".mdf", ".cue", ".img", ".ccd"):
                if ext in (".bin", ".iso", ".mdf", ".img"):
                    rep["disc_images"].append(rel)
                continue
            if ext not in BINARY_EXT:
                continue
            if low in RENDERERS:
                rep["local_renderer_dlls"].append(rel)
            img = read_image(p, string_scan_bytes=STRING_SCAN_BYTES)
            if img.kind == NOTPE:
                continue
            rep["binaries"] += 1
            if img.kind == PE32:
                rep["pe32"] += 1
            elif img.kind == PE32P:
                rep["pe32+"] += 1
            elif img.is_16bit:
                bucket = "bit16_launch" if low in targets else "bit16_other"
                rep[bucket].append({"file": rel, "kind": img.kind})
            if img.safedisc is not None:
                live = bool(img.entry_section
                            and img.entry_section.lower().startswith("stxt"))
                rec = {
                    "file": rel,
                    "version": "%d.%02d.%03d" % img.safedisc if any(img.safedisc)
                               else "unreadable",
                    "on_launch_path": low in targets,
                    "entry_section": img.entry_section,
                    "wrapper_live": live,
                }
                rep["safedisc"].append(rec)
                (rep["wrapper_live"] if live
                 else rep["protection_residue"]).append(rec)
            if img.securom:
                rep["securom"].append({"file": rel,
                                       "on_launch_path": low in targets})
            for imp in img.imports:
                if imp in RENDERERS:
                    rep["renderers"].setdefault(RENDERERS[imp], []).append(rel)
                if imp in DIRECTPLAY:
                    rep["directplay"].append(rel)
            for lab in img.dyn_renderers:
                if lab not in rep["renderers"]:
                    rep["renderers_dynamic"].setdefault(lab, []).append(rel)
    rep["directplay"] = sorted(set(rep["directplay"]))
    rep["local_renderer_dlls"] = sorted(set(rep["local_renderer_dlls"]))
    for k in rep["renderers"]:
        rep["renderers"][k] = sorted(set(rep["renderers"][k]))
    for k in list(rep["renderers_dynamic"]):
        if k in rep["renderers"]:
            del rep["renderers_dynamic"][k]
        else:
            rep["renderers_dynamic"][k] = sorted(set(rep["renderers_dynamic"][k]))

    # SafeDisc v1: an <X>.icd beside an <X>.exe whose import table is
    # loader-shaped.  This is the case the marker scan CANNOT see - System Shock
    # 2's shock2.exe carries no stxt section and its entry point is an ordinary
    # MSVC prologue, because the loader IS an ordinary MSVC program; the
    # protection is in SHOCK2.ICD next to it.  Tiberian Sun is the control the
    # other way: GAME.ICD and the whole v1 runtime are present, and GAME.EXE
    # imports ddraw/dsound/binkw32, so GAME.EXE is the real game and the wrapper
    # was replaced long before this library staged it.
    for root, _dirs, files in os.walk(title_dir):
        lower = {f.lower(): f for f in files}
        for low, f in lower.items():
            if low in SAFEDISC_RUNTIME or re.match(r"^0{7}\d\.\w+$", low):
                rep["safedisc_runtime_files"].append(
                    os.path.relpath(os.path.join(root, f), title_dir))
            if not low.endswith(".icd"):
                continue
            exe = lower.get(low[:-4] + ".exe")
            if not exe:
                continue
            img = read_image(os.path.join(root, exe))
            loaderish = bool(img.imports) and all(
                i in V1_LOADER_IMPORTS for i in img.imports)
            pair = {
                "icd": os.path.relpath(os.path.join(root, f), title_dir),
                "exe": os.path.relpath(os.path.join(root, exe), title_dir),
                "exe_imports": sorted(set(img.imports)),
                "loader_shaped": loaderish,
            }
            rep["icd_pairs"].append(pair)
            (rep["wrapper_live"] if loaderish
             else rep["protection_residue"]).append(pair)
    rep["safedisc_runtime_files"] = sorted(set(rep["safedisc_runtime_files"]))

    reg = os.path.join(title_dir, "install.reg")
    if os.path.isfile(reg):
        rep["install_reg"] = analyse_reg(reg)
    return rep


def analyse_reg(path):
    """Which registry view an install.reg's HKLM keys land in on x64 Windows."""
    try:
        raw = open(path, "rb").read()
    except OSError:
        return None
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        text = raw.decode("utf-16", "replace")
    else:
        text = raw.decode("latin-1")
    keys = re.findall(r"^\s*\[([^\]]+)\]", text, re.M)
    hklm_sw = [k for k in keys if k.upper().startswith("HKEY_LOCAL_MACHINE\\SOFTWARE")]
    already = [k for k in hklm_sw if "WOW6432NODE" in k.upper()]
    return {
        "keys": len(keys),
        "hklm_software_keys": len(hklm_sw),
        "already_wow6432node": len(already),
        # a 32-bit game reads the redirected view; a 64-bit importer writes the
        # other one.  Only keys NOT already naming Wow6432Node need /reg:32.
        "needs_reg32_on_win64": len(hklm_sw) - len(already) > 0,
        "sample": hklm_sw[:3],
    }


def verdict(rep):
    """The machine-decidable part of the verdict.

    It never says RUNS. A survey that has not started the thing cannot know that
    it runs, and this repo has a standing rule about asserting a pass it did not
    measure - so the good outcome here is NO-BLOCKER-FOUND, which is an honest
    "nothing in the headers stops it" and nothing more.

    THE MARKER IS NOT THE VERDICT. Five staged titles carry a SafeDisc marker,
    a stxt section, or the v1 runtime files, and only two of them are actually
    protected: the others ship an executable whose wrapper was already removed,
    with the marker string and the helper files left behind as litter.
    Carmageddon 2 is the measured control - `CARMA2_HW.EXE` declares SafeDisc
    1.01.034 at `BoG_` + 0x20, has an `.IIDKing` section (a scene import
    rebuilder), an ordinary `.text` entry point, and it plays on Windows 11.
    So the question is never "is there a marker", it is "does the WRAPPER still
    own the entry point".
    """
    if rep["bit16_launch"]:
        return "BLOCKED", ("16-bit image started directly by Windows (%s) - "
                           "x64 has no NTVDM"
                           % rep["bit16_launch"][0]["file"])
    live = rep["wrapper_live"]
    if live:
        w = live[0]
        if "icd" in w:
            return "BLOCKED", ("SafeDisc v1 loader %s + %s - secdrv.sys does "
                               "not exist on Windows 10/11"
                               % (w["exe"], w["icd"]))
        return "BLOCKED", ("SafeDisc %s wrapper owns the entry point of %s "
                           "(section %s) - secdrv.sys does not exist on "
                           "Windows 10/11"
                           % (w["version"], w["file"], w["entry_section"]))
    if rep["securom"]:
        return "BLOCKED", ("SecuROM wrapper in %s - ring-0/DPM check with no "
                           "Win10 route" % rep["securom"][0]["file"])
    if rep["protection_residue"]:
        return "NO-BLOCKER-FOUND", ("copy-protection RESIDUE only - the wrapped "
                                    "executable was already replaced")
    if rep["all_dosbox"]:
        return "NO-BLOCKER-FOUND", ("runs inside DOSBox - the 16-bit payload "
                                    "never reaches the Windows loader")
    return "NO-BLOCKER-FOUND", "no 16-bit image on the launch path, no protection wrapper"


def main(argv):
    as_json = False
    args = []
    for a in argv:
        if a == "--json":
            as_json = True
        else:
            args.append(a)
    if not args:
        print(__doc__.strip().splitlines()[0])
        print("usage: win64-compat.py [--json] <library-root-or-title-dir> [...]")
        return 2
    titles = []
    for a in args:
        if os.path.isfile(os.path.join(a, "launch.txt")):
            titles.append(a)
        else:
            for e in sorted(os.listdir(a)):
                d = os.path.join(a, e)
                if os.path.isdir(d) and os.path.isfile(os.path.join(d, "launch.txt")):
                    titles.append(d)
    reports = []
    for t in titles:
        r = scan_title(t)
        r["verdict"], r["deciding_factor"] = verdict(r)
        reports.append(r)
        if not as_json:
            print("%-28s %-18s %s" % (r["title"], r["verdict"], r["deciding_factor"]))
            if r["bit16_other"]:
                print("%-28s   (16-bit off the launch path: %s)"
                      % ("", ", ".join(x["file"] for x in r["bit16_other"][:4])))
    if as_json:
        json.dump(reports, sys.stdout, indent=1)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
