"""The 3dfx Control Panel offers only settings OUR stack actually reads.

scripts/3dfx/3dfxctl/ctl_logic.h holds the panel's settings table: one row per
control, naming the value, where the panel writes it (the Glide registry key,
the user environment, or vcr-kmd's Diag key) and when the stack reads it.
Clean-room lane (vcr-kmd + our h5 Glide + our MesaFX ICD), 2026-09-28.

THE RULE: a control whose value the stack never reads is not a control - it
is this project's signature failure, a tool reporting success and being
believed. So every our-stack row is checked here against the source that
reads it, IN THE PLACE the panel writes it:

- a Glide-registry row must be read by our h5 Glide through hwcGetenv (the
  GETENV / GLIDE_GETENV / GLIDE_FGETENV macros), which alone looks at the
  registry - a name Glide reads with plain getenv() never sees the registry;
- an environment row must be read with getenv() by the ICD or by Glide; and a
  name both read (FX_GLIDE_SWAPINTERVAL, FX_GLIDE_SWAPPENDINGCOUNT) is in the
  environment BECAUSE the ICD puts its own default into the process
  environment when it finds none there, which shadows a Glide-registry value
  in every OpenGL game (fxapi.c fx_perf_defaults);
- a kernel row must be read by vcr-kmd's miniport per request / per new
  display surface - never only at boot - and be one of the four the panel's
  writer accepts;
- the Glide key the panel writes is the key getRegPath() computes;
- the names deliberately NOT offered stay out, and the source facts that keep
  them out are pinned so a change there brings the question back.

The checker is shown to FAIL on a name nobody reads and on a getenv-only name
filed under the registry. The Glide and ICD sources are the gitignored fork
clones under voodoo-cleanroom/build/ (the main tree's when a worktree has
none); without them those checks SKIP loudly - they do not pass.
"""
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
CTL = REPO / "scripts" / "3dfx" / "3dfxctl"
LOGIC = (CTL / "ctl_logic.h").read_text()
PANEL = (CTL / "3dfxctl.c").read_text()
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
MAIN_CR = Path("/home/voidsstr/development/retro-agent/voodoo-cleanroom")
CR = REPO / "voodoo-cleanroom"
GLIDE = next((p / "build/retro3dfx-glide/glide3x/h5" for p in (CR, MAIN_CR)
              if (p / "build/retro3dfx-glide/glide3x/h5").is_dir()), None)
ICD = next((p / "build/retro3dfx-gl/src/mesa/drivers/glide" for p in (CR, MAIN_CR)
            if (p / "build/retro3dfx-gl/src/mesa/drivers/glide").is_dir()), None)


def _strip_comments(text):
    """C comments blanked, string literals KEPT (a GETENV("NAME") is a string)."""
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
            i = j + 1
            continue
        else:
            i += 1
            continue
        for k in range(i, min(j, n)):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


def _src(root, globs):
    """{relative path: comment-stripped text} - latin-1: 3dfx's sources are not UTF-8,
    and grep calls gglide.c a binary file (a search that skips it misses grBufferSwap)."""
    out = {}
    for g in globs:
        for p in sorted(root.glob(g)):
            out[str(p.relative_to(root))] = _strip_comments(p.read_text(encoding="latin-1"))
    return out


def _glide():
    if GLIDE is None:
        pytest.skip("retro3dfx-glide clone absent (voodoo-cleanroom/build/) - the Glide reads of "
                    "the panel's settings were NOT checked; run build-stack.sh")
    return _src(GLIDE, ("glide3/src/*.c", "glide3/src/*.h", "minihwc/*.c", "minihwc/*.h"))


def _icd():
    if ICD is None:
        pytest.skip("retro3dfx-gl clone absent (voodoo-cleanroom/build/) - the ICD reads of the "
                    "panel's settings were NOT checked; run build-stack.sh")
    return _src(ICD, ("*.c", "*.h"))


# ---- the table --------------------------------------------------------------------------

ROW = re.compile(r'\{\s*CTL_ID_(\w+),\s*(CTL_LANE_\w+),\s*(CTL_TAB_\w+),\s*(CTL_ST_\w+),\s*'
                 r'"([^"]*)",\s*(CTL_K_\w+),\s*(CTL_WHEN_\w+),\s*([^,]+),')


def rows(lane=None):
    out = []
    for m in ROW.finditer(LOGIC):
        r = dict(id=m.group(1), lane=m.group(2), tab=m.group(3), store=m.group(4),
                 name=m.group(5), kind=m.group(6), when=m.group(7), flags=m.group(8))
        if lane is None or r["lane"] == lane:
            out.append(r)
    return out


def names(r):
    return [n for n in r["name"].split("|") if n]


def test_the_table_parses():
    ours = rows("CTL_LANE_VCR")
    assert len(ours) >= 17, "the row pattern no longer matches ctl_logic.h"
    assert {r["store"] for r in ours} == {"CTL_ST_GLIDE", "CTL_ST_ENV", "CTL_ST_DIAG",
                                          "CTL_ST_DISPLAY"}
    assert len(rows("CTL_LANE_VINTAGE")) >= 10


# ---- the readers ---------------------------------------------------------------------------

REG_READ = r'\b(?:GETENV|GLIDE_GETENV|GLIDE_FGETENV|GLIDE_34GETENV|hwcGetenv)\s*\(\s*"{}"'
ENV_READ = r'\bgetenv\s*\(\s*"{}"'


def glide_reads_via_registry(src, name):
    """[files] where Glide reads `name` through hwcGetenv (env, HKCU, HKLM)."""
    rx = re.compile(REG_READ.format(re.escape(name)))
    return [f for f, t in src.items() if rx.search(t)]


def reads_via_getenv(src, name):
    rx = re.compile(ENV_READ.format(re.escape(name)))
    return [f for f, t in src.items() if rx.search(t)]


def reader_of(store, name, glide, icd):
    """Where the stack reads a value the panel stores in `store`; [] = nowhere,
    i.e. a control that would do nothing."""
    if store == "CTL_ST_GLIDE":
        return glide_reads_via_registry(glide, name)
    if store == "CTL_ST_ENV":
        return (reads_via_getenv(icd, name) + reads_via_getenv(glide, name) +
                glide_reads_via_registry(glide, name))
    raise AssertionError(store)


def _raw(rel):
    """a Glide source file as it is (comments included): for finding boundaries"""
    _glide()
    return (GLIDE / rel).read_text(encoding="latin-1")


def _between(text, start, end):
    s = text.index(start)
    return text[s:text.index(end, s)]


def test_the_hwcgetenv_macros_are_what_reads_the_registry():
    glide = _glide()
    fx = glide["glide3/src/fxglide.h"]
    assert re.search(r"#define\s+GETENV\(a\)\s+hwcGetenv\(a\)", fx)
    mh = glide["minihwc/minihwc.c"]
    assert re.search(r"#define\s+GETENV\s+hwcGetenv", mh)
    body = _strip_comments(_between(_raw("minihwc/minihwc.c"), "hwcGetenv(const char *a)",
                                    "/* _grGetenv */"))
    # the order the panel's override warnings rely on: environment, HKCU, HKLM - text only
    env, hkcu, hklm = (body.index("getenv(a)"), body.index("HKEY_CURRENT_USER"),
                       body.index("HKEY_LOCAL_MACHINE"))
    assert env < hkcu < hklm
    assert "type != REG_SZ" in body


def test_the_glide_key_is_getregpath_s_key():
    _glide()
    reg = _between(_raw("minihwc/minihwc.c"), "getRegPath() ", "} /* getRegPath */")
    nt5 = _strip_comments(reg[reg.index("win2k, winxp"):])
    keys = re.findall(r'"(SYSTEM\\\\[^"]+)"', nt5)
    assert keys, "getRegPath's NT 5 keys not found"
    unesc = [k.replace("\\\\", "\\") for k in keys]
    probe, if_there, otherwise = unesc[0], unesc[1], unesc[2]

    def define(name):
        m = re.search(rf'#define\s+{name}\s+"([^"]+)"', LOGIC)
        return m.group(1).replace("\\\\", "\\")
    assert define("CTL_KEY_3DFXVS_DEV0") == probe
    assert define("CTL_KEY_GLIDE_3DFXVS") == if_there
    assert define("CTL_KEY_GLIDE_BANSHEE") == otherwise
    # and the panel decides it the same way: the probe key opened, then the choice
    detect = PANEL[PANEL.index("static void stack_detect"):]
    assert re.search(r"G\.has_3dfxvs_dev0 = reg_key_exists\(HKEY_LOCAL_MACHINE, "
                     r"CTL_KEY_3DFXVS_DEV0\);\s*G\.glide_key = ctl_glide_regpath"
                     r"\(G\.has_3dfxvs_dev0\);", detect)


@pytest.mark.parametrize("r", [r for r in rows("CTL_LANE_VCR") if r["store"] == "CTL_ST_GLIDE"],
                         ids=lambda r: r["id"])
def test_every_glide_registry_row_is_read_through_hwcgetenv(r):
    glide = _glide()
    for n in names(r):
        assert glide_reads_via_registry(glide, n), (
            f"{r['id']}: {n} is written to the Glide registry key but our Glide never reads it "
            f"through hwcGetenv - the control would do nothing")


@pytest.mark.parametrize("r", [r for r in rows("CTL_LANE_VCR") if r["store"] == "CTL_ST_ENV"],
                         ids=lambda r: r["id"])
def test_every_environment_row_is_read_from_the_environment(r):
    glide, icd = _glide(), _icd()
    for n in names(r):
        assert reader_of("CTL_ST_ENV", n, glide, icd), f"{r['id']}: nobody reads {n}"
        if "CTL_F_R_ICD" in r["flags"]:
            assert reads_via_getenv(icd, n) or re.search(
                rf'\{{\s*"{n}"\s*,', icd.get("fxapi.c", "")), \
                f"{r['id']}: flagged as read by the ICD, but the ICD never getenv()s {n}"
        # environment only where it has to be: a name Glide reads through the
        # registry too belongs in the Glide key UNLESS the ICD shadows it
        if glide_reads_via_registry(glide, n) and "CTL_F_R_ICD" not in r["flags"]:
            pytest.fail(f"{r['id']}: {n} is Glide-only and registry-capable - it belongs in the "
                        f"Glide key, not the user environment")


def test_the_icd_shadows_exactly_the_two_timing_values():
    """Why vsync and the frame queue are ENVIRONMENT rows: when the process
    environment has no value, the ICD _putenv()s its own default, and Glide's
    hwcGetenv looks at the environment before the registry - so in every
    OpenGL game a Glide-registry value would be shadowed."""
    icd = _icd()
    api = icd["fxapi.c"]
    block = api[api.index("fx_perf_defaults[][2]"):]
    block = block[:block.index("};")]
    shadowed = set(re.findall(r'\{\s*"(\w+)"\s*,', block))
    assert {"FX_GLIDE_SWAPINTERVAL", "FX_GLIDE_SWAPPENDINGCOUNT"} <= shadowed
    assert "_putenv(buf)" in api and "if (!getenv(fx_perf_defaults[i][0]))" in api
    env_rows = {n for r in rows("CTL_LANE_VCR") if r["store"] == "CTL_ST_ENV" for n in names(r)}
    for n in shadowed & {"FX_GLIDE_SWAPINTERVAL", "FX_GLIDE_SWAPPENDINGCOUNT"}:
        assert n in env_rows, f"{n} is shadowed by the ICD's default but not an environment row"


def test_the_sli_aa_value_reaches_glide_and_the_icd_through_the_registry():
    glide, icd = _glide(), _icd()
    assert re.search(r'h5SliAaConfigEnv\(GLIDE_GETENV\("SSTH3_SLI_AA_CONFIGURATION", 2L\)',
                     glide["glide3/src/gpci.c"])
    assert 'grGetRegistryOrEnvironmentStringExt("SSTH3_SLI_AA_CONFIGURATION")' in icd["fxapi.c"]
    entry = _strip_comments(_between(_raw("glide3/src/diget.c"),
                                     "GR_EXT_ENTRY(grGetRegistryOrEnvironmentString",
                                     "} /* grGetRegistryOrEnvironmentString */"))
    assert "retval = hwcGetenv(theEntry)" in entry
    aa = [r for r in rows("CTL_LANE_VCR") if r["name"] == "SSTH3_SLI_AA_CONFIGURATION"]
    assert len(aa) == 1 and aa[0]["store"] == "CTL_ST_GLIDE" and aa[0]["kind"] == "CTL_K_AA"


def test_the_checker_fails_a_control_that_does_nothing():
    """The pin itself: a name nobody reads, and a name Glide reads only with
    getenv() filed under the registry, must both come back empty."""
    glide, icd = _glide(), _icd()
    assert reader_of("CTL_ST_GLIDE", "FX_GLIDE_NOBODY_READS_THIS", glide, icd) == []
    assert reader_of("CTL_ST_ENV", "FX_GLIDE_NOBODY_READS_THIS", glide, icd) == []
    # FX_GLIDE_NO_PLUGIN is read with getenv() only: in the registry it would do nothing
    assert reads_via_getenv(glide, "FX_GLIDE_NO_PLUGIN")
    assert reader_of("CTL_ST_GLIDE", "FX_GLIDE_NO_PLUGIN", glide, icd) == []
    # FX_GAMMA is the ICD's own getenv - the Glide key would do nothing
    assert reader_of("CTL_ST_GLIDE", "FX_GAMMA", glide, icd) == []


# ---- the kernel switches --------------------------------------------------------------------

def _mp(name):
    return _strip_comments((KMD / "miniport" / name).read_text(encoding="latin-1"))


def _enclosing_function(text, pos):
    """The name of the C function whose body contains offset pos."""
    depth, start = 0, None
    for i in range(pos, -1, -1):
        c = text[i]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                start = i
                break
            depth -= 1
    head = text[max(0, start - 300):start]
    m = re.findall(r"(\w+)\s*\([^;{}]*\)\s*$", head)
    return m[-1] if m else None


@pytest.mark.parametrize("r", [r for r in rows("CTL_LANE_VCR") if r["store"] == "CTL_ST_DIAG"],
                         ids=lambda r: r["id"])
def test_every_kernel_row_is_read_without_a_reboot(r):
    n = r["name"]
    assert "CTL_F_R_KERNEL" in r["flags"] and r["when"] == "CTL_WHEN_NOW"
    found = []
    for f in ("vcrmp.c", "vcrmp_multi.c"):
        t = _mp(f)
        for m in re.finditer(rf'VcrDiagGet\(L"{n}"', t):
            found.append((f, _enclosing_function(t, m.start())))
    assert found, f"vcr-kmd never reads Diag\\{n}"
    for f, fn in found:
        # read per SLI/AA request, or while answering IOCTL_VCR_INFO - which the
        # display driver asks at every new display surface. FindAdapter (boot) is not.
        assert fn in ("sli_aa_allowed", "fill_info"), \
            f"Diag\\{n} is read in {f}:{fn}, not per request / per surface"
        assert fn != "VcrFindAdapter"


def test_the_2d_switches_are_read_by_the_info_fill_and_at_every_new_surface():
    t = _mp("vcrmp.c")
    i = t.index("v->flags =")
    fn = _enclosing_function(t, i)
    blk = t[i:t.index(";", i)]
    for n in ("Accel2DText", "Accel2DPattern", "Accel2DLine"):
        assert f'VcrDiagGet(L"{n}"' in blk, f"{n} not read with the INFO flags"
    # ... and that fill runs for IOCTL_VCR_INFO
    assert re.search(rf"case IOCTL_VCR_INFO:[\s\S]{{0,400}}\b{fn}\s*\(", t), \
        f"{fn} is not what IOCTL_VCR_INFO answers with"
    dd = _strip_comments((KMD / "display" / "vcrdd.c").read_text(encoding="latin-1"))
    surf = dd[dd.index("HSURF APIENTRY DrvEnableSurface"):]
    surf = surf[:surf.index("VOID APIENTRY DrvDisableSurface")]
    assert "VcrDd2dInit(pd);" in surf, "the display driver no longer re-reads at a new surface"
    d2 = _strip_comments((KMD / "display" / "vcrdd_2d.c").read_text(encoding="latin-1"))
    init = d2[d2.index("void VcrDd2dInit(VCR_PDEV *pd)"):]
    init = init[:init.index("IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES")]
    for flag in ("VCR_INFO_F_TEXT2D", "VCR_INFO_F_PAT2D", "VCR_INFO_F_LINE2D"):
        assert flag in init


def test_the_aa_kill_switch_is_read_per_request_and_absent_means_off():
    t = _mp("vcrmp_multi.c")
    fn = t[t.index("static int sli_aa_allowed(void)"):]
    fn = fn[:fn.index("}") + 1]
    assert 'return VcrDiagGet(L"SliAA", 0) != 0;' in fn


# ---- deliberately NOT offered, and why (pinned so a change brings it back up) --------------

def _table_names():
    return {n for r in rows() for n in names(r)}


def test_boot_time_and_diagnostic_kernel_switches_are_not_offered():
    ours = {n for r in rows("CTL_LANE_VCR") if r["store"] == "CTL_ST_DIAG" for n in names(r)}
    assert ours == {"Accel2DText", "Accel2DPattern", "Accel2DLine"}
    t = _mp("vcrmp.c")
    fa = t[t.index("x->allow_poke = VcrDiagGet"):]
    fa = fa[:fa.index("hwinfo(x);")]
    # read once, at FindAdapter: a reboot each - not a no-reboot control
    for n in ("AllowPoke", "Accel2D", "D3D", "TexPortFlush", "D3D32", "Reset3D"):
        assert f'VcrDiagGet(L"{n}"' in fa, n
        assert n not in _table_names()


def test_names_that_would_do_nothing_or_harm_are_not_offered():
    glide = _glide()
    table = _table_names()
    gtex, gpci, mh = (glide["glide3/src/gtex.c"], glide["glide3/src/gpci.c"],
                      glide["minihwc/minihwc.c"])
    # FX_GLIDE_LOD_BIAS only offsets a game's OWN grTexLodBiasValue() call:
    # for a game that never makes one it changes nothing
    assert "FX_GLIDE_LOD_BIAS" not in table
    uses = [m.start() for m in re.finditer(r"environment\.lodBias", gtex)]
    assert uses and all(gtex.rfind("GR_ENTRY(grTexLodBiasValue", 0, u) >
                        gtex.rfind("GR_ENTRY(", 0, gtex.rfind("GR_ENTRY(grTexLodBiasValue", 0, u))
                        for u in uses)
    # SSTH3_GRXCLOCK / MEMCLOCK: read, but the PLL programming is compiled out
    # of the Windows build (HWC_ACCESS_DDRAW=1)
    assert "SSTH3_GRXCLOCK" not in table and "SSTH3_MEMCLOCK" not in table
    mk = (GLIDE / "glide3/src/Makefile.mingw").read_text(encoding="latin-1")
    assert "-DHWC_ACCESS_DDRAW=1" in mk
    clk = mh.index('if (GETENV("SSTH3_GRXCLOCK") || GETENV("SSTH3_MEMCLOCK"))')
    assert mh.rfind("#if !defined(HWC_ACCESS_DDRAW)", 0, clk) > mh.rfind("#endif", 0, clk)
    # FX_GLIDE_ANALOG_SLI: forced to 1 on a 4-way board
    assert "FX_GLIDE_ANALOG_SLI" not in table
    assert "return numChips >= 4 ? 1 : analogSli;" in glide["minihwc/h5sliaa.h"]
    # FX_GLIDE_NO_SPLASH: on Win32 grSplash draws only through the plugin, which
    # FX_GLIDE_NO_PLUGIN (offered) switches off entirely
    assert "FX_GLIDE_NO_SPLASH" not in table
    assert "FX_GLIDE_NO_PLUGIN" in table
    # hidden AA overrides: detected and offered for removal, never written
    for n in ("FX_GLIDE_AA_SAMPLE", "FX_GLIDE_NUM_CHIPS"):
        assert n not in table
        assert f'"{n}"' in PANEL                    # the override scan knows them
    assert re.search(r'if \(GETENV\("FX_GLIDE_AA_SAMPLE"\)\)\s*_GlideRoot\.environment\.aaSample',
                     gpci)
    # the SLI band height changes the request the kernel programs; never measured
    assert "FX_GLIDE_SLI_BAND_HEIGHT" not in table
    # the AA toggle hotkeys switch AA live, past every gate
    for n in ("FX_GLIDE_AA_TOGGLE_KEY", "FX_GLIDE_TAA_TOGGLE_KEY", "FX_GLIDE_SCREENSHOT_KEY"):
        assert n not in table


def test_the_vintage_rows_name_values_the_vintage_source_knows():
    """Carried over from the first 3dfxctl unchanged. A light pin only - that
    each name exists somewhere in the vintage H5 tree - when that repo is here."""
    tree = REPO.parent / "retro-3dfx" / "3dfx Driver Code"
    if not tree.is_dir():
        tree = Path("/home/voidsstr/development/retro-3dfx/3dfx Driver Code")
    if not tree.is_dir():
        pytest.skip("retro-3dfx checkout absent - the vintage names were NOT checked")
    want = {n for r in rows("CTL_LANE_VINTAGE") for n in names(r)}
    found = set()
    # the W2K display driver spells its SSTH3_ names as RK_PREFIX"..." (REGKEYS.H)
    regkeys = tree / "H5/W2K/Src/Video/Displays/H5/REGKEYS.H"
    rk = regkeys.read_bytes() if regkeys.is_file() else b""
    prefixed = b'#define RK_PREFIX                          "SSTH3_"' in rk
    for p in tree.rglob("*"):
        if p.suffix.upper() not in (".C", ".H", ".INF", ".CPP") or not p.is_file():
            continue
        data = p.read_bytes()
        for n in want - found:
            if n.encode() in data or (prefixed and n.startswith("SSTH3_") and
                                      f'RK_PREFIX"{n[6:]}"'.encode() in data):
                found.add(n)
        if found == want:
            break
    assert want - found == set(), f"not in the vintage source: {sorted(want - found)}"
