"""scripts/fleet/win64-compat.py - the four judgements that decide a title's
Windows 10/11 verdict, each pinned against the case that got it wrong first.

Every assertion here corresponds to a mistake the tool actually made during the
2026-09-01 compatibility survey, and three of them would have condemned titles
that were subsequently MEASURED running on a Windows 11 VM.
"""
import importlib.util
import json
import os
import struct
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(REPO, "scripts", "fleet", "win64-compat.py")


def _load():
    spec = importlib.util.spec_from_file_location("win64_compat", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


W = _load()


# --------------------------------------------------------------------------
# A minimal PE32 builder.  Enough header for the tool to parse: DOS stub, COFF
# header, optional header with an entry point and a data directory, a section
# table, and an import descriptor.  Nothing here is executable and nothing
# needs to be - the tool only ever reads headers.
# --------------------------------------------------------------------------
def build_pe(path, sections, entry_rva, imports=(), stub_extra=b"",
             machine=0x14C, magic=0x10B):
    """sections: [(name, vaddr, vsize, rawsize)] - file offsets are assigned."""
    stub = bytearray(b"MZ" + b"\0" * 0x3E)
    stub += stub_extra
    lfanew = len(stub)
    struct.pack_into("<I", stub, 0x3C, lfanew)

    nsec = len(sections)
    optsize = 0xE0
    coff = struct.pack("<IHHIIIHH", 0x00004550, machine, nsec, 0, 0, 0,
                       optsize, 0x0102)
    opt = bytearray(optsize)
    struct.pack_into("<H", opt, 0, magic)
    struct.pack_into("<I", opt, 16, entry_rva)
    struct.pack_into("<I", opt, 28, 0x400000)
    struct.pack_into("<H", opt, 48, 4)      # MajorSubsystemVersion
    struct.pack_into("<H", opt, 68, 2)      # Subsystem = GUI

    sechdr_off = lfanew + 4 + 20 + optsize
    body_off = sechdr_off + nsec * 40
    body_off = (body_off + 0x1FF) // 0x200 * 0x200

    # lay the import descriptor + names into the FIRST section's raw data
    imp_blob = b""
    imp_rva = 0
    if imports:
        base_rva = sections[0][1]
        n = len(imports)
        desc_size = (n + 1) * 20
        names_at = desc_size
        descs = b""
        names = b""
        for dll in imports:
            descs += struct.pack("<IIIII", 0, 0, 0, base_rva + names_at + len(names), 0)
            names += dll.encode() + b"\0"
        descs += b"\0" * 20
        imp_blob = descs + names
        imp_rva = base_rva
        struct.pack_into("<I", opt, 0x60 + 8, imp_rva)   # data directory 1
        struct.pack_into("<I", opt, 0x60 + 12, desc_size)

    sectab = b""
    raws = []
    cur = body_off
    for i, (name, vaddr, vsize, rawsize) in enumerate(sections):
        sectab += (name.encode()[:8].ljust(8, b"\0")
                   + struct.pack("<IIII", vsize, vaddr, rawsize, cur)
                   + b"\0" * 12 + struct.pack("<I", 0x60000020))
        raws.append((cur, rawsize))
        cur += rawsize

    with open(path, "wb") as fh:
        fh.write(bytes(stub))
        fh.write(coff)
        fh.write(bytes(opt))
        fh.write(sectab)
        fh.write(b"\0" * (body_off - fh.tell()))
        for i, (off, rawsize) in enumerate(raws):
            data = bytearray(rawsize)
            if i == 0 and imp_blob:
                data[:len(imp_blob)] = imp_blob
            fh.write(bytes(data))
    return path


ORDINARY = [(".text", 0x1000, 0x1000, 0x400), (".rdata", 0x2000, 0x400, 0x400),
            (".data", 0x3000, 0x400, 0x400)]
WRAPPED = ORDINARY + [("stxt774", 0x4000, 0x400, 0x400),
                      ("stxt371", 0x5000, 0x400, 0x400)]


def safedisc_stub(major, minor, sub):
    """A DOS-stub payload carrying the BoG_ marker and a version triple."""
    return (W.SAFEDISC_MARKER + b"\0" * (W.SAFEDISC_VERSION_AT - len(W.SAFEDISC_MARKER))
            + struct.pack("<III", major, minor, sub) + b"\0" * 16)


# --------------------------------------------------------------------------
# 1. SafeDisc 1.x has NO stxt sections.
# --------------------------------------------------------------------------
def test_safedisc_v1_is_found_without_an_stxt_section(tmp_path):
    """Carmageddon 2's Carma2_SW.exe is SafeDisc 1.01.034 with a plain
    .text/.rdata/.data section table.  The first cut of this tool identified
    SafeDisc from the SECTION NAMES alone and therefore called it clean.  The
    marker search has to run on every PE, not only on section hits."""
    p = build_pe(str(tmp_path / "carma.exe"), ORDINARY, 0x1000,
                 stub_extra=safedisc_stub(1, 1, 34))
    img = W.read_image(p)
    assert img.safedisc == (1, 1, 34)
    assert not any(n.lower().startswith("stxt") for n, *_ in img.sections)


def test_safedisc_2_is_found_from_marker_and_sections(tmp_path):
    p = build_pe(str(tmp_path / "mod.dll"), WRAPPED, 0x5000,
                 stub_extra=safedisc_stub(2, 0x50, 0x0A))
    img = W.read_image(p)
    assert img.safedisc == (2, 0x50, 0x0A)      # 2.80.010


# --------------------------------------------------------------------------
# 2. A marker is not a verdict - the ENTRY POINT is.
# --------------------------------------------------------------------------
def test_wrapper_is_live_only_when_it_owns_the_entry_point(tmp_path):
    """MEASURED 2026-09-01 on a Windows 11 VM: Carmageddon 2 carries a SafeDisc
    1.01.034 marker and PLAYS, because its wrapper was stripped long ago and
    only the marker string was left behind.  Max Payne carries a 2.51.020
    marker whose stxt371 section IS the entry point, and it exits with code 1
    before drawing anything.  Same evidence class, opposite outcome - so the
    tool must judge on where the entry point lands, not on the marker."""
    live = build_pe(str(tmp_path / "live.exe"), WRAPPED, 0x5000,
                    stub_extra=safedisc_stub(2, 51, 20))
    dead = build_pe(str(tmp_path / "dead.exe"), ORDINARY, 0x1000,
                    stub_extra=safedisc_stub(1, 1, 34))
    assert W.read_image(live).entry_section == "stxt371"
    assert W.read_image(dead).entry_section == ".text"


def _title(tmp_path, name, files, launch_rows):
    d = tmp_path / name
    d.mkdir()
    for fn, content in files.items():
        p = d / fn
        p.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            p.write_bytes(content)
        else:
            p.write_text(content)
    (d / "launch.txt").write_text("\n".join(launch_rows) + "\n")
    return str(d)


def test_icd_pair_is_live_only_when_the_exe_is_loader_shaped(tmp_path):
    """SafeDisc v1 leaves NO stxt section and NO wrapped entry point: the
    shipped .exe is a small loader and the game is the encrypted .icd beside
    it.  System Shock 2 is that case and is blocked; Tiberian Sun has the same
    GAME.ICD and the same v1 runtime files, but GAME.EXE imports ddraw/dsound,
    i.e. it IS the game and the loader was replaced.  Both were measured:
    shock2.exe dies with 0xC000001D, Tiberian Sun reaches its main menu."""
    ss2 = _title(tmp_path, "SS2", {}, ["Play.bat\tSS2\ticon.ico"])
    build_pe(os.path.join(ss2, "shock2.exe"), ORDINARY, 0x1000,
             imports=["kernel32.dll", "user32.dll", "advapi32.dll", "version.dll"])
    open(os.path.join(ss2, "SHOCK2.ICD"), "wb").write(b"\0" * 16)
    (open(os.path.join(ss2, "Play.bat"), "w")
     .write('@echo off\r\nstart "" "%~dp0shock2.exe"\r\n'))
    rep = W.scan_title(ss2)
    assert rep["icd_pairs"] and rep["icd_pairs"][0]["loader_shaped"] is True
    assert rep["wrapper_live"]

    ts = _title(tmp_path, "TS", {}, ["Play.bat\tTS\ticon.ico"])
    build_pe(os.path.join(ts, "GAME.EXE"), ORDINARY, 0x1000,
             imports=["kernel32.dll", "ddraw.dll", "dsound.dll"])
    open(os.path.join(ts, "GAME.ICD"), "wb").write(b"\0" * 16)
    (open(os.path.join(ts, "Play.bat"), "w")
     .write('@echo off\r\nstart "" "%~dp0GAME.EXE"\r\n'))
    rep = W.scan_title(ts)
    assert rep["icd_pairs"] and rep["icd_pairs"][0]["loader_shaped"] is False
    assert not rep["wrapper_live"]
    assert rep["protection_residue"]


# --------------------------------------------------------------------------
# 3. DOSBox indirection - the difference between eight BLOCKED and eight RUNS.
# --------------------------------------------------------------------------
def test_a_dosbox_shortcut_does_not_put_its_dos_payload_on_the_launch_path(tmp_path):
    """MAINPROG.EXE, RR.EXE, Sw.exe, WAR.EXE and ORION2.EXE are all DOS/LE
    images and all fatal if Windows loads them, because x64 has no NTVDM.
    Windows never loads them: their shortcuts start DOSBox, which is an
    ordinary Win32 PE.  Measured 2026-09-01 - Shadow Warrior reaches its main
    menu fullscreen on Windows 11 under the staged DOSBox."""
    d = _title(tmp_path, "SW", {}, ["Play SW.bat\tShadow Warrior\tsw.ico"])
    (open(os.path.join(d, "Play SW.bat"), "w").write(
        '@echo off\r\n'
        'rem the fleet DOSBox pattern\r\n'
        'cd /d "%~dp0DOSBOX"\r\n'
        'DOSBox.exe -conf "..\\dosbox_sw.conf"\r\n'
        'exit\r\n'))
    os.mkdir(os.path.join(d, "DOSBOX"))
    build_pe(os.path.join(d, "DOSBOX", "DOSBox.exe"), ORDINARY, 0x1000)
    # a DOS MZ payload with no extended header: the thing x64 cannot run
    open(os.path.join(d, "Sw.exe"), "wb").write(b"MZ" + b"\0" * 0x3C + b"\0\0\0\0")

    plan = W.launch_plan(d)
    assert plan and plan[0]["dosbox"] is True
    assert "sw.exe" not in W.win32_launch_targets(plan)

    rep = W.scan_title(d)
    rep["verdict"], rep["deciding_factor"] = W.verdict(rep)
    assert rep["bit16_launch"] == []
    assert [x["file"] for x in rep["bit16_other"]] == ["Sw.exe"]
    assert rep["verdict"] == "NO-BLOCKER-FOUND"


def test_a_16bit_image_started_directly_by_windows_is_blocked(tmp_path):
    """The control for the test above: the same DOS image, no emulator."""
    d = _title(tmp_path, "Direct", {}, ["Play.bat\tDirect\ticon.ico"])
    (open(os.path.join(d, "Play.bat"), "w")
     .write('@echo off\r\nstart "" "%~dp0OLD.EXE"\r\n'))
    open(os.path.join(d, "OLD.EXE"), "wb").write(b"MZ" + b"\0" * 0x3C + b"\0\0\0\0")
    rep = W.scan_title(d)
    rep["verdict"], rep["deciding_factor"] = W.verdict(rep)
    assert rep["verdict"] == "BLOCKED"
    assert "NTVDM" in rep["deciding_factor"]


def test_launch_txt_third_column_is_an_icon_not_an_executable(tmp_path):
    """Reading the third field as "the exe" makes the tool believe Master of
    Orion II launches ORION95.EXE.  It launches DOSBox; ORION95.EXE is where
    the shortcut's ICON comes from."""
    d = _title(tmp_path, "MOO2", {},
               ["Play MOO2.bat\tMaster of Orion II\tORION95.EXE"])
    (open(os.path.join(d, "Play MOO2.bat"), "w")
     .write('@echo off\r\ncd /d "%~dp0DOSBOX"\r\nDOSBox.exe -conf "..\\moo.conf"\r\n'))
    plan = W.launch_plan(d)
    assert plan[0]["dosbox"] is True
    assert "orion95.exe" not in W.win32_launch_targets(plan)


# --------------------------------------------------------------------------
# 4. The import directory is data-directory entry ONE.
# --------------------------------------------------------------------------
def test_imports_come_from_directory_entry_1_not_0(tmp_path):
    """Entry 0 is the EXPORT table.  Reading it and calling the result
    "imports" made the tool report zero imports for AvP.exe and Thief2.exe and,
    for hl.exe, 114 fragments of raw x86 - a silently wrong answer rather than
    a failure."""
    p = build_pe(str(tmp_path / "g.exe"), ORDINARY, 0x1000,
                 imports=["kernel32.dll", "ddraw.dll", "dinput.dll"])
    assert set(W.read_image(p).imports) == {"kernel32.dll", "ddraw.dll", "dinput.dll"}


# --------------------------------------------------------------------------
# 5. install.reg and the 32-on-64 registry view.
# --------------------------------------------------------------------------
def test_hklm_software_keys_need_reg32_but_wow6432node_ones_do_not(tmp_path):
    """MEASURED 2026-09-01: `reg import Halo\\install.reg` with the default
    64-bit reg.exe put "DigitalProductID" where a 32-bit halo.exe cannot see
    it and the game stopped at its EULA; `reg import ... /reg:32` put it in the
    32-bit view and Halo reached its main menu fullscreen.
    WarcraftII/directplay-win64.reg is the control the other way - it names
    Wow6432Node itself, so it wants the 64-bit importer."""
    plain = tmp_path / "install.reg"
    plain.write_text("REGEDIT4\r\n\r\n"
                     "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Microsoft Games\\Halo]\r\n"
                     '"Version"="1.10"\r\n')
    r = W.analyse_reg(str(plain))
    assert r["hklm_software_keys"] == 1
    assert r["already_wow6432node"] == 0
    assert r["needs_reg32_on_win64"] is True

    wow = tmp_path / "directplay-win64.reg"
    wow.write_text("Windows Registry Editor Version 5.00\r\n\r\n"
                   "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\DirectPlay]\r\n"
                   '"Path"="dpwsockx.dll"\r\n')
    r = W.analyse_reg(str(wow))
    assert r["already_wow6432node"] == 1
    assert r["needs_reg32_on_win64"] is False


# --------------------------------------------------------------------------
# 6. The tool must never claim a pass it did not measure.
# --------------------------------------------------------------------------
def test_verdict_never_says_runs(tmp_path):
    """A static sweep cannot know that a game runs.  The good outcome is
    NO-BLOCKER-FOUND; every RUNS in docs/win10-compatibility.md has a
    screenshot behind it or is labelled as analysis."""
    d = _title(tmp_path, "Clean", {}, ["Play.bat\tClean\ticon.ico"])
    (open(os.path.join(d, "Play.bat"), "w")
     .write('@echo off\r\nstart "" "%~dp0game.exe"\r\n'))
    build_pe(os.path.join(d, "game.exe"), ORDINARY, 0x1000,
             imports=["kernel32.dll"])
    rep = W.scan_title(d)
    v, _why = W.verdict(rep)
    assert v == "NO-BLOCKER-FOUND"
    assert v != "RUNS"


def test_tool_runs_end_to_end_as_json(tmp_path, capsys):
    d = _title(tmp_path, "T", {}, ["Play.bat\tT\ticon.ico"])
    (open(os.path.join(d, "Play.bat"), "w")
     .write('@echo off\r\nstart "" "%~dp0game.exe"\r\n'))
    build_pe(os.path.join(d, "game.exe"), ORDINARY, 0x1000, imports=["kernel32.dll"])
    assert W.main(["--json", str(tmp_path)]) == 0
    out = json.loads(capsys.readouterr().out)
    assert len(out) == 1 and out[0]["title"] == "T"
    assert out[0]["pe32"] == 1 and out[0]["pe32+"] == 0
