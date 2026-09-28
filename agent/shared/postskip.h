/*
 * postskip.h - the decision half of postskip.c (agent 1.86.0), Win32-free so
 * tests/native/test_postskip.c compiles the exact code the agent runs.
 *
 * WHY: .243 is a Compaq Deskpro 2000 (586C BIOS, 04/25/97) with a dead,
 * soldered CMOS battery and a POST that reports "301-Keyboard Error" at every
 * boot. Unless CMOS 2Dh bit 3 ("POST Error Handling: skip F1 message") is set,
 * POST waits for F1 and the box never comes back from a reboot unattended.
 * The bit was set by hand (cmosw9x postskip on, 2026-09-27) and proven on a
 * warm reboot; a power loss on a dead battery can take it away again, so the
 * agent re-asserts it at every start. While the power then stays on, every
 * reboot is unattended.
 *
 * What it can NOT fix: a CMOS whose checksum is invalid makes POST show 162
 * and wait for F1 by design (F000:965D) - and this code refuses to write to a
 * CMOS it cannot validate, so after such a power loss one person presses F1
 * once and the next agent start restores the bit.
 *
 * The ROM evidence (the box's own F000 shadow): F000:95F8, called once at the
 * end of POST, clears the wait flag BDA 40:12 bit 0 only when the standard
 * checksum (sum of 10h..2Dh at 2Eh/2Fh, F000:6049) is valid AND 2Dh bit 3 is
 * set. Compaq's Deskpro 4000 Technical Reference (same BIOS generation)
 * documents the bit. See scripts/fleet/win9x/cmosw9x.c.
 */
#ifndef RETRO_POSTSKIP_H
#define RETRO_POSTSKIP_H

#include <string.h>

#define PS_REG_FLAGS  0x2D
#define PS_REG_CSHI   0x2E
#define PS_REG_CSLO   0x2F
#define PS_SKIP_F1    0x08
#define PS_REG_DIAG   0x0E                  /* diagnostic status byte - NOT checksummed */
#define PS_DIAG_TIME_BAD 0x04               /* "time/date invalid": POST 163 */

/* The ROM this applies to, as offsets in the 64 KB F000 segment. */
#define PS_ROM_VENDOR_OFF 0xFFEA            /* "COMPAQ"   */
#define PS_ROM_DATE_OFF   0xFFF5            /* "04/25/97" */
#define PS_ROM_MODEL      "Compaq Deskpro 2000"

enum ps_plan {
    PS_ALREADY = 0,       /* bit already set - nothing to do */
    PS_APPLY,             /* write the image the plan filled in */
    PS_BAD_CHECKSUM,      /* refuse: cannot validate the CMOS we would write into */
    PS_NOT_CMOS,          /* refuse: the bank does not look like a populated CMOS */
};

/* Every PC CMOS holds the base memory size at 15h/16h (640 KB = 0280h), and
 * .243's does. A blank bank sums to a "valid" zero checksum, so the checksum
 * alone cannot tell a populated CMOS from an erased or unanswering one. */
static int ps_bank_sane(const unsigned char *cmos)
{
    return cmos[0x15] == 0x80 && cmos[0x16] == 0x02;
}

/* Is this the Compaq Deskpro 2000 586C 04/25/97 ROM? `rom` is the whole
 * 64 KB F000 segment. All three facts must hold. */
static int ps_rom_matches(const unsigned char *rom, unsigned long len)
{
    unsigned long i, n = sizeof(PS_ROM_MODEL) - 1;
    int model = 0;
    if (len < 0x10000UL) return 0;
    if (memcmp(rom + PS_ROM_VENDOR_OFF, "COMPAQ", 6) != 0) return 0;
    if (memcmp(rom + PS_ROM_DATE_OFF, "04/25/97", 8) != 0) return 0;
    for (i = 0; i + n <= len && !model; i++)
        if (rom[i] == 'C' && memcmp(rom + i, PS_ROM_MODEL, n) == 0) model = 1;
    return model;
}

static unsigned ps_sum10_2d(const unsigned char *cmos)
{
    unsigned s = 0;
    int i;
    for (i = 0x10; i <= 0x2D; i++) s += cmos[i];
    return s & 0xFFFF;
}

static int ps_cs_valid(const unsigned char *cmos)
{
    return (((unsigned)cmos[PS_REG_CSHI] << 8) | cmos[PS_REG_CSLO]) == ps_sum10_2d(cmos);
}

/* Does the RTC hold a time the ROM would accept, and one that was really SET?
 *
 * 1.86.1: POST sets 0Eh bit 2 when its own RTC check fails (F000:2180 calls
 * the check at F000:227C: date and month nonzero, every field valid BCD in
 * range) and NOTHING in POST ever clears it - the other `and al,0FBh` sites
 * are PIC masks. So after one power loss on the dead battery every later POST
 * says "163-Time & Date Not Set", reloads the clock and the CMOS defaults
 * (wiping 2Dh bit 3 before its F1 decision) and waits for F1 - measured on
 * .243 2026-09-27, five boots running. Windows' SetSystemTime (clockfix)
 * writes the RTC but never touches 0Eh. The bit may be cleared only when the
 * RTC passes that same check AND reads 2024 or later - the 1980 default a
 * power loss leaves is a time nobody set. BCD, 24-hour mode only (0Bh = what
 * .243 runs); anything else is not judged. */
static int ps_bcd_ok(unsigned v, unsigned max) { return (v & 0x0F) <= 9 && (v >> 4) <= 9 && v <= max; }

static int ps_rtc_time_valid(const unsigned char *c)
{
    if ((c[0x0B] & 0x04) || !(c[0x0B] & 0x02)) return 0;    /* binary or 12-hour mode */
    if (!ps_bcd_ok(c[0x00], 0x59) || !ps_bcd_ok(c[0x02], 0x59) || !ps_bcd_ok(c[0x04], 0x23)) return 0;
    if (!c[0x07] || !ps_bcd_ok(c[0x07], 0x31) || !c[0x08] || !ps_bcd_ok(c[0x08], 0x12)) return 0;
    if (!ps_bcd_ok(c[0x09], 0x99) || c[0x32] != 0x20 || c[0x09] < 0x24) return 0;
    return 1;
}

/* Is a set 0Eh bit 2 one this code may clear? */
static int ps_diag_clearable(const unsigned char *cmos)
{
    return (cmos[PS_REG_DIAG] & PS_DIAG_TIME_BAD) && ps_rtc_time_valid(cmos);
}

/* Decide, and when the answer is PS_APPLY fill `want` with the whole intended
 * bank: 2Dh with bit 3 set and the checksum recomputed from the LIVE bytes,
 * and/or 0Eh without bit 2 (outside the checksum). */
static enum ps_plan ps_plan(const unsigned char *cmos, unsigned char *want)
{
    unsigned cs;
    int skip, diag;
    if (!ps_bank_sane(cmos)) return PS_NOT_CMOS;
    if (!ps_cs_valid(cmos)) return PS_BAD_CHECKSUM;
    skip = !(cmos[PS_REG_FLAGS] & PS_SKIP_F1);
    diag = ps_diag_clearable(cmos);
    if (!skip && !diag) return PS_ALREADY;
    memcpy(want, cmos, 128);
    if (skip) {
        want[PS_REG_FLAGS] = (unsigned char)(cmos[PS_REG_FLAGS] | PS_SKIP_F1);
        cs = ps_sum10_2d(want);
        want[PS_REG_CSHI] = (unsigned char)(cs >> 8);
        want[PS_REG_CSLO] = (unsigned char)(cs & 0xFF);
    }
    if (diag) want[PS_REG_DIAG] = (unsigned char)(cmos[PS_REG_DIAG] & ~PS_DIAG_TIME_BAD);
    return PS_APPLY;
}

/* Registers a comparison must ignore: the running clock and the RTC's
 * read-only C/D. 0Ah/0Bh ARE compared - a stray index between our index and
 * data write could land a byte there. */
static int ps_volatile_reg(int i) { return i <= 0x09 || i == 0x0C || i == 0x0D; }

/* Bits a comparison must ignore within a compared register: 0Ah bit 7 is UIP,
 * read-only, and set by the RTC itself for ~2 ms of every second. Comparing it
 * would report a stray change - "do not reboot" - on a CMOS nobody touched. */
static unsigned char ps_cmp_mask(int i) { return (unsigned char)(i == 0x0A ? 0x7F : 0xFF); }

static int ps_reg_differs(const unsigned char *a, const unsigned char *b, int i)
{
    return !ps_volatile_reg(i) && ((a[i] ^ b[i]) & ps_cmp_mask(i)) != 0;
}

/* How many non-volatile registers of `got` differ from `expect`. */
static int ps_diff(const unsigned char *got, const unsigned char *expect)
{
    int i, n = 0;
    for (i = 0; i < 128; i++)
        if (ps_reg_differs(got, expect, i)) n++;
    return n;
}

/* The only registers the write path may change: 0Eh, 2Dh, 2Eh, 2Fh. */
static const int ps_regs[4] = { PS_REG_DIAG, PS_REG_FLAGS, PS_REG_CSHI, PS_REG_CSLO };
static int ps_writable(int idx)
{
    return idx == PS_REG_DIAG || idx == PS_REG_FLAGS || idx == PS_REG_CSHI || idx == PS_REG_CSLO;
}

/* Do this code's registers of `got` equal `expect`'s? */
static int ps_ours_equal(const unsigned char *got, const unsigned char *expect)
{
    int k;
    for (k = 0; k < 4; k++)
        if (got[ps_regs[k]] != expect[ps_regs[k]]) return 0;
    return 1;
}

/* Every byte this run put on the data port - the change, a restore and an
 * undo alike. Any of those writes can lose its index to another RTC access,
 * so any of them can be what turns up in another register. */
#define PS_LOG_MAX 64
typedef struct {
    unsigned char v[PS_LOG_MAX];
    int n;
} ps_wlog_t;

static void ps_log_add(ps_wlog_t *l, unsigned char v)
{
    int i;
    for (i = 0; i < l->n; i++) if (l->v[i] == v) return;
    if (l->n < PS_LOG_MAX) l->v[l->n++] = v;
}

/* Register `i` (outside 2Dh-2Fh) is evidence of OUR write going astray when it
 * changed and now holds - under the register's compare mask, so 0Ah's UIP bit
 * can neither hide nor fake a match - a byte this run wrote. */
static int ps_is_our_stray(const unsigned char *before, const unsigned char *after,
                           const ps_wlog_t *l, int i)
{
    unsigned char m = ps_cmp_mask(i);
    int k;
    if (ps_writable(i) || ps_volatile_reg(i) || !ps_reg_differs(after, before, i)) return 0;
    for (k = 0; k < l->n; k++)
        if (((after[i] ^ l->v[k]) & m) == 0) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * The port algorithm, behind an I/O interface so tests/native/test_postskip.c
 * runs THIS code against a simulated RTC (torn reads, lost indexes, UIP).
 * postskip.c supplies inb/outb on ports 70h/71h/84h and Sleep().
 *
 * ONE write per run and no automatic retry. Two reviews showed where retrying
 * leads: a retry re-reads whatever a lost restore byte left behind as the new
 * truth and reports success over a damaged 0Bh. So anything unexpected ends
 * the run - restored to the snapshot where that can be proven, loudly
 * otherwise - and the next agent start (or POSTSKIP apply) tries again.
 * ------------------------------------------------------------------------- */
typedef struct {
    unsigned char (*inb)(void *ctx, unsigned short port);
    void (*outb)(void *ctx, unsigned short port, unsigned char v);
    void (*sleep_ms)(void *ctx, unsigned ms);
    void *ctx;
} ps_io_t;

typedef struct {
    const char *state;              /* static string */
    char changed[100];              /* registers left different: "0Bh 02>34 ..." */
    int before_2d, now_2d;          /* -1 = not read / not verified */
    int before_0e, now_0e;          /* the diagnostic byte, same convention */
    int cs_before, cs_now;          /* checksum valid: at the start / at the last VERIFIED read */
    int attempts;                   /* 0 or 1: writes are never retried within a run */
    int strays_undone;              /* our bytes taken back out of other registers */
    int refused;                    /* 1 = refused before any write */
    int failed;                     /* 1 = the run did not end with the bit set and verified */
} ps_outcome_t;

#define PS_RESTORE_PASS  3

static void ps_delay(const ps_io_t *io) { (void)io->inb(io->ctx, 0x84); }   /* the ROM's own delay */

static unsigned char ps_rd(const ps_io_t *io, int idx)
{
    io->outb(io->ctx, 0x70, (unsigned char)(0x80 | idx));   /* bit 7: NMI masked while we hold the index */
    ps_delay(io);
    return io->inb(io->ctx, 0x71);
}
static void ps_done(const ps_io_t *io) { io->outb(io->ctx, 0x70, 0x0D); }   /* as the ROM leaves it */

static void ps_read_all(const ps_io_t *io, unsigned char *b)
{
    int i;
    for (i = 0; i < 128; i++) b[i] = ps_rd(io, i);
    ps_done(io);
}

/* A read is only trusted when two in a row agree. A read can lose its index
 * to another RTC access between the index write and the data read. */
static int ps_read_stable(const ps_io_t *io, unsigned char *b)
{
    unsigned char c[128];
    int k;
    for (k = 0; k < 5; k++) {
        ps_read_all(io, b);
        ps_read_all(io, c);
        if (ps_diff(b, c) == 0) return 1;
        io->sleep_ms(io->ctx, 200);
    }
    return 0;
}

/* The only data-port write: 2Dh-2Fh freely, any other register only to take
 * OUR byte back out of it (ps_is_our_stray), and never the clock or C/D. */
static int ps_put(const ps_io_t *io, ps_wlog_t *log, const ps_wlog_t *attr, int idx,
                  unsigned char v, const unsigned char *before, const unsigned char *after)
{
    if (!ps_writable(idx) && !ps_is_our_stray(before, after, attr, idx)) return -1;
    ps_log_add(log, v);
    io->outb(io->ctx, 0x70, (unsigned char)(0x80 | idx));
    ps_delay(io);
    io->outb(io->ctx, 0x71, v);
    ps_delay(io);
    return 0;
}

static int ps_strays(const unsigned char *before, const unsigned char *after, const ps_wlog_t *l)
{
    int i, n = 0;
    for (i = 0; i < 128; i++) if (ps_is_our_stray(before, after, l, i)) n++;
    return n;
}

/* "0Bh 02>34 ..." for every compared register of `got` that differs from `ref`,
 * ending "+N more" when the buffer cannot hold them all - an operator repairs
 * a bank from this list, so a cut list must never read as a complete one. */
static void ps_list_changes(const unsigned char *got, const unsigned char *ref, char *out, int cap)
{
    static const char hx[] = "0123456789ABCDEF";
    int i, o = 0, left = ps_diff(got, ref);
    out[0] = 0;
    for (i = 0; i < 128 && left > 0; i++) {
        if (!ps_reg_differs(got, ref, i)) continue;
        if (o + 10 + (left > 1 ? 11 : 0) >= cap) {     /* keep room for " +NNN more" */
            int n = left, k = 0;
            char num[4];
            if (o) out[o++] = ' ';
            out[o++] = '+';
            do { num[k++] = (char)('0' + n % 10); n /= 10; } while (n && k < 3);
            while (k) out[o++] = num[--k];
            memcpy(out + o, " more", 6);
            return;
        }
        if (o) out[o++] = ' ';
        out[o++] = hx[i >> 4]; out[o++] = hx[i & 15]; out[o++] = 'h'; out[o++] = ' ';
        out[o++] = hx[ref[i] >> 4]; out[o++] = hx[ref[i] & 15]; out[o++] = '>';
        out[o++] = hx[got[i] >> 4]; out[o++] = hx[got[i] & 15];
        left--;
    }
    out[o] = 0;
}

static void ps_note_now(ps_outcome_t *r, const unsigned char *b)
{
    r->now_0e = b[PS_REG_DIAG];
    r->now_2d = b[PS_REG_FLAGS];
    r->cs_now = ps_cs_valid(b);
}

/* Read, decide and (apply=1) set the bit. The caller has already proven this
 * is Win9x on the Deskpro 2000 04/25/97 ROM, and that clockfix is finished. */
static void ps_cmos_run(const ps_io_t *io, int apply, ps_outcome_t *r)
{
    unsigned char before[128], want[128], after[128];
    ps_wlog_t wl;
    int i, k, pass;
    memset(r, 0, sizeof(*r));
    memset(&wl, 0, sizeof(wl));
    r->before_2d = r->now_2d = r->before_0e = r->now_0e = -1;
    r->failed = 1;

    if (!ps_read_stable(io, before)) {
        r->state = "REFUSED: two CMOS reads disagree (another RTC access in progress) - not writing";
        r->refused = 1;
        return;
    }
    r->before_2d = before[PS_REG_FLAGS];
    r->before_0e = before[PS_REG_DIAG];
    r->cs_before = ps_cs_valid(before);
    ps_note_now(r, before);
    switch (ps_plan(before, want)) {
    case PS_NOT_CMOS:
        r->state = "REFUSED: the CMOS bank does not look populated (base memory at 15h/16h is not 640 KB) - not writing";
        r->refused = 1;
        return;
    case PS_BAD_CHECKSUM:
        r->state = "REFUSED: the CMOS checksum is invalid - not writing (POST will show 162 and wait for F1 once)";
        r->refused = 1;
        return;
    case PS_ALREADY:
        if (before[PS_REG_DIAG] & PS_DIAG_TIME_BAD) {
            /* Skip-F1 is set, but 163 will reset the CMOS before POST asks. */
            r->state = "skip-F1 set, but 0Eh says the time is invalid and the RTC does not hold a plausible "
                       "set time (clockfix?) - the next POST will stop at 163";
            return;
        }
        r->state = "already set";
        r->failed = 0;
        return;
    case PS_APPLY:
        if (!apply) { r->state = "NOT set (POST will wait for F1 on an error)"; return; }
        break;
    }

    /* The change: only the registers whose value actually changes. */
    r->attempts = 1;
    for (k = 0; k < 4; k++) {
        i = ps_regs[k];
        if (want[i] != before[i]) ps_put(io, &wl, &wl, i, want[i], before, before);
    }
    ps_done(io);

    if (!ps_read_stable(io, after)) {
        r->now_2d = r->now_0e = -1;
        r->state = "NOT VERIFIED: CMOS reads disagree after the write - state unknown, do not reboot";
        return;
    }
    ps_note_now(r, after);
    if (ps_ours_equal(after, want) && ps_cs_valid(after) && ps_strays(before, after, &wl) == 0) {
        r->failed = 0;
        ps_list_changes(after, want, r->changed, (int)sizeof(r->changed));   /* somebody else's, left alone */
        r->state = r->changed[0] ? "set now (another writer changed the registers listed - left alone)" : "set now";
        return;
    }

    /* Anything else: back to the snapshot - 2Dh-2Fh, and our strays out of
     * wherever they landed, judged BEFORE the checksum (a stray inside
     * 10h-2Ch is what spoils it). A few passes; never a second attempt. */
    for (pass = 0; pass < PS_RESTORE_PASS; pass++) {
        unsigned char seen[128];
        ps_wlog_t attr = wl;            /* what had been written when `seen` was read */
        memcpy(seen, after, sizeof(seen));
        for (i = 0; i < 128; i++) {
            if (ps_writable(i)) { if (seen[i] != before[i]) ps_put(io, &wl, &attr, i, before[i], before, seen); }
            else if (ps_is_our_stray(before, seen, &attr, i)) {
                ps_put(io, &wl, &attr, i, before[i], before, seen);
                r->strays_undone++;
            }
        }
        ps_done(io);
        if (!ps_read_stable(io, after)) {
            r->now_2d = r->now_0e = -1;
            r->state = "FAILED AND NOT VERIFIED: CMOS reads disagree after the restore - state unknown, do not reboot";
            return;
        }
        ps_note_now(r, after);
        if (ps_diff(after, before) == 0) break;
    }
    ps_list_changes(after, before, r->changed, (int)sizeof(r->changed));
    if (pass == PS_RESTORE_PASS) {
        r->state = ps_strays(before, after, &wl) || !ps_ours_equal(after, before) || !ps_cs_valid(after)
            ? "FAILED AND NOT RESTORED: the registers listed differ from the snapshot - do not reboot"
            : "FAILED: 2Dh-2Fh restored, but another writer changed the registers listed";
        return;
    }
    r->state = "FAILED: the write did not land cleanly - CMOS restored to the snapshot; the bit is NOT set";
}

#endif
