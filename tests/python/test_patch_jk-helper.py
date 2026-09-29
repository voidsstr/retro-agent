"""Jedi Knight DF2 / Mysteries of the Sith - JKMODE.EXE (provisioning/patches/jk-helper/).

The Sith engine stores its display mode as an INDEX into a DirectDraw mode list
it enumerates itself (64 entries at most), with b3DAccel and two device GUIDs,
in HKLM. JKMODE.EXE repeats that enumeration and writes the index of the box's
resolution; it must REFUSE where the game cannot select the mode (.240: the
X800 offers 1920x1080x16 at position 76). Verified findings:
.claude/evidence-1080p/_results/titles-verified.json "JediKnightDF2"/"JediKnightMotS".

The decision logic lives in jklogic.h and is compiled NATIVELY here (the same
header jkmode.c includes), fed synthetic devices and the four boxes' MEASURED
enumeration sequences (fixtures/ddenum_*_df2_sequence.txt). The expected
indices are what the GAME'S OWN CODE stores for those sequences - its
callbacks, its 64 cap, MotS's high-resolution 8-bpp filter and its qsort with
its comparator, run under emulation (emu_modelist.py). The game SORTS the list
before it indexes it, so an enumeration position is never the answer.
Share checks SKIP LOUDLY when the library is not mounted; the Windows build
SKIPS LOUDLY without mingw; the live emulation cross-check SKIPS LOUDLY without
unicorn/pefile.
"""
import hashlib
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
PATCH = os.path.abspath(os.path.join(HERE, "..", "..", "provisioning", "patches", "jk-helper"))
LIBRARY = "/mnt/retro-share/Files/Games-Library"

_spec = importlib.util.spec_from_file_location("jk_helper_apply", os.path.join(PATCH, "apply.py"))
jk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(jk)

# --------------------------------------------------------------------------- native probe
PROBE_C = r"""
#include <stdio.h>
#include <string.h>
#include "jklogic.h"
static jk_dev devs[16]; static int n = 0, tw = 0, th = 0, filt = 0;
static void hexguid(const char *s, jk_guid *g) {
    int i; unsigned v;
    for (i = 0; i < 16; i++) { sscanf(s + 2 * i, "%2x", &v); g->b[i] = (unsigned char)v; }
}
static void put32(unsigned char *d, unsigned off, unsigned v) {
    d[off] = v & 255; d[off+1] = (v >> 8) & 255; d[off+2] = (v >> 16) & 255; d[off+3] = v >> 24;
}
static void desc(unsigned char *d, unsigned cm, unsigned tex, unsigned shade, unsigned blend, unsigned rbd, unsigned zbd) {
    memset(d, 0, 0xcc); put32(d, 0, 0xcc); put32(d, 8, cm); put32(d, 0x80, shade);
    put32(d, 0x84, tex); put32(d, 0x8c, blend); put32(d, 0x9c, rbd); put32(d, 0xa0, zbd);
}
int main(void) {
    char cmd[32], g[64];
    while (scanf("%31s", cmd) == 1) {
        if (!strcmp(cmd, "target")) { scanf("%d %d", &tw, &th); }
        else if (!strcmp(cmd, "filter")) { scanf("%d", &filt); }
        else if (!strcmp(cmd, "sort")) { jk_sort_modes(&devs[n - 1]); }
        else if (!strcmp(cmd, "unknown")) { printf("unknown %d\n", jk_modes_unknown_bpp(&devs[n - 1])); }
        else if (!strcmp(cmd, "dump")) {
            int i; jk_dev *d = &devs[n - 1];
            for (i = 0; i < d->nmodes; i++)
                printf("m %s%dx%dx%d\n", d->modes[i].modex ? "X" : "", d->modes[i].w, d->modes[i].h, d->modes[i].bpp);
        } else if (!strcmp(cmd, "cmp")) {
            jk_mode a, b; scanf("%d %d %d %d %d %d %d %d", &a.modex, &a.w, &a.h, &a.bpp, &b.modex, &b.w, &b.h, &b.bpp);
            printf("cmp %d\n", jk_mode_cmp(&a, &b));
        }
        else if (!strcmp(cmd, "dev")) {
            jk_dev *d = &devs[n++]; memset(d, 0, sizeof(*d)); d->target_seen_at = -1;
            scanf("%d %d %d %63s", &d->primary, &d->has_guid, &d->is3d, g); hexguid(g, &d->guid);
        } else if (!strcmp(cmd, "mode")) {
            int pass, w, h, pitch; unsigned pf, bits; scanf("%d %d %d %d %x %u", &pass, &w, &h, &pitch, &pf, &bits);
            jk_mode_offer(&devs[n - 1], pass, w, h, pitch, pf, bits, tw, th, filt);
        } else if (!strcmp(cmd, "d3d")) {
            unsigned a[6], b[6]; unsigned char hw[0xcc], hel[0xcc]; jk_d3d *r;
            scanf("%x %x %x %x %x %x %63s %x %x %x %x %x %x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], g,
                  &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
            desc(hw, a[0], a[1], a[2], a[3], a[4], a[5]); desc(hel, b[0], b[1], b[2], b[3], b[4], b[5]);
            r = jk_d3d_add(&devs[n - 1]);
            if (!r) { printf("d3d full\n"); continue; }
            memset(r, 0, sizeof(*r)); hexguid(g, &r->guid); jk_d3d_from_desc(r, hw, hel);
            printf("d3d usable=%d addon=%d\n", jk_d3d_usable(r), jk_d3d_usable_addon(r));
        } else if (!strcmp(cmd, "pick")) {
            int di = -1, ti = -1;
            if (jk_pick_3d(devs, n, &di, &ti)) printf("pick %d %d\n", di, ti); else printf("pick none\n");
        } else if (!strcmp(cmd, "find")) {
            int d; scanf("%d", &d);
            printf("find %d %d %d %d\n", jk_find_mode(&devs[d], tw, th), devs[d].nmodes, devs[d].seen, devs[d].target_seen_at);
        } else if (!strcmp(cmd, "default")) { printf("default %d\n", jk_default_device(devs, n)); }
        else if (!strcmp(cmd, "stale")) {
            int rd; unsigned m; jk_guid gg; scanf("%d %63s %u", &rd, g, &m); hexguid(g, &gg);
            printf("stale %d\n", jk_stored_mode_is_stale(devs, n, rd, &gg, m));
        } else if (!strcmp(cmd, "resolve")) {
            int rd; jk_guid gg; scanf("%d %63s", &rd, g); hexguid(g, &gg);
            printf("resolve %d\n", jk_resolve_device(devs, n, rd, &gg));
        } else if (!strcmp(cmd, "bpp")) {
            unsigned pf, bits; scanf("%x %u", &pf, &bits); printf("bpp %d\n", jk_mode_bpp(pf, bits));
        }
    }
    return 0;
}
"""

ZERO = "00" * 16
HAL = "e03de684aa46cf11816f0000c020156e"      # IID_IDirect3DHALDevice, little-endian bytes
RGB = "60055ea45d3dd011a5de00a0c9053a0c"      # an emulation device GUID (any distinct value)
V2GUID = "11111111222233334444555555555555"
WINDOW = "009fa692fa13d11197c000a024293005"
# a HAL description: RGB colour model; perspective+alpha textures; 16 and 32 bpp render; 16-bit z
HAL_OK = "2 5 0 0 500 400"
HEL_RICH = "2 5 0 0 500 400"
EMPTY = "0 0 0 0 0 0"


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("SKIPPED LOUDLY: no host C compiler - jklogic.h not exercised")
    d = tmp_path_factory.mktemp("jkprobe")
    src = d / "probe.c"
    src.write_text(PROBE_C)
    exe = d / "probe"
    subprocess.run([cc, "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-result",
                    "-I", PATCH, "-o", str(exe), str(src)], check=True)

    def run(script):
        return subprocess.run([str(exe)], input=script, capture_output=True, text=True, check=True).stdout.split("\n")
    return run


def fixture_modes(box):
    """(pass1 [ModeX] entries, pass2 entries) as (w, h, pf, bits) from a measured sequence."""
    p1, p2 = [], []
    for line in open(os.path.join(PATCH, "fixtures", "ddenum_%s_df2_sequence.txt" % box)):
        m = re.match(r"\s*(\d+)\s+(X?)\s+(\d+)x(\d+)\s+(\d+)bpp pf=0x([0-9a-f]+)", line)
        assert m, line
        w, h, bpp, pf = int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6), 16)
        (p1 if m.group(2) else p2).append((w, h, pf, bpp))
    return p1, p2


def pitch_of(w, bpp, model="natural"):
    """The lPitch a runtime reports for a mode: width * bytes per pixel (what
    XP SP3's ddraw.dll computes - 0x73791fad: imul, shr 3, no alignment), or
    that rounded up to 32 bytes. MotS's filter tests it, so both are covered."""
    p = w * bpp // 8
    return (p + 31) & ~31 if model == "aligned32" else p


def enum_script(p1, p2, w=1920, h=1080, filt=0, pitch="natural", sort=True, extra=()):
    s = ["target %d %d" % (w, h), "filter %d" % filt, "dev 1 0 1 %s" % ZERO]
    s += ["mode 1 %d %d %d %x %d" % (m[0], m[1], pitch_of(m[0], m[3], pitch), m[2], m[3]) for m in p1]
    s += ["mode 2 %d %d %d %x %d" % (m[0], m[1], pitch_of(m[0], m[3], pitch), m[2], m[3]) for m in p2]
    s += (["sort"] if sort else []) + list(extra) + ["find 0"]
    return "\n".join(s) + "\n"


def mots_pass1(p2):
    """MotS's first pass runs at coop 0x11, so it sees the NORMAL list's
    320-wide modes, not the ModeX ones the DF2 fixture recorded under 0x51."""
    return [m for m in p2 if m[0] == 320]


def parse_find(out):
    line = [x for x in out if x.startswith("find")][0].split()
    return tuple(int(v) for v in line[1:])   # index, nmodes, seen, seen_at


# --------------------------------------------------------------------------- the mode list
def test_bpp_is_what_the_game_stores(probe):
    out = probe("bpp 60 8\nbpp 40 16\nbpp 40 32\nbpp 0 0\nbpp 20 0\n")
    assert out[:5] == ["bpp 8", "bpp 16", "bpp 32", "bpp -1", "bpp 8"]


@pytest.mark.parametrize("box", ["123", "145", "195", "240"])
def test_df2_index_is_what_the_game_code_stores_on_each_box(probe, box):
    """DF2 = pass 1 (coop 0x51, ALLOWMODEX) + pass 2, one counter, cap 64, then
    the game's qsort. The enumeration position (ddenum's 45/34/58) is NOT it."""
    p1, p2 = fixture_modes(box)
    idx, nmodes, seen, seen_at = parse_find(probe(enum_script(p1, p2)))
    want = jk.GAME_CODE_1080P["df2"][box]
    assert seen == len(p1) + len(p2)
    assert nmodes == min(64, seen)
    if want is None:
        assert idx == -1, "the game cannot select 1080p on .%s - JKMODE must refuse" % box
        assert seen_at == 76 and seen_at >= 64
    else:
        assert idx == want and idx < 64
        assert idx != seen_at, "the sorted index must not be confused with the enumeration position"


@pytest.mark.parametrize("pitch", ["natural", "aligned32"])
@pytest.mark.parametrize("box", ["123", "145", "195", "240"])
def test_mots_index_is_what_the_game_code_stores_on_each_box(probe, box, pitch):
    """MotS: coop 0x11 twice, its high-res 8-bpp filter (which depends on the
    lPitch the runtime reports), the cap, the qsort."""
    _, p2 = fixture_modes(box)
    idx, nmodes, _, seen_at = parse_find(probe(enum_script(mots_pass1(p2), p2, filt=1, pitch=pitch)))
    want = jk.GAME_CODE_1080P["mots" if pitch == "natural" else "mots_aligned32"][box]
    if want is None:
        assert idx == -1 and nmodes == 64 and seen_at >= 64   # seen on hardware: list ends at 1280x1024 16bpp
    else:
        assert idx == want


def test_the_builders_first_indices_would_have_picked_32bpp_or_run_past_the_end(probe):
    """Regression guard for the defect this review found: the enumeration
    positions (45 on .123 for DF2, 43 for MotS) are not what the game indexes."""
    p1, p2 = fixture_modes("123")
    out = probe(enum_script(p1, p2, extra=["dump"]))
    lst = [l.split()[1] for l in out if l.startswith("m ")]
    assert lst[45] == "1680x1050x32" and lst[31] == "1920x1080x16"
    out = probe(enum_script(mots_pass1(p2), p2, filt=1, extra=["dump"]))
    lst = [l.split()[1] for l in out if l.startswith("m ")]
    assert len(lst) == 42 < 44, "MotS keeps 42 modes on .123: an index of 43 reads past the end"


def test_cap_is_exactly_64_entries_in_enumeration_order(probe):
    filler = [(640 + i, 480, 0x40, 32) for i in range(63)]
    idx, n, seen, at = parse_find(probe(enum_script([], filler + [(1920, 1080, 0x40, 16)])))
    assert (n, at) == (64, 63)                                # the 64th enumerated is kept...
    assert idx == 0                                           # ...and the sort puts the lone 16bpp mode first
    idx, n, seen, at = parse_find(probe(enum_script([], filler + [(1, 1, 0x40, 8), (1920, 1080, 0x40, 16)])))
    assert (idx, n, seen, at) == (-1, 64, 65, 64)             # the 65th is dropped, whatever its sort key


def test_pass_one_keeps_only_320_wide_and_never_matches_a_modex_entry(probe):
    idx, n, seen, _ = parse_find(probe(enum_script([(1920, 1080, 0x40, 16), (320, 200, 0x40, 16)],
                                                   [(1920, 1080, 0x40, 16)], 1920, 1080)))
    assert (n, seen, idx) == (2, 2, 1)
    idx, n, _, _ = parse_find(probe(enum_script([(320, 240, 0x40, 16)], [(320, 240, 0x40, 16)], 320, 240)))
    assert (n, idx) == (2, 0), "same width: the comparator puts the ModeX copy AFTER the ordinary 320x240"


def test_mots_filter_drops_only_hires_modes_whose_pitch_equals_their_width(probe):
    """MotS 0x428fa1: cmp width,0x578 (1400) / jl keep / cmp width,lPitch / jne keep."""
    s = "\n".join(["target 1920 1080", "filter 1", "dev 1 0 1 %s" % ZERO,
                    "mode 2 1920 1080 1920 60 8",    # 8 bpp, pitch == width: dropped, not counted
                    "mode 2 1920 1080 3840 40 16",   # 16 bpp: pitch 2*width, kept
                    "mode 2 1400 1050 1400 60 8",    # exactly 1400: dropped
                    "mode 2 1399 1050 1399 60 8",    # below 1400: kept
                    "mode 2 1680 1050 1696 60 8",    # a padded pitch: kept - the test is on the value
                    "sort", "find 0"]) + "\n"
    idx, n, seen, at = parse_find(probe(s))
    assert (n, seen) == (3, 3)
    assert at == 0, "a dropped mode takes no slot: the 16bpp mode is enumerated FIRST"
    idx_df2, n_df2, _, _ = parse_find(probe(s.replace("filter 1", "filter 0")))
    assert n_df2 == 5, "DF2 has no such filter (0x425e6e)"


def test_comparator_is_the_games(probe):
    out = probe("cmp 1 320 200 8 0 640 480 8\n"      # ModeX vs other width: ModeX first
                "cmp 1 320 200 8 0 320 200 32\n"     # ModeX vs same width: ModeX LAST, whatever the bpp
                "cmp 0 320 200 32 1 320 200 8\n"
                "cmp 0 640 480 8 1 320 200 8\n"
                "cmp 0 1920 1080 8 0 640 480 16\n"   # bpp first
                "cmp 0 1280 1024 16 0 1280 720 16\n" # then width, then height
                "cmp 0 800 600 16 0 1024 768 16\n")
    got = [int(l.split()[1]) for l in out if l.startswith("cmp")]
    assert got[0] == -1 and got[1] == 1 and got[2] == -1 and got[3] == 1
    assert got[4] < 0 and got[5] > 0 and got[6] < 0


# The sorted order the GAME'S OWN CODE produces on .240's measured sequence
# (emu_modelist.py). This is the case where the comparator is not a strict weak
# order - ModeX and ordinary 320-wide modes in one list - so only the same qsort
# algorithm reproduces it; a correct-but-different sort would not.
GAME_CODE_240 = [
    "320x200x8", "320x240x8", "X320x200x8", "X320x240x8", "X320x200x16", "X320x240x16",
    "X320x200x32", "X320x240x32", "400x300x8", "512x384x8", "640x400x8", "640x432x8",
    "640x480x8", "720x480x8", "720x576x8", "800x480x8", "800x600x8", "848x480x8",
    "1024x768x8", "1152x864x8", "1280x720x8", "1280x768x8", "1280x800x8", "1280x960x8",
    "1280x1024x8", "1360x768x8", "320x200x16", "320x240x16", "400x300x16", "512x384x16",
    "640x400x16", "640x432x16", "640x480x16", "720x480x16", "720x576x16", "800x480x16",
    "800x600x16", "848x480x16", "1024x768x16", "1152x864x16", "1280x720x16", "1280x768x16",
    "1280x800x16", "1280x960x16", "1280x1024x16", "320x200x32", "320x240x32", "400x300x32",
    "512x384x32", "640x400x32", "640x432x32", "640x480x32", "720x480x32", "720x576x32",
    "800x480x32", "800x600x32", "848x480x32", "1024x768x32", "1152x864x32", "1280x720x32",
    "1280x768x32", "1280x800x32", "1280x960x32", "1280x1024x32",
]


@pytest.mark.parametrize("game", ["df2", "mots"])
def test_qsort_port_reproduces_the_game_order_on_240(probe, game):
    p1, p2 = fixture_modes("240")
    if game == "mots":
        p1 = mots_pass1(p2)
    out = probe(enum_script(p1, p2, filt=int(game == "mots"), extra=["dump"]))
    lst = [l.split()[1] for l in out if l.startswith("m ")]
    assert lst == GAME_CODE_240


def test_unknown_bpp_is_counted_so_the_helper_can_refuse(probe):
    out = probe("target 1920 1080\nfilter 0\ndev 1 0 1 %s\nmode 2 640 480 1280 0 16\n"
                "mode 2 1920 1080 3840 40 16\nunknown\n" % ZERO)
    assert "unknown 1" in out


def test_only_16_bpp_matches(probe):
    idx, _, _, _ = parse_find(probe(enum_script([], [(1920, 1080, 0x60, 8), (1920, 1080, 0x40, 32)])))
    assert idx == -1


# --------------------------------------------------------------------------- the 3D device
def test_d3d_record_usable_only_for_a_real_16bpp_rgb_hal(probe):
    s = "target 1 1\ndev 1 0 1 %s\n" % ZERO
    cases = [
        (HAL_OK + " " + HAL + " " + EMPTY, "usable=1"),                       # HAL
        (EMPTY + " " + RGB + " " + HEL_RICH, "usable=0"),                     # emulation: HW colour model 0
        ("2 5 0 0 100 400 %s %s" % (HAL, EMPTY), "usable=0"),                 # no DDBD_16 render
        ("1 5 0 0 500 400 %s %s" % (HAL, EMPTY), "usable=0"),                 # MONO only
        ("2 5 0 0 500 0 %s %s" % (HAL, EMPTY), "usable=0"),                   # no z-buffer
        ("2 4 0 0 500 400 %s %s" % (HAL, EMPTY), "usable=0"),                 # no perspective
    ]
    out = probe(s + "".join("d3d %s\n" % c for c, _ in cases[:4]) + "dev 1 0 1 %s\n" % ZERO
                + "".join("d3d %s\n" % c for c, _ in cases[4:]))
    got = [l.split()[1] for l in out if l.startswith("d3d")]
    assert got == [w for _, w in cases]


def test_addon_test_wants_alpha_or_the_shade_bits(probe):
    out = probe("target 1 1\ndev 1 0 1 %s\n" % ZERO
                + "d3d 2 1 0 0 400 400 %s %s\n" % (HAL, EMPTY)           # persp only: usable, not addon
                + "d3d 2 1 2000 0 400 400 %s %s\n" % (HAL, EMPTY)        # r14: shade 0x2000 without 0x1000
                + "d3d 2 1 3000 0 400 400 %s %s\n" % (HAL, EMPTY)        # 0x1000 set too: r14 off
                + "d3d 2 1 4000 8 400 400 %s %s\n" % (HAL, EMPTY))       # r18: blend 8 + shade 0x4000
    got = [l for l in out if l.startswith("d3d")]
    assert got == ["d3d usable=1 addon=0", "d3d usable=1 addon=1", "d3d usable=1 addon=0", "d3d usable=1 addon=1"]


def test_at_most_four_3d_devices(probe):
    out = probe("target 1 1\ndev 1 0 1 %s\n" % ZERO + ("d3d %s %s %s\n" % (EMPTY, RGB, HEL_RICH)) * 4
                + "d3d %s %s %s\npick\n" % (HAL_OK, HAL, EMPTY))
    assert "d3d full" in out
    assert "pick none" in out, "a HAL enumerated fifth is invisible to the game"


def test_pick_is_the_primary_hal_behind_the_emulators(probe):
    out = probe("target 1 1\ndev 1 0 1 %s\n" % ZERO
                + "d3d %s %s %s\n" % (EMPTY, RGB, HEL_RICH) * 2 + "d3d %s %s %s\npick\n" % (HAL_OK, HAL, EMPTY))
    assert "pick 0 2" in out


def test_pick_prefers_an_addon_3d_card(probe):
    base = "target 1 1\ndev 1 0 1 %s\nd3d %s %s %s\n" % (ZERO, HAL_OK, HAL, EMPTY)
    out = probe(base + "dev 0 1 1 %s\nd3d %s %s %s\npick\n" % (V2GUID, HAL_OK, HAL, EMPTY))
    assert "pick 1 0" in out, "loop 1: a secondary DDCAPS_3D device with a GUID wins"
    out = probe(base + "dev 0 1 1 %s\nd3d 2 1 0 0 400 400 %s %s\npick\n" % (V2GUID, HAL, EMPTY))
    assert "pick 0 0" in out, "an add-on failing the extra test loses to the first usable device"
    out = probe("target 1 1\ndev 1 0 0 %s\nd3d %s %s %s\n" % (ZERO, EMPTY, RGB, HEL_RICH)
                + "dev 0 1 1 %s\nd3d 2 1 0 0 400 400 %s %s\npick\n" % (V2GUID, HAL, EMPTY))
    assert "pick 1 0" in out, "loop 2 still finds it when nothing else is usable"


def test_no_usable_3d_device_means_refuse(probe):
    out = probe("target 1 1\ndev 1 0 0 %s\nd3d %s %s %s\npick\n" % (ZERO, EMPTY, RGB, HEL_RICH))
    assert "pick none" in out


# --------------------------------------------------------------------------- stale index
def test_default_device_and_resolution(probe):
    out = probe("target 1 1\ndev 1 0 1 %s\ndev 0 1 1 %s\ndefault\n" % (ZERO, V2GUID)
                + "resolve 1 %s\nresolve 1 %s\nresolve 0 %s\nresolve 1 %s\nresolve 1 %s\n"
                % (V2GUID, ZERO, V2GUID, "ab" * 16, WINDOW))
    assert out[:6] == ["default 1", "resolve 2", "resolve 1", "resolve 1", "resolve 1", "resolve 0"]
    out = probe("target 1 1\ndev 0 1 1 %s\ndev 1 0 0 %s\ndefault\n" % (V2GUID, ZERO))
    assert out[0] == "default 2", "the primary outscores a secondary even without DDCAPS_3D"


def test_stale_index_detected_only_past_the_end(probe):
    s = "target 1 1\ndev 1 0 1 %s\n" % ZERO + "mode 2 640 480 1280 40 16\n" * 10
    out = probe(s + "stale 1 %s 9\nstale 1 %s 10\nstale 0 %s 50\nstale 1 %s 99\n" % (ZERO, ZERO, ZERO, WINDOW))
    assert out[:4] == ["stale 0", "stale 1", "stale 1", "stale 0"]


# --------------------------------------------------------------------------- source <-> table
def read(p):
    return open(os.path.join(PATCH, p)).read()


def test_helper_uses_each_games_own_first_pass_flags_and_key():
    src = read("jkmode.c")
    for title, t in jk.TITLES.items():
        coop = "0x51" if t["game"] == "df2" else "0x11"
        filt = 0 if t["game"] == "df2" else 1          # MotS 0x428f9e has the high-res 8-bpp filter, DF2 not
        key_c = t["regkey"].replace("\\", "\\\\")
        assert re.search(r'\{\s*"%s",[^}]*"%s",\s*%s,\s*%d\s*\}' % (t["game"], re.escape(key_c), coop, filt),
                         src, re.S), title
    hexes = {row[2] for row in jk.JK_OFFSETS} | {row[2] for row in jk.MOTS_OFFSETS}
    assert "6a51" in hexes and "6a11" in hexes


def test_caps_in_the_header_match_the_asserted_instructions():
    h = read("jklogic.h")
    assert re.search(r"#define JK_MODE_CAP\s+64\b", h) and "83ff40" in {r[2] for r in jk.JK_OFFSETS}
    assert re.search(r"#define JK_DEV_CAP\s+16\b", h) and "83f810" in {r[2] for r in jk.JK_OFFSETS}
    assert re.search(r"#define JK_D3D_CAP\s+4\b", h) and "83f904" in {r[2] for r in jk.JK_OFFSETS}
    iid = [r[2] for r in jk.JK_OFFSETS if r[3].startswith("IID_IDirect3D")][0]
    assert iid == "8000ba3b2124cf11a31a00aa00b93356"
    assert "0x3bba0080, 0x2421, 0x11cf" in read("jkmode.c")


def test_helper_writes_reg_binary_like_the_game():
    src = read("jkmode.c")
    assert "RegSetValueExA(k, name, 0, REG_BINARY" in src
    assert re.search(r'reg_put\(key, "displayMode", &mode, 4', src)
    assert re.search(r'reg_put\(key, "b3DAccel", &one, 4', src)
    assert re.search(r'reg_put\(key, "displayDeviceGUID", pd->guid\.b, 16', src)


def test_helper_sorts_like_the_game_and_feeds_the_filter_the_real_pitch():
    src = read("jkmode.c")
    body = src[src.index("static int enum_device"):src.index("/* ---- registry")]
    assert body.index("SetCooperativeLevel(dd, g_hwnd, 8)") < body.index("jk_sort_modes(d)") \
        < body.index("QueryInterface(dd, &IID_D3D1"), "sort after the passes, as the game does before copying"
    assert "(int)jk_u32(p, 0x10)" in src, "lPitch (DDSURFACEDESC +0x10) must reach the MotS filter"
    assert "g_game->hires8_filter" in src
    assert "jk_modes_unknown_bpp(pd)" in src and "rc = 7" in src
    offs = {r[0] for r in jk.JK_OFFSETS} | {r[0] for r in jk.MOTS_OFFSETS}
    assert {0x422797, 0x4249d0, 0x4258da, 0x427b00, 0x428f9e, 0x425e6e} <= offs


def test_per_box_off_switch_is_read_only_and_checked_first():
    """JkMode=0 under HKLM\\Software\\RetroAgent must stop JKMODE before it loads
    ddraw or opens the game's key for writing - a box whose card lists 1080p16
    but cannot run the game's 3D there needs a way out that no launch undoes."""
    src = read("jkmode.c")
    body = src[src.index("static int run(void)"):]
    sw = body.index('"JkMode"')
    assert sw < body.index('LoadLibraryA("ddraw.dll")') < body.index("RegCreateKeyExA(")
    blk = body[body.index("the per-box off switch"):body.index("ddraw = LoadLibraryA")]
    assert '"JkMode"' in blk
    assert "KEY_QUERY_VALUE" in blk and "RegSetValue" not in blk and "RegCreateKey" not in blk
    assert "t == REG_DWORD" in blk and "v == 0" in blk and "return 0;" in blk


def test_helper_refuses_on_a_bad_version_before_writing():
    src = read("jkmode.c")
    assert src.index("version_ok(key") < src.index('reg_put(key, "displayDeviceGUID"')
    assert 'lstrcmpA(buf, "0.1")' in src


# --------------------------------------------------------------------------- apply.py pure
def synthetic_pe(payload_at=0x10, payload=b"\x6a\x51"):
    b = bytearray(0x400)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 0x40)
    b[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HH", b, 0x44, 0x14C, 1)
    struct.pack_into("<H", b, 0x54, 0xE0)
    struct.pack_into("<H", b, 0x58, 0x10B)
    struct.pack_into("<I", b, 0x58 + 28, 0x400000)
    sec = 0x58 + 0xE0
    b[sec:sec + 5] = b".text"
    struct.pack_into("<IIII", b, sec + 8, 0x200, 0x1000, 0x200, 0x200)
    b[0x200 + payload_at:0x200 + payload_at + len(payload)] = payload
    return bytes(b)


def test_verify_offsets_maps_va_and_checks_bytes():
    data = synthetic_pe()
    assert jk.va_to_off(data, 0x401010) == 0x210
    assert jk.verify_offsets(data, [(0x401010, 0x210, "6a51", "x")]) == []
    assert jk.verify_offsets(data, [(0x401010, 0x210, "6a11", "x")])            # wrong bytes
    assert jk.verify_offsets(data, [(0x401010, 0x211, "5100", "x")])            # wrong offset in the table
    assert jk.verify_offsets(data, [(0x409000, 0x210, "6a51", "x")])            # VA outside every section


def test_offset_tables_are_self_consistent():
    for table in (jk.JK_OFFSETS, jk.MOTS_OFFSETS):
        for va, off, hexb, why in table:
            assert len(bytes.fromhex(hexb)) > 0 and why
            assert va > 0x400000 and off < va - 0x400000 + 0x1000


@pytest.mark.parametrize("title", sorted(jk.TITLES))
def test_generated_launcher_obeys_the_house_rules(title):
    assert jk.launcher_problems(title) == []
    t = jk.TITLES[title]
    txt = jk.launcher_text(title)
    assert '"%%~dp0JKMODE.EXE" %s %%FR_W%% %%FR_H%% > "%%~dp0JKMODE.LOG" 2>&1' % t["game"] in txt
    assert txt.rstrip("\r\n").endswith("exit")
    assert jk.launch_txt_line(title) == "%s\t%s\t%s" % (t["launcher"], t["display"], t["exe"])


def test_launcher_problem_detector_catches_parentheses(monkeypatch):
    monkeypatch.setitem(jk.TITLES["JediKnightDF2"], "launcher", "Play Jedi Knight (3D).bat")
    assert any("parenthesis" in p for p in jk.launcher_problems("JediKnightDF2"))


# --------------------------------------------------------------------------- the Windows build
def test_build_is_xp_safe_and_deterministic():
    if not shutil.which("i686-w64-mingw32-gcc") or not shutil.which("objdump"):
        pytest.skip("SKIPPED LOUDLY: no i686-w64-mingw32-gcc/objdump - JKMODE.EXE not built")
    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        for d in (a, b):
            subprocess.run(["bash", os.path.join(PATCH, "build.sh"), d], check=True, stdout=subprocess.DEVNULL)
        ea, eb = os.path.join(a, "JKMODE.EXE"), os.path.join(b, "JKMODE.EXE")
        assert jk.helper_problems(ea) == []
        dlls, rsrc = jk.pe_imports_objdump(ea)
        assert dlls == {"KERNEL32.DLL", "USER32.DLL", "ADVAPI32.DLL"} and not rsrc
        assert hashlib.md5(open(ea, "rb").read()).hexdigest() == hashlib.md5(open(eb, "rb").read()).hexdigest()
        dis = subprocess.run(["objdump", "-d", ea], capture_output=True, text=True, check=True).stdout
        assert not re.search(r"\b(xmm\d|cmov\w*|fcomi)\b", dis), "Pentium-safe: no SSE, no cmov"
        strings = open(ea, "rb").read()
        assert b"ddraw.dll" in strings and b"DirectDrawEnumerateA" in strings and b"DirectDrawCreate" in strings


# --------------------------------------------------------------------------- the share
def _library():
    if not os.path.isdir(LIBRARY):
        pytest.skip("SKIPPED LOUDLY: %s is not mounted - staged JK.EXE/JKM.EXE not checked" % LIBRARY)
    return LIBRARY


@pytest.mark.parametrize("title", sorted(jk.TITLES))
def test_staged_original_is_the_binary_the_helper_was_written_against(title):
    lib = _library()
    t = jk.TITLES[title]
    tree = jk.find_ci(lib, title)
    assert tree, "%s not found (case-insensitive)" % title
    exe = jk.find_ci(tree, t["exe"])
    assert exe, "%s not found (case-insensitive)" % t["exe"]
    assert jk.check_exe(open(exe, "rb").read(), t) == []


def test_check_mode_passes_on_the_share():
    _library()
    assert jk.cmd_check(LIBRARY) == 0


# --------------------------------------------------------------------------- the game's own code
def _emu():
    try:
        import unicorn  # noqa: F401
        import pefile   # noqa: F401
    except ImportError:
        pytest.skip("SKIPPED LOUDLY: unicorn/pefile not installed - jklogic.h not cross-checked against the "
                    "game's own code (pip install unicorn pefile in a venv to run it)")
    _library()
    spec = importlib.util.spec_from_file_location("jk_emu", os.path.join(PATCH, "emu_modelist.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


@pytest.mark.parametrize("game", ["df2", "mots"])
@pytest.mark.parametrize("box", ["123", "145", "195", "240"])
def test_port_matches_the_game_code_run_under_emulation(probe, game, box):
    emu = _emu()
    p1, p2 = fixture_modes(box)
    if game == "mots":
        p1 = mots_pass1(p2)
    for pitch in ("natural", "aligned32"):
        _, got = emu.run(game, p1, p2, pitch, library=LIBRARY)
        want = ["%s%dx%dx%d" % ("X" if mx else "", w, h, b) for (mx, w, h, b) in got]
        out = probe(enum_script(p1, p2, filt=int(game == "mots"), pitch=pitch, extra=["dump"]))
        assert [l.split()[1] for l in out if l.startswith("m ")] == want, (game, box, pitch)


# --------------------------------------------------------------------------- --publish, never against the share
def _fake_publish_setup(tmp_path, monkeypatch, df2_share_copy=None, mots_share_copy=None):
    """A fake library and a fake sharewrite.py that only LOGS its argv - so the
    publish logic is exercised with no share, no NAS and no vault."""
    lib, out = tmp_path / "lib", tmp_path / "out"
    out.mkdir()
    helper = out / jk.HELPER
    helper.write_bytes(b"MZ new JKMODE build")
    new_md5 = hashlib.md5(helper.read_bytes()).hexdigest()
    outputs = []
    for title, share_copy in (("JediKnightDF2", df2_share_copy), ("JediKnightMotS", mots_share_copy)):
        t = jk.TITLES[title]
        tree = lib / title
        tree.mkdir(parents=True)
        (tree / t["exe"]).write_bytes(b"not the real " + t["exe"].encode())
        monkeypatch.setitem(t, "md5", hashlib.md5((tree / t["exe"]).read_bytes()).hexdigest())
        if share_copy is not None:
            (tree / jk.HELPER).write_bytes(share_copy)
        outputs.append({"share_path": "%s/%s/%s" % (jk.SHARE_LIB_REL, title, jk.HELPER),
                        "local_path": str(helper), "md5": new_md5, "size": helper.stat().st_size})
    (out / "manifest.json").write_text(__import__("json").dumps({"outputs": outputs}))
    log = tmp_path / "sharewrite-calls.txt"
    fake = tmp_path / "fake_sharewrite.py"
    fake.write_text("import os, sys\n"
                    "open(%r, 'a').write(' '.join(sys.argv[1:]) + '\\n')\n"
                    "n = sum(1 for _ in open(%r))\n"
                    "sys.exit(1 if os.environ.get('FAKE_SW_FAIL_ON') == str(n) else 0)\n" % (str(log), str(log)))
    monkeypatch.setattr(jk, "SHAREWRITE", str(fake))
    return lib, out, log, new_md5


def _calls(log):
    return [l.split() for l in open(log)] if os.path.exists(log) else []


def test_publish_puts_each_new_file_once_and_backs_up_nothing_that_did_not_exist(tmp_path, monkeypatch):
    lib, out, log, _ = _fake_publish_setup(tmp_path, monkeypatch)
    assert jk.cmd_publish(str(out), str(lib), dry=False) == 0
    calls = _calls(log)
    assert [c[0] for c in calls] == ["put", "put"]
    assert [c[2] for c in calls] == ["Files/Games-Library/JediKnightDF2/JKMODE.EXE",
                                     "Files/Games-Library/JediKnightMotS/JKMODE.EXE"]


def test_publish_backs_up_a_different_share_copy_first_and_skips_a_current_one(tmp_path, monkeypatch):
    lib, out, log, _ = _fake_publish_setup(tmp_path, monkeypatch, df2_share_copy=b"MZ OLD build",
                                           mots_share_copy=b"MZ new JKMODE build")
    assert jk.cmd_publish(str(out), str(lib), dry=False) == 0
    calls = _calls(log)
    assert len(calls) == 2, calls                              # MotS already current: skipped
    backup, put = calls
    assert backup[0] == "put" and backup[2] == "Files/Games-Library/_patches/JediKnightDF2/%s/JKMODE.EXE" % jk.BACKUP_STAMP
    assert put[2] == "Files/Games-Library/JediKnightDF2/JKMODE.EXE"


def test_publish_stops_at_the_first_failure(tmp_path, monkeypatch):
    lib, out, log, _ = _fake_publish_setup(tmp_path, monkeypatch, df2_share_copy=b"MZ OLD build")
    monkeypatch.setenv("FAKE_SW_FAIL_ON", "1")                 # the backup put fails
    assert jk.cmd_publish(str(out), str(lib), dry=False) == 1
    assert len(_calls(log)) == 1, "nothing may be replaced once its backup failed"


def test_publish_refuses_when_the_staged_game_is_not_the_binary_it_was_written_for(tmp_path, monkeypatch):
    lib, out, log, _ = _fake_publish_setup(tmp_path, monkeypatch)
    (lib / "JediKnightDF2" / "JK.EXE").write_bytes(b"a different JK.EXE")
    assert jk.cmd_publish(str(out), str(lib), dry=False) == 1
    assert _calls(log) == []


def test_publish_dry_run_passes_dry_run_to_every_sharewrite_call(tmp_path, monkeypatch):
    lib, out, log, _ = _fake_publish_setup(tmp_path, monkeypatch, df2_share_copy=b"MZ OLD build")
    assert jk.cmd_publish(str(out), str(lib), dry=True) == 0
    calls = _calls(log)
    assert calls and all(c[-1] == "--dry-run" for c in calls)
