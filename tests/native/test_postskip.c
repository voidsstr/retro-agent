/* agent/shared/postskip.h - the decision half of postskip.c (agent 1.86.0).
 *
 * .243 (Compaq Deskpro 2000) halts every POST on 301-Keyboard Error waiting
 * for F1 unless CMOS 2Dh bit 3 is set, and its soldered CMOS battery is dead,
 * so the agent re-asserts the bit at every start. A wrong decision here writes
 * the CMOS of a machine whose Computer Setup cannot be reached, so what is
 * pinned is mostly what it must REFUSE: any other ROM, a CMOS whose checksum is
 * already bad, and every register except 2Dh/2Eh/2Fh. The CMOS bytes and the
 * ROM identity are the box's own (read 2026-09-26/27). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../agent/shared/postskip.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

/* .243's CMOS 00h-3Fh before the skip-F1 bit was set (2Dh=00, checksum 0434h). */
static const char *CMOS_HEX =
    "540049002200002609262602508000004000f000038002003c41004400f00000"
    "000000007e2b00000000022300000434003c2080001101000000000000000000";

/* F000:FFE0-FFFF of the box's own ROM: "COMPAQ" at FFEA, "04/25/97" at FFF5. */
static const char *ROM_TAIL_HEX =
    "32803a80432005203033434f4d504151ea26f400f030342f32352f393720fc89";

static unsigned char rom[0x10000];
static unsigned char cmos[128];

static void hex(const char *h, unsigned char *out)
{
    unsigned v;
    while (h[0] && h[1]) { sscanf(h, "%2x", &v); *out++ = (unsigned char)v; h += 2; }
}

static void build_rom(void)
{
    memset(rom, 0xFF, sizeof(rom));
    hex(ROM_TAIL_HEX, rom + 0xFFE0);
    memcpy(rom + 0x433C, "Compaq Deskpro 2000", 19);  /* where the box has it */
}

static void build_cmos(void)
{
    memset(cmos, 0, sizeof(cmos));
    hex(CMOS_HEX, cmos);
}


/* ---- a simulated RTC for the port loop (ps_cmos_run) ---------------------
 * Index port 70h, data port 71h, delay port 84h. 0Ch/0Dh are read-only and
 * 0Ah bit 7 (UIP) is read-only. Fault hooks model the races two reviews found:
 * a read whose index another accessor moves, a DATA WRITE whose index is lost
 * (scheduled by write number, so a restore or undo byte can be the one that
 * strays), UIP flipping by itself, a dead cell, another writer mid-run. */
#define SIM_LOSE_MAX 4
typedef struct {
    unsigned char reg[128];
    int idx;
    int data_writes;                /* outb(71h) count */
    int writes_to[128];             /* data writes that LANDED per register */
    /* faults */
    int uip_every, uip_reads, uip_injected; /* every Nth read OF 0Ah reports UIP set */
    int tear_reg, tear_to, tear_count;      /* reads of tear_reg return tear_to's byte */
    int tear_alt, tear_seen;                /* ... only every other read of tear_reg */
    int lose_nth[SIM_LOSE_MAX], lose_to[SIM_LOSE_MAX], lost; /* data write #n lands in lose_to */
    int drop_reg, drop_count;               /* writes to drop_reg are ignored (-1 = always) */
    int drop2_reg, drop2_after, drop2_seen; /* writes to drop2_reg ignored after the first drop2_after */
    int other_reg, other_val, other_after_writes; /* another writer, after N of our data writes */
    unsigned slept;
} sim_t;

static unsigned char sim_inb(void *ctx, unsigned short port)
{
    sim_t *m = (sim_t *)ctx;
    int i;
    if (port != 0x71) return 0xFF;
    i = m->idx;
    if (i == m->tear_reg && m->tear_count > 0 && (!m->tear_alt || m->tear_seen++ % 2 == 0)) {
        m->tear_count--;
        m->idx = i = m->tear_to;
    }
    if (i == 0x0A && m->uip_every > 0 && ++m->uip_reads % m->uip_every == 0) {
        m->uip_injected++;
        return (unsigned char)(m->reg[0x0A] | 0x80);
    }
    return m->reg[i];
}

static void sim_outb(void *ctx, unsigned short port, unsigned char v)
{
    sim_t *m = (sim_t *)ctx;
    int i, k;
    if (port == 0x70) { m->idx = v & 0x7F; return; }
    if (port != 0x71) return;
    i = m->idx;
    m->data_writes++;
    for (k = 0; k < SIM_LOSE_MAX; k++)
        if (m->lose_nth[k] == m->data_writes) { i = m->idx = m->lose_to[k]; m->lost++; }
    if (i == m->drop_reg && m->drop_count != 0) { if (m->drop_count > 0) m->drop_count--; return; }
    if (i == m->drop2_reg && m->drop2_seen++ >= m->drop2_after) return;
    if (i == 0x0C || i == 0x0D) return;
    if (i == 0x0A) v = (unsigned char)((v & 0x7F) | (m->reg[0x0A] & 0x80));
    m->reg[i] = v;
    m->writes_to[i]++;
    if (m->other_reg >= 0 && m->data_writes == m->other_after_writes) m->reg[m->other_reg] = (unsigned char)m->other_val;
}

static void sim_sleep(void *ctx, unsigned ms) { ((sim_t *)ctx)->slept += ms; }

static void sim_init(sim_t *m)
{
    memset(m, 0, sizeof(*m));
    hex(CMOS_HEX, m->reg);
    m->idx = 0x0D;
    m->tear_reg = m->drop_reg = m->drop2_reg = m->other_reg = -1;
}

static void sim_lose(sim_t *m, int nth, int to)
{
    int k;
    for (k = 0; k < SIM_LOSE_MAX; k++)
        if (!m->lose_nth[k]) { m->lose_nth[k] = nth; m->lose_to[k] = to; return; }
}

static ps_outcome_t sim_run(sim_t *m, int apply)
{
    ps_io_t io;
    ps_outcome_t o;
    io.inb = sim_inb; io.outb = sim_outb; io.sleep_ms = sim_sleep; io.ctx = m;
    ps_cmos_run(&io, apply, &o);
    return o;
}

static ps_outcome_t sim_run2(sim_t *m, int apply, int ide2_want)
{
    ps_io_t io;
    ps_outcome_t o;
    io.inb = sim_inb; io.outb = sim_outb; io.sleep_ms = sim_sleep; io.ctx = m;
    ps_cmos_run2(&io, apply, ide2_want, &o);
    return o;
}

static int only_ours_changed(const sim_t *m, const unsigned char *orig)
{
    int i;
    for (i = 0; i < 128; i++)
        if (!ps_writable(i) && m->reg[i] != orig[i]) return 0;
    return 1;
}

/* After a failure the bank must be EXACTLY the snapshot, or the run must say so. */
static int restored(const sim_t *m, const unsigned char *orig) { return memcmp(m->reg, orig, 128) == 0; }

static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static void load_current_243(sim_t *m)
{
    /* The box after 2026-09-27's hand change, then 2Dh bit 3 lost: 1Bh=00,
     * 2Dh=00, checksum 03F0h - its plan writes 2Fh=F8h (bit 7 set). */
    m->reg[0x1B] = 0x00; m->reg[0x2D] = 0x00; m->reg[0x2E] = 0x03; m->reg[0x2F] = 0xF0;
}

static void loop_tests(void)
{
    sim_t m;
    ps_outcome_t o;
    unsigned char orig[128];

    sim_init(&m); memcpy(orig, m.reg, 128);
    o = sim_run(&m, 1);
    CHECK(!o.failed && strcmp(o.state, "set now") == 0 && o.attempts == 1, "sim: a clean run sets the bit in one write");
    CHECK(m.reg[0x2D] == 0x08 && m.reg[0x2E] == 0x04 && m.reg[0x2F] == 0x3C && ps_cs_valid(m.reg),
          "sim: 2Dh=08, checksum 043Ch, valid");
    CHECK(only_ours_changed(&m, orig), "sim: no register outside 2Dh-2Fh changed");
    CHECK(m.idx == 0x0D, "sim: the index is left at 0Dh (NMI enabled) as the ROM leaves it");
    CHECK(m.data_writes == 2 && m.writes_to[0x2E] == 0,
          "sim: only the registers that change are written (2Eh stays 04h - not rewritten)");
    CHECK(o.now_2d == 0x08 && o.cs_now && o.changed[0] == 0, "sim: the report comes from the verified read");

    m.data_writes = 0;
    o = sim_run(&m, 1);
    CHECK(strcmp(o.state, "already set") == 0 && !o.failed && m.data_writes == 0, "sim: bit already set - no write at all");

    sim_init(&m);
    o = sim_run(&m, 0);
    CHECK(o.failed && m.data_writes == 0 && m.reg[0x2D] == 0x00, "sim: report-only (POSTSKIP without apply) writes nothing");

    sim_init(&m); m.reg[0x2F] ^= 1;
    o = sim_run(&m, 1);
    CHECK(o.refused && m.data_writes == 0, "sim: a bad checksum is refused before any write");

    /* UIP flips by itself; the first draft compared it and cried "do not reboot". */
    sim_init(&m); m.uip_every = 2;
    o = sim_run(&m, 1);
    CHECK(!o.failed && m.reg[0x2D] == 0x08 && m.uip_injected > 0,
          "sim: 0Ah's UIP bit flipping on every other read of 0Ah does not fail the write (and it did flip)");
    {
        unsigned char x[128], y[128];
        memcpy(x, orig, 128); memcpy(y, orig, 128); y[0x0A] |= 0x80;
        CHECK(ps_diff(x, y) == 0 && x[0x0A] != y[0x0A], "UIP is masked out of the compare (the raw bytes differ)");
    }

    /* Torn reads. */
    sim_init(&m); memcpy(orig, m.reg, 128); m.tear_reg = 0x0B; m.tear_to = 0x00; m.tear_count = 1;
    o = sim_run(&m, 1);
    CHECK(!o.failed && m.reg[0x0B] == orig[0x0B] && m.reg[0x2D] == 0x08 && only_ours_changed(&m, orig),
          "sim: one torn read of 0Bh is caught by the double read");
    sim_init(&m); m.tear_reg = 0x0B; m.tear_to = 0x00; m.tear_count = 1000; m.tear_alt = 1;
    o = sim_run(&m, 1);
    CHECK(o.refused && m.data_writes == 0, "sim: reads that never agree are refused, nothing written");
    sim_init(&m); memcpy(orig, m.reg, 128); m.tear_reg = 0x0B; m.tear_to = 0x00; m.tear_count = 1000;
    o = sim_run(&m, 1);
    CHECK(m.reg[0x0B] == orig[0x0B] && only_ours_changed(&m, orig),
          "sim: a consistent mis-read of 0Bh (it agrees with itself) never becomes a write to 0Bh");
    /* A torn 0Bh snapshot AND our byte lost into 0Bh: the undo must write the
     * double-read 02h, never the torn 54h the first draft put back. */
    sim_init(&m); memcpy(orig, m.reg, 128); m.tear_reg = 0x0B; m.tear_to = 0x00; m.tear_count = 1;
    sim_lose(&m, 1, 0x0B);
    o = sim_run(&m, 1);
    CHECK(o.failed && restored(&m, orig) && o.strays_undone >= 1,
          "sim: torn 0Bh snapshot + our byte in 0Bh -> 0Bh back to 02h (not 54h), bank = snapshot");

    /* One write, no retry: a byte that loses its index ends the run, restored. */
    sim_init(&m); memcpy(orig, m.reg, 128); sim_lose(&m, 1, 0x0B);
    o = sim_run(&m, 1);
    CHECK(o.failed && o.attempts == 1 && restored(&m, orig) && o.strays_undone == 1
          && starts_with(o.state, "FAILED: the write did not land cleanly"),
          "sim: our 08h lost into 0Bh -> taken back out, bank = snapshot, reported FAILED, not retried");
    CHECK(o.now_2d == 0x00 && o.cs_now && o.changed[0] == 0, "sim: ... and the report says what is really there");
    sim_init(&m); memcpy(orig, m.reg, 128); sim_lose(&m, 1, 0x0A);
    o = sim_run(&m, 1);
    CHECK(o.failed && restored(&m, orig), "sim: our 08h lost into 0Ah (the divider) -> taken back out");
    sim_init(&m); memcpy(orig, m.reg, 128); sim_lose(&m, 1, 0x0A); m.uip_every = 1;
    o = sim_run(&m, 1);
    CHECK(o.failed && restored(&m, orig) && m.uip_injected > 0,
          "sim: ... even when every read of 0Ah shows UIP set (attribution is masked)");
    sim_init(&m); load_current_243(&m); memcpy(orig, m.reg, 128);
    CHECK(ps_cs_valid(m.reg), "sim: (the 1Bh=00 bank with 2Dh lost is valid: 03F0h)");
    sim_lose(&m, 2, 0x0A);          /* the 2Fh write, F8h: bit 7 lands on 0Ah's read-only UIP */
    o = sim_run(&m, 1);
    CHECK(o.failed && restored(&m, orig), "sim: our F8h lost into 0Ah reads back 78h and is still recognised");
    sim_init(&m); memcpy(orig, m.reg, 128); sim_lose(&m, 1, 0x12);
    o = sim_run(&m, 1);
    CHECK(o.failed && restored(&m, orig) && ps_cs_valid(m.reg),
          "sim: our byte lost into 12h (inside the checksum) is undone before the checksum is judged");

    /* The bytes of the RESTORE and the UNDO can stray too (review 2). */
    sim_init(&m); memcpy(orig, m.reg, 128); m.drop_reg = 0x2D; m.drop_count = 1;
    sim_lose(&m, 3, 0x0B);          /* #1 2Dh dropped, #2 2Fh=3C, #3 the 2Fh restore (34h) -> 0Bh */
    o = sim_run(&m, 1);
    CHECK(o.failed && restored(&m, orig) && m.lost == 1, "sim: a restore byte lost into 0Bh is recognised and taken back out");
    sim_init(&m); memcpy(orig, m.reg, 128); sim_lose(&m, 1, 0x0B);
    sim_lose(&m, 4, 0x0A);          /* #1 08h->0Bh, #2 2Fh, #3 2Fh restore, #4 the undo of 0Bh (02h) -> 0Ah */
    o = sim_run(&m, 1);
    CHECK(o.failed && restored(&m, orig) && m.lost == 2, "sim: an undo byte lost into 0Ah is recognised and taken back out");

    /* When it cannot be put back, it says so, and names the registers. */
    sim_init(&m); memcpy(orig, m.reg, 128); m.drop_reg = 0x2D; m.drop_count = -1; m.drop2_reg = 0x2F; m.drop2_after = 1;
    o = sim_run(&m, 1);
    CHECK(o.failed && starts_with(o.state, "FAILED AND NOT RESTORED") && strstr(o.changed, "2Fh 34>3C") != NULL,
          "sim: a restore that does not take says 'do not reboot' and lists 2Fh 34>3C");
    CHECK(!o.cs_now, "sim: ... and reports the checksum as invalid, as it really is");

    /* Someone else's write is not ours to undo. */
    sim_init(&m); m.other_reg = 0x40; m.other_val = 0x55; m.other_after_writes = 1;
    o = sim_run(&m, 1);
    CHECK(!o.failed && m.reg[0x40] == 0x55 && o.strays_undone == 0 && strstr(o.changed, "40h 00>55") != NULL,
          "sim: another writer's change to 40h is left alone and reported");

    /* ... also when the run fails: the restore puts back 2Dh-2Fh and OUR bytes,
     * never another writer's change (the first draft "restored" everything). */
    sim_init(&m); memcpy(orig, m.reg, 128); m.drop_reg = 0x2D; m.drop_count = -1;
    m.other_reg = 0x40; m.other_val = 0x55; m.other_after_writes = 2;   /* #1 is the dropped 2Dh write */
    o = sim_run(&m, 1);
    CHECK(o.failed && m.reg[0x40] == 0x55 && ps_ours_equal(m.reg, orig) && ps_cs_valid(m.reg)
          && starts_with(o.state, "FAILED: 2Dh-2Fh restored, but another writer") && strstr(o.changed, "40h 00>55"),
          "sim: a failed run leaves another writer's 40h alone and names it");

    /* One of our bytes turning up in another register is never success, even
     * when 2Dh-2Fh read back right (here another writer puts 08h into 40h -
     * the documented cost: indistinguishable from our own stray, so undone). */
    sim_init(&m); memcpy(orig, m.reg, 128); m.other_reg = 0x40; m.other_val = 0x08; m.other_after_writes = 1;
    o = sim_run(&m, 1);
    CHECK(o.failed && m.reg[0x40] == orig[0x40] && o.strays_undone == 1,
          "sim: a byte of ours found in another register fails the run and is taken back out");

    /* 1.86.1 end to end: .243 after a 163 boot - 0Eh=04, 2Dh=00. */
    sim_init(&m); m.reg[0x0E] = 0x04; memcpy(orig, m.reg, 128);
    o = sim_run(&m, 1);
    CHECK(!o.failed && m.reg[0x0E] == 0x00 && m.reg[0x2D] == 0x08 && ps_cs_valid(m.reg) && m.data_writes == 3,
          "sim: 163 flag cleared and skip-F1 set in one run (three writes: 0Eh, 2Dh, 2Fh)");
    {
        int k, others = 0;
        for (k = 0; k < 128; k++) if (k != 0x0E && k != 0x2D && k != 0x2F && m.reg[k] != orig[k]) others++;
        CHECK(others == 0 && o.now_0e == 0x00 && o.before_0e == 0x04, "sim: ... nothing else changed, and it is reported");
    }
    sim_init(&m); m.reg[0x0E] = 0x04; sim_lose(&m, 1, 0x0B);            /* the 0Eh write strays into 0Bh */
    memcpy(orig, m.reg, 128);
    o = sim_run(&m, 1);
    CHECK(o.failed && restored(&m, orig), "sim: a lost 0Eh byte is taken back out of 0Bh, bank = snapshot");

    /* A cell that never takes: one run, one write, restored, no retry loop. */
    sim_init(&m); memcpy(orig, m.reg, 128); m.drop_reg = 0x2D; m.drop_count = -1;
    o = sim_run(&m, 1);
    CHECK(o.failed && o.attempts == 1 && restored(&m, orig) && ps_cs_valid(m.reg),
          "sim: a dead 2Dh cell -> restored (checksum valid, no 162), reported, not retried");

    /* 1.92.0: CMOS 1Bh on request. .243 after a power loss: POST's defaults
     * put 2Dh=00 and 1Bh=44 (auto) back - the test bank IS that state. */
    sim_init(&m); memcpy(orig, m.reg, 128);
    o = sim_run2(&m, 1, 0x00);
    CHECK(!o.failed && m.reg[0x1B] == 0x00 && m.reg[0x2D] == 0x08 && m.reg[0x2E] == 0x03 && m.reg[0x2F] == 0xF8
          && ps_cs_valid(m.reg), "sim 1Bh: 1Bh=44 + 2Dh=00 -> ONE write pass: 1Bh=00, 2Dh=08, checksum 03F8h (what cmosw9x none left on .243)");
    CHECK(only_ours_changed(&m, orig) && m.data_writes == 4 && m.writes_to[0x1B] == 1,
          "sim 1Bh: 1Bh/2Dh/2Eh/2Fh written once each, nothing else touched");
    CHECK(o.before_1b == 0x44 && o.now_1b == 0x00, "sim 1Bh: the outcome reports 1Bh before and after");
    m.data_writes = 0;
    o = sim_run2(&m, 1, 0x00);
    CHECK(strcmp(o.state, "already set") == 0 && m.data_writes == 0, "sim 1Bh: a second start finds nothing to do");

    sim_init(&m); memcpy(orig, m.reg, 128);
    o = sim_run(&m, 1);
    CHECK(!o.failed && m.reg[0x1B] == 0x44 && m.writes_to[0x1B] == 0,
          "sim 1Bh: WITHOUT CmosIde2Type (ide2_want -1) 1Bh stays 44h, never written");

    sim_init(&m); m.reg[0x2D] = 0x08; m.reg[0x2F] = 0x3C; memcpy(orig, m.reg, 128);
    o = sim_run2(&m, 1, 0x00);
    CHECK(!o.failed && m.reg[0x1B] == 0x00 && m.reg[0x2D] == 0x08 && ps_cs_valid(m.reg) && m.writes_to[0x2D] == 0,
          "sim 1Bh: skip-F1 already set - 1Bh alone is still worth a run (2Dh not rewritten)");

    /* The first data write (1Bh's 00h) loses its index into 0Bh. */
    sim_init(&m); memcpy(orig, m.reg, 128); sim_lose(&m, 1, 0x0B);
    o = sim_run2(&m, 1, 0x00);
    CHECK(o.failed && restored(&m, orig) && ps_cs_valid(m.reg),
          "sim 1Bh: our 00h lost into 0Bh -> taken back out, bank = snapshot (1Bh 44h, checksum valid), reported");
    CHECK(o.now_1b == 0x44, "sim 1Bh: ... and the report says 1Bh is still 44h");
}

int main(void)
{
    unsigned char want[128], other[128];
    enum ps_plan p;
    int i, only_three;

    build_rom();
    CHECK(ps_rom_matches(rom, sizeof(rom)), "the Deskpro 2000 04/25/97 ROM matches");
    CHECK(!ps_rom_matches(rom, 0xFFFF), "a short ROM image is refused");

    build_rom(); memcpy(rom + PS_ROM_DATE_OFF, "06/01/99", 8);
    CHECK(!ps_rom_matches(rom, sizeof(rom)),
          "the ROMPaq SP15800 date is refused (the bit is proven on 04/25/97 only)");
    build_rom(); memcpy(rom + PS_ROM_VENDOR_OFF, "AWARD ", 6);
    CHECK(!ps_rom_matches(rom, sizeof(rom)), "a non-Compaq ROM is refused");
    build_rom(); memcpy(rom + 0x433C, "Compaq Deskpro 4000", 19);
    CHECK(!ps_rom_matches(rom, sizeof(rom)),
          "a Compaq ROM with the same date but another model is refused");
    build_rom(); memcpy(rom + 0x8000, "Compaq Deskpro 2000", 19);
    memset(rom + 0x433C, 0xFF, 19);
    CHECK(ps_rom_matches(rom, sizeof(rom)), "the model string is found wherever it sits");

    build_cmos();
    CHECK(ps_cs_valid(cmos) && ps_sum10_2d(cmos) == 0x0434, "the box's CMOS checksum is valid (0434h)");
    p = ps_plan(cmos, want);
    CHECK(p == PS_APPLY, "2Dh=00: plan to apply");
    CHECK(want[0x2D] == 0x08 && want[0x2E] == 0x04 && want[0x2F] == 0x3C,
          "want 2Dh=08 and the checksum recomputed to 043Ch");
    CHECK(ps_cs_valid(want), "the planned image has a valid checksum");
    only_three = 1;
    for (i = 0; i < 128; i++)
        if (i != 0x2D && i != 0x2E && i != 0x2F && want[i] != cmos[i]) only_three = 0;
    CHECK(only_three, "the plan changes 2Dh/2Eh/2Fh and nothing else");

    /* The failure this avoids: set the bit and keep the old checksum - POST then
     * reports 162 and waits for F1 at EVERY boot, the opposite of the goal. */
    memcpy(other, cmos, 128); other[0x2D] |= PS_SKIP_F1;
    CHECK(!ps_cs_valid(other), "old-buggy shape: the bit without a new checksum is an invalid CMOS");

    /* Today's state on the box: 1Bh=00, 2Dh=08, checksum 03F8h. */
    build_cmos(); cmos[0x1B] = 0x00; cmos[0x2D] = 0x08; cmos[0x2E] = 0x03; cmos[0x2F] = 0xF8;
    CHECK(ps_cs_valid(cmos), "the box's current CMOS (03F8h) is valid");
    memset(want, 0xAA, sizeof(want));
    CHECK(ps_plan(cmos, want) == PS_ALREADY && want[0] == 0xAA, "bit already set: nothing to do, want untouched");

    build_cmos(); cmos[0x2D] = 0x41; cmos[0x2F] = (unsigned char)(cmos[0x2F] + 0x41);
    p = ps_plan(cmos, want);
    CHECK(p == PS_APPLY && want[0x2D] == 0x49, "the other bits of 2Dh are kept (41h -> 49h)");

    build_cmos(); cmos[0x2F] ^= 0x01;
    memset(want, 0xAA, sizeof(want));
    CHECK(ps_plan(cmos, want) == PS_BAD_CHECKSUM && want[0] == 0xAA,
          "an invalid checksum (a power loss on a dead battery) is refused, nothing planned");
    memset(cmos, 0xFF, sizeof(cmos));
    CHECK(ps_plan(cmos, want) == PS_NOT_CMOS, "an all-FF bank (no CMOS answering) is refused");
    memset(cmos, 0, sizeof(cmos));
    CHECK(ps_cs_valid(cmos), "an erased bank sums to a 'valid' zero checksum ...");
    CHECK(ps_plan(cmos, want) == PS_NOT_CMOS, "... so it is refused by the 640 KB base-memory check instead");
    build_cmos();
    CHECK(ps_bank_sane(cmos), "the box's own bank reports 640 KB base memory");

    for (i = 0, only_three = 0; i < 128; i++) only_three += ps_writable(i);
    CHECK(only_three == 5 && ps_writable(0x0E) && ps_writable(0x1B) && ps_writable(0x2D) && ps_writable(0x2E)
          && ps_writable(0x2F), "only 0Eh/1Bh/2Dh/2Eh/2Fh are writable (1.86.1 adds 0Eh, 1.92.0 1Bh)");

    /* 1.92.0: 1Bh moves ONLY when a plan is asked for it (CmosIde2Type). */
    build_cmos();                                              /* 1Bh=44, 2Dh=00, 0434h */
    p = ps_plan2(cmos, want, -1);
    CHECK(p == PS_APPLY && want[0x1B] == 0x44 && want[0x2D] == 0x08,
          "no CmosIde2Type: the plan keeps 1Bh (44h) exactly as it was");
    p = ps_plan2(cmos, want, 0x00);
    CHECK(p == PS_APPLY && want[0x1B] == 0x00 && want[0x2D] == 0x08 && want[0x2E] == 0x03 && want[0x2F] == 0xF8
          && ps_cs_valid(want), "CmosIde2Type=0 after a power loss: 1Bh 44h->00h and 2Dh in one plan, checksum 03F8h");
    for (i = 0, only_three = 1; i < 128; i++)
        if (i != 0x1B && i != 0x2D && i != 0x2E && i != 0x2F && want[i] != cmos[i]) only_three = 0;
    CHECK(only_three, "... and it changes nothing but 1Bh/2Dh/2Eh/2Fh");
    build_cmos(); cmos[0x1B] = 0x00; cmos[0x2D] = 0x08; cmos[0x2E] = 0x03; cmos[0x2F] = 0xF8;
    memset(want, 0xAA, sizeof(want));
    CHECK(ps_plan2(cmos, want, 0x00) == PS_ALREADY && want[0] == 0xAA, "1Bh already 00h and the bit set: nothing to do");
    build_cmos(); cmos[0x2F] ^= 1;
    CHECK(ps_plan2(cmos, want, 0x00) == PS_BAD_CHECKSUM, "a bad checksum refuses the 1Bh plan too");
    build_cmos();
    CHECK(ps_plan2(cmos, want, 0x100) == PS_APPLY && want[0x1B] == 0x44, "an out-of-range CmosIde2Type is ignored");

    CHECK(ps_ide2_should_reboot(1, 0, -1, 1200), "1Bh fixed, disk not native, never rebooted for it: reboot once");
    CHECK(!ps_ide2_should_reboot(0, 0, -1, 1200), "1Bh not changed: no reboot");
    CHECK(!ps_ide2_should_reboot(1, 1, -1, 1200), "the disk is running under Windows anyway: no reboot");
    CHECK(!ps_ide2_should_reboot(1, 0, 300, 1200), "rebooted for it 5 minutes ago: no second reboot (no loop)");
    CHECK(ps_ide2_should_reboot(1, 0, 7200, 1200), "rebooted for it 2 hours ago (another power loss): reboot again");

    /* 1.86.1: 0Eh bit 2 ("time invalid", POST 163). POST sets it when its RTC
     * check fails and never clears it, so every later POST stops at 163 and
     * resets the CMOS - five boots on .243 after one power loss. */
    build_cmos(); cmos[0x0E] = 0x04;                                  /* .243 tonight: 2Dh=00 too */
    CHECK(ps_rtc_time_valid(cmos), "the box's RTC (2026-09-26 22:49:54, BCD 24h) passes the ROM's check");
    p = ps_plan(cmos, want);
    CHECK(p == PS_APPLY && want[0x0E] == 0x00 && want[0x2D] == 0x08 && want[0x2E] == 0x04 && want[0x2F] == 0x3C,
          "0Eh=04 + 2Dh=00: clear bit 2 AND set skip-F1 (the checksum does not cover 0Eh)");
    build_cmos(); cmos[0x0E] = 0x04; cmos[0x2D] = 0x08; cmos[0x2F] = 0x3C;
    p = ps_plan(cmos, want);
    CHECK(p == PS_APPLY && want[0x0E] == 0x00 && want[0x2D] == 0x08 && want[0x2F] == 0x3C,
          "skip-F1 already set: the stale 163 flag alone is still worth a run");
    build_cmos(); cmos[0x0E] = 0x84;
    p = ps_plan(cmos, want);
    CHECK(p == PS_APPLY && want[0x0E] == 0x80, "only bit 2 is cleared - 'RTC lost power' (bit 7) is not ours");
    build_cmos(); cmos[0x0E] = 0x04; cmos[0x2D] = 0x08; cmos[0x2F] = 0x3C;
    cmos[0x32] = 0x19; cmos[0x09] = 0x80; cmos[0x08] = 0x01; cmos[0x07] = 0x04;   /* the 1980-01-04 a power loss leaves */
    CHECK(!ps_rtc_time_valid(cmos) && ps_plan(cmos, want) == PS_ALREADY,
          "the 1980 default is a time nobody set: bit 2 stays (and ALREADY is reported as a failure)");
    build_cmos(); cmos[0x0E] = 0x04; cmos[0x09] = 0x23;
    CHECK(!ps_rtc_time_valid(cmos), "a year before 2024 is not 'set by clockfix'");
    build_cmos(); cmos[0x0E] = 0x04; cmos[0x07] = 0x00;
    CHECK(!ps_rtc_time_valid(cmos), "date 00 fails the ROM's own check");
    build_cmos(); cmos[0x0E] = 0x04; cmos[0x02] = 0x5A;
    CHECK(!ps_rtc_time_valid(cmos), "a non-BCD minute fails");
    build_cmos(); cmos[0x0E] = 0x04; cmos[0x0B] = 0x06;
    CHECK(!ps_rtc_time_valid(cmos), "binary mode (0Bh bit 2) is not judged");
    build_cmos(); cmos[0x0E] = 0x04; cmos[0x0B] = 0x00;
    CHECK(!ps_rtc_time_valid(cmos), "12-hour mode is not judged");
    CHECK(ps_writable(0x1B) && ps_plan(cmos, want) != PS_BAD_CHECKSUM,
          "1Bh is writable since 1.92.0 - but ps_plan() (no CmosIde2Type) never moves it (see above)");
    CHECK(!ps_writable(0x0B) && !ps_writable(0x0A), "the RTC control registers are never written");

    CHECK(ps_volatile_reg(0x00) && ps_volatile_reg(0x09) && ps_volatile_reg(0x0C) && ps_volatile_reg(0x0D),
          "the clock and RTC C/D are ignored when comparing");
    CHECK(!ps_volatile_reg(0x0A) && !ps_volatile_reg(0x0B) && !ps_volatile_reg(0x2D),
          "0Ah/0Bh and 2Dh are compared");
    build_cmos(); memcpy(other, cmos, 128);
    other[0x00] ^= 0x11; other[0x0C] ^= 0x80;
    CHECK(ps_diff(other, cmos) == 0, "a ticking clock is not a difference");
    other[0x0B] ^= 0x02;
    CHECK(ps_diff(other, cmos) == 1, "a stray write into 0Bh is");

    loop_tests();

    {   /* The list an operator repairs a bank from must say when it is cut short. */
        unsigned char a[128], b[128];
        char out[100];
        int k, entries = 0;
        const char *p;
        memset(a, 0, 128); memset(b, 0, 128);
        for (k = 0; k < 12; k++) b[0x40 + k] = (unsigned char)(0x11 + k);
        ps_list_changes(b, a, out, (int)sizeof(out));
        for (p = out; (p = strstr(p, "h ")) != NULL; p++) entries++;
        CHECK(strstr(out, " more") != NULL && strstr(out, "40h 00>11") == out,
              "12 changed registers in a 100-byte list end with '+N more'");
        {
            const char *plus = strrchr(out, '+');
            CHECK(plus && entries + atoi(plus + 1) == 12, "... and listed + '+N' = all 12");
        }
        ps_list_changes(b, a, out, 200);
        CHECK(strstr(out, "more") == NULL && strstr(out, "4Bh 00>1C") != NULL, "a big enough buffer lists all 12, no marker");
        b[0x0A] = 0x80; memset(b + 0x40, 0, 12);
        ps_list_changes(b, a, out, (int)sizeof(out));
        CHECK(out[0] == 0, "UIP alone is not a change");
    }

    printf("-- postskip (Compaq Deskpro 2000 skip-F1 + 163 flag + 1Bh, agent 1.92.0): %d/%d tests passed --\n", runs - fails, runs);
    return fails ? 1 : 0;
}
