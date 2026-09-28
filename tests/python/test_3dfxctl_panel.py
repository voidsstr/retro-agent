"""The 3dfx Control Panel (scripts/3dfx/3dfxctl) is safe on a 1998 CRT and a
V5 6000 whose anti-aliasing can freeze the PC, and it loads on Windows XP SP3.

Clean-room lane (vcr-kmd + our h5 Glide + our MesaFX ICD), 2026-09-28.

MONITOR. Every display-mode change the panel makes goes through
voodoo-cleanroom/vcr-kmd/tools/vcr_pace.h (at least 3 s between switches,
box-wide; a refused switch is not made), only to modes the driver enumerates,
never while the driver's list is unfiltered by the monitor, and never while a
Glide program holds the display. The 2D switches need a NEW display surface,
and a same-mode ChangeDisplaySettingsEx(CDS_RESET) is not one - measured on
XP SP3 with vcr-kmd in the QEMU test bed (2026-09-28): the recorder showed
only DrvAssertMode(FALSE/TRUE) and no DrvEnableSurface, so the switches were
never read. The panel therefore passes through another listed mode of the
same resolution and back (two paced switches; the recorder then shows
DrvEnablePDEV + DrvEnableSurface at the original mode). CDS_RESET is banned.

KERNEL. Only SliAA and the three Accel2D switches are ever written to
Services\\vcrmp\\Diag, a VALUE is deleted, never a key (that key also holds the
driver's phase history and every safety kill switch), and the writer itself
refuses any other Diag name.

AA. Diag\\SliAA = 1 is written only by the AA plan (which needs the person's
explicit confirmation - ctl_aa_make_plan, tests/native/test_3dfxctl_logic.c);
the confirmation's default button is No; no preset touches it; "Disarm AA
now" deletes it.

XP. The built exe imports only from kernel32, user32, gdi32, advapi32,
comctl32 and msvcrt, and EVERY imported name is in the export table of the
real XP SP3 DLL (scripts/3dfx/3dfxctl/xp_exports/, extracted from the I386
CABs); uxtheme is loaded at run time, never imported; the subsystem version is
below 6.0 (XP's loader refuses 6.0 before one instruction runs); the comctl32
v6 manifest and the icon are resources, and the icon's images are DIBs (a
PNG-compressed icon entry is Vista+).
"""
import re
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

pefile = pytest.importorskip("pefile")

REPO = Path(__file__).resolve().parents[2]
CTL = REPO / "scripts" / "3dfx" / "3dfxctl"
SRC = (CTL / "3dfxctl.c").read_text()
LOGIC = (CTL / "ctl_logic.h").read_text()
MAKEFILE = (CTL / "Makefile").read_text()
EXE = CTL / "3dfxctl.exe"
XP = CTL / "xp_exports"
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
XCC = shutil.which("i686-w64-mingw32-gcc")
XRC = shutil.which("i686-w64-mingw32-windres")

ALLOWED_DLLS = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "comctl32.dll",
                "msvcrt.dll"}


def _blank(text, strings=True):
    """comments (and string literals, unless strings=False) blanked, offsets kept"""
    out, i, n = list(text), 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif text[i] in "\"'":
            j = i + 1
            while j < n and text[j] != text[i]:
                j += 2 if text[j] == "\\" else 1
            j += 1
            if not strings:
                i = j
                continue
        else:
            i += 1
            continue
        for k in range(i, min(j, n)):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


def _functions(code):
    out, depth, start = {}, 0, 0
    for i, ch in enumerate(code):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                head = re.split(r"\n\s*\n|[;}]", code[max(0, start - 400):start])[-1]
                m = re.search(r"(\w+)\s*\(", head)
                if m:
                    out[m.group(1)] = (start, i + 1)
    return out


def _body(name, strings=True):
    s, e = _functions(_blank(SRC))[name]
    return _blank(SRC, strings)[s:e]


CODE = _blank(SRC)
CODE_S = _blank(SRC, strings=False)             # comments gone, strings kept

SWITCH = r"ChangeDisplaySettings(?:Ex)?[AW]?"


# ---- the monitor ----------------------------------------------------------------------------

def test_the_panel_includes_the_gate_and_is_rebuilt_when_it_changes():
    assert '#include "vcr_pace.h"' in SRC
    deps = re.search(r"^DEPS\s*=\s*((?:.*\\\n)*.*)$", MAKEFILE, re.M).group(1)
    for h in ("vcr_pace.h", "vcr_ioctl.h", "ctl_logic.h"):
        assert h in deps, f"the Makefile does not rebuild the panel when {h} changes"
    assert re.search(r"^3dfxctl\.exe:\s*\$\(DEPS\)", MAKEFILE, re.M)


def test_every_display_switch_goes_through_the_pace_gate():
    funcs = _functions(CODE)
    sites = [(m.start(), m.group(0)) for m in re.finditer(rf"\b{SWITCH}\s*\(", CODE)]
    assert sites, "the switch pattern no longer matches the panel"
    for pos, api in sites:
        fname = next(f for f, (s, e) in funcs.items() if s <= pos < e)
        s, e = funcs[fname]
        body = CODE[s:e]
        at = pos - s
        prev = [m.end() for m in re.finditer(rf"\b{SWITCH}\s*\(", body) if m.end() <= at]
        before = body[(prev[-1] if prev else 0):at]
        nxt = [m.start() for m in re.finditer(rf"\b{SWITCH}\s*\(", body) if m.start() > at]
        after = body[at:(nxt[0] if nxt else len(body))]
        assert re.search(r"\bvcr_pace_before_switch\s*\(", before), \
            f"{fname}: {api} is not preceded by vcr_pace_before_switch()"
        assert re.search(r"\bvcr_pace_after_(?:switch|restore)\s*\(", after), \
            f"{fname}: {api} is not followed by vcr_pace_after_switch/_restore()"
    # every switch is the gate's helper, and nowhere else
    assert {next(f for f, (s, e) in funcs.items() if s <= p < e) for p, _ in sites} == \
        {"paced_switch"}
    assert len(sites) == 1


def test_a_refused_switch_is_not_made():
    p = _body("paced_switch")
    gate = re.search(r"if \(!vcr_pace_before_switch\(\)\)\s*return 0;", p)
    assert gate and gate.end() < p.index("ChangeDisplaySettingsExA(")
    # a temporary mode is recorded AS held (the exit hold / crash filter pace
    # XP's revert), a lasting one as given back - and both whether or not the
    # driver accepted the change
    assert re.search(r"if \(flags & CDS_FULLSCREEN\)\s*vcr_pace_after_switch\(\);\s*else\s*"
                     r"vcr_pace_after_restore\(\);\s*return 1;", p)
    w = _body("disp_worker")
    assert "ChangeDisplaySettings" not in w
    # a refusal of the FIRST switch: nothing switched, said, the end
    assert re.search(r"if \(!j->paced\) \{[^}]*PostMessageA\(g_main, WM_APP_DISPDONE, 0, "
                     r"\(LPARAM\)j\);\s*return 0;", w)
    # the way back from the temporary mode: only after it was made, and a
    # refusal takes NO other road (the exit hold paces XP's revert)
    assert re.search(r"if \(j->paced && j->cds == DISP_CHANGE_SUCCESSFUL\) \{\s*"
                     r"j->paced2 = paced_switch\(&j->dm, 0, &j->cds2\);", w)
    for bad in ("RestoreDisplayMode", "ExitProcess", "TerminateProcess", "vcr_pace_before",
                "vcr_pace_after"):
        assert bad not in w, bad
    assert "paced_switch(&j->dm, CDS_UPDATEREGISTRY, &j->cds)" in w
    assert "paced_switch(&j->alt, CDS_FULLSCREEN, &j->cds)" in w


def test_no_same_mode_reset_and_no_other_switching_api():
    # CDS_RESET at the same mode does not build a new display surface on XP
    # (QEMU test bed, 2026-09-28) - the 2D switches would never be read
    assert "CDS_RESET" not in CODE
    for bad in ("SetDisplayMode", "RestoreDisplayMode", "DirectDrawCreate", "Direct3DCreate",
                "grSstWinOpen", "SetExclusiveMode"):
        assert bad not in CODE, bad


def test_modes_come_only_from_the_driver_list_and_never_blind():
    a = _body("apply_all")
    # a new refresh: the monitor filter, then the driver's own list, then the job
    ref = a.index("g_job.want_refresh = 1;")
    assert a.index("if (mon_unfiltered())") < a.index("ctl_mode_listed(G.modes, G.nmodes") < ref
    # the 2D pass-through mode: chosen from the list, refused when unfiltered
    assert re.search(r"if \(mon_unfiltered\(\) \|\| !ctl_bounce_mode\(G\.modes, G\.nmodes, "
                     r"&cur, &alt\)\)", a)
    # no display change while a Glide program owns the display
    job = a.index("if ((g_job.want_refresh || g_job.want_bounce) && G.have_dd && G.dd[1])")
    assert "g_job.want_refresh = g_job.want_bounce = 0;" in a[job:job + 600]
    # the list is EnumDisplaySettings' own
    d = _body("stack_detect")
    assert re.search(r"EnumDisplaySettingsA\(NULL, i, &dm\)", d)
    # the Glide override offers the driver's rates only, none when unfiltered
    b = _body("build_row")
    assert "ctl_glide_rates(G.modes, G.nmodes" in b and "if (mon_unfiltered())" in b
    assert re.search(r"if \(G\.have_cur && !mon_unfiltered\(\)\)\s*n = ctl_desk_rates", b)
    m = _body("mon_unfiltered")
    assert "!G.info.mon_filter" in m


def test_a_changed_refresh_is_put_back_unless_kept():
    k = _body("KeepProc")
    assert "g_countdown = 15;" in k and "EndDialog(h, IDCANCEL);" in k
    assert "BS_DEFPUSHBUTTON" in _blank(SRC, False)[_functions(CODE)["KeepProc"][0]:
                                                     _functions(CODE)["KeepProc"][1]]
    d = _body("on_disp_done")
    assert re.search(r"if \(keep != IDOK\) \{", d) and "nj.revert = 1;" in d


# ---- the kernel ---------------------------------------------------------------------------

DANGEROUS = ("AllowPoke", "SliPersistAll", "SliAAVendorRecipe", "SliAAReadback", "D3D32",
             "Reset3D", "FlipDeadline", "Disable", "DebugPort", "EdidFilter", "MonTrustEnvelope",
             "MonReset", "MaxBootAttempts", "BootAttempts", "HwCursor", "TexPortFlush", "Ddc",
             "Accel2D", "D3D", "Sli", "Sli6kClock", "LogLevel", "DesktopOffset")


def test_no_dangerous_diag_name_is_ever_written():
    lits = set(re.findall(r'"((?:[^"\\]|\\.)*)"', CODE_S))
    for n in DANGEROUS:
        assert n not in lits, f"the panel carries the Diag name {n} as a string"
    # the table's Diag rows are the three 2D switches, and the only other Diag
    # name the panel writes is SliAA
    diag_rows = re.findall(r'CTL_ST_DIAG,\s*"(\w+)"', LOGIC)
    assert sorted(diag_rows) == ["Accel2DLine", "Accel2DPattern", "Accel2DText"]
    assert re.search(r'static const char \*const ok\[\] = \{ "SliAA", "Accel2DText", '
                     r'"Accel2DPattern", "Accel2DLine" \};', LOGIC)
    # the writer refuses anything else under the Diag key, before any write
    w = _body("write_value", strings=False)
    guard = w.index("!ctl_diag_name_ok(name)")
    assert guard < w.index("reg_del_value") and guard < w.index("reg_set_dword")
    assert "strcmp(path, CTL_KEY_DIAG) == 0" in w


def test_a_value_is_deleted_never_a_key():
    for bad in ("RegDeleteKey", "SHDeleteKey", "RegDeleteTree", "SHDeleteValue"):
        assert bad not in CODE, bad
    d = _body("reg_del_value")
    assert "RegDeleteValueA(h, name)" in d


def test_the_panel_never_writes_the_display_class_key():
    # v56k_bench's old trap: SSTH3_SLI_AA_CONFIGURATION written under the display
    # class key, which our Glide never reads (vcr-kmd README "Registry trap")
    assert "4D36E968" not in SRC.upper()


# ---- anti-aliasing --------------------------------------------------------------------------

def test_the_aa_switch_is_armed_only_by_the_confirmed_aa_plan():
    writes = [m.start() for m in re.finditer(r'write_value\([^;]*"SliAA"', CODE_S, re.S)]
    assert len(writes) == 2, "SliAA is written from somewhere new"
    armed = [p for p in writes if 'plan.sliaa > 0 ? "1" : NULL' in CODE_S[p:p + 200]]
    disarm = [p for p in writes if re.match(r'write_value\(HKEY_LOCAL_MACHINE, CTL_KEY_DIAG, '
                                            r'"SliAA", NULL,', CODE_S[p:p + 120])]
    assert len(armed) == 1 and len(disarm) == 1
    a = _body("apply_all", strings=False)
    plan = a.index("ctl_aa_make_plan(G.nchips")
    assert "g_aa_confirmed" in a[plan:plan + 160]
    assert a.index("if (plan.refused)") < a.index('"SliAA"')


def test_the_confirmation_is_explicit_and_defaults_to_no():
    c = _body("aa_confirm", strings=False)
    assert "MB_DEFBUTTON2" in c and "return r == IDYES;" in c
    assert "FROZE THE WHOLE PC" in c and "ghost" in c
    # every "confirmed" follows a yes from aa_confirm, in the same block
    for m in re.finditer(r"g_aa_confirmed = 1;", CODE):
        seg = CODE[max(0, m.start() - 300):m.start()]
        assert "aa_confirm(" in seg, "g_aa_confirmed set without asking"
    # and a new load forgets it: every session asks again
    assert re.search(r"g_aa_confirmed = 0;", _body("rows_load"))
    # AA modes are listed only once experimental modes are allowed
    assert re.search(r"ctl_aa_choices\(G\.nchips, g_allow_exp,", _body("aa_fill_combo"))


def test_presets_never_touch_anti_aliasing_or_the_kernel():
    p = _body("preset_apply", strings=False)
    assert "SliAA" not in p and "CTL_KEY_DIAG" not in p
    assert re.search(r"if \(aa && aa->pend_present && ctl_aa_is_aa\(atol\(aa->pend\)\)\)\s*"
                     r"set_pend\(aa, NULL\);", p)


def test_environment_writes_are_announced_to_explorer():
    a = _body("apply_all", strings=False)
    i = a.index("if (env_changed) {")
    assert re.search(r'SendMessageTimeoutA\(HWND_BROADCAST, WM_SETTINGCHANGE, 0,\s*'
                     r'\(LPARAM\)"Environment"', a[i:i + 300])


def test_every_write_is_read_back():
    w = _body("write_value")
    assert w.count("reg_get_") >= 3, "a write is not read back"
    assert "the read-back differs" in _body("write_value", strings=False)
    assert "still there after the delete" in _body("write_value", strings=False)


# ---- Windows XP SP3 -------------------------------------------------------------------------

def _xp(dll):
    f = XP / f"{dll}.txt"
    return {ln.strip() for ln in f.read_text().splitlines() if ln.strip() and not ln.startswith("#")}


def test_the_xp_tables_are_xp_s():
    """The tables would pass anything if they were a modern Windows'."""
    assert "GetTickCount64" not in _xp("kernel32.dll")          # Vista
    assert "TaskDialog" not in _xp("comctl32.dll")              # Vista
    assert "InitCommonControlsEx" in _xp("comctl32.dll")
    assert "EnableThemeDialogTexture" in _xp("uxtheme.dll")
    assert "ChangeDisplaySettingsExA" in _xp("user32.dll")
    assert "_vsnprintf" in _xp("msvcrt.dll")
    for dll in ALLOWED_DLLS | {"uxtheme.dll"}:
        assert "XP SP3" in (XP / f"{dll}.txt").read_text().splitlines()[0]


def check_pe(path):
    """[problems] that would stop XP SP3 loading or theming the panel"""
    problems = []
    pe = pefile.PE(str(path))
    oh = pe.OPTIONAL_HEADER
    if oh.Subsystem != 2:
        problems.append(f"subsystem {oh.Subsystem}, want 2 (GUI)")
    if (oh.MajorSubsystemVersion, oh.MinorSubsystemVersion) > (5, 1):
        problems.append(f"subsystem version {oh.MajorSubsystemVersion}."
                        f"{oh.MinorSubsystemVersion} - XP's loader refuses 6.0+")
    if (oh.MajorOperatingSystemVersion, oh.MinorOperatingSystemVersion) > (5, 1):
        problems.append("OS version above XP")
    n = 0
    for imp in pe.DIRECTORY_ENTRY_IMPORT:
        dll = imp.dll.decode().lower()
        if dll not in ALLOWED_DLLS:
            problems.append(f"imports from {dll}")
            continue
        table = _xp(dll)
        for s in imp.imports:
            n += 1
            if s.name is None:
                problems.append(f"{dll} by ordinal {s.ordinal}")
            elif s.name.decode() not in table:
                problems.append(f"{dll}!{s.name.decode()} is not exported by XP SP3")
    if n < 50:
        problems.append(f"only {n} imports - not the panel?")
    data = path.read_bytes()
    if b"uxtheme.dll" not in data.lower():
        problems.append("uxtheme.dll is not loaded at run time")
    # resources: the v6 manifest and the icon
    res = {}
    if hasattr(pe, "DIRECTORY_ENTRY_RESOURCE"):
        for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            res[t.id] = t
    man = res.get(24)
    if not man:
        problems.append("no RT_MANIFEST")
    else:
        e = man.directory.entries[0]
        if e.id != 1:
            problems.append(f"manifest id {e.id}, want 1 (CREATEPROCESS_MANIFEST_RESOURCE_ID)")
        d = e.directory.entries[0].data.struct
        text = pe.get_data(d.OffsetToData, d.Size).decode("utf-8", "replace")
        if "Microsoft.Windows.Common-Controls" not in text or 'version="6.0.0.0"' not in text:
            problems.append("the manifest does not ask for comctl32 6.0")
    if 14 not in res:
        problems.append("no RT_GROUP_ICON")
    ver = re.search(r'#define CTL_VERSION\s+"([^"]+)"', SRC).group(1)
    if ver.encode() not in data:
        problems.append(f"the exe does not carry CTL_VERSION {ver}")
    return problems


def test_the_committed_exe_loads_on_xp_sp3():
    assert EXE.exists(), "3dfxctl.exe is not committed"
    assert check_pe(EXE) == []


def test_the_icon_is_readable_by_xp():
    ico = (CTL / "3dfxctl.ico").read_bytes()
    reserved, typ, count = struct.unpack_from("<HHH", ico)
    assert (reserved, typ) == (0, 1) and count == 3
    sizes = set()
    for i in range(count):
        w, h, _, _, _, bpp, size, off = struct.unpack_from("<BBBBHHII", ico, 6 + 16 * i)
        sizes.add(w or 256)
        assert ico[off:off + 4] != b"\x89PNG", "a PNG-compressed icon entry - Vista and later only"
        assert struct.unpack_from("<I", ico, off)[0] == 40, "not a BITMAPINFOHEADER"
        assert bpp == 32
    assert sizes == {16, 32, 48}


def test_a_fresh_build_loads_on_xp_sp3(tmp_path):
    if not (XCC and XRC):
        pytest.skip("i686-w64-mingw32-gcc/windres absent - the panel was NOT built")
    cflags = re.search(r"^CFLAGS\s*=\s*((?:.*\\\n)*.*)$", MAKEFILE, re.M).group(1)
    ldflags = re.search(r"^LDFLAGS\s*=\s*(.*)$", MAKEFILE, re.M).group(1)
    inc = f"-I{CTL} -I{KMD / 'include'} -I{KMD / 'tools'}"
    cflags = cflags.replace("\\\n", " ").replace("$(INC)", inc).split()
    res = tmp_path / "res.o"
    subprocess.run([XRC, "-O", "coff", "3dfxctl.rc", "-o", str(res)], cwd=CTL, check=True,
                   capture_output=True)
    out = tmp_path / "3dfxctl.exe"
    r = subprocess.run([XCC, *cflags, "-o", str(out), str(CTL / "3dfxctl.c"), str(res),
                        *ldflags.split()], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-2000:]
    assert "warning" not in r.stderr, r.stderr[-2000:]
    assert check_pe(out) == []


def test_the_native_suite_runs_every_logic_test():
    t = (REPO / "tests" / "native" / "test_3dfxctl_logic.c").read_text()
    assert '#include "../../scripts/3dfx/3dfxctl/ctl_logic.h"' in t
    defined = set(re.findall(r"^TEST\((\w+)\)", t, re.M))
    ran = set(re.findall(r"RUN\((\w+)\);", t))
    assert defined and defined == ran
