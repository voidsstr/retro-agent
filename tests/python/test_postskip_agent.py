"""postskip (agent 1.86.0) - the agent re-asserts CMOS 2Dh bit 3 ("skip F1
message") at every start on .243, a Compaq Deskpro 2000 with a dead soldered
CMOS battery whose POST otherwise waits for F1 on 301-Keyboard Error.

The decision AND the port loop live in agent/shared/postskip.h and run under
tests/native/test_postskip.c against a simulated RTC (torn reads, lost
indexes, the UIP bit, a dead cell, another writer). Pinned here is what the
host cannot run - the Win32 half in agent/src/postskip.c - plus the header's
write paths, because this code writes the CMOS of a machine whose Computer
Setup cannot be reached:

  Win9x -> ROM readable -> ROM is the Deskpro 2000 04/25/97 -> port loop;
  the startup pass waits for clockfix (the other RTC writer) first;
  the only data-port writes are the 2Dh/2Eh/2Fh writer and the undo of OUR
  OWN stray byte - never a register "restored" from a snapshot.

(Two reviews 2026-09-27, before release: the first draft restored every
differing register from a single, possibly torn, snapshot and compared 0Ah's
self-toggling UIP bit; the second retried, and a retry re-read a lost
restore byte as the new truth and reported success over a damaged 0Bh. Now:
one write per run, every byte written is logged and attributable, and the
run never races clockfix.)
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / "agent" / "src" / "postskip.c").read_text()
HDR = (ROOT / "agent" / "shared" / "postskip.h").read_text()
CODE = re.sub(r"/\*.*?\*/", "", SRC, flags=re.S)
HCODE = re.sub(r"/\*.*?\*/", "", HDR, flags=re.S)


def body(code, name):
    start = code.index(name)
    depth, i = 0, code.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        if depth == 0:
            return code[start:i + 1]
        i += 1


def test_one_function_writes_the_cmos_data_port_and_it_is_gated():
    writers = [m.start() for m in re.finditer(r"io->outb\(io->ctx, 0x71\s*,", HCODE)]
    assert len(writers) == 1, "every data-port write goes through ps_put"
    put = body(HCODE, "static int ps_put(")
    gate = "if (!ps_writable(idx) && !ps_is_our_stray(before, after, attr, idx)) return -1;"
    assert put.index(gate) < put.index("ps_log_add(log, v);") < put.index("io->outb(io->ctx, 0x71, v)")
    stray = body(HCODE, "static int ps_is_our_stray")
    assert "ps_writable(i) || ps_volatile_reg(i)" in stray
    assert "& m) == 0" in stray, "attribution is compared under the register's mask (UIP)"
    # the Win32 file has one generic port writer and no loop of its own
    assert len(re.findall(r'"outb %0,%1"', CODE)) == 1
    assert "0x71" not in CODE and "0x70" not in CODE


def test_reads_are_double_and_uip_is_masked():
    stable = body(HCODE, "static int ps_read_stable")
    assert stable.count("ps_read_all(io,") == 2 and "ps_diff(b, c) == 0" in stable
    assert "i == 0x0A ? 0x7F : 0xFF" in body(HCODE, "static unsigned char ps_cmp_mask")
    assert "ps_cmp_mask(i)" in body(HCODE, "static int ps_reg_differs")


def test_port_io_is_paced_and_leaves_the_index_at_0d():
    assert "io->inb(io->ctx, 0x84)" in body(HCODE, "static void ps_delay")
    assert "io->outb(io->ctx, 0x70, 0x0D)" in body(HCODE, "static void ps_done")
    rd = body(HCODE, "static unsigned char ps_rd(")
    assert rd.index("0x70") < rd.index("ps_delay(io)") < rd.index("0x71")


def test_one_write_per_run_planned_from_a_stable_read():
    run = body(HCODE, "static void ps_cmos_run2")
    first_write = run.index("ps_put(io, &wl, &wl, i, want[i], before, before)")
    order = [run.index("ps_read_stable(io, before)"),
             run.index("switch (ps_plan2(before, want, ide2_want))"),
             first_write]
    assert order == sorted(order), order
    for case in ("case PS_NOT_CMOS:", "case PS_BAD_CHECKSUM:", "case PS_ALREADY:"):
        seg = run[run.index(case):first_write]
        assert "return;" in seg.split("case ", 2)[1], case
    assert "if (!apply)" in run[:first_write]
    # no retry within a run (review 2: a retry re-read a lost restore byte as
    # the new truth and reported success over a damaged 0Bh)
    assert "r->attempts = 1;" in run and "r->attempts++" not in run
    assert not re.search(r"for \(r->attempts", run) and "PS_RETRY" not in HCODE
    # success needs 2Dh-2Fh, the checksum AND none of our bytes elsewhere
    assert "ps_ours_equal(after, want) && ps_cs_valid(after) && ps_strays(before, after, &wl) == 0" in run
    # the restore attributes from the log as it stood when `seen` was read
    assert "ps_wlog_t attr = wl;" in run


def test_the_win32_half_guards_before_the_loop():
    app = body(CODE, "static int ps_applicable")
    order = [app.index("ps_is_win9x()"),
             app.index("IsBadReadPtr((const void *)0xF0000, 0x10000)"),
             app.index("ps_rom_matches((const unsigned char *)0xF0000")]
    assert order == sorted(order), order
    run = body(CODE, "static void ps_run")
    assert run.index("if (!ps_applicable(&why))") < run.index("ps_ide2_want()") \
        < run.index("ps_cmos_run2(&io, apply, r->ide2_want, &r->o)")


def test_startup_thread_is_win9x_only_never_races_clockfix_and_has_an_off_switch():
    th = body(CODE, "DWORD WINAPI postskip_thread")
    run = th.index("ps_run(&r, 1)")
    assert th.index("if (!ps_is_win9x()) return 0;") < run
    # applicability decides before ANY registry write: a record is never
    # created on a box this does not apply to (review 3)
    app = th.index("if (!ps_applicable(&why)) {")
    first_store = th.index("ps_store_boot(")
    assert app < first_store
    assert "if (ps_boot_recorded()) ps_store_boot(why);" in th[app:th.index("}", app) + 1]
    assert '"PostSkip"' in th and app < th.index("if (!enabled)") < run
    # this boot says PENDING before it waits, so an old "already set" is never read as today's
    pending = th.index('ps_store_boot("PENDING:')
    wait = th.index("while (!clockfix_finished()")
    bail = th.index("if (!clockfix_finished()) {")
    assert th.index("if (!enabled)") < pending < wait < bail < run
    # a POSTSKIP command is waited for, not allowed to cancel the boot's pass
    assert re.search(r"for \(waited = 0; InterlockedExchange\(\(LONG \*\)&g_ps_busy, 1\);", th)
    assert "ps_store_boot(summary)" in th and '"PostSkipBoot"' in CODE


def test_command_answers_applicable_first_and_never_touches_ports_beside_clockfix():
    h = body(CODE, "void handle_postskip")
    app = h.index("if (!ps_applicable(&why))")
    gate = h.index("else if (!clockfix_finished())")
    run = h.index("ps_run(&r, apply)")
    assert app < gate < run, "report-only reads are gated too (review 3), after applicability"
    assert "NOT READ: clockfix" in h


def test_a_clockfix_thread_that_never_started_does_not_block_postskip_forever():
    main = (ROOT / "agent" / "src" / "main.c").read_text()
    assert re.search(r'if \(!spawn_helper\(clockfix_thread, "clockfix"\)\)\s*\n\s*clockfix_mark_finished\(\);', main)


def test_clockfix_reports_done_on_every_path():
    clk = re.sub(r"/\*.*?\*/", "", (ROOT / "agent" / "src" / "clockfix.c").read_text(), flags=re.S)
    th = body(clk, "DWORD WINAPI clockfix_thread")
    # one exit: every early way out is `goto out`, and `out:` raises the flag
    assert th.count("return") == 1, "a bare return would leave postskip waiting 4 minutes"
    assert th.index("out:") < th.index("InterlockedExchange((LONG *)&g_clk_done, 1)") < th.index("return 0;")
    assert th.count("goto out;") >= 7
    assert "return g_clk_done != 0;" in clk


def test_command_reports_state_and_applies_only_on_request():
    h = body(CODE, "void handle_postskip")
    assert '_stricmp(args, "apply") == 0' in h
    assert "ps_run(&r, apply)" in h
    for key in ("applicable", "state", "skip_f1", "checksum_valid", "last_boot",
                "write_attempts", "strays_undone", "registers_changed", "ok"):
        assert f'"{key}"' in h, key


def test_wired_into_the_agent():
    main = (ROOT / "agent" / "src" / "main.c").read_text()
    assert main.index('spawn_helper(clockfix_thread, "clockfix")') \
        < main.index('spawn_helper(postskip_thread, "postskip")')
    handlers = (ROOT / "agent" / "src" / "handlers.c").read_text()
    assert re.search(r'\{\s*"POSTSKIP",\s*1,\s*NULL,\s*handle_postskip,\s*0\s*\}', handlers)
    assert "$(SRCDIR)/postskip.c" in (ROOT / "agent" / "Makefile").read_text()


def test_1bh_is_moved_only_on_request_and_its_reboot_is_guarded():
    """1.92.0: .243's 80 GB disk runs natively only while POST leaves it alone
    (CMOS 1Bh = 00). The agent restores 1Bh after a power loss ONLY when
    CmosIde2Type asks, and reboots once - never twice within 20 minutes, never
    when the disk is running anyway - so a CMOS that will not keep 1Bh cannot
    become a reboot loop."""
    want = body(CODE, "static int ps_ide2_want")
    assert '"CmosIde2Type"' in want and "present && v <= 0xFF ? (int)v : -1" in want
    plan = body(HCODE, "static enum ps_plan ps_plan2")
    assert "ide2_want >= 0 && ide2_want <= 0xFF" in plan
    th = body(CODE, "DWORD WINAPI postskip_thread")
    fix = th.index("if (ps_ide2_fixed(&r))")
    seg = th[fix:]
    assert seg.index("ps_ide2_disk_native()") < seg.index("ps_ide2_should_reboot(1, native, since, PS_IDE2_REBOOT_GAP_S)")
    # the guard is recorded AND flushed (Win9x writes its registry lazily) before the reboot
    assert seg.index('"CmosIde2Last"') < seg.index("RegFlushKey(h)") < seg.index("agent_self_reboot_9x(")
    assert '"CmosIde2Reboot"' in seg
    rule = body(HCODE, "static int ps_ide2_should_reboot")
    assert "if (!ide2_fixed || disk_native_ok) return 0;" in rule
    assert "secs_since_last >= 0 && secs_since_last < min_gap_s" in rule
    fixed = body(CODE, "static int ps_ide2_fixed")
    assert "!r->o.failed" in fixed and "r->o.now_1b == r->ide2_want" in fixed
    h = (ROOT / "agent" / "src" / "handlers.c").read_text()
    sr = body(h, "void agent_self_reboot_9x")
    assert "if (is_win9x())" in sr and "INVALID_SOCKET" in sr
