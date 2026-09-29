"""vcr-kmd: the VSA-100 core clock, set LIVE (IOCTL_VCR_CLOCK, 2026-09-29).

The user asked for 3dfx clock settings that apply in real time. Nothing in our
stack could change the clock before: Glide's SSTH3_GRXCLOCK path is compiled
out of the Windows build (minihwc.c `#if !defined(HWC_ACCESS_DDRAW)`), and the
kernel never wrote pllCtrl1. The pure rule is include/vcr_clock.h
(tests/native/test_vcr_clock.c); these tests pin the hardware half's safety
properties in miniport/vcrmp_clock.c and the plumbing around it.
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
    j = src.index('\n}\n', i)
    return src[i:j]


CLK = _read('miniport/vcrmp_clock.c')


def test_a_get_changes_nothing():
    body = _func(CLK, 'VP_STATUS VcrCoreClock')
    get_ret = body.index('if (rq->op == VCR_CLOCK_OP_GET)')
    assert 'return NO_ERROR;' in body[get_ret:get_ret + 80]
    assert 'clk_write_step' not in body[:get_ret]
    assert 'VcrWr' not in body            # every write goes through clk_write_step


def test_the_kill_switch_and_the_checks_come_before_any_write():
    body = _func(CLK, 'VP_STATUS VcrCoreClock')
    first_write = body.index('clk_write_step(')
    for guard in ('VcrDiagGet(L"CoreClock", 1)', 'VCR_CLOCK_R_CHIPS', 'vcr_clock_in_range(target)',
                  'vcr_clock_plan('):
        assert body.index(guard) < first_write, guard
    # a step outside the range is never written, even mid-ramp
    assert 'if (!word)' in body


def test_writes_happen_only_while_every_chip_is_idle_with_the_irql_raised():
    body = _func(CLK, 'static ULONG clk_write_step')
    raise_ = body.index('KfRaiseIrql(VCR_DISPATCH_LEVEL)')
    check = body.index('clk_all_idle_now(x)', raise_)
    write = body.index('VcrWr(x, 0, VCR_R_PLLCTRL1, word)')
    lower = body.index('KfLowerIrql(old)', write)
    assert body.index('clk_all_idle(x)') < raise_ < check < write < lower
    # and the word is read back afterwards, a mismatch said as such
    assert 'VcrRd(x, 0, VCR_R_PLLCTRL1) == word ? VCR_CLOCK_R_OK : VCR_CLOCK_R_READBACK' in body[lower:]
    # no logging at DISPATCH_LEVEL between the raise and the lower
    assert 'VLOG' not in body[raise_:lower]


def test_only_the_master_is_written_the_slaves_follow_it_at_sli_enable():
    """Measured on .124 at boot (2026-09-29): chips 1-3 read pllCtrl1 0x0C01
    (their reset word, 50.1 MHz) with reset DRAM timings - uninitialized. The
    first build wrote the target to all four, which would have jumped an
    uninitialized slave's PLL from 50 to ~162 MHz for nothing. Every SLI
    enable copies the master's word into the slaves (init_slave), as the
    vendor's InitializeSlaveChipsInitRegs does - that is how the slaves get
    the new clock, at the next game start."""
    body = _func(CLK, 'static ULONG clk_write_step')
    assert len(re.findall(r'VcrWr\(', body)) == 1 and 'VcrWr(x, 0,' in body
    assert 'VcrWr(x, c,' not in CLK
    sli = _read('miniport/vcrmp_sli.c')
    copy = sli[sli.index('k_init_copy1[] = {'):]
    copy = copy[:copy.index('};')]
    assert 'VCR_R_PLLCTRL1' in copy
    init = _func(sli, 'static int init_slave')
    assert 'reg_r(io, 0, k_init_copy1[i].off)' in init       # FROM the master
    en = _func(sli, 'static int sli_enable')
    assert 'warn |= init_slave(io, c);' in en                  # at every SLI enable
    assert vcr_clock_says_so()


def vcr_clock_says_so():
    h = _read('include/vcr_clock.h')
    return 'The live write goes to the MASTER' in h and '0x0C01' in h


def test_restore_writes_the_boards_own_word():
    body = _func(CLK, 'VP_STATUS VcrCoreClock')
    assert 'VCR_CLOCK_OP_RESTORE ? x->core_boot_pll[0]' in body


def test_the_boot_words_are_never_captured_after_our_own_write():
    cap = _func(CLK, 'void VcrCoreClockCapture')
    assert 'x->core_changed' in cap and 'return;' in cap
    assert 'x->core_changed = 1;' in _func(CLK, 'VP_STATUS VcrCoreClock')
    mp = _read('miniport/vcrmp.c')
    multi = mp.index('VcrMultiInit(x);')
    assert mp.index('VcrCoreClockCapture(x, "boot")') > multi   # slaves mapped first


def test_nothing_else_in_the_driver_writes_the_core_pll():
    for sub in ('miniport', 'display', 'common'):
        for name in os.listdir(os.path.join(KMD, sub)):
            if not name.endswith('.c') or name == 'vcrmp_clock.c':
                continue
            src = _read(os.path.join(sub, name))
            assert not re.search(r'VcrWr\([^;]*VCR_R_PLLCTRL1', src), name


def test_the_ioctl_and_the_escape_are_wired():
    ioctl = _read('include/vcr_ioctl.h')
    assert 'IOCTL_VCR_CLOCK         VCR_CTL(0xa0f)' in ioctl
    assert 'VCR_ESC_CLOCK           (VCR_ESC_BASE + 11)' in ioctl
    mp = _read('miniport/vcrmp.c')
    case = mp[mp.index('case IOCTL_VCR_CLOCK:'):]
    case = case[:case.index('break;')]
    # METHOD_BUFFERED: the request is copied out before the answer is written
    assert case.index('VideoPortMoveMemory(&rq') < case.index('VcrCoreClock(')
    esc = _read('display/vcrdd_escape.c')
    assert 'esc <= VCR_ESC_CLOCK' in esc
    ec = esc[esc.index('case VCR_ESC_CLOCK:'):]
    ec = ec[:ec.index('return got;')]
    assert 'IOCTL_VCR_CLOCK' in ec and 'exclusive_pid = pd->exclusive_pid' in ec


def test_the_recorder_event_is_appended_not_renumbered():
    ev = _read('include/vcr_events.h')
    assert re.search(r'VCR_EV_CORE_CLOCK,\s+704,', ev)
    assert ev.index('VCR_EV_SLI_POKE_REFUSED, 703') < ev.index('VCR_EV_CORE_CLOCK,    704')


def test_vcrctl_answers_with_the_read_back_words():
    ctl = _read('tools/vcrctl.c')
    body = _func(ctl, 'static int cmd_clock')
    assert 'VCR_ESC_CLOCK' in body and 'cur_pll[c]' in body and 'boot_pll[c]' in body


def test_a_change_is_refused_while_a_glide_program_holds_the_board():
    esc = _read('display/vcrdd_escape.c')
    ec = esc[esc.index('case VCR_ESC_CLOCK: {'):]
    ec = ec[:ec.index('return got;')]
    # SET/RESTORE under an exclusive owner become a GET before the IOCTL, and
    # the answer says EXCLUSIVE - the clocks are still reported
    refuse = ec.index('(rq.op == VCR_CLOCK_OP_SET || rq.op == VCR_CLOCK_OP_RESTORE) && pd->exclusive_pid')
    assert ec.index('rq.op = VCR_CLOCK_OP_GET;', refuse) < ec.index('IOCTL_VCR_CLOCK, in, inlen')
    assert 'result = VCR_CLOCK_R_EXCLUSIVE' in ec
    assert '#define VCR_CLOCK_R_EXCLUSIVE   8' in _read('include/vcr_ioctl.h')
