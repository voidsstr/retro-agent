"""The per-target refresh has ONE implementation, and both writers call it.

WHY THIS EXISTS. Two programs write the id Tech 3 `fleetres.cfg` and Serious
Sam's `Game_startup.ini`: the title's launcher (FLEETRES.EXE -> FLEETRES.BAT ->
`echo seta r_displayRefresh ...`) at every start, and the agent's GAMERES pass
(agent/shared/gameres.h) at every sync. Each rewrites the file whenever one of
its settings is missing, so if they compute the refresh differently - by one
line, by one Hz - each undoes the other forever and the "0 value(s) changed"
signal that catches real faults is dead (CLAUDE.md, "A file BOTH writers touch
must get the SAME number from each").

That is why the shared cfgs used to carry only FR_HZ, the persisted desktop's
own rate: it was the one number both could reproduce. The per-target rates
(FR_HZW / FR_HZ43 / FR_HZQ2 / FR_HZQ3 = %HZW% %HZ43% %HZQ2% %HZQ3%) are the
highest refresh the monitor supports AT EACH TITLE'S OWN RESOLUTION, and they
can be shared only because FLEETRES.EXE no longer ports the rule: it compiles
agent/shared/gameres.h and calls the same gr_target_hz() over a mode list built
the same way. These assertions pin exactly that - the function, its inputs, and
the order the list is built in - so a later edit cannot quietly fork it.

Nothing here needs the share or a box.
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FLEETRES = os.path.join(REPO, "provisioning", "fleetres", "fleetres.c")
GAMERES_H = os.path.join(REPO, "agent", "shared", "gameres.h")
GAMERES_C = os.path.join(REPO, "agent", "src", "gameres.c")


def _src(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def _strip_comments(src):
    return re.sub(r"/\*.*?\*/", "", src, flags=re.S)


def test_fleetres_compiles_the_agents_header_not_a_port():
    src = _strip_comments(_src(FLEETRES))
    assert '#include "../../agent/shared/gameres.h"' in src, (
        "FLEETRES.EXE must compile agent/shared/gameres.h - a port of "
        "gr_target_hz() is a second decision")
    assert "gr_target_hz(" in src


def test_each_published_rate_comes_from_gr_target_hz_at_its_own_resolution():
    """FR_HZW is the rate AT FR_W x FR_H, FR_HZ43 at FR_W43 x FR_H43, and the
    index engines' at the table entry they actually render (q3mode 7 is
    1152x864 on the 1080p boxes, not the 1280x960 4:3 target)."""
    src = _strip_comments(_src(FLEETRES))
    want = {
        "hzw": r"hzw\s*=\s*gr_target_hz\(&gp,\s*&rl,\s*tgt_w,\s*tgt_h,",
        "hz43": r"hz43\s*=\s*gr_target_hz\(&gp,\s*&rl,\s*t43_w,\s*t43_h,",
        "hzq2": r"hzq2\s*=\s*gr_target_hz\(&gp,\s*&rl,\s*q2tab\[q2_mode_for\(t43_w,\s*t43_h\)\]\.w,",
        "hzq3": r"hzq3\s*=\s*gr_target_hz\(&gp,\s*&rl,\s*q3tab\[q3_mode_for\(t43_w,\s*t43_h\)\]\.w,",
    }
    for var, rx in want.items():
        assert re.search(rx, src), "%s is not gr_target_hz() at its own resolution" % var
    # the patched id Tech 2 exes render at gr_q2wide_res(FR_Q2WIDE), which is
    # 1920x1080 (60 Hz) where FR_HZQ2's 1280x960 is listed at 75
    assert re.search(r"hzq2w\s*=\s*gr_target_hz\(&gp,\s*&rl,\s*gr_q2wide_res\(q2w\)\.w,"
                     r"\s*gr_q2wide_res\(q2w\)\.h,", src)
    for fr, var in (("FR_HZW", "hzw"), ("FR_HZ43", "hz43"),
                    ("FR_HZQ2", "hzq2"), ("FR_HZQ3", "hzq3"),
                    ("FR_HZQ2WIDE", "hzq2w")):
        m = re.search(r'printf\("set \\"%s=%%d\\"\\n",\s*(\w+)\)' % fr, src)
        assert m, "%s is not printed" % fr
        assert m.group(1) == var, "%s prints %s, not %s" % (fr, m.group(1), var)
    assert re.search(r'printf\("set \\"FR_HZSRC=%s\\"\\n",\s*gr_hz_src_name\(', src)


def test_the_ceiling_is_known_before_the_first_mode_is_added():
    """gr_modes_add applies the EDID cap as each mode goes IN (only the best
    rate per resolution is kept), so FLEETRES must probe the panel and set
    hz_cap before its first add - gameres.c's order."""
    src = _strip_comments(_src(FLEETRES))
    probe = src.index("native_ok = panel_probe(&p);")
    cap = src.index("rl.hz_cap = native_ok ? p.vmax : 0;")
    first_add = src.index("gr_modes_add(&rl,")
    assert probe < cap < first_add
    assert src.count("native_ok = panel_probe(&p);") == 1, "probe the panel once"


def test_every_enumerated_mode_goes_into_the_rate_list_too():
    src = _strip_comments(_src(FLEETRES))
    loops = src.count("add_mode(dm.dmPelsWidth, dm.dmPelsHeight);")
    rated = len(re.findall(r"gr_modes_add\(&rl,\s*\(int\)dm\.dmPelsWidth,\s*"
                           r"\(int\)dm\.dmPelsHeight,\s*\(int\)dm\.dmDisplayFrequency\)", src))
    assert loops >= 2 and loops == rated, (
        "every EnumDisplaySettings loop (the NULL device AND the per-adapter "
        "retry for .143's GeForce 6800) must feed the rate list")


def test_both_writers_seed_the_list_with_the_same_two_modes():
    """The live mode claims no rate (a game may have left it anywhere); the
    persisted one carries its own - identically in both programs, or the
    no-EDID rule (the persisted rate at a mode no bigger than itself) answers
    differently in each."""
    fr = _strip_comments(_src(FLEETRES))
    assert "gr_modes_add(&rl, desk_w, desk_h, 0);" in fr
    assert "gr_modes_add(&rl, reg_w, reg_h, reg_hz);" in fr
    gc = _strip_comments(_src(GAMERES_C))
    assert "gr_modes_add(&c->modes, c->live_w, c->live_h, 0);" in gc
    assert "gr_modes_add(&c->modes, c->reg_w, c->reg_h, c->reg_hz);" in gc
    # the same depth filter on both sides
    assert "dm.dmBitsPerPel >= 16" in fr and "dm.dmBitsPerPel >= 16" in gc


def test_gameres_decides_every_published_rate_with_the_same_function():
    h = _strip_comments(_src(GAMERES_H))
    dec = h[h.index("GR_FN void gr_decide("):]
    dec = dec[:dec.index("\n}\n")]
    for field, args in (("t->hz ", "t->w, t->h"), ("t->hz43", "t->w43, t->h43"),
                        ("t->hzq2", "gr_q2tab[t->q2mode].w"),
                        ("t->hzq3", "gr_q3tab[t->q3mode].w"),
                        ("t->hzq2wide", "gr_q2wide_res(t->q2wide).w")):
        m = re.search(re.escape(field) + r"\s*=\s*gr_target_hz\(p, l, " + re.escape(args), dec)
        assert m, "gr_decide: %s is not gr_target_hz() at its own mode" % field.strip()


def test_every_rate_token_has_the_launchers_name():
    """%TOKEN% <-> FR_TOKEN is the convention the shared-cfg mirror test relies
    on; a rate token without an FR_ twin could never be written identically."""
    h = _src(GAMERES_H)
    fr = _src(FLEETRES)
    for tok in ("HZW", "HZ43", "HZQ2", "HZQ3", "HZSRC", "HZQ2WIDE"):
        assert '"%s"' % tok in h, "gr_expand does not know %%%s%%" % tok
        assert "FR_%s=" % tok in fr, "FLEETRES does not publish FR_%s" % tok


def test_fr_hz_is_the_agents_formula():
    """FR_HZ (launcher) and %FRHZ% (agent) land in the SAME id Tech 3 line,
    r_displayRefresh. Review 2026-09-29: they were two formulas - FLEETRES
    fell back to the LIVE rate and allowed up to 240, gameres.h to 60 and
    199 - so a box with a 0/1 "default" registry rate got a different number
    from each writer and the file fought forever. One function now."""
    fr = _strip_comments(_src(FLEETRES))
    h = _strip_comments(_src(GAMERES_H))
    assert re.search(r'printf\("set \\"FR_HZ=%d\\"\\n",\s*gr_fr_hz\(reg_hz\)\);', fr)
    assert "t->fr_hz = gr_fr_hz(reg_hz);" in h
    fn = h[h.index("GR_FN int gr_fr_hz("):]
    fn = fn[:fn.index("\n}\n")]
    assert "gr_hz_is_real(reg_hz) ? reg_hz : 60" in fn
    assert "desk_hz" not in fr[fr.index('"set \\"FR_HZ='):][:200], (
        "FR_HZ must not fall back to the live rate: the agent cannot see it")


def test_both_writers_retry_the_enumeration_on_the_same_condition():
    """The per-adapter retry adds rates to the list; if one writer retried and
    the other did not, their rate lists - and so their numbers - differ.
    gameres.c retries on its rate list's size before the two seeds; FLEETRES
    must decide on its rate list too (rl.n), not its own mode list."""
    fr = _strip_comments(_src(FLEETRES))
    gc = _strip_comments(_src(GAMERES_C))
    assert "if (c->modes.n < 4) {" in gc
    assert "if (rl.n < 4) {" in fr
    assert "if (g_nmodes < 4) {" not in fr
    # the retry is decided BEFORE the live/persisted seeds in both
    assert fr.index("if (rl.n < 4) {") < fr.index("gr_modes_add(&rl, desk_w, desk_h, 0);")
    assert gc.index("if (c->modes.n < 4) {") < gc.index(
        "gr_modes_add(&c->modes, c->live_w, c->live_h, 0);")


def test_no_edid_never_claims_the_drivers_unclamped_best():
    """The rule in words, pinned in the source the agent compiles: without a
    measured ceiling only the persisted desktop's own rate is vouched for."""
    h = _strip_comments(_src(GAMERES_H))
    fn = h[h.index("GR_FN int gr_target_hz("):]
    fn = fn[:fn.index("\n}\n")]
    assert "gr_have_ceiling(p)" in fn
    assert "gr_has_rate(l, w, h, reg_hz)" in fn
    assert "w <= reg_w && h <= reg_h" in fn
    assert "return 0;" in fn
