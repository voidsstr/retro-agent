"""vcr-kmd: the AA auto-disarm after a freeze (2026-09-30).

Quake II at 2x AA froze .124, AA stayed armed, and the NEXT boot froze too when a
game was started from the desktop. A frozen box cannot disarm itself, so the next
boot must. The pure rule is include/vcr_aaguard.h (tests/native/test_vcr_aaguard.c);
these pin the kernel wiring.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
KMD = os.path.normpath(os.path.join(HERE, '..', '..', 'voodoo-cleanroom', 'vcr-kmd'))


def _read(rel):
    return open(os.path.join(KMD, rel), encoding='latin-1').read()


def _func(src, name):
    i = src.index(name + '(')
    i = src.rindex('\n', 0, i) + 1
    return src[i:src.index('\n}\n', i)]


MULTI = _read('miniport/vcrmp_multi.c')


def test_the_marker_is_set_only_after_a_successful_aa_enable_and_flushed():
    body = _func(MULTI, 'VP_STATUS VcrSliRequest')
    ok = body.index('if (rc >= 0) {')
    mark = body.index('VcrDiagSet(L"SliAALive", 1, TRUE);')
    assert ok < mark
    assert 'if (r->ChipInfo.dwaaEn) {' in body[ok:mark]
    # before Glide's next MMIO: the marker precedes the SLI/AA-on log and readback
    assert mark < body.index('"SLI/AA on: %u chips -> %d')


def test_every_teardown_clears_it_through_vcrslioff():
    off = _func(MULTI, 'void VcrSliOff')
    assert off.index('x->sli_chips = 0;') < off.index('VcrDiagSet(L"SliAALive", 0, TRUE);')
    # Glide's disable, re-enable, mode set and display reset all use VcrSliOff
    assert 'VcrSliOff(x, "Glide asked")' in MULTI and 'VcrSliOff(x, "re-enable")' in MULTI
    assert 'VcrSliOff(x, "mode set")' in _read('miniport/vcrmp_hw.c')
    assert 'VcrSliOff(x, "display reset")' in _read('miniport/vcrmp.c')


def test_the_boot_guard_disarms_restores_glide_and_records():
    g = _func(MULTI, 'void VcrSliAABootGuard')
    assert 'vcr_aag_boot(live) != VCR_AAG_DISARM' in g
    assert g.index('VcrDiagSet(L"SliAA", 0, TRUE);') < g.index('VcrDiagSet(L"SliAALive", 0, TRUE);')
    assert 'VcrGlideAaConfigReset(vcr_aag_safe_cfg(chips), &changed)' in g
    assert 'VcrDiagSet(L"SliAAAutoOff", dead_boot, TRUE);' in g
    assert 'PrevBootCount' in g


def test_it_runs_once_after_the_slaves_are_placed():
    mp = _read('miniport/vcrmp.c')
    assert mp.index('VcrMultiInit(x);') < mp.index('VcrSliAABootGuard(x);')
    assert mp.count('VcrSliAABootGuard(x);') == 1


def test_the_glide_key_is_opened_never_created_and_only_an_aa_value_is_touched():
    f = _func(_read('miniport/vcrmp_log.c'), 'LONG VcrGlideAaConfigReset')
    assert 'ZwOpenKey(&h, VCR_KEY_READ_WRITE, &oa)' in f and 'ZwCreateKey' not in f
    assert 'Services\\\\3dfxvs\\\\Device0\\\\glide' in f
    assert 'vcr_aag_cfg_is_aa((unsigned)old)' in f
    assert f.index('ZwSetValueKey') < f.index('ZwFlushKey(h);')


def test_the_recorder_event_is_appended_not_renumbered():
    ev = _read('include/vcr_events.h')
    assert re.search(r'VCR_EV_SLI_AA_GUARD,\s+705,', ev)
    assert ev.index('VCR_EV_CORE_CLOCK,    704') < ev.index('VCR_EV_SLI_AA_GUARD,  705')
